#include "pch.h"
#include "CrashLog.h"

namespace
{
    std::atomic<int> g_Logged{ 0 };
    std::atomic<bool> g_InHandler{ false };
    constexpr int kMaxLogged = 6;
    const char* g_Phase = "init";

    //! "<module>+0x<rva>" for an address, or "0x<abs>" when it belongs to no module. Used instead of symbols: the
    //! game has none, and the mod's RVA can be resolved against DishonoredVR.pdb after the fact.
    std::string Where(const void* addr)
    {
        HMODULE m = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)addr, &m) && m)
        {
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(m, path, MAX_PATH);
            const wchar_t* leaf = wcsrchr(path, L'\\');
            leaf = leaf ? leaf + 1 : path;
            std::string name;
            for (; *leaf; leaf++) name += (char)(*leaf < 128 ? *leaf : '?');   // module names are ASCII
            return fmt::format("{}+0x{:X}", name, (uintptr_t)addr - (uintptr_t)m);
        }
        return fmt::format("0x{:016X}", (uintptr_t)addr);
    }

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
        case 0xC0000409:                      return "STACK_BUFFER_OVERRUN/fastfail";
        case 0xC0000374:                      return "HEAP_CORRUPTION";
        default:                              return "?";
        }
    }

    //! Only the codes that mean something is actually broken. The engine raises C++ exceptions (0xE06D7363) and
    //! the debugger thread-name exception in normal operation; logging those would bury the real fault.
    bool Interesting(DWORD c)
    {
        switch (c)
        {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_IN_PAGE_ERROR:
        case 0xC0000409:
        case 0xC0000374:
            return true;
        default:
            return false;
        }
    }

    //! One qword off a possibly-dead stack. Its own function because MSVC refuses __try in anything that has to
    //! unwind C++ objects, and the caller builds strings.
    bool ReadPtr(const uintptr_t* p, uintptr_t& out)
    {
        __try { out = *p; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void Report(const char* what, EXCEPTION_POINTERS* ep)
    {
        const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
        const CONTEXT* cx = ep ? ep->ContextRecord : nullptr;
        if (!er) return;
        std::string detail;
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        {
            const ULONG_PTR kind = er->ExceptionInformation[0];
            detail = fmt::format(" {} 0x{:016X}", kind == 0 ? "reading" : kind == 1 ? "writing" : "executing",
                                 (uintptr_t)er->ExceptionInformation[1]);
        }
        Log::ForceFile();   // before the ini was read (WriteLog decides only then): a crash is always written
        LOGI("CRASH [{}] {} (0x{:08X}){} at {} - phase '{}' thread {}",
             what, CodeName(er->ExceptionCode), (unsigned)er->ExceptionCode, detail,
             Where(er->ExceptionAddress), g_Phase, GetCurrentThreadId());
        if (cx)
            LOGI("CRASH   rip={} rsp=0x{:016X} rbp=0x{:016X} rcx=0x{:016X} rdx=0x{:016X} r8=0x{:016X} r9=0x{:016X}",
                 Where((void*)cx->Rip), (uintptr_t)cx->Rsp, (uintptr_t)cx->Rbp,
                 (uintptr_t)cx->Rcx, (uintptr_t)cx->Rdx, (uintptr_t)cx->R8, (uintptr_t)cx->R9);
        // The handler runs on the faulting stack, so its own frames are on top and the faulting ones below.
        void* frames[40] = {};
        USHORT n = RtlCaptureStackBackTrace(0, 40, frames, nullptr);
        for (USHORT i = 0; i < n; i++) LOGI("CRASH   #{:<2} {}", i, Where(frames[i]));
        // x64 release code has no frame pointers, so the capture above often stops early. Scanning the stack for
        // values that point into a loaded module recovers the callers it missed -- noisy, but it names the module.
        if (cx)
        {
            std::string scan;
            int found = 0;
            const uintptr_t* sp = (const uintptr_t*)cx->Rsp;
            for (int i = 0; i < 512 && found < 24; i++)
            {
                uintptr_t v = 0;
                if (!ReadPtr(sp + i, v)) break;
                if (v < 0x10000) continue;
                HMODULE m = nullptr;
                if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        (LPCWSTR)v, &m) || !m) continue;
                scan += " " + Where((void*)v);
                if (++found % 6 == 0) { LOGI("CRASH   stack:{}", scan); scan.clear(); }
            }
            if (!scan.empty()) LOGI("CRASH   stack:{}", scan);
        }
    }

    LONG CALLBACK Vectored(EXCEPTION_POINTERS* ep)
    {
        if (ep && ep->ExceptionRecord && Interesting(ep->ExceptionRecord->ExceptionCode)
            && g_Logged.load() < kMaxLogged && !g_InHandler.exchange(true))
        {
            g_Logged++;
            Report("first chance", ep);
            g_InHandler = false;
        }
        return EXCEPTION_CONTINUE_SEARCH;      // never swallow: the game's own __except may be the right handler
    }

    LONG WINAPI Unhandled(EXCEPTION_POINTERS* ep)
    {
        if (!g_InHandler.exchange(true)) { Report("fatal", ep); g_InHandler = false; }
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

void CrashLog::Install()
{
    AddVectoredExceptionHandler(1, Vectored);
    SetUnhandledExceptionFilter(Unhandled);
    LOGI("CrashLog: installed (first {} faults are logged with a backtrace)", kMaxLogged);
}

void CrashLog::SetPhase(const char* phase) { g_Phase = phase ? phase : "?"; }
std::string CrashLog::Where(const void* addr) { return ::Where(addr); }
