#include "pch.h"
#include "ScreenShadows.h"
#include "CVars.h"
#include "GameInfo.h"
#include "GpuTrace.h"
#include "PassProfile.h"
#include "ViewMatrixHook.h"
#include <detours/detours.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <mutex>
#include <unordered_set>
#include <thread>
#include <vector>
#include <cmath>
#include <algorithm>

namespace
{
    // ---- the shaders ----
    // P: the view's projection (view+0x7390) as stored (either matrix convention, as Fog reads it). V: its view matrix
    // (view+0x7450) rows -left / up / -forward with the translation in w. L: towards the sun, world (z up).
    // boxes (half resolution): every moving shadow caster's world box (from the sun cascades' caster lists) drawn over
    //   the screen; where the depth buffer's surface lies inside one, the smallest such box's index goes into BoxIdx.
    // trace (half resolution): from each pixel a ray towards the sun, sampled evenly ON SCREEN (no random jitter: the
    //   game has no TAA to average noise). A sample is blocked where it lies behind the visible surface there AND that
    //   surface is a caster's (a character, or a small object out of the shadow maps) AND the sample is inside that
    //   caster's box: its volume is what lies between its visible surface and the back of its box. (The depth buffer
    //   alone has no back sides: treating every surface as a slab drew each object's silhouette as a second "shell".)
    //   Optionally, within the contact length, anything blocks within a thin shell behind it.
    // apply (full resolution): the depth-aware upsample, MINed into the mask.
    const char* kHlsl = R"(
cbuffer C : register(b0)
{
    float4 P[4];
    float4 V[3];
    float4 L;       // xyz towards the sun (world), w: how much the shadow darkens (0..1)
    float4 g0;      // the casters' ray length (m, 0 = none), max steps, full width, full height
    float4 g1;      // 0, fade-out distance (m), the contact length (m, 0 = none), 0
    float4 g2;      // a character's thickness without its box (m), the contact thickness (m), half width, half height
};
Texture2D<float> Depth : register(t0);
Texture2D<float> Chars : register(t1);
Texture2D<float2> Half : register(t2);
StructuredBuffer<float4> Boxes : register(t3);   // per box: min.xyz + 1 if a small object (else 0), max.xyz + 0
Texture2D<uint> BoxIdx : register(t4);           // 0 = none, else box index + 1

struct VO { float4 pos : SV_Position; };
VO vs(uint id : SV_VertexID)
{
    VO o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 mark() : SV_Target { return 1; }   // a character's pixel (the eye's depth pass)

// the projection's terms (as Fog::Distance): s = +-1, depth = a s + b / forward, offsets (TAA jitter)
void Terms(out float s, out float a, out float b, out float ox, out float oy, out float sx, out float sy)
{
    float4 R0 = P[0], R1 = P[1], R2 = P[2], R3 = P[3];
    bool colVec = abs(abs(R3.z) - 1) < 1e-3;
    s = colVec ? R3.z : R2.w;
    a = R2.z;
    b = colVec ? R2.w : R3.z;
    ox = colVec ? R0.z : R2.x; oy = colVec ? R1.z : R2.y;
    sx = R0.x; sy = R1.y;
}
// the distance along the view's forward axis to what depth `d` shows; 0 = nothing (the sky)
float Fwd(float d, float s, float a, float b)
{
    float cleared = saturate(a * s) > 0.5 ? 1.0 : 0.0;
    if (d == cleared) return 0;
    float den = d - a * s;
    if (abs(den) < 1e-12) return 0;
    float f = b / den;
    return (f > 0 && f < 20000) ? f : 0;
}
float3 ToView(float3 w) { return float3(dot(V[0].xyz, w) + V[0].w, dot(V[1].xyz, w) + V[1].w, dot(V[2].xyz, w) + V[2].w); }
// the world position of what the full-resolution uv shows (forward distance f)
float3 World(float2 uv, float f, float s, float ox, float oy, float sx, float sy)
{
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float3 r = float3(s * (ndc.x * s - ox) / sx, s * (ndc.y * s - oy) / sy, -1);
    float3 eye = -(V[0].w * V[0].xyz + V[1].w * V[1].xyz + V[2].w * V[2].xyz);
    return eye + (r.x * V[0].xyz + r.y * V[1].xyz + r.z * V[2].xyz) * f;
}
float2 ToUv(float3 v, float s, float ox, float oy, float sx, float sy)
{
    float fq = -v.z;
    float2 qn = float2(v.x / fq * sx + ox * s, v.y / fq * sy + oy * s);
    return float2(qn.x * 0.5 + 0.5, 0.5 - qn.y * 0.5);
}

struct BO { float4 pos : SV_Position; nointerpolation uint id : BOXID; };
static const uint kCube[36] = { 0,2,1, 1,2,3, 4,5,6, 5,7,6, 0,1,4, 1,5,4, 2,6,3, 3,6,7, 0,4,2, 2,4,6, 1,3,5, 3,7,5 };
BO boxvs(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    float s, a, b, ox, oy, sx, sy; Terms(s, a, b, ox, oy, sx, sy);
    uint c = kCube[vid];
    float4 mn = Boxes[iid * 2], mx = Boxes[iid * 2 + 1];
    float3 w = float3((c & 1) ? mx.x : mn.x, (c & 2) ? mx.y : mn.y, (c & 4) ? mx.z : mn.z);
    float3 v = ToView(w);
    float fq = -v.z;
    // depth = the box's size: the smallest box wins where several hold the same surface
    float size = saturate(length(mx.xyz - mn.xyz) / 8);
    BO o;
    o.pos = float4(v.x * sx + ox * s * fq, v.y * sy + oy * s * fq, size * fq, fq);
    o.id = iid;
    return o;
}
uint boxps(BO i) : SV_Target
{
    float s, a, b, ox, oy, sx, sy; Terms(s, a, b, ox, oy, sx, sy);
    uint dw, dh; Depth.GetDimensions(dw, dh); float2 dsz = float2(dw, dh);
    float2 uv = (floor(i.pos.xy) * 2 + 0.5) / g0.zw;
    float f = Fwd(Depth.Load(int3(min(uv * dsz, dsz - 1), 0)), s, a, b);
    if (!(f > 0)) discard;
    float3 pos = World(uv, f, s, ox, oy, sx, sy);
    float4 mn = Boxes[i.id * 2], mx = Boxes[i.id * 2 + 1];
    float e = 0.01 + 0.001 * f;
    if (any(pos < mn.xyz - e) || any(pos > mx.xyz + e)) discard;
    return i.id + 1;
}

float2 trace(VO i) : SV_Target
{
    float s, a, b, ox, oy, sx, sy; Terms(s, a, b, ox, oy, sx, sy);
    uint dw, dh; Depth.GetDimensions(dw, dh); float2 dsz = float2(dw, dh);
    uint cw, ch; Chars.GetDimensions(cw, ch); float2 csz = float2(cw, ch);
    uint iw, ih; BoxIdx.GetDimensions(iw, ih); float2 isz = float2(iw, ih);
    float2 uv = (floor(i.pos.xy) * 2 + 0.5) / g0.zw;
    float f = Fwd(Depth.Load(int3(min(uv * dsz, dsz - 1), 0)), s, a, b);
    if (!(f > 0) || f > g1.y) return float2(1, f);
    // a character is not shadowed by characters here (its own body would read as its occluder)
    bool selfChar = cw > 1 && Chars.Load(int3(min(uv * csz, csz - 1), 0)) > 0.5;
    float maxT = max(g0.x, g1.z);
    if (!(maxT > 0)) return float2(1, f);
    float3 pos = World(uv, f, s, ox, oy, sx, sy);
    float start = 0.03 + 0.002 * f;            // off the surface itself
    // the ray's ends in view space; cut where it would pass behind the eye
    float fa = -ToView(pos + L.xyz * start).z, fb = -ToView(pos + L.xyz * maxT).z;
    if (fb < 0.05)
    {
        if (fa <= 0.05) return float2(1, f);
        maxT = start + (maxT - start) * (fa - 0.05) / (fa - fb);
        fb = 0.05;
    }
    // as many steps as the ray is long on screen (one per 1.5 half-resolution pixels), at most g0.y
    float2 ua = ToUv(ToView(pos + L.xyz * start), s, ox, oy, sx, sy), ub = ToUv(ToView(pos + L.xyz * maxT), s, ox, oy, sx, sy);
    float px = length((ub - ua) * g2.zw);
    int n = clamp((int)ceil(px / 1.5), 3, (int)g0.y);
    float occ = 0;
    [loop] for (int k = 1; k <= n; k++)
    {
        float u = (float)k / n;
        float sp = u * fa / ((1 - u) * fb + u * fa);    // even on screen (perspective-correct along the ray)
        float t = start + (maxT - start) * sp;
        float3 q = pos + L.xyz * t;
        float3 v = ToView(q);
        float fq = -v.z;
        if (!(fq > 0.05)) break;
        float2 qu = ToUv(v, s, ox, oy, sx, sy);
        if (any(qu < 0) || any(qu > 1)) break;      // off screen: the game's shadow maps alone
        uint idx = BoxIdx.Load(int3(min(qu * isz, isz - 1), 0));
        bool isChar = cw > 1 && Chars.Load(int3(min(qu * csz, csz - 1), 0)) > 0.5;
        float4 mn = 0, mx = 0;
        if (idx) { mn = Boxes[(idx - 1) * 2]; mx = Boxes[(idx - 1) * 2 + 1]; }
        bool caster = t <= g0.x && ((isChar && !selfChar) || (idx && mn.w > 0.5));
        bool contact = t <= g1.z;
        if (!caster && !contact) continue;
        float fs = Fwd(Depth.Load(int3(min(qu * dsz, dsz - 1), 0)), s, a, b);
        if (!(fs > 0)) continue;                     // the sky there
        float dz = fq - fs;                          // the ray point behind what the screen shows there
        if (!(dz > 0.02 + 0.003 * fq)) continue;
        if (caster)
        {
            if (idx)
            {   // inside the caster's box: between its visible surface and its back
                float e = 0.03;
                if (all(q >= mn.xyz - e) && all(q <= mx.xyz + e)) { occ = 1; break; }
            }
            else if (dz < g2.x) { occ = 1; break; }   // a character with no box found: a body's thickness
        }
        if (contact && dz < g2.y) { occ = 1; break; }
    }
    // thinner with distance, and at the screen's edges
    float2 e2 = min(uv, 1 - uv);
    float fade = saturate(1 - f / g1.y) * saturate(min(e2.x, e2.y) * 20);
    return float2(1 - occ * fade * L.w, f);
}

float4 apply(VO i) : SV_Target
{
    float s, a, b, ox, oy, sx, sy; Terms(s, a, b, ox, oy, sx, sy);
    uint dw, dh; Depth.GetDimensions(dw, dh); float2 dsz = float2(dw, dh);
    float2 p = floor(i.pos.xy);
    float2 uv = (p + 0.5) / g0.zw;
    float f = Fwd(Depth.Load(int3(min(uv * dsz, dsz - 1), 0)), s, a, b);
    if (!(f > 0)) return 1;
    float2 h = p * 0.5, base = floor(h), fr = h - base;
    int2 hmax = int2(g2.z, g2.w) - 1;
    float sum = 0, wsum = 0;
    [unroll] for (int k = 0; k < 4; k++)
    {
        int2 o = int2(k & 1, k >> 1);
        float2 v = Half.Load(int3(min(int2(base) + o, hmax), 0));
        float ws = (o.x ? fr.x : 1 - fr.x) * (o.y ? fr.y : 1 - fr.y) + 1e-3;
        float wd = saturate(1 - abs(v.y - f) / (0.03 * f + 0.05));
        sum += v.x * ws * wd; wsum += ws * wd;
    }
    return wsum > 1e-4 ? sum / wsum : 1;
}
)";

    std::atomic<bool> g_On{ true };
    std::atomic<float> g_Len{ 3.f }, g_Strength{ 1.f };
    std::atomic<int> g_Steps{ 16 };
    constexpr float kFadeDist = 80.f;           // m: beyond this the game's shadow maps alone
    constexpr uint32_t kViewProj = 0x7390, kViewMatrix = 0x7450, kViewFrame = 0x77B4;
    constexpr uint32_t kCascadeMatrices = 0x3309020;   // the sun cascades' render matrices (slice * 0x40)

    std::mutex g_M;
    bool g_Ready = false, g_Failed = false;
    ID3D11VertexShader* g_Vs = nullptr; ID3D11PixelShader* g_PsTrace = nullptr; ID3D11PixelShader* g_PsApply = nullptr; ID3D11Buffer* g_Cb = nullptr;
    ID3D11BlendState* g_BlendFor[16] = {};       // MIN into the channels the game's resolve writes (by its write mask)
    ID3D11DepthStencilState* g_NoDepth = nullptr; ID3D11RasterizerState* g_Rs = nullptr;
    // the characters' mask (R8, an eye's depth size): each skinned draw of the eye's depth pass drawn once more into it,
    // depth EQUAL (only where it is the visible surface), no depth write; used and cleared in the eye's SCREENSHADOWS
    ID3D11PixelShader* g_PsMark = nullptr; ID3D11DepthStencilState* g_DepthEqual = nullptr;
    bool g_MarkReady = false, g_MarkFailed = false;
    struct EyeTex
    {
        UINT cw = 0, ch = 0; ID3D11Texture2D* chars = nullptr; ID3D11RenderTargetView* charsRtv = nullptr; ID3D11ShaderResourceView* charsSrv = nullptr;
        UINT hw = 0, hh = 0; ID3D11Texture2D* half = nullptr; ID3D11RenderTargetView* halfRtv = nullptr; ID3D11ShaderResourceView* halfSrv = nullptr;
        ID3D11Texture2D* idx = nullptr; ID3D11RenderTargetView* idxRtv = nullptr; ID3D11ShaderResourceView* idxSrv = nullptr;
        ID3D11Texture2D* idxDs = nullptr; ID3D11DepthStencilView* idxDsv = nullptr;
        std::atomic<bool> marked{ false };       // something was drawn into the mask since its last clear
    };
    EyeTex g_Eye[2];
    std::mutex g_EyeM;
    ID3D11ShaderResourceView* g_NoChars = nullptr;   // a 1x1 R8 zero: no mask
    std::atomic<bool> g_Chars{ true }, g_CharsInMaps{ false };
    std::atomic<float> g_Contact{ 0.f };
    constexpr float kCharThick = 0.4f, kContactThick = 0.12f;   // m behind the visible surface that blocks a ray
    std::atomic<unsigned long long> g_Marked{ 0 }, g_CharShadowDraws{ 0 };
    // the small objects: moving shadow casters whose world box is under a size, taken out of the sun cascades' caster
    // lists (exe+0x328260) and drawn into the mask as boxes (the previous eye-frame's set)
    std::atomic<float> g_SmallR{ 0.f };          // bounding radius (m) below which; 0 = none (with ScreenSpaceShadows only)
    std::atomic<float> g_MinR{ 0.f };            // ShadowMapMinObjectSize: bounding radius (m) below which a moving object
                                                 // is left out of the sun's shadow maps, screen-space shadows or not
    struct Box { float mn[3]; float v; float mx[3]; float pad; };   // v: 1 = a small object (out of the shadow maps), 0 = other
    constexpr int kMaxBoxes = 4096;
    constexpr float kMaxBoxR = 2.f;              // m: casters' boxes up to 4 m across are drawn (a character's fits)
    std::mutex g_BoxM;
    std::vector<Box> g_BoxBuild, g_BoxReady;
    std::unordered_set<const void*> g_BoxSeen;
    int g_BoxFrame = -1;
    std::atomic<unsigned long long> g_SmallCut{ 0 }, g_SmallKept{ 0 }, g_BoxesDrawn{ 0 };
    ID3D11VertexShader* g_BoxVs = nullptr; ID3D11PixelShader* g_BoxPs = nullptr; ID3D11DepthStencilState* g_BoxLess = nullptr;
    ID3D11Buffer* g_BoxBuf = nullptr; ID3D11ShaderResourceView* g_BoxSrv = nullptr;
    Box g_BoxUpload[kMaxBoxes];
    std::atomic<unsigned long long> g_Drawn{ 0 }, g_NoTarget{ 0 }, g_NoDepthSrv{ 0 }, g_NoSun{ 0 }, g_BadFormat{ 0 }, g_NoResolve{ 0 };
    std::string g_Why = "not seen yet";
    int g_Logs = 0; float g_LoggedSun[3] = {};
    std::vector<std::string> g_LoggedMasks;      // the mask / depth / shader combinations already logged
    std::atomic<float> g_Cull{ 0.f };            // ShadowCullDistance (0 = the game's)
    std::atomic<bool> g_Trim{ true };            // ShadowUltraTrim
    std::atomic<bool> g_LampFewer{ true };       // ShadowLampFewerUpdates
    std::atomic<int> g_LampCache{ 32 };          // ShadowLampCacheSlices (0 = the game's)
    // the pixel shaders that resolve the sun's shadow maps into the screen's mask (seen at their creation: they read
    // ro_screenshadows_depthbuffer and ro_lightdepthcomparemap)
    std::mutex g_ResM; std::vector<uint64_t> g_Resolve;
    bool IsResolve(uint64_t h) { std::lock_guard lk(g_ResM); return std::find(g_Resolve.begin(), g_Resolve.end(), h) != g_Resolve.end(); }
    //! a resolve drawn in this thread's SCREENSHADOWS task, the screen-space shadows still to go on top: drawn before
    //! the task's next draw (the blur, which then softens them too) or at the task's end
    struct Pending { ID3D11RenderTargetView* rtv = nullptr; ID3D11ShaderResourceView* depth = nullptr; const void* view = nullptr; int eye = -1; UINT8 mask = 0xF; };
    thread_local Pending t_Pend;
    thread_local bool t_Busy = false;             // our own draw comes back through the draw hook

    template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
    bool ReadBytes(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool Init(ID3D11Device* dev)
    {
        if (g_Ready || g_Failed) return g_Ready;
        g_Failed = true;
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
        if (!compile) { g_Why = "d3dcompiler_47 not available"; LOGI("ScreenShadows: {}", g_Why); return false; }
        auto blob = [&](const char* entry, const char* target) -> ID3DBlob*
        {
            ID3DBlob* b = nullptr; ID3DBlob* err = nullptr;
            if (FAILED(compile(kHlsl, strlen(kHlsl), "sss", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err)))
            {
                LOGI("ScreenShadows: shader '{}' failed to compile: {}", entry, err ? (const char*)err->GetBufferPointer() : "?");
                if (err) err->Release(); return nullptr;
            }
            if (err) err->Release();
            return b;
        };
        ID3DBlob* bv = blob("vs", "vs_5_0"); ID3DBlob* bt = blob("trace", "ps_5_0"); ID3DBlob* ba = blob("apply", "ps_5_0");
        ID3DBlob* bbv = blob("boxvs", "vs_5_0"); ID3DBlob* bbp = blob("boxps", "ps_5_0");
        bool ok = bv && bt && ba && bbv && bbp && SUCCEEDED(dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &g_Vs))
                  && SUCCEEDED(dev->CreatePixelShader(bt->GetBufferPointer(), bt->GetBufferSize(), nullptr, &g_PsTrace))
                  && SUCCEEDED(dev->CreatePixelShader(ba->GetBufferPointer(), ba->GetBufferSize(), nullptr, &g_PsApply))
                  && SUCCEEDED(dev->CreateVertexShader(bbv->GetBufferPointer(), bbv->GetBufferSize(), nullptr, &g_BoxVs))
                  && SUCCEEDED(dev->CreatePixelShader(bbp->GetBufferPointer(), bbp->GetBufferSize(), nullptr, &g_BoxPs));
        if (bv) bv->Release(); if (bt) bt->Release(); if (ba) ba->Release(); if (bbv) bbv->Release(); if (bbp) bbp->Release();
        if (!ok) { g_Why = "shader compile failed"; return false; }
        {   // the casters' boxes (2 float4 each) and the depth test that keeps the smallest
            D3D11_BUFFER_DESC bb{}; bb.ByteWidth = sizeof g_BoxUpload; bb.Usage = D3D11_USAGE_DEFAULT; bb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bb.StructureByteStride = 16;
            D3D11_DEPTH_STENCIL_DESC dl{}; dl.DepthEnable = TRUE; dl.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dl.DepthFunc = D3D11_COMPARISON_LESS; dl.StencilEnable = FALSE;
            if (FAILED(dev->CreateBuffer(&bb, nullptr, &g_BoxBuf)) || FAILED(dev->CreateShaderResourceView(g_BoxBuf, nullptr, &g_BoxSrv))
                || FAILED(dev->CreateDepthStencilState(&dl, &g_BoxLess))) { g_Why = "box buffer"; return false; }
        }
        {   // no characters' mask: a 1x1 zero
            const uint8_t zero = 0;
            D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8_UNORM;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sd{ &zero, 1, 1 }; ID3D11Texture2D* t = nullptr;
            if (FAILED(dev->CreateTexture2D(&td, &sd, &t)) || FAILED(dev->CreateShaderResourceView(t, nullptr, &g_NoChars))) { if (t) t->Release(); g_Why = "textures"; return false; }
            t->Release();
        }
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 176; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_Cb))) { g_Why = "constant buffer"; return false; }
        D3D11_DEPTH_STENCIL_DESC ds{}; ds.DepthEnable = FALSE; ds.StencilEnable = FALSE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        if (FAILED(dev->CreateDepthStencilState(&ds, &g_NoDepth)) || FAILED(dev->CreateRasterizerState(&rd, &g_Rs)))
        { g_Why = "states"; return false; }
        g_Failed = false; g_Ready = true;
        LOGI("ScreenShadows: ready (the sun's screen-space shadows drawn into the game's shadow mask right after the game resolves its sun shadows into it)");
        return true;
    }
    //! MIN into the channels `mask` (the game's shadow stays where it is darker)
    ID3D11BlendState* BlendFor(ID3D11Device* dev, UINT8 mask)
    {
        mask &= 0xF; if (!mask) mask = 0xF;
        if (!g_BlendFor[mask])
        {
            D3D11_BLEND_DESC bl{}; bl.RenderTarget[0].BlendEnable = TRUE;
            bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE; bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MIN;
            bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MIN;
            bl.RenderTarget[0].RenderTargetWriteMask = mask;
            if (FAILED(dev->CreateBlendState(&bl, &g_BlendFor[mask]))) g_BlendFor[mask] = nullptr;
        }
        return g_BlendFor[mask];
    }

    //! The sun's direction (towards it, world, z up) from cascade 0's render matrix: an orthographic light
    //! view-projection, whose x and y rows (or columns) are both perpendicular to the light's direction.
    bool SunDir(float L[3])
    {
        float m[16];
        if (!ReadBytes(m, (const void*)(GameInfo::Base() + kCascadeMatrices), 64)) return false;
        for (float x : m) if (!std::isfinite(x)) return false;
        // which way it is stored: the translation in the last column (rows = output components) or in the last row
        const bool rowsOut = fabsf(m[12]) < 1e-4f && fabsf(m[13]) < 1e-4f && fabsf(m[14]) < 1e-4f && fabsf(m[15] - 1.f) < 1e-3f;
        const bool colsOut = fabsf(m[3]) < 1e-4f && fabsf(m[7]) < 1e-4f && fabsf(m[11]) < 1e-4f && fabsf(m[15] - 1.f) < 1e-3f;
        if (!rowsOut && !colsOut) return false;
        float x[3], y[3];
        for (int i = 0; i < 3; i++) { x[i] = rowsOut ? m[i] : m[i * 4]; y[i] = rowsOut ? m[4 + i] : m[i * 4 + 1]; }
        float d[3] = { x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0] };
        const float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l < 1e-8f) return false;
        for (float& v : d) v /= l;
        if (d[2] < 0) for (float& v : d) v = -v;   // the sun (or the moon) is above the horizon
        if (d[2] < 0.02f) return false;             // at the horizon: no sensible ray
        memcpy(L, d, 12);
        return true;
    }

    bool DepthFormat(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_UNORM:
            return true;
        default: return false;
        }
    }
    bool MaskFormat(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R11G11B10_FLOAT:
            return true;
        default: return false;
        }
    }

    //! What the mask's draw changes, put back exactly after it
    struct Saved
    {
        ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11GeometryShader* gs = nullptr;
        ID3D11Buffer* psCb = nullptr; ID3D11ShaderResourceView* psSrv[5] = {}; ID3D11ShaderResourceView* vsSrv3 = nullptr; ID3D11Buffer* vsCb = nullptr;
        D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED; ID3D11InputLayout* il = nullptr;
        ID3D11RasterizerState* rs = nullptr; D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {}; UINT nvp = 0;
        ID3D11BlendState* bs = nullptr; float bf[4] = {}; UINT mask = 0xFFFFFFFF;
        ID3D11DepthStencilState* dss = nullptr; UINT ref = 0;
        ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {}; ID3D11DepthStencilView* dsv = nullptr;
        void Save(ID3D11DeviceContext* c)
        {
            c->VSGetShader(&vs, nullptr, nullptr); c->PSGetShader(&ps, nullptr, nullptr); c->GSGetShader(&gs, nullptr, nullptr);
            c->PSGetConstantBuffers(0, 1, &psCb); c->PSGetShaderResources(0, 5, psSrv);
            c->VSGetShaderResources(3, 1, &vsSrv3); c->VSGetConstantBuffers(0, 1, &vsCb);
            c->IAGetPrimitiveTopology(&topo); c->IAGetInputLayout(&il);
            c->RSGetState(&rs); nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; c->RSGetViewports(&nvp, vp);
            c->OMGetBlendState(&bs, bf, &mask); c->OMGetDepthStencilState(&dss, &ref);
            c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        }
        void Restore(ID3D11DeviceContext* c)
        {
            // the targets first: the mask we drew into is the next draw's input (t0), and a resource bound as a
            // target cannot be bound as an input (the runtime would null it)
            c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
            c->VSSetShader(vs, nullptr, 0); c->PSSetShader(ps, nullptr, 0); c->GSSetShader(gs, nullptr, 0);
            c->PSSetConstantBuffers(0, 1, &psCb); c->PSSetShaderResources(0, 5, psSrv);
            c->VSSetShaderResources(3, 1, &vsSrv3); c->VSSetConstantBuffers(0, 1, &vsCb);
            c->IASetPrimitiveTopology(topo); c->IASetInputLayout(il);
            c->RSSetState(rs); c->RSSetViewports(nvp, nvp ? vp : nullptr);
            c->OMSetBlendState(bs, bf, mask); c->OMSetDepthStencilState(dss, ref);
            Rel(vs); Rel(ps); Rel(gs); Rel(psCb); for (auto*& v : psSrv) Rel(v); Rel(vsSrv3); Rel(vsCb); Rel(il); Rel(rs); Rel(bs); Rel(dss);
            for (auto*& p : rtv) Rel(p); Rel(dsv);
        }
    };

    // ---- the shadow settings log ----
    const char* kShadowCvars[] = {
        "r_shadowQualityPreset", "r_renderQualityPresets", "r_modelsQualityPreset", "r_downsampleQualityPreset",
        "r_maxShadowMapResolution", "r_maxShadowMapExtraResolution", "r_minShadowMapResolution",
        "r_shadowAtlasSliceCnt", "r_shadowAtlasSliceWidth", "r_shadowAtlasSliceHeight",
        "r_shadowCacheSliceCnt", "r_shadowCacheSliceWidth", "r_shadowCacheSliceHeight",
        "r_shadowParallelSliceCnt", "r_shadowParallelSliceSize", "r_shadowParallelResolutionDividerLevel",
        "r_shadowParallelPartialSliceUpdate", "r_shadowParallelPartialDynaUpdate", "r_tightenParallelShadows",
        "r_shadowCullDistance", "r_shadowCascadeFars", "r_shadows32Bits",
        "r_shadowSpotAndOmniLowResolutionDividerEnabled", "r_shadowSpotAndOmniPartialUpdateDynaThreshold",
        "r_shadowSpotAndOmniPartialUpdateDynaSqrDistanceThreshold", "r_areaLights_ShadowMin", "r_areaLights_ShadowMax",
        "r_shadowsRats", "r_shadowsBloodflies", "r_skipShadowModelSort",
    };
    std::string ReadCvar(const char* n)
    {
        const uintptr_t a = CVars::Address(n);
        if (!a) return "?";
        int iv = 0; float fv = 0; const char* sp = nullptr; char buf[48] = {};
        if (!ReadBytes(&iv, (const void*)(a + 0x28), 4) || !ReadBytes(&fv, (const void*)(a + 0x2C), 4)) return "?";
        if (ReadBytes(&sp, (const void*)a, 8) && sp) ReadBytes(buf, sp, sizeof buf - 1);
        buf[sizeof buf - 1] = 0;
        for (char& ch : buf) if (ch && (ch < 32 || ch > 126)) { ch = 0; break; }
        // the string is what the console shows; the int / float slots as well when they say something else
        const std::string s = buf;
        if (!s.empty()) return s;
        return fmt::format("{} ({:g})", iv, fv);
    }
    void SettingsLoop()
    {
        std::vector<std::string> last(std::size(kShadowCvars));
        bool first = true;
        for (;;)
        {
            std::string changed;
            for (size_t i = 0; i < std::size(kShadowCvars); i++)
            {
                const std::string v = ReadCvar(kShadowCvars[i]);
                if (first || v != last[i]) changed += fmt::format("{}{} = {}{}", changed.empty() ? "" : ", ", kShadowCvars[i], v,
                                                                  first || last[i].empty() ? std::string() : fmt::format(" (was {})", last[i]));
                last[i] = v;
            }
            if (!changed.empty()) LOGI("ScreenShadows: shadow settings {}: {}", first ? "now" : "CHANGED", changed);
            first = false;
            {   // ShadowCullDistance: held while the game's differs (a preset change in the menu sets it again)
                const float want = g_Cull.load();
                const float now = CVars::GetFloat("r_shadowCullDistance", -1.f);
                if (want > 0.f && now >= 0.f && fabsf(now - want) > 0.01f)
                {
                    CVars::SetFloat("r_shadowCullDistance", want);
                    LOGI("ScreenShadows: r_shadowCullDistance {:g} -> {:g} (ShadowCullDistance): shadow casters beyond {:g} m are not drawn into the shadow maps", now, want, want);
                    for (size_t i = 0; i < std::size(kShadowCvars); i++) if (!strcmp(kShadowCvars[i], "r_shadowCullDistance")) last[i] = ReadCvar(kShadowCvars[i]);
                }
            }
            {   // the characters: how many were marked for the screen-space shadows / met in the sun's shadow maps (every 30 s)
                static int tick = 0; static unsigned long long lastM = 0, lastS = 0;
                static unsigned long long lastCut = 0, lastKept = 0, lastBoxes = 0;
                if (++tick % 30 == 0 && ((g_On.load() && (g_Chars.load() || g_SmallR.load() > 0.f)) || g_MinR.load() > 0.f))
                {
                    const unsigned long long m = g_Marked.load(), sh = g_CharShadowDraws.load();
                    const unsigned long long cut = g_SmallCut.load(), kept = g_SmallKept.load(), boxes = g_BoxesDrawn.load();
                    if (m != lastM || sh != lastS || cut != lastCut)
                        LOGI("ScreenShadows: in the last 30 s: characters: {} draws marked in the eyes' depth passes, {} met in the sun's shadow maps ({}); small objects: {} of {} moving casters taken out of the sun's cascades ({:.0f} %), {} boxes drawn into the eyes' masks",
                             m - lastM, sh - lastS, g_CharsInMaps.load() ? "kept there" : "left out", cut - lastCut, (cut - lastCut) + (kept - lastKept),
                             (cut - lastCut) + (kept - lastKept) ? 100.0 * (double)(cut - lastCut) / (double)((cut - lastCut) + (kept - lastKept)) : 0.0, boxes - lastBoxes);
                    lastM = m; lastS = sh; lastCut = cut; lastKept = kept; lastBoxes = boxes;
                }
            }
            {   // ShadowUltraTrim: at Ultra, what costs without making the shadows sharper is held at High's values
                static int told = 0;
                if (g_Trim.load() && CVars::GetInt("r_shadowQualityPreset", -1) == 5)
                {
                    std::string held;
                    if (CVars::GetInt("r_shadowParallelPartialDynaUpdate", 1) != 1 && CVars::SetInt("r_shadowParallelPartialDynaUpdate", 1)) held += " r_shadowParallelPartialDynaUpdate 1";
                    struct { const char* n; float v; } fl[] = { { "r_shadowSpotAndOmniPartialUpdateDynaThreshold", 0.4f }, { "r_shadowSpotAndOmniPartialUpdateDynaSqrDistanceThreshold", 300.f } };
                    if (!g_LampFewer.load())   // (ShadowLampFewerUpdates holds these lower still, below)
                    for (auto& f : fl)
                        if (fabsf(CVars::GetFloat(f.n, f.v) - f.v) > 1e-3f && CVars::SetFloat(f.n, f.v)) held += fmt::format(" {} {:g}", f.n, f.v);
                    if (!held.empty() && told < 8)
                    {
                        told++;
                        LOGI("ScreenShadows: Ultra shadows trimmed (ShadowUltraTrim): held at High's values:{} (the sun's cascades, resolution and precision stay Ultra's)", held);
                    }
                }
            }
            {   // ShadowLampFewerUpdates: at every preset, the lamps redraw their moving casters (characters, bodies, objects
                // in flight) only for a lamp that covers 70 % of the screen or is within 8 m -- Low's values (High 0.4 /
                // 300, Ultra 0.2 / 900 = 30 m). The lamps' walls and floors are cached and unaffected.
                static int told = 0;
                if (g_LampFewer.load())
                {
                    std::string held;
                    struct { const char* n; float v; } fl[] = { { "r_shadowSpotAndOmniPartialUpdateDynaThreshold", 0.7f }, { "r_shadowSpotAndOmniPartialUpdateDynaSqrDistanceThreshold", 64.f } };
                    for (auto& f : fl)
                    {
                        const float now = CVars::GetFloat(f.n, -1.f);
                        if (now >= 0.f && fabsf(now - f.v) > 1e-3f && CVars::SetFloat(f.n, f.v)) held += fmt::format(" {} {:g} -> {:g}", f.n, now, f.v);
                    }
                    if (!held.empty() && told < 8)
                    {
                        told++;
                        LOGI("ScreenShadows: lamps' moving-caster updates held at Low's (ShadowLampFewerUpdates):{}", held);
                    }
                }
            }
            {   // ShadowLampCacheSlices: the lamps' shadow cache (r_shadowCacheSliceCnt, 12 slices of 2048 x 2048). A lamp's
                // walls and floors are drawn once into it and copied back each frame -- while they fit: at Ultra a frame's
                // lamps ask for more than 12 slices (an omni light at the 2048 "extra" resolution alone takes 6), the
                // cache evicts what the same frame needs again, and every lamp face is redrawn every frame in both eyes
                // (measured 2026-10-31: ~60 % of the faces new each frame, the same light's face new in the left frame
                // and again in the right). The count is read when the atlas and the cache are made (exe+0x373428, in
                // exe+0x373070), which the game does whenever r_shadows32Bits or r_shadowSpotAndOmniLowResolution-
                // DividerEnabled is changed (their "modified" flag, bit 18 of the flags at +0x48, tested every frame at
                // exe+0x3518D4): the count is set, then the divider's flag, as the game's own shadow-quality change does.
                static int told = 0;
                const int want = g_LampCache.load();
                const int now = CVars::GetInt("r_shadowCacheSliceCnt", -1);
                const uintptr_t div = CVars::Address("r_shadowSpotAndOmniLowResolutionDividerEnabled");
                if (want > 0 && now > 0 && now != want && div && CVars::SetInt("r_shadowCacheSliceCnt", want))
                {
                    InterlockedOr((volatile LONG*)(div + 0x48), 0x40000);
                    if (told++ < 8)
                        LOGI("ScreenShadows: the lamps' shadow cache: r_shadowCacheSliceCnt {} -> {} (ShadowLampCacheSlices; {} MB of video memory at 32 bits), "
                             "the atlas and the cache made again", now, want, (long long)want * 16);
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

void ScreenShadows::SetParams(bool on, float lengthM, int steps, float strength)
{
    g_On = on; g_Len = std::clamp(lengthM, 0.2f, 20.f); g_Steps = std::clamp(steps, 4, 64); g_Strength = std::clamp(strength, 0.f, 1.f);
    LOGI("ScreenShadows: {}", on ? fmt::format("on: the sun's shadows of what is on screen traced through the depth buffer ({:.1f} m towards the sun, {} steps, strength {:.2f}; ScreenSpaceShadows)",
                                               g_Len.load(), g_Steps.load(), g_Strength.load())
                                 : std::string("off (ScreenSpaceShadows = 0)"));
}
bool ScreenShadows::Wanted() { return g_On.load(); }

namespace
{
    //! the mask's shader and depth state (any thread: the depth pass's tasks record on several)
    bool MarkInit(ID3D11Device* dev)
    {
        std::lock_guard lk(g_EyeM);
        if (g_MarkReady || g_MarkFailed) return g_MarkReady;
        g_MarkFailed = true;
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
        ID3DBlob* b = nullptr; ID3DBlob* err = nullptr;
        if (!compile || FAILED(compile(kHlsl, strlen(kHlsl), "sss", nullptr, nullptr, "mark", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err)))
        { LOGI("ScreenShadows: the characters' mask shader failed to compile: {}", err ? (const char*)err->GetBufferPointer() : "no d3dcompiler"); if (err) err->Release(); return false; }
        if (err) err->Release();
        const bool ok = SUCCEEDED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_PsMark));
        b->Release();
        D3D11_DEPTH_STENCIL_DESC ds{}; ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; ds.DepthFunc = D3D11_COMPARISON_EQUAL; ds.StencilEnable = FALSE;
        if (!ok || FAILED(dev->CreateDepthStencilState(&ds, &g_DepthEqual))) return false;
        g_MarkFailed = false; g_MarkReady = true;
        LOGI("ScreenShadows: characters' mask ready (each eye's depth pass: skinned draws marked where they are the visible surface)");
        return true;
    }
    //! this eye's mask at the depth buffer's size (made or remade as needed); its view, or null
    ID3D11RenderTargetView* CharsRtv(ID3D11Device* dev, int eye, UINT w, UINT h)
    {
        std::lock_guard lk(g_EyeM);
        EyeTex& E = g_Eye[eye];
        if (E.chars && (E.cw != w || E.ch != h) && (unsigned long long)w * h < (unsigned long long)E.cw * E.ch) return nullptr;   // a smaller depth target in the pass: not the eye's
        if (E.cw != w || E.ch != h || !E.chars)
        {
            Rel(E.charsRtv); Rel(E.charsSrv); Rel(E.chars); E.cw = E.ch = 0;
            D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8_UNORM;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            std::vector<uint8_t> zero((size_t)w * h, 0);   // starts cleared: the first clear is at the eye's first SCREENSHADOWS
            D3D11_SUBRESOURCE_DATA sd{ zero.data(), w, 0 };
            if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &E.chars)) && SUCCEEDED(dev->CreateRenderTargetView(E.chars, nullptr, &E.charsRtv))
                && SUCCEEDED(dev->CreateShaderResourceView(E.chars, nullptr, &E.charsSrv))) { E.cw = w; E.ch = h; E.marked = false; LOGI("ScreenShadows: eye {}: the characters' mask {}x{}", eye, w, h); }
            else { Rel(E.charsRtv); Rel(E.charsSrv); Rel(E.chars); }
        }
        return E.charsRtv;
    }
    //! The screen-space shadows for `p` (a resolve just drawn): MIN into the mask's sun channel(s)
    void DrawPending(ID3D11DeviceContext* c, const Pending& p)
    {
        float L[3];
        if (!SunDir(L)) { g_NoSun++; return; }
        ID3D11Resource* res = nullptr; p.rtv->GetResource(&res);
        D3D11_RESOURCE_DIMENSION dim{}; if (res) res->GetType(&dim);
        D3D11_TEXTURE2D_DESC md{};
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) static_cast<ID3D11Texture2D*>(res)->GetDesc(&md);
        if (res) res->Release();
        D3D11_RENDER_TARGET_VIEW_DESC rvd{}; p.rtv->GetDesc(&rvd);
        D3D11_TEXTURE2D_DESC dd{};
        if (p.depth) { ID3D11Resource* r = nullptr; p.depth->GetResource(&r); if (r) { static_cast<ID3D11Texture2D*>(r)->GetDesc(&dd); r->Release(); } }
        const bool fmtOk = dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && (MaskFormat(md.Format) || MaskFormat(rvd.Format));
        std::lock_guard lk(g_M);
        const std::string key = fmt::format("{}x{}/{}/{}/{}", md.Width, md.Height, (int)md.Format, p.depth ? 1 : 0, (int)p.mask);
        if (g_Logs < 40 && std::find(g_LoggedMasks.begin(), g_LoggedMasks.end(), key) == g_LoggedMasks.end())
        {
            g_Logs++; g_LoggedMasks.push_back(key);
            LOGI("ScreenShadows: eye {}: the game's sun-shadow mask {}x{} format {} (view format {}), written to channels {}{}{}{}{}; the depth {}; the sun's direction {:.2f},{:.2f},{:.2f} ({:.0f} deg up)",
                 p.eye, md.Width, md.Height, (int)md.Format, (int)rvd.Format, (p.mask & 1) ? "R" : "", (p.mask & 2) ? "G" : "", (p.mask & 4) ? "B" : "", (p.mask & 8) ? "A" : "",
                 fmtOk ? "" : " -- NOT a format taken for a mask: nothing drawn",
                 p.depth ? fmt::format("{}x{} format {}", dd.Width, dd.Height, (int)dd.Format) : std::string("NOT found among the resolve's inputs: nothing drawn"),
                 L[0], L[1], L[2], asinf(std::clamp(L[2], -1.f, 1.f)) * 57.29578f);
        }
        if (!fmtOk) { g_BadFormat++; return; }
        if (!p.depth) { g_NoDepthSrv++; return; }
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);
        if (!dev) return;
        if (!g_Ready) Init(dev);
        ID3D11BlendState* blend = g_Ready ? BlendFor(dev, p.mask) : nullptr;
        dev->Release();
        if (!blend) return;
        if (fabsf(L[0] - g_LoggedSun[0]) + fabsf(L[1] - g_LoggedSun[1]) + fabsf(L[2] - g_LoggedSun[2]) > 0.05f)
        {
            memcpy(g_LoggedSun, L, 12);
            LOGI("ScreenShadows: the sun is {:.0f} deg up, towards {:.0f} deg (from cascade 0's matrix)", asinf(L[2]) * 57.29578f, atan2f(L[1], L[0]) * 57.29578f);
        }
        float P[16], V[16]; unsigned frame = 0;
        if (!ReadBytes(P, (const uint8_t*)p.view + kViewProj, 64) || !ReadBytes(V, (const uint8_t*)p.view + kViewMatrix, 64) || !ReadBytes(&frame, (const uint8_t*)p.view + kViewFrame, 4)) return;
        const float contact = g_Contact.load();
        // the boxes of the moving shadow casters (the previous eye-frame's set), and whether any is a small object
        int nBox = 0; bool anySmall = false;
        {
            std::lock_guard lb(g_BoxM);
            nBox = (int)std::min<size_t>(g_BoxReady.size(), kMaxBoxes);
            if (nBox) memcpy(g_BoxUpload, g_BoxReady.data(), sizeof(Box) * nBox);
        }
        for (int i = 0; i < nBox && !anySmall; i++) anySmall = g_BoxUpload[i].v > 0.5f;
        const int eye = p.eye == 1 ? 1 : 0;
        UINT hw = 0, hh = 0;
        ID3D11RenderTargetView* halfRtv = nullptr; ID3D11ShaderResourceView* halfSrv = nullptr;
        ID3D11RenderTargetView* idxRtv = nullptr; ID3D11ShaderResourceView* idxSrv = nullptr; ID3D11DepthStencilView* idxDsv = nullptr;
        ID3D11ShaderResourceView* chars = g_NoChars;
        {
            ID3D11Device* d2 = nullptr; c->GetDevice(&d2);
            if (!d2) return;
            std::lock_guard le(g_EyeM);
            EyeTex& E = g_Eye[eye];
            hw = (md.Width + 1) / 2; hh = (md.Height + 1) / 2;
            if (E.hw != hw || E.hh != hh || !E.half || !E.idx || !E.idxDs)
            {   // the half-resolution targets: the trace (visibility, distance), the box index and its depth
                Rel(E.halfRtv); Rel(E.halfSrv); Rel(E.half); Rel(E.idxRtv); Rel(E.idxSrv); Rel(E.idx); Rel(E.idxDsv); Rel(E.idxDs); E.hw = E.hh = 0;
                D3D11_TEXTURE2D_DESC td{}; td.Width = hw; td.Height = hh; td.MipLevels = td.ArraySize = 1; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                td.Format = DXGI_FORMAT_R16G16_FLOAT;
                bool ok = SUCCEEDED(d2->CreateTexture2D(&td, nullptr, &E.half)) && SUCCEEDED(d2->CreateRenderTargetView(E.half, nullptr, &E.halfRtv))
                          && SUCCEEDED(d2->CreateShaderResourceView(E.half, nullptr, &E.halfSrv));
                td.Format = DXGI_FORMAT_R16_UINT;
                ok = ok && SUCCEEDED(d2->CreateTexture2D(&td, nullptr, &E.idx)) && SUCCEEDED(d2->CreateRenderTargetView(E.idx, nullptr, &E.idxRtv))
                     && SUCCEEDED(d2->CreateShaderResourceView(E.idx, nullptr, &E.idxSrv));
                td.Format = DXGI_FORMAT_D16_UNORM; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
                ok = ok && SUCCEEDED(d2->CreateTexture2D(&td, nullptr, &E.idxDs)) && SUCCEEDED(d2->CreateDepthStencilView(E.idxDs, nullptr, &E.idxDsv));
                if (ok) { E.hw = hw; E.hh = hh; }
                else { Rel(E.halfRtv); Rel(E.halfSrv); Rel(E.half); Rel(E.idxRtv); Rel(E.idxSrv); Rel(E.idx); Rel(E.idxDsv); Rel(E.idxDs); }
            }
            d2->Release();
            halfRtv = E.halfRtv; halfSrv = E.halfSrv; idxRtv = E.idxRtv; idxSrv = E.idxSrv; idxDsv = E.idxDsv;
            if (E.charsSrv && E.marked.load()) chars = E.charsSrv;
        }
        if (!halfRtv || !halfSrv || !idxRtv || !idxSrv || !idxDsv) return;
        const bool anyChar = chars != g_NoChars && g_Chars.load();
        const float casterLen = (anyChar || anySmall) ? g_Len.load() : 0.f;
        if (!(casterLen > 0.f) && !(contact > 0.f)) return;   // nothing on screen casts them this frame
        struct { float P[16]; float V[12]; float L[4]; float g0[4]; float g1[4]; float g2[4]; } k{};
        memcpy(k.P, P, 64); memcpy(k.V, V, 48);
        k.L[0] = L[0]; k.L[1] = L[1]; k.L[2] = L[2]; k.L[3] = g_Strength.load();
        k.g0[0] = casterLen; k.g0[1] = (float)g_Steps.load(); k.g0[2] = (float)md.Width; k.g0[3] = (float)md.Height;
        k.g1[1] = kFadeDist; k.g1[2] = contact;
        k.g2[0] = kCharThick; k.g2[1] = kContactThick; k.g2[2] = (float)hw; k.g2[3] = (float)hh;
        Saved sv; sv.Save(c);
        c->UpdateSubresource(g_Cb, 0, nullptr, &k, 0, 0);
        ID3D11ShaderResourceView* none5[5] = {};
        c->PSSetShaderResources(0, 5, none5);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); c->IASetInputLayout(nullptr);
        c->GSSetShader(nullptr, nullptr, 0);
        c->PSSetConstantBuffers(0, 1, &g_Cb); c->VSSetConstantBuffers(0, 1, &g_Cb);
        c->RSSetState(g_Rs);
        D3D11_VIEWPORT vh{ 0.f, 0.f, (float)hw, (float)hh, 0.f, 1.f };
        c->RSSetViewports(1, &vh);
        // 0: the casters' boxes into the box index (half resolution; the smallest box wins by depth)
        { const UINT zero[4] = {}; c->ClearRenderTargetView(idxRtv, (const float*)zero); }
        c->ClearDepthStencilView(idxDsv, D3D11_CLEAR_DEPTH, 1.f, 0);
        if (nBox)
        {
            c->UpdateSubresource(g_BoxBuf, 0, nullptr, g_BoxUpload, 0, 0);
            c->OMSetRenderTargets(1, &idxRtv, idxDsv);
            c->PSSetShaderResources(0, 1, &p.depth);
            c->VSSetShaderResources(3, 1, &g_BoxSrv); c->PSSetShaderResources(3, 1, &g_BoxSrv);
            c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF); c->OMSetDepthStencilState(g_BoxLess, 0);
            c->VSSetShader(g_BoxVs, nullptr, 0); c->PSSetShader(g_BoxPs, nullptr, 0);
            c->DrawInstanced(36, (UINT)nBox, 0, 0);
            ID3D11ShaderResourceView* nb = nullptr; c->VSSetShaderResources(3, 1, &nb);
            g_BoxesDrawn += (unsigned long long)nBox;
        }
        c->OMSetDepthStencilState(g_NoDepth, 0);
        c->VSSetShader(g_Vs, nullptr, 0);
        // 1: the trace, at half resolution (the target before the inputs: the scene's depth may still be bound for
        // writing; the game reads it here through a read-only depth view)
        c->OMSetRenderTargets(1, &halfRtv, nullptr);
        { ID3D11ShaderResourceView* in[5] = { p.depth, chars, nullptr, g_BoxSrv, idxSrv }; c->PSSetShaderResources(0, 5, in); }
        c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        c->PSSetShader(g_PsTrace, nullptr, 0);
        c->Draw(3, 0);
        // 2: upsampled into the game's mask, MIN into the channels its resolve wrote
        ID3D11RenderTargetView* rtv = p.rtv;
        c->OMSetRenderTargets(1, &rtv, nullptr);
        { ID3D11ShaderResourceView* in[5] = { p.depth, nullptr, halfSrv, nullptr, nullptr }; c->PSSetShaderResources(0, 5, in); }
        D3D11_VIEWPORT vp{ 0.f, 0.f, (float)md.Width, (float)md.Height, 0.f, 1.f };
        c->RSSetViewports(1, &vp);
        c->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
        c->PSSetShader(g_PsApply, nullptr, 0);
        c->Draw(3, 0);
        c->PSSetShaderResources(0, 5, none5);
        sv.Restore(c);
        if (g_Drawn++ == 0) LOGI("ScreenShadows: first screen-space shadows drawn ({}x{}, traced at {}x{}; casters' rays {} m; {} casters' boxes{}; characters {}; contact {})", md.Width, md.Height, hw, hh,
                                 casterLen, nBox, anySmall ? " (small objects among them)" : "", anyChar ? "marked" : "none", contact > 0.f ? fmt::format("{} m", contact) : std::string("off"));
        g_Why = "running";
    }
    void Flush(ID3D11DeviceContext* c)
    {
        Pending p = t_Pend; t_Pend = Pending{};
        if (!p.rtv) return;
        t_Busy = true;
        if (g_On.load(std::memory_order_relaxed) && c && p.view) DrawPending(c, p);
        t_Busy = false;
        Rel(p.rtv); Rel(p.depth);
    }
}

void ScreenShadows::AfterMask(ID3D11DeviceContext* c, const void*, int eye)
{   // the end of the task: a resolve with no draw after it
    if (!t_Busy && t_Pend.rtv) Flush(c);
    if (t_Pend.rtv) { Rel(t_Pend.rtv); Rel(t_Pend.depth); t_Pend = Pending{}; }
    if (c && (eye == 0 || eye == 1))
    {   // the characters' mask is used: cleared for the eye's next depth pass
        ID3D11RenderTargetView* rtv = nullptr;
        { std::lock_guard lk(g_EyeM); if (g_Eye[eye].marked.load()) { rtv = g_Eye[eye].charsRtv; if (rtv) rtv->AddRef(); } g_Eye[eye].marked = false; }
        if (rtv) { const float z[4] = {}; c->ClearRenderTargetView(rtv, z); rtv->Release(); }
    }
}

bool ScreenShadows::CharactersWanted() { return g_On.load() && g_Chars.load(); }
bool ScreenShadows::CharactersInShadowMaps() { return g_CharsInMaps.load(std::memory_order_relaxed); }
void ScreenShadows::NoteCharacterShadowDraw() { g_CharShadowDraws.fetch_add(1, std::memory_order_relaxed); }

void ScreenShadows::SetObjects(bool characters, bool charactersInShadowMaps, float contactM)
{
    g_Chars = characters; g_CharsInMaps = charactersInShadowMaps; g_Contact = std::clamp(contactM, 0.f, 2.f);
    LOGI("ScreenShadows: casting screen-space shadows: {}{}; contact shadows of everything else {}", characters ? "characters (skinned models: people, animals, your hands)" : "no characters",
         characters ? (charactersInShadowMaps ? ", which stay in the sun's shadow maps too" : ", which are left out of the sun's shadow maps") : "",
         g_Contact.load() > 0.f ? fmt::format("within {:g} m", g_Contact.load()) : std::string("off"));
}

bool ScreenShadows::MarkBegin(ID3D11DeviceContext* c, Mark& m)
{
    const int eye = PassProfile::CharEye();
    if (!c || (eye != 0 && eye != 1) || !g_On.load(std::memory_order_relaxed)) return false;
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {}; ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    UINT w = 0, h = 0;
    if (dsv)
    {
        ID3D11Resource* r = nullptr; dsv->GetResource(&r);
        D3D11_RESOURCE_DIMENSION d{}; if (r) r->GetType(&d);
        if (d == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC t{}; static_cast<ID3D11Texture2D*>(r)->GetDesc(&t); if (t.ArraySize == 1 && t.SampleDesc.Count == 1) { w = t.Width; h = t.Height; } }
        if (r) r->Release();
    }
    ID3D11RenderTargetView* mask = nullptr;
    if (w && h)
    {
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);
        if (dev) { if (MarkInit(dev)) mask = CharsRtv(dev, eye, w, h); dev->Release(); }
    }
    if (!mask)
    {
        for (auto*& v : rtv) Rel(v);
        Rel(dsv);
        return false;
    }
    memcpy(m.rtv, rtv, sizeof rtv); m.dsv = dsv;
    c->PSGetShader(&m.ps, nullptr, nullptr);
    c->OMGetDepthStencilState(&m.dss, &m.ref);
    c->OMGetBlendState(&m.bs, m.bf, &m.sampleMask);
    c->OMSetRenderTargets(1, &mask, dsv);
    c->PSSetShader(g_PsMark, nullptr, 0);
    c->OMSetDepthStencilState(g_DepthEqual, 0);
    c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_Eye[eye].marked = true;
    g_Marked.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ScreenShadows::MarkEnd(ID3D11DeviceContext* c, Mark& m)
{
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, m.rtv, m.dsv);
    c->PSSetShader(m.ps, nullptr, 0);
    c->OMSetDepthStencilState(m.dss, m.ref);
    c->OMSetBlendState(m.bs, m.bf, m.sampleMask);
    for (auto*& v : m.rtv) Rel(v);
    Rel(m.dsv); Rel(m.ps); Rel(m.dss); Rel(m.bs);
}

void ScreenShadows::SetCullDistance(float m)
{
    g_Cull = m > 0.f ? std::clamp(m, 5.f, 200.f) : 0.f;
    LOGI("ScreenShadows: shadow casters' distance {}", g_Cull.load() > 0.f ? fmt::format("held at {:g} m (ShadowCullDistance; the game's presets: Low 15, High 32, Ultra 75)", g_Cull.load())
                                                                    : std::string("as the game's shadow quality sets it (ShadowCullDistance = 0)"));
}

void ScreenShadows::AtTaskDraw(ID3D11DeviceContext* c, const void* view, int eye)
{
    if (!c || t_Busy) return;
    if (t_Pend.rtv) Flush(c);           // the resolve is done: the screen-space shadows before this draw (the blur)
    if (!g_On.load(std::memory_order_relaxed)) return;
    ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr);
    if (!ps) return;
    const uint64_t h = GpuTrace::ShaderHashOf(ps);
    ps->Release();
    const bool resolve = IsResolve(h);
    {   // each different pixel shader of the pass once, for the record
        static std::mutex m; static std::vector<uint64_t> seen;
        std::lock_guard lk(m);
        if (seen.size() < 8 && std::find(seen.begin(), seen.end(), h) == seen.end())
        {
            seen.push_back(h);
            LOGI("ScreenShadows: the SCREENSHADOWS pass draws with pixel shader {:016x}{}", h, resolve ? " (the game's sun-shadow resolve: the screen-space shadows go on top of it)" : "");
        }
    }
    if (!resolve) { if (h) g_NoResolve++; return; }
    ID3D11RenderTargetView* rtv = nullptr; c->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) { g_NoTarget++; return; }
    D3D11_TEXTURE2D_DESC md{};
    { ID3D11Resource* r = nullptr; rtv->GetResource(&r); D3D11_RESOURCE_DIMENSION d{}; if (r) { r->GetType(&d); if (d == D3D11_RESOURCE_DIMENSION_TEXTURE2D) static_cast<ID3D11Texture2D*>(r)->GetDesc(&md); r->Release(); } }
    // the scene's depth: a depth-format 2D texture (not the cascades' array) the resolve reads, at the mask's size or its double
    ID3D11ShaderResourceView* depth = nullptr;
    {
        ID3D11ShaderResourceView* srv[16] = {}; c->PSGetShaderResources(0, 16, srv);
        for (int i = 0; i < 16; i++)
        {
            if (!srv[i]) continue;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; srv[i]->GetDesc(&sd);
            ID3D11Resource* r = nullptr; srv[i]->GetResource(&r);
            D3D11_RESOURCE_DIMENSION rd{}; if (r) r->GetType(&rd);
            if (!depth && rd == D3D11_RESOURCE_DIMENSION_TEXTURE2D && sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
            {
                D3D11_TEXTURE2D_DESC t{}; static_cast<ID3D11Texture2D*>(r)->GetDesc(&t);
                if ((DepthFormat(t.Format) || DepthFormat(sd.Format)) && t.ArraySize == 1 && md.Width && (t.Width == md.Width || t.Width == md.Width * 2) && t.Height * md.Width == t.Width * md.Height)
                { depth = srv[i]; depth->AddRef(); }
            }
            if (r) r->Release();
            srv[i]->Release();
        }
    }
    // the channels the resolve writes: ours go into the same ones
    UINT8 mask = 0xF;
    { ID3D11BlendState* bs = nullptr; float bf[4]; UINT sm = 0; c->OMGetBlendState(&bs, bf, &sm); if (bs) { D3D11_BLEND_DESC d{}; bs->GetDesc(&d); mask = d.RenderTarget[0].RenderTargetWriteMask; bs->Release(); } }
    t_Pend.rtv = rtv; t_Pend.depth = depth; t_Pend.view = view; t_Pend.eye = eye; t_Pend.mask = mask;
}

void ScreenShadows::OnShader(void* sh, const void* code, size_t len)
{
    if (!sh || !code || len < 32) return;
    auto has = [&](const char* k) { const size_t n = strlen(k); const char* p = (const char*)code; for (size_t i = 0; i + n <= len; i++) if (!memcmp(p + i, k, n)) return true; return false; };
    if (!has("ro_screenshadows_depthbuffer") || !has("ro_lightdepthcomparemap")) return;
    const uint64_t h = GpuTrace::ShaderHashOf(sh);
    if (!h) return;
    std::lock_guard lk(g_ResM);
    if (std::find(g_Resolve.begin(), g_Resolve.end(), h) != g_Resolve.end() || g_Resolve.size() >= 32) return;
    g_Resolve.push_back(h);
    if (g_Resolve.size() <= 4) LOGI("ScreenShadows: pixel shader {:016x} resolves the sun's shadow maps into the screen's shadow mask", h);
}

// ---- shadow casters by size (research, 2026-10-31: how much a minimum caster size could save) ----
// COLLECT_DYNAMIC_CASTERS (exe+0x330020): the task's own list (task+0: entities, count +8) with, in the task parameter
// whose object's +8 is 0x37F17877, a table at +0x10: bounding spheres (x y z r, 16 bytes each) at +0, a flags byte per
// entity at +0x3C010 (bit 1 a candidate, bit 2 the cull distance applies). The casters go into the lists of the
// parameter whose +8 is 0x963DEA7 (its +0x10: lists at +0, +0xB4010, +0x168020; idList: pointer +0, count +8).
// GATHER_CASTERS (exe+0x330C10): the static world's casters (through Umbra). Every 10 s: the dynamic casters' radii
// as a histogram (within the cull distance from the eye), and the output lists' sizes with a sample of what they hold.
namespace
{
    using TaskFn = uintptr_t (*)(void* task);
    TaskFn g_RealDyn = nullptr, g_RealGather = nullptr;
    std::atomic<double> g_DynAt{ 0 }, g_GatherAt{ 0 };
    std::atomic<int> g_DynLogs{ 0 }, g_GatherLogs{ 0 };
    double NowS() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart / (double)f.QuadPart; }
    template <class T> bool Rd(T& v, const void* p) { return ReadBytes(&v, p, sizeof v); }
    //! the class of a C++ object from its vtable's RTTI (MSVC x64: COL at vtable - 8, its type descriptor RVA at +12)
    std::string ClassOf(const void* obj)
    {
        uintptr_t vt = 0, col = 0; uint32_t td = 0; char name[96] = {};
        const uintptr_t base = GameInfo::Base();
        if (!Rd(vt, obj) || vt < base || vt > base + 0x4000000 || !Rd(col, (const void*)(vt - 8)) || col < base || col > base + 0x4000000
            || !Rd(td, (const void*)(col + 12)) || !ReadBytes(name, (const void*)(base + td + 0x10), sizeof name - 1)) return "?";
        name[sizeof name - 1] = 0;
        return fmt::format("{} (vtable exe+0x{:X})", name, vt - base);
    }
    //! the object a task parameter of this hash points to (task+0xE0: entries of 16 bytes, count +0xE8; *entry +8 = hash)
    const uint8_t* TaskParam(const void* task, uint32_t hash)
    {
        const uint8_t* list = nullptr; uint32_t n = 0;
        if (!Rd(list, (const uint8_t*)task + 0xE0) || !Rd(n, (const uint8_t*)task + 0xE8) || !list) return nullptr;
        n &= 0xFFFFFF;
        for (uint32_t i = 0; i < n && i < 64; i++)
        {
            const uint8_t* p = nullptr; uint32_t h = 0;
            if (!Rd(p, list + 16ull * i) || !p || !Rd(h, p + 8)) continue;
            if (h == hash) { const uint8_t* o = nullptr; return Rd(o, p + 0x10) ? o : nullptr; }
        }
        return nullptr;
    }
    std::string OutLists(const uint8_t* out)
    {
        if (!out) return "no output object";
        std::string s;
        const uint32_t offs[3] = { 0, 0xB4010, 0x168020 };
        for (uint32_t o : offs) { uint32_t c = 0; Rd(c, out + o + 8); s += fmt::format(" +0x{:X}: {}", o, c & 0xFFFFFF); }
        return s;
    }
    uintptr_t HkDyn(void* task)
    {
        const uintptr_t r = g_RealDyn(task);
        const double now = NowS();
        double at = g_DynAt.load();
        if (!Log::Enabled() || now - at < 10.0 || g_DynLogs.load() >= 60 || !g_DynAt.compare_exchange_strong(at, now)) return r;   // a log line only
        g_DynLogs++;
        const uint8_t* list = nullptr; uint32_t n = 0;
        if (!Rd(list, task) || !list || !Rd(n, list + 8)) return r;
        n &= 0xFFFFFF;
        const uint8_t* tbl = TaskParam(task, 0x37F17877);
        const float* sph = nullptr; const uint8_t* flags = nullptr;
        if (!tbl || !Rd(sph, tbl) || !Rd(flags, tbl + 0x3C010) || !sph || !flags) { LOGI("ScreenShadows: dynamic casters: {} entities, the bounds table not found", n); return r; }
        float eye[3] = {}; { float m[16], ax[9]; unsigned long long s = 0; float ha[9]; ViewMatrixHook::RenderView(m, eye, ax, &s, ha); }
        const float cull = CVars::GetFloat("r_shadowCullDistance", 1e9f);
        static const float kEdge[] = { 0.1f, 0.25f, 0.5f, 1.f, 2.f, 4.f };
        int hist[7] = {}, cand = 0, culled = 0;
        for (uint32_t i = 0; i < n && i < 16384; i++)
        {
            uint8_t f = 0; float sp[4];
            if (!Rd(f, flags + i) || !(f & 2) || !ReadBytes(sp, sph + 4ull * i, 16)) continue;
            const float d = sqrtf((sp[0] - eye[0]) * (sp[0] - eye[0]) + (sp[1] - eye[1]) * (sp[1] - eye[1]) + (sp[2] - eye[2]) * (sp[2] - eye[2]));
            if ((f & 4) && d > cull) { culled++; continue; }
            cand++;
            int b = 0; while (b < 6 && sp[3] >= kEdge[b]) b++;
            hist[b]++;
        }
        LOGI("ScreenShadows: dynamic shadow casters this frame: {} candidates within the cull distance ({:g} m; {} beyond it) of {} entities; by bounding radius: <10 cm {}, <25 cm {}, <50 cm {}, <1 m {}, <2 m {}, <4 m {}, more {}; the lists:{}",
             cand, cull, culled, n, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], OutLists(TaskParam(task, 0x963DEA7)));
        return r;
    }
    uintptr_t HkGather(void* task)
    {
        const uintptr_t r = g_RealGather(task);
        const double now = NowS();
        double at = g_GatherAt.load();
        if (now - at < 10.0 || g_GatherLogs.load() >= 3 || !g_GatherAt.compare_exchange_strong(at, now)) return r;
        const int k = g_GatherLogs++;
        const uint8_t* out = TaskParam(task, 0x963DEA7);
        std::string sample;
        if (out && k < 4)
        {   // what the lists hold, for working out each element's bounds: the first elements' class and raw bytes
            const uint32_t offs[3] = { 0, 0xB4010, 0x168020 };
            for (uint32_t o : offs)
            {
                const uint8_t* arr = nullptr; uint32_t c = 0;
                if (!Rd(arr, out + o) || !Rd(c, out + o + 8) || !arr) continue;
                c &= 0xFFFFFF;
                for (uint32_t i = 0; i < c && i < 2; i++)
                {
                    const uint8_t* el = nullptr; if (!Rd(el, arr + 8ull * i) || !el) continue;
                    std::string raw; uint64_t q[12] = {};
                    if (ReadBytes(q, el, sizeof q)) for (int j = 0; j < 12; j++) raw += fmt::format(" {:016X}", q[j]);
                    float fl[16] = {}; std::string fs;
                    if (ReadBytes(fl, el, sizeof fl)) for (int j = 0; j < 16; j++) fs += fmt::format(" {:.3g}", fl[j]);
                    sample += fmt::format("\n    list +0x{:X}[{}] {} {}: qwords{}; as floats{}", o, i, (const void*)el, ClassOf(el), raw, fs);
                }
            }
            // the parameter entries of the task, for the record
            const uint8_t* list = nullptr; uint32_t n = 0;
            if (Rd(list, (const uint8_t*)task + 0xE0) && Rd(n, (const uint8_t*)task + 0xE8) && list)
            {
                std::string ps;
                for (uint32_t i = 0; i < (n & 0xFFFFFF) && i < 16; i++) { const uint8_t* p = nullptr; uint32_t h = 0; if (Rd(p, list + 16ull * i) && p && Rd(h, p + 8)) ps += fmt::format(" {:08X}", h); }
                sample += fmt::format("\n    the task's parameters (hashes):{}", ps);
            }
        }
        LOGI("ScreenShadows: GATHER_CASTERS (the static world's shadow casters) this frame: the lists:{}{}", OutLists(out), sample);
        return r;
    }
}
// ---- the small objects (ScreenSpaceShadowSmallObjects): out of the sun's shadow maps, into the screen-space mask ----
// exe+0x328260(task, 0, params, maxDist, list, out): the sun cascade task's (exe+0x32EE50) pass over the moving shadow
// casters, called for its two lists (+0 and +0xB4010 of the casters' object). The list: idList (pointer +0, count +8
// low 24 bits); its elements 0x30 bytes, +0x10 a pointer to the caster's world box (min x y z at +0, max at +0x10),
// +0x28 a distance compared with maxDist. Here it gets a copy without the casters whose box is under the size; their
// boxes are kept (once per box per frame) for the next screen-space pass.
namespace
{
    using FilterFn = void (*)(void*, void*, void*, float, void*, void*);
    FilterFn g_RealFilter = nullptr;
    struct IdListRaw { const uint8_t* p; uint32_t n; uint32_t cap; };
    thread_local std::vector<uint8_t> t_Kept;
    thread_local std::vector<std::pair<const void*, Box>> t_Cut;

    void HkFilter(void* task, void* b, void* params, float maxDist, void* inList, void* out)
    {
        // what is cut: under the larger of the screen-space small objects' size (with screen-space shadows on) and
        // ShadowMapMinObjectSize; the boxes are only collected for the screen-space shadows
        const bool sss = g_On.load(std::memory_order_relaxed);
        const float R = std::max(sss ? g_SmallR.load(std::memory_order_relaxed) : 0.f, g_MinR.load(std::memory_order_relaxed));
        const bool chars = sss && g_Chars.load(std::memory_order_relaxed);
        const bool boxes = sss && (chars || R > 0.f);
        const uint8_t* p = nullptr; uint32_t n = 0;
        if ((!(R > 0.f) && !chars) || !inList || !Rd(p, inList) || !Rd(n, (const uint8_t*)inList + 8) || !p)
            return g_RealFilter(task, b, params, maxDist, inList, out);
        n &= 0xFFFFFF;
        t_Kept.resize((size_t)n * 0x30);
        t_Cut.clear();
        uint32_t k = 0, cut = 0;
        for (uint32_t i = 0; i < n; i++)
        {
            const uint8_t* el = p + 0x30ull * i;
            const float* bb = nullptr; float v[7];
            bool isSmall = false;
            if (Rd(bb, el + 0x10) && bb && ReadBytes(v, bb, sizeof v))
            {
                const float ex = v[4] - v[0], ey = v[5] - v[1], ez = v[6] - v[2];
                bool ok = ex >= 0 && ey >= 0 && ez >= 0;
                for (float x : v) ok = ok && std::isfinite(x);
                const float r = ok ? 0.5f * sqrtf(ex * ex + ey * ey + ez * ez) : 1e9f;
                isSmall = R > 0.f && r < R;
                if (boxes && r < kMaxBoxR)
                {   // every caster's box up to a size (a character's own box among them); small ones flagged
                    Box box{};
                    box.mn[0] = v[0]; box.mn[1] = v[1]; box.mn[2] = v[2]; box.mx[0] = v[4]; box.mx[1] = v[5]; box.mx[2] = v[6];
                    box.v = isSmall ? 1.f : 0.f;
                    t_Cut.emplace_back(bb, box);
                }
            }
            if (isSmall) { cut++; continue; }
            if (!ReadBytes(t_Kept.data() + 0x30ull * k, el, 0x30)) continue;
            k++;
        }
        g_SmallCut += cut; g_SmallKept += k;
        static std::atomic<int> sampled{ 0 };
        if (cut && sampled.load() < 2 && sampled.fetch_add(1) < 2)
        {   // for checking that the boxes are in the world (near the player), once
            std::string sm; int shown = 0;
            for (size_t i = 0; i < t_Cut.size() && shown < 4; i++)
            {
                const Box& x = t_Cut[i].second;
                if (x.v < 0.5f) continue;
                shown++;
                sm += fmt::format(" [{:.2f},{:.2f},{:.2f} .. {:.2f},{:.2f},{:.2f}]", x.mn[0], x.mn[1], x.mn[2], x.mx[0], x.mx[1], x.mx[2]);
            }
            LOGI("ScreenShadows: small objects: {} of {} casters in a cascade's list under {:g} m across; boxes{}", cut, n, 2.f * R, sm);
        }
        if (!t_Cut.empty())
        {
            int frame = -1; Rd(frame, (const void*)(GameInfo::Base() + 0x32FC64C));
            std::lock_guard lk(g_BoxM);
            if (frame != g_BoxFrame)
            {   // a new frame: the last one's set is what the screen-space pass uses
                g_BoxReady.swap(g_BoxBuild); g_BoxBuild.clear(); g_BoxSeen.clear(); g_BoxFrame = frame;
            }
            for (auto& c : t_Cut)
                if (g_BoxBuild.size() < (size_t)kMaxBoxes && g_BoxSeen.insert(c.first).second) g_BoxBuild.push_back(c.second);
        }
        IdListRaw f{ t_Kept.data(), k, k << 8 };
        g_RealFilter(task, b, params, maxDist, &f, out);
    }
}
void ScreenShadows::InstallSmallObjects()
{
    const bool sss = g_On.load() && (g_SmallR.load() > 0.f || g_Chars.load());
    if (g_RealFilter || (!sss && !(g_MinR.load() > 0.f))) return;
    g_RealFilter = (FilterFn)(GameInfo::Base() + 0x328260);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_RealFilter, (PVOID)HkFilter);
    const LONG e = DetourTransactionCommit();
    if (e != NO_ERROR) g_RealFilter = nullptr;
    const float cutR = std::max(g_On.load() ? g_SmallR.load() : 0.f, g_MinR.load());
    LOGI("ScreenShadows: the sun cascades' moving-caster pass exe+0x328260 hooked (detour={}): {}{}", e,
         cutR > 0.f ? fmt::format("moving objects under {:g} m across are left out of the sun's shadow maps", 2.f * cutR) : std::string("nothing left out of the shadow maps"),
         sss ? fmt::format("; every caster's box (under {:g} m) kept for the screen-space shadows", 2.f * kMaxBoxR) : std::string());
}
void ScreenShadows::SetShadowMapMinSize(float acrossM)
{
    g_MinR = acrossM > 0.f ? std::clamp(acrossM, 0.02f, 2.f) * 0.5f : 0.f;
}
void ScreenShadows::SetSmallObjects(float acrossM)
{
    g_SmallR = acrossM > 0.f ? std::clamp(acrossM, 0.05f, 4.f) * 0.5f : 0.f;
}

void ScreenShadows::InstallCasterLog()
{
    if (g_RealDyn) return;
    g_RealDyn = (TaskFn)(GameInfo::Base() + 0x330020);
    g_RealGather = (TaskFn)(GameInfo::Base() + 0x330C10);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_RealDyn, (PVOID)HkDyn);
    DetourAttach(&(PVOID&)g_RealGather, (PVOID)HkGather);
    const LONG e = DetourTransactionCommit();
    LOGI("ScreenShadows: shadow casters by size logged every 10 s (COLLECT_DYNAMIC_CASTERS exe+0x330020, GATHER_CASTERS exe+0x330C10): detour={}", e);
}


// ---- ShadowSunUpdateEvery: the sun's cascades beyond the nearest redrawn every Nth frame pair ----
// The cascade task (exe+0x32EE50) redraws cascade 0 every frame; with r_shadowParallelPartialSliceUpdate (on in every
// preset) cascades 1..n-2 on odd frames and n-1 on even ones (the frame counter exe+0x32FC64C), the rest copied from
// their caches. The parity test at exe+0x32EF1D (mov eax,[counter]; and eax,0x80000001; jge; dec; or eax,-2; inc: 20
// bytes, nothing jumps into it) is replaced by a call to a thunk: on the pairs to skip it answers so that THIS cascade
// (ebx, the last one being ecx) is not redrawn either way; on the others it gives the parity as before.
namespace
{
    volatile int g_SunMask = 0;                  // N - 1 (N a power of 2), 0 = as the game
    bool g_SunPatched = false;
}
void ScreenShadows::SetSunUpdateEvery(int pairs)
{
    const int n = pairs >= 4 ? 4 : pairs >= 2 ? 2 : 1;
    g_SunMask = n - 1;
}
void ScreenShadows::InstallSunUpdateRate()
{
    if (g_SunPatched || !g_SunMask) return;
    const uintptr_t base = GameInfo::Base();
    // Two places test the frame's parity for the sun's cascades, with the same rule (slice 0 always; with
    // r_shadowParallelPartialSliceUpdate, slices 1..n-2 on odd frames and n-1 on even): the cascade task
    // exe+0x32EE50 (at exe+0x32EF1D: draw the slice, or copy it from its cache) and the cascades' set-up
    // exe+0x32B280 (at exe+0x32B4AC, called from SHADOWS_PREPARE: fit the slice's matrix to the camera, or keep the
    // one its cached map was drawn with). Both are patched alike: patching only the task (the first version) let the
    // set-up move a slice's matrix with the camera while its map stayed the old one -- shadows in the right shape at
    // the wrong place, flickering as the camera moved (reported 2026-10-31).
    struct Site { uint32_t rva; uint8_t disp[4]; const char* what; };
    const Site sites[2] = { { 0x32EF1D, { 0x29, 0xD7, 0xFC, 0x02 }, "the cascade task's" }, { 0x32B4AC, { 0x9A, 0x11, 0xFD, 0x02 }, "the cascades' set-up's" } };
    for (const Site& st : sites)
    {
        uint8_t expect[20] = { 0x8B, 0x05, st.disp[0], st.disp[1], st.disp[2], st.disp[3], 0x25, 0x01, 0x00, 0x00, 0x80, 0x7D, 0x07, 0xFF, 0xC8, 0x83, 0xC8, 0xFE, 0xFF, 0xC0 };
        uint8_t now[20] = {};
        if (!ReadBytes(now, (const void*)(base + st.rva), sizeof now) || memcmp(now, expect, sizeof now))
        { LOGI("ScreenShadows: ShadowSunUpdateEvery: {} frame test at exe+0x{:X} is not as expected (another game version?): not applied", st.what, st.rva); return; }
    }
    // ...and a third copy, twice, in the cascades' split distances (exe+0x330880..: the frame counter in r12d, read at
    // exe+0x330880): a cascade that is redrawn takes this frame's split distance, one that is not keeps the one its map
    // was drawn with. Left alone (the second version), the far cascades took new distances over old maps on the pairs
    // the mask leaves out -- the distant shadows flickered (reported 2026-11-01). Site 1 compares ecx with ebp, site 2
    // ebx with ebp; 17 bytes each (mov eax,r12d; and eax,0x80000001; jge; dec; or eax,-2; inc), followed by test eax,eax.
    struct GSite { uint32_t rva; uint8_t cmpModrm; };
    const GSite gsites[2] = { { 0x330999, 0xE9 }, { 0x3309E1, 0xEB } };
    static const uint8_t kGather[17] = { 0x41, 0x8B, 0xC4, 0x25, 0x01, 0x00, 0x00, 0x80, 0x7D, 0x07, 0xFF, 0xC8, 0x83, 0xC8, 0xFE, 0xFF, 0xC0 };
    for (const GSite& g : gsites)
    {
        uint8_t now[19] = {};
        if (!ReadBytes(now, (const void*)(base + g.rva), sizeof now) || memcmp(now, kGather, 17) || now[17] != 0x85 || now[18] != 0xC0)
        { LOGI("ScreenShadows: ShadowSunUpdateEvery: the split distances' frame test at exe+0x{:X} is not as expected (another game version?): not applied", g.rva); return; }
    }
    // the thunks within 2 GB of the sites (rel32 calls)
    uint8_t* page = nullptr;
    for (uintptr_t a = (base - 0x10000) & ~(uintptr_t)0xFFFF; a > 0x10000 && base - a < 0x7FF00000ull && !page; a -= 0x10000)
        page = (uint8_t*)VirtualAlloc((void*)a, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!page) { LOGI("ScreenShadows: ShadowSunUpdateEvery: no memory near the game for the thunk: not applied"); return; }
    const uint64_t counter = base + 0x32FC64C, mask = (uint64_t)(uintptr_t)&g_SunMask;
    // Each returns eax = the frame's parity (as the replaced code), except on a pair the mask leaves out: then the
    // value that makes the code after it keep every slice but 0 (1 for the last slice, 0 for the others).
    // The task's: the slice in ebx, the last slice's index in ecx.
    uint8_t taskCode[] = {
        0x52, 0x51,                                             // push rdx; push rcx
        0x48, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0,                     // mov rdx, &counter
        0x8B, 0x02,                                             // mov eax, [rdx]
        0x48, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0,                     // mov rdx, &g_SunMask
        0x8B, 0x12,                                             // mov edx, [rdx]
        0x85, 0xD2, 0x74, 0x14,                                 // test edx, edx; jz normal
        0x89, 0xC1, 0xD1, 0xE9, 0x21, 0xD1, 0x74, 0x0C,         // mov ecx, eax; shr ecx, 1; and ecx, edx; jz normal
        0x8B, 0x0C, 0x24,                                       // mov ecx, [rsp]   (the last cascade's index)
        0x31, 0xC0, 0x39, 0xCB, 0x0F, 0x94, 0xC0,               // xor eax, eax; cmp ebx, ecx; sete al
        0xEB, 0x03,                                             // jmp done
        0x83, 0xE0, 0x01,                                       // normal: and eax, 1
        0x59, 0x5A, 0xC3 };                                     // done: pop rcx; pop rdx; ret
    memcpy(taskCode + 4, &counter, 8); memcpy(taskCode + 16, &mask, 8);
    // The set-up's: the slice in r14d, the last slice's index in edx.
    uint8_t setupCode[] = {
        0x51, 0x52,                                             // push rcx; push rdx
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,                     // mov rcx, &counter
        0x8B, 0x01,                                             // mov eax, [rcx]
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,                     // mov rcx, &g_SunMask
        0x8B, 0x09,                                             // mov ecx, [rcx]
        0x85, 0xC9, 0x74, 0x15,                                 // test ecx, ecx; jz normal
        0x89, 0xC2, 0xD1, 0xEA, 0x21, 0xCA, 0x74, 0x0D,         // mov edx, eax; shr edx, 1; and edx, ecx; jz normal
        0x8B, 0x14, 0x24,                                       // mov edx, [rsp]   (the last cascade's index)
        0x31, 0xC0, 0x44, 0x39, 0xF2, 0x0F, 0x94, 0xC0,         // xor eax, eax; cmp edx, r14d; sete al
        0xEB, 0x03,                                             // jmp done
        0x83, 0xE0, 0x01,                                       // normal: and eax, 1
        0x5A, 0x59, 0xC3 };                                     // done: pop rdx; pop rcx; ret
    memcpy(setupCode + 4, &counter, 8); memcpy(setupCode + 16, &mask, 8);
    memcpy(page, taskCode, sizeof taskCode);
    memcpy(page + 128, setupCode, sizeof setupCode);
    // The split distances' (both sites alike but for the compare): eax = the parity from r12d, or on a pair left out
    // 1 when the compared slice is the last (ecx / ebx == ebp), else 0 -- every slice but 0 keeps its distance.
    for (int i = 0; i < 2; i++)
    {
        uint8_t gc[] = {
            0x52,                                               // push rdx
            0x48, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0,                 // mov rdx, &g_SunMask
            0x8B, 0x12,                                         // mov edx, [rdx]
            0x85, 0xD2, 0x74, 0x12,                             // test edx, edx; jz normal
            0x44, 0x89, 0xE0, 0xD1, 0xE8, 0x21, 0xD0, 0x74, 0x09,   // mov eax, r12d; shr eax, 1; and eax, edx; jz normal
            0x31, 0xC0, 0x39, gsites[i].cmpModrm, 0x0F, 0x94, 0xC0, // xor eax, eax; cmp ecx|ebx, ebp; sete al
            0xEB, 0x06,                                         // jmp done
            0x44, 0x89, 0xE0, 0x83, 0xE0, 0x01,                 // normal: mov eax, r12d; and eax, 1
            0x5A, 0xC3 };                                       // done: pop rdx; ret
        memcpy(gc + 3, &mask, 8);
        memcpy(page + 256 + i * 64, gc, sizeof gc);
    }
    FlushInstructionCache(GetCurrentProcess(), page, 512);
    const uint8_t* thunks[2] = { page, page + 128 };
    uint8_t patch[2][20];
    for (int i = 0; i < 2; i++)
    {
        uint8_t* site = (uint8_t*)(base + sites[i].rva);
        const long long rel = (long long)(uintptr_t)thunks[i] - (long long)(uintptr_t)(site + 5);
        if (rel > INT32_MAX || rel < INT32_MIN) { VirtualFree(page, 0, MEM_RELEASE); LOGI("ScreenShadows: ShadowSunUpdateEvery: the thunk is out of reach: not applied"); return; }
        patch[i][0] = 0xE8; const int32_t r32 = (int32_t)rel; memcpy(patch[i] + 1, &r32, 4);
        for (int k = 5; k < 20; k++) patch[i][k] = 0x90;
    }
    for (int i = 0; i < 2; i++)
    {   // the split distances first (they run before the set-up and the task): E8 rel32 + 12 NOPs
        uint8_t* site = (uint8_t*)(base + gsites[i].rva);
        const long long rel = (long long)(uintptr_t)(page + 256 + i * 64) - (long long)(uintptr_t)(site + 5);
        if (rel > INT32_MAX || rel < INT32_MIN) { LOGI("ScreenShadows: ShadowSunUpdateEvery: the thunk is out of reach: not applied"); return; }
        uint8_t gp[17]; gp[0] = 0xE8; const int32_t r32 = (int32_t)rel; memcpy(gp + 1, &r32, 4);
        for (int k = 5; k < 17; k++) gp[k] = 0x90;
        DWORD old = 0;
        if (!VirtualProtect(site, 17, PAGE_EXECUTE_READWRITE, &old)) { LOGI("ScreenShadows: ShadowSunUpdateEvery: exe+0x{:X} cannot be written: not applied", gsites[i].rva); return; }
        memcpy(site, gp, 17);
        VirtualProtect(site, 17, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 17);
    }
    for (int i = 0; i < 2; i++)
    {   // the set-up's first: it runs earlier in the frame than the task
        const int k = 1 - i;
        uint8_t* site = (uint8_t*)(base + sites[k].rva);
        DWORD old = 0;
        if (!VirtualProtect(site, 20, PAGE_EXECUTE_READWRITE, &old))
        { LOGI("ScreenShadows: ShadowSunUpdateEvery: exe+0x{:X} cannot be written: {}", sites[k].rva, k == 1 ? "not applied" : "only the set-up patched (no effect)"); return; }
        memcpy(site, patch[k], 20);
        VirtualProtect(site, 20, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 20);
    }
    g_SunPatched = true;
    LOGI("ScreenShadows: ShadowSunUpdateEvery = {}: the sun's cascades beyond the nearest are redrawn every {} frame pairs (the nearest, every frame; the rest "
         "copied from their caches in between, with the matrices and split distances they were drawn with: exe+0x32EF1D, exe+0x32B4AC, exe+0x330999, exe+0x3309E1)", g_SunMask + 1, g_SunMask + 1);
}

void ScreenShadows::StartSettingsLog()
{
    static bool started = false;
    if (started || !CVars::Available()) return;
    started = true;
    std::thread(SettingsLoop).detach();
    LOGI("ScreenShadows: the game's shadow settings are logged now and whenever one changes (change the shadow quality in the game's menu to compare)");
}

std::string ScreenShadows::Status()
{
    return fmt::format("screen-space shadows {}: drawn {}, no sun {}, no target {}, no depth {}, not a mask format {}, the pass's other draws {}, characters marked {}, character draws in the sun's maps {} ({})", g_On.load() ? "on" : "off",
                       g_Drawn.load(), g_NoSun.load(), g_NoTarget.load(), g_NoDepthSrv.load(), g_BadFormat.load(), g_NoResolve.load(), g_Marked.load(), g_CharShadowDraws.load(), g_Why);
}

void ScreenShadows::SetLampCacheSlices(int n)
{
    g_LampCache = n < 0 ? 0 : (n > 128 ? 128 : n);
    LOGI("ScreenShadows: ShadowLampCacheSlices {}", g_LampCache.load() ? fmt::format("{}: the lamps' shadow cache holds {} slices of 2048 x 2048 (the game: 12)", g_LampCache.load(), g_LampCache.load())
                                                                      : std::string("0: the game's lamp shadow cache"));
}
void ScreenShadows::SetLampFewerUpdates(bool on)
{
    g_LampFewer = on;
    LOGI("ScreenShadows: ShadowLampFewerUpdates {}", on ? "on: the lamps redraw moving casters' shadows only when the lamp covers 70 % of the screen or is within 8 m (Low's thresholds, at any preset)"
                                                        : "off: the preset's lamp update thresholds");
}
void ScreenShadows::SetUltraTrim(bool on)
{
    g_Trim = on;
    LOGI("ScreenShadows: ShadowUltraTrim {}", on ? "on: at Ultra shadow quality, the sun's dynamic-caster updates and the lamps' update thresholds are held at High's values"
                                                 : "off: Ultra's shadow settings as the game sets them");
}
