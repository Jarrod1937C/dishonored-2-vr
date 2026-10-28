#pragma once
#include <cstdint>
#include <string>
namespace VRRuntime { struct PoseSet; }
namespace BodyHide { struct Anchor; }

//! THE 6DOF hook. idRenderMatrix::CreateViewMatrix(const idVec3* org, const idMat3* axis,
//! idRenderMatrix* out) = exe+0x109CC0 is what turns m_renderRV.vieworg/viewaxis into the
//! scene's view matrix (called from the view-setup chunk at exe+0x375B7F, right before the
//! inverse / view-projection products). We intercept the call made for the main view and
//! hand the engine an HMD-composed origin/axis instead. The renderView itself is untouched,
//! so nothing accumulates and the game's own values stay intact.
namespace ViewMatrixHook
{
    constexpr uint32_t kCreateViewMatrix = 0x109CC0;
    bool Install();
    void Uninstall();
    void SetEnabled(bool on);
    bool Enabled();
    void Recenter();
    void SetWorldScale(float s);
    //! Head-aim mode: keep the game's view axis (it already follows the HMD via KbmInject)
    //! and only add the HMD positional offset.
    void SetKeepGameAxis(bool on);
    //! Head-aim mode: hold the game eye's offset from the body axis fixed in the room (1, default) instead of letting it
    //! swing with the body's yaw (0, the old behaviour; measured as a 1.6 deg world shift over a 60 deg head turn).
    void SetPivotFix(bool on);
    //! [Display] CameraPitchFix: the game eye's forward/up travel with the game's pitch (a flat screen's neck) measured
    //! while standing still and taken out of play's view: the headset already moves the eyes with the real neck.
    void SetPitchFix(bool on);
    std::string PitchFixStatus();          //!< the measured curve (the heartbeat)
    //! The takedown view, in head-aim mode while PlayerState reports a scripted animation: 0 the game's camera,
    //! 1 the animation's camera plus the head's turn since it began, 2 (the default of vr weapon6dof) rotation from
    //! the head alone with the animation's position, rate-limited to `speed` m/s so its pops become a quick glide.
    void SetAnimCam(int mode);
    int  AnimCam();
    void SetAnimCamSpeed(float mps);
    float AnimCamSpeed();
    //! True from the first frame of a takedown until the view has been handed back to the game's camera.
    bool AnimViewOn();
    //! The same for any head-driven view: a takedown, a cutscene, the power wheel.
    bool AnimViewActive();
    //! While a takedown / cutscene view is on (mode 2, not the wheel): the room <-> game yaw it renders with, game yaw =
    //! room yaw + deg. The servo's target, the layer pose and the hands follow it, so all of them agree with the view.
    bool AnimYawOffset(float& deg);
    //! A cutscene's camera jumping this far, or turning this much, in one frame is a cut (a new shot): `vr cinecut`.
    void SetCineCut(float m, float deg);
    //! A cutscene's eye rides the cutscene camera however fast it moves; only a jump its motion does not explain is
    //! smoothed (1, default) -- or the old takedown follow, at most `animcamspeed` m/s (0).
    void SetCineTrack(bool on);
    //! In a cutscene that carries the player (a train, a skiff), the view turns as the player's body turns (1, default).
    void SetCineVehicle(bool on);
    //! Inside a possessed host: the game's yaw, the head's pitch, level roll (1, default), or the game's camera (0).
    void SetPossessView(bool on);
    std::string AnimStats();
    struct Stats { unsigned long long calls = 0, mainCalls = 0, applied = 0; float lastOrg[3] = {}, lastFwd[3] = {}; };
    Stats GetStats();

    //! The main view as the renderer actually received it this frame: the 4x4 idRenderMatrix the engine built
    //! (`out` of CreateViewMatrix) plus the origin/axis it was built from, after the HMD composition.
    //! GpuTrace needs it to express a world-space controller pose in the same view space the weapon's MVP lives in
    //! — the two must come from ONE source, because in head-aim mode the render axis is the game's, not the HMD's.
    //! False until the main view has been seen once. `mat` is 16 floats, row-major as the engine stores it.
    //! `headAxis` (optional): the HEAD's axis that view was composed from -- without a stereo eye's turn to its frustum's
    //! centre and without the cant of canted displays. Whatever takes only the view's YAW (the hands' room -> world map)
    //! must take it from here: with the eye's own axis each eye put the hands ~7 deg aside, each its own way.
    bool RenderView(float mat[16], float org[3], float axis[9], unsigned long long* serial = nullptr, float* headAxis = nullptr);
    //! The same for a given main view (the last 16 are kept): false when it is gone.
    bool ViewBySerial(unsigned long long serial, float mat[16], float org[3], float axis[9], float* headAxis = nullptr);
    //! The first-person entities (BodyHide::aCount anchors) and the game camera as they were when view `serial` was built
    //! -- the right eye of a synchronised pair has its left eye's -- so GpuTrace places each moved draw with ITS OWN view's
    //! transform and entities (one game tick). False when it has none (a flat view, or gone from the ring).
    bool ViewEntities(unsigned long long serial, BodyHide::Anchor* anch, float cam[3], float camAxis[9], bool* camValid);
    //! Which main view a draw belongs to, from the view matrix in ITS PerViewCB (StereoTaa::ViewMatrixOf): the serial
    //! of the newest view built with that matrix (either storage order), 0 = none of the last 16.
    unsigned long long SerialOfViewMatrix(const float m[16]);
    unsigned long long LatestSerial();
    //! The latest main view's origin, the GAME eye it was composed from (before the HMD offset, the pivot fix and the
    //! takedown view) and the XR position of the head/eye pose used. viewOrg - gameOrg is the composition's offset:
    //! added to the game eye of a LATER frame it says where that frame will be rendered from, which is how the game
    //! thread places something exactly where the next view will draw it (GunRay). False before an HMD-composed view.
    bool ViewAnchor(float viewOrg[3], float gameOrg[3], float hmdPos[3]);

    //! The pose the main view was ACTUALLY rendered from, in XR LOCAL space -- what the projection layer must claim
    //! so the compositor's timewarp can correct the difference to the head's pose at display time. In head-aim mode
    //! the render takes the GAME's axis (the servo's yaw and pitch, no roll), not the HMD's: claiming the HMD's
    //! orientation hid the servo's lag and the head's roll from the compositor, so both showed as the world moving.
    //! The game axis is carried into XR by the servo's world anchor (KbmInject::WorldYawOffset).
    //! `serial` 0 = the latest; otherwise the view with that serial if it is still in the ring.
    //! `flat`: the view was the flat game's own (ScreenMode::FlatRender), a picture for the menu screen, not for the eyes.
    //! `pair`: synchronised stereo (StereoSync), the serial of the LEFT view of the pair this view belongs to (its own
    //! for the left one); 0 = not part of a pair.
    //! `dispTime` / `dispBase` / `predKind` / `predicted`: the display time the view's poses were located for, the frame
    //! display time that prediction started from, the kind (VRRuntime::LocateForView) and whether it was a prediction.
    //! `tan` / `tanValid`: the frustum the view was RENDERED with, in its own frame (left, right, up, down tangents): the
    //! eye's own with the per-eye projection (CameraHook::ApplyEyeProjection), else the symmetric one of its fov. Each
    //! image is drawn into the eyes at exactly these angles, so a view whose projection differs from the next one's (a
    //! stereo pair, a mono view between pairs) is never placed with another view's frustum.
    struct XrPose { bool valid = false; unsigned long long serial = 0; float quat[4] = { 0, 0, 0, 1 }; float pos[3] = {}; bool fromGameAxis = false; int eye = -1; bool flat = false;
                    unsigned long long pair = 0; long long dispTime = 0, dispBase = 0; int predKind = -1; bool predicted = false;
                    float tan[4] = {}; bool tanValid = false; bool canted = false;
                    //! `warp`: a stereo eye view's render frame relative to that eye (v_eye = warp * v_render): its turn
                    //! towards its frustum's centre (CameraHook::EyeTans, mode 1); the identity otherwise. `quat` is the
                    //! render frame's: the eye itself was at quat * conj(warp).
                    float warp[4] = { 0, 0, 0, 1 }; };
    XrPose RenderPoseXr(unsigned long long serial = 0);
    //! The eye the main view about to be built will be rendered for (0/1, -1 = mono or flat): the decision Hook makes,
    //! from the same state. For the projection builder's hook, which runs just before it in the same view setup.
    int NextViewEye();
    //! The poses (eyes, head, controllers) a main view was built from; serial 0 = the latest. False when it is gone.
    bool ViewPoses(unsigned long long serial, VRRuntime::PoseSet& out);
}
