#pragma once
#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D11Texture2D;
struct XrCompositionLayerProjection;
struct XrCompositionLayerQuad;
struct XrCompositionLayerProjectionView;
struct ID3D11ShaderResourceView;

//! One eye's view pose + projection, in plain types (no OpenXR headers leaked).
struct XREyePose
{
    float posX = 0, posY = 0, posZ = 0;                 //!< position (meters)
    float quatX = 0, quatY = 0, quatZ = 0, quatW = 1;   //!< orientation quaternion
    float fovLeft = 0, fovRight = 0, fovUp = 0, fovDown = 0; //!< angles (radians)
    bool valid = false;
    bool held = false;   //!< tracking is lost: this is the last good pose, held (see XRSession::BeginFrame)
};

//! One controller's aim pose (in the reference space).
struct XRHandPose
{
    float posX = 0, posY = 0, posZ = 0;
    float quatX = 0, quatY = 0, quatZ = 0, quatW = 1;
    bool valid = false;
    bool held = false;   //!< tracking is lost: this is the last good pose, held (see XRSession::SyncInput)
};

//! The wrist HUD layer for one frame (WristHud fills it, XRSession draws and submits it). A square quad whose pose is
//! given in the left hand's GRIP space, so the compositor places it on the hand at display time.
struct XRWristLayer
{
    ID3D11ShaderResourceView* srv = nullptr;   //!< the interface texture (premultiplied alpha); null = no layer
    unsigned srcW = 0, srcH = 0;               //!< its size in pixels
    float center[2] = {};                      //!< crop centre in source pixels
    float half = 0;                            //!< crop half-side in source pixels
    float edgeFade = 0;                        //!< px: alpha fades out over this distance from the source's top edge
    float alpha = 0;                           //!< overall opacity (the fade in/out); 0 = no layer
    float backdrop = 0;                        //!< opacity of a soft dark disc behind the interface
    float size = 0.08f;                        //!< side of the quad, metres
    float quat[4] = { 0, 0, 0, 1 };            //!< pose in the chosen space (below)
    float pos[3] = {};
    int space = 1;                             //!< 1 = the left AIM pose (what the mod draws the arm from), 0 = grip
    bool gamma = false;                        //!< the source is gamma-encoded, premultiplied in gamma space (WristHud::Texture)
};

//! A flat panel in the room (XRSession::SetPanelLayer): a crop of a texture drawn into its own swapchain with rounded,
//! anti-aliased edges and submitted as a quad layer in LOCAL space. One frame: set again every frame it is wanted.
//! Slot 0 = the menu screen (ScreenMode), slot 1 = the power wheel's panel.
struct XRPanelLayer
{
    ID3D11Texture2D* tex = nullptr;            //!< source to COPY from (the backbuffer: not shader-readable), or
    ID3D11ShaderResourceView* srv = nullptr;   //!< a shader-readable source (the interface texture, premultiplied)
    unsigned srcW = 0, srcH = 0;               //!< source size, pixels
    float crop[4] = {};                        //!< x, y, w, h of the source shown, pixels
    bool premultiplied = false;                //!< the source's alpha is coverage (interface); else it is opaque
    bool gamma = false;                        //!< ...gamma-encoded and premultiplied in gamma space (WristHud::Texture)
    float alpha = 0;                           //!< overall opacity (fades); 0 = no layer
    float corner = 0;                          //!< corner radius, fraction of the quad's height
    float soft = 0;                            //!< alpha ramps to 0 over this width at the edges, fraction of the height
    float quat[4] = { 0, 0, 0, 1 };            //!< pose in LOCAL space; the quad's +Z faces the viewer
    float pos[3] = {};
    float width = 0, height = 0;               //!< metres
    unsigned outW = 0, outH = 0;               //!< swapchain size (the compositor samples it at display resolution)
};

//! What surrounds the menu screen (XRSession::SubmitScreen): the projection layer, drawn for each eye from its pose
//! right now -- a dim room-locked environment with, when there is one, the last picture of the game frozen in the room
//! where it was seen, darkened.
struct XRBackdrop
{
    float snapshot = 0;                        //!< brightness of the frozen game picture (0 = not shown)
    float env = 1;                             //!< brightness of the environment
    float fade = 1;                            //!< everything (the transition in)
    float desat = 0.35f;                       //!< how much colour the frozen picture loses
    float glowDir[3] = { 0, 0, -1 };           //!< the screen's centre, LOCAL direction from the eyes: a faint light spill
    float glow = 0;
};

//! Button bits in XRInputState::buttons / XRRenderHook::Buttons().
enum : unsigned
{
    kXRBtnA = 1, kXRBtnB = 2, kXRBtnX = 4, kXRBtnY = 8,
    kXRBtnThumbL = 16, kXRBtnThumbR = 32,
    kXRBtnMenu = 64,   //!< left controller menu button (the only spare Touch button)
};

//! Snapshot of controller input for a frame.
struct XRInputState
{
    float turnX = 0.0f;          //!< right thumbstick X (-1..1), for turning
    float turnY = 0.0f;          //!< right thumbstick Y (-1..1), for stick pitch
    float moveX = 0.0f, moveY = 0.0f; //!< left thumbstick, for locomotion
    float triggerL = 0.0f, triggerR = 0.0f; //!< 0..1
    float gripL = 0.0f, gripR = 0.0f;       //!< squeeze, 0..1 (Index: how far the fingers close round it, not force)
    //! The grip as a BUTTON, 0..1: what the grip actions (grab, block, the wheel) are decided on. Touch: the squeeze
    //! itself; Valve Index: its FORCE sensor -- its squeeze value is the fingers' curl, near 1 with the hand just resting
    //! on the grip. The squeeze value when the runtime binds nothing to it.
    float gripPressL = 0.0f, gripPressR = 0.0f;
    //! The trackpads (Valve Index only: bound for no other controller): position -1..1 and press force 0..1, per hand
    //! (0 = left). `pad` = the runtime has them (an Index controller is bound).
    float padX[2] = {}, padY[2] = {}, padForce[2] = {};
    bool pad = false;
    //! The fingers' curl, 0 = straight .. 1 = a fist, per hand (0 = left), thumb index middle ring little.
    //! fingerSrc: 0 none, 1 hand tracking (XR_EXT_hand_tracking: on an Index, SteamVR's from the finger sensors),
    //! 2 the controller's own sensors (Index: trigger, squeeze curl, thumb on a button; three groups, not five).
    //! Only while SetFingerTracking is on and an Index is bound.
    float fingerCurl[2][5] = {};
    int fingerSrc[2] = {};
    unsigned buttons = 0;        //!< kXRBtn* mask (a/b/x/y, stick clicks)
    XRHandPose hand[2];          //!< 0 = left, 1 = right (aim pose)
    //! XrTime (ns) the poses were predicted for. Velocities come from differences of this, not of the wall clock:
    //! the poses are PREDICTED for the display time, so two frames' poses are one display period apart whatever
    //! jitter the Present thread had between them.
    long long time = 0;
};

//! Manages the full OpenXR D3D11 session lifecycle. Designed to be driven from
//! the render thread (where the game's D3D11 device/context/backbuffer live).
//! No-ops gracefully if no OpenXR runtime is installed.
class XRSession
{
public:
    static constexpr int kMaxEyes = 2;

    //! Creates instance/system/session/swapchains bound to the game's device.
    //! @returns true on success. On failure, LastError() explains why.
    bool Init(ID3D11Device* device);
    bool IsInitialized() const { return m_Initialized; }
    const std::string& LastError() const { return m_LastError; }
    //! Init failed in a way that may pass later (no headset yet, the runtime still starting): call Init again.
    bool ShouldRetry() const { return m_Retry; }
    //! The headset connection or the runtime was lost: Shutdown, then Init again (VRRuntime does both).
    bool NeedsRestart() const { return m_Restart; }
    //! The runtime asked the app to leave VR (EXITING): stay flat, do not re-create the session.
    bool Exited() const { return m_Exited; }
    //! The controller profiles the runtime bound, e.g. "left /interaction_profiles/oculus/touch_controller, right ...".
    const std::string& Profiles() const { return m_Profiles; }
    //! [Controls] ControllerPitchOffset (degrees, + = up) and [Advanced] LogControllerInput. Before Init.
    void SetPitchOffset(float deg) { m_PitchOffset = deg; }
    void SetLogInput(bool on) { m_LogInput = on; }
    //! [Controls] FingerTracking: read the fingers' curl (XRInputState::fingerCurl) while an Index is bound.
    void SetFingerTracking(bool on) { m_WantFingers = on; }

    //! Pumps the OpenXR event queue; drives session state (begins/ends session). Since part 51 the OpenXR frame thread
    //! (below) does this; a call from the render thread is a no-op while that thread runs.
    void PollEvents();
    bool IsSessionRunning() const { return m_SessionRunning.load(); }
    int SessionState() const { return m_SessionState; }
    //! How many times the runtime moved the reference space (the user recentred: a long press of the Meta button).
    unsigned RecenterEvents() const { return m_RecenterEvents; }
    //! Recentres that have TAKEN EFFECT: bumped on the first frame whose poses are in the new space (the event's
    //! changeTime), so a consumer that compares it with the value it last saw re-seeds on exactly the frame the
    //! poses jump -- never before (the old space is still in force) and never after (one frame of jump leaks).
    unsigned RecenterGeneration() const { return m_RecenterGen; }
    //! Frames on which a device's pose was undefined and the last good one was held instead: 0 left, 1 right, 2 head.
    unsigned long long LostFrames(int dev) const { return (unsigned)dev < 3 ? m_LostFrames[dev] : 0; }

    //! Waits for the next frame and locates the eye views.
    //! @returns true if the app should render this frame.
    bool BeginFrame();

    //! Copies the source texture into eye swapchain image(s) and submits the
    //! projection layer. onlyEye = -1 copies into both eyes (mono); 0 or 1
    //! copies into that eye only — the other eye keeps its last released image
    //! (alternating-eye stereo). Must be called after BeginFrame() returned true.
    void SubmitFrame(ID3D11Texture2D* source, int onlyEye = -1);

    //! Synchronised stereo (StereoSync): the LEFT image of a pair is kept (a copy) with the pose it was rendered from,
    //! until its right image arrives a frame later; then SubmitPair draws both into their eyes and submits them in one
    //! frame. No OpenXR call: the left image's Present needs no frame of its own. Render thread.
    //! `tan`: the frustum each image was rendered with (left, right, up, down tangents in its own frame; null = the
    //! source tans in force, SetSourceTans).
    //! `warp`: its render frame relative to the eye (v_eye = warp * v_render; null = the same), see SetSourceWarp.
    bool StagePairLeft(ID3D11Texture2D* source, const float quat[4], const float pos[3], unsigned long long pair, const float* tan = nullptr, const float* warp = nullptr);
    //! False (nothing submitted) when the staged left image is not this pair's: the caller submits otherwise.
    bool SubmitPair(ID3D11Texture2D* right, const float quat[4], const float pos[3], unsigned long long pair, const float* tan = nullptr, const float* warp = nullptr);
    unsigned long long PairsSubmitted() const { return m_PairsSubmitted; }
    void DropStagedPair() { m_PairStaged = false; }   //!< the staged left image is not to be paired (the eye phasing disagreed)
    //! The last staged left image copied into `dst` (the desktop window shows one eye: VRRuntime's vr mirroreye).
    bool CopyPairLeft(ID3D11Texture2D* dst);
    unsigned EndFrameFails() const { return m_EndFrameFails; }
    //! The last frame's projection layer again (the swapchains keep their last released images). After BeginFrame.
    bool ResubmitLast();

    //! Full-rate stereo: submit BOTH eyes in one frame. Each source was
    //! rendered with that eye's pose ±IPD/2 AND its native fov (matched-fov),
    //! so both map across the full lens. Call after BeginFrame() returned true.
    void SubmitFrameStereo(ID3D11Texture2D* eye0, ID3D11Texture2D* eye1);

    //! Last swapchain image each eye was copied into (render thread only; valid
    //! while the session lives). For per-eye capture verification.
    ID3D11Texture2D* LastEyeTexture(int i) const { return m_LastEyeTex[i]; }

    //! Ends a frame that produced no rendering (BeginFrame returned false).
    void EndFrameNoRender();

    //! Game camera FOV (radians). When set (> 0), the projection layer
    //! advertises this SYMMETRIC fov for the submitted image instead of each
    //! eye's native asymmetric fov. A flat mono frame mapped onto two
    //! different asymmetric eye projections lands at different angles per eye
    //! and cannot be fused (seen as double vision on Quest 2); a common
    //! symmetric fov puts it at the same angular position in both eyes and at
    //! its true angular size.
    void SetLayerFov(float hRad, float vRad) { m_LayerFovH = hRad; m_LayerFovV = vRad; }

    //! Matched-fov stereo: the incoming frames were RENDERED for the target
    //! eye (pose ±IPD/2) with a SYMMETRIC fov spanning ±tanH x ±tanV (the
    //! engine cannot render asymmetric projections). Submission crops the
    //! eye's native fov out of the frame — full lens coverage, exact mapping.
    //! Off = flat mono frame (tan-space optical-center placement).
    void SetMatchedFov(bool matched) { m_MatchedFov = matched; }

    //! Orthoscopic mono placement (default on). The flat frame is DRAWN (scaled, filtered) into each eye's native fov
    //! at the angles it was rendered for: the source covers tan [l, r] x [d, u] of its own view, so it lands there in
    //! the eye. The old placement copied it 1:1 in pixels around the optical axis, so its angular size depended on
    //! the backbuffer's pixel count against the swapchain's: measured on the Quest 3, a 95.4 x 58.7 deg render showed
    //! as 80 x 61 deg -- squeezed 24% sideways, stretched 5% vertically, the hands pulled 11 deg towards the middle.
    void SetOrtho(bool on) { m_Ortho = on; }
    bool Ortho() const { return m_Ortho; }
    void SetSourceTans(float l, float r, float u, float d) { m_SrcTan[0] = l; m_SrcTan[1] = r; m_SrcTan[2] = u; m_SrcTan[3] = d; }
    //! The layer declares the fov each image was RENDERED with (its source tans, the whole image at the eye's pixel
    //! density) instead of the eye's native fov with the image drawn into it (VRRuntime::SetLayerFov). In force from the
    //! next submit on, until changed. Both are exact when the image's frame is the eye's; only the declared one is when
    //! it is not (a mono image on canted displays), and only it hands the compositor a margin beyond the eye's fov.
    void SetDeclareFov(bool on) { m_Declare = on; }
    bool DeclareFov() const { return m_Declare; }
    //! The pose (XR LOCAL) the frame about to be submitted was RENDERED from (ViewMatrixHook::RenderPoseXr). The mono
    //! layer claims it instead of the pose just located, so the compositor timewarps the real difference. One submit.
    void SetRenderPose(const float q[4], const float p[3]) { for (int i = 0; i < 4; i++) m_RenderPose[i] = q[i]; for (int i = 0; i < 3; i++) m_RenderPose[4 + i] = p[i]; m_RenderPoseValid = true; }
    bool OrthoReady() const { return m_Ortho && m_SrcTan[1] > m_SrcTan[0] && m_SrcTan[2] > m_SrcTan[3]; }
    unsigned long long OrthoDraws() const;
    void SetRenderedTans(float tanH, float tanV) { m_RenderedTanH = tanH; m_RenderedTanV = tanV; }

    //! Size of the image actually submitted for eye i — the pixels the lens
    //! gets, against the runtime's recommended EyeWidth/Height. The one number
    //! that says how sharp the headset can possibly look.
    void ContentSize(int i, int& w, int& h) const { w = m_ContentW[i]; h = m_ContentH[i]; }
    //! Where eye i's image went: the content rect inside its swapchain and the swapchain's size (BodyView).
    void Placement(int i, int r[4], uint32_t sw[2]) const
    { r[0] = m_ContentX[i]; r[1] = m_ContentY[i]; r[2] = m_ContentW[i]; r[3] = m_ContentH[i]; sw[0] = m_SwapW[i]; sw[1] = m_SwapH[i]; }

    //! HUD quad: when non-null, this texture (alpha = UI coverage) is copied
    //! into a dedicated swapchain and submitted as a head-locked quad layer on
    //! top of the projection layer — crisp UI at a comfortable virtual depth.
    //! Set per frame from the render thread before SubmitFrame.
    void SetQuadSource(ID3D11Texture2D* tex) { m_QuadSource = tex; }

    //! Quad placement. locked=false (default): head-locked (VIEW space, always
    //! in front of you). locked=true: WORLD-locked — on the first locked frame
    //! the panel pose is captured in LOCAL space (in front of where the head
    //! is right then, upright, facing the user) and stays there while you move
    //! your head (floating PDA/inventory panel). Unlocking clears the pose.
    void SetQuadLocked(bool locked) { m_QuadLocked = locked; if (!locked) m_QuadLockValid = false; }

    //! Quad distance from the head (m) and width (m), for both placements.
    void SetQuadParams(float dist, float width) { m_QuadDist = dist; m_QuadWidth = width; }

    //! The wrist HUD (see XRWristLayer). Set per frame before SubmitFrame; alpha 0 or a null srv = no layer.
    void SetWristLayer(const XRWristLayer& l) { m_Wrist = l; }

    //! A panel in the room (see XRPanelLayer), slot 0 or 1. Set per frame before the frame is submitted.
    void SetPanelLayer(int slot, const XRPanelLayer& l) { if ((unsigned)slot < 2) m_Panel[slot] = l; }
    //! The menu screen's frame (ScreenMode): instead of the game's picture across the eyes, the backdrop in the
    //! projection layer (each eye drawn for its pose right now) and panel slot 0 over it. Ends the frame.
    void SubmitScreen(const XRBackdrop& b);
    //! Brightness of the game's picture in this frame's projection layer (the fade back in after a menu). One frame.
    void SetFrameFade(float k) { m_FrameFade = k; }
    //! A further dimming of this frame's game picture, on top of the fade (0 = black): the snap-turn blank
    //! (KbmInject::SnapBlankFade). One frame, the lowest asked wins.
    void DimFrame(float k) { if (k < m_FrameDim) m_FrameDim = k; }
    //! The frozen backdrop: every play frame offers its picture (SubmitFrame keeps one every ~0.2 s, half size, with
    //! the pose it was seen from). Freeze picks the newest one at least minAge seconds old (the menu was already
    //! drawn over the last few frames before it was recognised) and at most maxAge; false = none. Unfreeze lets the
    //! ring run again.
    bool FreezeBackdrop(float minAge, float maxAge);
    void UnfreezeBackdrop();
    bool BackdropFrozen() const { return m_Frozen >= 0; }
    float BackdropAge() const;                 //!< seconds between the frozen picture and the freeze (-1: none)
    //! The next submitted frame shows its picture to BOTH eyes (alternating stereo coming back from a menu: the other
    //! eye's swapchain still holds the backdrop). One frame.
    void ResyncEyes() { m_ResyncEyes = true; }
    //! vr backdropeyes 0|1: the menu screen's frozen backdrop keeps both eyes' pictures of a synchronised pair (1) or
    //! the right eye's only (0, before 2026-10-23: the left eye's outer edge showed the dim environment)
    void SetBackdropEyes(bool on) { m_BackdropEyes = on; }
    bool BackdropEyes() const { return m_BackdropEyes; }
    unsigned long long ScreenFrames() const { return m_ScreenFrames; }
    unsigned long long PanelFrames(int slot) const { return (unsigned)slot < 2 ? m_PanelFrames[slot] : 0; }
    unsigned long long Snapshots() const { return m_Snapshots; }
    //! The left hand's pose this frame in the reference space (LOCAL): `space` 1 = the AIM pose as the runtime gives it
    //! (before ControllerPitchOffset), 0 = the grip pose (the aim pose stands in if the runtime has none). False if it
    //! is not tracked.
    bool WristPose(int space, float q[4], float p[3]) const;
    float PitchOffset() const { return m_PitchOffset; }
    unsigned long long WristLayers() const { return m_WristLayers; }   //!< frames that carried the wrist layer

    //! Pose prediction (VRRuntime::LocateForView): both eyes and both controllers' aim poses located for display time
    //! `time` (XrTime, ns), as the image about to be built will be shown -- not the time of the last frame. The
    //! controllers get ControllerPitchOffset as in SyncInput. False (nothing written) when the session does not run, the
    //! head is not tracked, or a recenter takes effect before `time` (its generation is bumped by BeginFrame only).
    //! Render thread: OpenXR allows xrLocateViews / xrLocateSpace while the frame thread is inside xrWaitFrame.
    //! `head` (optional): the head at that time (see Head()).
    bool LocateAt(long long time, XREyePose eyes[2], XRHandPose hands[2], XREyePose* head = nullptr);
    //! The begun frame's predicted display time and period (XrTime, ns); 0 before the first frame.
    long long FrameDisplayTime() const;
    long long FramePeriod() const;
    unsigned long long LocatesAhead() const { return m_LocatesAhead; }

    //! Syncs controller actions and locates hand poses for this frame.
    //! Call after BeginFrame(). No-op if input wasn't set up.
    void SyncInput();
    const XRInputState& Input() const { return m_Input; }
    //! One haptic pulse on a controller (0 left, 1 right): amplitude 0..1, duration in seconds, frequency in Hz
    //! (0 = the runtime's default). Render thread, like everything else here. False if there is no haptic output.
    bool Vibrate(int hand, float amplitude, float seconds, float hz = 0.f);
    unsigned long long HapticPulses() const { return m_HapticPulses; }

    //! Tears everything down. False (nothing done yet: call again) while the OpenXR frame thread is still inside a
    //! runtime call that does not return; processExit = from DllMain at exit (the other threads are already gone).
    bool Shutdown(bool processExit = false);
    //! The restart asked for is a NEW session for one the runtime stopped and did not offer back (not a lost connection):
    //! no reason to wait before starting it.
    bool RestartIsFresh() const { return m_RestartFresh; }

    // ---- The OpenXR frame thread ----
    // xrWaitFrame used to run on the game's render thread, so a runtime that stopped releasing frames (Air Link's
    // stream stuck: measured 33 s on the user's Quest 3) froze the game on the PC too, and every hitch of the game's own
    // left the headset without frames (the hourglass; the runtime then stops the session). Now a thread of its own
    // calls xrWaitFrame for the render thread (BeginFrame waits for it, bounded: past the bound the game goes on and that
    // Present is not sent), polls the events, and while the render thread sends nothing it keeps the headset fed with the
    // last frame's layers (the last picture held in the room).
    void SetKeepAlive(int mode) { m_KeepAliveMode = mode; }     //!< vr keepalive 0|1
    int KeepAlive() const { return m_KeepAliveMode; }
    void SetWaitBound(int ms) { m_WaitBoundMs = ms < 1 ? 1 : ms; }  //!< vr xrwaitbound <ms>: BeginFrame's bound
    //! vr xrkick <s> [all]: 0 = never start a new session. Only on Meta's runtime (where it was measured; the mock
    //! emulates it) unless `all`: elsewhere a stopped session is left to the runtime to offer back.
    void SetKick(float firstSec, bool anyRuntime = false) { m_KickSec = firstSec; m_KickAny = anyRuntime; }
    bool KickApplies() const;
    std::string PumpStatus() const;                              //!< the heartbeat line
    //! vr d3dmt 0|1 (research): the device's multithread protection, to measure what it costs (0 makes the keep-alive's
    //! device lock a no-op: do not leave it off)
    void SetD3DProtect(bool on);

    int EyeCount() const { return m_ViewCount; }
    const XREyePose& Eye(int i) const { return m_Eyes[i]; }
    //! The HEAD this frame: position = the eyes' midpoint, orientation = the VIEW reference space's (the head's own
    //! frame). Not the left eye's: on a headset with canted displays (Pimax without parallel projections) each eye is
    //! turned ~10 deg out from the head, and everything that took the left eye for the head was 10 deg off. When the
    //! runtime cannot locate VIEW (or gives something implausible) the two eyes' orientations averaged. No fov.
    const XREyePose& Head() const { return m_Head; }
    //! How far eye i's orientation is turned from the head's (degrees): 0 on parallel displays.
    float EyeCant(int i) const;
    uint32_t EyeWidth() const { return m_EyeWidth; }
    uint32_t EyeHeight() const { return m_EyeHeight; }
    uint64_t FramesSubmitted() const { return m_FramesSubmitted; }
    const std::string& RuntimeName() const { return m_RuntimeName; }
    const std::string& SystemName() const { return m_SystemName; }

private:
    bool m_Initialized = false;
    bool m_Retry = false, m_Exited = false;
    std::atomic<bool> m_Restart{ false };
    bool m_RestartFresh = false;
    int m_KeepAliveMode = 1, m_WaitBoundMs = 50;
    float m_KickSec = 5.f;
    bool m_KickAny = false;
    unsigned m_EndFrameFails = 0;   //!< xrEndFrame calls of the game's frames the runtime refused
    void PollEventsNow();                  //!< the event loop itself (the frame thread)
    void PumpLoop();
    static unsigned long __stdcall PumpThread(void* self);
    bool StartPump();
    bool StopPump(unsigned waitMs);
    void KeepAliveFrame(const void* frameState);
    void SaveLayers(const void* layers, unsigned count);
    void FrameEnded();
    void EndSessionWhenFree();
    int m_InitAttempts = 0;
    std::string m_Profiles;
    float m_PitchOffset = 0.f;
    bool m_LogInput = false;
    bool m_WantFingers = false;
    void ReadFingers();                      //!< SyncInput: XRInputState::fingerCurl
    void DestroyHandTrackers();
    void LogInputChanges();
    std::atomic<bool> m_SessionRunning{ false };
    int m_SessionState = 0;         //!< XrSessionState as int
    unsigned m_RecenterEvents = 0, m_RecenterGen = 0;
    bool m_RecenterPending = false; long long m_RecenterAt = 0;   //!< XrTime the pending change takes effect
    unsigned long long m_LostFrames[3] = {};
    unsigned long long m_LocatesAhead = 0;
    int m_ViewCount = 0;
    uint32_t m_EyeWidth = 0, m_EyeHeight = 0;
    uint64_t m_FramesSubmitted = 0;
    bool m_FrameStarted = false;
    bool m_ShouldRender = false;
    std::string m_LastError;
    std::string m_RuntimeName;
    std::string m_SystemName;
    XREyePose m_Eyes[kMaxEyes];
    XREyePose m_Head;                         //!< Head()
    unsigned long long m_HeadFromView = 0, m_HeadAveraged = 0;
    //! The head for these eyes at `time`: VIEW located, checked against the eyes, else their average.
    void LocateHead(long long time, const XREyePose eyes[2], XREyePose& head);
    XRInputState m_Input;
    bool m_InputReady = false;
    bool m_HapticReady = false;
    unsigned long long m_HapticPulses = 0;
    ID3D11Texture2D* m_LastEyeTex[kMaxEyes] = {}; //!< non-owning (swapchain images)
    float m_LayerFovH = 0, m_LayerFovV = 0;  //!< game fov for the layer (radians)
    bool m_MatchedFov = false;               //!< frames rendered with eye fov
    float m_RenderPose[7] = {}; bool m_RenderPoseValid = false;   //!< SetRenderPose
    bool m_Ortho = true;                     //!< mono frame drawn at its true angular size (SetOrtho)
    float m_SrcTan[4] = {};                  //!< the source's tan extents: left, right, up, down (0 = unknown)
    bool m_Declare = false;                  //!< SetDeclareFov
    bool m_EyeDecl[kMaxEyes] = {};           //!< eye i's last image went out with a declared fov (m_EyeDeclTan)
    float m_EyeDeclTan[kMaxEyes][4] = {};
    float m_PairTan[4] = {};                 //!< the staged left image's tans
    //! Eye i's layer view: the pose its image was rendered at, and its fov / sub-image as last drawn (declared or native).
    void FillEyeView(int i, XrCompositionLayerProjectionView& pv) const;
    //! Where eye i's image goes in its swapchain: a declared image fills (0,0)-(w,h) at the eye's density; a native one
    //! is the source's tan rect inside the eye's fov. Grows the swapchain as needed. False = no fov yet.
    bool EyeRect(int i, const float tan[4], bool declare, float& x, float& y, float& w, float& h);
    float m_RenderedTanH = 0, m_RenderedTanV = 0; //!< symmetric render half-tangents
    bool m_FovLogged = false;                //!< one-shot native-fov log
    ID3D11Texture2D* m_QuadSource = nullptr; //!< HUD texture for the quad layer
    uint32_t m_QuadW = 0, m_QuadH = 0;       //!< quad swapchain size
    bool m_QuadLocked = false;               //!< world-locked panel mode
    bool m_QuadLockValid = false;            //!< a lock pose has been captured
    float m_QuadLockPos[3] = {};             //!< captured pose (LOCAL space)
    float m_QuadLockQuat[4] = { 0, 0, 0, 1 };
    float m_QuadDist = 1.4f, m_QuadWidth = 1.4f;
    XRWristLayer m_Wrist;                    //!< this frame's wrist HUD (SetWristLayer)
    float m_WristPose[2][7] = {}; bool m_WristValid[2] = {};   //!< [0] grip, [1] raw aim (left hand)
    unsigned long long m_WristLayers = 0;
    bool AppendWristLayer(XrCompositionLayerQuad& quad);
    XRPanelLayer m_Panel[2];                 //!< this frame's panels (SetPanelLayer)
    unsigned long long m_PanelFrames[2] = {}, m_ScreenFrames = 0, m_Snapshots = 0;
    bool AppendPanelLayer(int slot, XrCompositionLayerQuad& quad);
    float m_FrameFade = 1.f, m_FrameDim = 1.f;
    bool m_ResyncEyes = false;
    // the frozen-backdrop ring (FreezeBackdrop): half-size pictures with the pose and tan extents they were seen with
    struct Snap { float quat[4] = { 0, 0, 0, 1 }; float tan[4] = {}; long long qpc = 0; bool valid = false;
                  bool two = false; float quatR[4] = { 0, 0, 0, 1 }; float tanR[4] = {}; };   //!< two: a pair, eye 0 and eye 1 kept
    Snap m_Snap[3];
    int m_SnapNext = 0, m_Frozen = -1; long long m_FrozenAt = 0, m_LastSnapQpc = 0;
    //! `left` (+ its pose and tans): a synchronised pair's left picture, kept for the left eye (`source` = the right)
    void OfferSnapshot(ID3D11Texture2D* source, const float quat[4], ID3D11ShaderResourceView* left = nullptr, const float leftQuat[4] = nullptr, const float leftTan[4] = nullptr);
    bool m_BackdropEyes = true;
    // the staged left image of a synchronised-stereo pair (StagePairLeft)
    ID3D11Texture2D* m_PairTex = nullptr; ID3D11ShaderResourceView* m_PairSrv = nullptr;
    unsigned m_PairW = 0, m_PairH = 0; int m_PairFmt = 0;
    float m_PairPose[7] = {}; unsigned long long m_PairId = 0; bool m_PairStaged = false;
    unsigned long long m_PairsSubmitted = 0;
    bool DrawEye(int i, ID3D11Texture2D* source, ID3D11ShaderResourceView* srv, const float pose[7], const float tan[4], const float* warp, float fade,
                 XrCompositionLayerProjectionView& pv);
    //! One image into eye i's swapchain image `dst` (DrawEye, SubmitFrame): declared, or placed at its tans in the eye's
    //! native fov -- warped when its render frame is turned from the eye's (`warp`: v_eye = warp * v_render, null =
    //! none). Records the content rect and the pose the layer claims (`pose` = the render pose; a warped image is
    //! claimed at the eye's own orientation, pose * conj(warp)).
    bool DrawInto(int i, ID3D11Texture2D* source, ID3D11ShaderResourceView* srv, ID3D11Texture2D* dst, const float tan[4], const float* warp,
                  bool declare, float fade, const float pose[7]);
    float m_SrcWarp[kMaxEyes][4] = { { 0, 0, 0, 1 }, { 0, 0, 0, 1 } };   //!< SetSourceWarp (one submit)
    float m_PairWarp[4] = { 0, 0, 0, 1 };                                //!< the staged left image's
    float m_EyeImgQuat[kMaxEyes][4] = { { 0, 0, 0, 1 }, { 0, 0, 0, 1 } }; //!< the render frame of eye i's last image
public:
    //! The next SubmitFrame's image was rendered in a frame turned from eye i's (v_eye = q * v_render): a mono image on
    //! canted displays, a stereo eye's turned frame. Placed into that eye's native fov with a warp. One submit.
    void SetSourceWarp(int i, const float q[4]) { if ((unsigned)i < (unsigned)kMaxEyes) for (int k = 0; k < 4; k++) m_SrcWarp[i][k] = q[k]; }
private:
    int m_ContentX[kMaxEyes] = {}, m_ContentY[kMaxEyes] = {};   //!< content rect in the
    int m_ContentW[kMaxEyes] = {}, m_ContentH[kMaxEyes] = {};   //!< eye texture (per eye)
    float m_EyeRenderPose[kMaxEyes][7] = {}; //!< pose eye i's image was rendered at (quat xyzw, pos xyz)
    bool m_EyePoseValid[kMaxEyes] = {};      //!< m_EyeRenderPose[i] holds a real pose
    uint32_t m_SwapW[kMaxEyes] = {}, m_SwapH[kMaxEyes] = {};    //!< actual swapchain size
    int64_t m_SwapFormat = 0;                                   //!< chosen swapchain format

    bool InitInput();
    //! Grows eye swapchain i to at least needW x needH (recreates it; the
    //! game backbuffer can exceed the recommended eye size — Quest 3 is
    //! 1824x1968 vs a 1920x1080 backbuffer — and the mono placement needs
    //! extra margin to center the frame on the eye's optical axis).
    bool EnsureEyeSwapchain(int i, uint32_t needW, uint32_t needH);
    //! Matched-fov: source rect of eye i's native fov inside the symmetric
    //! render (both linear in tan space). False if rendered tans are unset.
    bool MatchedCropRect(int i, unsigned srcW, unsigned srcH,
                         unsigned& sx, unsigned& sy, unsigned& w, unsigned& h) const;
    //! Appends the HUD quad layer (if a source is set) and ends the XR frame.
    void EndFrameWithLayers(XrCompositionLayerProjection& layer);
};
