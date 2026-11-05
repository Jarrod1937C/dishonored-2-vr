#pragma once
#include <string>

//! Gestures at people (2026-11-05). Built: a pickpocket by hand ([Gameplay.Interaction] PickpocketByHand: sneaking,
//! behind or beside someone, the grabbing hand at his belt, grip, pull back -- the game's own interact on him, so only
//! where the game offers it) and a wave to talk ([Gameplay.Interaction] WaveToTalk: a hand waved at someone within 3 m
//! you are looking at -- the game's interact on him, so only someone the game lets you talk to). Research (logging
//! only, for what the game does next): a wave at anyone (a greeting), a weapon held on someone (the pistol or
//! crossbow's ray on his head or chest within 5 m, or the sword's tip at his face or chest within 1.5 m, for 0.7 s: a
//! civilian would cower, then flee), a tap on someone's shoulder or upper back from behind (he would turn).
//!
//! People: the characters within 10 m (the entity table, every 0.3 s), alive and on their feet (entity+0xD1 & 0x28
//! clear); their skeleton (NpcSkel) only for what a hand touches (the belt, the shoulder).
namespace NpcGestures
{
    void SetPickpocket(bool on);
    void SetWaveToTalk(bool on);
    //! The game thread, once a frame.
    void Tick();
    std::string Status();
}
