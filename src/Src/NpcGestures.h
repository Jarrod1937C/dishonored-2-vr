#pragma once
#include <string>

//! Research on gestures at people (logging only, for what the game does next): a weapon held on someone (the pistol
//! or crossbow's ray on his head or chest within 5 m, or the sword's tip at his face or chest within 1.5 m, for 0.7 s:
//! a civilian would cower, then flee), a tap on someone's shoulder or upper back from behind (he would turn).
//! (The pickpocket by hand and the wave to talk built on it were dropped after play, 2026-11-05.)
//!
//! People: the characters within 10 m (the entity table, every 0.3 s), alive and on their feet (entity+0xD1 & 0x28
//! clear); their skeleton (NpcSkel) only for what a hand touches (the shoulder).
namespace NpcGestures
{
    //! The game thread, once a frame (nothing with logging off).
    void Tick();
    std::string Status();
}
