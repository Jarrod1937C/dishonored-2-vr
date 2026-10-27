#pragma once
#include <cstdint>

//! The render view: exe+0x3586A0 (named kComputeMatrices for history) only UPLOADS the view's matrices, already built
//! from m_renderRV (renderView_t at +0x39D0): rewriting vieworg/viewaxis there does not move the scene. The hook runs
//! as a pass-through that finds the main view and reads its fov. The head pose reaches the scene through
//! idRenderMatrix::CreateViewMatrix (exe+0x109CC0, ViewMatrixHook), and the fov override (`vr fov`) through the
//! projection builder below (exe+0x3742B0). See docs/ENGINE.md, "Rendering".
namespace CameraHook
{
    // RVAs / offsets (1.77.9.0) — see docs/ENGINE.md
    constexpr uint32_t kComputeMatrices = 0x3586A0;
    constexpr uint32_t kRenderRV        = 0x39D0;   //!< renderView_t m_renderRV inside the view object
    constexpr uint32_t kRV_FovX         = 0x9C;     //!< radians
    constexpr uint32_t kRV_FovY         = 0xA0;
    constexpr uint32_t kRV_ViewOrg      = 0xA8;
    constexpr uint32_t kRV_ViewAxis     = 0xCC;     //!< idMat3: rows fwd, left, up
    constexpr uint32_t kRV_ExplicitProj = 0x100;    //!< 4x4 floats
    constexpr uint32_t kRV_UseExplicit  = 0x140;    //!< bool
    constexpr uint32_t kView_Proj       = 0x7390;   //!< the view's projection, 4x4 row-major (jitter in [0][2] / [1][2])
    constexpr uint32_t kView_InvProj    = 0x73D0;   //!< its inverse (derived right after the projection builder)
    constexpr uint32_t kView_RenderW    = 0x7848;
    constexpr uint32_t kView_RenderH    = 0x784C;

    //! The engine's projection builder: `BuildProjection(renderView_t* rv, float out[16], ...)` copies
    //! rv->explicitProjectionMatrix when rv->useExplicitProjectionMatrix, else builds it from rv->fov_x/fov_y (TAA
    //! jitter included). Called from the view setup at exe+0x375AE4, BEFORE the inverse projection and the
    //! view-projection are derived from it, so a fov changed here is consistent everywhere downstream.
    constexpr uint32_t kBuildProjection = 0x3742B0;

    bool Install();
    void Uninstall();

    //! The main view's fov, in degrees (full angles). 0/0 = the game's own. Takes effect on the next frame.
    void SetFovOverride(float xDeg, float yDeg);
    void FovOverride(float& xDeg, float& yDeg);
    //! The fov the GAME asked for on the last main-view projection, before any override (degrees).
    void GameFov(float& xDeg, float& yDeg);
    //! The fov the main view was actually built with last (after any override), and how many times it was overridden.
    void BuiltFov(float& xDeg, float& yDeg, unsigned long long& overrides);
    //! Projections of the main view built from an explicit matrix (the fov is not used): counted, research.
    unsigned long long ExplicitBuilds();

    // ---- Per-eye frustum (`vr eyeproj`) ----
    // A stereo eye of a headset is asymmetric (Quest 3 left eye: 54 deg out, 40 in, 44 up, 55 down), so rendering every
    // eye with the symmetric frustum that covers both (vr fov auto) left a third of each eye's pixels unseen.
    // Mode 1 (default): each stereo eye's view is TURNED towards the centre of its own frustum (7 deg in, 5.5 down on
    // the Quest 3) and rendered with the smallest SYMMETRIC fov that covers it there. The engine only ever sees an
    // ordinary symmetric view -- every screen-space pass that rebuilds positions from tan(fov/2) stays right -- and the
    // image, in that turned frame, is declared as such to the compositor or warped into the eye (XRSession).
    // Mode 2 (research): the projection matrix itself made asymmetric (rewritten after the view matrix, before the
    // engine's VP). MEASURED BROKEN: the constants upload (exe+0x358A74) hands the shaders tan(fov/2) from the renderView,
    // and a pass that reconstructs view positions from it (tiled lighting) left dark, blocky tiles along the top.
    struct Tans { float l = 0, r = 0, u = 0, d = 0; bool valid = false; };   //!< tangents in the view's own frame: x right, y up
    //! mode 0 = off (every view the symmetric cover, as before), 1 = each stereo eye turned + its own symmetric fov,
    //! 2 = asymmetric matrix (research); margin = degrees added on each side of a stereo eye's frustum (overscan for the
    //! compositor's reprojection, see VRRuntime::SetLayerFov).
    void SetEyeProj(int mode, float marginDeg);
    int EyeProj();
    float EyeProjMargin();
    //! Stereo eye `eye` (0/1): the frustum it is rendered with (`t`, in its RENDER frame) and that frame's rotation
    //! relative to the eye (`rot`, x y z w: v_eye = rot * v_render; the identity in modes 0 and 2). The eye's own frame
    //! is its located pose (cant included). Both eyes padded to one tan extent (one render size, one pixel density).
    //! False = not known yet (no headset fov).
    bool EyeTans(int eye, Tans& t, float rot[4] = nullptr);
    //! Mode 1: the full fov (degrees) stereo eye `eye`'s view is built with (the projection builder's hook writes it).
    bool EyeViewFov(int eye, float& xDeg, float& yDeg);
    //! The frustum that covers both eyes from the head (mono): their native fovs' corners turned into the head's frame.
    bool UnionTans(Tans& t);
    //! Mode 2: the main view's projection rewritten to eye `eye`'s frustum (ViewMatrixHook, after CreateViewMatrix). `out`
    //! = the tans the view is rendered with now: the eye's, or the symmetric ones it was built with when nothing was applied.
    bool ApplyEyeProjection(void* view, int eye, Tans& out);
    //! The symmetric frustum the main view's projection was last built with (its fov).
    Tans BuiltTans();
    //! The tan extents (r - l, u - d) the render target must hold at the eye's pixel density (RenderRes): the per-eye
    //! frustum while it is in effect, else the symmetric fov override's. False = neither.
    bool RenderExtent(float& ex, float& ey);
    std::string EyeProjStatus();

    //! 0 = passthrough (log only), 1 = HMD orientation replaces the game's
    //! pitch/roll on top of the game yaw + positional offset.
    void SetMode(int mode);
    int  Mode();
    //! Zeroes the positional offset at the current head position.
    void Recenter();
    void SetWorldScale(float metersToUnits);

    //! The main (scene) view object last seen by the hook, or null. Used by ViewMatrixHook.
    void* MainView();
    //! One of the last few objects MainView() returned (the engine may keep more than one): a render task's view
    //! pointer is checked against these before anything is read from it.
    bool IsMainViewObject(const void* v);

    struct Stats
    {
        unsigned long long calls = 0, mainViewCalls = 0, applied = 0, refreshes = 0;
        float lastOrg[3] = {}, lastFwd[3] = {}, lastFovX = 0, lastFovY = 0;
        int lastW = 0, lastH = 0;
    };
    Stats GetStats();
}
