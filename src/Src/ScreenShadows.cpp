#include "pch.h"
#include "ScreenShadows.h"
#include "CVars.h"
#include "GameInfo.h"
#include "GpuTrace.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <mutex>
#include <thread>
#include <vector>
#include <cmath>
#include <algorithm>

namespace
{
    // ---- the shader ----
    // P: the view's projection (view+0x7390) as stored (either matrix convention, as Fog reads it). V: its view matrix
    // (view+0x7450) rows -left / up / -forward with the translation in w. L: towards the sun, world (z up).
    const char* kHlsl = R"(
cbuffer C : register(b0)
{
    float4 P[4];
    float4 V[3];
    float4 L;       // xyz towards the sun (world), w: how much the shadow darkens (0..1)
    float4 g0;      // ray length (m), steps, mask width, mask height
    float4 g1;      // frame (for the jitter), fade-out distance (m), 0, 0
};
Texture2D<float> Depth : register(t0);

struct VO { float4 pos : SV_Position; };
VO vs(uint id : SV_VertexID)
{
    VO o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

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
float Ign(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }

float4 ps(VO i) : SV_Target
{
    float s, a, b, ox, oy, sx, sy; Terms(s, a, b, ox, oy, sx, sy);
    uint dw, dh; Depth.GetDimensions(dw, dh);
    float2 dsz = float2(dw, dh);
    float2 uv = i.pos.xy / g0.zw;
    float f = Fwd(Depth.Load(int3(min(uv * dsz, dsz - 1), 0)), s, a, b);
    if (!(f > 0) || f > g1.y) return 1;
    // the pixel in the world: the eye + its ray (view space, forward = 1) x the forward distance
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float3 r = float3(s * (ndc.x * s - ox) / sx, s * (ndc.y * s - oy) / sy, -1);
    float3 eye = -(V[0].w * V[0].xyz + V[1].w * V[1].xyz + V[2].w * V[2].xyz);
    float3 pos = eye + (r.x * V[0].xyz + r.y * V[1].xyz + r.z * V[2].xyz) * f;
    int n = (int)g0.y;
    float j = Ign(i.pos.xy + g1.x * 5.588238);
    float start = 0.04 + 0.002 * f;            // off the surface itself
    float occ = 0;
    [loop] for (int k = 0; k < n; k++)
    {
        float t = start + (g0.x - start) * pow((k + j) / n, 1.6);   // denser near the surface
        float3 q = pos + L.xyz * t;
        float3 v = float3(dot(V[0].xyz, q) + V[0].w, dot(V[1].xyz, q) + V[1].w, dot(V[2].xyz, q) + V[2].w);
        float fq = -v.z;
        if (!(fq > 0.05)) break;
        float2 qn = float2(v.x / fq * sx + ox * s, v.y / fq * sy + oy * s);
        float2 qu = float2(qn.x * 0.5 + 0.5, 0.5 - qn.y * 0.5);
        if (any(qu < 0) || any(qu > 1)) break;      // off screen: the game's shadow maps alone
        float fs = Fwd(Depth.Load(int3(min(qu * dsz, dsz - 1), 0)), s, a, b);
        if (!(fs > 0)) continue;                     // the sky there
        float dz = fq - fs;                          // the ray point behind what the screen shows there
        float bias = 0.03 + 0.004 * fq, thick = max(0.4, 0.08 * fq);
        if (dz > bias && dz < thick) { occ = 1; break; }
    }
    // thinner with distance (the shadow maps take over), and at the screen's edges
    float2 e = min(uv, 1 - uv);
    float fade = saturate(1 - f / g1.y) * saturate(min(e.x, e.y) * 20);
    return 1 - occ * fade * L.w;
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
    ID3D11VertexShader* g_Vs = nullptr; ID3D11PixelShader* g_Ps = nullptr; ID3D11Buffer* g_Cb = nullptr;
    ID3D11BlendState* g_Blend = nullptr; ID3D11DepthStencilState* g_NoDepth = nullptr; ID3D11RasterizerState* g_Rs = nullptr;
    std::atomic<unsigned long long> g_Drawn{ 0 }, g_NoTarget{ 0 }, g_NoDepthSrv{ 0 }, g_NoSun{ 0 }, g_BadFormat{ 0 };
    std::string g_Why = "not seen yet";
    int g_Logs = 0; float g_LoggedSun[3] = {};

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
        ID3DBlob* bv = blob("vs", "vs_5_0"); ID3DBlob* bp = blob("ps", "ps_5_0");
        bool ok = bv && bp && SUCCEEDED(dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &g_Vs))
                           && SUCCEEDED(dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, &g_Ps));
        if (bv) bv->Release(); if (bp) bp->Release();
        if (!ok) { g_Why = "shader compile failed"; return false; }
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 160; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_Cb))) { g_Why = "constant buffer"; return false; }
        // MIN into the mask's first channel only: the game's shadow stays where it is darker
        D3D11_BLEND_DESC bl{}; bl.RenderTarget[0].BlendEnable = TRUE;
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE; bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MIN;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MIN;
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
        D3D11_DEPTH_STENCIL_DESC ds{}; ds.DepthEnable = FALSE; ds.StencilEnable = FALSE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        if (FAILED(dev->CreateBlendState(&bl, &g_Blend)) || FAILED(dev->CreateDepthStencilState(&ds, &g_NoDepth)) || FAILED(dev->CreateRasterizerState(&rd, &g_Rs)))
        { g_Why = "states"; return false; }
        g_Failed = false; g_Ready = true;
        LOGI("ScreenShadows: ready (the sun's screen-space shadows drawn into the game's shadow mask at the end of SCREENSHADOWS)");
        return true;
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
        ID3D11Buffer* psCb = nullptr; ID3D11ShaderResourceView* psSrv = nullptr;
        D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED; ID3D11InputLayout* il = nullptr;
        ID3D11RasterizerState* rs = nullptr; D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {}; UINT nvp = 0;
        ID3D11BlendState* bs = nullptr; float bf[4] = {}; UINT mask = 0xFFFFFFFF;
        ID3D11DepthStencilState* dss = nullptr; UINT ref = 0;
        ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {}; ID3D11DepthStencilView* dsv = nullptr;
        void Save(ID3D11DeviceContext* c)
        {
            c->VSGetShader(&vs, nullptr, nullptr); c->PSGetShader(&ps, nullptr, nullptr); c->GSGetShader(&gs, nullptr, nullptr);
            c->PSGetConstantBuffers(0, 1, &psCb); c->PSGetShaderResources(0, 1, &psSrv);
            c->IAGetPrimitiveTopology(&topo); c->IAGetInputLayout(&il);
            c->RSGetState(&rs); nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; c->RSGetViewports(&nvp, vp);
            c->OMGetBlendState(&bs, bf, &mask); c->OMGetDepthStencilState(&dss, &ref);
            c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        }
        void Restore(ID3D11DeviceContext* c)
        {
            c->VSSetShader(vs, nullptr, 0); c->PSSetShader(ps, nullptr, 0); c->GSSetShader(gs, nullptr, 0);
            c->PSSetConstantBuffers(0, 1, &psCb); c->PSSetShaderResources(0, 1, &psSrv);
            c->IASetPrimitiveTopology(topo); c->IASetInputLayout(il);
            c->RSSetState(rs); c->RSSetViewports(nvp, nvp ? vp : nullptr);
            c->OMSetBlendState(bs, bf, mask); c->OMSetDepthStencilState(dss, ref);
            c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
            Rel(vs); Rel(ps); Rel(gs); Rel(psCb); Rel(psSrv); Rel(il); Rel(rs); Rel(bs); Rel(dss);
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

void ScreenShadows::AfterMask(ID3D11DeviceContext* c, const void* view, int eye)
{
    if (!g_On.load(std::memory_order_relaxed) || !c || !view) return;
    float L[3];
    if (!SunDir(L)) { g_NoSun++; return; }
    // the mask: the render target the pass left bound
    ID3D11RenderTargetView* rtv = nullptr; c->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) { g_NoTarget++; std::lock_guard lk(g_M); if (g_Logs < 20) { g_Logs++; LOGI("ScreenShadows: the SCREENSHADOWS pass left no render target bound (a compute pass?): nothing drawn"); } return; }
    ID3D11Resource* res = nullptr; rtv->GetResource(&res);
    D3D11_RESOURCE_DIMENSION dim{}; if (res) res->GetType(&dim);
    D3D11_TEXTURE2D_DESC md{};
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) static_cast<ID3D11Texture2D*>(res)->GetDesc(&md);
    D3D11_RENDER_TARGET_VIEW_DESC rvd{}; rtv->GetDesc(&rvd);
    // the scene's depth: a depth-format texture the pass's pixel shader reads, at the mask's size (or its double)
    ID3D11ShaderResourceView* depth = nullptr; D3D11_TEXTURE2D_DESC dd{}; int depthSlot = -1;
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
                { depth = srv[i]; depth->AddRef(); dd = t; depthSlot = i; }
            }
            if (r) r->Release();
            srv[i]->Release();
        }
    }
    const bool fmtOk = dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && (MaskFormat(md.Format) || MaskFormat(rvd.Format));
    std::lock_guard lk(g_M);
    if (g_Logs < 6)
    {   // what the pass leaves, for checking the guess (the mask's first channel = the sun's light, 1 = lit)
        g_Logs++;
        ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr);
        const uint64_t h = ps ? GpuTrace::ShaderHashOf(ps) : 0;
        if (ps) ps->Release();
        LOGI("ScreenShadows: eye {}: the mask {}x{} format {} (view format {}){}; the depth {}; the sun's direction {:.2f},{:.2f},{:.2f} ({:.0f} deg up); the pass's pixel shader {:016x}",
             eye, md.Width, md.Height, (int)md.Format, (int)rvd.Format, fmtOk ? "" : " -- NOT a format taken for a mask: nothing drawn",
             depth ? fmt::format("t{} {}x{} format {}", depthSlot, dd.Width, dd.Height, (int)dd.Format) : std::string("NOT found among the pass's inputs: nothing drawn"),
             L[0], L[1], L[2], asinf(std::clamp(L[2], -1.f, 1.f)) * 57.29578f, h);
        static bool dumped = false;
        if (h && !dumped) { dumped = true; GpuTrace::DumpShaderByHash(h); }   // -> DVR_shader*.txt: what the mask holds
    }
    if (!fmtOk) g_BadFormat++;
    if (!depth) g_NoDepthSrv++;
    bool ok = fmtOk && depth;
    if (ok && !g_Ready) { ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (dev) { Init(dev); dev->Release(); } ok = g_Ready; }
    if (ok)
    {
        if (fabsf(L[0] - g_LoggedSun[0]) + fabsf(L[1] - g_LoggedSun[1]) + fabsf(L[2] - g_LoggedSun[2]) > 0.05f)
        {
            memcpy(g_LoggedSun, L, 12);
            LOGI("ScreenShadows: the sun is {:.0f} deg up, towards {:.0f} deg (from cascade 0's matrix)", asinf(L[2]) * 57.29578f, atan2f(L[1], L[0]) * 57.29578f);
        }
        float P[16], V[16]; unsigned frame = 0;
        ok = ReadBytes(P, (const uint8_t*)view + kViewProj, 64) && ReadBytes(V, (const uint8_t*)view + kViewMatrix, 64) && ReadBytes(&frame, (const uint8_t*)view + kViewFrame, 4);
        if (ok)
        {
            struct { float P[16]; float V[12]; float L[4]; float g0[4]; float g1[4]; } k{};
            memcpy(k.P, P, 64); memcpy(k.V, V, 48);
            k.L[0] = L[0]; k.L[1] = L[1]; k.L[2] = L[2]; k.L[3] = g_Strength.load();
            k.g0[0] = g_Len.load(); k.g0[1] = (float)g_Steps.load(); k.g0[2] = (float)md.Width; k.g0[3] = (float)md.Height;
            k.g1[0] = (float)(frame & 1023); k.g1[1] = kFadeDist;
            Saved sv; sv.Save(c);
            c->UpdateSubresource(g_Cb, 0, nullptr, &k, 0, 0);
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); c->IASetInputLayout(nullptr);
            c->VSSetShader(g_Vs, nullptr, 0); c->GSSetShader(nullptr, nullptr, 0); c->PSSetShader(g_Ps, nullptr, 0);
            c->PSSetConstantBuffers(0, 1, &g_Cb); c->PSSetShaderResources(0, 1, &depth);
            D3D11_VIEWPORT vp{ 0.f, 0.f, (float)md.Width, (float)md.Height, 0.f, 1.f };
            c->RSSetState(g_Rs); c->RSSetViewports(1, &vp);
            c->OMSetBlendState(g_Blend, nullptr, 0xFFFFFFFF); c->OMSetDepthStencilState(g_NoDepth, 0);
            c->OMSetRenderTargets(1, &rtv, nullptr);
            c->Draw(3, 0);
            ID3D11ShaderResourceView* none = nullptr; c->PSSetShaderResources(0, 1, &none);
            sv.Restore(c);
            if (g_Drawn++ == 0) LOGI("ScreenShadows: first screen-space shadows drawn ({}x{})", md.Width, md.Height);
            g_Why = "running";
        }
    }
    if (depth) depth->Release();
    if (res) res->Release();
    rtv->Release();
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
    return fmt::format("screen-space shadows {}: drawn {}, no sun {}, no target {}, no depth {}, not a mask format {} ({})", g_On.load() ? "on" : "off",
                       g_Drawn.load(), g_NoSun.load(), g_NoTarget.load(), g_NoDepthSrv.load(), g_BadFormat.load(), g_Why);
}
