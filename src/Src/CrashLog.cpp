#include "pch.h"
#include "CrashLog.h"
#include <detours/detours.h>
#include <psapi.h>
#include <dbghelp.h>   // the types only: MiniDumpWriteDump is looked up at run time (no dbghelp.lib)
#include <shlobj.h>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <eh.h>

// See CrashLog.h. The report is written without the heap (it may be what broke): fixed buffers, snprintf, WriteFile.
namespace
{
    // ---- state ----
    std::atomic<int> g_Logged{ 0 };            // first-chance faults noted in DishonoredVR.log (a hint, not a crash)
    std::atomic<bool> g_InHandler{ false };
    constexpr int kMaxLogged = 6;
    const char* volatile g_Phase = "init";
    ULONGLONG g_Start = 0;
    std::atomic<bool> g_Claimed{ false };      // one report per process: the first crash wins
    LPTOP_LEVEL_EXCEPTION_FILTER g_Next = nullptr;   // the filter someone else set (called after ours)
    wchar_t g_Dir[MAX_PATH] = {}, g_AltDir[MAX_PATH] = {};
    char g_Info[CrashLog::kInfoCount][4096] = {};

    // the report's thread: asleep until a crash
    HANDLE g_Go = nullptr, g_Done = nullptr, g_Writer = nullptr;
    DWORD g_WriterId = 0;
    std::atomic<bool> g_WriterReady{ false };   // the thread runs (it cannot start before DllMain returns)
    struct Request { EXCEPTION_POINTERS* ep; DWORD tid; const char* what; } g_Req{};

    // minidump (dbghelp, loaded by Prepare)
    using DumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    DumpFn g_Dump = nullptr;

    // ---- names ----
    const char* CodeName(DWORD c)
    {
        switch (c)
        {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
        case 0xC0000409:                      return "STACK_BUFFER_OVERRUN/fastfail";
        case 0xC0000374:                      return "HEAP_CORRUPTION";
        case 0xC0000420:                      return "ASSERTION_FAILURE";
        case 0xE06D7363:                      return "C++ exception";
        case 0xE0DE0001:                      return "abort() in the mod";
        case 0xE0DE0002:                      return "std::terminate in the mod";
        case 0xE0DE0003:                      return "pure virtual call in the mod";
        case 0xE0DE0004:                      return "invalid CRT parameter in the mod";
        default:                              return "?";
        }
    }
    //! Faults worth a hint in the log at first chance (the engine raises C++ exceptions and the thread-name exception
    //! in normal operation: those are not).
    bool Interesting(DWORD c)
    {
        switch (c)
        {
        case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_IN_PAGE_ERROR: case 0xC0000409: case 0xC0000374:
            return true;
        default: return false;
        }
    }
    //! The codes that end the process whoever catches them (seen first chance: the fast fail and the heap's report come
    //! after, and do not reach the unhandled filter).
    bool Fatal(const EXCEPTION_RECORD* er)
    {
        if (!er) return false;
        switch (er->ExceptionCode)
        {
        case 0xC0000374: case 0xC0000409: case EXCEPTION_STACK_OVERFLOW: case 0xC0000420: return true;
        default: return (er->ExceptionFlags & EXCEPTION_NONCONTINUABLE) && er->ExceptionCode != 0xE06D7363;
        }
    }

    // ---- reading memory that may not be there (POD helpers: __try) ----
    bool SafeCopy(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool ReadPtr(const uintptr_t* p, uintptr_t& out) { return SafeCopy(&out, p, sizeof out); }
    //! One frame of the unwind: the caller's context from this one's, by the module's unwind data (x64 .pdata). False at
    //! the end (no return address) or when the stack is unreadable.
    bool UnwindStep(CONTEXT* c, DWORD64* fnStart)
    {
        __try
        {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c->Rip, &base, nullptr);
            *fnStart = f ? base + f->BeginAddress : 0;
            if (!f)
            {   // a leaf function: its return address is at the top of the stack
                c->Rip = *(DWORD64*)c->Rsp; c->Rsp += 8;
            }
            else
            {
                PVOID hd = nullptr; DWORD64 est = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, f, c, &hd, &est, nullptr);
            }
            return c->Rip != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // ---- the report's writer (no heap) ----
    HANDLE g_Out = INVALID_HANDLE_VALUE;
    char g_Line[8192];
    void Put(const char* s, size_t n) { DWORD w = 0; if (g_Out != INVALID_HANDLE_VALUE && n) WriteFile(g_Out, s, (DWORD)n, &w, nullptr); }
    void P(const char* f, ...)
    {
        va_list a; va_start(a, f);
        int n = _vsnprintf_s(g_Line, sizeof g_Line, _TRUNCATE, f, a);
        va_end(a);
        if (n < 0) n = (int)strlen(g_Line);
        Put(g_Line, (size_t)n); Put("\r\n", 2);
    }
    //! "<module>+0x<rva>" into a buffer
    void WhereBuf(const void* a, char* out, size_t n)
    {
        HMODULE m = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m)
        {
            wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(m, p, MAX_PATH);
            const wchar_t* leaf = wcsrchr(p, L'\\'); leaf = leaf ? leaf + 1 : p;
            _snprintf_s(out, n, _TRUNCATE, "%ls+0x%llX", leaf, (unsigned long long)((uintptr_t)a - (uintptr_t)m));
        }
        else _snprintf_s(out, n, _TRUNCATE, "0x%016llX", (unsigned long long)(uintptr_t)a);
    }
    bool InModule(uintptr_t v)
    {
        HMODULE m = nullptr;
        return v >= 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)v, &m) && m;
    }
    void Hex(const char* title, const void* at, size_t n)
    {
        uint8_t b[16];
        for (size_t o = 0; o < n; o += 16)
        {
            const uint8_t* p = (const uint8_t*)at + o;
            if (!SafeCopy(b, p, 16)) { P("  %s %016llX: (unreadable)", title, (unsigned long long)(uintptr_t)p); return; }
            P("  %s %016llX: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X", title,
              (unsigned long long)(uintptr_t)p, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        }
    }
    //! the file: crash_log-<date>_<hh>-<mm>[-n].log in the exe's folder, else the fallback; `base` gets the name without
    //! its extension (for the minidump)
    HANDLE OpenReport(wchar_t* base, size_t baseLen)
    {
        SYSTEMTIME t; GetLocalTime(&t);
        for (const wchar_t* dir : { (const wchar_t*)g_Dir, (const wchar_t*)g_AltDir })
        {
            if (!dir[0]) continue;
            if (dir == g_AltDir) CreateDirectoryW(g_AltDir, nullptr);
            for (int k = 1; k < 10; k++)
            {
                if (k == 1) _snwprintf_s(base, baseLen, _TRUNCATE, L"%ls\\crash_log-%04u-%02u-%02u_%02u-%02u", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
                else _snwprintf_s(base, baseLen, _TRUNCATE, L"%ls\\crash_log-%04u-%02u-%02u_%02u-%02u-%d", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, k);
                wchar_t path[MAX_PATH + 16]; _snwprintf_s(path, MAX_PATH + 16, _TRUNCATE, L"%ls.log", base);
                HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) return h;
                if (GetLastError() != ERROR_FILE_EXISTS) break;   // not writable here: the next folder
            }
        }
        return INVALID_HANDLE_VALUE;
    }
    void TailLine(const char* s, size_t n, void*) { Put("  ", 2); Put(s, n); Put("\r\n", 2); }

    //! The whole report. On the report's thread (or, without it, on the crashing one).
    void WriteReport(const char* what, EXCEPTION_POINTERS* ep, DWORD tid)
    {
        static wchar_t base[MAX_PATH + 16];
        g_Out = OpenReport(base, MAX_PATH + 16);
        if (g_Out == INVALID_HANDLE_VALUE) return;
        const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
        const CONTEXT* cx = ep ? ep->ContextRecord : nullptr;
        SYSTEMTIME t; GetLocalTime(&t);
        char w[256], w2[256];
        P("Dishonored 2 VR crash report");
        P("==============================================================================================");
        P("time        %04u-%02u-%02u %02u:%02u:%02u (local), %.1f s after the mod loaded", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
          (GetTickCount64() - g_Start) / 1000.0);
        P("what        %s", what);
        P("process     %lu, faulting thread %lu, mod phase '%s'", GetCurrentProcessId(), tid, g_Phase);
        static const char* const kInfoName[CrashLog::kInfoCount] = { "mod", "system", "gpu", "runtime", "headset", "map", "render", "settings" };
        for (int i = 0; i < CrashLog::kInfoCount; i++) if (g_Info[i][0]) P("%-11s %s", kInfoName[i], g_Info[i]);
        P("");
        P("EXCEPTION");
        for (const EXCEPTION_RECORD* r = er; r; )
        {
            WhereBuf(r->ExceptionAddress, w, sizeof w);
            P("  code 0x%08lX %s, flags 0x%lX%s, at %s", r->ExceptionCode, CodeName(r->ExceptionCode), r->ExceptionFlags,
              (r->ExceptionFlags & EXCEPTION_NONCONTINUABLE) ? " (non-continuable)" : "", w);
            if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
                P("  %s address 0x%016llX", r->ExceptionInformation[0] == 0 ? "reading" : r->ExceptionInformation[0] == 1 ? "writing" : "executing",
                  (unsigned long long)r->ExceptionInformation[1]);
            for (DWORD i = 0; i < r->NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
                P("  parameter %lu: 0x%016llX", i, (unsigned long long)r->ExceptionInformation[i]);
            EXCEPTION_RECORD* nx = nullptr;
            if (!SafeCopy(&nx, &r->ExceptionRecord, sizeof nx) || !nx || nx == r) break;
            P("  (nested in:)"); r = nx;
        }
        if (cx)
        {
            P("");
            P("REGISTERS");
            WhereBuf((void*)cx->Rip, w, sizeof w);
            P("  rip %016llX  %s", cx->Rip, w);
            P("  rax %016llX  rbx %016llX  rcx %016llX  rdx %016llX", cx->Rax, cx->Rbx, cx->Rcx, cx->Rdx);
            P("  rsi %016llX  rdi %016llX  rbp %016llX  rsp %016llX", cx->Rsi, cx->Rdi, cx->Rbp, cx->Rsp);
            P("  r8  %016llX  r9  %016llX  r10 %016llX  r11 %016llX", cx->R8, cx->R9, cx->R10, cx->R11);
            P("  r12 %016llX  r13 %016llX  r14 %016llX  r15 %016llX", cx->R12, cx->R13, cx->R14, cx->R15);
            P("  eflags %08lX  mxcsr %08lX", cx->EFlags, cx->MxCsr);
            for (int i = 0; i < 6; i++)
            {
                const float* x = (const float*)&cx->Xmm0 + i * 4;
                P("  xmm%d %g %g %g %g", i, x[0], x[1], x[2], x[3]);
            }
            P("");
            P("CODE AT RIP (32 bytes before, 32 after)");
            Hex("code", (const uint8_t*)cx->Rip - 32, 64);
            P("");
            P("CALL STACK (unwound)");
            CONTEXT c = *cx;
            for (int i = 0; i < 64; i++)
            {
                WhereBuf((void*)c.Rip, w, sizeof w);
                DWORD64 fn = 0; const DWORD64 rip = c.Rip, rsp = c.Rsp;
                const bool more = UnwindStep(&c, &fn);
                if (fn) { WhereBuf((void*)fn, w2, sizeof w2); P("  #%-2d %-48s function %s +0x%llX  (rsp %016llX)", i, w, w2, (unsigned long long)(rip - fn), rsp); }
                else P("  #%-2d %-48s (no unwind data)  (rsp %016llX)", i, w, rsp);
                if (!more || c.Rsp <= rsp) break;
            }
            P("");
            P("STACK SCAN (values on the stack that point into a module: callers the unwind may have missed)");
            {
                char line[1024]; size_t len = 0; int found = 0;
                for (int i = 0; i < 1024 && found < 48; i++)
                {
                    uintptr_t v = 0;
                    if (!ReadPtr((const uintptr_t*)cx->Rsp + i, v)) break;
                    if (!InModule(v)) continue;
                    WhereBuf((void*)v, w, sizeof w);
                    len += _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " [rsp+%X] %s", i * 8, w);
                    if (++found % 4 == 0) { P("  %s", line); len = 0; line[0] = 0; }
                }
                if (len) P("  %s", line);
            }
            P("");
            P("STACK (512 bytes at rsp)");
            Hex("stack", (const void*)cx->Rsp, 512);
            P("");
            P("MEMORY AT THE REGISTERS (64 bytes where they point to readable memory)");
            const struct { const char* n; DWORD64 v; } regs[] = { { "rax", cx->Rax }, { "rbx", cx->Rbx }, { "rcx", cx->Rcx }, { "rdx", cx->Rdx },
                { "rsi", cx->Rsi }, { "rdi", cx->Rdi }, { "rbp", cx->Rbp }, { "r8", cx->R8 }, { "r9", cx->R9 }, { "r10", cx->R10 },
                { "r11", cx->R11 }, { "r12", cx->R12 }, { "r13", cx->R13 }, { "r14", cx->R14 }, { "r15", cx->R15 } };
            for (const auto& r : regs)
            {
                uint8_t probe;
                if (r.v < 0x10000 || !SafeCopy(&probe, (const void*)r.v, 1)) continue;
                WhereBuf((void*)r.v, w, sizeof w);
                P("  %s -> %s", r.n, w);
                Hex(r.n, (const void*)r.v, 64);
            }
        }
        P("");
        P("MODULES (base, size, link timestamp, path)");
        {
            static HMODULE mods[1024]; DWORD need = 0;
            if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need))
                for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; i++)
                {
                    MODULEINFO mi{}; K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi);
                    wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(mods[i], p, MAX_PATH);
                    DWORD stamp = 0; LONG lfa = 0;
                    if (SafeCopy(&lfa, (const uint8_t*)mods[i] + 0x3C, 4)) SafeCopy(&stamp, (const uint8_t*)mods[i] + lfa + 8, 4);
                    P("  %016llX %08lX %08lX %ls", (unsigned long long)(uintptr_t)mods[i], mi.SizeOfImage, stamp, p);
                }
        }
        P("");
        P("THE LOG'S LAST LINES (DishonoredVR.log; none with WriteLog = 0)");
        Log::CrashTail(&TailLine, nullptr);
        FlushFileBuffers(g_Out);
        // the minidump last: dbghelp may allocate, and the text above is already on disk
        if (g_Dump)
        {
            wchar_t dp[MAX_PATH + 16]; _snwprintf_s(dp, MAX_PATH + 16, _TRUNCATE, L"%ls.dmp", base);
            HANDLE d = CreateFileW(dp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (d != INVALID_HANDLE_VALUE)
            {
                MINIDUMP_EXCEPTION_INFORMATION mei{ tid, ep, FALSE };   // (a 4-byte packed struct: dbghelp.h's own type)
                // every thread's stack, what the stacks point at, the unloaded modules and the threads' times; not the
                // data segments (the game's are large): a few MB
                const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules | MiniDumpWithThreadInfo);
                const BOOL ok = g_Dump(GetCurrentProcess(), GetCurrentProcessId(), d, type, ep ? &mei : nullptr, nullptr, nullptr);
                const DWORD err = ok ? 0 : GetLastError();
                CloseHandle(d);
                P("");
                P("minidump: %ls.dmp %s (error %lu)", wcsrchr(base, L'\\') ? wcsrchr(base, L'\\') + 1 : base, ok ? "written" : "FAILED", err);
                if (!ok) DeleteFileW(dp);
            }
        }
        else { P(""); P("minidump: none (dbghelp.dll not loaded)"); }
        FlushFileBuffers(g_Out);
        CloseHandle(g_Out);
        g_Out = INVALID_HANDLE_VALUE;
    }

    DWORD WINAPI WriterThread(LPVOID)
    {
        g_WriterReady = true;
        WaitForSingleObject(g_Go, INFINITE);
        WriteReport(g_Req.what, g_Req.ep, g_Req.tid);
        SetEvent(g_Done);
        return 0;
    }
    //! A crash: the report, once. Hands it to the report's thread (its own stack) and waits for it.
    void Crash(const char* what, EXCEPTION_POINTERS* ep)
    {
        if (g_Claimed.exchange(true)) return;
        g_Req = { ep, GetCurrentThreadId(), what };
        if (g_WriterReady.load() && GetCurrentThreadId() != g_WriterId)
        {
            SetEvent(g_Go);
            WaitForSingleObject(g_Done, 60000);
        }
        else WriteReport(what, ep, GetCurrentThreadId());
    }

    // ---- the DishonoredVR.log hint (heap: not for the fatal codes) ----
    void LogHint(const char* what, EXCEPTION_POINTERS* ep)
    {
        const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
        if (!er) return;
        std::string detail;
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            detail = fmt::format(" {} 0x{:016X}", er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 1 ? "writing" : "executing",
                                 (uintptr_t)er->ExceptionInformation[1]);
        LOGI("fault [{}] {} (0x{:08X}){} at {} - phase '{}' thread {}", what, CodeName(er->ExceptionCode), (unsigned)er->ExceptionCode, detail,
             CrashLog::Where(er->ExceptionAddress), g_Phase, GetCurrentThreadId());
    }

    LONG CALLBACK Vectored(EXCEPTION_POINTERS* ep)
    {
        const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
        if (!er) return EXCEPTION_CONTINUE_SEARCH;
        if (Fatal(er)) { Crash("fatal exception (seen first chance: it ends the process whoever handles it)", ep); return EXCEPTION_CONTINUE_SEARCH; }
        // a fault the game (or the mod's own guarded reads) may well handle: a hint in the log, the first few only
        if (Interesting(er->ExceptionCode) && Log::Enabled() && g_Logged.load() < kMaxLogged && !g_InHandler.exchange(true))
        {
            g_Logged++;
            LogHint("first chance, may be handled", ep);
            g_InHandler = false;
        }
        return EXCEPTION_CONTINUE_SEARCH;      // never swallow: the game's own __except may be the right handler
    }

    LONG WINAPI Unhandled(EXCEPTION_POINTERS* ep)
    {
        Crash("unhandled exception", ep);
        if (!g_InHandler.exchange(true))
        {   // and the old one-line summary in DishonoredVR.log
            const DWORD c = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
            if (c != 0xC0000374 && c != EXCEPTION_STACK_OVERFLOW) { Log::ForceFile(); LogHint("fatal: see the crash_log file", ep); }
            g_InHandler = false;
        }
        if (g_Next && g_Next != Unhandled) return g_Next(ep);   // the game's own filter, after ours
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // The game (or a library) setting its own filter would replace ours: it is kept as the one ours calls next.
    LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI* g_RealSetFilter)(LPTOP_LEVEL_EXCEPTION_FILTER) = SetUnhandledExceptionFilter;
    LPTOP_LEVEL_EXCEPTION_FILTER WINAPI HookSetFilter(LPTOP_LEVEL_EXCEPTION_FILTER f)
    {
        if (f == Unhandled) return g_RealSetFilter(f);
        LPTOP_LEVEL_EXCEPTION_FILTER prev = g_Next;
        g_Next = f;
        return prev;
    }

    // ---- the mod's own CRT (it links the CRT statically: these are the mod's, not the game's) ----
    //! a report from here (no exception): the context of this call
    void SelfCrash(DWORD code)
    {
        static CONTEXT ctx; static EXCEPTION_RECORD rec;
        RtlCaptureContext(&ctx);
        memset(&rec, 0, sizeof rec);
        rec.ExceptionCode = code; rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE; rec.ExceptionAddress = (void*)ctx.Rip;
        EXCEPTION_POINTERS ep{ &rec, &ctx };
        Crash(CodeName(code), &ep);
    }
    void OnAbort(int) { SelfCrash(0xE0DE0001); }                       // then abort goes on
    void OnTerminate() { SelfCrash(0xE0DE0002); abort(); }
    void OnPurecall() { SelfCrash(0xE0DE0003); TerminateProcess(GetCurrentProcess(), 0xE0DE0003); }
    void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { SelfCrash(0xE0DE0004); TerminateProcess(GetCurrentProcess(), 0xE0DE0004); }

    //! the newest crash_log-*.log in the two folders: its full path
    std::wstring Latest()
    {
        std::wstring best; FILETIME bestT{};
        for (const wchar_t* dir : { (const wchar_t*)g_Dir, (const wchar_t*)g_AltDir })
        {
            if (!dir[0]) continue;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((std::wstring(dir) + L"\\crash_log-*.log").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do
                if (CompareFileTime(&fd.ftLastWriteTime, &bestT) > 0) { bestT = fd.ftLastWriteTime; best = std::wstring(dir) + L"\\" + fd.cFileName; }
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        return best;
    }
    std::string Narrow(const std::wstring& s)
    {
        std::string o; for (wchar_t c : s) o += (c < 128 ? (char)c : '?'); return o;
    }
}

void CrashLog::Install()
{
    g_Start = GetTickCount64();
    GetModuleFileNameW(nullptr, g_Dir, MAX_PATH);
    if (wchar_t* s = wcsrchr(g_Dir, L'\\')) *s = 0;
    wchar_t la[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) _snwprintf_s(g_AltDir, MAX_PATH, _TRUNCATE, L"%ls\\DishonoredVR", la);
    g_Go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_Done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // its own stack, large enough for the unwinder and dbghelp; asleep until a crash
    if (g_Go && g_Done) g_Writer = CreateThread(nullptr, 256 * 1024, WriterThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &g_WriterId);
    AddVectoredExceptionHandler(1, Vectored);
    g_Next = SetUnhandledExceptionFilter(Unhandled);   // one set before ours (if any) is called after it
    signal(SIGABRT, OnAbort);
    set_terminate(OnTerminate);
    _set_purecall_handler(OnPurecall);
    _set_invalid_parameter_handler(OnInvalidParameter);
    LOGI("CrashLog: installed (a crash writes crash_log-<date>_<time>.log and a minidump next to the game's exe)");
}

void CrashLog::Prepare()
{
    // dbghelp from System32 (not the game folder's, if it has one)
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    if (HMODULE h = LoadLibraryW((std::wstring(sys) + L"\\dbghelp.dll").c_str())) g_Dump = (DumpFn)GetProcAddress(h, "MiniDumpWriteDump");
    // keep our filter first: whoever sets one later is called after it
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_RealSetFilter, HookSetFilter);
    const LONG r = DetourTransactionCommit();
    LOGI("CrashLog: minidumps {}, the unhandled-exception filter kept first: detour={}", g_Dump ? "on" : "OFF (no dbghelp)", r);
}

void CrashLog::SetPhase(const char* phase) { g_Phase = phase ? phase : "?"; }

std::string CrashLog::Where(const void* addr)
{
    char b[256]; WhereBuf(addr, b, sizeof b); return b;
}

void CrashLog::SetInfo(Info what, const std::string& text)
{
    if (what < 0 || what >= kInfoCount) return;
    char* d = g_Info[what];
    const size_t n = std::min(text.size(), sizeof g_Info[0] - 1);
    d[0] = 0;   // a crash meanwhile reads an empty line rather than a torn one
    if (n > 1) memcpy(d + 1, text.data() + 1, n - 1);
    d[n] = 0;
    if (n) d[0] = text[0];
}

std::string CrashLog::LatestReport()
{
    const std::wstring p = Latest();
    const size_t s = p.find_last_of(L'\\');
    return Narrow(s == std::wstring::npos ? p : p.substr(s + 1));
}

bool CrashLog::SaveLatestToDesktop(std::string& result)
{
    const std::wstring src = Latest();
    if (src.empty()) { result = "no crash log to save"; return false; }
    PWSTR desk = nullptr;
    static const GUID kDesktop = { 0xB4BFCC3A, 0xDB2C, 0x424C, { 0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41 } };   // FOLDERID_Desktop
    if (FAILED(SHGetKnownFolderPath(kDesktop, 0, nullptr, &desk)) || !desk) { result = "the desktop folder was not found"; return false; }
    const std::wstring dir = desk; CoTaskMemFree(desk);
    const std::wstring name = src.substr(src.find_last_of(L'\\') + 1);
    bool ok = CopyFileW(src.c_str(), (dir + L"\\" + name).c_str(), FALSE) != 0;
    const DWORD err = ok ? 0 : GetLastError();
    bool dump = false;
    {   // its minidump, when there is one
        const std::wstring dsrc = src.substr(0, src.size() - 4) + L".dmp";
        if (GetFileAttributesW(dsrc.c_str()) != INVALID_FILE_ATTRIBUTES)
            dump = CopyFileW(dsrc.c_str(), (dir + L"\\" + name.substr(0, name.size() - 4) + L".dmp").c_str(), FALSE) != 0;
    }
    result = ok ? "saved " + Narrow(name) + (dump ? " and its minidump" : "") + " to the desktop" : fmt::format("could not copy {} to the desktop (error {})", Narrow(name), err);
    LOGI("CrashLog: {}", result);
    return ok;
}
