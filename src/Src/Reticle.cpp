#include "pch.h"
#include "Reticle.h"
#include "GpuTrace.h"
#include "GunRay.h"
#include "HandAimHook.h"
#include "KbmInject.h"
#include "PointerRay.h"
#include "GrabHand.h"
#include "SwordContact.h"
#include "MeleeAim.h"
#include "VRRuntime.h"
#include "CVars.h"
#include <imgui.h>
#include <cmath>
#include <algorithm>
#include <array>
#include <vector>

namespace
{
    std::atomic<int> g_Mode{ 0 };
    std::atomic<float> g_Range{ 8.f };
    std::atomic<int> g_Axes{ 0 };
    std::atomic<bool> g_Minimal{ false };
    std::atomic<float> g_Opacity{ 0.5f };
    //! `c` with its alpha scaled by ReticleOpacity
    ImU32 Op(ImU32 c)
    {
        const float a = (float)((c >> IM_COL32_A_SHIFT) & 0xFF) * g_Opacity.load(std::memory_order_relaxed);
        return (c & ~IM_COL32_A_MASK) | ((ImU32)(a + 0.5f) << IM_COL32_A_SHIFT);
    }
    std::mutex g_M; Reticle::Stats g_Stats;
    // Research markers (`vr reticlemark x y z`): world points drawn as crosses, to check the projection against the image.
    std::vector<std::array<float, 3>> g_Marks;

    // Left hand cyan, right hand amber. Dishonored 2 puts powers and the ranged weapon on the left and the sword
    // on the right, so the two rays mean different things and must not be the same colour.
    const ImU32 kCol[2]    = { IM_COL32(90, 210, 255, 200), IM_COL32(255, 180, 80, 200) };
    const ImU32 kColDim[2] = { IM_COL32(90, 210, 255,  70), IM_COL32(255, 180, 80,  70) };
    // Filled instead of hollow when the ring is on the engine's own landing point rather than a fixed range.
    const ImU32 kColHit[2] = { IM_COL32(120, 240, 255, 110), IM_COL32(255, 210, 120, 110) };

    bool Finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

    //! NDC -> pixels in the image the headset receives (ImGui's display size is the backbuffer, which is what
    //! VRRuntime submits). No viewport correction: the projection came from the same image's own draws.
    ImVec2 ToScreen(const float ndc[2], const ImVec2& size)
    {
        return ImVec2((ndc[0] * 0.5f + 0.5f) * size.x, (0.5f - ndc[1] * 0.5f) * size.y);
    }

    //! One hand's ray. Returns false when this frame has no pose for it (no weapon anchor yet, or hand not tracked).
    //! The grabbing hand draws its POINTER (PointerRay: the ray the interaction sensor picks along), ending on what it
    //! picked -- so what the ray touches on screen is what the grip will take.
    void DrawHand(int h, ImDrawList* dl, const ImVec2& size, Reticle::Stats& st)
    {
        float org[3], fwd[3];
        if (GrabHand::CarryHidesArm(h)) return;    // the hand carrying a corpse is hidden, and so is its ray
        const bool pointer = h == GrabHand::Hand() && GrabHand::Enabled() && PointerRay::Mode() > 0 && PointerRay::Get(h, org, fwd);
        // The hand with a gun or the crossbow: the ray the shot takes, from the muzzle as drawn along the barrel
        // (GunRay), not the controller's -- they differ by the muzzle's offset from the hand and by the grip trim.
        const bool gun = !pointer && h == HandAimHook::GunHand() && (GunRay::Mode() & 8) && GunRay::DrawnRay(h, org, fwd);
        if (!pointer && !gun && !GpuTrace::HandRay(h, org, fwd)) return;
        if (!Finite3(org) || !Finite3(fwd)) return;
        st.handValid |= 1 << h;

        // Where the ring goes. The engine computes a real landing point for the power hand while its sensor is
        // running (Far Reach's marker, the throw preview); that is worth far more than a guess at a fixed range,
        // because it accounts for geometry. Everything else gets the fixed range.
        float end[3];
        bool engineTarget = false;
        float pick[3];
        bool picked = false;            // the grab hand's ray is on something it can take
        const HandAimHook::Stats hs = HandAimHook::GetStats();
        // (+0x108, the tendril's END: +0x1C0, used before, is the curve's middle control point, which HandFx moves
        // with the tendril's start.)
        if (gun)
        {   // up to where the engine's own aim sensor, tracing this very ray, says it hits: the bolt lands on the ring
            float t = 0; bool hit = false;
            if (!GunRay::SensorHit(t, hit)) { t = g_Range.load(); hit = false; }
            for (int i = 0; i < 3; i++) end[i] = org[i] + fwd[i] * t;
            if (hit) { engineTarget = true; st.engineTarget |= 1 << h; }
        }
        else if (pointer)
        {   // up to the picked object (along the ray, then the ring on the object itself), else the sensor's reach
            picked = !GrabHand::HeldEntity() && GrabHand::HandHasTarget() && GrabHand::TargetPoint(pick) && Finite3(pick);
            const float reach = GrabHand::PickRange() > 0.f ? GrabHand::PickRange() : 3.f;
            float len = reach;
            if (picked)
            {
                const float d[3] = { pick[0] - org[0], pick[1] - org[1], pick[2] - org[2] };
                len = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
                len = len < 0.05f ? 0.05f : len;
                engineTarget = true;
                st.engineTarget |= 1 << h;
            }
            for (int i = 0; i < 3; i++) end[i] = picked ? pick[i] : org[i] + fwd[i] * len;
        }
        else if (h == (HandAimHook::PowerHand() & 1) && hs.powerActive && Finite3(hs.powerEnd)
            && (hs.powerEnd[0] || hs.powerEnd[1] || hs.powerEnd[2]))
        {
            memcpy(end, hs.powerEnd, 12);
            engineTarget = true;
            st.engineTarget |= 1 << h;
        }
        else
        {
            const float r = g_Range.load();
            for (int i = 0; i < 3; i++) end[i] = org[i] + fwd[i] * r;
        }

        // The ray, as a polyline. A straight world line does project to a straight screen line, but sampling it
        // lets the part that falls behind the eye drop out cleanly instead of taking the whole segment with it.
        constexpr int kSteps = 10;
        // skip the first 12 cm: a line that begins inside the hand reads badly (a gun's starts at its muzzle, 1 cm on)
        const float len = sqrtf((end[0] - org[0]) * (end[0] - org[0]) + (end[1] - org[1]) * (end[1] - org[1]) + (end[2] - org[2]) * (end[2] - org[2]));
        const float kStart = gun ? (len > 0.02f ? 0.01f / len : 0.f) : 0.12f;
        ImVec2 pts[kSteps + 1]; int n = 0;
        for (int i = 0; i <= kSteps; i++)
        {
            const float t = kStart + (1.f - kStart) * (float)i / kSteps;
            const float w[3] = { org[0] + (end[0] - org[0]) * t, org[1] + (end[1] - org[1]) * t, org[2] + (end[2] - org[2]) * t };
            float ndc[2];
            if (!GpuTrace::ProjectWorld(w, ndc)) { n = 0; continue; }     // restart the run past the eye plane
            pts[n++] = ToScreen(ndc, size);
        }
        const bool minimal = g_Minimal.load(std::memory_order_relaxed);
        if (n >= 2 && !minimal) dl->AddPolyline(pts, n, Op(kColDim[h]), 0, 2.0f);

        // The ring at the end.
        float ndc[2], depth = 0;
        if (!GpuTrace::ProjectWorld(end, ndc, &depth)) return;
        st.projValid |= 1 << h;
        const ImVec2 c = ToScreen(ndc, size);
        if (!pointer && h == HandAimHook::GunHand()) GunRay::NoteRing(c.x / size.x, c.y / size.y);   // the gun hand's ring, either mode (test scripts aim with it)
        // A takedown on offer (the game's assassination sensor, MeleeAim::KillOffered): the right hand's reticle turns RED
        // (lethal: the sword) inside a thick BLUE border (non-lethal: the choke) -- the game offers the two together.
        const bool takedown = h == 1 && MeleeAim::KillOffered();
        const ImU32 kTakedownRed = IM_COL32(235, 30, 30, 255), kTakedownBlue = IM_COL32(40, 110, 255, 255);
        if (minimal)
        {   // MinimalReticle: a white dot ringed in black, a fixed angular size (~0.3 deg across at the headset's density);
            // three times that while the grab hand's ray is on something it can take
            const float grow = picked ? 3.f : 1.f;
            const float r = std::max(2.5f, size.y * 0.0016f) * grow, edge = std::max(1.5f, size.y * 0.0008f) * (picked ? 1.5f : 1.f);
            if (takedown)
            {
                const float thick = std::max(3.f, size.y * 0.0024f) * (picked ? 1.5f : 1.f);   // three times the usual edge
                dl->AddCircleFilled(c, r + thick + 1.f, Op(IM_COL32(0, 0, 0, 255)), 20);       // a hairline of black keeps it readable on blue skies
                dl->AddCircleFilled(c, r + thick, Op(kTakedownBlue), 20);
                dl->AddCircleFilled(c, r, Op(kTakedownRed), 16);
                st.drawn++;
                return;
            }
            dl->AddCircleFilled(c, r + edge, Op(IM_COL32(0, 0, 0, 255)), 16);
            dl->AddCircleFilled(c, r, Op(IM_COL32(255, 255, 255, 255)), 16);
            st.drawn++;
            return;
        }
        const float rad = size.y * 0.012f;                  // a fixed angular size reads as a reticle; range does not
        if (takedown)
        {   // the ring: red, inside a thick blue band
            const float band = std::max(4.f, size.y * 0.004f);
            dl->AddCircle(c, rad + band * 0.5f + 1.f, Op(kTakedownBlue), 24, band);
            dl->AddCircle(c, rad, Op(kTakedownRed), 24, 2.5f);
            dl->AddCircleFilled(c, rad * 0.35f, Op(kTakedownRed), 12);
            st.drawn++;
            return;
        }
        dl->AddCircle(c, rad, Op(kCol[h]), 24, 2.0f);
        if (engineTarget) dl->AddCircleFilled(c, rad, Op(kColHit[h]), 24);
        dl->AddCircleFilled(c, 1.8f, Op(kCol[h]), 8);
        st.drawn++;
    }

    //! A world segment from `org` along `dir`, drawn if both ends project. Short lines are straight enough in
    //! screen space that two points are all it takes.
    void Seg(ImDrawList* dl, const ImVec2& size, const float org[3], const float dir[3], float len, ImU32 col, float w, bool dot)
    {
        const float end[3] = { org[0] + dir[0] * len, org[1] + dir[1] * len, org[2] + dir[2] * len };
        float a[2], b[2];
        if (!GpuTrace::ProjectWorld(org, a) || !GpuTrace::ProjectWorld(end, b)) return;
        const ImVec2 pa = ToScreen(a, size), pb = ToScreen(b, size);
        dl->AddLine(pa, pb, col, w);
        if (dot) dl->AddCircleFilled(pb, 3.5f, col, 10);
    }

    //! The measuring instrument: where the mod has put the PALM and how it is turned, next to the controller that
    //! is supposed to be driving it. Judging this from the rendered hand does not work -- with a power the hand is
    //! a fanned black glove at the edge of the frame -- so both frames get drawn on top of the image and the
    //! question "does the palm follow my real hand" becomes something you can read off a screenshot.
    //! At the palm: fingers WHITE and the palm normal MAGENTA (both with a dot at the far end), plus the
    //! palm bone's own axes in red/green/blue.
    //! At the controller: forward in the hand's colour with a dot, up and left the same colour, dimmer.
    void DrawAxes(int h, ImDrawList* dl, const ImVec2& size, Reticle::Stats& st)
    {
        float corg[3], crot[9];
        if (GpuTrace::HandPoseWorld(h, corg, crot))
        {
            const float fwd[3] = { crot[0], crot[3], crot[6] };      // columns are fwd/left/up
            const float left[3] = { crot[1], crot[4], crot[7] };
            const float up[3] = { crot[2], crot[5], crot[8] };
            Seg(dl, size, corg, fwd, 0.16f, kCol[h], 2.0f, true);
            Seg(dl, size, corg, up, 0.09f, kColDim[h], 2.0f, false);
            Seg(dl, size, corg, left, 0.09f, kColDim[h], 2.0f, false);
        }
        float porg[3], prot[9], fing[3], pn[3];
        if (!GpuTrace::PalmFrame(h, porg, prot, fing, pn)) return;
        const float px[3] = { prot[0], prot[3], prot[6] }, py[3] = { prot[1], prot[4], prot[7] }, pz[3] = { prot[2], prot[5], prot[8] };
        Seg(dl, size, porg, px, 0.07f, IM_COL32(255, 70, 70, 220), 2.0f, false);
        Seg(dl, size, porg, py, 0.07f, IM_COL32(70, 255, 70, 220), 2.0f, false);
        Seg(dl, size, porg, pz, 0.07f, IM_COL32(90, 120, 255, 220), 2.0f, false);
        Seg(dl, size, porg, fing, 0.13f, IM_COL32(255, 255, 255, 230), 2.5f, true);
        Seg(dl, size, porg, pn, 0.10f, IM_COL32(255, 80, 255, 230), 2.5f, true);      // out of the PALM, magenta
        float ndc[2];
        if (GpuTrace::ProjectWorld(porg, ndc)) dl->AddCircleFilled(ToScreen(ndc, size), 4.0f, IM_COL32(255, 255, 255, 230), 12);
        // The weapon in this hand (yellow): its origin and forward -- for the sword, where the fist's hole is.
        float wo[3], wa[9];
        if (GpuTrace::WeaponTargetPose(h, wo, wa))
        {
            Seg(dl, size, wo, wa, 0.12f, IM_COL32(255, 220, 0, 230), 2.0f, true);
            if (GpuTrace::ProjectWorld(wo, ndc)) dl->AddCircleFilled(ToScreen(ndc, size), 4.0f, IM_COL32(255, 220, 0, 230), 12);
        }
        st.drawn++;
    }
    //! The sword's contact detector (SwordContact): the blade it sweeps (orange, guard to tip), the boxes it sweeps it
    //! against nearest the tip (grey; red for 1.5 s after the blade struck one) and the last contact point (red).
    void DrawSword(ImDrawList* dl, const ImVec2& size)
    {
        static SwordContact::Debug d;   // ~20 KB: not on the stack
        if (!SwordContact::GetDebug(d)) return;
        // the bodies: bone capsules (cyan; red for 1.5 s after the blade struck that body)
        float a[2], b[2];
        for (int k = 0; k < d.segs; k++)
            if (GpuTrace::ProjectWorld(d.seg[k].a, a) && GpuTrace::ProjectWorld(d.seg[k].b, b))
                dl->AddLine(ToScreen(a, size), ToScreen(b, size), d.seg[k].touched ? IM_COL32(255, 40, 40, 220) : IM_COL32(60, 220, 255, 150), d.seg[k].r > 0.12f ? 3.0f : 1.5f);
        // sample by sample: the tip is often behind the eye's plane (blade up, looking down) and a line to it cannot be drawn.
        // Orange = a contact now strikes; yellow = not a swing of the arm (a touch only); grey = this swing already struck.
        const ImU32 bc = d.armed ? IM_COL32(255, 140, 0, 230) : d.ready ? IM_COL32(255, 230, 60, 200) : IM_COL32(150, 150, 150, 200);
        for (int i = 0; i + 1 < d.n; i++)
            if (GpuTrace::ProjectWorld(d.pts[i], a) && GpuTrace::ProjectWorld(d.pts[i + 1], b))
            {
                dl->AddLine(ToScreen(a, size), ToScreen(b, size), bc, 3.0f);
                dl->AddCircleFilled(ToScreen(b, size), i + 2 == d.n ? 6.0f : 2.5f, bc, 10);
            }
        for (int k = 0; k < d.boxes; k++)
        {
            ImVec2 sc[8]; bool ok[8];
            for (int c = 0; c < 8; c++) { float n[2]; ok[c] = GpuTrace::ProjectWorld(d.corners[k][c], n); if (ok[c]) sc[c] = ToScreen(n, size); }
            const ImU32 col = d.touched[k] ? IM_COL32(255, 40, 40, 240) : IM_COL32(200, 200, 200, 140);
            for (int x = 0; x < 8; x++) for (int bit = 1; bit < 8; bit <<= 1)
                if (!(x & bit) && ok[x] && ok[x | bit]) dl->AddLine(sc[x], sc[x | bit], col, d.touched[k] ? 2.0f : 1.0f);
        }
        float n[2];
        if (d.contact && GpuTrace::ProjectWorld(d.point, n)) dl->AddCircle(ToScreen(n, size), 9.0f, IM_COL32(255, 30, 30, 255), 16, 3.0f);
    }
    //! The held object's box as it is drawn (cyan), with its centre: what the grip has to put in the fist.
    void DrawHeld(ImDrawList* dl, const ImVec2& size)
    {
        GrabHand::Debug d;
        if (!GrabHand::GetDebug(d) || !d.box) return;
        ImVec2 s[8]; bool ok[8];
        for (int c = 0; c < 8; c++) { float n[2]; ok[c] = GpuTrace::ProjectWorld(d.corners[c], n); if (ok[c]) s[c] = ToScreen(n, size); }
        const ImU32 col = IM_COL32(0, 230, 255, 220);
        for (int a = 0; a < 8; a++) for (int bit = 1; bit < 8; bit <<= 1)
            if (!(a & bit) && ok[a] && ok[a | bit]) dl->AddLine(s[a], s[a | bit], col, 1.5f);
        float n[2];
        if (GpuTrace::ProjectWorld(d.center, n)) dl->AddCircleFilled(ToScreen(n, size), 5.0f, col, 12);
    }
}

void Reticle::Draw()
{
    const int mode = g_Mode.load();
    if (!mode && !g_Axes.load()) return;
    // Not over a menu. The reticle is aimed at the world, and while a menu is up there is nothing to aim at --
    // it was drawn straight across the main menu before this check.
    if (KbmInject::GetStats().menuSafe) return;
    // Nor during a takedown: the hands are the animation's, and a ray from a controller they are not attached to
    // points at nothing the player can act on.
    if (GpuTrace::AnimSuspended()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    if (!dl || size.x < 16.f || size.y < 16.f) return;
    // projected with the frustum of the image this lands on (the one about to be presented), not the latest view's: in
    // stereo the two eyes' projections differ (CameraHook::ApplyEyeProjection)
    struct ProjScope { ProjScope() { GpuTrace::ProjectForSerial(VRRuntime::PresentingSerial()); } ~ProjScope() { GpuTrace::ProjectForSerial(0); } } projScope;
    Stats st; st.frames = 1;
    if (mode == 1 || mode == 2) DrawHand(HandAimHook::PowerHand() & 1, dl, size, st);
    if (mode == 1 || mode == 3)
    {
        const int gun = HandAimHook::GunHand() & 1;
        // With both hands on the same controller (the D2 default) mode 1 must not draw it twice.
        if (mode == 3 || gun != (HandAimHook::PowerHand() & 1)) DrawHand(gun, dl, size, st);
        else DrawHand(gun ^ 1, dl, size, st);
    }
    if (g_Axes.load()) { DrawAxes(0, dl, size, st); DrawAxes(1, dl, size, st); DrawHeld(dl, size); DrawSword(dl, size); }
    {
        std::lock_guard lk(g_M);
        for (size_t i = 0; i < g_Marks.size(); i++)
        {
            float ndc[2];
            if (!GpuTrace::ProjectWorld(g_Marks[i].data(), ndc)) continue;
            const ImVec2 c = ToScreen(ndc, size);
            const ImU32 col = IM_COL32(255, 60, 255, 255);
            dl->AddLine(ImVec2(c.x - 8, c.y), ImVec2(c.x + 8, c.y), col, 2.f);
            dl->AddLine(ImVec2(c.x, c.y - 8), ImVec2(c.x, c.y + 8), col, 2.f);
            char t[8]; snprintf(t, sizeof t, "%zu", i);
            dl->AddText(ImVec2(c.x + 6, c.y + 4), col, t);
        }
    }
    std::lock_guard lk(g_M);
    g_Stats.frames += st.frames; g_Stats.drawn += st.drawn;
    g_Stats.handValid = st.handValid; g_Stats.projValid = st.projValid; g_Stats.engineTarget = st.engineTarget;
}

void Reticle::SetMode(int m) { g_Mode = m; }

namespace
{
    // The game's own crosshair (ark_ui_crosshair_style 0 off / 1 dot / 2 normal, ark_ui_crosshair_opacity) is the
    // centre of the game camera's view: with the hands aiming, it marks nothing. Off while the hand reticles are on and
    // the headset shows the game, and the player's values put back otherwise. Only the live values are written (as
    // WristHud does with the HUD option): the game's config keeps the player's choice. A value the player picks in the
    // options meanwhile is taken as their new choice, to put back later.
    int g_UserStyle = -1; float g_UserOpacity = -1.f; bool g_Forced = false;
    ULONGLONG g_NextCrossCheck = 0;
}

void Reticle::Tick()
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_NextCrossCheck || !CVars::Available()) return;
    g_NextCrossCheck = now + 1000;
    const bool want = g_Mode.load() != 0 && VRRuntime::GetStatus().running;
    const int style = CVars::GetInt("ark_ui_crosshair_style", -1);
    const float opacity = CVars::GetFloat("ark_ui_crosshair_opacity", -1.f);
    if (style < 0) return;
    if (want)
    {
        if (style != 0 || (opacity > 0.f))
        {
            // (only what differs from the forced value is the player's: the other one is still ours)
            if (style != 0 || g_UserStyle < 0) g_UserStyle = style;
            if (opacity > 0.f || (g_UserOpacity < 0.f && opacity >= 0.f)) g_UserOpacity = opacity;
            CVars::SetInt("ark_ui_crosshair_style", 0);
            if (opacity >= 0.f) CVars::SetFloat("ark_ui_crosshair_opacity", 0.f);
            if (!g_Forced) LOGI("Reticle: the game's crosshair off while the hand reticles are on (was style {}, opacity {:.2f})", style, opacity);
            g_Forced = true;
        }
    }
    else if (g_Forced)
    {
        if (g_UserStyle >= 0) CVars::SetInt("ark_ui_crosshair_style", g_UserStyle);
        if (g_UserOpacity >= 0.f) CVars::SetFloat("ark_ui_crosshair_opacity", g_UserOpacity);
        LOGI("Reticle: the game's crosshair back to the player's setting (style {}, opacity {:.2f})", g_UserStyle, g_UserOpacity);
        g_Forced = false; g_UserStyle = -1; g_UserOpacity = -1.f;
    }
}
void Reticle::SetStyle(bool minimal, float opacity)
{
    g_Minimal = minimal;
    g_Opacity = opacity < 0.05f ? 0.05f : (opacity > 1.f ? 1.f : opacity);
    LOGI("Reticle: {} at opacity {:.2f}", minimal ? "a dot where each hand points (MinimalReticle = 1)" : "rays and rings", g_Opacity.load());
}
void Reticle::AddMark(const float* w) { std::lock_guard lk(g_M); if (g_Marks.size() < 32) g_Marks.push_back({ w[0], w[1], w[2] }); }
void Reticle::ClearMarks() { std::lock_guard lk(g_M); g_Marks.clear(); }
void Reticle::SetAxes(int on) { g_Axes = on; }
int  Reticle::Axes() { return g_Axes.load(); }
int  Reticle::Mode() { return g_Mode.load(); }
void Reticle::SetRange(float m) { if (m > 0.1f && m < 500.f) g_Range = m; }
float Reticle::Range() { return g_Range.load(); }
Reticle::Stats Reticle::GetStats() { std::lock_guard lk(g_M); return g_Stats; }
