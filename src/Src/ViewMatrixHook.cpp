#include "pch.h"
#include "ViewMatrixHook.h"
#include "StereoSync.h"
#include "CameraHook.h"
#include "VrTweaks.h"
#include "HandAimHook.h"
#include "GameInfo.h"
#include "VRRuntime.h"
#include "VRMath.h"
#include "PlayerState.h"
#include "KbmInject.h"
#include "Game.h"
#include "PipeTrace.h"
#include "BodyHide.h"
#include "StereoTaa.h"
#include "ScreenMode.h"
#include <detours/detours.h>

namespace
{
    using Fn = void (*)(const float* org, const float* axis, float* out);
    Fn g_Real = nullptr;
    std::atomic<bool> g_Enabled{ true };
    std::atomic<bool> g_RecenterReq{ true };
    std::atomic<float> g_Scale{ 1.0f };
    std::atomic<bool> g_KeepGameAxis{ false };
    float g_RefPos[3] = {}; bool g_RefValid = false;
    std::mutex g_M; ViewMatrixHook::Stats g_Stats;

    // The main view as the renderer got it (see ViewMatrixHook::RenderView). Written on the render thread once per
    // frame, read from the D3D draw hooks on the recording threads, hence the mutex.
    std::mutex g_ViewM;
    float g_ViewMat[16] = {}, g_ViewOrg[3] = {}, g_ViewAxis[9] = {}, g_ViewHeadAxis[9] = {};
    float g_ViewGameOrg[3] = {}, g_ViewHmdPos[3] = {}; bool g_ViewHmdValid = false;   // the game eye a view was composed from, and the HMD pose used

    // ---- the takedown view (vr animcam) ----
    // A takedown animates the camera, and in head-aim mode the render takes the GAME's axis -- so for its duration
    // the world was glued to the face: turning the head did nothing, and every sway of the animation turned the
    // world. Measured on the prologue choke: yaw +-5 deg, pitch to -22, roll to -8, and the eye jumping 0.66 m in
    // ONE frame (crouched -> standing) at the grab and again at the release.
    //   2 (default) the ROTATION comes from the head alone, as if no animation were playing; only the animation's
    //     POSITION is kept, rate-limited so the 0.66 m pop becomes a quick rise. The arms, the victim and the
    //     weapon are placed in the world by the game from its animated camera, and a view is only where you look
    //     from -- so they stay on the victim's neck however the head turns.
    //   1 the animation's camera, with the head's rotation since the takedown began added on top.
    //   0 off: the game's camera, as before.
    // Both carry the render over continuously: the base yaw is (game yaw - head yaw) of the last normal frame, and
    // on the way out the view keeps following the head until the game's camera -- which the servo is bringing back
    // to the head -- is within a degree and a couple of centimetres, so the handover is invisible (measured
    // 0.00-0.23 s). When the servo cannot close the gap (it did not after an NPC's fatality left the game 45 deg
    // away) the view glides the rest of the way in 0.3 s rather than snapping.
    // The kills too: a Slash kill pitches the camera 40 deg in one frame, an Assassination rolls it +-15 deg and
    // lunges 1 m -- all of it measured, none of it reaches the view.
    std::atomic<int> g_AnimCam{ 0 };
    std::atomic<float> g_AnimCamSpeed{ 4.0f };               // m/s: how fast the eye may follow the animation's position (2.5 left the
                                                               // eye 0.7 m behind an assassination's lunge; 4 still turns the 0.66 m pop into 0.17 s)
    constexpr float kR2D = 57.29578f;
    constexpr float kPitchLimit = 80.f;                        // the servo's clamp; the view must use the same
    struct AnimView
    {
        bool on = false, holding = false, wheel = false;   // wheel: this one is the power wheel's view, not a takedown
        bool cine = false;                                       // a cutscene (PlayerState::Cinematic) has been seen in this one
        bool loco = false;                                       // the player moving (PlayerState::AnimLocoNow): rotation only
        float baseYaw = 0, pitchOff = 0;                        // deg: game - head, frozen at entry (pitch offset decays)
        float hmdYaw0 = 0, hmdPitch0 = 0;                       // mode 1: the head at entry
        float stick0 = 0;                                        // KbmInject's stick-turn total at entry
        float pos[3] = {};
        LARGE_INTEGER last{}, holdStart{};
        unsigned long long entries = 0, timeouts = 0, locoEntries = 0;
        float lastErrYaw = 0, lastErrPitch = 0, lastErrPos = 0, lastErrRoll = 0;
        // A cutscene's eye: the cutscene camera plus `popOff`, the part of a jump its motion did not
        // explain, closed at `animcamspeed` RELATIVE to the moving camera.
        bool cineTrack = false;
        float popOff[3] = {};
        // ... and the vehicle it may ride: the body's yaw last frame, the turn still to apply, how long the speeds
        // have been past the threshold
        bool vehicle = false, bodyPrevValid = false;
        float bodyYawPrev = 0, vehPending = 0, vehTurn = 0, vehOnFor = 0, vehOffFor = 0;
        float bodyPrev[3] = {}, bodyVel[2] = {};
    } g_AV;
    // The game camera's motion, followed on every main view: an exponential average of its velocity (and yaw rate), so
    // a frame's jump can be told from the motion it was already making. Measured on the train to
    // Addermire: the camera rides at ~14 m/s, 0.11-0.35 m per frame (the frames are uneven), and a loading hitch gave
    // 1.25-1.41 m in one frame -- which the plain "1.2 m in one frame" rule took for nine cuts in half a second.
    float g_CamVel[3] = {}, g_CamYawRate = 0.f, g_CamSpeed = 0.f; bool g_CamVelValid = false;
    LARGE_INTEGER g_PrevGameT{};
    constexpr float kVelTau = 0.12f;     // s: the velocity average (the frame noise is +-0.12 m at 14 m/s)
    constexpr float kPop = 0.25f;        // m: a jump beyond the motion's own, smaller than a cut, is a pop to smooth
    constexpr float kFarCut = 5.f;       // m: a cut this far is a new place (the shot is reframed whatever its direction)
    // A vehicle: the camera AND the player's body moving faster than a run for a moment (a cutscene camera dollying round
    // a player standing still is not one; the train is 14 m/s, a walk 1.5-2). It ends when either slows for a while.
    constexpr float kVehOn = 3.0f, kVehOff = 1.5f, kVehOnTime = 0.3f, kVehOffTime = 0.6f;
    constexpr float kVehMaxRate = 90.f;  // deg/s: the most the view turns with the vehicle (a single-tick jump is spread)
    std::atomic<bool> g_CineTrack{ true }, g_CineVehicle{ true };
    unsigned long long g_Pops = 0, g_VehicleRides = 0;
    // The eye pivot as play holds it (see Hook): the game eye's horizontal offset from the player's axis, in the game
    // camera's yaw frame, from the last frame play placed it. While the player is moving (a kLoco view) the pivot is
    // held with THIS offset and the rest of the eye's motion passes through unrotated: a mantle carries the eye 35 cm
    // off the axis (past the 30 cm the pivot rule accepts), and rotating an animation's lunge into the room put the eye
    // where the body is not going.
    float g_PivotBody[2] = {}; bool g_PivotBodyValid = false;
    struct AnimOut { bool loco = false; float blend = 0.f; };   // what the caller needs: rotation-only, and the hand-back glide
    float g_LastBaseYaw = 0, g_LastPitchOff = 0, g_LastOrg[3] = {}; bool g_LastValid = false;
    std::atomic<bool> g_AnimViewOn{ false }, g_TakedownView{ false };   // the view is the head's (a takedown or the wheel) / a takedown's
    // The room <-> game yaw the takedown / cutscene view renders with (game yaw = room yaw + this): the ONE mapping the
    // layer pose, the head's translation, the hands and the servo's target use while that view is on.
    // Before, the layer and the hands took the servo's anchor, which the servo re-seeds when the body jumps: a cutscene
    // turned the body 30 deg in one tick at its end, and the anchor and the view then disagreed by that much.
    std::atomic<bool> g_AnimOffValid{ false }; std::atomic<float> g_AnimOff{ 0.f };
    // Cutscene camera cuts (a new shot): the previous main view's game camera, and how many were taken
    float g_PrevGameO[3] = {}, g_PrevGameYaw = 0; bool g_PrevGameValid = false;
    unsigned long long g_Cuts = 0, g_CineEntries = 0;
    std::atomic<float> g_CutDist{ 1.2f }, g_CutYaw{ 60.f };   // vr cinecut <m> <deg>: a jump this large in one frame is a cut
    unsigned long long g_FlatViews = 0, g_WheelViews = 0;      // under g_M: main views rendered flat (ScreenMode) / for the wheel
    std::atomic<bool> g_PossessView{ true };                   // vr possessview: the level view inside a host
    std::atomic<bool> g_PivotFix{ true };                      // vr pivotfix: the game eye's offset from the body axis held in the room (see Hook)
    std::atomic<unsigned long long> g_PivotFixed{ 0 };
    std::atomic<float> g_HostScale{ 1.f };                     // head translation scale in a host (see Hook)

    // ---- The game eye's travel with its pitch ([Display] CameraPitchFix) ----
    // The game's eye is not on its pitch axis: the view pitch is spread over the first-person body's joints and the eye
    // sits ahead of the head joint (UpdateAim, exe+0x7F4B30: m_aimOrigin = the joint + |eye - joint| * the view's
    // forward; exe+0x7F4CD0 bends three joints by fractions of the pitch, m_armsPitch +0x25268..+0x25270). Looking down
    // carries the eye forward and down, looking up back and up: a neck for a flat screen. In head-aim mode the game's
    // pitch is the head's (the servo), and the headset already moves the eyes with the real neck -- so the world swayed
    // to and fro with every nod, the game's neck on top of the player's own.
    // How far the eye travels depends on the animation, so it is measured, not modelled: while the player stands still
    // (the player entity unmoved), each change of the game's pitch between two views is paired with the change of the
    // eye's offset from the player (forward in the body's yaw frame, and up), and the ratio (m per degree) is averaged
    // per pitch band. The curve is the integral of that from level, and play's eye is moved back by it: the eye stays
    // where it is with the view level, whatever the pitch. Unaffected by crouching or the eye height (a slope, not a
    // position); a sample whose offset changes faster than any neck could (a crouch, a mantle) is dropped.
    std::atomic<bool> g_PitchFix{ true };
    constexpr int kPfBins = 60;                                  // 3 deg each, -90..+90
    constexpr float kPfBin = 180.f / kPfBins;
    struct PitchCurve
    {
        float slopeF[kPfBins] = {}, slopeZ[kPfBins] = {}, w[kPfBins] = {};
        float lastP = 0, lastF = 0, lastZ = 0, lastPl[3] = {}; bool lastValid = false; int anchorAge = 0;
        unsigned long long samples = 0, rejected = 0, applied = 0;
        float lastCorrF = 0, lastCorrZ = 0;
    };
    PitchCurve g_Pf; std::mutex g_PfM;
    int PfBinOf(float p) { int b = (int)floorf((p + 90.f) / kPfBin); return b < 0 ? 0 : b >= kPfBins ? kPfBins - 1 : b; }
    //! the eye's travel (forward, up; m) from level to pitch p: the learned slopes integrated (g_PfM held)
    void PfCurve(float p, float& f, float& z)
    {
        f = 0; z = 0;
        if (p > 89.f) p = 89.f; if (p < -89.f) p = -89.f;
        const float dir = p >= 0 ? 1.f : -1.f;
        float at = 0.f;
        while (dir * (p - at) > 1e-4f)
        {
            const int b = PfBinOf(at + dir * 1e-3f);
            const float edge = -90.f + (b + (dir > 0 ? 1 : 0)) * kPfBin;      // the band's far edge in this direction
            const float to = dir > 0 ? std::min(edge, p) : std::max(edge, p);
            if (g_Pf.w[b] > 0.f) { f += g_Pf.slopeF[b] * (to - at); z += g_Pf.slopeZ[b] * (to - at); }
            at = to;
        }
    }
    //! one view of play: learn from it (standing still), and return the eye's travel at its pitch
    void PfStep(float pitch, float fwd, float up, const float pl[3], bool learn, float& corrF, float& corrZ)
    {
        std::lock_guard lk(g_PfM);
        PitchCurve& c = g_Pf;
        // measured against an anchor view, kept until the pitch has moved a degree (a slow nod moves it a fraction of a
        // degree a view) or the player moves; dropped after ~0.7 s (90 views), so idle sway cannot pile up in it
        bool reanchor = !learn || !c.lastValid || ++c.anchorAge > 90;
        if (!reanchor)
        {
            const float dp = pitch - c.lastP, df = fwd - c.lastF, dz = up - c.lastZ;
            const float mv = fabsf(pl[0] - c.lastPl[0]) + fabsf(pl[1] - c.lastPl[1]) + fabsf(pl[2] - c.lastPl[2]);
            if (mv >= 0.002f || fabsf(dp) > 12.f) reanchor = true;
            else if (fabsf(dp) >= 1.f)
            {
                reanchor = true;
                const float lim = 0.006f * fabsf(dp) + 0.002f;   // 0.34 m/rad: further than the eye is from any joint
                if (fabsf(df) > lim || fabsf(dz) > lim) c.rejected++;
                else
                {
                    const int b0 = PfBinOf(std::min(pitch, c.lastP)), b1 = PfBinOf(std::max(pitch, c.lastP) - 1e-3f);
                    const float k = std::min(0.3f, fabsf(dp) / 10.f);   // a bigger step is a better measurement
                    for (int b = b0; b <= b1; b++)                         // every band the step crossed
                    {
                        if (c.w[b] <= 0.f) { c.slopeF[b] = df / dp; c.slopeZ[b] = dz / dp; }
                        else { c.slopeF[b] += (df / dp - c.slopeF[b]) * k; c.slopeZ[b] += (dz / dp - c.slopeZ[b]) * k; }
                        c.w[b] += fabsf(dp);
                    }
                    c.samples++;
                }
            }
        }
        if (reanchor) { c.lastP = pitch; c.lastF = fwd; c.lastZ = up; memcpy(c.lastPl, pl, 12); c.lastValid = learn; c.anchorAge = 0; }
        PfCurve(pitch, corrF, corrZ);
        c.lastCorrF = corrF; c.lastCorrZ = corrZ; c.applied++;
    }
    constexpr float kStandingEye = 1.70f;                      // m, a standing Corvo's camera over his feet (measured)

    float Wrap180(float d) { while (d > 180.f) d -= 360.f; while (d < -180.f) d += 360.f; return d; }
    float Clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
    //! The HMD's yaw (about +Y, + = left, 0 = facing -Z) and pitch (+ = up), degrees -- exactly as KbmInject's servo
    //! reads them, so "the view follows the head" and "the servo brings the game to the head" agree.
    void HmdAngles(const float q[4], float& yaw, float& pitch)
    {
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        const float fx = -(2 * (x * z + y * w)), fy = -(2 * (y * z - x * w)), fz = -(1 - 2 * (x * x + y * y));
        yaw = atan2f(-fx, -fz) * kR2D;
        pitch = asinf(Clampf(fy, -1.f, 1.f)) * kR2D;
    }
    //! idMat3 (rows fwd/left/up) from yaw/pitch/roll in degrees, id conventions (yaw + = left, pitch + = up).
    void AxisFromAngles(float yawDeg, float pitchDeg, float rollDeg, float a[9])
    {
        const float y = yawDeg / kR2D, p = pitchDeg / kR2D, r = rollDeg / kR2D;
        const float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p), cr = cosf(r), sr = sinf(r);
        const float f[3] = { cp * cy, cp * sy, sp }, l[3] = { -sy, cy, 0.f }, u[3] = { -sp * cy, -sp * sy, cp };
        for (int i = 0; i < 3; i++) { a[i] = f[i]; a[3 + i] = cr * l[i] + sr * u[i]; a[6 + i] = -sr * l[i] + cr * u[i]; }
    }
    unsigned long long g_ViewSerial = 0;
    //! g_ViewSerial for readers on the hot path (every draw asks for the newest view): published after the rings hold
    //! the view, so a serial read here always has its records.
    std::atomic<unsigned long long> g_ViewSerialPub{ 0 };
    ViewMatrixHook::XrPose g_PoseRing[16];                     // under g_ViewM, indexed by serial
    struct ViewGeo { unsigned long long serial = 0; float mat[16] = {}, org[3] = {}, axis[9] = {}, headAxis[9] = {}; };
    ViewGeo g_GeoRing[16];                                     // under g_ViewM, indexed by serial: what each view WAS
    struct ViewSet { unsigned long long serial = 0; VRRuntime::PoseSet set; };
    ViewSet g_SetRing[16];                                     // under g_ViewM, indexed by serial: the poses each view was built from

    //! Quaternion (x,y,z,w) of the rotation whose COLUMNS are c0, c1, c2.
    void QuatFromCols(const float c0[3], const float c1[3], const float c2[3], float q[4])
    {
        const float m00 = c0[0], m10 = c0[1], m20 = c0[2], m01 = c1[0], m11 = c1[1], m21 = c1[2], m02 = c2[0], m12 = c2[1], m22 = c2[2];
        const float tr = m00 + m11 + m22;
        if (tr > 0) { const float s = sqrtf(tr + 1.f) * 2; q[3] = 0.25f * s; q[0] = (m21 - m12) / s; q[1] = (m02 - m20) / s; q[2] = (m10 - m01) / s; }
        else if (m00 > m11 && m00 > m22) { const float s = sqrtf(1.f + m00 - m11 - m22) * 2; q[3] = (m21 - m12) / s; q[0] = 0.25f * s; q[1] = (m01 + m10) / s; q[2] = (m02 + m20) / s; }
        else if (m11 > m22) { const float s = sqrtf(1.f + m11 - m00 - m22) * 2; q[3] = (m02 - m20) / s; q[0] = (m01 + m10) / s; q[1] = 0.25f * s; q[2] = (m12 + m21) / s; }
        else { const float s = sqrtf(1.f + m22 - m00 - m11) * 2; q[3] = (m10 - m01) / s; q[0] = (m02 + m20) / s; q[1] = (m12 + m21) / s; q[2] = 0.25f * s; }
        const float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        for (int i = 0; i < 4; i++) q[i] /= n;
    }

    //! Game yaw = room yaw + `deg`: the takedown / cutscene view's own mapping while it is on, the servo's anchor otherwise.
    bool RoomYawOffset(float& deg)
    {
        if (g_AnimOffValid.load()) { deg = g_AnimOff.load(); return true; }
        return KbmInject::WorldYawOffset(deg);
    }

    // ---- The first-person entities each view was built with (ViewMatrixHook::ViewEntities) ----
    // GpuTrace places a moved draw's arm or weapon with `target * entity^-1` applied to the draw's own model matrix, so
    // `entity` must be the transform of the same game tick. BodyHide samples the entities on the game thread once per
    // tick, and the draws are recorded on worker threads after the next view is built (in synchronised stereo the right
    // eye's a whole game frame after the left's): the latest sample could be a tick away from the draw, and the weapon
    // and the hand on it were drawn one tick of movement off (4 m/s at 90 ticks/s: 4.4 cm, sideways when strafing). Each
    // view records them when it is built; the right eye of a pair takes its left eye's (no tick lies between them).
    struct ViewSnap
    {
        unsigned long long serial = 0; bool valid = false;
        BodyHide::Anchor anch[BodyHide::aCount];
        float cam[3] = {}, camAxis[9] = {}; bool camValid = false;
    };
    ViewSnap g_SnapRing[16];                           // under g_ViewM, by serial % 16
    //! The record a view being built gets: the entities as they are now, and the game camera.
    void TakeSnap(ViewSnap& s)
    {
        s = ViewSnap();
        BodyHide::SampleNow(s.anch);
        s.camValid = HandAimHook::RealCamera(s.cam, s.camAxis);
        s.valid = true;
    }

    // [Controls] PhysicalCrouch: while the player's crouch is the one the real head made (KbmInject), the view keeps the
    // REAL head's height -- the game's crouch lowers its camera ~0.5 m, and the real head has already gone down: the two
    // together put the eye at the floor. So the game camera's drop below its standing height is added back: the eye is
    // then exactly where play puts a standing player's, less what the real head went down (as before the crouch). The
    // standing height is learned (the camera over the player's feet while not crouched); the lift stays until the
    // camera is back up after the player stands (or 2 s).
    std::atomic<float> g_StandCamH{ 1.70f }, g_CrouchLift{ 0.f }, g_CamH{ 0.f };
    float CrouchLift(const float camOrg[3], const ViewSnap& snap)
    {
        static double lastOwn = -1e9;
        LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
        const double now = (double)t.QuadPart / (double)f.QuadPart;
        BodyHide::Anchor live[BodyHide::aCount];
        const BodyHide::Anchor* an = snap.valid ? snap.anch : (BodyHide::GetAnchors(live), live);
        // (not in a takedown's view, whose eye is the animation's; a moving state -- a swing, a slide -- keeps play's eye, lift and all)
        if (!an[BodyHide::aPlayer].valid || KbmInject::Possessing() || (g_TakedownView.load() && !PlayerState::AnimLocoNow())) { g_CrouchLift = 0.f; return 0.f; }
        const float camH = camOrg[2] - an[BodyHide::aPlayer].org[2];
        g_CamH = camH;
        if (camH < 0.2f || camH > 2.6f) { g_CrouchLift = 0.f; return 0.f; }
        const bool low = PlayerState::Running("Sneaking") || PlayerState::Running("AutoCrouch") || PlayerState::Running("WallCrouch")
                      || PlayerState::Running("Sliding") || PlayerState::AnimActive();
        float stand = g_StandCamH.load();
        if (!low && fabsf(camH - stand) < 0.3f) { stand += (camH - stand) * 0.01f; g_StandCamH = stand; }
        const bool own = KbmInject::PhysicalCrouchOwns();
        // B back down after a kept stand-up (KbmInject::VirtualStand): the game never stood, so there is no camera
        // rise to wait for -- the lift fades out over 0.3 s instead of holding for 2
        static unsigned cancelsSeen = 0; static double fadeAt = -1e9; static float fadeFrom = 0.f;
        if (const unsigned c = KbmInject::CrouchLiftCancels(); c != cancelsSeen) { cancelsSeen = c; if (!own) { fadeAt = now; fadeFrom = g_CrouchLift.load(); lastOwn = -1e9; } }
        if (now - fadeAt < 0.3 && !own) { const float l = fadeFrom * (float)(1.0 - (now - fadeAt) / 0.3); g_CrouchLift = l; return l; }
        if (own) lastOwn = now;
        if (!own && (now - lastOwn > 2.0 || camH >= stand - 0.02f)) { lastOwn = -1e9; g_CrouchLift = 0.f; return 0.f; }
        const float lift = Clampf(stand - camH, 0.f, 1.0f);
        g_CrouchLift = lift;
        return lift;
    }

    //! `hp` is the HMD pose this view was composed with; `gameAxis` when the axis is the game's (head-aim mode).
    //! `eye` -2 = the alternating-stereo eye (VRRuntime::CurrentEye); `pair` see XrPose (~0 = this view starts one).
    //! Returns the view's serial.
    unsigned long long PublishView(const float* org, const float* axis, const float* out, const VRRuntime::HeadPose* hp = nullptr, bool gameAxis = false,
                                   const float* gameOrg = nullptr, bool flat = false, int eye = -2, unsigned long long pair = 0,
                                   const VRRuntime::PoseSet* ps = nullptr, const CameraHook::Tans* tans = nullptr, bool canted = false, const float* turn = nullptr,
                                   const float* headAxis = nullptr, const ViewSnap* snap = nullptr)
    {
        ViewMatrixHook::XrPose p;
        p.flat = flat; p.canted = canted;
        if (turn) memcpy(p.warp, turn, 16);
        {   // the frustum it is rendered with (see XrPose::tan)
            const CameraHook::Tans t = tans ? *tans : CameraHook::BuiltTans();
            if (t.valid) { p.tan[0] = t.l; p.tan[1] = t.r; p.tan[2] = t.u; p.tan[3] = t.d; p.tanValid = true; }
        }
        if (ps) { p.dispTime = ps->time; p.dispBase = ps->base; p.predKind = ps->kind; p.predicted = ps->predicted; }
        if (hp && hp->valid)
        {
            p.valid = true; memcpy(p.pos, hp->pos, 12); memcpy(p.quat, hp->quat, 16); p.eye = eye == -2 ? VRRuntime::CurrentEye() : eye;
            float off = 0;
            if (gameAxis && RoomYawOffset(off))
            {   // the game axis, turned into the room by the servo's world anchor (game yaw = xr yaw + off), then
                // id (x fwd, y left, z up) -> XR (x right, y up, -z fwd)
                const float c = cosf(-off / kR2D), sn = sinf(-off / kR2D);
                float xr[3][3];
                for (int k = 0; k < 3; k++)
                {
                    const float* v = axis + 3 * k; const float w[3] = { c * v[0] - sn * v[1], sn * v[0] + c * v[1], v[2] };
                    xr[k][0] = -w[1]; xr[k][1] = w[2]; xr[k][2] = -w[0];
                }
                const float right[3] = { -xr[1][0], -xr[1][1], -xr[1][2] }, back[3] = { -xr[0][0], -xr[0][1], -xr[0][2] };
                QuatFromCols(right, xr[2], back, p.quat);
                p.fromGameAxis = true;
            }
        }
        std::lock_guard lk(g_ViewM);
        memcpy(g_ViewMat, out, 64); memcpy(g_ViewOrg, org, 12); memcpy(g_ViewAxis, axis, 36); memcpy(g_ViewHeadAxis, headAxis ? headAxis : axis, 36);
        memcpy(g_ViewGameOrg, gameOrg ? gameOrg : org, 12);
        g_ViewHmdValid = hp && hp->valid; if (g_ViewHmdValid) memcpy(g_ViewHmdPos, hp->pos, 12);
        g_ViewSerial++;
        p.serial = g_ViewSerial;
        p.pair = pair == ~0ull ? g_ViewSerial : pair;
        g_PoseRing[g_ViewSerial % 16] = p;
        ViewGeo& geo = g_GeoRing[g_ViewSerial % 16];
        geo.serial = g_ViewSerial; memcpy(geo.mat, out, 64); memcpy(geo.org, org, 12); memcpy(geo.axis, axis, 36); memcpy(geo.headAxis, headAxis ? headAxis : axis, 36);
        ViewSet& vs = g_SetRing[g_ViewSerial % 16];
        vs.serial = ps ? g_ViewSerial : 0; if (ps) vs.set = *ps;
        ViewSnap& sn = g_SnapRing[g_ViewSerial % 16];
        if (snap && snap->valid) { sn = *snap; sn.serial = g_ViewSerial; } else { sn.serial = 0; sn.valid = false; }
        g_ViewSerialPub.store(g_ViewSerial, std::memory_order_release);
        PipeTrace::Event(PipeTrace::View, g_ViewSerial, (uint64_t)(int64_t)p.eye);
        return g_ViewSerial;
    }

    //! Synchronised stereo (StereoSync): what the LEFT view of a pair was composed from, so the right view -- built on
    //! the next, held frame, from the same instant of the world -- is the same camera with the other eye's offset, and
    //! the two eyes' poses come from ONE located frame. Main view thread only.
    struct PairBase
    {
        bool valid = false, keep = false;
        float o[3] = {}, a[9] = {}, tmpAxis[9] = {}, ref[3] = {}, gameOrg[3] = {};
        float scale = 1.f;
        VRRuntime::PoseSet set;             //!< both eyes, the head and the controllers, located once for the pair
        ViewSnap snap;                      //!< the entities the left view was built with: the right view's too
        unsigned long long serial = 0;      //!< the left view's
    } g_Pair;
    std::atomic<unsigned long long> g_PairsRight{ 0 }, g_PairsLeft{ 0 };

    //! The turn of stereo eye `eye`'s render frame from the eye (CameraHook::EyeTans, per-eye frustum mode 1: towards
    //! its frustum's centre), v_eye = turn * v_render; the identity when there is none. The projection builder's hook
    //! gave this view the matching symmetric fov a moment ago (from the same headset fov).
    void EyeTurn(int eye, float turn[4])
    {
        turn[0] = turn[1] = turn[2] = 0.f; turn[3] = 1.f;
        CameraHook::Tans t; float r[4];
        if (eye >= 0 && eye <= 1 && CameraHook::EyeProj() == 1 && VRRuntime::Stereo() && !ScreenMode::FlatRender() && CameraHook::EyeTans(eye, t, r)) memcpy(turn, r, 16);
    }
    //! A stereo eye's render axis: `axis` (the head's: the game's axis in head-aim mode) turned by the eye's rotation
    //! relative to the head in that view's pose set (canted displays) and then by its render frame's `turn`. False
    //! (nothing to do) on parallel displays without a turn: the axis is the head's, bit for bit, as before.
    std::atomic<unsigned long long> g_CantedViews{ 0 }, g_TurnedViews{ 0 };
    bool EyeAxis(const float axis[9], const VRRuntime::PoseSet& ps, int eye, const float turn[4], float out[9], bool* canted = nullptr)
    {
        if (eye < 0 || eye > 1 || !ps.head.valid || !ps.eye[eye].valid) return false;
        float rel[4]; VRMath::QConjMul(ps.head.quat, ps.eye[eye].quat, rel);
        const bool cant = fabsf(rel[3]) < 0.99999f;          // over 0.5 deg
        if (canted) *canted = cant;
        float total[4]; VRMath::QMul(rel, turn, total);
        if (fabsf(total[3]) > 0.9999999f) return false;
        VRMath::TurnAxisLocal(axis, total, out);
        if (cant) g_CantedViews++;
        if (fabsf(turn[3]) < 0.9999999f) g_TurnedViews++;
        return true;
    }

    //! A cutscene's vehicle (the development notes, see AnimViewStep): turns `v.baseYaw` as the player's body turns while the
    //! camera and the body both ride faster than a run. `dt` the frame's, `cut` a jump beyond the camera's motion this
    //! frame, `reframed` a cut that framed a new shot (its yaw replaces the vehicle's turn). Under g_M.
    void VehicleStep(AnimView& v, float dt, bool cut, bool reframed)
    {
        float po[3], pa[9];
        if (!Game::PlayerView(po, pa)) { v.bodyPrevValid = false; return; }
        const float by = atan2f(pa[1], pa[0]) * kR2D;
        // The body's horizontal speed, averaged: its position is caught mid-update on some frames (measured on the train,
        // +-0.1 m from one frame to the next while the camera moved smoothly), so a single frame says nothing.
        if (v.bodyPrevValid && dt > 0.f && !cut)
        {
            const float d[2] = { po[0] - v.bodyPrev[0], po[1] - v.bodyPrev[1] };
            if (d[0] * d[0] + d[1] * d[1] > 4.f) { v.bodyVel[0] = v.bodyVel[1] = 0.f; }   // a teleport, not a ride
            else
            {
                const float k = 1.f - expf(-dt / 0.25f);
                for (int i = 0; i < 2; i++) v.bodyVel[i] += (d[i] / dt - v.bodyVel[i]) * k;
            }
        }
        const float bodySpeed = sqrtf(v.bodyVel[0] * v.bodyVel[0] + v.bodyVel[1] * v.bodyVel[1]);
        if (!g_CamVelValid) {}     // the camera's speed is unknown for a frame after a cut: the timers wait
        else if (!v.vehicle)
        {
            v.vehOnFor = (g_CamSpeed > kVehOn && bodySpeed > kVehOn) ? v.vehOnFor + dt : 0.f;
            if (v.vehOnFor >= kVehOnTime)
            {
                v.vehicle = true; v.vehOffFor = 0.f; v.vehPending = 0.f; v.vehTurn = 0.f; g_VehicleRides++;
                LOGI("animcam: the cutscene carries the player (camera {:.1f} m/s, body {:.1f} m/s): the view turns with the body (yaw {:.1f})",
                     g_CamSpeed, bodySpeed, by);
            }
        }
        else
        {
            v.vehOffFor = (g_CamSpeed < kVehOff || bodySpeed < kVehOff) ? v.vehOffFor + dt : 0.f;
            if (v.vehOffFor >= kVehOffTime)
            {
                v.vehicle = false; v.vehOnFor = 0.f;
                LOGI("animcam: the ride is over (camera {:.1f} m/s, body {:.1f} m/s): turned {:.1f} deg with it{}", g_CamSpeed, bodySpeed, v.vehTurn,
                     fabsf(v.vehPending) > 0.5f ? fmt::format(", {:.1f} left unapplied", v.vehPending) : std::string());
                v.vehPending = 0.f;
            }
        }
        // (a cut along the way -- the camera making up a stall -- still carries the vehicle's turn: measured, the train lost
        // 11 deg of its curve when those frames were skipped)
        if (v.vehicle && v.bodyPrevValid && !reframed) v.vehPending += Wrap180(by - v.bodyYawPrev);
        if (v.vehicle)
        {   // at most kVehMaxRate: a body that jumps in one tick turns the view over a few frames, never in one
            const float maxStep = kVehMaxRate * dt, step = Clampf(v.vehPending, -maxStep, maxStep);
            v.baseYaw = Wrap180(v.baseYaw + step); v.vehPending -= step; v.vehTurn += step;
        }
        v.bodyYawPrev = by; v.bodyPrev[0] = po[0]; v.bodyPrev[1] = po[1]; v.bodyPrevValid = true;
    }

    //! The takedown view's state machine; runs on every main-view call in head-aim mode. `o`/`a` come in as the
    //! game's camera and leave as the one to render from (position before the HMD offset, which the caller adds).
    //! `pivotErr`: how far the room-held eye pivot of a moving state is from play's (m), for the hand-back.
    AnimOut AnimViewStep(float o[3], float a[9], const VRRuntime::HeadPose& eye, float pivotErr, const VRRuntime::HeadPose& head)
    {
        AnimOut out;
        // The power wheel takes the same view: while it is up the mouse picks the slot, so the servo cannot turn the
        // game with the head -- with the game's axis the world turned with the head (ScreenMode, the development notes).
        // Rotation from the head, position from the game; on the way out the view waits for the servo as after a
        // takedown (KbmInject keeps the servo's target through the wheel, so it turns the game to the head at once).
        const bool wheel = ScreenMode::WheelView();
        const int mode = wheel ? 2 : g_AnimCam.load();
        float hy = 0, hpRaw = 0; HmdAngles(head.valid ? head.quat : eye.quat, hy, hpRaw);
        const float hp = Clampf(hpRaw, -kPitchLimit, kPitchLimit);
        const float gy = atan2f(a[1], a[0]) * kR2D, gp = asinf(Clampf(a[2], -1.f, 1.f)) * kR2D;
        LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
        const bool want = wheel || (mode != 0 && PlayerState::AnimActiveNow());
        const bool locoNow = want && !wheel && PlayerState::AnimLocoNow();
        const bool cine = PlayerState::Cinematic();
        std::lock_guard lk(g_M);
        if (wheel) g_WheelViews++;
        AnimView& v = g_AV;
        // A cut in a cutscene: the camera is somewhere else, or facing elsewhere, from one frame to the next. That is a
        // new shot, not a movement: the eye goes there at once (a glide would fly through the set), and the shot is
        // framed in front of where the player is looking, as the director framed it -- turning the world at a cut is
        // invisible, it is a cut. (Measured on the shrine: its only jumps are the entry, 0.7 m and 3.6 deg, below both.)
        // A camera in MOTION (a train, a skiff, a travelling shot) moves every frame: what makes a cut is the part of the
        // jump the motion does not explain -- against the camera's averaged velocity and yaw rate.
        // A smaller unexplained jump is a pop (an animation's), smoothed by the eye below.
        bool cut = false, pop = false, reframed = false; float cutDist = 0.f, cutYaw = 0.f, jump[3] = {};
        if (g_PrevGameValid)
        {
            const float dtc = Clampf((float)(now.QuadPart - g_PrevGameT.QuadPart) / (float)fq.QuadPart, 1e-4f, 0.25f);
            const float d[3] = { o[0] - g_PrevGameO[0], o[1] - g_PrevGameO[1], o[2] - g_PrevGameO[2] };
            const float yawStep = Wrap180(gy - g_PrevGameYaw);
            cutDist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            const bool predict = g_CineTrack.load() && g_CamVelValid;
            for (int i = 0; i < 3; i++) jump[i] = d[i] - (predict ? g_CamVel[i] * dtc : 0.f);
            const float excess = sqrtf(jump[0] * jump[0] + jump[1] * jump[1] + jump[2] * jump[2]);
            cutYaw = fabsf(yawStep - (predict ? g_CamYawRate * dtc : 0.f));
            cut = cine && v.on && !v.holding && !v.wheel && (excess > g_CutDist.load() || cutYaw > g_CutYaw.load()) && cutDist < 500.f;
            pop = !cut && predict && excess > kPop;
            // A pop is ONE frame's jump; a second in a row is the camera moving at a new speed (a dolly starting at
            // once): the velocity is re-seeded from this frame, and only the first jump is smoothed.
            static bool popPrev = false;
            const bool newSpeed = pop && popPrev;
            popPrev = pop && !newSpeed;
            if (newSpeed) pop = false;
            if (cut || cutDist >= 500.f) { g_CamVelValid = false; memset(g_CamVel, 0, 12); g_CamYawRate = 0.f; }
            else if (!g_CamVelValid || newSpeed) { for (int i = 0; i < 3; i++) g_CamVel[i] = d[i] / dtc; g_CamYawRate = yawStep / dtc; g_CamVelValid = true; }
            else if (!pop)
            {
                const float k = 1.f - expf(-dtc / kVelTau);
                for (int i = 0; i < 3; i++) g_CamVel[i] += (d[i] / dtc - g_CamVel[i]) * k;
                g_CamYawRate += (yawStep / dtc - g_CamYawRate) * k;
            }
            g_CamSpeed = g_CamVelValid ? sqrtf(g_CamVel[0] * g_CamVel[0] + g_CamVel[1] * g_CamVel[1]) : 0.f;
        }
        memcpy(g_PrevGameO, o, 12); g_PrevGameYaw = gy; g_PrevGameValid = true; g_PrevGameT = now;
        if (want && !v.on)
        {
            v.on = true; v.holding = false; v.entries++; v.wheel = wheel;
            v.baseYaw = g_LastValid ? g_LastBaseYaw : Wrap180(gy - hy);
            v.pitchOff = g_LastValid ? g_LastPitchOff : gp - hp;
            v.hmdYaw0 = hy; v.hmdPitch0 = hpRaw; v.stick0 = KbmInject::StickTurnTotal();
            memcpy(v.pos, g_LastValid ? g_LastOrg : o, 12);   // the eye BEFORE the animation's first pop
            v.last = now;
            v.loco = locoNow; if (locoNow) v.locoEntries++;
            const char* name = wheel ? "the power wheel" : PlayerState::AnimName();   // empty when caught this frame, before Present names it
            LOGI("animcam: {} begins, mode {}: base yaw {:.1f} (game {:.1f} head {:.1f}), pitch offset {:.1f}{}", *name ? name : "scripted state", mode, v.baseYaw, gy, hy, v.pitchOff,
                 locoNow ? " -- the player moving: the head's rotation, play's eye" : "");
        }
        else if (!want && v.on && !v.holding) { v.holding = true; v.holdStart = now; LOGI("animcam: animation over, holding the head view until the game's camera catches up"); }
        else if (want && v.holding) v.holding = false;      // chained into another scripted state
        // (kept through the hold: the hand-back of a moving state waits for the roll and keeps play's eye)
        if (v.on && want && !v.wheel) v.loco = locoNow;
        if (v.on && !wheel && want) v.wheel = false;        // a takedown straight out of the wheel is a takedown
        if (v.on && cine && !v.wheel && !v.cine) { v.cine = true; g_CineEntries++; LOGI("animcam: a cutscene: the head turns the view, the cutscene moves the eye"); }
        if (!v.on) { v.cine = false; v.cineTrack = false; v.vehicle = false; v.bodyPrevValid = false; v.vehPending = 0.f; v.vehOnFor = v.vehOffFor = 0.f; }
        if (v.on && cut)
        {
            g_Cuts++;
            const float was = v.baseYaw;
            // The shot is reframed where the head looks only when the camera's DIRECTION jumped, or it is somewhere else
            // altogether. A jump along the way it was facing (the train after a 12 s loading stall made up the lost
            // ground in 1.25-1.41 m steps for half a second: nine "cuts") keeps the world's orientation: the eye goes
            // there and nothing turns.
            const bool reframe = !g_CineTrack.load() || cutYaw > g_CutYaw.load() || cutDist > kFarCut;
            if (reframe) v.baseYaw = Wrap180(gy - hy - (KbmInject::StickTurnTotal() - v.stick0));
            reframed = reframe;
            memcpy(v.pos, o, 12);
            LOGI("animcam: cutscene cut ({:.2f} m, {:.1f} deg in one frame, beyond the motion): the eye goes there{} (base yaw {:.1f} -> {:.1f})",
                 cutDist, cutYaw, reframe ? ", the shot framed where the head looks" : ", the same direction", was, v.baseYaw);
        }
        if (v.on)
        {
            float dt = (float)(now.QuadPart - v.last.QuadPart) / (float)fq.QuadPart; v.last = now;
            dt = Clampf(dt, 0.f, 0.1f);
            // Position: follow the animation, but never faster than the speed limit. A jump of several metres is a
            // teleport (a load, a blink), not an animation pop: take it at once.
            // The player moving (a slide, a mantle) is not a pop: the eye is play's, exactly.
            const float speed = g_AnimCamSpeed.load();
            if (v.cine && !v.wheel && g_CineTrack.load())
            {   // A cutscene: the eye is ON the cutscene camera, however fast it goes -- the takedown
                // rule below (follow the position at most 4 m/s) left it behind a train at 14 m/s, up to 5 m, and then
                // snapped it forward (its "a jump > 5 m is a teleport"): the player "staying behind" all the ride. Only
                // a jump the camera's motion does not explain (a pop, see above) is smoothed, closing at the speed limit
                // relative to the moving camera; a cut takes the eye there at once.
                if (!v.cineTrack)
                {   // from wherever the eye was (an entry's pop, or a takedown chaining into a cutscene)
                    v.cineTrack = true;
                    for (int i = 0; i < 3; i++) v.popOff[i] = v.pos[i] - o[i];
                }
                else if (cut) memset(v.popOff, 0, 12);
                else if (pop) { for (int i = 0; i < 3; i++) v.popOff[i] -= jump[i]; g_Pops++; }
                const float m = sqrtf(v.popOff[0] * v.popOff[0] + v.popOff[1] * v.popOff[1] + v.popOff[2] * v.popOff[2]);
                const float step = speed > 0.f ? speed * dt : m;
                const float keep = m > 5.f || m <= step ? 0.f : (m - step) / m;
                for (int i = 0; i < 3; i++) { v.popOff[i] *= keep; v.pos[i] = o[i] + v.popOff[i]; }
            }
            else
            {
                v.cineTrack = false;
                const float d[3] = { o[0] - v.pos[0], o[1] - v.pos[1], o[2] - v.pos[2] };
                const float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), step = speed * dt;
                if (!v.loco && speed > 0.f && n > step && n < 5.f) for (int i = 0; i < 3; i++) v.pos[i] += d[i] * step / n;
                else memcpy(v.pos, o, 12);
            }
            // A cutscene that CARRIES the player (the train to Addermire, the skiff to Karnaca): the view's base yaw turns
            // as the player's body turns. Measured on the train: the body rides at the camera (7 cm off it) and turns
            // with the train, 22.5 -> 89.8 deg over 7 s; with the base held, the carriage swung 67 deg round the player.
            // Only while both the camera and the body move faster than a run: a cutscene that turns the body of a
            // player standing still (the shrine, the mark, getting up at the end of the ride) leaves the world still.
            if (v.cine && !v.wheel && !v.holding && mode == 2 && g_CineVehicle.load()) VehicleStep(v, dt, cut, reframed);
            v.pitchOff *= expf(-dt / 0.3f);     // any pitch lag the servo had at entry fades out instead of snapping
            float vy, vp, vr = 0.f;
            if (mode == 1 && !v.holding)
            {
                vy = gy + Wrap180(hy - v.hmdYaw0); vp = gp + (hpRaw - v.hmdPitch0);
                vr = atan2f(a[5], a[8]) * kR2D;
            }
            else { vy = v.baseYaw + (KbmInject::StickTurnTotal() - v.stick0) + hy; vp = hp + v.pitchOff; }
            vp = Clampf(vp, -89.f, 89.f);
            v.lastErrYaw = Wrap180(gy - vy); v.lastErrPitch = gp - vp;
            const float gr = atan2f(a[5], a[8]) * kR2D;
            v.lastErrRoll = gr - vr;
            const float e[3] = { o[0] - v.pos[0], o[1] - v.pos[1], o[2] - v.pos[2] };
            v.lastErrPos = v.loco ? pivotErr : sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
            float pos[3]; memcpy(pos, v.pos, 12);
            if (v.holding)
            {
                // Normally the servo brings the game's camera to the head within a few frames (measured 0.00-0.23 s)
                // and the handover is invisible. It cannot always: measured, a stick snap during a swing made the
                // game over-turn by 10 deg and the servo's own jump guard then re-seeded its target on the body, so
                // the two never met and the old 1.5 s timeout ended in a 10 deg snap. After kHoldMax the view GLIDES
                // to the game's camera over kBlend instead: a third of a second, never one frame.
                constexpr float kHoldMax = 0.6f, kBlend = 0.3f;
                const float held = (float)(now.QuadPart - v.holdStart.QuadPart) / (float)fq.QuadPart;
                // A moving state also waits for the ROLL: a slide's -21 deg is blended out after Sliding ends (measured
                // 0.55 s, in Running) and yaw/pitch alone handed the view back onto it at once.
                const bool caught = fabsf(v.lastErrYaw) < 1.5f && fabsf(v.lastErrPitch) < 1.5f && v.lastErrPos < 0.02f &&
                                    (!v.loco || fabsf(v.lastErrRoll) < 1.5f);
                const float b = Clampf((held - kHoldMax) / kBlend, 0.f, 1.f);
                if (b > 0.f && !caught)
                {
                    vy += Wrap180(gy - vy) * b; vp += (gp - vp) * b; vr += (gr - vr) * b;
                    for (int i = 0; i < 3; i++) pos[i] += (o[i] - pos[i]) * b;
                    out.blend = b;
                }
                if (caught || b >= 1.f)
                {
                    if (!caught) v.timeouts++;
                    LOGI("animcam: handed back after {:.2f} s{} (yaw err {:.1f}, pitch err {:.1f}, roll err {:.1f}, pos err {:.3f} m){}",
                         held, caught ? "" : " (glided over the remainder)", v.lastErrYaw, v.lastErrPitch, v.lastErrRoll, v.lastErrPos,
                         v.loco ? " -- the player moving" : "");
                    v.on = v.holding = false;
                }
            }
            AxisFromAngles(vy, vp, vr, a);
            memcpy(o, pos, 12);
            // the room <-> game mapping this view renders with (see g_AnimOff): exact whatever the glide is doing
            if (v.on && !v.wheel && mode == 2) { g_AnimOff = Wrap180(vy - hy); g_AnimOffValid = true; }
            else g_AnimOffValid = false;
            out.loco = v.on && v.loco;
        }
        else
        {   // a normal frame: remember how game and head relate, so an animation starting next frame starts from here
            g_LastBaseYaw = Wrap180(gy - hy); g_LastPitchOff = gp - hp; memcpy(g_LastOrg, o, 12); g_LastValid = true;
            g_AnimOffValid = false;
        }
        g_AnimViewOn = v.on; g_TakedownView = v.on && !v.wheel;
        return out;
    }

    void Hook(const float* org, const float* axis, float* out)
    {
        { std::lock_guard lk(g_M); g_Stats.calls++; }
        // Main view = the object CameraHook identified (largest render size); its renderRV.vieworg
        // lives at view + 0x39D0 + 0xA8.
        uint8_t* mainView = (uint8_t*)CameraHook::MainView();
        bool isMain = mainView && org == (const float*)(mainView + CameraHook::kRenderRV + CameraHook::kRV_ViewOrg);
        if (isMain) VrTweaks::OnMainView(mainView);   // its render flags, set by now for this frame (VrTweaks)
        if (isMain && g_Enabled.load() && ScreenMode::FlatRender())
        {   // a menu, a loading screen or a cutscene on the screen in the room: the flat game's own view, no head in it
            {   // and whatever the takedown view was doing is over (a menu is a cut)
                std::lock_guard lk(g_M); g_Stats.mainCalls++; g_FlatViews++;
                g_AV.on = false; g_AV.holding = false; g_AV.cine = false; g_AnimViewOn = false; g_TakedownView = false; g_LastValid = false;
                g_AnimOffValid = false; g_PrevGameValid = false; g_CamVelValid = false;
            }
            g_Real(org, axis, out);
            StereoTaa::OnMainView(mainView, -1, out);
            PublishView(org, axis, out, nullptr, false, nullptr, true);
            g_Pair.valid = false;
            StereoSync::CancelPending();
            return;
        }
        if (isMain && g_Enabled.load())
        {
            const bool syncOn = StereoSync::Active();
            if (syncOn && StereoSync::LastFrameHeld() && g_Pair.valid)
            {   // Synchronised stereo, the RIGHT eye: the held frame drew the same world as the left view's frame. Same
                // camera (every state machine above already stepped once for the left view), the other eye's offset,
                // both eyes from one located frame.
                VRRuntime::HeadPose hp = g_Pair.set.eye[1];
                float o[3], a[9], ta[9]; memcpy(o, g_Pair.o, 12); memcpy(a, g_Pair.a, 36); memcpy(ta, g_Pair.tmpAxis, 36);
                float q0[4] = { 0, 0, 0, 1 };
                // the eye's render frame: turned to its frustum's centre (per-eye frustum), and on canted displays the eye
                // looks ~10 deg out of the head's axis (the game's)
                float turn[4]; EyeTurn(1, turn);
                bool canted = false;
                // the HEAD's axis (no eye turn, no cant), for whatever takes only the view's yaw: the hands' room -> world
                float headAxis[9];
                if (!g_Pair.keep)
                {   // (the eye's own pose: cant included)
                    float ho[3]; memcpy(ho, o, 12); memcpy(headAxis, a, 36); VRMath::ComposeHead(hp.pos, g_Pair.set.head.quat, g_Pair.ref, g_Pair.scale, ho, headAxis);
                    float q[4]; VRMath::QMul(hp.quat, turn, q); memcpy(hp.quat, q, 16);
                }
                if (g_Pair.keep) VRMath::ComposeHead(hp.pos, q0, g_Pair.ref, g_Pair.scale, o, ta);
                else VRMath::ComposeHead(hp.pos, hp.quat, g_Pair.ref, g_Pair.scale, o, a);
                if (g_Pair.keep) { memcpy(headAxis, a, 36); float ea[9]; if (EyeAxis(a, g_Pair.set, 1, turn, ea, &canted)) memcpy(a, ea, 36); }
                { std::lock_guard lk(g_M); g_Stats.mainCalls++; g_Stats.applied++; memcpy(g_Stats.lastOrg, o, 12); memcpy(g_Stats.lastFwd, a, 12); }
                g_Real(o, a, out);
                CameraHook::Tans tans; CameraHook::ApplyEyeProjection(mainView, 1, tans);   // mode 2 (research): its own matrix, before the engine's VP
                StereoTaa::OnMainView(mainView, 1, out);
                PublishView(o, a, out, &hp, g_Pair.keep, g_Pair.gameOrg, false, 1, g_Pair.serial, &g_Pair.set, &tans, canted, turn, headAxis, &g_Pair.snap);
                g_Pair.valid = false; g_PairsRight++;
                StereoSync::RightBuilt();
                return;
            }
            // Stereo: render this frame from the current eye's pose (position differs by ±IPD/2). Synchronised stereo:
            // the left eye, with both eyes' poses kept for the right view -- only in play (StereoSync::PairWanted):
            // otherwise a mono view, one picture submitted to both eyes.
            const bool sync = syncOn && StereoSync::PairWanted();
            int eye = sync ? 0 : syncOn ? -1 : VRRuntime::CurrentEye();
            if (syncOn) g_Pair.valid = false;
            // the poses this view is built from: located for when its image will be shown (VRRuntime::LocateForView)
            const VRRuntime::PoseSet ps = VRRuntime::LocateForView(sync ? 1 : eye >= 0 ? 2 : 0);
            VRRuntime::HeadPose hp = sync ? ps.eye[0] : (eye >= 0 ? ps.eye[eye] : ps.head);
            if (hp.valid)
            {
                ViewSnap snap; TakeSnap(snap);           // the entities this view is built with (ViewSnap)
                static unsigned refGen = 0;
                const bool recentred = hp.gen != refGen; refGen = hp.gen;     // the space moved THIS frame: see HeadPose::gen
                if (g_RecenterReq.exchange(false) || !g_RefValid || recentred)
                {   // reference = eye midpoint, so per-eye offsets stay symmetric around the game camera
                    const VRRuntime::HeadPose& mid = ps.head;
                    memcpy(g_RefPos, mid.valid ? mid.pos : hp.pos, 12); g_RefValid = true;
                }
                float o[3], a[9]; memcpy(o, org, 12); memcpy(a, axis, 36);
                if (HandAimHook::FpAnchor() == 2) HandAimHook::RealCamera(o, a);   // scene view carries the controller pose; render from the real camera
                o[2] -= HandAimHook::CamTest();          // research: undo the camera lift so only camera-anchored things move
                float turn[4]; EyeTurn(eye, turn);       // a stereo eye's render frame (per-eye frustum), see EyeTurn
                float headAxis[9];                       // the head's axis (no eye turn, no cant): see the right eye
                if (g_KeepGameAxis.load())
                {   // head-aim mode: the game view already tracks the HMD; only add the positional offset
                    // The game's eye is ~5 cm in front of the body's vertical axis, so when the servo turns the body
                    // with the head the eye swings round a 5 cm circle -- ON TOP of the real eyes' own swing round the
                    // neck, which the headset already tracks. Measured with the head still at -30 / 0 / +30 deg: the
                    // world shifted 1.6 deg. Hold that offset in the room's frame instead (rotated by the servo's
                    // anchor, not by the body's yaw): turning the head is then a pure rotation plus the real neck.
                    // Worked out on the game's own camera, before the view below replaces its axis: `pivFull` is play's
                    // correction, `pivLoco` the same with the offset play last used (g_PivotBody) -- what a moving
                    // state's view holds (see g_PivotBody).
                    float pivFull[2] = {}, pivLoco[2] = {}, pivL[2] = {}, pivErr = 0.f; bool pivAnchor = false, pivOk = false;
                    float pitchCorrF = 0.f, pitchCorrZ = 0.f;    // the game eye's travel with its pitch (g_PitchFix)
                    if (g_PivotFix.load())
                    {
                        float off = 0;
                        // the player as this view records it (ViewSnap), the same tick as `o`: the game thread's
                        // last sample can be a tick away from it
                        BodyHide::Anchor anLive[BodyHide::aCount]; const BodyHide::Anchor* an = snap.anch;
                        if (!snap.valid) { BodyHide::GetAnchors(anLive); an = anLive; }
                        const BodyHide::Anchor& pl = an[BodyHide::aPlayer];
                        if (pl.valid && KbmInject::WorldYawOffset(off))
                        {
                            pivAnchor = true;
                            const float gy = atan2f(a[1], a[0]), ay = off / kR2D;
                            const float cg = cosf(gy), sg = sinf(gy), ca = cosf(ay), sa = sinf(ay);
                            const float dx = o[0] - pl.org[0], dy = o[1] - pl.org[1];
                            if (dx * dx + dy * dy < 0.3f * 0.3f)
                            {
                                pivL[0] = cg * dx + sg * dy; pivL[1] = -sg * dx + cg * dy;       // in the camera's yaw frame
                                if (g_PitchFix.load())
                                {   // the game's neck out of play's eye (g_PitchFix): forward here, the height below
                                    // learned in play only; applied in play and to a moving state's eye (kLoco: a sword swing,
                                    // a slide), which is play's -- else the eye jumped by it as such a state began and ended
                                    const bool play = !g_AnimViewOn.load() && !KbmInject::Possessing();
                                    const float pitch = asinf(Clampf(a[2], -1.f, 1.f)) * kR2D;
                                    PfStep(pitch, pivL[0], o[2] - pl.org[2], pl.org, play, pitchCorrF, pitchCorrZ);
                                    if (KbmInject::Possessing()) pitchCorrF = pitchCorrZ = 0.f;
                                    pivL[0] -= pitchCorrF;
                                }
                                pivFull[0] = ca * pivL[0] - sa * pivL[1] - dx; pivFull[1] = sa * pivL[0] + ca * pivL[1] - dy;
                                pivOk = true;
                            }
                            if (g_PivotBodyValid)
                            {
                                const float bx = g_PivotBody[0], by = g_PivotBody[1];
                                pivLoco[0] = (ca * bx - sa * by) - (cg * bx - sg * by); pivLoco[1] = (sa * bx + ca * by) - (sg * bx + cg * by);
                            }
                            pivErr = sqrtf((pivFull[0] - pivLoco[0]) * (pivFull[0] - pivLoco[0]) + (pivFull[1] - pivLoco[1]) * (pivFull[1] - pivLoco[1]));
                        }
                    }
                    const AnimOut ao = AnimViewStep(o, a, hp, pivErr, ps.head);
                    if (pivAnchor && !g_AnimViewOn.load())
                    {   // play
                        o[0] += pivFull[0]; o[1] += pivFull[1]; o[2] -= pitchCorrZ;
                        if (pivOk) { g_PivotBody[0] = pivL[0]; g_PivotBody[1] = pivL[1]; g_PivotBodyValid = true; g_PivotFixed++; }
                    }
                    else if (pivAnchor && ao.loco)
                    {   // the player moving: the pivot play last held, the animation's own motion unrotated; the
                        // hand-back's glide carries it over to play's
                        o[0] += pivLoco[0] + (pivFull[0] - pivLoco[0]) * ao.blend;
                        o[1] += pivLoco[1] + (pivFull[1] - pivLoco[1]) * ao.blend;
                        o[2] -= pitchCorrZ;
                    }
                    if (g_PossessView.load() && KbmInject::Possessing() && !g_AnimViewOn.load())
                    {   // Inside a host the camera sways with the host's walk and idle (measured pitch +-1.5, roll
                        // +-2.5 deg while standing still), and that is the horizon rocking in a headset. Keep the
                        // game's yaw -- the servo drives it to the head, and it is where the host walks -- and take
                        // pitch from the head, roll level.
                        const VRRuntime::HeadPose& head = ps.head;
                        float hy = 0, hpt = 0; HmdAngles(head.valid ? head.quat : hp.quat, hy, hpt);
                        AxisFromAngles(atan2f(a[1], a[0]) * kR2D, Clampf(hpt, -kPitchLimit, kPitchLimit), 0.f, a);
                    }
                    float tmpAxis[9]; memcpy(tmpAxis, a, 36);
                    float q0[4] = { 0, 0, 0, 1 };
                    // In a small host the head's movement is scaled to the host: a rat's eye is 0.18 m off the floor
                    // (a standing Corvo's 1.70), and at human scale leaning down 0.3 m put the view under the floor
                    // (measured). The eye height is the camera over the player entity, which rides in the host at its
                    // feet. Eased over ~0.3 s so the change is hidden in the leap in and out.
                    static float hostScale = 1.f; static LARGE_INTEGER lastT{};
                    {
                        float target = 1.f, po[3], pa[9];
                        if (g_PossessView.load() && KbmInject::Possessing() && Game::PlayerView(po, pa))
                            target = Clampf((o[2] - po[2]) / kStandingEye, 0.08f, 1.f);
                        LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
                        const float dt = lastT.QuadPart ? Clampf((float)(now.QuadPart - lastT.QuadPart) / (float)fq.QuadPart, 0.f, 0.1f) : 0.f;
                        lastT = now;
                        hostScale += (target - hostScale) * (1.f - expf(-dt / 0.3f));
                        g_HostScale = hostScale;
                    }
                    // The head's offset is in the ROOM's frame (XR LOCAL), and the room meets the game world at the
                    // servo's anchor (game yaw = room yaw + off), not at the game's current yaw: that one includes the
                    // head's own turn, so a lean -- and alternating stereo's +-IPD/2 -- came out rotated by the head's
                    // yaw (with the head turned 90 deg the eyes were separated along the view). Same mapping as the hands
                    // (HandAimHook::XrOffsetToWorld) and as the layer pose (PublishView).
                    {
                        float off = 0;
                        if (RoomYawOffset(off)) AxisFromAngles(off, 0.f, 0.f, tmpAxis);
                        else
                        {   // no anchor yet: game yaw minus head yaw is the same thing, less the servo's error
                            float hy = 0, hpt = 0; HmdAngles(hp.quat, hy, hpt);
                            AxisFromAngles(atan2f(a[1], a[0]) * kR2D - hy, 0.f, 0.f, tmpAxis);
                        }
                    }
                    o[2] += CrouchLift(org, snap) * hostScale;     // PhysicalCrouch: the real head's height (see CrouchLift)
                    if (sync)
                    {
                        g_Pair.keep = true; memcpy(g_Pair.o, o, 12); memcpy(g_Pair.a, a, 36); memcpy(g_Pair.tmpAxis, tmpAxis, 36);
                        memcpy(g_Pair.ref, g_RefPos, 12); g_Pair.scale = g_Scale.load() * hostScale;
                    }
                    VRMath::ComposeHead(hp.pos, q0, g_RefPos, g_Scale.load() * hostScale, o, tmpAxis);   // rotation identity -> yaw-only body frame
                }
                else
                {
                    o[2] += CrouchLift(org, snap);
                    if (sync) { g_Pair.keep = false; memcpy(g_Pair.o, o, 12); memcpy(g_Pair.a, a, 36); memcpy(g_Pair.ref, g_RefPos, 12); g_Pair.scale = g_Scale.load(); }
                    {   // the head's axis for the yaw-only readers (see the right eye), then the eye's own pose (cant included)
                        float ho[3]; memcpy(ho, o, 12); memcpy(headAxis, a, 36);
                        VRMath::ComposeHead(hp.pos, ps.head.valid ? ps.head.quat : hp.quat, g_RefPos, g_Scale.load(), ho, headAxis);
                        float q[4]; VRMath::QMul(hp.quat, turn, q); memcpy(hp.quat, q, 16);
                    }
                    VRMath::ComposeHead(hp.pos, hp.quat, g_RefPos, g_Scale.load(), o, a);
                }
                // A stereo eye on canted displays looks ~10 deg out of the head's axis (with the head's own pose, the eye's
                // quaternion above already carries it), and its render frame is turned to its frustum's centre. The pair
                // keeps the head's axis: the right eye turns it its own way.
                const int viewEye = sync ? 0 : eye;
                bool canted = false;
                if (g_KeepGameAxis.load()) { memcpy(headAxis, a, 36); float ea[9]; if (EyeAxis(a, ps, viewEye, turn, ea, &canted)) memcpy(a, ea, 36); }
                { std::lock_guard lk(g_M); g_Stats.mainCalls++; g_Stats.applied++; memcpy(g_Stats.lastOrg, o, 12); memcpy(g_Stats.lastFwd, a, 12); }
                g_Real(o, a, out);
                CameraHook::Tans tans; CameraHook::ApplyEyeProjection(mainView, viewEye, tans);   // mode 2 (research): its own matrix
                StereoTaa::OnMainView(mainView, eye, out);    // each eye's own "previous frame" for the motion vectors
                const unsigned long long serial = PublishView(o, a, out, &hp, g_KeepGameAxis.load(), org, false, sync ? 0 : syncOn ? -1 : -2, sync ? ~0ull : 0, &ps, &tans, canted, turn, headAxis, &snap);
                if (sync)
                {
                    memcpy(g_Pair.gameOrg, org, 12); g_Pair.set = ps; g_Pair.snap = snap;
                    g_Pair.serial = serial; g_Pair.valid = true; g_PairsLeft++;
                    StereoSync::LeftBuilt();
                }
                return;
            }
        }
        if (isMain) { std::lock_guard lk(g_M); g_Stats.mainCalls++; }
        g_Real(org, axis, out);
        if (isMain) { StereoTaa::OnMainView(mainView, -1, out); PublishView(org, axis, out); }
    }
}

bool ViewMatrixHook::Install()
{
    if (!GameInfo::IsSupportedBuild()) return false;
    g_Real = (Fn)(GameInfo::Base() + kCreateViewMatrix);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_Real, Hook);
    LONG r = DetourTransactionCommit();
    LOGI("ViewMatrixHook: CreateViewMatrix@exe+0x{:X} detour={}", kCreateViewMatrix, r);
    return r == NO_ERROR;
}
void ViewMatrixHook::Uninstall()
{
    if (!g_Real) return;
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)g_Real, Hook); DetourTransactionCommit();
}
void ViewMatrixHook::SetEnabled(bool on) { g_Enabled = on; }
bool ViewMatrixHook::Enabled() { return g_Enabled; }
void ViewMatrixHook::Recenter() { g_RecenterReq = true; }
void ViewMatrixHook::SetWorldScale(float s) { g_Scale = s; }
void ViewMatrixHook::SetKeepGameAxis(bool on) { g_KeepGameAxis = on; }
void ViewMatrixHook::SetPitchFix(bool on)
{
    g_PitchFix = on;
    LOGI("ViewMatrixHook: the game eye's travel with its pitch {} (CameraPitchFix = {})", on ? "taken out of the view" : "kept", on ? 1 : 0);
}
std::string ViewMatrixHook::PitchFixStatus()
{
    std::lock_guard lk(g_PfM);
    std::string s = fmt::format("pitchfix {} samples {} rejected {} now fwd {:.1f} up {:.1f} cm; curve (deg: fwd/up cm)", g_PitchFix.load() ? 1 : 0,
                                g_Pf.samples, g_Pf.rejected, g_Pf.lastCorrF * 100.f, g_Pf.lastCorrZ * 100.f);
    for (float p : { -60.f, -30.f, 30.f, 60.f }) { float f, z; PfCurve(p, f, z); s += fmt::format(" {:.0f}: {:.1f}/{:.1f}", p, f * 100.f, z * 100.f); }
    return s;
}
void ViewMatrixHook::SetPivotFix(bool on) { g_PivotFix = on; LOGI("ViewMatrixHook: eye pivot held in the room {} ({} frames so far)", on, g_PivotFixed.load()); }
void ViewMatrixHook::SetAnimCam(int mode) { g_AnimCam = mode; }
int ViewMatrixHook::AnimCam() { return g_AnimCam.load(); }
void ViewMatrixHook::SetAnimCamSpeed(float mps) { g_AnimCamSpeed = mps; }
float ViewMatrixHook::AnimCamSpeed() { return g_AnimCamSpeed.load(); }
bool ViewMatrixHook::AnimViewOn() { return g_TakedownView.load(); }
bool ViewMatrixHook::AnimViewActive() { return g_AnimViewOn.load(); }
bool ViewMatrixHook::AnimYawOffset(float& deg) { if (!g_AnimOffValid.load()) return false; deg = g_AnimOff.load(); return true; }
void ViewMatrixHook::SetCineCut(float m, float deg) { g_CutDist = m; g_CutYaw = deg; LOGI("ViewMatrixHook: a cutscene cut is a jump of {:.2f} m or {:.0f} deg in one frame", m, deg); }
void ViewMatrixHook::SetCineTrack(bool on) { g_CineTrack = on; LOGI("ViewMatrixHook: a cutscene's eye {}", on ? "rides the cutscene camera (only pops smoothed)" : "follows at the takedown speed limit (old)"); }
void ViewMatrixHook::SetCineVehicle(bool on) { g_CineVehicle = on; LOGI("ViewMatrixHook: the view turns with a vehicle in a cutscene {}", on); }
void ViewMatrixHook::SetPossessView(bool on) { g_PossessView = on; LOGI("ViewMatrixHook: possession view {}", on); }
void ViewMatrixHook::CrouchInfo(float* standCamH, float* camH, float* lift) { *standCamH = g_StandCamH.load(); *camH = g_CamH.load(); *lift = g_CrouchLift.load(); }
std::string ViewMatrixHook::AnimStats()
{
    std::lock_guard lk(g_M);
    return fmt::format("mode {} speed {:.1f} on {} holding {} entries {} timeouts {} err yaw {:.1f} pitch {:.1f} pos {:.3f} possessview {} hostscale {:.2f} flatviews {} wheelviews {} "
                       "cine {} cineentries {} cuts {} roomoff {} loco {} locoentries {} roll {:.1f} cinetrack {} pops {} camspeed {:.1f} cinevehicle {} vehicle {} rides {} vehturn {:.1f}",
                       g_AnimCam.load(), g_AnimCamSpeed.load(), g_AV.on, g_AV.holding, g_AV.entries, g_AV.timeouts,
                       g_AV.lastErrYaw, g_AV.lastErrPitch, g_AV.lastErrPos, g_PossessView.load(), g_HostScale.load(), g_FlatViews, g_WheelViews,
                       PlayerState::Cinematic() ? 1 : 0, g_CineEntries, g_Cuts, g_AnimOffValid.load() ? fmt::format("{:.1f}", g_AnimOff.load()) : std::string("-"),
                       g_AV.on && g_AV.loco ? 1 : 0, g_AV.locoEntries, g_AV.lastErrRoll,
                       g_CineTrack.load() ? 1 : 0, g_Pops, g_CamSpeed, g_CineVehicle.load() ? 1 : 0, g_AV.vehicle ? 1 : 0, g_VehicleRides, g_AV.vehTurn);
}
ViewMatrixHook::Stats ViewMatrixHook::GetStats() { std::lock_guard lk(g_M); return g_Stats; }

ViewMatrixHook::XrPose ViewMatrixHook::RenderPoseXr(unsigned long long serial)
{
    std::lock_guard lk(g_ViewM);
    if (!g_ViewSerial) return XrPose();
    if (serial == 0) serial = g_ViewSerial;
    const XrPose& p = g_PoseRing[serial % 16];
    return p.serial == serial ? p : XrPose();
}
bool ViewMatrixHook::ViewAnchor(float viewOrg[3], float gameOrg[3], float hmdPos[3])
{
    std::lock_guard lk(g_ViewM);
    if (!g_ViewSerial || !g_ViewHmdValid) return false;
    memcpy(viewOrg, g_ViewOrg, 12); memcpy(gameOrg, g_ViewGameOrg, 12); memcpy(hmdPos, g_ViewHmdPos, 12);
    return true;
}
bool ViewMatrixHook::ViewPoses(unsigned long long serial, VRRuntime::PoseSet& out)
{
    std::lock_guard lk(g_ViewM);
    if (!g_ViewSerial) return false;
    if (serial == 0) serial = g_ViewSerial;
    const ViewSet& v = g_SetRing[serial % 16];
    if (v.serial != serial || !v.set.valid) return false;
    out = v.set; return true;
}
bool ViewMatrixHook::ViewBySerial(unsigned long long serial, float mat[16], float org[3], float axis[9], float* headAxis)
{
    std::lock_guard lk(g_ViewM);
    const ViewGeo& g = g_GeoRing[serial % 16];
    if (!serial || g.serial != serial) return false;
    memcpy(mat, g.mat, 64); memcpy(org, g.org, 12); memcpy(axis, g.axis, 36);
    if (headAxis) memcpy(headAxis, g.headAxis, 36);
    return true;
}
bool ViewMatrixHook::ViewEntities(unsigned long long serial, BodyHide::Anchor* anch, float cam[3], float camAxis[9], bool* camValid)
{
    std::lock_guard lk(g_ViewM);
    const ViewSnap& s = g_SnapRing[serial % 16];
    if (!serial || s.serial != serial || !s.valid) return false;
    memcpy(anch, s.anch, sizeof s.anch); memcpy(cam, s.cam, 12); memcpy(camAxis, s.camAxis, 36); if (camValid) *camValid = s.camValid;
    return true;
}
static unsigned long long SerialOfViewMatrixLocked(const float m[16]);
unsigned long long ViewMatrixHook::SerialOfViewMatrix(const float m[16])
{
    // Asked for draw after draw with the same matrix (one PerViewCB serves a whole pass): the last answer is kept per
    // thread until a new view is published -- no lock and no scan for the rest of the pass.
    thread_local float t_M[16] = {}; thread_local unsigned long long t_Gen = 0, t_Serial = 0;
    const unsigned long long gen = g_ViewSerialPub.load(std::memory_order_acquire);
    if (gen && gen == t_Gen && !memcmp(m, t_M, 64)) return t_Serial;
    const unsigned long long s = SerialOfViewMatrixLocked(m);
    memcpy(t_M, m, 64); t_Gen = gen; t_Serial = s;
    return s;
}
static unsigned long long SerialOfViewMatrixLocked(const float m[16])
{
    std::lock_guard lk(g_ViewM);
    unsigned long long best = 0;
    for (const ViewGeo& g : g_GeoRing)
    {
        if (!g.serial || g.serial <= best) continue;
        float e0 = 0, e1 = 0;
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++)
        { e0 = std::max(e0, fabsf(m[i * 4 + j] - g.mat[i * 4 + j])); e1 = std::max(e1, fabsf(m[i * 4 + j] - g.mat[j * 4 + i])); }
        if (std::min(e0, e1) < 1e-4f) best = g.serial;     // the newest that matches (a still head repeats a matrix)
    }
    return best;
}
int ViewMatrixHook::NextViewEye()
{   // as Hook decides (main view thread)
    if (!g_Enabled.load() || ScreenMode::FlatRender()) return -1;
    if (!StereoSync::Active()) return VRRuntime::CurrentEye();
    if (StereoSync::LastFrameHeld() && g_Pair.valid) return 1;
    return StereoSync::PairWanted() ? 0 : -1;
}
unsigned long long ViewMatrixHook::LatestSerial() { return g_ViewSerialPub.load(std::memory_order_acquire); }
bool ViewMatrixHook::RenderView(float mat[16], float org[3], float axis[9], unsigned long long* serial, float* headAxis)
{
    std::lock_guard lk(g_ViewM);
    if (!g_ViewSerial) return false;
    memcpy(mat, g_ViewMat, 64); memcpy(org, g_ViewOrg, 12); memcpy(axis, g_ViewAxis, 36);
    if (headAxis) memcpy(headAxis, g_ViewHeadAxis, 36);
    if (serial) *serial = g_ViewSerial;
    return true;
}
