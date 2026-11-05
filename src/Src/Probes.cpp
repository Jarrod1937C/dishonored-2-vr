#include "pch.h"
#include "Probes.h"
#include "GameInfo.h"
#include "CVars.h"
#include "PassProfile.h"
#include "Collision.h"
#include "Watchpoint.h"
#include <detours/detours.h>
#include <intrin.h>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <cmath>

namespace
{
    // ---- POD helpers (__try) ----
    bool Rd(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    const void* Ptr(const void* base, size_t off)
    {
        const void* p = nullptr;
        if (!base || !Rd(&p, (const uint8_t*)base + off, 8)) return nullptr;
        return p;
    }
    //! a printable C string at p (up to 95 chars), else ""
    void Str(const void* p, char* out, size_t cap)
    {
        out[0] = 0;
        if (!p) return;
        char tmp[96] = {};
        if (!Rd(tmp, p, sizeof tmp - 1))
        {   // near a page end: byte by byte
            size_t i = 0;
            for (; i < sizeof tmp - 1; i++) if (!Rd(tmp + i, (const uint8_t*)p + i, 1)) break;
            tmp[i] = 0;
        }
        size_t n = 0;
        while (n < sizeof tmp - 1 && tmp[n]) { if ((unsigned char)tmp[n] < 32 || (unsigned char)tmp[n] > 126) return; n++; }
        if (n < 2) return;
        strncpy_s(out, cap, tmp, _TRUNCATE);
    }
    std::string NameAt(const void* p, size_t off) { char b[96]; Str(Ptr(p, off), b, sizeof b); return b; }
    //! the nonzero dwords of [p, p+n), as "+off=hex(float)" (at most `max`)
    std::string Dump(const void* p, size_t from, size_t n, int max = 64)
    {
        std::string s; int k = 0;
        if (!p) return "(null)";
        for (size_t o = from; o + 4 <= from + n && k < max; o += 4)
        {
            uint32_t v = 0;
            if (!Rd(&v, (const uint8_t*)p + o, 4)) { s += fmt::format(" +{:X}=?", o); break; }
            if (!v) continue;
            float f; memcpy(&f, &v, 4);
            const bool fl = std::isfinite(f) && fabsf(f) > 1e-6f && fabsf(f) < 1e7f;
            s += fl ? fmt::format(" +{:X}={:X}({:g})", o, v, f) : fmt::format(" +{:X}={:X}", o, v);
            k++;
        }
        return s.empty() ? " (all zero)" : s;
    }
    double Now() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart / (double)f.QuadPart; }
    uintptr_t Base() { return GameInfo::Base(); }
    std::string Rva(const void* p)
    {
        const uintptr_t a = (uintptr_t)p, b = Base();
        return a >= b && a < b + 0x4000000 ? fmt::format("exe+0x{:X}", a - b) : fmt::format("{}", p);
    }

    // ---- doors ----
    using AnimUpd_t = uintptr_t(*)(void* state);   // (return values passed through: unknown)
    using SetSpeed_t = bool(*)(void* anim, int slot, int animId, float v);
    AnimUpd_t g_RealAnimUpd = nullptr; SetSpeed_t g_RealSetSpeed = nullptr;
    constexpr uintptr_t kAnimUpd = 0x74E7A0, kSetSpeed = 0x540D30;
    struct Region { const char* name; const void* p; size_t from, n; std::vector<uint32_t> last; };
    struct Track { void* state = nullptr; double t0 = 0; int lines = 0; std::vector<Region> regs; std::string name; void* anim = nullptr; float rate = -1.f; };
    std::mutex g_DM;
    std::vector<Track> g_Tracks;          // at most 2 at a time
    int g_DoorSessions = 0;               // 6 per run
    bool g_WpArmed = false; int g_WpDone = 0; double g_WpAt = 0;   // (g_DM)
    std::unordered_set<void*> g_DoorSeen;
    std::atomic<unsigned long long> g_AnimUpdCalls{ 0 }, g_SetSpeedCalls{ 0 };
    int g_SetSpeedLogged = 0;
    std::vector<uint32_t> Snap(const Region& r)
    {
        std::vector<uint32_t> v(r.n / 4, 0);
        if (r.p) Rd(v.data(), (const uint8_t*)r.p + r.from, r.n);
        return v;
    }
    //! the entity's arkCpntPhysicsBase (type exe+0x39E85B0) through the game's component lookup exe+0x877720 (game thread)
    using GetCpnt_t = void* (*)(const void* ent, const void* type, int how);
    const void* PhysOfRaw(const void* ent)
    {
        __try { return ent ? ((GetCpnt_t)(Base() + 0x877720))(ent, (const void*)(Base() + 0x39E85B0), 2) : nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    const void* PhysOf(const void* ent) { return PhysOfRaw(ent); }
    void StartTrack(void* state)
    {
        Track t; t.state = state; t.t0 = Now();
        const void* comp = Ptr(state, 0x18);
        const void* ent = Ptr(comp, 0x10);
        const void* anim = Ptr(comp, 0xF8);
        const void* player = anim ? (const uint8_t*)anim + 0x338 : nullptr;
        const void* playerObj = Ptr(player, 0);
        const void* slot0 = Ptr(anim, 0x3A0);
        const void* phys = PhysOf(ent);
        // round 5: the body follows its arkReferenceFrame ([entity+0x358]; pre-physics update exe+0x7545A0): the velocity
        // to reach its target, the parent's pose (+0x3C8, its joint +0x3D0 of the animation +0x400) times the local
        // offset (+0x3D4, rotation +0x3E0). The animation's variables: the block [[anim+0x338]], slot 0's at its +[slot0+4]
        const void* rf = Ptr(ent, 0x358);
        const void* rfParent = Ptr(rf, 0x3C8);
        const void* rfAnim = Ptr(rf, 0x400);
        const void* vars = Ptr(playerObj, 0);
        int slotOff = -1; Rd(&slotOff, (const uint8_t*)slot0 + 4, 4);
        t.name = NameAt(ent, 0x20);
        t.anim = (void*)anim;
        t.regs = { { "state", state, 0, 0x40, {} }, { "ent", ent, 0x2F0, 0x70, {} }, { "anim", anim, 0x330, 0x90, {} },
                   { "rf", rf, 0x3C0, 0x60, {} }, { "parent", rfParent, 0x2F0, 0x70, {} }, { "vars", vars, 0, 0x200, {} } };
        LOGI("Probes: door #{} '{}': reference frame {} (vtable {}): parent {} '{}' (vtable {}), its animation {} ({}), the door's animation's variables {} (slot 0's at +0x{:X});{}",
             g_DoorSessions, NameAt(ent, 0x20), rf, Rva(Ptr(rf, 0)), rfParent, NameAt(rfParent, 0x20), Rva(Ptr(rfParent, 0)), rfAnim,
             rfAnim == anim ? "the door's own" : rfAnim ? "another" : "none", vars, slotOff, Dump(rf, 0x3C0, 0x60, 24));
        LOGI("Probes: door #{} '{}' animating: state {} (vtable {}) comp {} (vtable {}) entity {} anim {} player {} slot0 {} physics {} (vtable {})",
             g_DoorSessions, t.name, state, Rva(Ptr(state, 0)), comp, Rva(Ptr(comp, 0)), ent, anim, playerObj, slot0, phys, Rva(Ptr(phys, 0)));
        for (Region& r : t.regs)
        {
            r.last = Snap(r);
            LOGI("Probes: door #{}   {} +{:X}..+{:X}:{}", g_DoorSessions, r.name, r.from, r.from + r.n, Dump(r.p, r.from, r.n, 96));
        }
        constexpr int kDoorWatches = 0;   // (done 2026-11-04: exe+0x6E26C2 in the player update exe+0x6E2650)
        if (anim && !g_WpArmed && g_WpDone < kDoorWatches)
        {   // what advances the door's animation: a write watchpoint on its anim+0x378 (the last step's time: 1 / 120 s or
            // 2 / 120 s a frame), 1.5 s. (Round 4, [entity+0x358]+0x9C: the reference frame's own update, exe+0x7546D7;
            // round 3, phys+0x158: the physics engine's copy, exe+0x18D143 / exe+0x18D491.)
            g_WpArmed = Watchpoint::Arm((uintptr_t)anim + 0x378, 4, 60, false, true);
            g_WpAt = Now(); g_WpDone++;
            LOGI("Probes: door #{}: write watchpoint on its animation's anim+0x378 {}", g_DoorSessions, g_WpArmed ? "armed" : "COULD NOT BE ARMED");
        }
        g_Tracks.push_back(std::move(t));
    }
    void FollowDoor(void* state);
    uintptr_t HkAnimUpd(void* state)
    {
        g_AnimUpdCalls++;
        const uintptr_t ret = g_RealAnimUpd(state);
        if (PassProfile::On()) FollowDoor(state);
        return ret;
    }
    void FollowDoor(void* state)
    {
        std::lock_guard lk(g_DM);
        bool tracked = false;
        for (Track& t : g_Tracks) if (t.state == state) tracked = true;
        if (!tracked && g_DoorSessions < 6 && g_Tracks.size() < 2 && !g_DoorSeen.count(state)) { g_DoorSeen.insert(state); g_DoorSessions++; StartTrack(state); tracked = true; }
        if (!tracked) return;
        const double now = Now();
        for (auto it = g_Tracks.begin(); it != g_Tracks.end();)
        {
            Track& t = *it;
            if (t.state != state) { ++it; continue; }
            std::string line;
            for (Region& r : t.regs)
            {
                std::vector<uint32_t> v = Snap(r);
                for (size_t i = 0; i < v.size() && i < r.last.size(); i++)
                    if (v[i] != r.last[i])
                    {
                        float a, b; memcpy(&a, &r.last[i], 4); memcpy(&b, &v[i], 4);
                        line += fmt::format(" {}+{:X}:{:g}->{:g}", r.name, r.from + i * 4, a, b);
                    }
                r.last = std::move(v);
            }
            if (!line.empty() && t.lines < 400) { t.lines++; LOGI("Probes: door '{}' t={:.3f}{}", t.name, now - t.t0, line); }
            if (now - t.t0 > 3.0 || t.lines >= 400) { LOGI("Probes: door '{}' followed for {:.1f} s ({} lines)", t.name, now - t.t0, t.lines); it = g_Tracks.erase(it); }
            else ++it;
        }
    }
    bool HkSetSpeed(void* anim, int slot, int animId, float v)
    {
        g_SetSpeedCalls++;
        const bool r = g_RealSetSpeed(anim, slot, animId, v);
        if (PassProfile::On() && g_SetSpeedLogged < 60)
        {
            g_SetSpeedLogged++;
            LOGI("Probes: exe+0x540D30(anim {}, slot {}, anim id {}, {:g}) -> {} from {}", anim, slot, animId, v, r, Rva(_ReturnAddress()));
        }
        return r;
    }

    // ---- hit reactions ----
    using HitMsg_t = uintptr_t(*)(void* comp, void* msg);
    using PlayHit_t = uintptr_t(*)(void* comp, void* info);
    HitMsg_t g_RealHitMsg = nullptr; PlayHit_t g_RealPlayHit = nullptr;
    constexpr uintptr_t kHitMsg = 0x591DF0, kPlayHit = 0x593860;
    std::atomic<int> g_HitLogged{ 0 };
    std::atomic<unsigned long long> g_HitMsgs{ 0 }, g_HitPlays{ 0 };
    std::unordered_set<const void*> g_DeclsDumped;   // (g_DM)
    uintptr_t HkHitMsg(void* comp, void* msg)
    {
        g_HitMsgs++;
        if (PassProfile::On() && g_HitLogged.load() < 40)
        {
            g_HitLogged++;
            const void* rec = Ptr(msg, 8);
            const void* decl = Ptr(rec, 0);
            const void* ent = Ptr(comp, 0x10);
            LOGI("Probes: hit #{}: NPC '{}' (comp {} vtable {}) handles a damage, from {}; msg{}", g_HitLogged.load(), NameAt(ent, 0x20), comp, Rva(Ptr(comp, 0)),
                 Rva(_ReturnAddress()), Dump(msg, 0, 0x20, 8));
            LOGI("Probes: hit #{}:   record {} (decl {} '{}' / '{}'):{}", g_HitLogged.load(), rec, decl, NameAt(decl, 8), NameAt(decl, 0x10), Dump(rec, 0, 0xC0, 48));
            bool first = false; { std::lock_guard lk(g_DM); first = decl && g_DeclsDumped.insert(decl).second && g_DeclsDumped.size() < 24; }
            if (first) LOGI("Probes: hit #{}:   its decl (vtable {}):{}", g_HitLogged.load(), Rva(Ptr(decl, 0)), Dump(decl, 0, 0x200, 128));
            LOGI("Probes: hit #{}:   the component before:{}", g_HitLogged.load(), Dump(comp, 0x120, 0x40, 16));
        }
        const uintptr_t ret = g_RealHitMsg(comp, msg);
        if (PassProfile::On() && g_HitLogged.load() <= 40)
            LOGI("Probes: hit: the component after:{}", Dump(comp, 0x120, 0x40, 16));
        return ret;
    }
    uintptr_t HkPlayHit(void* comp, void* info)
    {
        g_HitPlays++;
        if (PassProfile::On() && g_HitLogged.load() <= 40)
            LOGI("Probes: hit react PLAYED on '{}' from {}: info{}", NameAt(Ptr(comp, 0x10), 0x20), Rva(_ReturnAddress()), Dump(info, 0, 0x40, 16));
        return g_RealPlayHit(comp, info);
    }

    // ---- a door pushed by an impulse (Probes::DoorImpulseTest) ----
    struct Watch { const void* ent = nullptr; double t0 = 0, last = 0; std::string name; float imp = 0; };
    Watch g_Watch; int g_ImpulseTests = 0;
    std::atomic<bool> g_WatchOn{ false };

    // ---- objective markers ----
    // (done 2026-11-04: with the option off the game releases its objective markers at once; 2 = not run again)
    int g_ObjTest = 2; double g_ObjTestAt = 0;   // the option-off experiment: 0 not yet, 1 running, 2 done (render thread)
    std::atomic<const void*> g_MarkerMgr{ nullptr };   // marker+0x78 of any marker seen (arkEntityMarkerManager: pool at +0x18)
    constexpr uintptr_t kObjMarkerVt = 0x1B7E748, kMarkerVt = 0x1C39A98, kAwareVt = 0x1C371E8;
    std::atomic<unsigned long long> g_ObjProj{ 0 }, g_OtherProj{ 0 }, g_NoMarker{ 0 };
    std::unordered_map<const void*, double> g_MarkerLogged;   // (g_DM)
    int g_MarkerLines = 0;
    //! the marker object among the caller frames' stack slots (POD)
    const void* FindMarker(const uint8_t* sp, uintptr_t* vtOut)
    {
        const uintptr_t b = Base();
        for (int i = 0; i < 160; i++)
        {
            const void* p = nullptr;
            if (!Rd(&p, sp + i * 8, 8) || !p || ((uintptr_t)p & 7)) continue;
            uintptr_t vt = 0;
            if (!Rd(&vt, p, 8)) continue;
            if (vt == b + kObjMarkerVt || vt == b + kMarkerVt || vt == b + kAwareVt) { *vtOut = vt - b; if (const void* mgr = Ptr(p, 0x78)) g_MarkerMgr = mgr; return p; }
        }
        return nullptr;
    }
}

void Probes::OnMarkerProj(const float* world)
{
    if (!PassProfile::On() || !world) return;
    uintptr_t vt = 0;
    const void* m = FindMarker((const uint8_t*)_AddressOfReturnAddress(), &vt);
    if (!m) { g_NoMarker++; return; }
    if (vt != kObjMarkerVt) { g_OtherProj++; return; }
    g_ObjProj++;
    const double now = Now();
    std::lock_guard lk(g_DM);
    auto it = g_MarkerLogged.find(m);
    if (g_MarkerLines >= 40 || (it != g_MarkerLogged.end() && now - it->second < 15.0)) return;
    g_MarkerLogged[m] = now; g_MarkerLines++;
    float w[3]; memcpy(w, world, 12);
    // where the point is kept in the marker (and one pointer deep)
    std::string at;
    for (size_t o = 0; o + 12 <= 0x680; o += 4)
    {
        float v[3];
        if (Rd(v, (const uint8_t*)m + o, 12) && fabsf(v[0] - w[0]) < 0.05f && fabsf(v[1] - w[1]) < 0.05f && fabsf(v[2] - w[2]) < 0.05f) at += fmt::format(" marker+0x{:X}", o);
    }
    for (size_t o = 0; o + 8 <= 0x200; o += 8)
    {
        const void* q = Ptr(m, o);
        if (!q || ((uintptr_t)q & 7)) continue;
        for (size_t k = 0; k + 12 <= 0x400; k += 4)
        {
            float v[3];
            if (Rd(v, (const uint8_t*)q + k, 12) && fabsf(v[0] - w[0]) < 0.05f && fabsf(v[1] - w[1]) < 0.05f && fabsf(v[2] - w[2]) < 0.05f) { at += fmt::format(" [marker+0x{:X}]+0x{:X}", o, k); break; }
        }
    }
    int type = -1; Rd(&type, (const uint8_t*)m + 0x648, 4);
    LOGI("Probes: objective marker {} (type +0x648 = {}) projected at ({:.2f}, {:.2f}, {:.2f}); the point found at:{}; ark_ui_show_objective_markers {}",
         m, type, w[0], w[1], w[2], at.empty() ? std::string(" nowhere near") : at, CVars::GetInt("ark_ui_show_objective_markers", -1));
    LOGI("Probes: objective marker {}:{}", m, Dump(m, 0x8, 0x100, 64));
    LOGI("Probes: objective marker {} (tail):{}", m, Dump(m, 0x600, 0x60, 24));
}

bool Probes::DoorImpulseTest(void* door, const float point[3], const float normal[3], int hand)
{
    // (done 2026-11-04: impulses of 20 / 60 / 150 moved a door not at all -- its body is kinematic. Kept, off.)
    constexpr int kTests = 0;
    if (!PassProfile::On() || !door || kTests == 0) return false;
    std::lock_guard lk(g_DM);
    if (g_ImpulseTests >= kTests || g_WatchOn.load()) return false;
    static const float kImp[3] = { 20.f, 60.f, 150.f };   // (N s: a door of some tens of kg)
    const float imp = kImp[g_ImpulseTests++];
    const float v[3] = { -normal[0] * imp, -normal[1] * imp, -normal[2] * imp };
    Collision::Push(door, v, point);
    g_Watch.ent = door; g_Watch.t0 = g_Watch.last = Now(); g_Watch.name = NameAt(door, 0x20); g_Watch.imp = imp; g_WatchOn = true;
    LOGI("Probes: research: the {} hand's push on door '{}' sent as a physics impulse of {:g} at ({:.2f}, {:.2f}, {:.2f}) instead of the game's door push (test {} of 3)",
         hand ? "right" : "left", g_Watch.name, imp, point[0], point[1], point[2], g_ImpulseTests);
    return true;
}

void Probes::OnPresent()
{
    if (!PassProfile::On()) return;
    if (g_WatchOn.load())
    {   // the door an impulse was sent to: its rotation's first row and its velocities, 1.5 s
        const double t = Now();
        std::lock_guard lk(g_DM);
        if (t - g_Watch.last > 0.02)
        {
            g_Watch.last = t;
            float r[3] = {}, lv[3] = {}, av[3] = {};
            Rd(r, (const uint8_t*)g_Watch.ent + 0x30C, 12); Rd(lv, (const uint8_t*)g_Watch.ent + 0x33C, 12); Rd(av, (const uint8_t*)g_Watch.ent + 0x348, 12);
            LOGI("Probes: door '{}' after an impulse of {:g}: t={:.2f} rotation row ({:.4f}, {:.4f}, {:.4f}) velocity ({:.3f}, {:.3f}, {:.3f}) angular ({:.3f}, {:.3f}, {:.3f})",
                 g_Watch.name, g_Watch.imp, t - g_Watch.t0, r[0], r[1], r[2], lv[0], lv[1], lv[2], av[0], av[1], av[2]);
        }
        if (t - g_Watch.t0 > 1.5) g_WatchOn = false;
    }
    {   // the door watchpoint: its writers after 1.5 s
        std::lock_guard lk(g_DM);
        if (g_WpArmed && Now() - g_WpAt > 1.5) { Watchpoint::Dump(); Watchpoint::Disarm(); g_WpArmed = false; LOGI("Probes: door watchpoint dumped and disarmed"); }
    }
    {   // the objective markers' pool, every 5 s
        static double nextPool = 0;
        const double t = Now();
        const void* mgr = g_MarkerMgr.load();
        if (mgr && t >= nextPool)
        {
            nextPool = t + (g_ObjTest == 1 ? 2.0 : 5.0);
            int inUse = 0;
            const void* arr = Ptr(mgr, 0x18);
            uint32_t cnt = 0; Rd(&cnt, (const uint8_t*)mgr + 0x20, 4); cnt &= 0xFFFFFF;
            uint16_t ranges[5] = {}; Rd(ranges, (const uint8_t*)mgr + 0x8, 10);
            std::string s; int objs = 0;
            for (uint32_t i = 0; arr && i < cnt && i < 80; i++)
            {
                const void* m = Ptr(arr, i * 8);
                uintptr_t vt = 0;
                if (!m || !Rd(&vt, m, 8) || vt != Base() + kObjMarkerVt) continue;
                objs++;
                uint32_t a8 = 0, a10 = 0, a18 = 0, a640 = 0, a648 = 0, a650 = 0; float pos[3] = {}, dist = 0;
                Rd(&a8, (const uint8_t*)m + 0x8, 4); Rd(&a10, (const uint8_t*)m + 0x10, 4); Rd(&a18, (const uint8_t*)m + 0x18, 4);
                Rd(pos, (const uint8_t*)m + 0x3C, 12); Rd(&dist, (const uint8_t*)m + 0xD8, 4);
                Rd(&a640, (const uint8_t*)m + 0x640, 4); Rd(&a648, (const uint8_t*)m + 0x648, 4); Rd(&a650, (const uint8_t*)m + 0x650, 4);
                if (a8 == 0x3FFF0010 && pos[0] == 0 && pos[1] == 0) continue;   // (never used)
                if (a8 != 0x3FFF0010) inUse++;
                s += fmt::format(" [{}] +8={:X} +10={:X} +18={:X} pos ({:.1f}, {:.1f}, {:.1f}) +D8={:.1f} +640={:X} +648={} +650={:X};", i, a8, a10, a18, pos[0], pos[1], pos[2], dist, a640, a648, a650);
            }
            LOGI("Probes: marker pool {} ({} markers, ranges {} {} {} {} {}): {} objective markers{}; ark_ui_show_objective_markers {}", mgr, cnt, ranges[0], ranges[1], ranges[2], ranges[3], ranges[4],
                 objs, s.empty() ? std::string(" (none in use)") : s, CVars::GetInt("ark_ui_show_objective_markers", -1));
            // the experiment: with objective markers in use and the game's option on, the option off for 16 s, then back
            if (g_ObjTest == 0 && inUse > 0 && CVars::GetInt("ark_ui_show_objective_markers", -1) == 1)
            { g_ObjTest = 1; g_ObjTestAt = t; CVars::SetInt("ark_ui_show_objective_markers", 0); LOGI("Probes: research: {} objective markers in use: the game's objective markers option OFF for 16 s (ark_ui_show_objective_markers 0)", inUse); }
            else if (g_ObjTest == 1 && t - g_ObjTestAt > 16.0)
            { g_ObjTest = 2; CVars::SetInt("ark_ui_show_objective_markers", 1); LOGI("Probes: research: the game's objective markers option back ON"); }
        }
    }
    static double next = 0;
    const double now = Now();
    if (next == 0) next = now + 30.0;
    if (now < next) return;
    next = now + 30.0;
    LOGI("Probes: 30 s: door animation updates {} (exe+0x540D30 calls {}); NPC damage handled {}, hit reacts played {}; markers projected: objective {}, other {}, marker not found {}; ark_ui_show_objective_markers {}",
         g_AnimUpdCalls.load(), g_SetSpeedCalls.load(), g_HitMsgs.load(), g_HitPlays.load(), g_ObjProj.load(), g_OtherProj.load(), g_NoMarker.load(),
         CVars::GetInt("ark_ui_show_objective_markers", -1));
}

void Probes::Install()
{
    static bool done = false;
    if (done) return;
    done = true;
    const uintptr_t b = Base();
    // the expected first bytes (this build): nothing is hooked where they differ
    struct Site { uintptr_t rva; const char* name; uint8_t bytes[4]; void** real; void* hook; } sites[] = {
        { kAnimUpd, "door animation update", { 0x40, 0x53, 0x48, 0x81 }, (void**)&g_RealAnimUpd, (void*)HkAnimUpd },
        { kSetSpeed, "animation speed", { 0x44, 0x89, 0x44, 0x24 }, (void**)&g_RealSetSpeed, (void*)HkSetSpeed },
        { kHitMsg, "NPC damage -> hit react", { 0x48, 0x89, 0x5C, 0x24 }, (void**)&g_RealHitMsg, (void*)HkHitMsg },
        { kPlayHit, "NPC hit react", { 0x40, 0x55, 0x56, 0x48 }, (void**)&g_RealPlayHit, (void*)HkPlayHit },
    };
    std::string said;
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    for (Site& s : sites)
    {
        uint8_t have[4] = {};
        if (!Rd(have, (const void*)(b + s.rva), 4) || memcmp(have, s.bytes, 4) != 0)
        {
            said += fmt::format(" {} exe+0x{:X} NOT hooked (bytes {:02X} {:02X} {:02X} {:02X});", s.name, s.rva, have[0], have[1], have[2], have[3]);
            continue;
        }
        *s.real = (void*)(b + s.rva);
        DetourAttach(s.real, s.hook);
        said += fmt::format(" {} exe+0x{:X};", s.name, s.rva);
    }
    const LONG e = DetourTransactionCommit();
    LOGI("Probes: research hooks (ProfileRenderPasses) detour={}:{}", e, said);
}
