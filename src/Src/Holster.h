#pragma once
#include <cstdint>
#include <string>

//! Holsters ([Gameplay] HolsterWeapons). The LEFT hand, with its grip: the pistol at the right hip (a cross draw), the
//! crossbow behind the back, the last power it held on the chest. The RIGHT hand, with its grip: the sword at the left
//! hip. Each is a toggle: what is already in that hand is put away by the same move, leaving the hand empty (a power out
//! and the right hand free, say). KbmInject tells the zones and swallows that squeeze (no power wheel, no block).
//!
//! A draw is the game's own equip: ARK_MSG_INVENTORY_EQUIP_ITEM (0x9D, payload {arkDeclInventoryItem*, flags}) posted
//! to the player, what the script node ActionPlayerInventory sends (exe+0x534760: the sword with flags 5, its left-hand
//! equipment with 0) and what the inventory itself posts when an item has to be equipped (exe+0x619B93). Putting away
//! is ARK_MSG_INVENTORY_UNEQUIP_ITEM (0x9E, arkMsgInventoryUnequipItem {m_decl, m_slot, m_instant}), as that node's
//! Unequip sends it. The player's equip state then changes the hands with their animations.
//!
//! The pistol / crossbow are found in the inventory's equipment list (arkCpntInventory+0x58, count +0x60: pointers to
//! the item entries that have an equipment entity) by their item component's class (entry+0x10: arkCpntWeaponGun /
//! arkCpntWeaponCrossbow). Where there are several of one (one per ammunition), the one last held is drawn. Powers are
//! told by their item's name (models/powers/...); the last one seen in the left hand is the one drawn (before any: the
//! first one owned). The sword is what exe+0x614DD0(inventory, type 1, hand 0) finds, as the script node does.
namespace Holster
{
    enum Kind { kPistol = 0, kCrossbow = 1, kMagic = 2, kSword = 3 };
    void SetEnabled(bool on);
    bool Enabled();
    //! The player has that weapon (refreshed by Tick a few times a second).
    bool Owns(int kind);
    //! That weapon is in its hand now (the sword: the right hand; the others: the left).
    bool InHand(int kind);
    //! Draw it, or put it away when it is in its hand already (any thread): done by the next Tick.
    void Request(int kind);
    //! Game thread, once a frame (HandAimHook, next to Collision::Tick).
    void Tick();
    std::string Stats();
}
