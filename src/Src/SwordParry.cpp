#include "pch.h"
#include "SwordParry.h"
#include "GameInfo.h"
#include "SwordContact.h"
#include "VRRuntime.h"
#include <detours/detours.h>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    constexpr uintptr_t kOnPlayerHit = 0x6357D0;   // an NPC's melee strike on the player: (comp, record, player)
    constexpr uintptr_t kParry = 0x636140, kSparks = 0x635F30, kSetActive = 0x634890;
    constexpr size_t kCompDef = 0x08, kCompOwner = 0x10, kCompWindowF = 0x3424, kDefBlockable = 0x180;
    constexpr size_t kRecPoint = 0x30, kEntOrigin = 0x300, kEntId = 0xA8;
    constexpr float kReach = 0.14f;      // m: the blade this close to the blow's path parries it
    constexpr double kKeep = 0.15;       // s: the blade's positions this recent count (the strike is decided after the contact)
    constexpr int kRing = 32;

    using FnHit = uint8_t (*)(void*, void*, void*);
    FnHit g_Real = nullptr;
    std::atomic<bool> g_On{ true };
    std::atomic<unsigned long long> g_Blows{ 0 }, g_Parried{ 0 }, g_Unblockable{ 0 }, g_Faults{ 0 };
    std::atomic<int> g_Logs{ 0 };

    struct Sample { double t = 0; float g[3] = {}, p[3] = {}; };
    std::mutex g_M; Sample g_Ring[kRing]; int g_Head = 0;

    double Now() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart; }
    float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    //! the closest points of segments p0-p1 and q0-q1: distance, and where along each (0..1)
    float SegSeg(const float p0[3], const float p1[3], const float q0[3], const float q1[3], float& s, float& t)
    {
        const float d1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, d2[3] = { q1[0] - q0[0], q1[1] - q0[1], q1[2] - q0[2] };
        const float r[3] = { p0[0] - q0[0], p0[1] - q0[1], p0[2] - q0[2] };
        const float a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r);
        if (a < 1e-8f && e < 1e-8f) { s = t = 0.f; return sqrtf(Dot(r, r)); }
        if (a < 1e-8f) { s = 0.f; t = std::clamp(f / e, 0.f, 1.f); }
        else
        {
            const float c = Dot(d1, r);
            if (e < 1e-8f) { t = 0.f; s = std::clamp(-c / a, 0.f, 1.f); }
            else
            {
                const float b = Dot(d1, d2), den = a * e - b * b;
                s = den > 1e-8f ? std::clamp((b * f - c * e) / den, 0.f, 1.f) : 0.f;
                t = (b * s + f) / e;
                if (t < 0.f) { t = 0.f; s = std::clamp(-c / a, 0.f, 1.f); }
                else if (t > 1.f) { t = 1.f; s = std::clamp((b - c) / a, 0.f, 1.f); }
            }
        }
        float d[3]; for (int k = 0; k < 3; k++) d[k] = (p0[k] + d1[k] * s) - (q0[k] + d2[k] * t);
        return sqrtf(Dot(d, d));
    }

    //! what the blow is: the attacker's weapon, the point it meets the player, blockable (POD)
    bool ReadBlow(void* comp, void* rec, float w[3], float h[3], bool* blockable, uint32_t* attacker)
    {
        __try
        {
            const uint8_t* owner = *(const uint8_t* const*)((const uint8_t*)comp + kCompOwner);
            const uint8_t* def = *(const uint8_t* const*)((const uint8_t*)comp + kCompDef);
            if (!owner || !def) return false;
            memcpy(w, owner + kEntOrigin, 12); memcpy(h, (const uint8_t*)rec + kRecPoint, 12);
            *blockable = *(def + kDefBlockable) != 0;
            *attacker = *(const uint32_t*)(owner + 0xAC);
            return std::isfinite(w[0] + w[1] + w[2] + h[0] + h[1] + h[2]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    //! the game's own parry branch of exe+0x6357D0 (POD): the attacker parried, the sparks, the window closed
    bool RunParry(void* comp, void* rec, void* player)
    {
        __try
        {
            const uintptr_t b = GameInfo::Base();
            ((void (*)(void*, void*))(b + kParry))(comp, player);
            ((void (*)(void*, void*))(b + kSparks))(comp, rec);
            const float wf = *(const float*)((const uint8_t*)comp + kCompWindowF);
            ((void (*)(void*, uint8_t, uint8_t, uint8_t, float))(b + kSetActive))(comp, 0, 0, 0, wf);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    uint8_t Hook(void* comp, void* rec, void* player)
    {
        if (!g_On.load() || !comp || !rec) return g_Real(comp, rec, player);
        g_Blows++;
        float w[3], h[3]; bool blockable = true; uint32_t attacker = 0;
        if (!ReadBlow(comp, rec, w, h, &blockable, &attacker)) return g_Real(comp, rec, player);
        // the blade's recent positions against the blow's path (attacker's weapon -> the point on the player)
        const double now = Now();
        float best = 1e9f, bestT = 0.f, bestAge = 0.f; int samples = 0;
        {
            std::lock_guard lk(g_M);
            for (const Sample& s : g_Ring)
            {
                if (s.t <= 0 || now - s.t > kKeep) continue;
                samples++;
                float sb, tb;
                const float d = SegSeg(s.g, s.p, w, h, sb, tb);
                if (tb > 0.12f && d < best) { best = d; bestT = tb; bestAge = (float)(now - s.t); }
            }
        }
        const bool across = samples > 0 && best < kReach;
        const bool log = g_Logs.load() < 300;
        if (across && !blockable)
        {
            g_Unblockable++;
            if (log) { g_Logs++; LOGI("SwordParry: a blow from {:#x}: the blade was across it ({:.0f} cm) but it cannot be blocked: the game's", attacker, best * 100.f); }
            return g_Real(comp, rec, player);
        }
        if (across)
        {
            if (RunParry(comp, rec, player))
            {
                g_Parried++;
                VRRuntime::Vibrate(1, 0.9f, 0.12f);
                if (log) { g_Logs++; LOGI("SwordParry: a blow from {:#x} PARRIED by the blade ({:.0f} cm from its path, {:.0f}% of the way from the weapon to you, {:.0f} ms ago)",
                                          attacker, best * 100.f, bestT * 100.f, bestAge * 1000.f); }
                return 0;
            }
            g_Faults++;
            if (log) { g_Logs++; LOGI("SwordParry: the parry for a blow from {:#x} faulted: the game's own handling", attacker); }
            return g_Real(comp, rec, player);
        }
        if (log)
        {
            g_Logs++;
            float ag[3] = {}, ap[3] = {}; const bool bl = SwordContact::Blade(ag, ap);
            LOGI("SwordParry: a blow from {:#x} (weapon at {:.2f} {:.2f} {:.2f}, meets you at {:.2f} {:.2f} {:.2f}): {} -- the game's",
                 attacker, w[0], w[1], w[2], h[0], h[1], h[2],
                 !bl ? std::string("no blade in the right hand")
                     : samples == 0 ? std::string("no recent blade positions")
                     : best < 1e8f ? fmt::format("the blade {:.0f} cm from its path at the closest (parries within {:.0f})", best * 100.f, kReach * 100.f)
                     : std::string("the blade was nowhere between"));
        }
        return g_Real(comp, rec, player);
    }
}

bool SwordParry::Install()
{
    if (g_Real || !GameInfo::IsSupportedBuild()) return g_Real != nullptr;
    const uint8_t* p = (const uint8_t*)(GameInfo::Base() + kOnPlayerHit);
    static const uint8_t kExpect[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x57 };
    if (p[0] != 0xE9 && memcmp(p, kExpect, sizeof kExpect) != 0) { LOGI("SwordParry: exe+0x{:X} is not the melee strike on the player (another game build?): no parry by the blade", kOnPlayerHit); return false; }
    g_Real = (FnHit)(GameInfo::Base() + kOnPlayerHit);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_Real, Hook);
    const LONG r = DetourTransactionCommit();
    if (r != NO_ERROR) g_Real = nullptr;
    LOGI("SwordParry: an enemy's blow on you (exe+0x{:X}) is parried when your blade is across it: detour={}", kOnPlayerHit, r);
    return g_Real != nullptr;
}
void SwordParry::SetEnabled(bool on) { g_On = on; LOGI("SwordParry: {}", on ? "your blade held across an enemy's blow parries it (SwordParry)" : "off: only the block gesture / grip blocks"); }
bool SwordParry::Enabled() { return g_On.load(); }
void SwordParry::Tick()
{
    float g[3], t[3]; double age = 0;
    if (!SwordContact::Blade(g, t, &age) || age > 0.1) return;
    std::lock_guard lk(g_M);
    Sample& s = g_Ring[g_Head]; g_Head = (g_Head + 1) % kRing;
    s.t = Now() - age; memcpy(s.g, g, 12); memcpy(s.p, t, 12);
}
std::string SwordParry::Status()
{
    return fmt::format("sword parry {} (blows on you {}, parried by the blade {}, unblockable {}, faults {})", g_On.load() ? "on" : "off",
                       g_Blows.load(), g_Parried.load(), g_Unblockable.load(), g_Faults.load());
}
