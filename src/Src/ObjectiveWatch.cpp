#include "pch.h"
#include "ObjectiveWatch.h"
#include "GameInfo.h"
#include "HandAimHook.h"
#include "VRRuntime.h"
#include "WristHud.h"
#include <intrin.h>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    constexpr uintptr_t kObjMarkerVt = 0x1B7E748, kMarkerVt = 0x1C39A98, kAwareVt = 0x1C371E8;
    constexpr size_t kMarkerPoint = 0x3C;            // the marker's world point (measured 2026-11-04)
    constexpr float kParkX = 0.94f, kParkY = 0.90f;  // the parking spot, as a share of the HUD's width / height
    constexpr float kParkSide = 0.16f;               // its square left out of the HUD's panel, a share of the HUD's height
    constexpr double kFresh = 1.0;                   // s: an objective not projected for this long is gone
    constexpr int kMax = 8;

    std::atomic<bool> g_On{ true };
    struct Obj { const void* m = nullptr; float p[3] = {}; double t = 0; };
    std::mutex g_M;
    Obj g_O[kMax];
    std::atomic<unsigned long long> g_Direct{ 0 }, g_Scanned{ 0 }, g_Others{ 0 }, g_Unknown{ 0 }, g_Parked{ 0 };
    std::atomic<int> g_ParkLogged{ 0 };

    double Now() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart; }
    bool Rd(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    //! the marker whose world point this is: 1 objective, 0 another marker, -1 not a marker's (POD)
    int ClassAt(const void* m)
    {
        uintptr_t vt = 0;
        if (!m || ((uintptr_t)m & 7) || !Rd(&vt, m, 8)) return -1;
        const uintptr_t b = GameInfo::Base();
        if (vt == b + kObjMarkerVt) return 1;
        if (vt == b + kMarkerVt || vt == b + kAwareVt) return 0;
        return -1;
    }
    //! the marker among the callers' stack slots (the projection's callers hold it), when the point is a copy (POD)
    const void* ScanStack(const uint8_t* sp, int* cls)
    {
        for (int i = 0; i < 160; i++)
        {
            const void* p = nullptr;
            if (!Rd(&p, sp + i * 8, 8) || !p || ((uintptr_t)p & 7)) continue;
            const int c = ClassAt(p);
            if (c >= 0) { *cls = c; return p; }
        }
        return nullptr;
    }
}

void ObjectiveWatch::SetEnabled(bool on) { g_On = on; LOGI("ObjectiveWatch: {}", on ? "the objectives' directions on the watch, the game's objective markers off the HUD (WatchObjectives)" : "the game's own objective markers"); }
bool ObjectiveWatch::Enabled() { return g_On.load(); }
bool ObjectiveWatch::Active()
{
    if (!g_On.load() || WristHud::Watch() == 0) return false;
    // whether the headset runs, looked up 4 times a second (asked for every marker the game places)
    static std::atomic<unsigned long long> at{ 0 }; static std::atomic<bool> running{ false };
    const unsigned long long t = GetTickCount64();
    if (t - at.load(std::memory_order_relaxed) > 250) { at = t; running = VRRuntime::GetStatus().running; }
    return running.load(std::memory_order_relaxed);
}

bool ObjectiveWatch::OnMarker(const float* world)
{
    if (!world || !g_On.load()) return false;
    const void* m = (const uint8_t*)world - kMarkerPoint;
    int cls = ClassAt(m);
    if (cls >= 0) g_Direct++;
    else
    {
        m = ScanStack((const uint8_t*)_AddressOfReturnAddress(), &cls);
        if (!m) { g_Unknown++; return false; }
        g_Scanned++;
    }
    if (cls != 1) { g_Others++; return false; }
    float p[3];
    if (!Rd(p, world, 12) || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
    const double now = Now();
    std::lock_guard lk(g_M);
    Obj* slot = nullptr;
    for (Obj& o : g_O) if (o.m == m) { slot = &o; break; }
    if (!slot) for (Obj& o : g_O) if (!o.m || now - o.t > kFresh) { slot = &o; break; }
    if (!slot) return true;
    slot->m = m; memcpy(slot->p, p, 12); slot->t = now;
    return true;
}

void ObjectiveWatch::Park(float* out, int w, int h)
{
    if (!out || w <= 0 || h <= 0) return;
    const float ox = out[0], oy = out[1];
    out[0] = kParkX * (float)w; out[1] = kParkY * (float)h;
    out[2] = std::max(fabsf(out[2]), 0.1f);   // (in front: no flip to an edge)
    g_Parked++;
    if (g_ParkLogged.load() < 3) { g_ParkLogged++; LOGI("ObjectiveWatch: an objective marker moved off the HUD (from {:.0f},{:.0f} to the parking spot {:.0f},{:.0f} of {}x{}): its direction is on the watch", ox, oy, out[0], out[1], w, h); }
}

bool ObjectiveWatch::ParkHole(unsigned tw, unsigned th, float r[4])
{
    if (!Active() || !tw || !th) return false;
    const float cx = kParkX * (float)tw, cy = kParkY * (float)th, s = 0.5f * kParkSide * (float)th;
    r[0] = std::max(0.f, cx - s); r[1] = std::max(0.f, cy - s); r[2] = std::min((float)tw, cx + s); r[3] = std::min((float)th, cy + s);
    return true;
}

int ObjectiveWatch::Needles(Needle out[4])
{
    if (!Active()) return 0;
    float eye[3], ax[9];
    if (!HandAimHook::RealCamera(eye, ax)) return 0;   // the engine's axis rows: forward, left, up
    struct C { float x, y, d; } c[kMax]; int n = 0;
    {
        std::lock_guard lk(g_M);
        const double now = Now();
        for (const Obj& o : g_O)
        {
            if (!o.m || now - o.t > kFresh) continue;
            const float d[3] = { o.p[0] - eye[0], o.p[1] - eye[1], o.p[2] - eye[2] };
            const float f = d[0] * ax[0] + d[1] * ax[1] + d[2] * ax[2];
            const float l = d[0] * ax[3] + d[1] * ax[4] + d[2] * ax[5];
            const float horiz = sqrtf(f * f + l * l);
            if (horiz < 0.05f) continue;                    // straight above / below: no direction to show
            c[n++] = { -l / horiz, -f / horiz, sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) };   // right, down (ahead = up)
        }
    }
    std::sort(c, c + n, [](const C& a, const C& b) { return a.d < b.d; });
    const int k = std::min(n, 4);
    for (int i = 0; i < k; i++) out[i] = { c[i].x, c[i].y, i == 0 ? 1.f : 0.5f };
    return k;
}

std::string ObjectiveWatch::Status()
{
    int live = 0;
    { std::lock_guard lk(g_M); const double now = Now(); for (const Obj& o : g_O) if (o.m && now - o.t <= kFresh) live++; }
    return fmt::format("objectives on the watch {} (seen now {}; markers told by their point {}, from the stack {}, other markers {}, not markers {}; parked {})",
                       Active() ? "on" : g_On.load() ? "off (watch off / no headset)" : "off", live, g_Direct.load(), g_Scanned.load(), g_Others.load(), g_Unknown.load(), g_Parked.load());
}
