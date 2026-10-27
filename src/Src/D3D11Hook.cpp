#include "pch.h"
#include "D3D11Hook.h"
#include "RenderRes.h"
#include <detours/detours.h>

namespace
{
    using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Resize_t  = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    Present_t g_RealPresent = nullptr;
    Resize_t  g_RealResize = nullptr;
    D3D11Hook::Callbacks g_Cb;
    std::atomic<bool> g_InPresent{ false };
    // The game swapchain gets 3 back buffers, not the game's 2 (at its ResizeBuffers; RenderRes resizes it to the headset
    // size when the session starts). The VR compositor holds the game's GPU work after each submitted frame until it
    // takes that frame; with 2 buffers the next Present then had no buffer and blocked the render thread that long -- in
    // synchronised stereo the Present of each pair's left image (~10 ms a pair, ending just before the pair's display:
    // measured on SteamVR), so the next pair was always submitted too late for the next display period and the headset
    // got half its refresh. The device's maximum frame latency is raised to match.
    constexpr UINT kGameBuffers = 3;

    HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
    {
        if (!(flags & DXGI_PRESENT_TEST) && g_Cb.onPresent)
        {
            // Guard against re-entrancy (nested presents from our own work).
            if (!g_InPresent.exchange(true)) { g_Cb.onPresent(sc); g_InPresent = false; }
        }
        return g_RealPresent(sc, sync, flags);
    }
    HRESULT STDMETHODCALLTYPE HkResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags)
    {
        LOGI("ResizeBuffers {}x{} fmt={} count={}", w, h, (int)f, n);
        if (g_Cb.onResize) g_Cb.onResize(sc);
        RenderRes::OnResizeBuffers(sc, w, h);
        DXGI_SWAP_CHAIN_DESC d{}; sc->GetDesc(&d);
        const UINT had = n ? n : d.BufferCount;
        const UINT count = had < kGameBuffers ? kGameBuffers : n;
        HRESULT hr = g_RealResize(sc, count, w, h, f, flags);
        if (FAILED(hr) && count != n)
        {
            LOGI("D3D11Hook: ResizeBuffers with {} buffers failed (0x{:08X}): the game's {} kept", count, (unsigned)hr, had);
            return g_RealResize(sc, n, w, h, f, flags);
        }
        if (SUCCEEDED(hr) && count != n)
        {
            ID3D11Device* dev = nullptr; IDXGIDevice1* dx = nullptr; UINT lat = 0;
            if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) && dev && SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dx)) && dx)
            {
                dx->GetMaximumFrameLatency(&lat);
                if (lat < count && SUCCEEDED(dx->SetMaximumFrameLatency(count))) lat = count;
            }
            if (dx) dx->Release();
            if (dev) dev->Release();
            LOGI("D3D11Hook: game swapchain {} buffers (the game asked {}), max frame latency {}", count, had, lat);
        }
        return hr;
    }

    bool GetVTable(void** outPresent, void** outResize)
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"DVR_Dummy";
        RegisterClassExW(&wc);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) return false;

        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 1; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; IDXGISwapChain* sc = nullptr;
        D3D_FEATURE_LEVEL fl;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                   D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
        bool ok = SUCCEEDED(hr);
        if (ok)
        {
            void** vt = *(void***)sc;
            *outPresent = vt[8];   // IDXGISwapChain::Present
            *outResize = vt[13];   // IDXGISwapChain::ResizeBuffers
            sc->Release(); ctx->Release(); dev->Release();
        }
        else LOGI("dummy D3D11CreateDeviceAndSwapChain failed hr=0x{:08X}", (unsigned)hr);
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return ok;
    }
}

bool D3D11Hook::Install(const Callbacks& cb)
{
    g_Cb = cb;
    void* present = nullptr; void* resize = nullptr;
    if (!GetVTable(&present, &resize)) return false;
    g_RealPresent = (Present_t)present;
    g_RealResize = (Resize_t)resize;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_RealPresent, HkPresent);
    DetourAttach(&(PVOID&)g_RealResize, HkResize);
    LONG r = DetourTransactionCommit();
    LOGI("D3D11Hook: Present@{} ResizeBuffers@{} detour={}", present, resize, r);
    return r == NO_ERROR;
}

void D3D11Hook::Uninstall()
{
    if (!g_RealPresent) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)g_RealPresent, HkPresent);
    DetourDetach(&(PVOID&)g_RealResize, HkResize);
    DetourTransactionCommit();
}
