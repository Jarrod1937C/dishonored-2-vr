#include "pch.h"
#include "GunRay.h"
#include "GameInfo.h"
#include "HandAimHook.h"
#include "GpuTrace.h"
#include "ViewMatrixHook.h"
#include "VRRuntime.h"
#include "Watchpoint.h"
#include "HandFx.h"
#include "CVars.h"
#include "PlayerState.h"
#include "BodyHide.h"
#include <detours/detours.h>
#include <cmath>
#include <sstream>
#include <set>

namespace
{
    constexpr float kR2D = 57.2957795f;
    // arkCpntWeaponRanged
    constexpr uint32_t kRangedInit = 0x639F00;       //!< the ranged base's init (slot 3 of Gun/Crossbow calls it first)
    constexpr uint32_t kWeaponAimPoint = 0x639670;   //!< idVec3* (weapon, out): GetAimPoint(provider #9, out, decl+0x198)
    constexpr uint32_t kVtGun = 0x1B53F58, kVtCrossbow = 0x1B9CBF8;
    constexpr uint32_t kCpntEntity = 0x10, kCpntDecl = 0x8, kSpawnPos = 0xF4, kDeclMaxRange = 0x198;
    constexpr uint32_t kEntOrigin = 0x300, kEntAxis = 0x30C;
    // arkCpntPlayerSensorRangedTarget (vtable exe+0x1B41D28)
    constexpr uint32_t kSensorSubmit = 0x3C3FD0, kSensorResults = 0x3C3E10;
    constexpr uint32_t kSensorOn = 0x38, kSensorCam = 0x40, kSensorAim = 0x48, kSensorHas = 0x54, kSensorPoint = 0x58;
    constexpr uint32_t kSensorDecl = 0x8, kDeclRange = 0x74, kAimWorldDir = 0x2C;
    constexpr size_t kFakeCam = 0x200, kFakeAim = 0x80;
    constexpr float kEyeBehind = 0.30f;               // the fire's "eye": this far back from the muzzle along the barrel

    std::atomic<int> g_Mode{ 63 }, g_Spread{ 0 }, g_LogLeft{ 0 };
    std::atomic<float> g_RingX{ -1.f }, g_RingY{ -1.f };   // where the reticle drew the gun ring (frame fractions)
    std::atomic<unsigned long long> g_Submits{ 0 }, g_Aimed{ 0 }, g_Results{ 0 }, g_Hits{ 0 }, g_AimSet{ 0 }, g_ShotsNew{ 0 }, g_ShotsOld{ 0 }, g_NoPose{ 0 };

    bool Readable(const void* p, size_t n)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    void Normalize(float* v) { const float n = sqrtf(Dot(v, v)); if (n > 1e-9f) for (int i = 0; i < 3; i++) v[i] /= n; }
    float AngleDeg(const float* a, const float* b)
    {
        float x[3] = { a[0], a[1], a[2] }, y[3] = { b[0], b[1], b[2] }; Normalize(x); Normalize(y);
        const float c = Dot(x, y); return acosf(c > 1.f ? 1.f : c < -1.f ? -1.f : c) * kR2D;
    }
    //! Distance from point p to the line (o, unit d).
    float LineDist(const float* p, const float* o, const float* d)
    {
        const float v[3] = { p[0] - o[0], p[1] - o[1], p[2] - o[2] };
        const float a = Dot(v, d);
        return sqrtf(fmaxf(Dot(v, v) - a * a, 0.f));
    }
    //! idMat3 rows * local -> world offset
    void RowsMul(const float* axis, const float* l, float* w)
    {
        for (int i = 0; i < 3; i++) w[i] = l[0] * axis[i] + l[1] * axis[3 + i] + l[2] * axis[6 + i];
    }

    // ---- which weapon component is in the hand (the muzzle joint lives on it) ----
    std::mutex g_RegM;
    void* g_Comps[32] = {}; int g_NComps = 0;
    void* g_LastShotWeapon[2] = {};
    bool IsGunImpl(void* w)
    {
        if (!w || !Readable(w, 0x200)) return false;
        const uintptr_t vt = *(uintptr_t*)w, base = GameInfo::Base();
        return vt == base + kVtGun || vt == base + kVtCrossbow;
    }
    void Register(void* w)
    {
        if (!IsGunImpl(w)) return;
        std::lock_guard lk(g_RegM);
        for (int i = 0; i < g_NComps; i++) if (g_Comps[i] == w) return;
        if (g_NComps < 32) g_Comps[g_NComps++] = w;
        else { memmove(g_Comps, g_Comps + 1, sizeof(void*) * 31); g_Comps[31] = w; }
    }
    //! The gun/crossbow component whose entity is `ent` (the hand's anchored equipment). Components are learned when
    //! they initialise and when they fire; a level load frees them, so each is re-validated on use.
    void* CompFor(void* ent)
    {
        if (!ent) return nullptr;
        std::lock_guard lk(g_RegM);
        for (int i = g_NComps - 1; i >= 0; i--)
            if (IsGunImpl(g_Comps[i]) && *(void**)((uint8_t*)g_Comps[i] + kCpntEntity) == ent) return g_Comps[i];
        return nullptr;
    }

    // ---- the gun ray ----
    struct Cache { bool valid = false; float sp[3] = {}; void* comp = nullptr; void* ent = nullptr; ULONGLONG at = 0; float flatGap = 0; };
    Cache g_Cache[2];        // under g_RegM
    std::atomic<void*> g_GunEnt[2] = {};     // the equipment entity of each hand that holds a gun (set by PoseImpl)
    //! The weapon placed for the controller as it is now: the controller anchored to where the NEXT view will be drawn
    //! from (the last view's composition offset carried onto the game eye of this instant), then the weapon grip --
    //! the same chain GpuTrace draws the weapon with. Then the muzzle joint of the component in that hand.
    bool PoseImpl(int hand, float muzzle[3], float axis[9], void* weapon = nullptr, float* entOrg = nullptr)
    {
        float eye[3], cax[9], vorg[3], gorg[3], hmd[3];
        if (!HandAimHook::RealCamera(eye, cax) || !ViewMatrixHook::ViewAnchor(vorg, gorg, hmd)) { g_NoPose++; return false; }
        const float o[3] = { vorg[0] + eye[0] - gorg[0], vorg[1] + eye[1] - gorg[1], vorg[2] + eye[2] - gorg[2] };
        float horg[3], hax[9], torg[3], tax[9]; void* ent = nullptr;
        if (!HandAimHook::ControllerPoseAt(hand, o, cax, hmd, horg, hax)) { g_NoPose++; return false; }
        if (!GpuTrace::WeaponFromHand(hand, horg, hax, torg, tax, &ent)) { g_NoPose++; return false; }
        void* comp = weapon ? weapon : CompFor(ent);
        if (!comp || !IsGunImpl(comp)) { g_NoPose++; return false; }
        float sp[3]; memcpy(sp, (uint8_t*)comp + kSpawnPos, 12);
        if (!std::isfinite(sp[0]) || !std::isfinite(sp[1]) || !std::isfinite(sp[2]) || Dot(sp, sp) > 4.f) { g_NoPose++; return false; }
        float off[3]; RowsMul(tax, sp, off);
        for (int i = 0; i < 3; i++) muzzle[i] = torg[i] + off[i];
        memcpy(axis, tax, 36);
        if (entOrg) memcpy(entOrg, torg, 12);
        {
            std::lock_guard lk(g_RegM);
            Cache& c = g_Cache[hand & 1];
            c.valid = true; memcpy(c.sp, sp, 12); c.comp = comp; c.ent = ent; c.at = GetTickCount64();
        }
        g_GunEnt[hand & 1] = *(void**)((uint8_t*)comp + kCpntEntity) == ent ? ent : nullptr;
        return true;
    }

    // ---- the engine's aim sensor, aimed along the gun ray ----
    std::mutex g_SensM;                       // the swap of the sensor's camera / aim pointers must not nest
    alignas(16) uint8_t g_FakeCam[kFakeCam], g_FakeAim[kFakeAim];
    std::atomic<uint32_t> g_SensorMask{ 0 }, g_SensorFlags{ 0 };
    // gunpix: for a few frames the sensor traces the eye's ray through a screen point instead of the gun ray, so the
    // test target is found by the same query (dynamic props included) that the shot is judged by
    struct Probe { int frames = 0; float o[3] = {}, d[3] = {}; float fx = 0, fy = 0; } g_Probe;   // under g_SensM
    struct Target { bool valid = false; float p[3] = {}; } g_Target;                              // under g_SensM
    struct Sub { bool valid = false, probe = false; float o[3] = {}, f[3] = {}; float range = 0; ULONGLONG at = 0; } g_Sub;
    struct Hit { bool valid = false, has = false; float t = 0; float point[3] = {}; float o[3] = {}, f[3] = {}; ULONGLONG at = 0; } g_Hit;
    using FnThis = void* (*)(void*);
    FnThis g_RealSubmit = nullptr, g_RealResults = nullptr;

    void* SubmitHook(void* self)
    {
        g_Submits++;
        uint8_t* s = (uint8_t*)self;
        if (!(g_Mode.load() & 1) || HandAimHook::Mode() < 2 || !Readable(s, 0x80) || !s[kSensorOn]) { std::lock_guard lk(g_SensM); g_Sub.valid = false; return g_RealSubmit(self); }
        float m[3], ax[9];
        const bool pose = PoseImpl(HandAimHook::GunHand(), m, ax);
        std::lock_guard lk(g_SensM);
        const bool probe = g_Probe.frames > 0;
        if (probe) { g_Probe.frames--; memcpy(m, g_Probe.o, 12); memcpy(ax, g_Probe.d, 12); }
        else if (!pose) { g_Sub.valid = false; return g_RealSubmit(self); }
        uint8_t** camSlot = (uint8_t**)(s + kSensorCam); uint8_t** aimSlot = (uint8_t**)(s + kSensorAim);
        uint8_t* cam = *camSlot; uint8_t* aim = *aimSlot; uint8_t* decl = *(uint8_t**)(s + kSensorDecl);
        if (cam == g_FakeCam || !Readable(cam, kFakeCam) || !Readable(aim, kFakeAim) || !Readable(decl, kDeclRange + 4)) { g_Sub.valid = false; return g_RealSubmit(self); }
        // Copies of the camera and aim component with the gun ray in the two fields the submit reads (the eye through
        // exe+0x417820 = cam+0xD8, the direction at aim+0x2C); the real ones -- read by other sensors on other threads
        // -- are never written.
        memcpy(g_FakeCam, cam, kFakeCam); memcpy(g_FakeCam + HandAimHook::kCamOrigin, m, 12);
        memcpy(g_FakeAim, aim, kFakeAim); memcpy(g_FakeAim + kAimWorldDir, ax, 12);
        *camSlot = g_FakeCam; *aimSlot = g_FakeAim;
        void* r = g_RealSubmit(self);
        *camSlot = cam; *aimSlot = aim;
        g_SensorMask = *(uint32_t*)(decl + 0x70); g_SensorFlags = s[0x80];
        g_Sub.valid = true; g_Sub.probe = probe; memcpy(g_Sub.o, m, 12); memcpy(g_Sub.f, ax, 12); g_Sub.range = *(float*)(decl + kDeclRange); g_Sub.at = GetTickCount64();
        g_Aimed++;
        return r;
    }
    void* ResultsHook(void* self)
    {
        g_Results++;
        uint8_t* s = (uint8_t*)self;
        std::unique_lock lk(g_SensM);
        if (!(g_Mode.load() & 1) || !g_Sub.valid || !Readable(s, 0x80))
        {
            lk.unlock();
            return g_RealResults(self);
        }
        uint8_t** camSlot = (uint8_t**)(s + kSensorCam);
        uint8_t* cam = *camSlot;
        if (cam == g_FakeCam || !Readable(cam, kFakeCam)) { lk.unlock(); return g_RealResults(self); }
        // the results phase reads the eye only to store the hit's distance: give it the ray's origin
        memcpy(g_FakeCam, cam, kFakeCam); memcpy(g_FakeCam + HandAimHook::kCamOrigin, g_Sub.o, 12);
        *camSlot = g_FakeCam;
        void* r = g_RealResults(self);
        *camSlot = cam;
        Hit h; h.valid = true; h.has = s[kSensorHas] != 0; memcpy(h.o, g_Sub.o, 12); memcpy(h.f, g_Sub.f, 12); h.at = GetTickCount64();
        if (h.has)
        {
            memcpy(h.point, s + kSensorPoint, 12);
            const float d[3] = { h.point[0] - h.o[0], h.point[1] - h.o[1], h.point[2] - h.o[2] };
            h.t = Dot(d, h.f);
            g_Hits++;
        }
        else
        {
            h.t = g_Sub.range;
            for (int i = 0; i < 3; i++) h.point[i] = h.o[i] + h.f[i] * h.t;
        }
        const bool probe = g_Sub.probe;
        if (!probe) g_Hit = h;
        else if (h.has) { g_Target.valid = true; memcpy(g_Target.p, h.point, 12); }
        const Probe pr = g_Probe;
        lk.unlock();
        if (probe)
        {
            float m[3], ax[9];
            if (!h.has) LOGI("gunpix: {:.3f} {:.3f}: the sensor hit nothing along the eye ray", pr.fx, pr.fy);
            else if (!PoseImpl(HandAimHook::GunHand(), m, ax)) LOGI("gunpix: target ({:.3f},{:.3f},{:.3f}) {:.2f} m from the eye; no gun pose", h.point[0], h.point[1], h.point[2], h.t);
            else
            {
                float d[3] = { h.point[0] - m[0], h.point[1] - m[1], h.point[2] - m[2] };
                const float dist = sqrtf(Dot(d, d)); Normalize(d);
                const float yawG = atan2f(ax[1], ax[0]) * kR2D, pitchG = atan2f(ax[2], sqrtf(ax[0] * ax[0] + ax[1] * ax[1])) * kR2D;
                const float yawT = atan2f(d[1], d[0]) * kR2D, pitchT = atan2f(d[2], sqrtf(d[0] * d[0] + d[1] * d[1])) * kR2D;
                float dy = yawT - yawG; while (dy > 180.f) dy -= 360.f; while (dy < -180.f) dy += 360.f;
                LOGI("gunpix: {:.3f} {:.3f} -> target ({:.3f},{:.3f},{:.3f}) {:.2f} m from the eye, {:.2f} m from the muzzle; gun ray off by {:.2f} cm; turn yaw {:+.2f} pitch {:+.2f} deg",
                     pr.fx, pr.fy, h.point[0], h.point[1], h.point[2], h.t, dist, LineDist(h.point, m, ax) * 100.f, dy, pitchT - pitchG);
            }
            return r;
        }
        if (g_LogLeft.load() > 0)
        {
            g_LogLeft--;
            LOGI("GunRay: sensor ray ({:.3f},{:.3f},{:.3f}) fwd ({:.3f},{:.3f},{:.3f}) range {:.1f} -> {} t {:.3f} at ({:.3f},{:.3f},{:.3f})",
                 h.o[0], h.o[1], h.o[2], h.f[0], h.f[1], h.f[2], g_Sub.range, h.has ? "HIT" : "nothing", h.t, h.point[0], h.point[1], h.point[2]);
        }
        return r;
    }
    // ---- where the gun ray meets the world: the projectile's own sweep ----
    // The aim sensor only reports TARGETS (characters): against walls, tables and bottles it says nothing (measured: 0
    // hits in 1885 aimed frames on the boat). The engine checks a shot's path with exe+0x638800(weapon, from, to,
    // mask, float* dist): a sweep of the weapon's projectile shape (weapon+0x118) -- what ComputeAimTarget uses to see
    // whether the muzzle -> aim path is clear, mask 0x20000. Returns true when clear; on a hit writes the distance.
    // Run once a frame on the game thread (GunRay::Tick from the camera's aim update), as the engine itself runs it.
    constexpr uint32_t kSweep = 0x638800, kTraceRay = 0x638570, kProjShape = 0x118;
    using FnSweep = bool (*)(void*, const float*, const float*, uint32_t, float*);
    std::atomic<uint32_t> g_SweepMask{ 0x20000 };
    std::atomic<float> g_MaxRange{ 100.f };
    struct Trace { bool valid = false, hit = false, sensor = false; float t = 0; float o[3] = {}, f[3] = {}; ULONGLONG at = 0;
                   float sweepT = 0; bool sweepHit = false; } g_Trace;   // under g_SensM
    std::atomic<unsigned long long> g_Sweeps{ 0 }, g_SweepHits{ 0 }, g_SensorUsed{ 0 };
    float WeaponRange(void* w)
    {
        uint8_t* decl = *(uint8_t**)((uint8_t*)w + kCpntDecl);
        float r = Readable(decl, kDeclMaxRange + 4) ? *(float*)(decl + kDeclMaxRange) : 30.f;
        if (!std::isfinite(r) || r < 1.f) r = 30.f;
        return fminf(r, g_MaxRange.load());
    }
    //! false = no answer; hit=false with t = range when the path is clear.
    bool SweepImpl(void* w, const float* o, const float* f, float range, uint32_t mask, bool shape, float& t, bool& hit)
    {
        if (!IsGunImpl(w)) return false;
        // A weapon without a projectile shape (the pistol: weapon+0x118 is null) cannot be swept -- exe+0x638800
        // dereferences it (measured: a null read inside it the moment the pistol was drawn; ComputeAimTarget itself
        // skips its sweep when it is null). Its path is a ray, which is what it fires anyway.
        if (shape && !*(void**)((uint8_t*)w + kProjShape)) shape = false;
        const float to[3] = { o[0] + f[0] * range, o[1] + f[1] * range, o[2] + f[2] * range };
        float d = -1.f;
        const bool clear = shape ? ((FnSweep)(GameInfo::Base() + kSweep))(w, o, to, mask, &d)
                                 : ((FnSweep)(GameInfo::Base() + kTraceRay))(w, o, to, mask, &d);
        hit = !clear; t = hit ? d : range;
        return std::isfinite(t) && t >= 0.f;
    }
    bool SensorHitImpl(float& t, bool& hit)
    {
        std::lock_guard lk(g_SensM);
        const ULONGLONG now = GetTickCount64();
        if (g_Trace.valid && now - g_Trace.at <= 300) { t = g_Trace.t; hit = g_Trace.hit; return true; }
        if (!g_Hit.valid || now - g_Hit.at > 500) return false;
        t = g_Hit.t; hit = g_Hit.has;
        return std::isfinite(t) && t > 0.f;
    }

    // ---- the aim point: on the gun ray ----
    using FnAim = float* (*)(void*, float*);
    FnAim g_RealWeaponAim = nullptr;
    struct LastAim { void* w = nullptr; float out[3] = {}, eye[3] = {}; ULONGLONG at = 0; } g_LastAim;   // game thread
    float* WeaponAimHook(void* w, float* out)
    {
        float* r = g_RealWeaponAim(w, out);
        Register(w);
        if (!IsGunImpl(w)) return r;
        {   // what the spread step will be handed (overwritten below when the gun ray takes the aim)
            float cax[9];
            g_LastAim.w = w; memcpy(g_LastAim.out, out, 12); g_LastAim.at = GetTickCount64();
            if (!HandAimHook::RealCamera(g_LastAim.eye, cax)) g_LastAim.w = nullptr;
        }
        if (!(g_Mode.load() & 2) || HandAimHook::Mode() < 2) return r;
        float m[3], ax[9];
        if (!PoseImpl(HandAimHook::GunHand(), m, ax, w)) return r;
        float t = 0; bool hit = false;
        if (!SensorHitImpl(t, hit))
        {
            uint8_t* decl = *(uint8_t**)((uint8_t*)w + kCpntDecl);
            t = Readable(decl, kDeclMaxRange + 4) ? *(float*)(decl + kDeclMaxRange) : 30.f;
            if (!std::isfinite(t) || t < 1.f) t = 30.f;
        }
        for (int i = 0; i < 3; i++) out[i] = m[i] + ax[i] * t;
        memcpy(g_LastAim.out, out, 12);
        g_AimSet++;
        return r;
    }

    FnThis g_RealInit = nullptr;
    void* InitHook(void* self) { void* r = g_RealInit(self); Register(self); return r; }

    // ---- what the shot really did: the projectile after the launch, and the fire's target lock ----
    // exe+0x639970 Launch(weapon, projectile entity, &spawn, &axis, speedScale, targetEnt, targetJoint, int, int): sets
    // the entity's origin/axis and velocity (axis row 0 * the projectile's speed * speedScale).
    // exe+0x63A8B0 FireTarget(weapon, projectile, &targetEnt, &targetJoint) -> bool: before the aim, the gun asks the
    // player's target sensors (provider 16, provider 9) for an entity to hand the projectile.
    constexpr uint32_t kLaunch = 0x639970, kFireTarget = 0x63A8B0;
    using FnLaunch = void* (*)(void*, void*, const float*, const float*, float, void*, void*, int, int);
    using FnFireTarget = bool (*)(void*, void*, void**, void**);
    FnLaunch g_RealLaunch = nullptr;
    FnFireTarget g_RealFireTarget = nullptr;
    std::atomic<int> g_Track{ 0 }, g_NoLock{ 0 }, g_ShotBurst{ 0 }, g_ShotBurstN{ 0 }, g_ShotBurstStride{ 1 }, g_FreezeMs{ -1 };
    std::atomic<ULONGLONG> g_FreezeAt{ 0 };
    struct Flight
    {
        uint8_t* ent = nullptr; uintptr_t vt = 0; int frames = 0, still = 0; bool done = true;
        float spawn[3] = {}, dir[3] = {}, last[3] = {}, gunO[3] = {}, gunF[3] = {}, eye[3] = {}, look[3] = {};
        float path = 0; ULONGLONG at = 0;
    };
    std::mutex g_FlightM;
    Flight g_Flights[8];                      // under g_FlightM
    int g_NextFlight = 0;
    //! Follow an entity the shot put in the world (the projectile, the muzzle entity): where it goes, frame by frame.
    void StartFlight(void* w, uint8_t* ent, const char* what)
    {
        if (!Readable(ent, 0x350)) return;
        std::lock_guard lk(g_FlightM);
        for (const Flight& f : g_Flights) if (!f.done && f.ent == ent) return;
        Flight& f = g_Flights[g_NextFlight++ & 7];
        f = Flight(); f.ent = ent; f.vt = *(uintptr_t*)ent; f.done = false; f.at = GetTickCount64();
        memcpy(f.spawn, ent + kEntOrigin, 12); memcpy(f.dir, ent + kEntAxis, 12); memcpy(f.last, f.spawn, 12);
        float cax[9]; if (HandAimHook::RealCamera(f.eye, cax)) memcpy(f.look, cax, 12);
        float m[3], ax[9]; if (PoseImpl(HandAimHook::GunHand(), m, ax, w)) { memcpy(f.gunO, m, 12); memcpy(f.gunF, ax, 12); }
        const float* v = (const float*)(ent + 0x33C);
        const float dm[3] = { f.spawn[0] - f.gunO[0], f.spawn[1] - f.gunO[1], f.spawn[2] - f.gunO[2] };
        const bool moving = Dot(v, v) > 1e-6f;
        LOGI("GunFlight: {} ent {} (vt exe+{:#x}) at ({:.3f},{:.3f},{:.3f}) ax0 ({:.3f},{:.3f},{:.3f}) vel ({:.2f},{:.2f},{:.2f}) | axis vs gun {:.1f} deg, vs look {:.1f} deg; vel vs gun {:.1f}, vs look {:.1f}; {:.3f} m from the muzzle",
             what, (void*)ent, f.vt - GameInfo::Base(), f.spawn[0], f.spawn[1], f.spawn[2], f.dir[0], f.dir[1], f.dir[2], v[0], v[1], v[2], AngleDeg(f.dir, f.gunF), AngleDeg(f.dir, f.look),
             moving ? AngleDeg(v, f.gunF) : -1.f, moving ? AngleDeg(v, f.look) : -1.f, sqrtf(Dot(dm, dm)));
    }
    void* LaunchHook(void* w, void* ent, const float* spawn, const float* axis, float speed, void* tgt, void* joint, int a, int b)
    {
        void* r = g_RealLaunch(w, ent, spawn, axis, speed, tgt, joint, a, b);
        if (IsGunImpl(w)) if (int n = g_ShotBurst.exchange(0)) VRRuntime::RequestBurst(n, fmt::format("gunshot{}", ++g_ShotBurstN), g_ShotBurstStride.load());
        if (IsGunImpl(w)) { const int ms = g_FreezeMs.exchange(-1); if (ms >= 0) g_FreezeAt = GetTickCount64() + (ULONGLONG)ms; }
        if (!g_Track.load() || !IsGunImpl(w) || !Readable(ent, 0x350)) return r;
        LOGI("GunFlight: launch {} speed x{:.2f} target {} joint {} ({} {}) at ({:.3f},{:.3f},{:.3f}) dir ({:.3f},{:.3f},{:.3f})", w, speed, tgt, joint, a, b, spawn[0], spawn[1], spawn[2], axis[0], axis[1], axis[2]);
        StartFlight(w, (uint8_t*)ent, "projectile");
        return r;
    }
    bool FireTargetHook(void* w, void* proj, void** tgt, void** joint)
    {
        bool r = g_RealFireTarget(w, proj, tgt, joint);
        if (!g_Track.load() && !g_NoLock.load()) return r;
        uint8_t* own = Readable(w, 0x50) ? *(uint8_t**)((uint8_t*)w + 0x40) : nullptr;
        uint8_t* p16 = own && Readable(own, 0xA8) ? *(uint8_t**)(own + 0x20 + 16 * 8) : nullptr;
        uint8_t* p9 = own && Readable(own, 0xA8) ? *(uint8_t**)(own + 0x20 + 9 * 8) : nullptr;
        std::string s16 = "-", s9 = "-";
        if (Readable(p16, 0x98))
            s16 = fmt::format("vt exe+{:#x} on {} has {} {} ent {:#x} joint {} id {:#x} pt ({:.3f},{:.3f},{:.3f})", *(uintptr_t*)p16 - GameInfo::Base(), p16[0x38], p16[0x8C], p16[0x90],
                              *(uint32_t*)(p16 + 0x68), *(void**)(p16 + 0x70), *(uint32_t*)(p16 + 0x78), *(float*)(p16 + 0x7C), *(float*)(p16 + 0x80), *(float*)(p16 + 0x84));
        if (Readable(p9, 0x98))
            s9 = fmt::format("vt exe+{:#x} has {} ent {:#x} {:#x} pt ({:.3f},{:.3f},{:.3f})", *(uintptr_t*)p9 - GameInfo::Base(), p9[kSensorHas],
                             *(uint32_t*)(p9 + 0x78), *(uint32_t*)(p9 + 0x7C), *(float*)(p9 + kSensorPoint), *(float*)(p9 + kSensorPoint + 4), *(float*)(p9 + kSensorPoint + 8));
        LOGI("GunFlight: fire target -> {} ent {} joint {} | p16 {} | p9 {}{}", r, tgt ? *tgt : nullptr, joint ? *joint : nullptr, s16, s9, g_NoLock.load() ? " | DROPPED (vr gunnolock)" : "");
        if (g_NoLock.load() && IsGunImpl(w)) { if (tgt) *tgt = nullptr; if (joint) *joint = nullptr; return false; }
        return r;
    }
    // ---- the pistol's blast: an area hit placed from the CAMERA ----
    // exe+0x44F030(gun, area decl): before the bullet, the gun's shoot (exe+0x44E0C0, when its settings +0x98 -> +0x4A)
    // fires its m_spreadArea (decl +0x238, reduced +0x240, upgraded +0x248) through the game's area-hit call (game
    // vtable +0x3A0) at  eye (0x417820(this+0x48) = cam+0xD8) + rows(0x8067C0 = cam+0xFC) * m_spreadAreaOffset
    // (decl +0x250), facing the camera's forward. In VR that is the HEAD: aiming 90 deg to the side broke the bottle in
    // front of the eyes while the bullet flew along the barrel. For the call, the gun's camera pointer (this+0x48) reads
    // a copy of the camera with the gun ray's eye and barrel axis -- the real camera is not written.
    constexpr uint32_t kGunBlast = 0x44F030, kCpntCam = 0x48, kGunDeclBlastOffset = 0x250;
    using FnBlast = void (*)(void*, void*);
    FnBlast g_RealBlast = nullptr;
    alignas(16) uint8_t g_BlastCam[kFakeCam];
    std::atomic<unsigned long long> g_Blasts{ 0 }, g_BlastsMoved{ 0 };
    void BlastHook(void* w, void* area)
    {
        g_Blasts++;
        uint8_t* cam = Readable(w, 0x50) ? *(uint8_t**)((uint8_t*)w + kCpntCam) : nullptr;
        uint8_t* decl = Readable(w, 0x50) ? *(uint8_t**)((uint8_t*)w + kCpntDecl) : nullptr;
        if (!area || !IsGunImpl(w) || !Readable(cam, kFakeCam) || !Readable(decl, kGunDeclBlastOffset + 12)) { g_RealBlast(w, area); return; }
        const float* off = (const float*)(decl + kGunDeclBlastOffset);
        const float* eye = (const float*)(cam + HandAimHook::kCamOrigin); const float* ax = (const float*)(cam + HandAimHook::kCamAxis);
        float head[3]; RowsMul(ax, off, head); for (int i = 0; i < 3; i++) head[i] += eye[i];
        float m[3], gax[9];
        const bool move = (g_Mode.load() & 16) && HandAimHook::Mode() >= 2 && PoseImpl(HandAimHook::GunHand(), m, gax, w);
        if (!move)
        {
            LOGI("GunBlast: area {} offset ({:.2f},{:.2f},{:.2f}) at the HEAD: centre ({:.3f},{:.3f},{:.3f}) fwd ({:.3f},{:.3f},{:.3f}) (not moved)", area, off[0], off[1], off[2], head[0], head[1], head[2], ax[0], ax[1], ax[2]);
            g_RealBlast(w, area); return;
        }
        // The blast is a cone (pistol: 30 deg, damage radius 4.5 m, nothing within its 1 m safe radius) whose apex the
        // offset puts 0.75 m BEHIND the eye: in the flat game it covers from just in front of the face to ~3.7 m out,
        // i.e. it starts a little behind the muzzle. Keep exactly that geometry relative to the muzzle: the gun's "eye" is
        // as far behind the drawn muzzle, along the barrel, as the flat game's muzzle (the weapon entity * the muzzle
        // joint -- the entity still holds the flat pose) is ahead of the head's eye, measured on this very shot.
        float ahead = kEyeBehind;
        {
            uint8_t* ent = *(uint8_t**)((uint8_t*)w + kCpntEntity);
            if (Readable(ent + kEntOrigin, 48))
            {
                float fm[3], sp[3]; memcpy(sp, (uint8_t*)w + kSpawnPos, 12); RowsMul((const float*)(ent + kEntAxis), sp, fm);
                for (int i = 0; i < 3; i++) fm[i] += ((const float*)(ent + kEntOrigin))[i] - eye[i];
                const float a = Dot(fm, ax);
                if (std::isfinite(a)) ahead = fminf(fmaxf(a, 0.1f), 1.0f);
            }
        }
        float geye[3]; for (int i = 0; i < 3; i++) geye[i] = m[i] - gax[i] * ahead;
        memcpy(g_BlastCam, cam, kFakeCam);
        memcpy(g_BlastCam + HandAimHook::kCamOrigin, geye, 12); memcpy(g_BlastCam + HandAimHook::kCamAxis, gax, 36);
        float gun[3]; RowsMul(gax, off, gun); for (int i = 0; i < 3; i++) gun[i] += geye[i];
        uint8_t** slot = (uint8_t**)((uint8_t*)w + kCpntCam);
        *slot = g_BlastCam;
        g_RealBlast(w, area);
        *slot = cam;
        g_BlastsMoved++;
        float dh[3] = { gun[0] - head[0], gun[1] - head[1], gun[2] - head[2] };
        LOGI("GunBlast: area {} offset ({:.2f},{:.2f},{:.2f}) moved to the GUN (eye {:.2f} m behind the muzzle, as the flat muzzle is ahead of the eye): centre ({:.3f},{:.3f},{:.3f}) fwd ({:.3f},{:.3f},{:.3f}) | the head's would be ({:.3f},{:.3f},{:.3f}) fwd ({:.3f},{:.3f},{:.3f}), {:.2f} m / {:.1f} deg away",
             area, off[0], off[1], off[2], ahead, gun[0], gun[1], gun[2], gax[0], gax[1], gax[2], head[0], head[1], head[2], ax[0], ax[1], ax[2], sqrtf(Dot(dh, dh)), AngleDeg(gax, ax));
    }

    // ---- everything the engine places ON the weapon: on the weapon as DRAWN ----
    // The weapon ENTITY keeps the flat game's pose beside the head (the weapon's draws are identified by it, so it is
    // never moved). Whatever the engine hangs off the weapon asks for one of its joints in WORLD space through
    // exe+0x53D800(component, space 3, joint, offPos, offAxis, &pos, &axis) -- the model-space query exe+0x53D3F0 (HandFx's)
    // times the render model's world transform (+0x1860), which is the entity's -- and got that flat pose:
    //   - the muzzle flash / smoke / sparks FX (gun decl m_muzzleAttachedFx +0x230, spawned by exe+0x44EE80 on the
    //     m_bulletFiredTag joint, this+0xD4): bound, re-placed every frame by exe+0x4E3090 through exe+0x53DAC0;
    //   - the muzzle ENTITY exe+0x63B3C0 spawns at the same joint on every shot (an idEntityFx carrying
    //     muzzle_1p_emily.fx: the big flash, its light, the sparks, and the wad that drops to the floor);
    //   - entities bound to a weapon joint, placed every frame by the bind transform exe+0x7506D0: the paper cartridge
    //     (m_bulletAttachmentTag decl +0x220, entity at gun+0x158) the reload shows hanging in the air beside the head;
    // so all of it burst out in front of the face, along the view, whatever the hand did. Now a world-space query on a
    // held gun's entity answers with the pose that joint has on the weapon as DRAWN: the rigid transform that takes the
    // entity onto the drawn weapon (GpuTrace's chain, `PoseImpl`) applied to the engine's own answer. Nothing is written
    // to the entity, and the engine's model-space queries are untouched.
    constexpr uint32_t kFxUpdate = 0x4E3090, kJointWorld = 0x53D800, kEntId = 0xA8, kCpntOwner = 0x10, kEntModel = 0xF0;
    using FnFxUpdate = void (*)(void*, const float*, const float*, const float*, const float*, const float*, const float*);
    using FnJointWorld = bool (*)(void*, int, int, const float*, const float*, float*, float*);
    FnFxUpdate g_RealFxUpdate = nullptr;
    FnJointWorld g_RealJointWorld = nullptr;
    std::atomic<unsigned long long> g_JointMoved{ 0 }, g_FxMoved{ 0 }, g_JointBoundLeft{ 0 };
    std::atomic<float> g_JointLastGap{ 0.f };
    thread_local int t_InShoot = 0;
    std::atomic<int> g_JointDiag{ 0 };
    //! The gun/crossbow in either hand whose equipment entity is `ent` (or, with ent null, has the id `id`), and that hand.
    void* GunByEntity(const void* ent, uint32_t id, int& hand, uint8_t*& e)
    {
        std::lock_guard lk(g_RegM);
        for (int h = 0; h < 2; h++)
        {
            const Cache& c = g_Cache[h];
            if (!c.valid || !c.ent || GetTickCount64() - c.at > 2000 || !IsGunImpl(c.comp)) continue;
            uint8_t* ce = (uint8_t*)c.ent;
            if (*(void**)((uint8_t*)c.comp + kCpntEntity) != ce) continue;
            if (ent ? ce != ent : (!Readable(ce + kEntId, 4) || *(uint32_t*)(ce + kEntId) != id)) continue;
            hand = h; e = ce; return c.comp;
        }
        return nullptr;
    }
    //! A world pose on the weapon entity (as the engine has it), carried onto the weapon as drawn for that hand.
    bool OntoDrawn(int hand, void* comp, const uint8_t* ent, const float* p, const float* a, float* po, float* ao)
    {
        float m[3], dax[9], dorg[3];
        if (!PoseImpl(hand, m, dax, comp, dorg)) return false;
        const float* eo = (const float*)(ent + kEntOrigin); const float* ea = (const float*)(ent + kEntAxis);
        const float d[3] = { p[0] - eo[0], p[1] - eo[1], p[2] - eo[2] };
        const float l[3] = { Dot(d, ea), Dot(d, ea + 3), Dot(d, ea + 6) };   // in the entity's frame
        float w[3]; RowsMul(dax, l, w);
        for (int i = 0; i < 3; i++) po[i] = dorg[i] + w[i];
        for (int r = 0; r < 3; r++)
        {
            const float lr[3] = { Dot(a + 3 * r, ea), Dot(a + 3 * r, ea + 3), Dot(a + 3 * r, ea + 6) };
            RowsMul(dax, lr, ao + 3 * r);
        }
        return true;
    }
    // ---- the SWORD's joints (SwordContact research, the development notes): the melee weapon's world-space joint queries ----
    std::atomic<int> g_SwordJoints{ 0 };       // 1 = answer the right-hand sword entity's world joint queries as drawn
    std::atomic<int> g_SwordJointLog{ 0 };     // research: log the next n sword joint queries
    std::atomic<unsigned long long> g_SwordJointQ{ 0 }, g_SwordJointMoved{ 0 };
    //! The drawn pose of a hand's equipment ENTITY for the controller as it is now (PoseImpl's chain, no gun needed).
    bool DrawnEntity(int hand, float torg[3], float tax[9], void** ent)
    {
        float eye[3], cax[9], vorg[3], gorg[3], hmd[3];
        if (!HandAimHook::RealCamera(eye, cax) || !ViewMatrixHook::ViewAnchor(vorg, gorg, hmd)) return false;
        const float o[3] = { vorg[0] + eye[0] - gorg[0], vorg[1] + eye[1] - gorg[1], vorg[2] + eye[2] - gorg[2] };
        float horg[3], hax[9];
        if (!HandAimHook::ControllerPoseAt(hand, o, cax, hmd, horg, hax)) return false;
        return GpuTrace::WeaponFromHand(hand, horg, hax, torg, tax, ent);
    }
    bool SwordJoint(void* cp, int joint, float* pos, float* axis)
    {
        void* owner = *(void**)((uint8_t*)cp + kCpntOwner);
        void* sword = BodyHide::RightWeaponEntity();
        if (!owner || owner != sword) return false;
        g_SwordJointQ++;
        if (g_SwordJointLog.load() > 0)
        {
            g_SwordJointLog--;
            LOGI("GunRay: SWORD joint query cp {} joint {} from exe+{:#x} pos ({:.3f},{:.3f},{:.3f}) slash {}", cp, joint, (uintptr_t)_ReturnAddress() - GameInfo::Base(), pos[0], pos[1], pos[2], PlayerState::AnimName());
        }
        if (!g_SwordJoints.load() || HandAimHook::Mode() < 2) return false;
        float torg[3], tax[9]; void* ent = nullptr;
        if (!DrawnEntity(1, torg, tax, &ent) || ent != owner) return false;
        const uint8_t* e = (const uint8_t*)owner;
        const float* eo = (const float*)(e + kEntOrigin); const float* ea = (const float*)(e + kEntAxis);
        const float d[3] = { pos[0] - eo[0], pos[1] - eo[1], pos[2] - eo[2] };
        const float l[3] = { Dot(d, ea), Dot(d, ea + 3), Dot(d, ea + 6) };
        float w[3]; RowsMul(tax, l, w);
        float na[9];
        for (int r = 0; r < 3; r++) { const float lr[3] = { Dot(axis + 3 * r, ea), Dot(axis + 3 * r, ea + 3), Dot(axis + 3 * r, ea + 6) }; RowsMul(tax, lr, na + 3 * r); }
        for (int i = 0; i < 3; i++) pos[i] = torg[i] + w[i];
        memcpy(axis, na, 36);
        g_SwordJointMoved++;
        return true;
    }
    bool JointWorldHook(void* cp, int space, int joint, const float* offPos, const float* offAxis, float* pos, float* axis)
    {
        const bool r = g_RealJointWorld(cp, space, joint, offPos, offAxis, pos, axis);
        if (r && space == 3 && pos && axis && cp && SwordJoint(cp, joint, pos, axis)) return r;
        if (!r || space != 3 || !pos || !axis || !cp || !(g_Mode.load() & 32)) return r;
        void* owner = *(void**)((uint8_t*)cp + kCpntOwner);
        if (t_InShoot && g_JointDiag.load() > 0)
        {
            g_JointDiag--;
            LOGI("GunRay: joint query in the shot: component {} owner {} joint {} | gun entities L {} R {}", cp, owner, joint, g_GunEnt[0].load(), g_GunEnt[1].load());
        }
        if (!owner || (owner != g_GunEnt[0].load() && owner != g_GunEnt[1].load()) || HandAimHook::Mode() < 2) return r;
        // A MODEL bound to a weapon joint (the cartridge, gun+0x158): its draws sit by the flat weapon entity, so
        // GpuTrace claims them as the weapon's and gives them the weapon's own delta, in the same frame, exactly as
        // the weapon is drawn. Placing it on the drawn weapon here as well put it there twice over: it floated off
        // the pistol by one more entity -> drawn offset. Its bind keeps the flat pose; only what the
        // GPU does not move (FX, the muzzle entity's effects, the blast, the projectile) is placed in the engine.
        if (const uint8_t* be = (const uint8_t*)HandFx::BoundEntity(); be && be != owner && !(g_Mode.load() & 64) && Readable(be + kEntModel, 8) && *(void* const*)(be + kEntModel))
        { g_JointBoundLeft++; return r; }
        int hand = -1; uint8_t* ent = nullptr;
        void* comp = GunByEntity(owner, 0, hand, ent);
        float np[3], na[9];
        if (!comp || !OntoDrawn(hand, comp, ent, pos, axis, np, na)) return r;
        const float g[3] = { np[0] - pos[0], np[1] - pos[1], np[2] - pos[2] };
        g_JointLastGap = sqrtf(Dot(g, g));
        memcpy(pos, np, 12); memcpy(axis, na, 36);
        g_JointMoved++;
        return r;
    }
    // exe+0x4E3090(slot, posOverride, axisOverride, a4, a5, velOverride, angOverride): the per-frame re-placement of an FX
    // bound to an entity (slot +4 entity id, +8 joint, +0x24 0xFFFF = no joint). With a joint it asks exe+0x53DAC0, which
    // asks exe+0x53D800 (above);
    // without one it copies the entity's own origin/axis -- handed over here as overrides, carried onto the drawn weapon.
    void FxUpdateHook(void* slot, const float* pos, const float* axis, const float* a4, const float* a5, const float* vel, const float* ang)
    {
        uint8_t* s = (uint8_t*)slot;
        if (!(g_Mode.load() & 32) || pos || axis || HandAimHook::Mode() < 2 || !Readable(s, 0x28) || *(int32_t*)s == -1 || *(uint16_t*)(s + 0x24) != 0xFFFF)
        { g_RealFxUpdate(slot, pos, axis, a4, a5, vel, ang); return; }
        int hand = -1; uint8_t* ent = nullptr;
        void* comp = GunByEntity(nullptr, *(uint32_t*)(s + 4), hand, ent);
        float np[3], na[9];
        if (!comp || !OntoDrawn(hand, comp, ent, (const float*)(ent + kEntOrigin), (const float*)(ent + kEntAxis), np, na)) { g_RealFxUpdate(slot, pos, axis, a4, a5, vel, ang); return; }
        g_FxMoved++;
        g_RealFxUpdate(slot, np, na, a4, a5, vel, ang);
    }

    // ---- what the player HOLDS is never culled where the flat game has it ----
    // The engine culls a render model by the world box it keeps at model+0x1840 (min) / +0x1850 (max) -- the local box
    // through the model's transform (+0x1860/+0x186C), exe+0x25F6D2 -- and the bounding sphere exe+0x237340 derives
    // from it (+0x1BD8 centre, +0x1BE4 radius). Both describe the FLAT game's pose. The pistol's reload takes that pose
    // behind and below the eye (measured: the entity at fwd -0.08 left 0.43 up -0.62 m of the camera for 0.8 s), the
    // engine stops submitting the pistol, and the gun vanished from the drawn hand that still held it. The sphere hook
    // widens the box of every model drawn with a held weapon's delta (GpuTrace::WeaponClaim) to cover the pose the GPU draws
    // them at as well, then lets the engine derive the sphere from it again: culled only when both poses are out of view.
    constexpr uint32_t kModelSphere = 0x237340, kModelBoxMin = 0x1840, kModelBoxMax = 0x1850, kModelOrigin = 0x1860;
    using FnModelSphere = void (*)(void*);
    FnModelSphere g_RealModelSphere = nullptr;
    std::atomic<int> g_CullFix{ 1 };
    std::atomic<unsigned long long> g_CullWidened{ 0 };
    std::atomic<float> g_CullShift{ 0.f };
    void ModelSphereHook(void* model)
    {
        g_RealModelSphere(model);
        if (!model || !g_CullFix.load() || HandAimHook::Mode() < 2) return;
        // which hand's equipment this model is drawn with: the weapon test's own rule on the model's origin -- the
        // weapon, its upgrades (the scope, the drum: separate entities at its origin), the cartridge on its joint
        const int hand = GpuTrace::WeaponClaim((const float*)((const uint8_t*)model + kModelOrigin));
        if (hand < 0) return;
        // The weapon entity's pose from BodyHide's per-frame copy (validated on the game thread), not read through the
        // entity pointer: during a level load that pointer outlives the entity and this hook runs on a render thread --
        // measured 2026-10-14, loading a save in stereo with the crossbow and the sword out: an access violation here.
        BodyHide::Anchor an[BodyHide::aCount]; BodyHide::GetAnchors(an);
        const BodyHide::Anchor& wa = an[hand == 0 ? BodyHide::aWeaponL : BodyHide::aWeaponR];
        float to[3], ta[9];
        if (!wa.valid || !GpuTrace::WeaponTargetPose(hand, to, ta)) return;
        const float* eo = wa.org; const float* ea = wa.axis;
        float* mn = (float*)((uint8_t*)model + kModelBoxMin); float* mx = (float*)((uint8_t*)model + kModelBoxMax);
        if (!(mx[0] >= mn[0] && mx[1] >= mn[1] && mx[2] >= mn[2]) || mx[0] - mn[0] > 50.f) return;   // empty / unbounded: leave it
        float lo[3] = { mn[0], mn[1], mn[2] }, hi[3] = { mx[0], mx[1], mx[2] };
        for (int c = 0; c < 8; c++)
        {   // each corner of the flat box, carried by the rigid entity -> drawn-weapon transform the GPU applies
            const float p[3] = { (c & 1) ? mx[0] : mn[0], (c & 2) ? mx[1] : mn[1], (c & 4) ? mx[2] : mn[2] };
            const float d[3] = { p[0] - eo[0], p[1] - eo[1], p[2] - eo[2] };
            const float l[3] = { Dot(d, ea), Dot(d, ea + 3), Dot(d, ea + 6) };
            float w[3]; RowsMul(ta, l, w);
            for (int i = 0; i < 3; i++) { const float q = to[i] + w[i]; lo[i] = std::fmin(lo[i], q); hi[i] = std::fmax(hi[i], q); }
        }
        constexpr float kMargin = 0.1f;   // the hand moves between the frame this pose is from and the frame culled
        const float sh = sqrtf((lo[0] - mn[0]) * (lo[0] - mn[0]) + (hi[0] - mx[0]) * (hi[0] - mx[0]) + (lo[1] - mn[1]) * (lo[1] - mn[1]) + (hi[1] - mx[1]) * (hi[1] - mx[1]) + (lo[2] - mn[2]) * (lo[2] - mn[2]) + (hi[2] - mx[2]) * (hi[2] - mx[2]));
        for (int i = 0; i < 3; i++) { mn[i] = lo[i] - kMargin; mx[i] = hi[i] + kMargin; }
        g_RealModelSphere(model);   // the sphere, from the widened box
        g_CullWidened++; g_CullShift = sh;
    }
    // ---- research: the muzzle FX (m_muzzleAttachedFx, decl +0x230) ----
    // exe+0x44EE80 spawns it through exe+0x4E2B00(slot, fx decl, weapon ENTITY, &joint, ...), which creates the instance
    // with exe+0x19D100(fx world, entity id, decl) and copies the entity's transform (+0x300.., velocity +0x33C..) into
    // instance +0x38.. . Track the instance after the shot: is it bound (does it follow the weapon entity)?
    constexpr uint32_t kFxSpawn = 0x4E2B00, kFxCreate = 0x19D100;
    using FnFxSpawn = bool (*)(void*, void*, void*, void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    using FnFxCreate = void* (*)(void*, uint32_t, void*);
    FnFxSpawn g_RealFxSpawn = nullptr;
    FnFxCreate g_RealFxCreate = nullptr;
    thread_local void* t_FxEnt = nullptr;
    std::atomic<int> g_FxWatch{ 0 };
    struct FxTrack { uint8_t* inst = nullptr; uint8_t* ent = nullptr; int frames = 0; float last[3] = {}; } g_FxTrack;   // game thread
    // The bound FX is STARTED inside its spawn (exe+0x4E2B00 -> exe+0x1A2FF0(fx world, instance, 0, 0x1FFFF, 0.f)) with
    // the pose the spawn copied from the weapon ENTITY (+0x300.., the flat pose), before the weapon's per-frame update
    // ever re-places it, so whatever it emits on its first frame would leave from the flat muzzle. The start is hooked,
    // and inside a spawn on a held gun the instance is first put where the bound update will put it: the joint's world
    // pose, asked the way that update asks it (exe+0x53DAC0, which reaches the weapon-pose hook above).
    // NOT what puts the paper wad in front of the face: that is a mesh particle of muzzle_1p_emily.fx
    // (the muzzle entity's FX) placed relative to the CAMERA every frame -- still open.
    constexpr uint32_t kFxStart = 0x1A2FF0, kGetCpnt = 0x877720, kFxJointCpnt = 0x39DC9D0, kJointWorldByRef = 0x53DAC0;
    using FnFxStart = uintptr_t (*)(void*, void*, uintptr_t, uintptr_t, float);
    using FnGetCpnt = void* (*)(void*, void*, int);
    using FnJointWorldByRef = bool (*)(void*, int, const void*, float*, float*);
    FnFxStart g_RealFxStart = nullptr;
    struct FxSpawnCtx { uint8_t* ent = nullptr; const void* joint = nullptr; int hand = -1; void* comp = nullptr; };
    thread_local FxSpawnCtx t_FxSpawn;
    std::atomic<unsigned long long> g_FxStartsPlaced{ 0 };
    std::atomic<int> g_FxBlock{ 0 };           // research: the k-th FX created in the next shot is never started
    std::atomic<void*> g_FxBlockInst{ nullptr };
    thread_local int t_ShootFxCount = 0;
    uintptr_t FxStartHook(void* world, void* inst, uintptr_t a3, uintptr_t a4, float a5)
    {
        if (inst && inst == g_FxBlockInst.load()) { g_FxBlockInst = nullptr; LOGI("GunRay: fx {} NOT started (vr gunfxblock)", inst); return 0; }
        const FxSpawnCtx c = t_FxSpawn;
        if (c.ent && inst && (g_Mode.load() & 32) && Readable(inst, 0x80))
        {
            const uintptr_t base = GameInfo::Base();
            float p[3], a[9]; bool ok = false;
            if (c.joint)
                if (void* cp = ((FnGetCpnt)(base + kGetCpnt))(c.ent, (void*)(base + kFxJointCpnt), 2))
                    ok = ((FnJointWorldByRef)(base + kJointWorldByRef))(cp, 3, c.joint, p, a);   // drawn (the 0x53D800 hook)
            if (!ok) ok = OntoDrawn(c.hand, c.comp, c.ent, (const float*)(c.ent + kEntOrigin), (const float*)(c.ent + kEntAxis), p, a);
            if (ok)
            {
                memcpy((uint8_t*)inst + 0x38, p, 12); memcpy((uint8_t*)inst + 0x44, a, 36);
                g_FxStartsPlaced++;
            }
        }
        return g_RealFxStart(world, inst, a3, a4, a5);
    }
    bool FxSpawnHook(void* slot, void* decl, void* ent, void* joint, uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8)
    {
        void* prev = t_FxEnt; t_FxEnt = ent;
        const FxSpawnCtx prevCtx = t_FxSpawn;
        t_FxSpawn = FxSpawnCtx();
        if ((g_Mode.load() & 32) && HandAimHook::Mode() >= 2 && ent)
        {
            int hand = -1; uint8_t* e = nullptr;
            if (void* comp = GunByEntity(ent, 0, hand, e)) t_FxSpawn = { e, joint, hand, comp };
        }
        const bool r = g_RealFxSpawn(slot, decl, ent, joint, a5, a6, a7, a8);
        t_FxSpawn = prevCtx;
        t_FxEnt = prev;
        return r;
    }
    // research (`vr gunfxlog n`): every FX created after the next shot, who created it, and where it sits a frame later
    std::atomic<int> g_FxLogArm{ 0 }, g_FxLogLeft{ 0 };
    struct FxSeenRec { uint8_t* inst = nullptr; int frames = 0; std::string who; };
    std::mutex g_FxLogM;
    std::vector<FxSeenRec> g_FxLogRecs;
    //! A decl's name, for the logs: the first pointer in its first 0x40 bytes that leads to a printable string.
    std::string DeclName(const void* decl)
    {
        if (!Readable(decl, 0x40)) return "?";
        for (int i = 1; i < 8; i++)
        {
            const char* p = *(const char* const*)((const uint8_t*)decl + i * 8);
            if (!Readable(p, 8)) continue;
            int n = 0; while (n < 120 && Readable(p + n, 1) && p[n] >= 32 && p[n] < 127) n++;
            if (n >= 4 && Readable(p + n, 1) && p[n] == 0) return std::string(p, n);
            // idStr: {char* data; int len; ...} -- one more indirection
            const char* q = Readable(p, 8) ? *(const char* const*)p : nullptr;
            if (!Readable(q, 8)) continue;
            n = 0; while (n < 120 && Readable(q + n, 1) && q[n] >= 32 && q[n] < 127) n++;
            if (n >= 4 && Readable(q + n, 1) && q[n] == 0) return std::string(q, n);
        }
        return "?";
    }
    void* FxCreateHook(void* world, uint32_t entId, void* decl)
    {
        void* inst = g_RealFxCreate(world, entId, decl);
        if (t_InShoot && inst)
        {
            const int k = ++t_ShootFxCount;
            if (k == g_FxBlock.load()) { g_FxBlock = 0; g_FxBlockInst = inst; LOGI("GunRay: blocking fx #{} {} decl '{}'", k, inst, DeclName(decl)); }
        }
        if (t_InShoot && inst && g_Track.load())
        {   // an entity the shot spawned (the muzzle entity of exe+0x63B3C0): follow it
            uint8_t* e = (uint8_t*)BodyHide::EntityById((int)entId);
            void* comp = nullptr; { std::lock_guard lk(g_RegM); comp = g_Cache[HandAimHook::GunHand() & 1].comp; }
            if (e && IsGunImpl(comp) && e != *(uint8_t**)((uint8_t*)comp + kCpntEntity)) StartFlight(comp, e, "spawned");
        }
        if (g_FxLogLeft.load() > 0 && inst)
        {
            g_FxLogLeft--;
            void* frames[10] = {}; const USHORT n = RtlCaptureStackBackTrace(1, 10, frames, nullptr);
            const uintptr_t base = GameInfo::Base(); std::string ra;
            for (USHORT i = 0; i < n; i++) { const uintptr_t a = (uintptr_t)frames[i]; if (a >= base && a < base + 0x4000000) ra += fmt::format(" 0x{:X}", a - base); }
            std::string who = fmt::format("inst {} decl {} '{}' ent id {:#x} tid {} | callers{}", inst, decl, DeclName(decl), entId, GetCurrentThreadId(), ra);
            std::lock_guard lk(g_FxLogM); g_FxLogRecs.push_back({ (uint8_t*)inst, 0, who });
        }
        if (!t_FxEnt || !inst || !g_Track.load()) return inst;
        // only the gun's: the entity is the equipment entity of the gun hand
        void* comp = nullptr; { std::lock_guard lk(g_RegM); comp = g_Cache[HandAimHook::GunHand() & 1].comp; }
        if (!IsGunImpl(comp) || *(void**)((uint8_t*)comp + kCpntEntity) != t_FxEnt) return inst;
        g_FxTrack = FxTrack(); g_FxTrack.inst = (uint8_t*)inst; g_FxTrack.ent = (uint8_t*)t_FxEnt;
        LOGI("GunFx: muzzle fx instance {} decl {} on entity {} (id {:#x})", inst, decl, t_FxEnt, entId);
        if (int n = g_FxWatch.exchange(0)) Watchpoint::Arm((uintptr_t)inst + 0x38, 4, n, false, true);
        return inst;
    }
    // exe+0x44E0C0 arkCpntWeaponGun shoot (one per trigger pull): the muzzle fx, then the blast, then the bullet
    constexpr uint32_t kGunShoot = 0x44E0C0;
    FnThis g_RealGunShoot = nullptr;
    void* GunShootHook(void* w)
    {
        if (int n = g_FxLogArm.exchange(0)) g_FxLogLeft = n;
        if (!t_InShoot) t_ShootFxCount = 0;
        t_InShoot++;
        void* r = g_RealGunShoot(w);
        t_InShoot--;
        return r;
    }
    void TrackFx()
    {
        {
            std::lock_guard lk(g_FxLogM);
            for (auto it = g_FxLogRecs.begin(); it != g_FxLogRecs.end();)
            {
                if (++it->frames < 2) { ++it; continue; }
                float dm[3] = {}, df[3] = {}; const bool drawn = GunRay::DrawnRay(HandAimHook::GunHand(), dm, df);
                float eye[3], cax[9]; const bool cam = HandAimHook::RealCamera(eye, cax);
                if (Readable(it->inst, 0x80))
                {
                    const float* o = (const float*)(it->inst + 0x38); const float* a = (const float*)(it->inst + 0x44);
                    const float d[3] = { o[0] - dm[0], o[1] - dm[1], o[2] - dm[2] };
                    const float e[3] = { o[0] - eye[0], o[1] - eye[1], o[2] - eye[2] };
                    LOGI("GunFxLog: {} | at ({:.3f},{:.3f},{:.3f}) ax0 ({:.3f},{:.3f},{:.3f}) | drawn muzzle {:.3f} m, barrel {:.1f} deg | eye {:.3f} m, view {:.1f} deg",
                         it->who, o[0], o[1], o[2], a[0], a[1], a[2], drawn ? sqrtf(Dot(d, d)) : -1.f, drawn ? AngleDeg(a, df) : -1.f, cam ? sqrtf(Dot(e, e)) : -1.f, cam ? AngleDeg(a, cax) : -1.f);
                }
                else LOGI("GunFxLog: {} | gone", it->who);
                it = g_FxLogRecs.erase(it);
            }
        }
        FxTrack& f = g_FxTrack;
        if (!f.inst || f.frames >= 40) return;
        if (!Readable(f.inst, 0x140) || !Readable(f.ent, 0x350)) { f.inst = nullptr; return; }
        const float* o = (const float*)(f.inst + 0x38); const float* a = (const float*)(f.inst + 0x44);
        const float* eo = (const float*)(f.ent + kEntOrigin); const float* ea = (const float*)(f.ent + kEntAxis);
        const float d[3] = { o[0] - f.last[0], o[1] - f.last[1], o[2] - f.last[2] };
        float dm[3] = {}, df[3] = {}; const bool drawn = GunRay::DrawnRay(HandAimHook::GunHand(), dm, df);
        const float dd[3] = { o[0] - dm[0], o[1] - dm[1], o[2] - dm[2] };
        const float de[3] = { o[0] - eo[0], o[1] - eo[1], o[2] - eo[2] };
        if (f.frames < 6 || f.frames % 8 == 0)
            LOGI("GunFx: frame {} inst at ({:.3f},{:.3f},{:.3f}) ax0 ({:.3f},{:.3f},{:.3f}) moved {:.3f} | entity ({:.3f},{:.3f},{:.3f}) {:.3f} m away, ax0 {:.1f} deg | drawn muzzle {:.3f} m away, barrel {:.1f} deg",
                 f.frames, o[0], o[1], o[2], a[0], a[1], a[2], sqrtf(Dot(d, d)), eo[0], eo[1], eo[2], sqrtf(Dot(de, de)), AngleDeg(a, ea),
                 drawn ? sqrtf(Dot(dd, dd)) : -1.f, drawn ? AngleDeg(a, df) : -1.f);
        memcpy(f.last, o, 12); f.frames++;
    }

    //! Once a frame (game thread): where each tracked projectile is now; when it stops, how far its end lies from the
    //! gun ray and from the head's line of sight.
    void TrackFlights()
    {
        std::lock_guard lk(g_FlightM);
        for (Flight& f : g_Flights)
        {
            if (f.done) continue;
            if (!Readable(f.ent, 0x350) || *(uintptr_t*)f.ent != f.vt)
            {
                LOGI("GunFlight: ent {} gone after {} frames, last ({:.3f},{:.3f},{:.3f}) | {:.1f} m flown, off gun ray {:.1f} cm, off look ray {:.1f} cm",
                     (void*)f.ent, f.frames, f.last[0], f.last[1], f.last[2], f.path, LineDist(f.last, f.gunO, f.gunF) * 100.f, LineDist(f.last, f.eye, f.look) * 100.f);
                f.done = true; continue;
            }
            const float* o = (const float*)(f.ent + kEntOrigin); const float* v = (const float*)(f.ent + 0x33C);
            const float d[3] = { o[0] - f.last[0], o[1] - f.last[1], o[2] - f.last[2] };
            const float step = sqrtf(Dot(d, d));
            f.path += step; memcpy(f.last, o, 12); f.frames++;
            f.still = step < 0.002f ? f.still + 1 : 0;
            if (f.frames <= 4 || f.frames % 10 == 0 || f.still == 3)
                LOGI("GunFlight: ent {} frame {} at ({:.3f},{:.3f},{:.3f}) vel ({:.1f},{:.1f},{:.1f}) step {:.3f} | off gun ray {:.1f} cm, off look ray {:.1f} cm{}",
                     (void*)f.ent, f.frames, o[0], o[1], o[2], v[0], v[1], v[2], step, LineDist(o, f.gunO, f.gunF) * 100.f, LineDist(o, f.eye, f.look) * 100.f, f.still == 3 ? " STOPPED" : "");
            if (f.still >= 3 || f.frames > 240) f.done = true;
        }
    }

    //! Rotation taking unit a onto unit b, applied to v (Rodrigues).
    void RotateLike(const float* a, const float* b, const float* v, float* o)
    {
        const float k[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
        const float s = sqrtf(Dot(k, k)), c = Dot(a, b);
        if (s < 1e-7f) { memcpy(o, v, 12); return; }
        const float u[3] = { k[0] / s, k[1] / s, k[2] / s };
        const float uv[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        const float ud = Dot(u, v);
        for (int i = 0; i < 3; i++) o[i] = v[i] * c + uv[i] * s + u[i] * ud * (1.f - c);
    }
}

bool GunRay::Install()
{
    if (!GameInfo::IsSupportedBuild()) return false;
    const uintptr_t base = GameInfo::Base();
    g_RealSubmit = (FnThis)(base + kSensorSubmit);
    g_RealResults = (FnThis)(base + kSensorResults);
    g_RealWeaponAim = (FnAim)(base + kWeaponAimPoint);
    g_RealInit = (FnThis)(base + kRangedInit);
    g_RealLaunch = (FnLaunch)(base + kLaunch);
    g_RealFireTarget = (FnFireTarget)(base + kFireTarget);
    g_RealBlast = (FnBlast)(base + kGunBlast);
    g_RealFxSpawn = (FnFxSpawn)(base + kFxSpawn);
    g_RealFxUpdate = (FnFxUpdate)(base + kFxUpdate);
    g_RealJointWorld = (FnJointWorld)(base + kJointWorld);
    g_RealGunShoot = (FnThis)(base + kGunShoot);
    g_RealFxStart = (FnFxStart)(base + kFxStart);
    g_RealFxCreate = (FnFxCreate)(base + kFxCreate);
    g_RealModelSphere = (FnModelSphere)(base + kModelSphere);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_RealSubmit, SubmitHook);
    DetourAttach(&(PVOID&)g_RealResults, ResultsHook);
    DetourAttach(&(PVOID&)g_RealWeaponAim, WeaponAimHook);
    DetourAttach(&(PVOID&)g_RealInit, InitHook);
    DetourAttach(&(PVOID&)g_RealLaunch, LaunchHook);
    DetourAttach(&(PVOID&)g_RealFireTarget, FireTargetHook);
    DetourAttach(&(PVOID&)g_RealBlast, BlastHook);
    DetourAttach(&(PVOID&)g_RealFxSpawn, FxSpawnHook);
    DetourAttach(&(PVOID&)g_RealFxUpdate, FxUpdateHook);
    DetourAttach(&(PVOID&)g_RealJointWorld, JointWorldHook);
    DetourAttach(&(PVOID&)g_RealGunShoot, GunShootHook);
    DetourAttach(&(PVOID&)g_RealFxStart, FxStartHook);
    DetourAttach(&(PVOID&)g_RealFxCreate, FxCreateHook);
    DetourAttach(&(PVOID&)g_RealModelSphere, ModelSphereHook);
    const LONG r = DetourTransactionCommit();
    LOGI("GunRay: aim sensor submit exe+0x{:X} results exe+0x{:X}, weapon aim point exe+0x{:X}, ranged init exe+0x{:X}: detour={}",
         kSensorSubmit, kSensorResults, kWeaponAimPoint, kRangedInit, r);
    return r == NO_ERROR;
}
void GunRay::Uninstall()
{
    if (!g_RealSubmit) return;
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)g_RealSubmit, SubmitHook);
    DetourDetach(&(PVOID&)g_RealResults, ResultsHook);
    DetourDetach(&(PVOID&)g_RealWeaponAim, WeaponAimHook);
    DetourDetach(&(PVOID&)g_RealInit, InitHook);
    DetourDetach(&(PVOID&)g_RealLaunch, LaunchHook);
    DetourDetach(&(PVOID&)g_RealFireTarget, FireTargetHook);
    DetourDetach(&(PVOID&)g_RealBlast, BlastHook);
    DetourDetach(&(PVOID&)g_RealFxSpawn, FxSpawnHook);
    DetourDetach(&(PVOID&)g_RealFxUpdate, FxUpdateHook);
    DetourDetach(&(PVOID&)g_RealJointWorld, JointWorldHook);
    DetourDetach(&(PVOID&)g_RealGunShoot, GunShootHook);
    DetourDetach(&(PVOID&)g_RealFxStart, FxStartHook);
    DetourDetach(&(PVOID&)g_RealFxCreate, FxCreateHook);
    DetourDetach(&(PVOID&)g_RealModelSphere, ModelSphereHook);
    DetourTransactionCommit();
}
void GunRay::SetTrack(int on) { g_Track = on; LOGI("GunRay: projectile tracking {}", on); }
void GunRay::BurstNextShot(int frames) { g_ShotBurst = frames; LOGI("GunRay: the next shot captures {} frames (DVR_gunshot{}_b)", frames, g_ShotBurstN.load() + 1); }
//! Research (vr gunkids): which entities near the eye hold a reference to a held weapon entity -- its pointer or its
//! spawn id -- and at which offset of the entity (or of the object one pointer away), to find how a part is parented.
void GunRay::ProbeKids(float radius)
{
    uint8_t* game = *(uint8_t**)(GameInfo::Base() + 0x39A5290);
    float eye[3], cax[9];
    if (!game || !HandAimHook::RealCamera(eye, cax)) { LOGI("gunkids: no game / camera"); return; }
    uint8_t** table = (uint8_t**)(game + 0x3130B0);
    const uint8_t* w[2] = { (const uint8_t*)BodyHide::LeftWeaponEntity(), (const uint8_t*)BodyHide::RightWeaponEntity() };
    uint32_t wid[2] = {}; for (int h = 0; h < 2; h++) if (w[h] && Readable(w[h] + kEntId, 4)) wid[h] = *(const uint32_t*)(w[h] + kEntId);
    LOGI("gunkids: weapon entities L {} id {:#x} R {} id {:#x}", (const void*)w[0], wid[0], (const void*)w[1], wid[1]);
    for (int i = 0; i < 0x4000; i++)
    {
        uint8_t* e = table[i];
        if (!e || !Readable(e, 0x600) || e == w[0] || e == w[1]) continue;
        const float* o = (const float*)(e + kEntOrigin);
        const float d[3] = { o[0] - eye[0], o[1] - eye[1], o[2] - eye[2] };
        if (!(Dot(d, d) <= radius * radius)) continue;
        std::string hits;
        for (uint32_t off = 0; off + 8 <= 0x600; off += 4)
        {
            const uint64_t q = *(const uint64_t*)(e + off); const uint32_t v = *(const uint32_t*)(e + off);
            for (int h = 0; h < 2; h++)
            {
                if (w[h] && (off % 8) == 0 && q == (uint64_t)w[h]) hits += fmt::format(" +{:#x}=ptr{}", off, h ? 'R' : 'L');
                if (wid[h] && v == wid[h]) hits += fmt::format(" +{:#x}=id{}", off, h ? 'R' : 'L');
            }
            if ((off % 8) == 0)
            {   // one pointer away (components, the bind)
                const uint8_t* c = (const uint8_t*)q;
                if (q > 0x10000 && (q & 7) == 0 && Readable(c, 0x100))
                    for (uint32_t k = 0; k + 8 <= 0x100; k += 4)
                        for (int h = 0; h < 2; h++)
                        {
                            if (w[h] && (k % 8) == 0 && *(const uint64_t*)(c + k) == (uint64_t)w[h]) hits += fmt::format(" +{:#x}->+{:#x}=ptr{}", off, k, h ? 'R' : 'L');
                            if (wid[h] && *(const uint32_t*)(c + k) == wid[h]) hits += fmt::format(" +{:#x}->+{:#x}=id{}", off, k, h ? 'R' : 'L');
                        }
            }
        }
        if (!hits.empty()) LOGI("gunkids: ent {} vt exe+{:#x} id {:#x} at ({:.2f},{:.2f},{:.2f}) model {}:{}", (void*)e, *(uintptr_t*)e - GameInfo::Base(), *(uint32_t*)(e + kEntId), o[0], o[1], o[2], *(void**)(e + kEntModel), hits);
    }
}
void GunRay::NearEntities(float radius)
{
    // the entity table: game + 0x3130B0, 0x4000 slots indexed by spawn id >> 16, the entity's id at +0xA8 (exe+0x7B5E00)
    uint8_t* game = *(uint8_t**)(GameInfo::Base() + 0x39A5290);
    float eye[3], cax[9];
    if (!game || !HandAimHook::RealCamera(eye, cax)) { LOGI("nearents: no game / camera"); return; }
    uint8_t** table = (uint8_t**)(game + 0x3130B0);
    if (!Readable(table, 0x4000 * 8)) { LOGI("nearents: table unreadable"); return; }
    void* comp = nullptr; { std::lock_guard lk(g_RegM); comp = g_Cache[HandAimHook::GunHand() & 1].comp; }
    float m[3] = {}, dax[9] = {}, dorg[3] = {}; const bool pose = IsGunImpl(comp) && PoseImpl(HandAimHook::GunHand(), m, dax, comp, dorg);
    int n = 0;
    for (int i = 0; i < 0x4000; i++)
    {
        uint8_t* e = table[i];
        if (!e || !Readable(e, 0x350)) continue;
        const float* o = (const float*)(e + kEntOrigin);
        const float d[3] = { o[0] - eye[0], o[1] - eye[1], o[2] - eye[2] };
        const float dist = sqrtf(Dot(d, d));
        if (!(dist <= radius)) continue;
        const float fwd = Dot(d, cax), left = Dot(d, cax + 3), up = Dot(d, cax + 6);
        const float dm[3] = { o[0] - m[0], o[1] - m[1], o[2] - m[2] };
        LOGI("nearents: [{:#x}] ent {} vt exe+{:#x} id {:#x} at ({:.3f},{:.3f},{:.3f}) {:.2f} m: fwd {:.2f} left {:.2f} up {:.2f} | hidden {} | {:.2f} m from the drawn muzzle",
             i, (void*)e, *(uintptr_t*)e - GameInfo::Base(), *(uint32_t*)(e + kEntId), o[0], o[1], o[2], dist, fwd, left, up, e[0xD0] & 1, pose ? sqrtf(Dot(dm, dm)) : -1.f);
        if (++n >= 60) break;
    }
    LOGI("nearents: {} within {:.1f} m of the eye", n, radius);
}
void GunRay::ProbeAttachment(int n)
{
    void* comp = nullptr; { std::lock_guard lk(g_RegM); comp = g_Cache[HandAimHook::GunHand() & 1].comp; }
    if (!IsGunImpl(comp)) { LOGI("gunattach: no gun in the gun hand"); return; }
    uint8_t* att = *(uint8_t**)((uint8_t*)comp + 0x158);
    uint8_t* ent = *(uint8_t**)((uint8_t*)comp + kCpntEntity);
    if (!Readable(att, 0x350)) { LOGI("gunattach: gun {} has no attachment entity (+0x158 = {})", comp, (void*)att); return; }
    const float* ao = (const float*)(att + kEntOrigin); const float* eo = (const float*)(ent + kEntOrigin);
    float dm[3] = {}, df[3] = {}; const bool drawn = GunRay::DrawnRay(HandAimHook::GunHand(), dm, df);
    const float d1[3] = { ao[0] - eo[0], ao[1] - eo[1], ao[2] - eo[2] }, d2[3] = { ao[0] - dm[0], ao[1] - dm[1], ao[2] - dm[2] };
    // where it sits in the weapon ENTITY's frame and in the DRAWN weapon's frame (the bind's joint offset in the one it
    // really hangs from)
    const float* ea = (const float*)(ent + kEntAxis);
    const float le[3] = { Dot(d1, ea), Dot(d1, ea + 3), Dot(d1, ea + 6) };
    float m[3], dax[9], dorg[3], ld[3] = { -1, -1, -1 };
    if (PoseImpl(HandAimHook::GunHand(), m, dax, comp, dorg)) { const float d3[3] = { ao[0] - dorg[0], ao[1] - dorg[1], ao[2] - dorg[2] }; ld[0] = Dot(d3, dax); ld[1] = Dot(d3, dax + 3); ld[2] = Dot(d3, dax + 6); }
    LOGI("gunattach: gun {} entity {} attachment {} (vt exe+{:#x}) at ({:.3f},{:.3f},{:.3f}): {:.3f} m from the weapon entity, {:.3f} m from the drawn muzzle | in the entity's frame ({:.3f},{:.3f},{:.3f}), in the drawn weapon's ({:.3f},{:.3f},{:.3f})",
         comp, (void*)ent, (void*)att, *(uintptr_t*)att - GameInfo::Base(), ao[0], ao[1], ao[2], sqrtf(Dot(d1, d1)), drawn ? sqrtf(Dot(d2, d2)) : -1.f, le[0], le[1], le[2], ld[0], ld[1], ld[2]);
    {   // PoseImpl's prediction of the drawn weapon against the pose the GPU really draws it at this frame
        float to[3], ta[9];
        if (GpuTrace::WeaponTargetPose(HandAimHook::GunHand(), to, ta))
        {
            const float dt[3] = { dorg[0] - to[0], dorg[1] - to[1], dorg[2] - to[2] };
            float tr = 0; for (int i = 0; i < 3; i++) tr += Dot(dax + 3 * i, ta + 3 * i);
            const float ang = acosf(std::fmax(-1.f, std::fmin(1.f, (tr - 1.f) * 0.5f))) * 57.2958f;
            LOGI("gunattach: drawn-weapon prediction vs GPU target: {:.4f} m ({:.4f},{:.4f},{:.4f}), {:.2f} deg | target ({:.3f},{:.3f},{:.3f}) predicted ({:.3f},{:.3f},{:.3f})",
                 sqrtf(Dot(dt, dt)), dt[0], dt[1], dt[2], ang, to[0], to[1], to[2], dorg[0], dorg[1], dorg[2]);
        }
    }
    HandFx::ProbeEntity(att, n);
}
void GunRay::FreezeAfterShot(int ms) { g_FreezeMs = ms; LOGI("GunRay: the next shot freezes the game {} ms later", ms); }
void GunRay::BlockFx(int k) { g_FxBlock = k; LOGI("GunRay: the next shot does not start its fx #{}", k); }
void GunRay::LogFx(int n) { g_FxLogArm = n; g_JointDiag = 12; LOGI("GunRay: the next shot logs the next {} fx created", n); }
void GunRay::WatchFx(int hits) { g_FxWatch = hits; LOGI("GunRay: the next muzzle fx arms a write watchpoint on its origin ({} hits)", hits); }
void GunRay::SetNoLock(int on) { g_NoLock = on; LOGI("GunRay: fire target lock {}", on ? "DROPPED" : "kept"); }
void GunRay::Tick()
{
    if (const ULONGLONG at = g_FreezeAt.load(); at && GetTickCount64() >= at)
    {   // research: freeze the game this long after a shot (`vr gunfreeze ms`); `set g_stopTime 0` resumes
        g_FreezeAt = 0;
        LOGI("GunRay: shot freeze -> g_stopTime 1: {}", CVars::SetInt("g_stopTime", 1));
    }
    TrackFlights();
    TrackFx();
    if (!(g_Mode.load() & 10) || HandAimHook::Mode() < 2) return;
    float m[3], ax[9];
    void* w = nullptr;
    if (!PoseImpl(HandAimHook::GunHand(), m, ax)) { std::lock_guard lk(g_SensM); g_Trace.valid = false; return; }
    { std::lock_guard lk(g_RegM); w = g_Cache[HandAimHook::GunHand() & 1].comp; }
    float t = 0; bool hit = false;
    if (!SweepImpl(w, m, ax, WeaponRange(w), g_SweepMask.load(), true, t, hit)) { std::lock_guard lk(g_SensM); g_Trace.valid = false; return; }
    g_Sweeps++; if (hit) g_SweepHits++;
    std::lock_guard lk(g_SensM);
    // The sweep sees only STATIC collision (any mask: measured, a bottle standing 0.71 m out was not there, the table
    // behind it at 1.41 was). Props and characters live in the dynamic physics world, which is what the aim sensor
    // queries -- along this same ray, one frame late. Its hit counts while it still lies on the current ray (a hand that
    // has moved off the bottle must not keep the ring on it): within 2 cm, or 1% of the distance, of the ray.
    g_Trace.sensor = false; g_Trace.sweepT = t; g_Trace.sweepHit = hit;
    if (g_Hit.valid && g_Hit.has && GetTickCount64() - g_Hit.at <= 200)
    {
        const float d[3] = { g_Hit.point[0] - m[0], g_Hit.point[1] - m[1], g_Hit.point[2] - m[2] };
        const float along = Dot(d, ax), off = LineDist(g_Hit.point, m, ax);
        if (along > 0.f && along < t && off < fmaxf(0.02f, 0.01f * along)) { t = along; hit = true; g_Trace.sensor = true; g_SensorUsed++; }
    }
    g_Trace.valid = true; g_Trace.hit = hit; g_Trace.t = t; memcpy(g_Trace.o, m, 12); memcpy(g_Trace.f, ax, 12); g_Trace.at = GetTickCount64();
}
void GunRay::Probe()
{
    float m[3], ax[9];
    if (!PoseImpl(HandAimHook::GunHand(), m, ax)) { LOGI("gunprobe: no gun pose"); return; }
    void* w; { std::lock_guard lk(g_RegM); w = g_Cache[HandAimHook::GunHand() & 1].comp; }
    const float range = WeaponRange(w);
    uint8_t* decl = *(uint8_t**)((uint8_t*)w + kCpntDecl);
    LOGI("gunprobe: weapon {} shape {} range {:.1f} (decl {:.1f}) muzzle ({:.3f},{:.3f},{:.3f}) fwd ({:.3f},{:.3f},{:.3f})", w,
         *(void**)((uint8_t*)w + kProjShape), range, Readable(decl, kDeclMaxRange + 4) ? *(float*)(decl + kDeclMaxRange) : -1.f, m[0], m[1], m[2], ax[0], ax[1], ax[2]);
    LOGI("gunprobe: aim sensor mask {:#x} flag {} ({:#x} with it)", g_SensorMask.load(), g_SensorFlags.load(), g_SensorMask.load() | (g_SensorFlags.load() ? 1u << 25 : 0u));
    const uint32_t masks[] = { 0x20000, 0x800, 0x408, g_SensorMask.load(), g_SensorMask.load() | (1u << 25), 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400, 0x1000, 0x2000, 0x4000, 0x8000, 0x10000, 0x40000, 0x80000, 0x100000 };
    for (uint32_t mk : masks)
        for (int sh = 0; sh < 2; sh++)
        {
            float t = 0; bool hit = false;
            const bool ok = SweepImpl(w, m, ax, range, mk, sh != 0, t, hit);
            LOGI("gunprobe: {} mask {:#x}: {} t {:.3f}", sh ? "sweep" : "ray  ", mk, ok ? (hit ? "HIT" : "clear") : "n/a", t);
        }
}
void GunRay::Pixel(float fx, float fy)
{
    const float ndc[2] = { fx * 2.f - 1.f, 1.f - fy * 2.f };
    float eo[3], ed[3];
    if (!GpuTrace::UnprojectNdc(ndc, eo, ed)) { LOGI("gunpix: no projection"); return; }
    std::lock_guard lk(g_SensM);
    g_Probe.frames = 2; memcpy(g_Probe.o, eo, 12); memcpy(g_Probe.d, ed, 12); g_Probe.fx = fx; g_Probe.fy = fy;
    LOGI("gunpix: {:.3f} {:.3f}: eye ({:.3f},{:.3f},{:.3f}) dir ({:.3f},{:.3f},{:.3f}), asking the aim sensor", fx, fy, eo[0], eo[1], eo[2], ed[0], ed[1], ed[2]);
}
void GunRay::NoteRing(float fx, float fy) { g_RingX = fx; g_RingY = fy; }
void GunRay::ClearTarget() { std::lock_guard lk(g_SensM); g_Target.valid = false; }
void GunRay::SetSweep(uint32_t mask, float maxRange) { g_SweepMask = mask; if (maxRange > 1.f) g_MaxRange = maxRange; LOGI("GunRay: sweep mask {:#x} max range {}", mask, g_MaxRange.load()); }
void GunRay::SetMode(int m) { g_Mode = m; LOGI("GunRay: mode {} (1 sensor, 2 aim point, 4 muzzle, 8 reticle, 16 pistol blast, 32 weapon fx)", m); }
int  GunRay::Mode() { return g_Mode.load(); }
void GunRay::SetSpread(int on) { g_Spread = on; LOGI("GunRay: spread {}", on ? "kept (re-applied about the barrel)" : "off (exactly along the barrel)"); }
void GunRay::Log(int n) { g_LogLeft = n; }
bool GunRay::IsGun(void* w) { return IsGunImpl(w); }
bool GunRay::Pose(int hand, float muzzle[3], float axis[9]) { return PoseImpl(hand, muzzle, axis); }
bool GunRay::SensorHit(float& dist, bool& hit) { return SensorHitImpl(dist, hit); }
GunRay::TraceDbg GunRay::LastTrace()
{
    TraceDbg d;
    std::lock_guard lk(g_SensM);
    const ULONGLONG now = GetTickCount64();
    d.fromTrace = g_Trace.valid && now - g_Trace.at <= 300;
    d.valid = d.fromTrace || (g_Hit.valid && now - g_Hit.at <= 500);
    d.t = d.fromTrace ? g_Trace.t : g_Hit.t; d.hit = d.fromTrace ? g_Trace.hit : g_Hit.has;
    d.sweepT = g_Trace.sweepT; d.sweepHit = g_Trace.sweepHit; d.sensorUsed = g_Trace.sensor;
    d.sensorHas = g_Hit.has; d.sensorT = g_Hit.t; memcpy(d.sensorPoint, g_Hit.point, 12);
    d.traceAgeMs = g_Trace.valid ? (unsigned)std::min<ULONGLONG>(99999, now - g_Trace.at) : 99999;
    d.sensorAgeMs = g_Hit.valid ? (unsigned)std::min<ULONGLONG>(99999, now - g_Hit.at) : 99999;
    return d;
}

bool GunRay::DrawnRay(int hand, float muzzle[3], float fwd[3])
{
    float org[3], axis[9];
    if (!GpuTrace::WeaponTargetPose(hand, org, axis)) return false;
    // that hand must still hold the gun the muzzle was read from (an empty hand has a target pose too)
    void* ent = nullptr;
    {
        const float zo[3] = {}, ia[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; float d1[3], d2[9];
        if (!GpuTrace::WeaponFromHand(hand, zo, ia, d1, d2, &ent)) return false;
    }
    float sp[3];
    {
        std::lock_guard lk(g_RegM);
        const Cache& c = g_Cache[hand & 1];
        if (!c.valid || GetTickCount64() - c.at > 1000 || c.ent != ent || !IsGunImpl(c.comp)) return false;
        memcpy(sp, c.sp, 12);
    }
    float off[3]; RowsMul(axis, sp, off);
    for (int i = 0; i < 3; i++) muzzle[i] = org[i] + off[i];
    memcpy(fwd, axis, 12);
    return true;
}

namespace { std::atomic<unsigned long long> g_SpawnKept{ 0 }, g_SpawnFixed{ 0 }, g_SpawnBlocked{ 0 }; }
void GunRay::FixSpawn(const Shot& s, float* spawn)
{
    if (!s.valid) return;
    const float d[3] = { spawn[0] - s.muzzle[0], spawn[1] - s.muzzle[1], spawn[2] - s.muzzle[2] };
    const float gap = sqrtf(Dot(d, d));
    if (gap < 0.005f) { g_SpawnKept++; return; }
    // The engine moved the spawn off the muzzle: its blocked-muzzle fallback (ComputeAimTarget: a ray from the camera eye
    // to the muzzle, mask 0x800, and the projectile's sweep from the muzzle to the aim point, mask 0x20000; if either is
    // blocked the spawn slides along the eye's ray). Measured walking towards a table: 62 cm back, inside the body, still
    // on the gun ray. What that fallback is FOR is a weapon poking through a wall; in VR the weapon is in the hand, so
    // the question is whether anything solid lies between the hand (the weapon's grip, entOrg) and the muzzle.
    float lm[3] = { s.muzzle[0] - s.entOrg[0], s.muzzle[1] - s.entOrg[1], s.muzzle[2] - s.entOrg[2] };
    const float len = sqrtf(Dot(lm, lm)); Normalize(lm);
    float t = 0, te = 0, ta = 0; bool hit = true, hitEye = false, hitAim = false;
    const bool ok = SweepImpl(s.weapon, s.entOrg, lm, len, 0x800, false, t, hit);
    SweepImpl(s.weapon, s.eye, s.axis, kEyeBehind, 0x800, false, te, hitEye);
    { const float d2[3] = { s.aim[0] - s.muzzle[0], s.aim[1] - s.muzzle[1], s.aim[2] - s.muzzle[2] }; SweepImpl(s.weapon, s.muzzle, s.axis, sqrtf(Dot(d2, d2)), 0x20000, true, ta, hitAim); }
    const bool keepMuzzle = ok && !hit;
    LOGI("GunRay: engine spawn {:.1f} cm off the muzzle; grip->muzzle {} ({:.3f} of {:.3f}), eye->muzzle {}, muzzle->aim sweep {} at {:.3f} of {:.3f} -> {}",
         gap * 100.f, hit ? "BLOCKED" : "clear", t, len, hitEye ? "blocked" : "clear", hitAim ? "hit" : "clear", ta, s.dist,
         keepMuzzle ? "spawn at the muzzle" : "kept the engine's (the weapon is through something)");
    if (keepMuzzle) { memcpy(spawn, s.muzzle, 12); g_SpawnFixed++; }
    else g_SpawnBlocked++;
}
bool GunRay::Prepare(void* weapon, int hand, const float* aimPointIn, Shot& s)
{
    s = Shot();
    Register(weapon);
    if (!(g_Mode.load() & 4) || HandAimHook::Mode() < 2 || !IsGunImpl(weapon)) return false;
    uint8_t* ent = *(uint8_t**)((uint8_t*)weapon + kCpntEntity);
    if (!Readable(ent + kEntOrigin, 48)) return false;
    if (!PoseImpl(hand, s.muzzle, s.axis, weapon, s.entOrg)) return false;
    s.weapon = weapon; s.ent = ent;
    memcpy(s.entAxis, s.axis, 36);
    // The engine's muzzle is entity origin + entity axis * the component's muzzle joint: with the entity at the drawn
    // weapon's pose it IS the drawn muzzle. (If the component's entity is not the anchored one, the same pose still
    // puts its muzzle there: the joint offset is the component's.)
    float sp[3], off[3]; memcpy(sp, (uint8_t*)weapon + kSpawnPos, 12); RowsMul(s.axis, sp, off);
    for (int i = 0; i < 3; i++) s.entOrg[i] = s.muzzle[i] - off[i];
    for (int i = 0; i < 3; i++) s.eye[i] = s.muzzle[i] - s.axis[i] * kEyeBehind;
    memcpy(s.aimIn, aimPointIn, 12);
    float t = 0; bool hit = false;
    if (SensorHitImpl(t, hit)) { s.dist = t; s.sensorHit = hit; }
    else
    {   // no sensor answer: keep the game's distance, measured from the muzzle
        const float d[3] = { aimPointIn[0] - s.muzzle[0], aimPointIn[1] - s.muzzle[1], aimPointIn[2] - s.muzzle[2] };
        s.dist = sqrtf(Dot(d, d)); if (!std::isfinite(s.dist) || s.dist < 1.f) s.dist = 30.f;
    }
    if (g_Mode.load() & 2)
    {
        float dir[3] = { s.axis[0], s.axis[1], s.axis[2] };
        if (g_Spread.load() && g_LastAim.w == weapon && GetTickCount64() - g_LastAim.at < 200)
        {   // the game's spread step turned the aim about the HEAD eye: the same turn, about the barrel
            float a[3] = { g_LastAim.out[0] - g_LastAim.eye[0], g_LastAim.out[1] - g_LastAim.eye[1], g_LastAim.out[2] - g_LastAim.eye[2] };
            float b[3] = { aimPointIn[0] - g_LastAim.eye[0], aimPointIn[1] - g_LastAim.eye[1], aimPointIn[2] - g_LastAim.eye[2] };
            Normalize(a); Normalize(b);
            float d2[3]; RotateLike(a, b, dir, d2); memcpy(dir, d2, 12);
        }
        for (int i = 0; i < 3; i++) s.aim[i] = s.muzzle[i] + dir[i] * s.dist;
        s.aimSet = true;
    }
    s.valid = true;
    g_LastShotWeapon[hand & 1] = weapon;
    g_ShotsNew++;
    return true;
}

void GunRay::Report(void* weapon, int hand, const Shot* s, const float* spawn, const float* aimPoint, const float* oldOrg, const float* oldAxis)
{
    if (!s || !s->valid) g_ShotsOld++;
    float m[3], ax[9];
    const bool pose = s && s->valid ? (memcpy(m, s->muzzle, 12), memcpy(ax, s->axis, 36), true) : (IsGunImpl(weapon) && PoseImpl(hand, m, ax, weapon));
    float bolt[3] = { aimPoint[0] - spawn[0], aimPoint[1] - spawn[1], aimPoint[2] - spawn[2] }; Normalize(bolt);
    uint8_t* ent = IsGunImpl(weapon) ? *(uint8_t**)((uint8_t*)weapon + kCpntEntity) : nullptr;
    float sp[3] = {}; if (IsGunImpl(weapon)) memcpy(sp, (uint8_t*)weapon + kSpawnPos, 12);
    float t = 0; bool hit = false; const bool sens = SensorHitImpl(t, hit);
    std::string gun = "none";
    if (pose)
    {
        const float d[3] = { spawn[0] - m[0], spawn[1] - m[1], spawn[2] - m[2] };
        float H[3]; for (int i = 0; i < 3; i++) H[i] = m[i] + ax[i] * (sens ? t : 10.f);
        // How far the bolt passes from the point the gun ray (and the new reticle) marks, at that point.
        const float miss = LineDist(H, spawn, bolt);
        gun = fmt::format("muzzle ({:.3f},{:.3f},{:.3f}) fwd ({:.3f},{:.3f},{:.3f}) spawn-muzzle {:.1f} cm boltangle {:.2f} deg mark {} t {:.2f} miss {:.1f} cm",
                          m[0], m[1], m[2], ax[0], ax[1], ax[2], sqrtf(Dot(d, d)) * 100.f, AngleDeg(bolt, ax), sens ? (hit ? "hit" : "range") : "none", t, miss * 100.f);
    }
    // the reticle the user saw before this change: the controller ray, ring 8 m out
    std::string old = "none";
    float ro[3], rf[3];
    if (GpuTrace::HandRay(hand, ro, rf))
    {
        float ring[3]; for (int i = 0; i < 3; i++) ring[i] = ro[i] + rf[i] * 8.f;
        old = fmt::format("({:.3f},{:.3f},{:.3f}) fwd ({:.3f},{:.3f},{:.3f}) bolt-vs-ring8m {:.1f} cm", ro[0], ro[1], ro[2], rf[0], rf[1], rf[2], LineDist(ring, spawn, bolt) * 100.f);
    }
    float eye[3], cax[9]; const bool cam = HandAimHook::RealCamera(eye, cax);
    std::string spread = "n/a";
    if (cam && g_LastAim.w == weapon && GetTickCount64() - g_LastAim.at < 200)
    {   // how far the game's spread step turned the aim point (seen from the head eye it does it through)
        const float* in = s && s->valid ? s->aimIn : aimPoint;
        const float a[3] = { g_LastAim.out[0] - g_LastAim.eye[0], g_LastAim.out[1] - g_LastAim.eye[1], g_LastAim.out[2] - g_LastAim.eye[2] };
        const float b[3] = { in[0] - g_LastAim.eye[0], in[1] - g_LastAim.eye[1], in[2] - g_LastAim.eye[2] };
        spread = fmt::format("{:.2f} deg", AngleDeg(a, b));
    }
    Target tg; { std::lock_guard lk(g_SensM); tg = g_Target; }
    if (tg.valid)
    {
        const float d[3] = { tg.p[0] - spawn[0], tg.p[1] - spawn[1], tg.p[2] - spawn[2] };
        gun += fmt::format(" | target ({:.3f},{:.3f},{:.3f}) bolt passes {:.1f} cm from it at {:.2f} m", tg.p[0], tg.p[1], tg.p[2], LineDist(tg.p, spawn, bolt) * 100.f, sqrtf(Dot(d, d)));
    }
    {   // the shot's muzzle against the one the last VIEW drew (walking, turning: how far the fire and the picture differ)
        float dm[3], df[3];
        if (pose && GunRay::DrawnRay(hand, dm, df))
        {
            const float d[3] = { dm[0] - m[0], dm[1] - m[1], dm[2] - m[2] };
            gun += fmt::format(" | drawn muzzle {:.1f} cm, {:.2f} deg away", sqrtf(Dot(d, d)) * 100.f, AngleDeg(df, ax));
        }
    }
    gun += fmt::format(" | ring px {:.4f} {:.4f}", g_RingX.load(), g_RingY.load());
    LOGI("GunShot: {} hand {} weapon {} {} ent {} sp ({:.3f},{:.3f},{:.3f}) spawn ({:.3f},{:.3f},{:.3f}) aim ({:.3f},{:.3f},{:.3f}) bolt ({:.3f},{:.3f},{:.3f}) | gun {} | oldreticle {} | firecam ({:.3f},{:.3f},{:.3f}) | eye ({:.3f},{:.3f},{:.3f}) | gamespread {}",
         s && s->valid ? "NEW" : "OLD", hand, weapon, IsGunImpl(weapon) ? (*(uintptr_t*)weapon == GameInfo::Base() + kVtGun ? "gun" : "crossbow") : "other", (void*)ent,
         sp[0], sp[1], sp[2], spawn[0], spawn[1], spawn[2], aimPoint[0], aimPoint[1], aimPoint[2], bolt[0], bolt[1], bolt[2], gun, old,
         oldOrg ? oldOrg[0] : 0.f, oldOrg ? oldOrg[1] : 0.f, oldOrg ? oldOrg[2] : 0.f, cam ? eye[0] : 0.f, cam ? eye[1] : 0.f, cam ? eye[2] : 0.f, spread);
    (void)oldAxis;
}

//! `vr gun...` and the research commands of the gun ray (Commands.cpp hands them over: its else-if chain is at the
//! compiler's nesting limit).
bool GunRay::Command(const std::string& what, std::istringstream& ss)
{
    if (what == "gunray") { int m = 63; ss >> m; GunRay::SetMode(m); }
    else if (what == "gunspread") { int m = 0; ss >> m; GunRay::SetSpread(m); }
    else if (what == "gunlog") { int n = 10; ss >> n; GunRay::Log(n); }
    else if (what == "gunprobe") GunRay::Probe();
    else if (what == "guntrack") { int m = 1; ss >> m; GunRay::SetTrack(m); }
    else if (what == "gunnolock") { int m = 1; ss >> m; GunRay::SetNoLock(m); }
    else if (what == "swordjoints") { int m = 1; ss >> m; g_SwordJoints = m; LOGI("GunRay: sword joint queries {}", m ? "answered as DRAWN" : "left to the game"); }
    else if (what == "swordjointlog") { int n = 60; ss >> n; g_SwordJointLog = n; }
    else if (what == "nearents") { float r = 1.5f; ss >> r; GunRay::NearEntities(r); }
    else if (what == "gunfreeze") { int ms = 60; ss >> ms; GunRay::FreezeAfterShot(ms); }
    else if (what == "gunfxblock") { int k = 1; ss >> k; GunRay::BlockFx(k); }
    else if (what == "gunattach") { int n = 6; ss >> n; GunRay::ProbeAttachment(n); }
    else if (what == "gunparts") { int n = 24; ss >> n; GpuTrace::RequestParts(n); }
    else if (what == "gunkids") { float r = 1.5f; ss >> r; GunRay::ProbeKids(r); }
    else if (what == "guncull") { int m = 1; ss >> m; g_CullFix = m; LOGI("GunRay: held equipment culled {}", m ? "by its flat AND drawn pose" : "by the flat pose only (the game's)"); }
    else if (what == "gunfxlog") { int n = 30; ss >> n; GunRay::LogFx(n); }
    else if (what == "gunfxwatch") { int n = 20; ss >> n; GunRay::WatchFx(n); }
    else if (what == "gunburst") { int n = 12, k = 1; ss >> n >> k; g_ShotBurstStride = k < 1 ? 1 : k; GunRay::BurstNextShot(n); }
    else if (what == "gunpix") { float x = -1.f, y = 0.5f; ss >> x >> y; if (x < 0.f) GunRay::ClearTarget(); else GunRay::Pixel(x, y); }
    else if (what == "gunsweep") { std::string m; float r = 0; ss >> m >> r; GunRay::SetSweep((uint32_t)std::stoul(m.empty() ? "0x20000" : m, nullptr, 0), r); }
    else if (what == "gpushaderhash") { std::string w; ss >> w; GpuTrace::DumpShaderByHash(std::strtoull(w.c_str(), nullptr, 16)); }
    else return false;
    return true;
}

std::string GunRay::Stats()
{
    float t = 0; bool hit = false; const bool sens = SensorHitImpl(t, hit);
    std::lock_guard lk(g_RegM);
    return fmt::format("cull {} widened {} shift {:.2f} | swordjoints {} q {} moved {} | ", g_CullFix.load(), g_CullWidened.load(), g_CullShift.load(), g_SwordJoints.load(), g_SwordJointQ.load(), g_SwordJointMoved.load()) + fmt::format("ring {:.4f} {:.4f} | spawn kept {} fixed {} blocked {} | blasts {} moved {} | joints moved {} bound-left {} gap {:.3f} fx {} starts {} | ", g_RingX.load(), g_RingY.load(), g_SpawnKept.load(), g_SpawnFixed.load(), g_SpawnBlocked.load(), g_Blasts.load(), g_BlastsMoved.load(), g_JointMoved.load(), g_JointBoundLeft.load(), g_JointLastGap.load(), g_FxMoved.load(), g_FxStartsPlaced.load()) + fmt::format("mode {} spread {} submits {} aimed {} results {} hits {} sweeps {} hits {} sensorused {} now {} t {:.2f} aimset {} shots new {} old {} nopose {} comps {} sp L {} R {}",
                       g_Mode.load(), g_Spread.load(), g_Submits.load(), g_Aimed.load(), g_Results.load(), g_Hits.load(), g_Sweeps.load(), g_SweepHits.load(), g_SensorUsed.load(),
                       sens ? (hit ? "hit" : "range") : "none", t, g_AimSet.load(), g_ShotsNew.load(), g_ShotsOld.load(), g_NoPose.load(), g_NComps,
                       g_Cache[0].valid ? fmt::format("({:.3f},{:.3f},{:.3f})", g_Cache[0].sp[0], g_Cache[0].sp[1], g_Cache[0].sp[2]) : std::string("-"),
                       g_Cache[1].valid ? fmt::format("({:.3f},{:.3f},{:.3f})", g_Cache[1].sp[0], g_Cache[1].sp[1], g_Cache[1].sp[2]) : std::string("-"));
}
