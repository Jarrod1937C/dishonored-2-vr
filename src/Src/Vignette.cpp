#include "pch.h"
#include "Vignette.h"
#include <atomic>
#include <algorithm>
#include <cmath>

namespace
{
    // The speed that starts it, and the one that closes it all the way (m/s). Measured: walking ~2.9 m/s (ENGINE.md);
    // the running speed is in the status line's peak.
    constexpr float kSpeedFrom = 1.2f, kSpeedFull = 6.0f;
    // The shape, as angles off the eye's forward: open, the edge sits outside a headset's view (a corner of a ~110 deg
    // view is ~63 deg out); closed all the way, clear inside 29 deg and black past 52 (a strength of 1 at full speed).
    constexpr float kOpen = 1.15f, kClosed = 0.50f, kFeather = 0.40f;   // rad
    constexpr double kRise = 0.12, kFall = 0.35;                          // s: closes quickly, opens slowly
    constexpr double kStale = 0.15;                                       // s: no tick for this long = standing still
    constexpr float kTeleport = 25.f;                                     // m/s: faster than this in one tick is a jump, not a move

    std::atomic<float> g_Strength{ 0.6f };
    std::atomic<float> g_Speed{ 0.f };
    std::atomic<double> g_SpeedAt{ -1e9 };
    std::atomic<float> g_Peak{ 0.f };
    std::atomic<unsigned long long> g_Steps{ 0 }, g_Shown{ 0 };
    float g_Cur = 0.f; double g_Last = 0;   // the XR thread's
    std::atomic<float> g_CurOut{ 0.f };     // (for the status line)
    // the physics' thread: a little smoothing over its ticks (a tick's step is uneven on stairs and slopes)
    float g_Avg = 0.f;

    double Now() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart; }
}

void Vignette::SetStrength(float s)
{
    g_Strength = std::clamp(std::isfinite(s) ? s : 0.f, 0.f, 1.f);
    LOGI("Vignette: {}", g_Strength.load() > 0.f ? fmt::format("strength {:.2f}: the view's edges darken while the character moves (from {:.1f} m/s, all the way at {:.1f})",
                                                              g_Strength.load(), kSpeedFrom, kSpeedFull) : std::string("off (MovementVignette = 0)"));
}
float Vignette::Strength() { return g_Strength.load(); }

void Vignette::OnStep(float dx, float dy, float dt)
{
    if (!(dt > 1e-4f && dt < 0.25f)) return;
    float v = sqrtf(dx * dx + dy * dy) / dt;
    if (!std::isfinite(v) || v > kTeleport) v = 0.f;
    g_Avg += (v - g_Avg) * std::min(1.f, dt / 0.05f);
    g_Speed.store(g_Avg, std::memory_order_relaxed); g_SpeedAt.store(Now(), std::memory_order_relaxed);
    if (g_Avg > g_Peak.load(std::memory_order_relaxed)) g_Peak.store(g_Avg, std::memory_order_relaxed);
    g_Steps.fetch_add(1, std::memory_order_relaxed);
}

bool Vignette::Frame(float& inner, float& outer)
{
    const float s = g_Strength.load(std::memory_order_relaxed);
    const double now = Now();
    const float dt = g_Last ? (float)std::clamp(now - g_Last, 0.0, 0.1) : 0.f; g_Last = now;
    float speed = now - g_SpeedAt.load(std::memory_order_relaxed) > kStale ? 0.f : g_Speed.load(std::memory_order_relaxed);
    const float target = s * std::clamp((speed - kSpeedFrom) / (kSpeedFull - kSpeedFrom), 0.f, 1.f);
    const double tau = target > g_Cur ? kRise : kFall;
    g_Cur += (target - g_Cur) * (float)(1.0 - exp(-dt / tau));
    if (s <= 0.f) g_Cur = 0.f;
    g_CurOut.store(g_Cur, std::memory_order_relaxed);
    {   // the log, every 30 s while it shows (the speeds it saw: to tune kSpeedFrom / kSpeedFull)
        static double lastLog = 0; static unsigned long long lastShown = 0;
        if (now - lastLog > 30.0 && g_Shown.load(std::memory_order_relaxed) != lastShown) { lastLog = now; lastShown = g_Shown.load(); LOGI("Vignette: {}", Status()); }
    }
    if (g_Cur < 0.005f) { if (target == 0.f) g_Cur = target; return false; }
    inner = kOpen + (kClosed - kOpen) * g_Cur;
    outer = inner + kFeather;
    g_Shown.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::string Vignette::Status()
{
    return fmt::format("vignette strength {:.2f} now {:.2f} (speed {:.1f} m/s, peak {:.1f}; ticks {} frames shown {})", g_Strength.load(), g_CurOut.load(),
                       g_Speed.load(), g_Peak.load(), g_Steps.load(), g_Shown.load());
}
