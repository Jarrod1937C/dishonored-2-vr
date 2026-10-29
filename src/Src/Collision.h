#pragma once
#include <string>

//! Solid hands and held weapons ([Gameplay] HandCollision / WeaponCollision / PushObjects).
//!
//! The real controller cannot be stopped, so the DRAWN hand -- and the sword or gun in it, as one rigid piece -- stops
//! at what it meets and catches up when the real hand comes back. Two halves:
//!  - the game thread (Tick, once a frame from HandAimHook's aim update, where GunRay already traces): each point of a
//!    hand (the palm) and of its item (samples along the weapon model's box) is swept with the game's own Havok ray
//!    casts (hknpWorld::castRay, exe+0xC89B00, the query the line-of-sight check exe+0x636630 builds) from where it was
//!    drawn last frame to where the controller puts it now, sliding along what it hits. Every surface met becomes a
//!    contact plane tied to that point in the hand's frame. Loose physics props met that way are pushed with the game's
//!    own impulse message (0xFA, what Windblast and the damage knockback post);
//!  - the render thread (Apply, from GpuTrace where each view poses the hands): the hand's translation is pushed out of
//!    those planes using THIS view's controller pose, so the stop is exact and does not lag a frame behind.
//! Characters' bodies stop the blade too: SwordContact turns the raw blade's penetration of the bone capsules it strikes
//! with into contact planes (SetBodyContacts), so a strike still lands from the real swing while the drawn blade halts.
namespace Collision
{
    //! A plane the point `local` (in the hand's frame: x along the controller's forward, y left, z up) must stay
    //! `clear` metres in front of: dot(world(local) - p, n) >= clear.
    struct Contact { float local[3]; float p[3]; float n[3]; float clear; };

    void SetHands(bool on);                    //!< [Gameplay] HandCollision
    void SetWeapons(bool on);                  //!< [Gameplay] WeaponCollision (the sword, the left hand's guns, characters' bodies)
    void SetPush(bool on, float handMass);     //!< [Gameplay] PushObjects / PushStrength (the hand's effective mass, kg)
    void SetDoors(bool on);                    //!< [Gameplay] PushDoors: a hand pushing a door opens it (the game's own push)
    void SetOpenables(bool on);                //!< [Gameplay] PullToOpen: grab and pull drawers / cabinets open, push to close
    //! What a hand touched this frame (its first contact with an entity), fresh within ~0.1 s.
    struct Touch { bool valid = false; void* ent = nullptr; uint32_t id = 0; bool openable = false, open = false; float n[3] = {}; };
    bool Touched(int hand, Touch& out);
    //! A push on an open drawer / cabinet asks for the interact that closes it: its entity id, once.
    bool TakeCloseRequest(int hand, uint32_t* id);
    bool HandsOn();
    bool WeaponsOn();

    //! Game thread, once a frame.
    void Tick();
    //! Render thread: push a hand's world pose out of its contacts. `r` is the pose's rotation (row-major 3x3 whose
    //! COLUMNS are fwd/left/up, GpuTrace's Rigid), `t` its translation, moved in place. `applied` gets the translation
    //! added (zero when none). Returns true when the hand was moved.
    bool Apply(int hand, const float r[9], float t[3], float applied[3]);
    //! SwordContact: this frame's contacts of the raw blade with characters' bones (replaces the previous set).
    void SetBodyContacts(int hand, const Contact* c, int n);
    //! True while that hand's drawn pose is held back by something (for the reticle / logs).
    bool Touching(int hand);
    std::string Stats();
}
