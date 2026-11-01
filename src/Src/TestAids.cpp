#include "pch.h"
#include "TestAids.h"
#include "GameInfo.h"
#include "Game.h"
#include "BodyHide.h"
#include "GpuTrace.h"
#include <map>
#include <deque>
#include <set>

namespace
{
    std::mutex g_M;
    //! A research dump: the player's component of typeinfo `ti`, or (ti == 0) the object at `addr`. `depth` 1 also
    //! dumps `childLen` bytes of every heap object the first level points at.
    struct Req { uint32_t ti = 0; uintptr_t addr = 0; uint32_t len = 0, childLen = 0; int depth = 0; std::string tag; };
    std::deque<Req> g_Reqs;
    std::map<uint32_t, void*> g_Found;
    //! Actions for the game thread: give the mark, give powers, list items.
    struct Action { int kind; std::string arg; };   // 0 mark, 1 power, 2 list, 3 power upgrade
    std::deque<Action> g_Actions;
    std::atomic<int> g_InfMana{ 0 }, g_InfAmmo{ 0 }, g_InfHealth{ 0 };
    std::atomic<unsigned long long> g_HealthRefills{ 0 };
    std::atomic<int> g_AmmoCounter{ -1 };     // last value seen at inventory+0x168
    std::atomic<unsigned long long> g_ManaRefills{ 0 }, g_PowersGiven{ 0 };
    std::atomic<float> g_ManaLast{ -1.f }, g_ManaMaxLast{ -1.f };
    std::string g_AutoPowers;                 // under g_M; empty = off
    bool g_AutoAll = false;                   // under g_M: giveall for every new player
    unsigned g_AutoMask = 0;                  // under g_M: the ini's parts of giveall for every new player (TestAids::Unlock)
    void* g_AutoPlayer = nullptr; int g_AutoTicks = 0; unsigned g_AutoRuns = 0;

    bool Readable(const void* p, size_t n)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if ((uintptr_t)p < 0x10000 || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    //! How many bytes from `p` (at most `max`) can be read: one VirtualQuery per region, not one per byte (the names
    //! were probed byte by byte, tens of thousands of kernel calls per item table read).
    size_t ReadableSpan(const void* p, size_t max)
    {
        size_t got = 0;
        while (got < max)
        {
            const uint8_t* q = (const uint8_t*)p + got;
            MEMORY_BASIC_INFORMATION mbi{};
            if ((uintptr_t)q < 0x10000 || !VirtualQuery(q, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
            if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) break;
            got += (size_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - (uintptr_t)q);
        }
        return got < max ? got : max;
    }
    void* GetComponent(void* entity, uint32_t tiRva)
    {
        using Fn = void* (*)(void*, void*, int);
        const uintptr_t base = GameInfo::Base();
        return entity ? ((Fn)(base + BodyHide::kGetComponent))(entity, (void*)(base + tiRva), 2) : nullptr;
    }
    bool InExe(uintptr_t v) { const uintptr_t base = GameInfo::Base(); return v >= base && v < base + 0x8000000; }
    //! MSVC RTTI class name of an object whose first qword is a vtable in the exe, or "".
    std::string ClassOf(const void* obj)
    {
        const uintptr_t base = GameInfo::Base();
        if (!Readable(obj, 8)) return {};
        const uintptr_t vt = *(const uintptr_t*)obj;
        if (!InExe(vt) || !Readable((void*)(vt - 8), 8)) return {};
        const uint8_t* col = *(const uint8_t* const*)(vt - 8);
        if (!Readable(col, 24) || *(const uint32_t*)col != 1) return {};
        const char* name = (const char*)(base + *(const uint32_t*)(col + 12) + 0x10);
        if (!Readable(name, 4) || name[0] != '.' || name[1] != '?') return {};
        const size_t span = ReadableSpan(name, 100);
        std::string s; for (size_t i = 0; i < span && name[i]; i++) s += name[i];
        return s;
    }
    std::string CStr(uintptr_t v)
    {
        const char* s = (const char*)v;
        const size_t span = ReadableSpan(s, 241);
        if (span < 8) return {};
        size_t n = 0;
        while (n < 240 && n < span && s[n] >= 32 && s[n] < 127) n++;
        if (n >= 3 && n < span && s[n] == 0) return std::string(s, n);
        return {};
    }
    std::string Describe(uintptr_t v)
    {
        const uintptr_t base = GameInfo::Base();
        if (InExe(v)) return fmt::format("exe+0x{:X}", v - base);
        if (!Readable((void*)v, 16)) return {};
        std::string cls = ClassOf((void*)v);
        if (!cls.empty()) return "-> " + cls;
        std::string s = CStr(v);
        if (!s.empty()) return fmt::format("-> \"{}\"", s);
        // an idStr-like {char* data; ...} or a pointer to a pointer to a string
        uintptr_t inner = *(const uintptr_t*)v;
        std::string s2 = CStr(inner);
        if (!s2.empty()) return fmt::format("-> ptr -> \"{}\"", s2);
        std::string c2 = Readable((void*)inner, 8) ? ClassOf((void*)inner) : std::string();
        if (!c2.empty()) return "-> ptr -> " + c2;
        return "-> heap";
    }
    void DumpBlock(FILE* f, const uint8_t* p, uint32_t len, const char* indent, std::vector<uintptr_t>* children)
    {
        for (uint32_t off = 0; off + 8 <= len; off += 8)
        {
            if (!Readable(p + off, 8)) { fprintf(f, "%s+%04X unreadable\n", indent, off); break; }
            const uint64_t q = *(const uint64_t*)(p + off);
            if (q == 0) continue;                                       // zero qwords are left out
            const float* fl = (const float*)(p + off); const int32_t* in = (const int32_t*)(p + off);
            const std::string d = Describe((uintptr_t)q);
            fprintf(f, "%s+%04X %016llX  f %12.4g %12.4g  i %11d %11d  %s\n", indent, off, (unsigned long long)q, fl[0], fl[1], in[0], in[1], d.c_str());
            if (children && d.rfind("-> ", 0) == 0 && !InExe(q)) children->push_back((uintptr_t)q);
        }
    }
    void Dump(const void* c, const Req& r)
    {
        const std::string name = fmt::format("DVR_cpnt_{}.txt", r.tag);
        FILE* f = _wfopen((GameInfo::GameDir() + L"\\" + std::wstring(name.begin(), name.end())).c_str(), L"w");
        if (!f) { LOGI("cpnt: cannot write {}", name); return; }
        fprintf(f, "object %p class %s (typeinfo exe+0x%X), %u bytes, zero qwords omitted\n", c, ClassOf(c).c_str(), r.ti, r.len);
        std::vector<uintptr_t> kids;
        DumpBlock(f, (const uint8_t*)c, r.len, "", r.depth > 0 ? &kids : nullptr);
        std::set<uintptr_t> done;
        for (uintptr_t k : kids)
        {
            if (!done.insert(k).second) continue;
            fprintf(f, "\n  --- child %p %s\n", (void*)k, ClassOf((void*)k).c_str());
            DumpBlock(f, (const uint8_t*)k, r.childLen ? r.childLen : 0x100, "    ", nullptr);
        }
        fclose(f);
        LOGI("cpnt: {} ({}), {} bytes + {} children to {}", c, ClassOf(c), r.len, done.size(), name);
    }
}

namespace
{
    std::string DeclName(const void* decl)
    {
        if (!Readable(decl, 0x10)) return {};
        return CStr(*(const uintptr_t*)((const uint8_t*)decl + 8));
    }
    //! Calls `fn(decl, name)` for every entry of the inventory's item table whose decl is an arkDeclInventoryItem.
    //! Returns false (and logs why) when the table does not look like one -- the layout came from one exe version.
    // The table read once and kept: reading it means the RTTI class and the name of every one of its ~800 decls, each
    // probed byte by byte for readability, and that took ~1.4 s of the game's main thread -- per call. `vr autoall`
    // calls it 8 times on every new player, so every load froze the game for ~12 s a few seconds in (measured in the
    // mock, 2026-10-15: the train to Addermire stopped for 12.9 s mid-ride; on the headset, the Quest's
    // hourglass). The decls live as long as the game; the cache is checked against the table's
    // base, count and its first and last entries.
    struct ItemEntry { const void* decl; bool item; std::string name; };
    struct ItemCache { uint8_t* base = nullptr; int n = 0; const void* first = nullptr; const void* last = nullptr; std::vector<ItemEntry> e; int items = 0; };
    ItemCache g_ItemCache;
    unsigned long long g_ItemCacheBuilds = 0, g_ItemCacheHits = 0;
    template <class F> bool ForEachItem(uint8_t* inv, F fn, bool everyDecl = false)
    {
        if (!Readable(inv + TestAids::kInvItemCount, 4)) return false;
        uint8_t* base = *(uint8_t**)(inv + TestAids::kInvItems);
        const int n = *(const int*)(inv + TestAids::kInvItemCount) & 0xFFFFFF;
        if (n <= 0 || n > 20000 || !Readable(base, (size_t)n * TestAids::kInvItemStride))
        {
            LOGI("TestAids: the inventory item table does not look right (base {} count {})", (void*)base, n);
            return false;
        }
        const void* first = *(void**)base; const void* last = *(void**)(base + (size_t)(n - 1) * TestAids::kInvItemStride);
        ItemCache& c = g_ItemCache;
        if (c.base != base || c.n != n || c.first != first || c.last != last)
        {
            // The table holds more than inventory items (equipment upgrades too, measured): every entry must be SOME
            // decl or the layout is wrong; only arkDeclInventoryItem entries are handed on (unless everyDecl).
            ItemCache fresh; fresh.base = base; fresh.n = n; fresh.first = first; fresh.last = last;
            int bad = 0;
            for (int i = 0; i < n; i++)
            {
                const void* decl = *(void**)(base + (size_t)i * TestAids::kInvItemStride);
                if (!decl) continue;
                const std::string cls = ClassOf(decl);
                if (cls.rfind(".?AVarkDecl", 0) != 0)
                {
                    if (++bad > 3) { LOGI("TestAids: entry {} of {} is not a decl ({}): layout mismatch, stopping", i, n, cls); return false; }
                    continue;
                }
                const bool item = cls == ".?AVarkDeclInventoryItem@@";
                fresh.items += item ? 1 : 0;
                fresh.e.push_back({ decl, item, DeclName(decl) });
            }
            c = std::move(fresh);
            g_ItemCacheBuilds++;
            LOGI("TestAids: item table {} entries, {} inventory items (read once, kept)", n, c.items);
        }
        else g_ItemCacheHits++;
        for (const ItemEntry& x : c.e) if (everyDecl || x.item) fn(x.decl, x.name);
        return true;
    }
    int Quantity(void* inv, const void* decl)
    {
        using Fn = int (*)(void*, const void*);
        return ((Fn)(GameInfo::Base() + TestAids::kInvQuantity))(inv, decl);
    }
    void DoAction(const Action& a)
    {
        void* player = Game::LocalPlayer();
        uint8_t* inv = (uint8_t*)GetComponent(player, BodyHide::kEquipCpntTypeInfo);
        if (!inv) { LOGI("TestAids: no player inventory (player {})", player); return; }
        const uintptr_t base = GameInfo::Base();
        if (a.kind == 0)
        {
            void* mark = GetComponent(player, TestAids::kOutsiderMarkCpntTypeInfo);
            if (!mark) { LOGI("TestAids: the player has no Outsider's mark component"); return; }
            using HasFn = bool (*)(void*);
            if (((HasFn)(base + TestAids::kHasMark))(mark)) { LOGI("TestAids: the player already has the mark"); return; }
            using Fn = void (*)(void*);
            ((Fn)(base + TestAids::kGiveMark))(mark);
            LOGI("TestAids: GiveMark({}) called, has the mark now: {}", mark, ((HasFn)(base + TestAids::kHasMark))(mark));
            return;
        }
        if (a.kind == 1)
        {
            using Fn = void (*)(void*, const void*, int);
            int given = 0, had = 0;
            // "a,b,c" gives them in THAT order, then (with a trailing ",all") the rest: the quick slots 6..0 go to the
            // first five given, so the order picks what the number keys equip.
            std::vector<std::string> parts;
            for (size_t s = 0; s <= a.arg.size();) { size_t e = a.arg.find(',', s); if (e == std::string::npos) e = a.arg.size(); if (e > s) parts.push_back(a.arg.substr(s, e - s)); s = e + 1; }
            for (const std::string& part : parts)
                ForEachItem(inv, [&](const void* decl, const std::string& name) {
                    if (name.find("models/powers/") != 0) return;
                    if (part != "all" && name.find(part) == std::string::npos) return;
                    const int q = Quantity(inv, decl);
                    if (q > 0) { if (parts.size() == 1) { had++; LOGI("TestAids: already have {} (x{})", name, q); } return; }
                    ((Fn)(base + TestAids::kInvAddItem))(inv, decl, 1);
                    given++; g_PowersGiven++;
                    LOGI("TestAids: gave {} -> quantity now {}", name, Quantity(inv, decl));
                });
            LOGI("TestAids: givepower '{}': {} given, {} already owned", a.arg, given, had);
            return;
        }
        if (a.kind == 3)
        {
            uint8_t* ps = (uint8_t*)GetComponent(player, TestAids::kPowerSystemCpntTypeInfo);
            if (!ps || !Readable(ps + TestAids::kPowerSysDecl, 8)) { LOGI("TestAids: the player has no power system"); return; }
            uint8_t* decl = *(uint8_t**)(ps + TestAids::kPowerSysDecl);
            auto lower = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
            const std::string f = lower(a.arg);
            using Fn = void (*)(void*, const void*, int);
            int given = 0, had = 0, n = 0;
            // Two tables with the same shape: the powers' upgrades and the "enhancements" (Vitality, Agility, Strength,
            // Bloodthirsty, Shadow Kill, bone charm crafting...), what AddPowerUpgrade / AddEnhancementUpgrade give.
            const struct { uint32_t desc, array; const char* what; } tables[] = {
                { TestAids::kPowerUpgradeEnumDesc, TestAids::kPowerUpgradeArray, "power" },
                { TestAids::kEnhancementEnumDesc, TestAids::kEnhancementArray, "enhancement" } };
            for (const auto& t : tables)
            {
                const uint8_t* desc = *(const uint8_t* const*)(base + t.desc);
                if (!Readable(decl, t.array + 64 * TestAids::kPowerUpgradeStride) || !Readable(desc, 0x18)) { LOGI("TestAids: power system decl {} / {} enum desc {} unreadable", (void*)decl, t.what, (void*)desc); continue; }
                const uint8_t* ents = *(const uint8_t* const*)(desc + 0x10);
                for (int i = 0; i < 64 && Readable(ents + (size_t)i * 16, 16); i++)
                {
                    const std::string name = CStr(*(const uintptr_t*)(ents + (size_t)i * 16));
                    if (name.empty()) break;
                    const int64_t value = *(const int64_t*)(ents + (size_t)i * 16 + 8);
                    const uint8_t* e = decl + t.array + (size_t)i * TestAids::kPowerUpgradeStride;
                    const int id = *(const int*)e; const void* ud = *(void* const*)(e + 8);
                    n++;
                    const bool match = f == "all" || f == "list" || lower(name).find(f) != std::string::npos;
                    if (!match) continue;
                    const std::string cls = ud ? ClassOf(ud) : std::string();
                    const int q = ud && cls.rfind(".?AVarkDecl", 0) == 0 ? Quantity(inv, ud) : -1;
                    LOGI("TestAids: {} upgrade {} {} (value {}, entry id {}) decl {} {} {} x{}", t.what, i, name, value, id, ud, cls, ud ? DeclName(ud) : std::string(), q);
                    if (f == "list" || !ud || q < 0) continue;
                    if (q > 0) { had++; continue; }
                    ((Fn)(base + TestAids::kInvAddItem))(inv, ud, 1);
                    given++;
                    LOGI("TestAids: gave {} upgrade {} -> quantity now {}", t.what, name, Quantity(inv, ud));
                }
            }
            LOGI("TestAids: giveupgrade '{}': {} upgrades known, {} given, {} already owned", a.arg, n, given, had);
            return;
        }
        if (a.kind == 7 || a.kind == 8)
        {   // giveall: everything a player can own, for this character -- or (8) the parts of it in the mask (arg)
            const unsigned mask = a.kind == 7 ? TestAids::kUnlockAll : (unsigned)std::strtoul(a.arg.c_str(), nullptr, 10);
            std::string who = "corvo";
            if (uint8_t* ps = (uint8_t*)GetComponent(player, TestAids::kPowerSystemCpntTypeInfo); ps && Readable(ps + TestAids::kEquipUpgradeList, 0x10))
            {   // the black market's tree names its character-specific upgrades *_corvo / *_emily
                uint8_t* list = *(uint8_t**)(ps + TestAids::kEquipUpgradeList);
                const int n = *(const int*)(ps + TestAids::kEquipUpgradeList + 8) & 0xFFFFFF;
                int corvo = 0, emily = 0;
                for (int i = 0; i < n && i < 512 && Readable(list + (size_t)i * TestAids::kEquipUpgradeStride, TestAids::kEquipUpgradeStride); i++)
                {
                    const uint8_t* x = *(const uint8_t* const*)(list + (size_t)i * TestAids::kEquipUpgradeStride + 8);
                    if (!Readable(x, 0x40)) continue;
                    for (const void* d : { *(const void* const*)x, *(const void* const*)(x + 0x30) })
                        if (d) { const std::string nm = DeclName(d); corvo += nm.find("_corvo") != std::string::npos; emily += nm.find("_emily") != std::string::npos; }
                }
                if (emily > corvo) who = "emily";
            }
            LOGI("TestAids: give{} for {}", a.kind == 7 ? "all" : fmt::format(" parts {:#x}", mask), who);
            const struct { unsigned bit; Action act; } subs[] = {
                { TestAids::kUnlockMark | TestAids::kUnlockPowers, { 0, {} } }, { TestAids::kUnlockPowers, { 1, "all" } },
                { TestAids::kUnlockPowerUpgrades, { 3, "all" } }, { TestAids::kUnlockEquipUpgrades, { 5, "all" } },
                { TestAids::kUnlockWeapons, { 6, "models/weapons/gun_player/" } }, { TestAids::kUnlockWeapons, { 6, "models/weapons/crossbow/crossbow_" + who + "." } },
                // NOT the Heart: it is a story item the Outsider hands over in the Void at the start of the game, and
                // a player who already holds one cannot take it there (a tester was stuck in front of it, 2026-10-19).
                { TestAids::kUnlockWeapons, { 6, who == "corvo" ? "models/items/spyglass/spyglass_corvo." : "models/items/spyglass/spyglass.inventoryitem" } },
                { TestAids::kUnlockAmmo, { 6, "components/equipment/ammo/*max" } }, { TestAids::kUnlockAmmo, { 6, "components/equipment/gadgets/*max" } },
                { TestAids::kUnlockElixirs, { 6, "components/interactive/health_potion.*max" } }, { TestAids::kUnlockElixirs, { 6, "components/interactive/mana_potion.*max" } } };
            for (const auto& sub : subs) if (mask & sub.bit) DoAction(sub.act);
            return;
        }
        if (a.kind == 6)
        {   // giveitem <filter>[*count]: any inventory item, `count` of it (default 1); "<filter>*max" fills it up
            std::string f = a.arg; int count = 1; bool fill = false;
            const size_t star = f.find('*');
            if (star != std::string::npos) { const std::string c = f.substr(star + 1); f = f.substr(0, star); if (c == "max") fill = true; else count = std::atoi(c.c_str()); }
            using Fn = int (*)(void*, const void*, int);
            int given = 0;
            ForEachItem(inv, [&](const void* decl, const std::string& name) {
                if (f.empty() || name.find(f) == std::string::npos) return;
                const int q0 = Quantity(inv, decl);
                const int r = ((Fn)(base + TestAids::kInvAddItem))(inv, decl, fill ? 9999 : count);
                given++;
                LOGI("TestAids: giveitem {} x{} -> AddItem {} -> quantity now {} (was {})", name, fill ? 9999 : count, r, Quantity(inv, decl), q0);
            });
            LOGI("TestAids: giveitem '{}': {} items", a.arg, given);
            return;
        }
        if (a.kind == 5)
        {   // equipment upgrades (the black market's): the power system's list of upgrade instances
            uint8_t* ps = (uint8_t*)GetComponent(player, TestAids::kPowerSystemCpntTypeInfo);
            if (!ps || !Readable(ps + TestAids::kEquipUpgradeList, 0x10)) { LOGI("TestAids: the player has no power system"); return; }
            uint8_t* list = *(uint8_t**)(ps + TestAids::kEquipUpgradeList);
            const int n = *(const int*)(ps + TestAids::kEquipUpgradeList + 8) & 0xFFFFFF;
            if (n <= 0 || n > 512 || !Readable(list, (size_t)n * TestAids::kEquipUpgradeStride)) { LOGI("TestAids: equipment upgrade list {} x{} unreadable", (void*)list, n); return; }
            // "<filter>[:variant]": the paired upgrades (one of two: armour or camouflage, a sniper or a shotgun pistol...)
            // take their first choice (variant 1) unless a variant is given; 2 = the alternative.
            std::string f = a.arg; int variant = -1;
            const size_t colon = f.find(':');
            if (colon != std::string::npos) { variant = std::atoi(f.c_str() + colon + 1); f = f.substr(0, colon); }
            int given = 0, had = 0;
            for (int i = 0; i < n; i++)
            {
                const uint8_t* e = list + (size_t)i * TestAids::kEquipUpgradeStride;
                const uint8_t* x = *(const uint8_t* const*)(e + 8);
                if (!Readable(x, 0x40)) { LOGI("TestAids: equip {} info {} unreadable", i, (void*)x); continue; }
                const void* it0 = *(void* const*)x; const void* it1 = *(void* const*)(x + 0x30);
                LOGI("TestAids: equip {} owned {} variant {} | {} x{} | alt {} {} x{} | e {:X} {:X} {:X}", i, e[0x20], *(const int*)(e + 0x24), DeclName(it0),
                     it0 ? Quantity(inv, it0) : -1, x[0x38], it1 ? DeclName(it1) : std::string("-"), it1 ? Quantity(inv, it1) : -1,
                     *(const uint64_t*)e, *(const uint64_t*)(e + 0x10), *(const uint64_t*)(e + 0x18));
                if (f == "list" || (f != "all" && DeclName(it0).find(f) == std::string::npos && (!it1 || DeclName(it1).find(f) == std::string::npos))) continue;
                if (!it0) continue;                  // a category node of the tree, nothing to own
                if (e[0x20]) { had++; continue; }
                const int v = variant >= 0 ? variant : x[0x38] ? 1 : 0;
                using Fn = const void* (*)(void*, int, int);
                const void* r = ((Fn)(base + TestAids::kAddEquipUpgrade))(ps, i, v);
                given++;
                LOGI("TestAids: gave equip {} variant {} -> {} {} owned {} x{}", i, v, r, DeclName(r), e[0x20], r ? Quantity(inv, r) : -1);
            }
            LOGI("TestAids: giveequip '{}': {} equipment upgrades, {} given, {} already owned", a.arg, n, given, had);
            return;
        }
        int shown = 0;
        if (a.kind == 4)
        {   // research: every decl of the table, any class, with the entry's raw words
            uint8_t* tb = *(uint8_t**)(inv + TestAids::kInvItems);
            ForEachItem(inv, [&](const void* decl, const std::string& name) {
                if (!a.arg.empty() && name.find(a.arg) == std::string::npos) return;
                const uint8_t* e = tb; const int n = *(const int*)(inv + TestAids::kInvItemCount) & 0xFFFFFF;
                for (int i = 0; i < n; i++, e += TestAids::kInvItemStride) if (*(void* const*)e == decl) break;
                const uint32_t* w = (const uint32_t*)e;
                if (shown++ < 1000)
                    LOGI("TestAids: decl {} {} x{} ({}) raw {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", ClassOf(decl).substr(4), name,
                         Quantity(inv, decl), decl, w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13]);
            }, true);
            LOGI("TestAids: {} decls matched '{}'", shown, a.arg);
            return;
        }
        ForEachItem(inv, [&](const void* decl, const std::string& name) {
            if (!a.arg.empty() && name.find(a.arg) == std::string::npos) return;
            if (shown++ < 400) LOGI("TestAids: item {} x{} ({})", name, Quantity(inv, decl), decl);
        });
        LOGI("TestAids: {} items matched '{}'", shown, a.arg);
    }
    //! Infinite ammo: the game's own counter (a script node raises and lowers it), raised once per inventory while on
    //! and our raise taken back when switched off. A new inventory (load, level change) starts from its own value.
    void TickAmmo()
    {
        uint8_t* inv = (uint8_t*)GetComponent(Game::LocalPlayer(), BodyHide::kEquipCpntTypeInfo);
        if (!inv || !Readable(inv + TestAids::kInvInfiniteAmmo, 4)) return;
        int* c = (int*)(inv + TestAids::kInvInfiniteAmmo);
        static void* raisedOn = nullptr;
        if (g_InfAmmo.load() && raisedOn != inv) { (*c)++; raisedOn = inv; LOGI("TestAids: infinite ammo raised on inventory {} -> {}", (void*)inv, *c); }
        else if (!g_InfAmmo.load() && raisedOn == inv) { if (*c > 0) (*c)--; raisedOn = nullptr; LOGI("TestAids: infinite ammo lowered on inventory {} -> {}", (void*)inv, *c); }
        else if (!g_InfAmmo.load()) raisedOn = nullptr;
        g_AmmoCounter = *c;
    }
    //! Infinite consumables: ammo, gadgets (grenades, mines, springrazors) and elixirs are topped back up to the most
    //! the player has held of each since the switch went on (or since this inventory appeared). Items never held are
    //! left alone: `giveitem <filter>*max` fills them first. Twice a second, after the actions.
    std::atomic<int> g_InfItems{ 0 };
    std::atomic<unsigned long long> g_ItemRefills{ 0 };
    bool IsConsumable(const std::string& n)
    {
        return n.rfind("components/equipment/ammo/", 0) == 0 || n.rfind("components/equipment/gadgets/", 0) == 0
            || n == "components/interactive/health_potion.inventoryitem" || n == "components/interactive/mana_potion.inventoryitem";
    }
    void TickItems()
    {
        // The target is the CAPACITY, found by asking AddItem for far more than fits (it adds only what fits, and the
        // capacity grows with the storage upgrades): once when an item is first seen held, again every ~10 s.
        static void* forInv = nullptr; static std::map<const void*, int> cap; static std::vector<std::pair<const void*, std::string>> decls;
        static int ticks = 0, checks = 0;
        if (!g_InfItems.load()) { forInv = nullptr; return; }
        if (++ticks % 30) return;
        uint8_t* inv = (uint8_t*)GetComponent(Game::LocalPlayer(), BodyHide::kEquipCpntTypeInfo);
        if (!inv) return;
        if (inv != forInv)
        {   // a new inventory (load, level change): find its consumables once
            forInv = inv; cap.clear(); decls.clear();
            ForEachItem(inv, [&](const void* decl, const std::string& name) { if (IsConsumable(name)) decls.push_back({ decl, name }); });
            LOGI("TestAids: infinite items on inventory {}: {} consumables", (void*)inv, decls.size());
        }
        const bool probeAll = ++checks % 20 == 0;
        using Fn = int (*)(void*, const void*, int);
        for (const auto& [decl, name] : decls)
        {
            const int q = Quantity(inv, decl);
            auto it = cap.find(decl);
            if (it == cap.end() && q <= 0) continue;      // never held: not ours to hand out (giveitem/giveall do that)
            const bool fresh = it == cap.end();
            int& c = cap[decl];
            if (q > c) c = q;
            if (q >= c && !fresh && !probeAll) continue;
            ((Fn)(GameInfo::Base() + TestAids::kInvAddItem))(inv, decl, 9999);
            const int q2 = Quantity(inv, decl);
            if (q2 > c) c = q2;
            if (q2 > q)
            {
                g_ItemRefills++;
                if (g_ItemRefills < 200) LOGI("TestAids: refilled {} {} -> {}", name, q, q2);
            }
        }
    }
    // Infinite health (vr hp inf): `ark_god_player 1` does not keep Corvo alive against a guard's sword in the prologue
    // (measured twice: Game Over ~35 s into a fight with it at 1). The player's arkCpntHealthEvolved keeps the current
    // health at +0x20 and its maximum at +0x114 (both 56 on Corvo, easy, measured 2026-10-05).
    constexpr uint32_t kHealthCpntTypeInfo = 0x39BA160, kHealthCurrent = 0x20, kHealthMax = 0x114;
    std::atomic<float> g_SetMana{ -1.f };   //!< `vr mana <value>`: set once
    std::atomic<float> g_SetHealth{ -1.f }, g_HealthLast{ -1.f }, g_HealthMaxLast{ -1.f };   //!< `vr hp <value>`: set once
    void TickHealth()
    {
        void* h = GetComponent(Game::LocalPlayer(), kHealthCpntTypeInfo);
        if (!h || !Readable((uint8_t*)h + kHealthMax, 4)) return;
        float* cur = (float*)((uint8_t*)h + kHealthCurrent);
        const float mx = *(float*)((uint8_t*)h + kHealthMax);
        if (!(mx > 0.f && mx < 10000.f) || !(*cur < 10000.f)) return;
        const float set = g_SetHealth.exchange(-1.f);
        if (set >= 0.f) { LOGI("TestAids: health {:.1f} -> {:.1f} (max {:.1f})", *cur, std::min(set, mx), mx); *cur = std::min(set, mx); }
        g_HealthLast = *cur; g_HealthMaxLast = mx;
        if (!g_InfHealth.load()) return;
        if (*cur < mx) { *cur = mx; g_HealthRefills++; }
    }
    void TickMana()
    {
        void* m = GetComponent(Game::LocalPlayer(), TestAids::kManaCpntTypeInfo);
        if (!m || !Readable((uint8_t*)m + TestAids::kManaMax, 4)) return;
        float* cur = (float*)((uint8_t*)m + TestAids::kManaCurrent);
        float* cap = (float*)((uint8_t*)m + TestAids::kManaMax);
        // `vr mana <value>`: the current mana AND the level it regenerates to, as a few casts would leave them (a
        // test of the potions without casting: Blink threw Emily overboard and she drowned, 2026-10-05)
        const float set = g_SetMana.exchange(-1.f);
        if (set >= 0.f && set < 10000.f) { LOGI("TestAids: mana {:.1f}/{:.1f} -> {:.1f}", *cur, *cap, set); *cur = set; *cap = set; }
        g_ManaLast = *cur; g_ManaMaxLast = *cap;
        // +0x2C is not a fixed maximum: it is the level mana regenerates back to, and the game lowers it with every
        // cast (measured: 100 -> 80 after a possession; 100 -> 30 after two swarms and a few casts, and it stayed
        // there). Filling up to it made "infinite" mana run dry. Keep the highest value this component ever had, and at
        // least the full bar (100): a save made after a few casts loads with the level already lowered (55.7 measured
        // 2026-10-18), and filling to that left the bar half empty for the whole session.
        static void* seenFor = nullptr; static float seen = 0.f;
        if (m != seenFor) { seenFor = m; seen = 100.f; }
        if (*cap > seen && *cap < 10000.f) seen = *cap;
        if (!g_InfMana.load() || !(seen > 0.f)) return;
        if (*cap < seen) *cap = seen;
        if (*cur < seen) { *cur = seen; g_ManaRefills++; }
    }
}

void TestAids::GiveMark() { std::lock_guard lk(g_M); g_Actions.push_back({ 0, {} }); }
void TestAids::GivePower(const std::string& filter) { std::lock_guard lk(g_M); g_Actions.push_back({ 1, filter.empty() ? "all" : filter }); }
void TestAids::GiveUpgrade(const std::string& filter) { std::lock_guard lk(g_M); g_Actions.push_back({ 3, filter.empty() ? "list" : filter }); }
void TestAids::ListItems(const std::string& filter) { std::lock_guard lk(g_M); g_Actions.push_back({ 2, filter }); }
void TestAids::ListDecls(const std::string& filter) { std::lock_guard lk(g_M); g_Actions.push_back({ 4, filter }); }
void TestAids::GiveEquip(const std::string& filter) { std::lock_guard lk(g_M); g_Actions.push_back({ 5, filter.empty() ? "list" : filter }); }
void TestAids::GiveItem(const std::string& arg) { std::lock_guard lk(g_M); g_Actions.push_back({ 6, arg }); }
void TestAids::GiveAll() { std::lock_guard lk(g_M); g_Actions.push_back({ 7, {} }); }
void TestAids::SetAutoAll(int on)
{
    {
        std::lock_guard lk(g_M);
        g_AutoAll = on != 0;
        g_AutoPlayer = nullptr;               // apply to the current player too
    }
    // the infinite switches go with it: what is handed out stays topped up
    SetInfiniteMana(on); SetInfiniteAmmo(on); SetInfiniteItems(on);
    LOGI("TestAids: autoall {}", on);
}
void TestAids::SetAutoUnlock(unsigned mask)
{
    {
        std::lock_guard lk(g_M);
        if (mask == g_AutoMask) return;
        g_AutoMask = mask & kUnlockAll;
        g_AutoPlayer = nullptr;               // apply to the current player too
    }
    LOGI("TestAids: the ini's unlocks {:#x} for every save loaded (mark {}, powers {}, power upgrades {}, black market {}, weapons {}, ammo {}, elixirs {})", mask,
         (mask & kUnlockMark) != 0, (mask & kUnlockPowers) != 0, (mask & kUnlockPowerUpgrades) != 0, (mask & kUnlockEquipUpgrades) != 0,
         (mask & kUnlockWeapons) != 0, (mask & kUnlockAmmo) != 0, (mask & kUnlockElixirs) != 0);
}
void TestAids::SetInfiniteItems(int on) { g_InfItems = on; LOGI("TestAids: infinite items {}", on ? "on" : "off"); }
void TestAids::SetInfiniteAmmo(int on) { g_InfAmmo = on; LOGI("TestAids: infinite ammo {}", on ? "on" : "off"); }
void TestAids::SetInfiniteMana(int on) { g_InfMana = on; LOGI("TestAids: infinite mana {}", on ? "on" : "off"); }
void TestAids::SetHealth(float v) { g_SetHealth = v; }
void TestAids::SetMana(float v) { g_SetMana = v; }
void TestAids::SetInfiniteHealth(int on) { g_InfHealth = on; LOGI("TestAids: infinite health {}", on ? "on" : "off"); }
void TestAids::SetAutoPowers(const std::string& filter)
{
    std::lock_guard lk(g_M);
    g_AutoPowers = filter == "off" ? std::string() : filter;
    g_AutoPlayer = nullptr;                   // apply to the current player too
    LOGI("TestAids: autopowers '{}'", g_AutoPowers);
}

void TestAids::RequestDump(uint32_t typeInfoRva, uint32_t len, const std::string& tag, int depth, uint32_t childLen)
{
    std::lock_guard lk(g_M);
    g_Reqs.push_back({ typeInfoRva, 0, len ? len : 0x400, childLen, depth, tag.empty() ? fmt::format("{:X}", typeInfoRva) : tag });
}
void TestAids::RequestDumpAt(uintptr_t addr, uint32_t len, const std::string& tag, int depth, uint32_t childLen)
{
    std::lock_guard lk(g_M);
    g_Reqs.push_back({ 0, addr, len ? len : 0x400, childLen, depth, tag.empty() ? fmt::format("{:X}", addr) : tag });
}

void* TestAids::Component(uint32_t typeInfoRva)
{
    std::lock_guard lk(g_M);
    auto it = g_Found.find(typeInfoRva);
    return it == g_Found.end() ? nullptr : it->second;
}

void TestAids::Tick()
{
    void* player = Game::LocalPlayer();
    if (!player) return;
    TickMana();
    TickHealth();
    TickAmmo();
    {   // autopowers: once per player, after it has been ticking for ~3 s (a fresh load is still being set up)
        std::lock_guard lk(g_M);
        if (player != g_AutoPlayer) { g_AutoPlayer = player; g_AutoTicks = 0; }
        // Only while actually playing: the main menu runs a player too, and first-person draws are the mod's one
        // positive "in game" signal.
        const bool playing = GpuTrace::FirstPersonDraws() > 0;
        if ((!g_AutoPowers.empty() || g_AutoAll || g_AutoMask) && playing && g_AutoTicks < 180 && ++g_AutoTicks == 180)
        {
            if (!g_AutoPowers.empty() || g_AutoAll) g_Actions.push_back({ 0, {} });
            if (!g_AutoPowers.empty()) g_Actions.push_back({ 1, g_AutoPowers });
            if (g_AutoPowers.find("all") != std::string::npos) g_Actions.push_back({ 3, "all" });   // and every upgrade (possessing humans...)
            if (g_AutoAll) g_Actions.push_back({ 7, {} });
            else if (g_AutoMask) g_Actions.push_back({ 8, std::to_string(g_AutoMask) });
            g_AutoRuns++;
            LOGI("TestAids: autopowers for player {}: '{}'{}", player, g_AutoPowers, g_AutoAll ? ", and everything else (autoall)" : g_AutoMask ? fmt::format(", the ini's unlocks {:#x}", g_AutoMask) : std::string());
        }
    }
    for (;;)
    {
        Action a;
        { std::lock_guard lk(g_M); if (g_Actions.empty()) break; a = g_Actions.front(); g_Actions.pop_front(); }
        DoAction(a);
    }
    TickItems();
    for (;;)
    {
        Req r;
        { std::lock_guard lk(g_M); if (g_Reqs.empty()) break; r = g_Reqs.front(); g_Reqs.pop_front(); }
        const void* c = (const void*)r.addr;
        if (r.ti)
        {
            void* player = Game::LocalPlayer();
            void* cp = GetComponent(player, r.ti);
            { std::lock_guard lk(g_M); g_Found[r.ti] = cp; }
            if (!cp) { LOGI("cpnt: typeinfo exe+0x{:X}: the player has no such component (player {})", r.ti, player); continue; }
            c = cp;
        }
        if (!Readable(c, 8)) { LOGI("cpnt: {} is not readable", c); continue; }
        Dump(c, r);
    }
}

std::string TestAids::Stats()
{
    std::lock_guard lk(g_M);
    return "infhealth " + std::to_string(g_InfHealth.load()) + " hprefills " + std::to_string(g_HealthRefills.load()) + fmt::format(" hp {:.1f}/{:.1f} ", g_HealthLast.load(), g_HealthMaxLast.load()) + fmt::format("infitems {} refills {} infammo {} counter {} infmana {} mana {:.1f}/{:.1f} refills {} powersgiven {} autopowers '{}' autoall {} autoruns {}", g_InfItems.load(), g_ItemRefills.load(), g_InfAmmo.load(), g_AmmoCounter.load(), g_InfMana.load(), g_ManaLast.load(),
                       g_ManaMaxLast.load(), g_ManaRefills.load(), g_PowersGiven.load(), g_AutoPowers, g_AutoAll, g_AutoRuns)
         + fmt::format(" itemtable builds {} hits {}", g_ItemCacheBuilds, g_ItemCacheHits);
}
