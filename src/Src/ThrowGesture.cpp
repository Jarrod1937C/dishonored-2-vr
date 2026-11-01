#include "pch.h"
#include "ThrowGesture.h"
#include "GameInfo.h"
#include "XRSession.h"
#include "VRRuntime.h"
#include "HandAimHook.h"
#include "BodyHide.h"
#include "Game.h"
#include <cmath>
#include <sstream>
#include <detours/detours.h>

namespace
{
    constexpr uint32_t kSite = 0x63333C;            // call exe+0x87C2A0 (entity+0x33C = *rdx) inside Throw
    constexpr uint32_t kSetVelocity = 0x87C2A0;
    // `lea rdx, [rsp+0x50] ; mov rcx, r14` just before it: the velocity is Throw's local at rsp+0x50, and the spawn
    // point (ComputeAimTarget's output, then the launch's position argument) is the one at rsp+0x70 -- 0x20 further.
    const uint8_t kSiteBytes[] = { 0x48, 0x8D, 0x54, 0x24, 0x50, 0x49, 0x8B, 0xCE, 0xE8 };
    constexpr uint32_t kSpawnFromVel = 0x20;
    constexpr uint32_t kGrenadeCpntTypeInfo = 0x39E28E0;   // arkCpntGrenade (Throw itself asks for it after the launch)
    constexpr uint32_t kMineCpntTypeInfo = 0x39BFBA0;      // arkCpntMine
    constexpr uint32_t kEntityVelocity = 0x33C;
    constexpr uint32_t kEntityOrigin = 0x300;

    struct Cfg
    {
        // A controller weighs nothing, and people throw a weightless thing softer than a real grenade. 1 = physical.
        float gain = 1.5f;
        // m/s. The game's own throw is the cap, so a hard throw reaches as far as the game's and never further:
        // measured 17.2 m/s, 6 deg up, landing 19 m away on a flat deck (Emily, impact grenade).
        float maxSpeed = 17.f;
        // Where it leaves from. The game launches 0.30 s after the release (its throw animation, measured), and the
        // drawn grenade stays in the drawn hand until then. 3 = the hand where it is at the launch: the grenade is seen
        // in the hand until it leaves, no jump. 1 = the hand where it was at the release (it jumps back), 2 = as if it had
        // left at the release (it jumps ahead, through any wall in between), 0 = the game's (0.46 m ahead of the hand).
        float peakWin = 0.15f, span = 0.02f, maxAge = 1.5f, inherit = 1.f, spawn = 3.f, teleport = 20.f;
        float haptic = 1.f;
        // 1 = a real throw (2026-11-01): the grenade leaves AT the trigger's release, from the hand, with the hand's
        // velocity then -- the mod fires the throwable itself (ARK_MSG_WEAPON_FIRE) instead of waiting for the game's
        // throw animation, which launched it 0.4-0.7 s later (in the user's logs) from wherever the hand had got to;
        // the animation's own launch is then dropped. 0 = the game's timing (the velocity still the hand's).
        float instant = 1.f;
    } C;
    struct Named { const char* name; float* v; };
    const Named kCfg[] = {
        { "gain", &C.gain }, { "maxspeed", &C.maxSpeed }, { "peakwin", &C.peakWin }, { "span", &C.span },
        { "maxage", &C.maxAge }, { "inherit", &C.inherit }, { "spawn", &C.spawn }, { "teleport", &C.teleport },
        { "haptic", &C.haptic }, { "instant", &C.instant },
    };

    std::atomic<bool> g_On{ true };
    bool g_Installed = false;
    long long Qpc() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
    double QpcSec() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); return (double)Qpc() / (double)f.QuadPart; }
    float Len(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
    bool Readable(const void* p, size_t n)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    //! Flags 0, as Throw itself asks the projectile (exe+0x63344F). With 2 -- what the player's components want -- an
    //! impact grenade came back without its arkCpntGrenade (measured).
    void* GetComponent(void* entity, uint32_t tiRva)
    {
        using Fn = void* (*)(void*, void*, int);
        const uintptr_t base = GameInfo::Base();
        return entity ? ((Fn)(base + BodyHide::kGetComponent))(entity, (void*)(base + tiRva), 0) : nullptr;
    }

    // ---- each hand's motion (render thread): [0] left (the grenade's), [1] right (GrabHand's throw) ----
    struct Sample { double t; float p[3]; };
    constexpr int kHist = 256;   // ~2 s at 120 Hz: the whole throw from the trigger's press (32 was ~0.3 s)
    struct History { Sample s[kHist]; int n = 0, head = -1; };
    History g_Hand[2];
    std::mutex g_HM;                // the samples: written by Tick (render thread), read by HandVelocity too
    unsigned g_Gen = 0;
    bool g_PrevTrig = false; double g_TrigDownAt = -1, g_TrigDownXr = -1;
    // the player's own velocity, from its view origin (world, m/s)
    struct PSample { double t; float o[3]; };
    PSample g_P[16]; int g_PN = 0, g_PHead = -1;
    float g_PlayerVel[3] = {};

    // ---- a release waiting for the game's throw (render thread -> game thread) ----
    std::mutex g_M;
    struct Pending { bool valid = false; double t = 0; float vHand[3] = {}, vPlayer[3] = {}, pos[3] = {}; float speedXr = 0; bool havePos = false; double cook = 0;
                     double pressXr = -1; /*!< the trigger's press, XR time (the throw's window starts there) */ };
    Pending g_Pending;
    unsigned long long g_Releases = 0, g_Calls = 0, g_Applied = 0, g_NotGrenade = 0, g_NoRelease = 0, g_Stale = 0, g_Capped = 0;
    float g_LastGame[3] = {}, g_LastOut[3] = {}, g_LastDelay = 0, g_LastHand = 0;

    // ---- flight log ----
    std::atomic<void*> g_TrackEnt{ nullptr };
    double g_TrackStart = 0, g_TrackNext = 0; float g_TrackFor = 4.f; int g_TrackMs = 100; void* g_TrackVt = nullptr;
    float g_TrackSpawn[3] = {}, g_TrackV[3] = {};
    std::atomic<float> g_TrackArm{ 4.f };

    //! Fastest velocity (XR m/s) of a hand between `tStart` and `tEnd` (XR s), differences over >= `span`.
    bool PeakVelocity(const History& h, double tStart, double tEnd, float out[3], float& speed)
    {
        speed = 0; out[0] = out[1] = out[2] = 0;
        if (h.n < 2) return false;
        bool any = false;
        for (int k = 0; k < h.n; k++)
        {
            const Sample& s = h.s[(h.head - k + kHist) % kHist];
            if (s.t > tEnd) continue;
            if (s.t < tStart) break;
            for (int j = k + 1; j < h.n; j++)
            {
                const Sample& o = h.s[(h.head - j + kHist) % kHist];
                const double dt = s.t - o.t;
                if (dt < C.span && j < h.n - 1) continue;
                if (dt <= 1e-4 || dt > 0.1) break;
                const float v[3] = { (s.p[0] - o.p[0]) / (float)dt, (s.p[1] - o.p[1]) / (float)dt, (s.p[2] - o.p[2]) / (float)dt };
                const float sp = Len(v);
                if (sp < C.teleport && sp >= speed) { speed = sp; memcpy(out, v, 12); any = true; }
                break;
            }
        }
        return any;
    }
    //! ...over the `peakWin` before `tEnd`
    bool ReleaseVelocity(const History& h, double tEnd, float out[3], float& speed) { return PeakVelocity(h, tEnd - C.peakWin, tEnd, out, speed); }

    // Called from the thunk at exe+0x63333C (game thread), before the real setter runs with the same arguments.
    void OnSetVelocity(void* ent, float* vel)
    {
        g_Calls++;
        float* spawn = (float*)((uint8_t*)vel + kSpawnFromVel);
        const float game[3] = { vel[0], vel[1], vel[2] };
        memcpy(g_LastGame, game, 12);
        const bool grenade = GetComponent(ent, kGrenadeCpntTypeInfo) != nullptr;
        const bool mine = !grenade && GetComponent(ent, kMineCpntTypeInfo) != nullptr;
        float hand[3] = {};
        const bool haveHand = [&] { float o[3], a[9]; if (!HandAimHook::ControllerPose(0, o, a)) return false; memcpy(hand, o, 12); return true; }();
        LOGI("Throw: the game throws a {} (entity {}): v ({:.2f},{:.2f},{:.2f}) |v| {:.2f} m/s, {:.0f} deg up; spawn ({:.2f},{:.2f},{:.2f}), {:.2f} m from the left controller",
             grenade ? "GRENADE" : mine ? "MINE" : "projectile", ent, game[0], game[1], game[2], Len(game),
             asinf(fmaxf(-1.f, fminf(1.f, game[2] / fmaxf(1e-4f, Len(game))))) * 57.29578f, spawn[0], spawn[1], spawn[2],
             haveHand ? sqrtf((spawn[0] - hand[0]) * (spawn[0] - hand[0]) + (spawn[1] - hand[1]) * (spawn[1] - hand[1]) + (spawn[2] - hand[2]) * (spawn[2] - hand[2])) : -1.f);
        auto track = [&](const float* sp, const float* v)
        {
            if (g_TrackArm.load() > 0.f) { memcpy(g_TrackSpawn, sp, 12); memcpy(g_TrackV, v, 12); g_TrackStart = 0; g_TrackEnt.store(ent); }
        };
        if (!g_On.load() || HandAimHook::Mode() < 2) { track(spawn, game); return; }
        if (!grenade) { g_NotGrenade++; track(spawn, game); return; }   // mines and razors: the game's placement
        Pending p;
        { std::lock_guard lk(g_M); p = g_Pending; g_Pending.valid = false; }
        const double now = QpcSec();
        if (!p.valid) { g_NoRelease++; LOGI("Throw: no trigger release recorded: the game's own throw"); track(spawn, game); return; }
        if (now - p.t > C.maxAge) { g_Stale++; LOGI("Throw: the last release is {:.2f} s old (> maxage {:.2f}): the game's own throw", now - p.t, C.maxAge); track(spawn, game); return; }
        float v[3];
        for (int i = 0; i < 3; i++) v[i] = C.gain * p.vHand[i] + C.inherit * p.vPlayer[i];
        const float sp = Len(v);
        if (C.maxSpeed > 0.f && sp > C.maxSpeed) { for (float& x : v) x *= C.maxSpeed / sp; g_Capped++; }
        for (float x : v) if (!std::isfinite(x)) { LOGI("Throw: non-finite velocity, the game's own throw"); return; }
        const float before[3] = { spawn[0], spawn[1], spawn[2] };
        if (C.spawn >= 2.5f) { if (haveHand) memcpy(spawn, hand, 12); }
        else if (C.spawn >= 0.5f && p.havePos)
        {
            memcpy(spawn, p.pos, 12);
            if (C.spawn >= 1.5f)
            {   // as if it had left at the release: carried along the throw for the delay (no wall test -- research)
                const float d = (float)(now - p.t);
                for (int i = 0; i < 3; i++) spawn[i] += v[i] * d;
                spawn[2] -= 0.5f * 9.81f * d * d; v[2] -= 9.81f * d;
            }
        }
        memcpy(vel, v, 12);
        g_Applied++; memcpy(g_LastOut, v, 12); g_LastDelay = (float)(now - p.t); g_LastHand = p.speedXr;
        if (C.haptic > 0.f) VRRuntime::Vibrate(0, 0.7f * C.haptic, 0.04f);
        LOGI("Throw: THROWN BY HAND {:.0f} ms after the release (cooked {:.2f} s): hand {:.2f} m/s -> v ({:.2f},{:.2f},{:.2f}) |v| {:.2f} m/s, {:.0f} deg up (player {:.2f} m/s); spawn ({:.2f},{:.2f},{:.2f}) -> ({:.2f},{:.2f},{:.2f})",
             (now - p.t) * 1000.0, p.cook, p.speedXr, v[0], v[1], v[2], Len(v),
             asinf(fmaxf(-1.f, fminf(1.f, v[2] / fmaxf(1e-4f, Len(v))))) * 57.29578f, Len(p.vPlayer),
             before[0], before[1], before[2], spawn[0], spawn[1], spawn[2]);
        track(spawn, v);
    }
    extern "C" void ThrowGestureOnSetVelocity(void* ent, float* vel) { OnSetVelocity(ent, vel); }

    // ---- the real throw: the grenade fired at the trigger's release (C.instant) ----
    // arkCpntWeaponThrowable::Throw exe+0x632F70 is the throwable's handler of ARK_MSG_WEAPON_FIRE (0xC8; registered
    // by exe+0x633580 on the component's owner entity, through the thunk exe+0x633A60). Each call spawns a NEW
    // projectile from the throwable's entity def (exe+0x63B5F0: [this+0x98]+0x10), so it can be fired at any moment.
    // The game fires it from an animation event of the throw (the player state's event dispatcher exe+0x84BCA0 posts
    // the event's message, here 0xC8, to the weapon) 0.3 s or more after the release. The mod posts the same message
    // at the release (exe+0x8103F0(exe+0x810240(), 0xC8, the weapon entity's id): the queue the events use) and drops
    // the animation's launch that follows (Throw hooked); the left hand's weapon models are hidden in between
    // (BodyHide), so the grenade is not seen both in the hand and in the air.
    constexpr uint32_t kThrow = 0x632F70, kMsgSystem = 0x810240, kMsgPostEmpty = 0x8103F0, kMsgWeaponFire = 0xC8;
    constexpr uint32_t kInvLeftItem = 0x70, kItemCpnt = 0x10, kCpntOwner = 0x10, kEntId = 0xA8;
    using FnThrow = uintptr_t (*)(void*, void*, void*, void*);
    FnThrow g_RealThrow = nullptr;
    std::mutex g_FireM;
    bool g_FireWanted = false; double g_FireWantedAt = 0;   // render thread -> game thread
    void* g_PostCpnt = nullptr; double g_PostedAt = -1; bool g_PostTaken = true;
    double g_SkipUntil = 0, g_UnhideAt = 0; bool g_Hidden = false;
    unsigned long long g_Instant = 0, g_AnimDropped = 0, g_PostFails = 0;

    //! The left hand's throwable component (an arkCpntWeaponThrowable), or null. No C++ objects (__try).
    uint8_t* LeftThrowable()
    {
        __try
        {
            const uintptr_t base = GameInfo::Base();
            void* player = Game::LocalPlayer();
            if (!player) return nullptr;
            using Fn = uint8_t* (*)(void*, void*, int);
            uint8_t* eq = ((Fn)(base + BodyHide::kGetComponent))(player, (void*)(base + BodyHide::kEquipCpntTypeInfo), 2);
            if (!eq) return nullptr;
            uint8_t* item = *(uint8_t**)(eq + kInvLeftItem);
            if (!item) return nullptr;
            uint8_t* c = *(uint8_t**)(item + kItemCpnt);
            if (!c || *(uintptr_t*)c != base + BodyHide::kThrowableCpntVtable) return nullptr;
            return c;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    bool PostFire(uint8_t* cpnt)
    {
        __try
        {
            const uintptr_t base = GameInfo::Base();
            const uint8_t* owner = *(uint8_t**)(cpnt + kCpntOwner);
            if (!owner) return false;
            const uint32_t id = *(const uint32_t*)(owner + kEntId);
            using FnSys = void* (*)();
            void* sys = ((FnSys)(base + kMsgSystem))();
            if (!sys) return false;
            using FnPost = void (*)(void*, uint32_t, uint32_t);
            ((FnPost)(base + kMsgPostEmpty))(sys, kMsgWeaponFire, id);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    uintptr_t ThrowHook(void* self, void* a, void* b, void* c)
    {
        const double now = QpcSec();
        bool ours = false, drop = false;
        {
            std::lock_guard lk(g_FireM);
            if (self == g_PostCpnt && !g_PostTaken && now - g_PostedAt < 0.5) { ours = true; g_PostTaken = true; g_SkipUntil = now + 2.0; }
            else if (self == g_PostCpnt && now < g_SkipUntil) { drop = true; g_SkipUntil = 0; g_UnhideAt = now + 0.15; }
        }
        if (drop)
        {
            g_AnimDropped++;
            LOGI("Throw: the throw animation's own launch dropped (the grenade left at the release)");
            return 0;
        }
        if (ours)
        {
            g_Instant++;
            if (!g_Hidden) { BodyHide::SetHideWeapon(0, true); g_Hidden = true; }
            { std::lock_guard lk(g_FireM); g_UnhideAt = now + 1.5; }   // (the animation's launch shows it back sooner)
            LOGI("Throw: fired at the release ({:.0f} ms after it)", (now - g_FireWantedAt) * 1000.0);
        }
        return g_RealThrow(self, a, b, c);
    }

    //! Saves everything volatile (the caller was compiled against a three-store leaf), calls the C++ side with the
    //! same (rcx, rdx), restores, and jumps on to the real setter.
    uint8_t* EmitThunk(uint8_t* p, uintptr_t target)
    {
        const uint8_t pre[] = {
            0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,     // push rcx rdx r8 r9 r10 r11
            0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,                       // sub rsp, 0x88  (16-aligned)
            0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,
            0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,
            0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70,   // movdqu [rsp+0x20..], xmm0..5
        };
        memcpy(p, pre, sizeof pre); p += sizeof pre;
        const uintptr_t fn = (uintptr_t)&ThrowGestureOnSetVelocity;
        *p++ = 0x48; *p++ = 0xB8; memcpy(p, &fn, 8); p += 8;                // mov rax, fn   (rcx, rdx untouched)
        *p++ = 0xFF; *p++ = 0xD0;                                           // call rax
        const uint8_t post[] = {
            0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,
            0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,
            0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,
            0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00,                       // add rsp, 0x88
            0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,     // pop r11 r10 r9 r8 rdx rcx
        };
        memcpy(p, post, sizeof post); p += sizeof post;
        *p++ = 0x48; *p++ = 0xB8; memcpy(p, &target, 8); p += 8;            // mov rax, the real setter
        *p++ = 0xFF; *p++ = 0xE0;                                           // jmp rax
        return p;
    }
    uint8_t* AllocNear(uintptr_t base, size_t size)
    {
        for (uintptr_t a = (base - 0x10000) & ~0xFFFFull; a > base - 0x70000000ull; a -= 0x10000)
            if (void* q = VirtualAlloc((void*)a, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return (uint8_t*)q;
        return nullptr;
    }
}

bool ThrowGesture::Install()
{
    if (g_Installed) return true;
    if (!GameInfo::IsSupportedBuild()) return false;
    const uintptr_t base = GameInfo::Base();
    uint8_t* call = (uint8_t*)(base + kSite);
    const uint8_t* pre = call - (sizeof kSiteBytes - 1);
    if (memcmp(pre, kSiteBytes, sizeof kSiteBytes) || (uintptr_t)(call + 5 + *(int32_t*)(call + 1)) != base + kSetVelocity)
    { LOGI("ThrowGesture: the throw's velocity call is not what it was measured to be; not installed"); return false; }
    uint8_t* block = AllocNear(base, 0x1000);
    if (!block) { LOGI("ThrowGesture: no memory within 2 GB of the exe"); return false; }
    EmitThunk(block, base + kSetVelocity);
    const int64_t rel = (int64_t)block - (int64_t)(call + 5);
    if (rel != (int32_t)rel) { LOGI("ThrowGesture: thunk out of rel32 range"); return false; }
    DWORD old = 0;
    if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    InterlockedExchange((volatile LONG*)(call + 1), (LONG)(int32_t)rel);
    VirtualProtect(call + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_Installed = true;
    LOGI("ThrowGesture: the throw's velocity (exe+0x{:X}) redirected: grenades leave with the hand's velocity", kSite);
    {   // the real throw (C.instant): Throw hooked, to drop the animation's launch after the mod's own at the release
        const uint8_t* t = (const uint8_t*)(base + kThrow);
        static const uint8_t kThrowBytes[] = { 0x40, 0x55, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x68 };
        static const uint8_t kPostBytes[] = { 0x48, 0x83, 0xEC, 0x28, 0x45, 0x8B, 0xC8, 0x48 };
        if ((t[0] != 0xE9 && memcmp(t, kThrowBytes, sizeof kThrowBytes)) || memcmp((const void*)(base + kMsgPostEmpty), kPostBytes, sizeof kPostBytes))
            LOGI("ThrowGesture: arkCpntWeaponThrowable::Throw / the message post are not what they were measured to be: grenades keep the game's throw timing");
        else
        {
            g_RealThrow = (FnThrow)(base + kThrow);
            DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)g_RealThrow, (PVOID)&ThrowHook);
            const LONG r = DetourTransactionCommit();
            if (r != NO_ERROR) g_RealThrow = nullptr;
            LOGI("ThrowGesture: a grenade leaves at the trigger's release (Throw exe+0x{:X} hooked: detour={})", kThrow, r);
        }
    }
    return true;
}

void ThrowGesture::GameTick()
{
    if (!g_RealThrow) return;
    const double now = QpcSec();
    bool want = false; double wantAt = 0;
    {
        std::lock_guard lk(g_FireM);
        if (g_FireWanted) { want = true; wantAt = g_FireWantedAt; g_FireWanted = false; }
        if (g_Hidden && g_UnhideAt > 0 && now > g_UnhideAt) { g_UnhideAt = 0; BodyHide::SetHideWeapon(0, false); g_Hidden = false; }
    }
    if (!want || now - wantAt > 0.3) return;
    uint8_t* cpnt = LeftThrowable();
    if (!cpnt) { g_PostFails++; LOGI("Throw: the release wanted the grenade fired, but the left hand holds no throwable now: the game's own launch"); return; }
    { std::lock_guard lk(g_FireM); g_PostCpnt = cpnt; g_PostedAt = now; g_PostTaken = false; }
    if (!PostFire(cpnt)) { g_PostFails++; std::lock_guard lk(g_FireM); g_PostTaken = true; LOGI("Throw: the fire message could not be posted: the game's own launch"); }
}

void ThrowGesture::Tick(const XRInputState& in, bool triggerHeld)
{
    const double t = (double)in.time * 1e-9;
    const VRRuntime::HeadPose hp = VRRuntime::GetHeadPose();
    {
        std::lock_guard lk(g_HM);   // HandVelocity reads the samples from a game thread
        const bool recentred = hp.gen != g_Gen;                                 // the poses jumped
        g_Gen = hp.gen;
        for (int i = 0; i < 2; i++)
        {
            const XRHandPose& h = in.hand[i];
            History& H = g_Hand[i];
            if (recentred) { H.n = 0; H.head = -1; }
            // an untracked or held-over pose adds no sample (it no longer empties the history: a moment's tracking loss
            // in a fast throw lost the throw); a gap of more than 0.1 s makes no velocity (PeakVelocity)
            else if (!h.valid || h.held) {}
            else if (H.head < 0 || t > H.s[H.head].t)
            {
                H.head = (H.head + 1) % kHist; H.s[H.head] = { t, { h.posX, h.posY, h.posZ } }; if (H.n < kHist) H.n++;
            }
        }
    }
    {   // the player's velocity (walking, sprinting), from its view origin over ~0.1 s; a blink or a load is not one
        float o[3], a[9]; const double now = QpcSec();
        if (Game::PlayerView(o, a))
        {
            g_PHead = (g_PHead + 1) % 16; g_P[g_PHead] = { now, { o[0], o[1], o[2] } }; if (g_PN < 16) g_PN++;
            for (int k = 1; k < g_PN; k++)
            {
                const PSample& s = g_P[(g_PHead - k + 16) % 16];
                const double dt = now - s.t;
                if (dt < 0.08 && k < g_PN - 1) continue;
                float v[3] = {};
                if (dt > 1e-3 && dt < 0.3) for (int i = 0; i < 3; i++) v[i] = (o[i] - s.o[i]) / (float)dt;
                if (Len(v) < 10.f) memcpy(g_PlayerVel, v, 12); else memset(g_PlayerVel, 0, 12);
                break;
            }
        }
        else { g_PN = 0; g_PHead = -1; memset(g_PlayerVel, 0, 12); }
    }
    const double now = QpcSec();
    if (triggerHeld && !g_PrevTrig) { g_TrigDownAt = now; g_TrigDownXr = t; }
    if (!triggerHeld && g_PrevTrig)
    {   // the release: the hand's velocity now, into the world as the hand is drawn
        Pending p; p.valid = true; p.t = now; p.cook = g_TrigDownAt >= 0 ? now - g_TrigDownAt : 0; p.pressXr = g_TrigDownXr;
        float vXr[3], speed = 0;
        { std::lock_guard lk(g_HM); ReleaseVelocity(g_Hand[0], t, vXr, speed); }
        float co[3], ca[9];
        if (HandAimHook::RealCamera(co, ca)) HandAimHook::XrOffsetToWorld(vXr, ca, p.vHand);
        memcpy(p.vPlayer, g_PlayerVel, 12);
        p.speedXr = speed;
        float o[3], a[9];
        if (HandAimHook::ControllerPose(0, o, a)) { memcpy(p.pos, o, 12); p.havePos = true; }
        { std::lock_guard lk(g_M); g_Pending = p; }
        g_Releases++;
        // a real throw: the grenade leaves now (the game thread fires it: GameTick)
        if (C.instant >= 0.5f && g_RealThrow && g_On.load() && HandAimHook::Mode() >= 2 && BodyHide::HeldItemClass(0) == BodyHide::kThrowableCpntVtable)
        { std::lock_guard lk(g_FireM); g_FireWanted = true; g_FireWantedAt = now; }
        LOGI("Throw: left trigger released after {:.2f} s: hand {:.2f} m/s (xr {:.2f},{:.2f},{:.2f} -> world {:.2f},{:.2f},{:.2f}), player {:.2f} m/s, {} samples",
             p.cook, speed, vXr[0], vXr[1], vXr[2], p.vHand[0], p.vHand[1], p.vHand[2], Len(g_PlayerVel), g_Hand[0].n);
    }
    g_PrevTrig = triggerHeld;
}

void ThrowGesture::OnPresent()
{
    void* ent = g_TrackEnt.load();
    if (!ent) return;
    const double now = QpcSec();
    if (g_TrackStart == 0)
    {
        g_TrackStart = now; g_TrackNext = now; g_TrackFor = g_TrackArm.load();
        g_TrackVt = Readable(ent, 8) ? *(void**)ent : nullptr;
        LOGI("Throw: flight of {} from ({:.2f},{:.2f},{:.2f}) at ({:.2f},{:.2f},{:.2f}) |v| {:.2f}", ent, g_TrackSpawn[0], g_TrackSpawn[1], g_TrackSpawn[2],
             g_TrackV[0], g_TrackV[1], g_TrackV[2], Len(g_TrackV));
    }
    if (now < g_TrackNext) return;
    g_TrackNext = now + g_TrackMs * 1e-3;
    const uint8_t* e = (const uint8_t*)ent;
    if (now - g_TrackStart > g_TrackFor || !Readable(e + kEntityOrigin, 12) || *(void* const*)e != g_TrackVt)
    { LOGI("Throw: flight log ends ({:.2f} s)", now - g_TrackStart); g_TrackEnt.store(nullptr); return; }
    const float* o = (const float*)(e + kEntityOrigin);
    const float d[3] = { o[0] - g_TrackSpawn[0], o[1] - g_TrackSpawn[1], o[2] - g_TrackSpawn[2] };
    LOGI("Throw: flight t {:.2f} pos ({:.2f},{:.2f},{:.2f}) from spawn: horizontal {:.2f} m, up {:+.2f} m",
         now - g_TrackStart, o[0], o[1], o[2], sqrtf(d[0] * d[0] + d[1] * d[1]), d[2]);
}

bool ThrowGesture::HandVelocity(int hand, float world[3], float* speedXr)
{
    float vXr[3], speed = 0; bool any; int n;
    {
        std::lock_guard lk(g_HM);
        const History& H = g_Hand[hand ? 1 : 0];
        if (H.head < 0) return false;
        any = ReleaseVelocity(H, H.s[H.head].t, vXr, speed);
        n = H.n;
    }
    float co[3], ca[9];
    if (!HandAimHook::RealCamera(co, ca)) return false;
    HandAimHook::XrOffsetToWorld(vXr, ca, world);
    if (speedXr) *speedXr = speed;
    return any || n >= 2;
}
void ThrowGesture::PlayerVelocity(float world[3]) { memcpy(world, g_PlayerVel, 12); }
float ThrowGesture::Cfg(const char* name)
{
    for (const Named& n : kCfg) if (!strcmp(n.name, name)) return *n.v;
    return 0.f;
}
void ThrowGesture::SetEnabled(bool on) { g_On = on; }
bool ThrowGesture::Enabled() { return g_On; }
void ThrowGesture::Track(float seconds, int ms) { g_TrackArm = seconds; g_TrackMs = ms > 10 ? ms : 10; }
bool ThrowGesture::Set(const std::string& key, float v)
{
    for (const Named& n : kCfg) if (key == n.name) { *n.v = v; return true; }
    return false;
}
std::string ThrowGesture::Config()
{
    std::ostringstream o;
    for (const Named& n : kCfg) o << n.name << " " << *n.v << "  ";
    return o.str();
}
std::string ThrowGesture::Stats()
{
    std::ostringstream o;
    o << (g_On.load() ? "on" : "off") << " installed " << g_Installed << " releases " << g_Releases << " throws " << g_Calls
      << " byhand " << g_Applied << " notgrenade " << g_NotGrenade << " norelease " << g_NoRelease << " stale " << g_Stale
      << " capped " << g_Capped << " last game " << Len(g_LastGame) << " out " << Len(g_LastOut) << " hand " << g_LastHand
      << " delay " << g_LastDelay << " atrelease " << g_Instant << " animdropped " << g_AnimDropped << " postfails " << g_PostFails;
    return o.str();
}
