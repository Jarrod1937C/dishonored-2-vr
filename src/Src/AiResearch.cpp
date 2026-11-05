#include "pch.h"
#include "AiResearch.h"
#include "GameInfo.h"
#include "BodyHide.h"
#include "Collision.h"
#include <detours/detours.h>
#include <atomic>
#include <mutex>
#include <cstring>

namespace
{
    constexpr uintptr_t kPost = 0x8112C0, kHear = 0x5EBF80;
    using FnPost = void* (*)(void*, uint32_t, uint32_t, uint32_t);
    using FnHear = uint64_t (*)(void*, void*, void*, uint8_t);   // (its return passed through, whatever it is)
    FnPost g_RealPost = nullptr; FnHear g_RealHear = nullptr;
    std::atomic<unsigned long long> g_Posts{ 0 }, g_Hears{ 0 };

    struct Kind { uint32_t type; const char* name; };
    const Kind kKinds[] = { { 0x23, "AI_QUEUE_STIM" }, { 0x24, "AI_PANIC_INDUCED" }, { 0x87, "AI_HEARD_SOUND" }, { 0x8B, "AI_PANIC" }, { 0x8C, "AI_PANIC_CLEAR" },
                            { 0x53, "AI_ESCALATE_TO_ALERT" }, { 0x54, "AI_ESCALATE_TO_SUSPICIOUS" }, { 0xD5, "AI_SUSPICION_LEVEL_CHANGED" }, { 0x8F, "AI_GREETINGS_START" }, { 0x90, "AI_GREETINGS_FINISHED" }, { 0xFB, "ENTITY_HEARD_BY_AI" }, { 0x1B0, "AI_PANIC_FINISHED" },
                            { 0x1C4, "AI_PICKPOCKETED_ON" }, { 0x1C5, "AI_PICKPOCKETED_OFF" } };
    int KindOf(uint32_t t) { for (int i = 0; i < (int)(sizeof kKinds / sizeof kKinds[0]); i++) if (kKinds[i].type == t) return i; return -1; }
    int g_PerKind[16] = {};   // lines so far per kind (under g_M)
    constexpr int kPerKind = 40;

    struct Pending { int kind; uint32_t id, size; void* pl; uintptr_t stack[6]; int frames; };
    std::mutex g_M; Pending g_Pend[64]; int g_NPend = 0;

    bool Rd(void* d, const void* s, size_t n) { __try { memcpy(d, s, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
    std::string Stack(const uintptr_t* f, int n)
    {
        std::string s; const uintptr_t b = GameInfo::Base();
        for (int i = 0; i < n; i++) s += fmt::format("{}{}", i ? " < " : "", f[i] >= b && f[i] < b + 0x4000000 ? fmt::format("exe+{:#x}", f[i] - b) : std::string("?"));
        return s;
    }
    std::string Hex(const void* p, size_t n)
    {
        uint8_t b[0x40] = {}; if (n > sizeof b) n = sizeof b;
        if (!p || !Rd(b, p, n)) return "?";
        std::string s; for (size_t i = 0; i < n; i++) s += fmt::format("{:02x}{}", b[i], (i % 4 == 3 && i + 1 < n) ? " " : "");
        return s;
    }

    void* Post(void* queue, uint32_t type, uint32_t size, uint32_t id)
    {
        void* pl = g_RealPost(queue, type, size, id);
        const int k = KindOf(type);
        if (k < 0) return pl;
        g_Posts++;
        std::lock_guard lk(g_M);
        if (g_PerKind[k] >= kPerKind || g_NPend >= 64) return pl;
        g_PerKind[k]++;
        Pending& P = g_Pend[g_NPend++];
        P.kind = k; P.id = id; P.size = size; P.pl = pl;
        void* fr[8] = {}; const USHORT n = RtlCaptureStackBackTrace(1, 6, fr, nullptr);
        P.frames = n; for (int i = 0; i < n; i++) P.stack[i] = (uintptr_t)fr[i];
        return pl;
    }
    uint64_t Hear(void* comp, void* src, void* snd, uint8_t b)
    {
        static std::atomic<int> logs{ 0 };
        if (logs.load() < 150)
        {
            logs++; g_Hears++;
            void* fr[8] = {}; const USHORT n = RtlCaptureStackBackTrace(1, 6, fr, nullptr);
            uintptr_t st[6] = {}; for (int i = 0; i < n; i++) st[i] = (uintptr_t)fr[i];
            void* hearer = nullptr; Rd(&hearer, (uint8_t*)comp + 0x10, 8);
            LOGI("AiResearch: HEARD: {} hears from {} (flag {}): the sound {} -- from {}", hearer ? Collision::Describe(hearer) : std::string("?"),
                 src ? Collision::Describe(src) : std::string("nothing"), b, Hex(snd, 0x40), Stack(st, n));
        }
        return g_RealHear(comp, src, snd, b);
    }
    bool Attach(uintptr_t rva, const uint8_t* expect, size_t n, void** real, void* hook, const char* what)
    {
        const uint8_t* p = (const uint8_t*)(GameInfo::Base() + rva);
        if (p[0] != 0xE9 && memcmp(p, expect, n) != 0) { LOGI("AiResearch: exe+0x{:X} is not {} (another build?): not watched", rva, what); return false; }
        *real = (void*)p;
        DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
        DetourAttach(real, hook);
        const LONG r = DetourTransactionCommit();
        if (r != NO_ERROR) { *real = nullptr; LOGI("AiResearch: {} not watched (detour {})", what, r); return false; }
        return true;
    }
}

void AiResearch::Install()
{
    if (!Log::Enabled() || !GameInfo::IsSupportedBuild() || g_RealPost) return;
    static const uint8_t kPostBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x44, 0x89, 0x4C, 0x24, 0x20 };
    static const uint8_t kHearBytes[] = { 0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0x55, 0x41, 0x54 };
    const bool a = Attach(kPost, kPostBytes, sizeof kPostBytes, (void**)&g_RealPost, (void*)Post, "the entity message post");
    const bool b = Attach(kHear, kHearBytes, sizeof kHearBytes, (void**)&g_RealHear, (void*)Hear, "an NPC hearing a sound");
    LOGI("AiResearch: research logging for the gestures at people: the AI's messages {}, what NPCs hear {}", a ? "watched" : "NOT watched", b ? "watched" : "NOT watched");
}
void AiResearch::Tick()
{
    Pending pend[64]; int n = 0;
    { std::lock_guard lk(g_M); n = g_NPend; memcpy(pend, g_Pend, sizeof(Pending) * n); g_NPend = 0; }
    for (int i = 0; i < n; i++)
    {
        const Pending& P = pend[i];
        void* e = BodyHide::EntityById((int)P.id);
        LOGI("AiResearch: MSG {} ({:#x}) to {:#x} {}: payload {} bytes [{}] -- posted from {}", kKinds[P.kind].name, kKinds[P.kind].type, P.id,
             e ? Collision::Describe(e) : std::string("(not an entity now)"), P.size, Hex(P.pl, P.size), Stack(P.stack, P.frames));
    }
}
std::string AiResearch::Status() { return fmt::format("ai research: messages {}, sounds heard {}", g_Posts.load(), g_Hears.load()); }
