#include "pch.h"
#include "Foveation.h"
#include <d3d11.h>
#include <mutex>
#include <vector>
#include <cmath>
#include <algorithm>

namespace
{
    // ---- the NVAPI pieces used here (layouts and interface ids from NVIDIA's nvapi.h / nvapi_interface.h, MIT) ----
    using NvStatus = int;                                  // NVAPI_OK = 0
    enum NvRate : int { kX1PerPixel = 5, kX1Per2x2 = 8, kX1Per4x4 = 11 };   // NV_PIXEL_SHADING_RATE
#pragma pack(push, 8)
    struct NvViewportRates { bool enable; int table[16]; };                       // NV_D3D11_VIEWPORT_SHADING_RATE_DESC_V1
    struct NvViewportsRates { uint32_t version; uint32_t numViewports; NvViewportRates* viewports; };   // ..._VIEWPORTS_..._V1
    struct NvSrrvDesc { uint32_t version; DXGI_FORMAT format; int dimension; uint32_t mip, first, size; }; // NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_V1
#pragma pack(pop)
    static_assert(sizeof(NvViewportRates) == 68 && sizeof(NvViewportsRates) == 16 && sizeof(NvSrrvDesc) == 24, "NVAPI layouts");
    constexpr uint32_t Ver(size_t size, uint32_t v) { return (uint32_t)size | (v << 16); }   // MAKE_NVAPI_VERSION
    constexpr int kSrrvTexture2D = 4;                      // NV_SRRV_DIMENSION_TEXTURE2D
    constexpr UINT kTile = 16;                             // NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH / HEIGHT
    using QueryFn = void* (__cdecl*)(uint32_t id);
    using InitFn = NvStatus(__cdecl*)();
    using SetRatesFn = NvStatus(__cdecl*)(IUnknown* ctx, NvViewportsRates* desc);
    using CreateSrrvFn = NvStatus(__cdecl*)(ID3D11Device* dev, ID3D11Resource* res, NvSrrvDesc* desc, IUnknown** out);
    using SetSrrvFn = NvStatus(__cdecl*)(IUnknown* ctx, IUnknown* srrv);
    SetRatesFn g_SetRates = nullptr; CreateSrrvFn g_CreateSrrv = nullptr; SetSrrvFn g_SetSrrv = nullptr;

    std::atomic<bool> g_Want{ false };
    std::mutex g_M;                                        // everything below
    Foveation::Params g_P;
    bool g_Tried = false, g_Ready = false;
    std::string g_Why = "off";
    struct EyeMask { ID3D11Texture2D* tex = nullptr; IUnknown* srrv = nullptr; UINT w = 0, h = 0; float cx = -9, cy = -9; ULONGLONG builtAt = 0; };
    EyeMask g_Eye[2];
    UINT g_W = 0, g_H = 0;                                 // the eye's render size (learned from the depth buffer bound by the tasks)
    std::atomic<unsigned long long> g_Begun{ 0 }, g_Fails{ 0 }, g_NoSize{ 0 };
    int g_Coverage[3] = {};                                // tiles at full, 2x2, 4x4 in the last mask built
    constexpr uint32_t kViewProj = 0x7390;
    template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
    bool ReadBytes(void* d, const void* s, size_t n) { __try { memcpy(d, s, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

    bool InitNv()
    {
        if (g_Tried) return g_Ready;
        g_Tried = true;
        HMODULE m = LoadLibraryA("nvapi64.dll");
        QueryFn q = m ? (QueryFn)GetProcAddress(m, "nvapi_QueryInterface") : nullptr;
        if (!q) { g_Why = "nvapi64.dll not found (not an NVIDIA driver)"; LOGI("Foveation: {}: off", g_Why); return false; }
        InitFn init = (InitFn)q(0x0150E828);               // NvAPI_Initialize
        g_SetRates = (SetRatesFn)q(0x34F7938F);            // NvAPI_D3D11_RSSetViewportsPixelShadingRates
        g_CreateSrrv = (CreateSrrvFn)q(0x99CA2DFF);        // NvAPI_D3D11_CreateShadingRateResourceView
        g_SetSrrv = (SetSrrvFn)q(0x1B0C2F83);              // NvAPI_D3D11_RSSetShadingRateResourceView
        if (!init || !g_SetRates || !g_CreateSrrv || !g_SetSrrv) { g_Why = "the driver lacks the Variable Rate Shading functions"; LOGI("Foveation: {}: off", g_Why); return false; }
        const NvStatus s = init();
        if (s != 0) { g_Why = fmt::format("NvAPI_Initialize failed ({})", s); LOGI("Foveation: {}: off", g_Why); return false; }
        g_Ready = true;
        LOGI("Foveation: NVAPI ready (Variable Rate Shading)");
        return true;
    }
    //! the eye's projection axis on the screen (ndc) and the tangent of a pixel's angle from it
    struct Proj { float s, offx, offy, p00, p11; };
    bool ReadProj(const void* view, Proj& o)
    {
        float P[16];
        if (!ReadBytes(P, (const uint8_t*)view + kViewProj, 64)) return false;
        const bool colVec = fabsf(fabsf(P[14]) - 1.f) < 1e-3f;      // row 3 = (0, 0, +-1, 0)
        o.s = colVec ? P[14] : P[11];
        o.offx = colVec ? P[2] : P[8]; o.offy = colVec ? P[6] : P[9];
        o.p00 = P[0]; o.p11 = P[5];
        return std::isfinite(o.p00) && std::isfinite(o.p11) && fabsf(o.p00) > 1e-3f && fabsf(o.p11) > 1e-3f && fabsf(o.s) > 0.5f;
    }
    bool BuildMask(ID3D11Device* dev, int eye, const Proj& pr)
    {
        EyeMask& e = g_Eye[eye];
        const UINT tw = (g_W + kTile - 1) / kTile, th = (g_H + kTile - 1) / kTile;
        std::vector<uint8_t> px((size_t)tw * th);
        const float tin = tanf(g_P.innerDeg * 0.0174533f), tout = tanf(g_P.outerDeg * 0.0174533f);
        int cov[3] = {};
        for (UINT y = 0; y < th; y++)
            for (UINT x = 0; x < tw; x++)
            {
                const float nx = ((x + 0.5f) * kTile) / g_W * 2.f - 1.f, ny = 1.f - ((y + 0.5f) * kTile) / g_H * 2.f;
                const float tx = (nx * pr.s - pr.offx) / pr.p00, ty = (ny * pr.s - pr.offy) / pr.p11;
                // the tangent of the angle from the axis (exact for one axis, close enough for a ring)
                const float t = sqrtf(tx * tx + ty * ty);
                const uint8_t v = t <= tin ? 0 : t <= tout ? 1 : 2;
                px[(size_t)y * tw + x] = v; cov[v]++;
            }
        Rel(e.srrv); Rel(e.tex);
        D3D11_TEXTURE2D_DESC td{}; td.Width = tw; td.Height = th; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8_UINT;
        td.SampleDesc = { 1, 0 }; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{ px.data(), tw, 0 };
        if (FAILED(dev->CreateTexture2D(&td, &sd, &e.tex))) { g_Why = "mask texture"; return false; }
        NvSrrvDesc d{}; d.version = Ver(sizeof(NvSrrvDesc), 1); d.format = DXGI_FORMAT_R8_UINT; d.dimension = kSrrvTexture2D;
        const NvStatus s = g_CreateSrrv(dev, e.tex, &d, &e.srrv);
        if (s != 0 || !e.srrv) { g_Why = fmt::format("NvAPI_D3D11_CreateShadingRateResourceView failed ({}): this GPU may not support it", s); LOGI("Foveation: {}", g_Why); Rel(e.tex); return false; }
        e.w = g_W; e.h = g_H; e.builtAt = GetTickCount64();
        e.cx = pr.offx * pr.s; e.cy = pr.offy * pr.s;
        memcpy(g_Coverage, cov, sizeof cov);
        const int total = std::max(1, cov[0] + cov[1] + cov[2]);
        // shading work relative to full rate: full tiles 1, 2x2 tiles 1/4, 4x4 tiles 1/16
        const double work = (cov[0] + cov[1] / 4.0 + cov[2] / 16.0) / total;
        LOGI("Foveation: eye {} mask {}x{} tiles for {}x{}: axis at ({:.3f},{:.3f}) ndc; full rate {:.0f}% of the image, 2x2 {:.0f}%, 4x4 {:.0f}% "
             "(within {:.0f} / {:.0f} deg): about {:.0f}% of the shading work", eye, tw, th, g_W, g_H, e.cx, e.cy,
             100.0 * cov[0] / total, 100.0 * cov[1] / total, 100.0 * cov[2] / total, g_P.innerDeg, g_P.outerDeg, 100.0 * work);
        return true;
    }
}

void Foveation::SetParams(const Params& p)
{
    std::lock_guard lk(g_M);
    g_P = p; g_P.outerDeg = std::max(g_P.outerDeg, g_P.innerDeg);
    g_Want = p.on;
    if (p.on) LOGI("Foveation: fixed foveated rendering ON (FoveatedRendering): full shading within {:.0f} deg of each lens's centre, 2x2 to {:.0f} deg, 4x4 beyond",
                   g_P.innerDeg, g_P.outerDeg);
}
bool Foveation::Wanted() { return g_Want.load(); }

bool Foveation::Begin(ID3D11DeviceContext* c, const void* view, int eye)
{
    if (!g_Want.load(std::memory_order_relaxed) || !c || !view || eye < 0 || eye > 1) return false;
    std::lock_guard lk(g_M);
    if (!InitNv()) { g_Want = false; return false; }
    if (!g_W || !g_H) { g_NoSize++; return false; }
    Proj pr;
    if (!ReadProj(view, pr)) return false;
    EyeMask& e = g_Eye[eye];
    const float cx = pr.offx * pr.s, cy = pr.offy * pr.s;
    const bool moved = fabsf(cx - e.cx) * g_W * 0.5f > 2.f * kTile || fabsf(cy - e.cy) * g_H * 0.5f > 2.f * kTile;
    if (!e.srrv || e.w != g_W || e.h != g_H || (moved && GetTickCount64() - e.builtAt > 1000))
    {
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);
        const bool ok = dev && BuildMask(dev, eye, pr);
        if (dev) dev->Release();
        if (!ok && !e.srrv) { g_Fails++; if (g_Fails.load() > 8) g_Want = false; return false; }
    }
    NvViewportRates vr{}; vr.enable = true;
    for (int i = 0; i < 16; i++) vr.table[i] = kX1PerPixel;
    vr.table[1] = kX1Per2x2; vr.table[2] = kX1Per4x4;
    NvViewportsRates d{ Ver(sizeof(NvViewportsRates), 1), 1, &vr };
    const NvStatus a = g_SetSrrv(c, e.srrv), b = g_SetRates(c, &d);
    if (a != 0 || b != 0)
    {
        g_Fails++;
        if (g_Fails.load() <= 3) LOGI("Foveation: setting the shading rates on a {} context failed ({} / {})",
                                      c->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED ? "deferred" : "immediate", a, b);
        if (g_Fails.load() > 8) { g_Want = false; g_Why = "the driver refused the shading rates"; LOGI("Foveation: {}: off", g_Why); }
        NvViewportsRates off{ Ver(sizeof(NvViewportsRates), 1), 0, nullptr };
        g_SetRates(c, &off); g_SetSrrv(c, nullptr);
        return false;
    }
    if (g_Begun.fetch_add(1) == 0) LOGI("Foveation: first foveated task (eye {}, {} context)", eye, c->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED ? "deferred" : "immediate");
    g_Why = "running";
    return true;
}

void Foveation::End(ID3D11DeviceContext* c, bool begun)
{
    if (!c) return;
    // the render size, for the masks: the depth buffer the task drew with (all foveated passes bind the eye's)
    ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(1, &rtv, &dsv);
    if (dsv)
    {
        ID3D11Resource* r = nullptr; dsv->GetResource(&r);
        D3D11_RESOURCE_DIMENSION dim{}; if (r) r->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        {
            D3D11_TEXTURE2D_DESC d{}; static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
            if (d.Width >= 256 && d.Height >= 256 && (d.Width != g_W || d.Height != g_H))
            {
                std::lock_guard lk(g_M);
                if (d.Width != g_W || d.Height != g_H) { LOGI("Foveation: the eyes render at {}x{}", d.Width, d.Height); g_W = d.Width; g_H = d.Height; }
            }
        }
        if (r) r->Release();
        dsv->Release();
    }
    if (rtv) rtv->Release();
    if (!begun || !g_SetRates) return;
    NvViewportsRates off{ Ver(sizeof(NvViewportsRates), 1), 0, nullptr };
    g_SetRates(c, &off);
    g_SetSrrv(c, nullptr);
}

std::string Foveation::Status()
{
    std::lock_guard lk(g_M);
    return fmt::format("foveation want {} ready {} tasks {} fails {} nosize {} size {}x{} ({})", g_Want.load() ? 1 : 0, g_Ready ? 1 : 0, g_Begun.load(),
                       g_Fails.load(), g_NoSize.load(), g_W, g_H, g_Why);
}
