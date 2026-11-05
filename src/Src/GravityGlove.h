#pragma once
#include <cstdint>
#include <string>

//! Gravity gloves ([Gameplay.Interaction] GravityGloves; as in Half-Life: Alyx): point the grabbing hand at a loose
//! object out of reach (a bottle, a glass, an item: coins, an elixir, a pouch), squeeze the grip and flick the hand
//! back or up: the object leaps to the hand. Keep the grip closed and it is caught -- held, as the grip holds anything
//! within reach (a bottle in the hand; an item in the hand, to put behind your back or eat). Let go of the grip on the
//! way and it just falls.
//!
//! The object is moved by the game's physics, with the game's impulse message (0xFA, a linear impulse when its point is
//! zero). Its mass is not known beforehand: the first impulse is sized for a light object, and the speed it actually
//! gave (the entity's velocity, which the game syncs from its body each frame) gives the mass for the corrections that
//! steer it to the hand on a ballistic path, and for the catch, which brakes it to the hand's own speed.
namespace GravityGlove
{
    void SetEnabled(bool on);
    bool Enabled();
    //! The game thread, once a frame (after Collision::Tick).
    void Tick();
    //! KbmInject: the pulled object has reached the hand with the grip still closed -- once: its id, kind (1 an object
    //! the game holds, 2 an item) and where it is (for the interaction sensor to be aimed at as it is taken).
    bool TakeCatch(uint32_t* id, int* kind, float point[3]);
    //! The hand's grip is pulling or has picked an object (KbmInject leaves that grip alone).
    bool Busy();
    std::string Status();
}
