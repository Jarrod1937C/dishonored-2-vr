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
    // equipment entity (count in the low 24 bits of +0x60; what exe+0x614DD0 walks to find "the sword"), +0x70 the left
    // hand's entry. An entry: +0x00 its arkDeclInventoryItem, +0x08 its equipment entity's id, +0x10 the item component
    // (valid while that entity exists: exe+0x63C890), +0x18 the equipment decl (+0x68 the hand: 0 right, 1 left;
    // +0x11C the item type), +0x20 the quantity.
    constexpr uint32_t kInvEquipList = 0x58, kInvEquipCount = 0x60, kInvLeft = 0x70;
    constexpr uint32_t kEntryDecl = 0x00, kEntryEntId = 0x08, kEntryCpnt = 0x10, kEntryInfo = 0x18, kEntryCount = 0x20;
    constexpr uint32_t kInfoHand = 0x68, kInfoType = 0x11C;
    // ARK_MSG_INVENTORY_EQUIP_ITEM: exe+0x8112C0(queue = exe+0x810240() + 0xC8E8, 0x9D, 0x10, the player's id)
    constexpr uint32_t kMsgSystem = 0x810240, kMsgPost = 0x8112C0, kMsgQueue = 0xC8E8, kMsgEquipItem = 0x9D;
    constexpr uint32_t kEntId = 0xA8;
    // int arkCpntInventory::GetQuantity(inventory, decl) (TestAids::kInvQuantity)
    constexpr uint32_t kInvQuantity = 0x615310;
    constexpr int kKinds = 3;

    std::atomic<bool> g_On{ true };
    std::atomic<int> g_Req{ -1 };
    std::atomic<bool> g_Owns[kKinds] = {}, g_InHand[kKinds] = {};
    const void* g_LastDecl[kKinds] = {};     // game thread: the item of that kind last seen in the left hand
    double g_LastPost = -10, g_LastScan = -10;
    struct Pending { int kind = -1; double at = 0; const void* decl = nullptr; } g_Pending;
    std::atomic<unsigned long long> g_Requests{ 0 }, g_Posts{ 0 }, g_Drawn{ 0 }, g_NotDrawn{ 0 }, g_Missing{ 0 }, g_Already{ 0 };

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
    const char* KindName(int k) { return k == Holster::kPistol ? "pistol" : k == Holster::kCrossbow ? "crossbow" : "magic"; }
    //! A power ("magic"): its item is models/powers/<name>/... (the Heart, a story item held the same way, is not).
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

    struct Entry { const uint8_t* e; const void* decl; int kind; int count; int hand; int type; };
    struct Scan { std::vector<Entry> w; const uint8_t* left = nullptr; const void* leftDecl = nullptr; int leftKind = -1; bool ok = false; };
    //! The left-hand weapons and the powers in the inventory's equipment list, and what the left hand holds.
    Scan Read(uint8_t* inv)
    {
        Scan s;
        const uintptr_t base = GameInfo::Base();
        const uint8_t* list = nullptr; uint32_t n = 0;
        if (!SafeRead(&list, inv + kInvEquipList, 8) || !SafeRead(&n, inv + kInvEquipCount, 4) || !list) return s;
        n &= 0xFFFFFF;
        if (n > 512) return s;
        SafeRead(&s.left, inv + kInvLeft, 8);
        if (s.left) SafeRead(&s.leftDecl, s.left + kEntryDecl, 8);
        if (IsPower(s.leftDecl)) s.leftKind = Holster::kMagic;
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
    bool Post(uint32_t playerId, const void* decl)
    {
        using FnSys = uint8_t* (*)();
        using FnPost = uint64_t* (*)(void*, uint32_t, uint32_t, uint32_t);
        const uintptr_t base = GameInfo::Base();
        __try
        {
            uint8_t* sys = ((FnSys)(base + kMsgSystem))();
            if (!sys) return false;
            uint64_t* pl = ((FnPost)(base + kMsgPost))(sys + kMsgQueue, kMsgEquipItem, 0x10, playerId);
            if (!pl) return false;
            pl[0] = (uint64_t)(uintptr_t)decl;
            pl[1] = 0;      // flags: none (the script's left-hand equip clears its three bits)
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

void Holster::SetEnabled(bool on) { g_On = on; LOGI("Holster: {}", on ? "on (left hand: pistol at the left hip, crossbow behind the back, with the left trigger)" : "off"); }
bool Holster::Enabled() { return g_On.load(); }
bool Holster::Owns(int k) { return k >= 0 && k < kKinds && g_Owns[k].load(); }
bool Holster::InHand(int k) { return k >= 0 && k < kKinds && g_InHand[k].load(); }
void Holster::Request(int k) { if (k >= 0 && k < kKinds) { g_Req = k; g_Requests++; } }

void Holster::Tick()
{
    if (!g_On.load()) { g_Req = -1; return; }
    const double now = Now();
    const int req = g_Req.load();
    // The list is read a few times a second for Owns / InHand (the zone's buzz), and at once for a draw.
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
    for (int k = 0; k < kKinds; k++) { g_Owns[k] = owns[k]; g_InHand[k] = s.leftKind == k; }

    if (g_Pending.kind >= 0)
    {   // did the draw take? (the equip runs its animation: give it a moment)
        if (s.leftKind == g_Pending.kind && (g_Pending.kind != kMagic || s.leftDecl == g_Pending.decl)) { g_Drawn++; LOGI("Holster: {} drawn ({:.2f} s)", KindName(g_Pending.kind), now - g_Pending.at); g_Pending = Pending{}; }
        else if (now - g_Pending.at > 2.0) { g_NotDrawn++; LOGI("Holster: {} NOT drawn 2 s after the equip message ({}): the left hand still holds {}", KindName(g_Pending.kind), DeclName(g_Pending.decl), s.leftKind >= 0 ? KindName(s.leftKind) : "something else"); g_Pending = Pending{}; }
    }
    if (req < 0) return;
    g_Req = -1;
    if (now - g_LastPost < 0.5) return;
    if (s.leftKind == req) { g_Already++; LOGI("Holster: the {} is already in the left hand", KindName(req)); return; }
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
    uint32_t pid = 0; SafeRead(&pid, (uint8_t*)player + kEntId, 4);
    if (!pid || !Post(pid, pick)) { LOGI("Holster: the equip message for the {} could not be posted", KindName(req)); return; }
    g_Posts++; g_LastPost = now;
    g_Pending = Pending{ req, now, pick };
    LOGI("Holster: drawing the {}: {}{} (the inventory has:{})", KindName(req), DeclName(pick),
         pick == g_LastDecl[req] ? " (the last one held)" : " (none held yet this session: the first one had)", all);
}

std::string Holster::Stats()
{
    return fmt::format("on {} owns pistol {} crossbow {} magic {} left {} requests {} posted {} drawn {} notdrawn {} missing {} already {}",
                       (int)g_On.load(), (int)g_Owns[0].load(), (int)g_Owns[1].load(), (int)g_Owns[2].load(),
                       g_InHand[0].load() ? "pistol" : g_InHand[1].load() ? "crossbow" : g_InHand[2].load() ? "magic" : "other",
                       g_Requests.load(), g_Posts.load(), g_Drawn.load(), g_NotDrawn.load(), g_Missing.load(), g_Already.load());
}
