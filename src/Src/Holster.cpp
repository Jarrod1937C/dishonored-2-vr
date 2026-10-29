#include "pch.h"
#include "Holster.h"
#include "GameInfo.h"
#include "Game.h"
#include "BodyHide.h"
#include <cmath>
#include <unordered_map>

namespace
{
    // arkCpntInventory (BodyHide::kEquipCpntTypeInfo): +0x58 an idList of pointers to the item entries that have an
    // equipment entity (count in the low 24 bits of +0x60), +0x68 the right hand's entry, +0x70 the left hand's. An
    // entry: +0x00 its arkDeclInventoryItem, +0x08 its equipment entity's id, +0x10 the item component (valid while that
    // entity exists: exe+0x63C890), +0x18 the equipment decl (+0x68 the hand: 0 right, 1 left; +0x11C the item type,
    // 1 = the sword), +0x20 the quantity.
    constexpr uint32_t kInvEquipList = 0x58, kInvEquipCount = 0x60, kInvRight = 0x68, kInvLeft = 0x70;
    constexpr uint32_t kEntryDecl = 0x00, kEntryEntId = 0x08, kEntryCpnt = 0x10, kEntryInfo = 0x18, kEntryCount = 0x20;
    constexpr uint32_t kInfoHand = 0x68, kInfoType = 0x11C;
    // entry* exe+0x614DD0(inventory, type, hand): the owned item of that type for that hand (what the script node
    // ActionPlayerInventory equips as "the sword": type 1, hand 0).
    constexpr uint32_t kInvFindEquip = 0x614DD0;
    // The entity messages: exe+0x8112C0(queue = exe+0x810240() + 0xC8E8, type, payload size, the player's id).
    // ARK_MSG_INVENTORY_EQUIP_ITEM 0x9D {arkDeclInventoryItem*, flags}, handled by the player's exe+0x7F8D60: flags bit 0 =
    // only into an empty hand, bit 1 = instant (EQUIPCHANGE_INSTANT_EQUIP), bit 2 = EQUIPCHANGE_THIS_HAND_ONLY (0x100 of
    // arkPlayerStateEquipChange's flags). Without bit 2 the change re-equips BOTH hands, the way the game always pairs
    // them (measured: drawing the pistol brought the sword back too) -- so every draw here sends kEquipThisHand.
    // ARK_MSG_INVENTORY_UNEQUIP_ITEM 0x9E (arkMsgInventoryUnequipItem: m_decl @0, m_slot @8, bool m_instant @0xC), handled
    // by exe+0x7F9140: with m_decl 0, m_slot 1 empties the left hand (player state 0x40), 4 a special slot, 0x10 nothing,
    // anything else the right hand (state 0x35).
    constexpr uint32_t kMsgSystem = 0x810240, kMsgPost = 0x8112C0, kMsgQueue = 0xC8E8, kMsgEquipItem = 0x9D, kMsgUnequipItem = 0x9E;
    constexpr uint32_t kEntId = 0xA8;
    constexpr uint64_t kEquipThisHand = 4;
    // int arkCpntInventory::GetQuantity(inventory, decl) (TestAids::kInvQuantity)
    constexpr uint32_t kInvQuantity = 0x615310;
    constexpr int kKinds = 4;
    bool LeftHand(int k) { return k != Holster::kSword; }

    std::atomic<bool> g_On{ true };
    std::atomic<int> g_Req{ -1 };
    std::atomic<bool> g_Owns[kKinds] = {}, g_InHand[kKinds] = {};
    const void* g_LastDecl[kKinds] = {};     // game thread: the item of that kind last seen in its hand
    double g_LastPost = -10, g_LastScan = -10;
    // the draw / holster sent, checked until it shows (or 2 s)
    struct Pending { int kind = -1; bool draw = true; double at = 0; const void* decl = nullptr; int slot = -1; } g_Pending;
    std::atomic<unsigned long long> g_Requests{ 0 }, g_Posts{ 0 }, g_Done{ 0 }, g_NotDone{ 0 }, g_Missing{ 0 };

    double Now() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart / (double)f.QuadPart; }
    bool SafeRead(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    std::string CStr(const char* s)
    {
        char buf[200]; size_t n = 0;
        for (; n < sizeof buf - 1; n++) { char c = 0; if (!SafeRead(&c, s + n, 1) || !c) break; buf[n] = c; }
        return std::string(buf, n);
    }
    std::string DeclName(const void* decl)
    {
        const char* p = nullptr;
        return decl && SafeRead(&p, (const uint8_t*)decl + 8, 8) && p ? CStr(p) : std::string("?");
    }
    void* GetComponent(void* entity, uint32_t tiRva)
    {
        using Fn = void* (*)(void*, void*, int);
        const uintptr_t base = GameInfo::Base();
        __try { return entity ? ((Fn)(base + BodyHide::kGetComponent))(entity, (void*)(base + tiRva), 2) : nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    const char* KindName(int k) { return k == Holster::kPistol ? "pistol" : k == Holster::kCrossbow ? "crossbow" : k == Holster::kMagic ? "power" : "sword"; }
    //! A power: its item is models/powers/<name>/... (the Heart, a story item held the same way, is not).
    std::unordered_map<const void*, bool> g_IsPower;     // game thread: decls live as long as the game
    bool IsPower(const void* decl)
    {
        if (!decl) return false;
        auto it = g_IsPower.find(decl);
        if (it != g_IsPower.end()) return it->second;
        const bool p = DeclName(decl).rfind("models/powers/", 0) == 0;
        g_IsPower[decl] = p;
        return p;
    }
    int Quantity(void* inv, const void* decl)
    {
        using Fn = int (*)(void*, const void*);
        __try { return ((Fn)(GameInfo::Base() + kInvQuantity))(inv, decl); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    const uint8_t* FindEquip(void* inv, int type, int hand)
    {
        using Fn = const uint8_t* (*)(void*, int, int);
        __try { return ((Fn)(GameInfo::Base() + kInvFindEquip))(inv, type, hand); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    struct Entry { const uint8_t* e; const void* decl; int kind; int count; int hand; int type; };
    struct Scan
    {
        std::vector<Entry> w;
        const uint8_t* left = nullptr; const void* leftDecl = nullptr; int leftKind = -1;
        const uint8_t* right = nullptr; const uint8_t* sword = nullptr; bool swordInHand = false;
        bool ok = false;
    };
    //! The left-hand weapons and the powers in the inventory's equipment list, the sword, and what the hands hold.
    Scan Read(uint8_t* inv)
    {
        Scan s;
        const uintptr_t base = GameInfo::Base();
        const uint8_t* list = nullptr; uint32_t n = 0;
        if (!SafeRead(&list, inv + kInvEquipList, 8) || !SafeRead(&n, inv + kInvEquipCount, 4) || !list) return s;
        n &= 0xFFFFFF;
        if (n > 512) return s;
        SafeRead(&s.left, inv + kInvLeft, 8);
        SafeRead(&s.right, inv + kInvRight, 8);
        if (s.left) SafeRead(&s.leftDecl, s.left + kEntryDecl, 8);
        if (IsPower(s.leftDecl)) s.leftKind = Holster::kMagic;
        s.sword = FindEquip(inv, 1, 0);
        s.swordInHand = s.sword && s.right == s.sword;
        for (uint32_t i = 0; i < n; i++)
        {
            const uint8_t* e = nullptr;
            if (!SafeRead(&e, list + 8ull * i, 8) || !e) continue;
            const void* decl = nullptr; int32_t id = 0; const uint8_t* cpnt = nullptr; const uint8_t* info = nullptr; int32_t count = 0;
            if (!SafeRead(&decl, e + kEntryDecl, 8) || !SafeRead(&id, e + kEntryEntId, 4) || !SafeRead(&cpnt, e + kEntryCpnt, 8)
                || !SafeRead(&info, e + kEntryInfo, 8) || !SafeRead(&count, e + kEntryCount, 4)) continue;
            if (!decl) continue;
            if (IsPower(decl)) { s.w.push_back({ e, decl, Holster::kMagic, count, -1, -1 }); continue; }
            if (!cpnt || !BodyHide::EntityById(id)) continue;   // the component is only valid while its entity is
            uintptr_t vt = 0; if (!SafeRead(&vt, cpnt, 8)) continue;
            const int kind = vt == base + BodyHide::kGunCpntVtable ? Holster::kPistol : vt == base + BodyHide::kCrossbowCpntVtable ? Holster::kCrossbow : -1;
            if (kind < 0) continue;
            int32_t hand = -1, type = -1;
            if (info) { SafeRead(&hand, info + kInfoHand, 4); SafeRead(&type, info + kInfoType, 4); }
            s.w.push_back({ e, decl, kind, count, hand, type });
            if (e == s.left) s.leftKind = kind;
        }
        s.ok = true;
        return s;
    }
    bool InHandNow(const Scan& s, int k) { return k == Holster::kSword ? s.swordInHand : s.leftKind == k; }
    uint8_t* Queue()
    {
        using FnSys = uint8_t* (*)();
        __try { return ((FnSys)(GameInfo::Base() + kMsgSystem))(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    bool PostEquip(uint32_t playerId, const void* decl, uint64_t flags)
    {
        using FnPost = uint64_t* (*)(void*, uint32_t, uint32_t, uint32_t);
        uint8_t* sys = Queue();
        if (!sys) return false;
        __try
        {
            uint64_t* pl = ((FnPost)(GameInfo::Base() + kMsgPost))(sys + kMsgQueue, kMsgEquipItem, 0x10, playerId);
            if (!pl) return false;
            pl[0] = (uint64_t)(uintptr_t)decl;
            pl[1] = flags;     // only the low byte is read
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool PostUnequip(uint32_t playerId, int slot)
    {
        using FnPost = uint8_t* (*)(void*, uint32_t, uint32_t, uint32_t);
        uint8_t* sys = Queue();
        if (!sys) return false;
        __try
        {
            uint8_t* pl = ((FnPost)(GameInfo::Base() + kMsgPost))(sys + kMsgQueue, kMsgUnequipItem, 0x10, playerId);
            if (!pl) return false;
            memset(pl, 0, 0x10);
            *(int32_t*)(pl + 8) = slot;     // m_slot; m_decl and m_instant stay 0 (animated), as the script node sends
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

void Holster::SetEnabled(bool on)
{
    g_On = on;
    LOGI("Holster: {}", on ? "on (left grip: pistol at the right hip, crossbow behind the back, last power on the chest; right grip: sword at the left hip; "
                             "the same again puts it away)" : "off");
}
bool Holster::Enabled() { return g_On.load(); }
bool Holster::Owns(int k) { return k >= 0 && k < kKinds && g_Owns[k].load(); }
bool Holster::InHand(int k) { return k >= 0 && k < kKinds && g_InHand[k].load(); }
void Holster::Request(int k) { if (k >= 0 && k < kKinds) { g_Req = k; g_Requests++; } }

void Holster::Tick()
{
    if (!g_On.load()) { g_Req = -1; return; }
    const double now = Now();
    const int req = g_Req.load();
    // The list is read a few times a second for Owns / InHand (the zones' buzz), and at once for a request.
    if (req < 0 && g_Pending.kind < 0 && now - g_LastScan < 0.25) return;
    g_LastScan = now;
    void* player = Game::LocalPlayer();
    uint8_t* inv = player ? (uint8_t*)GetComponent(player, BodyHide::kEquipCpntTypeInfo) : nullptr;
    if (!inv) { for (int k = 0; k < kKinds; k++) { g_Owns[k] = false; g_InHand[k] = false; } return; }
    const Scan s = Read(inv);
    if (!s.ok) return;
    if (s.leftKind >= 0 && s.leftDecl) g_LastDecl[s.leftKind] = s.leftDecl;   // the last pistol / crossbow / power held
    bool owns[kKinds] = {};
    for (const Entry& x : s.w) if (x.kind != kMagic || x.count > 0) owns[x.kind] = true;
    // the last power may not be in the equipment list: then its quantity says whether it is still had
    if (!owns[kMagic] && g_LastDecl[kMagic] && Quantity(inv, g_LastDecl[kMagic]) > 0) owns[kMagic] = true;
    owns[kSword] = s.sword != nullptr;
    for (int k = 0; k < kKinds; k++) { g_Owns[k] = owns[k]; g_InHand[k] = InHandNow(s, k); }
    uint32_t pid = 0; SafeRead(&pid, (uint8_t*)player + kEntId, 4);

    if (g_Pending.kind >= 0)
    {   // did it take? (the equip runs its animation: give it a moment)
        const int k = g_Pending.kind;
        const bool in = InHandNow(s, k) && (k != kMagic || s.leftDecl == g_Pending.decl);
        if (in == g_Pending.draw)
        {
            g_Done++;
            LOGI("Holster: {} {} ({:.2f} s{})", KindName(k), g_Pending.draw ? "drawn" : "put away", now - g_Pending.at,
                 g_Pending.draw ? "" : fmt::format(", m_slot {}", g_Pending.slot));
            g_Pending = Pending{};
        }
        else if (now - g_Pending.at > 2.0)
        {
            g_NotDone++;
            LOGI("Holster: {} NOT {} 2 s after the message{}", KindName(k), g_Pending.draw ? "drawn" : "put away",
                 g_Pending.draw ? fmt::format(" ({}): the left hand holds {}", DeclName(g_Pending.decl), s.leftKind >= 0 ? KindName(s.leftKind) : "something else")
                                : fmt::format(" (m_slot {})", g_Pending.slot));
            g_Pending = Pending{};
        }
    }
    if (req < 0) return;
    g_Req = -1;
    if (now - g_LastPost < 0.5 || !pid) return;

    if (InHandNow(s, req))
    {   // in the hand already: this holster puts it away
        const int slot = LeftHand(req) ? 1 : 0;
        if (!PostUnequip(pid, slot)) { LOGI("Holster: the unequip message for the {} could not be posted", KindName(req)); return; }
        g_Posts++; g_LastPost = now;
        g_Pending = Pending{ req, false, now, nullptr, slot };
        LOGI("Holster: putting the {} away ({} hand, m_slot {})", KindName(req), LeftHand(req) ? "left" : "right", slot);
        return;
    }
    if (req == kSword)
    {
        if (!s.sword) { g_Missing++; LOGI("Holster: no sword in the inventory: nothing drawn"); return; }
        const void* decl = nullptr; SafeRead(&decl, s.sword + kEntryDecl, 8);
        if (!decl || !PostEquip(pid, decl, kEquipThisHand)) { LOGI("Holster: the equip message for the sword could not be posted"); return; }
        g_Posts++; g_LastPost = now;
        g_Pending = Pending{ req, true, now, decl, -1 };
        LOGI("Holster: drawing the sword: {}", DeclName(decl));
        return;
    }
    // Which one: the one last held, else the first with a quantity, else any (a weapon: one entry per ammunition)
    const void* pick = nullptr;
    std::string all;
    for (const Entry& x : s.w)
    {
        if (x.kind != req) continue;
        all += fmt::format(" [{} x{} hand {} type {}]", DeclName(x.decl), x.count, x.hand, x.type);
        if (x.decl == g_LastDecl[req] && x.count > 0) pick = x.decl;
    }
    // the last power held, even when the equipment list does not have it, as long as it is still owned
    if (!pick && req == kMagic && g_LastDecl[req] && Quantity(inv, g_LastDecl[req]) > 0) pick = g_LastDecl[req];
    if (!pick) for (const Entry& x : s.w) if (x.kind == req && x.count > 0) { pick = x.decl; break; }
    if (!pick && req != kMagic) for (const Entry& x : s.w) if (x.kind == req) { pick = x.decl; break; }
    if (!pick) { g_Missing++; LOGI("Holster: no {} in the inventory: nothing drawn", KindName(req)); return; }
    if (!PostEquip(pid, pick, kEquipThisHand)) { LOGI("Holster: the equip message for the {} could not be posted", KindName(req)); return; }
    g_Posts++; g_LastPost = now;
    g_Pending = Pending{ req, true, now, pick, -1 };
    LOGI("Holster: drawing the {}: {}{} (the inventory has:{})", KindName(req), DeclName(pick),
         pick == g_LastDecl[req] ? " (the last one held)" : " (none held yet this session: the first one had)", all);
}

std::string Holster::Stats()
{
    return fmt::format("on {} owns pistol {} crossbow {} power {} sword {} left {} right {} requests {} posted {} done {} notdone {} missing {}",
                       (int)g_On.load(), (int)g_Owns[0].load(), (int)g_Owns[1].load(), (int)g_Owns[2].load(), (int)g_Owns[3].load(),
                       g_InHand[0].load() ? "pistol" : g_InHand[1].load() ? "crossbow" : g_InHand[2].load() ? "power" : "other",
                       g_InHand[3].load() ? "sword" : "other",
                       g_Requests.load(), g_Posts.load(), g_Done.load(), g_NotDone.load(), g_Missing.load());
}
