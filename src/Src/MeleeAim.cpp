#include "pch.h"
#include "MeleeAim.h"
#include "GameInfo.h"
#include "HandAimHook.h"
#include "PlayerState.h"
#include "CVars.h"
#include "GpuTrace.h"
#include <cmath>
#include <sstream>
#include <algorithm>
#include <vector>
#include <intrin.h>
#include <detours/detours.h>

namespace
{
    constexpr uint32_t kGetViewAxis = 0x8067C0;     // lea rax, [rcx+0xFC] ; ret
    constexpr uint32_t kGetViewOrigin = 0x417820;   // lea rax, [rcx+0xD8] ; ret
    const uint8_t kAxisBytes[] = { 0x48, 0x8D, 0x81, 0xFC, 0x00, 0x00, 0x00, 0xC3 };
    const uint8_t kOrgBytes[] = { 0x48, 0x8D, 0x81, 0xD8, 0x00, 0x00, 0x00, 0xC3 };

    struct Site { uint32_t rva; bool axis; };
    // Every `call` to the two accessors between exe+0x60D000 and exe+0x636000 (tools/callers.py), i.e. the player's
    // melee component and the two helpers it calls below it. The ranged weapons' ComputeAimTarget (exe+0x638AE0) is
    // just above and deliberately left out: the fire ray already has its own hook.
    const Site kSites[] = {
        { 0x60D7C9, true }, { 0x61B64F, true }, { 0x62EC41, true }, { 0x62F0A5, true },
        { 0x62F30A, true }, { 0x62F755, true }, { 0x62F99A, true }, { 0x631792, true },
        { 0x60D7BE, false }, { 0x61B641, false }, { 0x62EBD5, false }, { 0x62F2EF, false },
        { 0x62F97D, false }, { 0x6308E2, false },
    };
    constexpr int kNumSites = (int)(sizeof kSites / sizeof kSites[0]);

    std::atomic<bool> g_On{ true };
    std::atomic<int> g_Origin{ 0 };
    std::atomic<int> g_TraceLeft{ 0 };   // research: log the next n melee accessor / cone-test calls
    bool g_Installed = false;
    std::atomic<uint64_t> g_Calls[kNumSites] = {}, g_Redirected[kNumSites] = {};
    int g_Patched = 0;

    // The last redirected axis and the view it replaced, for the per-swing log line (written by whichever thread
    // runs the melee code, read at Present; a torn read only garbles one log line).
    float g_LastHand[3] = {}, g_LastView[3] = {};
    std::atomic<uint64_t> g_AxisRedirects{ 0 };

    thread_local float t_Axis[9];
    thread_local float t_Org[3];

    // The gesture's frozen direction (SetGestureAxis). Written on the Present thread, read on the melee code's.
    std::mutex g_GestM;
    float g_GestAxis[9] = {};
    long long g_GestUntil = 0;                  // QPC ticks; 0 = none
    std::atomic<uint64_t> g_GestRedirects{ 0 };
    long long Qpc() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
    long long QpcFreq() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); return f.QuadPart; }
    std::atomic<int> g_FreezeMs{ -1 }; long long g_FreezeAt = 0;   // slashfreeze (research)
    bool GestureAxis(float out[9])
    {
        std::lock_guard lk(g_GestM);
        if (!g_GestUntil || Qpc() > g_GestUntil) return false;
        memcpy(out, g_GestAxis, 36);
        return true;
    }

    void AxisFromFwd(const float* fw, float* axis);
    std::atomic<int> g_BladeAim{ 1 };
    //! The trigger's strike direction. The sword's grip is -90 deg of pitch (GpuTrace), so the controller's forward is
    //! PERPENDICULAR to the blade: with the blade held forward (a thrust) it pointed at the floor or the sky (user report,
    //! 2026-10-04). Of the two candidate directions -- along the blade, and the fist's knuckles (the controller's forward)
    //! -- the strike takes the one closer to where the head looks: blade up = the knuckles, blade forward = the blade.
    bool TriggerAxis(const float* corg, const float* caxis, float* out)
    {
        float wo[3], wa[9], eye[3], view[9];
        if (!g_BladeAim.load() || !GpuTrace::WeaponFromHand(1, corg, caxis, wo, wa) || !HandAimHook::RealCamera(eye, view)) return false;
        const float* blade = wa; const float* knuckles = caxis;
        const float db = blade[0] * view[0] + blade[1] * view[1] + blade[2] * view[2];
        const float dk = knuckles[0] * view[0] + knuckles[1] * view[1] + knuckles[2] * view[2];
        AxisFromFwd(db >= dk ? blade : knuckles, out);
        return true;
    }

    const float* Resolve(void* cam, int idx)
    {
        const Site& s = kSites[idx];
        g_Calls[idx].fetch_add(1, std::memory_order_relaxed);

        const float* real = (const float*)((uint8_t*)cam + (s.axis ? 0xFC : 0xD8));
        if (!g_On.load() || HandAimHook::Mode() < 2 || !cam || cam != HandAimHook::CameraObject()) return real;
        float org[3], axis[9];
        if (!HandAimHook::ControllerPose(1, org, axis)) return real;
        for (float v : axis) if (!std::isfinite(v)) return real;
        if (s.axis)
        {
            if (GestureAxis(t_Axis)) g_GestRedirects.fetch_add(1, std::memory_order_relaxed);
            else if (!TriggerAxis(org, axis, t_Axis)) memcpy(t_Axis, axis, 36);
            memcpy(axis, t_Axis, 36);
            float view[9];
            if (HandAimHook::RealCamera(org, view)) memcpy(g_LastView, view, 12);
            memcpy(g_LastHand, axis, 12);
            g_AxisRedirects.fetch_add(1, std::memory_order_relaxed);
            g_Redirected[idx].fetch_add(1, std::memory_order_relaxed);
            return t_Axis;
        }
        if (g_Origin.load() == 1)
        {
            memcpy(t_Org, org, 12);
            g_Redirected[idx].fetch_add(1, std::memory_order_relaxed);
            return t_Org;
        }
        // Head origin: the game's own eye -- but the saved copy, not the live field, which a CamSwap on another
        // thread (the fire ray) may be holding at the hand for the length of one call.
        float view[9];
        if (HandAimHook::RealCamera(t_Org, view)) return t_Org;
        return real;
    }
    extern "C" const float* MeleeAimResolve(void* cam, int idx) { return Resolve(cam, idx); }

    //! A thunk for one call site. The accessors it stands in for touch nothing but rax, so everything volatile is
    //! saved around the C++ call -- the callers were compiled against a one-instruction leaf.
    uint8_t* EmitThunk(uint8_t* p, int idx)
    {
        const uint8_t pre[] = {
            0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,     // push rcx rdx r8 r9 r10 r11
            0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,                       // sub rsp, 0x88  (6 pushes + 0x88: 16-aligned)
            0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20,                             // movdqu [rsp+0x20], xmm0
            0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,                             //        [rsp+0x30], xmm1
            0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40,                             //        [rsp+0x40], xmm2
            0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,                             //        [rsp+0x50], xmm3
            0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60,                             //        [rsp+0x60], xmm4
            0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70,                             //        [rsp+0x70], xmm5
        };
        memcpy(p, pre, sizeof pre); p += sizeof pre;
        *p++ = 0xBA; memcpy(p, &idx, 4); p += 4;                            // mov edx, idx   (rcx = cam, untouched)
        const uintptr_t fn = (uintptr_t)&MeleeAimResolve;
        *p++ = 0x48; *p++ = 0xB8; memcpy(p, &fn, 8); p += 8;                // mov rax, fn
        *p++ = 0xFF; *p++ = 0xD0;                                           // call rax
        const uint8_t post[] = {
            0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,
            0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,
            0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,   // movdqu xmm0..5 back
            0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00,                       // add rsp, 0x88
            0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,     // pop r11 r10 r9 r8 rdx rcx
            0xC3,                                                           // ret (rax = the pointer)
        };
        memcpy(p, post, sizeof post); p += sizeof post;
        return p;
    }

    //! rel32 calls need the thunks within +-2 GB of the exe: walk down from the image base in 64 KB steps.
    uint8_t* AllocNear(uintptr_t base, size_t size)
    {
        for (uintptr_t a = (base - 0x10000) & ~0xFFFFull; a > base - 0x70000000ull; a -= 0x10000)
            if (void* p = VirtualAlloc((void*)a, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return (uint8_t*)p;
        return nullptr;
    }

    // Per-swing log: one line when a Slash begins, once the hook has redirected at least once during it.
    bool g_InSlash = false, g_Logged = false; uint64_t g_SlashStart = 0;
    float Deg(float r) { return r * 57.29578f; }

    // ---- the attack window and what it may hit (SwordContact, the development notes) ----
    // exe+0x634890(comp, active, b1, b2, float) writes comp+0x3428 bits 0..2 and comp+0x3424; the per-frame hit check
    // exe+0x631020(comp) does nothing unless bit 0 is set. Measured on a plain Slash: an animation event (exe+0x84E26D)
    // opens it 0.20 s after the click, another (exe+0x637139) closes it at 0.33 s, and a HIT closes it from inside the
    // hit check (exe+0x631405). Opening it from the hit check's own thread (research `vr meleeopen`) works: the checks
    // ran 24 ms after the Slash began. Every candidate the check finds goes through exe+0x62F900 (comp, point, shape):
    // "is this point inside the attack's 30 deg cone" -- the place to say "only what the blade touched".
    constexpr uint32_t kConeTest = 0x62F900, kSetActive = 0x634890, kHitCheck = 0x631020;
    constexpr uint32_t kRetHitClose = 0x63140A, kRetAnimOpen = 0x84E272;
    // A strike that lands on a CHARACTER: the strike function (around exe+0x6347ED: the damage through the component's
    // vtable +0x110, then exe+0x630540 with the target's id) closes the window itself right after (measured on the guard:
    // opened 0.25 s after the contact, closed 3 ms later from here, and he turned round wounded).
    constexpr uint32_t kRetStrikeClose = 0x634857;
    // In the hit check's loop a candidate that is not a damageable target (a prop, the world) and lies in the cone is
    // struck through exe+0x635140 (a bottle breaks there, a wall makes the blade recoil); its cone test returns here.
    constexpr uint32_t kRetObjectHit = 0x63136A;
    constexpr uint32_t kVtMeleePlayer = 0x1B9D508, kActiveFlags = 0x3428;
    // While the window is open the hit check adds the frame's dt to comp+0x3B78 for each valid candidate inside the cone,
    // and strikes when it reaches the constant at exe+0x1B9D6F0 (0.09 s) -- the target must DWELL in the cone ~90 ms (the
    // window being closed zeroes it every frame, exe+0x6314AC). With bit 1 of the flags it strikes at once, but bit 1 also
    // makes the attack face the body's axis instead of the view (slot 40), so a contact strike pre-loads the dwell instead.
    constexpr uint32_t kDwell = 0x3B78, kDwellNeeded = 0x1B9D6F0;
    using FnCone = bool (*)(void*, const float*, uint8_t*);
    using FnSetActive = void (*)(void*, uint8_t, uint8_t, uint8_t, float);
    using FnHitCheck = void (*)(void*, void*, void*, void*);
    FnCone g_RealCone = nullptr;
    FnSetActive g_RealSetActive = nullptr;
    FnHitCheck g_RealHitCheck = nullptr;
    bool IsPlayerMelee(void* comp) { return comp && *(uintptr_t*)comp == GameInfo::Base() + kVtMeleePlayer; }

    // What the blade touched, with when (QPC). Written on the Present thread, read by the cone test (game workers).
    std::mutex g_BoxM;
    std::vector<std::pair<MeleeAim::HitBox, long long>> g_Boxes;
    std::atomic<bool> g_Filter{ false };        // this Slash may only hit what the blade touched
    std::atomic<int> g_FilterMode{ 2 };         // vr swordfilter: 2 = only (and every) touched candidate, 1 = touched AND in the cone, 0 = never
    std::atomic<float> g_BoxMargin{ 0.04f };
    std::atomic<float> g_SweepFresh{ 0.6f };   // a contact steers the sweep this long (the Slash's own window opens 0.2 s after its click)
    std::atomic<long long> g_OpenUntil{ 0 };    // a pending "open the window now" (QPC deadline), 0 = none
    std::atomic<long long> g_OpenAskedAt{ 0 };
    std::atomic<bool> g_SlashRunning{ false };
    std::atomic<unsigned> g_SlashSerial{ 0 };
    std::atomic<bool> g_HitThisSlash{ false }, g_OpenedEarly{ false };
    // The running Slash's attack window: 0 = not opened yet, 1 = open, 2 = closed (by the animation or a hit). A contact
    // that comes once it has closed cannot be carried by that Slash any more (SwordContact queues a click instead).
    std::atomic<int> g_WindowPhase{ 0 };
    std::atomic<uint64_t> g_Forced{ 0 };       // touched candidates the 30 deg cone had refused (swordfilter 2)
    std::atomic<uint8_t> g_LastB1{ 0 }, g_LastB2{ 0 }; std::atomic<float> g_LastF{ 1.f };
    std::atomic<uint64_t> g_Opens{ 0 }, g_Hits{ 0 }, g_HitsEarly{ 0 }, g_Rejected{ 0 }, g_Accepted{ 0 }, g_Suppressed{ 0 }, g_ConeCalls{ 0 };
    std::atomic<long long> g_LastHitAt{ 0 };
    float g_LastHitPt[3] = {};                  // the last accepted candidate (torn reads only garble a log line)
    std::atomic<int> g_OpenAtMs{ -1 };          // research: open the window this long after the next Slash begins
    std::atomic<long long> g_OpenDue{ 0 };
    std::atomic<int> g_Shape42Left{ 0 };   // research: meleeshape

    //! A candidate's physics body -> its entity, the way the game's own target test does it (exe+0x635B00 / 0x6350B0):
    //! the body table (game+0x60410: records of 0xB0 at +0x28, count +0x30), the record's +0x70 must be the id and +0x44
    //! flagged, then exe+0x17D10(record) +8 +0x10 is the entity (its +0xA8 = its spawn id). Null when it does not resolve.
    constexpr uint32_t kGamePtr = 0x39A5290, kBodyTable = 0x60410, kBodyOwner = 0x17D10;
    using FnBodyOwner = void* (*)(void*);
    void* BodyEntity(uint32_t id)
    {
        __try
        {
            const uint8_t* game = *(const uint8_t* const*)(GameInfo::Base() + kGamePtr);
            if (!game) return nullptr;
            const uint8_t* tbl = *(const uint8_t* const*)(game + kBodyTable);
            const uint32_t idx = id & 0xFFFFFF;
            if (!tbl || idx == 0xFFFFFF || idx >= *(const uint32_t*)(tbl + 0x30)) return nullptr;
            uint8_t* rec = *(uint8_t* const*)(tbl + 0x28) + (size_t)idx * 0xB0;
            if (!(rec[0x44] & 3) || *(const uint32_t*)(rec + 0x70) != id) return nullptr;
            const uint8_t* x = (const uint8_t*)((FnBodyOwner)(GameInfo::Base() + kBodyOwner))(rec);
            if (!x) return nullptr;
            const uint8_t* y = *(const uint8_t* const*)(x + 8);
            return y ? *(void* const*)(y + 0x10) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    bool InsideBox(const MeleeAim::HitBox& b, const float* p, float m)
    {
        const float d[3] = { p[0] - b.org[0], p[1] - b.org[1], p[2] - b.org[2] };
        for (int r = 0; r < 3; r++)
        {
            const float l = d[0] * b.axis[r * 3] + d[1] * b.axis[r * 3 + 1] + d[2] * b.axis[r * 3 + 2];
            if (l < b.mn[r] - m || l > b.mx[r] + m) return false;
        }
        return true;
    }
    std::atomic<uint64_t> g_ById{ 0 }, g_ByBox{ 0 }, g_OtherEnt{ 0 };
    //! Did the blade touch what this candidate is? By the candidate body's ENTITY when it resolves -- a bottle's box
    //! also holds the table under its base, and the table's contact points there struck the table (measured) -- and by
    //! position only when the body has no entity.
    bool Touched(const float* p, uint32_t body)
    {
        void* ent = BodyEntity(body);
        const long long now = Qpc(), keep = (long long)(0.8 * (double)QpcFreq());
        const float m = g_BoxMargin.load();
        std::lock_guard lk(g_BoxM);
        for (const auto& [b, at] : g_Boxes)
        {
            if (now - at >= keep) continue;
            if (ent) { if (ent == b.ent) { g_ById++; return true; } }
            else if (InsideBox(b, p, m)) { g_ByBox++; return true; }
        }
        if (ent) g_OtherEnt++;
        return false;
    }

    bool FreshBox() { const long long now = Qpc(), keep = (long long)(0.8 * (double)QpcFreq()); std::lock_guard lk(g_BoxM); for (const auto& e : g_Boxes) if (now - e.second < keep) return true; return false; }
    bool ConeHook(void* comp, const float* pt, uint8_t* shape)
    {
        bool r = g_RealCone(comp, pt, shape);
        g_ConeCalls.fetch_add(1, std::memory_order_relaxed);
        // With the filter, what decides is what the BLADE touched, not the 30 deg cone around the attack axis: the axis is
        // aimed from the eye at the contact, but a body the blade met at its edge (an arm, a corpse's leg) can lie outside
        // the cone, and the swing then hit nothing (swordfilter 2, the default; 1 = the cone AND the touch).
        if ((r || g_FilterMode.load() >= 2) && IsPlayerMelee(comp) && g_Filter.load())
        {
            if (Touched(pt, *(const uint32_t*)((const uint8_t*)pt - 0x30 + 0x18)))
            {
                if (!r) g_Forced++;
                r = true; g_Accepted++; memcpy(g_LastHitPt, pt, 12);
                if (g_TraceLeft.load() <= 0 && g_Accepted.load() % 8 == 1) LOGI("MeleeAim: candidate at ({:.2f},{:.2f},{:.2f}) accepted (the blade touched it), from exe+{:#x}", pt[0], pt[1], pt[2], (uintptr_t)_ReturnAddress() - GameInfo::Base());
            }
            else { if (r) g_Rejected++; r = false; }
        }
        else if (r && IsPlayerMelee(comp)) memcpy(g_LastHitPt, pt, 12);
        if (r && IsPlayerMelee(comp) && (uintptr_t)_ReturnAddress() - GameInfo::Base() == kRetObjectHit && !g_HitThisSlash.exchange(true))
        {
            g_Hits++; if (g_OpenedEarly.load()) g_HitsEarly++;
            const long long now = Qpc(); g_LastHitAt = now;
            const long long asked = g_OpenAskedAt.load();
            LOGI("MeleeAim: the strike HIT an object at ({:.2f},{:.2f},{:.2f}) {} (filter {})", pt[0], pt[1], pt[2],
                 asked && g_OpenedEarly.load() ? fmt::format("{:.0f} ms after the contact", (double)(now - asked) * 1000.0 / (double)QpcFreq()) : std::string("(the game's own window)"), g_Filter.load());
        }
        if (g_TraceLeft.load() > 0 && IsPlayerMelee(comp) && (*((uint8_t*)comp + kActiveFlags) & 1))
        {
            g_TraceLeft--;
            LOGI("meleetrace: CONE ret exe+{:#x} comp {} pt ({:.3f},{:.3f},{:.3f}) cos {:.3f} -> {} (filter {}) flags {:#x} dwell {:.3f} entry +0x18 {:#x}",
                 (uintptr_t)_ReturnAddress() - GameInfo::Base(), comp, pt[0], pt[1], pt[2], *(float*)(shape + 0x14), r, g_Filter.load(),
                 *((uint8_t*)comp + kActiveFlags), *(float*)((uint8_t*)comp + kDwell), *(const uint32_t*)((const uint8_t*)pt - 0x30 + 0x18));
        }
        return r;
    }
    void SetActiveHook(void* comp, uint8_t on, uint8_t b1, uint8_t b2, float f)
    {
        const uintptr_t ret = (uintptr_t)_ReturnAddress() - GameInfo::Base();
        if (IsPlayerMelee(comp))
        {
            if (on && ret == kRetAnimOpen)
            {
                g_LastB1 = b1; g_LastB2 = b2; g_LastF = f;
                // The window was already opened (and spent) by a contact: the animation's own opening would be a second
                // hit of the same swing.
                if (g_OpenedEarly.load() && g_HitThisSlash.load()) { g_Suppressed++; LOGI("MeleeAim: the animation's attack window suppressed (this swing already hit)"); return; }
            }
            if (on) g_WindowPhase = 1;
            else if (g_WindowPhase.load() == 1) g_WindowPhase = 2;
            if (!on && (ret == kRetHitClose || ret == kRetStrikeClose))
            {
                g_HitThisSlash = true; g_Hits++; if (g_OpenedEarly.load()) g_HitsEarly++;
                const long long now = Qpc(); g_LastHitAt = now;
                const long long asked = g_OpenAskedAt.load();
                LOGI("MeleeAim: the strike HIT at ({:.2f},{:.2f},{:.2f}) {} (filter {}, accepted {} rejected {})", g_LastHitPt[0], g_LastHitPt[1], g_LastHitPt[2],
                     asked ? fmt::format("{:.0f} ms after the contact", (double)(now - asked) * 1000.0 / (double)QpcFreq()) : std::string("(the game's own window)"),
                     g_Filter.load(), g_Accepted.load(), g_Rejected.load());
            }
            if (g_TraceLeft.load() > 0) LOGI("meleewin: SetActive on {} b1 {} b2 {} f {:.3f} from exe+{:#x} (flags before {:#x})", on, b1, b2, f, ret, *((uint8_t*)comp + kActiveFlags));
        }
        g_RealSetActive(comp, on, b1, b2, f);
    }
    // The contacts' VELOCITY. exe+0x636B40 reads the three casts' results each frame and builds the candidates with the
    // weapon's velocity, (owner entity +0x300 - comp+0x33E0) / dt, then stores the owner's position in comp+0x33E0. The
    // owner is the ANIMATED weapon: 20 ms into a Slash it is still winding up, so a contact strike reached the flask in
    // 30 ms but only knocked it over (measured) -- the canned arc at full speed shatters it. With a fresh contact the
    // previous position is set so that this velocity is the drawn blade's at the contact.
    constexpr uint32_t kGather = 0x636B40, kPrevPos = 0x33E0, kFrameDt = 0x877EB0;
    using FnGather = void (*)(void*);
    using FnFrameDt = float* (*)(void*, float*);
    FnGather g_RealGather = nullptr;
    std::atomic<uint64_t> g_VelSet{ 0 };
    std::atomic<int> g_ContactVel{ 0 };
    void SweepState(float P[3], float V[3], float& speed, long long& at);
    void GatherHook(void* comp)
    {
        if (g_ContactVel.load() && IsPlayerMelee(comp) && !g_HitThisSlash.load())
        {
            float P[3], V[3], speed; long long at;
            SweepState(P, V, speed, at);
            uint8_t* owner = *(uint8_t**)((uint8_t*)comp + 0x10);
            if (at && owner && (double)(Qpc() - at) / (double)QpcFreq() < g_SweepFresh.load() && speed > 0.f)
            {
                float dtv = 0.f, tmp[4] = {};
                if (float* d = ((FnFrameDt)(GameInfo::Base() + kFrameDt))(owner, tmp)) dtv = *d;
                if (dtv > 1e-4f && dtv < 0.1f)
                {
                    const float* o = (const float*)(owner + 0x300);
                    float* prev = (float*)((uint8_t*)comp + kPrevPos);
                    for (int i = 0; i < 3; i++) prev[i] = o[i] - V[i] * speed * dtv;
                    g_VelSet++;
                }
            }
        }
        g_RealGather(comp);
    }

    // research: the hit loop's two gates -- exe+0x635B00(comp, entry) "is the candidate a damageable target" and
    // exe+0x62EB20(comp) "veto" (true = do not strike now)
    using FnValid = bool (*)(void*, void*);
    using FnVeto = bool (*)(void*);
    FnValid g_RealValid = nullptr; FnVeto g_RealVeto = nullptr;
    bool ValidHook(void* comp, void* entry)
    {
        const bool r = g_RealValid(comp, entry);
        if (g_TraceLeft.load() > 0 && IsPlayerMelee(comp) && (*((uint8_t*)comp + kActiveFlags) & 1))
        {
            const float* p = (const float*)((uint8_t*)entry + 0x30);
            LOGI("meleetrace: VALID body {:#x} ({:.2f},{:.2f},{:.2f}) -> {} dwell {:.3f}", *(uint32_t*)((uint8_t*)entry + 0x18), p[0], p[1], p[2], r, *(float*)((uint8_t*)comp + kDwell));
        }
        return r;
    }
    bool VetoHook(void* comp)
    {
        const bool r = g_RealVeto(comp);
        if (g_TraceLeft.load() > 0 && IsPlayerMelee(comp)) LOGI("meleetrace: VETO -> {} (an enemy in front: the strike waits for it)", r);
        return r;
    }
    // ---- the blade's own strikes ([Gameplay] SwordBladeDamage) ----
    // What deals a sword's damage is the component's strike, vtable slot 39 (the player's exe+0x62E550: its attack
    // modifiers, then the base strike exe+0x634320(comp, hit record, needs the window, close it on a hit, damage scale,
    // 0)): it skips a target already struck in this window (comp+0x33D0, cleared by closing the window, exe+0x634890),
    // asks slot 35 whether the target may be struck, builds the damage (the attack's table entry x the scale x the
    // weapon's base, exe+0x780A80), sends it (exe+0x884920), plays the hit (slot 34: blood, impact) and records the
    // target (exe+0x630540). The game calls it from the swing's animation, ~0.25 s after the click, on a hit record of
    // its physics sweep (lists comp+0x78 / +0x1188 / +0x2298, 0x88-byte records: +0x18 the body, +0x20 -> +0x10 the
    // entity, +0x30 the point). The per-frame hit check only plays the hit -- the "push" an early window gave.
    // A blade contact on a character therefore strikes it HERE, on the game thread: the attack window is opened so the
    // sweep runs (slot 41, steered to end at the contact), and as soon as a record of the touched character comes back
    // the strike is called on it, scaled by the blade's speed. No Slash, no click: the blade is the attack.
    std::atomic<bool> g_Direct{ false };
    std::atomic<float> g_DirectScale{ 1.f };
    struct DirReq { void* ent = nullptr; float mul = 1.f; long long at = 0; bool opened = false; unsigned serial = 0; };
    std::mutex g_DirM; DirReq g_Dir;
    std::atomic<unsigned> g_DirSerial{ 0 }, g_DirDone{ 0 }, g_DirLanded{ 0 }, g_DirDefended{ 0 };   // the last request, the last finished, the last that struck
    std::atomic<uint64_t> g_DirAsked{ 0 }, g_DirStruck{ 0 }, g_DirRefused{ 0 }, g_DirExpired{ 0 };
    constexpr double kDirWait = 0.35;      // s: the sweep's record of the touched character must come back by then
    constexpr int kSlotStrike = 39;
    using FnStrike = bool (*)(void*, void*, uint8_t, uint8_t, float, uint64_t);
    //! A hit record of `ent` in the sweep's lists, or null.
    uint8_t* RecordOf(void* comp, void* ent)
    {
        static const uint32_t lists[3][2] = { { 0x78, 0x80 }, { 0x1188, 0x1190 }, { 0x2298, 0x22A0 } };
        for (const auto& l : lists)
        {
            uint8_t* arr = *(uint8_t**)((uint8_t*)comp + l[0]);
            const int n = (int)(*(const uint32_t*)((uint8_t*)comp + l[1]) & 0xFFFFFF);
            for (int i = 0; arr && i < n && i < 256; i++)
            {
                uint8_t* e = arr + (size_t)i * 0x88;
                const uint8_t* b = *(const uint8_t* const*)(e + 0x20);
                void* owner = b ? *(void* const*)(b + 0x10) : nullptr;
                if (owner == ent || (!owner && BodyEntity(*(const uint32_t*)(e + 0x18)) == ent)) return e;
            }
        }
        return nullptr;
    }
    // A guard can still DEFEND against the blade. The game's swing does not call the strike directly: exe+0x62CB80 runs
    // each record of a character through a per-class handler first -- NPCs (class ids between the globals exe+0x39CE2C0
    // and +4) go to exe+0x6355F0(comp, record, entity), which reads the target's combat component (component type
    // exe+0x39B3F70, at +0xC8 its state, +0xCC who it faces): state 5 against the player = BLOCKING -- a roll against
    // the target's parry chance either parries (exe+0x636140) or blocks (the player's slot 36: the recoil), then the
    // sparks (exe+0x635F30) and the window closes, no damage; state 3 against the player = no effect at all; otherwise
    // (and when the attack is not blockable, comp+8 -> +0x180 == 0) the strike, slot 39, at x1. The blade's strike goes
    // through that handler too; the strike under it (exe+0x62E550, hooked) gets the blade's scale instead of the x1.
    constexpr uint32_t kNpcHandler = 0x6355F0, kStrikePlayer = 0x62E550, kGetComponent = 0x877720;
    constexpr uint32_t kCombatType = 0x39B3F70, kNpcClassRange = 0x39CE2C0;
    using FnHandler = bool (*)(void*, void*, void*);
    using FnGetComponent = uint8_t* (*)(void*, const void*, int);
    FnStrike g_RealStrike = nullptr;
    thread_local bool t_InDefend = false;
    thread_local float t_DefMul = 1.f;
    thread_local int t_DefStruck = -1;                       // -1 the strike was not called, 0 it refused, 1 it struck
    std::atomic<uint64_t> g_DirBlocked{ 0 }, g_DirIgnored{ 0 };
    bool StrikeHook(void* comp, void* rec, uint8_t needWin, uint8_t closeOnHit, float scale, uint64_t q)
    {
        if (!t_InDefend || !IsPlayerMelee(comp)) return g_RealStrike(comp, rec, needWin, closeOnHit, scale, q);
        const bool r = g_RealStrike(comp, rec, needWin, closeOnHit, t_DefMul, q);
        t_DefStruck = r ? 1 : 0;
        return r;
    }
    //! Is `ent` an NPC (the class range the game's swing sends through the defending handler)?
    bool IsNpcClass(void* ent)
    {
        __try
        {
            using FnType = const uint8_t* (*)(void*);
            const uint8_t* ti = ((FnType)(*(void***)ent)[0])(ent);
            if (!ti) return false;
            const int id = *(const int*)(ti + 0x60);
            const int* range = (const int*)(GameInfo::Base() + kNpcClassRange);
            return id >= range[0] && id <= range[1];
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    //! The target's combat state against the player: its state (+0xC8), or -1 without a combat component.
    int CombatState(void* ent, bool* facesPlayer)
    {
        *facesPlayer = false;
        __try
        {
            const uint8_t* c = ((FnGetComponent)(GameInfo::Base() + kGetComponent))(ent, (const void*)(GameInfo::Base() + kCombatType), 2);
            if (!c) return -1;
            *facesPlayer = *(const uint32_t*)(c + 0xCC) != 0;
            return *(const int*)(c + 0xC8);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
    //! The blade's strike through the game's own defence: 1 struck, 0 the strike refused, 2 defended (blocked, parried
    //! or ignored by the target), -1 the handler could not be called (strike directly instead).
    int DefendedStrike(void* comp, uint8_t* rec, void* ent, float mul, int* stateOut)
    {
        bool faces = false;
        *stateOut = CombatState(ent, &faces);
        t_InDefend = true; t_DefMul = mul; t_DefStruck = -1;
        bool called = false;
        __try { ((FnHandler)(GameInfo::Base() + kNpcHandler))(comp, rec, ent); called = true; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        t_InDefend = false;
        if (!called) return -1;
        return t_DefStruck < 0 ? 2 : t_DefStruck;
    }
    bool CallStrike(void* comp, uint8_t* rec, float mul)
    {
        __try
        {
            const FnStrike fn = (FnStrike)(*(void***)comp)[kSlotStrike];
            return fn(comp, rec, 0, 1, mul, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void DirectStep(void* comp)
    {
        DirReq r; { std::lock_guard lk(g_DirM); r = g_Dir; }
        if (!r.ent) return;
        const long long now = Qpc();
        const double age = (double)(now - r.at) / (double)QpcFreq();
        uint8_t& flags = *((uint8_t*)comp + kActiveFlags);
        auto finish = [&](bool closeIfOurs) {
            if (closeIfOurs && r.opened && (flags & 1) && !g_SlashRunning.load()) g_RealSetActive(comp, 0, 0, 0, 1.f);
            std::lock_guard lk(g_DirM); if (g_Dir.serial == r.serial) g_Dir = DirReq{};
            g_DirDone = r.serial;
        };
        if (age > kDirWait)
        {
            g_DirExpired++;
            LOGI("MeleeAim: blade strike: the sweep brought back no record of the touched character in {:.0f} ms (window {}, records {} / {} / {})", age * 1000.0,
                 flags & 1, *(uint32_t*)((uint8_t*)comp + 0x80) & 0xFFFFFF, *(uint32_t*)((uint8_t*)comp + 0x1190) & 0xFFFFFF, *(uint32_t*)((uint8_t*)comp + 0x22A0) & 0xFFFFFF);
            finish(true);
            return;
        }
        if (!(flags & 1))
        {   // the sweep runs while the window is open: open it (from this thread, as the contacts always did)
            g_RealSetActive(comp, 1, 0, 0, g_LastF.load());
            if (!r.opened) { std::lock_guard lk(g_DirM); if (g_Dir.serial == r.serial) g_Dir.opened = true; r.opened = true; }
        }
        uint8_t* rec = RecordOf(comp, r.ent);
        if (!rec) return;                                  // next frame: the sweep's results arrive a frame later
        const float* p = (const float*)(rec + 0x30);
        // an NPC defends itself the way it does against the game's swing (the handler reads the attack at comp+8)
        int how = -1, state = -1;
        if (IsNpcClass(r.ent) && *(void**)((uint8_t*)comp + 8)) how = DefendedStrike(comp, rec, r.ent, r.mul, &state);
        if (how == 2)
        {
            if (state == 5) g_DirBlocked++; else g_DirIgnored++;
            LOGI("MeleeAim: blade strike {} at ({:.2f},{:.2f},{:.2f}) (combat state {}), {:.0f} ms after the contact", state == 5 ? "BLOCKED / PARRIED by the target" : "ignored by the target",
                 p[0], p[1], p[2], state, age * 1000.0);
            g_HitThisSlash = true;
            { std::lock_guard lk(g_DirM); if (g_Dir.serial == r.serial) g_Dir = DirReq{}; }
            g_DirDefended = r.serial; g_DirDone = r.serial;
            if (r.opened && (flags & 1) && !g_SlashRunning.load()) g_RealSetActive(comp, 0, 0, 0, 1.f);
            return;
        }
        const bool ok = how >= 0 ? how == 1 : CallStrike(comp, rec, r.mul);
        if (ok)
        {
            g_DirStruck++; g_DirLanded = r.serial; g_HitThisSlash = true; g_Hits++; g_LastHitAt = now;
            LOGI("MeleeAim: blade strike LANDED at ({:.2f},{:.2f},{:.2f}), damage x{:.2f}, {:.0f} ms after the contact", p[0], p[1], p[2], r.mul, age * 1000.0);
        }
        else
        {
            g_DirRefused++;
            LOGI("MeleeAim: blade strike refused by the game at ({:.2f},{:.2f},{:.2f}) (struck already in this window, or not a target)", p[0], p[1], p[2]);
        }
        finish(!ok);   // a strike that landed closed the window itself
    }

    void HitCheckHook(void* comp, void* a, void* b, void* c)
    {
        if (g_Direct.load(std::memory_order_relaxed) && IsPlayerMelee(comp))
        {
            DirectStep(comp);
            // while a blade strike waits for its record, the game's own check stays out: it would only play the hit (the
            // push) on the touched character and close the window, and the strike would then be refused
            if (g_DirDone.load() != g_DirSerial.load()) return;
        }
        if (IsPlayerMelee(comp))
        {
            if (g_Shape42Left.load() > 0 && (*((uint8_t*)comp + kActiveFlags) & 1))
            {   // research (meleeshape): the master contact list, comp+0x1188 {entries, count}, stride 0x88
                const uint8_t* lst = *(const uint8_t* const*)((uint8_t*)comp + 0x1188);
                const int n = (int)(*(const uint32_t*)((uint8_t*)comp + 0x1190) & 0xFFFFFF);
                std::string ids;
                for (int i = 0; i < n && i < 12 && lst; i++)
                {
                    const uint8_t* e = lst + i * 0x88; const float* p = (const float*)(e + 0x30);
                    ids += fmt::format(" [{:#x} ({:.2f},{:.2f},{:.2f}) +0x54 {:.3f}]", *(const uint32_t*)(e + 0x18), p[0], p[1], p[2], *(const float*)(e + 0x54));
                }
                const uint8_t* l78 = *(const uint8_t* const*)((uint8_t*)comp + 0x78); const int n78 = (int)(*(const uint32_t*)((uint8_t*)comp + 0x80) & 0xFFFFFF);
                const uint8_t* l22 = *(const uint8_t* const*)((uint8_t*)comp + 0x2298); const int n22 = (int)(*(const uint32_t*)((uint8_t*)comp + 0x22A0) & 0xFFFFFF);
                std::string a, b;
                for (int i = 0; i < n78 && i < 8 && l78; i++) { const uint8_t* e = l78 + i * 0x88; const float* p = (const float*)(e + 0x30); a += fmt::format(" [{:#x} ({:.2f},{:.2f},{:.2f})]", *(const uint32_t*)(e + 0x18), p[0], p[1], p[2]); }
                for (int i = 0; i < n22 && i < 8 && l22; i++) { const uint8_t* e = l22 + i * 0x88; const float* p = (const float*)(e + 0x30); b += fmt::format(" [{:#x} ({:.2f},{:.2f},{:.2f})]", *(const uint32_t*)(e + 0x18), p[0], p[1], p[2]); }
                LOGI("meleelist: flags {:#x} entities(0x1188) {}:{} | enemies(0x78) {}:{} | world(0x2298) {}:{}", *((uint8_t*)comp + kActiveFlags), n, ids, n78, a, n22, b);
            }
            const long long now = Qpc();
            const long long due = g_OpenDue.load();
            if (due && now >= due) { g_OpenDue = 0; LOGI("meleewin: opening the window NOW (research)"); g_RealSetActive(comp, 1, g_LastB1.load(), g_LastB2.load(), g_LastF.load()); }
            const long long until = g_OpenUntil.load();
            if (until && now > until) { g_OpenUntil = 0; LOGI("MeleeAim: a contact's strike expired (no Slash to carry it)"); }
            else if (until && g_SlashRunning.load() && !g_HitThisSlash.load())
            {
                g_OpenUntil = 0;
                const uint8_t flags = *((uint8_t*)comp + kActiveFlags);
                if (!(flags & 1)) g_RealSetActive(comp, 1, g_LastB1.load(), g_LastB2.load(), g_LastF.load());
                g_OpenedEarly = true; g_Opens++;
                const long long asked = g_OpenAskedAt.load();
                LOGI("MeleeAim: attack window opened by the contact{} ({:.0f} ms after it)", (flags & 1) ? " (it was already open)" : "",
                     asked ? (double)(now - asked) * 1000.0 / (double)QpcFreq() : -1.0);
            }
        }
        if (IsPlayerMelee(comp) && g_Filter.load() && (*((uint8_t*)comp + kActiveFlags) & 1) && !g_HitThisSlash.load() && FreshBox())
        {   // a contact strike: the blade already touched the target, it does not need to dwell in the cone
            float* dwell = (float*)((uint8_t*)comp + kDwell);
            const float need = *(const float*)(GameInfo::Base() + kDwellNeeded);
            if (*dwell < need) *dwell = need;
        }
        g_RealHitCheck(comp, a, b, c);
    }

    // research: slot 42 (exe+0x62F290, the attack's start + shape: {centre, dims, axis}) -- what it hands out, per call
    constexpr uint32_t kSlot42 = 0x62F290;
    using FnSlot42 = float* (*)(void*, float*);
    FnSlot42 g_RealSlot42 = nullptr;
    // research: slot 41 (exe+0x634990, the sweep the three physics casts use): out start, out end, out axis, out dir
    constexpr uint32_t kSlot41 = 0x634990;
    using FnSlot41 = void* (*)(void*, float*, float*, float*, float*);
    FnSlot41 g_RealSlot41 = nullptr;
    // The contact's sweep (SwordContact): where the drawn blade touched and the way it was moving there.
    std::mutex g_SweepM; float g_SweepP[3] = {}, g_SweepV[3] = {}, g_SweepSpeed = 0.f; long long g_SweepAt = 0;
    void SweepState(float P[3], float V[3], float& speed, long long& at) { std::lock_guard lk(g_SweepM); memcpy(P, g_SweepP, 12); memcpy(V, g_SweepV, 12); speed = g_SweepSpeed; at = g_SweepAt; }
    std::atomic<uint64_t> g_SweepsMoved{ 0 };
    std::atomic<int> g_SweepMode{ 0 };   // 0 = the game's sweep (default), 1 = its start + the contact as the end, 2 = along the blade's motion
    void* Slot41Hook(void* comp, float* start, float* end, float* axis, float* dir)
    {
        void* r = g_RealSlot41(comp, start, end, axis, dir);
        // The three physics casts the hit check reads next frame sweep the attack box along this segment: measured, from
        // the ANIMATED weapon's previous position 2.8 m back along its motion to where it is now (both 0.8 m back along
        // the attack axis, decl+0x17C) -- so they find what the canned arc in front of the camera passes through, ~0.25 s
        // into the Slash. A contact of the DRAWN blade replaces it: the sweep ends at the contact point, arriving the way
        // the blade was moving, so the physics finds the touched object on the next frame.
        if (IsPlayerMelee(comp) && !g_HitThisSlash.load())
        {
            float P[3], V[3]; long long at;
            { std::lock_guard lk(g_SweepM); memcpy(P, g_SweepP, 12); memcpy(V, g_SweepV, 12); at = g_SweepAt; }
            if (at && (double)(Qpc() - at) / (double)QpcFreq() < g_SweepFresh.load())
            {
                const float back = *(float*)(*(uint8_t**)((uint8_t*)comp + 8) + 0x17C);
                const float range = *(float*)((uint8_t*)comp + 0x33C8);
                const float len = range > 0.5f && range < 10.f ? range : 3.f;
                int mode = g_SweepMode.load();
                if (g_Direct.load() && g_DirDone.load() != g_DirSerial.load()) mode = 2;   // a blade strike: the sweep runs into the contact
                if (mode == 1)
                {   // the game's start (behind the animated weapon, by the camera), our end: the sweep runs into the touched point
                    float d[3]; for (int i = 0; i < 3; i++) { end[i] = P[i] - axis[i] * back; d[i] = end[i] - start[i]; }
                    const float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    if (n > 1e-3f) for (int i = 0; i < 3; i++) dir[i] = d[i] / n;
                }
                else if (mode == 2)
                {   // along the blade's motion, ending at the touched point
                    for (int i = 0; i < 3; i++) { end[i] = P[i] - axis[i] * back; start[i] = end[i] - V[i] * len; dir[i] = V[i]; }
                }
                if (mode) g_SweepsMoved++;
                if (mode && g_Shape42Left.load() > 0)
                    LOGI("meleesweep: CONTACT mode {} start ({:.2f},{:.2f},{:.2f}) end ({:.2f},{:.2f},{:.2f}) P ({:.2f},{:.2f},{:.2f})", mode, start[0], start[1], start[2], end[0], end[1], end[2], P[0], P[1], P[2]);
            }
        }
        if (g_Shape42Left.load() > 0 && IsPlayerMelee(comp))
        {
            const uint8_t* ent = *(const uint8_t* const*)((uint8_t*)comp + 0x10);
            const float* eo = ent ? (const float*)(ent + 0x300) : nullptr;
            LOGI("meleesweep: start ({:.3f},{:.3f},{:.3f}) end ({:.3f},{:.3f},{:.3f}) axis ({:.3f},{:.3f},{:.3f}) dir ({:.3f},{:.3f},{:.3f}) | owner {} at ({:.3f},{:.3f},{:.3f}) prev ({:.3f},{:.3f},{:.3f}) range {:.3f} off ({:.3f},{:.3f},{:.3f}) decl+0x17C {:.3f}",
                 start[0], start[1], start[2], end[0], end[1], end[2], axis[0], axis[1], axis[2], dir[0], dir[1], dir[2], (const void*)ent,
                 eo ? eo[0] : 0.f, eo ? eo[1] : 0.f, eo ? eo[2] : 0.f, *(float*)((uint8_t*)comp + 0x33E0), *(float*)((uint8_t*)comp + 0x33E4), *(float*)((uint8_t*)comp + 0x33E8),
                 *(float*)((uint8_t*)comp + 0x33C8), *(float*)((uint8_t*)comp + 0x33EC), *(float*)((uint8_t*)comp + 0x33F0), *(float*)((uint8_t*)comp + 0x33F4),
                 *(float*)(*(uint8_t**)((uint8_t*)comp + 8) + 0x17C));
        }
        return r;
    }
    float* Slot42Hook(void* comp, float* out)
    {
        float* r = g_RealSlot42(comp, out);
        if (g_Shape42Left.load() > 0 && IsPlayerMelee(comp))
        {
            g_Shape42Left--;
            LOGI("meleeshape: centre ({:.3f},{:.3f},{:.3f}) dims ({:.3f},{:.3f},{:.3f}) fwd ({:.3f},{:.3f},{:.3f}) | decl +0x178 {:.3f} +0x188 {:.3f} {:.3f} {:.3f} {:.3f} +0x1A0 {:.3f} +0x1A4 {:.3f} {:.3f} {:.3f} {:.3f}",
                 r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], *(float*)(*(uint8_t**)((uint8_t*)comp + 8) + 0x178),
                 *(float*)(*(uint8_t**)((uint8_t*)comp + 8) + 0x188), *(float*)(*(uint8_t**)((uint8_t*)comp + 8) + 0x18C), *(float*)(*(uint8_t**)((uint8_t*)comp + 8) + 0x190), *(float*)(*(uint8_t**)((uint8_t*)comp + 8) + 0x194),
                 *(float*)(*(uint8_t**)((uint8_t*)comp + 8) + 0x1A0), *(float*)(*(uint8_t**)((uint8_t*)comp + 8) + 0x1A4), *(float*)(*(uint8_t**)((uint8_t*)comp + 8) + 0x1A8), *(float*)(*(uint8_t**)((uint8_t*)comp + 8) + 0x1AC), *(float*)(*(uint8_t**)((uint8_t*)comp + 8) + 0x1B0));
        }
        return r;
    }
    //! idMat3 rows fwd/left/up from a forward direction, no roll.
    void AxisFromFwd(const float* fw, float* axis)
    {
        float f[3] = { fw[0], fw[1], fw[2] };
        const float n = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]); if (n < 1e-6f) return;
        for (float& v : f) v /= n;
        float l[3] = { -f[1], f[0], 0.f }; float ln = sqrtf(l[0] * l[0] + l[1] * l[1]);
        if (ln < 1e-3f) { l[0] = 0; l[1] = 1; ln = 1; }
        l[0] /= ln; l[1] /= ln;
        const float u[3] = { f[1] * l[2] - f[2] * l[1], f[2] * l[0] - f[0] * l[2], f[0] * l[1] - f[1] * l[0] };
        const float m[9] = { f[0], f[1], f[2], l[0], l[1], l[2], u[0], u[1], u[2] };
        memcpy(axis, m, 36);
    }
}

bool MeleeAim::Install()
{
    if (g_Installed) return true;
    if (!GameInfo::IsSupportedBuild()) return false;
    const uintptr_t base = GameInfo::Base();
    if (memcmp((void*)(base + kGetViewAxis), kAxisBytes, sizeof kAxisBytes) || memcmp((void*)(base + kGetViewOrigin), kOrgBytes, sizeof kOrgBytes))
    { LOGI("MeleeAim: the view accessors are not what they were measured to be; not installed"); return false; }
    uint8_t* block = AllocNear(base, 0x1000);
    if (!block) { LOGI("MeleeAim: no memory within 2 GB of the exe"); return false; }
    uint8_t* p = block;
    for (int i = 0; i < kNumSites; i++)
    {
        uint8_t* call = (uint8_t*)(base + kSites[i].rva);
        const uintptr_t target = base + (kSites[i].axis ? kGetViewAxis : kGetViewOrigin);
        if (call[0] != 0xE8 || (uintptr_t)(call + 5 + *(int32_t*)(call + 1)) != target)
        { LOGI("MeleeAim: site exe+0x{:X} is not a call to the accessor; skipped", kSites[i].rva); continue; }
        uint8_t* thunk = p; p = EmitThunk(p, i); p = (uint8_t*)(((uintptr_t)p + 15) & ~15ull);
        const int64_t rel = (int64_t)thunk - (int64_t)(call + 5);
        if (rel != (int32_t)rel) { LOGI("MeleeAim: thunk out of rel32 range"); continue; }
        DWORD old = 0;
        if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
        const int32_t r32 = (int32_t)rel;
        InterlockedExchange((volatile LONG*)(call + 1), r32);   // 4-byte store; the site never runs half-patched in practice (installed at init)
        VirtualProtect(call + 1, 4, old, &old);
        g_Patched++;
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_RealCone = (FnCone)(base + kConeTest);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_RealCone, ConeHook);
    g_RealSetActive = (FnSetActive)(base + kSetActive); g_RealHitCheck = (FnHitCheck)(base + kHitCheck);
    DetourAttach(&(PVOID&)g_RealSetActive, SetActiveHook);
    DetourAttach(&(PVOID&)g_RealHitCheck, HitCheckHook);
    g_RealSlot42 = (FnSlot42)(base + kSlot42);
    DetourAttach(&(PVOID&)g_RealSlot42, Slot42Hook);
    g_RealSlot41 = (FnSlot41)(base + kSlot41);
    DetourAttach(&(PVOID&)g_RealSlot41, Slot41Hook);
    g_RealValid = (FnValid)(base + 0x635B00); g_RealVeto = (FnVeto)(base + 0x62EB20);
    DetourAttach(&(PVOID&)g_RealValid, ValidHook);
    DetourAttach(&(PVOID&)g_RealVeto, VetoHook);
    g_RealGather = (FnGather)(base + kGather);
    DetourAttach(&(PVOID&)g_RealGather, GatherHook);
    g_RealStrike = (FnStrike)(base + kStrikePlayer);
    DetourAttach(&(PVOID&)g_RealStrike, StrikeHook);
    LOGI("MeleeAim: cone test exe+0x{:X} hooked: detour={}", kConeTest, DetourTransactionCommit());
    g_Installed = true;
    LOGI("MeleeAim: {} of {} melee call sites redirected (view axis -> right controller)", g_Patched, kNumSites);
    return true;
}

void MeleeAim::SetGestureAxis(const float axis[9], float seconds)
{
    std::lock_guard lk(g_GestM);
    memcpy(g_GestAxis, axis, 36);
    g_GestUntil = Qpc() + (long long)(seconds * (double)QpcFreq());
}
void MeleeAim::AddHitBox(const HitBox& b)
{
    const long long now = Qpc(), keep = (long long)(0.8 * (double)QpcFreq());
    std::lock_guard lk(g_BoxM);
    g_Boxes.erase(std::remove_if(g_Boxes.begin(), g_Boxes.end(), [&](const auto& e) { return now - e.second >= keep; }), g_Boxes.end());
    if (g_Boxes.size() < 16) g_Boxes.push_back({ b, now });
}
void MeleeAim::SetBladeDamage(bool on, float scale)
{
    g_Direct = on; g_DirectScale = scale < 0.1f ? 0.1f : (scale > 5.f ? 5.f : scale);
    LOGI("MeleeAim: {} (damage x{:.2f})", on ? "the blade strikes what it touches itself (SwordBladeDamage = 1): its damage scales with the blade's speed" : "the blade's contacts click the game's attack (SwordBladeDamage = 0)", g_DirectScale.load());
}
bool MeleeAim::BladeDamage() { return g_Direct.load(); }
unsigned MeleeAim::BladeStrike(void* ent, float bladeSpeed)
{
    // the game's own strike is x1; a blade at 4 m/s (a firm slash) is x1, a light 2 m/s touch x0.5, a hard 8 m/s swing x2
    const float mul = std::clamp(bladeSpeed / 4.f, 0.4f, 2.f) * g_DirectScale.load();
    std::lock_guard lk(g_DirM);
    g_Dir = DirReq{}; g_Dir.ent = ent; g_Dir.mul = mul; g_Dir.at = Qpc(); g_Dir.serial = ++g_DirSerial;
    g_DirAsked++;
    return g_Dir.serial;
}
int MeleeAim::BladeStrikeState(unsigned serial)
{
    if (!serial) return 0;
    if (g_DirLanded.load() == serial) return 2;
    if (g_DirDefended.load() == serial) return 4;
    if (g_DirDone.load() == serial) return 3;
    return g_DirSerial.load() == serial ? 1 : 3;
}
void MeleeAim::RequestOpen(float maxWait)
{
    const long long now = Qpc();
    g_OpenAskedAt = now;
    g_OpenUntil = now + (long long)(maxWait * (double)QpcFreq());
}
bool MeleeAim::SlashRunning() { return g_SlashRunning.load(); }
bool MeleeAim::HitThisSlash() { return g_HitThisSlash.load(); }
namespace { long long g_TrigAt = 0; }
void MeleeAim::NoteTrigger(bool held) { if (held) g_TrigAt = Qpc(); }
void MeleeAim::SetFilterMode(int m) { g_FilterMode = m; LOGI("MeleeAim: contact filter {}", m >= 2 ? "2 (a contact's Slash hits what the blade touched, cone or not)" : m ? "1 (what the blade touched, inside the 30 deg cone)" : "off"); }
int MeleeAim::WindowPhase() { return g_WindowPhase.load(); }
uint64_t MeleeAim::Accepted() { return g_Accepted.load(); }
unsigned MeleeAim::SlashSerial() { return g_SlashSerial.load(); }
uint64_t MeleeAim::Hits() { return g_Hits.load(); }
int MeleeAim::FilterMode() { return g_FilterMode.load(); }
void MeleeAim::SetBoxMargin(float m) { g_BoxMargin = m; }
void MeleeAim::SetBladeAim(int on) { g_BladeAim = on; LOGI("MeleeAim: trigger aim {}", on ? "blade/knuckles" : "controller forward"); }
void MeleeAim::SetContactSweep(const float point[3], const float vel[3])
{
    float v[3] = { vel[0], vel[1], vel[2] };
    const float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n < 1e-4f) return;
    for (float& x : v) x /= n;
    std::lock_guard lk(g_SweepM); memcpy(g_SweepP, point, 12); memcpy(g_SweepV, v, 12); g_SweepSpeed = n; g_SweepAt = Qpc();
}
void MeleeAim::SetContactVel(int on) { g_ContactVel = on; LOGI("MeleeAim: contact velocity {}", on); }
void MeleeAim::SetSweepMode(int m) { g_SweepMode = m; LOGI("MeleeAim: contact sweep mode {}", m); }
void MeleeAim::ShapeLog(int n) { g_Shape42Left = n; }
void MeleeAim::OpenAt(int ms) { g_OpenAtMs = ms; LOGI("MeleeAim: the next Slash opens its window {} ms in", ms); }
void MeleeAim::Trace(int n) { g_TraceLeft = n; LOGI("MeleeAim: tracing the next {} melee calls", n); }
void MeleeAim::ClearGestureAxis() { std::lock_guard lk(g_GestM); g_GestUntil = 0; }
void MeleeAim::FreezeAfterSlash(int ms) { g_FreezeAt = 0; g_FreezeMs = ms; }

void MeleeAim::SetEnabled(bool on) { g_On = on; }
bool MeleeAim::Enabled() { return g_On; }
void MeleeAim::SetOrigin(int m) { g_Origin = m ? 1 : 0; }
int MeleeAim::Origin() { return g_Origin; }

void MeleeAim::OnPresent()
{
    const bool slash = !strcmp(PlayerState::AnimName(), "Slash");
    static uint64_t gestStart = 0;
    if (slash && !g_InSlash) { g_SlashStart = g_AxisRedirects.load(); gestStart = g_GestRedirects.load(); g_Logged = false; }
    if (slash && !g_InSlash)
    {   // a new swing: one hit, and filtered unless the trigger started it
        g_SlashSerial++; g_HitThisSlash = false; g_OpenedEarly = false; g_WindowPhase = 0;
        const bool trig = g_TrigAt && (double)(Qpc() - g_TrigAt) / (double)QpcFreq() < 0.25;
        g_Filter = g_FilterMode.load() && !trig;
    }
    g_SlashRunning = slash;
    if (slash && !g_InSlash && g_OpenAtMs.load() >= 0) { g_OpenDue = Qpc() + (long long)(g_OpenAtMs.load() * 1e-3 * (double)QpcFreq()); LOGI("meleewin: Slash began; window forced open in {} ms", g_OpenAtMs.load()); g_OpenAtMs = -1; }
    {   // The sword's trail is hidden while the swing runs and while it fades out after it (GpuTrace kTrailHashes).
        // The fade is counted in SIMULATED time only: with the game paused or frozen the trail stays on screen just as
        // it was, and a wall-clock tail ran out under it (measured: the trail came back on a frozen frame 1.2 s later).
        static long long last = 0; static unsigned long long lastAim = 0; static double tail = 0;
        const long long now = Qpc(); const double dt = last ? (double)(now - last) / (double)QpcFreq() : 0.0; last = now;
        const unsigned long long aim = HandAimHook::AimTicks();
        const bool simulated = aim != lastAim; lastAim = aim;
        if (slash) tail = 0.8; else if (simulated) tail -= dt;
        GpuTrace::SetTrailWindow(slash || tail > 0.0);
    }
    if (g_FreezeMs.load() >= 0)
    {
        if (slash && !g_InSlash) g_FreezeAt = Qpc() + (long long)(g_FreezeMs.load() * 1e-3 * (double)QpcFreq());
        if (g_FreezeAt && Qpc() >= g_FreezeAt)
        {
            const bool ok = CVars::SetInt("g_stopTime", 1);
            LOGI("MeleeAim: slashfreeze: time frozen {} ms after the Slash began -> {}", g_FreezeMs.load(), ok);
            g_FreezeAt = 0; g_FreezeMs = -1;
        }
    }
    g_InSlash = slash;
    if (slash && !g_Logged && g_AxisRedirects.load() > g_SlashStart)
    {
        g_Logged = true;
        const float* h = g_LastHand; const float* v = g_LastView;
        auto yaw = [](const float* f) { return Deg(atan2f(f[1], f[0])); };
        auto pitch = [](const float* f) { return Deg(asinf(f[2] > 1 ? 1 : f[2] < -1 ? -1 : f[2])); };
        float dy = yaw(h) - yaw(v); while (dy > 180) dy -= 360; while (dy < -180) dy += 360;
        LOGI("MeleeAim: swing aimed by the {}: hand yaw {:.1f} pitch {:.1f}, view yaw {:.1f} pitch {:.1f} (hand - view: yaw {:+.1f} pitch {:+.1f})",
             g_GestRedirects.load() > gestStart ? "GESTURE" : "hand", yaw(h), pitch(h), yaw(v), pitch(v), dy, pitch(h) - pitch(v));
    }
}

std::string MeleeAim::Stats()
{
    std::ostringstream o;
    uint64_t calls = 0, red = 0;
    for (int i = 0; i < kNumSites; i++) { calls += g_Calls[i].load(); red += g_Redirected[i].load(); }
    o << (g_On.load() ? "on" : "off") << " origin " << (g_Origin.load() ? "hand" : "head") << " sites " << g_Patched
      << " calls " << calls << " redirected " << red << " gesture " << g_GestRedirects.load()
      << " | filter " << g_Filter.load() << " mode " << g_FilterMode.load() << " opens " << g_Opens.load() << " hits " << g_Hits.load() << " early " << g_HitsEarly.load()
      << " accepted " << g_Accepted.load() << " rejected " << g_Rejected.load() << " suppressed " << g_Suppressed.load() << " cones " << g_ConeCalls.load() << " byid " << g_ById.load() << " bybox " << g_ByBox.load() << " otherent " << g_OtherEnt.load() << " sweeps " << g_SweepsMoved.load() << " velset " << g_VelSet.load() << " bladeaim " << g_BladeAim.load() << " forced " << g_Forced.load() << " window " << g_WindowPhase.load()
      << " blade asked " << g_DirAsked.load() << " struck " << g_DirStruck.load() << " blocked " << g_DirBlocked.load() << " ignored " << g_DirIgnored.load()
      << " refused " << g_DirRefused.load() << " expired " << g_DirExpired.load();
    return o.str();
}

void MeleeAim::DumpSites()
{
    for (int i = 0; i < kNumSites; i++)
        LOGI("MeleeAim: site exe+0x{:X} {} calls {} redirected {}", kSites[i].rva, kSites[i].axis ? "axis" : "org ", g_Calls[i].load(), g_Redirected[i].load());
}
