#include "pch.h"
#include "PaceLog.h"
#include "PassProfile.h"
#include "GameInfo.h"
#include <d3d11_4.h>
#include <mutex>
#include <vector>
#include <map>
#include <algorithm>
#include <cmath>

namespace
{
    using ConvFn = int32_t(__stdcall*)(void* instance, int64_t time, LARGE_INTEGER* counter);   // xrConvertTimeToWin32PerformanceCounterKHR
    void* g_Inst = nullptr; ConvFn g_Conv = nullptr;

    double QpcF() { static double f = 0; if (f == 0) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = (double)q.QuadPart; } return f; }
    double Now() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / QpcF(); }

    // one submitted frame (a pair in synchronised stereo): CPU times (c) and GPU completion times (g), QPC seconds
    struct Frame
    {
        uint64_t id = 0;
        double cLeft = 0, cWait0 = 0, cWait1 = 0, cEnd0 = 0, cEnd1 = 0;
        double gLeft = 0, gBefore = 0, gAfter = 0;
        double disp = 0, period = 0;          // its predicted display time (QPC seconds, 0 = not converted), the period (s)
        uint64_t vBefore = 0, vAfter = 0;     // its fence values before / after xrEndFrame
        int64_t dispXr = 0, periodXr = 0;
        bool done = false;
    };
    constexpr int kRing = 1024;              // (5 s at 120 frames a second fits)
    std::mutex g_M;                           // everything below
    Frame g_F[kRing];
    uint64_t g_Cur = 1;                       // the frame being prepared (render thread)
    uint64_t g_Reported = 0;                  // frames up to this one are in a report
    double g_LastReport = 0;
    bool g_SaidNoConv = false, g_SaidNoFence = false, g_SaidStart = false;
    Frame& F(uint64_t id) { Frame& f = g_F[id % kRing]; if (f.id != id) { f = Frame{}; f.id = id; } return f; }

    // ---- the fence: when the GPU got past each point ----
    struct Sig { uint64_t v = 0, frame = 0; int kind = 0; };
    constexpr int kSigs = 4096;
    Sig g_Sig[kSigs];
    ID3D11DeviceContext* g_Imm = nullptr; ID3D11DeviceContext4* g_Ctx4 = nullptr; ID3D11Fence* g_Fence = nullptr;
    bool g_FenceTried = false;
    uint64_t g_Next = 0;                      // the last value signalled
    std::atomic<uint64_t> g_Issued{ 0 };
    HANDLE g_EvIssued = nullptr, g_EvDone = nullptr;

    DWORD WINAPI Waiter(void*)
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        uint64_t seen = 0;
        for (;;)
        {
            if (g_Issued.load() <= seen) { WaitForSingleObject(g_EvIssued, 100); continue; }
            uint64_t c = g_Fence->GetCompletedValue();
            if (c == UINT64_MAX) return 0;        // the device is gone
            if (c <= seen)
            {
                g_Fence->SetEventOnCompletion(seen + 1, g_EvDone);
                c = g_Fence->GetCompletedValue();
                if (c <= seen) { WaitForSingleObject(g_EvDone, 100); c = g_Fence->GetCompletedValue(); }
                if (c == UINT64_MAX) return 0;
            }
            if (c <= seen) continue;
            const double t = Now();
            std::lock_guard lk(g_M);
            for (uint64_t v = seen + 1; v <= c && v <= seen + kSigs; v++)
            {
                const Sig& s = g_Sig[v % kSigs];
                if (s.v != v) continue;
                Frame& f = g_F[s.frame % kRing];
                if (f.id != s.frame) continue;
                (s.kind == 0 ? f.gLeft : s.kind == 1 ? f.gBefore : f.gAfter) = t;
            }
            seen = c;
        }
    }
    bool FenceReady(ID3D11DeviceContext* imm)
    {
        if (g_Fence) return true;
        if (g_FenceTried || !imm) return false;
        g_FenceTried = true;
        ID3D11Device* dev = nullptr; imm->GetDevice(&dev);
        ID3D11Device5* d5 = nullptr;
        if (dev) dev->QueryInterface(__uuidof(ID3D11Device5), (void**)&d5);
        if (dev) dev->Release();
        ID3D11Fence* fence = nullptr; ID3D11DeviceContext4* c4 = nullptr;
        const bool ok = d5 && SUCCEEDED(d5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), (void**)&fence))
                     && SUCCEEDED(imm->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&c4));
        if (d5) d5->Release();
        g_EvIssued = CreateEventW(nullptr, FALSE, FALSE, nullptr); g_EvDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ok || !g_EvIssued || !g_EvDone)
        {
            if (fence) fence->Release(); if (c4) c4->Release();
            LOGI("PaceLog: no D3D11 fence (Direct3D 11.4 not available): the GPU's side of the pacing is not measured");
            return false;
        }
        g_Imm = imm; g_Ctx4 = c4; g_Fence = fence;   // (kept for the process's life: the waiter thread uses them)
        HANDLE th = CreateThread(nullptr, 0, Waiter, nullptr, 0, nullptr);
        if (!th) { LOGI("PaceLog: the fence's thread could not start: the GPU's side of the pacing is not measured"); g_Fence = nullptr; return false; }
        CloseHandle(th);
        return true;
    }
    //! (g_M held, render thread) the GPU passing this point of the frame being prepared
    void Signal(int kind)
    {
        if (!g_Fence || !g_Ctx4) return;
        const uint64_t v = ++g_Next;
        g_Sig[v % kSigs] = Sig{ v, g_Cur, kind };
        if (kind == 1) F(g_Cur).vBefore = v; else if (kind == 2) F(g_Cur).vAfter = v;
        g_Ctx4->Signal(g_Fence, v);
        g_Issued.store(v);
        SetEvent(g_EvIssued);
    }
    double ToQpc(int64_t xr)
    {
        if (!g_Conv || !g_Inst || xr <= 0) return 0;
        LARGE_INTEGER q{};
        if (g_Conv(g_Inst, xr, &q) < 0) return 0;
        return (double)q.QuadPart / QpcF();
    }

    // ---- the game's frame-sync Flush ----
    struct FlushRec { double t0 = 0, t1 = 0; int state = -1; uint64_t frame = 0; bool afterLeft = false; };
    std::vector<FlushRec> g_Flushes;          // since the last report (g_M)
    std::atomic<bool> g_FsProfile{ false }, g_FsSkip{ false };
    std::atomic<unsigned long long> g_FsSkipped{ 0 };
    bool g_FsPatched = false;
    constexpr uint32_t kFsSite = 0x3591FC, kFsCtx = 0x32D4EF8;   // mov rcx,[ctx]; mov rdx,[rcx]; call [rdx+0x378] (16 bytes)
    const uint8_t kFsOrig[16] = { 0x48, 0x8B, 0x0D, 0xF5, 0xBC, 0xF7, 0x02, 0x48, 0x8B, 0x11, 0xFF, 0x92, 0x78, 0x03, 0x00, 0x00 };
    using FlushFn = void(__stdcall*)(void*);
    void FrameSyncFlush()
    {
        void* ctx = *(void**)(GameInfo::Base() + kFsCtx);
        if (g_FsSkip.load(std::memory_order_relaxed)) { g_FsSkipped++; return; }
        if (!ctx) return;
        FlushFn fn = (FlushFn)((*(void***)ctx)[0x378 / 8]);
        if (!g_FsProfile.load(std::memory_order_relaxed) || !PassProfile::On()) { fn(ctx); return; }
        FlushRec r;
        {
            std::lock_guard lk(g_M);
            r.frame = g_Cur; r.afterLeft = F(g_Cur).cLeft > 0;
            const Frame& p = g_F[(g_Cur - 1) % kRing];
            if (g_Fence && p.id == g_Cur - 1 && p.vAfter)
            {
                const uint64_t c = g_Fence->GetCompletedValue();
                r.state = c < p.vBefore ? 0 : c < p.vAfter ? 1 : 2;
            }
        }
        r.t0 = Now();
        fn(ctx);
        r.t1 = Now();
        std::lock_guard lk(g_M);
        if (g_Flushes.size() < 4096) g_Flushes.push_back(r);
    }
    std::string Stats(std::vector<double> v);
    void FlushReport()
    {
        static unsigned long long lastSkipped = 0;
        const unsigned long long sk = g_FsSkipped.load();
        if (sk != lastSkipped) { LOGI("PaceLog: SkipFrameSyncFlush: {} of the game's frame-sync Flushes skipped in this window", sk - lastSkipped); lastSkipped = sk; }
        if (g_Flushes.empty()) return;
        static const double edges[] = { 0.1, 0.5, 1, 2, 4, 8, 1e9 };
        static const char* names[] = { "<0.1", "0.1-0.5", "0.5-1", "1-2", "2-4", "4-8", "8+" };
        int bins[7] = {}; double total = 0;
        double byState[4] = {}; int nState[4] = {};
        double side[2] = {}; int nSide[2] = {};
        std::vector<double> toRelease, blockedMs;   // a blocked Flush begun in SteamVR's hand-over: its end - SteamVR's release
        for (const FlushRec& r : g_Flushes)
        {
            const double ms = (r.t1 - r.t0) * 1000.0;
            total += ms;
            int b = 0; while (ms >= edges[b]) b++; bins[b]++;
            const int s = r.state + 1; byState[s] += ms; nState[s]++;
            side[r.afterLeft ? 1 : 0] += ms; nSide[r.afterLeft ? 1 : 0]++;
            if (ms >= 0.5)
            {
                blockedMs.push_back(ms);
                const Frame& p = g_F[(r.frame - 1) % kRing];
                if (r.state == 1 && p.id == r.frame - 1 && p.gAfter > 0) toRelease.push_back((r.t1 - p.gAfter) * 1000.0);
            }
        }
        std::string h; for (int i = 0; i < 7; i++) if (bins[i]) h += fmt::format(" {}:{}", names[i], bins[i]);
        LOGI("PaceLog: the game's frame-sync Flush (exe+0x3591B0): {} calls, {:.1f} ms in all ({:.2f} ms a call); by how long one took (ms: calls):{}",
             g_Flushes.size(), total, total / g_Flushes.size(), h);
        LOGI("PaceLog: Flush by where the GPU was when it began: still on the last pair {} calls {:.1f} ms | in SteamVR's hand-over {} calls {:.1f} ms | past it {} calls {:.1f} ms | not known {} calls {:.1f} ms; "
             "in the left frame {} calls {:.1f} ms, in the right frame {} calls {:.1f} ms",
             nState[1], byState[1], nState[2], byState[2], nState[3], byState[3], nState[0], byState[0], nSide[0], side[0], nSide[1], side[1]);
        LOGI("PaceLog: Flushes that blocked (0.5 ms or more) {} ms; of those begun in SteamVR's hand-over, their end minus SteamVR releasing the GPU {} ms (about 0 = the Flush waits for that release)",
             Stats(blockedMs), Stats(toRelease));
        g_Flushes.clear();
    }

    // ---- the report ----
    std::string Stats(std::vector<double> v)
    {
        if (v.empty()) return "-";
        std::sort(v.begin(), v.end());
        double s = 0; for (double x : v) s += x;
        auto p = [&](double q) { return v[std::min(v.size() - 1, (size_t)(q * (v.size() - 1) + 0.5))]; };
        return fmt::format("avg {:.2f} (min {:.2f}, p10 {:.2f}, median {:.2f}, p90 {:.2f}, max {:.2f})", s / v.size(), v.front(), p(0.1), p(0.5), p(0.9), v.back());
    }
    void Report(double now)
    {
        struct FlushAfter { ~FlushAfter() { FlushReport(); } } flushAfter;   // (after the frames' lines, whatever returns early)
        // frames whose fence times are in (two frames back: the GPU is at most that far behind)
        std::vector<const Frame*> fr;
        for (uint64_t id = std::max<uint64_t>(g_Reported + 1, g_Cur > (uint64_t)kRing - 8 ? g_Cur - (kRing - 8) : 1); id + 2 < g_Cur; id++)
        {
            const Frame& f = g_F[id % kRing];
            if (f.id == id && f.done) fr.push_back(&f);
        }
        const double span = now - g_LastReport;
        g_LastReport = now;
        if (fr.empty()) return;
        g_Reported = fr.back()->id;
        const double per = fr.back()->period;
        int rf[5] = {};                                       // shown 1, 2, 3, 4+ refreshes after the frame before (index 0: not known)
        std::map<int, int> gaps;                              // time between submits, 1 ms bins
        std::vector<double> waitMs, aheadMs, leftMs, beforeMs, afterMs, holdMs, cpuMs, lagMs, gpuLeftMs, gpuRightMs, cpuLeftMs, cpuRightMs;
        std::vector<double> beforeBy[3];                      // GPU done before display, by refreshes after the last (1, 2, 3+)
        int phase[8] = {}; int nPhase = 0;
        for (size_t i = 0; i < fr.size(); i++)
        {
            const Frame& f = *fr[i];
            const Frame* p = nullptr;
            { const Frame& q = g_F[(f.id - 1) % kRing]; if (q.id == f.id - 1 && q.done) p = &q; }
            int r = 0;
            if (p && f.periodXr > 0 && f.dispXr > p->dispXr) r = std::clamp((int)std::lround((double)(f.dispXr - p->dispXr) / (double)f.periodXr), 1, 4);
            rf[r]++;
            if (p && f.cEnd0 > p->cEnd0) gaps[std::min(40, (int)((f.cEnd0 - p->cEnd0) * 1000.0))]++;
            if (f.cWait1 > f.cWait0 && f.cWait0 > 0) waitMs.push_back((f.cWait1 - f.cWait0) * 1000.0);
            if (f.disp > 0 && f.cWait1 > 0) aheadMs.push_back((f.disp - f.cWait1) * 1000.0);
            if (f.disp > 0 && f.cEnd0 > 0) cpuMs.push_back((f.disp - f.cEnd0) * 1000.0);
            if (f.gBefore > 0 && f.cEnd0 > 0) lagMs.push_back((f.gBefore - f.cEnd0) * 1000.0);
            if (f.disp > 0 && f.gLeft > 0) leftMs.push_back((f.disp - f.gLeft) * 1000.0);
            if (f.disp > 0 && f.gBefore > 0)
            {
                beforeMs.push_back((f.disp - f.gBefore) * 1000.0);
                if (r >= 1) beforeBy[std::min(r, 3) - 1].push_back((f.disp - f.gBefore) * 1000.0);
            }
            if (f.disp > 0 && f.gAfter > 0)
            {
                afterMs.push_back((f.gAfter - f.disp) * 1000.0);
                if (f.period > 0)
                {
                    double ph = fmod((f.gAfter - f.disp) / f.period, 1.0); if (ph < 0) ph += 1.0;
                    phase[std::min(7, (int)(ph * 8))]++; nPhase++;
                }
            }
            if (f.gAfter > 0 && f.gBefore > 0) holdMs.push_back((f.gAfter - f.gBefore) * 1000.0);
            if (p && p->gAfter > 0 && f.gLeft > p->gAfter) gpuLeftMs.push_back((f.gLeft - p->gAfter) * 1000.0);
            if (f.gLeft > 0 && f.gBefore > f.gLeft) gpuRightMs.push_back((f.gBefore - f.gLeft) * 1000.0);
            if (p && p->cEnd1 > 0 && f.cLeft > p->cEnd1) cpuLeftMs.push_back((f.cLeft - p->cEnd1) * 1000.0);
            if (f.cLeft > 0 && f.cEnd0 > f.cLeft) cpuRightMs.push_back((f.cEnd0 - f.cLeft) * 1000.0);
        }
        const int known = rf[1] + rf[2] + rf[3] + rf[4];
        auto pc = [&](int n, int of) { return of ? 100.0 * n / of : 0.0; };
        std::string g; for (auto& [ms, n] : gaps) g += fmt::format(" {}{}:{}", ms >= 40 ? ">=" : "", ms, n);
        LOGI("PaceLog: ===== {:.1f} s: {} frames submitted (a frame = a pair in synchronised stereo), the headset's refresh {:.2f} ms | shown 1 refresh after the frame before {:.0f}%, 2 {:.0f}%, 3 {:.0f}%, 4+ {:.0f}% ({} known)",
             span, fr.size(), per * 1000.0, pc(rf[1], known), pc(rf[2], known), pc(rf[3], known), pc(rf[4], known), known);
        LOGI("PaceLog: time between submits (ms: frames):{}", g.empty() ? std::string(" -") : g);
        LOGI("PaceLog: xrWaitFrame held the frame {} ms; when it returned, the frame's display time was {} ms ahead; xrEndFrame called {} ms before the display time",
             Stats(waitMs), Stats(aheadMs), Stats(cpuMs));
        LOGI("PaceLog: CPU: the left image {} ms (from the last submit to its Present), the right image {} ms (its Present to the submit); the GPU finished the pair {} ms after the CPU submitted it",
             Stats(cpuLeftMs), Stats(cpuRightMs), Stats(lagMs));
        if (!g_Fence) return;
        LOGI("PaceLog: GPU: the left image {} ms (from SteamVR releasing the last frame to the left image done), the right image {} ms; SteamVR's hand-over (xrEndFrame's GPU work) {} ms",
             Stats(gpuLeftMs), Stats(gpuRightMs), Stats(holdMs));
        if (beforeMs.empty()) return;
        LOGI("PaceLog: GPU against the display time: the left image done {} ms before it, the right image (the whole pair) {} ms before it; SteamVR released the GPU {} ms after it",
             Stats(leftMs), Stats(beforeMs), Stats(afterMs));
        LOGI("PaceLog: the pair done before its display time, by how it was shown: 1 refresh after the last {} | 2 refreshes {} | 3+ {}",
             Stats(beforeBy[0]), Stats(beforeBy[1]), Stats(beforeBy[2]));
        LOGI("PaceLog: where in the refresh SteamVR released the GPU (eighths of the refresh after a display time; all in one = it waits for the refresh): {:.0f}% {:.0f}% {:.0f}% {:.0f}% {:.0f}% {:.0f}% {:.0f}% {:.0f}%",
             pc(phase[0], nPhase), pc(phase[1], nPhase), pc(phase[2], nPhase), pc(phase[3], nPhase), pc(phase[4], nPhase), pc(phase[5], nPhase), pc(phase[6], nPhase), pc(phase[7], nPhase));
    }
}

void PaceLog::SetTimeConverter(void* instance, void* convertFn)
{
    std::lock_guard lk(g_M);
    g_Inst = instance; g_Conv = (ConvFn)convertFn;
}

void PaceLog::WaitBegin()
{
    if (!PassProfile::On()) return;
    std::lock_guard lk(g_M);
    F(g_Cur).cWait0 = Now();
}

void PaceLog::WaitEnd(int64_t displayTime, int64_t period)
{
    if (!PassProfile::On()) return;
    const double t = Now();
    std::lock_guard lk(g_M);
    Frame& f = F(g_Cur);
    f.cWait1 = t;
    if (!f.dispXr) { f.dispXr = displayTime; f.periodXr = period; }
}

void PaceLog::AtLeft()
{
    if (!PassProfile::On()) return;
    std::lock_guard lk(g_M);
    F(g_Cur).cLeft = Now();
    // A fence Signal is a sync point with the driver's own thread, like the Flush it replaces: with SkipFrameSyncFlush on
    // it took over that Flush's wait (2026-11-04), so the left image is not marked then (the GPU's left / right split
    // is left out of the report)
    if (!g_FsSkip.load(std::memory_order_relaxed)) Signal(0);   // (the game's Present follows at once: it sends the signal to the GPU)
}

void PaceLog::BeforeEnd(ID3D11DeviceContext* immediate, int64_t displayTime, int64_t period)
{
    if (!PassProfile::On()) return;
    std::lock_guard lk(g_M);
    if (!g_SaidStart)
    {
        g_SaidStart = true; g_LastReport = Now();
        LOGI("PaceLog: the headset's frame pacing measured (ProfileRenderPasses), a report every 5 s{}{}", g_Conv ? "" : "; the runtime cannot convert its display times to this PC's clock: those parts are left out",
             "; the GPU's side by a D3D11 fence (with a Flush after each xrEndFrame, profiling only)");
    }
    FenceReady(immediate);
    Frame& f = F(g_Cur);
    f.cEnd0 = Now();
    f.dispXr = displayTime; f.periodXr = period;
    f.period = (double)period * 1e-9;
    f.disp = ToQpc(displayTime);
    Signal(1);   // (xrEndFrame sends it: the runtime flushes)
}

void PaceLog::AfterEnd()
{
    if (!PassProfile::On()) return;
    std::lock_guard lk(g_M);
    Frame& f = F(g_Cur);
    f.cEnd1 = Now();
    Signal(2);
    if (g_Imm && g_Fence) g_Imm->Flush();   // (so the GPU sees the signal at once, not with the next frame's work)
    f.done = true;
    g_Cur++;
    if (f.cEnd1 - g_LastReport >= 5.0) Report(f.cEnd1);
}

void PaceLog::SetFrameSync(bool profile, bool skip)
{
    g_FsProfile = profile;
    const bool was = g_FsSkip.exchange(skip);
    if (skip != was || skip)
        LOGI("PaceLog: SkipFrameSyncFlush {}", skip ? "on (experimental): the game's Flush at the end of each frame (exe+0x3591B0) is not made" : "off: the game's frame-sync Flush as the game makes it");
    if (g_FsPatched || !(profile || skip)) return;
    uint8_t* site = (uint8_t*)(GameInfo::Base() + kFsSite);
    if (memcmp(site, kFsOrig, sizeof(kFsOrig)) != 0) { LOGI("PaceLog: the game's frame-sync Flush (exe+0x{:X}) is not the expected code: not timed, not skipped", kFsSite); return; }
    uint8_t code[16] = { 0x48, 0xB8 };                    // mov rax, FrameSyncFlush
    const uint64_t fn = (uint64_t)(void*)&FrameSyncFlush;
    memcpy(code + 2, &fn, 8);
    code[10] = 0xFF; code[11] = 0xD0;                     // call rax (the same stack and shadow space as the game's call)
    code[12] = 0x0F; code[13] = 0x1F; code[14] = 0x40; code[15] = 0x00;   // nop
    DWORD old = 0;
    if (!VirtualProtect(site, sizeof(code), PAGE_EXECUTE_READWRITE, &old)) { LOGI("PaceLog: exe+0x{:X} cannot be written: the frame-sync Flush is not timed or skipped", kFsSite); return; }
    memcpy(site, code, sizeof(code));
    VirtualProtect(site, sizeof(code), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(code));
    g_FsPatched = true;
    LOGI("PaceLog: the game's frame-sync Flush (exe+0x{:X}) now goes through the mod ({})", kFsSite, skip ? "skipped" : "timed while profiling");
}
