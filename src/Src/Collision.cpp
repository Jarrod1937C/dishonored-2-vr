#include "pch.h"
#include "Collision.h"
#include "GameInfo.h"
#include "GpuTrace.h"
#include "HandAimHook.h"
#include "BodyHide.h"
#include "GrabHand.h"
#include "VRRuntime.h"
#include <cmath>
#include <sstream>
#include <unordered_map>

namespace
{
    // ---- the game's Havok ray cast (2026-10-29: exe+0x636630 and exe+0x638570 build the same query) ----
    // castRay(hknpWorld*, const hknpRayCastQuery*, hknpCollisionQueryCollector*), the world at *(game+0x60410).
    // The query (0x70): +0x00 the world's shape tag codec (world+0x9D8), +0x08 the game's filter, +0x10 u16 material
    // 0xFFFF, +0x14 u32 filter info 0, +0x18 u64 user data 0, +0x20 u32 2 (level of detail), +0x30 the ray's origin
    // (w 0), +0x40 its direction = to - from (w 1: the fraction), +0x50 1/direction (FLT_MAX where 0; w = 0x3F000000 |
    // the signs: bit k = direction.k >= 0), +0x60 u32 0.
    // The filter (0x28): vtable exe+0x1AD4B68, +0x10 u32 0x1FFFF, +0x18 u8 4, +0x20 the LAYERS a body may be on (its
    // collision filter info & 0x1F indexes this mask), +0x24 the flags it must ALL have ((info >> 5) & f == f). The
    // sword's world sweep ("MeleeWeaponWorld") asks layers & 0x27 with flags 0x100000, its sweep for characters
    // ("MeleeWeaponEnemies") layers & 0x4480; the gun's trace layers 0x2F.
    // The all-hits collector (vtable exe+0x1AD2640): +0x0C hits found, +0x20 the early-out fraction (a vector of
    // 1.8e19), +0x30 an hkArray of 0x70-byte results (+0x38 count, +0x3C capacity | 0x80000000 = inline storage, which
    // follows at +0x50). A result: +0x00 the point, +0x10 the normal, +0x20 the fraction along the ray, +0x48 the hit
    // body's id, +0x54 its shape's filter info, +0x60 its user data (-> +0x08 -> +0x10 = the entity).
    constexpr uint32_t kGamePtr = 0x39A5290, kPhysWorld = 0x60410, kCodec = 0x9D8;
    constexpr uint32_t kCastRay = 0xC89B00;
    constexpr uint32_t kFilterVt = 0x1AD4B68, kFilterBaseVt = 0x1AD40F8, kFilterDtor = 0xA9E0D0;
    constexpr uint32_t kAllHitsVt = 0x1AD2640, kCollectorDtor = 0xA9E0B0, kHeapAllocator = 0xA9DE90;
    // The entity messages (exe+0x8112C0(queue = exe+0x810240() + 0xC8E8, type, payload size, entity id) -> payload):
    // 0xFA = an impulse {impulse xyz, world point xyz} (Windblast exe+0x4D7750, the damage knockback exe+0x7B2D19), sent
    // only to entities with entity+0xD1 bit 3 (physics-driven) -- the test the game makes too.
    constexpr uint32_t kMsgSystem = 0x810240, kMsgPost = 0x8112C0, kMsgQueue = 0xC8E8, kMsgImpulse = 0xFA;
    // Doors: an entity whose class id (vtable slot 0 -> type info +0x60) lies between the globals exe+0x39C0350 / +4 is
    // an arkDoor. Windblast opens one with ARK_MSG_DOOR_PUSH (0x138, payload = the pusher's entity id; exe+0x4D7E80,
    // chosen by that class test at exe+0x4D89D5) -- the push NPCs walking through use too: the door swings away from
    // the pusher.
    constexpr uint32_t kDoorClassRange = 0x39C0350, kMsgDoorPush = 0x138;
    // Drawers, cabinets (and some doors) carry an arkCpntOpenable (type exe+0x39C59A0, registered exe+0x13BB9B0; vtable
    // exe+0x1C569C8): +0x1E8 = open now. It is opened and closed by the player's interact on it, which applies the
    // game's own rules (a lock component refuses it, a key opens it) -- so a hand's grab-and-pull, or its push on an open
    // one, presses the interact (KbmInject) rather than forcing the state (exe+0x74E4C0(openable, open, animate), the
    // script's Open / Close, checks no lock).
    constexpr uint32_t kOpenableType = 0x39C59A0, kOpenableIsOpen = 0x1E8, kGetComponent = 0x877720;
    // Pickups: arkCpntLoot (type exe+0x39C5880, registered exe+0x13BB850: coins, ammo, elixirs, runes, charms, keys...)
    // and arkCpntMissionItem (type exe+0x39AE970, exe+0x13A0430). Taking one is the player's interact on it.
    constexpr uint32_t kLootType = 0x39C5880, kMissionItemType = 0x39AE970;
    // Safe dials: arkCpntUsableWithStateList (type exe+0x39BA8B0, registered exe+0x13AE680) with exactly 10 stages (its
    // decl, usable+0x08: stages at +0x70, count +0x78, start +0x80; the current stage at usable+0x28, +0x30 bit 1 = turning).
    // The script node ActionUsable steps it with exe+0x77AEC0 (next, what a click does) and exe+0x77B740 (previous),
    // both through the same transition (exe+0x77A880: the dial's animation and the stage event the safe's script checks).
    constexpr uint32_t kUsableStatesType = 0x39BA8B0, kUsableNext = 0x77AEC0, kUsablePrev = 0x77B740;
    constexpr uint32_t kEntId = 0xA8, kEntFlags = 0xD1, kEntVelocity = 0x33C, kEntModel = 0xF0, kEntScale = 0x330, kModelBoundsSlot = 0x80;
    // The layers (the game's own names, arkCollisionSettings' m_collisionLayers): 0 WORLD_LOW, 1 WORLD_LOW_HIGH,
    // 2 WORLD_HIGH, 3 RIGID_BODY, 4 DEBRIS, 5 MOVER, 6 PLAYER, 7 PLAYER_HITBOX, 8 PLAYER_DEAD, 9 NPC,
    // 10 NPC_BODY_ANIMATED, 11 PROJECTILE, 12 CLOTH, 13 CLOTH_COLLIDER, 14 NPC_BODY_RAGDOLL, 15 INTERACTABLE, 16 TRIGGER,
    // 17 VISION, 18 PLAYER_PHYSICS, 19 DECAL_QUERY, 20 WEAPON_MELEE, 21 CHAIN, 22 TRIGGER_WATER, 23 PICKUP, 25 GHOST,
    // 26 HIDDEN. The hands meet the world, loose props, movers (doors, drawers), interactables and pickups; the flag is
    // the melee sweeps' (bodies a weapon meets).
    constexpr uint32_t kWorldLayers = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 15) | (1u << 23);
    constexpr uint32_t kWorldFlags = 0x100000;

    struct alignas(16) RayQuery
    {
        void* codec; const void* filter;
        uint16_t material; uint16_t pad0; uint32_t cfi;
        uint64_t user;
        uint32_t lod; uint32_t pad1[3];
        float org[4]; float dir[4]; float inv[4];
        uint32_t id; uint32_t pad2[3];
    };
    static_assert(sizeof(RayQuery) == 0x70, "hknpRayCastQuery");
    struct alignas(16) QueryFilter
    {
        const void* vt; uint64_t z08;
        uint32_t a10; uint32_t pad14;
        uint8_t b18; uint8_t pad19[7];
        uint32_t layers; uint32_t flags;
        uint64_t pad28;
    };
    static_assert(offsetof(QueryFilter, layers) == 0x20, "filter layers");
    constexpr int kInline = 16;
    struct alignas(16) AllHits
    {
        const void* vt; uint64_t q08;
        uint8_t b10; uint8_t pad11[15];
        float earlyOut[4];
        uint8_t* buf; int32_t count; uint32_t cap;
        uint8_t pad40[0x10];
        alignas(16) uint8_t inl[kInline * 0x70];
    };
    static_assert(offsetof(AllHits, buf) == 0x30 && offsetof(AllHits, inl) == 0x50, "collector layout");

    struct Cfg
    {
        float palmR = 0.045f;        // m: the palm's clearance
        // the fingers of an empty hand (the hand's frame: x ahead, y to the left, z up), open; they draw in as it closes
        float fingerMid = 0.06f, fingerTip = 0.095f, fingerSpread = 0.022f;
        float fingerR = 0.025f, fingerTipR = 0.018f;
        float itemRMin = 0.012f, itemRMax = 0.035f;
        float maxSep = 0.35f;        // m: the drawn hand further than this from the real one lets go (it passes through)
        float jumpYaw = 6.f;         // deg of the camera's yaw in one tick (a snap turn): start again from the eye
        float jumpPos = 0.5f;        // m of the camera in one tick (a blink, a load)
        float stale = 0.08f;         // s: contacts older than this are dropped
        float pushMin = 0.05f;       // m/s into a prop before it is pushed
        float pushMaxV = 6.f;        // m/s: the push's cap
        float buzzMin = 0.12f, buzzMax = 0.7f, buzzGap = 0.15f;
        float bodyDepth = 0.22f;     // m: the raw blade this deep in a body lets go (SwordContact applies it)
        float doorSpeed = 0.35f;     // m/s into a door, or...
        float doorPress = 0.04f;     // ...the real hand this far past the door's surface: a push opens it
        float doorGap = 1.5f;        // s before the same door is pushed again
    } C;

    std::atomic<bool> g_Hands{ false }, g_Weapons{ false }, g_Push{ false }, g_Doors{ false };
    std::atomic<float> g_HandMass{ 1.f };

    double Now() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart / (double)f.QuadPart; }
    float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    float Len(const float* a) { return sqrtf(Dot(a, a)); }
    void ToWorld(const float r[9], const float t[3], const float l[3], float w[3])
    {   // r: columns fwd/left/up
        for (int i = 0; i < 3; i++) w[i] = r[i * 3] * l[0] + r[i * 3 + 1] * l[1] + r[i * 3 + 2] * l[2] + t[i];
    }
    void ToLocal(const float r[9], const float t[3], const float w[3], float l[3])
    {
        const float d[3] = { w[0] - t[0], w[1] - t[1], w[2] - t[2] };
        for (int c = 0; c < 3; c++) l[c] = r[c] * d[0] + r[3 + c] * d[1] + r[6 + c] * d[2];
    }
    bool Finite(const float* v, int n) { for (int i = 0; i < n; i++) if (!std::isfinite(v[i]) || fabsf(v[i]) > 1e6f) return false; return true; }

    bool SafeRead(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void* HitEntity(const uint8_t* rec)
    {
        __try
        {
            const uint8_t* u = *(const uint8_t* const*)(rec + 0x60);
            if (!u) return nullptr;
            const uint8_t* o = *(const uint8_t* const*)(u + 0x08);
            return o ? *(void* const*)(o + 0x10) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    struct RayHit { bool hit = false; float pos[3] = {}, n[3] = {}, dist = 0.f; void* ent = nullptr; uint32_t body = 0, cfi = 0; };
    std::atomic<uint64_t> g_Casts{ 0 }, g_CastHits{ 0 }, g_CastFaults{ 0 }, g_Skipped{ 0 };

    using FnCastRay = void (*)(void*, const RayQuery*, void*);
    using FnVoid = void (*)(void*);
    using FnAllocator = void* (*)();
    bool CallCast(void* world, const RayQuery* q, AllHits* c)
    {
        __try { ((FnCastRay)(GameInfo::Base() + kCastRay))(world, q, c); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void FreeCollector(AllHits* c, QueryFilter* f)
    {
        __try
        {
            const uintptr_t base = GameInfo::Base();
            if (!(c->cap & 0x80000000u) && c->buf && c->buf != c->inl)
            {   // the results outgrew the inline storage: the array went to the heap (freed as the game frees it)
                void* a = ((FnAllocator)(base + kHeapAllocator))();
                using FnFree = void (*)(void*, void*, int);
                ((FnFree)(*(void***)a)[4])(a, c->buf, (int)((c->cap & 0x3FFFFFFF) * 0x70));
            }
            c->buf = nullptr; c->count = 0;
            ((FnVoid)(base + kCollectorDtor))(c);
            f->vt = (const void*)(base + kFilterBaseVt);
            ((FnVoid)(base + kFilterDtor))(f);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    //! The nearest hit from a to b that is not one of `skip`. Game thread only.
    bool CastRay(const float a[3], const float b[3], uint32_t layers, uint32_t flags, void* const* skip, int nskip, RayHit& out)
    {
        out = RayHit{};
        const uintptr_t base = GameInfo::Base();
        uint8_t* game = nullptr; void* world = nullptr; void* codec = nullptr;
        if (!SafeRead(&game, (void*)(base + kGamePtr), 8) || !game) return false;
        if (!SafeRead(&world, game + kPhysWorld, 8) || !world) return false;
        if (!SafeRead(&codec, (uint8_t*)world + kCodec, 8)) return false;

        QueryFilter f{}; f.vt = (const void*)(base + kFilterVt); f.a10 = 0x1FFFF; f.b18 = 4; f.layers = layers; f.flags = flags;
        RayQuery q{}; q.codec = codec; q.filter = &f; q.material = 0xFFFF; q.lod = 2;
        float dir[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        const float len = Len(dir);
        if (len < 1e-5f || !Finite(a, 3) || !Finite(b, 3)) return false;
        uint32_t signs = 0;
        for (int k = 0; k < 3; k++)
        {
            q.org[k] = a[k]; q.dir[k] = dir[k];
            if (dir[k] == 0.f) { const uint32_t big = 0x7F7FFFEE; memcpy(&q.inv[k], &big, 4); }
            else q.inv[k] = 1.f / dir[k];
            if (dir[k] >= 0.f) signs |= 1u << k;
        }
        q.org[3] = 0.f; q.dir[3] = 1.f;
        { const uint32_t w = 0x3F000000u | signs; memcpy(&q.inv[3], &w, 4); }

        AllHits c{}; c.vt = (const void*)(base + kAllHitsVt);
        for (float& e : c.earlyOut) { const uint32_t v = 0x5F7FFFF0; memcpy(&e, &v, 4); }
        c.buf = c.inl; c.count = 0; c.cap = 0x80000000u | kInline;
        g_Casts++;
        if (!CallCast(world, &q, &c)) { g_CastFaults++; return false; }
        float bestF = 2.f; const uint8_t* best = nullptr; void* bestEnt = nullptr;
        const int n = c.count < 0 ? 0 : (c.count > 4096 ? 4096 : c.count);
        for (int i = 0; i < n; i++)
        {
            const uint8_t* rec = c.buf + (size_t)i * 0x70;
            float fr = 2.f; if (!SafeRead(&fr, rec + 0x20, 4) || !(fr >= 0.f && fr <= 1.f) || fr >= bestF) continue;
            void* e = HitEntity(rec);
            bool sk = false; for (int k = 0; k < nskip; k++) if (e && e == skip[k]) sk = true;
            if (sk) { g_Skipped++; continue; }
            bestF = fr; best = rec; bestEnt = e;
        }
        if (best)
        {
            float p[4], nn[4]; uint32_t body = 0, cfi = 0;
            if (SafeRead(p, best, 16) && SafeRead(nn, best + 0x10, 16) && Finite(p, 3) && Finite(nn, 3))
            {
                SafeRead(&body, best + 0x48, 4); SafeRead(&cfi, best + 0x54, 4);
                out.hit = true; memcpy(out.pos, p, 12); memcpy(out.n, nn, 12);
                const float nl = Len(out.n); if (nl > 1e-4f) for (float& v : out.n) v /= nl; else { for (int k = 0; k < 3; k++) out.n[k] = -dir[k] / len; }
                if (Dot(out.n, dir) > 0.f) for (float& v : out.n) v = -v;   // the side the ray came from
                out.dist = bestF * len; out.ent = bestEnt; out.body = body & 0xFFFFFF; out.cfi = cfi;
                g_CastHits++;
            }
        }
        FreeCollector(&c, &f);
        return out.hit;
    }

    // ---- the points of a hand and its item ----
    constexpr int kMaxPts = 13, kMaxContacts = 16;
    struct Pt { float local[3]; float r; };
    struct Pub { Collision::Contact c[kMaxContacts]; int n = 0; double at = 0; };
    std::mutex g_PubM;
    Pub g_Pub[2], g_Body[2];                  // world contacts (game thread) / characters' bodies (SwordContact)
    std::atomic<bool> g_Touch[2]{ false, false };

    struct HandState
    {
        bool valid = false, ghost = false; int npts = 0; void* item = nullptr;
        float prevRaw[kMaxPts][3] = {}, prevRes[kMaxPts][3] = {};
        double lastBuzz = 0; bool contact = false;
    };
    HandState g_H[2];                          // game thread
    double g_LastTick = 0; float g_LastYaw = 0, g_LastCam[3] = {}; bool g_CamValid = false;
    std::atomic<uint64_t> g_Ticks{ 0 }, g_Stops[2]{ 0, 0 }, g_Ghosts[2]{ 0, 0 }, g_Pushes{ 0 }, g_Resets{ 0 }, g_BodyStops{ 0 };
    std::atomic<float> g_LastOff[2]{ 0.f, 0.f };
    std::string g_LastHitDesc;                // game thread
    std::mutex g_DescM;

    struct BoxCache { void* model = nullptr; bool ok = false; float mn[3] = {}, mx[3] = {}; };
    std::unordered_map<void*, BoxCache> g_Boxes;   // game thread
    using BoundsFn = const float* (*)(void*);
    bool CallBounds(void* model, uintptr_t fn, float out[6])
    {
        __try { const float* b = ((BoundsFn)fn)(model); if (!b) return false; memcpy(out, b, 24); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    //! The weapon entity's model box (entity frame, scaled), as SwordContact reads boxes.
    bool ItemBox(void* ent, float mn[3], float mx[3])
    {
        void* model = nullptr; float sc[3] = { 1, 1, 1 };
        if (!SafeRead(&model, (uint8_t*)ent + kEntModel, 8) || !model) return false;
        SafeRead(sc, (uint8_t*)ent + kEntScale, 12);
        BoxCache& c = g_Boxes[ent];
        if (c.model != model)
        {
            c = BoxCache{}; c.model = model;
            const uintptr_t base = GameInfo::Base(); uintptr_t vt = 0, fn = 0; float b[6];
            if (SafeRead(&vt, model, 8) && vt > base && vt - base < 0x4000000 && SafeRead(&fn, (void*)(vt + kModelBoundsSlot), 8)
                && fn > base && fn - base < 0x4000000 && CallBounds(model, fn, b) && Finite(b, 6))
            {
                c.ok = true;
                for (int i = 0; i < 3; i++) { c.mn[i] = fminf(b[i], b[3 + i]); c.mx[i] = fmaxf(b[i], b[3 + i]); }
            }
            if (g_Boxes.size() > 64) { BoxCache keep = c; g_Boxes.clear(); g_Boxes[ent] = keep; }
        }
        if (!c.ok) return false;
        for (int i = 0; i < 3; i++)
        {
            const float s = std::isfinite(sc[i]) && fabsf(sc[i]) > 1e-3f && fabsf(sc[i]) < 100.f ? fabsf(sc[i]) : 1.f;
            mn[i] = c.mn[i] * s; mx[i] = c.mx[i] * s;
        }
        return true;
    }

    //! The points that collide for this hand, in its frame. The palm always (hands on), and while the hand holds no
    //! item its fingers: one point over the middle of the fingers and two at the tips (index and little finger sides),
    //! reaching ~10 cm ahead of the palm when the hand is open and drawn in to the knuckles as it closes (the grip's
    //! curl, `curl` 0..1, what closes the drawn fingers too). A hand holding something has its fingers round it: the
    //! item's own points stand for them. With weapons on, samples along the item's model box: its long axis (5 points
    //! through the middle of the box) and, for a wide item (a crossbow's limbs), the two ends of the second axis. A
    //! sword whose box cannot be read uses the measured blade (0.14 - 0.69 m). The palm stays point 0.
    int BuildPoints(int h, const float hr[9], const float ht[3], bool item, const float* wo, const float* wa, void* ent, float curl, Pt* out)
    {
        int n = 0;
        if (g_Hands.load())
        {
            out[n].local[0] = out[n].local[1] = out[n].local[2] = 0.f; out[n].r = C.palmR; n++;
            if (!item)
            {
                const float c = std::clamp(curl, 0.f, 1.f);
                auto lerp = [&](float open, float closed) { return open + (closed - open) * c; };
                const float mid[3] = { lerp(C.fingerMid, 0.045f), 0.f, lerp(-0.005f, -0.02f) };
                const float tipX = lerp(C.fingerTip, 0.05f), tipZ = lerp(-0.01f, -0.035f);
                const float tips[2][3] = { { tipX, C.fingerSpread, tipZ }, { tipX - 0.012f, -C.fingerSpread, tipZ } };
                memcpy(out[n].local, mid, 12); out[n].r = C.fingerR; n++;
                for (const auto& t : tips) { memcpy(out[n].local, t, 12); out[n].r = C.fingerTipR; n++; }
            }
        }
        if (!item || !g_Weapons.load()) return n;
        // the item's frame: wo + l.x * fwd + l.y * left + l.z * up (wa rows)
        auto add = [&](const float l[3], float r) {
            if (n >= kMaxPts) return;
            float w[3]; for (int i = 0; i < 3; i++) w[i] = wo[i] + l[0] * wa[i] + l[1] * wa[3 + i] + l[2] * wa[6 + i];
            ToLocal(hr, ht, w, out[n].local); out[n].r = r; n++;
        };
        float mn[3], mx[3];
        if (ent && ItemBox(ent, mn, mx))
        {
            float ext[3]; for (int k = 0; k < 3; k++) ext[k] = mx[k] - mn[k];
            int a = 0; for (int k = 1; k < 3; k++) if (ext[k] > ext[a]) a = k;
            int b = (a + 1) % 3, c = (a + 2) % 3; if (ext[c] > ext[b]) std::swap(b, c);
            if (ext[a] < 0.05f || ext[a] > 2.5f) return n;            // not a hand-held item's size
            const float r = fminf(C.itemRMax, fmaxf(C.itemRMin, 0.5f * ext[c]));
            float mid[3]; for (int k = 0; k < 3; k++) mid[k] = 0.5f * (mn[k] + mx[k]);
            for (int i = 0; i < 5; i++)
            {
                float l[3] = { mid[0], mid[1], mid[2] };
                l[a] = mn[a] + r + (ext[a] - 2.f * r) * (float)i / 4.f;
                add(l, r);
            }
            if (ext[b] > 0.15f)
            {
                float l[3] = { mid[0], mid[1], mid[2] };
                l[b] = mn[b] + r; add(l, r);
                l[b] = mx[b] - r; add(l, r);
            }
        }
        else if (h == 1)
        {
            for (int i = 0; i < 5; i++) { const float l[3] = { 0.14f + 0.55f * (float)i / 4.f, 0.f, 0.f }; add(l, 0.02f); }
        }
        return n;
    }

    //! Push a set of points (hand frame) out of the contact planes: the hand's translation. `cap` = drop all if larger.
    void Solve(const float r[9], const float t[3], const Collision::Contact* c, int n, float off[3])
    {
        off[0] = off[1] = off[2] = 0.f;
        for (int it = 0; it < 4; it++)
        {
            bool moved = false;
            for (int i = 0; i < n; i++)
            {
                float w[3]; ToWorld(r, t, c[i].local, w);
                const float d[3] = { w[0] + off[0] - c[i].p[0], w[1] + off[1] - c[i].p[1], w[2] + off[2] - c[i].p[2] };
                const float pen = Dot(d, c[i].n) - c[i].clear;
                if (pen < -1e-4f) { for (int k = 0; k < 3; k++) off[k] -= c[i].n[k] * pen; moved = true; }
            }
            if (!moved) break;
        }
    }

    void PostImpulse(void* ent, const float imp[3], const float pt[3])
    {
        __try
        {
            const uint32_t id = *(const uint32_t*)((const uint8_t*)ent + kEntId);
            const uintptr_t base = GameInfo::Base();
            uint8_t* sys = ((uint8_t* (*)())(base + kMsgSystem))();
            if (!sys) return;
            using FnPost = float* (*)(void*, uint32_t, uint32_t, uint32_t);
            float* pl = ((FnPost)(base + kMsgPost))(sys + kMsgQueue, kMsgImpulse, 0x18, id);
            if (!pl) return;
            pl[0] = imp[0]; pl[1] = imp[1]; pl[2] = imp[2]; pl[3] = pt[0]; pl[4] = pt[1]; pl[5] = pt[2];
            g_Pushes++;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    bool IsDoor(void* ent)
    {
        if (!ent) return false;
        __try
        {
            using FnType = const uint8_t* (*)(void*);
            const uint8_t* ti = ((FnType)(*(void***)ent)[0])(ent);
            if (!ti) return false;
            const int id = *(const int*)(ti + 0x60);
            const int* range = (const int*)(GameInfo::Base() + kDoorClassRange);
            return id >= range[0] && id <= range[1];
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    std::atomic<uint64_t> g_DoorPushes{ 0 };
    //! The game's own door push, from the player (as Windblast sends it).
    bool PushDoor(void* door, void* player)
    {
        __try
        {
            const uint32_t id = *(const uint32_t*)((const uint8_t*)door + kEntId);
            const uint32_t from = player ? *(const uint32_t*)((const uint8_t*)player + kEntId) : 0x3FFF0010u;
            const uintptr_t base = GameInfo::Base();
            uint8_t* sys = ((uint8_t* (*)())(base + kMsgSystem))();
            if (!sys) return false;
            using FnPost = uint32_t* (*)(void*, uint32_t, uint32_t, uint32_t);
            uint32_t* pl = ((FnPost)(base + kMsgPost))(sys + kMsgQueue, kMsgDoorPush, 4, id);
            if (!pl) return false;
            *pl = from;
            g_DoorPushes++;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    std::unordered_map<void*, double> g_DoorAt;   // game thread: when each door was last pushed
    //! The openable component of an entity: 1 = one that is open, 0 = closed, -1 = none.
    int OpenableState(void* ent)
    {
        if (!ent) return -1;
        __try
        {
            using FnGet = uint8_t* (*)(void*, const void*, int);
            const uint8_t* c = ((FnGet)(GameInfo::Base() + kGetComponent))(ent, (const void*)(GameInfo::Base() + kOpenableType), 2);
            if (!c) return -1;
            return c[kOpenableIsOpen] ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
    bool IsItem(void* ent)
    {
        if (!ent) return false;
        __try
        {
            using FnGet = uint8_t* (*)(void*, const void*, int);
            const uintptr_t base = GameInfo::Base();
            return ((FnGet)(base + kGetComponent))(ent, (const void*)(base + kLootType), 2) != nullptr
                || ((FnGet)(base + kGetComponent))(ent, (const void*)(base + kMissionItemType), 2) != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    std::atomic<bool> g_Items{ false };
    std::atomic<bool> g_Dials{ false };
    std::atomic<uint64_t> g_DialSteps{ 0 };
    //! The 10-stage usable (a safe's dial) of an entity, or null.
    uint8_t* DialOf(void* ent)
    {
        if (!ent) return nullptr;
        __try
        {
            using FnGet = uint8_t* (*)(void*, const void*, int);
            uint8_t* u = ((FnGet)(GameInfo::Base() + kGetComponent))(ent, (const void*)(GameInfo::Base() + kUsableStatesType), 2);
            if (!u) return nullptr;
            const uint8_t* decl = *(const uint8_t* const*)(u + 0x08);
            return decl && (*(const uint32_t*)(decl + 0x78) & 0xFFFFFF) == 10 ? u : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    //! One step of a dial: +1 = next (up), -1 = previous (down). False while it is still turning.
    bool StepDial(uint8_t* u, int dir, int* stage)
    {
        __try
        {
            if (u[0x30] & 0x2) return false;
            ((void (*)(void*))(GameInfo::Base() + (dir > 0 ? kUsableNext : kUsablePrev)))(u);
            *stage = *(const int*)(u + 0x28);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    struct DialSwipe { void* ent = nullptr; uint8_t* u = nullptr; float acc = 0.f; int pending = 0; double lastAt = 0; float prev[3] = {}; bool prevOk = false; };
    // Sliding windows have no component of their own: they are doors / openables placed by the level. An entity's name
    // (the level's, a char* at entity+0x20, what the game's own errors print through exe+0x1D9090) tells them: one that
    // opens and whose name holds "window" is raised by a hand sliding up on it and lowered by one sliding down -- the
    // game's interact (KbmInject taps it when the pointer has the window: its locks and animation apply).
    std::atomic<bool> g_Windows{ false };
    std::atomic<uint64_t> g_WindowMoves{ 0 };
    struct WinSwipe { void* ent = nullptr; float acc = 0.f; double lastAt = 0, movedAt = 0; float prev[3] = {}; bool prevOk = false; int lastDir = 0; };
    WinSwipe g_Win[2];                          // game thread
    constexpr float kWindowSwipe = 0.06f;       // m of the hand along the window, up or down
    //! The entity's level name into `buf` (printable characters only; empty when unreadable). Kept free of C++ objects:
    //! MSVC refuses __try in a function that has to unwind one (C2712).
    void ReadEntityName(void* ent, char (&buf)[128])
    {
        buf[0] = 0;
        __try
        {
            const char* s = *(const char* const*)((const uint8_t*)ent + 0x20);
            if (!s) return;
            for (int i = 0; i < 127; i++) { const char c = s[i]; buf[i] = 0; if (!c) break; if (c < 32 || c > 126) { buf[0] = 0; break; } buf[i] = c; buf[i + 1] = 0; }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = 0; }
    }
    //! The entity's level name, or "".
    std::string EntityName(void* ent)
    {
        char buf[128];
        ReadEntityName(ent, buf);
        return std::string(buf);
    }
    bool NameHas(const std::string& s, const char* what)
    {
        std::string l = s; for (char& c : l) c = (char)tolower((unsigned char)c);
        return l.find(what) != std::string::npos;
    }
    DialSwipe g_Dial[2];                        // game thread
    constexpr float kDialStep = 0.035f;         // m of the hand along the dial (up / down) per digit
    // after the interact that stores an item: is its entity still in the world (the game refused it: inventory full)?
    std::atomic<uint32_t> g_WatchId{ 0 }; std::atomic<int> g_WatchState{ -1 };
    std::atomic<uint32_t> g_PointerItem{ 0 };      // the pointer's pick when it is an item (game thread -> KbmInject)
    std::mutex g_TouchM;
    Collision::Touch g_TouchPub[2];
    double g_TouchAt[2] = {};
    struct CloseReq { uint32_t id = 0; double at = 0; } g_CloseReq[2];   // under g_TouchM
    std::atomic<bool> g_Openables{ false };
    bool Dynamic(void* ent)
    {
        uint8_t f = 0;
        return ent && SafeRead(&f, (uint8_t*)ent + kEntFlags, 1) && (f & 0x8);
    }

    struct HitLog { void* ent; uint32_t body, cfi; float p[3]; };

    //! Sweep a point from `from` to `goal`, sliding along what it meets; appends the planes met.
    void Sweep(const float from[3], const float goal[3], float r, void* const* skip, int nskip, const float local[3],
               Collision::Contact* out, int& nout, void** ents, HitLog* log, float res[3])
    {
        float cur[3] = { from[0], from[1], from[2] }, target[3] = { goal[0], goal[1], goal[2] };
        for (int it = 0; it < 3; it++)
        {
            float d[3] = { target[0] - cur[0], target[1] - cur[1], target[2] - cur[2] };
            const float L = Len(d);
            if (L < 1e-4f) break;
            const float u[3] = { d[0] / L, d[1] / L, d[2] / L };
            const float end[3] = { target[0] + u[0] * r, target[1] + u[1] * r, target[2] + u[2] * r };
            RayHit h;
            if (!CastRay(cur, end, kWorldLayers, kWorldFlags, skip, nskip, h)) { memcpy(cur, target, 12); break; }
            const float stop = fmaxf(0.f, fminf(L, h.dist - r));
            for (int k = 0; k < 3; k++) cur[k] += u[k] * stop;
            if (nout < kMaxContacts)
            {
                Collision::Contact& c = out[nout];
                memcpy(c.local, local, 12); memcpy(c.p, h.pos, 12); memcpy(c.n, h.n, 12); c.clear = r;
                ents[nout] = h.ent; log[nout] = HitLog{ h.ent, h.body, h.cfi, { h.pos[0], h.pos[1], h.pos[2] } };
                nout++;
            }
            const float dt[3] = { target[0] - h.pos[0], target[1] - h.pos[1], target[2] - h.pos[2] };
            const float pen = Dot(dt, h.n) - r;
            if (pen < 0.f) for (int k = 0; k < 3; k++) target[k] -= h.n[k] * pen;   // slide: the goal onto the plane
        }
        memcpy(res, cur, 12);
    }
}

void Collision::SetHands(bool on) { g_Hands = on; LOGI("Collision: hands {}", on ? "SOLID (they stop at walls, tables and props)" : "pass through everything"); }
void Collision::SetWeapons(bool on) { g_Weapons = on; LOGI("Collision: held weapons {}", on ? "SOLID (the sword and guns stop at the world and at characters' bodies)" : "pass through everything"); }
void Collision::SetPush(bool on, float m)
{
    g_Push = on; g_HandMass = fminf(10.f, fmaxf(0.1f, m));
    LOGI("Collision: pushing loose objects {} (the hand weighs {:.1f} kg)", on ? "on" : "off", g_HandMass.load());
}
void Collision::SetWindows(bool on) { g_Windows = on; LOGI("Collision: sliding windows {}", on ? "rise when a hand slides up on them and lower when it slides down" : "use the interact button"); }
void Collision::SetDials(bool on) { g_Dials = on; LOGI("Collision: safe dials {}", on ? "turn when a hand slides up / down on them" : "turn with the interact button"); }
void Collision::SetItems(bool on) { g_Items = on; LOGI("Collision: items {}", on ? "are taken by hand: grab one, put it behind your back (or food to your mouth)" : "are taken with the interact button"); }
bool Collision::ItemsOn() { return g_Items.load() && g_Hands.load(); }   // a hand must be able to touch them
uint32_t Collision::PointerItem() { return g_PointerItem.load(); }
void Collision::WatchItem(uint32_t id) { g_WatchState = -1; g_WatchId = id; }
int Collision::ItemPresent() { return g_WatchState.load(); }
void Collision::SetOpenables(bool on) { g_Openables = on; LOGI("Collision: drawers and cabinets {}", on ? "open by grabbing and pulling, close by pushing (with HandCollision)" : "use the interact button only"); }
bool Collision::Touched(int hand, Touch& out)
{
    const int h = hand & 1;
    std::lock_guard lk(g_TouchM);
    out = g_TouchPub[h];
    return out.valid && Now() - g_TouchAt[h] < 0.12;
}
bool Collision::TakeCloseRequest(int hand, uint32_t* id)
{
    const int h = hand & 1;
    std::lock_guard lk(g_TouchM);
    if (!g_CloseReq[h].id || Now() - g_CloseReq[h].at > 0.3) { g_CloseReq[h] = CloseReq{}; return false; }
    *id = g_CloseReq[h].id; g_CloseReq[h] = CloseReq{};
    return true;
}
void Collision::SetDoors(bool on) { g_Doors = on; LOGI("Collision: doors {}", on ? "open when a hand pushes them (with HandCollision)" : "only open with the interact button"); }
bool Collision::HandsOn() { return g_Hands.load(); }
bool Collision::WeaponsOn() { return g_Weapons.load(); }
bool Collision::Touching(int hand) { return g_Touch[hand & 1].load(); }

void Collision::Tick()
{
    if (!g_Hands.load() && !g_Weapons.load()) return;
    if (HandAimHook::Mode() < 2) { std::lock_guard lk(g_PubM); g_Pub[0].n = g_Pub[1].n = 0; return; }
    const double now = Now();
    const double dt = g_LastTick > 0 ? now - g_LastTick : 0.0;
    g_LastTick = now;
    g_Ticks++;

    float eye[3], view[9];
    if (!HandAimHook::RealCamera(eye, view)) return;
    // a snap turn or a teleport moves every point at once: start each again from the eye
    const float yaw = atan2f(view[1], view[0]) * 57.29578f;
    bool jump = !g_CamValid || dt <= 0.0 || dt > 0.25;
    if (g_CamValid)
    {
        float dy = yaw - g_LastYaw; while (dy > 180.f) dy -= 360.f; while (dy < -180.f) dy += 360.f;
        const float dp[3] = { eye[0] - g_LastCam[0], eye[1] - g_LastCam[1], eye[2] - g_LastCam[2] };
        if (fabsf(dy) > C.jumpYaw || Len(dp) > C.jumpPos) jump = true;
    }
    g_LastYaw = yaw; memcpy(g_LastCam, eye, 12); g_CamValid = true;
    if (jump) g_Resets++;

    BodyHide::Anchor an[BodyHide::aCount]; BodyHide::GetAnchors(an);
    if (const uint32_t wid = g_WatchId.load())
    {   // the item being stored: gone from the world (taken, eaten) or still there (refused)
        void* we = BodyHide::EntityById((int)wid);
        uint8_t hidden = 0; if (we) SafeRead(&hidden, (uint8_t*)we + 0xD0, 1);
        g_WatchState = (we && !(hidden & 1) && IsItem(we)) ? 1 : 0;
    }
    if (g_Items.load())
    {   // is what the pointer has picked an item (taken only by hand, PhysicalPickup)?
        const int pid = GrabHand::TargetId();
        void* pe = pid > 0 && pid != 0x3FFF0010 ? BodyHide::EntityById(pid) : nullptr;
        g_PointerItem = pe && IsItem(pe) ? (uint32_t)pid : 0u;
    }
    void* skip[8]; int nskip = 0;
    for (int k = 0; k < BodyHide::aCount; k++) if (an[k].valid && an[k].ent) skip[nskip++] = an[k].ent;
    if (void* held = GrabHand::HeldEntity()) skip[nskip++] = held;

    float curl[2] = {};                         // how far each hand is closed (the grip's curl: the drawn fingers' too)
    { XRInputState xin; if (VRRuntime::GetInput(xin)) { curl[0] = xin.gripL; curl[1] = xin.gripR; } }
    for (int h = 0; h < 2; h++)
    {
        HandState& S = g_H[h];
        float hr[9], ht[3];
        if (!GpuTrace::RawHandPose(h, ht, hr)) { S.valid = false; std::lock_guard lk(g_PubM); g_Pub[h].n = 0; g_Touch[h] = false; continue; }
        float wo[3], wa[9]; void* ent = nullptr;
        const bool item = GpuTrace::HandItemDraws(h) > 0 && GpuTrace::RawItemPose(h, wo, wa, &ent);
        Pt pts[kMaxPts];
        const int np = BuildPoints(h, hr, ht, item, wo, wa, ent, curl[h], pts);
        if (np != S.npts || ent != S.item) { S.valid = false; S.npts = np; S.item = ent; }
        if (jump) S.valid = false;

        float goal[kMaxPts][3];
        for (int i = 0; i < np; i++) ToWorld(hr, ht, pts[i].local, goal[i]);

        Contact cs[kMaxContacts]; void* cents[kMaxContacts] = {}; HitLog logs[kMaxContacts] = {}; int nc = 0;
        if (S.ghost)
        {   // passing through: back to solid once the eye sees the palm again (it is out in the open)
            RayHit eh;
            if (np > 0 && !CastRay(eye, goal[0], kWorldLayers, kWorldFlags, skip, nskip, eh)) { S.ghost = false; S.valid = false; }
        }
        if (!S.ghost)
        {
            for (int i = 0; i < np; i++)
            {
                float res[3];
                if (!S.valid)
                {   // from the eye: nothing between the head and the hand, or the hand stops at what is
                    Sweep(eye, goal[i], pts[i].r, skip, nskip, pts[i].local, cs, nc, cents, logs, res);
                }
                else Sweep(S.prevRes[i], goal[i], pts[i].r, skip, nskip, pts[i].local, cs, nc, cents, logs, res);
            }
        }
        float off[3];
        Solve(hr, ht, cs, nc, off);
        const float sep = Len(off);
        if (sep > C.maxSep)
        {   // the real hand went far through: let the drawn one follow it rather than lag behind
            nc = 0; off[0] = off[1] = off[2] = 0.f; if (!S.ghost) g_Ghosts[h]++; S.ghost = true;
        }
        g_LastOff[h] = sep > C.maxSep ? 0.f : sep;

        // doors: pushed by the real hand (moving into it, or pressed past its surface) -- the game's own door push
        if (g_Doors.load() && S.valid && dt > 1e-4 && dt < 0.1 && nc > 0)
        {
            for (int i = 0; i < nc; i++)
            {
                void* e = cents[i];
                if (!e || !IsDoor(e)) continue;
                if (g_Windows.load() && NameHas(EntityName(e), "window")) continue;   // a window moves by sliding, not pushing
                auto it = g_DoorAt.find(e);
                if (it != g_DoorAt.end() && now - it->second < C.doorGap) continue;
                int pi = -1; for (int k = 0; k < np; k++) if (!memcmp(pts[k].local, cs[i].local, 12)) { pi = k; break; }
                if (pi < 0) continue;
                float v[3]; for (int k = 0; k < 3; k++) v[k] = (goal[pi][k] - S.prevRaw[pi][k]) / (float)dt;
                const float into = -Dot(v, cs[i].n);
                const float d[3] = { goal[pi][0] - cs[i].p[0], goal[pi][1] - cs[i].p[1], goal[pi][2] - cs[i].p[2] };
                const float past = cs[i].clear - Dot(d, cs[i].n);    // how far the real point is behind the surface
                if (into < C.doorSpeed && past < C.doorPress) continue;
                if (g_DoorAt.size() > 64) g_DoorAt.clear();
                g_DoorAt[e] = now;
                const bool ok = PushDoor(e, an[BodyHide::aPlayer].valid ? an[BodyHide::aPlayer].ent : nullptr);
                uint32_t id = 0; SafeRead(&id, (uint8_t*)e + kEntId, 4);
                LOGI("Collision: {} hand pushed door {} (id {:#x}) at {:.2f} m/s, {:.0f} cm past its surface: {}",
                     h ? "right" : "left", e, id, into, past * 100.f, ok ? "the game's door push sent" : "COULD NOT SEND");
                VRRuntime::Vibrate(h, 0.35f, 0.04f);
            }
        }
        // a safe's dial under the hand: sliding the hand up turns it on a digit (the game's click), down back one
        if (g_Dials.load() && S.valid)
        {
            DialSwipe& D = g_Dial[h];
            int di = -1; uint8_t* du = nullptr;
            for (int i = 0; i < nc && di < 0; i++) if (cents[i] && (du = DialOf(cents[i])) != nullptr) di = i;
            if (di >= 0)
            {
                int pi = -1; for (int k = 0; k < np; k++) if (!memcmp(pts[k].local, cs[di].local, 12)) { pi = k; break; }
                if (D.ent != cents[di] || now - D.lastAt > 0.3) { D = DialSwipe{}; D.ent = cents[di]; D.u = du; }
                if (pi >= 0)
                {
                    if (D.prevOk) D.acc += goal[pi][2] - D.prev[2];       // world up (z)
                    memcpy(D.prev, goal[pi], 12); D.prevOk = true;
                    while (D.acc >= kDialStep && D.pending < 3) { D.pending++; D.acc -= kDialStep; }
                    while (D.acc <= -kDialStep && D.pending > -3) { D.pending--; D.acc += kDialStep; }
                    if (D.acc > kDialStep || D.acc < -kDialStep) D.acc = 0.f;   // more queued than it can take: drop the rest
                }
                D.lastAt = now;
            }
            if (D.u && D.pending != 0 && now - D.lastAt < 1.0)
            {   // one step a frame, as soon as the dial has finished turning
                int stage = -1;
                if (StepDial(D.u, D.pending > 0 ? 1 : -1, &stage))
                {
                    LOGI("Collision: {} hand turned a safe dial {} -> {}", h ? "right" : "left", D.pending > 0 ? "up" : "down", stage);
                    D.pending += D.pending > 0 ? -1 : 1; g_DialSteps++;
                    VRRuntime::Vibrate(h, 0.2f, 0.015f);
                }
            }
            else if (now - D.lastAt >= 1.0) D = DialSwipe{};
        }
        // a sliding window under the (grabbing) hand: slide up raises it, slide down lowers it
        if (g_Windows.load() && S.valid && h == GrabHand::Hand())
        {
            WinSwipe& W = g_Win[h];
            int wi = -1; int wstate = -1;
            for (int i = 0; i < nc && wi < 0; i++)
            {
                void* e = cents[i];
                if (!e) continue;
                const int os = OpenableState(e);
                if ((os >= 0 || IsDoor(e)) && NameHas(EntityName(e), "window")) { wi = i; wstate = os; }
            }
            if (wi >= 0)
            {
                int pi = -1; for (int k = 0; k < np; k++) if (!memcmp(pts[k].local, cs[wi].local, 12)) { pi = k; break; }
                if (W.ent != cents[wi] || now - W.lastAt > 0.3) { W = WinSwipe{}; W.ent = cents[wi]; }
                if (pi >= 0)
                {
                    if (W.prevOk) W.acc += goal[pi][2] - W.prev[2];
                    memcpy(W.prev, goal[pi], 12); W.prevOk = true;
                }
                W.lastAt = now;
                const int dir = W.acc >= kWindowSwipe ? 1 : (W.acc <= -kWindowSwipe ? -1 : 0);
                if (dir && now - W.movedAt > 1.0)
                {   // up opens a closed one, down closes an open one; with no open state to read (a door), every swipe
                    // toggles, but not twice the same way in a row
                    const bool want = wstate >= 0 ? (dir > 0 ? wstate == 0 : wstate == 1) : dir != W.lastDir;
                    if (want)
                    {
                        uint32_t id = 0; SafeRead(&id, (uint8_t*)cents[wi] + kEntId, 4);
                        { std::lock_guard lk(g_TouchM); g_CloseReq[h] = CloseReq{ id, now }; }
                        W.movedAt = now; W.lastDir = dir; g_WindowMoves++;
                        LOGI("Collision: {} hand slid {} on window '{}' (id {:#x}, {}): interact", h ? "right" : "left", dir > 0 ? "UP" : "DOWN",
                             EntityName(cents[wi]), id, wstate < 0 ? "a door" : wstate ? "open" : "closed");
                    }
                    W.acc = 0.f;
                }
            }
        }
        // what this hand touches (the first contact with an entity), for KbmInject's grab-and-pull on drawers and cabinets;
        // pushing an OPEN one (not a door: those have their own push) asks for the interact that closes it
        {
            Touch tch;
            for (int i = 0; i < nc && !tch.valid; i++)
            {
                void* e = cents[i];
                if (!e) continue;
                tch.valid = true; tch.ent = e; memcpy(tch.n, cs[i].n, 12); memcpy(tch.p, cs[i].p, 12);
                tch.item = g_Items.load() && IsItem(e);
                SafeRead(&tch.id, (uint8_t*)e + kEntId, 4);
                const int os = g_Openables.load() ? OpenableState(e) : -1;
                tch.openable = os >= 0; tch.open = os == 1;
                if (tch.open && !IsDoor(e) && !(g_Windows.load() && NameHas(EntityName(e), "window")) && S.valid && dt > 1e-4 && dt < 0.1)
                {
                    int pi = -1; for (int k = 0; k < np; k++) if (!memcmp(pts[k].local, cs[i].local, 12)) { pi = k; break; }
                    if (pi >= 0)
                    {
                        float v[3]; for (int k = 0; k < 3; k++) v[k] = (goal[pi][k] - S.prevRaw[pi][k]) / (float)dt;
                        const float into = -Dot(v, cs[i].n);
                        const float d[3] = { goal[pi][0] - cs[i].p[0], goal[pi][1] - cs[i].p[1], goal[pi][2] - cs[i].p[2] };
                        const float past = cs[i].clear - Dot(d, cs[i].n);
                        auto it = g_DoorAt.find(e);
                        if ((into >= C.doorSpeed || past >= C.doorPress) && (it == g_DoorAt.end() || now - it->second >= C.doorGap))
                        {
                            g_DoorAt[e] = now;
                            std::lock_guard lk(g_TouchM); g_CloseReq[h] = CloseReq{ tch.id, now };
                            LOGI("Collision: {} hand pushed an open drawer / cabinet {} (id {:#x}) at {:.2f} m/s: close it", h ? "right" : "left", e, tch.id, into);
                        }
                    }
                }
            }
            std::lock_guard lk(g_TouchM); g_TouchPub[h] = tch; g_TouchAt[h] = now;
        }
        // pushes: what the real hand drives into a loose object (relative to the object's own velocity)
        if (g_Push.load() && S.valid && dt > 1e-4 && dt < 0.1 && nc > 0)
        {
            void* pushed[kMaxContacts]; int npushed = 0;
            for (int i = 0; i < nc; i++)
            {
                void* e = cents[i];
                if (!Dynamic(e) || IsDoor(e) || OpenableState(e) >= 0) continue;
                bool dup = false; for (int k = 0; k < npushed; k++) if (pushed[k] == e) dup = true;
                if (dup) continue;
                int pi = -1; for (int k = 0; k < np; k++) if (!memcmp(pts[k].local, cs[i].local, 12)) { pi = k; break; }
                if (pi < 0) continue;
                float v[3]; for (int k = 0; k < 3; k++) v[k] = (goal[pi][k] - S.prevRaw[pi][k]) / (float)dt;
                float vb[3] = {}; SafeRead(vb, (uint8_t*)e + kEntVelocity, 12); if (!Finite(vb, 3)) memset(vb, 0, 12);
                const float rel[3] = { v[0] - vb[0], v[1] - vb[1], v[2] - vb[2] };
                const float into = fminf(C.pushMaxV, -Dot(rel, cs[i].n));
                if (into < C.pushMin) continue;
                const float J = g_HandMass.load() * into;
                const float imp[3] = { -cs[i].n[0] * J, -cs[i].n[1] * J, -cs[i].n[2] * J };
                PostImpulse(e, imp, cs[i].p);
                pushed[npushed++] = e;
            }
        }
        // a buzz on touching, as hard as the hand came in
        const bool touching = nc > 0;
        if (touching && !S.contact && S.valid && dt > 1e-4 && now - S.lastBuzz > C.buzzGap)
        {
            float vn = 0.f;
            for (int i = 0; i < nc; i++)
                for (int k = 0; k < np; k++)
                    if (!memcmp(pts[k].local, cs[i].local, 12))
                    {
                        float v[3]; for (int j = 0; j < 3; j++) v[j] = (goal[k][j] - S.prevRaw[k][j]) / (float)dt;
                        vn = fmaxf(vn, -Dot(v, cs[i].n));
                    }
            VRRuntime::Vibrate(h, fminf(C.buzzMax, C.buzzMin + 0.25f * vn), 0.025f);
            S.lastBuzz = now;
            g_Stops[h]++;
            std::lock_guard lk(g_DescM);
            g_LastHitDesc = fmt::format("hand {} met entity {} '{}' body {:#x} layer {} flags {:#x} at ({:.2f},{:.2f},{:.2f}){} {:.2f} m/s",
                                        h, logs[0].ent, logs[0].ent ? EntityName(logs[0].ent) : std::string(), logs[0].body, logs[0].cfi & 0x1F, logs[0].cfi >> 5, logs[0].p[0], logs[0].p[1], logs[0].p[2],
                                        Dynamic(logs[0].ent) ? " (loose: pushed)" : "", vn);
            LOGI("Collision: {}", g_LastHitDesc);
        }
        S.contact = touching;
        g_Touch[h] = touching;

        for (int i = 0; i < np; i++)
            for (int k = 0; k < 3; k++) { S.prevRaw[i][k] = goal[i][k]; S.prevRes[i][k] = goal[i][k] + off[k]; }
        S.valid = np > 0;
        std::lock_guard lk(g_PubM);
        Pub& P = g_Pub[h]; P.n = nc; P.at = now;
        for (int i = 0; i < nc; i++) P.c[i] = cs[i];
    }
}

bool Collision::Apply(int hand, const float r[9], float t[3], float applied[3])
{
    const int h = hand & 1;
    applied[0] = applied[1] = applied[2] = 0.f;
    if (!g_Hands.load() && !g_Weapons.load()) return false;
    Contact cs[2 * kMaxContacts]; int n = 0;
    {
        const double now = Now();
        std::lock_guard lk(g_PubM);
        if (g_Pub[h].n && now - g_Pub[h].at < C.stale) for (int i = 0; i < g_Pub[h].n; i++) cs[n++] = g_Pub[h].c[i];
        if (g_Weapons.load() && g_Body[h].n && now - g_Body[h].at < C.stale) for (int i = 0; i < g_Body[h].n; i++) cs[n++] = g_Body[h].c[i];
    }
    if (!n) return false;
    float off[3];
    Solve(r, t, cs, n, off);
    const float sep = Len(off);
    if (sep < 1e-5f || sep > C.maxSep || !Finite(off, 3)) return false;
    for (int k = 0; k < 3; k++) { t[k] += off[k]; applied[k] = off[k]; }
    return true;
}

void Collision::SetBodyContacts(int hand, const Contact* c, int n)
{
    const int h = hand & 1;
    if (n > kMaxContacts) n = kMaxContacts;
    std::lock_guard lk(g_PubM);
    Pub& P = g_Body[h];
    if (n > 0 && P.n == 0) g_BodyStops++;
    P.n = n; P.at = Now();
    for (int i = 0; i < n; i++) P.c[i] = c[i];
}

std::string Collision::Stats()
{
    std::ostringstream o;
    o << "hands " << g_Hands.load() << " weapons " << g_Weapons.load() << " push " << g_Push.load() << " ticks " << g_Ticks.load()
      << " casts " << g_Casts.load() << " hits " << g_CastHits.load() << " faults " << g_CastFaults.load() << " skipped " << g_Skipped.load()
      << " stops L " << g_Stops[0].load() << " R " << g_Stops[1].load() << " ghosts L " << g_Ghosts[0].load() << " R " << g_Ghosts[1].load()
      << " touching L " << g_Touch[0].load() << " R " << g_Touch[1].load() << " off L " << g_LastOff[0].load() << " R " << g_LastOff[1].load()
      << " pushes " << g_Pushes.load() << " doors " << g_DoorPushes.load() << " dialsteps " << g_DialSteps.load() << " windows " << g_WindowMoves.load() << " bodystops " << g_BodyStops.load() << " resets " << g_Resets.load();
    std::lock_guard lk(g_DescM);
    if (!g_LastHitDesc.empty()) o << " | last: " << g_LastHitDesc;
    return o.str();
}
