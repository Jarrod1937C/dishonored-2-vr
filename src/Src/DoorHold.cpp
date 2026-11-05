#include "pch.h"
#include "DoorHold.h"
#include "GameInfo.h"
#include <detours/detours.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    // exe+0x6E2650: an animation player's update, (player, lod, time as a double in r8, a4, skipTime, a6, a7) -> u16;
    // dt = time - [player+0x38], kept at player+0x40 (exe+0x6E26C2), the graph stepped by it.
    using PlayerUpd_t = uint16_t(*)(void* player, uint16_t lod, uint64_t timeBits, uint8_t a4, uint8_t skipTime, void* a6, void* a7);
    constexpr uintptr_t kPlayerUpd = 0x6E2650;
    constexpr uint8_t kPlayerUpdBytes[7] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10 };
    constexpr uintptr_t kGetComponent = 0x877720, kOpenableType = 0x39C59A0;
    constexpr uintptr_t kVtAnimated = 0x1C56948;           // arkStateOpenable_Animated (+0x28 the phase: 1 opening, 2 closing)
    constexpr size_t kCompAnim = 0xF8, kAnimPlayer = 0x338, kPlayerTime = 0x38;
    constexpr size_t kEntPos = 0x300, kEntRot = 0x30C;     // the door entity's origin (on the hinge) and rotation rows
    constexpr float kOmega = 2.8f;     // rad/s: the swing's speed at rate 1 (measured 2.77 on two doors)
    constexpr float kGain = 8.f;       // 1/s: the real hand this far past the surface asks for this edge speed
    constexpr float kShove = 1.8f;     // m/s into the door: let it swing on by itself
    constexpr int kSlots = 6;

    struct Slot
    {
        std::atomic<void*> player{ nullptr };   // animation+0x338: what the hook matches
        std::atomic<float> rate{ 0.f };
        std::atomic<int> release{ 0 };          // 1 = hand the player back to real time at its next update
        std::atomic<bool> interacted{ false };  // the game's interact was sent to this door (Collision::NoteInteract)
        // the hook's thread only
        bool init = false; double lastReal = 0, virt = 0;
        // game thread
        void* door = nullptr; void* comp = nullptr; void* anim = nullptr;
        double t0 = 0, ourAt = 0, relAt = 0, lastPress = 0;
        bool seenAnim = false; int sign = 0; float yaw = 0;
        float want[2] = {}; double wantAt[2] = {};
        float moved = 0;   // rad the swing advanced while held (the log)
        char name[48] = {};
    };
    Slot g_S[kSlots];
    std::atomic<int> g_Used{ 0 };
    std::atomic<bool> g_On{ false };
    bool g_Installed = false;
    PlayerUpd_t g_Real = nullptr;
    std::atomic<unsigned long long> g_Holds{ 0 }, g_Lets{ 0 }, g_Hooked{ 0 };

    // ---- POD helpers (__try) ----
    bool Rd(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void* PtrAt(const void* base, size_t off) { void* p = nullptr; if (!base || !Rd(&p, (const uint8_t*)base + off, 8)) return nullptr; return p; }
    void* OpenableOf(void* ent)
    {
        __try { return ((void* (*)(const void*, const void*, int))(GameInfo::Base() + kGetComponent))(ent, (const void*)(GameInfo::Base() + kOpenableType), 2); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    //! the openable's current state when it is Animated: its phase (1 opening, 2 closing), else -1
    int AnimPhase(const void* comp)
    {
        const uintptr_t vt = GameInfo::Base() + kVtAnimated;
        for (size_t off = 0x40; off < 0x60; off += 8)
        {
            const void* st = PtrAt(comp, off);
            uintptr_t v = 0;
            if (!st || (uintptr_t)st < 0x10000 || !Rd(&v, st, 8) || v != vt) continue;
            int ph = 0; Rd(&ph, (const uint8_t*)st + 0x28, 4);
            return ph < 0 ? 0 : ph;
        }
        return -1;
    }
    void DoorName(const void* ent, char* out, size_t cap)
    {
        out[0] = 0;
        const char* s = (const char*)PtrAt(ent, 0x20);
        if (!s) return;
        char tmp[48] = {};
        if (!Rd(tmp, s, sizeof tmp - 1)) return;
        size_t i = 0; for (; i + 1 < cap && i < sizeof tmp - 1 && tmp[i] >= 32 && tmp[i] < 127; i++) out[i] = tmp[i];
        out[i] = 0;
    }
    bool DoorPose(const void* ent, float h[3], float& yaw)
    {
        float r[2];
        if (!Rd(h, (const uint8_t*)ent + kEntPos, 12) || !Rd(r, (const uint8_t*)ent + kEntRot, 8)) return false;
        if (!std::isfinite(h[0]) || !std::isfinite(r[0]) || !std::isfinite(r[1])) return false;
        yaw = atan2f(r[1], r[0]);
        return true;
    }

    uint16_t HkPlayerUpd(void* pl, uint16_t lod, uint64_t tb, uint8_t a4, uint8_t skipTime, void* a6, void* a7)
    {
        if (g_Used.load(std::memory_order_relaxed) == 0 || skipTime) return g_Real(pl, lod, tb, a4, skipTime, a6, a7);
        for (Slot& s : g_S)
        {
            if (s.player.load(std::memory_order_acquire) != pl) continue;
            g_Hooked++;
            double real; memcpy(&real, &tb, 8);
            double* last = (double*)((uint8_t*)pl + kPlayerTime);
            if (!s.init) { s.init = true; s.lastReal = *last; s.virt = *last; }   // it ran on the game's time until now
            double d = real - s.lastReal;
            if (!(d >= 0.0)) d = 0.0;
            if (d > 0.25) d = 0.25;
            s.lastReal = real;
            if (s.release.load())
            {   // back on the game's time: this step is the real one, no jump
                *last = real - d;
                s.player.store(nullptr);
                g_Used--;
                return g_Real(pl, lod, tb, a4, skipTime, a6, a7);
            }
            s.virt += d * (double)std::clamp(s.rate.load(std::memory_order_relaxed), 0.f, 1.f);
            uint64_t vb; memcpy(&vb, &s.virt, 8);
            return g_Real(pl, lod, vb, a4, skipTime, a6, a7);
        }
        return g_Real(pl, lod, tb, a4, skipTime, a6, a7);
    }

    void Install()
    {
        if (g_Installed) return;
        g_Installed = true;
        const uintptr_t b = GameInfo::Base();
        uint8_t have[7] = {};
        if (!Rd(have, (const void*)(b + kPlayerUpd), 7) || memcmp(have, kPlayerUpdBytes, 7) != 0)
        {
            LOGI("DoorHold: the animation players' update exe+0x{:X} is not as expected (bytes {:02X} {:02X} {:02X} {:02X}): doors swing as the game has them",
                 kPlayerUpd, have[0], have[1], have[2], have[3]);
            return;
        }
        g_Real = (PlayerUpd_t)(b + kPlayerUpd);
        DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_Real, HkPlayerUpd);
        const LONG e = DetourTransactionCommit();
        if (e != NO_ERROR) g_Real = nullptr;
        LOGI("DoorHold: animation players' update exe+0x{:X} hooked (detour={}): a pushed door's swing goes as far as the hand pushes", kPlayerUpd, e);
    }

    Slot* Find(void* door)
    {
        for (Slot& s : g_S) if (s.door == door && s.player.load()) return &s;
        return nullptr;
    }
    void Let(Slot& s, const char* why, double now)
    {
        if (!s.player.load() || s.release.load()) return;
        s.release = 1; s.relAt = now;
        g_Lets++;
        LOGI("DoorHold: door '{}' let go ({}): it moved {:.0f} deg while held", s.name, why, s.moved * 57.29578f);
    }
}

void DoorHold::SetEnabled(bool on)
{
    g_On = on;
    if (on) Install();
    if (!on) { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); for (Slot& s : g_S) Let(s, "DoorsFollowHand off", (double)c.QuadPart / (double)f.QuadPart); }
    LOGI("DoorHold: {}", on && g_Real ? "doors open as far as a hand pushes them (DoorsFollowHand)" : "doors swing on by themselves once pushed");
}
bool DoorHold::Enabled() { return g_On.load() && g_Real != nullptr; }
bool DoorHold::Held(void* door) { const Slot* s = Find(door); return s && !s->release.load(); }

void DoorHold::Begin(void* door, double now)
{
    if (!Enabled() || !door) return;
    if (Slot* s = Find(door)) { if (!s->release.load()) s->ourAt = now; return; }
    void* comp = OpenableOf(door);
    void* anim = PtrAt(comp, kCompAnim);
    if (!comp || !anim) { LOGI("DoorHold: a pushed door has no openable animation: it swings as the game has it"); return; }
    void* player = (uint8_t*)anim + kAnimPlayer;
    for (Slot& s : g_S)
    {
        if (s.player.load()) continue;
        s.init = false; s.lastReal = s.virt = 0;
        s.door = door; s.comp = comp; s.anim = anim;
        s.t0 = now; s.ourAt = now; s.relAt = 0; s.lastPress = 0;
        s.seenAnim = false; s.sign = 0; s.moved = 0; s.interacted.store(false);
        float h[3]; if (!DoorPose(door, h, s.yaw)) s.yaw = 0;
        s.want[0] = s.want[1] = 0; s.wantAt[0] = s.wantAt[1] = 0;
        DoorName(door, s.name, sizeof s.name);
        s.rate.store(0.f); s.release.store(0);
        g_Used++;
        s.player.store(player, std::memory_order_release);
        g_Holds++;
        LOGI("DoorHold: door '{}' held: its swing goes as far as the hand pushes (player {})", s.name, player);
        return;
    }
    LOGI("DoorHold: {} doors held already: this one swings as the game has it", kSlots);
}

int DoorHold::Press(void* door, int hand, const float p[3], const float n[3], float past, float into, double now)
{
    if (!Enabled()) return 0;
    Slot* s = Find(door);
    if (!s || s->release.load()) return 0;
    float h[3], yaw;
    if (!DoorPose(door, h, yaw)) return 0;
    const float rx = p[0] - h[0], ry = p[1] - h[1];
    const float r = std::max(0.2f, sqrtf(rx * rx + ry * ry));
    float push[2] = { -n[0], -n[1] };
    const float pl = sqrtf(push[0] * push[0] + push[1] * push[1]);
    if (pl < 0.2f) return 1;                         // pressing from above / below: nothing to turn it with
    push[0] /= pl; push[1] /= pl;
    if (s->sign != 0)
    {   // where that point of the door goes as the swing advances (counter-clockwise from above for sign +1)
        const float m[2] = { -ry * (float)s->sign / r, rx * (float)s->sign / r };
        if (push[0] * m[0] + push[1] * m[1] < -0.3f) return -1;
    }
    if (into > kShove) { Let(*s, "a quick shove: it swings on by itself", now); return 1; }
    const float edge = std::max(0.f, into) + kGain * std::max(0.f, past - 0.005f);
    const int k = hand & 1;
    s->want[k] = std::clamp(edge / (kOmega * r), 0.f, 1.f);
    s->wantAt[k] = now;
    s->lastPress = now;
    return 1;
}

void DoorHold::Tick(double now)
{
    if (g_Used.load() == 0) return;
    for (Slot& s : g_S)
    {
        if (!s.player.load()) continue;
        if (s.release.load())
        {   // the hook hands it back at the player's next update; a player that no longer updates (its level gone)
            if (now - s.relAt > 3.0)
            {
                s.player.store(nullptr); g_Used--;
                LOGI("DoorHold: door '{}' freed (its animation no longer updates)", s.name);
            }
            continue;
        }
        if (PtrAt(s.comp, kCompAnim) != s.anim) { Let(s, "no longer the same door", now); continue; }
        if (s.interacted.exchange(false))
        {   // the game's interact on it: the hand's own (a push from the other side turns the swing round) or not
            if (now - s.ourAt < 2.0) { s.sign = 0; LOGI("DoorHold: door '{}' turned round by the hand's push: still held", s.name); }
            else { Let(s, "the interact (the trigger, the grip's pull): it swings as the game has it", now); continue; }
        }
        // the rate: the hands pressing on it this frame
        float rate = 0.f;
        for (int k = 0; k < 2; k++) if (now - s.wantAt[k] < 0.06) rate = std::max(rate, s.want[k]);
        s.rate.store(rate, std::memory_order_relaxed);
        // which way it turns as it advances (learnt from the swing itself), and how far it went
        float h[3], yaw;
        if (DoorPose(s.door, h, yaw))
        {
            float dy = yaw - s.yaw; while (dy > 3.14159265f) dy -= 6.2831853f; while (dy < -3.14159265f) dy += 6.2831853f;
            if (fabsf(dy) > 0.004f) { if (rate > 0.f) s.sign = dy > 0 ? 1 : -1; s.moved += fabsf(dy); s.yaw = yaw; }
        }
        // (the Animated state's +0x28 says only which way the door moved last frame: it flips as a stopped door
        // settles, so it is not read as a change of direction)
        if (AnimPhase(s.comp) >= 0) s.seenAnim = true;
        else if (s.seenAnim) { Let(s, "its swing ended", now); continue; }
        else if (now - std::max(s.t0, s.ourAt) > 2.5) { Let(s, "no swing started", now); continue; }
    }
}

void DoorHold::OnInteract(void* door)
{
    if (!door || g_Used.load() == 0) return;
    for (Slot& s : g_S) if (s.door == door && s.player.load() && !s.release.load()) s.interacted.store(true);
}

std::string DoorHold::Status()
{
    int held = 0; for (const Slot& s : g_S) if (s.player.load() && !s.release.load()) held++;
    return fmt::format("doors held {} (holds {}, let go {}, held updates {}){}", held, g_Holds.load(), g_Lets.load(), g_Hooked.load(),
                       g_On.load() ? (g_Real ? "" : " (hook not installed)") : " (off)");
}
