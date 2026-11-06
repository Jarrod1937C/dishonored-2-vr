#include "pch.h"
#include "RenderRes.h"
#include "CrashLog.h"
#include "GameInfo.h"
#include "CVars.h"
#include "CameraHook.h"
#include "VRRuntime.h"
#include "Settings.h"
#include <detours/detours.h>
#include <cmath>
#include <algorithm>

namespace
{
    // Measured on 1.77.9.0 (docs/ENGINE.md, "Render resolution").
    constexpr uintptr_t kRetWndProc = 0xE9FF1;      // WndProc, WM_WINDOWPOSCHANGED: GetClientRect -> r_windowWidth/Height
    constexpr uintptr_t kRetHdcSize = 0x346364;     // GetHdcClientWindowSize: GetClientRect -> render targets, swapchain
    constexpr uintptr_t kRetCursor = 0xE27DF;       // the cursor for the interface: GetCursorPos + ScreenToClient
    constexpr uintptr_t kWriteConfig = 0x41770;     // WriteConfigToFile(common, name, int, mask, bool): every cfg write
    constexpr uintptr_t kPlaceWindow = 0x347630;    // the window's placement from r_windowWidth/Height (SetWindowPos)
    constexpr uintptr_t kDisplaySize = 0x32FAF80;   // int width, height: the engine's window size (render size follows)

    using GetClientRect_t = BOOL(WINAPI*)(HWND, LPRECT);
    using ScreenToClient_t = BOOL(WINAPI*)(HWND, LPPOINT);
    // FIVE arguments: the bool is on the stack (callers write [rsp+0x20], the function reads [rsp+0x60]). A hook with
    // four handed it garbage and the game wrote a config without its cvars.
    using WriteConfig_t = void(*)(void*, const char*, int, int, bool);
    using PlaceWindow_t = void(*)(void*);
    GetClientRect_t g_RealGCR = nullptr;
    ScreenToClient_t g_RealS2C = nullptr;
    WriteConfig_t g_RealWrite = nullptr;
    PlaceWindow_t g_RealPlace = nullptr;
    uintptr_t g_Base = 0;
    bool g_Installed = false;

    HWND g_Hwnd = nullptr;
    // Windowed, forced (2026-11-05, asked): fullscreen and borderless render at the MONITOR's size (ComputeTarget), so
    // while the headset runs the game is switched to windowed the way its own Alt+Enter does in its window procedure
    // (exe+0xEA5E6): idCVar::SetInt(r_fullscreen exe+0x32FB1D0, 0, force) -- the engine's own set: the string, the
    // modified flag -- then the render system (*exe+0x22FCD70, idRenderSystemLocal) vtable +0x200 applies the window
    // mode. Done on the window's thread, from the mod's message in its window procedure (Overlay), never in a Present.
    constexpr uintptr_t kFullscreenCvar = 0x32FB1D0, kCvarSetInt = 0x1028E0, kRenderSystemPtr = 0x22FCD70;
    constexpr size_t kApplyWindowSlot = 0x200 / 8;
    constexpr double kWindowedRetry = 3.0;
    std::atomic<int> g_WindowedAsks{ 0 }, g_WindowedDone{ 0 };
    double g_WindowedAt = -1e9; int g_WindowedFails = 0;
    std::atomic<int> g_TW{ 0 }, g_TH{ 0 };          // the target in effect (0 = the window's own size)
    int g_WantW = 0, g_WantH = 0, g_WantFrames = 0; // a new target has to hold this many frames before it is applied
    std::atomic<int> g_ManualW{ 0 }, g_ManualH{ 0 }; // vr rendersize <w> <h>: fixed target (research)
    std::atomic<int> g_Mode{ 1 };                   // 1 = auto, 0 = off (the window's size, the old behaviour)
    std::atomic<float> g_Scale{ 1.f };
    std::atomic<int> g_RtMaxW{ 0 }, g_RtMaxH{ 0 };   // the runtime's largest swapchain image (0 = not known yet)
    std::atomic<bool> g_Reapply{ false };
    std::string g_Why = "starting";
    unsigned long long g_Applies = 0, g_Spoofed = 0, g_Buffers = 0, g_CfgGuards = 0, g_PlaceGuards = 0;
    int g_BufW = 0, g_BufH = 0;                     // the swapchain's buffers, last seen
    int g_RsSaved = -1;                             // rs_enable as the game had it before the mod held it off
    int g_TbSaved = -1;                             // r_swapChainTripleBufferEnable, likewise
    int g_HbaoSaved = -1;                           // r_hbao (the game's HBAO+ option), likewise
    // Present rate, for measuring what the resolution costs
    LARGE_INTEGER g_FpsT{}; int g_FpsN = 0; float g_Fps = 0.f;
    // GPU time per frame: a timestamp at every Present (inside a disjoint query per frame), read back 8 frames later.
    // The difference of two consecutive ones is the GPU's frame time when it is the bottleneck (it never idles); with the
    // CPU or the headset pacing, it is the frame period. Averaged over the last 2 s, like the fps.
    struct GpuStamp { ID3D11Query* disj = nullptr; ID3D11Query* ts = nullptr; bool issued = false; unsigned long long frame = 0; };
    constexpr int kStamps = 8;
    GpuStamp g_Stamp[kStamps]; int g_StampHead = 0; bool g_StampBad = false; unsigned long long g_StampFrame = 0;
    unsigned long long g_LastTsFrame = 0; UINT64 g_LastTs = 0;
    double g_GpuRing[256] = {}; LARGE_INTEGER g_GpuRingT[256] = {}; int g_GpuHead = 0, g_GpuCount = 0; float g_GpuMs = 0.f;
    void GpuTime(IDXGISwapChain* sc)
    {
        if (g_StampBad) return;
        ID3D11Device* dev = nullptr;
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return;
        ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
        GpuStamp& s = g_Stamp[g_StampHead];
        if (!s.ts)
        {
            D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, qt = { D3D11_QUERY_TIMESTAMP, 0 };
            if (FAILED(dev->CreateQuery(&qd, &s.disj)) || FAILED(dev->CreateQuery(&qt, &s.ts))) { g_StampBad = true; LOGI("RenderRes: GPU timestamps not available"); }
        }
        if (!g_StampBad && s.issued)
        {   // this slot's frame, 8 Presents ago
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {}; UINT64 t = 0;
            if (ctx->GetData(s.disj, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && ctx->GetData(s.ts, &t, sizeof t, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
            {
                if (!dj.Disjoint && dj.Frequency && g_LastTs && g_LastTsFrame + 1 == s.frame && t > g_LastTs)
                {
                    LARGE_INTEGER now; QueryPerformanceCounter(&now);
                    g_GpuRing[g_GpuHead] = (double)(t - g_LastTs) * 1000.0 / (double)dj.Frequency; g_GpuRingT[g_GpuHead] = now;
                    g_GpuHead = (g_GpuHead + 1) % 256; if (g_GpuCount < 256) g_GpuCount++;
                    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
                    double sum = 0; int n = 0;
                    for (int i = 0; i < g_GpuCount; i++)
                    {
                        const int k = (g_GpuHead - 1 - i + 256) % 256;
                        if ((double)(now.QuadPart - g_GpuRingT[k].QuadPart) / f.QuadPart > 2.0) break;
                        sum += g_GpuRing[k]; n++;
                    }
                    if (n) g_GpuMs = (float)(sum / n);
                }
                g_LastTs = dj.Disjoint ? 0 : t; g_LastTsFrame = s.frame;
            }
            s.issued = false;
        }
        if (!g_StampBad && !s.issued)
        {
            ctx->Begin(s.disj); ctx->End(s.ts); ctx->End(s.disj);
            s.issued = true; s.frame = ++g_StampFrame;
            g_StampHead = (g_StampHead + 1) % kStamps;
        }
        ctx->Release(); dev->Release();
    }

    BOOL WINAPI HkGetClientRect(HWND h, LPRECT r)
    {
        const BOOL ok = g_RealGCR(h, r);
        const int tw = g_TW.load(), th = g_TH.load();
        if (ok && r && tw > 0 && th > 0 && h == g_Hwnd)
        {
            const uintptr_t ra = (uintptr_t)_ReturnAddress() - g_Base;
            if (ra == kRetWndProc || ra == kRetHdcSize) { r->left = r->top = 0; r->right = tw; r->bottom = th; g_Spoofed++; }
        }
        return ok;
    }

    //! The interface lays itself out in the render size, so the cursor it reads is mapped from the window into it (the
    //! way the window shows the frame, scaled). Confining and centring the cursor keep the real window.
    BOOL WINAPI HkScreenToClient(HWND h, LPPOINT p)
    {
        const BOOL ok = g_RealS2C(h, p);
        const int tw = g_TW.load(), th = g_TH.load();
        int rw = 0, rh = 0;
        if (ok && p && tw > 0 && th > 0 && h == g_Hwnd && (uintptr_t)_ReturnAddress() - g_Base == kRetCursor && RenderRes::RealClient(rw, rh))
        {
            p->x = (LONG)lround((double)p->x * tw / rw);
            p->y = (LONG)lround((double)p->y * th / rh);
        }
        return ok;
    }

    //! A window-size cvar's live value (int, float and string) replaced without the engine's modified flags, so nothing
    //! reacts to it: only what reads it in between sees it. The string is the inline buffer of the idStr (CVars.cpp).
    struct CvarInt
    {
        uintptr_t a = 0; int i = 0; float f = 0; char s[16] = {}; int len = 0; bool ok = false;
        static bool Writable(uintptr_t a) { return a && *(uintptr_t*)a == a + 0x10 && *(uint32_t*)(a + 0x0C) >= 16; }
        void Save(const char* name)
        {
            a = CVars::Address(name); ok = Writable(a);
            if (!ok) return;
            i = *(int*)(a + 0x28); f = *(float*)(a + 0x2C); len = *(int*)(a + 0x08); memcpy(s, (void*)(a + 0x10), 16);
        }
        void Put(int v) const
        {
            if (!ok) return;
            *(int*)(a + 0x28) = v; *(float*)(a + 0x2C) = (float)v;
            char b[16]; const int n = snprintf(b, sizeof b, "%d", v);
            memcpy((void*)(a + 0x10), b, (size_t)n + 1); *(int*)(a + 0x08) = n;
        }
        void Restore() const
        {
            if (!ok) return;
            *(int*)(a + 0x28) = i; *(float*)(a + 0x2C) = f; memcpy((void*)(a + 0x10), s, 16); *(int*)(a + 0x08) = len;
        }
    };

    //! While the target is in effect, the window-size cvars hold it (the engine put it there). For the length of `fn`
    //! they hold the real window instead -- unless they hold something else, i.e. the player just picked a window size
    //! in the options, which is theirs to keep.
    template <class F> bool WithRealWindow(F&& fn)
    {
        const int tw = g_TW.load(), th = g_TH.load();
        int rw = 0, rh = 0;
        CvarInt cw, ch; cw.Save("r_windowWidth"); ch.Save("r_windowHeight");
        const bool swap = tw > 0 && cw.ok && ch.ok && cw.i == tw && ch.i == th && RenderRes::RealClient(rw, rh);
        int* ds = (int*)(g_Base + kDisplaySize);
        const int dw = ds[0], dh = ds[1];
        if (swap) { cw.Put(rw); ch.Put(rh); ds[0] = rw; ds[1] = rh; }
        fn();
        if (swap) { cw.Restore(); ch.Restore(); ds[0] = dw; ds[1] = dh; }
        return swap;
    }

    void HkWriteConfig(void* common, const char* name, int a, int mask, bool b)
    {
        if (WithRealWindow([&] { g_RealWrite(common, name, a, mask, b); }))
        {
            g_CfgGuards++;
            LOGI("RenderRes: {} written with the real window size", name ? name : "config");
        }
    }

    void HkPlaceWindow(void* display)
    {
        if (WithRealWindow([&] { g_RealPlace(display); }))
        {
            g_PlaceGuards++;
            LOGI("RenderRes: window placed at the real window size (the engine renders at {}x{})", g_TW.load(), g_TH.load());
        }
    }

    //! The size the flat frame needs: each eye's pixel density (the runtime's recommended size over its fov, linear in
    //! tan like the eye buffer) times the tan extent the frame covers (XRSession draws it there at its true angles).
    bool ComputeTarget(int& w, int& h, std::string& why)
    {
        if (g_Mode.load() == 0) { why = "off"; return false; }
        const int mw = g_ManualW.load(), mh = g_ManualH.load();
        if (mw > 0 && mh > 0) { w = mw; h = mh; why = "manual"; return true; }
        const VRRuntime::Status st = VRRuntime::GetStatus();
        if (!st.running || !st.eyeW || !st.eyeH) { why = "no headset session"; return false; }
        // Windowed only. Borderless (2) is a window too, but the engine takes the MONITOR's size for its render there
        // whatever GetClientRect says: measured 2026-10-16, buffers 2266x2348 with the engine at 1920x1080 = a torn picture.
        if (CVars::GetInt("r_fullscreen", 0) != 0) { why = "game in fullscreen or borderless mode (windowed needed)"; return false; }
        float dx = 0.f, dy = 0.f;
        for (int e = 0; e < 2; e++)
        {
            const VRRuntime::Placement pl = VRRuntime::GetPlacement(e);
            if (!pl.valid) continue;
            const float tw = tanf(pl.fov[1]) - tanf(pl.fov[0]), th = tanf(pl.fov[2]) - tanf(pl.fov[3]);
            if (tw > 0.1f) dx = std::max(dx, st.eyeW / tw);
            if (th > 0.1f) dy = std::max(dy, st.eyeH / th);
        }
        if (dx <= 0.f || dy <= 0.f) { why = "eye fov not known yet"; return false; }
        const float s = std::clamp(g_Scale.load(), 0.25f, 2.f);
        float ox = 0.f, oy = 0.f; CameraHook::FovOverride(ox, oy);
        float fw, fh, ex = 0.f, ey = 0.f;
        if (ox > 1.f && oy > 1.f && CameraHook::RenderExtent(ex, ey))
        {   // the mod's render fov (vr fov auto: both eyes' fov): any aspect, so both axes at the eye's density. In stereo
            // with the per-eye projection the frustum is one eye's own (CameraHook::RenderExtent): fewer pixels at the
            // same density, not a sharper picture
            fw = dx * ex * s;
            fh = dy * ey * s;
            why = CameraHook::EyeProj() && VRRuntime::Stereo() && fabsf(ex - 2.f * tanf(ox * 0.5f / 57.29578f)) > 1e-3f ? "headset, per eye" : "headset";
        }
        else
        {   // the game's own fov follows the window's aspect: keep the aspect, the horizontal at the eye's density. Only
            // ever larger than the window: a menu's camera can be narrow (38 deg at the title) and its 2D interface is
            // laid out in pixels, so a smaller frame would only blur the text.
            float gx = 0.f, gy = 0.f; unsigned long long n = 0; CameraHook::BuiltFov(gx, gy, n);
            int rw = 0, rh = 0;
            if (gx < 1.f || !RenderRes::RealClient(rw, rh)) { why = "render fov not known yet"; return false; }
            fw = dx * 2.f * tanf(gx * 0.5f / 57.29578f) * s;
            if (fw <= (float)rw) { why = "the window is enough"; return false; }
            fh = fw * (float)rh / (float)rw;
            why = "headset, game fov";
        }
        // Limits, the aspect kept. The eye swapchain holds this frame, so never more than the runtime's largest swapchain
        // image (SteamVR 8192; 4096 until the runtime has said) nor D3D11's 16384. Until 2026-11-01 also at most 4096 a
        // side and 16 MP, for video memory (the game's whole set of render targets follows this size): that capped a
        // Bigscreen Beyond 2 asking 4875x5036 to 3936x4066, so a higher resolution in SteamVR changed nothing. Now only
        // the optional budget [Display] MaxRenderMegapixels (0 = none), for a card that runs out of video memory.
        const int rtw = g_RtMaxW.load(), rth = g_RtMaxH.load();
        const float maxW = (float)std::min(rtw > 0 ? rtw : 4096, 16384), maxH = (float)std::min(rth > 0 ? rth : 4096, 16384);
        const float mp = Settings::MaxRenderMegapixels();
        float k = std::min({ 1.f, maxW / fw, maxH / fh });
        if (mp > 0.f) k = std::min(k, sqrtf(mp * 1.0e6f / (fw * fh)));
        if (k < 1.f)
        {
            static int lastW = 0;
            if (lastW != (int)fw)
            {
                lastW = (int)fw;
                LOGI("RenderRes: the headset asks for {}x{}: capped to {}x{} (at most {}x{}{})", (int)fw, (int)fh, (int)(fw * k), (int)(fh * k),
                     (int)maxW, (int)maxH, mp > 0.f ? fmt::format(", {:.1f} MP: MaxRenderMegapixels", mp) : std::string());
            }
            fw *= k; fh *= k;
        }
        w = std::clamp((int)lroundf(fw), 320, (int)maxW);
        h = std::clamp((int)lroundf(fh), 240, (int)maxH);
        return true;
    }

    //! The engine re-reads its window size: SWP_FRAMECHANGED sends WM_WINDOWPOSCHANGED without moving the window.
    void Trigger()
    {
        SetWindowPos(g_Hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    //! A game option held at 0 while the headset shows the game, and the player's value put back after. Only the live int
    //! is written (no modified flag, the string untouched): nothing of it reaches the config.
    void HoldOff(const char* name, int& saved, bool hold, const char* what)
    {
        const uintptr_t a = CVars::Address(name);
        if (!a) return;
        const int v = *(int*)(a + 0x28);
        if (hold)
        {
            if (v != 0) { if (saved < 0) saved = v; CVars::SetInt(name, 0); LOGI("RenderRes: the game's {} held off ({} was {})", what, name, v); }
        }
        else if (saved >= 0)
        {
            if (v == 0) CVars::SetInt(name, saved);
            LOGI("RenderRes: the game's {} back ({} {})", what, name, CVars::GetInt(name, -1));
            saved = -1;
        }
    }
    //! The game's dynamic resolution aims at the MONITOR's refresh rate and dropped to 85% under the headset's pacing.
    //! Its triple buffering (r_swapChainTripleBufferEnable, read where the swapchain is made and resized: exe+0x368C09,
    //! exe+0x36AD3C) gives the swapchain 2 + 1 buffers and the device a maximum frame latency of 1 + 1 (set before the
    //! ResizeBuffers, exe+0x36AD66). In VR the swapchain's buffers and latency are the mod's (D3D11Hook: 3 and 3, the
    //! measured need of synchronised stereo); with the option on the game asked 3 itself and the latency stayed at its 2,
    //! so VR ran differently with the option on and off. The option trades latency for even frame pacing on a monitor;
    //! in VR the headset paces the frames. Held off: every player gets the pipeline the mod was tested with.
    void HoldGameOptions(bool hold)
    {
        HoldOff("rs_enable", g_RsSaved, hold, "dynamic resolution");
        HoldOff("r_swapChainTripleBufferEnable", g_TbSaved, hold, "triple buffering");
        // HBAO+ (NVIDIA's, the game's menu option r_hbao) instead of the game's own ambient occlusion (r_SsaoType): with
        // AmbientOcclusion = 1 VR uses the game's own, the one the mod's measurements and any replacement compare
        // against (2026-11-03); HBAO+ is the costlier of the two
        HoldOff("r_hbao", g_HbaoSaved, hold, "HBAO+ ambient occlusion");
    }
}

namespace
{
    //! the game's own switch to windowed (POD)
    bool ApplyWindowed()
    {
        __try
        {
            const uintptr_t b = GameInfo::Base();
            ((void (*)(void*, int, bool))(b + kCvarSetInt))((void*)(b + kFullscreenCvar), 0, true);
            void* rs = *(void**)(b + kRenderSystemPtr);
            if (!rs) return false;
            void** vt = *(void***)rs;
            ((void (*)(void*))vt[kApplyWindowSlot])(rs);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}
bool RenderRes::OnWindowMessage(unsigned msg)
{
    if (msg != kMsgWindowed) return false;
    const int was = CVars::GetInt("r_fullscreen", -1);
    if (was == 0) return true;
    const bool ok = g_Installed && ApplyWindowed();
    if (ok) g_WindowedDone++;
    LOGI("RenderRes: the game switched to WINDOWED for VR (r_fullscreen {} -> {}){}", was, CVars::GetInt("r_fullscreen", -1),
         ok ? "" : ": the game's switch faulted");
    return true;
}

void RenderRes::SetRuntimeLimit(int w, int h)
{
    g_RtMaxW = w; g_RtMaxH = h;
    LOGI("RenderRes: the runtime's largest swapchain image is {}x{}: the render size's limit{}", w, h,
         Settings::MaxRenderMegapixels() > 0.f ? fmt::format(" (and MaxRenderMegapixels {:.1f} MP)", Settings::MaxRenderMegapixels()) : std::string());
}

void RenderRes::Install()
{
    if (!GameInfo::IsSupportedBuild()) { LOGI("RenderRes: unsupported build, not installed"); return; }
    g_Base = GameInfo::Base();
    g_Scale = Settings::RenderScale();
    if (g_Scale.load() <= 0.f) g_Mode = 0;
    g_RealGCR = (GetClientRect_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetClientRect");
    g_RealS2C = (ScreenToClient_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "ScreenToClient");
    g_RealWrite = (WriteConfig_t)(g_Base + kWriteConfig);
    g_RealPlace = (PlaceWindow_t)(g_Base + kPlaceWindow);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_RealGCR, HkGetClientRect);
    DetourAttach(&(PVOID&)g_RealS2C, HkScreenToClient);
    DetourAttach(&(PVOID&)g_RealWrite, HkWriteConfig);
    DetourAttach(&(PVOID&)g_RealPlace, HkPlaceWindow);
    const LONG r = DetourTransactionCommit();
    g_Installed = r == NO_ERROR;
    LOGI("RenderRes: GetClientRect + ScreenToClient + WriteConfigToFile@exe+0x{:X} + window placement@exe+0x{:X} detour={} (render scale {:.2f}{})",
         kWriteConfig, kPlaceWindow, r, g_Scale.load(), g_Mode.load() ? "" : ", off: the window's size");
}

bool RenderRes::RealClient(int& w, int& h)
{
    RECT c{};
    if (!g_Hwnd || !(g_RealGCR ? g_RealGCR(g_Hwnd, &c) : GetClientRect(g_Hwnd, &c))) return false;
    w = c.right - c.left; h = c.bottom - c.top;
    return w > 0 && h > 0;
}

void RenderRes::OnResizeBuffers(IDXGISwapChain*, UINT& w, UINT& h)
{
    const int tw = g_TW.load(), th = g_TH.load();
    if (tw > 0 && th > 0)
    {
        LOGI("RenderRes: swapchain buffers {}x{} (asked {}x{})", tw, th, w, h);
        w = (UINT)tw; h = (UINT)th; g_Buffers++;
    }
}

void RenderRes::OnPresent(IDXGISwapChain* sc)
{
    if (!g_Installed) return;
    if (!g_Hwnd) { DXGI_SWAP_CHAIN_DESC d{}; if (SUCCEEDED(sc->GetDesc(&d))) g_Hwnd = d.OutputWindow; if (!g_Hwnd) return; }
    {
        LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
        // Presents per second over the last 2 s (a ring of their times). It was a count restarted every second and
        // read whenever the heartbeat was written, so one hitch in the current window showed as the rate (46.8 read
        // against 55-62 Presents/s measured over 4 s in synchronised stereo).
        static LARGE_INTEGER ring[512]; static int head = 0, count = 0;
        ring[head] = now; head = (head + 1) % 512; if (count < 512) count++;
        int n = 0; double span = 0;
        for (int i = 1; i < count; i++)
        {
            const LARGE_INTEGER& t = ring[(head - 1 - i + 512) % 512];
            const double age = double(now.QuadPart - t.QuadPart) / f.QuadPart;
            if (age > 2.0) break;
            n = i; span = age;
        }
        g_Fps = span > 0.25 ? (float)(n / span) : g_Fps;
        (void)g_FpsT; (void)g_FpsN;
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d))) { g_BufW = (int)d.BufferDesc.Width; g_BufH = (int)d.BufferDesc.Height; }
        GpuTime(sc);
    }

    int w = 0, h = 0; std::string why;
    if (!ComputeTarget(w, h, why)) w = h = 0;
    g_Why = why;

    // A changed target is applied once it has held for 45 frames (the fov override and the eye fov arrive a few frames
    // apart when the package switches on); a vanished one at once. Changes under 1% are not worth a resize.
    const int cw = g_TW.load(), ch = g_TH.load();
    const bool close = cw > 0 && w > 0 && std::abs(w - cw) <= cw / 100 && std::abs(h - ch) <= ch / 100;
    if ((w == cw && h == ch) || close) g_WantFrames = 0;
    else
    {
        if (w != g_WantW || h != g_WantH) { g_WantW = w; g_WantH = h; g_WantFrames = 0; }
        if (w == 0 || ++g_WantFrames >= 45)
        {
            int rw = 0, rh = 0; RealClient(rw, rh);
            g_TW = w; g_TH = h; g_WantFrames = 0; g_Applies++;
            if (w > 0) LOGI("RenderRes: the game renders at {}x{} ({}; window {}x{}, scale {:.2f})", w, h, why, rw, rh, g_Scale.load());
            else LOGI("RenderRes: the game renders at its window's size {}x{} ({})", rw, rh, why);
            CrashLog::SetInfo(CrashLog::kRender, w > 0 ? fmt::format("{}x{} per image ({}), scale {:.2f}", w, h, why, g_Scale.load()) : std::string("the window's size"));
            g_Reapply = true;
        }
    }
    // held off whenever the headset shows the game, with or without a target: with the game's own fov the window can
    // already be dense enough (no target), and the 85% would still be there
    HoldGameOptions(g_Mode.load() != 0 && VRRuntime::GetStatus().running);
    // fullscreen or borderless while the headset runs: windowed, asked of the window's thread (see kFullscreenCvar)
    if (g_Mode.load() != 0 && VRRuntime::GetStatus().running && g_WindowedFails < 3)
    {
        const int fs = CVars::GetInt("r_fullscreen", 0);
        LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
        const double t = (double)c.QuadPart / (double)f.QuadPart;
        if (fs == 0) g_WindowedFails = 0;
        else if (t - g_WindowedAt > kWindowedRetry)
        {
            if (g_WindowedAt > 0 && ++g_WindowedFails >= 3) LOGI("RenderRes: the game stays in {} mode after 3 tries: set Display mode to windowed in its Options", fs == 1 ? "fullscreen" : "borderless");
            else if (PostMessageW(g_Hwnd, kMsgWindowed, 0, 0)) { g_WindowedAsks++; LOGI("RenderRes: the game is in {} mode: asking for windowed (VR renders at the headset's resolution only in a window)", fs == 1 ? "fullscreen" : "borderless"); }
            g_WindowedAt = t;
        }
    }
    if (g_Reapply.exchange(false)) Trigger();
}

std::string RenderRes::Status()
{
    int rw = 0, rh = 0; RealClient(rw, rh);
    const int* ds = g_Base ? (const int*)(g_Base + kDisplaySize) : nullptr;
    return fmt::format("renderres target {}x{} ({}) engine {}x{} buffers {}x{} window {}x{} scale {:.2f} rs {} tb {} fps {:.1f} gpu {:.2f} ms applies {} spoofed {} bufs {} cfgguard {} placeguard {} windowed asks {} done {}",
                       g_TW.load(), g_TH.load(), g_Why, ds ? ds[0] : 0, ds ? ds[1] : 0, g_BufW, g_BufH, rw, rh, g_Scale.load(),
                       CVars::GetInt("rs_enable", -1), CVars::GetInt("r_swapChainTripleBufferEnable", -1), g_Fps, g_GpuMs, g_Applies, g_Spoofed, g_Buffers, g_CfgGuards, g_PlaceGuards, g_WindowedAsks.load(), g_WindowedDone.load());
}

bool RenderRes::Command(const std::string& what, std::istringstream& ss)
{
    if (what == "rendersize")
    {   // vr rendersize auto | off | <w> <h>
        std::string a; ss >> a; int h = 0; ss >> h;
        if (a == "off" || a == "0") { g_Mode = 0; g_ManualW = 0; g_ManualH = 0; }
        else if (a == "auto" || a.empty()) { g_Mode = 1; g_ManualW = 0; g_ManualH = 0; }
        else { g_Mode = 1; g_ManualW = atoi(a.c_str()); g_ManualH = h; }
        LOGI("cmd vr rendersize {} {} -> mode {} manual {}x{}", a, h, g_Mode.load(), g_ManualW.load(), g_ManualH.load());
        return true;
    }
    if (what == "renderscale")
    {   // vr renderscale <s>: the target per axis relative to the eye's pixel density (0 = off: the window's size)
        float s = 1.f; ss >> s;
        if (s <= 0.f) g_Mode = 0; else { g_Mode = 1; g_Scale = s; }
        LOGI("cmd vr renderscale {} -> mode {} scale {:.2f}", s, g_Mode.load(), g_Scale.load());
        return true;
    }
    if (what == "renderres") { LOGI("{}", Status()); return true; }
    return false;
}
