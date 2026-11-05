#include "pch.h"
#include "Undetected.h"
#include "BodyHide.h"
#include "GameInfo.h"
#include <detours/detours.h>
#include <atomic>
#include <cstring>

namespace
{
    constexpr uintptr_t kGetRelationship = 0x604C60;   // int (void* manager, const int* idA, const int* idB): A regarding B
    constexpr uintptr_t kSetRelationship = 0x605990;   // void (void* manager, entity A, entity B, int): A's own entry for B
    constexpr int kNeutral = 1, kEnemy = 2;   // (0 ally, 3 undefined)
    constexpr uint32_t kNoId = 0x3FFF0010u;
    using FnRel = int (*)(void*, const int*, const int*);
    FnRel g_Real = nullptr;
    using FnSet = void (*)(void*, void*, void*, int);
    FnSet g_RealSet = nullptr;
    std::atomic<bool> g_On{ false };
    std::atomic<uint32_t> g_PlayerId{ 0 };
    std::atomic<unsigned long long> g_Masked{ 0 }, g_Blocked{ 0 }, g_PlayerRegards{ 0 };
    unsigned long long g_LoggedMasked = 0; double g_LogAt = 0;

    double Now() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart; }

    int Hook(void* mgr, const int* a, const int* b)
    {
        const int r = g_Real(mgr, a, b);
        if (r != kEnemy || !g_On.load(std::memory_order_relaxed) || !a || !b) return r;
        const uint32_t pid = g_PlayerId.load(std::memory_order_relaxed);
        if (!pid || pid == kNoId) return r;
        if ((uint32_t)*b == pid && (uint32_t)*a != pid) { g_Masked.fetch_add(1, std::memory_order_relaxed); return kNeutral; }
        if ((uint32_t)*a == pid) g_PlayerRegards.fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    uint32_t ReadId(void* e)
    {
        if (!e) return 0;
        __try { return *(const uint32_t*)((const uint8_t*)e + 0xA8); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    //! someone turning on the player is not even written (the read is neutral anyway: a condition that sets it each
    //! time it finds it unset would otherwise set it, and post its "relationship changed", over and over)
    void HookSet(void* mgr, void* a, void* b, int rel)
    {
        if (rel == kEnemy && g_On.load(std::memory_order_relaxed) && a != b)
        {
            const uint32_t pid = g_PlayerId.load(std::memory_order_relaxed);
            if (pid && pid != kNoId && ReadId(b) == pid) { g_Blocked.fetch_add(1, std::memory_order_relaxed); return; }
        }
        g_RealSet(mgr, a, b, rel);
    }
}

bool Undetected::Install()
{
    if (g_Real || !GameInfo::IsSupportedBuild()) return g_Real != nullptr;
    const uint8_t* p = (const uint8_t*)(GameInfo::Base() + kGetRelationship);
    static const uint8_t kExpect[] = { 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57 };
    if (p[0] != 0xE9 && memcmp(p, kExpect, sizeof kExpect) != 0) { LOGI("Undetected: exe+0x{:X} is not the relationship lookup (another game build?): the cheat does nothing", kGetRelationship); return false; }
    g_Real = (FnRel)(GameInfo::Base() + kGetRelationship);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_Real, Hook);
    const LONG r = DetourTransactionCommit();
    if (r != NO_ERROR) g_Real = nullptr;
    LOGI("Undetected: the relationship lookup exe+0x{:X} hooked (detour={}): while the cheat is on, nobody regards you as an enemy", kGetRelationship, r);
    const uint8_t* q = (const uint8_t*)(GameInfo::Base() + kSetRelationship);
    static const uint8_t kExpectSet[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
    if (g_Real && (q[0] == 0xE9 || memcmp(q, kExpectSet, sizeof kExpectSet) == 0))
    {
        g_RealSet = (FnSet)(GameInfo::Base() + kSetRelationship);
        DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_RealSet, HookSet);
        const LONG r2 = DetourTransactionCommit();
        if (r2 != NO_ERROR) g_RealSet = nullptr;
        LOGI("Undetected: the relationship setter exe+0x{:X} hooked (detour={}): someone turning on you is not written while it is on", kSetRelationship, r2);
    }
    return g_Real != nullptr;
}
void Undetected::SetEnabled(bool on)
{
    g_On = on;
    LOGI("Undetected: {}", on ? "on: guards and hostile people regard you as neutral (they see you, and do not attack or hunt you)" : "off");
}
bool Undetected::Enabled() { return g_On.load(); }
void Undetected::Tick()
{
    BodyHide::Anchor an[BodyHide::aCount]; BodyHide::GetAnchors(an);
    if (an[BodyHide::aPlayer].valid && an[BodyHide::aPlayer].ent)
    {
        const uint32_t id = ReadId(an[BodyHide::aPlayer].ent);
        if (id && id != g_PlayerId.load())
        {
            g_PlayerId = id;
            LOGI("Undetected: the player's entity id {:#x}", id);
        }
    }
    if (!g_On.load() || !Log::Enabled()) return;
    const double now = Now();
    if (now - g_LogAt < 30.0) return;
    g_LogAt = now;
    const unsigned long long m = g_Masked.load();
    if (m != g_LoggedMasked)
    {
        LOGI("Undetected: in the last 30 s, {} reads of someone regarding you as an ENEMY turned to neutral ({} in all; someone turning on you not written: {}; the player regarding others as enemies: {}, left alone)",
             m - g_LoggedMasked, m, g_Blocked.load(), g_PlayerRegards.load());
        g_LoggedMasked = m;
    }
}
std::string Undetected::Status()
{
    return fmt::format("undetected {} (enemy reads of you turned neutral {}, turns against you not written {})", g_On.load() ? "on" : "off", g_Masked.load(), g_Blocked.load());
}
