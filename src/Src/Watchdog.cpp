#include "pch.h"
#include "Watchdog.h"
#include "CrashLog.h"
#include "GameInfo.h"
#include "Settings.h"
#include <fstream>
#include <sstream>

namespace
{
    double Now()
    {
        static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        return (double)c.QuadPart / (double)f.QuadPart;
    }

    // ---- the marks: which OpenXR call a watched thread is in (0 = the render thread, 1 = the OpenXR frame thread) ----
    std::atomic<DWORD> g_Tid[2]{};
    std::atomic<const char*> g_Where[2]{};
    std::atomic<long long> g_Since[2]{};      // microseconds (QPC) the current mark began
    long long NowUs() { return (long long)(Now() * 1e6); }
    int SlotOfThisThread()
    {
        const DWORD t = GetCurrentThreadId();
        return t == g_Tid[0].load() ? 0 : t == g_Tid[1].load() ? 1 : -1;
    }

    // ---- the heartbeat ----
    std::atomic<double> g_LastPresent{ 0.0 };
    HWND g_GameWnd = nullptr;
    std::atomic<int> g_XrState{ 0 };
    std::atomic<int> g_StallMs{ 0 };
    std::atomic<bool> g_InStall{ false };
    std::atomic<unsigned> g_Stalls{ 0 }, g_Hitches{ 0 };
    std::atomic<double> g_LongestStall{ 0.0 }, g_LongestGap{ 0.0 };
    std::string g_LastStall;                  // one line about the last stall, for the heartbeat (under g_M)
    std::mutex g_M;
    bool g_Installed = false;

    const char* XrStateName(int s)
    {
        static const char* n[] = { "unknown", "IDLE", "READY", "SYNCHRONIZED", "VISIBLE", "FOCUSED", "STOPPING", "LOSS_PENDING", "EXITING" };
        return s >= 0 && s <= 8 ? n[s] : "?";
    }

    //! The foreground window, without sending it a message (GetWindowText on a window of this process SENDS WM_GETTEXT,
    //! which would hang this thread exactly when the game's window is the one not answering).
    std::string Foreground()
    {
        HWND fg = GetForegroundWindow();
        if (!fg) return "none";
        DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
        using InternalGetWindowTextFn = int(WINAPI*)(HWND, LPWSTR, int);
        static auto igwt = (InternalGetWindowTextFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "InternalGetWindowText");
        wchar_t title[128] = {};
        if (igwt) igwt(fg, title, 127);
        wchar_t exe[MAX_PATH] = L"?";
        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
        {
            DWORD n = MAX_PATH; QueryFullProcessImageNameW(p, 0, exe, &n); CloseHandle(p);
        }
        const wchar_t* leaf = wcsrchr(exe, L'\\'); leaf = leaf ? leaf + 1 : exe;
        auto narrow = [](const wchar_t* w) { std::string s; for (; *w; w++) s += (char)(*w < 128 ? *w : '?'); return s; };
        return fmt::format("'{}' ({}{})", narrow(title), narrow(leaf), pid == GetCurrentProcessId() ? ", the game" : "");
    }

    //! A code pointer that a CALL pushed: the bytes before it are a call instruction. Filters the stack scan down to
    //! return addresses (a value that merely points into a module, a vtable or a string, is not one).
    bool ReadCode(const uint8_t* p, uint8_t* out, size_t n)
    {
        __try { memcpy(out, p, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool AfterCall(uintptr_t v)
    {
        uint8_t b[7];
        if (!ReadCode((const uint8_t*)(v - 7), b, 7)) return false;
        if (b[2] == 0xE8) return true;                                           // call rel32
        if (b[1] == 0xFF && ((b[2] >> 3) & 7) == 2) return true;                // call [rip+disp32] / [reg+disp32]
        if (b[4] == 0xFF && ((b[5] >> 3) & 7) == 2) return true;                // call [reg+disp8]
        if (b[5] == 0xFF && ((b[6] >> 3) & 7) == 2) return true;                // call reg / [reg]
        if (b[4] == 0x41 && b[5] == 0xFF) return true;                           // call r8-r15 (REX.B)
        return false;
    }
    bool IsCode(uintptr_t v)
    {
        HMODULE m = nullptr;
        return v > 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)v, &m) && m;
    }

    //! The render thread's instruction pointer and the return addresses on its stack. The thread is suspended only for
    //! GetThreadContext and a copy of its stack (ReadProcessMemory, no lock it could hold); everything else -- module
    //! lookups, formatting, the log, which all take locks the stuck thread might be holding -- runs after it resumed.
    void SampleRenderThread(const char* tag)
    {
        const DWORD tid = g_Tid[0].load();
        if (!tid || !Log::Enabled()) return;   // the stuck thread's calls are for the log only
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!th) { LOGI("Watchdog: {}: cannot open the render thread ({})", tag, GetLastError()); return; }
        CONTEXT cx = {}; cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        static uintptr_t stack[8192];        // 64 KB of the stuck thread's stack (one sampler at a time: the watchdog)
        SIZE_T got = 0;
        bool ok = false;
        if (SuspendThread(th) != (DWORD)-1)
        {
            ok = GetThreadContext(th, &cx) != 0;
            if (ok)
                for (SIZE_T off = 0; off < sizeof stack; off += 4096)
                {   // page by page from rsp up: the first unreadable page is past the stack's base
                    SIZE_T n = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(), (const uint8_t*)cx.Rsp + off, (uint8_t*)stack + off, 4096, &n) || n != 4096) break;
                    got = off + 4096;
                }
            ResumeThread(th);
        }
        CloseHandle(th);
        if (!ok) { LOGI("Watchdog: {}: no context for the render thread", tag); return; }
        std::string calls;
        int found = 0;
        for (SIZE_T i = 0; i < got / 8 && found < 28; i++)
        {
            const uintptr_t v = stack[i];
            if (!IsCode(v) || !AfterCall(v)) continue;
            calls += (found ? " " : "") + CrashLog::Where((void*)v);
            found++;
        }
        LOGI("Watchdog: {}: render thread at {} | called from: {}", tag, CrashLog::Where((void*)cx.Rip), calls.empty() ? "(nothing recognisable on its stack)" : calls);
    }

    std::string WhereText(int slot, double now)
    {
        const char* w = g_Where[slot].load();
        if (!w) return slot == 0 ? "in the game's own code (no OpenXR call)" : "idle (no OpenXR call)";
        return fmt::format("in {} for {:.1f} s", w, now - (double)g_Since[slot].load() * 1e-6);
    }

    void Report(const char* tag, double gap)
    {
        const double now = Now();
        LOGI("Watchdog: {}: no Present for {:.1f} s. Render thread {}; OpenXR frame thread {}; session {}; foreground {}; game window {}",
             tag, gap, WhereText(0, now), g_Tid[1].load() ? WhereText(1, now) : std::string("not running"), XrStateName(g_XrState.load()),
             Foreground(), g_GameWnd ? (IsHungAppWindow(g_GameWnd) ? "NOT RESPONDING (Windows ghosts it)" : "responding") : "?");
        SampleRenderThread(tag);
        std::lock_guard lk(g_M);
        const char* w = g_Where[0].load();
        g_LastStall = fmt::format("{:.1f}s {}", gap, w ? w : "game");
    }

    void WriteFile(double now)
    {
        const double last = g_LastPresent.load();
        std::ostringstream o;
        o << "present_gap " << (last > 0 ? now - last : -1) << "\n"
          << "in_stall " << g_InStall.load() << "\n"
          << "stalls " << g_Stalls.load() << " longest " << g_LongestStall.load() << "\n"
          << "render_where " << (g_Where[0].load() ? g_Where[0].load() : "game") << "\n"
          << "frame_where " << (g_Tid[1].load() ? (g_Where[1].load() ? g_Where[1].load() : "idle") : "none") << "\n"
          << "xr_state " << XrStateName(g_XrState.load()) << "\n";
        std::ofstream f(GameInfo::GameDir() + L"\\DVR_watchdog.txt", std::ios::trunc);
        f << o.str();
    }

    DWORD WINAPI Thread(LPVOID)
    {
        double nextFile = 0;
        int reports = 0;
        for (;;)
        {
            Sleep(100);
            const double now = Now(), last = g_LastPresent.load();
            if (now >= nextFile) { nextFile = now + 0.5; if (Settings::StatusFile()) WriteFile(now); }   // with the heartbeat
            if (last <= 0) continue;                   // the game has not presented yet
            const double gap = now - last;
            if (!g_InStall.load() && gap > 2.0)
            {
                g_InStall = true; g_Stalls++; reports = 0;
                Report(fmt::format("STALL #{}", g_Stalls.load()).c_str(), gap);
            }
            else if (g_InStall.load() && reports < 3 && now >= last + (reports == 0 ? 10.0 : reports == 1 ? 30.0 : 90.0))
            {
                reports++;
                Report(fmt::format("STALL #{} still", g_Stalls.load()).c_str(), gap);
            }
        }
    }
}

void Watchdog::Install()
{
    if (g_Installed) return;
    g_Installed = true;
    if (HANDLE h = CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr)) CloseHandle(h);
    LOGI("Watchdog: watching the game's Present (a stall of more than 2 s is reported with where the render thread is)");
}

void Watchdog::OnPresent(IDXGISwapChain* sc)
{
    const double now = Now();
    if (!g_Tid[0].load())
    {
        g_Tid[0] = GetCurrentThreadId();
        DXGI_SWAP_CHAIN_DESC d = {};
        if (sc && SUCCEEDED(sc->GetDesc(&d))) g_GameWnd = d.OutputWindow;
    }
    const double last = g_LastPresent.exchange(now);
    if (last > 0)
    {
        const double gap = now - last;
        if (gap > g_LongestGap.load()) g_LongestGap = gap;
        if (gap > 0.5) g_Hitches++;
        if (g_InStall.exchange(false))
        {
            if (gap > g_LongestStall.load()) g_LongestStall = gap;
            LOGI("Watchdog: STALL #{} over: Present is back after {:.1f} s (session {})", g_Stalls.load(), gap, XrStateName(g_XrState.load()));
        }
        else if (gap > 1.0) LOGI("Watchdog: a hitch: {:.2f} s between two Presents", gap);
    }
    if (const int ms = g_StallMs.exchange(0))
    {   // vr stall: a game hitch, on the render thread, where the game's own would be
        LOGI("Watchdog: test hitch: the render thread sleeps {} ms", ms);
        Sleep((DWORD)ms);
    }
}

void Watchdog::SetXrState(int state) { g_XrState = state; }
void Watchdog::RegisterFrameThread() { g_Tid[1] = GetCurrentThreadId(); }
unsigned long Watchdog::RenderThreadId() { return g_Tid[0].load(); }
unsigned long Watchdog::FrameThreadId() { return g_Tid[1].load(); }

Watchdog::Mark::Mark(const char* what) : slot(SlotOfThisThread()), prev(nullptr), prevSince(0)
{
    if (slot < 0) return;
    prev = g_Where[slot].exchange(what);
    prevSince = g_Since[slot].exchange(NowUs());
}
Watchdog::Mark::~Mark()
{
    if (slot < 0) return;
    g_Where[slot] = prev;
    g_Since[slot] = prevSince;
}

void Watchdog::Stall(int ms) { g_StallMs = ms < 0 ? 0 : (ms > 600000 ? 600000 : ms); LOGI("cmd vr stall {} ms (at the next Present)", g_StallMs.load()); }

std::string Watchdog::Status()
{
    std::lock_guard lk(g_M);
    return fmt::format("watchdog stalls {} longest {:.1f} last {} hitches {} longestgap {:.2f}",
                       g_Stalls.load(), g_LongestStall.load(), g_LastStall.empty() ? "-" : g_LastStall, g_Hitches.load(), g_LongestGap.load());
}
