#include "pch.h"
#include "Fog.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <mutex>
#include <vector>
#include <cmath>

namespace
{
    // ---- the shaders ----
    // PerView: the TAA's own PerViewCB (800 bytes), as float4s; the projection's first row at float4 index g3.x.
    // Either matrix convention is handled: the row or column holding (0, 0, +-1, 0) says which.
    const char* kHlsl = R"(
cbuffer PerView : register(b0) { float4 pv[50]; }
cbuffer FogCB : register(b1)
{
    float4 g0;   // start (m), 1 / (end - start), opacity, use the manual colour (0 / 1)
    float4 g1;   // manual colour (linear), unused
    float4 g2;   // width, height, sample step of the sky average, the sky colour's blend per frame
    float4 g3;   // projection row index in PerView, groups of the sky average in x, total groups, unused
};
Texture2D<float4> Color : register(t0);
Texture2D<float> Depth : register(t1);
StructuredBuffer<float4> FogColorIn : register(t2);
RWTexture2D<float4> Out : register(u0);
RWStructuredBuffer<float4> Partial : register(u1);
RWStructuredBuffer<float4> FogColorOut : register(u2);

// the distance (m) along the eye's ray to what a pixel shows; 0 = the sky: the depth buffer's clear value (no
// geometry), or nothing nearer than 20 km
float Distance(uint2 p, float depth)
{
    uint r = (uint)g3.x;
    float4 R0 = pv[r], R1 = pv[r + 1], R2 = pv[r + 2], R3 = pv[r + 3];
    bool colVec = abs(abs(R3.z) - 1) < 1e-3;
    float s = colVec ? R3.z : R2.w;
    float a = R2.z;
    float b = colVec ? R2.w : R3.z;
    float offx = colVec ? R0.z : R2.x, offy = colVec ? R1.z : R2.y;
    float cleared = saturate(a * s) > 0.5 ? 1.0 : 0.0;   // depth at infinity: 1 (standard) or 0 (reversed)
    if (depth == cleared) return 0;
    float den = depth - a * s;
    if (abs(den) < 1e-12) return 0;
    float d = b / den;
    if (!(d > 0) || d > 20000) return 0;
    float2 ndc = float2((p.x + 0.5) / g2.x * 2 - 1, 1 - (p.y + 0.5) / g2.y * 2);
    float rx = (ndc.x * s - offx) / R0.x, ry = (ndc.y * s - offy) / R1.y;
    return d * sqrt(1 + rx * rx + ry * ry);
}

groupshared float4 gs[256];
[numthreads(16, 16, 1)]
void sky(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    uint2 p = (gid.xy * 16 + tid.xy) * (uint)g2.z;
    float4 acc = 0;
    if (p.x < (uint)g2.x && p.y < (uint)g2.y && Distance(p, Depth[p]) == 0)
    {
        float3 c = min(Color[p].rgb, 64.0);
        float w = 1 + 3 * (p.y / g2.y);   // towards the bottom of the screen: the sky near the horizon
        if (all(c == c)) acc = float4(c * w, w);
    }
    gs[gi] = acc;
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 128; k > 0; k >>= 1)
    {
        if (gi < k) gs[gi] += gs[gi + k];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0) Partial[gid.y * (uint)g3.y + gid.x] = gs[0];
}

[numthreads(256, 1, 1)]
void skyfinal(uint gi : SV_GroupIndex)
{
    float4 acc = 0;
    for (uint i = gi; i < (uint)g3.z; i += 256) acc += Partial[i];
    gs[gi] = acc;
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 128; k > 0; k >>= 1)
    {
        if (gi < k) gs[gi] += gs[gi + k];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0)
    {
        float4 prev = FogColorOut[0];
        float4 t = gs[0];
        if (t.w > 4)   // enough sky in view: move towards it; else keep what it was
        {
            float3 c = t.rgb / t.w;
            FogColorOut[0] = prev.w > 0 ? float4(lerp(prev.rgb, c, g2.w), 1) : float4(c, 1);
        }
    }
}

[numthreads(8, 8, 1)]
void fog(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)g2.x || id.y >= (uint)g2.y) return;
    float4 c = Color[id.xy];
    float d = Distance(id.xy, Depth[id.xy]);
    float4 fc = FogColorIn[0];
    float3 col = fc.rgb;
    if (g0.w > 0.5)
    {   // the manual colour, at the sky's brightness (the scene's values are in the game's exposure)
        float ls = dot(fc.rgb, float3(0.2126, 0.7152, 0.0722));
        float lm = max(dot(g1.rgb, float3(0.2126, 0.7152, 0.0722)), 1e-4);
        col = g1.rgb * (fc.w > 0 ? ls / lm : 1);
    }
    float f = 0;
    if (d > 0 && fc.w > 0)
    {
        float t = saturate((d - g0.x) * g0.y);
        f = g0.z * t * t * (3 - 2 * t);
    }
    Out[id.xy] = float4(lerp(c.rgb, col, f), c.a);
}
)";

    std::atomic<bool> g_Want{ true }, g_GameFogOff{ false };
    std::mutex g_PM; Fog::Params g_P;

    std::mutex g_M;                              // everything below
    bool g_Failed = false, g_Ready = false;
    ID3D11Device* g_Dev = nullptr;
    ID3D11ComputeShader* g_CsSky = nullptr; ID3D11ComputeShader* g_CsFinal = nullptr; ID3D11ComputeShader* g_CsFog = nullptr;
    ID3D11Buffer* g_Cb = nullptr;
    // per size / format
    UINT g_W = 0, g_H = 0; DXGI_FORMAT g_Fmt = DXGI_FORMAT_UNKNOWN;
    ID3D11Texture2D* g_Out = nullptr; ID3D11UnorderedAccessView* g_OutUav = nullptr; ID3D11ShaderResourceView* g_OutSrv = nullptr;
    ID3D11Buffer* g_Partial = nullptr; ID3D11UnorderedAccessView* g_PartialUav = nullptr; UINT g_GroupsX = 0, g_GroupsY = 0;
    ID3D11Buffer* g_Final = nullptr; ID3D11UnorderedAccessView* g_FinalUav = nullptr; ID3D11ShaderResourceView* g_FinalSrv = nullptr;
    constexpr UINT kSkyStep = 8;
    // the scene colour targets the TAA reads, and the depth buffer drawn with each
    struct Pair { ID3D11Resource* color = nullptr; ID3D11Texture2D* depth = nullptr; ID3D11ShaderResourceView* depthSrv = nullptr; UINT w = 0, h = 0; };
    Pair g_Pairs[4];
    std::atomic<ID3D11Resource*> g_Known[4];     // colour targets seen at the TAA (for OnDraw, lock-free)
    std::atomic<bool> g_NeedDepth{ false };
    std::atomic<unsigned long long> g_Applied{ 0 }, g_NoDepth{ 0 }, g_DepthLearned{ 0 }, g_Skipped{ 0 };
    std::string g_Why = "not seen yet";

    template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

    bool Init(ID3D11Device* dev)
    {
        if (g_Ready || g_Failed) return g_Ready;
        g_Failed = true;
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
        if (!compile) { g_Why = "d3dcompiler_47 not available"; LOGI("Fog: {}", g_Why); return false; }
        auto make = [&](const char* entry, ID3D11ComputeShader** out) -> bool
        {
            ID3DBlob* b = nullptr; ID3DBlob* err = nullptr;
            if (FAILED(compile(kHlsl, strlen(kHlsl), "fog", nullptr, nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err)))
            {
                LOGI("Fog: shader '{}' failed to compile: {}", entry, err ? (const char*)err->GetBufferPointer() : "?");
                if (err) err->Release(); return false;
            }
            const HRESULT hr = dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, out);
            b->Release(); if (err) err->Release();
            return SUCCEEDED(hr) && *out;
        };
        if (!make("sky", &g_CsSky) || !make("skyfinal", &g_CsFinal) || !make("fog", &g_CsFog)) { g_Why = "shader compile failed"; return false; }
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_Cb))) { g_Why = "constant buffer"; return false; }
        // the fog colour: one float4, written by skyfinal (UAV) and read by fog (SRV); w = 0 until the sky was seen
        D3D11_BUFFER_DESC fd{}; fd.ByteWidth = 16; fd.Usage = D3D11_USAGE_DEFAULT; fd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        fd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; fd.StructureByteStride = 16;
        const float zero[4] = {}; D3D11_SUBRESOURCE_DATA sd{ zero, 0, 0 };
        if (FAILED(dev->CreateBuffer(&fd, &sd, &g_Final))) { g_Why = "fog colour buffer"; return false; }
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = 1;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = DXGI_FORMAT_UNKNOWN; vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; vd.Buffer.NumElements = 1;
        if (FAILED(dev->CreateUnorderedAccessView(g_Final, &ud, &g_FinalUav)) || FAILED(dev->CreateShaderResourceView(g_Final, &vd, &g_FinalSrv)))
        { g_Why = "fog colour views"; return false; }
        g_Dev = dev; g_Dev->AddRef();
        g_Failed = false; g_Ready = true;
        LOGI("Fog: shaders ready (sky colour, fog)");
        return true;
    }
    bool Sized(UINT w, UINT h, DXGI_FORMAT fmt)
    {
        if (g_Out && w == g_W && h == g_H && fmt == g_Fmt) return true;
        Rel(g_OutSrv); Rel(g_OutUav); Rel(g_Out); Rel(g_PartialUav); Rel(g_Partial);
        DXGI_FORMAT tf = fmt;
        UINT sup = 0;
        if (FAILED(g_Dev->CheckFormatSupport(fmt, &sup)) || !(sup & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) tf = DXGI_FORMAT_R16G16B16A16_FLOAT;
        D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = tf; td.SampleDesc = { 1, 0 };
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_Dev->CreateTexture2D(&td, nullptr, &g_Out)) || FAILED(g_Dev->CreateUnorderedAccessView(g_Out, nullptr, &g_OutUav))
            || FAILED(g_Dev->CreateShaderResourceView(g_Out, nullptr, &g_OutSrv)))
        { LOGI("Fog: the fog texture {}x{} (format {}) could not be made", w, h, (int)tf); Rel(g_OutSrv); Rel(g_OutUav); Rel(g_Out); return false; }
        g_GroupsX = (w + 16 * kSkyStep - 1) / (16 * kSkyStep); g_GroupsY = (h + 16 * kSkyStep - 1) / (16 * kSkyStep);
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16 * g_GroupsX * g_GroupsY; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 16;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = g_GroupsX * g_GroupsY;
        if (FAILED(g_Dev->CreateBuffer(&bd, nullptr, &g_Partial)) || FAILED(g_Dev->CreateUnorderedAccessView(g_Partial, &ud, &g_PartialUav)))
        { LOGI("Fog: the sky buffer could not be made"); Rel(g_PartialUav); Rel(g_Partial); return false; }
        g_W = w; g_H = h; g_Fmt = fmt;
        LOGI("Fog: fog texture {}x{} (scene format {}, the fog's {}), sky colour from {}x{} groups of samples", w, h, (int)fmt, (int)tf, g_GroupsX, g_GroupsY);
        return true;
    }
    DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }
    Pair* PairOf(ID3D11Resource* color)
    {
        for (Pair& p : g_Pairs) if (p.color == color) return &p;
        return nullptr;
    }
}

void Fog::SetParams(const Params& p)
{
    { std::lock_guard lk(g_PM); g_P = p; }
    g_Want = p.on;
    if (p.on) LOGI("Fog: the mod's own fog when the game's is off (CheapFog): from {} m, full at {} m, opacity {:.2f}, colour {}", p.start, p.end, p.opacity,
                   p.sky ? std::string("of the sky") : fmt::format("{:.2f},{:.2f},{:.2f} (linear) at the sky's brightness", p.rgb[0], p.rgb[1], p.rgb[2]));
}

void Fog::SetGameFogOff(bool off)
{
    if (g_GameFogOff.exchange(off) != off && g_Want.load())
        LOGI("Fog: the mod's fog {}", off ? "ON (the game's fog is off)" : "off (the game's own fog is on)");
}

void Fog::OnDraw(ID3D11DeviceContext* c)
{
    if (!g_NeedDepth.load(std::memory_order_relaxed)) return;
    static thread_local unsigned n = 0;
    if ((++n & 7) != 0) return;                  // one draw in eight is plenty: the scene target takes thousands
    ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(1, &rtv, &dsv);
    if (rtv && dsv)
    {
        ID3D11Resource* color = nullptr; rtv->GetResource(&color);
        bool known = false;
        for (auto& k : g_Known) if (k.load() == color) known = true;
        if (known)
        {
            ID3D11Resource* dr = nullptr; dsv->GetResource(&dr);
            D3D11_RESOURCE_DIMENSION dim{}; if (dr) dr->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
            {
                ID3D11Texture2D* dt = static_cast<ID3D11Texture2D*>(dr);
                D3D11_TEXTURE2D_DESC dd{}; dt->GetDesc(&dd);
                D3D11_TEXTURE2D_DESC cd{}; static_cast<ID3D11Texture2D*>(color)->GetDesc(&cd);
                std::lock_guard lk(g_M);
                Pair* p = PairOf(color);
                if (p && !p->depth && dd.Width == cd.Width && dd.Height == cd.Height)
                {
                    const DXGI_FORMAT sf = DepthSrvFormat(dd.Format);
                    if (!(dd.BindFlags & D3D11_BIND_SHADER_RESOURCE) || sf == DXGI_FORMAT_UNKNOWN || dd.SampleDesc.Count > 1)
                    {
                        static int told = 0;
                        if (told++ < 3) LOGI("Fog: the scene's depth buffer ({}x{} format {}, bind 0x{:X}, samples {}) cannot be read by a shader: no fog",
                                             dd.Width, dd.Height, (int)dd.Format, dd.BindFlags, dd.SampleDesc.Count);
                        g_Why = "depth buffer not readable";
                    }
                    else
                    {
                        D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = sf; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
                        if (dd.ArraySize > 1) { vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY; vd.Texture2DArray.ArraySize = 1; vd.Texture2DArray.MipLevels = 1; }
                        if (g_Dev && SUCCEEDED(g_Dev->CreateShaderResourceView(dt, &vd, &p->depthSrv)))
                        {
                            p->depth = dt; dt->AddRef(); g_DepthLearned++;
                            LOGI("Fog: the scene's depth buffer: {}x{} format {} (read as {}), drawn with the colour target {}", dd.Width, dd.Height,
                                 (int)dd.Format, (int)sf, (void*)color);
                        }
                    }
                    bool all = true; for (Pair& q : g_Pairs) if (q.color && !q.depth) all = false;
                    if (all) g_NeedDepth = false;
                }
            }
            if (dr) dr->Release();
        }
        if (color) color->Release();
    }
    if (rtv) rtv->Release();
    if (dsv) dsv->Release();
}

ID3D11ShaderResourceView* Fog::BeforeTaa(ID3D11DeviceContext* c, int colorSlot, int viewCbSlot, int projOffset)
{
    if (!g_Want.load() || !g_GameFogOff.load() || colorSlot < 0 || viewCbSlot < 0 || projOffset < 0 || (projOffset & 15)) return nullptr;
    ID3D11ShaderResourceView* src = nullptr; c->CSGetShaderResources(colorSlot, 1, &src);
    if (!src) return nullptr;
    ID3D11Resource* color = nullptr; src->GetResource(&color);
    D3D11_RESOURCE_DIMENSION dim{}; if (color) color->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) { if (color) color->Release(); src->Release(); return nullptr; }
    D3D11_TEXTURE2D_DESC cd{}; static_cast<ID3D11Texture2D*>(color)->GetDesc(&cd);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; src->GetDesc(&sd);
    Params P; { std::lock_guard lk(g_PM); P = g_P; }

    std::unique_lock lk(g_M);
    if (!g_Ready)
    {
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);
        if (dev) { Init(dev); dev->Release(); }
        if (!g_Ready) { color->Release(); src->Release(); return nullptr; }
    }
    Pair* pr = PairOf(color);
    if (!pr)
    {   // a new scene colour target: remember it, and learn its depth buffer from the draws
        for (int i = 0; i < 4 && !pr; i++) if (!g_Pairs[i].color) { pr = &g_Pairs[i]; pr->color = color; color->AddRef(); g_Known[i] = color; pr->w = cd.Width; pr->h = cd.Height; }
        if (!pr)
        {   // more than four: start again
            for (int i = 0; i < 4; i++) { Rel(g_Pairs[i].color); Rel(g_Pairs[i].depthSrv); Rel(g_Pairs[i].depth); g_Known[i] = nullptr; }
            pr = &g_Pairs[0]; pr->color = color; color->AddRef(); g_Known[0] = color; pr->w = cd.Width; pr->h = cd.Height;
        }
        LOGI("Fog: the TAA reads the scene from {} ({}x{} format {}): looking for its depth buffer", (void*)color, cd.Width, cd.Height, (int)cd.Format);
        g_NeedDepth = true;
    }
    if (!pr->depthSrv) { g_NoDepth++; g_NeedDepth = true; color->Release(); src->Release(); return nullptr; }
    if (!Sized(cd.Width, cd.Height, sd.Format)) { color->Release(); src->Release(); return nullptr; }

    // the constants
    struct { float g0[4], g1[4], g2[4], g3[4]; } k{};
    const float range = std::max(1.f, P.end - P.start);
    k.g0[0] = P.start; k.g0[1] = 1.f / range; k.g0[2] = std::min(1.f, std::max(0.f, P.opacity)); k.g0[3] = P.sky ? 0.f : 1.f;
    for (int i = 0; i < 3; i++) k.g1[i] = P.rgb[i];
    k.g2[0] = (float)cd.Width; k.g2[1] = (float)cd.Height; k.g2[2] = (float)kSkyStep; k.g2[3] = 0.03f;
    k.g3[0] = (float)(projOffset / 16); k.g3[1] = (float)g_GroupsX; k.g3[2] = (float)(g_GroupsX * g_GroupsY);
    c->UpdateSubresource(g_Cb, 0, nullptr, &k, 0, 0);

    // save what the compute stage has bound in the slots used here
    ID3D11ComputeShader* sCs = nullptr; ID3D11ClassInstance* sCi[256]; UINT sCiN = 256;
    c->CSGetShader(&sCs, sCi, &sCiN);
    ID3D11Buffer* sCb[2] = {}; c->CSGetConstantBuffers(0, 2, sCb);
    ID3D11ShaderResourceView* sSrv[3] = {}; c->CSGetShaderResources(0, 3, sSrv);
    ID3D11UnorderedAccessView* sUav[3] = {}; c->CSGetUnorderedAccessViews(0, 3, sUav);
    ID3D11Buffer* view = nullptr; c->CSGetConstantBuffers(viewCbSlot, 1, &view);

    ID3D11Buffer* cbs[2] = { view, g_Cb };
    c->CSSetConstantBuffers(0, 2, cbs);
    ID3D11UnorderedAccessView* none3[3] = {};
    // 1) the sky's colour
    {
        ID3D11ShaderResourceView* srv[3] = { src, pr->depthSrv, nullptr };
        c->CSSetShaderResources(0, 3, srv);
        ID3D11UnorderedAccessView* uav[3] = { nullptr, g_PartialUav, g_FinalUav };
        c->CSSetUnorderedAccessViews(0, 3, uav, nullptr);
        c->CSSetShader(g_CsSky, nullptr, 0);
        c->Dispatch(g_GroupsX, g_GroupsY, 1);
        c->CSSetShader(g_CsFinal, nullptr, 0);
        c->Dispatch(1, 1, 1);
        c->CSSetUnorderedAccessViews(0, 3, none3, nullptr);
    }
    // 2) the fog, into the mod's texture
    {
        ID3D11ShaderResourceView* srv[3] = { src, pr->depthSrv, g_FinalSrv };
        c->CSSetShaderResources(0, 3, srv);
        ID3D11UnorderedAccessView* uav[3] = { g_OutUav, nullptr, nullptr };
        c->CSSetUnorderedAccessViews(0, 3, uav, nullptr);
        c->CSSetShader(g_CsFog, nullptr, 0);
        c->Dispatch((cd.Width + 7) / 8, (cd.Height + 7) / 8, 1);
        c->CSSetUnorderedAccessViews(0, 3, none3, nullptr);
    }
    // put the compute stage back as it was
    c->CSSetShader(sCs, sCiN ? sCi : nullptr, sCiN);
    c->CSSetConstantBuffers(0, 2, sCb);
    c->CSSetShaderResources(0, 3, sSrv);
    c->CSSetUnorderedAccessViews(0, 3, sUav, nullptr);
    if (sCs) sCs->Release();
    for (UINT i = 0; i < sCiN; i++) if (sCi[i]) sCi[i]->Release();
    for (auto* p : sCb) if (p) p->Release();
    for (auto* p : sSrv) if (p) p->Release();
    for (auto* p : sUav) if (p) p->Release();
    if (view) view->Release();

    // and the TAA reads the fogged scene
    ID3D11ShaderResourceView* mine = g_OutSrv;
    c->CSSetShaderResources(colorSlot, 1, &mine);
    g_Applied++;
    g_Why = "running";
    color->Release();
    return src;                                  // the engine's view (AddRef'd by CSGetShaderResources): put back after the dispatch
}

std::string Fog::Status()
{
    std::lock_guard lk(g_M);
    return fmt::format("fog want {} gamefogoff {} ready {} applied {} nodepth {} depths {} ({})", g_Want.load() ? 1 : 0, g_GameFogOff.load() ? 1 : 0,
                       g_Ready ? 1 : 0, g_Applied.load(), g_NoDepth.load(), g_DepthLearned.load(), g_Why);
}
