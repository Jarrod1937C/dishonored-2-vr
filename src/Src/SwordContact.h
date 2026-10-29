#pragma once
#include <string>
struct XRInputState;

//! The sword hits what the DRAWN blade touches, when it touches it (2026-10-04, the development notes).
//!
//! The game's melee is a canned attack: a click starts `Slash`, ~0.2 s later an animation event opens the attack
//! window (exe+0x634890 sets comp+0x3428 bit 0) and, while it is open, the per-frame hit check (exe+0x631020) sweeps
//! a box from the eye along the attack direction and hits the first candidate inside a 30 deg cone (exe+0x62F900).
//! In VR that meant: a swing only CLICKED, the hit came 0.2 s later, in one frozen direction, and anything inside the
//! cone broke -- whatever the blade you see did. Here, every frame on the Present thread:
//!   - the blade as drawn (GpuTrace::WeaponTargetPose: guard -> tip along the model's forward) is swept from where it
//!     was last frame to where it is now, as `samples` segments, against the boxes of the entities around (render model
//!     bounds * entity scale, in the entity's axes -- the same box GrabHand fits into the fist);
//!   - a segment ENTERING a box with the blade at that point faster than `minspeed` is a contact. Then: the attack is
//!     aimed at the contact point (MeleeAim::SetGestureAxis), the box is the only thing that strike may hit
//!     (MeleeAim::AddHitBox), and the attack window is opened at once (MeleeAim::RequestOpen) -- in the Slash a swing
//!     already started, or in one the contact starts with a click;
//!   - a swing that touches nothing still plays the game's Slash (and its sound, the user's call) but hits nothing:
//!     the cone test only lets through what a blade touched.
//! A charged power attack is released by the contact (SwordGesture keeps it held through the thrust until then).
namespace SwordContact
{
    struct Out { bool tap = false; bool contact = false; };
    //! KbmInject: the swing gesture recognised a slash. Instead of clicking at once, the click waits for this swing to
    //! touch something (that contact clicks, aimed at it) or to end (then it clicks, the Slash's sound in the air).
    //! False = not deferred (contact off, `swordcfg airdefer 0`): click now.
    bool DeferAirTap();
    //! Present thread, from KbmInject::Tick in play mode, BEFORE SwordGesture::Tick. `active` = the sword gestures may
    //! act (not holding an object, not in a takedown...). Out.tap = click the attack now (no usable Slash running).
    Out Tick(const XRInputState& in, bool active);
    void Reset();
    void SetEnabled(int on);                 //!< vr swordhit 0|1 (default 1); 0 = the swing gestures as before (A/B)
    int  Enabled();
    //! [Gameplay] SwordBodyDepth (cm): with WeaponCollision, how far the drawn blade sinks into a body before it stops
    //! (at most 80% of the bone's own radius, so it never comes out the other side).
    void SetBodySink(float metres);
    bool Set(const std::string& key, float v);
    std::string Config();
    std::string Stats();                     //!< heartbeat
    //! Research: log every target box around the blade (vr swordscan [radius]).
    void Scan(float radius);
    //! What the reticle draws with `vr handaxes 1`: the blade, the boxes near it, the last contact.
    struct Debug
    {
        bool blade = false; float guard[3] = {}, tip[3] = {}; int n = 0; float pts[24][3] = {};
        bool armed = false;   //!< a contact now would strike (a swing of the arm, not spent)
        bool ready = false;   //!< not spent (a faster swing would strike)
        int boxes = 0; float corners[8][8][3] = {}; bool touched[8] = {};
        static constexpr int kSegs = 600;
        struct Seg { float a[3], b[3], r; bool touched; };
        int segs = 0; Seg seg[kSegs] = {};   //!< the bodies' bone capsules
        bool contact = false; float point[3] = {}; float age = 0;
    };
    bool GetDebug(Debug& d);
}
