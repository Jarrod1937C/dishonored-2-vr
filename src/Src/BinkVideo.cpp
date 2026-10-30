#include "pch.h"
#include "BinkVideo.h"
#include <detours/detours.h>

namespace
{
    // Every hook only passes its register arguments through (x64: rcx, rdx, r8, r9), so the declared signatures need
    // not match the library's exactly beyond the first argument, which is all that is read.
    using OpenFn = void* (*)(const char* name, uint64_t flags);
    using OpenOptFn = void* (*)(const char* name, const void* options, uint64_t flags);
    using CloseFn = void (*)(void* bink);
    using DoFrameFn = int (*)(void* bink);
    using DoFrameAsyncFn = int (*)(void* bink, uint64_t a, uint64_t b);
    using NextFrameFn = void (*)(void* bink);
    OpenFn g_Open = nullptr; OpenOptFn g_OpenOpt = nullptr; CloseFn g_Close = nullptr;
    DoFrameFn g_DoFrame = nullptr; DoFrameAsyncFn g_DoFrameAsync = nullptr; NextFrameFn g_NextFrame = nullptr;
    std::atomic<bool> g_Installed{ false };

    struct Video { void* h = nullptr; char name[96] = {}; unsigned long long frames = 0; LARGE_INTEGER last{}; bool skip = false; };
    // [Advanced] SkipIntroVideos: the start-up videos (the legal and warning screens, the publisher's and the studio's
    // logos -- legal / warning / arkbeth / void.bk2, ~25 s) are sent to their last frame on their first frame
    // (BinkGoto), so the game ends them at once. The game's own +com_showLoadingScreen 0 also skips them, but the mod
    // then does not start properly (reported).
    using GotoFn = void (*)(void* bink, uint32_t frame, int32_t flags);
    GotoFn g_Goto = nullptr;
    std::atomic<bool> g_SkipIntros{ false };
    bool IsIntro(const char* n) { return !_stricmp(n, "legal.bk2") || !_stricmp(n, "warning.bk2") || !_stricmp(n, "arkbeth.bk2") || !_stricmp(n, "void.bk2"); }
    uint32_t FrameCount(void* h) { __try { return *(const uint32_t*)((const uint8_t*)h + 8); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
    constexpr int kMax = 8;
    std::mutex g_M;
    Video g_V[kMax];
    unsigned long long g_Opens = 0, g_Closes = 0, g_Frames = 0;
    std::string g_LastName;
    constexpr double kPlayingWindow = 0.5;   // s: a frame this recent = still playing
    //! Before a video's frame is decoded: an intro to skip goes to its end (once).
    void MaybeSkip(void* h)
    {
        bool go = false;
        {
            std::lock_guard lk(g_M);
            for (auto& v : g_V) if (v.h == h && v.skip) { v.skip = false; go = true; }
        }
        if (!go || !g_Goto) return;
        const uint32_t n = FrameCount(h);
        if (n > 1 && n < 100000) { g_Goto(h, n, 0); LOGI("BinkVideo: intro video skipped (to its frame {})", n); }
    }

    //! The file name of a Bink open call; a handle-based open (no name) or an unreadable pointer gives "?".
    void ReadName(const char* p, char out[96])
    {
        strcpy_s(out, 96, "?");
        if (!p) return;
        __try
        {
            int i = 0;
            for (; i < 95 && p[i]; i++) { if ((unsigned char)p[i] < 0x20) { strcpy_s(out, 96, "?"); return; } out[i] = p[i]; }
            out[i] = 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, 96, "?"); }
    }
    const char* BaseName(const char* s) { const char* b = s; for (const char* p = s; *p; p++) if (*p == '/' || *p == '\\') b = p + 1; return b; }

    void Opened(void* h, const char* name)
    {
        if (!h) return;
        char n[96]; ReadName(name, n);
        std::lock_guard lk(g_M);
        g_Opens++;
        Video* slot = nullptr;
        for (auto& v : g_V) if (v.h == h) slot = &v;
        if (!slot) for (auto& v : g_V) if (!v.h) { slot = &v; break; }
        if (!slot) slot = &g_V[0];
        *slot = Video(); slot->h = h; strcpy_s(slot->name, BaseName(n));
        slot->skip = g_SkipIntros.load() && IsIntro(slot->name);
        g_LastName = slot->name;
        LOGI("BinkVideo: opened {} ({})", slot->name, h);
    }
    void Frame(void* h)
    {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        std::lock_guard lk(g_M);
        g_Frames++;
        for (auto& v : g_V) if (v.h == h) { v.frames++; v.last = now; return; }
    }

    void* HookOpen(const char* name, uint64_t flags) { void* h = g_Open(name, flags); Opened(h, name); return h; }
    void* HookOpenOpt(const char* name, const void* o, uint64_t flags) { void* h = g_OpenOpt(name, o, flags); Opened(h, name); return h; }
    void HookClose(void* h)
    {
        {
            std::lock_guard lk(g_M);
            for (auto& v : g_V) if (v.h == h)
            {
                g_Closes++;
                LOGI("BinkVideo: closed {} after {} frames", v.name, v.frames);
                v = Video();
            }
        }
        g_Close(h);
    }
    int HookDoFrame(void* h) { MaybeSkip(h); Frame(h); return g_DoFrame(h); }
    int HookDoFrameAsync(void* h, uint64_t a, uint64_t b) { MaybeSkip(h); Frame(h); return g_DoFrameAsync(h, a, b); }
    void HookNextFrame(void* h) { Frame(h); g_NextFrame(h); }

    bool PlayingLocked(const Video** which)
    {
        LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
        for (const auto& v : g_V)
            if (v.h && v.frames && (double)(now.QuadPart - v.last.QuadPart) / (double)fq.QuadPart < kPlayingWindow)
            { if (which) *which = &v; return true; }
        return false;
    }
}

void BinkVideo::Install()
{
    if (g_Installed.load()) return;
    HMODULE m = GetModuleHandleW(L"bink2w64.dll");
    if (!m) return;
    g_Open = (OpenFn)GetProcAddress(m, "BinkOpen");
    g_OpenOpt = (OpenOptFn)GetProcAddress(m, "BinkOpenWithOptions");
    g_Close = (CloseFn)GetProcAddress(m, "BinkClose");
    g_DoFrame = (DoFrameFn)GetProcAddress(m, "BinkDoFrame");
    g_DoFrameAsync = (DoFrameAsyncFn)GetProcAddress(m, "BinkDoFrameAsync");
    g_NextFrame = (NextFrameFn)GetProcAddress(m, "BinkNextFrame");
    g_Goto = (GotoFn)GetProcAddress(m, "BinkGoto");
    g_Installed = true;    // one attempt once the library is there, whatever comes of it
    if (!g_Close || !(g_Open || g_OpenOpt) || !(g_DoFrame || g_DoFrameAsync || g_NextFrame))
    { LOGI("BinkVideo: bink2w64.dll lacks the expected exports: videos are not tracked"); return; }
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    if (g_Open) DetourAttach(&(PVOID&)g_Open, HookOpen);
    if (g_OpenOpt) DetourAttach(&(PVOID&)g_OpenOpt, HookOpenOpt);
    DetourAttach(&(PVOID&)g_Close, HookClose);
    if (g_DoFrame) DetourAttach(&(PVOID&)g_DoFrame, HookDoFrame);
    if (g_DoFrameAsync) DetourAttach(&(PVOID&)g_DoFrameAsync, HookDoFrameAsync);
    if (g_NextFrame) DetourAttach(&(PVOID&)g_NextFrame, HookNextFrame);
    const LONG r = DetourTransactionCommit();
    LOGI("BinkVideo: bink2w64.dll hooked (open {} openopt {} close 1 doframe {} async {} next {}), detour={}",
         g_Open != nullptr, g_OpenOpt != nullptr, g_DoFrame != nullptr, g_DoFrameAsync != nullptr, g_NextFrame != nullptr, r);
}

bool BinkVideo::Installed() { return g_Installed.load(); }
void BinkVideo::SetSkipIntros(bool on) { g_SkipIntros = on; if (on) LOGI("BinkVideo: the start-up videos are skipped (SkipIntroVideos)"); }
bool BinkVideo::Playing() { std::lock_guard lk(g_M); return PlayingLocked(nullptr); }
std::string BinkVideo::Current() { std::lock_guard lk(g_M); const Video* v = nullptr; return PlayingLocked(&v) ? std::string(v->name) : std::string(); }
std::string BinkVideo::Status()
{
    std::lock_guard lk(g_M);
    const Video* v = nullptr; const bool playing = PlayingLocked(&v);
    int open = 0; for (const auto& x : g_V) if (x.h) open++;
    return fmt::format("bink hooked {} playing {} current {} open {} opens {} closes {} frames {} last {}", g_Installed.load() ? 1 : 0, playing ? 1 : 0,
                       playing ? v->name : "-", open, g_Opens, g_Closes, g_Frames, g_LastName.empty() ? "-" : g_LastName);
}
