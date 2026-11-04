#include "pch.h"
#include "WeaponPose.h"
#include "GameInfo.h"
#include "HandAimHook.h"
#include "BodyHide.h"
#include "GpuTrace.h"
#include "Game.h"
#include <detours/detours.h>
#include <cmath>
#include <unordered_set>

namespace
{
    using Fn = void* (*)(void*, void*, void*, void*);
    Fn g_Real = nullptr;
    std::atomic<int> g_Enabled{ 0 };   // 0 off, 1 weapon models, 2 arm models, 3 both
    std::atomic<bool> g_Research{ false };
    std::mutex g_M; WeaponPose::Stats g_Stats;
    // the two per-call counters, outside the lock: both hooks run for every model / component of the frame, on every
    // render worker (2026-11-03, CpuProfile: their lock and Hook's VirtualQuery calls were the mod's largest waits there)
    std::atomic<unsigned long long> g_CallsA{ 0 }, g_TsetCallsA{ 0 };
    struct Grip { float rot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; float t[3] = {}; };
    // Measured (idle animation): weapon grip relative to the FP anchor, in the anchor frame (fwd, left, up):
    // left hand ~ (+0.31, +0.28, -0.21), right hand ~ (+0.285, -0.23, -0.23). The anchor is placed at controller - offset.
    Grip g_Grip[2] = { { { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, { -0.31f, -0.28f, 0.21f } }, { { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, { -0.285f, 0.23f, 0.23f } } };
    std::unordered_set<void*> g_Logged;

    bool Readable(const void* p, size_t n)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    // r = a * b for 3x3 row-major (rows = basis vectors): world = ctrlAxis rows composed with a local rotation
    void Mul3(const float a[9], const float b[9], float r[9])
    {
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[i * 3 + j] = a[i * 3 + 0] * b[0 * 3 + j] + a[i * 3 + 1] * b[1 * 3 + j] + a[i * 3 + 2] * b[2 * 3 + j];
    }

    //! Controller pose with grip offset for a hand. False when inactive.
    bool GripPose(int hand, float org[3], float a2[9])
    {
        float axis[9];
        if (!HandAimHook::ControllerPose(hand, org, axis)) return false;
        Grip g; { std::lock_guard lk(g_M); g = g_Grip[hand]; }
        Mul3(g.rot, axis, a2);
        for (int i = 0; i < 3; i++) org[i] += a2[0 + i] * g.t[0] + a2[3 + i] * g.t[1] + a2[6 + i] * g.t[2];
        return true;
    }
    // arkTransformSet::SetOriginAndAxis: the render transform of every model. Weapon models get the controller pose.
    using FnSet = void (*)(void*, const float*, const float*);
    FnSet g_RealSet = nullptr;
    void SetOriginAxisHook(void* self, const float* org, const float* axis)
    {
        g_TsetCallsA.fetch_add(1, std::memory_order_relaxed);
        if (g_Enabled.load())
        {
            int hand = -1;
            int mode = g_Enabled.load();
            for (int h = 0; h < 2 && hand < 0; h++) for (int k = 0; k < 2; k++)
                if (self && (((mode & 1) && self == BodyHide::WeaponTransformSet(h, k)) || ((mode & 2) && self == BodyHide::ArmTransformSet(h, k)))) { hand = h; break; }
            float o[3], a[9];
            if (hand >= 0 && (mode & 4)) { std::lock_guard lk(g_M); g_Stats.tsetApplied++; return; }   // research: freeze the transform set
            // Mirror the pose the GPU rewrite is actually drawing the model at, so the muzzle flash sits on the
            // barrel by construction and keeps sitting there when the grip is trimmed. Only when that is
            // unavailable (6DOF off) does the local grip below stand in.
            if (hand >= 0 && GpuTrace::WeaponTargetPose(hand, o, a))
            { g_RealSet(self, o, a); std::lock_guard lk(g_M); g_Stats.tsetApplied++; return; }
            if (hand >= 0 && GripPose(hand, o, a)) { g_RealSet(self, o, a); std::lock_guard lk(g_M); g_Stats.tsetApplied++; return; }
        }
        g_RealSet(self, org, axis);
    }

    void* Hook(void* self, void* b, void* c, void* d)
    {
        void* r = g_Real(self, b, c, d);
        g_CallsA.fetch_add(1, std::memory_order_relaxed);
        // Everything below serves the research log or modes 8 / 16 (the VR package runs mode 1, which only needs
        // SetOriginAxisHook): with neither, nothing to do -- before, every component's update paid three VirtualQuery
        // calls and the lock first.
        if (!g_Research.load(std::memory_order_relaxed) && !(g_Enabled.load(std::memory_order_relaxed) & (8 | 16))) return r;
        uint8_t* cp = (uint8_t*)self;
        if (!Readable(cp, 0xE0)) return r;
        uint8_t* ent = *(uint8_t**)(cp + WeaponPose::kCpntEntity);
        uint8_t* parent = *(uint8_t**)(cp + WeaponPose::kCpntParent);
        if (!Readable(ent, 0x340)) return r;
        bool rooted = parent == cp + 0x20;
        uint8_t* parentEnt = (!rooted && Readable(parent, 0x20)) ? *(uint8_t**)(parent + WeaponPose::kCpntEntity) : nullptr;
        void* larm = BodyHide::LeftArmEntity(); void* rarm = BodyHide::RightArmEntity();

        if (g_Research.load())
        {
            uint8_t* cam = (uint8_t*)HandAimHook::CameraObject();
            if (cam && Readable(cam + HandAimHook::kCamOrigin, 12))
            {
                const float* eye = (const float*)(cam + HandAimHook::kCamOrigin); const float* o = (const float*)(ent + WeaponPose::kEntityOrigin);
                float dx = o[0] - eye[0], dy = o[1] - eye[1], dz = o[2] - eye[2];
                if (dx * dx + dy * dy + dz * dz < 2.25f)
                {
                    std::lock_guard lk(g_M);
                    if (g_Logged.size() < 64 && g_Logged.insert(ent).second)
                        LOGI("WeaponPose: entity {} (vt exe+0x{:X}) cpnt {} parentProv {} rooted={} parentEnt {} (larm {} rarm {} player? ) org=({:.2f},{:.2f},{:.2f}) spawnId={}",
                             (void*)ent, *(uintptr_t*)ent - GameInfo::Base(), (void*)cp, (void*)parent, rooted, (void*)parentEnt, larm, rarm, o[0], o[1], o[2], *(int*)(ent + 0xA8));
                }
            }
        }
        if (!g_Enabled.load()) return r;
        if ((g_Enabled.load() & 8) && ent == (uint8_t*)Game::LocalPlayer())
        {   // research: rotate the player entity axis by the right-hand grip yaw and shift it by the grip translation
            float* eo = (float*)(ent + WeaponPose::kEntityOrigin); float* ea = (float*)(ent + WeaponPose::kEntityAxis);
            Grip g; { std::lock_guard lk(g_M); g = g_Grip[1]; }
            float a2[9]; Mul3(g.rot, ea, a2); memcpy(ea, a2, 36);
            for (int i = 0; i < 3; i++) eo[i] += a2[0 + i] * g.t[0] + a2[3 + i] * g.t[1] + a2[6 + i] * g.t[2];
            memcpy(cp + WeaponPose::kCpntOrigin, eo, 12); memcpy(cp + WeaponPose::kCpntAxis, ea, 36);
            std::lock_guard lk(g_M); g_Stats.appliedR++;
            return r;
        }
        // Bit 16 = also move the equipment ENTITY. Off by default because GpuTrace identifies the weapon's draws by
        // how close they are to that entity, and the draws do NOT follow it (their placement comes from the
        // camera-relative joint palette). Moving it therefore hides the weapon from the identification: measured,
        // gpu6dof_weapondraws fell from 26 to 5 and the weapon came apart.
        if (!(g_Enabled.load() & 16)) return r;
        void* lw = BodyHide::LeftWeaponEntity(); void* rw = BodyHide::RightWeaponEntity();
        int hand = (lw && ent == lw) ? 0 : (rw && ent == rw) ? 1 : -1;
        if (hand < 0) return r;
        float org[3], axis[9];
        if (!HandAimHook::ControllerPose(hand, org, axis)) return r;
        float a2[9]; Grip g; { std::lock_guard lk(g_M); g = g_Grip[hand]; }
        Mul3(g.rot, axis, a2);                       // local grip rotation, then controller orientation
        for (int i = 0; i < 3; i++) org[i] += a2[0 + i] * g.t[0] + a2[3 + i] * g.t[1] + a2[6 + i] * g.t[2];
        memcpy(ent + WeaponPose::kEntityOrigin, org, 12); memcpy(ent + WeaponPose::kEntityAxis, a2, 36);
        memcpy(cp + WeaponPose::kCpntOrigin, org, 12); memcpy(cp + WeaponPose::kCpntAxis, a2, 36);
        std::lock_guard lk(g_M);
        if (hand == 0) { g_Stats.appliedL++; g_Stats.lastL = ent; } else { g_Stats.appliedR++; g_Stats.lastR = ent; }
        return r;
    }
}

bool WeaponPose::Install()
{
    if (!GameInfo::IsSupportedBuild()) return false;
    g_Real = (Fn)(GameInfo::Base() + kTransformUpdate);
    g_RealSet = (FnSet)(GameInfo::Base() + kSetOriginAxis);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_Real, Hook);
    DetourAttach(&(PVOID&)g_RealSet, SetOriginAxisHook);
    LONG r = DetourTransactionCommit();
    LOGI("WeaponPose: transform update@exe+0x{:X} detour={}", kTransformUpdate, r);
    return r == NO_ERROR;
}
void WeaponPose::Uninstall()
{
    if (!g_Real) return;
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)g_Real, Hook); DetourDetach(&(PVOID&)g_RealSet, SetOriginAxisHook); DetourTransactionCommit();
}
void WeaponPose::ApplyGrip(int hand, float org[3], float axis[9])
{
    Grip g; { std::lock_guard lk(g_M); g = g_Grip[hand & 1]; }
    float a2[9]; Mul3(g.rot, axis, a2); memcpy(axis, a2, 36);
    for (int i = 0; i < 3; i++) org[i] += axis[0 + i] * g.t[0] + axis[3 + i] * g.t[1] + axis[6 + i] * g.t[2];
}
void WeaponPose::SetEnabled(int mode) { g_Enabled = mode; }
int WeaponPose::Enabled() { return g_Enabled; }
void WeaponPose::SetResearchLog(bool on) { g_Research = on; if (on) { std::lock_guard lk(g_M); g_Logged.clear(); } }
void WeaponPose::SetGrip(int hand, float pitchDeg, float yawDeg, float rollDeg, float fwd, float left, float up)
{
    // rotation matrix rows = rotated basis (fwd/left/up) expressed in the controller frame; yaw about up, pitch about left, roll about fwd
    float p = pitchDeg * 0.0174533f, y = yawDeg * 0.0174533f, rl = rollDeg * 0.0174533f;
    float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p), cr = cosf(rl), sr = sinf(rl);
    // id Tech idAngles::ToMat3 convention (rows fwd/left/up)
    float m[9] = {
        cp * cy,                 cp * sy,                 -sp,
        sr * sp * cy + cr * -sy, sr * sp * sy + cr * cy,  sr * cp,
        cr * sp * cy + -sr * -sy, cr * sp * sy + -sr * cy, cr * cp };
    std::lock_guard lk(g_M);
    memcpy(g_Grip[hand & 1].rot, m, 36); g_Grip[hand & 1].t[0] = fwd; g_Grip[hand & 1].t[1] = left; g_Grip[hand & 1].t[2] = up;
}
WeaponPose::Stats WeaponPose::GetStats() { std::lock_guard lk(g_M); Stats s = g_Stats; s.calls = g_CallsA.load(); s.tsetCalls = g_TsetCallsA.load(); return s; }
