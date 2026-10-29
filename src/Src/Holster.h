#pragma once
#include <cstdint>
#include <string>

//! Holstered weapons ([Gameplay] HolsterWeapons): the left hand draws the pistol from the left hip and the crossbow
//! from behind the back -- the hand there and the left trigger pulled (KbmInject tells the zone and swallows that
//! press; the trigger does nothing else).
//!
//! The draw is the game's own equip: ARK_MSG_INVENTORY_EQUIP_ITEM (0x9D, payload {arkDeclInventoryItem*, flags}) posted
//! to the player, what the script node ActionPlayerInventory sends for its left-hand equipment (exe+0x53485A, flags 0)
//! and what the inventory itself posts when an item has to be equipped (exe+0x619B93). The player's equip state then
//! changes the item with its animation, as the quick slots do.
//!
//! The weapon is found in the inventory's equipment list (arkCpntInventory+0x58, count +0x60: pointers to the item
//! entries that have an equipment entity) by its item component's class (entry+0x10: arkCpntWeaponGun /
//! arkCpntWeaponCrossbow). Where there are several of one (one per ammunition), the one last held in the left hand is
//! drawn, so a drawn crossbow comes back with the bolts it had.
namespace Holster
{
    enum Kind { kPistol = 0, kCrossbow = 1 };
    void SetEnabled(bool on);
    bool Enabled();
    //! The player has that weapon (refreshed by Tick a few times a second).
    bool Owns(int kind);
    //! That weapon is in the left hand now.
    bool InHand(int kind);
    //! Draw it (any thread): done by the next Tick.
    void Request(int kind);
    //! Game thread, once a frame (HandAimHook, next to Collision::Tick).
    void Tick();
    std::string Stats();
}
