#include "pch.h"
#include "BodyDrag.h"
#include "BodyHide.h"
#include "Collision.h"
#include "GpuTrace.h"
#include "GrabHand.h"
#include "HandAimHook.h"
#include "NpcSkel.h"
#include "VRRuntime.h"
#include "ViewMatrixHook.h"
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    // The pull: a spring from the grabbed joint to the hand, slack within kSlack, its speed at most kMaxSpeed.
    // (2026-11-05 play: at 4/s, 2.8 m/s and 45 % the body shook and its arm flew up with the hand: softer now, and the
    // pull's target kept low -- a body is dragged along the floor, not lifted into the air)
    constexpr float kSlack = 0.10f, kGain = 3.0f, kMaxSpeed = 2.0f;   // m, 1/s, m/s
    constexpr float kShare = 0.30f;          // of the speed error corrected per tick (an impulse acts a tick later)
    constexpr float kLiftMax = 0.55f;        // m: the pull's target at most this far above the body's lowest joint
    constexpr float kMass0 = 20.f, kMassMin = 3.f, kMassMax = 160.f;  // kg: the part's effective mass, first guess and bounds
    constexpr float kMaxImpulse = 140.f;     // N s in one tick, whatever the mass
    // Onto the back: the hand over the shoulder -- within kShoulderDown of the eyes' height or above, within
    // kShoulderR of the head across the floor, not more than kShoulderFwd in front of it -- having come up kRise since
    // the grip closed.
    constexpr float kShoulderDown = 0.22f, kShoulderR = 0.38f, kShoulderFwd = 0.12f, kRise = 0.30f;

    std::atomic<bool> g_On{ true };
    std::mutex g_M;
    struct Cmd { unsigned seq = 0; bool on = false; int hand = 1; uint32_t id = 0; } g_Cmd;   // under g_M
    struct Shoulder { bool ready = false; uint32_t id = 0; float p[3] = {}; } g_Sh;          // under g_M

    // the game thread's
    struct St
    {
        unsigned seq = 0; bool on = false; int hand = 1; uint32_t id = 0;
        double t0 = 0, lastT = 0, lastLog = 0, lastBuzz = 0;
        int joint = -1; float prev[3] = {}; bool prevOk = false; float vel[3] = {};
        float mass = kMass0; float lastJ[3] = {}; float vBefore[3] = {}; bool judge = false;
        float handStart[3] = {}; bool startOk = false; float p0[3] = {}; float gap = 0.f;
        bool shoulderSent = false; bool ragdoll = false; int impulses = 0; float moved = 0.f;
        // for the log: the hand's highest since the grip, and where it was then against the shoulder zone
        float maxRise = -1e9f, atAcross = 0.f, atUp = 0.f, atFwd = 0.f; int traces = 0; bool skelTold = false; float lastAlong = 0.f;
    } S;
    int g_DragsLogged = 0;   // the first drags are traced impulse by impulse
    NpcSkel::Skel g_Sk;
    std::atomic<unsigned long long> g_Drags{ 0 }, g_Lifts{ 0 }, g_Impulses{ 0 };
    std::atomic<float> g_LastMoved{ 0.f }, g_LastMass{ 0.f };
    int g_DetailLogs = 0;   // the first drags are logged second by second

    double Now() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart; }
    float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    float Len(const float* a) { return sqrtf(Dot(a, a)); }

    void End(const char* why)
    {
        if (!S.on) return;
        g_LastMoved = S.moved; g_LastMass = S.mass;
        LOGI("BodyDrag: body {:#x} let go ({}) after {:.1f} s: dragged {:.2f} m with {} impulses (the part's mass taken as {:.0f} kg; {})",
             S.id, why, Now() - S.t0, S.moved, S.impulses, S.mass, S.ragdoll ? "a ragdoll" : "posed by its animation");
        if (!S.shoulderSent && S.maxRise > -1e8f)
            LOGI("BodyDrag: not lifted onto the back: the hand rose at most {:.0f} cm since the grip (needs {:.0f}); at its highest it was {:.0f} cm from the eyes across (needs under {:.0f}), {:.0f} cm above / below them (needs over -{:.0f}), {:.0f} cm in front of them (needs under {:.0f})",
                 S.maxRise * 100.f, kRise * 100.f, S.atAcross * 100.f, kShoulderR * 100.f, S.atUp * 100.f, kShoulderDown * 100.f, S.atFwd * 100.f, kShoulderFwd * 100.f);
        S.on = false;
    }
}

void BodyDrag::SetEnabled(bool on)
{
    g_On = on;
    LOGI("BodyDrag: {}", on ? "a body is dragged by the hand that grips it, and goes onto your back when that hand comes up over your shoulder (DragBodies)"
                            : "off: the grip on a body picks it up at once (the game's carry)");
}
bool BodyDrag::Enabled() { return g_On.load(); }
void BodyDrag::Start(int hand, uint32_t id)
{
    std::lock_guard lk(g_M);
    g_Cmd.seq++; g_Cmd.on = true; g_Cmd.hand = hand ? 1 : 0; g_Cmd.id = id; g_Sh = Shoulder{};
}
void BodyDrag::Stop(const char* why)
{
    std::lock_guard lk(g_M);
    if (g_Cmd.on) LOGI("BodyDrag: stop ({})", why ? why : "?");
    g_Cmd.on = false;
}
bool BodyDrag::Active() { std::lock_guard lk(g_M); return g_Cmd.on; }
bool BodyDrag::TakeShoulder(uint32_t* id, float point[3])
{
    std::lock_guard lk(g_M);
    if (!g_Sh.ready) return false;
    *id = g_Sh.id; memcpy(point, g_Sh.p, 12); g_Sh.ready = false;
    return true;
}

void BodyDrag::Tick()
{
    Cmd c;
    { std::lock_guard lk(g_M); c = g_Cmd; }
    if (!c.on || !g_On.load()) { End(g_On.load() ? "the grip" : "DragBodies off"); return; }
    const double now = Now();
    if (c.seq != S.seq || !S.on)
    {
        S = St{}; S.seq = c.seq; S.on = true; S.hand = c.hand; S.id = c.id; S.t0 = now; S.lastT = now; g_DragsLogged++;
        float r[9]; S.startOk = GpuTrace::RawHandPose(S.hand, S.handStart, r);
        g_Drags++;
    }
    void* ent = BodyHide::EntityById((int)S.id);
    if (!ent) { End("the body is gone"); std::lock_guard lk(g_M); g_Cmd.on = false; return; }
    float hp[3], hr[9], eye[3], ax[9];
    if (!GpuTrace::RawHandPose(S.hand, hp, hr) || !HandAimHook::RealCamera(eye, ax)) return;
    {   // the real head, where the view puts it (the game's eye plus the view's own offset: a lean, the crouch lift)
        float vo[3], go[3], hm[3];
        if (ViewMatrixHook::ViewAnchor(vo, go, hm)) for (int i = 0; i < 3; i++) { const float d = vo[i] - go[i]; if (fabsf(d) < 1.5f) eye[i] += d; }
    }
    if (!S.startOk) { memcpy(S.handStart, hp, 12); S.startOk = true; }
    const float dt = (float)std::clamp(now - S.lastT, 1e-3, 0.1); S.lastT = now;

    // the grabbed part: the joint nearest the hand when the grip closed (or the body's middle, without a skeleton)
    float p[3];
    const bool sk = NpcSkel::Get(ent, g_Sk) && g_Sk.n >= 5;
    if (sk)
    {
        if (S.joint < 0 || S.joint >= g_Sk.n)
        {
            float best = 1e9f;
            for (int j = 0; j < g_Sk.n; j++) { const float d[3] = { g_Sk.p[j][0] - hp[0], g_Sk.p[j][1] - hp[1], g_Sk.p[j][2] - hp[2] }; const float l = Len(d); if (l < best) { best = l; S.joint = j; } }
            S.ragdoll = g_Sk.ragdoll;
            LOGI("BodyDrag: {} hand took hold of body {:#x} ({}) at joint {} of {}, {:.0f} cm from the hand ({})", S.hand ? "right" : "left", S.id,
                 Collision::Describe(ent), S.joint, g_Sk.n, best * 100.f, g_Sk.ragdoll ? "a ragdoll" : "posed by its animation");
        }
        memcpy(p, g_Sk.p[S.joint], 12);
    }
    else
    {
        if (!S.skelTold) { S.skelTold = true; LOGI("BodyDrag: body {:#x} ({}): no skeleton read ({}): pulled by its middle", S.id, Collision::Describe(ent), NpcSkel::Stats()); }
        if (!GrabHand::EntityCenter((int)S.id, p)) return;
    }
    if (!S.prevOk) { memcpy(S.p0, p, 12); }
    if (S.prevOk)
    {
        const float k = std::min(1.f, dt / 0.03f);
        for (int i = 0; i < 3; i++) S.vel[i] += ((p[i] - S.prev[i]) / dt - S.vel[i]) * k;
        const float d0[3] = { p[0] - S.p0[0], p[1] - S.p0[1], 0.f };
        S.moved = std::max(S.moved, Len(d0));
    }
    memcpy(S.prev, p, 12); S.prevOk = true;

    // onto the back: the hand up over the shoulder
    {
        const float rise = hp[2] - S.handStart[2];
        const float rel[3] = { hp[0] - eye[0], hp[1] - eye[1], hp[2] - eye[2] };
        float fw[3] = { ax[0], ax[1], 0.f }; const float fl = Len(fw); if (fl > 1e-4f) { fw[0] /= fl; fw[1] /= fl; }
        const float across = sqrtf(rel[0] * rel[0] + rel[1] * rel[1]);
        const bool zone = rel[2] > -kShoulderDown && across < kShoulderR && Dot(rel, fw) < kShoulderFwd;
        if (rise > S.maxRise) { S.maxRise = rise; S.atAcross = across; S.atUp = rel[2]; S.atFwd = Dot(rel, fw); }
        if (!S.shoulderSent && zone && rise > kRise && now - S.t0 > 0.25)
        {
            S.shoulderSent = true; g_Lifts++;
            { std::lock_guard lk(g_M); g_Sh.ready = true; g_Sh.id = S.id; memcpy(g_Sh.p, p, 12); g_Cmd.on = false; }
            VRRuntime::Vibrate(S.hand, 0.5f, 0.08f);
            LOGI("BodyDrag: {} hand brought body {:#x} up over the shoulder (the hand {:.0f} cm up since the grip, {:.0f} cm from the eyes across, {:.0f} cm above / below them): onto the back (the game's carry)",
                 S.hand ? "right" : "left", S.id, rise * 100.f, across * 100.f, rel[2] * 100.f);
            End("onto the back");
            return;
        }
    }

    // how heavy: the speed the last impulse gave it, along the impulse (it takes effect about a tick later)
    const float jl = Len(S.lastJ);
    if (S.judge && jl > 1.f)
    {
        const float dv[3] = { S.vel[0] - S.vBefore[0], S.vel[1] - S.vBefore[1], S.vel[2] - S.vBefore[2] };
        const float along = Dot(dv, S.lastJ) / jl;
        S.lastAlong = along;
        if (along > 0.05f)
        {
            const float m = std::clamp(jl / along, kMassMin, kMassMax);
            S.mass = expf(logf(S.mass) + (logf(m) - logf(S.mass)) * 0.25f);
        }
        else if (along < 0.01f && jl > S.mass * 0.5f) S.mass = std::min(kMassMax, S.mass * 1.08f);   // no response: heavier than thought (or held by the floor)
        S.judge = false;
    }

    // the pull: towards the hand, but no higher than kLiftMax over the body's lowest part
    float floorZ = p[2];
    if (sk) for (int j = 0; j < g_Sk.n; j++) floorZ = std::min(floorZ, g_Sk.p[j][2]);
    const float tz = std::min(hp[2], floorZ + kLiftMax);
    const float d[3] = { hp[0] - p[0], hp[1] - p[1], tz - p[2] };
    const float dl = Len(d);
    float vdes[3] = {};
    if (dl > kSlack) { const float s = std::min(kMaxSpeed, kGain * (dl - kSlack)) / dl; for (int i = 0; i < 3; i++) vdes[i] = d[i] * s; }
    float J[3]; for (int i = 0; i < 3; i++) J[i] = S.mass * (vdes[i] - S.vel[i]) * kShare;
    if (dl <= kSlack) J[2] = std::max(J[2], 0.f);   // in the slack: only stop it, never press it into the floor
    float jn = Len(J);
    const float cap = std::min(kMaxImpulse, S.mass * 2.0f);
    if (jn > cap) { for (float& v : J) v *= cap / jn; jn = cap; }
    if (jn > 0.5f)
    {
        Collision::Push(ent, J, p);
        memcpy(S.lastJ, J, 12); memcpy(S.vBefore, S.vel, 12); S.judge = true;
        S.impulses++; g_Impulses++;
    }
    else { S.lastJ[0] = S.lastJ[1] = S.lastJ[2] = 0.f; S.judge = false; }
    S.gap = dl;
    if (g_DragsLogged <= 5 && S.traces < 60 && now - S.t0 < 1.5)
    {   // the first drags, tick by tick: does an impulse move the part, and how much
        S.traces++;
        LOGI("BodyDrag: body {:#x} at {:.2f} s: joint {} at ({:.2f} {:.2f} {:.2f}), {:.0f} cm from the hand, moving {:.2f} m/s ({:.2f} {:.2f} {:.2f}); the last impulse gave {:.2f} m/s along it; impulse now {:.1f} ({:.1f} {:.1f} {:.1f}), mass {:.0f} kg",
             S.id, now - S.t0, S.joint, p[0], p[1], p[2], dl * 100.f, Len(S.vel), S.vel[0], S.vel[1], S.vel[2], S.lastAlong, jn, J[0], J[1], J[2], S.mass);
    }
    if (dl > 0.2f && now - S.lastBuzz > 0.2)
    {   // the weight on the hand
        S.lastBuzz = now;
        VRRuntime::Vibrate(S.hand, std::min(0.35f, 0.08f + 0.25f * dl), 0.03f);
    }
    if (g_DetailLogs < 120 && now - S.lastLog > 1.0)
    {
        S.lastLog = now; g_DetailLogs++;
        LOGI("BodyDrag: body {:#x}: the part {:.0f} cm from the hand, moving {:.2f} m/s, dragged {:.2f} m; impulse {:.1f} (mass taken as {:.0f} kg)",
             S.id, dl * 100.f, Len(S.vel), S.moved, jn, S.mass);
    }
}

std::string BodyDrag::Status()
{
    return fmt::format("body drag {} (drags {}, onto the back {}, impulses {}; last dragged {:.2f} m, mass {:.0f} kg)", g_On.load() ? "on" : "off",
                       g_Drags.load(), g_Lifts.load(), g_Impulses.load(), g_LastMoved.load(), g_LastMass.load());
}
