#pragma once
#include <string>

//! [Cheats.Player] Undetected: guards and other hostile people do not notice you -- live in the world without being
//! hunted. Attacking someone (or a fight already on) still makes him fight back.
//!
//! How (ENGINE.md 10.6): the game's stealth reads the player's attributes (arkCpntAttributes, type exe+0x39EDB90, on the
//! player entity): ARK_ATTRIBUTE_PLAYER_VISIBILITY_FACTOR and _VISIBILITY_WHEN_MOVING_FACTOR (how fast you are seen),
//! _RUNNING / _SPRINTING_FOOTSTEP_NOISE_FACTOR (how loud your steps are), _STEALTH_ILLUMINATION_FACTOR (how lit you
//! are), _INVISIBLE_WHEN_NOT_MOVING (_CROUCHED) (the bone charms' unseen when still). Each is an entry of the
//! component's table (comp+0x48, 0x28 bytes each: its name atom at +0x00, the value the game reads at +0x24), found by
//! name with the component's own lookup exe+0x60CE50(comp, &index, &atom). While on, each game tick the factors are held
//! at 0 and the two "invisible" ones at 1; off, the values found are put back.
namespace Undetected
{
    void SetEnabled(bool on);
    bool Enabled();
    void Tick();   //!< the game thread, once a frame
    std::string Status();
}
