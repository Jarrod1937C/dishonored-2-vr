#pragma once
#include <string>

//! Parry with the blade ([Gameplay.Sword] SwordParry; as in Blade & Sorcery): hold your sword in the way of an
//! enemy's blow and it is parried -- the game's own parry, as a well-timed block: the attacker reels, no damage.
//!
//! Where the blow lands (ENGINE.md 7.3): an NPC's melee strike on the player goes through exe+0x6357D0(attacker's
//! melee component, hit record, player): the game asks whether the player is blocking towards the attacker
//! (exe+0x60D750) and, if so, its Block state (exe+0x859980) whether it was in time: 2 = a parry, exe+0x636140(comp,
//! player) -- the attacker gets the parry's damage (a stagger) -- then the sparks (exe+0x635F30) and the attack's
//! window closes (exe+0x634890); no damage. Hooked: before the game's test, when the drawn blade is (or was, within
//! the last 0.15 s) across the blow's path -- within 14 cm of the line from the attacker's weapon (the weapon entity
//! the game animates, comp+0x10) to the point the blow meets the player (record+0x30), between the two -- the game's
//! own parry branch is run instead. An unblockable attack (the attack def's +0x180 = 0) is left to the game.
namespace SwordParry
{
    bool Install();
    void SetEnabled(bool on);
    bool Enabled();
    //! The game thread, once a frame: the blade's recent positions.
    void Tick();
    std::string Status();
}
