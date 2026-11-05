#pragma once
#include <string>

//! Doors that open as far as the hand pushes them ([Gameplay.Collision] DoorsFollowHand, with PushDoors).
//!
//! A door is not moved by physics (measured 2026-11-04, ENGINE.md 8.4): its body follows its arkReferenceFrame
//! ([entity+0x358]), which every frame asks the physics body for the velocity that reaches the pose of a joint of the
//! door structure's rig (+0x3C8 the 'door_structure_animated_*' entity, +0x3D0 the joint), and that joint is posed by
//! the door's animation (the openable's [component+0xF8], slot 0: the swing, ~2.8 rad/s). The animation's player
//! (animation+0x338) advances by the time that passed since its last update: exe+0x6E2650(player, lod, time, ...)
//! takes the time (a double, passed in r8), keeps it at player+0x38 and steps the graph by the difference.
//!
//! So a held door's player is given a time of its own: it advances only by the real time that passed times the rate
//! the hand asks for (0 = the swing stops where it is). The game's door push (Collision) still starts the swing: the
//! door is held from that moment, and each frame a hand presses on it the rate is the hand's push -- its speed into the
//! door plus how far the real hand is past the door's surface -- over the speed the swing would move that point at.
//! A quick shove lets it swing on by itself (as before); pulling the hand away stops it. A push from the other side
//! turns the swing round (the game's interact on it), and the hand drives that too. A held
//! door is let go -- back on the game's time, with no jump -- when its swing ends, when the player's own interact
//! (the trigger, the grip's pull) goes to it, or when it is no longer the same door.
namespace DoorHold
{
    void SetEnabled(bool on);                //!< [Gameplay.Collision] DoorsFollowHand (installs the hook the first time)
    bool Enabled();
    //! Collision (game thread): the game's push / interact was just sent to this door by a hand -- hold its swing (or,
    //! already held, note that the next change of direction is the hand's).
    void Begin(void* door, double now);
    bool Held(void* door);
    //! Collision (game thread): a hand presses on a held door this frame, at world point p (surface normal n towards
    //! the hand), moving `into` m/s into it, the real hand `past` m behind its surface. 1 = it drives the swing,
    //! -1 = it pushes against the swing (the caller turns it round with the game's interact), 0 = not held.
    int Press(void* door, int hand, const float p[3], const float n[3], float past, float into, double now);
    //! Collision::NoteInteract (any thread): the game's interact was sent to this entity -- on a held door, the hand's
    //! own (Begin just before: the swing turns round, still held) or the player's (the trigger, the grip: let go).
    void OnInteract(void* door);
    //! Game thread, once a frame after the hands: each held door's rate, and whether to let it go.
    void Tick(double now);
    std::string Status();
}
