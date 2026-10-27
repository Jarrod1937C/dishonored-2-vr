#pragma once
#include <cstdint>

//! Hides the first-person arms and/or body while keeping the equipment (weapons) visible.
//! The player is several entities (PlayerBody = the player entity, PlayerLeftArm, PlayerRightArm, Player*Equipment).
//! Each has an arkSkinnedRenderModel at entity+0xF0 (and a second one at +0x368); zeroing the model's
//! m_meshVisibilityMasks (+0x2100, 5 qwords) makes the engine recompute m_numVisibleMeshes = 0 every frame.
//! Tick() runs on the game thread once per frame (from HandAimHook's UpdateAim detour).
namespace BodyHide
{
    constexpr uint32_t kGetComponent      = 0x877720;   //!< cpnt* GetComponent(entity, typeinfo*, int flags)
    constexpr uint32_t kBodyCpntTypeInfo  = 0x39CB7A0;  //!< typeinfo of the player component holding the arm entity ids
    constexpr uint32_t kGetEntityById     = 0x6DC560;   //!< entity* GetEntityById(int spawnId): table at *(exe+kEntityWorld) + 0x3130B0
    constexpr uint32_t kEntityWorld       = 0x39A5290;  //!< the global GetEntityById reads its table through (null while a level loads)
    constexpr uint32_t kEntityTable       = 0x3130B0;
    //! GetEntityById, safe to call at any time: null while the world is not there. The game's own reads the table
    //! through a global that is NULL during a level change, and calling it then faults (entering the Clockwork Mansion,
    //! 2026-10-18: GrabHand asked for the entity its pointer last picked, mid-load). Use this, never the raw function.
    void* EntityById(int id);
    constexpr uint32_t kLeftArmEntityId   = 0x370;      //!< in the body component
    constexpr uint32_t kRightArmEntityId  = 0x680;
    constexpr uint32_t kEntityModelA      = 0xF0;       //!< arkSkinnedRenderModel* (+0x360 is the skeleton object, whose +0x38 is this model: NpcSkel)
    constexpr uint32_t kEntityModelB      = 0x368;
    constexpr uint32_t kSkinnedModelVtable = 0x1B0D628; //!< arkSkinnedRenderModel
    constexpr uint32_t kMeshVisibilityMasks = 0x2100;   //!< 5 x uint64
    constexpr uint32_t kNumVisibleMeshes  = 0x1F40;

    void SetHideArms(bool on);
    void SetHideBody(bool on);
    void SetHideWeapon(int hand, bool on);   //!< hide the weapon models of one hand (used to show only the anchored hand's weapon)
    void SetHideArmSide(int side, bool on);  //!< hide ONE arm's models (0 left, 1 right); GpuTrace uses it to learn which draws are which arm
    void* ArmModel(int side, int idx);       //!< arkSkinnedRenderModel of that arm (idx 0 = entity+0xF0, 1 = +0x368), null when unknown
    bool HideArms();
    bool HideBody();
    void Tick();            //!< game thread, once per frame
    void LogMasks(const char* tag);   //!< research: the six player models' submesh visibility masks, to the log
    void* LeftArmEntity();  //!< PlayerLeftArm entity (resolved by Tick; null when unknown)
    void* RightArmEntity();
    void  ResolveNow();     //!< force entity resolution on the game thread
    void* LeftWeaponEntity();   //!< PlayerLeftEquipment (arkWeapon) entity, null when unknown
    //! What a hand holds, by the class of the inventory item's component (item+0x10; item = equipment cpnt +0x70 L /
    //! +0x68 R): arkCpntHeart, arkCpntWeaponThrowable (grenades, mines, razors), arkCpntWeaponGun / Crossbow...
    //! The vtable RVA, 0 when unknown. HoldsGadget: the Heart or a throwable -- held in the palm, not aimed.
    constexpr uint32_t kItemCpnt          = 0x10;
    constexpr uint32_t kHeartCpntVtable   = 0x1B99C68;  //!< arkCpntHeart
    constexpr uint32_t kThrowableCpntVtable = 0x1B9D8B8; //!< arkCpntWeaponThrowable
    constexpr uint32_t kPlaceableCpntVtable = 0x1B54538; //!< arkCpntWeaponPlaceable (stun mine, spring razor)
    uint32_t HeldItemClass(int side);
    constexpr uint32_t kGunCpntVtable      = 0x1B53F58; //!< the pistol's gun component
    constexpr uint32_t kCrossbowCpntVtable = 0x1B9CBF8; //!< the crossbow's
    bool HoldsGadget(int side);
    //! A firearm (pistol, crossbow): aimed like a weapon all the time, the reload included -- its animation brings the
    //! gun into the palm with only a draw or two, which is what an unknown item in the palm looks like.
    bool HoldsGun(int side);
    void* RightWeaponEntity();
    //! arkTransformSet of the weapon render models (entity+0xF0 / +0x480 -> arkSkinnedRenderModel+0x1F48). idx 0..1, null when unknown.
    void* WeaponTransformSet(int hand, int idx);
    void* WeaponModel(int hand, int idx);          //!< the weapon's arkSkinnedRenderModel (entity+0xF0 / +0x480), null when unknown
    void* ArmTransformSet(int hand, int idx);      //!< arkTransformSet of the arm render models (entity+0xF0 / +0x368)
    constexpr uint32_t kWeaponModelB      = 0x480;
    constexpr uint32_t kModelTransformSet = 0x1F48;
    constexpr uint32_t kEquipCpntTypeInfo = 0x39DCD30;  //!< player equipment component: +0x68 right item, +0x70 left item; item+0x8 = entity spawn id
    constexpr uint32_t kWeaponVtable      = 0x1CE1E98;  //!< arkWeapon
    //! `staleModels` counts how often a bound render model stopped being one -- i.e. how often the entity behind a
    //! pointer we were about to WRITE through had been freed. It is the measurement behind the level-load crash:
    //! non-zero means the window was real, not theoretical.
    struct Stats { int modelsHidden = 0; int numVisibleLeft = -1, numVisibleRight = -1, numVisibleBody = -1; void* leftArm = nullptr; void* rightArm = nullptr;
                   unsigned staleModels = 0, playerChanges = 0; };
    Stats GetStats();

    //! World transform of every entity: origin at +0x300, idMat3 axis (rows fwd/left/up) at +0x30C. Measured by
    //! WeaponPose from the transform component's update, and the only ground truth we have for WHERE the weapon is.
    constexpr uint32_t kEntityOrigin = 0x300;
    constexpr uint32_t kEntityAxis   = 0x30C;
    //! The first-person entities, sampled on the game thread once per frame. The renderer identifies the weapon's
    //! draws by matching each draw's recovered model origin against these, which needs no scan, no flicker and no
    //! diff taken across two different frames -- see the "6DOF weapon" section of ENGINE.md.
    enum AnchorId { aWeaponL, aWeaponR, aArmL, aArmR, aPlayer, aCount };
    //! `ent` identifies WHICH entity this is: a weapon swap builds a new equipment entity, and the 6DOF anchor
    //! (the model's rest pose) belongs to the old model and has to be retaken when it changes.
    struct Anchor { bool valid = false; void* ent = nullptr; float org[3] = {}; float axis[9] = {}; };
    void GetAnchors(Anchor out[aCount]);
    //! The same entities read NOW, on the calling thread (the pointers Tick resolved, each re-validated): what
    //! ViewMatrixHook records with a view when it is built, so the view and the entities come from one game tick.
    void SampleNow(Anchor out[aCount]);
    const char* AnchorName(int id);
}
