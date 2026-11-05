#pragma once
#include <string>

//! [Cheats.Player] Undetected: guards and other hostile people do not regard you as an enemy -- walk around the world
//! in the open, seen, without being attacked or hunted. People who are not your enemies (civilians, shopkeepers) see
//! and react to you as usual.
//!
//! How (ENGINE.md 10.7): every question "how does A regard B" goes through exe+0x604C60(relationship manager, &A's
//! entity id, &B's id) -> 0 ALLY, 1 NEUTRAL, 2 ENEMY, 3 UNDEFINED: A's own entry for B (set by exe+0x605990 when he
//! turns on you: trespass, a body, an attack...), else his faction's relationship to B's faction. Hooked: while the
//! cheat is on, ENEMY from anyone regarding the player reads NEUTRAL -- whatever set it, and for as long as it is set,
//! so a guard who is turned against you is turned back the moment you switch it on; and exe+0x605990(manager, A, B,
//! ENEMY) with B the player is not run (a condition that sets it whenever it reads it unset would set it, and post its
//! ARK_MSG_AI_RELATIONSHIP_CHANGED, over and over). What the player regards others as (A = the player) is left alone.
namespace Undetected
{
    bool Install();
    void SetEnabled(bool on);
    bool Enabled();
    void Tick();   //!< the game thread, once a frame: the player's entity id
    std::string Status();
}
