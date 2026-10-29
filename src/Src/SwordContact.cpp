#include "pch.h"
#include "SwordContact.h"
#include "XRSession.h"
#include "VRRuntime.h"
#include "GameInfo.h"
#include "GpuTrace.h"
#include "BodyHide.h"
#include "GrabHand.h"
#include "HandAimHook.h"
#include "MeleeAim.h"
#include "NpcSkel.h"
#include "PlayerState.h"
#include "SwordGesture.h"
#include "Collision.h"
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <vector>

namespace
{
    // Tunables. Distances in m, speeds in m/s, times in s.
    struct Cfg
    {
        // The blade along the drawn sword model's forward (row 0 of WeaponTargetPose), from its origin (the grip).
        float guard = 0.14f, tip = 0.69f, samples = 10;   // measured on Corvo's sword: crossguard 0.16, tip 0.67
        // the blade's half thickness: what it touches is grown by this. 4 cm (was 2.5): reported, "not contacting most of
        // the time" -- a skeleton's capsules are thinner than the clothes drawn on them
        float bladeR = 0.04f;
        // A STRIKE needs a real swing of the arm, measured on the controller in the room (XR space: turning, walking and the
        // game's camera do not count): the hand faster than `hitHand` at some point of the last `hitWin` s, and travelled at
        // least `hitPath` within the last `pathWin` s. Measured on the Quest 3 at the bookcase (2026-10-04): wrist flicks
        // reached 0.1-0.9 m/s of hand (a few 1.1-1.9, but moving the hand < 15 cm) with the tip at 3-12 m/s -- and every one
        // of them that touched a book or a pane struck. A deliberate slash moves the hand at 2-5 m/s over 20-40 cm.
        float hitHand = 1.3f, hitWin = 0.15f, hitPath = 0.12f, pathWin = 0.25f;
        // ...and the blade point that enters faster than this, relative to the player (walking into a bottle with the
        // sword held out is not a strike). Low on purpose: the arm's swing is what tells a strike from a flick, and a real
        // slash that meets a bottle with the inner half of the blade enters it at ~2.5 m/s (measured in the mock).
        float minSpeed = 1.5f;
        // One strike per swing: the next needs the hand to have slowed below this (the swing is over, or it turned back).
        float rearmHand = 0.7f;
        float rearm = 0.35f;                  // and the same entity is not struck again before this
        float maxDim = 2.4f, maxVol = 1.2f;   // a box bigger than this is furniture / architecture, not a target
        float radius = 3.0f, scanSec = 0.10f; // targets: entities within this of the eye, refreshed this often
        float skelMin = 20;                   // a skinned model with at least this many joints is struck by its skeleton
        float attach = 0.30f;                 // a box touched this close to a character's body (his sword, hat) strikes him too
        // m/s: a blade point faster than this jumped (a re-pose, a turn), it did not swing. Was 25: the Index logs show
        // the tip of a real slash at 25-40 m/s, and a frame with ANY point above this tested no contact at all -- the
        // fastest part of every hard swing, the part that meets the guard, was thrown away.
        float teleport = 80.f;
        // m/s: the controller faster than this is a tracking glitch. Was 10: measured on an Index, a fast slash moves the
        // controller at 10-18 m/s (peaks near 30), so most real swings were thrown away as glitches and never struck
        // (reported: "the sword only works when I hit the NPC at the surface"). A real glitch is a jump of metres.
        float handJump = 40.f;
        float lostHold = 0.30f;               // after tracking comes back, no strike for this long
        float haptic = 1.f, touchBuzz = 0.18f;
        // The swing gesture's click (SwordGesture: the Slash and its sound in the air) waits for the swing to touch something,
        // so that the Slash the game runs is the one aimed at what the blade met; with nothing touched it clicks when the
        // swing ends (hand below rearmHand) or after airMax s. 0 = click at once (part 35).
        float airDefer = 1, airMax = 0.30f;
        // 1 = open the game's attack window at the contact instead of waiting for the Slash's own (0.2 s after the click).
        // Measured (part 35): the strike then lands 30 ms after the contact but only KNOCKS a bottle over. Research, off.
        float early = 0;
        float pendMax = 0.40f;
        float strikeWait = 0.6f, reclick = 0.15f;   // a contact's click is repeated every `reclick` s until a Slash begins
    } C;
    struct Named { const char* name; float* v; };
    const Named kCfg[] = {
        { "guard", &C.guard }, { "tip", &C.tip }, { "samples", &C.samples }, { "blader", &C.bladeR },
        { "hithand", &C.hitHand }, { "hitwin", &C.hitWin }, { "hitpath", &C.hitPath }, { "pathwin", &C.pathWin },
        { "minspeed", &C.minSpeed }, { "rearmhand", &C.rearmHand }, { "rearm", &C.rearm },
        { "maxdim", &C.maxDim }, { "maxvol", &C.maxVol }, { "radius", &C.radius }, { "scan", &C.scanSec }, { "skelmin", &C.skelMin },
        { "attach", &C.attach }, { "teleport", &C.teleport }, { "handjump", &C.handJump }, { "losthold", &C.lostHold },
        { "haptic", &C.haptic }, { "touchbuzz", &C.touchBuzz }, { "airdefer", &C.airDefer }, { "airmax", &C.airMax },
        { "early", &C.early }, { "pendmax", &C.pendMax }, { "strikewait", &C.strikeWait }, { "reclick", &C.reclick },
    };

    constexpr uint32_t kGamePtr = 0x39A5290, kEntTable = 0x3130B0, kEntSlots = 0x4000;
    constexpr uint32_t kEntHidden = 0xD0, kEntModel = 0xF0, kEntId = 0xA8, kEntScale = 0x330, kModelBoundsSlot = 0x80;
    constexpr int kMaxSamples = 24;

    std::atomic<int> g_On{ 1 };
    std::atomic<float> g_BodySink{ 0.04f };      // m: how far the drawn blade sinks into a body (WeaponCollision)

    // ---- reading the game's entities from the Present thread: everything through SEH, nothing trusted ----
    bool SafeCopy(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    using BoundsFn = const float* (*)(void*);
    bool SafeBounds(void* model, uintptr_t fn, float out[6])
    {
        __try
        {
            const float* b = ((BoundsFn)fn)(model);
            if (!b) return false;
            memcpy(out, b, 24);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool Finite(const float* v, int n) { for (int i = 0; i < n; i++) if (!std::isfinite(v[i]) || fabsf(v[i]) > 1e6f) return false; return true; }
    float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    float Len(const float* a) { return sqrtf(Dot(a, a)); }
    void Sub(const float* a, const float* b, float* o) { for (int k = 0; k < 3; k++) o[k] = a[k] - b[k]; }

    struct Target
    {
        void* ent = nullptr; uint32_t id = 0; uintptr_t vt = 0;
        float mn[3] = {}, mx[3] = {}, vol = 0;   // local box (scaled)
        float org[3] = {}, axis[9] = {};          // the entity's pose, refreshed every frame
        bool skel = false;                        // struck by its skeleton (sk, refreshed every frame), not the box
        int sk = -1;                              // index in g_Skels
    };
    struct BoundsCache { void* model; bool ok; float mn[3], mx[3]; };
    std::unordered_map<void*, BoundsCache> g_Bounds;
    std::vector<Target> g_Targets;
    std::vector<NpcSkel::Skel> g_Skels;           // this frame's skeletons, Target::sk indexes them
    double g_LastScan = -1;

    //! The entity's local box (render model bounds * entity scale), cached per (entity, model).
    bool EntityBox(uint8_t* e, float mn[3], float mx[3])
    {
        void* model = nullptr; float sc[3];
        if (!SafeCopy(&model, e + kEntModel, 8) || !model || !SafeCopy(sc, e + kEntScale, 12)) return false;
        auto it = g_Bounds.find(e);
        if (it == g_Bounds.end() || it->second.model != model)
        {
            BoundsCache c{ model, false, {}, {} };
            uintptr_t vt = 0, fn = 0; const uintptr_t base = GameInfo::Base();
            float b[6];
            if (SafeCopy(&vt, model, 8) && vt > base && vt - base < 0x4000000 && SafeCopy(&fn, (void*)(vt + kModelBoundsSlot), 8)
                && fn > base && fn - base < 0x4000000 && SafeBounds(model, fn, b) && Finite(b, 6))
            {
                c.ok = true;
                for (int i = 0; i < 3; i++)
                {
                    c.mn[i] = b[i]; c.mx[i] = b[3 + i];
                    if (c.mn[i] > c.mx[i]) std::swap(c.mn[i], c.mx[i]);
                }
            }
            if (g_Bounds.size() > 8192) g_Bounds.clear();
            it = g_Bounds.insert_or_assign(e, c).first;
        }
        if (!it->second.ok) return false;
        for (int i = 0; i < 3; i++)
        {
            const float s = std::isfinite(sc[i]) && fabsf(sc[i]) > 1e-3f && fabsf(sc[i]) < 100.f ? fabsf(sc[i]) : 1.f;
            mn[i] = it->second.mn[i] * s; mx[i] = it->second.mx[i] * s;
        }
        return true;
    }
    bool InBox(const Target& t, const float* p, float m)
    {
        const float d[3] = { p[0] - t.org[0], p[1] - t.org[1], p[2] - t.org[2] };
        for (int r = 0; r < 3; r++) { const float l = Dot(d, t.axis + 3 * r); if (l < t.mn[r] - m || l > t.mx[r] + m) return false; }
        return true;
    }
    bool RefreshPose(Target& t)
    {
        uint8_t hdr[8]; float o[3], a[9];
        if (!SafeCopy(hdr, t.ent, 8) || *(uintptr_t*)hdr != t.vt) return false;
        if (!SafeCopy(o, (uint8_t*)t.ent + BodyHide::kEntityOrigin, 12) || !SafeCopy(a, (uint8_t*)t.ent + BodyHide::kEntityAxis, 36)) return false;
        if (!Finite(o, 3) || !Finite(a, 9)) return false;
        memcpy(t.org, o, 12); memcpy(t.axis, a, 36);
        return true;
    }

    //! A world point in the XR LOCAL space (the mock's coordinates: right, up, back), through the same map the controllers
    //! come in by (HandAimHook::XrOffsetToWorld, inverted: it is a rotation) from the eye = the head. For test scripts.
    bool WorldToXr(const float eye[3], const float view[9], const float w[3], float xr[3])
    {
        const VRRuntime::HeadPose hp = VRRuntime::GetHeadPose();
        if (!hp.valid) return false;
        float M[3][3];
        for (int c = 0; c < 3; c++) { float e[3] = { 0, 0, 0 }; e[c] = 1.f; HandAimHook::XrOffsetToWorld(e, view, M[c]); }
        const float d[3] = { w[0] - eye[0], w[1] - eye[1], w[2] - eye[2] };
        for (int c = 0; c < 3; c++) xr[c] = hp.pos[c] + Dot(M[c], d);
        return true;
    }
    float g_ScanView[9] = {}; bool g_ScanViewOk = false;
    NpcSkel::Skel g_Tmp;
    //! Every entity around the eye that could be struck: not the player or what the player holds, not hidden. A skinned
    //! model with a skeleton (a character, alive or dead) is struck by its bones; anything else by its box, unless that box
    //! is furniture-sized or the player stands in it (the body's own models).
    void ScanTargets(const float eye[3], bool log)
    {
        g_Targets.clear();
        uint8_t* game = nullptr;
        if (!SafeCopy(&game, (void*)(GameInfo::Base() + kGamePtr), 8) || !game) return;
        uint8_t** table = (uint8_t**)(game + kEntTable);
        BodyHide::Anchor an[BodyHide::aCount]; BodyHide::GetAnchors(an);
        void* held = GrabHand::HeldEntity();
        float player[3] = { eye[0], eye[1], eye[2] - 1.0f };
        if (an[BodyHide::aPlayer].valid) memcpy(player, an[BodyHide::aPlayer].org, 12);
        int seen = 0;
        for (uint32_t i = 0; i < kEntSlots; i++)
        {
            uint8_t* e = nullptr;
            if (!SafeCopy(&e, table + i, 8) || !e) continue;
            seen++;
            bool own = e == held;
            for (int k = 0; k < BodyHide::aCount; k++) if (an[k].valid && an[k].ent == e) own = true;
            if (own) continue;
            Target t; t.ent = e;
            if (!SafeCopy(&t.vt, e, 8)) continue;
            uint8_t hidden = 0; SafeCopy(&hidden, e + kEntHidden, 1);
            if (hidden & 1) continue;
            if (!RefreshPose(t)) continue;
            if (const int wc = GpuTrace::WeaponClaim(t.org); wc >= 0)
            {   // drawn as part of a held weapon (its upgrades, the cartridge): the player's own
                if (log) SafeCopy(&t.id, e + kEntId, 4);
                if (log) LOGI("swordscan: ent {} id {:#x} vt exe+{:#x} at ({:.2f},{:.2f},{:.2f}) -- skipped: drawn with the {} hand's weapon", (void*)e, t.id, t.vt - GameInfo::Base(), t.org[0], t.org[1], t.org[2], wc ? "right" : "left");
                continue;
            }
            const float d[3] = { t.org[0] - eye[0], t.org[1] - eye[1], t.org[2] - eye[2] };
            if (Dot(d, d) > (C.radius + C.maxDim) * (C.radius + C.maxDim)) continue;
            if (!EntityBox(e, t.mn, t.mx)) continue;
            const float sx = t.mx[0] - t.mn[0], sy = t.mx[1] - t.mn[1], sz = t.mx[2] - t.mn[2];
            t.vol = sx * sy * sz;
            const float big = fmaxf(sx, fmaxf(sy, sz));
            SafeCopy(&t.id, e + kEntId, 4);
            t.skel = NpcSkel::Get(e, g_Tmp) && g_Tmp.n >= (int)C.skelMin;
            const char* why = nullptr;
            float w[3];
            if (t.skel)
            {   // reach: the nearest joint within the radius of the eye
                float best = 1e9f;
                for (int j = 0; j < g_Tmp.n; j++) { float dj[3]; Sub(g_Tmp.p[j], eye, dj); best = fminf(best, Dot(dj, dj)); }
                if (best > C.radius * C.radius) continue;
                // where to aim at it (test scripts): the trunk's centre, or the joints' if it has no trunk
                int nt = 0; float acc[3] = {};
                for (int j = 0; j < g_Tmp.n; j++) if (g_Tmp.r[j] >= 0.15f) { nt++; for (int k = 0; k < 3; k++) acc[k] += g_Tmp.p[j][k]; }
                if (!nt) for (int j = 0; j < g_Tmp.n; j++) { nt++; for (int k = 0; k < 3; k++) acc[k] += g_Tmp.p[j][k]; }
                for (int k = 0; k < 3; k++) w[k] = acc[k] / (float)nt;
            }
            else
            {
                if (big < 0.02f) why = "tiny";
                else if (big > C.maxDim || t.vol > C.maxVol) why = "too big";
                else if (InBox(t, eye, 0.05f) || InBox(t, player, 0.05f)) why = "the player is inside it";
                const float c[3] = { 0.5f * (t.mn[0] + t.mx[0]), 0.5f * (t.mn[1] + t.mx[1]), 0.5f * (t.mn[2] + t.mx[2]) };
                for (int k = 0; k < 3; k++) w[k] = t.org[k] + c[0] * t.axis[k] + c[1] * t.axis[3 + k] + c[2] * t.axis[6 + k];
                const float dc[3] = { w[0] - eye[0], w[1] - eye[1], w[2] - eye[2] };
                if (!why && Dot(dc, dc) > C.radius * C.radius) continue;
            }
            if (log)
            {
                float xr[3] = {}; const bool okx = g_ScanViewOk && WorldToXr(eye, g_ScanView, w, xr);
                LOGI("swordscan: ent {} id {:#x} vt exe+{:#x} at ({:.2f},{:.2f},{:.2f}) {:.2f} m: {} {:.2f} x {:.2f} x {:.2f} vol {:.3f} centre xr {} {:.3f} {:.3f} {:.3f}{}{}",
                     (void*)e, t.id, t.vt - GameInfo::Base(), t.org[0], t.org[1], t.org[2], sqrtf(Dot(d, d)),
                     t.skel ? fmt::format("SKELETON {} joints{} spanning", g_Tmp.n, g_Tmp.humanoid ? " (humanoid)" : "") : std::string("box"),
                     t.skel ? g_Tmp.hi[0] - g_Tmp.lo[0] : sx, t.skel ? g_Tmp.hi[1] - g_Tmp.lo[1] : sy, t.skel ? g_Tmp.hi[2] - g_Tmp.lo[2] : sz,
                     t.vol, okx, xr[0], xr[1], xr[2], why ? " -- skipped: " : "", why ? why : "");
            }
            if (!why) g_Targets.push_back(t);
            if (g_Targets.size() >= 256) break;
        }
        if (log) LOGI("swordscan: {} entities in the table, {} targets within {:.1f} m", seen, g_Targets.size(), C.radius);
    }

    //! Segment p0->p1 against the box (local frame of t, grown by m): entry fraction in [0,1], or -1. Starts inside = -1
    //! (a contact is an ENTRY: the blade resting in a box and then moving away is not a strike).
    float SegEntry(const Target& t, const float* p0, const float* p1, float m)
    {
        float a[3], b[3];
        const float d0[3] = { p0[0] - t.org[0], p0[1] - t.org[1], p0[2] - t.org[2] }, d1[3] = { p1[0] - t.org[0], p1[1] - t.org[1], p1[2] - t.org[2] };
        for (int r = 0; r < 3; r++) { a[r] = Dot(d0, t.axis + 3 * r); b[r] = Dot(d1, t.axis + 3 * r); }
        float t0 = 0.f, t1 = 1.f; bool inside = true;
        for (int r = 0; r < 3; r++)
        {
            const float lo = t.mn[r] - m, hi = t.mx[r] + m, dv = b[r] - a[r];
            if (a[r] < lo || a[r] > hi) inside = false;
            if (fabsf(dv) < 1e-7f) { if (a[r] < lo || a[r] > hi) return -1.f; continue; }
            float e0 = (lo - a[r]) / dv, e1 = (hi - a[r]) / dv;
            if (e0 > e1) std::swap(e0, e1);
            t0 = fmaxf(t0, e0); t1 = fminf(t1, e1);
            if (t0 > t1) return -1.f;
        }
        return inside ? -1.f : t0;
    }

    // ---- the skeleton as capsules: each joint to the joint it is linked to, radius r[j] (+ the blade's) ----
    float PointSegDist2(const float* p, const float* a, const float* b)
    {
        float ab[3], ap[3]; Sub(b, a, ab); Sub(p, a, ap);
        const float l = Dot(ab, ab);
        float s = l > 1e-9f ? Dot(ap, ab) / l : 0.f; s = fminf(1.f, fmaxf(0.f, s));
        const float q[3] = { a[0] + ab[0] * s - p[0], a[1] + ab[1] * s - p[1], a[2] + ab[2] * s - p[2] };
        return Dot(q, q);
    }
    //! Inside the body (any capsule) at p? Returns the joint, or -1.
    int InSkel(const NpcSkel::Skel& s, const float* p, float grow)
    {
        for (int j = 0; j < s.n; j++)
        {
            const float R = s.r[j] + grow;
            const float* b = s.link[j] >= 0 ? s.p[s.link[j]] : s.p[j];
            if (PointSegDist2(p, s.p[j], b) < R * R) return j;
        }
        return -1;
    }
    //! WeaponCollision: the raw blade inside a character's bone capsules becomes contact planes for Collision, so the drawn
    //! blade stops at the body's surface (the strike itself is the raw blade's entry, as before). It always stops: each
    //! blade point keeps the side it came in from (its first contact's normal) for as long as it stays inside the body, so
    //! a fast swing whose raw blade crosses a bone's axis is not pushed out the far side (it used to pass through then,
    //! and a point deeper than 22 cm was let go: reported, "it should always collide").
    float g_EntryN[16][3]; bool g_EntryOn[16] = {};
    Collision::Contact g_BladeCs[16]; int g_BladeN = 0;          // this tick's blade contacts (the right hand's)
    Collision::Contact g_HandCs[2][16]; int g_HandN[2] = {};     // this tick's hand contacts (HandBodies)
    void BodyContacts(const float (*cur)[3], int n)
    {
        Collision::Contact cs[16]; int nc = 0;
        float hr[9], ht[3];
        bool inside[16] = {};
        if (n > 16) n = 16;
        if (Collision::WeaponsOn() && GpuTrace::RawHandPose(1, ht, hr))
        {
            for (const NpcSkel::Skel& s : g_Skels)
                for (int i = 0; i < n && nc < 16; i++)
                {
                    const float* p = cur[i];
                    bool inReach = true;
                    for (int k = 0; k < 3; k++) if (p[k] < s.lo[k] - 0.4f || p[k] > s.hi[k] + 0.4f) inReach = false;
                    if (!inReach) continue;
                    int bj = -1; float bpen = 0.f; float bq[3] = {};
                    for (int j = 0; j < s.n; j++)
                    {
                        const float* a = s.p[j]; const float* b = s.link[j] >= 0 ? s.p[s.link[j]] : s.p[j];
                        float ab[3], ap[3]; Sub(b, a, ab); Sub(p, a, ap);
                        const float l = Dot(ab, ab);
                        float u = l > 1e-9f ? Dot(ap, ab) / l : 0.f; u = fminf(1.f, fmaxf(0.f, u));
                        const float q[3] = { a[0] + ab[0] * u, a[1] + ab[1] * u, a[2] + ab[2] * u };
                        float d[3]; Sub(p, q, d);
                        const float pen = s.r[j] + C.bladeR - Len(d);
                        if (pen > bpen) { bpen = pen; bj = j; memcpy(bq, q, 12); }
                    }
                    if (bj < 0) continue;
                    inside[i] = true;
                    // the plane sits `sink` inside the bone's surface: the blade goes a little into the body before it stops
                    const float sink = fminf(g_BodySink.load(), 0.8f * s.r[bj]);
                    if (bpen <= sink && !g_EntryOn[i]) continue;    // not that deep yet: nothing to stop
                    float nrm[3]; Sub(p, bq, nrm); const float nl = Len(nrm);
                    float un[3];
                    if (g_EntryOn[i]) memcpy(un, g_EntryN[i], 12);   // the side it came in from
                    else
                    {
                        if (nl < 1e-3f) continue;
                        for (int k = 0; k < 3; k++) un[k] = nrm[k] / nl;
                        memcpy(g_EntryN[i], un, 12); g_EntryOn[i] = true;
                    }
                    Collision::Contact& c = cs[nc++];
                    for (int k = 0; k < 3; k++) { c.n[k] = un[k]; c.p[k] = bq[k] + c.n[k] * (s.r[bj] - sink); }
                    c.clear = C.bladeR;
                    const float d[3] = { p[0] - ht[0], p[1] - ht[1], p[2] - ht[2] };
                    for (int k = 0; k < 3; k++) c.local[k] = hr[k] * d[0] + hr[3 + k] * d[1] + hr[6 + k] * d[2];
                }
        }
        for (int i = 0; i < 16; i++) if (!inside[i]) g_EntryOn[i] = false;   // out of every body: the next entry decides again
        memcpy(g_BladeCs, cs, sizeof(Collision::Contact) * nc); g_BladeN = nc;
    }
    // [Gameplay] HandCollision: the hands stop at characters' bodies as the blade does -- reported, "my hands pass
    // through guards", and a knockout blow felt floaty for it. Each hand's palm and fingers (the points Collision uses
    // for the world) against the bone capsules of the characters near; a point keeps the side it came in from while it
    // stays inside. Not on a body that is down (a ragdoll: the hands carry and search those).
    float g_HandEntryN[2][4][3]; bool g_HandEntryOn[2][4] = {};
    double g_HandBodyLog = -10;
    void HandBodies(double t)
    {
        static const float kLocal[4][3] = { { 0.f, 0.f, 0.f }, { 0.06f, 0.f, -0.005f }, { 0.095f, 0.022f, -0.01f }, { 0.083f, -0.022f, -0.01f } };
        static const float kR[4] = { 0.045f, 0.025f, 0.018f, 0.018f };
        for (int h = 0; h < 2; h++)
        {
            g_HandN[h] = 0;
            float hr[9], ht[3];
            bool inside[4] = {};
            if (GpuTrace::RawHandPose(h, ht, hr))
            {
                for (const Target& tg : g_Targets)
                {
                    if (!tg.skel || tg.sk < 0 || tg.sk >= (int)g_Skels.size()) continue;
                    uint8_t f = 0; SafeCopy(&f, (const uint8_t*)tg.ent + 0xD1, 1);
                    if (f & 0x28) continue;
                    const NpcSkel::Skel& s = g_Skels[tg.sk];
                    for (int i = 0; i < 4 && g_HandN[h] < 16; i++)
                    {
                        float p[3]; for (int k = 0; k < 3; k++) p[k] = hr[k * 3] * kLocal[i][0] + hr[k * 3 + 1] * kLocal[i][1] + hr[k * 3 + 2] * kLocal[i][2] + ht[k];
                        bool inReach = true;
                        for (int k = 0; k < 3; k++) if (p[k] < s.lo[k] - 0.4f || p[k] > s.hi[k] + 0.4f) inReach = false;
                        if (!inReach) continue;
                        int bj = -1; float bpen = 0.f; float bq[3] = {};
                        for (int j = 0; j < s.n; j++)
                        {
                            const float* a = s.p[j]; const float* b = s.link[j] >= 0 ? s.p[s.link[j]] : s.p[j];
                            float ab[3], ap[3]; Sub(b, a, ab); Sub(p, a, ap);
                            const float l = Dot(ab, ab);
                            float u = l > 1e-9f ? Dot(ap, ab) / l : 0.f; u = fminf(1.f, fmaxf(0.f, u));
                            const float q[3] = { a[0] + ab[0] * u, a[1] + ab[1] * u, a[2] + ab[2] * u };
                            float d[3]; Sub(p, q, d);
                            const float pen = s.r[j] + kR[i] - Len(d);
                            if (pen > bpen) { bpen = pen; bj = j; memcpy(bq, q, 12); }
                        }
                        if (bj < 0) continue;
                        inside[i] = true;
                        float un[3];
                        if (g_HandEntryOn[h][i]) memcpy(un, g_HandEntryN[h][i], 12);
                        else
                        {
                            float nrm[3]; Sub(p, bq, nrm); const float nl = Len(nrm);
                            if (nl < 1e-3f) continue;
                            for (int k = 0; k < 3; k++) un[k] = nrm[k] / nl;
                            memcpy(g_HandEntryN[h][i], un, 12); g_HandEntryOn[h][i] = true;
                            if (t - g_HandBodyLog > 2.0)
                            {
                                g_HandBodyLog = t;
                                LOGI("SwordContact: {} hand stopped at the body of {} ({:.0f} cm in)", h ? "right" : "left", Collision::Describe(tg.ent), bpen * 100.f);
                            }
                        }
                        Collision::Contact& c = g_HandCs[h][g_HandN[h]++];
                        for (int k = 0; k < 3; k++) { c.n[k] = un[k]; c.p[k] = bq[k] + un[k] * s.r[bj]; }
                        c.clear = kR[i];
                        memcpy(c.local, kLocal[i], 12);
                    }
                }
            }
            for (int i = 0; i < 4; i++) if (!inside[i]) g_HandEntryOn[h][i] = false;
        }
    }
    //! The body contacts of this tick to Collision: each hand's own, and the blade's with the right hand's.
    void PublishBodies()
    {
        Collision::Contact cs[16]; int n = 0;
        for (int i = 0; i < g_HandN[1] && n < 16; i++) cs[n++] = g_HandCs[1][i];
        for (int i = 0; i < g_BladeN && n < 16; i++) cs[n++] = g_BladeCs[i];
        Collision::SetBodyContacts(1, cs, n);
        Collision::SetBodyContacts(0, g_HandCs[0], g_HandN[0]);
    }
    //! The characters near and their bones this frame (once per tick: the hands and the blade share them).
    double g_BodiesAt = -1;
    void RefreshBodies(const float eye[3], double t)
    {
        if (g_BodiesAt == t) return;
        g_BodiesAt = t;
        if (g_LastScan < 0 || t - g_LastScan > C.scanSec || t < g_LastScan) { ScanTargets(eye, false); g_LastScan = t; }
        g_Skels.clear(); g_Skels.reserve(8);
        for (size_t i = 0; i < g_Targets.size();)
        {
            Target& tg = g_Targets[i];
            bool ok = RefreshPose(tg);
            if (ok && tg.skel)
            {   // the bones as they are this frame (a guard swinging at you, a body falling)
                g_Skels.emplace_back();
                if (NpcSkel::Get(tg.ent, g_Skels.back())) tg.sk = (int)g_Skels.size() - 1;
                else { g_Skels.pop_back(); ok = false; }
            }
            if (ok) i++; else { g_Targets[i] = g_Targets.back(); g_Targets.pop_back(); }
        }
    }
    float SkelDist(const NpcSkel::Skel& s, const float* p)
    {   // distance from the body's surface (negative inside)
        float best = 1e9f;
        for (int j = 0; j < s.n; j++)
        {
            const float* b = s.link[j] >= 0 ? s.p[s.link[j]] : s.p[j];
            best = fminf(best, sqrtf(PointSegDist2(p, s.p[j], b)) - s.r[j]);
        }
        return best;
    }
    //! A blade point moving p0 -> p1 entering the body: the entry fraction (bisection on "inside"), or -1 (no entry, or it
    //! started inside).
    float SkelEntry(const NpcSkel::Skel& s, const float* p0, const float* p1, float grow, int* joint)
    {
        // quick reject on the joints' bounds
        for (int k = 0; k < 3; k++)
        {
            const float lo = fminf(p0[k], p1[k]), hi = fmaxf(p0[k], p1[k]);
            if (hi < s.lo[k] - 0.3f || lo > s.hi[k] + 0.3f) return -1.f;
        }
        if (InSkel(s, p0, grow) >= 0) return -1.f;
        // march: a capsule is >= 7 cm thick, the step is below half of that
        float d[3]; Sub(p1, p0, d);
        const int steps = std::max(1, std::min(24, (int)ceilf(Len(d) / 0.03f)));
        float prevT = 0.f;
        for (int i = 1; i <= steps; i++)
        {
            const float t = (float)i / (float)steps;
            const float q[3] = { p0[0] + d[0] * t, p0[1] + d[1] * t, p0[2] + d[2] * t };
            const int j = InSkel(s, q, grow);
            if (j >= 0)
            {
                float lo = prevT, hi = t;
                for (int it = 0; it < 6; it++)
                {
                    const float m = 0.5f * (lo + hi);
                    const float r[3] = { p0[0] + d[0] * m, p0[1] + d[1] * m, p0[2] + d[2] * m };
                    if (InSkel(s, r, grow) >= 0) hi = m; else lo = m;
                }
                if (joint) *joint = j;
                return hi;
            }
            prevT = t;
        }
        return -1.f;
    }

    // ---- the hand in the room (XR space): is this a swing? ----
    struct HandSample { double t; float p[3]; };
    constexpr int kHist = 48;
    HandSample g_H[kHist]; int g_HN = 0, g_HHead = -1;
    double g_LostUntil = -1; unsigned g_Gen = 0;
    bool g_Armed = true;
    float g_HandSpeed = 0, g_HandPeak = 0, g_HandPath = 0; bool g_SwingOk = false;
    void HandClear() { g_HN = 0; g_HHead = -1; }
    void HandTrack(const XRInputState& in, double t)
    {
        const XRHandPose& h = in.hand[1];
        const VRRuntime::HeadPose hp = VRRuntime::GetHeadPose();
        if (!h.valid || h.held) { HandClear(); g_LostUntil = t + C.lostHold; g_HandSpeed = g_HandPeak = g_HandPath = 0; g_SwingOk = false; return; }
        if (hp.valid && hp.gen != g_Gen) { g_Gen = hp.gen; HandClear(); g_LostUntil = t + C.lostHold; }
        const double ht = (double)in.time * 1e-9;
        if (g_HHead >= 0 && ht <= g_H[g_HHead].t) return;   // no new pose
        HandSample s{ ht, { h.posX, h.posY, h.posZ } };
        // speed over >= 18 ms
        float v = -1.f;
        for (int k = 0; k < g_HN; k++)
        {
            const HandSample& o = g_H[(g_HHead - k + kHist) % kHist];
            const double dt = ht - o.t;
            if (dt < 0.018 && k < g_HN - 1) continue;
            if (dt <= 1e-4 || dt > 0.1) break;
            float d[3]; Sub(s.p, o.p, d); v = Len(d) / (float)dt; break;
        }
        if (v > C.handJump)
        {   // a tracking glitch: forget the motion, hold strikes off a moment
            LOGI("SwordContact: hand jump ignored ({:.1f} m/s)", v);
            HandClear(); g_LostUntil = t + C.lostHold;
        }
        g_HHead = (g_HHead + 1) % kHist; g_H[g_HHead] = s; if (g_HN < kHist) g_HN++;
        if (v >= 0.f) g_HandSpeed = v;
        // peak speed over hitWin (from consecutive samples >= 18 ms apart), chord over pathWin
        float peak = g_HandSpeed, chord = 0.f;
        const HandSample* last = &g_H[g_HHead];
        for (int k = 1; k < g_HN; k++)
        {
            const HandSample& o = g_H[(g_HHead - k + kHist) % kHist];
            const double age = ht - o.t;
            if (age > fmax(C.hitWin, C.pathWin)) break;
            float d[3]; Sub(s.p, o.p, d);
            if (age <= C.pathWin) chord = fmaxf(chord, Len(d));
            const double dt = last->t - o.t;
            if (age <= C.hitWin && dt >= 0.018) { float e[3]; Sub(last->p, o.p, e); peak = fmaxf(peak, Len(e) / (float)dt); last = &o; }
        }
        g_HandPeak = peak; g_HandPath = chord;
        g_SwingOk = peak >= C.hitHand && chord >= C.hitPath;
        if (g_HandSpeed < C.rearmHand) g_Armed = true;
    }

    // ---- per-frame state (Present thread) ----
    float g_Prev[kMaxSamples][3]; bool g_PrevValid = false; int g_PrevN = 0; long long g_PrevT = 0;
    float g_PrevPlayer[3] = {}; bool g_PrevPlayerOk = false;
    std::unordered_map<void*, double> g_LastStruck;
    std::unordered_map<void*, double> g_LastTouchBuzz;
    double g_TapUntil = -1;                  // a contact is waiting for the running Slash to end before it clicks
    double g_AirAt = -1;                     // a swing gesture's click waiting for the swing to touch something
    double g_LastStrikeAt = -10;
    // A contact's click must START a Slash: measured in a fight, a click 30 ms after a Slash the guard cut short (0.31 s)
    // started nothing -- the player was in the recoil. The strike is clicked again until a Slash begins (or strikeWait).
    double g_AwaitSince = -1, g_LastClick = -1; unsigned g_AwaitSerial = 0; int g_Reclicks = 0;
    unsigned long long g_ReclickTotal = 0, g_StrikeLost = 0, g_StrikeStarted = 0;
    uint64_t g_SeenHits = 0, g_SeenAccepted = 0; double g_ImpactAt = -10;
    // statistics / debug
    unsigned long long g_Contacts = 0, g_Touches = 0, g_Taps = 0, g_Opens = 0, g_Releases = 0, g_Teleports = 0, g_Frames = 0;
    unsigned long long g_RejSwing = 0, g_RejArmed = 0, g_RejSlow = 0, g_RejLost = 0, g_AirTaps = 0, g_AirDropped = 0, g_Queued = 0, g_Impacts = 0, g_SkelHits = 0, g_Attach = 0;
    std::mutex g_DbgM; SwordContact::Debug g_Dbg; double g_DbgContactAt = -1;
    double g_LastContactAt = -1; float g_LastSpeed = 0; uint32_t g_LastId = 0;

    double Now() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart / (double)f.QuadPart; }
    void Buzz(float amp, float sec) { if (C.haptic > 0.f && amp > 0.f) VRRuntime::Vibrate(1, fminf(1.f, amp * C.haptic), sec); }
    void Corners(const Target& t, float out[8][3])
    {
        for (int c = 0; c < 8; c++)
        {
            const float l[3] = { (c & 1) ? t.mx[0] : t.mn[0], (c & 2) ? t.mx[1] : t.mn[1], (c & 4) ? t.mx[2] : t.mn[2] };
            for (int k = 0; k < 3; k++) out[c][k] = t.org[k] + l[0] * t.axis[k] + l[1] * t.axis[3 + k] + l[2] * t.axis[6 + k];
        }
    }
    void HitBoxOf(const Target& tg, MeleeAim::HitBox& hb)
    {
        hb.ent = tg.ent;
        if (tg.skel && tg.sk >= 0)
        {   // the joints' world bounds (used only when a candidate's body does not resolve to an entity)
            const NpcSkel::Skel& s = g_Skels[tg.sk];
            const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
            memset(hb.org, 0, 12); memcpy(hb.axis, I, 36);
            for (int k = 0; k < 3; k++) { hb.mn[k] = s.lo[k] - 0.12f; hb.mx[k] = s.hi[k] + 0.12f; }
        }
        else { memcpy(hb.org, tg.org, 12); memcpy(hb.axis, tg.axis, 36); memcpy(hb.mn, tg.mn, 12); memcpy(hb.mx, tg.mx, 12); }
    }
}

void SwordContact::Reset() { g_PrevValid = false; g_PrevPlayerOk = false; g_TapUntil = -1; g_AirAt = -1; HandClear(); }
void SwordContact::SetBodySink(float m)
{
    g_BodySink = fminf(0.15f, fmaxf(0.f, m));
    LOGI("SwordContact: with WeaponCollision the blade sinks {:.0f} cm into a body before it stops", g_BodySink.load() * 100.f);
}
void SwordContact::SetEnabled(int on) { g_On = on; Reset(); LOGI("SwordContact: {}", on ? "on (the blade strikes what it touches)" : "off"); }
int SwordContact::Enabled() { return g_On.load(); }

bool SwordContact::DeferAirTap()
{
    if (!g_On.load() || C.airDefer < 0.5f) return false;
    // a strike already carried this swing (the blade met something before the gesture was recognised): no second click
    if (Now() - g_LastStrikeAt < 0.5) { g_AirDropped++; return true; }
    g_AirAt = Now();
    return true;
}

SwordContact::Out SwordContact::Tick(const XRInputState& in, bool active)
{
    Out o;
    const double t = Now();
    auto awaitSlash = [&]() { g_AwaitSince = t; g_LastClick = t; g_AwaitSerial = MeleeAim::SlashSerial(); g_Reclicks = 0; };
    // A blade strike (SwordBladeDamage) that could not be made -- no record of the character came back, or the game
    // refused it: the game's attack instead, as without it.
    static unsigned bladeSerial = 0;
    if (bladeSerial)
    {
        const int st = MeleeAim::BladeStrikeState(bladeSerial);
        if (st == 2 || st == 4) bladeSerial = 0;   // landed, or the guard blocked it: the block stands
        else if (st == 3)
        {
            bladeSerial = 0;
            if (!MeleeAim::SlashRunning() && g_TapUntil < 0) { o.tap = true; g_Taps++; awaitSlash(); LOGI("SwordContact: the blade strike could not be made: the game's attack instead"); }
        }
    }
    // A contact that came while the running Slash could not carry it any more: click as soon as that Slash is over.
    if (g_TapUntil >= 0)
    {
        if (t > g_TapUntil) { g_TapUntil = -1; g_StrikeLost++; LOGI("SwordContact: the queued strike expired (the Slash never ended)"); }
        else if (!MeleeAim::SlashRunning()) { o.tap = true; g_Taps++; g_TapUntil = -1; awaitSlash(); LOGI("SwordContact: the queued strike clicks now (the previous Slash ended)"); }
    }
    // A contact's click that has not started a Slash yet: click again while the game is not swinging.
    if (g_AwaitSince >= 0 && !o.tap)
    {
        if (MeleeAim::SlashSerial() != g_AwaitSerial)
        {
            g_StrikeStarted++;
            if (g_Reclicks) LOGI("SwordContact: the strike's Slash began {:.0f} ms after the contact ({} extra clicks)", (t - g_AwaitSince) * 1000.0, g_Reclicks);
            g_AwaitSince = -1;
        }
        else if (t - g_AwaitSince > C.strikeWait)
        {
            g_StrikeLost++; g_AwaitSince = -1;
            LOGI("SwordContact: the strike never started a Slash ({} clicks in {:.0f} ms; states: {})", g_Reclicks + 1, C.strikeWait * 1000.f, PlayerState::Stats());
        }
        else if (!MeleeAim::SlashRunning() && t - g_LastClick > C.reclick)
        {
            o.tap = true; g_LastClick = t; g_Reclicks++; g_ReclickTotal++;
            LOGI("SwordContact: the strike's click started no Slash: clicking again (states: {})", PlayerState::Stats());
        }
    }
    // A strike that landed: the impact in the hand.
    // A strike that landed (a character: the game closed the window on the hit; an object: a candidate of what the blade
    // touched was let through, which is where a bottle breaks): the impact in the hand, once per strike.
    {
        const uint64_t h = MeleeAim::Hits(), a = MeleeAim::Accepted();
        if ((h != g_SeenHits || a != g_SeenAccepted) && t - g_LastStrikeAt < 1.0 && g_ImpactAt < g_LastStrikeAt) { Buzz(1.f, 0.07f); g_Impacts++; g_ImpactAt = t; }
        g_SeenHits = h; g_SeenAccepted = a;
    }
    HandTrack(in, t);
    // the bodies the hands (and below, the blade) stop at: published on every way out of this tick
    g_BladeN = 0;
    struct PublishAtEnd { ~PublishAtEnd() { PublishBodies(); } } publishAtEnd;
    {
        float he[3], hv[9];
        if (Collision::HandsOn() && HandAimHook::RealCamera(he, hv)) { RefreshBodies(he, t); HandBodies(t); }
        else g_HandN[0] = g_HandN[1] = 0;
    }
    if (!g_On.load() || !active) { g_PrevValid = false; g_AirAt = -1; return o; }

    float wo[3], wa[9], eye[3], view[9];
    // the blade where the REAL hand holds it: Collision may stop the drawn one at a body, and the strike is still the swing's
    const bool sword = GpuTrace::HandItemDraws(1) != 0 && GpuTrace::WeaponTargetPoseRaw(1, wo, wa);
    if (!sword || !HandAimHook::RealCamera(eye, view)) { g_PrevValid = false; g_AirAt = -1; std::lock_guard lk(g_DbgM); g_Dbg.blade = false; return o; }
    g_Frames++;

    RefreshBodies(eye, t);

    const int n = (int)fminf(fmaxf(C.samples, 2.f), (float)kMaxSamples);
    float cur[kMaxSamples][3];
    for (int i = 0; i < n; i++)
    {
        const float s = C.guard + (C.tip - C.guard) * (float)i / (float)(n - 1);
        for (int k = 0; k < 3; k++) cur[i][k] = wo[k] + wa[k] * s;
    }
    BodyContacts(cur, n);
    LARGE_INTEGER qc, qf; QueryPerformanceCounter(&qc); QueryPerformanceFrequency(&qf);
    const double dt = g_PrevValid ? (double)(qc.QuadPart - g_PrevT) / (double)qf.QuadPart : 0.0;
    // the player's own motion this frame: the blade's speed that counts is relative to the body
    float player[3] = {}, pd[3] = {}; bool playerOk = false;
    { BodyHide::Anchor an[BodyHide::aCount]; BodyHide::GetAnchors(an); if (an[BodyHide::aPlayer].valid) { memcpy(player, an[BodyHide::aPlayer].org, 12); playerOk = true; } }
    if (playerOk && g_PrevPlayerOk) { Sub(player, g_PrevPlayer, pd); if (Len(pd) > 1.f) memset(pd, 0, 12); }

    // The sweep: each blade point from where it was to where it is. The earliest entry wins; among entries at the same
    // moment a body before a box, then the smallest box (the bottle, not the shelf it stands on).
    int best = -1; float bestT = 2.f, bestVol = 1e9f, bestSpeed = 0; float bestPt[3] = {}, bestVel[3] = {}; bool bestSkel = false;
    bool jumped = false; float jumpV = 0.f;
    if (g_PrevValid && g_PrevN == n && dt > 1e-4 && dt < 0.1)
    {
        for (int i = 0; i < n; i++)
        {
            float d[3]; Sub(cur[i], g_Prev[i], d); for (int k = 0; k < 3; k++) d[k] -= pd[k];
            if (Dot(d, d) > C.teleport * C.teleport * (float)(dt * dt)) { jumped = true; jumpV = std::max(jumpV, Len(d) / (float)dt); }
        }
        if (jumped)
        {
            g_Teleports++;
            static double logAt = -10;
            const double tn = (double)qc.QuadPart / (double)qf.QuadPart;
            if (tn - logAt > 2.0) { logAt = tn; LOGI("SwordContact: a blade point moved {:.0f} m/s in one frame (over {:.0f}): a jump, no contact tested this frame ({} so far)", jumpV, C.teleport, g_Teleports); }
        }
        for (size_t ti = 0; ti < g_Targets.size() && !jumped; ti++)
        {
            const Target& tg = g_Targets[ti];
            for (int i = 0; i < n; i++)
            {
                const float e = tg.skel && tg.sk >= 0 ? SkelEntry(g_Skels[tg.sk], g_Prev[i], cur[i], C.bladeR, nullptr)
                                                      : SegEntry(tg, g_Prev[i], cur[i], C.bladeR);
                if (e < 0.f) continue;
                float d[3]; Sub(cur[i], g_Prev[i], d);
                const float rel[3] = { d[0] - pd[0], d[1] - pd[1], d[2] - pd[2] };
                const float speed = Len(rel) / (float)dt;
                const float vol = tg.skel ? 0.f : tg.vol;
                const bool better = e < bestT - 0.05f || (fabsf(e - bestT) <= 0.05f && (tg.skel && !bestSkel || (tg.skel == bestSkel && vol < bestVol)));
                if (best < 0 || better)
                {
                    best = (int)ti; bestT = e; bestVol = vol; bestSpeed = speed; bestSkel = tg.skel;
                    for (int k = 0; k < 3; k++) { bestPt[k] = g_Prev[i][k] + d[k] * e; bestVel[k] = rel[k] / (float)dt; }
                }
            }
        }
    }
    memcpy(g_Prev, cur, sizeof(float) * 3 * n); g_PrevN = n; g_PrevValid = true; g_PrevT = qc.QuadPart;
    memcpy(g_PrevPlayer, player, 12); g_PrevPlayerOk = playerOk;

    if (best >= 0)
    {
        const Target& tg = g_Targets[best];
        auto it = g_LastStruck.find(tg.ent);
        const bool recent = it != g_LastStruck.end() && t - it->second < C.rearm;
        const bool lost = t < g_LostUntil;
        const char* why = lost ? "tracking just came back" : !g_SwingOk ? "not a swing of the arm" : !g_Armed ? "this swing already struck"
                        : bestSpeed < C.minSpeed ? "blade too slow" : recent ? "struck a moment ago" : nullptr;
        if (why)
        {
            g_Touches++;
            if (lost) g_RejLost++; else if (!g_SwingOk) g_RejSwing++; else if (!g_Armed) g_RejArmed++; else if (bestSpeed < C.minSpeed) g_RejSlow++;
            // the blade brushing something: a light touch in the hand, once per thing per 0.3 s
            auto bz = g_LastTouchBuzz.find(tg.ent);
            if (bz == g_LastTouchBuzz.end() || t - bz->second > 0.3) { Buzz(C.touchBuzz, 0.02f); g_LastTouchBuzz[tg.ent] = t; if (g_LastTouchBuzz.size() > 64) g_LastTouchBuzz.clear(); }
            static double lastLog = -10;
            if (t - lastLog > 0.4) { lastLog = t; LOGI("SwordContact: touch, no strike ({}) {} {}: blade {:.2f} m/s, hand peak {:.2f} m/s over {:.2f} m", why, tg.skel ? "body" : "box", Collision::Describe(tg.ent), bestSpeed, g_HandPeak, g_HandPath); }
        }
        else
        {
            g_Armed = false; g_LastStrikeAt = t;
            g_LastStruck[tg.ent] = t;
            if (g_LastStruck.size() > 64) g_LastStruck.clear();
            g_Contacts++; o.contact = true; if (tg.skel) g_SkelHits++;
            g_LastContactAt = t; g_LastSpeed = bestSpeed; g_LastId = tg.id;
            // the strike: aimed from the eye at the contact point, only this entity may be hit...
            MeleeAim::HitBox hb; HitBoxOf(tg, hb); MeleeAim::AddHitBox(hb);
            // ...and the character whose sword, hat or arm guard the blade met first
            uint32_t ownerId = 0;
            if (!tg.skel)
                for (const Target& ch : g_Targets)
                    if (ch.skel && ch.sk >= 0 && SkelDist(g_Skels[ch.sk], bestPt) < C.attach)
                    { MeleeAim::HitBox h2; HitBoxOf(ch, h2); MeleeAim::AddHitBox(h2); ownerId = ch.id; g_Attach++; break; }
            MeleeAim::SetContactSweep(bestPt, bestVel);
            const float f[3] = { bestPt[0] - eye[0], bestPt[1] - eye[1], bestPt[2] - eye[2] };
            const float fl = Len(f);
            if (fl > 1e-3f)
            {
                const float fw[3] = { f[0] / fl, f[1] / fl, f[2] / fl };
                float l[3] = { -fw[1], fw[0], 0.f }; float ln = sqrtf(l[0] * l[0] + l[1] * l[1]); if (ln < 1e-3f) { l[0] = 0; l[1] = 1; ln = 1; }
                l[0] /= ln; l[1] /= ln;
                const float u[3] = { fw[1] * l[2] - fw[2] * l[1], fw[2] * l[0] - fw[0] * l[2], fw[0] * l[1] - fw[1] * l[0] };
                const float ax[9] = { fw[0], fw[1], fw[2], l[0], l[1], l[2], u[0], u[1], u[2] };
                MeleeAim::SetGestureAxis(ax, 0.6f);
            }
            Buzz(0.6f, 0.04f);
            g_AirAt = -1;   // this swing has its click
            const char* how;
            const int win = MeleeAim::WindowPhase();
            const bool kill = MeleeAim::KillOffered();   // the game offers a stealth kill: its attack IS the takedown
            if (MeleeAim::BladeDamage() && tg.skel && !SwordGesture::Charging() && !kill)
            {   // the blade is the attack: the game's strike on this character, scaled by the blade's speed (MeleeAim)
                bladeSerial = MeleeAim::BladeStrike(tg.ent, bestSpeed);
                how = "the blade strikes it itself (no Slash)";
            }
            else if (SwordGesture::Charging()) { SwordGesture::ReleaseCharge(); g_Releases++; how = "releases the charged attack"; }
            else if (MeleeAim::SlashRunning() && !MeleeAim::HitThisSlash() && win < 2)
            {
                if (C.early > 0.5f) { MeleeAim::RequestOpen(C.pendMax); g_Opens++; }
                how = win == 1 ? "the running Slash's open window strikes it" : "the running Slash strikes it (its window opens shortly)";
            }
            else if (MeleeAim::SlashRunning()) { g_TapUntil = t + 0.9; g_Queued++; how = MeleeAim::HitThisSlash() ? "queued: the running Slash already hit" : "queued: the running Slash's window has closed"; }
            else { if (C.early > 0.5f && !kill) MeleeAim::RequestOpen(C.pendMax); o.tap = true; g_Taps++; awaitSlash(); how = kill ? "clicks the attack: the STEALTH KILL on offer" : "clicks a Slash"; }
            LOGI("SwordContact: CONTACT {} {}{} at ({:.2f},{:.2f},{:.2f}), {:.2f} m from the eye, blade {:.2f} m/s (hand peak {:.2f} m/s over {:.2f} m) at {:.0f}% of the frame: {}",
                 tg.skel ? "BODY" : fmt::format("box {:.2f}x{:.2f}x{:.2f}", tg.mx[0] - tg.mn[0], tg.mx[1] - tg.mn[1], tg.mx[2] - tg.mn[2]), Collision::Describe(tg.ent),
                 ownerId ? fmt::format(" (held by / on the body of {:#x}: it too)", ownerId) : std::string(),
                 bestPt[0], bestPt[1], bestPt[2], fl, bestSpeed, g_HandPeak, g_HandPath, bestT * 100.f, how);
        }
    }

    // The swing gesture's deferred click: the swing touched nothing -- click when it is over (the Slash's sound in the air).
    if (g_AirAt >= 0)
    {
        const bool over = t - g_AirAt > C.airMax || (g_HandSpeed < C.rearmHand && t - g_AirAt > 0.05);
        if (over)
        {
            g_AirAt = -1;
            if (!MeleeAim::SlashRunning() && g_TapUntil < 0) { o.tap = true; g_AirTaps++; LOGI("SwordContact: the swing touched nothing: air Slash (sound)"); }
            else g_AirDropped++;
        }
    }

    {   // debug for the reticle
        std::lock_guard lk(g_DbgM);
        g_Dbg.blade = true; memcpy(g_Dbg.guard, cur[0], 12); memcpy(g_Dbg.tip, cur[n - 1], 12); g_Dbg.n = n; memcpy(g_Dbg.pts, cur, sizeof(float) * 3 * n);
        g_Dbg.armed = g_Armed && g_SwingOk && t >= g_LostUntil; g_Dbg.ready = g_Armed && t >= g_LostUntil;
        if (o.contact) { g_Dbg.contact = true; memcpy(g_Dbg.point, bestPt, 12); g_DbgContactAt = t; }
        g_Dbg.age = g_DbgContactAt >= 0 ? (float)(t - g_DbgContactAt) : 1e9f;
        if (g_Dbg.age > 1.5f) g_Dbg.contact = false;
        // the boxes nearest the tip, and the bodies
        g_Dbg.boxes = 0; g_Dbg.segs = 0;
        std::vector<std::pair<float, int>> byDist;
        for (size_t i = 0; i < g_Targets.size(); i++)
        {
            const float d[3] = { g_Targets[i].org[0] - cur[n - 1][0], g_Targets[i].org[1] - cur[n - 1][1], g_Targets[i].org[2] - cur[n - 1][2] };
            byDist.push_back({ Dot(d, d), (int)i });
        }
        std::sort(byDist.begin(), byDist.end());
        for (size_t k = 0; k < byDist.size(); k++)
        {
            const Target& tg = g_Targets[byDist[k].second];
            auto it = g_LastStruck.find(tg.ent);
            const bool touched = it != g_LastStruck.end() && t - it->second < 1.5;
            if (tg.skel && tg.sk >= 0)
            {
                const NpcSkel::Skel& s = g_Skels[tg.sk];
                for (int j = 0; j < s.n && g_Dbg.segs < SwordContact::Debug::kSegs; j++)
                {
                    if (s.link[j] < 0) continue;
                    auto& sg = g_Dbg.seg[g_Dbg.segs++];
                    memcpy(sg.a, s.p[j], 12); memcpy(sg.b, s.p[s.link[j]], 12); sg.r = s.r[j]; sg.touched = touched;
                }
            }
            else if (g_Dbg.boxes < 8)
            {
                Corners(tg, g_Dbg.corners[g_Dbg.boxes]);
                g_Dbg.touched[g_Dbg.boxes] = touched;
                g_Dbg.boxes++;
            }
        }
    }
    return o;
}

bool SwordContact::GetDebug(Debug& d) { std::lock_guard lk(g_DbgM); d = g_Dbg; return d.blade; }

void SwordContact::Scan(float radius)
{
    float eye[3], view[9];
    if (!HandAimHook::RealCamera(eye, view)) { LOGI("swordscan: no camera"); return; }
    const float keep = C.radius; if (radius > 0.f) C.radius = radius;
    memcpy(g_ScanView, view, 36); g_ScanViewOk = true;
    ScanTargets(eye, true);
    C.radius = keep;
    Debug d; GetDebug(d);
    float a[2] = {}, b[2] = {}; const bool pa = GpuTrace::ProjectWorld(d.guard, a), pb = GpuTrace::ProjectWorld(d.tip, b);
    float wo[3] = {}, wa[9] = {}; const bool w = GpuTrace::WeaponTargetPoseRaw(1, wo, wa);
    LOGI("swordscan: blade {} guard ({:.2f},{:.2f},{:.2f}) ndc {} ({:.2f},{:.2f}) tip ({:.2f},{:.2f},{:.2f}) ndc {} ({:.2f},{:.2f}) | weapon {} ({:.2f},{:.2f},{:.2f}) fwd ({:.2f},{:.2f},{:.2f}) | eye ({:.2f},{:.2f},{:.2f})",
         d.blade, d.guard[0], d.guard[1], d.guard[2], pa, a[0], a[1], d.tip[0], d.tip[1], d.tip[2], pb, b[0], b[1], w, wo[0], wo[1], wo[2], wa[0], wa[1], wa[2], eye[0], eye[1], eye[2]);
    g_LastScan = -1;
}

bool SwordContact::Set(const std::string& key, float v)
{
    for (const Named& n : kCfg) if (key == n.name) { *n.v = v; return true; }
    return false;
}
std::string SwordContact::Config()
{
    std::ostringstream o;
    for (const Named& n : kCfg) o << n.name << " " << *n.v << "  ";
    return o.str();
}
std::string SwordContact::Stats()
{
    std::ostringstream o;
    o << "on " << g_On.load() << " targets " << g_Targets.size() << " bodies " << g_Skels.size() << " frames " << g_Frames << " contacts " << g_Contacts
      << " bodyhits " << g_SkelHits << " attach " << g_Attach << " touches " << g_Touches
      << " (noswing " << g_RejSwing << " spent " << g_RejArmed << " slow " << g_RejSlow << " lost " << g_RejLost << ")"
      << " taps " << g_Taps << " started " << g_StrikeStarted << " reclicks " << g_ReclickTotal << " lost " << g_StrikeLost << " airtaps " << g_AirTaps << " airdropped " << g_AirDropped << " queued " << g_Queued << " impacts " << g_Impacts
      << " opens " << g_Opens << " releases " << g_Releases << " teleports " << g_Teleports
      << " armed " << g_Armed << " swing " << g_SwingOk << " hand " << g_HandSpeed << " peak " << g_HandPeak << " path " << g_HandPath
      << " last id " << std::hex << g_LastId << std::dec << " speed " << g_LastSpeed << " | skel " << NpcSkel::Stats();
    return o.str();
}
