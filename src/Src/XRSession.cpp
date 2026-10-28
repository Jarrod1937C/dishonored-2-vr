#include <vector>
#include <algorithm>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "XRSession.h"
#include "MetaAsw.h"
#include "SysInfo.h"
#include "Watchdog.h"
#include <cmath>
#include <chrono>
#include <condition_variable>

// Single OpenXR state block (only one session ever exists).
namespace
{
struct XRState
{
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    XrSwapchain swapchains[XRSession::kMaxEyes] = {};
    std::vector<XrSwapchainImageD3D11KHR> images[XRSession::kMaxEyes];

    // HUD quad layer (head-locked UI panel).
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrSwapchain quadSwapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> quadImages;

    XrFrameState frameState = { XR_TYPE_FRAME_STATE };
    XrView views[XRSession::kMaxEyes] = {};

    // Input (controller actions)
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction turnAction = XR_NULL_HANDLE;
    XrAction turnYAction = XR_NULL_HANDLE;
    XrAction moveAction = XR_NULL_HANDLE;
    XrAction triggerAction[2] = {};   // 0=L, 1=R
    XrAction gripAction[2] = {};      // 0=L, 1=R (squeeze)
    XrAction btnAction[7] = {};       // a, b, x, y, thumbL, thumbR, menu (clicks)
    XrAction aimAction[2] = {};       // 0=L, 1=R
    XrAction hapticAction[2] = {};    // 0=L, 1=R (vibration output)
    XrSpace aimSpace[2] = {};
    // The left hand's GRIP pose: the anatomical one (origin in the palm, -Z along the straightened index finger, +X out
    // of the left palm, so -X out of the back of the hand). The wrist HUD rides on it.
    XrAction wristAction = XR_NULL_HANDLE;
    XrSpace wristSpace = XR_NULL_HANDLE;
    XrSwapchain wristSwapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> wristImages;
    // Panels in the room (XRPanelLayer): 0 the menu screen, 1 the power wheel.
    XrSwapchain panelSwapchain[2] = {};
    std::vector<XrSwapchainImageD3D11KHR> panelImages[2];
    uint32_t panelW[2] = {}, panelH[2] = {};
} g;

double NowSec()
{
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

// ---- The OpenXR frame thread's shared state (XRSession::PumpLoop, the development notes) ----
// The lock is NEVER held across an OpenXR call: a runtime call that does not return must not take the render thread
// with it (that is the whole point).
struct PumpState
{
    std::mutex m;
    std::condition_variable cv;
    HANDLE thread = nullptr;
    bool quit = false;
    bool request = false;        //!< the render thread wants a frame (BeginFrame is waiting for one)
    bool waited = false;         //!< a frame xrWaitFrame released, not begun yet
    XrFrameState state = { XR_TYPE_FRAME_STATE };
    double waitedAt = 0;
    bool waiting = false;        //!< the frame thread is inside xrWaitFrame
    bool mainOpen = false;       //!< the render thread is between its xrBeginFrame and xrEndFrame
    bool kaOpen = false;         //!< a keep-alive frame is between its xrBeginFrame and xrEndFrame
    bool ending = false;         //!< xrEndSession in progress
    bool endPending = false;     //!< STOPPING came while a frame was open: end the session when it closes
    bool lastTimedOut = false;   //!< the render thread's last BeginFrame ran out of time
    double timedOutSince = 0;
    double lastMainBegin = 0, lastMainEnd = 0;
    // The last frame's layers, for the keep-alive frames (the wrist watch is left out: it rides the hand).
    XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView views[XRSession::kMaxEyes] = {};
    XrCompositionLayerQuad quads[4] = {};
    int nQuads = 0;
    bool haveLayers = false;
    // the runtime taking the session away (STOPPING -> IDLE without our asking) and not offering it back
    bool wasRunning = false; double idleSince = 0, nextKick = 0;
    // keep-alive runs
    bool inRun = false; double runStart = 0; unsigned long long runFrames = 0;
    double lastKeepAlive = 0, period = 1.0 / 72.0;   //!< keep-alive frames at most once per display period
    ID3D11Multithread* mt = nullptr;   //!< the device's lock, taken around the keep-alive's frame calls
    // stats
    unsigned long long waits = 0, mainFrames = 0, keepAlives = 0, keepAliveRuns = 0, mainTimeouts = 0, timeoutRuns = 0,
                       waitFails = 0, kicks = 0, imageTimeouts = 0;
    double longestWait = 0, longestRun = 0, longestTimeout = 0;
} pump;

// ---- Swapchain images, waited for with a bound ----
// xrWaitSwapchainImage used to wait for ever: with the compositor stuck holding its images, so was the game. Now it
// waits 100 ms; an image that is not free by then stays acquired (as OpenXR requires: it may not be released before a
// successful wait) and is waited for again on the next frame. The layer meanwhile shows that swapchain's last released
// image.
struct ImageWait { XrSwapchain sc = XR_NULL_HANDLE; uint32_t index = 0; bool pending = false; };
ImageWait g_ImageWait[8];
ImageWait& WaitSlot(XrSwapchain sc)
{
    for (ImageWait& w : g_ImageWait) if (w.sc == sc) return w;
    for (ImageWait& w : g_ImageWait) if (w.sc == XR_NULL_HANDLE) { w = {}; w.sc = sc; return w; }
    static ImageWait spare; spare = {}; spare.sc = sc; return spare;
}
bool AcquireWaitImage(XrSwapchain sc, uint32_t& index)
{
    if (!sc) return false;
    ImageWait& w = WaitSlot(sc);
    if (!w.pending)
    {
        XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        Watchdog::Mark mk("xrAcquireSwapchainImage");
        if (XR_FAILED(xrAcquireSwapchainImage(sc, &ai, &w.index))) return false;
        w.pending = true;
    }
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = 100000000;   // 100 ms
    XrResult r;
    { Watchdog::Mark mk("xrWaitSwapchainImage"); r = xrWaitSwapchainImage(sc, &wi); }
    if (r != XR_SUCCESS)
    {
        if (pump.imageTimeouts++ < 8) LOGI("XRSession: a swapchain image was not free after 100 ms ({}): this frame keeps its last one", (int)r);
        return false;
    }
    w.pending = false;
    index = w.index;
    return true;
}
//! Destroys a swapchain and forgets what referred to it (its wait slot, the keep-alive's copy of the layers).
void DestroySwapchain(XrSwapchain& sc)
{
    if (!sc) return;
    for (ImageWait& w : g_ImageWait) if (w.sc == sc) w = {};
    { std::lock_guard<std::mutex> lk(pump.m); pump.haveLayers = false; }
    xrDestroySwapchain(sc);
    sc = XR_NULL_HANDLE;
}

// ---- the scaled blit (orthoscopic mono placement, see XRSession::SetOrtho) ----
// Drawn in a context state of its own (ID3D11DeviceContext1::SwapDeviceContextState): the game's whole pipeline
// state is swapped out and back in untouched, so nothing it set can leak into the blit or the other way round.
const char* kBlitHlsl = R"(
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID)
{
    V o; float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
Texture2D tx : register(t0); SamplerState sm : register(s0);
cbuffer C : register(b0) { float4 k; };   // x: brightness (the fade back in after a menu)
float4 ps(V i) : SV_Target { return float4(tx.Sample(sm, i.uv).rgb * k.x, 1); }
// The warp: the image was rendered in a frame turned from the eye's (a stereo eye turned to its frustum's centre, a mono
// image on canted displays). Each eye pixel is a direction in the eye's frame (its native fov, linear in tan), taken into
// the render frame by the rows r0..r2 and looked up in the image at its own tans; what it does not cover is black.
cbuffer W : register(b1) { float4 kw; float4 dstT; float4 srcT; float4 r0; float4 r1; float4 r2; };   // tans: L R U D
float4 psw(V i) : SV_Target
{
    const float3 e = float3(lerp(dstT.x, dstT.y, i.uv.x), lerp(dstT.z, dstT.w, i.uv.y), -1);
    const float3 d = float3(dot(r0.xyz, e), dot(r1.xyz, e), dot(r2.xyz, e));
    if (d.z > -1e-4) return float4(0, 0, 0, 1);
    const float2 s = d.xy / -d.z;
    const float2 uv = float2((s.x - srcT.x) / (srcT.y - srcT.x), (srcT.z - s.y) / (srcT.z - srcT.w));
    if (any(uv < 0) || any(uv > 1)) return float4(0, 0, 0, 1);
    return float4(tx.SampleLevel(sm, uv, 0).rgb * kw.x, 1);
}
)";
struct Blitter
{
    bool failed = false, ready = false;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11SamplerState* smp = nullptr;
    ID3D11RasterizerState* rs = nullptr; ID3D11Buffer* cb = nullptr; float cbScale = -1.f;
    ID3D11PixelShader* psw = nullptr; ID3D11Buffer* cbw = nullptr;   // the warp (psw)
    unsigned long long warps = 0;
    ID3DDeviceContextState* state = nullptr;
    ID3D11Texture2D* src = nullptr; ID3D11ShaderResourceView* srv = nullptr;
    UINT w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    unsigned long long draws = 0;

    bool Init(ID3D11Device* dev)
    {
        if (ready || failed) return ready;
        failed = true;
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
        if (!compile) { LOGI("XRSession: blit: d3dcompiler_47 not available"); return false; }
        ID3DBlob *bv = nullptr, *bp = nullptr, *bw = nullptr, *err = nullptr;
        if (FAILED(compile(kBlitHlsl, strlen(kBlitHlsl), "blit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &bv, &err)) ||
            FAILED(compile(kBlitHlsl, strlen(kBlitHlsl), "blit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &bp, &err)) ||
            FAILED(compile(kBlitHlsl, strlen(kBlitHlsl), "blit", nullptr, nullptr, "psw", "ps_5_0", 0, 0, &bw, &err)))
        {
            LOGI("XRSession: blit shader compile failed: {}", err ? (const char*)err->GetBufferPointer() : "?");
            if (err) err->Release(); if (bv) bv->Release(); if (bp) bp->Release(); if (bw) bw->Release();
            return false;
        }
        dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &vs);
        dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, &ps);
        dev->CreatePixelShader(bw->GetBufferPointer(), bw->GetBufferSize(), nullptr, &psw);
        bv->Release(); bp->Release(); bw->Release();
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &smp);
        D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        dev->CreateRasterizerState(&rd, &rs);
        D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&bd, nullptr, &cb);
        bd.ByteWidth = 6 * 16; dev->CreateBuffer(&bd, nullptr, &cbw);
        ID3D11Device1* d1 = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&d1)) && d1)
        {
            const D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
            d1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &state);
            d1->Release();
        }
        if (!vs || !ps || !psw || !smp || !rs || !cb || !cbw || !state) { LOGI("XRSession: blit: resource creation failed (state {})", (void*)state); return false; }
        failed = false; ready = true;
        LOGI("XRSession: orthoscopic blit ready");
        return true;
    }
    //! Draw `source` into `dst` so that it covers the pixel rect (x, y, w, h) -- which may reach outside dst.
    //! A view format for a typeless texture: the swapchain's own (the runtime may allocate TYPELESS images for an
    //! sRGB swapchain -- Meta's does; the mock does not, which is why this only failed on the headset).
    static DXGI_FORMAT ViewFormat(DXGI_FORMAT tex, DXGI_FORMAT swapFmt)
    {
        switch (tex)
        {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return swapFmt != DXGI_FORMAT_UNKNOWN ? swapFmt : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return swapFmt != DXGI_FORMAT_UNKNOWN ? swapFmt : DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return tex;
        }
    }
    unsigned long long fails = 0;
    bool Fail(const char* what, long hr)
    {
        if (fails++ < 5) LOGI("XRSession: orthoscopic blit failed at {} (hr 0x{:08X}); falling back to the 1:1 copy", what, (unsigned)hr);
        return false;
    }
    bool Draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* source, ID3D11Texture2D* dst, float x, float y, float rw, float rh,
              DXGI_FORMAT swapFmt = DXGI_FORMAT_UNKNOWN, float scale = 1.f)
    {
        if (!Copy(dev, ctx, source)) return false;
        return DrawSrv(dev, ctx, srv, dst, x, y, rw, rh, swapFmt, scale, true);
    }
    //! This frame's picture into the blitter's own shader-readable copy (src / srv).
    bool Copy(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* source)
    {
        if (!Init(dev)) return false;
        D3D11_TEXTURE2D_DESC sd = {}; source->GetDesc(&sd);
        if (!src || sd.Width != w || sd.Height != h || sd.Format != fmt)
        {
            if (srv) { srv->Release(); srv = nullptr; } if (src) { src->Release(); src = nullptr; }
            D3D11_TEXTURE2D_DESC cd = sd; cd.MipLevels = 1; cd.ArraySize = 1; cd.SampleDesc = { 1, 0 };
            cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.CPUAccessFlags = 0; cd.MiscFlags = 0;
            cd.Format = sd.Format;
            D3D11_SHADER_RESOURCE_VIEW_DESC vd = {}; vd.Format = ViewFormat(sd.Format, DXGI_FORMAT_UNKNOWN);
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
            if (FAILED(dev->CreateTexture2D(&cd, nullptr, &src)) || FAILED(dev->CreateShaderResourceView(src, &vd, &srv)))
            { LOGI("XRSession: blit: source copy {}x{} fmt {} failed", sd.Width, sd.Height, (int)sd.Format); failed = true; ready = false; return false; }
            w = sd.Width; h = sd.Height; fmt = sd.Format;
        }
        ctx->CopyResource(src, source);
        return true;
    }
    //! Draw `view` into `dst` covering the pixel rect (x, y, w, h) at brightness `scale`; `clear` = black first.
    bool DrawSrv(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* view, ID3D11Texture2D* dst, float x, float y,
                 float rw, float rh, DXGI_FORMAT swapFmt, float scale, bool clear)
    {
        if (!Init(dev) || !view) return false;
        ID3D11RenderTargetView* rtv = nullptr;
        {
            D3D11_TEXTURE2D_DESC dd = {}; dst->GetDesc(&dd);
            D3D11_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = ViewFormat(dd.Format, swapFmt); rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            const HRESULT hr = dev->CreateRenderTargetView(dst, &rd, &rtv);
            if (FAILED(hr) || !rtv) return Fail("render target view", hr);
        }
        ID3D11DeviceContext1* c1 = nullptr;
        { const HRESULT hr = ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1); if (FAILED(hr) || !c1) { rtv->Release(); return Fail("context1", hr); } }
        ID3DDeviceContextState* prev = nullptr;
        c1->SwapDeviceContextState(state, &prev);
        if (scale != cbScale) { const float k[4] = { scale, 0, 0, 0 }; c1->UpdateSubresource(cb, 0, nullptr, k, 0, 0); cbScale = scale; }
        const float black[4] = { 0, 0, 0, 1 };
        if (clear) c1->ClearRenderTargetView(rtv, black);
        c1->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = { x, y, rw, rh, 0.f, 1.f };
        c1->RSSetViewports(1, &vp);
        c1->RSSetState(rs);
        c1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c1->VSSetShader(vs, nullptr, 0);
        c1->PSSetShader(ps, nullptr, 0);
        c1->PSSetShaderResources(0, 1, &view);
        c1->PSSetSamplers(0, 1, &smp);
        c1->PSSetConstantBuffers(0, 1, &cb);
        c1->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr; c1->PSSetShaderResources(0, 1, &none);
        c1->OMSetRenderTargets(0, nullptr, nullptr);
        c1->SwapDeviceContextState(prev, nullptr);
        if (prev) prev->Release();
        c1->Release(); rtv->Release();
        draws++;
        return true;
    }
    //! Draw `view` over the WHOLE of `dst`, warped: dst is an eye's native fov (dstT: tans L R U D), the view an image
    //! rendered at srcT in a frame turned from the eye's; `m` (3x3, rows) takes an eye direction into that frame.
    bool DrawWarp(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* view, ID3D11Texture2D* dst, const float dstT[4],
                  const float srcT[4], const float m[9], DXGI_FORMAT swapFmt, float scale)
    {
        if (!Init(dev) || !view) return false;
        ID3D11RenderTargetView* rtv = nullptr;
        D3D11_TEXTURE2D_DESC dd = {}; dst->GetDesc(&dd);
        {
            D3D11_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = ViewFormat(dd.Format, swapFmt); rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            const HRESULT hr = dev->CreateRenderTargetView(dst, &rd, &rtv);
            if (FAILED(hr) || !rtv) return Fail("render target view (warp)", hr);
        }
        ID3D11DeviceContext1* c1 = nullptr;
        { const HRESULT hr = ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1); if (FAILED(hr) || !c1) { rtv->Release(); return Fail("context1", hr); } }
        ID3DDeviceContextState* prev = nullptr;
        c1->SwapDeviceContextState(state, &prev);
        const float k[24] = { scale, 0, 0, 0, dstT[0], dstT[1], dstT[2], dstT[3], srcT[0], srcT[1], srcT[2], srcT[3],
                              m[0], m[1], m[2], 0, m[3], m[4], m[5], 0, m[6], m[7], m[8], 0 };
        c1->UpdateSubresource(cbw, 0, nullptr, k, 0, 0);
        c1->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = { 0.f, 0.f, (float)dd.Width, (float)dd.Height, 0.f, 1.f };
        c1->RSSetViewports(1, &vp);
        c1->RSSetState(rs);
        c1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c1->VSSetShader(vs, nullptr, 0);
        c1->PSSetShader(psw, nullptr, 0);
        c1->PSSetShaderResources(0, 1, &view);
        c1->PSSetSamplers(0, 1, &smp);
        c1->PSSetConstantBuffers(1, 1, &cbw);
        c1->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr; c1->PSSetShaderResources(0, 1, &none);
        c1->OMSetRenderTargets(0, nullptr, nullptr);
        c1->SwapDeviceContextState(prev, nullptr);
        if (prev) prev->Release();
        c1->Release(); rtv->Release();
        draws++; warps++;
        return true;
    }
    void Release()
    {
        for (IUnknown* u : std::initializer_list<IUnknown*>{ vs, ps, psw, smp, rs, cb, cbw, state, srv, src }) if (u) u->Release();
        vs = nullptr; ps = nullptr; psw = nullptr; smp = nullptr; rs = nullptr; cb = nullptr; cbw = nullptr; state = nullptr; srv = nullptr; src = nullptr; ready = false;
        cbScale = -1.f; failed = false;
    }
} g_Blit;

//! Shaders of the mod's own passes, compiled at run time (d3dcompiler_47 ships with Windows 10/11).
bool CompileFx(ID3D11Device* dev, const char* hlsl, const char* name, ID3D11VertexShader** vs, ID3D11PixelShader** ps)
{
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) { LOGI("XRSession: {}: d3dcompiler_47 not available", name); return false; }
    ID3DBlob *bv = nullptr, *bp = nullptr, *err = nullptr;
    if (FAILED(compile(hlsl, strlen(hlsl), name, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &bv, &err)) ||
        FAILED(compile(hlsl, strlen(hlsl), name, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &bp, &err)))
    {
        LOGI("XRSession: {} shader compile failed: {}", name, err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release(); if (bv) bv->Release(); if (bp) bp->Release();
        return false;
    }
    dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, vs);
    dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, ps);
    bv->Release(); bp->Release();
    return *vs && *ps;
}
//! A pass of its own: shaders, one sampler, one constant buffer, its own context state (the game's is swapped out and
//! back in untouched, as for the blit).
struct Pass
{
    bool failed = false, ready = false;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11SamplerState* smp = nullptr;
    ID3D11RasterizerState* rs = nullptr; ID3D11Buffer* cb = nullptr; ID3DDeviceContextState* state = nullptr;
    unsigned long long draws = 0, fails = 0;
    bool Init(ID3D11Device* dev, const char* hlsl, const char* name, UINT cbBytes)
    {
        if (ready || failed) return ready;
        failed = true;
        if (!CompileFx(dev, hlsl, name, &vs, &ps)) return false;
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &smp);
        D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        dev->CreateRasterizerState(&rd, &rs);
        D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = cbBytes; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&bd, nullptr, &cb);
        ID3D11Device1* d1 = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&d1)) && d1)
        {
            const D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
            d1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &state);
            d1->Release();
        }
        if (!vs || !ps || !smp || !rs || !cb || !state) { LOGI("XRSession: {}: resource creation failed", name); return false; }
        failed = false; ready = true;
        LOGI("XRSession: {} pass ready", name);
        return true;
    }
    //! One full-target triangle into `dst` (w x h) reading `view` (may be null), constants `k`.
    bool Run(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, UINT w, UINT h, DXGI_FORMAT swapFmt,
             const void* k, ID3D11ShaderResourceView* view, const char* name)
    {
        ID3D11RenderTargetView* rtv = nullptr;
        {
            D3D11_TEXTURE2D_DESC dd = {}; dst->GetDesc(&dd);
            D3D11_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = Blitter::ViewFormat(dd.Format, swapFmt); rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            const HRESULT hr = dev->CreateRenderTargetView(dst, &rd, &rtv);
            if (FAILED(hr) || !rtv) { if (fails++ < 5) LOGI("XRSession: {}: render target view failed (hr 0x{:08X})", name, (unsigned)hr); return false; }
        }
        ID3D11DeviceContext1* c1 = nullptr;
        if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1)) || !c1) { rtv->Release(); return false; }
        ID3DDeviceContextState* prev = nullptr;
        c1->SwapDeviceContextState(state, &prev);
        c1->UpdateSubresource(cb, 0, nullptr, k, 0, 0);
        c1->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = { 0.f, 0.f, (float)w, (float)h, 0.f, 1.f };
        c1->RSSetViewports(1, &vp);
        c1->RSSetState(rs);
        c1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c1->VSSetShader(vs, nullptr, 0);
        c1->PSSetShader(ps, nullptr, 0);
        c1->PSSetShaderResources(0, 1, &view);
        c1->PSSetSamplers(0, 1, &smp);
        c1->PSSetConstantBuffers(0, 1, &cb);
        c1->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr; c1->PSSetShaderResources(0, 1, &none);
        c1->OMSetRenderTargets(0, nullptr, nullptr);
        c1->SwapDeviceContextState(prev, nullptr);
        if (prev) prev->Release();
        c1->Release(); rtv->Release();
        draws++;
        return true;
    }
    void Release()
    {
        for (IUnknown* u : std::initializer_list<IUnknown*>{ vs, ps, smp, rs, cb, state }) if (u) u->Release();
        vs = nullptr; ps = nullptr; smp = nullptr; rs = nullptr; cb = nullptr; state = nullptr; ready = false; failed = false;
    }
};

// ---- a panel in the room (XRPanelLayer) ----
// The crop is resampled to the panel's swapchain with four taps a quarter of an output pixel apart (the compositor
// samples the quad at display resolution; this keeps text from shimmering when the source is a little larger), and
// shaped: rounded corners and an edge that fades out over a few pixels, so the panel's outline is anti-aliased instead
// of the stair-stepped edge a quad's border gets in the compositor. Premultiplied out, whichever the source.
const char* kPanelHlsl = R"(
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID)
{
    V o; float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
cbuffer C : register(b0) { float4 crop; float4 src; float4 shape; };   // crop: x y w h (px); src: w h alpha premultiplied; shape: corner soft aspect gamma
Texture2D tx : register(t0); SamplerState sm : register(s0);
float3 Linear(float3 s) { return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4); }
float4 ps(V i) : SV_Target
{
    float2 px = crop.xy + i.uv * crop.zw;
    float2 dx = ddx(px) * 0.25, dy = ddy(px) * 0.25;
    float4 c = (tx.Sample(sm, (px + dx + dy) / src.xy) + tx.Sample(sm, (px + dx - dy) / src.xy)
              + tx.Sample(sm, (px - dx + dy) / src.xy) + tx.Sample(sm, (px - dx - dy) / src.xy)) * 0.25;
    if (src.w < 0.5) c.a = 1;
    if (shape.w > 0.5) c.rgb = Linear(c.a > 1e-4 ? saturate(c.rgb / c.a) : 0) * c.a;   // gamma-space premultiplied -> linear
    float2 q = (i.uv - 0.5) * float2(shape.z, 1);
    float2 h = float2(shape.z, 1) * 0.5 - shape.x;
    float2 a = abs(q) - h;
    float d = length(max(a, 0)) + min(max(a.x, a.y), 0) - shape.x;   // signed distance to the rounded rectangle (units: the height)
    float m = saturate(-d / max(shape.y, 1e-4));
    return c * (m * src.z);
}
)";
struct PanelRenderer
{
    Pass pass;
    ID3D11Texture2D* copy = nullptr; ID3D11ShaderResourceView* copySrv = nullptr;   // a crop of a copy-only source
    UINT cw = 0, ch = 0; DXGI_FORMAT cf = DXGI_FORMAT_UNKNOWN;
    bool Draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, const XRPanelLayer& l, ID3D11Texture2D* dst, UINT w, UINT h, DXGI_FORMAT swapFmt)
    {
        if (!pass.Init(dev, kPanelHlsl, "panel", 48)) return false;
        ID3D11ShaderResourceView* view = l.srv;
        float crop[4] = { l.crop[0], l.crop[1], l.crop[2], l.crop[3] }, sw = (float)l.srcW, sh = (float)l.srcH;
        if (!view && l.tex)
        {   // copy the crop: the backbuffer cannot be read by a shader
            D3D11_TEXTURE2D_DESC sd = {}; l.tex->GetDesc(&sd);
            const UINT x0 = (UINT)std::max(0.f, crop[0]), y0 = (UINT)std::max(0.f, crop[1]);
            const UINT x1 = std::min(sd.Width, (UINT)(crop[0] + crop[2])), y1 = std::min(sd.Height, (UINT)(crop[1] + crop[3]));
            if (x1 <= x0 + 8 || y1 <= y0 + 8) return false;
            const UINT nw = x1 - x0, nh = y1 - y0;
            if (!copy || cw != nw || ch != nh || cf != sd.Format)
            {
                if (copySrv) { copySrv->Release(); copySrv = nullptr; } if (copy) { copy->Release(); copy = nullptr; }
                D3D11_TEXTURE2D_DESC cd = {}; cd.Width = nw; cd.Height = nh; cd.MipLevels = 1; cd.ArraySize = 1; cd.Format = sd.Format;
                cd.SampleDesc = { 1, 0 }; cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SHADER_RESOURCE_VIEW_DESC vd = {}; vd.Format = Blitter::ViewFormat(sd.Format, DXGI_FORMAT_UNKNOWN);
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
                if (FAILED(dev->CreateTexture2D(&cd, nullptr, &copy)) || FAILED(dev->CreateShaderResourceView(copy, &vd, &copySrv)))
                {
                    if (pass.fails++ < 5) LOGI("XRSession: panel: crop copy {}x{} format {} failed", nw, nh, (int)sd.Format);
                    if (copy) { copy->Release(); copy = nullptr; } return false;
                }
                cw = nw; ch = nh; cf = sd.Format;
            }
            const D3D11_BOX box = { x0, y0, 0, x1, y1, 1 };
            ctx->CopySubresourceRegion(copy, 0, 0, 0, 0, l.tex, 0, &box);
            view = copySrv; crop[0] = 0; crop[1] = 0; crop[2] = (float)nw; crop[3] = (float)nh; sw = (float)nw; sh = (float)nh;
        }
        if (!view) return false;
        const float k[12] = { crop[0], crop[1], crop[2], crop[3], sw, sh, l.alpha, l.premultiplied ? 1.f : 0.f,
                              l.corner, std::max(l.soft, 1.5f / (float)h), l.height > 0 ? l.width / l.height : 1.f, l.gamma ? 1.f : 0.f };
        return pass.Run(dev, ctx, dst, w, h, swapFmt, k, view, "panel");
    }
    void Release()
    {
        pass.Release();
        if (copySrv) copySrv->Release(); if (copy) copy->Release();
        copySrv = nullptr; copy = nullptr; cw = ch = 0;
    }
} g_PanelR;

// ---- the menu screen's surroundings (XRBackdrop) ----
// Each eye's pixel is a direction in the room (its fov's tangents turned by its pose right now): a dark room-locked
// environment by that direction (a faint horizon, the screen's light spilling round it), and where the frozen picture
// of the game covers that direction, the picture -- desaturated and darkened -- exactly where it was seen.
const char* kBackdropHlsl = R"(
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID)
{
    V o; float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
cbuffer C : register(b0) { float4 eyeQ; float4 tans; float4 snapQ; float4 snapT; float4 p0; float4 glow; };   // p0: picture brightness, environment, fade, desaturation
Texture2D snap : register(t0); SamplerState sm : register(s0);
float3 qrot(float4 q, float3 v) { return v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
float4 ps(V i) : SV_Target
{
    float3 dl = normalize(float3(lerp(tans.x, tans.y, i.uv.x), lerp(tans.z, tans.w, i.uv.y), -1));
    float3 w = qrot(eyeQ, dl);
    float y = w.y;
    float3 sky = lerp(float3(0.0075, 0.0080, 0.0105), float3(0.0020, 0.0022, 0.0032), saturate(y * 1.4));
    float3 flo = lerp(float3(0.0055, 0.0052, 0.0056), float3(0.0012, 0.0012, 0.0014), saturate(-y * 2.0));
    float3 env = lerp(flo, sky, smoothstep(-0.03, 0.03, y));
    env += float3(0.0040, 0.0035, 0.0030) * exp(-y * y * 250);
    float g = saturate(dot(w, glow.xyz));
    env += float3(0.020, 0.022, 0.026) * pow(g, 14) * glow.w;
    float3 col = env * p0.y;
    if (p0.x > 0)
    {
        float3 s = qrot(float4(-snapQ.xyz, snapQ.w), w);
        if (s.z < -0.05)
        {
            float sx = s.x / -s.z, sy = s.y / -s.z;
            float2 uv = float2((sx - snapT.x) / (snapT.y - snapT.x), (snapT.z - sy) / (snapT.z - snapT.w));
            float2 e = min(uv, 1 - uv);
            float edge = saturate(min(e.x, e.y) / 0.08);
            if (edge > 0)
            {
                float3 c = snap.SampleLevel(sm, saturate(uv), 0).rgb;
                c = lerp(c, dot(c, float3(0.2126, 0.7152, 0.0722)), p0.w) * p0.x;
                col = lerp(col, c, edge);
            }
        }
    }
    col *= p0.z;
    float n = frac(sin(dot(i.p.xy, float2(12.9898, 78.233))) * 43758.5453) - 0.5;
    return float4(max(col + n * 0.0005, 0), 1);
}
)";
Pass g_BackdropR;
// the frozen-backdrop ring's textures (XRSession::OfferSnapshot)
// [slot][eye]: a stereo pair keeps BOTH eyes' pictures (each eye's turned frustum covers its own outer edge); a mono
// picture only [slot][0]
ID3D11Texture2D* g_SnapTex[3][2] = {}; ID3D11ShaderResourceView* g_SnapSrv[3][2] = {}; UINT g_SnapW = 0, g_SnapH = 0; DXGI_FORMAT g_SnapFmt = DXGI_FORMAT_UNKNOWN;
void ReleaseSnaps()
{
    for (int i = 0; i < 3; i++) for (int e = 0; e < 2; e++)
    { if (g_SnapSrv[i][e]) g_SnapSrv[i][e]->Release(); if (g_SnapTex[i][e]) g_SnapTex[i][e]->Release(); g_SnapSrv[i][e] = nullptr; g_SnapTex[i][e] = nullptr; }
    g_SnapW = g_SnapH = 0;
}
long long Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
double QpcSec(long long d) { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); return (double)d / (double)f.QuadPart; }


// ---- the wrist HUD (XRWristLayer): the game's interface cropped into a square quad on the left wrist ----
// The source is premultiplied (WristHud repeats the game's interface draws into a transparent texture), so is the
// output. The crop is softened where it would show a hard line: at the edges of the source (the cluster's smoke runs
// off the top of a 16:9 screen) and at the quad's rim; a dark disc behind keeps it legible over a bright scene.
const char* kWristHlsl = R"(
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID)
{
    V o; float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
cbuffer C : register(b0) { float4 crop; float4 src; float4 opt; };   // crop: centre x, y, half side, edge fade (px); src: w, h, alpha, backdrop; opt.x: gamma source
Texture2D tx : register(t0); SamplerState sm : register(s0);
float3 Linear(float3 s) { return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4); }
float4 ps(V i) : SV_Target
{
    float2 d = i.uv * 2 - 1;
    float2 px = crop.xy + d * crop.z;
    float4 c = tx.Sample(sm, px / src.xy);
    if (opt.x > 0.5) c.rgb = Linear(c.a > 1e-4 ? saturate(c.rgb / c.a) : 0) * c.a;   // gamma-space premultiplied -> linear
    float edge = min(min(px.x, src.x - px.x), min(px.y, src.y - px.y));
    c *= saturate(edge / max(crop.w, 1));
    float r = length(d);
    c *= 1 - smoothstep(0.86, 1.0, r);
    float ba = src.w * (1 - smoothstep(0.78, 0.98, r));
    float4 b = float4(float3(0.012, 0.012, 0.016) * ba, ba);
    return (c + b * (1 - c.a)) * src.z;
}
)";
struct WristRenderer
{
    bool failed = false, ready = false;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11SamplerState* smp = nullptr;
    ID3D11RasterizerState* rs = nullptr; ID3D11Buffer* cb = nullptr;
    ID3DDeviceContextState* state = nullptr;
    unsigned long long draws = 0, fails = 0;

    bool Init(ID3D11Device* dev)
    {
        if (ready || failed) return ready;
        failed = true;
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
        if (!compile) { LOGI("XRSession: wrist HUD: d3dcompiler_47 not available"); return false; }
        ID3DBlob *bv = nullptr, *bp = nullptr, *err = nullptr;
        if (FAILED(compile(kWristHlsl, strlen(kWristHlsl), "wrist", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &bv, &err)) ||
            FAILED(compile(kWristHlsl, strlen(kWristHlsl), "wrist", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &bp, &err)))
        {
            LOGI("XRSession: wrist HUD shader compile failed: {}", err ? (const char*)err->GetBufferPointer() : "?");
            if (err) err->Release(); if (bv) bv->Release(); if (bp) bp->Release();
            return false;
        }
        dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &vs);
        dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, &ps);
        bv->Release(); bp->Release();
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;   // outside the screen: transparent
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &smp);
        D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        dev->CreateRasterizerState(&rd, &rs);
        D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 48; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&bd, nullptr, &cb);
        ID3D11Device1* d1 = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&d1)) && d1)
        {
            const D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
            d1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &state);
            d1->Release();
        }
        if (!vs || !ps || !smp || !rs || !cb || !state) { LOGI("XRSession: wrist HUD: resource creation failed"); return false; }
        failed = false; ready = true;
        LOGI("XRSession: wrist HUD renderer ready");
        return true;
    }
    bool Draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, const XRWristLayer& l, ID3D11Texture2D* dst, UINT w, UINT h, DXGI_FORMAT swapFmt)
    {
        if (!Init(dev)) return false;
        ID3D11RenderTargetView* rtv = nullptr;
        {
            D3D11_TEXTURE2D_DESC dd = {}; dst->GetDesc(&dd);
            D3D11_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = Blitter::ViewFormat(dd.Format, swapFmt); rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            const HRESULT hr = dev->CreateRenderTargetView(dst, &rd, &rtv);
            if (FAILED(hr) || !rtv) { if (fails++ < 5) LOGI("XRSession: wrist HUD: render target view failed (hr 0x{:08X})", (unsigned)hr); return false; }
        }
        ID3D11DeviceContext1* c1 = nullptr;
        if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1)) || !c1) { rtv->Release(); return false; }
        ID3DDeviceContextState* prev = nullptr;
        c1->SwapDeviceContextState(state, &prev);
        const float k[12] = { l.center[0], l.center[1], l.half, l.edgeFade, (float)l.srcW, (float)l.srcH, l.alpha, l.backdrop, l.gamma ? 1.f : 0.f, 0.f, 0.f, 0.f };
        c1->UpdateSubresource(cb, 0, nullptr, k, 0, 0);
        const float clear[4] = { 0, 0, 0, 0 };
        c1->ClearRenderTargetView(rtv, clear);
        c1->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = { 0.f, 0.f, (float)w, (float)h, 0.f, 1.f };
        c1->RSSetViewports(1, &vp);
        c1->RSSetState(rs);
        c1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c1->VSSetShader(vs, nullptr, 0);
        c1->PSSetShader(ps, nullptr, 0);
        ID3D11ShaderResourceView* srv = l.srv;
        c1->PSSetShaderResources(0, 1, &srv);
        c1->PSSetSamplers(0, 1, &smp);
        c1->PSSetConstantBuffers(0, 1, &cb);
        c1->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr; c1->PSSetShaderResources(0, 1, &none);
        c1->OMSetRenderTargets(0, nullptr, nullptr);
        c1->SwapDeviceContextState(prev, nullptr);
        if (prev) prev->Release();
        c1->Release(); rtv->Release();
        draws++;
        return true;
    }
    void Release()
    {
        for (IUnknown* u : std::initializer_list<IUnknown*>{ vs, ps, smp, rs, cb, state }) if (u) u->Release();
        vs = nullptr; ps = nullptr; smp = nullptr; rs = nullptr; cb = nullptr; state = nullptr; ready = false; failed = false;
    }
} g_Wrist;

XrPath MakePath(const char* s)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g.instance, s, &p);
    return p;
}

const char* StateName(XrSessionState s)
{
    switch (s)
    {
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    default: return "?";
    }
}
} // namespace

#define XR_FAILED_LOG(call, what)                                              \
    do {                                                                       \
        XrResult _r = (call);                                                  \
        if (XR_FAILED(_r)) {                                                   \
            m_LastError = std::string(what) + " failed: " + std::to_string((int)_r); \
            LOGI("XRSession: {}", m_LastError);                     \
            return false;                                                      \
        }                                                                      \
    } while (0)

bool XRSession::Init(ID3D11Device* device)
{
    if (m_Initialized)
        return true;
    if (!device)
    {
        m_LastError = "null device";
        return false;
    }

    m_Retry = false;
    // --- Instance (require the D3D11 graphics extension). Kept across retries: only the system is asked again. ---
    if (!g.instance)
    {
        const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
        XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
        ici.enabledExtensionCount = 1;
        ici.enabledExtensionNames = exts;
        strncpy(ici.applicationInfo.applicationName, "DishonoredVR", XR_MAX_APPLICATION_NAME_SIZE - 1);
        // OpenXR 1.0: nothing here needs 1.1, and runtimes that only speak 1.0 (older Virtual Desktop, Pico, WMR builds)
        // refuse an instance that asks for 1.1. A runtime that refuses 1.0 gets asked for the loader's version below.
        ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        // SteamVR's runtime returns XR_ERROR_RUNTIME_FAILURE for a little while after the probe's instance is destroyed
        // (async teardown) and while it is still starting, so the first attempt retries for a few seconds. Later
        // attempts (VRRuntime retries every few seconds) try once: this runs on the game's render thread.
        XrResult r = XR_ERROR_RUNTIME_FAILURE;
        const int attempts = m_InitAttempts++ == 0 ? 10 : 1;
        for (int attempt = 0; attempt < attempts; ++attempt)
        {
            r = xrCreateInstance(&ici, &g.instance);
            if (r == XR_ERROR_API_VERSION_UNSUPPORTED && ici.applicationInfo.apiVersion != XR_CURRENT_API_VERSION)
            {
                LOGI("XRSession: the runtime refuses OpenXR 1.0: asking for {}.{}", XR_VERSION_MAJOR(XR_CURRENT_API_VERSION), XR_VERSION_MINOR(XR_CURRENT_API_VERSION));
                ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
                r = xrCreateInstance(&ici, &g.instance);
            }
            if (XR_SUCCEEDED(r) || r != XR_ERROR_RUNTIME_FAILURE)
                break;
            LOGI("XRSession: xrCreateInstance attempt {} failed: {}", attempt + 1, (int)r);
            Sleep(500);
        }
        if (XR_FAILED(r))
        {
            g.instance = XR_NULL_HANDLE;
            // Retried for about a minute only: creating an instance STARTS some runtimes (SteamVR), which then close
            // themselves again without a headset -- retrying for ever would pop SteamVR up every few seconds on a
            // player who started the game to play it flat.
            // LIMIT_REACHED: another instance in this process is still alive (Meta's runtime allows one).
            m_Retry = (r == XR_ERROR_RUNTIME_FAILURE || r == XR_ERROR_INSTANCE_LOST || r == XR_ERROR_LIMIT_REACHED) && m_InitAttempts < 20;
            if (r == XR_ERROR_RUNTIME_UNAVAILABLE)
                m_LastError = "no OpenXR runtime is available (active runtime: " + SysInfo::ActiveOpenXRRuntime() + ")";
            else if (r == XR_ERROR_EXTENSION_NOT_PRESENT)
                m_LastError = "the active OpenXR runtime does not support Direct3D 11 (XR_KHR_D3D11_enable)";
            else
                m_LastError = "xrCreateInstance failed: " + std::to_string((int)r) + (m_Retry ? " (the runtime may still be starting: retrying)"
                    : " -- start the headset and its PC app (Meta Horizon Link / SteamVR / Virtual Desktop) before the game");
            return false;
        }
        XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
        if (XR_SUCCEEDED(xrGetInstanceProperties(g.instance, &ip)))
        {
            m_RuntimeName = ip.runtimeName;
            LOGI("XRSession: OpenXR runtime '{}' version {}.{}.{}", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
                 XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
        }
    }

    // --- System (HMD). Not there = the headset is off, asleep or not streaming (Link / Air Link / Virtual Desktop
    // not connected yet): keep the instance and let VRRuntime ask again in a few seconds. ---
    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    {
        const XrResult r = xrGetSystem(g.instance, &sgi, &g.systemId);
        if (XR_FAILED(r))
        {
            m_Retry = r == XR_ERROR_FORM_FACTOR_UNAVAILABLE || r == XR_ERROR_RUNTIME_FAILURE;
            if (r == XR_ERROR_INSTANCE_LOST) { xrDestroyInstance(g.instance); g.instance = XR_NULL_HANDLE; m_Retry = true; }
            m_LastError = r == XR_ERROR_FORM_FACTOR_UNAVAILABLE
                ? "no headset found (off, asleep, or Link / Air Link / Virtual Desktop not connected yet): waiting for it"
                : "xrGetSystem failed: " + std::to_string((int)r);
            return false;
        }
    }

    XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
    if (XR_SUCCEEDED(xrGetSystemProperties(g.instance, g.systemId, &sp)))
    {
        m_SystemName = sp.systemName;
        LOGI("XRSession: headset '{}' (vendor {:#x}), max swapchain {}x{}, orientation tracking {}, position tracking {}", sp.systemName,
             sp.vendorId, sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight,
             (bool)sp.trackingProperties.orientationTracking, (bool)sp.trackingProperties.positionTracking);
    }

    // --- Required: query graphics requirements before session creation. The runtime composites on ONE adapter, and
    // the session must be created on a device of that adapter: on a laptop with two GPUs the game may be running on the
    // integrated one, and the runtime then refuses the session. Compare, so the log can say exactly that. ---
    PFN_xrGetD3D11GraphicsRequirementsKHR pfnGetReq = nullptr;
    xrGetInstanceProcAddr(g.instance, "xrGetD3D11GraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&pfnGetReq);
    bool luidMismatch = false;
    if (pfnGetReq)
    {
        XrGraphicsRequirementsD3D11KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
        if (XR_SUCCEEDED(pfnGetReq(g.instance, g.systemId, &req)))
        {
            IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr; DXGI_ADAPTER_DESC d{};
            if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && dd && SUCCEEDED(dd->GetAdapter(&ad)) && ad) ad->GetDesc(&d);
            if (ad) ad->Release(); if (dd) dd->Release();
            // a zero LUID = the runtime does not say (the mock): nothing to compare
            const bool said = req.adapterLuid.LowPart || req.adapterLuid.HighPart;
            luidMismatch = said && (d.AdapterLuid.LowPart != req.adapterLuid.LowPart || d.AdapterLuid.HighPart != req.adapterLuid.HighPart);
            LOGI("XRSession: the runtime needs the GPU with LUID {:08X}:{:08X}; the game renders on LUID {:08X}:{:08X}{}",
                 (unsigned)req.adapterLuid.HighPart, (unsigned)req.adapterLuid.LowPart, (unsigned)d.AdapterLuid.HighPart, (unsigned)d.AdapterLuid.LowPart,
                 !said ? " (the runtime does not say which)" : luidMismatch ? " -- DIFFERENT GPUs" : " (same GPU)");
        }
    }

    // --- View configuration (stereo = 2 views) ---
    uint32_t viewCount = 0;
    XR_FAILED_LOG(xrEnumerateViewConfigurationViews(g.instance, g.systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr),
        "xrEnumerateViewConfigurationViews(count)");
    if (viewCount == 0 || viewCount > kMaxEyes)
    {
        m_LastError = "unexpected view count " + std::to_string(viewCount);
        return false;
    }
    std::vector<XrViewConfigurationView> viewConfigs(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    XR_FAILED_LOG(xrEnumerateViewConfigurationViews(g.instance, g.systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, viewConfigs.data()),
        "xrEnumerateViewConfigurationViews");
    m_ViewCount = (int)viewCount;
    m_EyeWidth = viewConfigs[0].recommendedImageRectWidth;
    m_EyeHeight = viewConfigs[0].recommendedImageRectHeight;

    // --- Session bound to the game's D3D11 device ---
    g.device = device;
    device->GetImmediateContext(&g.context);
    XrGraphicsBindingD3D11KHR binding = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    binding.device = device;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &binding;
    sci.systemId = g.systemId;
    {
        const XrResult r = xrCreateSession(g.instance, &sci, &g.session);
        if (XR_FAILED(r))
        {
            m_LastError = (r == XR_ERROR_GRAPHICS_DEVICE_INVALID || luidMismatch)
                ? "xrCreateSession failed (" + std::to_string((int)r) + "): the game is running on a different GPU than the headset needs. "
                  "Windows Settings > System > Display > Graphics: set Dishonored2.exe to 'High performance', then restart the game"
                : "xrCreateSession failed: " + std::to_string((int)r);
            g.session = XR_NULL_HANDLE;
            return false;
        }
    }

    // --- Reference space ---
    XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XR_FAILED_LOG(xrCreateReferenceSpace(g.session, &rsci, &g.space), "xrCreateReferenceSpace");

    // VIEW space for head-locked layers (HUD quad). Best-effort.
    XrReferenceSpaceCreateInfo vsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    vsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vsci.poseInReferenceSpace.orientation.w = 1.0f;
    xrCreateReferenceSpace(g.session, &vsci, &g.viewSpace);

    // --- Pick a swapchain color format ---
    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(g.session, 0, &fmtCount, nullptr);
    std::vector<int64_t> formats(fmtCount);
    xrEnumerateSwapchainFormats(g.session, fmtCount, &fmtCount, formats.data());
    // sRGB, not whichever the runtime happens to list first. The backbuffer we
    // copy from holds pixels the game already gamma-encoded for a monitor. A
    // plain UNORM swapchain tells the runtime they are LINEAR, so it encodes
    // them a SECOND time: a 0.22 mid-grey leaves as 0.50 — lifted midtones,
    // grey blacks, flat colour (user-reported on Quest 3, whose runtime lists
    // UNORM first; SteamVR listed sRGB first, which is why this never showed
    // headless). CopySubresourceRegion between the two is legal — same
    // R8G8B8A8_TYPELESS family — and copies the bits verbatim, which is exactly
    // the reinterpretation we want.
    int64_t chosen = formats.empty() ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : formats[0];
    for (int64_t f : formats)
    {
        if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        {
            chosen = f;
            break;
        }
        if (f == DXGI_FORMAT_R8G8B8A8_UNORM)
            chosen = f; // usable fallback; keep looking for the sRGB variant
    }

    // --- One swapchain per eye ---
    for (int i = 0; i < m_ViewCount; i++)
    {
        XrSwapchainCreateInfo scci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        scci.format = chosen;
        scci.sampleCount = 1;
        scci.width = m_EyeWidth;
        scci.height = m_EyeHeight;
        scci.faceCount = 1;
        scci.arraySize = 1;
        scci.mipCount = 1;
        XR_FAILED_LOG(xrCreateSwapchain(g.session, &scci, &g.swapchains[i]), "xrCreateSwapchain");
        m_SwapW[i] = m_EyeWidth;
        m_SwapH[i] = m_EyeHeight;
        m_SwapFormat = chosen;

        uint32_t imgCount = 0;
        xrEnumerateSwapchainImages(g.swapchains[i], 0, &imgCount, nullptr);
        g.images[i].assign(imgCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        xrEnumerateSwapchainImages(g.swapchains[i], imgCount, &imgCount,
            (XrSwapchainImageBaseHeader*)g.images[i].data());
    }

    // Controller input (best-effort; VR still works without it).
    m_InputReady = InitInput();
    if (!m_InputReady)
        LOGI("XRSession: input setup skipped/failed (VR view still works)");

    m_Initialized = true;
    LOGI("XRSession: initialized. runtime='{}' system='{}' views={} eye={}x{} fmt={} input={}",
        m_RuntimeName, m_SystemName, m_ViewCount, m_EyeWidth, m_EyeHeight, (int)chosen, m_InputReady);
    // The keep-alive frames are sent from the frame thread while the render thread may be inside the game's D3D calls,
    // and a runtime may touch the immediate context in its frame calls: the device's own lock serialises the two (once
    // on, every immediate-context call takes it -- uncontended, a few ns).
    if (!pump.mt && SUCCEEDED(g.context->QueryInterface(__uuidof(ID3D11Multithread), (void**)&pump.mt)) && pump.mt)
    {
        const bool was = pump.mt->GetMultithreadProtected() != 0;
        if (!was && m_KeepAliveMode) pump.mt->SetMultithreadProtected(TRUE);
        LOGI("XRSession: D3D11 multithread protection {} (keep-alive frames lock the device)", was ? "was already on" : m_KeepAliveMode ? "switched on" : "off (keep-alive off)");
        if (!was && !m_KeepAliveMode) { pump.mt->Release(); pump.mt = nullptr; }
    }
    StartPump();
    return true;
}

bool XRSession::InitInput()
{
    XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy_s(asci.actionSetName, "dishonoredvr");
    strcpy_s(asci.localizedActionSetName, "Dishonored 2 VR");
    if (XR_FAILED(xrCreateActionSet(g.instance, &asci, &g.actionSet)))
        return false;

    // The localized names are what SteamVR's binding editor shows (Settings > Controllers > Manage Controller Bindings),
    // where a player re-assigns a button the runtime's automatic conversion left out (the PS VR2's Create button).
    auto mkAction = [&](const char* name, const char* shown, XrActionType type, XrAction& out) -> bool
    {
        XrActionCreateInfo aci = { XR_TYPE_ACTION_CREATE_INFO };
        strcpy_s(aci.actionName, name);
        strcpy_s(aci.localizedActionName, shown);
        aci.actionType = type;
        return XR_SUCCEEDED(xrCreateAction(g.actionSet, &aci, &out));
    };

    if (!mkAction("turn", "Turn (right stick X)", XR_ACTION_TYPE_FLOAT_INPUT, g.turnAction)) return false;
    if (!mkAction("turn_y", "Right stick Y (menus)", XR_ACTION_TYPE_FLOAT_INPUT, g.turnYAction)) return false;
    if (!mkAction("move", "Move (left stick)", XR_ACTION_TYPE_VECTOR2F_INPUT, g.moveAction)) return false;
    if (!mkAction("trigger_l", "Left trigger: power / left-hand weapon", XR_ACTION_TYPE_FLOAT_INPUT, g.triggerAction[0])) return false;
    if (!mkAction("trigger_r", "Right trigger: sword / right-hand weapon", XR_ACTION_TYPE_FLOAT_INPUT, g.triggerAction[1])) return false;
    if (!mkAction("grip_l", "Left grip: power wheel", XR_ACTION_TYPE_FLOAT_INPUT, g.gripAction[0])) return false;
    if (!mkAction("grip_r", "Right grip: grab / interact / block", XR_ACTION_TYPE_FLOAT_INPUT, g.gripAction[1])) return false;
    if (!mkAction("btn_a", "A: jump (wheel open: mana potion)", XR_ACTION_TYPE_BOOLEAN_INPUT, g.btnAction[0])) return false;
    if (!mkAction("btn_b", "B: crouch (wheel open: health potion)", XR_ACTION_TYPE_BOOLEAN_INPUT, g.btnAction[1])) return false;
    if (!mkAction("btn_x", "X: quick slots 2 / 4", XR_ACTION_TYPE_BOOLEAN_INPUT, g.btnAction[2])) return false;
    if (!mkAction("btn_y", "Y: quick slots 1 / 3", XR_ACTION_TYPE_BOOLEAN_INPUT, g.btnAction[3])) return false;
    if (!mkAction("btn_thumb_l", "Left stick click: sprint", XR_ACTION_TYPE_BOOLEAN_INPUT, g.btnAction[4])) return false;
    if (!mkAction("btn_thumb_r", "Right stick click: zoom", XR_ACTION_TYPE_BOOLEAN_INPUT, g.btnAction[5])) return false;
    if (!mkAction("btn_menu", "Menu: journal (short) / pause (hold)", XR_ACTION_TYPE_BOOLEAN_INPUT, g.btnAction[6])) return false;
    if (!mkAction("aim_l", "Left hand pose", XR_ACTION_TYPE_POSE_INPUT, g.aimAction[0])) return false;
    if (!mkAction("aim_r", "Right hand pose", XR_ACTION_TYPE_POSE_INPUT, g.aimAction[1])) return false;
    // Optional: without it the wrist HUD falls back to the aim pose.
    if (!mkAction("wrist_l", "Left wrist pose (HUD)", XR_ACTION_TYPE_POSE_INPUT, g.wristAction)) g.wristAction = XR_NULL_HANDLE;
    // Haptics are a nicety: without them every input still works, so a runtime that refuses them is not fatal.
    m_HapticReady = mkAction("haptic_l", "Left vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT, g.hapticAction[0])
                 && mkAction("haptic_r", "Right vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT, g.hapticAction[1]);
    if (!m_HapticReady) LOGI("XRSession: no haptic output actions (the controllers will not vibrate)");

    // Suggested bindings, one call per controller family. Each call stands alone: a runtime that does not know a
    // profile (or one of its paths) rejects that call only, and the others still bind. What the runtime then actually
    // uses is reported by XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED (logged in PollEvents).
    //  - Meta Touch: Quest 1/2/3/Pro on any runtime (Meta Link, Virtual Desktop, SteamVR). SteamVR also presents the
    //    PS VR2 Sense controllers through this profile (measured by another OpenXR mod: Square/Triangle = X/Y,
    //    Cross/Circle = A/B), but its conversion drops the menu button -- hence the menu fallback in KbmInject (both
    //    stick clicks) and the readable action names above for SteamVR's binding editor.
    //  - Valve Index: the same layout; its left A/B are X/Y, and the menu is the left trackpad pressed (force).
    struct B { XrAction* a; const char* path; };
    auto suggest = [&](const char* profile, std::initializer_list<B> list)
    {
        std::vector<XrActionSuggestedBinding> binds;
        for (const B& b : list)
        {
            if (!*b.a) continue;
            XrPath p = MakePath(b.path);
            if (p != XR_NULL_PATH) binds.push_back({ *b.a, p });
        }
        XrInteractionProfileSuggestedBinding sug = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        sug.interactionProfile = MakePath(profile);
        sug.suggestedBindings = binds.data();
        sug.countSuggestedBindings = (uint32_t)binds.size();
        const XrResult r = xrSuggestInteractionProfileBindings(g.instance, &sug);
        LOGI("XRSession: bindings for {}: {}", profile, XR_SUCCEEDED(r) ? "accepted" : "rejected (" + std::to_string((int)r) + ")");
    };
    suggest("/interaction_profiles/oculus/touch_controller", {
        { &g.turnAction, "/user/hand/right/input/thumbstick/x" }, { &g.turnYAction, "/user/hand/right/input/thumbstick/y" },
        { &g.moveAction, "/user/hand/left/input/thumbstick" },
        { &g.triggerAction[0], "/user/hand/left/input/trigger/value" }, { &g.triggerAction[1], "/user/hand/right/input/trigger/value" },
        { &g.gripAction[0], "/user/hand/left/input/squeeze/value" }, { &g.gripAction[1], "/user/hand/right/input/squeeze/value" },
        { &g.btnAction[0], "/user/hand/right/input/a/click" }, { &g.btnAction[1], "/user/hand/right/input/b/click" },
        { &g.btnAction[2], "/user/hand/left/input/x/click" }, { &g.btnAction[3], "/user/hand/left/input/y/click" },
        { &g.btnAction[4], "/user/hand/left/input/thumbstick/click" }, { &g.btnAction[5], "/user/hand/right/input/thumbstick/click" },
        { &g.btnAction[6], "/user/hand/left/input/menu/click" },
        { &g.aimAction[0], "/user/hand/left/input/aim/pose" }, { &g.aimAction[1], "/user/hand/right/input/aim/pose" },
        { &g.wristAction, "/user/hand/left/input/grip/pose" },
        { &g.hapticAction[0], "/user/hand/left/output/haptic" }, { &g.hapticAction[1], "/user/hand/right/output/haptic" } });
    suggest("/interaction_profiles/valve/index_controller", {
        { &g.turnAction, "/user/hand/right/input/thumbstick/x" }, { &g.turnYAction, "/user/hand/right/input/thumbstick/y" },
        { &g.moveAction, "/user/hand/left/input/thumbstick" },
        { &g.triggerAction[0], "/user/hand/left/input/trigger/value" }, { &g.triggerAction[1], "/user/hand/right/input/trigger/value" },
        { &g.gripAction[0], "/user/hand/left/input/squeeze/value" }, { &g.gripAction[1], "/user/hand/right/input/squeeze/value" },
        { &g.btnAction[0], "/user/hand/right/input/a/click" }, { &g.btnAction[1], "/user/hand/right/input/b/click" },
        { &g.btnAction[2], "/user/hand/left/input/a/click" }, { &g.btnAction[3], "/user/hand/left/input/b/click" },
        { &g.btnAction[4], "/user/hand/left/input/thumbstick/click" }, { &g.btnAction[5], "/user/hand/right/input/thumbstick/click" },
        { &g.btnAction[6], "/user/hand/left/input/trackpad/force" },
        { &g.aimAction[0], "/user/hand/left/input/aim/pose" }, { &g.aimAction[1], "/user/hand/right/input/aim/pose" },
        { &g.wristAction, "/user/hand/left/input/grip/pose" },
        { &g.hapticAction[0], "/user/hand/left/output/haptic" }, { &g.hapticAction[1], "/user/hand/right/output/haptic" } });

    XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach.countActionSets = 1;
    attach.actionSets = &g.actionSet;
    if (XR_FAILED(xrAttachSessionActionSets(g.session, &attach)))
        return false;

    for (int i = 0; i < 2; i++)
    {
        XrActionSpaceCreateInfo asci2 = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        asci2.action = g.aimAction[i];
        asci2.poseInActionSpace.orientation.w = 1.0f;
        xrCreateActionSpace(g.session, &asci2, &g.aimSpace[i]);
    }
    if (g.wristAction)
    {
        XrActionSpaceCreateInfo wsci = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        wsci.action = g.wristAction;
        wsci.poseInActionSpace.orientation.w = 1.0f;
        if (XR_FAILED(xrCreateActionSpace(g.session, &wsci, &g.wristSpace))) g.wristSpace = XR_NULL_HANDLE;
    }
    return true;
}

void XRSession::SyncInput()
{
    if (!m_InputReady || !m_SessionRunning)
        return;

    XrActiveActionSet active = { g.actionSet, XR_NULL_PATH };
    XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(g.session, &syncInfo)))
        return;

    auto getFloat = [&](XrAction a) -> float
    {
        XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = a;
        XrActionStateFloat st = { XR_TYPE_ACTION_STATE_FLOAT };
        if (XR_SUCCEEDED(xrGetActionStateFloat(g.session, &gi, &st)) && st.isActive)
            return st.currentState;
        return 0.0f;
    };

    auto getBool = [&](XrAction a) -> bool
    {
        XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = a;
        XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
        if (XR_SUCCEEDED(xrGetActionStateBoolean(g.session, &gi, &st)) && st.isActive)
            return st.currentState != XR_FALSE;
        return false;
    };

    m_Input.time = (long long)g.frameState.predictedDisplayTime;
    m_Input.turnX = getFloat(g.turnAction);
    m_Input.turnY = getFloat(g.turnYAction);
    m_Input.triggerL = getFloat(g.triggerAction[0]);
    m_Input.triggerR = getFloat(g.triggerAction[1]);
    m_Input.gripL = getFloat(g.gripAction[0]);
    m_Input.gripR = getFloat(g.gripAction[1]);
    m_Input.buttons =
        (getBool(g.btnAction[0]) ? kXRBtnA : 0) |
        (getBool(g.btnAction[1]) ? kXRBtnB : 0) |
        (getBool(g.btnAction[2]) ? kXRBtnX : 0) |
        (getBool(g.btnAction[3]) ? kXRBtnY : 0) |
        (getBool(g.btnAction[4]) ? kXRBtnThumbL : 0) |
        (getBool(g.btnAction[5]) ? kXRBtnThumbR : 0) |
        (getBool(g.btnAction[6]) ? kXRBtnMenu : 0);

    {
        XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = g.moveAction;
        XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
        if (XR_SUCCEEDED(xrGetActionStateVector2f(g.session, &gi, &st)) && st.isActive)
        {
            m_Input.moveX = st.currentState.x;
            m_Input.moveY = st.currentState.y;
        }
    }

    if (m_LogInput) LogInputChanges();

    for (int i = 0; i < 2; i++)
    {
        if (!g.aimSpace[i])
            continue;
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        XRHandPose& h = m_Input.hand[i];
        constexpr XrSpaceLocationFlags kBoth = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_SUCCEEDED(xrLocateSpace(g.aimSpace[i], g.space, g.frameState.predictedDisplayTime, &loc))
            && (loc.locationFlags & kBoth) == kBoth)
        {
            if (m_PitchOffset != 0.f)
            {   // [Controls] ControllerPitchOffset: tilt the controller's pointing direction about its own X axis
                // (positive = up). For controllers whose aim pose sits at another angle than Touch's.
                const float h = m_PitchOffset * 0.5f * 3.14159265f / 180.f, sx = sinf(h), cw = cosf(h);
                const XrQuaternionf q = loc.pose.orientation;   // q * (sx, 0, 0, cw)
                loc.pose.orientation = { q.w * sx + q.x * cw, q.y * cw + q.z * sx, q.z * cw - q.y * sx, q.w * cw - q.x * sx };
            }
            h.posX = loc.pose.position.x; h.posY = loc.pose.position.y; h.posZ = loc.pose.position.z;
            h.quatX = loc.pose.orientation.x; h.quatY = loc.pose.orientation.y;
            h.quatZ = loc.pose.orientation.z; h.quatW = loc.pose.orientation.w;
            if (h.held) LOGI("XRSession: {} controller tracking is back", i ? "right" : "left");
            h.valid = true; h.held = false;
        }
        else if (h.valid || h.held)
        {   // Tracking lost (controller out of the cameras' view, put down, asleep). The pose is UNDEFINED with the
            // valid bits clear -- a runtime may hand back zeros -- and everything downstream (hand, IK, fire ray,
            // reticle) would take it at face value: the hand would fly to the floor under the origin. Hold the last
            // good pose instead; it comes back by itself when the runtime does.
            if (!h.held) LOGI("XRSession: {} controller lost tracking: holding its last pose", i ? "right" : "left");
            h.held = true; h.valid = true; m_LostFrames[i]++;
        }
    }
    for (int s = 0; s < 2; s++)
    {   // the wrist HUD's spaces, where they are right now: [0] the left grip pose (the aim pose if the runtime has no
        // grip), [1] the left aim pose as the runtime gives it (the hand above has ControllerPitchOffset applied)
        const XrSpace ws = s == 0 && g.wristSpace ? g.wristSpace : g.aimSpace[0];
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        constexpr XrSpaceLocationFlags kBoth = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        m_WristValid[s] = ws && XR_SUCCEEDED(xrLocateSpace(ws, g.space, g.frameState.predictedDisplayTime, &loc))
                       && (loc.locationFlags & kBoth) == kBoth;
        if (m_WristValid[s])
        {
            const XrPosef& w = loc.pose;
            m_WristPose[s][0] = w.orientation.x; m_WristPose[s][1] = w.orientation.y; m_WristPose[s][2] = w.orientation.z; m_WristPose[s][3] = w.orientation.w;
            m_WristPose[s][4] = w.position.x; m_WristPose[s][5] = w.position.y; m_WristPose[s][6] = w.position.z;
        }
    }
}

void XRSession::LocateHead(long long time, const XREyePose eyes[2], XREyePose& head)
{
    const XREyePose& a = eyes[0]; const XREyePose& b = eyes[m_ViewCount > 1 ? 1 : 0];
    head = a; head.fovLeft = head.fovRight = head.fovUp = head.fovDown = 0.f;
    head.posX = (a.posX + b.posX) * 0.5f; head.posY = (a.posY + b.posY) * 0.5f; head.posZ = (a.posZ + b.posZ) * 0.5f;
    // the eyes' average (nlerp; the two are at most a few tens of degrees apart): the fallback
    float s = a.quatX * b.quatX + a.quatY * b.quatY + a.quatZ * b.quatZ + a.quatW * b.quatW;
    const float k = s < 0.f ? -1.f : 1.f;
    float q[4] = { a.quatX + k * b.quatX, a.quatY + k * b.quatY, a.quatZ + k * b.quatZ, a.quatW + k * b.quatW };
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 1e-6f) { head.quatX = q[0] / n; head.quatY = q[1] / n; head.quatZ = q[2] / n; head.quatW = q[3] / n; }
    bool fromView = false;
    if (g.viewSpace && time > 0)
    {
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        constexpr XrSpaceLocationFlags kOri = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_SUCCEEDED(xrLocateSpace(g.viewSpace, g.space, (XrTime)time, &loc)) && (loc.locationFlags & kOri))
        {   // plausible: within 45 deg of the eyes' average, and its origin within 10 cm of their midpoint (the spec puts it
            // between the eyes; a runtime that gives something else is not trusted with the head)
            const XrQuaternionf& v = loc.pose.orientation;
            const float d = fabsf(v.x * head.quatX + v.y * head.quatY + v.z * head.quatZ + v.w * head.quatW);
            const float dx = loc.pose.position.x - head.posX, dy = loc.pose.position.y - head.posY, dz = loc.pose.position.z - head.posZ;
            const bool posOk = !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) || dx * dx + dy * dy + dz * dz < 0.01f;
            if (d > 0.92f && posOk) { head.quatX = v.x; head.quatY = v.y; head.quatZ = v.z; head.quatW = v.w; fromView = true; }
        }
    }
    if (fromView) m_HeadFromView++; else m_HeadAveraged++;
}

float XRSession::EyeCant(int i) const
{
    if (i < 0 || i >= m_ViewCount || !m_Eyes[i].valid) return 0.f;
    const XREyePose& e = m_Eyes[i]; const XREyePose& h = m_Head;
    const float d = fabsf(e.quatX * h.quatX + e.quatY * h.quatY + e.quatZ * h.quatZ + e.quatW * h.quatW);
    return 2.f * acosf(d > 1.f ? 1.f : d) * 57.29578f;
}

bool XRSession::LocateAt(long long time, XREyePose eyes[2], XRHandPose hands[2], XREyePose* head)
{
    if (!m_Initialized || !m_SessionRunning || !g.session || !g.space || m_ViewCount != 2 || time <= 0) return false;
    {   // a recenter that takes effect by then: the poses would be in the new space with the old generation
        std::lock_guard<std::mutex> lk(pump.m);
        if (m_RecenterPending && time >= m_RecenterAt) return false;
    }
    XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = (XrTime)time;
    vli.space = g.space;
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    XrView views[kMaxEyes] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t got = 0;
    XrResult r;
    { Watchdog::Mark mk("xrLocateViews (ahead)"); r = xrLocateViews(g.session, &vli, &vs, kMaxEyes, &got, views); }
    constexpr XrViewStateFlags kViewBoth = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    if (XR_FAILED(r) || got < 2 || (vs.viewStateFlags & kViewBoth) != kViewBoth) return false;
    for (int i = 0; i < 2; i++)
    {
        XREyePose& e = eyes[i]; e = m_Eyes[i];   // (fov from the frame)
        e.posX = views[i].pose.position.x; e.posY = views[i].pose.position.y; e.posZ = views[i].pose.position.z;
        e.quatX = views[i].pose.orientation.x; e.quatY = views[i].pose.orientation.y;
        e.quatZ = views[i].pose.orientation.z; e.quatW = views[i].pose.orientation.w;
        e.valid = true; e.held = false;
    }
    if (head) LocateHead(time, eyes, *head);
    for (int i = 0; i < 2; i++)
    {
        hands[i] = m_Input.hand[i];   // a hand that is not tracked keeps what SyncInput holds for it
        if (!m_InputReady || !g.aimSpace[i] || m_Input.hand[i].held) continue;
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        constexpr XrSpaceLocationFlags kBoth = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_FAILED(xrLocateSpace(g.aimSpace[i], g.space, (XrTime)time, &loc)) || (loc.locationFlags & kBoth) != kBoth) continue;
        if (m_PitchOffset != 0.f)
        {   // as SyncInput
            const float h = m_PitchOffset * 0.5f * 3.14159265f / 180.f, sx = sinf(h), cw = cosf(h);
            const XrQuaternionf q = loc.pose.orientation;
            loc.pose.orientation = { q.w * sx + q.x * cw, q.y * cw + q.z * sx, q.z * cw - q.y * sx, q.w * cw - q.x * sx };
        }
        XRHandPose& h = hands[i];
        h.posX = loc.pose.position.x; h.posY = loc.pose.position.y; h.posZ = loc.pose.position.z;
        h.quatX = loc.pose.orientation.x; h.quatY = loc.pose.orientation.y; h.quatZ = loc.pose.orientation.z; h.quatW = loc.pose.orientation.w;
        h.valid = true; h.held = false;
    }
    m_LocatesAhead++;
    return true;
}
long long XRSession::FrameDisplayTime() const { return (long long)g.frameState.predictedDisplayTime; }
long long XRSession::FramePeriod() const { return (long long)g.frameState.predictedDisplayPeriod; }

bool XRSession::WristPose(int space, float q[4], float p[3]) const
{
    const int s = space ? 1 : 0;
    if (!m_WristValid[s]) return false;
    for (int i = 0; i < 4; i++) q[i] = m_WristPose[s][i];
    for (int i = 0; i < 3; i++) p[i] = m_WristPose[s][4 + i];
    return true;
}

bool XRSession::Vibrate(int hand, float amplitude, float seconds, float hz)
{
    if (!m_InputReady || !m_HapticReady || !m_SessionRunning || (unsigned)hand > 1) return false;
    XrHapticVibration vib = { XR_TYPE_HAPTIC_VIBRATION };
    vib.amplitude = amplitude < 0.f ? 0.f : (amplitude > 1.f ? 1.f : amplitude);
    vib.duration = (XrDuration)(seconds * 1e9);
    vib.frequency = hz > 0.f ? hz : XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hai = { XR_TYPE_HAPTIC_ACTION_INFO };
    hai.action = g.hapticAction[hand];
    if (XR_FAILED(xrApplyHapticFeedback(g.session, &hai, (const XrHapticBaseHeader*)&vib))) return false;
    m_HapticPulses++;
    return true;
}

void XRSession::PollEvents()
{
    if (!m_Initialized || pump.thread)   // the OpenXR frame thread polls (PumpLoop)
        return;
    PollEventsNow();
}

void XRSession::PollEventsNow()
{
    if (!m_Initialized)
        return;

    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    for (;;)
    {
        XrResult pr;
        { Watchdog::Mark mk("xrPollEvent"); pr = xrPollEvent(g.instance, &ev); }
        if (pr != XR_SUCCESS) break;
        if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
        {   // the user recentred (Meta button held): the LOCAL origin jumps to where the head is now
            auto* rc = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&ev);
            m_RecenterEvents++;
            if (rc->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL || rc->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE)
            { std::lock_guard<std::mutex> lk(pump.m); m_RecenterPending = true; m_RecenterAt = rc->changeTime; }
            LOGI("XRSession: reference space change pending (recenter #{}, space {}, takes effect at {})", m_RecenterEvents, (int)rc->referenceSpaceType, (long long)rc->changeTime);
        }
        else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED)
        {   // which controller profile the runtime actually bound (Touch, Index...): the first thing to look at when a
            // tester's buttons do nothing
            std::string prof;
            for (int h = 0; h < 2; h++)
            {
                XrInteractionProfileState st = { XR_TYPE_INTERACTION_PROFILE_STATE };
                char buf[XR_MAX_PATH_LENGTH] = "none"; uint32_t n = 0;
                if (g.session && XR_SUCCEEDED(xrGetCurrentInteractionProfile(g.session, MakePath(h ? "/user/hand/right" : "/user/hand/left"), &st))
                    && st.interactionProfile != XR_NULL_PATH)
                    xrPathToString(g.instance, st.interactionProfile, sizeof buf, &n, buf);
                prof += fmt::format("{}{} {}", h ? ", " : "", h ? "right" : "left", buf);
            }
            if (prof != m_Profiles) LOGI("XRSession: controllers bound as: {}", prof);   // runtimes repeat the event
            m_Profiles = prof;
        }
        else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
        {
            LOGI("XRSession: the OpenXR runtime is going away (instance loss pending): VR stops and restarts when it is back");
            m_Restart = true;
        }
        else if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            auto* ssc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            m_SessionState = ssc->state;
            Watchdog::SetXrState((int)ssc->state);
            LOGI("XRSession: state -> {}", StateName(ssc->state));
            if (ssc->state == XR_SESSION_STATE_FOCUSED) MetaAsw::OnFocused(m_RuntimeName);   // Meta's ASW off for this game
            {   // the runtime taking a running session away and not offering it back: see PumpLoop (the new session)
                std::lock_guard<std::mutex> lk(pump.m);
                if (ssc->state == XR_SESSION_STATE_FOCUSED || ssc->state == XR_SESSION_STATE_VISIBLE) pump.wasRunning = true;
                else if (ssc->state == XR_SESSION_STATE_IDLE && pump.wasRunning) { pump.wasRunning = false; pump.idleSince = NowSec(); }
                else if (ssc->state == XR_SESSION_STATE_READY) pump.idleSince = 0;
            }
            if (ssc->state == XR_SESSION_STATE_LOSS_PENDING)
            {   // the headset was disconnected (Link cable, Air Link / Virtual Desktop connection, SteamVR closed...):
                // tear everything down and start over when it is back (VRRuntime retries)
                LOGI("XRSession: the headset connection was lost: VR stops and restarts when the headset is back");
                m_Restart = true;
            }
            else if (ssc->state == XR_SESSION_STATE_EXITING)
            {   // the runtime asked the app to quit (closed from the headset's menu, or the runtime is shutting down):
                // the game keeps running flat; the session is not re-created by itself (that would reopen it at once)
                LOGI("XRSession: the runtime asked the game to leave VR (EXITING): VR stays off until the game is restarted");
                m_Exited = true;
            }

            if (ssc->state == XR_SESSION_STATE_READY)
            {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult br;
                { Watchdog::Mark mk("xrBeginSession"); br = xrBeginSession(g.session, &bi); }
                if (XR_SUCCEEDED(br))
                {
                    std::lock_guard<std::mutex> lk(pump.m);
                    m_SessionRunning = true; pump.endPending = false;
                }
            }
            else if (ssc->state == XR_SESSION_STATE_STOPPING)
                EndSessionWhenFree();
        }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

//! The render thread's frame. The wait is the frame thread's (PumpLoop): this asks it for a frame and waits for it,
//! with a bound -- when the runtime does not release one in time the game goes on, this Present is not sent, and the
//! next one asks again (with a short bound, so the game runs at its own pace until the runtime is back).
bool XRSession::BeginFrame()
{
    if (!m_SessionRunning)
        return false;

    bool recenterPending = false; long long recenterAt = 0;
    if (pump.thread)
    {
        std::unique_lock<std::mutex> lk(pump.m);
        if (!pump.waited) { pump.request = true; pump.cv.notify_all(); }
        const int bound = pump.lastTimedOut ? 2 : m_WaitBoundMs;
        pump.cv.wait_for(lk, std::chrono::milliseconds(bound), [] { return pump.waited || pump.quit || pump.ending; });
        const double now = NowSec();
        if (!pump.waited || pump.ending || !m_SessionRunning)
        {
            pump.mainTimeouts++;
            if (!pump.lastTimedOut)
            {
                pump.lastTimedOut = true; pump.timedOutSince = now; pump.timeoutRuns++;
                if (pump.timeoutRuns <= 20)
                    LOGI("XRSession: the runtime has not released a frame in {} ms (frame thread {}): the game goes on, the headset waits",
                         bound, pump.waiting ? "inside xrWaitFrame" : "not waiting");
            }
            return false;
        }
        if (pump.lastTimedOut)
        {
            const double gone = now - pump.timedOutSince;
            if (gone > pump.longestTimeout) pump.longestTimeout = gone;
            if (pump.timeoutRuns <= 20) LOGI("XRSession: the runtime releases frames again ({:.2f} s without)", gone);
            pump.lastTimedOut = false;
        }
        if (pump.inRun)
        {
            const double d = now - pump.runStart;
            if (d > pump.longestRun) pump.longestRun = d;
            if (pump.keepAliveRuns <= 30) LOGI("XRSession: keep-alive over: the game's frames are back ({} held frames over {:.2f} s)", pump.runFrames, d);
            pump.inRun = false;
        }
        g.frameState = pump.state;
        pump.waited = false; pump.request = false;
        pump.mainOpen = true; pump.lastMainBegin = now; pump.mainFrames++;
        recenterPending = m_RecenterPending; recenterAt = m_RecenterAt;
    }
    else
    {   // no frame thread (it could not be started): the old way, on this thread
        g.frameState = { XR_TYPE_FRAME_STATE };
        XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
        XrResult wr;
        { Watchdog::Mark mk("xrWaitFrame"); wr = xrWaitFrame(g.session, &fwi, &g.frameState); }
        if (XR_FAILED(wr))
            return false;
        recenterPending = m_RecenterPending; recenterAt = m_RecenterAt;
    }

    XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
    XrResult r;
    { Watchdog::Mark mk("xrBeginFrame"); r = xrBeginFrame(g.session, &fbi); }
    if (XR_FAILED(r))
    {
        FrameEnded();
        return false;
    }
    m_FrameStarted = true;

    m_ShouldRender = g.frameState.shouldRender != 0;
    if (!m_ShouldRender)
        return false;

    // Locate the eye views for this frame.
    XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = g.frameState.predictedDisplayTime;
    vli.space = g.space;
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    uint32_t got = 0;
    for (int i = 0; i < m_ViewCount; i++)
        g.views[i] = { XR_TYPE_VIEW };
    { Watchdog::Mark mk("xrLocateViews"); xrLocateViews(g.session, &vli, &vs, m_ViewCount, &got, g.views); }
    if (recenterPending && g.frameState.predictedDisplayTime >= recenterAt)
    {   // from this frame on the poses are in the recentred space (the controllers are located at the same time)
        { std::lock_guard<std::mutex> lk(pump.m); m_RecenterPending = false; }
        m_RecenterGen++;
        LOGI("XRSession: recenter #{} in effect", m_RecenterGen);
    }

    // Head tracking lost (headset taken off, or its cameras covered): the view poses are undefined, and zeros would
    // drop the view to the floor. Hold the last good ones, as for the controllers.
    constexpr XrViewStateFlags kViewBoth = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    if ((vs.viewStateFlags & kViewBoth) != kViewBoth && m_Eyes[0].valid)
    {
        if (!m_Eyes[0].held) LOGI("XRSession: head lost tracking: holding its last pose");
        for (int i = 0; i < m_ViewCount; i++) m_Eyes[i].held = true;
        m_LostFrames[2]++;
        return true;
    }
    if (m_Eyes[0].held) LOGI("XRSession: head tracking is back");
    for (int i = 0; i < m_ViewCount; i++)
    {
        XREyePose& e = m_Eyes[i];
        e.held = false;
        e.posX = g.views[i].pose.position.x;
        e.posY = g.views[i].pose.position.y;
        e.posZ = g.views[i].pose.position.z;
        e.quatX = g.views[i].pose.orientation.x;
        e.quatY = g.views[i].pose.orientation.y;
        e.quatZ = g.views[i].pose.orientation.z;
        e.quatW = g.views[i].pose.orientation.w;
        e.fovLeft = g.views[i].fov.angleLeft;
        e.fovRight = g.views[i].fov.angleRight;
        e.fovUp = g.views[i].fov.angleUp;
        e.fovDown = g.views[i].fov.angleDown;
        e.valid = (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    }
    LocateHead((long long)g.frameState.predictedDisplayTime, m_Eyes, m_Head);
    if (!m_FovLogged && got > 0)
    {
        m_FovLogged = true;
        const float r2d = 180.0f / 3.14159265f;
        for (int i = 0; i < m_ViewCount; i++)
            LOGI("XRSession: eye {} native fov deg L={} R={} U={} D={}", i,
                g.views[i].fov.angleLeft * r2d, g.views[i].fov.angleRight * r2d,
                g.views[i].fov.angleUp * r2d, g.views[i].fov.angleDown * r2d);
        LOGI("XRSession: eyes turned from the head by {:.2f} / {:.2f} deg (canted displays if not 0), head orientation from {}", EyeCant(0),
             EyeCant(m_ViewCount > 1 ? 1 : 0), m_HeadFromView ? "the VIEW space" : "the eyes' average");
    }
    return true;
}

//! Grow-only recreate of eye swapchain i so needW x needH fits. The runtime's
//! recommended eye size can be SMALLER than the game backbuffer (Quest 3:
//! 1824x1968 vs 1920x1080) — the old fixed-size copy silently cropped the
//! frame and defeated the per-eye placement (double image on hardware).
bool XRSession::EnsureEyeSwapchain(int i, uint32_t needW, uint32_t needH)
{
    if (needW <= m_SwapW[i] && needH <= m_SwapH[i])
        return true;
    const uint32_t w = needW > m_SwapW[i] ? needW : m_SwapW[i];
    const uint32_t h = needH > m_SwapH[i] ? needH : m_SwapH[i];

    DestroySwapchain(g.swapchains[i]);
    g.images[i].clear();
    m_LastEyeTex[i] = nullptr;

    XrSwapchainCreateInfo scci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    scci.format = m_SwapFormat;
    scci.sampleCount = 1;
    scci.width = w;
    scci.height = h;
    scci.faceCount = 1;
    scci.arraySize = 1;
    scci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(g.session, &scci, &g.swapchains[i])))
    {
        LOGI("XRSession: eye {} swapchain regrow to {}x{} FAILED", i, w, h);
        m_SwapW[i] = m_SwapH[i] = 0;
        return false;
    }
    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(g.swapchains[i], 0, &imgCount, nullptr);
    g.images[i].assign(imgCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    xrEnumerateSwapchainImages(g.swapchains[i], imgCount, &imgCount,
        (XrSwapchainImageBaseHeader*)g.images[i].data());
    m_SwapW[i] = w;
    m_SwapH[i] = h;
    LOGI("XRSession: eye {} swapchain regrown to {}x{}", i, w, h);
    return true;
}

//! Matched-fov crop: the frame was rendered symmetric (±renderedTan); the
//! eye's native fov is a sub-rectangle of it, both being linear in tan space.
bool XRSession::MatchedCropRect(int i, unsigned srcW, unsigned srcH,
                                unsigned& sx, unsigned& sy, unsigned& w, unsigned& h) const
{
    if (m_RenderedTanH <= 0.0f || m_RenderedTanV <= 0.0f || !srcW || !srcH)
        return false;
    const XrFovf& fv = g.views[i].fov;
    const float kx = srcW / (2.0f * m_RenderedTanH);
    const float ky = srcH / (2.0f * m_RenderedTanV);
    float x0 = (tanf(fv.angleLeft) + m_RenderedTanH) * kx;
    float x1 = (tanf(fv.angleRight) + m_RenderedTanH) * kx;
    float y0 = (m_RenderedTanV - tanf(fv.angleUp)) * ky;
    float y1 = (m_RenderedTanV - tanf(fv.angleDown)) * ky;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > srcW) x1 = (float)srcW;
    if (y1 > srcH) y1 = (float)srcH;
    if (x1 - x0 < 16 || y1 - y0 < 16)
        return false;
    sx = (unsigned)x0; sy = (unsigned)y0;
    w = (unsigned)x1 - sx; h = (unsigned)y1 - sy;
    return true;
}

void XRSession::SubmitFrame(ID3D11Texture2D* source, int onlyEye)
{
    if (!m_FrameStarted)
        return;
    // Back from the menu screen in alternating stereo: the other eye's swapchain holds the backdrop, so this one picture
    // goes to both eyes (as mono) and the alternation resumes on the next frame.
    if (m_ResyncEyes) { onlyEye = -1; m_ResyncEyes = false; }
    const float fk = m_FrameFade * m_FrameDim;
    const float fade = fk < 0.f ? 0.f : (fk > 1.f ? 1.f : fk);
    m_FrameFade = 1.f; m_FrameDim = 1.f;

    XrCompositionLayerProjectionView projViews[kMaxEyes] = {};

    for (int i = 0; i < m_ViewCount; i++)
    {
        // Alternating-eye stereo: only the active eye gets this frame's image;
        // the other eye's layer view keeps referencing its last released image.
        if (onlyEye < 0 || i == onlyEye)
        {
            // Source rect for this eye + required swapchain size. Matched-fov:
            // crop the eye's native fov out of the symmetric render. Mono: the
            // whole source, plus margin to sit centered on the optical axis
            // (off-center by the fov asymmetry: the frame center lands at
            // fraction cx,cy of the texture, so each side needs sw/2 of room).
            unsigned srcX = 0, srcY = 0, cropW = 0, cropH = 0;
            bool cropped = false;
            float ox = 0, oy = 0, ow = 0, oh = 0; bool orthoRect = false;   // the ortho placement (EyeRect)
            if (source)
            {
                D3D11_TEXTURE2D_DESC sd = {};
                source->GetDesc(&sd);
                uint32_t needW = sd.Width, needH = sd.Height;
                if (m_MatchedFov)
                {
                    cropped = MatchedCropRect(i, sd.Width, sd.Height, srcX, srcY, cropW, cropH);
                    if (cropped) { needW = cropW; needH = cropH; }
                }
                else if (OrthoReady())
                {   // drawn scaled: the runtime's recommended size is all the eye can show (a declared fov: that density
                    // over the image's own frustum)
                    orthoRect = EyeRect(i, m_SrcTan, m_Declare, ox, oy, ow, oh);
                    needW = m_EyeWidth; needH = m_EyeHeight;
                }
                else
                {
                    const XrFovf& fv = g.views[i].fov;
                    const float tL = tanf(fv.angleLeft), tR = tanf(fv.angleRight);
                    const float tU = tanf(fv.angleUp), tD = tanf(fv.angleDown);
                    float cx = (0.0f - tL) / (tR - tL);
                    float cy = (tU - 0.0f) / (tU - tD);
                    cx = cx < 0.1f ? 0.1f : (cx > 0.9f ? 0.9f : cx);
                    cy = cy < 0.1f ? 0.1f : (cy > 0.9f ? 0.9f : cy);
                    const float halfW = sd.Width * 0.5f, halfH = sd.Height * 0.5f;
                    const float wA = halfW / cx, wB = halfW / (1.0f - cx);
                    const float hA = halfH / cy, hB = halfH / (1.0f - cy);
                    needW = (uint32_t)ceilf(wA > wB ? wA : wB);
                    needH = (uint32_t)ceilf(hA > hB ? hA : hB);
                }
                EnsureEyeSwapchain(i, needW, needH);
            }
            uint32_t imgIndex = 0;
            if (AcquireWaitImage(g.swapchains[i], imgIndex))
            {
                // Copy the source backbuffer into this eye image, placed at
                // the eye's OPTICAL CENTER under its native asymmetric fov.
                // Quest 2 eye fovs are mirrored-asymmetric (~15°), so a
                // pixel-centered copy lands at different angles per eye — an
                // un-fusable double image (advertising a custom symmetric fov
                // did not help; the compositor maps by the native fov). The
                // native projection is linear in tan-angle space: forward
                // (angle 0) sits at W*(-tanL)/(tanR-tanL). The copy is
                // unscaled, so the frame spans whatever angle its pixel size
                // covers there (~68° for 1080px) — smaller, but fused.
                ID3D11Texture2D* dst = g.images[i][imgIndex].texture;
                bool drawn = false;
                if (source && dst && g.context && g.device && !m_MatchedFov && OrthoReady() && orthoRect)
                {   // the source's tan rect inside the eye's native fov (both linear in tan), in swapchain pixels -- or, a
                    // declared fov, the whole image (EyeRect); warped when its frame is turned from the eye's
                    const XrPosef& rp = g.views[i].pose;
                    float pose[7] = { rp.orientation.x, rp.orientation.y, rp.orientation.z, rp.orientation.w, rp.position.x, rp.position.y, rp.position.z };
                    if (m_RenderPoseValid) for (int k = 0; k < 7; k++) pose[k] = m_RenderPose[k];   // what it was really rendered from
                    if (DrawInto(i, source, nullptr, dst, m_SrcTan, m_SrcWarp[i], m_Declare, fade, pose))
                    {
                        drawn = true;   // (DrawInto recorded the content rect and the pose the layer claims)
                    }
                }
                if (!drawn && source && dst && g.context && g.device)
                {
                    D3D11_TEXTURE2D_DESC sd = {}, dd = {};
                    source->GetDesc(&sd);
                    dst->GetDesc(&dd);
                    UINT w = cropped ? cropW : sd.Width;
                    UINT h = cropped ? cropH : sd.Height;
                    if (w > dd.Width) w = dd.Width;
                    if (h > dd.Height) h = dd.Height;

                    UINT dx = 0, dy = 0;
                    if (!m_MatchedFov)
                    {
                        const XrFovf& fv = g.views[i].fov;
                        const float tL = tanf(fv.angleLeft), tR = tanf(fv.angleRight);
                        const float tU = tanf(fv.angleUp), tD = tanf(fv.angleDown);
                        const float xC = dd.Width * (0.0f - tL) / (tR - tL);
                        const float yC = dd.Height * (tU - 0.0f) / (tU - tD);
                        float dxf = xC - w * 0.5f, dyf = yC - h * 0.5f;
                        if (dxf < 0) dxf = 0; if (dyf < 0) dyf = 0;
                        if (dxf + w > dd.Width) dxf = (float)(dd.Width - w);
                        if (dyf + h > dd.Height) dyf = (float)(dd.Height - h);
                        dx = (UINT)dxf; dy = (UINT)dyf;
                    }

                    // Clear first: the border region shows stale swapchain data
                    // otherwise (the copy no longer covers a fixed rect).
                    ID3D11RenderTargetView* rtv = nullptr;
                    if (SUCCEEDED(g.device->CreateRenderTargetView(dst, nullptr, &rtv)))
                    {
                        const float black[4] = { 0, 0, 0, 1 };
                        g.context->ClearRenderTargetView(rtv, black);
                        rtv->Release();
                    }

                    D3D11_BOX box = { srcX, srcY, 0, srcX + w, srcY + h, 1 };
                    g.context->CopySubresourceRegion(dst, 0, dx, dy, 0, source, 0, &box);
                    m_LastEyeTex[i] = dst; m_EyeDecl[i] = false;
                    m_ContentX[i] = (int)dx; m_ContentY[i] = (int)dy;
                    m_ContentW[i] = (int)w;  m_ContentH[i] = (int)h;
                    // This image was rendered with THIS frame's view pose —
                    // remember it so later frames submit the image under the
                    // pose it was made with (the compositor timewarps the
                    // difference). Alternating-eye holds each image for 2+
                    // frames; submitting it under the CURRENT pose skipped
                    // exactly one frame of head motion in the stale eye —
                    // constant jitter during head turns (Quest 3 user report).
                    const XrPosef& rp = g.views[i].pose;
                    m_EyeRenderPose[i][0] = rp.orientation.x;
                    m_EyeRenderPose[i][1] = rp.orientation.y;
                    m_EyeRenderPose[i][2] = rp.orientation.z;
                    m_EyeRenderPose[i][3] = rp.orientation.w;
                    m_EyeRenderPose[i][4] = rp.position.x;
                    m_EyeRenderPose[i][5] = rp.position.y;
                    m_EyeRenderPose[i][6] = rp.position.z;
                    if (m_RenderPoseValid) for (int k = 0; k < 7; k++) m_EyeRenderPose[i][k] = m_RenderPose[k];
                    m_EyePoseValid[i] = true;
                }

                XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                Watchdog::Mark mk("xrReleaseSwapchainImage");
                xrReleaseSwapchainImage(g.swapchains[i], &ri);
            }
        }

        // Native per-eye fov: the standard path every VR app uses, so
        // compositor reprojection has nothing to disagree about.
        // - Mono flat frame: full texture; its placement inside the texture
        //   (above) puts it on each eye's optical axis.
        // - Matched-fov stereo: the content rect was RENDERED with this fov,
        //   so it maps across the eye's whole fov (compositor scales).
        // Each eye under the pose its IMAGE was rendered with (fresh eye:
        // cached just above = this frame's; stale alternating eye: the pose
        // from its own render frame). The compositor timewarps the difference
        // to display time. Submitting the stale image under the CURRENT pose
        // hid exactly one frame of head motion from the compositor — seen as
        // constant one-eye jitter during head turns (Quest 3 user report).
        // Mono placement maps the WHOLE texture across the native fov, so the extent is the actual (possibly regrown)
        // swapchain size; a declared fov maps exactly the image's pixels (FillEyeView).
        FillEyeView(i, projViews[i]);
        if (m_MatchedFov && m_ContentW[i] > 0)
        {
            projViews[i].fov = g.views[i].fov;
            projViews[i].subImage.imageRect.offset = { 0, 0 };
            projViews[i].subImage.imageRect.extent = { m_ContentW[i], m_ContentH[i] };
        }
    }

    // Mono (both eyes get the same flat frame): give both projection views the
    // SAME pose (eye midpoint). Any per-eye pose/mapping difference on a flat
    // frame shows as an un-fusable double image (Quest 2, user-reported).
    if (onlyEye < 0 && m_ViewCount == 2)
    {
        XrPosef mid = g.views[0].pose;
        mid.position.x = (g.views[0].pose.position.x + g.views[1].pose.position.x) * 0.5f;
        mid.position.y = (g.views[0].pose.position.y + g.views[1].pose.position.y) * 0.5f;
        mid.position.z = (g.views[0].pose.position.z + g.views[1].pose.position.z) * 0.5f;
        if (m_RenderPoseValid)
        {   // the pose the image was really rendered from (see SetRenderPose)
            mid.orientation = { m_RenderPose[0], m_RenderPose[1], m_RenderPose[2], m_RenderPose[3] };
            mid.position = { m_RenderPose[4], m_RenderPose[5], m_RenderPose[6] };
        }
        // (an eye whose image was warped from a turned frame -- canted displays -- is claimed at its own orientation,
        // which DrawInto set: the render pose turned back by the warp)
        for (int i = 0; i < 2; i++)
            if (m_EyeDecl[i] || !(fabsf(m_SrcWarp[i][3]) < 0.9999999f)) projViews[i].pose = mid;
    }
    m_RenderPoseValid = false;
    for (int i = 0; i < kMaxEyes; i++) { m_SrcWarp[i][0] = m_SrcWarp[i][1] = m_SrcWarp[i][2] = 0.f; m_SrcWarp[i][3] = 1.f; }
    // a picture of the game for the menu screen's backdrop: the pose it is claimed at is where it was seen
    if (source && OrthoReady() && !m_MatchedFov)
    {
        const int e = onlyEye >= 0 && onlyEye < m_ViewCount ? onlyEye : 0;
        // (the frame the picture was RENDERED in, which its tans are in: a warped eye is claimed at another orientation)
        OfferSnapshot(source, m_EyeImgQuat[e]);
    }

    XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    layer.space = g.space;
    layer.viewCount = m_ViewCount;
    layer.views = projViews;

    EndFrameWithLayers(layer);
}

bool XRSession::StagePairLeft(ID3D11Texture2D* source, const float quat[4], const float pos[3], unsigned long long pair, const float* tan, const float* warp)
{
    m_PairStaged = false;
    memcpy(m_PairTan, tan ? tan : m_SrcTan, 16);
    if (warp) memcpy(m_PairWarp, warp, 16); else { m_PairWarp[0] = m_PairWarp[1] = m_PairWarp[2] = 0.f; m_PairWarp[3] = 1.f; }
    if (!source || !g.device || !g.context) return false;
    D3D11_TEXTURE2D_DESC sd = {}; source->GetDesc(&sd);
    if (!m_PairTex || sd.Width != m_PairW || sd.Height != m_PairH || (int)sd.Format != m_PairFmt)
    {
        if (m_PairSrv) { m_PairSrv->Release(); m_PairSrv = nullptr; } if (m_PairTex) { m_PairTex->Release(); m_PairTex = nullptr; }
        D3D11_TEXTURE2D_DESC cd = sd; cd.MipLevels = 1; cd.ArraySize = 1; cd.SampleDesc = { 1, 0 };
        cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.CPUAccessFlags = 0; cd.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd = {}; vd.Format = Blitter::ViewFormat(sd.Format, DXGI_FORMAT_UNKNOWN);
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
        if (FAILED(g.device->CreateTexture2D(&cd, nullptr, &m_PairTex)) || FAILED(g.device->CreateShaderResourceView(m_PairTex, &vd, &m_PairSrv)))
        {
            LOGI("XRSession: the pair's left copy {}x{} fmt {} could not be created", sd.Width, sd.Height, (int)sd.Format);
            if (m_PairTex) { m_PairTex->Release(); m_PairTex = nullptr; }
            return false;
        }
        m_PairW = sd.Width; m_PairH = sd.Height; m_PairFmt = (int)sd.Format;
    }
    g.context->CopyResource(m_PairTex, source);
    for (int k = 0; k < 4; k++) m_PairPose[k] = quat[k];
    for (int k = 0; k < 3; k++) m_PairPose[4 + k] = pos[k];
    m_PairId = pair; m_PairStaged = true;
    return true;
}

//! One eye of a synchronised pair: the image (a texture to copy, or a shader-readable view) drawn at its true angles
//! into the eye's swapchain, claimed at the pose it was rendered from.
bool XRSession::EyeRect(int i, const float tan[4], bool declare, float& x, float& y, float& w, float& h)
{
    const XrFovf& fv = g.views[i].fov;
    const float tL = tanf(fv.angleLeft), tR = tanf(fv.angleRight), tU = tanf(fv.angleUp), tD = tanf(fv.angleDown);
    if (!(tR > tL) || !(tU > tD) || !(tan[1] > tan[0]) || !(tan[2] > tan[3])) return false;
    if (declare)
    {   // the whole image, at the eye's own pixel density (the runtime's recommended size over its native fov)
        const float kx = (float)m_EyeWidth / (tR - tL), ky = (float)m_EyeHeight / (tU - tD);
        const uint32_t nw = (uint32_t)std::min(4096.f, ceilf(kx * (tan[1] - tan[0]))), nh = (uint32_t)std::min(4096.f, ceilf(ky * (tan[2] - tan[3])));
        EnsureEyeSwapchain(i, nw, nh);
        x = 0.f; y = 0.f; w = (float)std::min(nw, m_SwapW[i]); h = (float)std::min(nh, m_SwapH[i]);
        return w > 0.f && h > 0.f;
    }
    EnsureEyeSwapchain(i, m_EyeWidth, m_EyeHeight);
    // the source's tan rect inside the eye's native fov (both linear in tan), in swapchain pixels
    const float W = (float)m_SwapW[i], H = (float)m_SwapH[i];
    const float x0 = (tan[0] - tL) / (tR - tL) * W, x1 = (tan[1] - tL) / (tR - tL) * W;
    const float y0 = (tU - tan[2]) / (tU - tD) * H, y1 = (tU - tan[3]) / (tU - tD) * H;
    x = x0; y = y0; w = x1 - x0; h = y1 - y0;
    return W > 0.f && H > 0.f;
}

void XRSession::FillEyeView(int i, XrCompositionLayerProjectionView& pv) const
{
    pv = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
    const float* p = m_EyePoseValid[i] ? m_EyeRenderPose[i] : nullptr;
    if (p) { pv.pose.orientation = { p[0], p[1], p[2], p[3] }; pv.pose.position = { p[4], p[5], p[6] }; }
    else pv.pose = g.views[i].pose;
    pv.subImage.swapchain = g.swapchains[i];
    pv.subImage.imageArrayIndex = 0;
    pv.subImage.imageRect.offset = { 0, 0 };
    if (m_EyeDecl[i] && m_ContentW[i] > 0 && m_ContentH[i] > 0)
    {   // the fov the image was rendered with, and exactly the pixels it fills
        const float* t = m_EyeDeclTan[i];
        pv.fov = { atanf(t[0]), atanf(t[1]), atanf(t[2]), atanf(t[3]) };
        pv.subImage.imageRect.extent = { m_ContentW[i], m_ContentH[i] };
    }
    else
    {
        pv.fov = g.views[i].fov;
        pv.subImage.imageRect.extent = { (int)m_SwapW[i], (int)m_SwapH[i] };
    }
}

bool XRSession::DrawInto(int i, ID3D11Texture2D* source, ID3D11ShaderResourceView* srv, ID3D11Texture2D* dst, const float tan[4], const float* warp,
                         bool declare, float fade, const float pose[7])
{
    float x0 = 0, y0 = 0, rw = 0, rh = 0;
    if (!dst || !EyeRect(i, tan, declare, x0, y0, rw, rh)) return false;
    const bool turned = !declare && warp && fabsf(warp[3]) < 0.9999999f;
    float claim[7]; memcpy(claim, pose, 28);
    bool drawn = false;
    if (turned)
    {   // the eye's native fov, each of its pixels looked up in the turned image (Blitter::DrawWarp)
        ID3D11ShaderResourceView* v = srv;
        if (!v && g_Blit.Copy(g.device, g.context, source)) v = g_Blit.srv;
        const XrFovf& fv = g.views[i].fov;
        const float dstT[4] = { tanf(fv.angleLeft), tanf(fv.angleRight), tanf(fv.angleUp), tanf(fv.angleDown) };
        const float x = -warp[0], y = -warp[1], z = -warp[2], w = warp[3];   // conj(warp): eye -> render frame
        const float m[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                             2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                             2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) };
        drawn = v && g_Blit.DrawWarp(g.device, g.context, v, dst, dstT, tan, m, (DXGI_FORMAT)m_SwapFormat, fade);
        if (drawn)
        {   // claimed at the eye's orientation when it was rendered: render * conj(warp)
            const float* q = pose; const float c[4] = { -warp[0], -warp[1], -warp[2], warp[3] };
            claim[0] = q[3] * c[0] + q[0] * c[3] + q[1] * c[2] - q[2] * c[1]; claim[1] = q[3] * c[1] - q[0] * c[2] + q[1] * c[3] + q[2] * c[0];
            claim[2] = q[3] * c[2] + q[0] * c[1] - q[1] * c[0] + q[2] * c[3]; claim[3] = q[3] * c[3] - q[0] * c[0] - q[1] * c[1] - q[2] * c[2];
            x0 = 0.f; y0 = 0.f; rw = (float)m_SwapW[i]; rh = (float)m_SwapH[i];
        }
    }
    else
        drawn = srv ? g_Blit.DrawSrv(g.device, g.context, srv, dst, x0, y0, rw, rh, (DXGI_FORMAT)m_SwapFormat, fade, true)
                    : g_Blit.Draw(g.device, g.context, source, dst, x0, y0, rw, rh, (DXGI_FORMAT)m_SwapFormat, fade);
    if (!drawn) return false;
    m_LastEyeTex[i] = dst;
    m_ContentX[i] = (int)lroundf(x0); m_ContentY[i] = (int)lroundf(y0);
    m_ContentW[i] = (int)lroundf(rw); m_ContentH[i] = (int)lroundf(rh);
    m_EyeDecl[i] = declare; memcpy(m_EyeDeclTan[i], tan, 16);
    memcpy(m_EyeImgQuat[i], pose, 16);
    for (int k = 0; k < 7; k++) m_EyeRenderPose[i][k] = claim[k];
    m_EyePoseValid[i] = true;
    return true;
}

bool XRSession::DrawEye(int i, ID3D11Texture2D* source, ID3D11ShaderResourceView* srv, const float pose[7], const float tan[4], const float* warp,
                        float fade, XrCompositionLayerProjectionView& pv)
{
    const bool declare = m_Declare;
    { float x0, y0, rw, rh; EyeRect(i, tan, declare, x0, y0, rw, rh); }   // (the swapchain grown BEFORE an image is acquired)
    uint32_t imgIndex = 0;
    const bool acquired = AcquireWaitImage(g.swapchains[i], imgIndex);   // (not: the eye keeps its last image)
    ID3D11Texture2D* dst = acquired ? g.images[i][imgIndex].texture : nullptr;
    const bool drawn = DrawInto(i, source, srv, dst, tan, warp, declare, fade, pose);
    if (acquired)
    {
        XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        Watchdog::Mark mk("xrReleaseSwapchainImage"); xrReleaseSwapchainImage(g.swapchains[i], &ri);
    }
    FillEyeView(i, pv);
    return drawn;
}

bool XRSession::CopyPairLeft(ID3D11Texture2D* dst)
{
    if (!dst || !m_PairTex || !g.context) return false;
    D3D11_TEXTURE2D_DESC d = {}; dst->GetDesc(&d);
    if (d.Width != m_PairW || d.Height != m_PairH || (int)d.Format != m_PairFmt || d.SampleDesc.Count != 1) return false;
    g.context->CopyResource(dst, m_PairTex);
    return true;
}

bool XRSession::ResubmitLast()
{
    if (!m_FrameStarted) return false;
    XrCompositionLayerProjection layer; XrCompositionLayerProjectionView views[kMaxEyes];
    {
        std::lock_guard<std::mutex> lk(pump.m);
        if (!pump.haveLayers) return false;
        layer = pump.proj;
        for (uint32_t v = 0; v < layer.viewCount && v < (uint32_t)kMaxEyes; v++) views[v] = pump.views[v];
    }
    layer.views = views;
    EndFrameWithLayers(layer);
    return true;
}

bool XRSession::SubmitPair(ID3D11Texture2D* right, const float quat[4], const float pos[3], unsigned long long pair, const float* tan, const float* warp)
{
    float tanR[4]; memcpy(tanR, tan ? tan : m_SrcTan, 16);
    if (!m_FrameStarted || m_ViewCount != 2 || !OrthoReady() || m_MatchedFov) return false;
    if (!m_PairStaged || m_PairId != pair || !m_PairSrv) return false;
    const float fk = m_FrameFade * m_FrameDim;
    const float fade = fk < 0.f ? 0.f : (fk > 1.f ? 1.f : fk);
    m_FrameFade = 1.f; m_FrameDim = 1.f; m_ResyncEyes = false; m_RenderPoseValid = false;
    const float poseR[7] = { quat[0], quat[1], quat[2], quat[3], pos[0], pos[1], pos[2] };
    XrCompositionLayerProjectionView projViews[kMaxEyes] = {};
    DrawEye(0, nullptr, m_PairSrv, m_PairPose, m_PairTan, m_PairWarp, fade, projViews[0]);
    DrawEye(1, right, nullptr, poseR, tanR, warp, fade, projViews[1]);
    m_PairStaged = false;
    memcpy(m_SrcTan, tanR, 16);       // (the snapshot keeps the frustum of the picture it keeps)
    // the menu screen's frozen backdrop: both eyes' pictures (vr backdropeyes 1), or the right one's for both (0, before)
    OfferSnapshot(right, quat, m_BackdropEyes ? m_PairSrv : nullptr, m_PairPose, m_PairTan);
    XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    layer.space = g.space;
    layer.viewCount = m_ViewCount;
    layer.views = projViews;
    m_PairsSubmitted++;
    EndFrameWithLayers(layer);
    return true;
}

//! HUD quad layer: head-locked panel at a comfortable virtual depth. The
//! compositor samples it every display frame — always crisp, always fused,
//! unlike UI baked into the projection. Then ends the frame.
void XRSession::EndFrameWithLayers(XrCompositionLayerProjection& layer)
{
    XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    bool haveQuad = false;
    if (m_QuadSource && g.viewSpace)
    {
        D3D11_TEXTURE2D_DESC qd = {};
        m_QuadSource->GetDesc(&qd);

        if (g.quadSwapchain && (m_QuadW != qd.Width || m_QuadH != qd.Height))
        {
            DestroySwapchain(g.quadSwapchain);
            g.quadImages.clear();
        }
        if (!g.quadSwapchain)
        {
            XrSwapchainCreateInfo scci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
            scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            scci.format = qd.Format;
            scci.sampleCount = 1;
            scci.width = qd.Width;
            scci.height = qd.Height;
            scci.faceCount = 1;
            scci.arraySize = 1;
            scci.mipCount = 1;
            if (XR_SUCCEEDED(xrCreateSwapchain(g.session, &scci, &g.quadSwapchain)))
            {
                uint32_t n = 0;
                xrEnumerateSwapchainImages(g.quadSwapchain, 0, &n, nullptr);
                g.quadImages.assign(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
                xrEnumerateSwapchainImages(g.quadSwapchain, n, &n,
                    (XrSwapchainImageBaseHeader*)g.quadImages.data());
                m_QuadW = qd.Width;
                m_QuadH = qd.Height;
            }
        }

        if (g.quadSwapchain)
        {
            uint32_t qi = 0;
            if (AcquireWaitImage(g.quadSwapchain, qi))
            {
                if (g.context && g.quadImages[qi].texture)
                    g.context->CopyResource(g.quadImages[qi].texture, m_QuadSource);
                XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(g.quadSwapchain, &ri);

                // Locked-panel mode shows a backbuffer copy — its alpha
                // channel is not meaningful, so composite it opaque.
                quad.layerFlags = m_QuadLocked
                    ? 0 : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = g.quadSwapchain;
                quad.subImage.imageRect.offset = { 0, 0 };
                quad.subImage.imageRect.extent = { (int)m_QuadW, (int)m_QuadH };

                if (m_QuadLocked)
                {
                    // World-locked panel: capture the pose ONCE, in LOCAL
                    // space — in front of where the head is right now,
                    // upright (yaw only), facing the user. It then stays
                    // put while the head moves (floating inventory panel).
                    if (!m_QuadLockValid)
                    {
                        const XREyePose& e = m_Head;
                        // Head forward = quat * (0,0,-1).
                        const float fx = -(2.0f * (e.quatX * e.quatZ + e.quatW * e.quatY));
                        const float fz = -(1.0f - 2.0f * (e.quatX * e.quatX + e.quatY * e.quatY));
                        const float len = sqrtf(fx * fx + fz * fz);
                        const float nx = (len > 1e-4f) ? fx / len : 0.0f;
                        const float nz = (len > 1e-4f) ? fz / len : -1.0f;
                        m_QuadLockPos[0] = e.posX + nx * m_QuadDist;
                        m_QuadLockPos[1] = e.posY;
                        m_QuadLockPos[2] = e.posZ + nz * m_QuadDist;
                        // Yaw-only orientation whose -Z is the head forward
                        // (the quad's +Z face points back at the user).
                        const float yaw = atan2f(-nx, -nz);
                        m_QuadLockQuat[0] = 0.0f;
                        m_QuadLockQuat[1] = sinf(yaw * 0.5f);
                        m_QuadLockQuat[2] = 0.0f;
                        m_QuadLockQuat[3] = cosf(yaw * 0.5f);
                        m_QuadLockValid = true;
                    }
                    quad.space = g.space;                       // world (LOCAL)
                    quad.pose.position = { m_QuadLockPos[0], m_QuadLockPos[1], m_QuadLockPos[2] };
                    quad.pose.orientation = { m_QuadLockQuat[0], m_QuadLockQuat[1],
                                              m_QuadLockQuat[2], m_QuadLockQuat[3] };
                }
                else
                {
                    m_QuadLockValid = false;
                    quad.space = g.viewSpace;                   // head-locked
                    quad.pose.orientation.w = 1.0f;
                    quad.pose.position = { 0.0f, 0.0f, -m_QuadDist };
                }
                quad.size = { m_QuadWidth, m_QuadWidth * (float)m_QuadH / (float)m_QuadW };
                haveQuad = true;
            }
        }
    }

    XrCompositionLayerQuad panel[2] = { { XR_TYPE_COMPOSITION_LAYER_QUAD }, { XR_TYPE_COMPOSITION_LAYER_QUAD } };
    const bool havePanel[2] = { AppendPanelLayer(0, panel[0]), AppendPanelLayer(1, panel[1]) };
    XrCompositionLayerQuad wrist = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    const bool haveWrist = AppendWristLayer(wrist);
    const XrCompositionLayerBaseHeader* layers[5] = { (XrCompositionLayerBaseHeader*)&layer };
    uint32_t nLayers = 1;
    if (haveQuad) layers[nLayers++] = (XrCompositionLayerBaseHeader*)&quad;
    for (int s = 0; s < 2; s++) if (havePanel[s]) layers[nLayers++] = (XrCompositionLayerBaseHeader*)&panel[s];
    if (haveWrist) layers[nLayers++] = (XrCompositionLayerBaseHeader*)&wrist;
    XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
    fei.displayTime = g.frameState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = nLayers;
    fei.layers = layers;
    SaveLayers(layers, haveWrist ? nLayers - 1 : nLayers);   // the keep-alive's copy (the watch, last, left out)
    XrResult er;
    { Watchdog::Mark mk("xrEndFrame"); er = xrEndFrame(g.session, &fei); }
    if (XR_FAILED(er) && m_EndFrameFails++ < 10)   // the runtime refused the frame: a black headset, said nowhere else
        LOGI("XRSession: xrEndFrame FAILED {} ({} layers{})", (int)er, nLayers, m_EndFrameFails == 10 ? "; not logged again" : "");

    m_FrameStarted = false;
    m_FramesSubmitted++;
    FrameEnded();
}

//! The wrist HUD quad (XRWristLayer): drawn into its own swapchain and placed in the wrist's action space, so the
//! compositor puts it on the controller as it is at display time -- no frame of lag, whatever the game's frame rate.
bool XRSession::AppendWristLayer(XrCompositionLayerQuad& quad)
{
    const XRWristLayer l = m_Wrist;
    m_Wrist.alpha = 0;                      // one frame: WristHud sets it again every frame it wants it
    const XrSpace ws = l.space == 0 && g.wristSpace ? g.wristSpace : g.aimSpace[0];
    if (!l.srv || l.alpha <= 0.f || !ws || !g.context || !g.device || l.srcW == 0 || l.srcH == 0) return false;
    constexpr uint32_t kSize = 512;
    if (!g.wristSwapchain)
    {
        XrSwapchainCreateInfo scci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        scci.format = m_SwapFormat;
        scci.sampleCount = 1; scci.width = kSize; scci.height = kSize; scci.faceCount = 1; scci.arraySize = 1; scci.mipCount = 1;
        const XrResult r = xrCreateSwapchain(g.session, &scci, &g.wristSwapchain);
        if (XR_FAILED(r))
        {
            static int logged = 0;
            if (logged++ < 3) LOGI("XRSession: wrist HUD swapchain failed ({})", (int)r);
            g.wristSwapchain = XR_NULL_HANDLE;
            return false;
        }
        uint32_t n = 0;
        xrEnumerateSwapchainImages(g.wristSwapchain, 0, &n, nullptr);
        g.wristImages.assign(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        xrEnumerateSwapchainImages(g.wristSwapchain, n, &n, (XrSwapchainImageBaseHeader*)g.wristImages.data());
        LOGI("XRSession: wrist HUD swapchain {}x{} ({} images); grip pose {}", kSize, kSize, n, g.wristSpace ? "available" : "NOT available (aim only)");
    }
    uint32_t idx = 0;
    if (!AcquireWaitImage(g.wristSwapchain, idx)) return false;
    const bool drawn = idx < g.wristImages.size() && g.wristImages[idx].texture
                    && g_Wrist.Draw(g.device, g.context, l, g.wristImages[idx].texture, kSize, kSize, (DXGI_FORMAT)m_SwapFormat);
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(g.wristSwapchain, &ri);
    if (!drawn) return false;
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // premultiplied (the default)
    quad.space = ws;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g.wristSwapchain;
    quad.subImage.imageRect.offset = { 0, 0 };
    quad.subImage.imageRect.extent = { (int)kSize, (int)kSize };
    quad.pose.orientation = { l.quat[0], l.quat[1], l.quat[2], l.quat[3] };
    quad.pose.position = { l.pos[0], l.pos[1], l.pos[2] };
    quad.size = { l.size, l.size };
    m_WristLayers++;
    return true;
}

//! A panel in the room (XRPanelLayer): its crop drawn into the slot's own swapchain, a quad in LOCAL space.
bool XRSession::AppendPanelLayer(int slot, XrCompositionLayerQuad& quad)
{
    const XRPanelLayer l = m_Panel[slot];
    m_Panel[slot].alpha = 0;                // one frame: set again every frame it is wanted
    if ((!l.tex && !l.srv) || l.alpha <= 0.f || !g.context || !g.device || l.outW < 16 || l.outH < 16 || l.width <= 0.f || l.height <= 0.f)
        return false;
    if (g.panelSwapchain[slot] && (g.panelW[slot] != l.outW || g.panelH[slot] != l.outH))
    {
        DestroySwapchain(g.panelSwapchain[slot]); g.panelImages[slot].clear();
    }
    if (!g.panelSwapchain[slot])
    {
        XrSwapchainCreateInfo scci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        scci.format = m_SwapFormat;
        scci.sampleCount = 1; scci.width = l.outW; scci.height = l.outH; scci.faceCount = 1; scci.arraySize = 1; scci.mipCount = 1;
        const XrResult r = xrCreateSwapchain(g.session, &scci, &g.panelSwapchain[slot]);
        if (XR_FAILED(r))
        {
            static int logged = 0;
            if (logged++ < 5) LOGI("XRSession: panel {} swapchain {}x{} failed ({})", slot, l.outW, l.outH, (int)r);
            g.panelSwapchain[slot] = XR_NULL_HANDLE;
            return false;
        }
        uint32_t n = 0;
        xrEnumerateSwapchainImages(g.panelSwapchain[slot], 0, &n, nullptr);
        g.panelImages[slot].assign(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        xrEnumerateSwapchainImages(g.panelSwapchain[slot], n, &n, (XrSwapchainImageBaseHeader*)g.panelImages[slot].data());
        g.panelW[slot] = l.outW; g.panelH[slot] = l.outH;
        LOGI("XRSession: panel {} swapchain {}x{} ({} images)", slot, l.outW, l.outH, n);
    }
    uint32_t idx = 0;
    if (!AcquireWaitImage(g.panelSwapchain[slot], idx)) return false;
    const bool drawn = idx < g.panelImages[slot].size() && g.panelImages[slot][idx].texture
                    && g_PanelR.Draw(g.device, g.context, l, g.panelImages[slot][idx].texture, l.outW, l.outH, (DXGI_FORMAT)m_SwapFormat);
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(g.panelSwapchain[slot], &ri);
    if (!drawn) return false;
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // premultiplied (the default)
    quad.space = g.space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g.panelSwapchain[slot];
    quad.subImage.imageRect.offset = { 0, 0 };
    quad.subImage.imageRect.extent = { (int)l.outW, (int)l.outH };
    quad.pose.orientation = { l.quat[0], l.quat[1], l.quat[2], l.quat[3] };
    quad.pose.position = { l.pos[0], l.pos[1], l.pos[2] };
    quad.size = { l.width, l.height };
    m_PanelFrames[slot]++;
    return true;
}

//! Every ~0.2 s of play, the picture just drawn (still in the blitter's copy) goes into the ring at half size, with
//! the pose it was claimed at and its tangent extents. Not while one is frozen in the ring.
void XRSession::OfferSnapshot(ID3D11Texture2D* source, const float quat[4], ID3D11ShaderResourceView* left, const float leftQuat[4], const float leftTan[4])
{
    const long long now = Qpc();
    if (QpcSec(now - m_LastSnapQpc) < 0.2 || !g_Blit.ready || !g_Blit.srv || !g.device || !g.context) return;
    D3D11_TEXTURE2D_DESC sd = {}; source->GetDesc(&sd);
    const UINT w = std::max(64u, sd.Width / 2), h = std::max(64u, sd.Height / 2);
    const DXGI_FORMAT f = Blitter::ViewFormat(sd.Format, DXGI_FORMAT_UNKNOWN);
    if (!g_SnapTex[0][0] || g_SnapW != w || g_SnapH != h || g_SnapFmt != f)
    {
        ReleaseSnaps();
        for (Snap& s : m_Snap) s.valid = false;
        m_Frozen = -1;
        for (int i = 0; i < 6; i++)
        {
            D3D11_TEXTURE2D_DESC cd = {}; cd.Width = w; cd.Height = h; cd.MipLevels = 1; cd.ArraySize = 1; cd.Format = f;
            cd.SampleDesc = { 1, 0 }; cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(g.device->CreateTexture2D(&cd, nullptr, &g_SnapTex[i / 2][i % 2])) || FAILED(g.device->CreateShaderResourceView(g_SnapTex[i / 2][i % 2], nullptr, &g_SnapSrv[i / 2][i % 2])))
            {
                LOGI("XRSession: backdrop pictures {}x{} format {} could not be created", w, h, (int)f);
                ReleaseSnaps(); m_LastSnapQpc = now;   // tried again on a later offer
                return;
            }
        }
        g_SnapW = w; g_SnapH = h; g_SnapFmt = f;
    }
    int slot = m_SnapNext;
    if (slot == m_Frozen) slot = (slot + 1) % 3;
    // a pair: the left picture (its staged copy) into eye 0, the right one (still in the blitter's copy) into eye 1
    const bool two = left && leftQuat && leftTan;
    if (two && !g_Blit.DrawSrv(g.device, g.context, left, g_SnapTex[slot][0], 0.f, 0.f, (float)w, (float)h, f, 1.f, false)) return;
    if (!g_Blit.DrawSrv(g.device, g.context, g_Blit.srv, g_SnapTex[slot][two ? 1 : 0], 0.f, 0.f, (float)w, (float)h, f, 1.f, false)) return;
    Snap& s = m_Snap[slot];
    s.two = two;
    for (int i = 0; i < 4; i++)
    {
        s.quat[i] = two ? leftQuat[i] : quat[i]; s.tan[i] = two ? leftTan[i] : m_SrcTan[i];
        s.quatR[i] = quat[i]; s.tanR[i] = m_SrcTan[i];
    }
    s.qpc = now; s.valid = true;
    m_SnapNext = (slot + 1) % 3;
    m_LastSnapQpc = now;
    m_Snapshots++;
}

bool XRSession::FreezeBackdrop(float minAge, float maxAge)
{
    const long long now = Qpc();
    int best = -1; long long bestQpc = 0;
    for (int i = 0; i < 3; i++)
    {
        if (!m_Snap[i].valid || !g_SnapSrv[i][0]) continue;
        const double age = QpcSec(now - m_Snap[i].qpc);
        if (age < minAge || age > maxAge) continue;
        if (best < 0 || m_Snap[i].qpc > bestQpc) { best = i; bestQpc = m_Snap[i].qpc; }
    }
    m_Frozen = best; m_FrozenAt = now;
    return best >= 0;
}
void XRSession::UnfreezeBackdrop() { m_Frozen = -1; }
float XRSession::BackdropAge() const { return m_Frozen >= 0 ? (float)QpcSec(m_FrozenAt - m_Snap[m_Frozen].qpc) : -1.f; }

//! The menu screen's frame: each eye's projection view is the backdrop drawn for the pose that eye has right now (so
//! it holds still in the room with no reprojection), panel slot 0 is the screen over it.
void XRSession::SubmitScreen(const XRBackdrop& b)
{
    if (!m_FrameStarted)
        return;
    XrCompositionLayerProjectionView projViews[kMaxEyes] = {};
    const bool snap = m_Frozen >= 0 && b.snapshot > 0.f && g_SnapSrv[m_Frozen][0];
    for (int i = 0; i < m_ViewCount; i++)
    {
        if (!m_SwapW[i] || !m_SwapH[i]) EnsureEyeSwapchain(i, m_EyeWidth, m_EyeHeight);
        uint32_t imgIndex = 0;
        if (AcquireWaitImage(g.swapchains[i], imgIndex))
        {
            ID3D11Texture2D* dst = g.images[i][imgIndex].texture;
            if (dst && g.device && g.context && g_BackdropR.Init(g.device, kBackdropHlsl, "backdrop", 96))
            {
                const XrPosef& p = g.views[i].pose; const XrFovf& fv = g.views[i].fov;
                const Snap& sn = m_Snap[snap ? m_Frozen : 0];
                // each eye its own eye's picture when the pair kept both (its outer edge is in it), else the one picture
                const bool own = snap && sn.two && i == 1;
                const float* sq = own ? sn.quatR : sn.quat; const float* st = own ? sn.tanR : sn.tan;
                const float k[24] = {
                    p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w,
                    tanf(fv.angleLeft), tanf(fv.angleRight), tanf(fv.angleUp), tanf(fv.angleDown),
                    sq[0], sq[1], sq[2], sq[3],
                    st[0], st[1], st[2], st[3],
                    snap ? b.snapshot : 0.f, b.env, b.fade, b.desat,
                    b.glowDir[0], b.glowDir[1], b.glowDir[2], b.glow };
                if (g_BackdropR.Run(g.device, g.context, dst, m_SwapW[i], m_SwapH[i], (DXGI_FORMAT)m_SwapFormat, k, snap ? g_SnapSrv[m_Frozen][own ? 1 : 0] : nullptr, "backdrop"))
                {
                    m_LastEyeTex[i] = dst;
                    m_ContentX[i] = 0; m_ContentY[i] = 0; m_ContentW[i] = (int)m_SwapW[i]; m_ContentH[i] = (int)m_SwapH[i];
                }
            }
            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(g.swapchains[i], &ri);
        }
        // the poses the eyes have now: what the backdrop was drawn for
        projViews[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
        projViews[i].pose = g.views[i].pose;
        projViews[i].fov = g.views[i].fov;
        projViews[i].subImage.swapchain = g.swapchains[i];
        projViews[i].subImage.imageArrayIndex = 0;
        projViews[i].subImage.imageRect.offset = { 0, 0 };
        projViews[i].subImage.imageRect.extent = { (int)m_SwapW[i], (int)m_SwapH[i] };
        m_EyePoseValid[i] = false;          // that eye's last image is no longer a picture of the game
    }
    m_RenderPoseValid = false;
    m_FrameFade = 1.f; m_FrameDim = 1.f;
    m_Wrist.alpha = 0;                      // no watch over a menu
    m_Panel[1].alpha = 0;                   // nor the wheel's panel
    m_ScreenFrames++;
    XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    layer.space = g.space;
    layer.viewCount = m_ViewCount;
    layer.views = projViews;
    EndFrameWithLayers(layer);
}

void XRSession::SubmitFrameStereo(ID3D11Texture2D* eye0, ID3D11Texture2D* eye1)
{
    if (!m_FrameStarted)
        return;

    ID3D11Texture2D* src[2] = { eye0, eye1 };
    XrCompositionLayerProjectionView projViews[kMaxEyes] = {};

    for (int i = 0; i < m_ViewCount && i < 2; i++)
    {
        // Crop the eye's native fov out of the symmetric render (see
        // MatchedCropRect); falls back to the whole source if tans are unset.
        unsigned srcX = 0, srcY = 0, cropW = 0, cropH = 0;
        bool cropped = false;
        if (src[i])
        {
            D3D11_TEXTURE2D_DESC sd = {};
            src[i]->GetDesc(&sd);
            cropped = MatchedCropRect(i, sd.Width, sd.Height, srcX, srcY, cropW, cropH);
            EnsureEyeSwapchain(i, cropped ? cropW : sd.Width, cropped ? cropH : sd.Height);
        }

        uint32_t imgIndex = 0;
        if (AcquireWaitImage(g.swapchains[i], imgIndex))
        {

            ID3D11Texture2D* dst = g.images[i][imgIndex].texture;
            if (src[i] && dst && g.context && g.device)
            {
                D3D11_TEXTURE2D_DESC sd = {}, dd = {};
                src[i]->GetDesc(&sd);
                dst->GetDesc(&dd);
                UINT w = cropped ? cropW : sd.Width;
                UINT h = cropped ? cropH : sd.Height;
                if (w > dd.Width) w = dd.Width;
                if (h > dd.Height) h = dd.Height;

                ID3D11RenderTargetView* rtv = nullptr;
                if (SUCCEEDED(g.device->CreateRenderTargetView(dst, nullptr, &rtv)))
                {
                    const float black[4] = { 0, 0, 0, 1 };
                    g.context->ClearRenderTargetView(rtv, black);
                    rtv->Release();
                }
                D3D11_BOX box = { srcX, srcY, 0, srcX + w, srcY + h, 1 };
                g.context->CopySubresourceRegion(dst, 0, 0, 0, 0, src[i], 0, &box);
                m_LastEyeTex[i] = dst;
                m_ContentX[i] = 0; m_ContentY[i] = 0;
                m_ContentW[i] = (int)w; m_ContentH[i] = (int)h;
            }

            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(g.swapchains[i], &ri);
        }

        // Both frames were RENDERED under the eye's native pose + fov
        // (matched-fov), so the standard per-eye mapping applies directly.
        projViews[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
        projViews[i].pose = g.views[i].pose;
        projViews[i].fov = g.views[i].fov;
        projViews[i].subImage.swapchain = g.swapchains[i];
        projViews[i].subImage.imageArrayIndex = 0;
        projViews[i].subImage.imageRect.offset = { 0, 0 };
        projViews[i].subImage.imageRect.extent =
            { m_ContentW[i] > 0 ? m_ContentW[i] : (int)m_EyeWidth,
              m_ContentH[i] > 0 ? m_ContentH[i] : (int)m_EyeHeight };
    }

    XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    layer.space = g.space;
    layer.viewCount = m_ViewCount;
    layer.views = projViews;

    EndFrameWithLayers(layer);
}

void XRSession::EndFrameNoRender()
{
    if (!m_FrameStarted)
        return;
    XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
    fei.displayTime = g.frameState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = 0;
    fei.layers = nullptr;
    XrResult er;
    { Watchdog::Mark mk("xrEndFrame"); er = xrEndFrame(g.session, &fei); }
    if (XR_FAILED(er) && m_EndFrameFails++ < 10)
        LOGI("XRSession: xrEndFrame (no layers) FAILED {}{}", (int)er, m_EndFrameFails == 10 ? "; not logged again" : "");
    m_FrameStarted = false;
    FrameEnded();
}

// ---------------- the OpenXR frame thread ----------------

//! The render thread's frame is over (or never began): the frame thread may keep the headset fed and end the session.
void XRSession::FrameEnded()
{
    std::lock_guard<std::mutex> lk(pump.m);
    pump.mainOpen = false;
    pump.lastMainEnd = NowSec();
    pump.cv.notify_all();
}

//! A copy of the frame's layers (projection + quads) for the keep-alive. The swapchains they name keep their last
//! released image, which is what a layer shows; DestroySwapchain drops the copy.
void XRSession::SaveLayers(const void* layersIn, unsigned count)
{
    const XrCompositionLayerBaseHeader* const* layers = (const XrCompositionLayerBaseHeader* const*)layersIn;
    std::lock_guard<std::mutex> lk(pump.m);
    pump.haveLayers = false; pump.nQuads = 0;
    for (unsigned i = 0; i < count; i++)
    {
        const XrCompositionLayerBaseHeader* l = layers[i];
        if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
        {
            const auto* pl = (const XrCompositionLayerProjection*)l;
            if (pl->viewCount > (uint32_t)kMaxEyes) return;
            pump.proj = *pl;
            for (uint32_t v = 0; v < pl->viewCount; v++) pump.views[v] = pl->views[v];
            pump.proj.views = pump.views;
            pump.haveLayers = true;
        }
        else if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD && pump.nQuads < 4)
            pump.quads[pump.nQuads++] = *(const XrCompositionLayerQuad*)l;
    }
}

//! One frame for the headset with the last frame's layers, at the time this wait was for: the last picture, held where it
//! was in the room (the compositor reprojects it for the head as it is now). Called on the frame thread, unlocked, with
//! pump.kaOpen set. The device's lock (ID3D11Multithread) is held around the frame calls: a runtime may use the game's
//! immediate context in them (SteamVR does), and the render thread may be using it at that moment.
void XRSession::KeepAliveFrame(const void* fsIn)
{
    const XrFrameState& fs = *(const XrFrameState*)fsIn;
    XrCompositionLayerProjection proj; XrCompositionLayerProjectionView views[kMaxEyes];
    XrCompositionLayerQuad quads[4]; int nQuads = 0; bool have = false;
    {
        std::lock_guard<std::mutex> lk(pump.m);
        have = pump.haveLayers;
        if (have)
        {
            proj = pump.proj;
            for (uint32_t v = 0; v < proj.viewCount && v < (uint32_t)kMaxEyes; v++) views[v] = pump.views[v];
            proj.views = views;
            nQuads = pump.nQuads;
            for (int q = 0; q < nQuads; q++) quads[q] = pump.quads[q];
        }
        if (!pump.inRun)
        {
            pump.inRun = true; pump.runStart = NowSec(); pump.runFrames = 0; pump.keepAliveRuns++;
            if (pump.keepAliveRuns <= 30)
                LOGI("XRSession: keep-alive: no frame from the game for {:.2f} s: the headset gets the last picture, held in the room",
                     NowSec() - (pump.lastMainEnd > pump.lastMainBegin ? pump.lastMainEnd : pump.lastMainBegin));
        }
        pump.runFrames++;
    }
    const XrCompositionLayerBaseHeader* layers[5]; uint32_t n = 0;
    if (have && fs.shouldRender)
    {
        layers[n++] = (const XrCompositionLayerBaseHeader*)&proj;
        for (int q = 0; q < nQuads; q++) layers[n++] = (const XrCompositionLayerBaseHeader*)&quads[q];
    }
    if (pump.mt) pump.mt->Enter();
    XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
    XrResult r;
    { Watchdog::Mark mk("xrBeginFrame (keep-alive)"); r = xrBeginFrame(g.session, &fbi); }
    if (XR_SUCCEEDED(r))
    {
        XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = n;
        fei.layers = n ? layers : nullptr;
        { Watchdog::Mark mk("xrEndFrame (keep-alive)"); r = xrEndFrame(g.session, &fei); }
    }
    if (pump.mt) pump.mt->Leave();
    std::lock_guard<std::mutex> lk(pump.m);
    pump.lastKeepAlive = NowSec();
    if (fs.predictedDisplayPeriod > 1000000 && fs.predictedDisplayPeriod < 100000000) pump.period = (double)fs.predictedDisplayPeriod * 1e-9;
    if (XR_SUCCEEDED(r)) pump.keepAlives++;
    else if (pump.waitFails++ < 8) LOGI("XRSession: a keep-alive frame failed ({})", (int)r);
}

//! STOPPING: end the session -- once no frame is open (the render thread's or a keep-alive's), from the frame thread.
void XRSession::EndSessionWhenFree()
{
    {
        std::lock_guard<std::mutex> lk(pump.m);
        if (pump.mainOpen || pump.kaOpen) { pump.endPending = true; return; }
        pump.endPending = false; pump.ending = true;
        m_SessionRunning = false; pump.waited = false; pump.request = false;
        pump.cv.notify_all();
    }
    { Watchdog::Mark mk("xrEndSession"); xrEndSession(g.session); }
    std::lock_guard<std::mutex> lk(pump.m);
    pump.ending = false;
    pump.cv.notify_all();
}

unsigned long __stdcall XRSession::PumpThread(void* self) { ((XRSession*)self)->PumpLoop(); return 0; }

void XRSession::PumpLoop()
{
    Watchdog::RegisterFrameThread();
    constexpr double kQuiet = 0.20;   // s without a frame from the game before the keep-alive takes over
    for (;;)
    {
        { std::lock_guard<std::mutex> lk(pump.m); if (pump.quit) break; }
        PollEventsNow();
        std::unique_lock<std::mutex> lk(pump.m);
        if (pump.quit) break;
        if (pump.endPending && !pump.mainOpen && !pump.kaOpen) { lk.unlock(); EndSessionWhenFree(); continue; }
        const double now = NowSec();
        if (!m_SessionRunning)
        {
            pump.waited = false; pump.request = false; pump.cv.notify_all();
            // The runtime stopped the running session (no LOSS, no EXITING) and has not offered it back. Measured on the
            // user's Quest 3 over Air Link: such a session came back 23-169 s later, while a NEW session was READY at once.
            if (m_KickSec > 0.f && pump.idleSince > 0 && !m_Exited && !m_Restart.load() && now - pump.idleSince > m_KickSec && now >= pump.nextKick
                && !KickApplies())
            {   // another runtime: what it does with a stopped session was never measured (SteamVR and Virtual Desktop offer
                // it back on reconnect; a new instance while one is disconnected can fail), so it is left to the runtime
                pump.idleSince = 0;
                LOGI("XRSession: the runtime ('{}') stopped the session and has not offered it back yet: waiting for it "
                     "(a new session is only started on Meta's runtime; vr xrkick <s> all = on any)", m_RuntimeName);
            }
            if (m_KickSec > 0.f && pump.idleSince > 0 && !m_Exited && !m_Restart.load() && now - pump.idleSince > m_KickSec && now >= pump.nextKick)
            {
                LOGI("XRSession: the runtime stopped the session {:.1f} s ago and has not offered it back: starting a new session (vr xrkick 0 = wait for it)",
                     now - pump.idleSince);
                pump.idleSince = 0; pump.kicks++; pump.nextKick = now + 20.0;
                m_RestartFresh = true; m_Restart = true;
            }
            pump.cv.wait_for(lk, std::chrono::milliseconds(20));
            continue;
        }
        const bool keepAlive = m_KeepAliveMode != 0;
        const double lastMain = pump.lastMainEnd > pump.lastMainBegin ? pump.lastMainEnd : pump.lastMainBegin;
        const bool gameQuiet = keepAlive && !pump.mainOpen && lastMain > 0 && now - lastMain > kQuiet;
        if (pump.waited)
        {   // a frame the game asked for and has not taken (its BeginFrame gave up): to the headset after a while
            if (gameQuiet && now - pump.waitedAt > kQuiet)
            {
                XrFrameState st = pump.state; pump.waited = false; pump.kaOpen = true;
                lk.unlock(); KeepAliveFrame(&st); lk.lock();
                pump.kaOpen = false; pump.cv.notify_all();
                continue;
            }
            pump.cv.wait_for(lk, std::chrono::milliseconds(10));
            continue;
        }
        if (gameQuiet && !pump.request && now - pump.lastKeepAlive < pump.period * 0.9)
        {   // at most one keep-alive frame per display period, whatever the runtime's xrWaitFrame does
            pump.cv.wait_for(lk, std::chrono::microseconds((long long)((pump.period * 0.9 - (now - pump.lastKeepAlive)) * 1e6) + 1));
            continue;
        }
        if (pump.request || gameQuiet)
        {
            const bool forGame = pump.request;
            pump.request = false; pump.waiting = true;
            lk.unlock();
            XrFrameState st = { XR_TYPE_FRAME_STATE };
            XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
            const double t0 = NowSec();
            XrResult r;
            { Watchdog::Mark mk("xrWaitFrame"); r = xrWaitFrame(g.session, &wi, &st); }
            const double t1 = NowSec(), took = t1 - t0;
            lk.lock();
            pump.waiting = false; pump.waits++;
            if (took > pump.longestWait) pump.longestWait = took;
            if (took > 1.0) LOGI("XRSession: xrWaitFrame took {:.2f} s (the runtime held the frame back; the game went on)", took);
            if (XR_FAILED(r))
            {
                if (pump.waitFails++ < 8) LOGI("XRSession: xrWaitFrame failed ({})", (int)r);
                pump.cv.wait_for(lk, std::chrono::milliseconds(5));
                continue;
            }
            if (pump.quit || !m_SessionRunning || pump.ending) continue;
            const double lastMain2 = pump.lastMainEnd > pump.lastMainBegin ? pump.lastMainEnd : pump.lastMainBegin;
            const bool stillQuiet = keepAlive && !pump.mainOpen && lastMain2 > 0 && t1 - lastMain2 > kQuiet;
            if (forGame || pump.request || !stillQuiet)
            {
                pump.state = st; pump.waited = true; pump.waitedAt = t1; pump.request = false;
                pump.cv.notify_all();
            }
            else
            {
                pump.kaOpen = true;
                lk.unlock(); KeepAliveFrame(&st); lk.lock();
                pump.kaOpen = false; pump.cv.notify_all();
            }
            continue;
        }
        pump.cv.wait_for(lk, std::chrono::milliseconds(10));
    }
}

bool XRSession::StartPump()
{
    {
        std::lock_guard<std::mutex> lk(pump.m);
        pump.quit = false; pump.request = pump.waited = pump.waiting = pump.mainOpen = pump.kaOpen = false;
        pump.ending = pump.endPending = pump.lastTimedOut = pump.haveLayers = pump.inRun = pump.wasRunning = false;
        pump.lastMainBegin = pump.lastMainEnd = pump.idleSince = 0;
    }
    DWORD tid = 0;
    pump.thread = CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)&XRSession::PumpThread, this, 0, &tid);
    if (!pump.thread) { LOGI("XRSession: the OpenXR frame thread could not start ({}): xrWaitFrame stays on the render thread", GetLastError()); return false; }
    SetThreadPriority(pump.thread, THREAD_PRIORITY_ABOVE_NORMAL);   // it paces the frames, as the render thread did
    LOGI("XRSession: OpenXR frame thread {} started (xrWaitFrame, events, keep-alive {}; the render thread waits at most {} ms)",
         tid, m_KeepAliveMode, m_WaitBoundMs);
    return true;
}

//! Asks the frame thread to stop and waits up to waitMs. False if it is still inside a runtime call.
bool XRSession::KickApplies() const
{
    if (m_KickAny) return true;
    std::string n = m_RuntimeName;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)tolower(c); });
    return n.find("oculus") != std::string::npos || n.find("meta") != std::string::npos || n.find("mock") != std::string::npos;
}

bool XRSession::StopPump(unsigned waitMs)
{
    if (!pump.thread) return true;
    { std::lock_guard<std::mutex> lk(pump.m); pump.quit = true; pump.cv.notify_all(); }
    if (WaitForSingleObject(pump.thread, waitMs) != WAIT_OBJECT_0) return false;
    CloseHandle(pump.thread);
    pump.thread = nullptr;
    return true;
}

void XRSession::SetD3DProtect(bool on)
{
    if (!pump.mt) { LOGI("cmd vr d3dmt: no ID3D11Multithread (no session yet)"); return; }
    pump.mt->SetMultithreadProtected(on ? TRUE : FALSE);
    LOGI("cmd vr d3dmt {}: D3D11 multithread protection {}", on, pump.mt->GetMultithreadProtected() ? "on" : "off");
}

std::string XRSession::PumpStatus() const
{
    std::lock_guard<std::mutex> lk(pump.m);
    const double now = NowSec();
    return fmt::format("xrpump on {} waits {} gameframes {} keepalive {} frames {} runs {} longestrun {:.2f} timeouts {} runs {} longest {:.2f} "
                       "longestwait {:.2f} imagetimeouts {} kicks {} waitfails {} bound {} kickafter {:.1f} waiting {} gameopen {} quiet {:.2f} mtlock {}",
                       pump.thread != nullptr, pump.waits, pump.mainFrames, m_KeepAliveMode, pump.keepAlives, pump.keepAliveRuns, pump.longestRun,
                       pump.mainTimeouts, pump.timeoutRuns, pump.longestTimeout, pump.longestWait, pump.imageTimeouts, pump.kicks, pump.waitFails,
                       m_WaitBoundMs, m_KickSec, pump.waiting, pump.mainOpen,
                       pump.lastMainEnd > 0 ? now - (pump.lastMainEnd > pump.lastMainBegin ? pump.lastMainEnd : pump.lastMainBegin) : -1.0,
                       pump.mt != nullptr);
}

unsigned long long XRSession::OrthoDraws() const { return g_Blit.draws; }

bool XRSession::Shutdown(bool processExit)
{
    // The frame thread first: nothing may be torn down under a call it is in. A runtime that does not return keeps the
    // session alive until it does (VRRuntime calls this again every Present).
    // Briefly: VRRuntime calls this every Present until it succeeds, and a 2 s wait each time held the game at 0.5 fps
    // for as long as a hung runtime kept the frame thread inside xrWaitFrame.
    if (!StopPump(processExit ? 0 : 50) && !processExit)
    {
        static double lastLog = -100;
        if (NowSec() - lastLog > 10) { lastLog = NowSec(); LOGI("XRSession: the OpenXR frame thread is still inside a runtime call: the teardown waits for it"); }
        return false;
    }
    g_Blit.Release();
    if (m_PairSrv) { m_PairSrv->Release(); m_PairSrv = nullptr; }
    if (m_PairTex) { m_PairTex->Release(); m_PairTex = nullptr; }
    m_PairStaged = false; m_PairW = m_PairH = 0;
    g_Wrist.Release();
    g_PanelR.Release();
    g_BackdropR.Release();
    ReleaseSnaps();
    for (Snap& s : m_Snap) s.valid = false;
    m_Frozen = -1;
    for (int s = 0; s < 2; s++)
    {
        DestroySwapchain(g.panelSwapchain[s]);
        g.panelImages[s].clear(); g.panelW[s] = g.panelH[s] = 0; m_Panel[s].alpha = 0;
    }
    DestroySwapchain(g.wristSwapchain); g.wristImages.clear();
    if (g.wristSpace) { xrDestroySpace(g.wristSpace); g.wristSpace = XR_NULL_HANDLE; }
    g.wristAction = XR_NULL_HANDLE; m_WristValid[0] = m_WristValid[1] = false;
    for (int i = 0; i < 2; i++)
        if (g.aimSpace[i]) { xrDestroySpace(g.aimSpace[i]); g.aimSpace[i] = XR_NULL_HANDLE; }
    if (g.actionSet) { xrDestroyActionSet(g.actionSet); g.actionSet = XR_NULL_HANDLE; }
    for (int i = 0; i < kMaxEyes; i++)
        DestroySwapchain(g.swapchains[i]);
    DestroySwapchain(g.quadSwapchain); g.quadImages.clear();
    if (g.viewSpace) { xrDestroySpace(g.viewSpace); g.viewSpace = XR_NULL_HANDLE; }
    if (g.space) { xrDestroySpace(g.space); g.space = XR_NULL_HANDLE; }
    if (g.session) { xrDestroySession(g.session); g.session = XR_NULL_HANDLE; }
    if (g.context) { g.context->Release(); g.context = nullptr; }
    if (g.instance) { xrDestroyInstance(g.instance); g.instance = XR_NULL_HANDLE; }
    g.systemId = XR_NULL_SYSTEM_ID;
    g.device = nullptr;
    for (int i = 0; i < kMaxEyes; i++) { g.images[i].clear(); m_LastEyeTex[i] = nullptr; m_SwapW[i] = m_SwapH[i] = 0; m_EyePoseValid[i] = false; }
    g.turnAction = g.turnYAction = g.moveAction = XR_NULL_HANDLE;
    for (int i = 0; i < 2; i++) { g.triggerAction[i] = g.gripAction[i] = g.aimAction[i] = g.hapticAction[i] = XR_NULL_HANDLE; }
    for (auto& a : g.btnAction) a = XR_NULL_HANDLE;
    m_Initialized = false;
    m_SessionRunning = false;
    m_FrameStarted = false;
    m_InputReady = m_HapticReady = false;
    m_SessionState = 0;
    // A shutdown after a LOSS starts over: Init is to be called again (VRRuntime does, every few seconds), with a
    // fresh allowance of instance retries (the runtime may be restarting too).
    if (m_Restart) { m_Retry = true; m_InitAttempts = 1; }
    m_Restart = false; m_RestartFresh = false;
    m_Profiles.clear();
    if (pump.mt) { pump.mt->Release(); pump.mt = nullptr; }
    for (ImageWait& w : g_ImageWait) w = {};
    Watchdog::SetXrState(0);
    return true;
}

//! Diagnostics ([Advanced] LogControllerInput): every press and release of a button, trigger or grip, so a tester's
//! log shows exactly what arrived from their controllers -- the answer to "button X does nothing". Capped per session.
void XRSession::LogInputChanges()
{
    static unsigned prevButtons = 0; static bool prevT[2] = {}, prevG[2] = {};
    static int logged = 0; constexpr int kMax = 3000;
    if (logged >= kMax) return;
    static const char* kNames[] = { "A", "B", "X", "Y", "left stick click", "right stick click", "menu" };
    const unsigned b = m_Input.buttons;
    for (int i = 0; i < 7; i++)
        if (((b ^ prevButtons) >> i) & 1) { LOGI("input: {} {}", kNames[i], (b >> i) & 1 ? "pressed" : "released"); logged++; }
    prevButtons = b;
    const float t[2] = { m_Input.triggerL, m_Input.triggerR }, gr[2] = { m_Input.gripL, m_Input.gripR };
    for (int h = 0; h < 2; h++)
    {
        // hysteresis: pressed past 0.6, released under 0.3
        const bool tn = prevT[h] ? t[h] > 0.3f : t[h] > 0.6f, gn = prevG[h] ? gr[h] > 0.3f : gr[h] > 0.6f;
        if (tn != prevT[h]) { LOGI("input: {} trigger {}", h ? "right" : "left", tn ? "pressed" : "released"); logged++; prevT[h] = tn; }
        if (gn != prevG[h]) { LOGI("input: {} grip {}", h ? "right" : "left", gn ? "pressed" : "released"); logged++; prevG[h] = gn; }
    }
    if (logged >= kMax) LOGI("input: {} changes logged: no more this session", kMax);
}
