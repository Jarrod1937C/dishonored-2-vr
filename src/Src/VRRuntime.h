#pragma once
#include <dxgi.h>
#include <string>
#include "XRSession.h"

//! Drives the OpenXR session from the Present hook (render thread):
//! init once on the game's device, then per frame poll -> begin -> submit the
//! backbuffer as a flat frame (stage 1). Later stages feed real per-eye images.
namespace VRRuntime
{
    void OnPresent(IDXGISwapChain* sc);
    void Shutdown(bool processExit = false);
    //! The OpenXR frame thread (XRSession, the development notes): keep-alive frames while the game sends none, the render
    //! thread's bound on the wait for a frame, the new session for one the runtime stopped and did not offer back.
    void SetKeepAlive(int mode);
    void SetWaitBound(int ms);
    void SetKick(float seconds, bool anyRuntime = false);
    std::string PumpStatus();
    std::string SyncStatus();     //!< synchronised stereo (StereoSync): pairs staged / submitted / unpaired, the heartbeat line
    void SetD3DProtect(bool on);

    //! Game camera horizontal/vertical FOV in degrees used for the flat layer
    //! (until the camera hook supplies the real one). Any thread.
    void SetGameFov(float hDeg, float vDeg);

    //! Dumps the backbuffer and the last per-eye images to PNGs in the game
    //! dir on the next Present: DVR_<tag>_frame.png / DVR_<tag>_eyeN.png.
    void RequestCapture(const std::string& tag);
    //! `frames` consecutive frames (max 60) -> DVR_<tag>_bNN.png, copied on the GPU and written after the last one.
    void RequestBurst(int frames, const std::string& tag, int stride = 1);


    //! Latest located head pose (XR LOCAL space; midpoint of the eyes, eye-0 orientation).
    //! `gen` is the recentre generation the pose belongs to (XRSession::RecenterGeneration): when it changes, the pose
    //! jumped because the space moved, not because the head did.
    struct HeadPose { float pos[3] = {}; float quat[4] = { 0, 0, 0, 1 }; bool valid = false; unsigned gen = 0; };
    HeadPose GetHeadPose();

    //! Latest controller input snapshot (synced each frame). Returns false when
    //! the session/input isn't running (callers fall back to real devices).
    bool GetInput(XRInputState& out);
    //! A haptic pulse on a controller (0 left, 1 right). RENDER THREAD only (the Present path: KbmInject::Tick).
    bool Vibrate(int hand, float amplitude, float seconds, float hz = 0.f);
    //! ControllerPitchOffset / FingerTracking changed (a controller profile, the VR options tabs): to the session now
    void SetControllerOptions(float pitchOffsetDeg, bool fingerTracking);
    //! [Performance] FpsCounter / FpsCounterSize / FpsCounterAngle (XRSession::SetFpsCounter)
    void SetFpsCounter(int position, float size, float angleDeg);
    //! The frame a main view's picture (render serial) was drawn in, as the HUD's panel is placed for it: the head's
    //! orientation and position of that view and the picture's tangents. False when the serial is no longer known.
    bool HudFrameOf(unsigned long long serial, float q[4], float pos[3], float tan[4]);
    unsigned long long HapticPulses();

    //! Stage 5: alternating-eye stereo. Each game frame is rendered from ONE eye's
    //! pose (ViewMatrixHook uses GetRenderEyePose()) and submitted to that eye only;
    //! eyes alternate every frame (half temporal rate per eye, full-rate head tracking).
    void SetStereo(bool on);
    bool Stereo();
    //! Eye index of the frame being BUILT (synchronised stereo: 1 while the held frame's view is being built), or -1 for mono.
    int  CurrentEye();
    //! Eye index the image PRESENTED at the latest Present was rendered for (its view record): 0/1, -1 = mono / unknown.
    int  PresentedEye();

    //! The poses one VIEW is built from (pose prediction, `vr predict`): both eyes, the head (eye midpoint, eye-0
    //! orientation) and the controllers, located for the display time the image will actually be shown at -- the last
    //! frame's predicted display time plus the gap learned between a view's build and the frame that shows it (k display
    //! periods). Without a learned gap, or with prediction off, the latest located poses (as before). Synchronised
    //! stereo locates ONE set per pair, at its left view, and the right view, the layer and the hands all use it.
    struct PoseSet { bool valid = false; HeadPose eye[2]; HeadPose head; XRInputState input; bool inputValid = false;
                     long long time = 0, base = 0; int kind = -1; bool predicted = false; };
    //! kind: 0 mono, 1 a synchronised pair (its left view), 2 alternating stereo. Main view thread only.
    PoseSet LocateForView(int kind);
    //! The head pose the head-aim servo should drive the game to: the head at the display time of the NEXT view (the
    //! one the game will build from the mouse sent now). The latest located pose when prediction is off.
    HeadPose GetServoHeadPose();
    void SetPredict(int mode);           //!< vr predict 0 off | 1 k display periods (default) | 2 the learned gap as measured
    void SetPredictServo(bool on);       //!< vr predictservo 0|1
    void SetPredictMax(float ms);        //!< vr predictmax <ms>: the farthest ahead a pose is located
    void SetPhaseClosed(bool on);        //!< vr phaseloop 0|1: the presented image's view by measurement (TAA) over the frame-order rule
    void SetStereoHealth(bool on);       //!< vr stereohealth 0|1: fall back to mono when the stereo pairs stop
    void SetUnpairedOwnEye(bool on);     //!< vr unpairedeye 0|1: an unpaired stereo image to its own eye (1) or both as mono (0, before)
    void SetBackdropEyes(bool on);       //!< vr backdropeyes 0|1: the frozen pause backdrop keeps both eyes' pictures (1) or one (0, before)
    void SetMirrorEye(int mode);         //!< vr mirroreye -1 alternate (old) | 0 left | 1 right: the desktop window in synchronised stereo
    std::string PredictStatus();         //!< the heartbeat line `predict`
    void NoteStereoCommand();
    void SetPhaseOffset(int d);          //!< vr phaseoffset <n> (test): put the frame-order rule n views off; the loop must undo it            //!< vr stereo was given: a stereo fallback in force is forgotten

    //! Located pose of eye i (XR LOCAL space).
    HeadPose GetEyePose(int i);
    //! Both eyes and the head from ONE located frame (separate GetEyePose calls can straddle the next frame's update).
    void GetEyePoses(HeadPose eyes[2], HeadPose* head = nullptr);
    //! Eye i's orientation relative to the head (conj(head) * eye, XR): the identity on parallel displays, a ~10 deg yaw
    //! out on canted ones (Pimax). From the last located frame; false before one.
    bool EyeRelative(int i, float q[4]);
    //! The view serial of the image the NEXT Present will show (the frame-order rule VRRuntime applies at that Present):
    //! for what draws onto the backbuffer just before it (the reticle, Overlay::OnPresent). 0 = unknown.
    unsigned long long PresentingSerial();
    //! The projection layer's view for a stereo eye's image (`vr layerfov`): 0 (default) = the eye's native pose and
    //! fov, the image warped into it (XRSession's warp blit), as 1.0 placed an unturned image; 1 = the pose and fov each
    //! image was RENDERED with (its turned frame, its symmetric fov): spec-correct, but Meta's runtime (1.208, Quest 3
    //! over Link, 2026-10-24) does NOT honour it: every eye came out double, near and far.
    //! Mono images always go into the native fov (warped there on canted displays).
    void SetLayerFov(int mode);
    int LayerFov();

    //! How the last frame was laid out for eye i: the game image's size, the content rect it was copied to inside the
    //! eye swapchain, the swapchain size and the eye's fov (radians L R U D). For BodyView. Any thread.
    struct Placement { bool valid = false; unsigned srcW = 0, srcH = 0; int rect[4] = {}; unsigned swap[2] = {}; float fov[4] = {}; };
    Placement GetPlacement(int i);
    //! 1 (default) = the flat frame is drawn at its true angular size in each eye; 0 = the old 1:1 pixel copy.
    void SetOrtho(bool on);
    bool Ortho();
    unsigned long long OrthoDraws();
    //! 1 (default) = the mono layer claims the pose each image was actually rendered from (ViewMatrixHook::RenderPoseXr);
    //! 0 = the head pose located at submit, as before.
    void SetTruePose(bool on);
    bool TruePose();
    void SetPoseSource(int s);   //!< research: 2 executed command lists (default), 1 draws, 0 latest view, 3 the view before the latest
    struct RenderPoseStats { unsigned long long used = 0, fromDraws = 0, gameAxis = 0, behindLatest = 0, phaseFix = 0, fromExec = 0; float lastDiff = 0, maxDiff = 0; };
    RenderPoseStats GetRenderPoseStats();

    struct Status
    {
        bool initialized = false, running = false;
        int state = 0;
        unsigned long long framesSubmitted = 0;
        unsigned eyeW = 0, eyeH = 0;
        std::string runtime, system, error;
    };
    Status GetStatus();
    //! The headset's display period (ns) of the frame begun last (xrWaitFrame's predictedDisplayPeriod), 0 before any.
    long long DisplayPeriodNs();
    //! Count of runtime recenters (XrEventDataReferenceSpaceChangePending). Any thread.
    unsigned RecenterEvents();
    unsigned long long LostFrames(int dev);   //!< frames a lost device's last good pose was held: 0 left, 1 right, 2 head
}
