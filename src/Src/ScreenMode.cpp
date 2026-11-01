#include "pch.h"
#include "ScreenMode.h"
#include "PassProfile.h"
#include "XRSession.h"
#include "KbmInject.h"
#include "WristHud.h"
#include "VRRuntime.h"
#include "Settings.h"
#include "Game.h"
#include "VRMath.h"
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <detours/detours.h>
#include "GameInfo.h"
#include "ViewMatrixHook.h"
#include <vector>

namespace
{
    constexpr float kD2R = 0.01745329f, kR2D = 57.29578f;
    std::atomic<int> g_Mode{ 1 };
    std::atomic<bool> g_Flat{ false }, g_Wheel{ false }, g_Lift{ false };
    std::atomic<bool> g_RecenterReq{ false };
    // [Display] StereoHud / HudDistance: in stereo, the game's interface (drawn into each eye's picture at the same
    // pixels) is taken out of the pictures and shown on ONE panel for both eyes (PlayHud). The same pixels in two
    // pictures are not the same direction: each eye's picture is rendered for its own frustum, turned towards that
    // frustum's centre (wider on the nose side), so a HUD element came out at two directions -- double, and diverging.
    std::atomic<bool> g_HudOn{ true };
    std::atomic<float> g_HudDist{ 2.f };
    // [Display] HudScale (2026-10-31, reported: interface elements too far out towards the edges of the view): the
    // panel covers this share of the picture's frustum, about its centre; the markers are placed for it (MarkerProjHook)
    std::atomic<float> g_HudScale{ 0.8f };
    // [Display] HudMarkers 1 (2026-10-31, reported: the alert markers in the right direction but at the HUD's depth, not
    // over the guards' heads): the markers the game placed this tick (MarkerProjHook: their HUD pixels and distance),
    // in batches -- one game tick's calls come together -- tagged with the newest main view then. PlayHud lifts each
    // one's square of the HUD (HudMarkerSize of its height) out of the HUD's panel onto a panel of its own at the
    // character's distance, in the same direction from the head: the same picture, at the right depth.
    struct MarkRec { float px = 0, py = 0, f = 0; int w = 0, h = 0; };
    struct MarkBatch { unsigned long long serial = 0; double t = 0; int n = 0; MarkRec m[16]; };
    constexpr int kMarkPanels = 6;
    std::mutex g_MkM; MarkBatch g_Mk[4]; int g_MkCur = -1; double g_MkLast = 0;
    std::atomic<float> g_MarkBox{ 0.07f };
    std::atomic<bool> g_MarkersOn{ true };
    std::atomic<unsigned long long> g_MarkPanels{ 0 };
    double NowSecs() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart; }
    void NoteMarker(unsigned long long serial, float px, float py, float f, int w, int h)
    {
        std::lock_guard lk(g_MkM);
        const double now = NowSecs();
        if (g_MkCur < 0 || now - g_MkLast > 0.003) { g_MkCur = (g_MkCur + 1) & 3; g_Mk[g_MkCur] = MarkBatch{}; g_Mk[g_MkCur].serial = serial; g_Mk[g_MkCur].t = now; }
        g_MkLast = now;
        MarkBatch& b = g_Mk[g_MkCur];
        if (b.n < 16) b.m[b.n++] = { px, py, f, w, h };
    }
    std::atomic<unsigned long long> g_HudPanels{ 0 };
    std::mutex g_M;                            // the config and the state below (render thread + the command thread)

    struct Cfg
    {
        float size = 62.f;          // deg: the screen's width seen from where it is placed
        float dist = 2.4f;          // m
        float pitchMin = -30.f, pitchMax = 15.f;   // deg: it is placed at the head's pitch within these
        float follow = 40.f;        // deg: the gaze this far from the screen's centre (its edges are at ~31)...
        float followDelay = 0.8f;   // s: ...this long, and it comes back in front (0 = never)
        float glide = 0.45f;        // s: how long a move takes
        float fadeIn = 0.22f;       // s: the screen and its surroundings in
        float fadeOut = 0.28f;      // s: the game back in from black after a menu
        float backdrop = 0.22f;     // brightness of the frozen game picture around it (0 = none)
        float env = 1.f;            // brightness of the environment
        float glow = 1.f;           // the screen's light spill on the surroundings
        float corner = 0.018f;      // corner radius, fraction of the screen's height
        float density = 28.f;       // px per degree of the screen's swapchain (sampled by the compositor at display resolution)
        int wheel = 1;              // the power wheel on a panel; 0 = left in the picture
        float wheelSize = 72.f;     // deg: width of the game's whole interface frame on the wheel's panel (the ring is ~40% of it)
        float wheelDist = 1.5f;     // m
        float wheelPitchMin = -40.f, wheelPitchMax = 30.f;
        float wheelFade = 0.12f;    // s
        int shopCrop = 1;           // a black market's screen: 0 = the band, 1 = from the band's top to the frame's bottom, 2 = all
        int enterFrames = 2;        // frames of menu mode before the screen comes up (a one-frame flicker is not a menu)
        int exitFrames = 2;         // frames of play before it goes
    } g_C;

    enum Kind { kPlay = 0, kScreen = 1, kWheel = 2 };
    const char* KindName(int k) { return k == kScreen ? "screen" : k == kWheel ? "wheel" : "play"; }
    Kind g_Kind = kPlay, g_PendKind = kPlay; int g_Pending = 0;
    int g_Scene = 0;                           // KbmInject's menu over the world (2 = a black market)
    float g_LastCrop[4] = {};                  // what of the picture the screen showed last (px)

    //! Where a panel is: the eye it was placed for, the direction and distance from that eye.
    struct Place { float eye[3] = {}; float yaw = 0, pitch = 0, d = 2.4f; bool valid = false; };
    Place g_Cur, g_From, g_To; float g_GlideT = -1.f;   // the screen, and its move (-1: not moving)
    Place g_WheelAt; float g_WheelAlpha = 0.f;
    float g_ScreenAlpha = 0.f, g_SnapFade = 1.f;
    bool g_FromPlay = false;                   // this screen came straight out of play (its frozen picture is the game a moment ago)
    float g_LastBackdrop = 0.f, g_ExitFrom = 0.f;   // the frozen picture's brightness on the last screen frame / where the game fades back in from
    double g_AwaySince = -1.0;                 // the gaze has been off the screen since (s)
    bool g_ShowedScreen = false; double g_GameSince = -100.0;
    double g_LastGame = -100.0;                // the last frame that showed the game (s)
    int g_FlatOnly = 0;                        // frames shown as the screen only because the picture was rendered flat
    void* g_Player = nullptr;
    unsigned g_RecenterGen = 0;
    double g_Last = 0.0;
    std::atomic<double> g_ExpectUntil{ 0.0 };   // a menu asked for by the controllers is expected until (s)
    std::string g_ExpectWhat;
    struct St { unsigned long long frames = 0, screenFrames = 0, flatOnly = 0, entries = 0, exits = 0, wheels = 0, wheelPanels = 0,
                recenters = 0, follows = 0, freezes = 0, noFreeze = 0, snapDropped = 0, expects = 0, expectMissed = 0; std::string lastWhy; } g_S;

    double Now() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart; }
    float Ease(float t) { t = std::clamp(t, 0.f, 1.f); return t * t * (3.f - 2.f * t); }
    float Wrap(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
    void QRot(const float q[4], const float v[3], float o[3])
    {
        const float u[3] = { q[0], q[1], q[2] }, w = q[3];
        const float c1[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        const float c2[3] = { u[1] * c1[2] - u[2] * c1[1], u[2] * c1[0] - u[0] * c1[2], u[0] * c1[1] - u[1] * c1[0] };
        for (int i = 0; i < 3; i++) o[i] = v[i] + 2 * (w * c1[i] + c2[i]);
    }
    //! XR direction (x right, y up, -z ahead) of a yaw (about +Y, + = left) and a pitch (+ = up).
    void Dir(float yaw, float pitch, float o[3]) { o[0] = -sinf(yaw) * cosf(pitch); o[1] = sinf(pitch); o[2] = -cosf(yaw) * cosf(pitch); }
    //! The quad's orientation: yaw, then pitch about its own X; its +Z faces back along the direction it is in.
    void Quat(float yaw, float pitch, float q[4])
    {
        const float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f), cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
        q[0] = cy * sp; q[1] = sy * cp; q[2] = -sy * sp; q[3] = cy * cp;
    }
    //! The head now: the eyes' midpoint, the head's own orientation (XRSession::Head: not the left eye's, which a
    //! headset with canted displays turns ~10 deg out).
    bool Head(XRSession& xr, float p[3], float q[4])
    {
        if (xr.EyeCount() < 1 || !xr.Eye(0).valid) return false;
        const XREyePose& h = xr.Head();
        p[0] = h.posX; p[1] = h.posY; p[2] = h.posZ;
        q[0] = h.quatX; q[1] = h.quatY; q[2] = h.quatZ; q[3] = h.quatW;
        return true;
    }
    //! In front of the head: its yaw, its pitch within [pmin, pmax], `dist` away.
    Place Ahead(const float p[3], const float q[4], float dist, float pmin, float pmax)
    {
        const float fl[3] = { 0, 0, -1 }; float f[3]; QRot(q, fl, f);
        Place o; memcpy(o.eye, p, 12);
        o.yaw = atan2f(-f[0], -f[2]);
        o.pitch = std::clamp(asinf(std::clamp(f[1], -1.f, 1.f)), pmin * kD2R, pmax * kD2R);
        o.d = dist; o.valid = true;
        return o;
    }
    void Centre(const Place& pl, float c[3]) { float d[3]; Dir(pl.yaw, pl.pitch, d); for (int i = 0; i < 3; i++) c[i] = pl.eye[i] + d[i] * pl.d; }
    Place Lerp(const Place& a, const Place& b, float t)
    {
        Place o; o.valid = true;
        for (int i = 0; i < 3; i++) o.eye[i] = a.eye[i] + (b.eye[i] - a.eye[i]) * t;
        o.yaw = a.yaw + Wrap(b.yaw - a.yaw) * t; o.pitch = a.pitch + (b.pitch - a.pitch) * t; o.d = a.d + (b.d - a.d) * t;
        return o;
    }
    void GlideTo(const Place& to, const char* why)
    {
        g_From = g_Cur; g_To = to; g_GlideT = 0.f; g_AwaySince = -1.0;
        LOGI("ScreenMode: screen moves in front ({}): yaw {:.0f} -> {:.0f}, pitch {:.0f} -> {:.0f} deg", why, g_Cur.yaw * kR2D, to.yaw * kR2D, g_Cur.pitch * kR2D, to.pitch * kR2D);
    }

    //! Where the game lays its menus out: a 16:9 band of the picture (measured on the title, pause, load, options and
    //! journal screens at 2266x2348: every item inside the centred band of height W*9/16).
    void Band(unsigned W, unsigned H, float r[4])
    {
        if ((float)H * 16.f >= (float)W * 9.f) { r[2] = (float)W; r[3] = (float)W * 9.f / 16.f; r[0] = 0.f; r[1] = ((float)H - r[3]) * 0.5f; }
        else { r[3] = (float)H; r[2] = (float)H * 16.f / 9.f; r[1] = 0.f; r[0] = ((float)W - r[2]) * 0.5f; }
    }

    //! What of the picture goes on the screen. A menu: the band. A black market lays its interface out against the
    //! frame's EDGES instead: its item carousel sits on the bottom edge of the 2266x2348 frame (rows
    //! ~2000-2280, measured), below the band, and the band alone showed the shopkeeper and no shop. So its screen runs
    //! from the band's top down to the frame's bottom (shopCrop 1): the same width, and so the same text size, as a menu.
    void Crop(unsigned W, unsigned H, float r[4])
    {
        Band(W, H, r);
        if (g_Scene != 2 || g_C.shopCrop == 0) return;
        if (g_C.shopCrop == 2) { r[0] = 0.f; r[1] = 0.f; r[2] = (float)W; r[3] = (float)H; }
        else r[3] = (float)H - r[1];
    }

    //! The kind the controls are in, once it has held for its frames.
    Kind Decide(XRSession& xr)
    {
        Kind want = kPlay;
        if (g_Mode.load() != 0 && xr.IsSessionRunning() && KbmInject::Enabled())
        {
            const KbmInject::Stats ks = KbmInject::GetStats();
            const int m = ks.mode;
            g_Scene = ks.scene;
            want = m == 1 ? kScreen : m == 2 ? kWheel : kPlay;
            // the controllers just sent the pause / journal key: the menu is coming, take it now
            const double until = g_ExpectUntil.load();
            if (until > 0.0)
            {
                if (m == 1) g_ExpectUntil = 0.0;                         // recognised: from here on as any menu
                else if (Now() < until) { want = kScreen; g_PendKind = kScreen; g_Pending = 1000; }
                else { g_ExpectUntil = 0.0; g_S.expectMissed++; LOGI("ScreenMode: no menu came after the {}: back to play", g_ExpectWhat); }
            }
        }
        if (want == g_Kind) { g_Pending = 0; return g_Kind; }
        if (want != g_PendKind) { g_PendKind = want; g_Pending = 0; }
        const int need = want == kScreen ? g_C.enterFrames : g_Kind == kScreen ? g_C.exitFrames : 0;
        return ++g_Pending > need ? want : g_Kind;
    }
}

bool ScreenMode::FlatRender() { return g_Flat.load(); }
bool ScreenMode::WheelView() { return g_Wheel.load(); }
bool ScreenMode::LiftUi() { return g_Lift.load(); }
void ScreenMode::Expect(const char* what)
{
    if (g_Mode.load() == 0 || g_Flat.load()) return;
    { std::lock_guard lk(g_M); g_ExpectWhat = what; g_S.expects++; }
    g_ExpectUntil = Now() + 0.7;
    LOGI("ScreenMode: the controllers asked for the {}: the screen comes up now", what);
}
void ScreenMode::RequestRecenter(const char* why) { if (g_Flat.load()) { g_RecenterReq = true; std::lock_guard lk(g_M); g_S.lastWhy = why; } }

bool ScreenMode::OnXrFrame(XRSession& xr, ID3D11Texture2D* bb, bool imageFlat, bool imageKnown)
{
    PassProfile::Scope prof(PassProfile::kSecScreen);   // ([Advanced] ProfileRenderPasses)
    std::lock_guard lk(g_M);
    const double now = Now();
    const float dt = g_Last > 0.0 ? (float)std::clamp(now - g_Last, 0.0, 0.1) : 0.f;
    g_Last = now;
    g_S.frames++;
    float hp[3] = {}, hq[4] = { 0, 0, 0, 1 };
    const bool head = Head(xr, hp, hq);

    // ---- the mode ----
    const Kind kind = Decide(xr);
    if (kind != g_Kind)
    {
        const Kind was = g_Kind;
        g_Kind = kind;
        LOGI("ScreenMode: {} -> {}", KindName(was), KindName(kind));
        if (kind == kScreen)
        {
            g_S.entries++;
            g_ScreenAlpha = 0.f; g_SnapFade = 1.f; g_GlideT = -1.f; g_AwaySince = -1.0;
            g_Cur = head ? Ahead(hp, hq, g_C.dist, g_C.pitchMin, g_C.pitchMax) : Place();
            g_RecenterGen = xr.RecenterGeneration();
            g_Player = Game::LocalPlayer();
            // the game, as it was a moment ago (the menu was already drawn over the last few frames before it was known)
            const bool fresh = now - g_LastGame < 1.0;
            const bool expected = g_ExpectUntil.load() > 0.0;
            // A menu over the world (the character choice, a black market) shows its interface a few frames before its
            // state's first tick says so (measured: the shop's item carousel was in the 0.3 s picture, a ghost of it
            // under the screen): the oldest picture in the ring there, the usual one if none is that old.
            const bool frozen = fresh && g_C.backdrop > 0.f &&
                ((!expected && g_Scene != 0 && xr.FreezeBackdrop(0.45f, 2.5f)) || xr.FreezeBackdrop(expected ? 0.f : 0.3f, 2.5f));
            if (frozen) g_S.freezes++; else g_S.noFreeze++;
            g_FromPlay = frozen;
            LOGI("ScreenMode: screen at yaw {:.0f} pitch {:.0f} deg, {:.1f} m, {:.0f} deg wide; backdrop {}", g_Cur.yaw * kR2D, g_Cur.pitch * kR2D,
                 g_C.dist, g_C.size, frozen ? fmt::format("the game {:.2f} s before", xr.BackdropAge()) : fresh ? "none (no picture to freeze)" : "none (not coming from play)");
        }
        if (was == kScreen) { g_S.exits++; xr.UnfreezeBackdrop(); }
        if (kind == kWheel)
        {
            g_S.wheels++; g_WheelAlpha = 0.f;
            g_WheelAt = head ? Ahead(hp, hq, g_C.wheelDist, g_C.wheelPitchMin, g_C.wheelPitchMax) : Place();
        }
    }
    unsigned wtw = 0, wth = 0;
    bool wgamma = false;
    ID3D11ShaderResourceView* wsrv = WristHud::Texture(wtw, wth, &wgamma);
    g_Flat = g_Kind == kScreen;
    g_Wheel = g_Kind == kWheel;
    g_Lift = (g_Kind == kWheel && g_C.wheel != 0 && wsrv && wtw && wth)
          || (g_HudOn.load() && g_Kind == kPlay && g_Mode.load() && VRRuntime::Stereo() && wsrv && wtw && wth);   // PlayHud

    // ---- what this frame shows ----
    // A picture rendered flat is shown on the screen even once play has resumed (the pipeline is deeper than one
    // Present): across the eyes it would be the flat game's view at the headset's angles for a frame or two.
    bool screen = g_Kind == kScreen;
    if (!screen && imageKnown && imageFlat && g_ShowedScreen && g_FlatOnly < 8) { screen = true; g_FlatOnly++; g_S.flatOnly++; }
    if (!screen) g_FlatOnly = 0;
    if (screen && bb && head)
    {
        // the screen: placed, moved in front on request / after looking away for a while, gliding when it moves
        if (!g_Cur.valid || xr.RecenterGeneration() != g_RecenterGen)
        {
            g_Cur = Ahead(hp, hq, g_C.dist, g_C.pitchMin, g_C.pitchMax); g_GlideT = -1.f; g_RecenterGen = xr.RecenterGeneration();
        }
        if (g_RecenterReq.exchange(false))
        {
            g_S.recenters++;
            GlideTo(Ahead(hp, hq, g_C.dist, g_C.pitchMin, g_C.pitchMax), g_S.lastWhy.empty() ? "request" : g_S.lastWhy.c_str());
            if (Settings::Haptics()) { xr.Vibrate(0, 0.25f, 0.02f); xr.Vibrate(1, 0.25f, 0.02f); }
        }
        float c[3]; Centre(g_Cur, c);
        {
            const float fl[3] = { 0, 0, -1 }; float f[3]; QRot(hq, fl, f);
            float v[3] = { c[0] - hp[0], c[1] - hp[1], c[2] - hp[2] };
            const float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            const float off = n > 1e-4f ? acosf(std::clamp((v[0] * f[0] + v[1] * f[1] + v[2] * f[2]) / n, -1.f, 1.f)) * kR2D : 0.f;
            if (g_GlideT < 0.f && g_C.followDelay > 0.f && off > g_C.follow)
            {
                if (g_AwaySince < 0.0) g_AwaySince = now;
                else if (now - g_AwaySince > g_C.followDelay) { g_S.follows++; GlideTo(Ahead(hp, hq, g_C.dist, g_C.pitchMin, g_C.pitchMax), "out of sight"); }
            }
            else g_AwaySince = -1.0;
        }
        if (g_GlideT >= 0.f)
        {
            g_GlideT += g_C.glide > 0.f ? dt / g_C.glide : 1.f;
            g_Cur = g_GlideT >= 1.f ? g_To : Lerp(g_From, g_To, Ease(g_GlideT));
            if (g_GlideT >= 1.f) g_GlideT = -1.f;
            Centre(g_Cur, c);
        }
        g_ScreenAlpha = g_C.fadeIn > 0.f ? std::min(1.f, g_ScreenAlpha + dt / g_C.fadeIn) : 1.f;
        const float a = Ease(g_ScreenAlpha);
        // A load builds a new player: the frozen picture is of the place left behind. Let it go.
        if (g_Player != Game::LocalPlayer() && g_SnapFade > 0.f && xr.BackdropFrozen())
        {
            if (g_SnapFade >= 1.f) { g_S.snapDropped++; LOGI("ScreenMode: a new player (a load): the frozen picture fades out"); }
            g_SnapFade = std::max(0.f, g_SnapFade - dt / 0.5f);
        }

        D3D11_TEXTURE2D_DESC bd = {}; bb->GetDesc(&bd);
        XRPanelLayer l;
        l.tex = bb; l.srcW = bd.Width; l.srcH = bd.Height;
        Crop(bd.Width, bd.Height, l.crop);
        memcpy(g_LastCrop, l.crop, sizeof g_LastCrop);
        l.premultiplied = false;
        l.alpha = a;
        l.corner = g_C.corner; l.soft = 0.0025f;
        const float aspect = l.crop[3] > 0.f ? l.crop[2] / l.crop[3] : 16.f / 9.f;
        const float grow = 0.965f + 0.035f * a;          // it opens out a little as it fades in
        l.width = 2.f * g_C.dist * tanf(g_C.size * 0.5f * kD2R) * grow; l.height = l.width / aspect;
        Quat(g_Cur.yaw, g_Cur.pitch, l.quat);
        memcpy(l.pos, c, 12);
        l.outW = (unsigned)std::clamp(std::min(l.crop[2], g_C.size * g_C.density), 256.f, 4096.f);
        l.outH = (unsigned)std::clamp(l.outW / aspect, 144.f, 4096.f);
        xr.SetPanelLayer(0, l);
        XRBackdrop b;
        if (g_FromPlay && xr.BackdropFrozen())
        {   // Out of play: the frozen picture IS the game a moment ago, so it starts at full brightness and colour and
            // dims down while the screen comes up -- the world stops and darkens, no cut to black.
            b.snapshot = (1.f + (g_C.backdrop - 1.f) * a) * g_SnapFade; b.desat = 0.35f * a; b.env = g_C.env * a; b.fade = 1.f;
        }
        else { b.snapshot = g_C.backdrop * g_SnapFade; b.env = g_C.env; b.fade = a; }
        g_LastBackdrop = xr.BackdropFrozen() ? b.snapshot : 0.f;
        for (int i = 0; i < 3; i++) b.glowDir[i] = c[i] - hp[i];
        { const float n = sqrtf(b.glowDir[0] * b.glowDir[0] + b.glowDir[1] * b.glowDir[1] + b.glowDir[2] * b.glowDir[2]); if (n > 1e-4f) for (float& x : b.glowDir) x /= n; }
        b.glow = g_C.glow * a;
        xr.SubmitScreen(b);
        g_S.screenFrames++;
        g_ShowedScreen = true;
        return true;
    }

    // ---- the game ----
    if (g_ShowedScreen)
    {   // back from the screen: both eyes start over from this picture, faded in from black
        g_ShowedScreen = false; g_GameSince = now;
        g_ExitFrom = std::clamp(g_LastBackdrop, 0.f, 1.f);   // from the darkened frozen world, or from black
        xr.ResyncEyes();
    }
    g_LastGame = now;
    if (g_C.fadeOut > 0.f && now - g_GameSince < g_C.fadeOut) xr.SetFrameFade(g_ExitFrom + (1.f - g_ExitFrom) * Ease((float)((now - g_GameSince) / g_C.fadeOut)));
    if (g_Lift.load() && head)
    {   // the wheel: the game's interface on a panel where it was opened
        if (!g_WheelAt.valid) g_WheelAt = Ahead(hp, hq, g_C.wheelDist, g_C.wheelPitchMin, g_C.wheelPitchMax);
        g_WheelAlpha = g_C.wheelFade > 0.f ? std::min(1.f, g_WheelAlpha + dt / g_C.wheelFade) : 1.f;
        XRPanelLayer l;
        l.srv = wsrv; l.srcW = wtw; l.srcH = wth;
        l.crop[0] = 0.f; l.crop[1] = 0.f; l.crop[2] = (float)wtw; l.crop[3] = (float)wth;
        l.premultiplied = true; l.gamma = wgamma;
        l.alpha = Ease(g_WheelAlpha);
        l.corner = 0.f; l.soft = 0.12f;                  // the interface's own dark veil fades out at the panel's edge
        const float aspect = (float)wtw / (float)wth;
        l.width = 2.f * g_C.wheelDist * tanf(g_C.wheelSize * 0.5f * kD2R); l.height = l.width / aspect;
        Quat(g_WheelAt.yaw, g_WheelAt.pitch, l.quat);
        Centre(g_WheelAt, l.pos);
        l.outW = (unsigned)std::clamp(std::min((float)wtw, g_C.wheelSize * g_C.density), 256.f, 4096.f);
        l.outH = (unsigned)std::clamp(l.outW / aspect, 144.f, 4096.f);
        xr.SetPanelLayer(1, l);
        g_S.wheelPanels++;
    }
    else if (g_Kind != kWheel) g_WheelAt.valid = false;
    return false;
}

// ---- each marker's whole drawing: its icon AND its label (2026-11-01) ----
// Reported (the Heart's runes and bone charms): the label did not line up with the marker, parts of it seemed at
// different depths, words cut in the middle unless the head was just so. Only a fixed square (HudMarkerSize) round
// each marker's point was lifted to its distance; a label wider than that was split -- its middle at the marker's
// distance, its ends left on the HUD's layer at HudDistance. Now the interface texture is reduced on the GPU to a
// mask of 8x8-pixel cells holding anything (alpha), read back a couple of frames later, and from each marker's square
// the box grows over its neighbouring cells (across gaps up to about a letter's spacing, within a window round the
// point) until it holds all of the marker's drawing. That box (relative to the marker's point) is kept for the
// marker -- matched by place and distance -- and lifted whole; overlapping boxes are lifted together, at the nearer
// one's distance.
namespace
{
    const char* kMaskHlsl = R"(
Texture2D<float4> Hud : register(t0);
RWTexture2D<uint> Mask : register(u0);
cbuffer C : register(b0) { uint4 sz; };   // texture w, h; cells w, h
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= sz.z || id.y >= sz.w) return;
    float a = 0;
    uint2 b = id.xy * 8;
    [unroll] for (uint y = 0; y < 4; y++)
        [unroll] for (uint x = 0; x < 4; x++)
            a = max(a, Hud[min(b + uint2(1 + 2 * x, 1 + 2 * y), sz.xy - 1)].a);
    Mask[id.xy] = a > 0.06 ? 1 : 0;
}
)";
    constexpr int kCell = 8, kStages = 3;
    struct ShapeRec { float cx = 0, cy = 0, f = 0; };                 // a marker's point in the HUD texture (px), its distance
    struct Shape { float cx = 0, cy = 0, f = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0; double t = 0; };   // its box, relative to the point (px)
    struct Stage { ID3D11Texture2D* tex = nullptr; bool pending = false; int n = 0; ShapeRec m[16]; unsigned tw = 0, th = 0; double t = 0; };
    struct MaskGpu
    {
        bool tried = false, ok = false;
        ID3D11ComputeShader* cs = nullptr; ID3D11Buffer* cb = nullptr;
        ID3D11Texture2D* mask = nullptr; ID3D11UnorderedAccessView* uav = nullptr;
        unsigned tw = 0, th = 0, cw = 0, ch = 0;
        Stage st[kStages]; int next = 0;
        unsigned long long lastSerial = 0; double lastAt = 0;
    } g_MG;
    std::vector<Shape> g_Shapes;
    std::vector<int> g_Integral;
    std::atomic<unsigned long long> g_ShapesMeasured{ 0 }, g_ShapesGrown{ 0 }, g_MarkMerged{ 0 };
    float g_PanelW = 0, g_PanelH = 0; double g_PanelShrinkSince = 0;   // the markers' panels' swapchain size (shared)

    template <class T> void RelP(T*& p) { if (p) { p->Release(); p = nullptr; } }
    bool MaskInit(ID3D11Device* dev)
    {
        if (g_MG.tried) return g_MG.ok;
        g_MG.tried = true;
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
        ID3DBlob* b = nullptr; ID3DBlob* err = nullptr;
        if (!compile || FAILED(compile(kMaskHlsl, strlen(kMaskHlsl), "hudmask", nullptr, nullptr, "main", "cs_5_0", 0, 0, &b, &err)))
        {
            LOGI("ScreenMode: the markers' mask shader is not available ({}): each marker lifts its square only",
                 err ? (const char*)err->GetBufferPointer() : "no d3dcompiler_47");
            RelP(err); return false;
        }
        RelP(err);
        const bool okCs = SUCCEEDED(dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_MG.cs));
        b->Release();
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (!okCs || FAILED(dev->CreateBuffer(&bd, nullptr, &g_MG.cb))) { LOGI("ScreenMode: the markers' mask: no shader / buffer"); return false; }
        g_MG.ok = true;
        return true;
    }
    bool MaskSize(ID3D11Device* dev, unsigned tw, unsigned th)
    {
        if (g_MG.mask && g_MG.tw == tw && g_MG.th == th) return true;
        RelP(g_MG.uav); RelP(g_MG.mask);
        for (Stage& s : g_MG.st) { RelP(s.tex); s = Stage{}; }
        g_MG.tw = tw; g_MG.th = th; g_MG.cw = (tw + kCell - 1) / kCell; g_MG.ch = (th + kCell - 1) / kCell;
        D3D11_TEXTURE2D_DESC td{}; td.Width = g_MG.cw; td.Height = g_MG.ch; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8_UINT;
        td.SampleDesc = { 1, 0 }; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_MG.mask)) || FAILED(dev->CreateUnorderedAccessView(g_MG.mask, nullptr, &g_MG.uav))) { RelP(g_MG.mask); return false; }
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (Stage& s : g_MG.st) if (FAILED(dev->CreateTexture2D(&td, nullptr, &s.tex))) { RelP(g_MG.uav); RelP(g_MG.mask); return false; }
        LOGI("ScreenMode: the markers' mask: {}x{} cells of the {}x{} interface (each marker's box grows to hold its label)", g_MG.cw, g_MG.ch, tw, th);
        return true;
    }
    //! One read-back mask: each of its frame's markers' boxes grown over the occupied cells round it
    void MaskMeasure(const uint8_t* data, UINT pitch, const Stage& s, float square)
    {
        const int cw = (int)g_MG.cw, ch = (int)g_MG.ch;
        g_Integral.assign((size_t)(cw + 1) * (ch + 1), 0);
        auto I = [&](int x, int y) -> int& { return g_Integral[(size_t)y * (cw + 1) + x]; };
        for (int y = 0; y < ch; y++)
        {
            int row = 0; const uint8_t* r = data + (size_t)y * pitch;
            for (int x = 0; x < cw; x++) { row += r[x] ? 1 : 0; I(x + 1, y + 1) = I(x + 1, y) + row; }
        }
        // occupied cells in [x0, x1] x [y0, y1] (inclusive, clamped)
        auto count = [&](int x0, int y0, int x1, int y1) -> int
        {
            x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, cw - 1); y1 = std::min(y1, ch - 1);
            if (x1 < x0 || y1 < y0) return 0;
            return I(x1 + 1, y1 + 1) - I(x0, y1 + 1) - I(x1 + 1, y0) + I(x0, y0);
        };
        const float th = (float)s.th;
        const int gh = std::max(2, (int)std::lround(0.011f * th / kCell)), gv = std::max(2, (int)std::lround(0.009f * th / kCell));   // gaps crossed
        const int wx = (int)(0.2f * th / kCell), wy = (int)(0.08f * th / kCell);                                                        // how far it may grow
        const double now = NowSecs();
        for (int i = 0; i < s.n; i++)
        {
            const ShapeRec& m = s.m[i];
            const int cx = (int)(m.cx / kCell), cy = (int)(m.cy / kCell), hs = std::max(1, (int)(0.5f * square / kCell));
            const int lx0 = cx - wx, lx1 = cx + wx, ly0 = cy - wy, ly1 = cy + wy;
            int x0 = cx - hs, x1 = cx + hs, y0 = cy - hs, y1 = cy + hs;
            if (count(x0, y0, x1, y1) == 0) continue;   // nothing drawn there (yet)
            for (int it = 0; it < 96; it++)
            {
                bool grew = false;
                for (int c = std::min(x1 + gh, lx1); c > x1; c--) if (count(c, y0, c, y1)) { x1 = c; grew = true; break; }
                for (int c = std::max(x0 - gh, lx0); c < x0; c++) if (count(c, y0, c, y1)) { x0 = c; grew = true; break; }
                for (int r = std::min(y1 + gv, ly1); r > y1; r--) if (count(x0, r, x1, r)) { y1 = r; grew = true; break; }
                for (int r = std::max(y0 - gv, ly0); r < y0; r++) if (count(x0, r, x1, r)) { y0 = r; grew = true; break; }
                if (!grew) break;
            }
            // tightened to what is drawn, a cell's margin, never less than the square
            int tx0 = x0, tx1 = x1, ty0 = y0, ty1 = y1;
            while (tx0 < tx1 && !count(tx0, y0, tx0, y1)) tx0++;
            while (tx1 > tx0 && !count(tx1, y0, tx1, y1)) tx1--;
            while (ty0 < ty1 && !count(tx0, ty0, tx1, ty0)) ty0++;
            while (ty1 > ty0 && !count(tx0, ty1, tx1, ty1)) ty1--;
            Shape sh; sh.cx = m.cx; sh.cy = m.cy; sh.f = m.f; sh.t = now;
            sh.x0 = std::min((float)(tx0 - 1) * kCell - m.cx, -0.5f * square); sh.x1 = std::max((float)(tx1 + 2) * kCell - m.cx, 0.5f * square);
            sh.y0 = std::min((float)(ty0 - 1) * kCell - m.cy, -0.5f * square); sh.y1 = std::max((float)(ty1 + 2) * kCell - m.cy, 0.5f * square);
            g_ShapesMeasured++;
            if (sh.x1 - sh.x0 > square * 1.1f || sh.y1 - sh.y0 > square * 1.1f) g_ShapesGrown++;
            // replaces the one kept for this marker (nearby, about as far)
            bool put = false;
            for (Shape& o : g_Shapes)
                if (fabsf(o.cx - sh.cx) < 0.05f * th && fabsf(o.cy - sh.cy) < 0.05f * th && fabsf(logf(o.f / sh.f)) < 0.3f) { o = sh; put = true; break; }
            if (!put) g_Shapes.push_back(sh);
        }
        g_Shapes.erase(std::remove_if(g_Shapes.begin(), g_Shapes.end(), [&](const Shape& o) { return now - o.t > 0.6; }), g_Shapes.end());
        if (g_Shapes.size() > 48) g_Shapes.erase(g_Shapes.begin(), g_Shapes.begin() + (g_Shapes.size() - 48));
    }
    //! The interface's mask for this frame's markers (once per frame), and any earlier one that is back from the GPU
    void MaskFrame(ID3D11ShaderResourceView* srv, unsigned tw, unsigned th, const ShapeRec* ms, int n, unsigned long long serial, float square)
    {
        ID3D11Device* dev = nullptr; srv->GetDevice(&dev);
        if (!dev) return;
        ID3D11DeviceContext* c = nullptr; dev->GetImmediateContext(&c);
        if (c && MaskInit(dev) && MaskSize(dev, tw, th))
        {
            for (int k = 0; k < kStages; k++)
            {   // the oldest first
                Stage& s = g_MG.st[(g_MG.next + k) % kStages];
                if (!s.pending) continue;
                D3D11_MAPPED_SUBRESOURCE m{};
                const HRESULT hr = c->Map(s.tex, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
                if (SUCCEEDED(hr)) { if (s.tw == tw && s.th == th) MaskMeasure((const uint8_t*)m.pData, m.RowPitch, s, square); c->Unmap(s.tex, 0); s.pending = false; }
                else if (hr != DXGI_ERROR_WAS_STILL_DRAWING || NowSecs() - s.t > 1.0) s.pending = false;
            }
            Stage& s = g_MG.st[g_MG.next];
            if (n > 0 && !s.pending && serial != g_MG.lastSerial && NowSecs() - g_MG.lastAt > 0.03)   // ~30 times a second: the labels' shapes hold still
            {
                g_MG.lastSerial = serial; g_MG.lastAt = NowSecs();
                const UINT k[4] = { tw, th, g_MG.cw, g_MG.ch };
                c->UpdateSubresource(g_MG.cb, 0, nullptr, k, 0, 0);
                // the compute stage's state as it was
                ID3D11ComputeShader* oc = nullptr; ID3D11Buffer* ob = nullptr; ID3D11ShaderResourceView* os = nullptr; ID3D11UnorderedAccessView* ou = nullptr;
                c->CSGetShader(&oc, nullptr, nullptr); c->CSGetConstantBuffers(0, 1, &ob); c->CSGetShaderResources(0, 1, &os); c->CSGetUnorderedAccessViews(0, 1, &ou);
                c->CSSetShader(g_MG.cs, nullptr, 0); c->CSSetConstantBuffers(0, 1, &g_MG.cb); c->CSSetShaderResources(0, 1, &srv);
                c->CSSetUnorderedAccessViews(0, 1, &g_MG.uav, nullptr);
                c->Dispatch((g_MG.cw + 7) / 8, (g_MG.ch + 7) / 8, 1);
                ID3D11UnorderedAccessView* nu = nullptr; ID3D11ShaderResourceView* ns = nullptr;
                c->CSSetUnorderedAccessViews(0, 1, &nu, nullptr); c->CSSetShaderResources(0, 1, &ns);
                const UINT keep = (UINT)-1;
                c->CSSetShader(oc, nullptr, 0); c->CSSetConstantBuffers(0, 1, &ob); c->CSSetShaderResources(0, 1, &os); c->CSSetUnorderedAccessViews(0, 1, &ou, &keep);
                RelP(oc); RelP(ob); RelP(os); RelP(ou);
                c->CopyResource(s.tex, g_MG.mask);
                s.pending = true; s.n = std::min(n, 16); memcpy(s.m, ms, sizeof(ShapeRec) * s.n); s.tw = tw; s.th = th; s.t = NowSecs();
                g_MG.next = (g_MG.next + 1) % kStages;
            }
        }
        if (c) c->Release();
        dev->Release();
    }
    //! The kept box for a marker at (cx, cy) about f m away; false = none yet (its square)
    bool ShapeOf(float cx, float cy, float f, float th, Shape& out)
    {
        const Shape* best = nullptr; float bd = 1e30f;
        for (const Shape& o : g_Shapes)
        {
            if (fabsf(logf(o.f / f)) > 0.3f) continue;
            const float d = (o.cx - cx) * (o.cx - cx) + (o.cy - cy) * (o.cy - cy);
            if (d < bd && d < 0.06f * th * 0.06f * th) { bd = d; best = &o; }
        }
        if (best) out = *best;
        return best != nullptr;
    }
}

void ScreenMode::PlayHud(XRSession& xr, const float quat[4], const float pos[3], const float tan[4], unsigned long long serial)
{
    PassProfile::Scope prof(PassProfile::kSecScreen);   // ([Advanced] ProfileRenderPasses)
    std::lock_guard lk(g_M);
    if (!g_Lift.load() || g_Kind != kPlay) return;
    unsigned tw = 0, th = 0; bool gamma = false;
    ID3D11ShaderResourceView* srv = WristHud::Texture(tw, th, &gamma);
    if (!srv || !tw || !th) return;
    const float d = g_HudDist.load();
    const float l = tan[0], r = tan[1], u = tan[2], dn = tan[3];
    if (!(r > l) || !(u > dn)) return;
    XRPanelLayer p;
    p.srv = srv; p.srcW = tw; p.srcH = th;
    p.crop[0] = 0.f; p.crop[1] = 0.f; p.crop[2] = (float)tw; p.crop[3] = (float)th;
    p.premultiplied = true; p.gamma = gamma; p.alpha = 1.f; p.corner = 0.f; p.soft = 0.f;
    // the plane the picture was rendered on, at d: the interface's pixels are where the picture's are -- shrunk about
    // its centre by HudScale, so what the game puts at the screen's edges is not at the edge of the lenses
    const float hs = g_HudScale.load();
    p.width = d * (r - l) * hs; p.height = d * (u - dn) * hs;
    const float c[3] = { d * (r + l) * 0.5f, d * (u + dn) * 0.5f, -d };
    float w[3]; VRMath::QRotate(quat, c, w);
    for (int i = 0; i < 3; i++) p.pos[i] = pos[i] + w[i];
    memcpy(p.quat, quat, 16);
    p.outW = std::min(tw, 4096u); p.outH = std::min(th, 4096u);
    if (g_MarkersOn.load() && g_MarkBox.load() > 0.f)
    {   // the markers at their characters' distance: the batch of the tick this picture was drawn from (the newest one
        // made no later than its view), each marker's square of the HUD on a panel of its own, left out of the HUD's
        MarkRec ms[16]; int n = 0;
        {
            std::lock_guard mk(g_MkM);
            const double now = NowSecs();
            const MarkBatch* best = nullptr;
            for (const MarkBatch& b : g_Mk)
            {
                if (!b.n || now - b.t > 0.25 || (serial && b.serial > serial)) continue;
                if (!best || b.serial > best->serial || (b.serial == best->serial && b.t > best->t)) best = &b;
            }
            if (best) { n = best->n; memcpy(ms, best->m, sizeof(MarkRec) * n); }
        }
        std::sort(ms, ms + n, [](const MarkRec& a, const MarkRec& b) { return a.f < b.f; });
        const float S = g_MarkBox.load() * (float)th;
        // each marker's box: its whole drawing, as last measured (MaskMeasure), else its square; overlapping ones together
        struct Box { float x0, y0, x1, y1, f; };
        Box bx[16]; int nb = 0;
        ShapeRec recs[16]; int nr = 0;
        for (int i = 0; i < n; i++)
        {
            const MarkRec& m = ms[i];
            if (m.w <= 0 || m.h <= 0 || !(m.f > 0.5f) || m.f > 500.f) continue;
            const float cx = m.px * (float)tw / (float)m.w, cy = m.py * (float)th / (float)m.h;
            {   // the square at the HUD's edge (clamped there: a direction): left on the HUD
                const float x0 = std::max(0.f, cx - S * 0.5f), y0 = std::max(0.f, cy - S * 0.5f);
                const float x1 = std::min((float)tw, cx + S * 0.5f), y1 = std::min((float)th, cy + S * 0.5f);
                if (x1 - x0 < S * 0.5f || y1 - y0 < S * 0.5f) continue;
            }
            recs[nr++] = { cx, cy, m.f };
            Shape sh;
            Box b = ShapeOf(cx, cy, m.f, (float)th, sh) ? Box{ cx + sh.x0, cy + sh.y0, cx + sh.x1, cy + sh.y1, m.f }
                                                         : Box{ cx - S * 0.5f, cy - S * 0.5f, cx + S * 0.5f, cy + S * 0.5f, m.f };
            b.x0 = std::max(0.f, b.x0); b.y0 = std::max(0.f, b.y0); b.x1 = std::min((float)tw, b.x1); b.y1 = std::min((float)th, b.y1);
            bx[nb++] = b;
        }
        if (nr > 0 || g_MG.st[0].pending || g_MG.st[1].pending || g_MG.st[2].pending) MaskFrame(srv, tw, th, recs, nr, serial, S);
        for (bool merged = true; merged;)
        {   // overlapping boxes: one, at the nearer one's distance (they are sorted nearest first)
            merged = false;
            for (int a = 0; a < nb && !merged; a++)
                for (int b = a + 1; b < nb && !merged; b++)
                    if (bx[a].x0 < bx[b].x1 && bx[b].x0 < bx[a].x1 && bx[a].y0 < bx[b].y1 && bx[b].y0 < bx[a].y1)
                    {
                        bx[a] = { std::min(bx[a].x0, bx[b].x0), std::min(bx[a].y0, bx[b].y0), std::max(bx[a].x1, bx[b].x1), std::max(bx[a].y1, bx[b].y1), std::min(bx[a].f, bx[b].f) };
                        for (int k = b; k + 1 < nb; k++) bx[k] = bx[k + 1];
                        nb--; merged = true; g_MarkMerged++;
                    }
        }
        // the panels' swapchain size: the largest box's (in 64 px steps), one size for all of them so a panel slot is
        // not made again as markers change places; shrunk only after a while
        {
            float need[2] = { 128.f, 128.f };
            for (int i = 0; i < std::min(nb, kMarkPanels); i++) { need[0] = std::max(need[0], bx[i].x1 - bx[i].x0); need[1] = std::max(need[1], bx[i].y1 - bx[i].y0); }
            const float nw = std::min(2048.f, ceilf(need[0] / 64.f) * 64.f), nh = std::min(2048.f, ceilf(need[1] / 64.f) * 64.f);
            const double now = NowSecs();
            if (nw > g_PanelW || nh > g_PanelH) { g_PanelW = std::max(g_PanelW, nw); g_PanelH = std::max(g_PanelH, nh); g_PanelShrinkSince = 0; }
            else if (nb && (nw < 0.7f * g_PanelW || nh < 0.7f * g_PanelH))
            {
                if (g_PanelShrinkSince <= 0) g_PanelShrinkSince = now;
                else if (now - g_PanelShrinkSince > 3.0) { g_PanelW = nw; g_PanelH = nh; g_PanelShrinkSince = 0; }
            }
            else g_PanelShrinkSince = 0;
        }
        int used = 0;
        for (int i = 0; i < nb && used < kMarkPanels; i++)
        {
            const Box& m = bx[i];
            const float x0 = m.x0, y0 = m.y0, x1 = m.x1, y1 = m.y1;
            if (x1 - x0 < 4.f || y1 - y0 < 4.f) continue;
            p.holes[used][0] = x0; p.holes[used][1] = y0; p.holes[used][2] = x1; p.holes[used][3] = y1;
            XRPanelLayer mp = p;
            mp.nHoles = 0;
            mp.crop[0] = x0; mp.crop[1] = y0; mp.crop[2] = x1 - x0; mp.crop[3] = y1 - y0;
            // the box's centre as a direction from the head (the HUD's panel: its frustum shrunk by HudScale), out to
            // the character's distance (the game's metres)
            const float mx = 0.5f * (x0 + x1), my = 0.5f * (y0 + y1);
            const float tx = 0.5f * (r + l) + (mx / (float)tw - 0.5f) * (r - l) * hs;
            const float ty = 0.5f * (u + dn) - (my / (float)th - 0.5f) * (u - dn) * hs;
            const float v[3] = { tx * m.f, ty * m.f, -m.f };
            float wv[3]; VRMath::QRotate(quat, v, wv);
            for (int k = 0; k < 3; k++) mp.pos[k] = pos[k] + wv[k];
            mp.width = m.f * (x1 - x0) / (float)tw * (r - l) * hs;
            mp.height = m.f * (y1 - y0) / (float)th * (u - dn) * hs;
            mp.outW = (unsigned)g_PanelW; mp.outH = (unsigned)g_PanelH;
            xr.SetPanelLayer(2 + used, mp);
            used++;
        }
        p.nHoles = used;
        if (used) g_MarkPanels++;
    }
    xr.SetPanelLayer(1, p);
    g_HudPanels++;
}

// ---- the markers over characters and objectives ([Display] HudMarkers, 2026-10-31) ----
// Reported: a guard's awareness marker did not sit over his head. The markers (arkEntityMarkerManager: awareness,
// objectives, runes...) are placed by the game thread: the marker's world point through exe+0x6DDA50 (the game camera's
// view-projection at camera+0x3EB0, exe+0x88D970, then to the HUD's pixels with its size at [exe+0x39A5290]+0x340EA8).
// That is the flat game's camera -- its own fov and aspect -- while the HUD is shown over the headset's picture: on
// its panel (StereoHud), the plane of the frustum the view was rendered with (its tangents), in the head's frame.
// So the point is projected again here the way the panel shows it: from the latest main view's origin, in the head's
// axis (no eye turn), through that view's tangents. The game's own clamping to the screen's edge runs on the result.
namespace
{
    using MarkerProj_t = void* (*)(void* cam, const float* world, float* out);
    constexpr uintptr_t kMarkerProj = 0x6DDA50, kHudGlobal = 0x39A5290, kHudSize = 0x340EA8;
    MarkerProj_t g_RealMarkerProj = nullptr;
    std::atomic<unsigned long long> g_MarkersFixed{ 0 }, g_MarkersKept{ 0 };
    std::atomic<int> g_MarkerLogs{ 0 };
    std::atomic<double> g_MarkerLogAt{ 0 };
    bool HudSize(int& w, int& h)
    {
        __try
        {
            const uint8_t* g = *(const uint8_t* const*)(GameInfo::Base() + kHudGlobal);
            if (!g) return false;
            w = *(const int*)(g + kHudSize); h = *(const int*)(g + kHudSize + 4);
            return w > 0 && h > 0 && w < 32768 && h < 32768;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void* MarkerProjHook(void* cam, const float* world, float* out)
    {
        void* r = g_RealMarkerProj(cam, world, out);
        if (!g_MarkersOn.load(std::memory_order_relaxed) || !world || !out || g_Flat.load() || g_Wheel.load()) return r;
        float mat[16], org[3], axis[9], head[9]; unsigned long long serial = 0;
        if (!ViewMatrixHook::RenderView(mat, org, axis, &serial, head)) return r;
        const ViewMatrixHook::XrPose rp = ViewMatrixHook::RenderPoseXr(serial);
        if (!rp.valid || !rp.tanValid || rp.flat) return r;
        const float l = rp.tan[0], rt = rp.tan[1], u = rp.tan[2], dn = rp.tan[3];
        int w = 0, h = 0;
        if (!(rt > l) || !(u > dn) || !HudSize(w, h)) return r;
        const float d[3] = { world[0] - org[0], world[1] - org[1], world[2] - org[2] };
        // the engine's axis rows: forward, left, up
        const float f = d[0] * head[0] + d[1] * head[1] + d[2] * head[2];
        const float lf = d[0] * head[3] + d[1] * head[4] + d[2] * head[5];
        const float up = d[0] * head[6] + d[1] * head[7] + d[2] * head[8];
        if (!(f > 0.05f)) { g_MarkersKept++; return r; }      // behind the head: the game's own (it clamps it to an edge)
        const float tx = -lf / f, ty = up / f;
        // on the HUD's panel (StereoHud): its frustum shrunk about the centre by HudScale
        const float hs = g_Lift.load() ? g_HudScale.load() : 1.f;
        const float nx = 2.f * (tx - 0.5f * (rt + l)) / ((rt - l) * hs), ny = 2.f * (ty - 0.5f * (u + dn)) / ((u - dn) * hs);
        const float ox = out[0], oy = out[1];
        out[0] = (nx + 1.f) * (float)w * 0.5f; out[1] = (1.f - ny) * (float)h * 0.5f;
        g_MarkersFixed++;
        if (g_Lift.load() && fabsf(nx) <= 1.f && fabsf(ny) <= 1.f) NoteMarker(serial, out[0], out[1], f, w, h);   // for its own panel (PlayHud)
        const double now = Now();
        if (g_MarkerLogs.load() < 10 && now - g_MarkerLogAt.load() > 2.0)
        {
            g_MarkerLogAt = now; g_MarkerLogs++;
            LOGI("ScreenMode: a marker over a character / objective placed for the headset's view: at {:.0f},{:.0f} of the HUD's {}x{} "
                 "(the game's camera had it at {:.0f},{:.0f}); {:.1f} m ahead, {:.1f} deg right, {:.1f} deg up",
                 out[0], out[1], w, h, ox, oy, f, atanf(tx) * kR2D, atanf(ty) * kR2D);
        }
        return r;
    }
}
void ScreenMode::InstallMarkers(bool on)
{
    g_MarkersOn = on;
    if (!g_RealMarkerProj)
    {
        g_RealMarkerProj = (MarkerProj_t)(GameInfo::Base() + kMarkerProj);
        DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_RealMarkerProj, MarkerProjHook);
        const LONG r = DetourTransactionCommit();
        LOGI("ScreenMode: the markers over characters and objectives (exe+0x{:X}) {}: detour={}", kMarkerProj,
             on ? "placed for the headset's view (HudMarkers = 1)" : "where the game's flat camera puts them (HudMarkers = 0)", r);
    }
}
std::string ScreenMode::MarkerStats()
{
    return fmt::format("markers placed for the headset {}, left to the game (behind the head) {}, HUD frames with markers on panels of their own {}; "
                       "markers' drawings measured {} (larger than the square {}), overlapping boxes joined {}, panels {}x{}",
                       g_MarkersFixed.load(), g_MarkersKept.load(), g_MarkPanels.load(), g_ShapesMeasured.load(), g_ShapesGrown.load(), g_MarkMerged.load(),
                       g_PanelW, g_PanelH);
}
void ScreenMode::SetHud(bool on, float dist, float scale, float markerSize)
{
    g_HudOn = on; g_HudDist = std::clamp(dist, 0.5f, 10.f); g_HudScale = std::clamp(scale, 0.3f, 1.f); g_MarkBox = std::clamp(markerSize, 0.f, 0.25f);
    LOGI("ScreenMode: stereo HUD {} ({:.1f} m, {:.0f} % of the view's width: HudScale)", on ? "on one panel for both eyes (StereoHud = 1)" : "in each eye's picture (StereoHud = 0)",
         g_HudDist.load(), g_HudScale.load() * 100.f);
}

void ScreenMode::SetMode(int m)
{
    g_Mode = m ? 1 : 0;
    if (!m) { g_Flat = false; g_Wheel = false; g_Lift = false; }
    LOGI("ScreenMode: {}", m ? "on: menus on a screen in the room, the wheel on a panel" : "off: menus across the eyes, as before");
}
int ScreenMode::Mode() { return g_Mode.load(); }

bool ScreenMode::Set(const std::string& k, float v)
{
    std::lock_guard lk(g_M);
    Cfg& c = g_C;
    if (k == "size") c.size = std::clamp(v, 20.f, 120.f); else if (k == "dist") c.dist = std::clamp(v, 0.5f, 20.f);
    else if (k == "pitchmin") c.pitchMin = v; else if (k == "pitchmax") c.pitchMax = v;
    else if (k == "follow") c.follow = v; else if (k == "followdelay") c.followDelay = v; else if (k == "glide") c.glide = v;
    else if (k == "fadein") c.fadeIn = v; else if (k == "fadeout") c.fadeOut = v;
    else if (k == "backdrop") c.backdrop = std::clamp(v, 0.f, 1.f); else if (k == "env") c.env = std::clamp(v, 0.f, 4.f); else if (k == "glow") c.glow = std::clamp(v, 0.f, 4.f);
    else if (k == "corner") c.corner = std::clamp(v, 0.f, 0.5f); else if (k == "density") c.density = std::clamp(v, 8.f, 64.f);
    else if (k == "wheel") c.wheel = (int)v; else if (k == "wheelsize") c.wheelSize = std::clamp(v, 20.f, 120.f); else if (k == "wheeldist") c.wheelDist = std::clamp(v, 0.4f, 10.f);
    else if (k == "wheelpitchmin") c.wheelPitchMin = v; else if (k == "wheelpitchmax") c.wheelPitchMax = v; else if (k == "wheelfade") c.wheelFade = v;
    else if (k == "shopcrop") c.shopCrop = std::clamp((int)v, 0, 2);
    else if (k == "enter") c.enterFrames = std::max(0, (int)v); else if (k == "exit") c.exitFrames = std::max(0, (int)v);
    else return false;
    return true;
}

std::string ScreenMode::Config()
{
    std::lock_guard lk(g_M);
    const Cfg& c = g_C;
    return fmt::format("size {} dist {} pitchmin {} pitchmax {} follow {} followdelay {} glide {} fadein {} fadeout {} backdrop {} env {} glow {} corner {} density {} "
                       "wheel {} wheelsize {} wheeldist {} wheelpitchmin {} wheelpitchmax {} wheelfade {} shopcrop {} enter {} exit {}",
                       c.size, c.dist, c.pitchMin, c.pitchMax, c.follow, c.followDelay, c.glide, c.fadeIn, c.fadeOut, c.backdrop, c.env, c.glow, c.corner, c.density,
                       c.wheel, c.wheelSize, c.wheelDist, c.wheelPitchMin, c.wheelPitchMax, c.wheelFade, c.shopCrop, c.enterFrames, c.exitFrames);
}

bool ScreenMode::Command(const std::string& what, std::istringstream& ss)
{
    if (what == "screen") { int m = 1; ss >> m; SetMode(m); return true; }
    if (what == "screencfg")
    {
        std::string k; float v = 0;
        if (ss >> k >> v) LOGI("cmd vr screencfg {} {} -> {}", k, v, Set(k, v) ? "ok" : "UNKNOWN NAME");
        LOGI("screencfg: {}", Config());
        return true;
    }
    if (what == "screenrecenter") { g_RecenterReq = true; { std::lock_guard lk(g_M); g_S.lastWhy = "command"; } LOGI("cmd vr screenrecenter"); return true; }
    return false;
}

std::string ScreenMode::Status()
{
    std::lock_guard lk(g_M);
    float c[3] = {}; if (g_Cur.valid) Centre(g_Cur, c);
    return fmt::format("screen on {} kind {} scene {} crop {:.0f} {:.0f} {:.0f} {:.0f} flat {} wheelview {} lift {} alpha {:.2f} entries {} exits {} frames {} screenframes {} flatonly {} "
                       "place yaw {:.1f} pitch {:.1f} dist {:.2f} centre {:.2f} {:.2f} {:.2f} gliding {} recenters {} follows {} "
                       "backdrop freezes {} none {} dropped {} snapfade {:.2f} expects {} missed {} wheels {} wheelpanels {} wheelalpha {:.2f} wheelyaw {:.1f} wheelpitch {:.1f} hud {} hudpanels {}",
                       g_Mode.load(), KindName(g_Kind), g_Scene, g_LastCrop[0], g_LastCrop[1], g_LastCrop[2], g_LastCrop[3], g_Flat.load() ? 1 : 0, g_Wheel.load() ? 1 : 0, g_Lift.load() ? 1 : 0, g_ScreenAlpha,
                       g_S.entries, g_S.exits, g_S.frames, g_S.screenFrames, g_S.flatOnly,
                       g_Cur.yaw * kR2D, g_Cur.pitch * kR2D, g_Cur.d, c[0], c[1], c[2], g_GlideT >= 0.f ? 1 : 0, g_S.recenters, g_S.follows,
                       g_S.freezes, g_S.noFreeze, g_S.snapDropped, g_SnapFade, g_S.expects, g_S.expectMissed, g_S.wheels, g_S.wheelPanels, g_WheelAlpha, g_WheelAt.yaw * kR2D, g_WheelAt.pitch * kR2D, g_HudOn.load() ? 1 : 0, g_HudPanels.load());
}
