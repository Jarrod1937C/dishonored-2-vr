#include "pch.h"
#include "BodyHide.h"
#include "GameInfo.h"
#include "Game.h"
#include <set>

namespace
{
    std::atomic<bool> g_HideArms{ false }, g_HideBody{ false };
    std::mutex g_M; BodyHide::Stats g_Stats;

    struct Target { uint8_t* model = nullptr; uint64_t saved[5] = {}; bool zeroed = false; };
    Target g_Targets[6];          // body A/B, left A/B, right A/B
    Target g_WTargets[2][2];      // weapon models per hand
    std::atomic<bool> g_HideWeapon[2];
    std::atomic<bool> g_HideArmSide[2];
    std::atomic<void*> g_LeftArm{ nullptr }, g_RightArm{ nullptr }, g_LeftWeapon{ nullptr }, g_RightWeapon{ nullptr };
    std::atomic<void*> g_CarryEnt{ nullptr };     // PhysicalPickup: the item being carried (the aCarry anchor)
    std::atomic<uint32_t> g_ItemClass[2] = {};
    std::atomic<void*> g_WeaponTset[2][2], g_ArmTset[2][2], g_WeaponModel[2][2];
    int g_Frame = 0;
    unsigned g_StaleModels = 0, g_PlayerChanges = 0;
    std::mutex g_AnchorM; BodyHide::Anchor g_Anchors[BodyHide::aCount];

    bool Readable(const void* p, size_t n)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    //! Does this still look like a live arkSkinnedRenderModel? A model belongs to an entity, and the engine destroys
    //! every first-person entity on a level load, on a save load and on a weapon change. The pointer is re-resolved
    //! only every 30 frames, so between the entity dying and the next Resolve() it addresses freed heap -- and the
    //! visibility masks are WRITTEN, not read. Measured: with `vr hidearms 1` the game died two save loads later
    //! with a null dereference inside its own entity update (exe+0x7BBFA7), the classic shape of a 40-byte write
    //! into a block the allocator had handed to somebody else. So nothing is written without this check first.
    bool LiveModel(const uint8_t* m)
    {
        return Readable(m, BodyHide::kMeshVisibilityMasks + 0x28)
            && *(const uintptr_t*)m == GameInfo::Base() + BodyHide::kSkinnedModelVtable;
    }
    uint8_t* ModelOf(uint8_t* entity, uint32_t off)
    {
        if (!Readable(entity + off, 8)) return nullptr;
        uint8_t* m = *(uint8_t**)(entity + off);
        return LiveModel(m) ? m : nullptr;
    }
    void Restore(Target& t)
    {
        if (t.zeroed && LiveModel(t.model))
        {
            uint64_t* m = (uint64_t*)(t.model + BodyHide::kMeshVisibilityMasks);
            LOGI("BodyHide: restore model {} masks now {:#x} {:#x} {:#x} -> saved {:#x} {:#x} {:#x}", (void*)t.model, m[0], m[1], m[2], t.saved[0], t.saved[1], t.saved[2]);
            memcpy(t.model + BodyHide::kMeshVisibilityMasks, t.saved, 0x28);
        }
        t.zeroed = false;
    }
    void Bind(Target& t, uint8_t* model)
    {
        if (t.model == model) return;
        Restore(t);
        t.model = model;
    }
    void Apply(Target& t, bool hide)
    {
        if (!t.model) return;
        if (!LiveModel(t.model)) { g_StaleModels++; t.model = nullptr; t.zeroed = false; return; }   // its entity is gone
        uint64_t* masks = (uint64_t*)(t.model + BodyHide::kMeshVisibilityMasks);
        if (hide)
        {
            bool any = false; for (int i = 0; i < 5; i++) any |= masks[i] != 0;
            if (any)
            {
                if (!t.zeroed) { memcpy(t.saved, masks, 0x28); LOGI("BodyHide: hide model {} saved masks {:#x} {:#x} {:#x}", (void*)t.model, t.saved[0], t.saved[1], t.saved[2]); }
                else LOGI("BodyHide: model {} re-shown by the game while hidden: masks {:#x} {:#x} {:#x} (saved {:#x} {:#x} {:#x})", (void*)t.model, masks[0], masks[1], masks[2], t.saved[0], t.saved[1], t.saved[2]);
                memset(masks, 0, 0x28); t.zeroed = true;
            }
        }
        else Restore(t);
    }
    //! Copy the world transform of the first-person entities for the render thread. Called every frame on the game
    //! thread: the POINTERS are only refreshed every 30 frames, so each one is re-validated here -- a weapon swap
    //! destroys the entity and the stale pointer would otherwise be read as geometry.
    void SampleInto(BodyHide::Anchor* a)
    {
        void* ents[BodyHide::aCount] = { g_LeftWeapon.load(), g_RightWeapon.load(), g_LeftArm.load(), g_RightArm.load(), Game::LocalPlayer(), g_CarryEnt.load() };
        for (int i = 0; i < BodyHide::aCount; i++)
        {
            uint8_t* e = (uint8_t*)ents[i];
            if (!e || !Readable(e + BodyHide::kEntityAxis, 36)) continue;
            a[i].ent = e;
            memcpy(a[i].org, e + BodyHide::kEntityOrigin, 12);
            memcpy(a[i].axis, e + BodyHide::kEntityAxis, 36);
            // A destroyed entity leaves plausible-looking garbage; an axis that is not orthonormal is the cheapest
            // way to notice, and it also rejects a transform the engine has not filled in yet.
            float n = 0.f; for (int k = 0; k < 3; k++) n += a[i].axis[k] * a[i].axis[k];
            if (!(n > 0.9f && n < 1.1f)) continue;
            a[i].valid = true;
        }
    }
    void SampleAnchors()
    {
        BodyHide::Anchor a[BodyHide::aCount];
        SampleInto(a);
        std::lock_guard lk(g_AnchorM);
        memcpy(g_Anchors, a, sizeof g_Anchors);
    }

    void Resolve()
    {
        using GetComponentFn = void* (*)(void*, void*, int);
        uintptr_t base = GameInfo::Base();
        uint8_t* player = (uint8_t*)Game::LocalPlayer();
        g_LeftArm = nullptr; g_RightArm = nullptr;
        for (auto& t : g_Targets) { if (!player) { Restore(t); t.model = nullptr; } }
        if (!player) return;
        Bind(g_Targets[0], ModelOf(player, BodyHide::kEntityModelA));
        Bind(g_Targets[1], ModelOf(player, BodyHide::kEntityModelB));
        // equipment entities (weapons): follow the player's equipment component items
        {
            static void* lastL = nullptr; static void* lastR = nullptr;
            uint8_t* eq = (uint8_t*)((GetComponentFn)(base + BodyHide::kGetComponent))(player, (void*)(base + BodyHide::kEquipCpntTypeInfo), 2);
            void* w[2] = {}; uint32_t cls[2] = {};
            for (int a = 0; a < 2 && eq && Readable(eq + 0x78, 8); a++)
            {
                uint8_t* item = *(uint8_t**)(eq + (a == 0 ? 0x70 : 0x68));
                if (!item || !Readable(item + 8, 4)) continue;
                if (Readable(item + BodyHide::kItemCpnt, 8))
                {   // the item's own component says what it is (measured: the Heart's is arkCpntHeart)
                    uint8_t* c = *(uint8_t**)(item + BodyHide::kItemCpnt);
                    if (c && Readable(c, 8)) { const uintptr_t v = *(uintptr_t*)c; if (v > base && v - base < 0x4000000) cls[a] = (uint32_t)(v - base); }
                }
                int id = *(int*)(item + 8);
                uint8_t* ent = (uint8_t*)BodyHide::EntityById(id);
                // Any entity the equipment component points at counts. Requiring the arkWeapon vtable dropped
                // everything that is not a gun: with a mine in hand the left slot resolved to nothing, so the 6DOF
                // rewrite had no entity to match draws against and the gadget stayed glued to the camera. The reads
                // that follow are bounds-checked, and the transform is rejected unless its axis is orthonormal, so
                // the vtable adds no safety here -- the models are validated separately by ModelOf().
                if (ent && Readable(ent, 0x340))
                {
                    w[a] = ent;
                    static std::set<uintptr_t> seen;
                    const uintptr_t vt = *(uintptr_t*)ent - base;
                    if (vt != BodyHide::kWeaponVtable && seen.insert(vt).second)
                        LOGI("BodyHide: equipment {} entity {} has vtable exe+0x{:X} (not arkWeapon; used anyway)", a ? "R" : "L", (void*)ent, vt);
                }
            }
            g_LeftWeapon.store(w[0]); g_RightWeapon.store(w[1]);
            for (int a = 0; a < 2; a++)
                if (g_ItemClass[a].exchange(cls[a]) != cls[a])
                    LOGI("BodyHide: {} hand item component vtable exe+0x{:X} ({})", a ? "R" : "L", cls[a],
                         cls[a] == BodyHide::kHeartCpntVtable ? "the Heart" : cls[a] == BodyHide::kThrowableCpntVtable ? "a throwable" : cls[a] == BodyHide::kPlaceableCpntVtable ? "a placeable (mine / razor)" : cls[a] ? "other" : "none");
            for (int a = 0; a < 2; a++) for (int k = 0; k < 2; k++)
            {
                uint8_t* m = w[a] ? ModelOf((uint8_t*)w[a], k == 0 ? BodyHide::kEntityModelA : BodyHide::kWeaponModelB) : nullptr;
                Bind(g_WTargets[a][k], m);
                void* ts = (m && Readable(m + BodyHide::kModelTransformSet, 8)) ? *(void**)(m + BodyHide::kModelTransformSet) : nullptr;
                g_WeaponTset[a][k].store(ts);
                g_WeaponModel[a][k].store(m);
            }
            if (w[0] != lastL || w[1] != lastR) { LOGI("BodyHide: weapon entities L={} R={}", w[0], w[1]); lastL = w[0]; lastR = w[1]; }
        }
        void* body = ((GetComponentFn)(base + BodyHide::kGetComponent))(player, (void*)(base + BodyHide::kBodyCpntTypeInfo), 2);
        if (!body || !Readable((uint8_t*)body + BodyHide::kRightArmEntityId, 4)) { for (int i = 2; i < 6; i++) Bind(g_Targets[i], nullptr); return; }
        int ids[2] = { *(int*)((uint8_t*)body + BodyHide::kLeftArmEntityId), *(int*)((uint8_t*)body + BodyHide::kRightArmEntityId) };
        for (int a = 0; a < 2; a++)
        {
            uint8_t* ent = (uint8_t*)BodyHide::EntityById(ids[a]);
            (a == 0 ? g_LeftArm : g_RightArm).store(ent);
            Bind(g_Targets[2 + a * 2], ent ? ModelOf(ent, BodyHide::kEntityModelA) : nullptr);
            Bind(g_Targets[3 + a * 2], ent ? ModelOf(ent, BodyHide::kEntityModelB) : nullptr);
            for (int k = 0; k < 2; k++)
            {
                uint8_t* m = g_Targets[2 + a * 2 + k].model;
                g_ArmTset[a][k].store((m && Readable(m + BodyHide::kModelTransformSet, 8)) ? *(void**)(m + BodyHide::kModelTransformSet) : nullptr);
            }
        }
    }
}

void BodyHide::SetHideArms(bool on) { g_HideArms = on; }
void BodyHide::SetHideBody(bool on) { g_HideBody = on; }
void BodyHide::SetHideWeapon(int hand, bool on) { g_HideWeapon[hand & 1] = on; }
void BodyHide::SetHideArmSide(int side, bool on) { g_HideArmSide[side & 1] = on; }
void* BodyHide::ArmModel(int side, int idx) { return g_Targets[2 + (side & 1) * 2 + (idx & 1)].model; }
void* BodyHide::EntityById(int id)
{
    // exe+0x7B5E00, the lookup: 0x3FFF0010 = none; table[id >> 16], kept only if that entity's +0xA8 is the same id
    if (id <= 0 || id == 0x3FFF0010) return nullptr;
    const uintptr_t base = GameInfo::Base();
    const uintptr_t world = Readable((void*)(base + kEntityWorld), 8) ? *(const uintptr_t*)(base + kEntityWorld) : 0;
    if (!world || !Readable((void*)(world + kEntityTable + (uintptr_t)(id >> 16) * 8), 8)) return nullptr;
    using GetEntityFn = void* (*)(int);
    return ((GetEntityFn)(base + kGetEntityById))(id);
}

bool BodyHide::HideArms() { return g_HideArms; }
bool BodyHide::HideBody() { return g_HideBody; }

void BodyHide::Tick()
{
    bool arms = g_HideArms.load(), body = g_HideBody.load();
    static bool wasActive = false;
    bool active = arms || body || g_HideWeapon[0].load() || g_HideWeapon[1].load() || g_HideArmSide[0].load() || g_HideArmSide[1].load();
    // A level load, a save load or a death builds a new player entity, and every model hanging off the old one is
    // freed with it. Waiting up to 30 frames to notice leaves the arms visible for half a second AND (before the
    // check in Apply) wrote into freed memory; re-binding the moment the player changes closes both.
    // Is anything we are bound to already dead? Checked FIRST and unconditionally, so the count means what it says:
    // every hit is a frame on which the old code would have written 0x28 bytes through a freed pointer.
    bool changed = false;
    for (auto& t : g_Targets) if (t.model && !LiveModel(t.model)) { g_StaleModels++; changed = true; }
    for (auto& w : g_WTargets) for (auto& t : w) if (t.model && !LiveModel(t.model)) { g_StaleModels++; changed = true; }
    // And from the other side: the player pointer itself. A weapon swap frees models without changing it, a load
    // changes it before the models have necessarily been touched -- neither test alone catches both.
    static void* lastPlayer = nullptr;
    void* player = Game::LocalPlayer();
    if (player != lastPlayer) { if (lastPlayer) g_PlayerChanges++; changed = true; }
    lastPlayer = player;
    if (changed || (g_Frame++ % 30) == 0 || (active && !wasActive)) Resolve();
    SampleAnchors();                                                    // every frame: the weapon moves every frame
    if (!active && !wasActive) return;
    wasActive = active;
    Apply(g_Targets[0], body); Apply(g_Targets[1], body);
    for (int i = 2; i < 6; i++) Apply(g_Targets[i], arms || g_HideArmSide[(i - 2) / 2].load());
    for (int a = 0; a < 2; a++) for (int k = 0; k < 2; k++) Apply(g_WTargets[a][k], g_HideWeapon[a].load());
    std::lock_guard lk(g_M);
    g_Stats = {};
    for (auto& t : g_Targets) g_Stats.modelsHidden += t.zeroed;
    auto nv = [](const Target& t) { return t.model && Readable(t.model + kNumVisibleMeshes, 4) ? *(int*)(t.model + kNumVisibleMeshes) : -1; };
    g_Stats.numVisibleBody = nv(g_Targets[0]); g_Stats.numVisibleLeft = nv(g_Targets[2]); g_Stats.numVisibleRight = nv(g_Targets[4]);
    g_Stats.leftArm = g_LeftArm.load(); g_Stats.rightArm = g_RightArm.load();
    g_Stats.staleModels = g_StaleModels; g_Stats.playerChanges = g_PlayerChanges;
}
void BodyHide::LogMasks(const char* tag)
{
    const char* kName[6] = { "bodyA", "bodyB", "leftA", "leftB", "rightA", "rightB" };
    for (int i = 0; i < 6; i++)
    {
        const Target& t = g_Targets[i];
        if (!t.model || !LiveModel(t.model)) { LOGI("armmasks {} {}: none", tag, kName[i]); continue; }
        const uint64_t* m = (const uint64_t*)(t.model + kMeshVisibilityMasks);
        LOGI("armmasks {} {}: model {} numVisible {} masks {:#x} {:#x} {:#x} {:#x} {:#x} zeroed {}", tag, kName[i], (void*)t.model,
             *(const int*)(t.model + kNumVisibleMeshes), m[0], m[1], m[2], m[3], m[4], t.zeroed);
    }
}
void* BodyHide::LeftArmEntity() { return g_LeftArm.load(); }
void* BodyHide::RightArmEntity() { return g_RightArm.load(); }
void BodyHide::ResolveNow() { Resolve(); }
void* BodyHide::ArmTransformSet(int hand, int idx) { return g_ArmTset[hand & 1][idx & 1].load(); }
void* BodyHide::WeaponTransformSet(int hand, int idx) { return g_WeaponTset[hand & 1][idx & 1].load(); }
void* BodyHide::WeaponModel(int hand, int idx) { return g_WeaponModel[hand & 1][idx & 1].load(); }
void* BodyHide::LeftWeaponEntity() { return g_LeftWeapon.load(); }
uint32_t BodyHide::HeldItemClass(int side) { return g_ItemClass[side & 1].load(); }
bool BodyHide::HoldsGadget(int side) { const uint32_t c = g_ItemClass[side & 1].load(); return c == kHeartCpntVtable || c == kThrowableCpntVtable || c == kPlaceableCpntVtable; }
bool BodyHide::HoldsGun(int side) { const uint32_t c = g_ItemClass[side & 1].load(); return c == kGunCpntVtable || c == kCrossbowCpntVtable; }
void* BodyHide::RightWeaponEntity() { return g_RightWeapon.load(); }
BodyHide::Stats BodyHide::GetStats() { std::lock_guard lk(g_M); return g_Stats; }
void BodyHide::SetCarryEntity(void* ent) { g_CarryEnt = ent; }
void BodyHide::GetAnchors(Anchor out[aCount]) { std::lock_guard lk(g_AnchorM); memcpy(out, g_Anchors, sizeof g_Anchors); }
void BodyHide::SampleNow(Anchor out[aCount]) { for (int i = 0; i < aCount; i++) out[i] = Anchor(); SampleInto(out); }
const char* BodyHide::AnchorName(int id)
{
    static const char* kN[aCount] = { "weaponL", "weaponR", "armL", "armR", "player", "carry" };
    return (unsigned)id < aCount ? kN[id] : "?";
}
