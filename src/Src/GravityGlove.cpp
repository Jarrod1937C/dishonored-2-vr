#include "pch.h"
#include "GravityGlove.h"
#include "BodyHide.h"
#include "Collision.h"
#include "GpuTrace.h"
#include "GrabHand.h"
#include "HandAimHook.h"
#include "KbmInject.h"
#include "PointerRay.h"
#include "VRRuntime.h"
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    constexpr float kMinDist = 0.8f, kMaxDist = 9.0f;      // m from the hand: nearer is grabbed by hand; further is out of reach
    constexpr float kConeDeg[2] = { 3.5f, 7.0f };          // the rays round the pointer (a small bottle at 5 m is hard to hit)
    constexpr float kFlick = 1.3f;                         // m/s of the hand back towards you / up: the flick
    constexpr float kG = 9.5f;                             // m/s^2 (measured on grenades: 9.4-9.7)
    constexpr float kMass0 = 0.8f, kMassMin = 0.05f, kMassMax = 40.f;   // kg: the first guess, bounds
    constexpr float kCatchR = 0.30f;                       // m from the hand: caught
    constexpr double kSearch = 0.35;                       // s after the grip closes to find something (then it is a grip like any)

    std::atomic<bool> g_On{ true };
    std::mutex g_M;
    struct Catch { bool ready = false; uint32_t id = 0; int kind = 0; float p[3] = {}; } g_Catch;   // under g_M
    std::atomic<bool> g_Busy{ false };
    std::atomic<unsigned long long> g_Picks{ 0 }, g_Flicks{ 0 }, g_Catches{ 0 }, g_Drops{ 0 }, g_Stuck{ 0 };
    std::atomic<float> g_LastMass{ 0.f };

    enum Phase { Idle, Aiming, Flying };
    struct St
    {
        Phase ph = Idle; bool gripWas = false; double gripAt = -1; bool searched = false;
        void* ent = nullptr; uint32_t id = 0; int kind = 0;
        double t0 = 0, T = 0.6; float mass = kMass0; bool massKnown = false;
        float c0[3] = {}, vBefore[3] = {}, J0[3] = {};
        float hPrev[3] = {}, ePrev[3] = {}; bool hPrevOk = false; float hv[3] = {}, ev[3] = {}; double lastT = 0;
        int corrections = 0; float closest = 1e9f;
        // for the log
        Collision::PullHit miss; bool missValid = false; float peakFlick = 0.f; double lastTrace = 0; int traces = 0;
    } S;
    int g_Pulls = 0, g_MissLogs = 0;   // the game thread's: pulls so far (the first are traced), searches that found nothing

    double Now() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart; }
    float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    float Len(const float* a) { return sqrtf(Dot(a, a)); }
    const char* KindName(int k) { return k == 1 ? "an object to hold" : "an item"; }

    //! the pointer's best pullable: the ray itself, else the nearest to it of the rays round it
    bool Search(int hand, const float hp[3], Collision::PullHit& out, Collision::PullHit* centre)
    {
        float o[3], f[3];
        if (!PointerRay::Get(hand, o, f)) return false;
        // two axes across the ray
        float up[3] = { 0, 0, 1 }; if (fabsf(f[2]) > 0.95f) { up[0] = 1; up[2] = 0; }
        float a[3] = { f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0] };
        float al = Len(a); for (float& v : a) v /= al;
        const float b[3] = { f[1] * a[2] - f[2] * a[1], f[2] * a[0] - f[0] * a[2], f[0] * a[1] - f[1] * a[0] };
        for (int ring = -1; ring < 2; ring++)
        {
            const int n = ring < 0 ? 1 : 8;
            for (int i = 0; i < n; i++)
            {
                float d[3] = { f[0], f[1], f[2] };
                if (ring >= 0)
                {
                    const float ang = kConeDeg[ring] / 57.29578f, t = (float)i * 6.2831853f / (float)n;
                    const float s = tanf(ang);
                    for (int k = 0; k < 3; k++) d[k] += s * (cosf(t) * a[k] + sinf(t) * b[k]);
                    const float l = Len(d); for (float& v : d) v /= l;
                }
                const float to[3] = { o[0] + d[0] * kMaxDist, o[1] + d[1] * kMaxDist, o[2] + d[2] * kMaxDist };
                Collision::PullHit h;
                const bool ok = Collision::PullRay(o, to, h);
                if (ring < 0 && centre) *centre = h;
                if (!ok) continue;
                const float dh[3] = { h.p[0] - hp[0], h.p[1] - hp[1], h.p[2] - hp[2] };
                if (Len(dh) < kMinDist) { if (ring < 0 && centre) { centre->why = "within reach of the hand (the grip takes it there)"; centre->hitEnt = h.ent; } continue; }
                out = h; return true;
            }
            (void)ring;
        }
        return false;
    }
    void ToIdle(const char* why, bool log)
    {
        if (log && S.ph != Idle) LOGI("GravityGlove: {} {:#x}: {} ({:.2f} s{})", S.ph == Flying ? "pull of" : "aim at", S.id, why, Now() - S.t0,
                                      S.ph == Flying ? fmt::format(", {} corrections, mass taken as {:.2f} kg, closest {:.0f} cm", S.corrections, S.mass, S.closest * 100.f) : std::string());
        S.ph = Idle; S.ent = nullptr; S.id = 0; g_Busy = false;
    }
    bool Center(const St& s, float c[3]) { return GrabHand::EntityCenter((int)s.id, c); }
}

void GravityGlove::SetEnabled(bool on)
{
    g_On = on;
    if (!on) g_Busy = false;
    LOGI("GravityGlove: {}", on ? "point at a loose object out of reach, squeeze the grip and flick the hand back: it flies to the hand (GravityGloves)" : "off");
}
bool GravityGlove::Enabled() { return g_On.load(); }
bool GravityGlove::Busy() { return g_Busy.load(); }
bool GravityGlove::TakeCatch(uint32_t* id, int* kind, float point[3])
{
    std::lock_guard lk(g_M);
    if (!g_Catch.ready) return false;
    *id = g_Catch.id; *kind = g_Catch.kind; memcpy(point, g_Catch.p, 12); g_Catch.ready = false;
    return true;
}

void GravityGlove::Tick()
{
    const int hand = GrabHand::Hand();
    float hp[3], hr[9], eye[3], ax[9];
    if (!g_On.load() || !GpuTrace::RawHandPose(hand, hp, hr) || !HandAimHook::RealCamera(eye, ax)) { ToIdle("off / no hand", true); S.hPrevOk = false; return; }
    const double now = Now();
    const float dt = (float)std::clamp(now - S.lastT, 1e-3, 0.1); S.lastT = now;
    if (S.hPrevOk)
    {   // the hand's speed, and the eye's (the hand's own motion is the difference: walking is not a flick)
        const float k = std::min(1.f, dt / 0.03f);
        for (int i = 0; i < 3; i++) { S.hv[i] += ((hp[i] - S.hPrev[i]) / dt - S.hv[i]) * k; S.ev[i] += ((eye[i] - S.ePrev[i]) / dt - S.ev[i]) * k; }
        const float jump[3] = { eye[0] - S.ePrev[0], eye[1] - S.ePrev[1], eye[2] - S.ePrev[2] };
        if (Len(jump) > 0.5f) { memset(S.hv, 0, 12); memset(S.ev, 0, 12); }   // a blink, a load
    }
    memcpy(S.hPrev, hp, 12); memcpy(S.ePrev, eye, 12); S.hPrevOk = true;

    // the grip, as KbmInject leaves it: closed and doing nothing else (not taking, drawing, drinking, carrying)
    const bool grip = KbmInject::GripIdle(hand);
    if (grip && !S.gripWas) { S.gripAt = now; S.searched = false; }
    S.gripWas = grip;

    switch (S.ph)
    {
    case Idle:
        if (grip && !S.searched && now - S.gripAt >= kSearch && S.missValid)
        {   // the grip closed pointing at nothing it could pull: what the pointer met, and why not (for the log)
            S.missValid = false;
            if (g_MissLogs < 300)
            {
                g_MissLogs++;
                const Collision::PullHit& m = S.miss;
                LOGI("GravityGlove: {} grip closed with nothing to pull: the pointer met {} {:.1f} m away -- {}", hand ? "right" : "left",
                     m.hitEnt ? Collision::Describe(m.hitEnt) : std::string("nothing"), m.dist, m.why ? m.why : "?");
            }
        }
        if (grip && !S.searched && now - S.gripAt < kSearch)
        {
            Collision::PullHit h, centre;
            if (!Search(hand, hp, h, &centre)) { S.miss = centre; S.missValid = true; }
            else
            {
                S.missValid = false; S.peakFlick = 0.f;
                S.searched = true; S.ph = Aiming; S.ent = h.ent; S.id = h.id; S.kind = h.kind; S.t0 = now; g_Busy = true; g_Picks++;
                VRRuntime::Vibrate(hand, 0.2f, 0.03f);
                LOGI("GravityGlove: {} hand picked {:#x} ({}, {}) {:.1f} m away: flick the hand back to pull it", hand ? "right" : "left", h.id,
                     Collision::Describe(h.ent), KindName(h.kind), h.dist);
            }
        }
        else if (!grip) S.searched = false;
        break;
    case Aiming:
    {
        if (!grip) { ToIdle(fmt::format("the grip opened before a flick (the hand's fastest back / up {:.1f} m/s; a flick is {:.1f})", S.peakFlick, kFlick).c_str(), true); break; }
        float c[3];
        if (BodyHide::EntityById((int)S.id) != S.ent || !Center(S, c)) { ToIdle("it is gone", true); break; }
        const float dc[3] = { c[0] - hp[0], c[1] - hp[1], c[2] - hp[2] };
        if (Len(dc) > kMaxDist + 2.f) { ToIdle("out of reach", true); break; }
        // the flick: the hand's own speed back towards you and up
        float back[3] = { eye[0] - c[0], eye[1] - c[1], 0.f }; const float bl = Len(back);
        if (bl > 1e-3f) { back[0] /= bl; back[1] /= bl; }
        float u[3] = { back[0] * 0.6f, back[1] * 0.6f, 0.8f }; const float ul = Len(u); for (float& v : u) v /= ul;
        const float rel[3] = { S.hv[0] - S.ev[0], S.hv[1] - S.ev[1], S.hv[2] - S.ev[2] };
        const float flick = Dot(rel, u);
        S.peakFlick = std::max(S.peakFlick, flick);
        if (flick < kFlick) break;
        // launch: a ballistic path to the hand, its time growing with the distance
        const float dist = Len(dc);
        S.T = std::clamp(0.30 + 0.07 * dist, 0.40, 0.85);
        float v[3] = {}; Collision::EntityVelocity(S.ent, v);
        const float T = (float)S.T;
        const float goal[3] = { hp[0] + S.hv[0] * 0.05f, hp[1] + S.hv[1] * 0.05f, hp[2] };
        float vdes[3] = { (goal[0] - c[0]) / T, (goal[1] - c[1]) / T, (goal[2] - c[2]) / T + 0.5f * kG * T };
        float J[3]; for (int i = 0; i < 3; i++) J[i] = S.mass * (vdes[i] - v[i]);
        const float zero[3] = {};
        Collision::Push(S.ent, J, zero);
        memcpy(S.c0, c, 12); memcpy(S.vBefore, v, 12); memcpy(S.J0, J, 12);
        S.ph = Flying; S.t0 = now; S.massKnown = false; S.corrections = 0; S.closest = dist; g_Flicks++;
        g_Pulls++; S.traces = 0; S.lastTrace = now;
        VRRuntime::Vibrate(hand, 0.4f, 0.05f);
        LOGI("GravityGlove: flick ({:.1f} m/s): {:#x} pulled from {:.1f} m, to arrive in {:.2f} s (speed asked {:.1f} m/s, impulse {:.2f} for {:.2f} kg)",
             flick, S.id, dist, S.T, Len(vdes), Len(J), S.mass);
        break;
    }
    case Flying:
    {
        float c[3], v[3] = {};
        if (BodyHide::EntityById((int)S.id) != S.ent || !Center(S, c)) { ToIdle("it is gone", true); g_Drops++; break; }
        Collision::EntityVelocity(S.ent, v);
        const float el = (float)(now - S.t0);
        const float toHand[3] = { hp[0] - c[0], hp[1] - c[1], hp[2] - c[2] };
        const float dh = Len(toHand);
        S.closest = std::min(S.closest, dh);
        if (!grip) { ToIdle("the grip opened on the way: it falls", true); g_Drops++; break; }
        if (!S.massKnown && el > 0.06f)
        {   // the first impulse's effect: the speed it gave (gravity's share put back), along it
            const float jl = Len(S.J0);
            const float dv[3] = { v[0] - S.vBefore[0], v[1] - S.vBefore[1], v[2] - S.vBefore[2] + kG * el };
            const float along = jl > 1e-4f ? Dot(dv, S.J0) / jl : 0.f;
            const float m0 = S.c0[0] - c[0], m1 = S.c0[1] - c[1], m2 = S.c0[2] - c[2];
            const float movedFrom = sqrtf(m0 * m0 + m1 * m1 + m2 * m2);
            if (along > 0.3f)
            {
                S.mass = std::clamp(jl / along, kMassMin, kMassMax); S.massKnown = true; g_LastMass = S.mass;
                LOGI("GravityGlove: {:#x} took {:.1f} m/s from an impulse of {:.2f}: its mass {:.2f} kg", S.id, along, jl, S.mass);
            }
            else if (el > 0.30f && movedFrom < 0.05f)
            {
                if (S.mass < kMassMax * 0.5f)
                {   // it did not move: much heavier than guessed (or stuck): once more, ten times harder
                    S.mass *= 10.f;
                    float again[3]; for (int i = 0; i < 3; i++) again[i] = S.J0[i] * 9.f;
                    const float zero[3] = {};
                    Collision::Push(S.ent, again, zero);
                    for (int i = 0; i < 3; i++) S.J0[i] *= 10.f;
                    S.t0 = now; memcpy(S.vBefore, v, 12);
                    LOGI("GravityGlove: {:#x} did not move: pulled again for {:.1f} kg", S.id, S.mass);
                    break;
                }
                g_Stuck++; ToIdle("it does not move (fixed, or far too heavy)", true); break;
            }
        }
        // caught: near the hand, or past it
        const float rv[3] = { v[0] - S.hv[0], v[1] - S.hv[1], v[2] - S.hv[2] };
        const bool leaving = Dot(rv, toHand) < 0.f && dh < 0.6f;
        if (dh < kCatchR || leaving)
        {
            // brake it to the hand's own speed
            float J[3]; for (int i = 0; i < 3; i++) J[i] = S.mass * (S.hv[i] - v[i]) * 0.9f;
            const float zero[3] = {};
            if (Len(J) > 1e-3f) Collision::Push(S.ent, J, zero);
            { std::lock_guard lk(g_M); g_Catch.ready = true; g_Catch.id = S.id; g_Catch.kind = S.kind; memcpy(g_Catch.p, c, 12); }
            g_Catches++;
            VRRuntime::Vibrate(hand, 0.6f, 0.06f);
            LOGI("GravityGlove: {:#x} reached the hand ({:.0f} cm, {:.2f} s): caught -- {}", S.id, dh * 100.f, el, S.kind == 1 ? "held as the grip holds it" : "in the hand, as an item taken by hand");
            ToIdle("caught", false);
            break;
        }
        if (g_Pulls <= 15 && S.traces < 40 && now - S.lastTrace >= 0.06)
        {   // the first pulls, step by step: where it is against the hand, how fast, whether it is steered
            S.lastTrace = now; S.traces++;
            LOGI("GravityGlove: {:#x} at {:.2f} s: {:.2f} m from the hand ({:.2f} {:.2f} {:.2f} to it), speed {:.1f} m/s ({:.1f} {:.1f} {:.1f}), {} corrections, mass {}",
                 S.id, el, dh, toHand[0], toHand[1], toHand[2], Len(v), v[0], v[1], v[2], S.corrections, S.massKnown ? fmt::format("{:.2f} kg", S.mass) : std::string("not measured yet"));
        }
        if (el > S.T + 1.0f || dh > kMaxDist + 3.f) { ToIdle("it did not arrive", true); g_Drops++; break; }
        if (S.massKnown)
        {   // steer: the ballistic speed that reaches the hand where it is now in the time left
            const float Tr = std::max(0.12f, (float)S.T - el);
            const float vdes[3] = { toHand[0] / Tr, toHand[1] / Tr, toHand[2] / Tr + 0.5f * kG * Tr };
            float dv[3] = { vdes[0] - v[0], vdes[1] - v[1], vdes[2] - v[2] };
            if (Len(dv) > 0.4f)
            {
                float J[3]; for (int i = 0; i < 3; i++) J[i] = S.mass * dv[i] * 0.5f;
                const float jl = Len(J), cap = S.mass * 6.f;
                if (jl > cap) for (float& x : J) x *= cap / jl;
                const float zero[3] = {};
                Collision::Push(S.ent, J, zero);
                S.corrections++;
            }
        }
        break;
    }
    }
}

std::string GravityGlove::Status()
{
    return fmt::format("gravity gloves {} (picked {}, pulled {}, caught {}, dropped {}, stuck {}; last mass {:.2f} kg)", g_On.load() ? "on" : "off",
                       g_Picks.load(), g_Flicks.load(), g_Catches.load(), g_Drops.load(), g_Stuck.load(), g_LastMass.load());
}
