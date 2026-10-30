#pragma once
#include <string>

//! A held object (a bottle, a glass: the game's HoldObject) follows the grabbing hand (`vr grabhand`, the RIGHT by
//! default) instead of the camera.
//!
//! The game holds an object by BINDING its entity to joint 8 of the player's body model with a fixed local offset
//! (measured: (0.55, -0.30, -0.253) and a tilt -- the object rides 0.55 m ahead, 0.30 right and 0.25 below the eye,
//! rigid with the camera to three decimals at any pitch or yaw). The bind's transform comes from the engine's
//! model-space joint query with an offset (exe+0x53D3F0, which HandFx hooks); the physics body is keyframed to the
//! bound transform (it reaches 17 m/s for one frame when the head turns) and the render model is placed by a
//! second query with the same offset. So both are moved by answering those two queries with the hand's pose.
//!
//! The pose is relative: when the hold begins, the object's pose in the grabbing controller's frame is captured (the
//! object still sits where it was picked up) and kept, Skyrim/Fallout VR style, with the distance limited so an
//! object picked from afar comes to the hand. Nothing is written to the engine.
namespace GrabHand
{
    //! The throw of a held object (the LMB, "Lanzar") leaves with the grabbing hand's velocity: redirects the one call
    //! inside the game's throw (exe+0x83E473) that runs with the final velocity in registers. From InitThread.
    bool Install();
    void SetThrowMode(int mode);                 //!< 0 = the game's velocity (logged), 1 = the hand's (default)
    //! What the hand can take is picked by the game's own interaction sensor (both its phases, see GrabHand.cpp)
    //! aimed with the grabbing hand's pointer (PointerRay), its casts starting `back` metres behind the controller
    //! (mode 1, default), or aimed as the game aims it (0: the camera's aim fields). back < 0 keeps the current value.
    void SetReach(int mode, float back);
    //! Does the grabbing hand have something to take (the sensor, aimed with the hand, picked one)?
    bool HandHasTarget();
    //! The picked object's origin (world), while there is one; for the reticle.
    bool TargetPoint(float out[3]);
    //! The middle of an entity's box (world), by its spawn id; false when it is not in the world.
    bool EntityCenter(int id, float out[3]);
    int TargetId();                              //!< the sensor's pick (spawn id), 0x3FFF0010 = none
    //! The sensor's CARRY pick (sensor+0x88): what the interact would pick up and hold -- a bottle, a glass, a body --
    //! as opposed to what it only uses (a door, a lever, a note). 0x3FFF0010 = none.
    int CarryTargetId();
    //! PhysicalPickup: aim the interaction sensor from the eye at this world point (an item being carried) instead of
    //! along the hand's pointer, until UnlockSensor (or 8 s).
    void LockSensor(const float point[3]);
    void UnlockSensor();
    float PickRange();                           //!< the sensor's reach (its decl, the game's 3 m)
    //! Letting go now: throw (the hand is moving faster than grabcfg dropspeed) or drop? `speed` = the hand's, m/s.
    bool ReleaseThrows(float* speed = nullptr);
    void* Sensor();                              //!< the player's interaction sensor (research)
    //! Per frame (Present): which entity HoldObject holds (its state object + 0x68 = the spawn id).
    void OnPresent();
    //! From HandFx's joint-query hook (game threads). `bindEnt` = the entity being bound when the query runs inside
    //! the entity bind (null otherwise). True when `outPos`/`outAxis` (model space) were replaced.
    bool OnJointQuery(void* bindEnt, void* obj, const float* offPos, float* outPos, float* outAxis);

    void SetEnabled(bool on);
    //! Which hand takes, holds and throws: 0 = left, 1 = right (default). The interaction sweep follows it.
    void SetHand(int hand);
    int Hand();
    //! The game hides the right arm (and for some objects the left) while holding: 0 = leave it so, 1 = show the
    //! grabbing hand's arm, 2 = both (default).
    void SetShowArms(int mode);
    //! Carrying a corpse (CarryCorpse) the game leaves both arms shown; the hand that carries is busy: 0 = leave them,
    //! 1 = hide the grabbing hand's arm (default), 2 = both. The hidden hand's pointer is not drawn either.
    void SetCarryHide(int mode);
    bool CarryHidesArm(int side);
    //! Carrying a corpse, the interaction sensor aims where the eyes look (1, default: the grip's F opens the door looked
    //! at, as in the flat game) or along the hidden hand's pointer (0: F found nothing and dropped the body).
    void SetCarryGaze(bool on);
    //! How a held object is let go: 0 = the trigger (drop when the hand is slow, throw when it moves), 1 = hold the
    //! grip to hold it (letting go drops or throws, by speed), 2 = another grip press drops, the trigger throws (default).
    void SetReleaseMode(int mode);
    int ReleaseMode();
    bool Enabled();
    bool Set(const std::string& key, float v);   //!< grabcfg: maxrel, cone, downbias, ...
    std::string Config();
    std::string Stats();
    //! Research: logs every probe of the next `frames` sensor sweeps (direction, what it hit, its world position and
    //! distance from the hand) -- what the pick ends up being and why, without needing a screenshot to guess at it.
    void Diag(int frames);
    void* HeldEntity();                          //!< the entity being held now, or null
    //! Research (`vr grabcal`): logs the drawn palm in the drawn controller, the controller poses the hold can be
    //! anchored to against the drawn one, the weapon in the palm and the held object (and its box centre) in the palm.
    void Calibrate();
    //! For the axes overlay: the held object's box (8 world corners, bit 0/1/2 = max x/y/z) and centre as drawn.
    struct Debug { bool box = false; float corners[8][3] = {}; float center[3] = {}; };
    bool GetDebug(Debug& out);
}
