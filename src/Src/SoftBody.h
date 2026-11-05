#pragma once
#include <string>

//! Characters that give under the hands ([Gameplay.Collision] SoftCharacters, with HandCollision): a hand pressing on a
//! standing character bends his skeleton where it presses -- a shoulder turns away, an arm is pushed aside, the head
//! tilts -- and it springs back with a little sway when the hand leaves; with the grip squeezed on him the hand pulls
//! that part along instead. Drawn only: the character does not move in the world and the game does not know.
//!
//! Where the pose goes to be drawn (measured 2026-11-04, ENGINE.md 6.6): each animation update copies the posed joints
//! (model space, skeleton object+0x198) into the pose array (obj+0x2D8) one "space" at a time, exe+0x540A90(obj, space)
//! appending the entries (exe+0x53BAD0) and setting the space's remap (obj+0x2E8 -> an array of {short*, count}, one
//! per space, kept inline at obj+0x2F8); space 3 is the last block, and the skinning matrices are made from its J
//! entries (exe+0x25EFE0, given [[obj+0x2E8]+0x30][0]: each through exe+0x9B980, then times the skeleton definition's
//! inverse binds at def+0x120, exe+0x24C340, into the render model). After that copy the bent
//! part's entries are turned about its pivot joint (an entry's rows are the joint's axes in model space, +0x30 its
//! position). The parents are not stored anywhere the mod has found, so they are worked out once per skeleton: a
//! joint's true parent is the earlier joint in whose frame it keeps the same offset in the bind pose (the inverse
//! binds inverted) and the animated one (space 2).
namespace SoftBody
{
    void SetEnabled(bool on);
    bool Enabled();
    //! SwordContact's hand-body test (its thread), once a tick per hand: the deepest point of that hand inside a
    //! standing character's body this tick (ent null = no contact): the joint, the point on the body's surface (world),
    //! the normal out of it towards the hand, how deep the real hand is in (m), the real hand's position. Returns how far
    //! the body has given at that point (m), which the hand's stop moves in by.
    float Press(int hand, void* ent, int joint, const float surface[3], const float n[3], float depth, const float handPos[3], double t);
    //! The same thread, after both hands: the springs.
    void Tick(double t);
    std::string Status();
}
