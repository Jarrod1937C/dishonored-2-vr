#include "pch.h"
#include "SwordGesture.h"
#include "XRSession.h"
#include "VRRuntime.h"
#include "VRMath.h"
#include "HandAimHook.h"
#include "MeleeAim.h"
#include "PlayerState.h"
#include "GpuTrace.h"
#include "SwordContact.h"
#include <cmath>
#include <sstream>

namespace
{
    // Tunables. Speeds in m/s, times in s, distances in m. The defaults are starting points from how people swing in
    // VR (a deliberate slash moves the hand at 2-5 m/s, a wrist flick moves the tip fast and the hand under 1 m/s);
    // they are meant to be tuned on the headset with the per-swing log lines, not trusted.
    struct Cfg
    {
        float bladeLen = 0.60f;
        float slashTip = 3.0f, slashHand = 1.5f, slashFrames = 2, rearmHand = 0.9f, cooldown = 0.45f;
        // A slash brings the tip TOWARDS where you look; the wind-up before it takes the tip AWAY. Measured in the
        // mock: a fast wind-up (0.2 s) struck, and the real swing 0.2 s later fell in the cooldown and was lost. So a
        // strike needs the angle between head->tip and the gaze to be closing (deg/s below -frontRate). It also keeps
        // a quick move of the fist to the left shoulder (to block) from striking. 0 = off.
        float frontGate = 1, frontRate = 20.f;
        // A pose faster than this is a tracking glitch or a jump, not a motion. Measured in the mock: re-posing the
        // hand in one frame (0.2 m and 135 deg) read as hand 11 m/s -- under a hand-only bound of 12 -- and struck
        // the table. The TIP bound is what catches a pose that turned in one frame.
        // Raised after the first Index test: real fast slashes move the controller at 10-18 m/s (peaks near 30) and the tip
        // faster still, and 128 of them in one session were thrown away here as "jumps" -- the swings that did nothing.
        float peakWin = 0.15f, aimHold = 0.9f, teleport = 40.f, teleportTip = 80.f;
        // chargeFull: measured, a hold of 0.45 s still strikes as a normal slash (0.23 s after the release) and one
        // of 0.55 s strikes 0.05 s after it -- the charged attack. So "full" is at ~0.5 s.
        float chargeStill = 0.6f, chargeDwell = 0.20f, chargeRelease = 1.4f, chargeCone = 40.f, chargeFull = 0.55f;
        // Lowering the blade out of the charge (more than chargeDrop deg from forward, slowly, for chargeDropTime s)
        // CANCELS it: the game drops a charge when block is pressed while the attack is still held (measured: no
        // strike on the release), so the cancel is a short block with the attack held, then both released.
        float chargeDrop = 70.f, chargeDropTime = 0.3f, cancelHold = 0.12f;
        // the charge box around the right ear, head yaw frame: right, up, forward (of the head)
        float earX0 = 0.04f, earX1 = 0.35f, earY0 = -0.22f, earY1 = 0.15f, earZ0 = -0.30f, earZ1 = 0.15f;
        float blockR = 0.20f, blockExit = 0.26f, blockAfterSlash = 0.25f, blockBlade = -0.3f, blockCrouch = 0;
        // the left shoulder from the head, head yaw frame: right, up, forward
        float shX = -0.17f, shY = -0.20f, shZ = -0.02f;
        float haptic = 1.f;
        // A slash also needs the HAND to travel: its chord over the last `slashPathWin` s at least `slashPath`. A wrist
        // flick swings the tip fast and can carry the controller past slashHand, but it moves it a few cm, not 15.
        float slashPath = 0.15f, slashPathWin = 0.15f;
        // The charged attack's thrust (SwordContact on): the attack stays held through the thrust until the blade touches
        // something (released on the contact) or the thrust is over (hand below thrustEnd, or thrustMax s): an air thrust.
        float thrustEnd = 0.6f, thrustMax = 0.35f;
        // With no sword in the right hand (sheathed with a long INTERACT, or never drawn) an empty hand does nothing
        // (the user's call): a click there would draw the sword and swing it. The sword counts as gone once none of its
        // draws has been seen for this long, so a frame that happens to miss them does not switch anything. 0 = off.
        float noSword = 0.3f;
    } C;
    struct Named { const char* name; float* v; };
    const Named kCfg[] = {
        { "bladelen", &C.bladeLen }, { "slashtip", &C.slashTip }, { "slashhand", &C.slashHand },
        { "slashframes", &C.slashFrames }, { "rearm", &C.rearmHand }, { "cooldown", &C.cooldown },
        { "frontgate", &C.frontGate }, { "frontrate", &C.frontRate },
        { "peakwin", &C.peakWin }, { "aimhold", &C.aimHold }, { "teleport", &C.teleport }, { "teleporttip", &C.teleportTip },
        { "chargestill", &C.chargeStill }, { "chargedwell", &C.chargeDwell }, { "chargerelease", &C.chargeRelease },
        { "chargecone", &C.chargeCone }, { "chargefull", &C.chargeFull },
        { "chargedrop", &C.chargeDrop }, { "chargedroptime", &C.chargeDropTime }, { "cancelhold", &C.cancelHold },
        { "earx0", &C.earX0 }, { "earx1", &C.earX1 }, { "eary0", &C.earY0 }, { "eary1", &C.earY1 },
        { "earz0", &C.earZ0 }, { "earz1", &C.earZ1 },
        { "blockr", &C.blockR }, { "blockexit", &C.blockExit }, { "blockafter", &C.blockAfterSlash },
        { "blockblade", &C.blockBlade }, { "blockcrouch", &C.blockCrouch },
        { "slashpath", &C.slashPath }, { "slashpathwin", &C.slashPathWin }, { "thrustend", &C.thrustEnd }, { "thrustmax", &C.thrustMax },
        { "shx", &C.shX }, { "shy", &C.shY }, { "shz", &C.shZ }, { "haptic", &C.haptic }, { "nosword", &C.noSword },
    };

    // While one of these runs the right hand is not swinging a sword (carrying a body, climbing, a takedown, the
    // wheel...): the gestures stand down. The trigger still does whatever it does.
    const char* const kNoGesture[] = { "CarryCorpse", "HoldObject", "Swimming", "SwimMantle", "Climb", "Mantle",
        "Vault", "Choke", "DropChoke", "Assassination", "Cinematic", "SyncAnim", "KillCam", "Dead", "Powerwheel",
        "Journal", "ReadNote", "LootObject", "Interact", "Spyglass", "SpyglassZoom", "KnockedDown", "EatRat" };

    int g_Mask = 7;
    struct Sample { double t; float p[3], b[3], tip[3]; };
    constexpr int kHist = 16;
    Sample g_H[kHist]; int g_N = 0, g_Head = -1;          // ring buffer
    unsigned g_Gen = 0;
    SwordGesture::Out g_Last;                             // outputs of the last sample (hold / block persist)

    bool g_Armed = true; int g_Above = 0; double g_LastFire = -10;
    bool g_PeakOpen = false, g_PeakCharge = false; double g_PeakStart = 0; float g_PeakSpeed = 0; float g_PeakDir[3] = {};
    double g_DwellSince = -1; bool g_Charging = false; double g_ChargeStart = 0; bool g_ReadyBuzzed = false;
    double g_DropSince = -1, g_CancelUntil = -1; unsigned long long g_Cancels = 0;
    // the charged thrust in flight (SwordContact): attack still held until a contact or the end of the thrust
    bool g_Thrusting = false; double g_ThrustStart = 0; float g_ThrustPeak = 0; std::atomic<bool> g_ReleaseAsked{ false };
    unsigned long long g_ThrustHits = 0, g_ThrustAir = 0, g_RejPath = 0;
    bool g_Blocking = false; bool g_PrevTrigger = false;
    // the current burst of fast motion, for the one-line-per-swing log
    bool g_InBurst = false, g_BurstFired = false; float g_BurstHand = 0, g_BurstTip = 0; const char* g_BurstWhy = nullptr;
    double g_BurstStart = 0;

    unsigned long long g_Fired = 0, g_Power = 0, g_Blocks = 0, g_Charges = 0, g_Teleports = 0;
    unsigned long long g_RejCool = 0, g_RejRearm = 0, g_RejTrig = 0, g_RejState = 0, g_RejAway = 0, g_Weak = 0;
    double g_SwordSeen = -1e9; bool g_NoSword = false; unsigned long long g_RejNoSword = 0;
    float g_LastHand = 0, g_LastTip = 0, g_LastPeak = 0, g_LastYaw = 0, g_LastPitch = 0, g_ViewYaw = 0, g_ViewPitch = 0;
    int g_LogSamples = 0;

    float Len(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
    float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    void Rot(const float q[4], const float v[3], float o[3])
    {   // o = q v q*
        const float u[3] = { q[0], q[1], q[2] }, w = q[3];
        const float c1[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        const float c2[3] = { u[1] * c1[2] - u[2] * c1[1], u[2] * c1[0] - u[0] * c1[2], u[0] * c1[1] - u[1] * c1[0] };
        for (int i = 0; i < 3; i++) o[i] = v[i] + 2 * (w * c1[i] + c2[i]);
    }
    void Buzz(float amp, float sec) { if (C.haptic > 0.f) VRRuntime::Vibrate(1, amp * C.haptic, sec); }
    bool NoGestureState()
    {
        for (const char* s : kNoGesture) if (PlayerState::Running(s)) return true;
        return false;
    }
    void ClearHistory() { g_N = 0; g_Head = -1; g_Above = 0; }

    //! Freeze an XR-space direction (head -> tip) as the melee direction, for what is left of aimHold.
    void Publish(const float dXr[3], double t)
    {
        float co[3], ca[9];
        if (!HandAimHook::RealCamera(co, ca)) return;
        float w[3]; HandAimHook::XrOffsetToWorld(dXr, ca, w);
        const float n = Len(w); if (n < 1e-4f) return;
        float f[3] = { w[0] / n, w[1] / n, w[2] / n };
        // idMat3 rows fwd / left / up, id axes (x fwd, y left, z up), no roll
        float l[3] = { -f[1], f[0], 0.f }; float ln = Len(l);
        if (ln < 1e-3f) { l[0] = 0; l[1] = 1; l[2] = 0; ln = 1; }
        l[0] /= ln; l[1] /= ln;
        const float u[3] = { f[1] * l[2] - f[2] * l[1], f[2] * l[0] - f[0] * l[2], f[0] * l[1] - f[1] * l[0] };
        const float axis[9] = { f[0], f[1], f[2], l[0], l[1], l[2], u[0], u[1], u[2] };
        const float left = C.aimHold - (float)(t - g_LastFire);
        // With SwordContact the strike is aimed at what the blade touches; a gesture's peak direction is only logged.
        if (left > 0.f && !SwordContact::Enabled()) MeleeAim::SetGestureAxis(axis, left);
        g_LastYaw = atan2f(f[1], f[0]) * 57.29578f; g_LastPitch = asinf(f[2] > 1 ? 1 : (f[2] < -1 ? -1 : f[2])) * 57.29578f;
        g_ViewYaw = atan2f(ca[1], ca[0]) * 57.29578f; g_ViewPitch = asinf(ca[2] > 1 ? 1 : (ca[2] < -1 ? -1 : ca[2])) * 57.29578f;
    }
    void OpenPeak(bool charge, double t, float speed, const float dir[3])
    {
        g_PeakOpen = true; g_PeakCharge = charge; g_PeakStart = t; g_PeakSpeed = speed;
        memcpy(g_PeakDir, dir, 12);
        Publish(dir, t);
    }
    void CloseBurst()
    {
        if (!g_InBurst) return;
        g_InBurst = false;
        if (g_BurstFired) return;   // the FIRED line was written when it fired
        const char* why = g_BurstWhy ? g_BurstWhy
                        : g_BurstTip <= C.slashTip ? "tip too slow" : g_BurstHand <= C.slashHand ? "hand too slow (a wrist flick?)" : "too short";
        if (!g_BurstWhy) g_Weak++;
        LOGI("Sword: fast motion, no strike ({}): peak hand {:.2f} m/s, tip across the blade {:.2f} m/s (need {:.2f} / {:.2f})",
             why, g_BurstHand, g_BurstTip, C.slashHand, C.slashTip);
    }
}

void SwordGesture::Reset()
{
    ClearHistory();
    if (g_Charging) LOGI("Sword: charge dropped (left play)");
    g_Charging = false; g_Thrusting = false; g_DwellSince = -1; g_DropSince = -1; g_CancelUntil = -1; g_Blocking = false; g_PeakOpen = false; g_InBurst = false;
    g_Last = Out{};
}

SwordGesture::Out SwordGesture::Tick(const XRInputState& in, bool triggerHeld, bool gripHeld)
{
    if (triggerHeld && !g_PrevTrigger) MeleeAim::ClearGestureAxis();   // a trigger swing aims with the controller
    g_PrevTrigger = triggerHeld;
    // Switched off while charging: the charge must be CANCELLED (a block pulse with the attack held), not let go --
    // letting the attack go IS the power attack.
    const double tNow = (double)in.time * 1e-9;
    if (g_Charging && !(g_Mask & 2))
    {
        g_Charging = false; g_Cancels++; g_CancelUntil = tNow + C.cancelHold;
        LOGI("Sword: charge cancelled (the power-attack gesture was switched off)");
    }
    if (!g_Mask)
    {
        if (g_CancelUntil >= 0 && tNow < g_CancelUntil) { Out o; o.hold = true; o.block = true; return o; }
        Reset(); return Out{};
    }
    const XRHandPose& h = in.hand[1];
    const VRRuntime::HeadPose hp = VRRuntime::GetHeadPose();
    // No tracking: keep whatever is held (a charge released by a tracking drop would strike at random) and wait.
    if (!h.valid || h.held || !hp.valid) { ClearHistory(); Out o = g_Last; o.tap = false; return o; }
    const double t = (double)in.time * 1e-9;
    if (g_Head >= 0 && t <= g_H[g_Head].t) { Out o = g_Last; o.tap = false; return o; }   // no new pose this tick
    if (hp.gen != g_Gen) { g_Gen = hp.gen; ClearHistory(); }                                  // recentred: poses jumped

    Sample s{}; s.t = t;
    s.p[0] = h.posX; s.p[1] = h.posY; s.p[2] = h.posZ;
    const float q[4] = { h.quatX, h.quatY, h.quatZ, h.quatW };
    const float up[3] = { 0, 1, 0 }; Rot(q, up, s.b);
    for (int i = 0; i < 3; i++) s.tip[i] = s.p[i] + s.b[i] * C.bladeLen;
    g_Head = (g_Head + 1) % kHist; g_H[g_Head] = s; if (g_N < kHist) g_N++;

    // Velocity over >= 18 ms (two samples at 90 Hz): one frame's difference is noisier than it needs to be.
    float vHand[3] = {}, vTip[3] = {};
    bool haveV = false; const Sample* ref = nullptr; double refDt = 0;
    for (int k = 1; k < g_N; k++)
    {
        const Sample& o = g_H[(g_Head - k + kHist) % kHist];
        const double dt = t - o.t;
        if (dt < 0.018 && k < g_N - 1) continue;
        if (dt <= 1e-4 || dt > 0.1) break;
        for (int i = 0; i < 3; i++) { vHand[i] = (s.p[i] - o.p[i]) / (float)dt; vTip[i] = (s.tip[i] - o.tip[i]) / (float)dt; }
        haveV = true; ref = &o; refDt = dt; break;
    }
    const float hand = Len(vHand);
    if (haveV && (hand > C.teleport || Len(vTip) > C.teleportTip))
    {   // a tracking glitch or a pose that jumped: not a motion
        g_Teleports++;
        LOGI("Sword: pose jump ignored (hand {:.1f} m/s, tip {:.1f} m/s: a glitch or a re-pose, not a swing)", hand, Len(vTip));
        ClearHistory(); g_Head = 0; g_H[0] = s; g_N = 1;
        Out o = g_Last; o.tap = false; return o;
    }
    const float along = Dot(vTip, s.b);
    const float vPerp[3] = { vTip[0] - along * s.b[0], vTip[1] - along * s.b[1], vTip[2] - along * s.b[2] };
    const float tip = Len(vPerp), tipAll = Len(vTip);
    g_LastHand = hand; g_LastTip = tip;

    // The head's yaw frame: forward (horizontal), right, up.
    float hf[3], hr[3], hu[3]; VRMath::QuatBasis(hp.quat, hf, hr, hu);
    float f[3] = { hf[0], 0.f, hf[2] }; { float n = Len(f); if (n < 1e-3f) { f[0] = 0; f[2] = -1; n = 1; } f[0] /= n; f[2] /= n; }
    const float right[3] = { -f[2], 0.f, f[0] };
    const float rel[3] = { s.p[0] - hp.pos[0], s.p[1] - hp.pos[1], s.p[2] - hp.pos[2] };
    const float rx = Dot(rel, right), ry = rel[1], rz = Dot(rel, f);
    float toTip[3] = { s.tip[0] - hp.pos[0], s.tip[1] - hp.pos[1], s.tip[2] - hp.pos[2] };
    { const float n = Len(toTip); if (n > 1e-4f) for (float& v : toTip) v /= n; }
    // How fast the tip closes on the gaze (the head's full forward, pitch included: you look at what you hit), in
    // deg/s; negative = towards it.
    float gazeDeg = acosf(fminf(1.f, fmaxf(-1.f, Dot(toTip, hf)))) * 57.29578f, frontRate = 0.f;
    if (ref)
    {
        float pt[3] = { ref->tip[0] - hp.pos[0], ref->tip[1] - hp.pos[1], ref->tip[2] - hp.pos[2] };
        const float n = Len(pt);
        if (n > 1e-4f) { for (float& v : pt) v /= n; frontRate = (gazeDeg - acosf(fminf(1.f, fmaxf(-1.f, Dot(pt, hf)))) * 57.29578f) / (float)refDt; }
    }

    // Is the sword in the hand? -1 = not measured (the 6DOF path is off): assume it is, as before.
    const int swordDraws = GpuTrace::HandItemDraws(1);
    if (swordDraws != 0) g_SwordSeen = t;
    const bool noSword = C.noSword > 0.f && t - g_SwordSeen > C.noSword;
    if (noSword != g_NoSword)
    {
        g_NoSword = noSword;
        LOGI("Sword: {}", noSword ? "no sword in the right hand (sheathed or empty): the gestures stand down"
                                  : "the sword is in the right hand again: gestures on");
    }
    const bool blocked = NoGestureState() || noSword;
    Out o;

    // The peak of a swing that already struck: keep the direction of the fastest instant.
    if (g_PeakOpen)
    {
        const float sp = g_PeakCharge ? tipAll : tip;
        if (sp > g_PeakSpeed) { g_PeakSpeed = sp; memcpy(g_PeakDir, toTip, 12); Publish(toTip, t); }
        if (t - g_PeakStart > C.peakWin || sp < 0.5f * g_PeakSpeed)
        {
            g_PeakOpen = false; g_LastPeak = g_PeakSpeed;
            float dy = g_LastYaw - g_ViewYaw; while (dy > 180) dy -= 360; while (dy < -180) dy += 360;
            LOGI("Sword: {} aimed at yaw {:.1f} pitch {:.1f} (world; {:+.1f} left / {:+.1f} up of the view), peak {} {:.2f} m/s",
                 g_PeakCharge ? "power attack" : "slash", g_LastYaw, g_LastPitch, dy, g_LastPitch - g_ViewPitch,
                 g_PeakCharge ? "tip" : "tip across the blade", g_PeakSpeed);
        }
    }

    // A charge being cancelled: block pressed with the attack still held, then both let go (see chargeDrop).
    const bool canceling = g_CancelUntil >= 0 && t < g_CancelUntil;
    if (canceling) { o.hold = true; o.block = true; }
    else g_CancelUntil = -1;

    // Power attack: charge in the pose, release with a thrust.
    if ((g_Mask & 2) && !canceling)
    {
        if (g_Charging && gripHeld)
        {   // the grip blocks, and a block drops the charge in the game itself: let the attack go under it, no strike
            g_Charging = false; g_Thrusting = false; g_Cancels++;
            LOGI("Sword: charge cancelled by the grip (block) after {:.2f} s", t - g_ChargeStart);
        }
        else if (g_Charging && g_Thrusting)
        {   // the charged thrust in flight: held until the blade touches something or the thrust is over
            o.hold = true;
            g_ThrustPeak = fmaxf(g_ThrustPeak, hand);
            const bool hit = g_ReleaseAsked.exchange(false);
            const bool over = t - g_ThrustStart > C.thrustMax || (hand < C.thrustEnd && t - g_ThrustStart > 0.05);
            if (hit || over)
            {
                g_Charging = false; g_Thrusting = false; o.hold = false; g_Power++; g_LastFire = t; g_Armed = false;
                if (hit) g_ThrustHits++; else g_ThrustAir++;
                OpenPeak(true, t, tipAll, toTip);
                Buzz(hit ? 0.3f : 1.f, 0.05f);
                LOGI("Sword: POWER ATTACK released {} {:.0f} ms into the thrust (peak hand {:.2f} m/s, {:.2f} s of charge)",
                     hit ? "by the blade's CONTACT" : "at the end of an air thrust", (t - g_ThrustStart) * 1000.0, g_ThrustPeak, t - g_ChargeStart);
            }
        }
        else if (g_Charging)
        {
            o.hold = true;
            if (!g_ReadyBuzzed && t - g_ChargeStart > C.chargeFull) { Buzz(0.8f, 0.06f); g_ReadyBuzzed = true; }
            const bool dropped = Dot(s.b, f) < cosf(C.chargeDrop * 3.14159265f / 180.f) && hand < C.chargeRelease;
            if (!dropped) g_DropSince = -1;
            else if (g_DropSince < 0) g_DropSince = t;
            if (g_DropSince >= 0 && t - g_DropSince >= C.chargeDropTime)
            {
                g_Charging = false; g_Cancels++; g_DropSince = -1; g_CancelUntil = t + C.cancelHold;
                o.hold = true; o.block = true;
                LOGI("Sword: charge cancelled (blade lowered {:.0f} deg from forward for {:.2f} s): block pulse with the attack held",
                     acosf(fminf(1.f, fmaxf(-1.f, Dot(s.b, f)))) * 57.29578f, C.chargeDropTime);
            }
            else if (hand > C.chargeRelease && SwordContact::Enabled())
            {   // the thrust begins: keep the attack held until the blade meets something (SwordContact::Tick)
                g_Thrusting = true; g_ThrustStart = t; g_ThrustPeak = hand; g_ReleaseAsked = false;
                LOGI("Sword: charged THRUST begins (hand {:.2f} m/s, {:.2f} s of charge): held until the blade touches something", hand, t - g_ChargeStart);
            }
            else if (hand > C.chargeRelease)
            {
                g_Charging = false; o.hold = false; g_Power++; g_LastFire = t; g_Armed = false;
                OpenPeak(true, t, tipAll, toTip);
                Buzz(1.f, 0.05f);
                LOGI("Sword: POWER ATTACK released after {:.2f} s of charge (hand {:.2f} m/s, tip {:.2f} m/s)", t - g_ChargeStart, hand, tipAll);
            }
            else if (blocked) { g_Charging = false; o.hold = false; LOGI("Sword: charge dropped (a state that is not a sword fight)"); }
        }
        else if (!blocked && !triggerHeld && !gripHeld)
        {
            const float cone = cosf(C.chargeCone * 3.14159265f / 180.f);
            const bool inBox = rx > C.earX0 && rx < C.earX1 && ry > C.earY0 && ry < C.earY1 && rz > C.earZ0 && rz < C.earZ1;
            const bool pose = inBox && Dot(s.b, f) > cone && hand < C.chargeStill;
            if (!pose) g_DwellSince = -1;
            else if (g_DwellSince < 0) g_DwellSince = t;
            else if (t - g_DwellSince >= C.chargeDwell)
            {
                g_Charging = true; g_ChargeStart = t; g_ReadyBuzzed = false; o.hold = true; g_Charges++; g_DwellSince = -1; g_DropSince = -1;
                g_Blocking = false;
                Buzz(0.3f, 0.04f);
                LOGI("Sword: charging (fist at right {:.2f} up {:.2f} fwd {:.2f} from the head, blade {:.0f} deg off forward)",
                     rx, ry, rz, acosf(fminf(1.f, fmaxf(-1.f, Dot(s.b, f)))) * 57.29578f);
            }
        }
    }

    // Slash.
    if ((g_Mask & 1) && !g_Charging && !o.hold && haveV)
    {
        // the hand's chord over the last slashPathWin seconds
        float chord = 0.f;
        for (int k = 1; k < g_N; k++)
        {
            const Sample& o2 = g_H[(g_Head - k + kHist) % kHist];
            if (t - o2.t > C.slashPathWin) break;
            const float dd[3] = { s.p[0] - o2.p[0], s.p[1] - o2.p[1], s.p[2] - o2.p[2] };
            chord = fmaxf(chord, Len(dd));
        }
        const bool travelled = chord >= C.slashPath;
        const bool fast = tip > C.slashTip && hand > C.slashHand && travelled;
        if (tip > C.slashTip && hand > C.slashHand && !travelled && !g_BurstWhy) { g_BurstWhy = "the hand barely travelled (a wrist flick)"; g_RejPath++; }
        g_Above = fast ? g_Above + 1 : 0;
        if (hand < C.rearmHand) g_Armed = true;
        const bool burst = tip > 0.5f * C.slashTip || hand > 0.5f * C.slashHand;
        if (burst)
        {
            if (!g_InBurst) { g_InBurst = true; g_BurstFired = false; g_BurstHand = 0; g_BurstTip = 0; g_BurstWhy = nullptr; g_BurstStart = t; }
            g_BurstHand = fmaxf(g_BurstHand, hand); g_BurstTip = fmaxf(g_BurstTip, tip);
        }
        else CloseBurst();
        if (g_Above >= (int)C.slashFrames && !g_BurstFired)
        {
            const bool away = C.frontGate > 0.5f && frontRate > -C.frontRate;
            const char* why = noSword ? "no sword in the hand" : blocked ? "not a sword state" : triggerHeld ? "trigger held"
                            : away ? "the tip is moving away from where you look (a wind-up?)"
                            : !g_Armed ? "not re-armed (the hand never slowed down)"
                            : (t - g_LastFire < C.cooldown) ? "cooldown" : nullptr;
            if (!why)
            {
                o.tap = true; g_LastFire = t; g_Armed = false; g_Fired++; g_BurstFired = true;
                OpenPeak(false, t, tip, toTip);
                Buzz(SwordContact::Enabled() ? 0.2f : 0.6f, 0.025f);   // the strong one is the blade's contact (SwordContact)
                LOGI("Sword: SLASH #{} (hand {:.2f} m/s, tip across the blade {:.2f} m/s, {:.0f} ms into the motion)",
                     g_Fired, hand, tip, (t - g_BurstStart) * 1000.0);
            }
            else if (g_BurstWhy != why)
            {
                g_BurstWhy = why;
                if (noSword) g_RejNoSword++; else if (blocked) g_RejState++; else if (triggerHeld) g_RejTrig++; else if (away) g_RejAway++; else if (!g_Armed) g_RejRearm++; else g_RejCool++;
            }
        }
    }
    else CloseBurst();

    // Block: the fist at the left shoulder.
    if (g_Mask & 4)
    {
        const float sh[3] = { hp.pos[0] + right[0] * C.shX + f[0] * C.shZ, hp.pos[1] + C.shY, hp.pos[2] + right[2] * C.shX + f[2] * C.shZ };
        const float d[3] = { s.p[0] - sh[0], s.p[1] - sh[1], s.p[2] - sh[2] };
        const float dist = Len(d);
        const bool bladeOk = Dot(s.b, right) > C.blockBlade;
        const bool allowed = !blocked && !g_Charging && !o.tap && (t - g_LastFire > C.blockAfterSlash)
                          && (C.blockCrouch > 0.5f || !PlayerState::Running("Sneaking"));
        const bool was = g_Blocking;
        g_Blocking = allowed && bladeOk && dist < (was ? C.blockExit : C.blockR);
        if (g_Blocking && !was) { g_Blocks++; Buzz(0.25f, 0.02f); LOGI("Sword: block (fist {:.2f} m from the left shoulder, hand {:.2f} m/s)", dist, hand); }
        if (!g_Blocking && was) LOGI("Sword: block released (fist {:.2f} m from the shoulder)", dist);
        o.block = o.block || g_Blocking;   // the cancel's block pulse stays on
    }

    if (g_LogSamples > 0)
    {
        g_LogSamples--;
        LOGI("Sword sample t {:.3f} hand {:.2f} tipPerp {:.2f} tip {:.2f} gaze {:.1f} deg rate {:.0f} deg/s rel r {:.2f} u {:.2f} f {:.2f} blade.f {:.2f} blade.r {:.2f} above {} armed {} out {}{}{}",
             t, hand, tip, tipAll, gazeDeg, frontRate, rx, ry, rz, Dot(s.b, f), Dot(s.b, right), g_Above, g_Armed, o.tap ? "T" : "-", o.hold ? "H" : "-", o.block ? "B" : "-");
    }
    g_Last = o; g_Last.tap = false;
    return o;
}

bool SwordGesture::StandsDown() { return NoGestureState(); }
bool SwordGesture::Charging() { return g_Charging; }
void SwordGesture::ReleaseCharge() { if (g_Thrusting) g_ReleaseAsked = true; }
void SwordGesture::SetMask(int mask) { g_Mask = mask; if (!(mask & 4)) g_Blocking = false; }   // a charge is cancelled by Tick
int SwordGesture::Mask() { return g_Mask; }
void SwordGesture::LogSamples(int n) { g_LogSamples = n; }

bool SwordGesture::Set(const std::string& key, float v)
{
    for (const Named& n : kCfg) if (key == n.name) { *n.v = v; return true; }
    return false;
}

std::string SwordGesture::Config()
{
    std::ostringstream o;
    for (const Named& n : kCfg) o << n.name << " " << *n.v << "  ";
    return o.str();
}

std::string SwordGesture::Stats()
{
    std::ostringstream o;
    o << "mask " << g_Mask << " slashes " << g_Fired << " power " << g_Power << " charges " << g_Charges << " cancels " << g_Cancels << " blocks " << g_Blocks
      << " rejected away " << g_RejAway << " cooldown " << g_RejCool << " rearm " << g_RejRearm << " trigger " << g_RejTrig << " state " << g_RejState
      << " nosword " << g_RejNoSword << " swordin " << !g_NoSword << " weak " << g_Weak << " teleports " << g_Teleports << " charging " << g_Charging << " blocking " << g_Blocking
      << " hand " << g_LastHand << " tip " << g_LastTip << " lastpeak " << g_LastPeak << " aim " << g_LastYaw << "/" << g_LastPitch
      << " thrusthits " << g_ThrustHits << " thrustair " << g_ThrustAir << " rejpath " << g_RejPath << " thrusting " << g_Thrusting
      << " haptics " << VRRuntime::HapticPulses();
    return o.str();
}
