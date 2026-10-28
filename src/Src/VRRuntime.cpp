#include "pch.h"
#include "VRRuntime.h"
#include "XRSession.h"
#include "XRCapture.h"
#include "GameInfo.h"
#include "CameraHook.h"
#include "ViewMatrixHook.h"
#include "GpuTrace.h"
#include "PipeTrace.h"
#include "Settings.h"
#include "XRProbe.h"
#include "WristHud.h"
#include "KbmInject.h"
#include "ScreenMode.h"
#include "StereoSync.h"
#include "StereoTaa.h"
#include "VRMath.h"
#include <cmath>

namespace
{
    XRSession g_Session;
    bool g_InitAttempted = false;
    std::atomic<float> g_FovH{ 90.f }, g_FovV{ 0.f };  //!< 0 = derive from aspect
    std::mutex g_CapMutex;
    std::string g_CapTag;
    std::atomic<bool> g_CapRequested{ false };
    std::mutex g_StatusMutex;
    VRRuntime::Status g_Status;
    std::mutex g_PoseMutex;
    VRRuntime::HeadPose g_Head;
    XRInputState g_Input; bool g_InputValid = false;
    ULONGLONG g_InputAt = 0;   //!< GetTickCount64 of the last synced input (no frame = no fresh input)
    VRRuntime::HeadPose g_Eye[2];
    std::atomic<bool> g_Stereo{ false };
    std::atomic<int> g_CurEye{ 0 };
    VRRuntime::Placement g_Place[2];   // under g_StatusMutex
    std::atomic<bool> g_Ortho{ true };
    std::atomic<bool> g_TruePose{ true };
    VRRuntime::RenderPoseStats g_RP;   // under g_StatusMutex
    struct SyncStats { unsigned long long staged = 0, stageFailed = 0, pairs = 0, unpaired = 0, unpairedRight = 0, mono = 0, leftAlone = 0, dropped = 0, ownEye = 0; } g_Sync;   // under g_StatusMutex
    // vr unpairedeye 1: a stereo eye's image that found no partner goes to ITS eye only, the other keeps its last image
    // (0 = to both eyes as mono, before 2026-10-23: with the per-eye turned frustum the other eye's outer edge was black)
    std::atomic<int> g_UnpairedOwn{ 1 };
    long long g_FrameD = 0, g_FramePeriod = 0;   // the begun frame's predicted display time / period (under g_PoseMutex)
    std::atomic<int> g_PresentedEye{ -1 };
    std::atomic<int> g_LayerFov{ 0 };   // vr layerfov (see the header)

    // ---- Pose prediction (VRRuntime::LocateForView) ----
    // Each view used the poses located at the LAST BeginFrame, and its image reaches the headset some frames later: in
    // mono the view is built a frame before the Present before the one that shows it (2 frames), a synchronised pair's
    // left view 2 pair periods before (4 frames). Measured with the head turning at 60 deg/s: the image 36-55 ms behind
    // the head in mono, 74-115 ms in stereo 2. The gap from the frame display time a view's poses were
    // located with to the display time of the frame that shows it is measured on every submitted image (per kind of
    // view), and the next view of that kind is located for base + that gap.
    std::atomic<int> g_Predict{ 1 };             // vr predict: 0 off, 1 k display periods, 2 the learned gap as is
    std::atomic<bool> g_PredictServo{ true };    // vr predictservo
    std::atomic<float> g_PredictMaxMs{ 120.f };  // vr predictmax
    struct GapStat { double ema = 0; unsigned long long n = 0; double errAbs = 0, lastErr = 0; unsigned long long errN = 0; };
    GapStat g_Gap[3];                            // under g_StatusMutex; [kind]
    unsigned long long g_SetsAhead[3] = {}, g_SetsLatest[3] = {}, g_ServoAhead = 0, g_ServoLatest = 0;   // under g_StatusMutex
    long long g_ServoT = 0; VRRuntime::HeadPose g_ServoHead; bool g_ServoHeadValid = false;          // render thread
    void HeadFromEyes(const XREyePose e[2], const XREyePose& h, unsigned gen, VRRuntime::HeadPose eyes[2], VRRuntime::HeadPose& head)
    {
        for (int i = 0; i < 2; i++)
        {
            eyes[i].valid = e[i].valid; eyes[i].gen = gen;
            eyes[i].pos[0] = e[i].posX; eyes[i].pos[1] = e[i].posY; eyes[i].pos[2] = e[i].posZ;
            eyes[i].quat[0] = e[i].quatX; eyes[i].quat[1] = e[i].quatY; eyes[i].quat[2] = e[i].quatZ; eyes[i].quat[3] = e[i].quatW;
        }
        head.valid = e[0].valid; head.gen = gen;
        for (int k = 0; k < 3; k++) head.pos[k] = (eyes[0].pos[k] + eyes[1].pos[k]) * 0.5f;
        // the head's own orientation (XRSession::Head), not the left eye's: canted displays turn each eye ~10 deg out
        head.quat[0] = h.quatX; head.quat[1] = h.quatY; head.quat[2] = h.quatZ; head.quat[3] = h.quatW;
    }
    //! The display time a view of this kind built now will be shown at, from the frame display time `base`.
    bool PredictTime(int kind, long long base, long long& t)
    {
        const int m = g_Predict.load();
        if (!m || base <= 0 || kind < 0 || kind > 2) return false;
        double gap; unsigned long long n;
        { std::lock_guard lk(g_StatusMutex); gap = g_Gap[kind].ema; n = g_Gap[kind].n; }
        if (n < 8) return false;                 // not learned yet
        const double mx = (double)g_PredictMaxMs.load() * 1e6;
        if (gap > mx) gap = mx;
        if (gap <= 0) return false;
        long long per; { std::lock_guard lk(g_PoseMutex); per = g_FramePeriod; }
        if (m == 1 && per > 0) { const long long k = llround(gap / (double)per); t = base + k * per; }
        else t = base + (long long)gap;
        return t > base;
    }
    //! An image is being submitted: how far its view's poses were from this frame's display time (learns the gap).
    void NoteShown(const ViewMatrixHook::XrPose& rp)
    {
        if (!rp.valid || rp.predKind < 0 || rp.predKind > 2 || rp.dispBase <= 0) return;
        const long long d = g_Session.FrameDisplayTime();
        if (d <= 0) return;
        const double gap = (double)(d - rp.dispBase);
        if (gap < 0 || gap > 0.5e9) return;
        std::lock_guard lk(g_StatusMutex);
        GapStat& g = g_Gap[rp.predKind];
        g.ema = g.n ? g.ema + (gap - g.ema) * 0.1 : gap; g.n++;
        const double err = (double)(d - rp.dispTime);   // > 0: shown later than its poses were located for
        g.lastErr = err; g.errAbs += fabs(err); g.errN++;
    }

    // ---- Closed-loop eye phasing (vr phaseloop) ----
    // Which view the presented image is was a frame-order rule (source 4); the TAA dispatch of that image measures it.
    // Now the measurement wins when there is one; a disagreement drops that image (the headset keeps the last frame),
    // and the same disagreement twice in a row moves the rule's phase, so it cannot stay off once the TAA stops.
    std::atomic<bool> g_PhaseClosed{ true };
    long long g_PhaseOffset = 0, g_LastPhaseD = 0; int g_PhaseRun = 0;     // render thread
    unsigned long long g_PhaseFixes = 0, g_PhaseDrops = 0;                 // under g_StatusMutex
    bool g_DropThis = false;                                               // render thread: this Present's image is dropped

    // ---- Stereo health (vr stereohealth) ----
    // Synchronised stereo that delivers no pair for a second of play (every pair unpaired, an eye with no new image), or
    // whose frames the runtime keeps refusing, falls back to mono -- logged -- and tries again later (10 s, doubling).
    std::atomic<bool> g_Health{ true };
    double g_LastPairT = 0, g_GateOpenSince = 0, g_RetryAt = 0, g_Backoff = 10; bool g_FellBack = false;
    unsigned g_FailsAt = 0; double g_FailsT = 0, g_LastAttemptT = 0; unsigned long long g_Fallbacks = 0;
    // The back-off doubles at each fallback (10 -> 80 s); after a minute of stereo that delivers pairs it is forgiven:
    // one bad patch an hour ago must not make the next one cost 80 s of mono.
    double g_StableSince = 0; unsigned long long g_BackoffResets = 0;
    std::atomic<int> g_MirrorEye{ 0 };           // vr mirroreye: the desktop window in stereo 2 (-1 = as presented, alternating)
    unsigned long long g_Mirrored = 0;
    double NowS() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart / (double)f.QuadPart; }

    //! The pose the image about to be presented was rendered from.
    //! Source 4 (default): the view that was the latest one at the PREVIOUS Present. The main thread builds view N+1 and
    //! then presents image N, every frame, in that order (measured with vr pipetrace: V N+1, P, then the worker threads
    //! finish N+1's command lists). The old sources all named view N+1 at that Present -- the executed command lists'
    //! serial (2) is never seen (no ExecuteCommandList reaches the hook here), so it fell back to the draws' (1) or the
    //! latest view (0): the layer claimed the NEXT view's pose for every image, and the stereo eyes came out swapped
    //! (measured on the mock: every disparity of the right size with the wrong sign; source 3 / 4 put it right).
    //! Checked every frame against the view of the image's own TAA dispatch (StereoTaa), when the TAA runs.
    std::atomic<int> g_PoseSource{ 4 };   // vr posesource: 4 the view before the previous Present, 2 exec, 1 draws, 0 latest, 3 latest - 1
    unsigned long long g_PresentedSerial = 0, g_LatestAtPresent = 0;   // render thread
    unsigned long long g_TaaAgree = 0, g_TaaDisagree = 0, g_TaaNone = 0;     // under g_StatusMutex
    ViewMatrixHook::XrPose PickRenderPose(int& source)
    {
        unsigned long long s = 0, age = 0;
        const int allow = g_PoseSource.load();
        if (allow == 4)
        {
            source = 4;
            const ViewMatrixHook::XrPose rp = g_PresentedSerial ? ViewMatrixHook::RenderPoseXr(g_PresentedSerial) : ViewMatrixHook::XrPose();
            return rp;
        }
        if (allow == 3)
        {   // research: the view BEFORE the latest (the pipeline depth PipeTrace showed)
            const ViewMatrixHook::XrPose last = ViewMatrixHook::RenderPoseXr(0);
            const ViewMatrixHook::XrPose prev = last.valid ? ViewMatrixHook::RenderPoseXr(last.serial - 1) : last;
            source = 0; return prev.valid ? prev : last;
        }
        if (allow >= 2 && GpuTrace::ExecutedViewSerial(s, age) && age <= 1)
        { const ViewMatrixHook::XrPose rp = ViewMatrixHook::RenderPoseXr(s); if (rp.valid) { source = 2; return rp; } }
        if (allow >= 1 && GpuTrace::DrawViewSerial(s, age) && age <= 1)
        { const ViewMatrixHook::XrPose rp = ViewMatrixHook::RenderPoseXr(s); if (rp.valid) { source = 1; return rp; } }
        source = 0;
        return ViewMatrixHook::RenderPoseXr(0);
    }

    void Capture(IDXGISwapChain* sc, ID3D11Device* dev, ID3D11Texture2D* bb)
    {
        std::string tag;
        { std::lock_guard lk(g_CapMutex); tag = g_CapTag; }
        ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
        std::wstring dir = GameInfo::GameDir() + L"\\";
        std::wstring wtag(tag.begin(), tag.end());
        bool ok = XRCapture::SaveTextureToPng(dev, ctx, bb, (dir + L"DVR_" + wtag + L"_frame.png").c_str());
        int eyes = 0;
        if (g_Session.IsInitialized())
            for (int i = 0; i < 2; i++)
                if (ID3D11Texture2D* t = g_Session.LastEyeTexture(i))
                    eyes += XRCapture::SaveTextureToPng(dev, ctx, t, (dir + L"DVR_" + wtag + L"_eye" + std::to_wstring(i) + L".png").c_str()) ? 1 : 0;
        ctx->Release();
        LOGI("Capture '{}': frame={} eyes={}", tag, ok, eyes);
    }
}

namespace
{
    // vr burst: N CONSECUTIVE frames, copied on the GPU at each Present (no stall, so the game's timing is untouched)
    // and written out as PNGs once the last one is in. What a single capture cannot show: how a thing moves from one
    // frame to the next.
    std::mutex g_BurstM; int g_BurstWant = 0, g_BurstStride = 1, g_BurstTick = 0; std::string g_BurstTag; std::vector<ID3D11Texture2D*> g_BurstTex;
    void Burst(ID3D11Device* dev, ID3D11Texture2D* bb)
    {
        std::lock_guard lk(g_BurstM);
        if (g_BurstWant <= 0 && g_BurstTex.empty()) return;
        ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
        if (g_BurstWant > 0)
        {
            D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
            d.BindFlags = 0; d.MiscFlags = 0; d.CPUAccessFlags = 0; d.Usage = D3D11_USAGE_DEFAULT;
            ID3D11Texture2D* t = nullptr;
            if (g_BurstTick++ % g_BurstStride == 0)   // every stride-th frame (vr gunburst n stride: a longer window)
            {
                if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t)) && t) { ctx->CopyResource(t, bb); g_BurstTex.push_back(t); }
                g_BurstWant--;
            }
        }
        else
        {
            const std::wstring dir = GameInfo::GameDir() + L"\\DVR_" + std::wstring(g_BurstTag.begin(), g_BurstTag.end()) + L"_b";
            int ok = 0;
            for (size_t i = 0; i < g_BurstTex.size(); i++)
            {
                wchar_t n[16]; swprintf_s(n, L"%02zu.png", i);
                ok += XRCapture::SaveTextureToPng(dev, ctx, g_BurstTex[i], (dir + n).c_str()) ? 1 : 0;
                g_BurstTex[i]->Release();
            }
            LOGI("burst '{}': {} of {} frames written", g_BurstTag, ok, g_BurstTex.size());
            g_BurstTex.clear();
        }
        ctx->Release();
    }
}
void VRRuntime::RequestBurst(int frames, const std::string& tag, int stride)
{
    std::lock_guard lk(g_BurstM);
    if (!g_BurstTex.empty()) return;                 // one burst at a time
    g_BurstWant = frames < 1 ? 1 : frames > 60 ? 60 : frames; g_BurstTag = tag; g_BurstStride = stride < 1 ? 1 : stride; g_BurstTick = 0;
}
void VRRuntime::SetGameFov(float hDeg, float vDeg) { g_FovH = hDeg; g_FovV = vDeg; }

void VRRuntime::RequestCapture(const std::string& tag)
{
    { std::lock_guard lk(g_CapMutex); g_CapTag = tag; }
    g_CapRequested = true;
}

void VRRuntime::OnPresent(IDXGISwapChain* sc)
{
    {   // which view this Present shows (see PickRenderPose), and the check against the image's own TAA dispatch
        g_DropThis = false;
        const unsigned long long rule = g_LatestAtPresent ? (unsigned long long)((long long)g_LatestAtPresent + g_PhaseOffset) : 0;
        g_PresentedSerial = rule;
        g_LatestAtPresent = ViewMatrixHook::LatestSerial();
        const unsigned long long taa = StereoTaa::TakeTaaSerial();
        std::lock_guard lk(g_StatusMutex);
        if (!taa || !rule) g_TaaNone++;
        else if (taa == rule) { g_TaaAgree++; g_PhaseRun = 0; }
        else
        {
            g_TaaDisagree++;
            const long long d = (long long)taa - (long long)rule;
            if (g_PhaseClosed.load() && taa <= g_LatestAtPresent)
            {   // the measurement names the image: claim ITS pose, but do not show it (it may be paired with the wrong
                // eye's staged image); after the same offset twice the rule itself moves
                g_PresentedSerial = taa; g_DropThis = true; g_PhaseDrops++;
                g_PhaseRun = d == g_LastPhaseD ? g_PhaseRun + 1 : 1; g_LastPhaseD = d;
                if (g_PhaseRun >= 2 && g_PhaseOffset + d >= -3 && g_PhaseOffset + d <= 3)
                {
                    g_PhaseOffset += d; g_PhaseRun = 0; g_PhaseFixes++;
                    LOGI("VRRuntime: eye phase corrected: the presented image is the view {} from the frame-order rule (TAA measured it {} times in a row); rule offset now {}",
                         d > 0 ? fmt::format("+{}", d) : fmt::format("{}", d), 2, g_PhaseOffset);
                }
            }
            if (g_TaaDisagree <= 10 || g_TaaDisagree % 1000 == 0)
                LOGI("VRRuntime: the presented image's TAA ran with view {}, the frame order says view {} ({} disagreements){}", taa, rule, g_TaaDisagree,
                     g_DropThis ? ": that image is dropped, its view taken from the measurement" : "");
        }
    }
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return;
    ID3D11Texture2D* bb = nullptr;
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);

    {   // Start the OpenXR session; again every few seconds while there is no headset yet (it may be switched on, or
        // Link / Air Link / Virtual Desktop connected, after the game started); and from scratch after the connection
        // is lost. Until it runs, the game is the plain flat game (the VR package waits for it: Commands).
        static ULONGLONG nextTry = 0; static std::string lastError;
        const ULONGLONG now = GetTickCount64();
        if (g_Session.IsInitialized() && g_Session.NeedsRestart())
        {   // (Shutdown is false while the OpenXR frame thread is still inside a runtime call: again next Present)
            const bool fresh = g_Session.RestartIsFresh();
            if (g_Session.Shutdown())
            {
                nextTry = fresh ? now : now + 3000; lastError.clear();
                if (fresh) LOGI("VRRuntime: OpenXR session closed: a new one now (the runtime did not offer the old one back)");
                else LOGI("VRRuntime: OpenXR session closed after a loss: starting again in 3 s");
            }
        }
        // Not while the startup probe still holds its own instance (it runs on the init thread and can take seconds):
        // Meta's runtime refuses a second instance in the same process (-10), and then there is no VR at all.
        if (XRProbe::Done() && !g_Session.IsInitialized() && !g_Session.Exited() && (!g_InitAttempted || (g_Session.ShouldRetry() && now >= nextTry)))
        {
            g_InitAttempted = true;
            g_Session.SetPitchOffset(Settings::ControllerPitchOffset());
            g_Session.SetLogInput(Settings::LogInput());
            if (!g_Session.Init(dev))
            {
                nextTry = now + 3000;
                if (g_Session.LastError() != lastError)
                {   // once per distinct reason, not every retry
                    lastError = g_Session.LastError();
                    LOGI("VRRuntime: OpenXR session not started ({}). Running flat{}.", lastError, g_Session.ShouldRetry() ? ", retrying every 3 s" : "");
                }
            }
            else lastError.clear();
        }
    }

    if (g_Health.load() && g_Session.IsSessionRunning())
    {   // synchronised stereo that produces no pairs falls back to mono, and tries again later
        const double now = NowS();
        if (StereoSync::Active())
        {
            if (!StereoSync::PairWanted() || !g_GateOpenSince) g_GateOpenSince = StereoSync::PairWanted() ? now : 0;
            const unsigned fails = g_Session.EndFrameFails();
            if (now - g_FailsT > 2.0) { g_FailsT = now; g_FailsAt = fails; }      // refusals counted over 2 s windows
            // (only while the runtime takes frames at all: a stalled runtime is not a stereo fault)
            const bool noPairs = g_GateOpenSince && now - g_GateOpenSince > 1.0 && now - g_LastPairT > 1.0 && now - g_LastAttemptT < 0.25;
            const bool refused = fails - g_FailsAt >= 10;
            if (noPairs || refused)
            {
                unsigned long long unp, mono, alone;
                { std::lock_guard lk(g_StatusMutex); unp = g_Sync.unpaired; mono = g_Sync.mono; alone = g_Sync.leftAlone; g_Fallbacks++; }
                LOGI("VRRuntime: STEREO FALLBACK to mono: {} (unpaired {}, mono {}, left images alone {}, xrEndFrame refused {}); trying stereo again in {:.0f} s",
                     refused ? "the runtime refused 10 frames in a row" : "no stereo pair reached the headset for a second of play", unp, mono, alone, fails, g_Backoff);
                StereoSync::SetActive(false); StereoTaa::OnStereoChange(false);
                g_FellBack = true; g_RetryAt = now + g_Backoff; g_Backoff = g_Backoff * 2 > 80 ? 80 : g_Backoff * 2;
                g_FailsAt = fails; g_GateOpenSince = 0; g_StableSince = 0;
            }
            else
            {
                if (!g_StableSince) g_StableSince = now;
                if (g_Backoff > 10 && now - g_StableSince > 60.0 && now - g_LastPairT < 1.0)
                {
                    LOGI("VRRuntime: synchronised stereo healthy for {:.0f} s: the fallback back-off is back to 10 s (was {:.0f})", now - g_StableSince, g_Backoff);
                    g_Backoff = 10; g_BackoffResets++;
                }
            }
        }
        else if (g_FellBack && now >= g_RetryAt)
        {
            LOGI("VRRuntime: stereo fallback over: synchronised stereo again");
            g_FellBack = false; g_LastPairT = now; g_GateOpenSince = 0; g_FailsAt = g_Session.EndFrameFails(); g_StableSince = now;
            StereoTaa::OnStereoChange(true); StereoSync::SetActive(true);
        }
    }

    if (g_Session.IsInitialized() && bb)
    {
        g_Session.PollEvents();
        if (g_Session.IsSessionRunning())
        {
            D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
            float h = g_FovH.load() * 3.14159265f / 180.f;
            float v = g_FovV.load() > 0 ? g_FovV.load() * 3.14159265f / 180.f
                                        : 2.f * atanf(tanf(h * 0.5f) * (float)d.Height / (float)d.Width);
            g_Session.SetLayerFov(h, v);
            g_Session.SetMatchedFov(false);
            {   // the flat frame's real extent, from the fov the main view was built with (CameraHook::BuiltFov):
                // the image is drawn at exactly those angles in each eye (XRSession::SetOrtho)
                float bx = 0, by = 0; unsigned long long n = 0; CameraHook::BuiltFov(bx, by, n);
                if (bx > 1.f && by > 1.f && bx < 170.f && by < 170.f)
                {
                    const float tx = tanf(bx * 0.5f / 57.29578f), ty = tanf(by * 0.5f / 57.29578f);
                    g_Session.SetSourceTans(-tx, tx, ty, -ty);
                }
                g_Session.SetOrtho(g_Ortho.load());
            }
            // Each image is drawn at the frustum ITS view was rendered with (ViewMatrixHook::XrPose::tan): with the per-eye
            // projection the two eyes' frusta differ, and a mono view between pairs has the cover of both.
            auto useTans = [&](const ViewMatrixHook::XrPose& rp) { if (rp.valid && rp.tanValid) g_Session.SetSourceTans(rp.tan[0], rp.tan[1], rp.tan[2], rp.tan[3]); };
            // Its frame relative to each eye's (warped into the eye's native fov when they differ): a mono image is the
            // head's (canted displays turn each eye out of it), a stereo eye's is that eye's turned to its frustum's
            // centre (ViewMatrixHook::XrPose::warp). warp_i = conj(eye i rel. head) * (image frame rel. head).
            auto useWarps = [&](const ViewMatrixHook::XrPose& rp, bool bothEyes) {
                float img[4] = { 0, 0, 0, 1 };
                if (rp.valid && rp.eye >= 0) { float rel[4]; if (EyeRelative(rp.eye, rel)) VRMath::QMul(rel, rp.warp, img); else memcpy(img, rp.warp, 16); }
                for (int i = 0; i < 2; i++)
                {
                    if (!bothEyes && i != rp.eye) continue;
                    float rel[4] = { 0, 0, 0, 1 }, w[4]; EyeRelative(i, rel);
                    VRMath::QConjMul(rel, img, w);
                    g_Session.SetSourceWarp(i, w);
                }
            };
            // The game's interface, lifted out of the stereo pictures (ScreenMode::PlayHud, StereoHud): one panel for both
            // eyes on the plane of the picture it was drawn over, in the frame the game camera rendered it as -- the
            // eye's turn to its frustum (warp) and the eye's own offset from the head taken back out
            auto playHud = [&](const ViewMatrixHook::XrPose& rp) {
                if (!rp.valid || !rp.tanValid || rp.flat) return;
                float q[4]; memcpy(q, rp.quat, 16);
                if (rp.eye >= 0)
                {
                    const float cw[4] = { -rp.warp[0], -rp.warp[1], -rp.warp[2], rp.warp[3] };
                    float e[4]; VRMath::QMul(rp.quat, cw, e);     // the eye
                    float rel[4];
                    if (EyeRelative(rp.eye, rel)) { const float cr[4] = { -rel[0], -rel[1], -rel[2], rel[3] }; VRMath::QMul(e, cr, q); }
                    else memcpy(q, e, 16);
                }
                float pos[3]; memcpy(pos, rp.pos, 12);
                VRRuntime::PoseSet vps;
                if (ViewMatrixHook::ViewPoses(rp.pair ? rp.pair : rp.serial, vps) && vps.valid && vps.head.valid) memcpy(pos, vps.head.pos, 12);
                ScreenMode::PlayHud(g_Session, q, pos, rp.tan);
            };
            // The layer's fov (vr layerfov): a stereo eye's image declares its own (margin included) when asked; a mono
            // image goes into each eye's native fov (warped there on canted displays)
            auto declareFor = [&](bool stereoEye) { g_Session.SetDeclareFov(stereoEye && g_LayerFov.load() != 0); };
            // Synchronised stereo (StereoSync): the LEFT image of a pair is only kept until its right image arrives on
            // the next Present; that Present submits both. No OpenXR frame for the left one, so the game is free to
            // render two images per headset frame (BeginFrame paces it to the headset's rate once per pair).
            bool pairLeft = false;
            if (StereoSync::Active() && g_Session.EyeCount() == 2 && g_TruePose.load())
            {
                int source = 0;
                const ViewMatrixHook::XrPose rp = PickRenderPose(source);
                // its right view is already built (the pipeline: view N+1 is built before image N is presented); when
                // it is not (the hold was refused), this left image goes to both eyes now instead of waiting for nothing
                const ViewMatrixHook::XrPose next = rp.valid ? ViewMatrixHook::RenderPoseXr(rp.serial + 1) : ViewMatrixHook::XrPose();
                const bool rightComing = next.valid && next.eye == 1 && next.pair == rp.pair;
                // (vr syncdbg 256, test of the health check: no left image is ever staged, so no pair forms)
                if (rp.valid && rp.eye == 0 && rp.pair && !rp.flat && g_Session.OrthoReady() && !g_DropThis && rightComing && !(StereoSync::Debug() & 256))
                {
                    pairLeft = g_Session.StagePairLeft(bb, rp.quat, rp.pos, rp.pair, rp.tanValid ? rp.tan : nullptr, rp.warp);
                    std::lock_guard lk(g_StatusMutex);
                    if (pairLeft) g_Sync.staged++; else g_Sync.stageFailed++;
                    PipeTrace::Event(PipeTrace::Present, 0);
                }
                else if (rp.valid && rp.eye == 0 && rp.pair && !rightComing) { std::lock_guard lk(g_StatusMutex); g_Sync.leftAlone++; }
            }
            if (pairLeft) {}
            else if (g_Session.BeginFrame())
            {
                const XREyePose& e0 = g_Session.Eye(0);
                const XREyePose& e1 = g_Session.Eye(g_Session.EyeCount() > 1 ? 1 : 0);
                {
                    std::lock_guard lk(g_PoseMutex);
                    g_FrameD = g_Session.FrameDisplayTime(); g_FramePeriod = g_Session.FramePeriod();
                    g_Head.valid = e0.valid; g_Head.gen = g_Session.RecenterGeneration();
                    g_Head.pos[0] = (e0.posX + e1.posX) * 0.5f; g_Head.pos[1] = (e0.posY + e1.posY) * 0.5f; g_Head.pos[2] = (e0.posZ + e1.posZ) * 0.5f;
                    const XREyePose& hd = g_Session.Head();   // the head's orientation, not the left eye's (canted displays)
                    g_Head.quat[0] = hd.quatX; g_Head.quat[1] = hd.quatY; g_Head.quat[2] = hd.quatZ; g_Head.quat[3] = hd.quatW;
                    const XREyePose* eyes[2] = { &e0, &e1 };
                    for (int i = 0; i < 2; i++)
                    {
                        g_Eye[i].valid = eyes[i]->valid; g_Eye[i].gen = g_Head.gen;
                        g_Eye[i].pos[0] = eyes[i]->posX; g_Eye[i].pos[1] = eyes[i]->posY; g_Eye[i].pos[2] = eyes[i]->posZ;
                        g_Eye[i].quat[0] = eyes[i]->quatX; g_Eye[i].quat[1] = eyes[i]->quatY; g_Eye[i].quat[2] = eyes[i]->quatZ; g_Eye[i].quat[3] = eyes[i]->quatW;
                    }
                }
                g_Session.SyncInput();
                { std::lock_guard lk(g_PoseMutex); g_Input = g_Session.Input(); g_InputValid = true; g_InputAt = GetTickCount64(); }
                // the watch on the left wrist: decided on this frame's poses, drawn into this frame's layers
                WristHud::OnXrFrame(g_Session, KbmInject::GetStats().mode == 1);
                // Menus, loading screens and cutscenes on a screen in the room (ScreenMode): it submits the frame
                // itself. Which kind of picture this is comes from its own view record (a flat view, or not).
                bool imageFlat = false, imageKnown = false;
                if (g_TruePose.load())
                {
                    int source = 0;
                    const ViewMatrixHook::XrPose rp = PickRenderPose(source);
                    imageKnown = rp.serial != 0; imageFlat = rp.flat;
                    // a snap turn still on its way into the picture (KbmInject, SnapTurnBlank): black, then a short fade in
                    if (!imageFlat) g_Session.DimFrame(KbmInject::SnapBlankFade(rp.pair ? rp.pair : rp.serial));
                }
                else g_Session.DimFrame(KbmInject::SnapBlankFade(0));
                if (g_DropThis && !imageFlat)
                {   // a disagreement of the eye phasing (above): nothing new to the headset this frame
                    g_Session.DropStagedPair(); g_Session.ResubmitLast();
                    std::lock_guard lk(g_StatusMutex); g_Sync.dropped++;
                }
                else if (ScreenMode::OnXrFrame(g_Session, bb, imageFlat, imageKnown))
                    PipeTrace::Event(PipeTrace::Present, (uint64_t)-2);
                else if (StereoSync::Active() && g_Session.EyeCount() == 2)
                {   // the RIGHT image of a pair: both eyes now, each claimed at the pose it was rendered from
                    int source = 0;
                    const ViewMatrixHook::XrPose rp = PickRenderPose(source);
                    PipeTrace::Event(PipeTrace::Present, 1);
                    playHud(rp);
                    declareFor(true);
                    if (rp.valid && rp.eye == 1 && rp.pair && g_Session.SubmitPair(bb, rp.quat, rp.pos, rp.pair, rp.tanValid ? rp.tan : nullptr, rp.warp))
                    {
                        PipeTrace::Event(PipeTrace::Submit, rp.serial, 2);
                        NoteShown(rp); g_LastPairT = g_LastAttemptT = NowS(); g_PresentedEye = 1;
                        // the desktop window: the left image again instead of the right one (it alternated: shaking)
                        if (g_MirrorEye.load() == 0 && g_Session.CopyPairLeft(bb)) g_Mirrored++;
                        std::lock_guard lk(g_StatusMutex); g_Sync.pairs++;
                        g_RP.used++; if (source) g_RP.fromDraws++; if (source == 2) g_RP.fromExec++; if (rp.fromGameAxis) g_RP.gameAxis++;
                    }
                    else if (g_UnpairedOwn.load() && rp.valid && rp.eye >= 0 && g_Session.LastEyeTexture(rp.eye ^ 1))
                    {   // no pair, but an eye's image (its own turned frustum, which does not cover the other eye's outer
                        // edge): into its own eye; the other eye keeps its last image, claimed where that was rendered
                        g_Session.SetRenderPose(rp.quat, rp.pos);
                        PipeTrace::Event(PipeTrace::Submit, rp.serial, (uint64_t)(int64_t)rp.eye);
                        useTans(rp); g_Session.SetSourceWarp(rp.eye, rp.warp); declareFor(true);
                        g_Session.SubmitFrame(bb, rp.eye);
                        NoteShown(rp); g_PresentedEye = rp.eye; g_LastAttemptT = NowS();
                        std::lock_guard lk(g_StatusMutex);
                        g_Sync.unpaired++; g_Sync.ownEye++;
                        if (rp.eye == 1) g_Sync.unpairedRight++;
                    }
                    else
                    {   // no pair (a frame the stereo did not render as one: entering play, a hitch): this image to both
                        // eyes, at its own pose, as mono
                        if (rp.valid) g_Session.SetRenderPose(rp.quat, rp.pos);
                        PipeTrace::Event(PipeTrace::Submit, rp.serial, (uint64_t)-1);
                        useTans(rp); useWarps(rp, true); declareFor(false);
                        g_Session.SubmitFrame(bb);
                        NoteShown(rp); g_PresentedEye = -1; g_LastAttemptT = NowS();
                        static int logged = 0;
                        const char* why = !rp.valid ? "no view record" : rp.eye < 0 ? nullptr : rp.eye == 0 ? "a left image whose right view was not built"
                                        : !rp.pair ? "not part of a pair" : "the staged left image is not this pair's";
                        if (why && logged < 10) { logged++; LOGI("VRRuntime: a stereo image went to both eyes alone: {} (view {} eye {} pair {})", why, rp.serial, rp.eye, rp.pair); }
                        std::lock_guard lk(g_StatusMutex);
                        if (rp.valid && rp.eye < 0) g_Sync.mono++; else g_Sync.unpaired++;
                        if (rp.valid && rp.eye == 1) g_Sync.unpairedRight++;
                    }
                }
                else if (g_Stereo.load() && g_Session.EyeCount() == 2)
                {
                    // Pipeline phase: the engine builds the NEXT frame's view (our CreateViewMatrix hook,
                    // which reads g_CurEye) before it presents the PREVIOUS one, so the backbuffer we
                    // hold here was rendered with the eye BEFORE the current value. Measured 2026-08-29
                    // with the mock (near-object disparity came out with the wrong sign otherwise).
                    // Now the view's own record says which eye and pose the image was drawn from (the serial its
                    // draws were placed with); the phase guess is only the fallback.
                    int eye = g_CurEye.load();
                    int target = eye ^ 1;
                    PipeTrace::Event(PipeTrace::Present, (uint64_t)eye);
                    if (g_TruePose.load())
                    {
                        int source = 0;
                        const ViewMatrixHook::XrPose rp = PickRenderPose(source);
                        playHud(rp);
                        if (rp.valid && rp.eye >= 0)
                        {
                            NoteShown(rp);
                            g_Session.SetRenderPose(rp.quat, rp.pos);
                            std::lock_guard lk(g_StatusMutex);
                            g_RP.used++; if (source) g_RP.fromDraws++; if (source == 2) g_RP.fromExec++; if (rp.fromGameAxis) g_RP.gameAxis++;
                            if (rp.eye != target) g_RP.phaseFix++;
                            target = rp.eye;
                        }
                        PipeTrace::Event(PipeTrace::Submit, rp.serial, (uint64_t)(int64_t)target);
                        useTans(rp); if (rp.valid && rp.eye >= 0) g_Session.SetSourceWarp(rp.eye, rp.warp);
                    }
                    declareFor(true);
                    g_Session.SubmitFrame(bb, target);
                    g_CurEye.store(eye ^ 1);      // next frame renders from the other eye
                }
                else
                {
                    PipeTrace::Event(PipeTrace::Present, (uint64_t)-1);
                    if (g_TruePose.load())
                    {   // the view this image was drawn from (PickRenderPose)
                        int source = 0;
                        ViewMatrixHook::XrPose rp = PickRenderPose(source);
                        const ViewMatrixHook::XrPose latest = ViewMatrixHook::RenderPoseXr(0);
                        PipeTrace::Event(PipeTrace::Submit, rp.serial, (uint64_t)-1);
                        if (rp.valid)
                        {
                            g_Session.SetRenderPose(rp.quat, rp.pos);
                            NoteShown(rp); g_PresentedEye = -1;
                            std::lock_guard lk(g_StatusMutex);
                            g_RP.used++; if (source) g_RP.fromDraws++; if (source == 2) g_RP.fromExec++; if (rp.fromGameAxis) g_RP.gameAxis++;
                            if (latest.serial > rp.serial) g_RP.behindLatest++;
                            useTans(rp); useWarps(rp, true);
                            // how far the claimed pose is from the head right now (deg): what the compositor corrects
                            const float* q = rp.quat; const float* h = g_Head.quat;
                            const float d = fabsf(q[0] * h[0] + q[1] * h[1] + q[2] * h[2] + q[3] * h[3]);
                            const float ang = 2.f * acosf(d > 1.f ? 1.f : d) * 57.29578f;
                            g_RP.lastDiff = ang; if (ang > g_RP.maxDiff) g_RP.maxDiff = ang;
                        }
                    }
                    declareFor(false);
                    g_Session.SubmitFrame(bb);
                }
                g_Session.EndFrameNoRender();   // a frame nothing above submitted is ended here (no-op otherwise)
            }
            else g_Session.EndFrameNoRender();
        }
    }

    PipeTrace::OnPresent();
    if (g_CapRequested.exchange(false) && bb) Capture(sc, dev, bb);
    if (bb) Burst(dev, bb);

    {
        std::lock_guard lk(g_StatusMutex);
        if (bb && g_Session.IsSessionRunning())
        {
            D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
            for (int i = 0; i < 2 && i < g_Session.EyeCount(); i++)
            {
                VRRuntime::Placement& pl = g_Place[i];
                pl.srcW = d.Width; pl.srcH = d.Height;
                g_Session.Placement(i, pl.rect, pl.swap);
                const XREyePose& e = g_Session.Eye(i);
                pl.fov[0] = e.fovLeft; pl.fov[1] = e.fovRight; pl.fov[2] = e.fovUp; pl.fov[3] = e.fovDown;
                pl.valid = pl.rect[2] > 0;
            }
        }
        g_Status.initialized = g_Session.IsInitialized();
        g_Status.running = g_Session.IsSessionRunning();
        g_Status.state = g_Session.SessionState();
        g_Status.framesSubmitted = g_Session.FramesSubmitted();
        g_Status.eyeW = g_Session.EyeWidth(); g_Status.eyeH = g_Session.EyeHeight();
        g_Status.runtime = g_Session.RuntimeName(); g_Status.system = g_Session.SystemName();
        g_Status.error = g_Session.LastError();
    }
    if (bb) bb->Release();
    dev->Release();
}

void VRRuntime::SetOrtho(bool on) { g_Ortho = on; LOGI("VRRuntime: flat frame placement {}", on ? "orthoscopic (true angular size)" : "1:1 pixels at the optical centre (old)"); }
bool VRRuntime::Ortho() { return g_Ortho.load(); }
void VRRuntime::SetTruePose(bool on) { g_TruePose = on; LOGI("VRRuntime: layer pose {}", on ? "= the pose each image was rendered from" : "= the head pose located at submit (old)"); }
bool VRRuntime::TruePose() { return g_TruePose.load(); }
void VRRuntime::SetPoseSource(int s) { g_PoseSource = s; LOGI("VRRuntime: render pose source {}", s); }
VRRuntime::RenderPoseStats VRRuntime::GetRenderPoseStats() { std::lock_guard lk(g_StatusMutex); return g_RP; }
unsigned long long VRRuntime::OrthoDraws() { return g_Session.OrthoDraws(); }
VRRuntime::Placement VRRuntime::GetPlacement(int i) { std::lock_guard lk(g_StatusMutex); return g_Place[i & 1]; }
VRRuntime::Status VRRuntime::GetStatus() { std::lock_guard lk(g_StatusMutex); return g_Status; }
VRRuntime::HeadPose VRRuntime::GetHeadPose() { std::lock_guard lk(g_PoseMutex); return g_Head; }
VRRuntime::HeadPose VRRuntime::GetEyePose(int i) { std::lock_guard lk(g_PoseMutex); return g_Eye[i & 1]; }
void VRRuntime::GetEyePoses(HeadPose eyes[2], HeadPose* head)
{
    std::lock_guard lk(g_PoseMutex);
    eyes[0] = g_Eye[0]; eyes[1] = g_Eye[1]; if (head) *head = g_Head;
}
bool VRRuntime::EyeRelative(int i, float q[4])
{
    std::lock_guard lk(g_PoseMutex);
    if (!g_Head.valid || !g_Eye[i & 1].valid) return false;
    VRMath::QConjMul(g_Head.quat, g_Eye[i & 1].quat, q);
    return true;
}
unsigned long long VRRuntime::PresentingSerial()
{   // as at the top of OnPresent (render thread: the reticle draws on that thread, just before it)
    return g_LatestAtPresent ? (unsigned long long)((long long)g_LatestAtPresent + g_PhaseOffset) : 0;
}
void VRRuntime::SetLayerFov(int m) { g_LayerFov = m; LOGI("VRRuntime: projection layer fov {}", m ? "= what each image was rendered with (margin included)" : "= the eye's native fov (mono on canted displays still declares its own)"); }
int VRRuntime::LayerFov() { return g_LayerFov.load(); }
VRRuntime::PoseSet VRRuntime::LocateForView(int kind)
{
    PoseSet ps; ps.kind = kind;
    {
        std::lock_guard lk(g_PoseMutex);
        ps.eye[0] = g_Eye[0]; ps.eye[1] = g_Eye[1]; ps.head = g_Head; ps.input = g_Input; ps.base = g_FrameD;
        ps.inputValid = g_InputValid && g_Session.IsSessionRunning() && GetTickCount64() - g_InputAt <= 500;
    }
    ps.valid = ps.head.valid; ps.time = ps.base;
    long long t = 0;
    bool ahead = false;
    if (ps.valid && PredictTime(kind, ps.base, t))
    {
        XREyePose e[2], hd; XRHandPose h[2];
        if (g_Session.LocateAt(t, e, h, &hd))
        {
            HeadFromEyes(e, hd, ps.head.gen, ps.eye, ps.head);
            ps.input.hand[0] = h[0]; ps.input.hand[1] = h[1]; ps.input.time = t;
            ps.time = t; ps.predicted = true; ahead = true;
        }
    }
    if (kind >= 0 && kind <= 2) { std::lock_guard lk(g_StatusMutex); if (ahead) g_SetsAhead[kind]++; else g_SetsLatest[kind]++; }
    return ps;
}
VRRuntime::HeadPose VRRuntime::GetServoHeadPose()
{
    const HeadPose h = GetHeadPose();
    if (!g_PredictServo.load() || !h.valid) return h;
    const int kind = StereoSync::Active() ? (StereoSync::PairWanted() ? 1 : 0) : g_Stereo.load() ? 2 : 0;
    long long base; { std::lock_guard lk(g_PoseMutex); base = g_FrameD; }
    long long t = 0;
    if (!PredictTime(kind, base, t)) { std::lock_guard lk(g_StatusMutex); g_ServoLatest++; return h; }
    if (g_ServoHeadValid && t == g_ServoT && g_ServoHead.gen == h.gen) return g_ServoHead;
    XREyePose e[2], hd; XRHandPose hh[2];
    if (!g_Session.LocateAt(t, e, hh, &hd)) { std::lock_guard lk(g_StatusMutex); g_ServoLatest++; return h; }
    HeadPose eyes[2], head; HeadFromEyes(e, hd, h.gen, eyes, head);
    g_ServoT = t; g_ServoHead = head; g_ServoHeadValid = true;
    { std::lock_guard lk(g_StatusMutex); g_ServoAhead++; }
    return head;
}
void VRRuntime::SetPredict(int m) { g_Predict = m; LOGI("VRRuntime: pose prediction {}", m == 0 ? "off (the poses of the last frame, as before)" : m == 1 ? "on: k display periods ahead" : "on: the learned gap as measured"); }
void VRRuntime::SetPredictServo(bool on) { g_PredictServo = on; LOGI("VRRuntime: the head-aim servo drives the game to the head {}", on ? "at the next view's display time" : "of the last frame (as before)"); }
void VRRuntime::SetPredictMax(float ms) { g_PredictMaxMs = ms; LOGI("VRRuntime: poses located at most {:.0f} ms ahead", ms); }
void VRRuntime::SetPhaseClosed(bool on) { g_PhaseClosed = on; LOGI("VRRuntime: eye phasing {}", on ? "closed loop (the TAA's view wins over the frame-order rule)" : "open loop (the frame-order rule only, as before)"); }
void VRRuntime::SetStereoHealth(bool on) { g_Health = on; LOGI("VRRuntime: stereo health check {}", on); }
void VRRuntime::SetUnpairedOwnEye(bool on) { g_UnpairedOwn = on ? 1 : 0; LOGI("VRRuntime: an unpaired stereo image goes {}", on ? "to its own eye only (the other keeps its last image)" : "to both eyes as mono (before)"); }
void VRRuntime::SetBackdropEyes(bool on) { g_Session.SetBackdropEyes(on); LOGI("VRRuntime: the menu screen's frozen backdrop: {}", on ? "each eye its own picture of the pair" : "the right eye's picture for both (before)"); }
void VRRuntime::SetMirrorEye(int m) { g_MirrorEye = m; LOGI("VRRuntime: desktop window in synchronised stereo: {}", m == 0 ? "the left eye" : "as presented (alternating eyes)"); }
std::string VRRuntime::PredictStatus()
{
    std::lock_guard lk(g_StatusMutex);
    std::string s = fmt::format("predict mode {} servo {} max {:.0f} ms period {:.2f} ms", g_Predict.load(), g_PredictServo.load() ? 1 : 0, g_PredictMaxMs.load(), (double)g_FramePeriod * 1e-6);
    const char* names[3] = { "mono", "pair", "alt" };
    for (int k = 0; k < 3; k++)
    {
        const GapStat& g = g_Gap[k];
        if (!g.n && !g_SetsAhead[k] && !g_SetsLatest[k]) continue;
        s += fmt::format(" | {} gap {:.1f} ms n {} err mean|.| {:.1f} last {:+.1f} ms sets ahead {} latest {}", names[k], g.ema * 1e-6, g.n,
                         g.errN ? g.errAbs / (double)g.errN * 1e-6 : 0.0, g.lastErr * 1e-6, g_SetsAhead[k], g_SetsLatest[k]);
    }
    s += fmt::format(" | servo ahead {} latest {} | phase loop {} offset {} fixes {} drops {} | health {} fallbacks {} fellback {} backoff {:.0f} resets {} | mirror {} {} | locates {}",
                     g_ServoAhead, g_ServoLatest, g_PhaseClosed.load() ? 1 : 0, g_PhaseOffset, g_PhaseFixes, g_PhaseDrops, g_Health.load() ? 1 : 0, g_Fallbacks,
                     g_FellBack ? 1 : 0, g_Backoff, g_BackoffResets, g_MirrorEye.load(), g_Mirrored, g_Session.LocatesAhead());
    return s;
}
void VRRuntime::SetPhaseOffset(int d) { std::lock_guard lk(g_StatusMutex); g_PhaseOffset = d; LOGI("cmd vr phaseoffset {} (test: the frame-order rule shifted by that many views)", d); }
void VRRuntime::NoteStereoCommand() { g_FellBack = false; g_Backoff = 10; g_GateOpenSince = 0; g_LastPairT = 0; g_StableSince = 0; }
int VRRuntime::PresentedEye() { return g_PresentedEye.load(); }
void VRRuntime::SetStereo(bool on) { g_Stereo = on; }
bool VRRuntime::Stereo() { return g_Stereo.load() || StereoSync::Active(); }   // either kind of stereo
int  VRRuntime::CurrentEye()
{
    if (StereoSync::Active()) return StereoSync::LastFrameHeld() ? 1 : 0;  // the eye of the frame being built
    return g_Stereo.load() ? g_CurEye.load() : -1;
}
std::string VRRuntime::SyncStatus()
{
    std::lock_guard lk(g_StatusMutex);
    return fmt::format("syncstereo staged {} stagefail {} pairs {} unpaired {} (right {}, own eye {} rule {}) mono {} leftalone {} dropped {} submitted {} backdropeyes {} | presented view by frame order, checked by its TAA: agree {} disagree {} no-taa {}",
                       g_Sync.staged, g_Sync.stageFailed, g_Sync.pairs, g_Sync.unpaired, g_Sync.unpairedRight, g_Sync.ownEye, g_UnpairedOwn.load(), g_Sync.mono, g_Sync.leftAlone, g_Sync.dropped,
                       g_Session.PairsSubmitted(), g_Session.BackdropEyes() ? 1 : 0, g_TaaAgree, g_TaaDisagree, g_TaaNone);
}
unsigned VRRuntime::RecenterEvents() { return g_Session.RecenterEvents(); }
unsigned long long VRRuntime::LostFrames(int dev) { return g_Session.LostFrames(dev); }
bool VRRuntime::GetInput(XRInputState& out)
{
    std::lock_guard lk(g_PoseMutex);
    // Stale = the runtime has given no frame for a while (stalled, or the session going away): the controllers' last
    // state must not keep driving the game (a stick held at that moment walked the player into the wall for good).
    if (!g_InputValid || !g_Session.IsSessionRunning() || GetTickCount64() - g_InputAt > 500) return false;
    out = g_Input; return true;
}

bool VRRuntime::Vibrate(int hand, float amplitude, float seconds, float hz)
{
    if (!Settings::Haptics()) return false;   // [Controls] Haptics = 0
    return g_Session.IsInitialized() && g_Session.Vibrate(hand, amplitude, seconds, hz);
}
unsigned long long VRRuntime::HapticPulses() { return g_Session.HapticPulses(); }

void VRRuntime::Shutdown(bool processExit)
{
    if (g_Session.IsInitialized()) g_Session.Shutdown(processExit);
}
void VRRuntime::SetKeepAlive(int m) { g_Session.SetKeepAlive(m); LOGI("cmd vr keepalive {} ({})", m, m ? "the headset gets the last picture while the game sends none" : "off: the headset waits (the hourglass)"); }
void VRRuntime::SetWaitBound(int ms) { g_Session.SetWaitBound(ms); LOGI("cmd vr xrwaitbound {} ms", ms); }
void VRRuntime::SetKick(float s, bool any) { g_Session.SetKick(s, any); LOGI("cmd vr xrkick {} s ({}{})", s, s > 0 ? "a session the runtime stopped and did not offer back is replaced by a new one" : "off", s > 0 ? (any ? ", on any runtime" : ", on Meta's runtime only") : ""); }
std::string VRRuntime::PumpStatus() { return g_Session.PumpStatus(); }
void VRRuntime::SetD3DProtect(bool on) { g_Session.SetD3DProtect(on); }
