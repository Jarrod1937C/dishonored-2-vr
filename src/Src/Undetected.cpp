#include "pch.h"
#include "Undetected.h"
#include "BodyHide.h"
#include "GameInfo.h"
#include <atomic>
#include <cstring>

namespace
{
    constexpr uintptr_t kGetComponent = 0x877720, kAttrType = 0x39EDB90, kFind = 0x60CE50;
    constexpr size_t kEntries = 0x48, kStride = 0x28, kValue = 0x24;
    struct Attr { const char* name; uintptr_t atom; float want; int idx = -1; float orig = 0.f; bool saved = false; };
    Attr g_A[] = {
        { "visibility", 0x32CD9F0, 0.f }, { "visibility when moving", 0x32CD9F8, 0.f },
        { "running footstep noise", 0x32CDA08, 0.f }, { "sprinting footstep noise", 0x32CDA10, 0.f },
        { "illumination", 0x32CDA88, 0.f }, { "invisible when still", 0x32CDA78, 1.f }, { "invisible when still crouched", 0x32CDA80, 1.f },
    };
    constexpr int kN = (int)(sizeof g_A / sizeof g_A[0]);
    std::atomic<bool> g_On{ false };
    void* g_Comp = nullptr; bool g_Resolved = false; int g_Found = 0;
    std::atomic<unsigned long long> g_Writes{ 0 };

    void* AttrComp(void* player)
    {
        __try { return ((void* (*)(void*, const void*, int))(GameInfo::Base() + kGetComponent))(player, (const void*)(GameInfo::Base() + kAttrType), 2); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    //! the entry of `atom` in the component's table, checked against its key (POD)
    float* Entry(void* comp, uintptr_t atom, int* idx)
    {
        __try
        {
            const uintptr_t b = GameInfo::Base();
            int out = -1;
            ((int* (*)(void*, int*, const void*))(b + kFind))(comp, &out, (const void*)(b + atom));
            *idx = out;
            if (out < 0 || out > 4096) return nullptr;
            uint8_t* entries = *(uint8_t**)((uint8_t*)comp + kEntries);
            if (!entries) return nullptr;
            uint8_t* e = entries + (size_t)out * kStride;
            if (*(const uint64_t*)e != *(const uint64_t*)(b + atom)) return nullptr;   // not that attribute's entry
            return (float*)(e + kValue);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    bool Wr(float* p, float v, float* old)
    {
        __try { if (old) *old = *p; if (*p != v) { *p = v; return true; } return false; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void Restore()
    {
        if (!g_Comp) return;
        int restored = 0;
        for (Attr& a : g_A)
        {
            if (!a.saved) continue;
            int idx; if (float* p = Entry(g_Comp, a.atom, &idx)) { Wr(p, a.orig, nullptr); restored++; }
            a.saved = false;
        }
        if (restored) LOGI("Undetected: off: {} of the player's stealth attributes put back as they were", restored);
    }
}

void Undetected::SetEnabled(bool on)
{
    if (!on && g_On.load()) { /* restored on the game thread */ }
    g_On = on;
    LOGI("Undetected: {}", on ? "on: guards and hostile people do not notice you (the cheat Undetected; a fight still makes them fight back)" : "off");
}
bool Undetected::Enabled() { return g_On.load(); }
void Undetected::Tick()
{
    if (!g_On.load()) { if (g_Resolved) { Restore(); g_Resolved = false; g_Comp = nullptr; } return; }
    BodyHide::Anchor an[BodyHide::aCount]; BodyHide::GetAnchors(an);
    if (!an[BodyHide::aPlayer].valid || !an[BodyHide::aPlayer].ent) return;
    void* comp = AttrComp(an[BodyHide::aPlayer].ent);
    if (!comp) return;
    if (comp != g_Comp)
    {   // a new player (a level loaded): find the entries again
        g_Comp = comp; g_Resolved = true; g_Found = 0;
        std::string list;
        for (Attr& a : g_A)
        {
            a.saved = false; int idx = -1;
            float* p = Entry(comp, a.atom, &idx);
            a.idx = p ? idx : -1;
            if (p) { float v = 0.f; Wr(p, *p, &v); a.orig = v; a.saved = true; g_Found++; }
            list += fmt::format("{}{} {}", list.empty() ? "" : ", ", a.name, p ? fmt::format("= {:.2f} (entry {})", a.orig, idx) : std::string("NOT FOUND"));
        }
        LOGI("Undetected: the player's attributes component {}: {} of {} stealth attributes found -- {}", comp, g_Found, kN, list);
    }
    for (Attr& a : g_A)
    {
        if (a.idx < 0) continue;
        int idx; float* p = Entry(comp, a.atom, &idx);
        if (p && Wr(p, a.want, nullptr)) g_Writes++;
    }
}
std::string Undetected::Status()
{
    return fmt::format("undetected {} ({} of {} attributes held; rewritten {} times)", g_On.load() ? "on" : "off", g_Found, kN, g_Writes.load());
}
