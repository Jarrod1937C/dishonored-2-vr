#include "pch.h"
#include "CameraHook.h"
#include "GameInfo.h"
#include "VRRuntime.h"
#include "ScreenMode.h"
#include "VRMath.h"
#include "ViewMatrixHook.h"
#include <detours/detours.h>
#include <cmath>
#include <algorithm>

namespace
{
    using Fn = void* (*)(void*, void*, void*, void*);
    Fn g_Real = nullptr;
    std::atomic<int> g_Mode{ 1 };
    std::atomic<float> g_WorldScale{ 1.0f };
    std::atomic<bool> g_RecenterReq{ true };
    float g_RefPos[3] = {};          //!< head position at recenter (XR local space)
    bool g_RefValid = false;
    std::mutex g_StatsMutex;
    CameraHook::Stats g_Stats;

    // ComputeMatrices runs several times per frame on the same view object and the
    // game does not rewrite m_renderRV in between, so we must always derive from the
    // game's ORIGINAL values, never from what we wrote last time.
    struct Baseline { void* view = nullptr; float org[3], axis[9], wOrg[3], wAxis[9]; bool valid = false; } g_Base;
    extern std::atomic<unsigned long long> g_Refreshes;
    extern std::atomic<void*> g_MainView;

    void GetBaseline(void* view, float* org, float* axis, float baseOrg[3], float baseAxis[9])
    {
        if (g_Base.valid && g_Base.view == view &&
            memcmp(org, g_Base.wOrg, 12) == 0 && memcmp(axis, g_Base.wAxis, 36) == 0)
        {   // untouched since our last write -> the game hasn't refreshed it; keep the original
            memcpy(baseOrg, g_Base.org, 12); memcpy(baseAxis, g_Base.axis, 36);
        }
        else
        {   // fresh game values
            memcpy(baseOrg, org, 12); memcpy(baseAxis, axis, 36);
            g_Base.view = view; memcpy(g_Base.org, org, 12); memcpy(g_Base.axis, axis, 36); g_Base.valid = true;
            g_Refreshes++;
        }
    }
    std::atomic<unsigned long long> g_Refreshes{ 0 };
    std::atomic<void*> g_MainView{ nullptr };
    std::atomic<void*> g_MainViews[4];                 // the last few distinct main view objects (IsMainViewObject)
    std::atomic<unsigned> g_MainViewNext{ 0 };
    void RememberWritten(const float* org, const float* axis) { memcpy(g_Base.wOrg, org, 12); memcpy(g_Base.wAxis, axis, 36); }

    // --- math helpers -------------------------------------------------------
    // OpenXR: +X right, +Y up, -Z forward.   id Tech: +X forward, +Y left, +Z up.
    inline void XrToId(const float v[3], float out[3]) { out[0] = -v[2]; out[1] = -v[0]; out[2] = v[1]; }

    //! Rotates unit basis vectors by quaternion (x,y,z,w): returns fwd(-Z), right(+X), up(+Y) in XR space.
    void QuatBasis(const float q[4], float fwd[3], float right[3], float up[3])
    {
        float x = q[0], y = q[1], z = q[2], w = q[3];
        // rotation matrix columns
        float m00 = 1 - 2 * (y * y + z * z), m01 = 2 * (x * y - z * w), m02 = 2 * (x * z + y * w);
        float m10 = 2 * (x * y + z * w), m11 = 1 - 2 * (x * x + z * z), m12 = 2 * (y * z - x * w);
        float m20 = 2 * (x * z - y * w), m21 = 2 * (y * z + x * w), m22 = 1 - 2 * (x * x + y * y);
        right[0] = m00; right[1] = m10; right[2] = m20;   // R * (1,0,0)
        up[0] = m01;    up[1] = m11;    up[2] = m21;      // R * (0,1,0)
        fwd[0] = -m02;  fwd[1] = -m12;  fwd[2] = -m22;    // R * (0,0,-1)
    }

    void* Hook(void* a, void* view, void* c, void* d)
    {
        // Signature: ComputeMatrices(constantBlock* rcx, RenderView* rdx) — the view object is the 2nd arg.
        uint8_t* rv = (uint8_t*)view + CameraHook::kRenderRV;
        float* org = (float*)(rv + CameraHook::kRV_ViewOrg);
        float* axis = (float*)(rv + CameraHook::kRV_ViewAxis);
        int w = *(int*)((uint8_t*)view + CameraHook::kView_RenderW);
        int h = *(int*)((uint8_t*)view + CameraHook::kView_RenderH);
        // Main view heuristic: the largest render target (UI/portal/shadow views are smaller).
        bool main = w >= 1024 && h >= 576;
        if (main)
        {
            g_MainView.store(view);
            bool known = false;
            for (auto& v : g_MainViews) if (v.load(std::memory_order_relaxed) == view) { known = true; break; }
            if (!known) g_MainViews[g_MainViewNext.fetch_add(1) % 4].store(view);
        }

        // Experiments: 3 = halve the FOV, 4 = forceIdentityViewMatrix. If these don't alter the
        // image either, this object does not feed the scene pass.
        if (main && g_Mode.load() == 3) { *(float*)(rv + CameraHook::kRV_FovX) *= 0.5f; *(float*)(rv + CameraHook::kRV_FovY) *= 0.5f; }
        if (main && g_Mode.load() == 4) { *(uint8_t*)(rv + 0x15C) = 1; }
        if (main && (g_Mode.load() == 1 || g_Mode.load() == 2))
        {
            VRRuntime::HeadPose hp = VRRuntime::GetHeadPose();
            if (hp.valid)
            {
                if (g_RecenterReq.exchange(false) || !g_RefValid) { memcpy(g_RefPos, hp.pos, 12); g_RefValid = true; }

                float bOrg[3], bAxis[9];
                GetBaseline(view, org, axis, bOrg, bAxis);

                // Body frame = the game's yaw (mouse/controller), flattened.
                float yaw = atan2f(bAxis[1], bAxis[0]);
                float cy = cosf(yaw), sy = sinf(yaw);
                auto toWorld = [&](const float l[3], float out[3]) {   // local (x fwd, y left, z up) -> world
                    out[0] = l[0] * cy - l[1] * sy;
                    out[1] = l[0] * sy + l[1] * cy;
                    out[2] = l[2];
                };

                float fx[3], rx[3], ux[3]; QuatBasis(hp.quat, fx, rx, ux);
                float fl[3], rl[3], ul[3]; XrToId(fx, fl); XrToId(rx, rl); XrToId(ux, ul);
                float fw[3], rw[3], uw[3]; toWorld(fl, fw); toWorld(rl, rw); toWorld(ul, uw);
                axis[0] = fw[0]; axis[1] = fw[1]; axis[2] = fw[2];          // forward
                axis[3] = -rw[0]; axis[4] = -rw[1]; axis[5] = -rw[2];       // left = -right
                axis[6] = uw[0]; axis[7] = uw[1]; axis[8] = uw[2];          // up

                float dxr[3] = { hp.pos[0] - g_RefPos[0], hp.pos[1] - g_RefPos[1], hp.pos[2] - g_RefPos[2] };
                float dl[3]; XrToId(dxr, dl);
                float dw[3]; toWorld(dl, dw);
                float s = g_WorldScale.load();
                org[0] = bOrg[0] + dw[0] * s; org[1] = bOrg[1] + dw[1] * s; org[2] = bOrg[2] + dw[2] * s;
                RememberWritten(org, axis);
                if (g_Mode.load() == 2)
                {   // experiment: also push into m_gameRV (+0) of the same object
                    memcpy((uint8_t*)view + CameraHook::kRV_ViewOrg, org, 12);
                    memcpy((uint8_t*)view + CameraHook::kRV_ViewAxis, axis, 36);
                }

                std::lock_guard lk(g_StatsMutex); g_Stats.applied++;
            }
        }
        {
            std::lock_guard lk(g_StatsMutex);
            g_Stats.calls++;
            if (main)
            {
                g_Stats.mainViewCalls++;
                memcpy(g_Stats.lastOrg, org, 12); memcpy(g_Stats.lastFwd, axis, 12);
                g_Stats.lastFovX = *(float*)(rv + CameraHook::kRV_FovX);
                g_Stats.lastFovY = *(float*)(rv + CameraHook::kRV_FovY);
                g_Stats.lastW = w; g_Stats.lastH = h;
                // Tell the flat-frame layer the real game FOV so the image lands at its true angular size.
                if (g_Stats.lastFovX > 0.1f && g_Stats.lastFovY > 0.1f)
                    VRRuntime::SetGameFov(g_Stats.lastFovX * 57.29578f, g_Stats.lastFovY * 57.29578f);
            }
        }
        void* ret = g_Real(a, view, c, d);
        return ret;
    }

    // ---- the projection (see kBuildProjection) ----
    // The game renders a 95.4 x 58.7 deg window (16:9-ish backbuffer); a Quest 3 eye sees 94 x 99 deg, from 44 up to
    // 55 down. A relaxed hand sits 30-50 deg below the gaze, i.e. OUTSIDE the game's image (measured with the mock
    // compositor, BodyView): the arms were "almost never seen". The projection is the one place to change that.
    using ProjFn = void (*)(void* rv, float* out, float a, float b, void* e, int f, int g);
    ProjFn g_RealProj = nullptr;
    std::atomic<float> g_FovOvX{ 0.f }, g_FovOvY{ 0.f };
    std::atomic<float> g_GameFovX{ 0.f }, g_GameFovY{ 0.f }, g_BuiltFovX{ 0.f }, g_BuiltFovY{ 0.f };
    std::atomic<unsigned long long> g_FovOverrides{ 0 }, g_FlatWidened{ 0 }, g_ExplicitBuilds{ 0 };
    std::atomic<unsigned long long> g_EyeFovViews[2] = {};   // views built with a stereo eye's own fov (per-eye frustum, mode 1)
    void ProjHook(void* rv, float* out, float a, float b, void* e, int f, int g)
    {
        uint8_t* mv = (uint8_t*)g_MainView.load();
        if (mv && rv == mv + CameraHook::kRenderRV && *((uint8_t*)rv + CameraHook::kRV_UseExplicit)) g_ExplicitBuilds++;
        if (mv && rv == mv + CameraHook::kRenderRV && !*((uint8_t*)rv + CameraHook::kRV_UseExplicit))
        {
            float* fx = (float*)((uint8_t*)rv + CameraHook::kRV_FovX);
            float* fy = (float*)((uint8_t*)rv + CameraHook::kRV_FovY);
            g_GameFovX = *fx * 57.29578f; g_GameFovY = *fy * 57.29578f;
            float ox = g_FovOvX.load(), oy = g_FovOvY.load();
            // A stereo eye's view (CameraHook::EyeTans, mode 1): its own symmetric fov, in the frame ViewMatrixHook turns it
            // to. Which view this is comes from the same state ViewMatrixHook decides by a moment later, on this thread.
            if (ox > 1.f && oy > 1.f && VRRuntime::Stereo() && !ScreenMode::FlatRender())
            {
                const int ne = ViewMatrixHook::NextViewEye();
                float ex = 0.f, ey = 0.f;
                if (ne >= 0 && CameraHook::EyeViewFov(ne, ex, ey)) { ox = ex; oy = ey; g_EyeFovViews[ne]++; }
            }
            // On the menu screen (ScreenMode) the picture is the flat game's: its own fov (the title screen's camera is
            // a 19-degree shot; at the headset's 108 degrees it was a wide view of a set). The game's fov follows the
            // render size's aspect from its horizontal, so the 16:9 band shown is exactly the flat game's 16:9 view.
            if (ox > 1.f && oy > 1.f && ox < 170.f && oy < 170.f && !ScreenMode::FlatRender())
            {   // written into the view's own renderView, not just the matrix: whatever reads the fov later this frame
                // (culling, the shader parms) sees the same frustum
                *fx = ox / 57.29578f; *fy = oy / 57.29578f;
                g_FovOverrides++;
            }
            else if (ScreenMode::FlatRender())
            {   // The game keeps its VERTICAL fov whatever the aspect (Hor+; measured on the title: 38.2 x 20.1 deg at
                // 1902x973, 19.4 x 20.1 at 2266x2348), so in the headset's tall frame the 16:9 band the screen shows was
                // the flat game's view zoomed 1.78x. Widen it until that band spans the game's vertical fov: the band is
                // then exactly what a 16:9 monitor shows.
                // (relative, so not twice: a second build on the same values this frame finds our own numbers there)
                static float lastX = 0.f, lastY = 0.f;
                const float tx = tanf(*fx * 0.5f), ty = tanf(*fy * 0.5f), r = tx > 1e-4f ? ty / tx : 0.f;   // r = H / W
                if (r > 9.f / 16.f + 1e-3f && r < 4.f && !(*fx == lastX && *fy == lastY))
                {
                    const float nx = ty * 16.f / 9.f, ny = nx * r;
                    *fx = 2.f * atanf(nx); *fy = 2.f * atanf(ny);
                    lastX = *fx; lastY = *fy;
                    g_FlatWidened++;
                }
            }
            g_BuiltFovX = *fx * 57.29578f; g_BuiltFovY = *fy * 57.29578f;
        }
        g_RealProj(rv, out, a, b, e, f, g);
    }
}

namespace
{
    // ---- per-eye projection (see the header) ----
    std::atomic<int> g_EyeProj{ 1 };
    std::atomic<float> g_EyeMargin{ 0.f };
    std::atomic<bool> g_EyeLayoutOk{ true };          // the engine's projection has the shape the rewrite expects
    std::atomic<unsigned long long> g_EyeApplied[2] = {}, g_EyeSkipped{ 0 };
    constexpr float kMaxSide = 75.f / 57.29578f;      // no side beyond 75 deg (as vr fov auto's 150 deg cap)

    float ClampAng(float a) { return std::clamp(a, -kMaxSide, kMaxSide); }
    //! Eye e's native fov (radians L R U D) from the last located frame.
    bool EyeFov(int e, float f[4])
    {
        const VRRuntime::Placement pl = VRRuntime::GetPlacement(e);
        if (!pl.valid || !(pl.fov[1] > pl.fov[0]) || !(pl.fov[2] > pl.fov[3])) return false;
        memcpy(f, pl.fov, 16); return true;
    }
    //! Mode 1, one candidate: the frame turned to (ax, ay) (radians, + right / + up, no roll) as a rotation matrix m
    //! (columns = its axes in the eye's frame: v_eye = m v_render), and the symmetric half-tans that cover the eye
    //! frustum's corners there (a central projection keeps straight lines: the frustum is the quadrilateral of its corners).
    float TurnCover(const float cx[4], const float cy[4], float ax, float ay, float m[9], float& hx, float& hy)
    {
        const float fw[3] = { sinf(ax) * cosf(ay), sinf(ay), -cosf(ax) * cosf(ay) };
        float rt[3] = { -fw[2], 0.f, fw[0] };   // fw x (0,1,0)
        const float n = sqrtf(rt[0] * rt[0] + rt[2] * rt[2]); rt[0] /= n; rt[2] /= n;
        const float up[3] = { rt[1] * fw[2] - rt[2] * fw[1], rt[2] * fw[0] - rt[0] * fw[2], rt[0] * fw[1] - rt[1] * fw[0] };
        for (int k = 0; k < 3; k++) { m[k * 3] = rt[k]; m[k * 3 + 1] = up[k]; m[k * 3 + 2] = -fw[k]; }
        hx = 0.f; hy = 0.f;
        for (int c = 0; c < 4; c++)
        {
            const float v[3] = { cx[c], cy[c], -1.f };
            const float x = m[0] * v[0] + m[3] * v[1] + m[6] * v[2], y = m[1] * v[0] + m[4] * v[1] + m[7] * v[2], z = m[2] * v[0] + m[5] * v[1] + m[8] * v[2];
            const float d = std::max(-z, 0.05f);
            hx = std::max(hx, fabsf(x / d)); hy = std::max(hy, fabsf(y / d));
        }
        return hx * hy;   // the pixel count at a fixed density
    }
    //! Mode 1: the frame eye frustum `f` (radians L R U D, margin included) is rendered in -- the turn that needs the
    //! fewest pixels for a symmetric cover (searched from the angular centre, steps halving to 0.05 deg) -- as `rot`
    //! (v_eye = rot * v_render) and its half-tans. Quest 3 left eye: turned 6.5 deg in and 5.25 down, 79% of the pixels
    //! of the symmetric cover of both eyes (the asymmetric matrix would be 67%, see the header).
    void TurnedFrame(const float f[4], float rot[4], float& hx, float& hy)
    {
        const float tl = tanf(f[0]), tr = tanf(f[1]), tu = tanf(f[2]), td = tanf(f[3]);
        const float cx[4] = { tl, tr, tl, tr }, cy[4] = { tu, tu, td, td };
        float ax = (f[0] + f[1]) * 0.5f, ay = (f[2] + f[3]) * 0.5f, m[9];
        float best = TurnCover(cx, cy, ax, ay, m, hx, hy);
        for (float step = 2.f / 57.29578f; step > 0.05f / 57.29578f; step *= 0.5f)
            for (bool moved = true; moved;)
            {
                moved = false;
                const float tries[4][2] = { { step, 0 }, { -step, 0 }, { 0, step }, { 0, -step } };
                for (const auto& t : tries)
                {
                    float mm[9], x, y; const float a = TurnCover(cx, cy, ax + t[0], ay + t[1], mm, x, y);
                    if (a < best - 1e-6f) { best = a; ax += t[0]; ay += t[1]; moved = true; break; }
                }
            }
        TurnCover(cx, cy, ax, ay, m, hx, hy);
        // rotation matrix (columns = the render frame's axes in the eye's) -> quaternion
        const float m00 = m[0], m01 = m[1], m02 = m[2], m10 = m[3], m11 = m[4], m12 = m[5], m20 = m[6], m21 = m[7], m22 = m[8];
        const float tr3 = m00 + m11 + m22;
        if (tr3 > 0) { const float s = sqrtf(tr3 + 1.f) * 2; rot[3] = 0.25f * s; rot[0] = (m21 - m12) / s; rot[1] = (m02 - m20) / s; rot[2] = (m10 - m01) / s; }
        else if (m00 > m11 && m00 > m22) { const float s = sqrtf(1.f + m00 - m11 - m22) * 2; rot[3] = (m21 - m12) / s; rot[0] = 0.25f * s; rot[1] = (m01 + m10) / s; rot[2] = (m02 + m20) / s; }
        else if (m11 > m22) { const float s = sqrtf(1.f + m11 - m00 - m22) * 2; rot[3] = (m02 - m20) / s; rot[0] = (m01 + m10) / s; rot[1] = 0.25f * s; rot[2] = (m12 + m21) / s; }
        else { const float s = sqrtf(1.f + m22 - m00 - m11) * 2; rot[3] = (m10 - m01) / s; rot[0] = (m02 + m20) / s; rot[1] = (m12 + m21) / s; rot[2] = 0.25f * s; }
        const float n = sqrtf(rot[0] * rot[0] + rot[1] * rot[1] + rot[2] * rot[2] + rot[3] * rot[3]);
        for (int k = 0; k < 4; k++) rot[k] /= n;
    }
    //! TurnedFrame, kept until the eye's fov or the margin changes (EyeTans is asked several times a frame)
    struct TurnCache { float f[4] = {}; float rot[4] = { 0, 0, 0, 1 }; float hx = 0, hy = 0; bool valid = false; };
    TurnCache g_Turn[2]; std::mutex g_TurnM;
    void TurnedFrameCached(int e, const float f[4], float rot[4], float& hx, float& hy)
    {
        std::lock_guard lk(g_TurnM);
        TurnCache& c = g_Turn[e & 1];
        if (!c.valid || memcmp(c.f, f, 16) != 0) { TurnedFrame(f, c.rot, c.hx, c.hy); memcpy(c.f, f, 16); c.valid = true; }
        memcpy(rot, c.rot, 16); hx = c.hx; hy = c.hy;
    }
    //! Both eyes' frusta with the margin, padded to one extent (rot: the render frame in the eye's, mode 1).
    bool EyeTansBoth(CameraHook::Tans t[2], float rot[2][4])
    {
        const float m = std::clamp(g_EyeMargin.load(), 0.f, 15.f) / 57.29578f;
        const int mode = g_EyeProj.load();
        float hx[2] = {}, hy[2] = {};
        for (int e = 0; e < 2; e++)
        {
            float f[4]; if (!EyeFov(e, f)) return false;
            const float g[4] = { ClampAng(f[0] - m), ClampAng(f[1] + m), ClampAng(f[2] + m), ClampAng(f[3] - m) };
            rot[e][0] = rot[e][1] = rot[e][2] = 0.f; rot[e][3] = 1.f;
            if (mode == 1) TurnedFrameCached(e, g, rot[e], hx[e], hy[e]);
            t[e].l = tanf(g[0]); t[e].r = tanf(g[1]); t[e].u = tanf(g[2]); t[e].d = tanf(g[3]);
            t[e].valid = true;
        }
        if (mode == 1)
        {   // one symmetric fov for both turned eyes
            const float x = std::max(hx[0], hx[1]), y = std::max(hy[0], hy[1]);
            for (int e = 0; e < 2; e++) { t[e].l = -x; t[e].r = x; t[e].u = y; t[e].d = -y; }
            return true;
        }
        // one render size for both eyes: the narrower eye is widened (evenly) to the wider one's extent, so the pixel
        // density is the same in both (headsets whose eyes are not mirror images)
        const float ex = std::max(t[0].r - t[0].l, t[1].r - t[1].l), ey = std::max(t[0].u - t[0].d, t[1].u - t[1].d);
        for (int e = 0; e < 2; e++)
        {
            const float px = (ex - (t[e].r - t[e].l)) * 0.5f, py = (ey - (t[e].u - t[e].d)) * 0.5f;
            t[e].l -= px; t[e].r += px; t[e].d -= py; t[e].u += py;
        }
        return true;
    }
}

void CameraHook::SetEyeProj(int mode, float marginDeg)
{
    g_EyeProj = std::clamp(mode, 0, 2); g_EyeMargin = std::clamp(marginDeg, 0.f, 15.f); g_EyeLayoutOk = true;
    LOGI("CameraHook: per-eye frustum {} (margin {:.1f} deg a side)", g_EyeProj.load() == 1 ? "on: each stereo eye turned to its frustum's centre, its own symmetric fov"
         : g_EyeProj.load() == 2 ? "ASYMMETRIC MATRIX (research: breaks the engine's tiled lighting)" : "off: every view the symmetric cover", g_EyeMargin.load());
}
int CameraHook::EyeProj() { return g_EyeProj.load(); }
float CameraHook::EyeProjMargin() { return g_EyeMargin.load(); }

bool CameraHook::EyeTans(int eye, Tans& t, float rot[4])
{
    Tans b[2]; float r[2][4];
    if (eye < 0 || eye > 1 || !g_EyeProj.load() || !EyeTansBoth(b, r)) return false;
    t = b[eye]; if (rot) memcpy(rot, r[eye], 16);
    return true;
}

bool CameraHook::EyeViewFov(int eye, float& xDeg, float& yDeg)
{
    Tans t; if (g_EyeProj.load() != 1 || !EyeTans(eye, t)) return false;
    xDeg = 2.f * atanf(t.r) * 57.29578f; yDeg = 2.f * atanf(t.u) * 57.29578f;
    return xDeg > 1.f && yDeg > 1.f && xDeg < 170.f && yDeg < 170.f;
}

bool CameraHook::UnionTans(Tans& t)
{
    t = Tans(); t.l = t.d = 1e9f; t.r = t.u = -1e9f;
    for (int e = 0; e < 2; e++)
    {
        float f[4]; if (!EyeFov(e, f)) return false;
        float rel[4] = { 0, 0, 0, 1 }; VRRuntime::EyeRelative(e, rel);
        const float tl = tanf(ClampAng(f[0])), tr = tanf(ClampAng(f[1])), tu = tanf(ClampAng(f[2])), td = tanf(ClampAng(f[3]));
        const float cx[4] = { tl, tr, tl, tr }, cy[4] = { tu, tu, td, td };
        for (int c = 0; c < 4; c++)
        {   // the eye's corner direction (XR: -z forward), into the head's frame
            const float v[3] = { cx[c], cy[c], -1.f }; float h[3]; VRMath::QRotate(rel, v, h);
            const float z = std::max(-h[2], 0.05f);
            t.l = std::min(t.l, h[0] / z); t.r = std::max(t.r, h[0] / z);
            t.d = std::min(t.d, h[1] / z); t.u = std::max(t.u, h[1] / z);
        }
    }
    const float k = tanf(kMaxSide);
    t.l = std::max(t.l, -k); t.r = std::min(t.r, k); t.d = std::max(t.d, -k); t.u = std::min(t.u, k);
    t.valid = true;
    return true;
}

CameraHook::Tans CameraHook::BuiltTans()
{
    Tans t; const float x = g_BuiltFovX.load(), y = g_BuiltFovY.load();
    if (x > 1.f && y > 1.f && x < 179.f && y < 179.f)
    {
        const float tx = tanf(x * 0.5f / 57.29578f), ty = tanf(y * 0.5f / 57.29578f);
        t.l = -tx; t.r = tx; t.u = ty; t.d = -ty; t.valid = true;
    }
    return t;
}

bool CameraHook::ApplyEyeProjection(void* view, int eye, Tans& out)
{
    out = BuiltTans();
    if (!view || eye < 0 || eye > 1 || g_EyeProj.load() != 2 || !VRRuntime::Stereo()) return false;
    Tans t; if (!EyeTans(eye, t)) { g_EyeSkipped++; return false; }
    float* P = (float*)((uint8_t*)view + kView_Proj);
    // the shape the rewrite assumes (a perspective projection, rows x/y/z/w, w = depth): anything else is left alone
    const float w2 = P[14];
    const bool shape = fabsf(fabsf(w2) - 1.f) < 1e-3f && fabsf(P[12]) < 1e-6f && fabsf(P[13]) < 1e-6f && fabsf(P[15]) < 1e-6f &&
                       fabsf(P[1]) < 1e-6f && fabsf(P[3]) < 1e-6f && fabsf(P[4]) < 1e-6f && fabsf(P[7]) < 1e-6f &&
                       fabsf(P[0]) > 1e-3f && fabsf(P[5]) > 1e-3f;
    if (!shape)
    {
        if (g_EyeLayoutOk.exchange(false))
            LOGI("CameraHook: per-eye projection NOT applied: the main view's projection is not the expected shape ({:.4f} {:.4f} {:.4f} {:.4f} / {:.4f} {:.4f} {:.4f} {:.4f} / w {:.4f} {:.4f} {:.4f} {:.4f})",
                 P[0], P[1], P[2], P[3], P[4], P[5], P[6], P[7], P[12], P[13], P[14], P[15]);
        g_EyeSkipped++; return false;
    }
    // ndc = t / tx for the symmetric projection (tx = 1 / |p00| with |w| = 1); for [l, r]: ndc = (2t - (r + l)) / (r - l).
    // The x row is scaled by a = 2 tx / (r - l) and gets the centre -(r + l) / (r - l); whatever the projection already
    // carried in column 2 (the TAA jitter, in ndc) is kept as it is.
    const float tx = 1.f / fabsf(P[0]), ty = 1.f / fabsf(P[5]);
    const float ax = 2.f * tx / (t.r - t.l), bx = -(t.r + t.l) / (t.r - t.l);
    const float ay = 2.f * ty / (t.u - t.d), by = -(t.u + t.d) / (t.u - t.d);
    P[0] *= ax; P[2] += bx * w2;
    P[5] *= ay; P[6] += by * w2;
    float inv[16];
    if (VRMath::Inverse4(P, inv)) memcpy((uint8_t*)view + kView_InvProj, inv, 64);
    g_EyeApplied[eye]++;
    out = t;
    return true;
}

bool CameraHook::RenderExtent(float& ex, float& ey)
{
    Tans t;
    if (g_EyeProj.load() && (g_EyeProj.load() == 1 || g_EyeLayoutOk.load()) && VRRuntime::Stereo() && EyeTans(0, t)) { ex = t.r - t.l; ey = t.u - t.d; return true; }
    const float ox = g_FovOvX.load(), oy = g_FovOvY.load();
    if (ox > 1.f && oy > 1.f) { ex = 2.f * tanf(ox * 0.5f / 57.29578f); ey = 2.f * tanf(oy * 0.5f / 57.29578f); return true; }
    return false;
}

std::string CameraHook::EyeProjStatus()
{
    Tans l, r, u; const bool hl = EyeTans(0, l), hr = EyeTans(1, r), hu = UnionTans(u);
    float ex = 0, ey = 0; const bool he = RenderExtent(ex, ey);
    auto deg = [](float t) { return atanf(t) * 57.29578f; };
    std::string s = fmt::format("eyeproj mode {} margin {:.1f} turnedviews L {} R {} | matrix shapeok {} applied L {} R {} skipped {} | extent {}", g_EyeProj.load(), g_EyeMargin.load(),
                                g_EyeFovViews[0].load(), g_EyeFovViews[1].load(), g_EyeLayoutOk.load() ? 1 : 0, g_EyeApplied[0].load(), g_EyeApplied[1].load(),
                                g_EyeSkipped.load(), he ? fmt::format("{:.3f}x{:.3f}", ex, ey) : std::string("-"));
    float rl[4] = { 0, 0, 0, 1 }, rr[4] = { 0, 0, 0, 1 }; EyeTans(0, l, rl); EyeTans(1, r, rr);
    auto turn = [](const float q[4]) {   // the turned frame's forward in the eye's: yaw (+ right) / pitch (+ up)
        const float f[3] = { 0, 0, -1 }; float v[3]; VRMath::QRotate(q, f, v);
        return fmt::format("{:+.1f}/{:+.1f}", atan2f(v[0], -v[2]) * 57.29578f, asinf(std::clamp(v[1], -1.f, 1.f)) * 57.29578f); };
    if (hl) s += fmt::format(" | eye0 deg {:.1f} {:.1f} {:.1f} {:.1f} turn {}", deg(l.l), deg(l.r), deg(l.u), deg(l.d), turn(rl));
    if (hr) s += fmt::format(" eye1 {:.1f} {:.1f} {:.1f} {:.1f} turn {}", deg(r.l), deg(r.r), deg(r.u), deg(r.d), turn(rr));
    if (hu) s += fmt::format(" | union {:.1f} {:.1f} {:.1f} {:.1f}", deg(u.l), deg(u.r), deg(u.u), deg(u.d));
    return s;
}

void CameraHook::SetFovOverride(float x, float y) { g_FovOvX = x; g_FovOvY = y; LOGI("CameraHook: fov override {:.1f} x {:.1f} deg{}", x, y, x > 1.f ? "" : " (off: the game's own)"); }
void CameraHook::FovOverride(float& x, float& y) { x = g_FovOvX.load(); y = g_FovOvY.load(); }
void CameraHook::GameFov(float& x, float& y) { x = g_GameFovX.load(); y = g_GameFovY.load(); }
void CameraHook::BuiltFov(float& x, float& y, unsigned long long& n) { x = g_BuiltFovX.load(); y = g_BuiltFovY.load(); n = g_FovOverrides.load(); }
unsigned long long CameraHook::ExplicitBuilds() { return g_ExplicitBuilds.load(); }

bool CameraHook::Install()
{
    if (!GameInfo::IsSupportedBuild()) { LOGI("CameraHook: unsupported build, not installed"); return false; }
    g_Real = (Fn)(GameInfo::Base() + kComputeMatrices);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_Real, Hook);
    g_RealProj = (ProjFn)(GameInfo::Base() + kBuildProjection);
    DetourAttach(&(PVOID&)g_RealProj, ProjHook);
    LONG r = DetourTransactionCommit();
    LOGI("CameraHook: ComputeMatrices@exe+0x{:X} + BuildProjection@exe+0x{:X} detour={}", kComputeMatrices, kBuildProjection, r);
    return r == NO_ERROR;
}

void CameraHook::Uninstall()
{
    if (!g_Real) return;
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)g_Real, Hook);
    if (g_RealProj) DetourDetach(&(PVOID&)g_RealProj, ProjHook);
    DetourTransactionCommit();
}

void* CameraHook::MainView() { return g_MainView.load(); }
bool CameraHook::IsMainViewObject(const void* v)
{
    if (!v) return false;
    for (auto& m : g_MainViews) if (m.load(std::memory_order_relaxed) == v) return true;
    return false;
}
void CameraHook::SetMode(int m) { g_Mode = m; }
int  CameraHook::Mode() { return g_Mode; }
void CameraHook::Recenter() { g_RecenterReq = true; }
void CameraHook::SetWorldScale(float s) { g_WorldScale = s; }
CameraHook::Stats CameraHook::GetStats() { std::lock_guard lk(g_StatsMutex); auto s = g_Stats; s.refreshes = g_Refreshes.load(); return s; }
