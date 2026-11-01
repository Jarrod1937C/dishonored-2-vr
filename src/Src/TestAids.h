#pragma once
#include <cstdint>
#include <string>

//! Testing aids: reach the player's gameplay components (mana, powers, the Outsider's mark) from the mod. Everything
//! that touches them runs on the game thread (Tick, from HandAimHook's UpdateAim detour), the thread that owns them.
namespace TestAids
{
    //! Component typeinfo records (GetComponent's second argument), from tools/tirec.py on exe 1.77.9.0.
    constexpr uint32_t kManaCpntTypeInfo        = 0x39D13B0;   //!< arkCpntMana
    constexpr uint32_t kPowerSystemCpntTypeInfo = 0x39E2BB0;   //!< arkCpntPlayerPowerSystem
    constexpr uint32_t kOutsiderMarkCpntTypeInfo = 0x39D15F0;  //!< arkCpntPlayerOutsiderMark
    constexpr uint32_t kAttributesCpntTypeInfo  = 0x39EDB90;   //!< arkCpntAttributes

    //! arkCpntPlayerOutsiderMark::GiveMark(mark): what the game's "Give Mark" script node calls (exe+0x642330).
    constexpr uint32_t kGiveMark     = 0x4C1E50;
    //! bool arkCpntPlayerOutsiderMark::HasMark(mark): any item of the mark's decl owned (the function after GiveMark).
    constexpr uint32_t kHasMark      = 0x4C1F20;
    //! arkCpntInventory::AddItem(inventory, arkDeclInventoryItem*, int count): GiveMark calls it per item of its decl.
    constexpr uint32_t kInvAddItem   = 0x6156E0;
    //! int arkCpntInventory::GetQuantity(inventory, arkDeclInventoryItem*): the mark's "has the mark" test uses it.
    constexpr uint32_t kInvQuantity  = 0x615310;
    //! The inventory's full item table (every item the loadout knows, owned or not): entries of 0x38 bytes, the
    //! first qword the item's decl (name at decl+0x8); the count in the low 24 bits of the next dword.
    constexpr uint32_t kInvItems = 0x38, kInvItemCount = 0x40, kInvItemStride = 0x38;
    //! Power upgrades (what arkCpntPlayerPowerSystem::AddPowerUpgrade exe+0x455AB0 gives): the power system's decl
    //! (cpnt+0x8) holds an inline array at +0x88 of {int upgradeId; arkDeclEquipmentUpgrade* decl} (16 bytes),
    //! indexed like the entries of the arkPowerUpgradeId enum desc (*(exe+0x32CDEA8)+0x10: {char* name; int64 value},
    //! ended by an empty name). AddPowerUpgrade hands that decl to AddItem and then spends its rune cost (decl+0xC8).
    constexpr uint32_t kPowerSysDecl = 0x8, kPowerUpgradeArray = 0x88, kPowerUpgradeStride = 0x10;
    constexpr uint32_t kPowerUpgradeEnumDesc = 0x32CDEA8;
    //! Enhancements (Vitality, Agility, Strength, Bloodthirsty, Shadow Kill, bone charm crafting...): the same shape,
    //! array at decl+0x2F8, enum desc *(exe+0x32CDEB8)+0x10 (arkCpntPlayerPowerSystem::AddEnhancementUpgrade exe+0x455660).
    constexpr uint32_t kEnhancementEnumDesc = 0x32CDEB8, kEnhancementArray = 0x2F8;
    //! arkCpntInventory+0x168: the game's own "infinite ammo" counter (int; > 0 = on). The script node
    //! arkKisculeNodeInfiniteAmmo increments it on one pin (exe+0x7A0BC0) and decrements it on the other (exe+0x7A0B80).
    constexpr uint32_t kInvInfiniteAmmo = 0x168;
    //! arkCpntMana: current mana and the level it regenerates back to (floats). The second one drops with every cast,
    //! so it is not the maximum the bar started with.
    constexpr uint32_t kManaCurrent = 0x20, kManaMax = 0x2C;
    //! Equipment upgrades (the black market's): arkCpntPlayerPowerSystem+0x70 is an idList of upgrade instances of
    //! 0x28 bytes {?; info*; ...; bool owned @0x20; int variant @0x24}, count in the low 24 bits of +0x78. info+0 is the
    //! upgrade's inventory item, info+0x30 an alternative one used when info+0x38 is set and variant != 1.
    //! arkCpntPlayerPowerSystem::AddEquipmentUpgrade(ps, index, variant) exe+0x4557A0 adds that item and marks the
    //! entry owned (what arkCpntBlackMarketUpgrade calls with the shop entry's index<<2|variant).
    constexpr uint32_t kEquipUpgradeList = 0x70, kEquipUpgradeStride = 0x28;
    constexpr uint32_t kAddEquipUpgrade = 0x4557A0;

    void Tick();
    void GiveMark();
    //! Give every power whose item name contains `filter` ("all" = every models/powers/ item). A comma list gives them
    //! in that order ("blink,grapple,all"): the quick slots 6..0 take the first five given. Logs what it did.
    void GivePower(const std::string& filter);
    //! Give every power upgrade whose enum name contains `filter` (case-insensitive; "all" = every one, "list" = only
    //! log them), the way the journal does but without spending runes.
    void GiveUpgrade(const std::string& filter);
    //! Log the inventory's items whose decl name contains `filter` (empty = all), with the quantity owned.
    void ListItems(const std::string& filter);
    //! Research: every decl of the inventory's table (any class: items, equipment upgrades...) with the raw entry.
    void ListDecls(const std::string& filter);
    //! Give every equipment upgrade (weapons, gadgets, gear: the black market's) whose item name contains `filter`
    //! ("all" = every one, "list" = only log them), without spending coins.
    void GiveEquip(const std::string& filter);
    //! Give `count` of every inventory item whose name contains `filter` ("<filter>*<count>", "*max" = fill up).
    void GiveItem(const std::string& arg);
    //! The game's own infinite ammo (inventory+0x168), kept on for every player while 1.
    void SetInfiniteAmmo(int on);
    //! Ammo, gadgets and elixirs topped back up to the most held of each while 1.
    void SetInfiniteItems(int on);
    //! Everything for the player's character: the mark, every power, every power upgrade and enhancement, every
    //! equipment upgrade, the pistol and crossbow, the Heart, the spyglass, ammo, gadgets and elixirs filled up.
    void GiveAll();
    //! GiveAll for every new player (each load / level change, ~3 s in), plus infinite mana, ammo and items. For
    //! DVR_autoexec.txt: what is given lives in the session (and in any save made after it), a load forgets it.
    void SetAutoAll(int on);
    //! The parts of GiveAll, for the ini's cheats ([Cheats.Player] Unlock* / Fill*, 2026-11-01): given to every new
    //! player (~3 s into play) and once to the current one when the set changes. Nothing is taken back when one is off.
    enum Unlock : unsigned
    {
        kUnlockMark = 1u,              //!< the Outsider's mark
        kUnlockPowers = 2u,            //!< every power (and the mark they need)
        kUnlockPowerUpgrades = 4u,     //!< every power upgrade and enhancement, bone charm crafting...
        kUnlockEquipUpgrades = 8u,     //!< every black market upgrade (weapons, gadgets, gear)
        kUnlockWeapons = 16u,          //!< the pistol, the character's crossbow, the spyglass
        kUnlockAmmo = 32u,             //!< ammo and gadgets filled up
        kUnlockElixirs = 64u,          //!< health and mana elixirs filled up
        kUnlockAll = 127u
    };
    void SetAutoUnlock(unsigned mask);
    //! Mana: 1 = kept full every frame (infinite), 0 = the game's own.
    void SetInfiniteMana(int on);
    void SetInfiniteHealth(int on);   //!< vr hp inf|normal: the player's health kept full (ark_god_player does not hold)
    void SetHealth(float v);          //!< `vr hp <value>`: set the player's health once (testing; clamped to the max)
    void SetMana(float v);            //!< `vr mana <value>`: set the mana and its regen level once (testing)
    //! Give the mark and the powers matching `filter` to every new player (each load / level change), ~3 s after it
    //! appears. Empty or "off" = off. Meant for DVR_autoexec.txt: what is given lives in memory and a load forgets it.
    void SetAutoPowers(const std::string& filter);
    //! Research: resolve the player's component of that typeinfo on the next game tick, log its address and dump
    //! `len` bytes to <game>\DVR_cpnt_<tag>.txt (qwords with float/int views and what each pointer points at).
    //! depth 1 also dumps `childLen` bytes of each heap object the component points at. Requests queue up.
    void RequestDump(uint32_t typeInfoRva, uint32_t len, const std::string& tag, int depth = 0, uint32_t childLen = 0x100);
    void RequestDumpAt(uintptr_t addr, uint32_t len, const std::string& tag, int depth = 0, uint32_t childLen = 0x100);
    //! Last address resolved for that typeinfo (null until a dump or a feature resolved it).
    void* Component(uint32_t typeInfoRva);
    std::string Stats();
}
