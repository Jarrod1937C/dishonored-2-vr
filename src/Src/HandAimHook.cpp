#include "pch.h"
#include "HandAimHook.h"
#include "Collision.h"
#include "GameInfo.h"
#include "VRRuntime.h"
#include "XRSession.h"
#include "VRMath.h"
#include "Game.h"
#include "KbmInject.h"
#include "BodyHide.h"
#include "TestAids.h"
#include "WeaponPose.h"
#include "GunRay.h"
#include <detours/detours.h>
#include <cmath>

namespace
{
    using Fn = void* (*)(void*, void*, void*, void*);
    std::atomic<int> g_Mode{ 0 };
    std::atomic<int> g_GunHand{ 0 };
    std::atomic<int> g_PowerHand{ 0 };
    // Only the throw-preview sensor (2). Far Reach and the other powers follow the hand through the aim override
    // alone (measured: identical landing point with mask 0), and every extra swap window is a chance for another
    // thread to read the hand as the camera.
    // 64: the mine / razor placement ray (2026-10-01), so a mine goes where the hand points like everything else.
    std::atomic<int> g_PowerAimMask{ 2 | 64 };
    std::atomic<int> g_AimOverride{ 1 };
    std::atomic<void*> g_Cam{ nullptr };
    std::atomic<void*> g_AimCpnt{ nullptr };
    std::atomic<void*> g_Gun{ nullptr };
    std::atomic<unsigned long long> g_GetAimCalls{ 0 };
    std::atomic<float> g_CamTest{ 0.f }, g_CamTest2{ 0.f };
    std::atomic<int> g_FpAnchor{ 0 }, g_AnchorHandCfg{ -1 }, g_AnchorHand{ 0 }, g_ArmTest{ 0 };
    std::atomic<bool> g_ArmsFollow{ false };
    float g_SavedLimits[18] = {}; uint8_t g_SavedFollow = 0; bool g_LimitsSaved = false, g_LimitsWidened = false;
    LARGE_INTEGER g_LastMelee{};
    float g_RealCamOrg[3] = {}, g_RealCamAxis[9] = {}; std::atomic<bool> g_RealCamValid{ false };   // main thread
    std::mutex g_M; HandAimHook::Stats g_Stats;

    //! idMat3 (rows fwd/left/up) = yaw(bodyFwd) * controllerRotation.
    void HandAxis(const float bodyFwd[3], const float q[4], float out[9])
    {
        float yaw = atan2f(bodyFwd[1], bodyFwd[0]);
        float cy = cosf(yaw), sy = sinf(yaw);
        auto toWorld = [&](const float l[3], float o[3]) { o[0] = l[0] * cy - l[1] * sy; o[1] = l[0] * sy + l[1] * cy; o[2] = l[2]; };
        float fx[3], rx[3], ux[3]; VRMath::QuatBasis(q, fx, rx, ux);
        float fl[3], rl[3], ul[3]; VRMath::XrToId(fx, fl); VRMath::XrToId(rx, rl); VRMath::XrToId(ux, ul);
        float fw[3], rw[3], uw[3]; toWorld(fl, fw); toWorld(rl, rw); toWorld(ul, uw);
        out[0] = fw[0]; out[1] = fw[1]; out[2] = fw[2];
        out[3] = -rw[0]; out[4] = -rw[1]; out[5] = -rw[2];
        out[6] = uw[0]; out[7] = uw[1]; out[8] = uw[2];
    }
    //! Yaw (radians, about +Y, positive = left) of an XR quaternion's forward (-Z).
    float XrYaw(const float q[4])
    {
        float fx[3], rx[3], ux[3]; VRMath::QuatBasis(q, fx, rx, ux);
        return atan2f(-fx[0], -fx[2]);
    }
    //! out = rotY(-yaw) * q
    void RemoveYaw(const float q[4], float yaw, float out[4])
    {
        float h = -yaw * 0.5f, s = sinf(h), c = cosf(h);       // r = (0, s, 0, c)
        out[0] = c * q[0] + s * q[2];
        out[1] = c * q[1] + s * q[3];
        out[2] = c * q[2] - s * q[0];
        out[3] = c * q[3] - s * q[1];
    }
    //! Controller pose in game world space, relative to the game eye (eyeOrg) and body frame (yaw of viewAxis).
    //! `eyePosXr` (optional) is the XR-space position the caller's anchor was built from; without it the offset is
    //! taken from the head midpoint, which is what the game camera corresponds to.
    bool CtrlPose(int hand, const float eyeOrg[3], const float viewAxis[9], float org[3], float axis[9],
                  const float* eyePosXr = nullptr, const XRInputState* given = nullptr, const float* headQuat = nullptr)
    {
        XRInputState in;
        if (given) in = *given; else if (!VRRuntime::GetInput(in)) return false;
        if (!in.hand[hand].valid) return false;
        VRRuntime::HeadPose hp = VRRuntime::GetHeadPose(); if (!hp.valid) return false;
        if (headQuat) memcpy(hp.quat, headQuat, 16);
        const float* ref = eyePosXr ? eyePosXr : hp.pos;
        float q[4] = { in.hand[hand].quatX, in.hand[hand].quatY, in.hand[hand].quatZ, in.hand[hand].quatW };
        float d[3] = { in.hand[hand].posX - ref[0], in.hand[hand].posY - ref[1], in.hand[hand].posZ - ref[2] };
        if (KbmInject::HeadAim())
        {   // the game yaw already follows the HMD: express hand pose in the HMD-yaw frame
            float yaw = XrYaw(hp.quat), qq[4]; RemoveYaw(q, yaw, qq); memcpy(q, qq, 16);
            float c = cosf(-yaw), s = sinf(-yaw); float dx = d[0] * c + d[2] * s, dz = -d[0] * s + d[2] * c; d[0] = dx; d[2] = dz;
        }
        HandAxis(viewAxis, q, axis);
        float yaw = atan2f(viewAxis[1], viewAxis[0]), cy = cosf(yaw), sy = sinf(yaw);
        float dl[3]; VRMath::XrToId(d, dl);
        org[0] = eyeOrg[0] + dl[0] * cy - dl[1] * sy; org[1] = eyeOrg[1] + dl[0] * sy + dl[1] * cy; org[2] = eyeOrg[2] + dl[2];
        return true;
    }
    //! An XR-space displacement into the world, the same way CtrlPose places the controller: HMD yaw removed when the
    //! game yaw already tracks it, then XR->id axes, then the view's yaw. Used for head-minus-eye offsets.
    void XrOffsetToWorldImpl(const float dXr[3], const float viewAxis[9], float out[3], const float* headQuat = nullptr)
    {
        float d[3] = { dXr[0], dXr[1], dXr[2] };
        if (KbmInject::HeadAim())
        {
            VRRuntime::HeadPose hp = VRRuntime::GetHeadPose();
            if (headQuat) { memcpy(hp.quat, headQuat, 16); hp.valid = true; }
            if (hp.valid) { float yaw = XrYaw(hp.quat); float c = cosf(-yaw), s = sinf(-yaw); float dx = d[0] * c + d[2] * s, dz = -d[0] * s + d[2] * c; d[0] = dx; d[2] = dz; }
        }
        float yaw = atan2f(viewAxis[1], viewAxis[0]), cy = cosf(yaw), sy = sinf(yaw);
        float dl[3]; VRMath::XrToId(d, dl);
        out[0] = dl[0] * cy - dl[1] * sy; out[1] = dl[0] * sy + dl[1] * cy; out[2] = dl[2];
    }
    //! Controller pose from the current camera component. False when inactive / no tracking.
    //! Real (game) camera pose: the saved copy when the FP anchor rewrites the camera fields, else the fields themselves.
    //! g_Cam is captured from the camera component's update and never cleared, so across a level load it points at
    //! freed memory until the new component ticks. Reading it unguarded from the render thread crashed the game
    //! during a load - so validate before dereferencing, rather than trusting the pointer to still be live.
    bool Readable(const void* p, size_t n)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    bool RealCam(const float*& org, const float*& axis)
    {
        uint8_t* cam = (uint8_t*)g_Cam.load(); if (!cam) return false;
        // Prefer the copy taken right after the engine's last camera writer. The live fields are briefly REPLACED by
        // the controller pose while a hooked call runs (CamSwap), on whatever thread that call is on; a reader on
        // another thread that lands inside that window gets the hand instead of the head. Measured: the head-aim
        // pitch servo read a hand pitched 40 deg down as the camera and tilted the view 40 deg up.
        if (g_RealCamValid.load()) { org = g_RealCamOrg; axis = g_RealCamAxis; }
        else
        {
            if (!Readable(cam + HandAimHook::kCamAxis, 36)) return false;
            org = (const float*)(cam + HandAimHook::kCamOrigin); axis = (const float*)(cam + HandAimHook::kCamAxis);
        }
        return true;
    }
    bool ActivePoseFor(int hand, float org[3], float axis[9])
    {
        const float* co; const float* ca;
        if (g_Mode.load() < 2 || !RealCam(co, ca)) return false;
        return CtrlPose(hand, co, ca, org, axis);
    }
    bool ActivePose(float org[3], float axis[9]) { return ActivePoseFor(g_GunHand.load(), org, axis); }

    //! For the duration of one engine call, the camera eye/axis, the player's first-person origin/axis and the
    //! aim component's direction/position READ AS the controller pose of `hand`; the destructor puts every field
    //! back. This is what ComputeAimTarget has always done for the fire ray, factored out so the power targeting
    //! update and the throw-preview sensor can do the same. The pointers are validated before every use: they are
    //! learned from an update and never cleared, so across a level load they address freed memory.
    //! Every CamSwap holds this for as long as the fields read as the hand, and the camera's own per-frame steps that
    //! read them (the post-shake snapshot the view is rendered from, the renderView copy) take it too. Without it the
    //! sensors' swaps, which run on the job threads every frame, landed inside the snapshot: measured, with the left
    //! wrist rolled 70 deg one frame in four was rendered from the left controller (eye 1.03 m lower, yaw 44 deg off),
    //! and a held object hung off that camera jumped 1.2 m a step. Two swaps on two threads also saved each other's
    //! hand pose as "the real camera". Recursive: a hooked call may run inside another on the same thread.
    std::recursive_mutex g_SwapMx;
    struct CamSwap
    {
        uint8_t* cam = nullptr; uint8_t* pl = nullptr; uint8_t* ac = nullptr;
        float sCam[12], sPl[12], sAc[6], sAim[12]; float org[3], axis[9];
        bool applied = false, aimSwapped = false;
        std::unique_lock<std::recursive_mutex> lk{ g_SwapMx, std::defer_lock };
        explicit CamSwap(int hand) { if (hand >= 0) Apply(hand, nullptr, nullptr); }
        //! An explicit pose instead of a controller's (GunRay: the fire reads the gun ray).
        CamSwap(const float* o, const float* a) { Apply(-1, o, a); }
        void Apply(int hand, const float* o, const float* a)
        {
            lk.lock();
            cam = (uint8_t*)g_Cam.load(); ac = (uint8_t*)g_AimCpnt.load(); pl = (uint8_t*)Game::LocalPlayer();
            if (!Readable(cam + HandAimHook::kCamAxis, 36)) cam = nullptr;
            if (ac && !Readable(ac + HandAimHook::kAimCpntPos, 12)) ac = nullptr;
            if (!cam || !pl || !Readable(pl + Game::kPlayerViewOrigin, 48)) return;
            if (o) { memcpy(org, o, 12); memcpy(axis, a, 36); }
            else if (!ActivePoseFor(hand, org, axis)) return;
            memcpy(sCam, cam + HandAimHook::kCamOrigin, 12); memcpy(sCam + 3, cam + HandAimHook::kCamAxis, 36);
            memcpy(sPl, pl + Game::kPlayerViewOrigin, 12); memcpy(sPl + 3, pl + Game::kPlayerViewAxis, 36);
            memcpy(cam + HandAimHook::kCamOrigin, org, 12); memcpy(cam + HandAimHook::kCamAxis, axis, 36);
            memcpy(pl + Game::kPlayerViewOrigin, org, 12); memcpy(pl + Game::kPlayerViewAxis, axis, 36);
            if (ac)
            {
                memcpy(sAc, ac + HandAimHook::kAimCpntWorldDir, 12); memcpy(sAc + 3, ac + HandAimHook::kAimCpntPos, 12);
                memcpy(ac + HandAimHook::kAimCpntWorldDir, axis, 12); memcpy(ac + HandAimHook::kAimCpntPos, org, 12);
            }
            // The camera's m_aimOrigin/m_aimAxis (UpdateAim's outputs). Weapons ignore them, but the Far Reach
            // sensor builds its ray from exactly these (measured: exe+0x3B6E98..0x3B6F69 copies cam+0x2527C and
            // cam+0x25288 into the sensor), so swapping the eye alone left the landing point on the view.
            if (Readable(cam + HandAimHook::kAimAxis, 36))
            {
                memcpy(sAim, cam + HandAimHook::kAimOrigin, 12); memcpy(sAim + 3, cam + HandAimHook::kAimAxis, 36);
                memcpy(cam + HandAimHook::kAimOrigin, org, 12); memcpy(cam + HandAimHook::kAimAxis, axis, 36);
                aimSwapped = true;
            }
            applied = true;
        }
        ~CamSwap()
        {
            if (!applied) return;
            memcpy(cam + HandAimHook::kCamOrigin, sCam, 12); memcpy(cam + HandAimHook::kCamAxis, sCam + 3, 36);
            memcpy(pl + Game::kPlayerViewOrigin, sPl, 12); memcpy(pl + Game::kPlayerViewAxis, sPl + 3, 36);
            if (ac) { memcpy(ac + HandAimHook::kAimCpntWorldDir, sAc, 12); memcpy(ac + HandAimHook::kAimCpntPos, sAc + 3, 12); }
            if (aimSwapped) { memcpy(cam + HandAimHook::kAimOrigin, sAim, 12); memcpy(cam + HandAimHook::kAimAxis, sAim + 3, 36); }
        }
    };

    bool ActivePoseFor(int hand, float org[3], float axis[9]);
    // ---- component discovery hooks ----
    Fn g_RealCamUpdate = nullptr;
    void* CamUpdateHook(void* self, void* b, void* c, void* d) { void* r = g_RealCamUpdate(self, b, c, d); g_Cam.store(self); return r; }
    Fn g_RealUpdateAim = nullptr;
    void* UpdateAimHook(void* self, void* b, void* c, void* d)
    {
        void* r = g_RealUpdateAim(self, b, c, d);
        g_Cam.store(self);
        // Powers aim with the camera's m_aimOrigin/m_aimAxis, which UpdateAim has just written from the view
        // angles. Thirty call sites in the exe read them (every player sensor, the power components, the
        // throwables' aim), and none of them is a weapon or the renderer -- so rewriting them here, once per
        // frame, makes every power target where the controller points without hooking each one. Measured first
        // on Far Reach: the sensor copies exactly these two fields into its ray (exe+0x3B6E98..0x3B6F69).
        if (g_AimOverride.load() && g_Mode.load() >= 2)
        {
            uint8_t* o = (uint8_t*)self; float org[3], axis[9];
            if (Readable(o + HandAimHook::kAimAxis, 36) && ActivePoseFor(g_PowerHand.load(), org, axis))
            {
                memcpy(o + HandAimHook::kAimOrigin, org, 12); memcpy(o + HandAimHook::kAimAxis, axis, 36);
                std::lock_guard lk(g_M); g_Stats.aimOverrides++;
            }
        }
        {   // arms follow the controllers (see SetArmsFollow): per-arm aiming angles relative to the view, limits widened
            uint8_t* o = (uint8_t*)self; float* lim = (float*)(o + HandAimHook::kLeftArmAiming);
            if (g_ArmsFollow.load())
            {
                if (!g_LimitsSaved) { memcpy(g_SavedLimits, lim, 72); g_LimitsSaved = true; }
                const float* co; const float* ca;
                if (RealCam(co, ca))
                {
                    for (int hand = 0; hand < 2; hand++)
                    {
                        float* arm = (float*)(o + (hand == 0 ? HandAimHook::kLeftArmAiming : HandAimHook::kRightArmAiming));
                        arm[0] = arm[1] = arm[2] = 3.0f;                 // yaw/yaw2/pitch limits (rad)
                        float org[3], axis[9];
                        if (!CtrlPose(hand, co, ca, org, axis)) continue;
                        // controller axis in the view frame: rel[i][j] = ctrlRow_i . viewRow_j
                        float rel[9];
                        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) rel[i * 3 + j] = axis[i * 3 + 0] * ca[j * 3 + 0] + axis[i * 3 + 1] * ca[j * 3 + 1] + axis[i * 3 + 2] * ca[j * 3 + 2];
                        float fz = rel[2] > 1 ? 1 : rel[2] < -1 ? -1 : rel[2];
                        float yaw = atan2f(rel[1], rel[0]);              // + = left (id convention)
                        float pitch = -asinf(fz);                        // + = down
                        float roll = atan2f(-rel[5], rel[8]);            // left vector z vs up vector z
                        arm[3] = hand == 0 ? -yaw : yaw; arm[4] = pitch; arm[5] = roll;   // measured: the left arm's yaw slot is mirrored
                    }
                }
                g_LimitsWidened = true;
            }
            else if (g_LimitsWidened) { memcpy(lim, g_SavedLimits, 72); g_LimitsWidened = false; }
        }
        if (int at = g_ArmTest.load())
        {   // research: orient the arms from the anchor-hand controller, relative to the (real) camera view
            uint8_t* o = (uint8_t*)self; const float* co; const float* ca; float org[3], axis[9];
            if (RealCam(co, ca) && CtrlPose(g_AnchorHand.load(), co, ca, org, axis))
            {
                // view-relative rotation: rel = viewAxis^T * ctrlAxis (rows = ctrl basis in view frame)
                float rel[9];
                for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) rel[i * 3 + j] = axis[i * 3 + 0] * ca[j * 3 + 0] + axis[i * 3 + 1] * ca[j * 3 + 1] + axis[i * 3 + 2] * ca[j * 3 + 2];
                float pitch = -asinf(rel[2] > 1 ? 1 : rel[2] < -1 ? -1 : rel[2]);   // ctrl fwd z in view frame -> pitch (id: positive = down)
                if (at & 1) *(float*)(o + HandAimHook::kArmsPitch) = pitch;
                if (at & 2) { memcpy(o + HandAimHook::kLeftArmAiming, rel, 36); memcpy(o + HandAimHook::kRightArmAiming, rel, 36); }
                if (at & 4) { memcpy(o + HandAimHook::kAimAxis, axis, 36); }
            }
        }
        BodyHide::Tick();                       // game thread, once per frame
        GunRay::Tick();                         // the gun ray's sweep: where the next shot would land
        Collision::Tick();                      // solid hands and weapons: what they meet this frame
        TestAids::Tick();
        { std::lock_guard lk(g_M); g_Stats.calls++; }
        return r;
    }
    Fn g_RealAimCpnt = nullptr;
    void* AimCpntHook(void* self, void* b, void* c, void* d) { g_AimCpnt.store(self); return g_RealAimCpnt(self, b, c, d); }
    Fn g_RealGetAim = nullptr;
    void* GetAimHook(void* self, void* org, void* axis, void* d) { g_GetAimCalls++; return g_RealGetAim(self, org, axis, d); }
    Fn g_RealShake = nullptr;
    void* ShakeHook(void* self, void* rv, void* c, void* d)
    {
        std::lock_guard swapLk(g_SwapMx);      // never shake or snapshot a camera that reads as the hand
        void* r = g_RealShake(self, rv, c, d);
        { std::lock_guard lk(g_M); g_Stats.shakeCalls++; }
        float dz = g_CamTest.load();
        uint8_t* cam = rv ? (uint8_t*)rv - HandAimHook::kCamRenderView : nullptr;
        if (dz != 0.f && cam) { ((float*)(cam + HandAimHook::kCamOrigin))[2] += dz; }
        if (cam)
        {
            memcpy(g_RealCamOrg, cam + HandAimHook::kCamOrigin, 12); memcpy(g_RealCamAxis, cam + HandAimHook::kCamAxis, 36); g_RealCamValid = true;
            if (g_FpAnchor.load() == 1)
            {
                float o[3], a[9];
                if (CtrlPose(g_GunHand.load(), g_RealCamOrg, g_RealCamAxis, o, a)) { memcpy(cam + HandAimHook::kCamOrigin, o, 12); memcpy(cam + HandAimHook::kCamAxis, a, 36); }
            }
        }
        return r;
    }
    // renderView_t copy(dst, src): when the source is the camera's renderView and the FP anchor is active, give the scene the real camera.
    Fn g_RealRvCopy = nullptr;
    void* RvCopyHook(void* dst, void* src, void* c, void* d)
    {
        std::lock_guard swapLk(g_SwapMx);
        void* r = g_RealRvCopy(dst, src, c, d);
        uint8_t* cam = (uint8_t*)g_Cam.load();
        int mode = g_FpAnchor.load();
        { std::lock_guard lk(g_M); g_Stats.rvCalls++; }
        if (mode && g_RealCamValid.load() && cam && src == cam + HandAimHook::kCamRenderView && dst)
        {
            { std::lock_guard lk(g_M); g_Stats.rvMatched++; }
            if (mode == 1)
            {
                memcpy((uint8_t*)dst + 0xA8, g_RealCamOrg, 12); memcpy((uint8_t*)dst + 0xCC, g_RealCamAxis, 36);
                std::lock_guard lk(g_M); g_Stats.rvApplied++;
                memcpy(g_Stats.rvOrg, g_RealCamOrg, 12); memcpy(g_Stats.rvFwd, g_RealCamAxis, 12);
            }
            else
            {   // mode 2: the scene renderView (FP anchor) takes the anchor-hand controller pose (with the grip offset so the
                // weapon lands on the controller); ViewMatrixHook renders from the real camera.
                int hand = g_AnchorHandCfg.load();
                if (hand < 0)
                {
                    XRInputState in; LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
                    if (VRRuntime::GetInput(in) && in.triggerR > 0.5f) g_LastMelee = now;
                    bool melee = g_LastMelee.QuadPart && (now.QuadPart - g_LastMelee.QuadPart) < f.QuadPart * 2;
                    hand = melee ? 1 : g_GunHand.load();
                }
                g_AnchorHand = hand;
                float o[3], a[9];
                if (CtrlPose(hand, g_RealCamOrg, g_RealCamAxis, o, a))
                {
                    WeaponPose::ApplyGrip(hand, o, a);
                    memcpy((uint8_t*)dst + 0xA8, o, 12); memcpy((uint8_t*)dst + 0xCC, a, 36);
                    std::lock_guard lk(g_M); g_Stats.rvApplied++;
                    memcpy(g_Stats.rvOrg, o, 12); memcpy(g_Stats.rvFwd, a, 12);
                }
                else { std::lock_guard lk(g_M); g_Stats.rvPoseFail++; }
            }
        }
        return r;
    }
    Fn g_RealCopyView = nullptr;
    void* CopyViewHook(void* self, void* b, void* c, void* d)
    {
        void* r = g_RealCopyView(self, b, c, d);
        float dz = g_CamTest2.load();
        if (dz != 0.f && self) ((float*)((uint8_t*)self + HandAimHook::kCamFpOrigin))[2] += dz;
        return r;
    }
    Fn g_RealGunSpawn = nullptr;
    void* GunSpawnHook(void* self, void* b, void* c, void* d)
    {
        void* r = g_RealGunSpawn(self, b, c, d);
        g_Gun.store(self);
        std::lock_guard lk(g_M); g_Stats.gunCalls++; memcpy(g_Stats.gunSpawn, (uint8_t*)self + 0xF4, 12);
        return r;
    }

    // ---- fire ray ----
    // GetAimPoint(provider, out, dist): re-target along the controller ray keeping the eye->target distance.
    using FnAimPoint = float* (*)(void*, float*, float);
    FnAimPoint g_RealGetAimPoint = nullptr;
    float* GetAimPointHook(void* provider, float* out, float dist)
    {
        float* r = g_RealGetAimPoint(provider, out, dist);
        { std::lock_guard lk(g_M); g_Stats.aimPointCalls++; }
        float org[3], axis[9];
        uint8_t* cam = (uint8_t*)g_Cam.load();
        if (!cam || !ActivePose(org, axis)) return r;
        const float* eye; const float* ea; if (!RealCam(eye, ea)) return r;
        float dd[3] = { out[0] - eye[0], out[1] - eye[1], out[2] - eye[2] };
        float n = sqrtf(dd[0] * dd[0] + dd[1] * dd[1] + dd[2] * dd[2]);
        if (n < 1.f) n = dist > 1.f ? dist : 30.f;
        for (int i = 0; i < 3; i++) out[i] = org[i] + axis[i] * n;
        std::lock_guard lk(g_M); g_Stats.aimPointApplied++; memcpy(g_Stats.lastAimPoint, out, 12); memcpy(g_Stats.ctrlOrg, org, 12); memcpy(g_Stats.ctrlFwd, axis, 12);
        return r;
    }

    // ComputeAimTarget(this, out spawnPos, viewAxis, aimPoint, flags, flags2): while it runs, the camera eye/axis, the
    // player's first-person origin/axis (muzzle placement) and the aim component read the controller pose.
    using FnTarget = bool (*)(void*, float*, const float*, const float*, uint64_t, uint64_t);
    FnTarget g_RealTarget = nullptr;
    //! For one engine call, an entity's world transform (+0x300 origin / +0x30C axis) reads as another pose.
    struct EntSwap
    {
        uint8_t* e = nullptr; float sOrg[3], sAxis[9];
        EntSwap(uint8_t* ent, const float* o, const float* a)
        {
            if (!ent || !Readable(ent + 0x300, 48)) return;
            e = ent; memcpy(sOrg, e + 0x300, 12); memcpy(sAxis, e + 0x30C, 36);
            memcpy(e + 0x300, o, 12); memcpy(e + 0x30C, a, 36);
        }
        ~EntSwap() { if (e) { memcpy(e + 0x300, sOrg, 12); memcpy(e + 0x30C, sAxis, 36); } }
    };
    bool ComputeAimTargetHook(void* self, float* out, const float* viewAxis, const float* aimPoint, uint64_t e, uint64_t f)
    {
        { std::lock_guard lk(g_M); g_Stats.targetCalls++; }
        const int hand = g_GunHand.load();
        {   // a gun or the crossbow: the shot goes along the weapon as it is DRAWN (GunRay). The projectile leaves from
            // the component's entity transform * its muzzle joint, so the entity reads as the drawn weapon for the call;
            // the camera fields read the gun ray (the engine's checks and its blocked-muzzle fallback use the eye and
            // its axis); and the aim point -- the caller's own local, which it turns into the projectile's direction
            // after this returns -- is put on the gun ray.
            GunRay::Shot shot;
            if (GunRay::Prepare(self, hand, aimPoint, shot))
            {
                bool r;
                {
                    CamSwap s(shot.eye, shot.axis);
                    EntSwap es(shot.ent, shot.entOrg, shot.entAxis);
                    if (shot.aimSet) memcpy(const_cast<float*>(aimPoint), shot.aim, 12);
                    r = g_RealTarget(self, out, shot.axis, aimPoint, e, f);
                    GunRay::FixSpawn(shot, out);
                    if (s.applied) { std::lock_guard lk(g_M); g_Stats.targetApplied++; }
                }
                GunRay::Report(self, hand, &shot, out, aimPoint, shot.eye, shot.axis);
                return r;
            }
        }
        CamSwap s(hand);
        if (!s.applied) { bool r0 = g_RealTarget(self, out, viewAxis, aimPoint, e, f); GunRay::Report(self, hand, nullptr, out, aimPoint, nullptr, nullptr); return r0; }
        bool r = g_RealTarget(self, out, s.axis, aimPoint, e, f);
        { std::lock_guard lk(g_M); g_Stats.targetApplied++; }
        GunRay::Report(self, hand, nullptr, out, aimPoint, s.org, s.axis);
        LOGI("FireRay: hand {} org=({:.2f},{:.2f},{:.2f}) fwd=({:.3f},{:.3f},{:.3f}) spawn=({:.2f},{:.2f},{:.2f}) aimPoint=({:.2f},{:.2f},{:.2f}) view fwd=({:.3f},{:.3f},{:.3f})",
             g_GunHand.load(), s.org[0], s.org[1], s.org[2], s.axis[0], s.axis[1], s.axis[2], out[0], out[1], out[2], aimPoint[0], aimPoint[1], aimPoint[2], s.sCam[3], s.sCam[4], s.sCam[5]);
        return r;
    }

    // ---- powers and the throw preview ----
    // The power component's per-frame targeting update (Far Reach: landing marker + destination) and the
    // throw-preview sensor both read the camera / player view and nothing we already hook. Wrapping each call in
    // a CamSwap makes them see the controller instead, and only them.
    using FnThis = void* (*)(void*);
    FnThis g_RealPowerTarget = nullptr;
    void* PowerTargetHook(void* self)
    {
        { std::lock_guard lk(g_M); g_Stats.powerCalls++; }
        const bool on = (g_PowerAimMask.load() & 1) != 0;
        CamSwap s(on ? g_PowerHand.load() : -1);
        void* r = g_RealPowerTarget(self);
        uint8_t* p = (uint8_t*)self;
        std::lock_guard lk(g_M);
        if (s.applied) { g_Stats.powerApplied++; memcpy(g_Stats.powerOrg, s.org, 12); memcpy(g_Stats.powerFwd, s.axis, 12); }
        // Every owned power of that kind runs this update each frame (with all of them given, several), so only the
        // one that is targeting -- or the last one that was, to report it going idle -- may write the stats.
        static void* lastActive = nullptr;
        if (Readable(p + 0x1D0, 4) && (p[0xA4] || self == lastActive))
        {   // where the power thinks it is going. +0xA4 = targeting active; read from the FX update (exe+0x4430D0):
            // +0x1B4 = the tendril's START (the hand joint, see HandFx), +0x1C0 = the curve's middle control point,
            // blended between that start and the end, +0x108 = the END, the sensor's landing point (copied to +0x270).
            if (p[0xA4]) lastActive = self;
            g_Stats.powerActive = p[0xA4];
            memcpy(g_Stats.powerHit, p + 0x1B4, 12); memcpy(g_Stats.powerTarget, p + 0x1C0, 12); memcpy(g_Stats.powerEnd, p + 0x108, 12);
            static unsigned logged = 0;
            if (g_Stats.powerActive && logged++ % 60 == 0)
                LOGI("PowerAim: active swap={} hand {} org=({:.2f},{:.2f},{:.2f}) fwd=({:.3f},{:.3f},{:.3f}) hit=({:.2f},{:.2f},{:.2f}) dest=({:.2f},{:.2f},{:.2f}) cpnt {}",
                     s.applied, g_PowerHand.load(), s.org[0], s.org[1], s.org[2], s.axis[0], s.axis[1], s.axis[2],
                     g_Stats.powerHit[0], g_Stats.powerHit[1], g_Stats.powerHit[2], g_Stats.powerTarget[0], g_Stats.powerTarget[1], g_Stats.powerTarget[2], self);
        }
        return r;
    }
    // The sensors are virtual methods reached through their vtable; keep the full register convention.
    Fn g_RealThrowHit = nullptr;
    void* ThrowHitHook(void* self, void* b, void* cc, void* d)
    {
        { std::lock_guard lk(g_M); g_Stats.throwCalls++; }
        const bool on = (g_PowerAimMask.load() & 2) != 0;
        CamSwap s(on ? g_PowerHand.load() : -1);
        void* r = g_RealThrowHit(self, b, cc, d);
        if (s.applied) { std::lock_guard lk(g_M); g_Stats.throwApplied++; }
        return r;
    }
    FnThis g_RealPlacementQuery = nullptr;
    void* PlacementQueryHook(void* self)
    {
        { std::lock_guard lk(g_M); g_Stats.placeCalls++; }
        const bool on = (g_PowerAimMask.load() & 64) != 0;
        CamSwap s(on ? g_PowerHand.load() : -1);
        void* r = g_RealPlacementQuery(self);
        if (s.applied) { std::lock_guard lk(g_M); g_Stats.placeApplied++; }
        return r;
    }
    Fn g_RealGrappleQuery = nullptr;
    void* GrappleQueryHook(void* self, void* b, void* cc, void* d)
    {
        { std::lock_guard lk(g_M); g_Stats.grappleCalls++; }
        const bool on = (g_PowerAimMask.load() & 8) != 0;
        CamSwap s(on ? g_PowerHand.load() : -1);
        void* r = g_RealGrappleQuery(self, b, cc, d);
        if (s.applied) { std::lock_guard lk(g_M); g_Stats.grappleApplied++; }
        return r;
    }
    // bool Compute(sensor, const idVec3* from): two register arguments, result in al.
    using FnCompute = bool (*)(void*, const float*);
    FnCompute g_RealGrappleCompute = nullptr;
    bool GrappleComputeHook(void* self, const float* from)
    {
        const bool on = (g_PowerAimMask.load() & 16) != 0;
        CamSwap s(on ? g_PowerHand.load() : -1);
        return g_RealGrappleCompute(self, from);
    }
    FnThis g_RealPowerUpdate = nullptr;
    void* PowerUpdateHook(void* self)
    {
        const bool on = (g_PowerAimMask.load() & 32) != 0;
        CamSwap s(on ? g_PowerHand.load() : -1);
        return g_RealPowerUpdate(self);
    }
    Fn g_RealSearchTarget = nullptr;
    void* SearchTargetHook(void* self, void* b, void* cc, void* d)
    {
        { std::lock_guard lk(g_M); g_Stats.searchCalls++; }
        const bool on = (g_PowerAimMask.load() & 4) != 0;
        CamSwap s(on ? g_PowerHand.load() : -1);
        void* r = g_RealSearchTarget(self, b, cc, d);
        if (s.applied) { std::lock_guard lk(g_M); g_Stats.searchApplied++; }
        return r;
    }
}

bool HandAimHook::Install()
{
    if (!GameInfo::IsSupportedBuild()) return false;
    uintptr_t base = GameInfo::Base();
    g_RealCamUpdate = (Fn)(base + kCameraUpdate);
    g_RealUpdateAim = (Fn)(base + kUpdateAim);
    g_RealGetAim = (Fn)(base + kGetAimOriginAxis);
    g_RealAimCpnt = (Fn)(base + kAimCpntUpdate);
    g_RealGunSpawn = (Fn)(base + kGunSpawnPos);
    g_RealShake = (Fn)(base + kCamApplyShake);
    g_RealCopyView = (Fn)(base + kCamCopyView);
    g_RealRvCopy = (Fn)(base + kRenderViewCopy);
    g_RealGetAimPoint = (FnAimPoint)(base + kGetAimPoint);
    g_RealTarget = (FnTarget)(base + kComputeAimTarget);
    g_RealPowerTarget = (FnThis)(base + kPowerTargetUpdate);
    g_RealThrowHit = (Fn)(base + kThrowHitUpdate);
    g_RealSearchTarget = (Fn)(base + kSearchTargetUpdate);
    g_RealPlacementQuery = (FnThis)(base + kPlacementQuery);
    g_RealGrappleQuery = (Fn)(base + kGrappleSensorQuery);
    g_RealGrappleCompute = (FnCompute)(base + kGrappleSensorCompute);
    g_RealPowerUpdate = (FnThis)(base + kPowerUpdate);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_RealCamUpdate, CamUpdateHook);
    DetourAttach(&(PVOID&)g_RealUpdateAim, UpdateAimHook);
    DetourAttach(&(PVOID&)g_RealGetAim, GetAimHook);
    DetourAttach(&(PVOID&)g_RealAimCpnt, AimCpntHook);
    DetourAttach(&(PVOID&)g_RealGunSpawn, GunSpawnHook);
    DetourAttach(&(PVOID&)g_RealShake, ShakeHook);
    DetourAttach(&(PVOID&)g_RealCopyView, CopyViewHook);
    DetourAttach(&(PVOID&)g_RealRvCopy, RvCopyHook);
    DetourAttach(&(PVOID&)g_RealGetAimPoint, GetAimPointHook);
    DetourAttach(&(PVOID&)g_RealTarget, ComputeAimTargetHook);
    DetourAttach(&(PVOID&)g_RealPowerTarget, PowerTargetHook);
    DetourAttach(&(PVOID&)g_RealThrowHit, ThrowHitHook);
    DetourAttach(&(PVOID&)g_RealSearchTarget, SearchTargetHook);
    DetourAttach(&(PVOID&)g_RealPlacementQuery, PlacementQueryHook);
    DetourAttach(&(PVOID&)g_RealGrappleQuery, GrappleQueryHook);
    DetourAttach(&(PVOID&)g_RealGrappleCompute, GrappleComputeHook);
    DetourAttach(&(PVOID&)g_RealPowerUpdate, PowerUpdateHook);
    LONG r = DetourTransactionCommit();
    LOGI("HandAimHook: UpdateAim@exe+0x{:X} aimCpnt@exe+0x{:X} GetAimPoint@exe+0x{:X} ComputeAimTarget@exe+0x{:X} PowerTarget@exe+0x{:X} ThrowHit@exe+0x{:X} detour={}", kUpdateAim, kAimCpntUpdate, kGetAimPoint, kComputeAimTarget, kPowerTargetUpdate, kThrowHitUpdate, r);
    return r == NO_ERROR;
}
void HandAimHook::Uninstall()
{
    if (!g_RealCamUpdate) return;
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)g_RealCamUpdate, CamUpdateHook);
    DetourDetach(&(PVOID&)g_RealUpdateAim, UpdateAimHook);
    DetourDetach(&(PVOID&)g_RealGetAim, GetAimHook);
    DetourDetach(&(PVOID&)g_RealAimCpnt, AimCpntHook);
    DetourDetach(&(PVOID&)g_RealGunSpawn, GunSpawnHook);
    DetourDetach(&(PVOID&)g_RealShake, ShakeHook);
    DetourDetach(&(PVOID&)g_RealCopyView, CopyViewHook);
    DetourDetach(&(PVOID&)g_RealRvCopy, RvCopyHook);
    DetourDetach(&(PVOID&)g_RealGetAimPoint, GetAimPointHook);
    DetourDetach(&(PVOID&)g_RealTarget, ComputeAimTargetHook);
    DetourDetach(&(PVOID&)g_RealPowerTarget, PowerTargetHook);
    DetourDetach(&(PVOID&)g_RealThrowHit, ThrowHitHook);
    DetourDetach(&(PVOID&)g_RealSearchTarget, SearchTargetHook);
    DetourDetach(&(PVOID&)g_RealPlacementQuery, PlacementQueryHook);
    DetourDetach(&(PVOID&)g_RealGrappleQuery, GrappleQueryHook);
    DetourDetach(&(PVOID&)g_RealGrappleCompute, GrappleComputeHook);
    DetourDetach(&(PVOID&)g_RealPowerUpdate, PowerUpdateHook);
    DetourTransactionCommit();
}
void HandAimHook::SetMode(int m) { g_Mode = m; }
void HandAimHook::SetCamTest(float dz) { g_CamTest = dz; }
void HandAimHook::SetCamTest2(float dz) { g_CamTest2 = dz; }
void HandAimHook::SetFpAnchor(int on) { g_FpAnchor = on; }
void HandAimHook::SetAnchorHand(int h) { g_AnchorHandCfg = h; }
void HandAimHook::SetArmTest(int m) { g_ArmTest = m; }
void HandAimHook::SetArmsFollow(bool on) { g_ArmsFollow = on; }
bool HandAimHook::ArmsFollow() { return g_ArmsFollow; }
int HandAimHook::AnchorHand() { return g_AnchorHand; }
int HandAimHook::FpAnchor() { return g_FpAnchor; }
bool HandAimHook::RealCamera(float org[3], float axis[9]) { const float* o; const float* a; if (!RealCam(o, a)) return false; memcpy(org, o, 12); memcpy(axis, a, 36); return true; }
float HandAimHook::CamTest() { return g_CamTest; }
int HandAimHook::Mode() { return g_Mode; }
void HandAimHook::SetGunHand(int h) { g_GunHand = h ? 1 : 0; }
int HandAimHook::GunHand() { return g_GunHand; }
void HandAimHook::SetPowerHand(int h) { g_PowerHand = h ? 1 : 0; }
void HandAimHook::SetPowerAimMask(int m) { g_PowerAimMask = m; }
void HandAimHook::SetAimOverride(int m) { g_AimOverride = m; }
int HandAimHook::AimOverride() { return g_AimOverride; }
int HandAimHook::PowerAimMask() { return g_PowerAimMask; }
int HandAimHook::PowerHand() { return g_PowerHand; }
bool HandAimHook::ControllerPose(int hand, float org[3], float axis[9])
{
    const float* co; const float* ca; if (!RealCam(co, ca)) return false;
    return CtrlPose(hand, co, ca, org, axis);
}
bool HandAimHook::ControllerPoseAt(int hand, const float eyeOrg[3], const float viewAxis[9], const float eyePosXr[3],
                                   float org[3], float axis[9], const XRInputState* in, const float* headQuat)
{
    return CtrlPose(hand, eyeOrg, viewAxis, org, axis, eyePosXr, in, headQuat);
}
void HandAimHook::XrOffsetToWorld(const float dXr[3], const float viewAxis[9], float out[3], const float* headQuat) { XrOffsetToWorldImpl(dXr, viewAxis, out, headQuat); }
void* HandAimHook::CameraObject() { return g_Cam.load(); }
void* HandAimHook::AimCpnt() { return g_AimCpnt.load(); }
void HandAimHook::SetAimCpnt(void* p) { g_AimCpnt.store(p); }
void* HandAimHook::GunCpnt() { return g_Gun.load(); }
unsigned long long HandAimHook::GetAimCalls() { return g_GetAimCalls.load(); }
HandAimHook::Stats HandAimHook::GetStats() { std::lock_guard lk(g_M); return g_Stats; }
unsigned long long HandAimHook::AimTicks() { std::lock_guard lk(g_M); return g_Stats.calls; }
