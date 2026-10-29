#pragma once
#include <d3d11.h>
#include <atomic>
#include <string>
#include <vector>

//! GPU route to "put the weapon model in your hand" (see docs/ENGINE.md, 2026-08-30): every CPU-side pose the
//! engine exposes was measured to have no effect on the first-person weapon, because its placement comes from a
//! camera-relative joint palette we could not find in memory. So we go one level down: find the draw call that
//! draws the weapon and transform its skinning constants on the way to the shader.
//!
//! Today it does much more than the weapon (docs/ARCHITECTURE.md, "The body on the GPU"): every draw's per-instance
//! MVP is factored into projection + pose; a draw is a hand's weapon when that hand's equipment ENTITY is the nearest
//! first-person entity to its model origin (no scan/diff any more); the arm palettes are rewritten per draw (hand
//! delta, two-bone IK, rest pose, weapon grip). The draw-signature dumps below (the first stage: one dump with the
//! weapon shown, one hidden, and the diff) remain as research tools.
namespace GpuTrace
{
    //! Install from the game's own swapchain (call at the first Present, not from init).
    bool Install(IDXGISwapChain* sc);
    //! Hooks the immediate context's other implementation once it is in use (multithread protection switches it). Render thread, every Present.
    void RehookImmediate();
    //! vr handsfromview: the hands / weapons placed with the poses their view was built from (1) or the latest (0).
    void SetHandsFromView(bool on);
    bool HandsFromView();
    void Uninstall();

    //! Record the next frame's draw signatures and write them to the log under this tag.
    void RequestDump(const char* tag);
    //! Frame boundary (called from Present): flush a pending dump.
    void OnPresent();

    //! Draws selected explicitly by index count (stable across runs, unlike the shader pointers). Mostly a research
    //! tool now: index counts are ambiguous (the same count is used by unrelated meshes) and specific to one save.
    void SetTargets(const std::vector<unsigned>& idx);
    void TargetsFromWeapon();
    //! Skip the target draws. Proves an identified signature really is the weapon (its geometry vanishes).
    void SetSkip(bool on);
    //! Hide every draw (all draw calls) whose vertex shader has one of these DXBC-header hashes (gpudump prints
    //! them). Stable across runs, unlike the shader pointers. Empty = hide nothing.
    void SetSkipVsHashes(const std::vector<uint64_t>& hashes);
    unsigned long long SkipVsHits();
    //! The sword's trail: 1 (default) hides it around the player's swing (see kTrailHashes in the .cpp). The window
    //! is driven by MeleeAim from the Slash state.
    void SetNoTrail(bool on);
    bool NoTrail();
    void SetTrailWindow(bool active);
    std::string TrailStats();

    //! Research: on the next draw whose index count matches the skip set, grab that draw's VS constant buffers and
    //! dump them at the following Present — the read has to happen there because a deferred context cannot Map for
    //! reading, so the buffers are copied to a staging resource on the immediate context. Looks for the joint
    //! palette: reports every offset holding an orthonormal 3x3.
    void RequestConstantDump();

    //! Move the weapon by offsetting the view matrix in a private copy of its constant buffer, bound only around
    //! the target draws (view-space metres: right, up, forward). The world is drawn with the untouched buffer.
    //! All zero = off.
    void SetViewOffset(float x, float y, float z);
    //! Bisect switch: turn the draw-hook constant-buffer swap off while leaving the Present-side work on.
    void SetBind(bool on);
    //! Bisect which constants the weapon's shader reads: zero this float range in the patched copy (-1 = off).
    void SetZeroRange(int from, int count);
    //! Which VS constant buffer slot to patch (slot 1 is bound but unused by the weapon's shader).
    void SetSlot(int slot);
    //! Disassemble the vertex shader of the next target draw to <game>\DVR_shader.txt.
    void RequestShaderDump();
    void DumpShaderByHash(uint64_t hash);   //!< research: disassemble the vertex shader with this DXBC hash

    //! --- stage 5: 6DOF weapon ---
    //! The weapon's placement is a full model-view-projection matrix handed to the shader per draw in `PerInstanceCB`
    //! (cb2), and the vertices arrive in MODEL space. So the way to put the model anywhere is to rewrite that matrix.
    //!
    //! We never need the engine's view or projection for it. A perspective projection's last row is (0,0,1,0), so the
    //! MVP's w row IS the third row of model->view: from it the whole thing factors, per draw, into a projection and a
    //! rigid model->view pose. Rebuilding with a different pose and the SAME projection moves the model and nothing
    //! else — and because every pass (depth, velocity, material) is factored with its own projection, they all receive
    //! the same pose and cannot disagree about depth.
    //!
    //! mode 0 = off, 1 = the weapon follows the controller (delta from the pose it had when calibrated).
    void SetSixDof(int mode);
    int  SixDof();
    //! Re-anchor: the controller's current pose becomes the rest pose, so the weapon does not jump when enabled.
    void Calibrate();
    //! Which controller drives it: -1 = whatever HandAimHook::GunHand() says (so the model and the fire ray agree).
    void SetSixDofHand(int hand);
    //! How the weapon is held. 1 (default) = DETERMINISTIC: the weapon's forward is the controller's forward, so
    //! the barrel points where the controller points, identically in every session, and `gpugrip` trims the
    //! model's own axis mismatch once and for all. 0 = the legacy hold, captured from whatever rotation the
    //! controller happened to have relative to the weapon at the instant the mode anchored -- which made the hold
    //! different on every load and left the weapon visibly rotated when the anchor landed on an idle wrist.
    void SetHoldMode(int mode);
    int  HoldMode();
    //! Grip trim in the CONTROLLER's frame (right/up/forward metres): where the weapon sits relative to the hand.
    //! hand: 0 = left, 1 = right, -1 = both. Per hand because the two holds are genuinely different -- a pistol
    //! points along the controller, a sword stands vertical out of the fist.
    void SetSixDofOffset(int hand, float right, float up, float fwd);
    //! Grip rotation in the controller's frame, degrees: yaw about up, pitch about left, roll about forward. The
    //! model's own axes need not line up with the controller's, so position alone cannot fix the hold.
    void SetGrip(int hand, float yawDeg, float pitchDeg, float rollDeg);
    //! The HAND's own grip and trim, used when a POWER rather than a weapon is in that hand. Separate from the
    //! weapon's because the weapon grip aligns the barrel with the controller and a casting hand has no barrel:
    //! with the weapon grip the palm came out facing forward where the game's power pose holds it facing sideways.
    void SetHandGrip(int hand, float yawDeg, float pitchDeg, float rollDeg);
    void SetHandOffset(int hand, float right, float up, float fwd);
    void HandGripOf(int hand, float ypr[3], float off[3]);
    //! How an EMPTY hand (a power, not a weapon) is posed. 2 (default) = ANATOMICALLY, from the mesh itself: the
    //! hand's own long axis along the controller and the palm turned towards the body's midline, which is how a
    //! hand holds a controller and what the user asked for. 1 = replay the hold learned while a weapon was in that
    //! hand -- measured to be whichever weapon came last, and a pistol and a crossbow teach holds 90 degrees apart,
    //! so the same power changed pose from one minute to the next. 0 = rebuilt from the socket constant (pre
    //! 2026-09-15). A/B switch on hardware.
    void SetUseHandHold(int on);
    //! 1 (default): crouched, the weapon hand keeps the hold it had standing instead of turning over with the animation.
    void SetCrouchHold(int on);
    int  UseHandHold();
    //! Ask for the next `lines` handrel lines to be logged, so a measurement can be taken at a known moment
    //! (this weapon, this pose, the time frozen) instead of whenever the periodic gate happens to fire.
    void RequestHandRelLog(int lines);
    //! vr handtrace <frames> [every]: the drawn palm on the weapon, the flat game's palm on its entity, the fingers and
    //! the learned hold, against a reference (`ref` takes the current pose as it), with the player's states.
    void HandTrace(int frames, int every); void HandTraceRef();
    //! vr weapongrip <0|1|2>: 1 (default) = a hand on a weapon is always posed from that weapon's learned grip (palm and
    //! fingers, the game's idle grip, learned only when no state moves the hand on the weapon, kept in DVR_grips.txt);
    //! 0 = the old way (the animation's hand; crouched, the standing hold); 2 = learn, but draw the animation (A/B).
    void SetWeaponGrip(int mode);
    std::string XfViewStats();          //!< research: the moved draws' view vs the view their transform was made for
    unsigned long long EyeFixedDraws();   //!< draws whose previous MVP was taken to their own eye's last view (StereoTaa)
    void SetPrevPerEye(bool on);        //!< vr aaarms 0|1: stereo, the moved draws' previous delta from the same eye (StereoTaa)
    void SetArmForeign(int on);         //!< vr armforeign 0|1: 0 = analyse arm meshes during Shadow Walk too (the old bug, A/B)
    void WeaponGripForget(bool file);   //!< vr weapongrip forget (session) / forgetall (and the file)
    std::string WeaponGripStats();

    //! --- the reticle (screen-space overlay) ---
    //! World point -> NDC (x/y in [-1,1], +y up) through the projection the engine handed the shaders THIS frame,
    //! recovered by the MVP factorisation. Using the engine's own projection means there is nothing to calibrate:
    //! the aspect it really renders at, any letterboxing and the TAA jitter are already in it. `depth` (optional)
    //! comes back as the view-space forward distance. False when the point is behind the eye or no view is current.
    bool ProjectWorld(const float w[3], float ndc[2], float* depth = nullptr);
    //! The inverse: the world ray (eye, unit direction) through an NDC point of the same projection and view.
    bool UnprojectNdc(const float ndc[2], float org[3], float dir[3]);
    //! On this thread, ProjectWorld / UnprojectNdc use the projection and view of main view `serial` when its draws were
    //! seen (the last 4 views are kept), else the latest as before; 0 = the latest. The reticle draws onto the image
    //! about to be presented, which is not the latest view (the pipeline), and in stereo with the per-eye projection
    //! the two eyes' projections differ by a third of the picture.
    void ProjectForSerial(unsigned long long serial);
    //! This frame's controller ray in world space, anchored to the RENDER view exactly as the weapon and the hand
    //! are, so the reticle cannot disagree with where the gun is drawn. Rows/columns as elsewhere: fwd = column 0.
    bool HandRay(int hand, float org[3], float fwd[3]);
    void SetProjAny(int on);                     //!< research: 1 = reticle projection from any factored draw (the pre-2026-10-03 bug)
    //! The whole controller pose in world space (columns fwd/left/up), for the axes overlay.
    bool HandPoseWorld(int hand, float org[3], float rot[9]);
    //! Where the mod has just PUT the hand: the palm bone's frame in world space after this frame's delta, with the
    //! direction the fingers point (the analysed mesh's bind axis, rotated into it). Drawing these is how a hand
    //! pose gets judged: a fanned glove silhouette has no readable direction, and guessing from one cost a whole
    //! session. False when this side produced no palm in the last few frames.
    bool PalmFrame(int hand, float org[3], float rot[9], float fingers[3], float normal[3] = nullptr);
    //! --- effects anchored to the arm (HandFx) ---
    //! Re-pose a joint the ENGINE computed from the animated skeleton so that it lands where this mod DRAWS it: the
    //! same per-segment delta the palette rewrite applies to bone `joint` of that side, from the last rendered frame.
    //! `pos`/`axis` are in the model space of that arm's draws, engine convention (axis rows = the joint's basis
    //! vectors). Callable from any thread. False (and nothing touched) when the rig has no frame, or the joint is not
    //! one the rewrite moves. `seg` (optional) = 1 hand, 2 forearm, 3 upper arm, 4 root.
    bool ArmCorrectJoint(int side, int joint, float pos[3], float axis[9], int* seg = nullptr);
    //! Research for HandFx: the last frame's model->world of that side's draws (rows r|t, 12 floats), the animated
    //! palette bone `bone` (12 floats as the palette stores it) and the palette's bone count.
    bool ArmSnapshot(int side, float m2w[12], int bone, float boneRT[12], int* bones, unsigned long long* frame = nullptr);
    unsigned long long PresentFrame();         //!< frames presented so far (the clock the snapshots above are stamped with)
    //! The hand delta's change between consecutive frames at the wrist (what an effect placed with last frame's
    //! delta is off by): mean / max in metres since the last call, and the sample count. Resets.
    void ArmFxLag(int side, double& mean, double& max, unsigned long long& n);
    //! First-person draws in the last frame (arms + both hands' equipment), or -1 when nothing counts them. Zero
    //! means a menu, a loading screen or a cutscene: nothing first-person is being drawn.
    int FirstPersonDraws();
    //! Research: log the factorisation of the next N target draws (buffer layout, projection, pose, residuals) and,
    //! for the 384-byte layout, cross-check it against the model matrix the shader is handed alongside the MVP.
    void SetDiag(int draws);
    void SetArmCbDump(int draws);
    void SetGadget(int on, float palmMax);   //!< gpugadget: a 1-2 draw item with its entity in the palm is held like a weapon    //!< research: log the whole cb2 of the next n arm draws
    //! Bisect the view-basis signs if the factorisation's axes come out mirrored: bit0 right, bit1 up, bit2 forward.
    void SetBasis(int mask);
    //! Identify the weapon by where it is drawn instead of by index count: any draw whose model origin sits within
    //! `metres` of the eye is first-person geometry. Index counts are specific to one weapon in one save; this is
    //! not. 0 = off (index counts only).
    void  SetNear(float metres);
    float Near();
    //! Research: factor every draw for `frames` frames and log those within `metres` of the eye, with the screen
    //! position of each — read the weapon's index counts straight off that list.
    void RequestFind(float metres, int frames);
    //! Work out which of the near-eye draws are the WEAPON rather than the arms and torso, by scanning once
    //! normally and once with BodyHide hiding the weapons and keeping what disappeared. The weapon is a submesh of
    //! the same skinned model as the arms and shares its matrix, but it has its OWN draws — and since the constants
    //! are rewritten per draw, telling them apart by draw is enough. Costs a brief flicker; re-run on weapon change.
    void IdentifyWeapon();
    //! How the weapon's draws are picked. 1 (default) = the equipment ENTITY's world transform: a draw is the
    //! weapon when its recovered model origin is nearer the weapon entity than to the arms or the player, decided
    //! per draw and per frame. 0 = the legacy scan/hide/scan diff, which was not deterministic
    //! and is kept only for comparison.
    void SetIdMode(int mode);
    int  IdMode();
    //! Sanity bound on how far a draw may sit from its entity and still count as that entity's, in metres.
    void SetAnchorRadius(float metres);
    //! Restrict the 6DOF rewrite to those draws (default on). Off = everything near the eye moves, arms included.
    void SetOnlyWeapon(bool on);
    //! Where this hand's weapon model is being drawn this frame, in world space: origin + idMat3 (rows fwd/left/up),
    //! i.e. `hand * gripBase * grip`. False when the weapon is not being moved. WeaponPose feeds this back into the
    //! engine's own transform for the weapon render models, so that everything the engine anchors to the weapon --
    //! the muzzle flash above all -- follows the model instead of staying on the character. It has to be THIS pose
    //! and not a second, independently computed one, or the flash drifts off the barrel the moment the grip is
    //! trimmed with gputrim/gpugrip.
    bool WeaponTargetPose(int hand, float org[3], float axis[9]);
    //! WeaponTargetPose before Collision pushed the hand out of what it touches: where the real controller holds it
    //! (SwordContact strikes with this blade, so a blade stopped at a body still strikes it).
    bool WeaponTargetPoseRaw(int hand, float org[3], float axis[9]);
    //! The same, only while the hand holds an item (a weapon entity is anchored to it), with that entity (Collision).
    bool RawItemPose(int hand, float org[3], float axis[9], void** ent);
    //! The controller pose (columns fwd/left/up, as HandPoseWorld) of the latest view BEFORE Collision moved it.
    bool RawHandPose(int hand, float org[3], float rot[9]);
    //! The hand whose weapon a model with its origin at w (world) is drawn with -- the weapon test's own rule (nearest
    //! first-person entity is that hand's equipment, within the anchor radius) -- or -1. Lock-free, any thread.
    int WeaponClaim(const float w[3]);
    //! The pose this hand's weapon WOULD be drawn at for a given controller pose (`hand * gripBase * grip`, the same
    //! product EnsureTransform uses), so another thread -- the game's, when it fires -- can place the weapon for the
    //! controller as it is NOW instead of as it was when the last view was recorded. axis = idMat3 rows fwd/left/up.
    //! `ent` (optional) = the equipment entity this hand's weapon is anchored to. False when no weapon is anchored.
    bool WeaponFromHand(int hand, const float horg[3], const float haxis[9], float org[3], float axis[9], void** ent = nullptr);
    //! --- the hands ---
    //! The first-person ARM models (PlayerLeftArm / PlayerRightArm) are skinned meshes whose model origin sits at the
    //! arm entity (measured: 0.000 m for all eight of their draws), and each arm reads its own joint palette through
    //! a VS shader resource. With this on, those draws get the SAME rigid delta as that hand's weapon, so the hand
    //! (and, for now, the arm it is attached to) lands on the controller. Which palette belongs to which arm is
    //! learned once by hiding the left arm for a few frames. Needs `vr hidearms 0`.
    void SetHandFollow(int on);
    int  HandFollow();
    std::string HandStats();
    //! Draw only the hand: the listed bones of that arm's palette are collapsed onto the hand socket for the draw.
    void SetHandCollapse(int on);
    void SetCollapseBones(int side, const std::vector<int>& bones, bool clear);
    void LogBoneStats(int side);                   //!< per-bone rigidity against the hand socket (research)
    void AutoCollapse(int side, float thresholdCm); //!< collapse every bone that does not follow the socket rigidly
    void ResetBoneStats(int side);
    //! Research: write the vertex/index buffers and input layout of the next arm draw with this index count.
    void RequestVertexDump(unsigned indexCount, int side = 0);
    //! --- the full arm and the torso (vr body 1) ---
    //! Arm mode 1 rewrites every bone of the arm palettes: M_i' = D_i * M_i with a rigid delta per segment -- the hand's
    //! delta for the hand, the deltas of a two-bone IK (shoulder under the head -> elbow -> the hand's wrist) for the
    //! forearm and the upper arm, the HMD body delta for what is left. The bind pose is never needed. Mode 0 is the
    //! hand-only collapse.
    void SetArmMode(int mode);
    int  ArmMode();
    //! A scripted animation (a takedown) owns the arms and the weapon: leave their draws as the game made them. They
    //! are still identified and counted (FirstPersonDraws), only not rewritten; WeaponTargetPose reports nothing.
    void SetAnimSuspend(bool on);
    bool AnimSuspended();
    unsigned long long AnimSuspendedDraws();
    //! The player entity's own models (torso, legs) follow the HMD body frame: leaning and crouching move them.
    void SetBodyFollow(int on);
    //! Where the shoulder sits relative to the head, in the yaw-only body frame (fwd, outward, up; metres).
    void SetShoulder(float fwd, float out, float up);
    //! The drawn arm's shoulder-to-wrist length target (m, 0 = the game's own) and the largest stretch allowed.
    void SetArmLength(float metres, float maxScale = 0.f);
    //! Which way the elbow points (fwd, outward, up in the body frame): the IK's pole vector.
    void SetPole(float fwd, float out, float up);
    //! Segment lengths assumed until the joints are measured from the animation: wrist->elbow, elbow->shoulder.
    void SetArmPriors(float forearm, float upper);
    //! Manual segment for bones of the main palette: 0 = automatic, 1 hand, 2 forearm, 3 upper arm, 4 root, 5 leave alone.
    void SetArmSegment(int side, const std::vector<int>& bones, int seg);
    void ResetArmJoints(int side);                //!< -1 = both: forget what the animation taught about the joints
    void LogArmRig(int side);                     //!< everything about one arm's rig, joints and this frame's IK
    std::string BodyStats();
    //! vr handjit: per arm draw, the drawn palm against the palm its own data aimed at (mm, in the draw's view).
    //! Resets the statistics and logs the next `draws` arm draws in detail.
    void RequestHandJit(int draws);
    void RequestParts(int draws);   //!< research: the next n weapon draws, where each is drawn in the drawn weapon's frame
    //! 1 (default): each arm draw is posed from its own palette; 0: the old shared per-Present frame (comparison only).
    void SetArmSolve(int mode);

    std::string HandJitStats();

    void SetHandCut(float metres);      //!< bind-pose distance from the fingertip beyond which a bone is forearm (default 0.22)
    void LogArmBones(int side);
    std::string WeaponDraws();
    //! Draws claimed by that hand's equipment in the last completed frame (0 = the hand holds nothing that is drawn:
    //! a sheathed sword, an empty hand). -1 when the 6DOF path is off and nothing is counted.
    int HandItemDraws(int hand);
    //! While GrabHand holds an object in a hand, its fingers take the fist learned from that hand's weapon grip
    //! (1, default) or stay as the game animates them (0).
    void SetGrabFist(int mode);
    std::string FistStats();
    //! An EMPTY hand (nothing equipped, nothing held) keeps one pose instead of the walk cycle's fingers: the game's
    //! open hand, learned standing still, closed towards a fist by `curl` (0..1, < 0 keeps it) and, with `grip` 1, by
    //! the controller's grip (< 0 keeps it). mode 0 = the game's fingers. Forget = learn both poses again.
    void SetHandRest(int mode, float curl, int grip);
    void ForgetHandRest();
    //! [Controls] FingerTracking (Index layout): the drawn fingers of an empty hand or a hand with a power follow the
    //! controller's fingers (XRInputState::fingerCurl), each finger its own.
    void SetFingerTracking(bool on);
    std::string HandRestStats();
    std::string SixDofStats();

    // ---- body view: where the drawn body is, for comparing it with the real one (BodyView) ----
    //! One arm's IK this frame, in WORLD space: the animated joints (S/E/W) and where the IK put them (Sp/Ep/Wp).
    struct ArmJoints
    {
        bool valid = false; unsigned long long frame = 0;
        float S[3], E[3], W[3], Sp[3], Ep[3], Wp[3], head[3], ctrl[3];
        float L1 = 0, L2 = 0, reach = 0; bool stretched = false;
    };
    bool GetArmJoints(int side, ArmJoints& out);
    //! The render view this frame: world -> view, rows right/up/forward (the factorisation's basis), and its origin.
    bool GetRenderView(float r[9], float t[3], float org[3]);
    //! The projection of the player's own draws (what the reticle uses). False when stale.
    bool GetPlayerProjection(float p[4][4]);
    //! The view serial (ViewMatrixHook) the draws recorded since the last Present were placed with, and how many
    //! Presents ago that was last refreshed. The image about to be presented was rendered from THAT view.
    bool DrawViewSerial(unsigned long long& serial, unsigned long long& age);
    //! The view serial carried by the last command list executed on the immediate context (the image about to be
    //! presented), and how many Presents ago that was. The reliable one; DrawViewSerial is the fallback.
    bool ExecutedViewSerial(unsigned long long& serial, unsigned long long& age);
    //! Census of the projections every factored draw used over the next `frames` frames: distinct (fx, fy, depth row)
    //! with counts, the nearest and farthest model origin, and how many of them were the player's own draws. Logged
    //! when done ("projcensus"), and kept for ProjCensusText.
    void RequestProjCensus(int frames);
    std::string ProjCensusText();

    struct Stats { std::atomic<unsigned long long> draws{ 0 }; unsigned long long frames = 0; bool installed = false; };
    unsigned long long Draws();
    unsigned long long Frames();
    bool Installed();
    //! Why the constant-buffer swap did or did not happen � the difference between "no effect" and "never ran".
    std::string SwapStats();
    //! Size of the vertex-shader bytecode cache. A level load creates a whole new set of shaders and the old set's
    //! bytecode is never handed back, so this is where that growth shows up.
    std::string ShaderCache();
    size_t ShaderCount();      //!< vertex shaders whose bytecode was captured at creation (hundreds in play)
    uint64_t VsHashOf(const void* vs);   //!< the DXBC header hash of a live vertex shader (0 = unknown)
    uint64_t ShaderHashOf(const void* sh);   //!< the same for a vertex, pixel or compute shader
}
