#include "pch.h"
#include "Roomscale.h"
#include "GameInfo.h"
#include "Collision.h"
#include "Vignette.h"
#include <detours/detours.h>
#include <cmath>
#include <algorithm>

namespace
{
    constexpr uintptr_t kIntegrate = 0x85A40;   // arkCharacterProxy::integrate(proxy, stepInfo, a, b, c)
    constexpr size_t kProxyPos = 0x80, kProxyVel = 0xB0, kStepDelta = 0x08;
    constexpr float kStopAt = 0.02f;            // m: following ends this close
    constexpr double kFresh = 0.25;             // s: an offset older than this is not followed (no view in play)

    using Fn = uintptr_t (*)(void*, void*, void*, void*, void*);
    Fn g_Real = nullptr;
    std::atomic<bool> g_On{ true };
    std::atomic<float> g_Dead{ 0.10f }, g_MaxSpeed{ 3.0f };

    std::mutex g_M;   // the view's thread and the physics' thread
    float g_Offset[2] = {}; bool g_Allowed = false; double g_OffsetAt = -1e9;
    float g_Pending[2] = {};                    // moved since the view last took it
    bool g_Following = false;
    unsigned long long g_Ticks = 0, g_Pushed = 0, g_Blocked = 0, g_Starts = 0, g_BlockStops = 0;
    // Blocked (2026-11-01, reported: leaning over a table to pick something up moved the view BACK): the head leaves the
    // character, it follows into the table, the game's collision stops it and pushes it back out -- and the view, which
    // rides the character, went back with it, every time the following started again. Now a push that the character
    // could not make (2 ticks in a row under a quarter of it) stops the following in that direction until the head
    // comes back within the deadzone or moves off sideways; and the character's being pushed back along the request
    // is credited to the view like a step (the view stays where the head is).
    bool g_BlockOn = false; float g_BlockDir[2] = {}; int g_BlockTicks = 0;
    double g_MovedTotal = 0;
    bool g_Logged = false;

    // ---- the wall fade ----
    constexpr float kFadeBefore = 0.05f, kFadeThrough = 0.10f;   // m: clear this far before the surface, black this far through
    constexpr double kFadeDown = 0.1, kFadeUp = 0.25;            // s
    std::atomic<bool> g_FadeOn{ true };
    std::atomic<float> g_FadeRgb[3] = { 20.f / 255.f, 34.f / 255.f, 82.f / 255.f };
    std::mutex g_HeadM; float g_Eye[3] = {}, g_Head[3] = {}; double g_HeadAt = -1e9;
    std::atomic<float> g_FadeTarget{ 1.f };
    std::atomic<unsigned long long> g_FadeRays{ 0 }, g_FadeHits{ 0 }, g_Fades{ 0 };

    double Now() { LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return (double)t.QuadPart / (double)f.QuadPart; }

    //! no C++ objects: __try
    bool ReadProxy(const uint8_t* proxy, float pos[3], float vel[3])
    {
        __try { memcpy(pos, proxy + kProxyPos, 12); memcpy(vel, proxy + kProxyVel, 12); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    //! the tick's step time (POD)
    float StepDt(const void* step)
    {
        __try { return *(const float*)((const uint8_t*)step + kStepDelta); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0.f; }
    }
    //! The comfort vignette's speed: the proxy's move this tick, less roomscale's part `rs` (the head's), across the floor
    void VignetteStep(const float p0[3], const float p1[3], const float rs[2], float dt)
    {
        Vignette::OnStep(p1[0] - p0[0] - rs[0], p1[1] - p0[1] - rs[1], dt);
    }
    uintptr_t Real(void* proxy, void* step, void* a, void* b, void* c)
    {   // not following: the call as it is, its move measured for the vignette (when it is on)
        float p0[3], v0[3], p1[3], v1[3];
        const bool vig = Vignette::Strength() > 0.f && proxy && step && ReadProxy((const uint8_t*)proxy, p0, v0);
        const uintptr_t r = g_Real(proxy, step, a, b, c);
        if (vig && ReadProxy((const uint8_t*)proxy, p1, v1)) { const float none[2] = {}; VignetteStep(p0, p1, none, StepDt(step)); }
        return r;
    }

    uintptr_t Hook(void* proxy, void* step, void* a, void* b, void* c)
    {
        if (!g_On.load() || !proxy || !step) return Real(proxy, step, a, b, c);
        float e[2] = {};   // the displacement asked of this tick
        float dt = 0.f;
        {
            std::lock_guard lk(g_M);
            g_Ticks++;
            dt = *(const float*)((const uint8_t*)step + kStepDelta);
            const bool fresh = g_Allowed && Now() - g_OffsetAt < kFresh;
            if (!fresh || !(dt > 1e-4f && dt < 0.25f)) g_Following = false;
            else
            {
                const float rx = g_Offset[0] - g_Pending[0], ry = g_Offset[1] - g_Pending[1], d = sqrtf(rx * rx + ry * ry);
                if (g_BlockOn)
                {   // blocked that way: not again until the head comes back, or goes off that way
                    const float along = d > 1e-4f ? (rx * g_BlockDir[0] + ry * g_BlockDir[1]) / d : 0.f;
                    if (d < g_Dead.load() || along < 0.5f) g_BlockOn = false;
                }
                if (g_BlockOn) g_Following = false;
                else if (!g_Following && d > g_Dead.load()) { g_Following = true; g_Starts++; }
                if (g_Following && d < kStopAt) g_Following = false;
                if (g_Following)
                {
                    const float maxStep = g_MaxSpeed.load() * dt, k = d > maxStep ? maxStep / d : 1.f;
                    e[0] = rx * k; e[1] = ry * k;
                }
            }
        }
        const float el = sqrtf(e[0] * e[0] + e[1] * e[1]);
        float p0[3], v0[3];
        if (el < 1e-5f || !ReadProxy((const uint8_t*)proxy, p0, v0)) return Real(proxy, step, a, b, c);
        float* vel = (float*)((uint8_t*)proxy + kProxyVel);
        vel[0] += e[0] / dt; vel[1] += e[1] / dt;
        const uintptr_t r = g_Real(proxy, step, a, b, c);
        float p1[3], v1[3];
        if (!ReadProxy((const uint8_t*)proxy, p1, v1)) return r;
        // what of the request the character moved: along it, the game's own velocity's part taken out, at most the request
        const float ux = e[0] / el, uy = e[1] / el;
        float along = ((p1[0] - p0[0]) - v0[0] * dt) * ux + ((p1[1] - p0[1]) - v0[1] * dt) * uy;
        along = along < -el ? -el : along > el ? el : along;   // (pushed back by the collision: credited too)
        const float mx = along * ux, my = along * uy;
        { const float rs[2] = { mx, my }; VignetteStep(p0, p1, rs, dt); }
        // the game keeps only its own velocity (its acceleration, animation and footsteps go by it)
        const float vk = along > 0.f ? along : 0.f;
        vel[0] -= vk * ux / dt; vel[1] -= vk * uy / dt;
        {
            std::lock_guard lk(g_M);
            g_Pending[0] += mx; g_Pending[1] += my; g_MovedTotal += along;
            g_Pushed++; if (along < 0.5f * el) g_Blocked++;
            if (along < 0.25f * el)
            {
                if (++g_BlockTicks >= 2) { g_BlockOn = true; g_BlockDir[0] = ux; g_BlockDir[1] = uy; g_Following = false; g_BlockTicks = 0; g_BlockStops++; }
            }
            else g_BlockTicks = 0;
            if (!g_Logged) { g_Logged = true; LOGI("Roomscale: first push: proxy {} at ({:.2f}, {:.2f}, {:.2f}), dt {:.4f}: asked {:.3f} m, moved {:.3f} m", proxy, p0[0], p0[1], p0[2], dt, el, along); }
        }
        return r;
    }
}

void Roomscale::Install()
{
    if (g_Real || !GameInfo::IsSupportedBuild()) return;
    const uint8_t* p = (const uint8_t*)(GameInfo::Base() + kIntegrate);
    static const uint8_t kExpect[] = { 0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x40, 0x18 };   // mov rax, rsp; mov [rax+18h], r8
    if (p[0] != 0xE9 && memcmp(p, kExpect, sizeof kExpect) != 0) { LOGI("Roomscale: exe+0x{:X} is not the character proxy's integrate (another game build?): no roomscale", kIntegrate); return; }
    g_Real = (Fn)(GameInfo::Base() + kIntegrate);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_Real, Hook);
    const LONG r = DetourTransactionCommit();
    if (r != NO_ERROR) g_Real = nullptr;
    LOGI("Roomscale: the player's character proxy (arkCharacterProxy::integrate exe+0x{:X}) follows the head: detour={}", kIntegrate, r);
}
void Roomscale::SetParams(bool on, float deadzone, float maxSpeed)
{
    g_On = on; g_Dead = std::clamp(deadzone, 0.f, 0.5f); g_MaxSpeed = std::clamp(maxSpeed, 0.5f, 10.f);
    if (!on) { std::lock_guard lk(g_M); g_Following = false; }
    LOGI("Roomscale: {}", on ? fmt::format("on: walking in the room walks the character (it follows once the head is {:.0f} cm from it, at up to {:.1f} m/s)",
                                           g_Dead.load() * 100.f, g_MaxSpeed.load()) : std::string("off: the view leaves the character when you walk (until a recentre)"));
}
bool Roomscale::Enabled() { return g_On.load() && g_Real; }
void Roomscale::SetWallFade(bool on, const float srgb[3])
{
    g_FadeOn = on; if (!on) g_FadeTarget = 1.f;
    for (int c = 0; c < 3; c++) g_FadeRgb[c] = std::clamp(srgb[c], 0.f, 1.f);
    LOGI("Roomscale: wall fade {}", on ? fmt::format("on: the headset fades to {},{},{} when your head goes into a wall the character cannot (WallFade = 1)",
                                                     (int)lroundf(g_FadeRgb[0] * 255.f), (int)lroundf(g_FadeRgb[1] * 255.f), (int)lroundf(g_FadeRgb[2] * 255.f)) : std::string("off"));
}
void Roomscale::WallFadeColor(float srgb[3]) { for (int c = 0; c < 3; c++) srgb[c] = g_FadeRgb[c].load(); }
void Roomscale::PublishHead(const float gameEye[3], const float head[3])
{
    std::lock_guard lk(g_HeadM);
    memcpy(g_Eye, gameEye, 12); memcpy(g_Head, head, 12); g_HeadAt = Now();
}
void Roomscale::Tick()
{
    if (!g_FadeOn.load()) return;
    float eye[3], head[3]; double at;
    { std::lock_guard lk(g_HeadM); memcpy(eye, g_Eye, 12); memcpy(head, g_Head, 12); at = g_HeadAt; }
    if (Now() - at > kFresh) { g_FadeTarget = 1.f; return; }
    const float d[3] = { head[0] - eye[0], head[1] - eye[1], head[2] - eye[2] };
    const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 0.02f) || len > 20.f) { g_FadeTarget = 1.f; return; }
    // the ray goes on past the head by kFadeBefore, so a surface just in front of the face starts the fade
    const float k = (len + kFadeBefore) / len;
    const float to[3] = { eye[0] + d[0] * k, eye[1] + d[1] * k, eye[2] + d[2] * k };
    float hit = 0.f;
    g_FadeRays++;
    if (!Collision::WorldRay(eye, to, hit)) { g_FadeTarget = 1.f; return; }
    g_FadeHits++;
    const float through = len - hit;   // the head past the surface (negative: still before it)
    const float t = (through + kFadeBefore) / (kFadeBefore + kFadeThrough);
    g_FadeTarget = 1.f - (t < 0.f ? 0.f : t > 1.f ? 1.f : t);
}
float Roomscale::WallFade()
{
    static float cur = 1.f; static double last = 0;
    const double now = Now(); const float dt = last ? (float)std::min(0.1, now - last) : 0.f; last = now;
    const float target = g_FadeOn.load() ? g_FadeTarget.load() : 1.f;
    const float rate = (float)(1.0 / (target < cur ? kFadeDown : kFadeUp));
    const float prev = cur;
    cur = target < cur ? std::max(target, cur - rate * dt) : std::min(target, cur + rate * dt);
    if (prev >= 0.99f && cur < 0.99f) g_Fades++;
    return cur;
}
void Roomscale::Publish(const float offset[2], bool allowed)
{
    bool log = false;
    {
        std::lock_guard lk(g_M);
        g_Offset[0] = offset[0]; g_Offset[1] = offset[1]; g_Allowed = allowed; g_OffsetAt = Now();
        if (!allowed) g_Following = false;
        static double lastLog = 0; static unsigned long long lastPushed = 0;
        if (g_OffsetAt - lastLog > 20.0 && g_Pushed != lastPushed) { lastLog = g_OffsetAt; lastPushed = g_Pushed; log = true; }
    }
    if (log) LOGI("Roomscale: {}", Stats());
}
bool Roomscale::TakeMoved(float moved[2])
{
    std::lock_guard lk(g_M);
    moved[0] = g_Pending[0]; moved[1] = g_Pending[1]; g_Pending[0] = g_Pending[1] = 0.f;
    return moved[0] != 0.f || moved[1] != 0.f;
}
void Roomscale::Reset()
{
    std::lock_guard lk(g_M);
    g_Pending[0] = g_Pending[1] = 0.f; g_Offset[0] = g_Offset[1] = 0.f; g_Following = false; g_BlockOn = false; g_BlockTicks = 0;
}
std::string Roomscale::Stats()
{
    std::lock_guard lk(g_M);
    return fmt::format("roomscale {} ticks {} pushes {} blocked {} (stopped {}) starts {} moved {:.1f} m offset {:.2f} m{}{}; wall fade rays {} hits {} fades {}; {}", g_On.load() ? "on" : "off", g_Ticks, g_Pushed,
                       g_Blocked, g_BlockStops, g_Starts, g_MovedTotal, sqrtf(g_Offset[0] * g_Offset[0] + g_Offset[1] * g_Offset[1]), g_Following ? " following" : "",
                       g_Allowed ? "" : " (held: a takedown's view, a host or sitting)", g_FadeRays.load(), g_FadeHits.load(), g_Fades.load(), Vignette::Status());
}
