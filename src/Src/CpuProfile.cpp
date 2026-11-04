#include "pch.h"
#include "CpuProfile.h"
#include "Watchdog.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <intrin.h>
#include <algorithm>
#include <map>
#include <unordered_map>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace
{
    std::atomic<bool> g_On{ false }, g_Started{ false };
    constexpr int kTaskTids = 64;
    std::atomic<DWORD> g_TaskTid[kTaskTids]{};
    DWORD g_SelfTid = 0;

    constexpr double kWindowS = 5.0;          // one report per window
    constexpr int kPeriodUs = 2000;           // a sample of each hot thread every 2 ms
    constexpr int kMaxHot = 6;
    constexpr double kHotCores = 0.15;

    // ---- the process's modules (the sampler thread only) ----
    struct Module { uintptr_t base = 0, end = 0; std::string name; };
    std::vector<Module> g_Mods;
    uintptr_t g_ExeBase = 0, g_ExeEnd = 0, g_SelfBase = 0, g_SelfEnd = 0;

    using EnumModsFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
    using ModInfoFn = BOOL(WINAPI*)(HANDLE, HMODULE, LPMODULEINFO, DWORD);
    using ModNameFn = DWORD(WINAPI*)(HANDLE, HMODULE, LPWSTR, DWORD);
    using ThreadDescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    using NtQitFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

    std::string Narrow(const wchar_t* w)
    {
        std::string s;
        for (; w && *w; w++) s += (char)(*w < 128 ? *w : '?');
        return s;
    }

    void LoadModules()
    {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        static auto en = (EnumModsFn)GetProcAddress(k32, "K32EnumProcessModules");
        static auto inf = (ModInfoFn)GetProcAddress(k32, "K32GetModuleInformation");
        static auto nm = (ModNameFn)GetProcAddress(k32, "K32GetModuleBaseNameW");
        g_Mods.clear();
        if (!en || !inf || !nm) return;
        static HMODULE mods[2048];
        DWORD need = 0;
        if (!en(GetCurrentProcess(), mods, sizeof mods, &need)) return;
        const DWORD n = std::min<DWORD>(need / sizeof(HMODULE), 2048);
        for (DWORD i = 0; i < n; i++)
        {
            MODULEINFO mi{};
            if (!inf(GetCurrentProcess(), mods[i], &mi, sizeof mi)) continue;
            wchar_t w[MAX_PATH] = {};
            nm(GetCurrentProcess(), mods[i], w, MAX_PATH);
            g_Mods.push_back({ (uintptr_t)mi.lpBaseOfDll, (uintptr_t)mi.lpBaseOfDll + mi.SizeOfImage, Narrow(w) });
        }
        std::sort(g_Mods.begin(), g_Mods.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
        const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&LoadModules, &self);
        for (const Module& m : g_Mods)
        {
            if (m.base == exe) { g_ExeBase = m.base; g_ExeEnd = m.end; }
            if (m.base == (uintptr_t)self) { g_SelfBase = m.base; g_SelfEnd = m.end; }
        }
    }
    const Module* ModOf(uintptr_t a)
    {
        auto it = std::upper_bound(g_Mods.begin(), g_Mods.end(), a, [](uintptr_t v, const Module& m) { return v < m.base; });
        if (it == g_Mods.begin()) return nullptr;
        --it;
        return a < it->end ? &*it : nullptr;
    }
    bool InExe(uintptr_t a) { return a >= g_ExeBase && a < g_ExeEnd; }
    bool InSelf(uintptr_t a) { return a >= g_SelfBase && a < g_SelfEnd; }

    // ---- code reads that cannot fault (POD helpers) ----
    bool ReadCode(const void* p, void* out, size_t n)
    {
        __try { memcpy(out, p, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    //! a value a CALL pushed: the bytes before it are a call instruction (as Watchdog's stack scan)
    bool AfterCall(uintptr_t v)
    {
        uint8_t b[7];
        if (v < 0x10000 || !ReadCode((const void*)(v - 7), b, 7)) return false;
        if (b[2] == 0xE8) return true;                                           // call rel32
        if (b[1] == 0xFF && ((b[2] >> 3) & 7) == 2) return true;                // call [rip+disp32] / [reg+disp32]
        if (b[4] == 0xFF && ((b[5] >> 3) & 7) == 2) return true;                // call [reg+disp8]
        if (b[5] == 0xFF && ((b[6] >> 3) & 7) == 2) return true;                // call reg / [reg]
        if (b[4] == 0x41 && b[5] == 0xFF) return true;                           // call r8-r15 (REX.B)
        return false;
    }
    //! the thread stopped right after a SYSCALL instruction: waiting, or in the kernel
    bool InSyscall(uintptr_t rip)
    {
        uint8_t b[2];
        return rip > 0x10000 && ReadCode((const void*)(rip - 2), b, 2) && b[0] == 0x0F && b[1] == 0x05;
    }
    //! the start of the function holding `a` (its unwind table entry, a chained one followed to its primary), 0 = none
    //! (a leaf function, or code outside any module's table)
    uintptr_t FuncStart(uintptr_t a)
    {
        __try
        {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry((DWORD64)a, &base, nullptr);
            if (!f) return 0;
            for (int i = 0; i < 8; i++)
            {
                const uint8_t* ui = (const uint8_t*)(base + f->UnwindData);
                const uint8_t flags = ui[0] >> 3, codes = ui[2];
                if (!(flags & 4)) break;                                        // UNW_FLAG_CHAININFO
                f = (PRUNTIME_FUNCTION)(ui + 4 + (size_t)((codes + 1) & ~1) * 2);
            }
            return (uintptr_t)base + f->BeginAddress;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // ---- the threads ----
    struct Th
    {
        HANDLE h = nullptr;
        DWORD tid = 0;
        uintptr_t start = 0;
        std::string desc;
        ULONG64 cycles = 0, created = 0;
        bool seen = false, hot = false;
        double cores = 0;                                 // the last window
        unsigned samples = 0, sys = 0, failed = 0;
        std::unordered_map<uintptr_t, unsigned> self;     // function start (bit 0 set: no unwind entry, the 64-byte block) -> samples
        std::unordered_map<uintptr_t, unsigned> via;      // the samples outside the game and the mod, by their nearest caller there
        std::unordered_map<uintptr_t, unsigned> incl;     // the game's and the mod's functions on the stack (inclusive time)
        std::unordered_map<std::string, unsigned> waits;  // the samples in a system call, by where the game / mod waited
        unsigned unwound = 0;                             // samples whose stack was walked to a game / mod frame
    };
    std::map<DWORD, Th> g_Th;

    std::string Name(uintptr_t key)
    {
        if (!key) return "(none on the stack)";
        const bool leaf = key & 1;
        const uintptr_t a = key & ~(uintptr_t)1;
        const Module* m = ModOf(a);
        std::string s = m ? fmt::format("{}+0x{:X}", m->name, a - m->base) : fmt::format("0x{:X}", a);
        if (m && m->base == g_ExeBase) s = fmt::format("exe+0x{:X}", a - m->base);
        return leaf ? "~" + s : s;
    }
    bool IsTaskThread(DWORD tid)
    {
        for (auto& t : g_TaskTid) if (t.load(std::memory_order_relaxed) == tid) return true;
        return false;
    }
    std::string Label(const Th& t, DWORD firstTid)
    {
        std::string l;
        auto add = [&](const std::string& s) { l += (l.empty() ? "" : ", ") + s; };
        if (t.tid == Watchdog::RenderThreadId()) add("render thread (Present)");
        if (t.tid == Watchdog::FrameThreadId()) add("the mod's OpenXR frame thread");
        if (IsTaskThread(t.tid)) add("records render tasks");
        if (t.tid == firstTid) add("the process's first thread");
        if (t.tid == g_SelfTid) add("this profiler");
        if (!t.desc.empty()) add("'" + t.desc + "'");
        add("start " + Name(t.start));
        return l;
    }

    void Refresh(double dtS, double tscPerS)
    {
        static auto desc = (ThreadDescFn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
        static auto nqit = (NtQitFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
        for (auto& kv : g_Th) kv.second.seen = false;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE)
        {
            THREADENTRY32 te{}; te.dwSize = sizeof te;
            const DWORD pid = GetCurrentProcessId();
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            {
                if (te.th32OwnerProcessID != pid) continue;
                Th& t = g_Th[te.th32ThreadID];
                t.seen = true;
                if (!t.h)
                {
                    t.tid = te.th32ThreadID;
                    t.h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, t.tid);
                    if (!t.h) continue;
                    if (nqit) { PVOID s = nullptr; if (nqit(t.h, 9 /*ThreadQuerySetWin32StartAddress*/, &s, sizeof s, nullptr) == 0) t.start = (uintptr_t)s; }
                    if (desc) { PWSTR w = nullptr; if (SUCCEEDED(desc(t.h, &w)) && w) { t.desc = Narrow(w); LocalFree(w); } }
                    FILETIME c{}, e{}, k{}, u{};
                    if (GetThreadTimes(t.h, &c, &e, &k, &u)) t.created = ((ULONG64)c.dwHighDateTime << 32) | c.dwLowDateTime;
                    QueryThreadCycleTime(t.h, &t.cycles);
                    t.cores = 0;
                    continue;
                }
                ULONG64 cyc = 0;
                if (QueryThreadCycleTime(t.h, &cyc))
                {
                    t.cores = dtS > 0 && tscPerS > 0 ? (double)(cyc - t.cycles) / (tscPerS * dtS) : 0;
                    t.cycles = cyc;
                }
            }
            CloseHandle(snap);
        }
        for (auto it = g_Th.begin(); it != g_Th.end();)
            if (!it->second.seen) { if (it->second.h) CloseHandle(it->second.h); it = g_Th.erase(it); }
            else ++it;
    }

    //! The return addresses of the sampled thread, walked with the unwind tables from its context over the COPY of its
    //! stack (the thread runs on meanwhile): the stack pointer, and the frame pointer when it points into the stack, are
    //! moved onto the copy, and the walk stops where the copy ends. (The first version scanned the stack for anything
    //! that looked like a return address: stale ones left in unused slots -- operator new's, above all -- were taken
    //! for callers.)
    int Unwind(const CONTEXT& cx0, const uint8_t* copy, SIZE_T got, uintptr_t* out, int max)
    {
        int n = 0;
        __try
        {
            CONTEXT c = cx0;
            const DWORD64 lo = cx0.Rsp, hi = cx0.Rsp + got, delta = (DWORD64)copy - lo;
            const DWORD64 clo = (DWORD64)copy, chi = (DWORD64)copy + got;
            c.Rsp += delta;
            if (c.Rbp >= lo && c.Rbp < hi) c.Rbp += delta;
            for (int i = 0; i < 64 && n < max; i++)
            {
                const DWORD64 pc = c.Rip;
                DWORD64 base = 0;
                PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(pc, &base, nullptr);
                if (!f)
                {   // a leaf: its return address is where the stack pointer is
                    if (c.Rsp < clo || c.Rsp + 8 > chi) break;
                    c.Rip = *(const DWORD64*)c.Rsp; c.Rsp += 8;
                }
                else
                {
                    PVOID hd = nullptr; DWORD64 est = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, f, &c, &hd, &est, nullptr);
                }
                if (c.Rip < 0x10000 || c.Rsp < clo || c.Rsp >= chi) break;
                if (c.Rbp >= lo && c.Rbp < hi) c.Rbp += delta;   // a frame pointer restored from the stack: the real one
                out[n++] = (uintptr_t)c.Rip;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return n;
    }
    void Sample(Th& t)
    {
        alignas(16) CONTEXT cx{};
        cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        static uint8_t stk[16384];                          // the top 16 KB of its stack (this thread only)
        SIZE_T got = 0;
        // suspended for GetThreadContext and the stack copy only: no lock, no allocation, no log until it runs again
        if (SuspendThread(t.h) == (DWORD)-1) { t.failed++; return; }
        const bool ok = GetThreadContext(t.h, &cx) != 0;
        if (ok)
        {   // page by page from rsp up: the first unreadable page is past the stack's base
            SIZE_T off = 0;
            while (off < sizeof stk)
            {
                const SIZE_T want = off ? std::min<SIZE_T>(4096, sizeof stk - off) : std::min<SIZE_T>(4096 - (cx.Rsp & 0xFFF), sizeof stk);
                SIZE_T n = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (const void*)(cx.Rsp + off), stk + off, want, &n) || n != want) break;
                off += want;
            }
            got = off;
        }
        ResumeThread(t.h);
        if (!ok) { t.failed++; return; }
        t.samples++;
        const uintptr_t rip = (uintptr_t)cx.Rip;
        const bool sys = InSyscall(rip);
        if (sys) t.sys++;
        const uintptr_t fn = FuncStart(rip);
        t.self[fn ? fn : ((rip & ~(uintptr_t)63) | 1)]++;
        uintptr_t ret[48];
        const int nr = Unwind(cx, stk, got, ret, 48);
        // inclusive: every game / mod function on the stack, once
        uintptr_t seen[48]; int ns = 0;
        uintptr_t own[2] = {}; int no = 0;
        std::string between;                                  // the modules between the stop and the first game / mod frame
        if (InExe(rip) || InSelf(rip)) { seen[ns++] = fn ? fn : rip; own[no++] = fn ? fn : (rip | 1); }
        for (int i = 0; i < nr; i++)
        {
            const uintptr_t a = ret[i];
            if (InExe(a) || InSelf(a))
            {
                const uintptr_t f = FuncStart(a); const uintptr_t k = f ? f : (a | 1);
                bool dup = false; for (int j = 0; j < ns; j++) if (seen[j] == k) dup = true;
                if (!dup && ns < 48) seen[ns++] = k;
                if (no < 2) own[no++] = k;
            }
            else if (!no)
            {
                const Module* m = ModOf(a);
                if (m && m->name != "ntdll.dll" && m->name != "KERNELBASE.dll" && m->name != "KERNEL32.DLL" && between.find(m->name) == std::string::npos)
                    between += (between.empty() ? "" : " < ") + m->name;
            }
        }
        for (int j = 0; j < ns; j++) t.incl[seen[j]]++;
        if (no) t.unwound++;
        if (!(InExe(rip) || InSelf(rip))) t.via[no ? own[0] : 0]++;
        if (sys)
            t.waits[(no ? Name(own[0]) : std::string("?")) + (no > 1 ? " < " + Name(own[1]) : std::string()) + (between.empty() ? "" : " [via " + between + "]")]++;
    }

    template <class M> std::string Top(const M& m, unsigned total, size_t n)
    {
        std::vector<std::pair<unsigned, uintptr_t>> v;
        for (auto& kv : m) v.push_back({ kv.second, kv.first });
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::string s;
        for (size_t i = 0; i < v.size() && i < n; i++)
            s += fmt::format("{}{} {:.1f}%", i ? ", " : "", Name(v[i].second), 100.0 * v[i].first / std::max(1u, total));
        return s.empty() ? std::string("-") : s;
    }

    void Report(double dtS)
    {
        DWORD firstTid = 0; ULONG64 firstAt = ~0ull;
        for (auto& kv : g_Th) if (kv.second.created && kv.second.created < firstAt) { firstAt = kv.second.created; firstTid = kv.first; }
        std::vector<Th*> v;
        double total = 0;
        for (auto& kv : g_Th) { v.push_back(&kv.second); total += kv.second.cores; }
        std::sort(v.begin(), v.end(), [](const Th* a, const Th* b) { return a->cores > b->cores; });
        std::string list;
        int shown = 0; double rest = 0; int restN = 0;
        for (const Th* t : v)
        {
            if (shown < 14 && t->cores >= 0.02) { list += fmt::format("{}tid {} {:.2f} ({})", shown ? " | " : "", t->tid, t->cores, Label(*t, firstTid)); shown++; }
            else { rest += t->cores; restN++; }
        }
        LOGI("CpuProfile: {:.1f} s: CPU time by thread, in cores (1.00 = one core busy all the time): {}{}; all {} threads {:.2f} cores",
             dtS, list, restN ? fmt::format(" | {} others {:.2f}", restN, rest) : std::string(), g_Th.size(), total);
        for (const Th* t : v)
        {
            if (!t->hot || !t->samples) continue;
            std::unordered_map<uintptr_t, unsigned> mods;
            unsigned inExe = 0, inSelf = 0;
            for (auto& kv : t->self)
            {
                const uintptr_t a = kv.first & ~(uintptr_t)1;
                const Module* m = ModOf(a);
                mods[m ? m->base : 0] += kv.second;
                if (InExe(a)) inExe += kv.second; else if (InSelf(a)) inSelf += kv.second;
            }
            std::vector<std::pair<unsigned, uintptr_t>> mv;
            for (auto& kv : mods) mv.push_back({ kv.second, kv.first });
            std::sort(mv.begin(), mv.end(), [](auto& a, auto& b) { return a.first > b.first; });
            std::string ms;
            for (size_t i = 0; i < mv.size() && i < 8; i++)
            {
                const Module* m = ModOf(mv[i].second);
                ms += fmt::format("{}{} {:.1f}%", i ? ", " : "", m ? (m->base == g_ExeBase ? std::string("the game (exe)") : m->name) : std::string("(no module)"),
                                  100.0 * mv[i].first / t->samples);
            }
            const unsigned outside = t->samples - inExe - inSelf;
            LOGI("CpuProfile: tid {} ({:.2f} cores; {}): {} samples, {:.0f}% stopped in a system call (waiting, or in the kernel); by module: {}",
                 t->tid, t->cores, Label(*t, firstTid), t->samples, 100.0 * t->sys / t->samples, ms);
            LOGI("CpuProfile:   tid {} hottest functions (self time; ~ = no unwind entry, the 64-byte block): {}", t->tid, Top(t->self, t->samples, 16));
            if (outside)
                LOGI("CpuProfile:   tid {} its {:.1f}% outside the game and the mod, by the nearest caller in them: {}", t->tid,
                     100.0 * outside / t->samples, Top(t->via, outside, 10));
            LOGI("CpuProfile:   tid {} on the stack (inclusive: the function or what it called; {:.0f}% of the samples walked to the game or the mod): {}", t->tid,
                 100.0 * t->unwound / t->samples, Top(t->incl, t->samples, 20));
            if (t->sys)
            {
                std::vector<std::pair<unsigned, const std::string*>> w;
                for (auto& kv : t->waits) w.push_back({ kv.second, &kv.first });
                std::sort(w.begin(), w.end(), [](auto& a, auto& b) { return a.first > b.first; });
                std::string s;
                for (size_t i = 0; i < w.size() && i < 10; i++) s += fmt::format("{}{} {:.1f}%", i ? " | " : "", *w[i].second, 100.0 * w[i].first / t->samples);
                LOGI("CpuProfile:   tid {} stopped in a system call ({:.0f}% of the samples), by where the game / the mod called (innermost < its caller [modules between]): {}",
                     t->tid, 100.0 * t->sys / t->samples, s);
            }
        }
    }

    DWORD WINAPI Thread(LPVOID)
    {
        g_SelfTid = GetCurrentThreadId();
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        const bool hires = timer != nullptr;
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
        LARGE_INTEGER w0{}; unsigned long long tsc0 = 0;
        bool running = false, told = false;
        for (;;)
        {
            if (!g_On.load())
            {
                running = false;
                Sleep(250);
                continue;
            }
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            if (!running)
            {
                LoadModules();
                Refresh(0, 0);
                for (auto& kv : g_Th) kv.second.hot = false;
                w0 = now; tsc0 = __rdtsc(); running = true;
                if (!told)
                {
                    told = true;
                    LOGI("CpuProfile: the game's threads' CPU time every {:.0f} s, and the busiest {} (at least {:.2f} of a core) sampled every {} ms ({} timer){}",
                         kWindowS, kMaxHot, kHotCores, kPeriodUs / 1000.0, hires ? "high-resolution" : "standard: about every 15 ms",
                         timer ? "" : " -- no timer: Sleep");
                }
            }
            if (timer)
            {
                LARGE_INTEGER due; due.QuadPart = -(LONGLONG)kPeriodUs * 10;
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 100);
            }
            else Sleep(2);
            for (auto& kv : g_Th) if (kv.second.hot && kv.second.h) Sample(kv.second);
            QueryPerformanceCounter(&now);
            const double dt = (double)(now.QuadPart - w0.QuadPart) / (double)qf.QuadPart;
            if (dt >= kWindowS)
            {
                const unsigned long long tsc = __rdtsc();
                const double tscPerS = (double)(tsc - tsc0) / dt;
                LoadModules();
                Refresh(dt, tscPerS);
                Report(dt);
                // the next window's hot threads: this window's busiest
                std::vector<Th*> v;
                for (auto& kv : g_Th) { kv.second.hot = false; kv.second.samples = kv.second.sys = kv.second.failed = 0; kv.second.self.clear(); kv.second.via.clear(); kv.second.incl.clear(); kv.second.waits.clear(); kv.second.unwound = 0; if (kv.first != g_SelfTid) v.push_back(&kv.second); }
                std::sort(v.begin(), v.end(), [](const Th* a, const Th* b) { return a->cores > b->cores; });
                for (size_t i = 0; i < v.size() && i < (size_t)kMaxHot; i++) if (v[i]->cores >= kHotCores) v[i]->hot = true;
                w0 = now; tsc0 = tsc;
            }
        }
    }
}

void CpuProfile::SetEnabled(bool on)
{
    g_On = on;
    if (on && !g_Started.exchange(true))
        if (HANDLE h = CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr)) CloseHandle(h);
}

void CpuProfile::NoteTaskThread()
{
    thread_local bool t_Noted = false;
    if (t_Noted) return;
    t_Noted = true;
    const DWORD me = GetCurrentThreadId();
    for (auto& t : g_TaskTid) { DWORD e = 0; if (t.load() == me || t.compare_exchange_strong(e, me)) return; }
}
