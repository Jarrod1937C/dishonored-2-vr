#include "pch.h"
#include "SimStep.h"
#include "GameInfo.h"
#include "VRRuntime.h"
#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstring>

namespace
{
    constexpr uintptr_t kRateInsn = 0x59E0D;       // movss xmm10, [rip+disp] -> the 120.0 at kRateConst
    constexpr uint8_t kRateOp[5] = { 0xF3, 0x44, 0x0F, 0x10, 0x15 };
    constexpr uintptr_t kRateConst = 0x1AC9C28;     // 120.0f (read-only, also read elsewhere)
    constexpr uintptr_t kStepVar = 0x2280A54;       // 1/120 s (writable, read only by the clock, exe+0x59F55)
    constexpr float kGameHz = 120.f;

    std::atomic<bool> g_On{ true };
    bool g_Tried = false, g_Ok = false;
    float* g_Rate = nullptr;                        // the mod's own rate, within reach of the instruction
    std::atomic<float> g_Cur{ kGameHz };
    std::atomic<unsigned> g_Changes{ 0 };

    bool Rd(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    //! a page within a rel32 of `target` (below the image first, then above it)
    void* AllocNear(uintptr_t target)
    {
        SYSTEM_INFO si; GetSystemInfo(&si);
        const uintptr_t g = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
        const uintptr_t lim = 0x70000000ull;
        for (uintptr_t a = (target - 0x1000000) & ~(g - 1); a > g && target - a < lim; a -= g)
            if (void* p = VirtualAlloc((void*)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return p;
        for (uintptr_t a = (target + 0x8000000) & ~(g - 1); a - target < lim; a += g)
            if (void* p = VirtualAlloc((void*)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return p;
        return nullptr;
    }
    bool Patch()
    {
        if (g_Tried) return g_Ok;
        g_Tried = true;
        const uintptr_t b = GameInfo::Base();
        uint8_t* insn = (uint8_t*)(b + kRateInsn);
        uint8_t op[5] = {}; int32_t disp = 0; float rate = 0.f, step = 0.f;
        if (!Rd(op, insn, 5) || memcmp(op, kRateOp, 5) != 0 || !Rd(&disp, insn + 5, 4) || (uintptr_t)(insn + 9 + disp) != b + kRateConst ||
            !Rd(&rate, (const void*)(b + kRateConst), 4) || rate != kGameHz || !Rd(&step, (const void*)(b + kStepVar), 4) || fabsf(step - 1.f / kGameHz) > 1e-6f)
        {
            LOGI("SimStep: the game clock's step is not as expected (rate {:g}, step {:g}): left at the game's 1/120 s", rate, step);
            return false;
        }
        g_Rate = (float*)AllocNear((uintptr_t)insn);
        if (!g_Rate) { LOGI("SimStep: no memory within reach of the game's clock: left at 1/120 s"); return false; }
        *g_Rate = kGameHz;
        const long long nd = (long long)(uintptr_t)g_Rate - (long long)(uintptr_t)(insn + 9);
        if (nd < INT32_MIN || nd > INT32_MAX) { LOGI("SimStep: the mod's rate is out of the instruction's reach: left at 1/120 s"); return false; }
        DWORD old = 0;
        if (!VirtualProtect(insn + 5, 4, PAGE_EXECUTE_READWRITE, &old)) { LOGI("SimStep: could not patch the game's clock"); return false; }
        const int32_t d32 = (int32_t)nd;
        memcpy(insn + 5, &d32, 4);
        VirtualProtect(insn + 5, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), insn, 9);
        g_Ok = true;
        LOGI("SimStep: the game clock's step rate now read from the mod ({}), at 120 until the headset runs", (void*)g_Rate);
        return true;
    }
    void Apply(float hz)
    {
        if (!g_Ok) return;
        const uintptr_t b = GameInfo::Base();
        *(volatile float*)(b + kStepVar) = 1.f / hz;   // (the step first: a frame between the two takes the old count of new steps)
        *(volatile float*)g_Rate = hz;
        const float was = g_Cur.exchange(hz);
        g_Changes++;
        LOGI("SimStep: the game's simulation step {:.2f} ms ({:.1f} steps a second; was {:.1f}){}", 1000.f / hz, hz, was,
             hz == kGameHz ? ": the game's own" : ": one step per headset frame");
    }
}

void SimStep::SetEnabled(bool on)
{
    g_On = on;
    if (on) Patch();
    LOGI("SimStep: {}", on ? "the game's simulation steps follow the headset's refresh (SimStepAtRefresh)" : "the game's own 1/120 s steps");
    if (!on && g_Ok && g_Cur.load() != kGameHz) Apply(kGameHz);
}

void SimStep::OnPresent()
{
    if (!g_Ok) return;
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 250;
    float want = kGameHz;
    if (g_On.load() && VRRuntime::GetStatus().running)
    {
        const long long per = VRRuntime::DisplayPeriodNs();
        if (per > 0)
        {
            const float hz = roundf(1e10f / (float)per) / 10.f;   // (to 0.1 Hz)
            if (hz >= 50.f && hz <= 200.f) want = hz;
        }
    }
    if (fabsf(want - g_Cur.load()) > 0.25f) Apply(want);
}

std::string SimStep::Status()
{
    return fmt::format("simulation step {:.2f} ms ({:.1f}/s; {}, changes {})", 1000.f / g_Cur.load(), g_Cur.load(),
                       g_Ok ? (g_On.load() ? "at the headset's refresh" : "off") : "not patched", g_Changes.load());
}
