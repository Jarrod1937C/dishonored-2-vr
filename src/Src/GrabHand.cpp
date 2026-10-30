#include "pch.h"
#include "GrabHand.h"
#include "GameInfo.h"
#include "HandAimHook.h"
#include "HandFx.h"
#include "BodyHide.h"
#include "PlayerState.h"
#include "Game.h"
#include "ThrowGesture.h"
#include "PointerRay.h"
#include "GpuTrace.h"
#include "ViewMatrixHook.h"
#include <detours/detours.h>
#include <cmath>
#include <sstream>

namespace
{
    constexpr uint32_t kHeldIdInState = 0x68;     // arkPlayerStateHoldObject: spawn id of the held entity
    constexpr uint32_t kEntityOrigin = 0x300, kEntityAxis = 0x30C, kEntityVelocity = 0x33C;

    struct Cfg
    {
        // metres: an object picked up farther from the hand than this is brought to this distance (same direction)
        float maxRel = 0.15f;
        // metres from the player's vertical axis (its origin, the feet): the PHYSICS target is kept at least this far
        // out, so the held body never overlaps the player's capsule. The drawn object stays on the hand. 0 = off.
        float clear = 0.45f;
        // metres from the EYE: the physics target is kept out of this sphere (the game's own hold point sits 0.55 m
        // ahead and 0.25 m down; a body at the face is in the camera's way). The drawn object stays on the hand.
        float eyeClear = 0.45f;
        // m/s (the hand, in the XR space): letting go faster than this THROWS (the game's "Lanzar", with the hand's
        // velocity), slower DROPS (the game's "Soltar"). A throw at any speed is a thrown object, and a thrown bottle
        // shatters on its first contact (measured: let go still, it broke on the bottle 7 cm below); a drop does not.
        float dropSpeed = 1.0f;
        // The SNAP (1, default): on taking it, the object glides in `snapTime` seconds from where it was to a fixed
        // place in the hand -- its centre `snapPalm` metres out of the palm (the drawn empty hand's palm faces the
        // body's midline: the controller's left for the right hand), `snapFwd`/`snapUp` along the controller -- turned
        // so that what was UP in the world when it was taken (a bottle or a glass standing on a table: its own up)
        // runs along the fist, the controller's up, tilted `snapTilt` degrees towards the controller's forward.
        // 0 = it stays where it was taken (part 21; `maxRel` applies then).
        // Most props have no centre the game knows (a bottle: no such component; the game holds it by its origin)
        // and their origin is their BASE, so the palm goes `snapHeight` metres up the object from it.
        float snap = 2.f, snapTime = 0.15f, snapPalm = 0.05f, snapFwd = 0.f, snapUp = 0.f, snapTilt = 0.f, snapHeight = 0.07f;
        // snap 2 (default), the FIT: the object's own box (its render model's bounds) decides how it sits in the DRAWN
        // fist. Its centre goes to the fist's hole -- `fitDepth` metres down the hole's axis from where the sword's guard
        // sits in that fist -- pushed out of the palm by whatever of its thickness exceeds `fitHoleR`; an object at least
        // `fitLong` times longer than wide runs THROUGH the fist, the end that was up coming out by the thumb, and turns
        // its thinner side to the palm when one side is `fitFlat` times the other (a flask lies flat in the hand).
        // One round in section (two sizes within `fitRound` of each other, the third off by more than that) runs its axis
        // of symmetry through the fist instead (a glass). Anything else goes in as it stood: up by the thumb.
        float fitDepth = 0.045f, fitHoleR = 0.018f, fitLong = 1.5f, fitFlat = 1.25f, fitRound = 1.1f;
    } C;
    struct Named { const char* name; float* v; };
    const Named kCfg[] = { { "maxrel", &C.maxRel }, { "clear", &C.clear }, { "eyeclear", &C.eyeClear }, { "dropspeed", &C.dropSpeed },
                           { "snap", &C.snap }, { "snaptime", &C.snapTime }, { "snappalm", &C.snapPalm }, { "snapfwd", &C.snapFwd },
                           { "snapup", &C.snapUp }, { "snaptilt", &C.snapTilt }, { "snapheight", &C.snapHeight },
                           { "fitdepth", &C.fitDepth }, { "fitholer", &C.fitHoleR }, { "fitlong", &C.fitLong }, { "fitflat", &C.fitFlat }, { "fitround", &C.fitRound } };

    std::atomic<bool> g_On{ true };
    std::atomic<int> g_Hand{ 1 };                  // the hand that takes and holds: 0 = left, 1 = right (default)
    std::atomic<int> g_ReleaseMode{ 2 };           // how it is let go: see GrabHand::SetReleaseMode
    const char* HandName(int h) { return h ? "right" : "left"; }
    std::atomic<void*> g_Held{ nullptr };          // the entity being held (Present thread writes, hooks read)
    std::atomic<const float*> g_OffPtr{ nullptr }; // the bind's offset: the render placement queries with the same one
    int g_LastId = 0;
    std::mutex g_M;                                // the relative pose and the pre-hold pose
    bool g_RelValid = false;
    float g_RelPos[3] = {}, g_RelRot[9] = {};      // object in the controller's frame: position, rows = object basis
    // The target's pose while it is only targeted (written at Present): the hold starts from it, because by the
    // first query of the hold the game has already pulled the object towards its own hold point (measured: 0.49 m
    // from the hand for a bottle the palm was touching).
    void* g_PreEnt = nullptr; float g_PrePos[3] = {}, g_PreRot[9] = {}; ULONGLONG g_PreAt = 0;
    // The snap: the held object's centre in its own axes (from the game's own centre, see ObjectCenter), and the
    // glide from where it was taken (g_RelPos/g_RelRot, as captured) to its place in the hand.
    bool g_CenterValid = false; float g_CenterLocal[3] = {};
    bool g_Snapping = false; float g_SnapPos[3] = {}, g_SnapRot[9] = {}; LONGLONG g_SnapT0 = 0;
    float g_SnapUpCtrl[3] = {}, g_SnapUpObj[3] = {};   // world up at the capture: in the controller's frame, in the object's
    std::atomic<unsigned long long> g_Holds{ 0 }, g_Matches{ 0 }, g_Applied{ 0 }, g_NoPose{ 0 }, g_NoModel{ 0 }, g_Cleared{ 0 }, g_Jumps{ 0 }, g_EyeCleared{ 0 };
    float g_LastTarget[3] = {}, g_CapturedDist = 0, g_LastAxisDist = 0, g_LastEyeDist = 0; bool g_CapturedPre = false;
    void* g_FlightEnt = nullptr; uintptr_t g_FlightVt = 0; float g_FlightFrom[3] = {}; double g_FlightStart = 0;   // Present thread
    // What is drawn: the pose the render placement query was last answered with, and the held object's local box.
    std::mutex g_DrawM;
    float g_DrawPos[3] = {}, g_DrawRot[9] = {}; ULONGLONG g_DrawAt = 0;
    bool g_BoxValid = false; float g_BoxMin[3] = {}, g_BoxMax[3] = {};

    bool Readable(const void* p, size_t n)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    // ---- rotations for the snap (3x3, row-major; a basis given as rows is a rotation matrix like any other) ----
    void MatToQuat(const float* m, float q[4])
    {
        const float tr = m[0] + m[4] + m[8];
        if (tr > 0.f) { const float s = sqrtf(tr + 1.f) * 2.f; q[0] = 0.25f * s; q[1] = (m[7] - m[5]) / s; q[2] = (m[2] - m[6]) / s; q[3] = (m[3] - m[1]) / s; }
        else if (m[0] > m[4] && m[0] > m[8]) { const float s = sqrtf(1.f + m[0] - m[4] - m[8]) * 2.f; q[0] = (m[7] - m[5]) / s; q[1] = 0.25f * s; q[2] = (m[1] + m[3]) / s; q[3] = (m[2] + m[6]) / s; }
        else if (m[4] > m[8]) { const float s = sqrtf(1.f + m[4] - m[0] - m[8]) * 2.f; q[0] = (m[2] - m[6]) / s; q[1] = (m[1] + m[3]) / s; q[2] = 0.25f * s; q[3] = (m[5] + m[7]) / s; }
        else { const float s = sqrtf(1.f + m[8] - m[0] - m[4]) * 2.f; q[0] = (m[3] - m[1]) / s; q[1] = (m[2] + m[6]) / s; q[2] = (m[5] + m[7]) / s; q[3] = 0.25f * s; }
    }
    void QuatToMat(const float* q, float* m)
    {
        const float w = q[0], x = q[1], y = q[2], z = q[3];
        m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - w * z);     m[2] = 2 * (x * z + w * y);
        m[3] = 2 * (x * y + w * z);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - w * x);
        m[6] = 2 * (x * z - w * y);     m[7] = 2 * (y * z + w * x);     m[8] = 1 - 2 * (x * x + y * y);
    }
    //! The rotation between two bases (rows), a fraction t of the way, by the shortest path.
    void SlerpRot(const float* a, const float* b, float t, float* out)
    {
        float qa[4], qb[4], q[4]; MatToQuat(a, qa); MatToQuat(b, qb);
        float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
        if (d < 0.f) { for (float& x : qb) x = -x; d = -d; }
        float wa = 1.f - t, wb = t;
        if (d < 0.9995f) { const float th = acosf(d), sn = sinf(th); wa = sinf((1.f - t) * th) / sn; wb = sinf(t * th) / sn; }
        float n = 0.f; for (int i = 0; i < 4; i++) { q[i] = wa * qa[i] + wb * qb[i]; n += q[i] * q[i]; }
        n = sqrtf(n); for (float& x : q) x /= n;
        QuatToMat(q, out);
    }
    //! The rotation taking unit vector a onto unit vector b by the shortest arc, applied to v.
    void RotateArc(const float* a, const float* b, const float* v, float* out)
    {
        const float c = Dot(a, b);
        float k[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
        if (c < -0.9999f)
        {   // opposite: half a turn about any axis perpendicular to a
            const float t[3] = { fabsf(a[0]) < 0.9f ? 1.f : 0.f, fabsf(a[0]) < 0.9f ? 0.f : 1.f, 0.f };
            k[0] = a[1] * t[2] - a[2] * t[1]; k[1] = a[2] * t[0] - a[0] * t[2]; k[2] = a[0] * t[1] - a[1] * t[0];
            const float n = sqrtf(Dot(k, k)); for (float& x : k) x /= n;
            const float kv = Dot(k, v);
            for (int i = 0; i < 3; i++) out[i] = 2.f * kv * k[i] - v[i];
            return;
        }
        // Rodrigues with k = a x b (|k| = sin): v + k x v + k x (k x v) / (1 + cos)
        const float kxv[3] = { k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0] };
        const float kxkxv[3] = { k[1] * kxv[2] - k[2] * kxv[1], k[2] * kxv[0] - k[0] * kxv[2], k[0] * kxv[1] - k[1] * kxv[0] };
        for (int i = 0; i < 3; i++) out[i] = v[i] + kxv[i] + kxkxv[i] / (1.f + c);
    }

    // ---- rigid poses, idMat3 convention: a[] rows are the frame's fwd/left/up axes in world ----
    struct Pose { float o[3] = {}; float a[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; };
    //! From GpuTrace's Rigid layout (basis in the COLUMNS of r).
    Pose FromCols(const float* t, const float* r)
    {
        Pose p; memcpy(p.o, t, 12);
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) p.a[j * 3 + i] = r[i * 3 + j];
        return p;
    }
    //! B expressed in A's frame.
    Pose Rel(const Pose& A, const Pose& B)
    {
        Pose r; const float d[3] = { B.o[0] - A.o[0], B.o[1] - A.o[1], B.o[2] - A.o[2] };
        for (int k = 0; k < 3; k++) r.o[k] = Dot(d, A.a + 3 * k);
        for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) r.a[i * 3 + k] = Dot(B.a + 3 * i, A.a + 3 * k);
        return r;
    }
    //! A frame given in A's frame (rel), in world.
    Pose Compose(const Pose& A, const Pose& rel)
    {
        Pose w;
        for (int j = 0; j < 3; j++) w.o[j] = A.o[j] + rel.o[0] * A.a[j] + rel.o[1] * A.a[3 + j] + rel.o[2] * A.a[6 + j];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
            w.a[i * 3 + j] = rel.a[i * 3] * A.a[j] + rel.a[i * 3 + 1] * A.a[3 + j] + rel.a[i * 3 + 2] * A.a[6 + j];
        return w;
    }
    float AngleDeg(const float* a9) { const float c = (a9[0] + a9[4] + a9[8] - 1.f) * 0.5f; return acosf(c > 1.f ? 1.f : c < -1.f ? -1.f : c) * 57.29578f; }
    //! The controller as the hand is DRAWN from it now: anchored to where the next view will be rendered from (the
    //! last view's offset from the game eye carried onto the game eye of this instant), as GunRay places the gun.
    //! ControllerPose anchors to the GAME camera instead, which is off the drawn hand by the head's room offset.
    bool HandNow(int hand, Pose& out)
    {
        float eye[3], cax[9], vorg[3], gorg[3], hmd[3];
        if (!HandAimHook::RealCamera(eye, cax) || !ViewMatrixHook::ViewAnchor(vorg, gorg, hmd)) return false;
        const float o[3] = { vorg[0] + eye[0] - gorg[0], vorg[1] + eye[1] - gorg[1], vorg[2] + eye[2] - gorg[2] };
        return HandAimHook::ControllerPoseAt(hand, o, cax, hmd, out.o, out.a);
    }
    void Normalize(float* v) { const float n = sqrtf(Dot(v, v)); if (n > 1e-6f) for (int i = 0; i < 3; i++) v[i] /= n; }
    void Orthonormalize(float* a)
    {
        Normalize(a);
        const float d = Dot(a + 3, a); for (int i = 0; i < 3; i++) a[3 + i] -= d * a[i];
        Normalize(a + 3);
        a[6] = a[1] * a[5] - a[2] * a[4]; a[7] = a[2] * a[3] - a[0] * a[5]; a[8] = a[0] * a[4] - a[1] * a[3];
    }

    // ---- the fist, measured on the DRAWN hand (Present thread writes, the joint queries read) ----
    // Nothing here is a per-object number: the hand is always the same fist, and the object is fitted into it.
    std::mutex g_FitM;
    // The drawn palm in the drawn controller's frame while an object is held. NOT the weapon hold: with the sword the
    // palm sits 7 cm from the controller, holding an object (the empty hand's anatomical pose) on it -- which is why
    // a place in the hand cannot be a fixed offset from the controller. Default = measured 2026-10-04 (Corvo, right).
    Pose g_PalmInCtrl = { { 0.f, 0.f, 0.f }, { -1.00f, -0.04f, -0.06f, 0.05f, -0.97f, -0.24f, -0.05f, -0.24f, 0.97f } };
    // The fist's hole, in the palm's frame: where the sword's guard and blade sit while that hand holds the sword (the
    // fist a held object gets is the one learned from that grip, so the handle marks the hole). Learned live; default =
    // measured. And the palm's normal (out of the palm), in the palm's frame.
    float g_GuardInPalm[3] = { -0.052f, -0.025f, 0.030f }, g_BladeInPalm[3] = { -0.27f, -0.06f, 0.96f };
    float g_NormalInPalm[3] = { -0.13f, -0.97f, -0.24f };
    unsigned long long g_PalmSamples = 0, g_GuardSamples = 0;
    //! Per Present. While holding: where the empty hand's palm is drawn against its controller. While the right hand
    //! holds the sword (and nothing else): where the sword is in that palm.
    void LearnFist()
    {
        const int hand = g_Hand.load();
        float t[3], r[9], fing[3], pn[3];
        if (!GpuTrace::PalmFrame(hand, t, r, fing, pn)) return;
        const Pose palm = FromCols(t, r);
        if (g_Held.load())
        {
            float ct[3], cr[9];
            if (!GpuTrace::HandPoseWorld(hand, ct, cr)) return;
            Pose rel = Rel(FromCols(ct, cr), palm);
            if (Dot(rel.o, rel.o) > 0.15f * 0.15f) return;
            Orthonormalize(rel.a);
            float n[3]; for (int k = 0; k < 3; k++) n[k] = Dot(pn, palm.a + 3 * k);
            Normalize(n);
            std::lock_guard lk(g_FitM); g_PalmInCtrl = rel; memcpy(g_NormalInPalm, n, 12); g_PalmSamples++;
            return;
        }
        if (hand != 1 || GpuTrace::HandItemDraws(hand) <= 0 || GpuTrace::AnimSuspended()) return;
        float wo[3], wa[9];
        if (!GpuTrace::WeaponTargetPose(hand, wo, wa)) return;
        Pose w; memcpy(w.o, wo, 12); memcpy(w.a, wa, 36);
        const Pose rel = Rel(palm, w);
        if (Dot(rel.o, rel.o) > 0.12f * 0.12f) return;
        std::lock_guard lk(g_FitM);
        const float k = g_GuardSamples ? 0.05f : 1.f;
        for (int i = 0; i < 3; i++) { g_GuardInPalm[i] += k * (rel.o[i] - g_GuardInPalm[i]); g_BladeInPalm[i] += k * (rel.a[i] - g_BladeInPalm[i]); }
        Normalize(g_BladeInPalm);
        g_GuardSamples++;
    }

    // ---- the object's own size: its render model's bounds, in the entity's axes ----
    // exe+0x8780D0 (EntityBounds below) builds the world AABB from the render model (entity+0xF0): vtable +0x80 returns
    // its local bounds (min xyz, max xyz), which exe+0xFB8C0 scales per axis by entity+0x330 and turns by +0x30C.
    constexpr uint32_t kModelBoundsSlot = 0x80, kEntityScale = 0x330;
    bool LocalBounds(void* ent, float mn[3], float mx[3])
    {
        const uint8_t* e = (const uint8_t*)ent;
        if (!e || !Readable(e + 0xF0, 8) || !Readable(e + kEntityScale, 12)) return false;
        void* model = *(void* const*)(e + 0xF0);
        if (!model || !Readable(model, 8)) return false;
        const uintptr_t vt = *(const uintptr_t*)model, base = GameInfo::Base();
        if (vt < base || vt - base > 0x4000000 || !Readable((void*)(vt + kModelBoundsSlot), 8)) return false;
        using BoundsFn = const float* (*)(void*);
        const float* b = ((BoundsFn)(*(const uintptr_t*)(vt + kModelBoundsSlot)))(model);
        if (!b || !Readable(b, 24)) return false;
        const float* s = (const float*)(e + kEntityScale);
        for (int i = 0; i < 3; i++)
        {
            const float sc = std::isfinite(s[i]) && fabsf(s[i]) > 1e-3f && fabsf(s[i]) < 100.f ? s[i] : 1.f;
            mn[i] = b[i] * sc; mx[i] = b[3 + i] * sc;
            if (mn[i] > mx[i]) std::swap(mn[i], mx[i]);
            if (!std::isfinite(mn[i]) || !std::isfinite(mx[i]) || mx[i] - mn[i] > 5.f) return false;
        }
        return true;
    }

    // ---- the object's centre, as the game takes it ----
    // arkPlayerStateHoldObject's enter (exe+0x83D86F) gets a component of the held entity (typeinfo exe+0x39B4630)
    // and calls exe+0x731470(component, out): the mean WORLD position of the component's physics bodies (one joint
    // query each, exe+0x53D940). It then holds the object by that point: the bind's offset is the decl's hold offset
    // plus (origin - centre) turned into the hold. Measured: a bottle's origin is its BASE (it rests with the origin
    // on the table), so holding it by the origin would put the hand at its foot.
    constexpr uint32_t kCenterCpntTypeInfo = 0x39B4630, kObjectCenter = 0x731470;
    bool ObjectCenter(void* ent, float out[3])
    {
        using GetComponentFn = void* (*)(void*, void*, int);
        using CenterFn = float* (*)(void*, float*);
        const uintptr_t base = GameInfo::Base();
        void* c = ((GetComponentFn)(base + BodyHide::kGetComponent))(ent, (void*)(base + kCenterCpntTypeInfo), 2);
        if (!c || !Readable(c, 0x150)) { LOGI("GrabHand: no centre component on {} ({})", ent, c); return false; }
        ((CenterFn)(base + kObjectCenter))(c, out);
        const float* o = (const float*)((const uint8_t*)ent + kEntityOrigin);
        const float d[3] = { out[0] - o[0], out[1] - o[1], out[2] - o[2] };
        const bool ok = std::isfinite(d[0]) && std::isfinite(d[1]) && std::isfinite(d[2]) && Dot(d, d) < 1.f;
        if (!ok) LOGI("GrabHand: the centre component {} gave ({:.3f},{:.3f},{:.3f}), origin ({:.3f},{:.3f},{:.3f}): rejected", c, out[0], out[1], out[2], o[0], o[1], o[2]);
        return ok;
    }
    //! The object's place in the hand (g_SnapPos/g_SnapRot, in the controller's frame) from the capture and the
    //! current snap* config. Under g_M.
    void SnapTarget(int hand)
    {
        const float tilt = C.snapTilt * 3.14159265f / 180.f;
        const float fist[3] = { sinf(tilt), 0.f, cosf(tilt) };   // the controller's up, tilted towards its forward
        for (int i = 0; i < 3; i++) RotateArc(g_SnapUpCtrl, fist, g_RelRot + 3 * i, g_SnapRot + 3 * i);
        // the point held on the palm: the right hand's palm faces the controller's left, the left hand's its right
        const float palm[3] = { C.snapFwd, (hand ? 1.f : -1.f) * C.snapPalm, C.snapUp };
        // the point held, in the object's axes: the game's centre, or snapHeight up from its base
        float held[3];
        for (int i = 0; i < 3; i++) held[i] = g_CenterValid ? g_CenterLocal[i] : g_SnapUpObj[i] * C.snapHeight;
        for (int k = 0; k < 3; k++)
        {
            float c = 0.f; for (int i = 0; i < 3; i++) c += held[i] * g_SnapRot[i * 3 + k];
            g_SnapPos[k] = palm[k] - c;
        }
    }
    //! The FIT (snap 2): the same output as SnapTarget (the object's origin and axes in the controller's frame), from the
    //! object's box and the drawn fist instead of fixed offsets. Under g_M, with g_BoxValid.
    const char* g_FitRule = "";   // which rule the last fit took, for the log
    void FitTarget()
    {
        Pose pc; float guard[3], blade[3], nrm[3];
        {
            std::lock_guard lk(g_FitM);
            pc = g_PalmInCtrl; memcpy(guard, g_GuardInPalm, 12); memcpy(blade, g_BladeInPalm, 12); memcpy(nrm, g_NormalInPalm, 12);
        }
        // the hole's centre and axis (towards the thumb) and the palm's normal, in the controller's frame
        float hole[3], ax[3], n[3];
        for (int j = 0; j < 3; j++)
        {
            float h = pc.o[j];
            for (int k = 0; k < 3; k++) h += (guard[k] - blade[k] * C.fitDepth) * pc.a[k * 3 + j];
            hole[j] = h;
            ax[j] = blade[0] * pc.a[j] + blade[1] * pc.a[3 + j] + blade[2] * pc.a[6 + j];
            n[j] = nrm[0] * pc.a[j] + nrm[1] * pc.a[3 + j] + nrm[2] * pc.a[6 + j];
        }
        Normalize(ax);
        { const float d = Dot(n, ax); for (int j = 0; j < 3; j++) n[j] -= d * ax[j]; Normalize(n); }   // across the hole
        float he[3], cl[3];
        for (int i = 0; i < 3; i++) { he[i] = 0.5f * (g_BoxMax[i] - g_BoxMin[i]); cl[i] = 0.5f * (g_BoxMax[i] + g_BoxMin[i]); }
        int L = he[0] >= he[1] && he[0] >= he[2] ? 0 : he[1] >= he[2] ? 1 : 2, A = (L + 1) % 3, B = (L + 2) % 3;
        bool longish = he[L] >= C.fitLong * std::max(he[A], he[B]);
        g_FitRule = longish ? "long: through the fist" : "as it stood: up by the thumb";
        if (!longish)
        {   // round in section (a glass, a jar: two sizes alike, the third not): its axis of symmetry runs through the
            // fist the same way, which is how a glass is held
            for (int k = 0; k < 3 && !longish; k++)
            {
                const int a = (k + 1) % 3, b = (k + 2) % 3;
                const float lo = std::min(he[a], he[b]), hi = std::max(he[a], he[b]), odd = he[k];
                if (hi <= C.fitRound * lo && fabsf(odd - 0.5f * (lo + hi)) >= (C.fitRound - 1.f) * hi) { L = k; A = a; B = b; longish = true; g_FitRule = "round: its axis through the fist"; }
            }
        }
        float R[9]; memcpy(R, g_RelRot, 36);
        if (!longish)
        {   // no axis to go by: what was up in the world when it was taken goes up the fist, by the thumb
            float R2[9]; for (int i = 0; i < 3; i++) RotateArc(g_SnapUpCtrl, ax, R + 3 * i, R2 + 3 * i);
            memcpy(R, R2, 36);
        }
        else
        {   // through the fist: the axis onto the hole's, the end that was up (or, lying down, the nearer end) by the thumb
            float u[3] = { R[L * 3], R[L * 3 + 1], R[L * 3 + 2] };
            const float up = Dot(u, g_SnapUpCtrl);
            if (fabsf(up) > 0.3f ? up < 0.f : Dot(u, ax) < 0.f) for (float& x : u) x = -x;
            float R2[9]; for (int i = 0; i < 3; i++) RotateArc(u, ax, R + 3 * i, R2 + 3 * i);
            memcpy(R, R2, 36);
            const float wide = std::max(he[A], he[B]), thin = std::min(he[A], he[B]);
            if (wide >= C.fitFlat * thin)
            {   // and its thin side to the palm: a turn about the hole's axis, the shorter of the two ways
                const int T = he[A] <= he[B] ? A : B;
                const float t[3] = { R[T * 3], R[T * 3 + 1], R[T * 3 + 2] };
                float m[3] = { n[0], n[1], n[2] };
                if (Dot(t, m) < 0.f) for (float& x : m) x = -x;
                for (int i = 0; i < 3; i++) RotateArc(t, m, R + 3 * i, R2 + 3 * i);
                memcpy(R, R2, 36);
                g_FitRule = "long and flat: through the fist, thin side to the palm";
            }
        }
        // out of the palm by what of its thickness the hole cannot take
        float hn = 0.f; for (int i = 0; i < 3; i++) hn += he[i] * fabsf(Dot(R + 3 * i, n));
        const float push = std::max(0.f, hn - C.fitHoleR);
        memcpy(g_SnapRot, R, 36);
        for (int j = 0; j < 3; j++)
        {
            const float c = cl[0] * R[j] + cl[1] * R[3 + j] + cl[2] * R[6 + j];
            g_SnapPos[j] = hole[j] + n[j] * push - c;
        }
    }

    // ---- what the hand can take: the game's own interaction sensor, aimed with the hand's POINTER ----
    // arkCpntPlayerSensorInteraction works in TWO phases, on the camera component at sensor+0x40 (the player's), and
    // both read the aim origin/axis (cam+0x2527C/+0x25288) at the moment they run:
    // - SUBMIT, exe+0x3B8D60: up to five ASYNCHRONOUS sphere casts, one per slot, each only if that slot has no query
    //   in flight (handle at sensor+0x70+4i == -1). Slot i sweeps a sphere of radius decl+0x74+4i (shapes made at
    //   init, exe+0x3B82C0) from aim origin + axis*radius to aim origin + axis*decl+0x70 (the 3 m reach).
    // - RESULTS, exe+0x3B8430 (from exe+0x95000E): clears the pick (sensor+0x84/+0x88/+0x8C = 0x3FFF0010), casts a
    //   SYNCHRONOUS ray along the aim axis (exe+0x6C6760) and takes an interactable it hits; then walks the slots'
    //   finished results in order -- thinnest sphere first, so the narrowest cast that finds something wins -- and
    //   takes the first hit that passes the interactable's own rules (exe+0x3B80A0: within its distance of the
    //   EYE, cam+0xD8, and within its height of the eye) and has a clear line from the aim origin
    //   (exe+0x3B7B90, the slot's shape swept to the hit point). sensor+0x84 = the pick's spawn id: what the game
    //   highlights, prompts for and interacts with.
    // Until 2026-10-03 only SUBMIT was hooked, and called 17x per frame along a cone: every call after the first found
    // its slots busy and submitted nothing, every "probe" read the same stale +0x84, and RESULTS then validated the
    // hits against the camera's aim fields -- which `vr aimoverride` points along the LEFT hand. What got picked had
    // little to do with where the right hand pointed (the user: "a veces lo detecta mirando a otro sitio, a veces no
    // mirando justo al objeto").
    // Now both phases run against a COPY of the camera whose aim fields hold the grabbing hand's pointer
    // (PointerRay: world-anchored, filtered), by swapping the sensor's own camera pointer for the call. The real
    // camera's aim fields -- which the power and throw sensors read on other threads -- are never touched. The rest
    // of the pick (reach, radii, line of sight, the interactables' own rules) is the game's, unchanged: the hand
    // points the way the mouse looked.
    constexpr uint32_t kInteractSubmit = 0x3B8D60, kInteractResults = 0x3B8430;
    constexpr uint32_t kSensorCam = 0x40, kSensorDecl = 0x8, kDeclRange = 0x70, kDeclRadii = 0x74, kSensorTargetId = 0x84;
    constexpr uint32_t kSensorCarryId = 0x88;     //!< the CARRY pick: something the interact picks up (a bottle, a body)
    constexpr uint32_t kCamCopySize = 0x252B0;     // the camera component up to and including the aim fields
    constexpr int kNoTarget = 0x3FFF0010;
    using FnThis = void* (*)(void*);
    FnThis g_RealSubmit = nullptr, g_RealResults = nullptr;
    std::mutex g_SensorM;                          // one aimed call at a time: the swap of sensor+0x40 must not nest
    uint8_t* g_FakeCam = nullptr;
    std::atomic<void*> g_Sensor{ nullptr };
    std::atomic<int> g_ReachMode{ 1 };   // 0 = the game's own aim fields (the left hand under aimoverride), 1 = the grabbing hand's pointer
    std::atomic<int> g_TargetId{ kNoTarget };
    std::atomic<int> g_CarryId{ kNoTarget };
    std::atomic<ULONGLONG> g_TargetAt{ 0 };
    std::atomic<unsigned long long> g_Submits{ 0 }, g_Results{ 0 }, g_Aimed{ 0 }, g_Switches{ 0 }, g_GazeAimed{ 0 };
    std::atomic<bool> g_CarryGaze{ true };   // `vr carrygaze 0|1`: carrying a corpse, the sensor aims with the eyes (1) or the hidden hand (0, old)
    float g_GameRange = 0, g_Radii[5] = {};
    // metres behind the controller the pointer's casts start: a hand touching an object starts its ray outside it
    float g_Back = 0.10f;
    std::atomic<int> g_DiagFrames{ 0 };   // `vr grabdiag n`: log the next n RESULTS calls (pointer, pick, its offset from the ray)
    // PhysicalPickup: while an item is carried to the back / the mouth, the sensor keeps aiming at where it was taken,
    // from the eye, so the interact that stores it finds it (the hand's own pointer is behind the player by then)
    std::mutex g_LockM; bool g_LockOn = false; float g_LockPt[3] = {}; ULONGLONG g_LockAt = 0;
    std::atomic<unsigned long long> g_LockAimed{ 0 };

    // An entity's world AABB (min xyz, max xyz) from its render model's bounds and its transform (+0x300/+0x30C/+0x330):
    // exe+0x8780D0(entity, out). The interaction sensor's own height rule takes the object's height from it.
    constexpr uint32_t kEntityBounds = 0x8780D0, kEntityRenderModel = 0xF0;
    bool EntityBounds(void* ent, float out[6])
    {
        if (!ent || !Readable((uint8_t*)ent + kEntityRenderModel, 8) || !*(void**)((uint8_t*)ent + kEntityRenderModel)) return false;
        using BoundsFn = float* (*)(void*, float*);
        ((BoundsFn)(GameInfo::Base() + kEntityBounds))(ent, out);
        for (int i = 0; i < 6; i++) if (!std::isfinite(out[i])) return false;
        return out[3] >= out[0] && out[4] >= out[1] && out[5] >= out[2] && out[3] - out[0] < 10.f && out[5] - out[2] < 10.f;
    }
    void* EntityFromId(int id)
    {
        if (id <= 0 || id == kNoTarget) return nullptr;
        void* e = BodyHide::EntityById(id);
        return e && Readable((uint8_t*)e + kEntityOrigin, 0x30) ? e : nullptr;
    }
    //! A unit vector `dir` extended to a right-handed orthonormal frame (rows fwd/left/up, row0 = dir). The casts are
    //! spheres and the ray a line, so only row0 matters; the other two complete a valid idMat3.
    void BuildAxisFrame(const float* dir, float* out9)
    {
        const float worldUp[3] = { 0.f, 0.f, 1.f };
        float left[3] = { worldUp[1] * dir[2] - worldUp[2] * dir[1], worldUp[2] * dir[0] - worldUp[0] * dir[2], worldUp[0] * dir[1] - worldUp[1] * dir[0] };
        float ln = sqrtf(Dot(left, left));
        if (ln < 1e-4f)
        {
            const float alt[3] = { 1.f, 0.f, 0.f };
            left[0] = alt[1] * dir[2] - alt[2] * dir[1]; left[1] = alt[2] * dir[0] - alt[0] * dir[2]; left[2] = alt[0] * dir[1] - alt[1] * dir[0];
            ln = sqrtf(Dot(left, left));
        }
        for (float& x : left) x /= ln;
        const float up2[3] = { dir[1] * left[2] - dir[2] * left[1], dir[2] * left[0] - dir[0] * left[2], dir[0] * left[1] - dir[1] * left[0] };
        out9[0] = dir[0]; out9[1] = dir[1]; out9[2] = dir[2];
        out9[3] = left[0]; out9[4] = left[1]; out9[5] = left[2];
        out9[6] = up2[0]; out9[7] = up2[1]; out9[8] = up2[2];
    }

    //! Runs one phase of the sensor with the grabbing hand's pointer as its aim. While holding, the reach is 0 so the
    //! pick is empty and the game's interact is always "drop" (with a target in reach it interacts with that instead
    //! -- measured, a table clock took the F and the bottle stayed in hand).
    void* RunAimed(void* self, FnThis real, bool results)
    {
        uint8_t* s = (uint8_t*)self;
        g_Sensor = self;
        (results ? g_Results : g_Submits)++;
        auto finish = [&](void* r) {
            if (results)
            {
                const int id = s && Readable(s + kSensorTargetId, 4) ? *(int*)(s + kSensorTargetId) : kNoTarget;
                const int carry = s && Readable(s + kSensorCarryId, 4) ? *(int*)(s + kSensorCarryId) : kNoTarget;
                if (id != g_TargetId.load()) g_Switches++;
                g_TargetId = id; g_CarryId = carry; g_TargetAt = GetTickCount64();
            }
            return r;
        };
        uint8_t* decl = s && Readable(s + kSensorDecl, 8) ? *(uint8_t**)(s + kSensorDecl) : nullptr;
        if (!g_On.load() || !g_ReachMode.load() || HandAimHook::Mode() < 2 || !g_FakeCam || !decl || !Readable(decl + kDeclRange, 0x18))
            return finish(real(self));
        // Carrying a corpse the grabbing arm is hidden and its pointer not drawn, so nothing tells where it points --
        // and F on nothing is "drop": at the Clockwork Mansion's exit door the grip dropped Sokolov instead of opening
        // it (2026-10-18). Carrying, the sensor looks where the eyes do, as in the flat game: F opens the door looked at
        // and drops the body elsewhere. Decided at the submit and kept for that query's results (one frame's pair).
        static bool s_Gaze = false;
        if (!results) s_Gaze = g_CarryGaze.load() && PlayerState::Running("CarryCorpse");
        float po[3], pf[3], back = g_Back;
        bool locked = false; float lockPt[3] = {};
        {
            std::lock_guard lk(g_LockM);
            if (g_LockOn && GetTickCount64() - g_LockAt > 8000) g_LockOn = false;   // never longer than a carry can last
            if (g_LockOn) { locked = true; memcpy(lockPt, g_LockPt, 12); }
        }
        float lockAxis[9];
        if (locked && HandAimHook::RealCamera(po, lockAxis))
        {
            for (int k = 0; k < 3; k++) pf[k] = lockPt[k] - po[k];
            const float n = sqrtf(Dot(pf, pf));
            if (n < 1e-3f) return finish(real(self));
            for (float& v : pf) v /= n;
            back = 0.f;
            g_LockAimed++;
        }
        else if (s_Gaze)
        {
            float ca[9];
            if (!HandAimHook::RealCamera(po, ca)) return finish(real(self));
            pf[0] = ca[0]; pf[1] = ca[1]; pf[2] = ca[2]; back = 0.f;   // axis rows: forward, left, up
            g_GazeAimed++;
        }
        else if (!PointerRay::Get(g_Hand.load(), po, pf)) return finish(real(self));
        std::lock_guard lk(g_SensorM);
        const bool holding = g_Held.load() != nullptr;
        uint8_t** camSlot = (uint8_t**)(s + kSensorCam);
        uint8_t* cam = *camSlot;
        if (!cam || cam == g_FakeCam || !Readable(cam, kCamCopySize)) return finish(real(self));
        g_GameRange = *(float*)(decl + kDeclRange); memcpy(g_Radii, decl + kDeclRadii, 20);
        // a snapshot of the real camera, so anything in the call that reads another field of it still reads the camera
        memcpy(g_FakeCam, cam, kCamCopySize);
        float* aimO = (float*)(g_FakeCam + HandAimHook::kAimOrigin);
        float* aimA = (float*)(g_FakeCam + HandAimHook::kAimAxis);
        for (int k = 0; k < 3; k++) aimO[k] = po[k] - pf[k] * back;
        BuildAxisFrame(pf, aimA);
        float* range = (float*)(decl + kDeclRange);
        const float savedRange = *range;
        if (holding) *range = 0.f;
        *camSlot = g_FakeCam;
        void* r = real(self);
        g_Aimed++;
        // No selection hysteresis here, and there must never be a second RESULTS call in a frame: the first one
        // consumes the slots' query ids, and a second asks the collision system for results by ids it has already
        // retired -- "Collision query accessed with outdated or corrupted id" (exe+0x92B2E), a fatal error that quits
        // the game with no exception (measured 2026-10-03: a hysteresis that re-ran RESULTS aimed at the last pick
        // closed the game within seconds). Not needed either: with the filtered pointer the pick never flipped
        // between two objects, and on an object's edge against nothing only within ~0.5 deg (0.4 deg tremor) --
        // the flat game's own edge, as with a mouse.
        *camSlot = cam;
        if (holding) *range = savedRange;
        if (results && g_DiagFrames.load() > 0)
        {
            g_DiagFrames--;
            const int id = Readable(s + kSensorTargetId, 12) ? *(int*)(s + kSensorTargetId) : kNoTarget;
            const int carry = Readable(s + kSensorTargetId, 12) ? *(int*)(s + kSensorTargetId + 4) : kNoTarget;
            const int nearC = Readable(s + kSensorTargetId, 12) ? *(int*)(s + kSensorTargetId + 8) : kNoTarget;
            LOGI("GrabHand: grabdiag use {:#x} carry {:#x} near {:#x} direct {} flags {:#x}", id, carry, nearC,
                 Readable(s + 0x98, 8) ? *(void**)(s + 0x98) : nullptr, Readable(s + 0xB8, 1) ? s[0xB8] : 0);
            const uint8_t* e = (const uint8_t*)EntityFromId(id);
            if (e)
            {
                const float* eo = (const float*)(e + kEntityOrigin);
                const float d[3] = { eo[0] - aimO[0], eo[1] - aimO[1], eo[2] - aimO[2] };
                const float along = Dot(d, pf), off = sqrtf(fmaxf(Dot(d, d) - along * along, 0.f));
                LOGI("GrabHand: grabdiag pointer ({:.3f},{:.3f},{:.3f}) fwd ({:.3f},{:.3f},{:.3f}) -> id {:#x} entity {} at ({:.3f},{:.3f},{:.3f}): {:.2f} m along, {:.2f} m off the ray ({:.1f} deg)",
                     aimO[0], aimO[1], aimO[2], pf[0], pf[1], pf[2], id, (const void*)e, eo[0], eo[1], eo[2], along, off, atan2f(off, along) * 57.29578f);
            }
            else LOGI("GrabHand: grabdiag pointer ({:.3f},{:.3f},{:.3f}) fwd ({:.3f},{:.3f},{:.3f}) -> none (id {:#x}){}",
                      aimO[0], aimO[1], aimO[2], pf[0], pf[1], pf[2], id, holding ? ", holding" : "");
        }
        return finish(r);
    }
    void* SubmitHook(void* self) { return RunAimed(self, g_RealSubmit, false); }
    void* ResultsHook(void* self) { return RunAimed(self, g_RealResults, true); }

    // ---- the throw of a held object (arkPlayerStateHoldObject's throw, exe+0x83DE90, the LMB "Lanzar") ----
    // It builds the velocity (towards the crosshair's point, plus the player's own, times the pickable's scale) and
    // hands it on in an event; between the two, `call exe+0x810240` (a leaf that returns a global) runs with the final
    // velocity in [rbp-0x50] (x, y) and r12d (z), the entity in rdi and the position it was just placed at in
    // [rbp-0x60]. That call is redirected to a thunk that may rewrite the three before the event is filled.
    // Measured: the game's own throw of a glass, 10.7 m/s towards the crosshair; the hand at 1.87 m/s -> 2.81 m/s
    // (gain 1.5) along the hand's motion, a 1.7 m lob. It leaves on the click, no animation delay.
    constexpr uint32_t kThrowSite = 0x83E473, kThrowCallee = 0x810240;
    const uint8_t kThrowSiteBytes[] = { 0xF3, 0x0F, 0x11, 0x45, 0xC4, 0xE8 };   // movss [rbp-0x3c], xmm0 ; call
    bool g_ThrowInstalled = false;
    std::atomic<int> g_ThrowMode{ 1 };   // 0 = log only, 1 = the hand's velocity
    std::atomic<unsigned long long> g_Throws{ 0 }, g_ThrowsByHand{ 0 };
    float g_LastGameV = 0, g_LastHandV = 0;

    uint32_t OnHeldThrow(uint8_t* rbp, uint8_t* ent, uint32_t vzBits)
    {
        g_Throws++;
        float* vxy = (float*)(rbp - 0x50);
        float v[3] = { vxy[0], vxy[1], 0 }; memcpy(&v[2], &vzBits, 4);
        const float* pos = (const float*)(rbp - 0x60);
        g_LastGameV = sqrtf(Dot(v, v));
        float hand[3] = {}, hs = 0, pv[3] = {};
        const bool haveHand = ThrowGesture::HandVelocity(g_Hand.load(), hand, &hs);
        ThrowGesture::PlayerVelocity(pv);
        LOGI("GrabHand: the game throws {} from ({:.2f},{:.2f},{:.2f}): v ({:.2f},{:.2f},{:.2f}) {:.2f} m/s | hand ({:.2f},{:.2f},{:.2f}) {:.2f} m/s, player {:.2f} m/s",
             (void*)ent, pos[0], pos[1], pos[2], v[0], v[1], v[2], g_LastGameV, hand[0], hand[1], hand[2], hs, sqrtf(Dot(pv, pv)));
        if (g_ThrowMode.load() < 1 || !haveHand || ent != (uint8_t*)g_Held.load()) return vzBits;
        const float gain = ThrowGesture::Cfg("gain"), maxSpeed = ThrowGesture::Cfg("maxspeed"), inherit = ThrowGesture::Cfg("inherit");
        float o[3];
        for (int i = 0; i < 3; i++) o[i] = gain * hand[i] + inherit * pv[i];
        const float s = sqrtf(Dot(o, o));
        if (maxSpeed > 0.f && s > maxSpeed) for (float& x : o) x *= maxSpeed / s;
        for (float x : o) if (!std::isfinite(x)) return vzBits;
        vxy[0] = o[0]; vxy[1] = o[1];
        uint32_t bits; memcpy(&bits, &o[2], 4);
        g_ThrowsByHand++; g_LastHandV = sqrtf(Dot(o, o));
        LOGI("GrabHand: THROWN BY HAND: v ({:.2f},{:.2f},{:.2f}) {:.2f} m/s", o[0], o[1], o[2], g_LastHandV);
        return bits;
    }
    extern "C" uint32_t GrabHandOnHeldThrow(uint8_t* rbp, uint8_t* ent, uint32_t vz) { return OnHeldThrow(rbp, ent, vz); }

    //! Saves the volatile registers (the caller was compiled against a one-instruction leaf), calls the C++ side with
    //! (rbp, rdi, r12d), puts its answer in r12d, restores, and jumps on to the real leaf.
    uint8_t* EmitThrowThunk(uint8_t* p, uintptr_t target)
    {
        const uint8_t pre[] = {
            0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,     // push rcx rdx r8 r9 r10 r11
            0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,                       // sub rsp, 0x88  (16-aligned)
            0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,
            0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,
            0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70,   // movdqu [rsp+0x20..], xmm0..5
            0x48, 0x89, 0xE9,                                               // mov rcx, rbp
            0x48, 0x89, 0xFA,                                               // mov rdx, rdi
            0x45, 0x89, 0xE0,                                               // mov r8d, r12d
        };
        memcpy(p, pre, sizeof pre); p += sizeof pre;
        const uintptr_t fn = (uintptr_t)&GrabHandOnHeldThrow;
        *p++ = 0x48; *p++ = 0xB8; memcpy(p, &fn, 8); p += 8;                // mov rax, fn
        *p++ = 0xFF; *p++ = 0xD0;                                           // call rax
        *p++ = 0x41; *p++ = 0x89; *p++ = 0xC4;                              // mov r12d, eax
        const uint8_t post[] = {
            0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,
            0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,
            0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,
            0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00,                       // add rsp, 0x88
            0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,     // pop r11 r10 r9 r8 rdx rcx
        };
        memcpy(p, post, sizeof post); p += sizeof post;
        *p++ = 0x48; *p++ = 0xB8; memcpy(p, &target, 8); p += 8;            // mov rax, the real leaf
        *p++ = 0xFF; *p++ = 0xE0;                                           // jmp rax
        return p;
    }
    uint8_t* AllocNear(uintptr_t base, size_t size)
    {
        for (uintptr_t a = (base - 0x10000) & ~0xFFFFull; a > base - 0x70000000ull; a -= 0x10000)
            if (void* q = VirtualAlloc((void*)a, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return (uint8_t*)q;
        return nullptr;
    }

    void FlightLog()
    {   // after a release: where the object goes, every 100 ms for 2 s
        static double next = 0;
        const double now = (double)GetTickCount64() * 1e-3;
        if (!g_FlightEnt || now < next) return;
        next = now + 0.1;
        const uint8_t* e = (const uint8_t*)g_FlightEnt;
        if (now - g_FlightStart > 2.0 || !Readable(e + kEntityOrigin, 0x48) || *(const uintptr_t*)e != g_FlightVt) { LOGI("GrabHand: flight log ends"); g_FlightEnt = nullptr; return; }
        const float* o = (const float*)(e + kEntityOrigin); const float* v = (const float*)(e + kEntityVelocity);
        const float d[3] = { o[0] - g_FlightFrom[0], o[1] - g_FlightFrom[1], o[2] - g_FlightFrom[2] };
        LOGI("GrabHand: flight t {:.1f} pos ({:.2f},{:.2f},{:.2f}) horizontal {:.2f} m up {:+.2f} m | v ({:.2f},{:.2f},{:.2f}) {:.2f} m/s",
             now - g_FlightStart, o[0], o[1], o[2], sqrtf(d[0] * d[0] + d[1] * d[1]), d[2], v[0], v[1], v[2], sqrtf(Dot(v, v)));
    }
    // ---- the arms the hold hides ----
    // arkPlayerStateHoldObject's enter (exe+0x83D770) HIDES the right arm entity (the body component's +0x680 id, via
    // exe+0x774A10 = GetEntityById + Hide) and, for objects whose decl has +0xA8 set, the left one too (+0x370); it also
    // unequips the right hand (the sword's equipment entity is destroyed and a new one made on the release). Hide /
    // Show (vtable +0x80 / +0x88) are a single bit, entity+0xD0 bit 0, plus a render refresh -- no count -- so showing
    // the arm again for the hold and leaving the game's own Show at the release is safe.
    constexpr uint32_t kEntityFlags = 0xD0, kEntityShowSlot = 0x88;
    std::atomic<int> g_ShowArms{ 2 };    // 0 = as the game (hidden), 1 = the grabbing hand's arm, 2 = both
    std::atomic<unsigned long long> g_ArmShows{ 0 };
    void* ArmEntity(int side)
    {
        using GetComponentFn = void* (*)(void*, void*, int);
        const uintptr_t base = GameInfo::Base();
        void* player = Game::LocalPlayer();
        uint8_t* body = player ? (uint8_t*)((GetComponentFn)(base + BodyHide::kGetComponent))(player, (void*)(base + BodyHide::kBodyCpntTypeInfo), 2) : nullptr;
        const uint32_t idOff = side ? BodyHide::kRightArmEntityId : BodyHide::kLeftArmEntityId;
        if (!body || !Readable(body + idOff, 4)) return nullptr;
        const int id = *(int*)(body + idOff);
        uint8_t* e = (uint8_t*)BodyHide::EntityById(id);
        if (!e || !Readable(e, kEntityFlags + 8)) return nullptr;
        const uintptr_t vt = *(uintptr_t*)e;
        return vt > base && vt - base < 0x4000000 && Readable((void*)(vt + kEntityShowSlot), 8) ? e : nullptr;
    }
    //! While holding: the arms the hold hid are shown again (Present thread, which is the game's main thread).
    void ShowArms()
    {
        const int mode = g_ShowArms.load();
        if (!mode) return;
        for (int side = 0; side < 2; side++)
        {
            if (mode == 1 && side != g_Hand.load()) continue;
            uint8_t* e = (uint8_t*)ArmEntity(side);
            if (!e || !(e[kEntityFlags] & 1)) continue;
            using ShowFn = void (*)(void*);
            ((ShowFn)*(void**)(*(uintptr_t*)e + kEntityShowSlot))(e);
            g_ArmShows++;
            LOGI("GrabHand: the hold hid the {} arm (entity {}): shown again -> flags {:#x}", HandName(side), (void*)e, e[kEntityFlags]);
        }
    }
    // ---- the arm a carried corpse occupies ----
    // CarryCorpse (the body on the shoulder, glued to the camera) is not a HoldObject: the game leaves both arms shown,
    // and the grabbing hand then floats free beside a body it is supposed to be carrying. That arm is hidden with the
    // game's own Hide (vtable +0x80, the one HoldObject uses) for as long as the state runs, and shown again after --
    // only if it is still hidden and it was us who hid it.
    constexpr uint32_t kEntityHideSlot = 0x80;
    std::atomic<int> g_CarryHide{ 1 };   // 0 = as the game, 1 = the grabbing hand's arm, 2 = both
    std::atomic<bool> g_CarryHid[2];
    std::atomic<unsigned long long> g_CarryHides{ 0 };
    void CallSlot(uint8_t* e, uint32_t slot) { using Fn = void (*)(void*); ((Fn)*(void**)(*(uintptr_t*)e + slot))(e); }
    void CarryArms()
    {
        const bool carrying = PlayerState::Running("CarryCorpse");
        const int mode = g_CarryHide.load();
        for (int side = 0; side < 2; side++)
        {
            const bool want = carrying && (mode == 2 || (mode == 1 && side == g_Hand.load()));
            if (want == g_CarryHid[side].load()) continue;
            uint8_t* e = (uint8_t*)ArmEntity(side);
            if (want)
            {
                if (!e) continue;
                if (!(e[kEntityFlags] & 1)) { CallSlot(e, kEntityHideSlot); g_CarryHides++; }
                g_CarryHid[side] = true;
                LOGI("GrabHand: carrying a corpse: the {} arm (entity {}) hidden -> flags {:#x}", HandName(side), (void*)e, e[kEntityFlags]);
            }
            else
            {
                g_CarryHid[side] = false;
                if (e && (e[kEntityFlags] & 1)) CallSlot(e, kEntityShowSlot);
                LOGI("GrabHand: {}: the {} arm shown again -> flags {:#x}", carrying ? "carry hide switched off" : "the corpse is let go",
                     HandName(side), e ? e[kEntityFlags] : 0);
            }
        }
    }
    //! The entity the sensor picked, or null (Present thread).
    void* TargetEntity()
    {
        const int id = g_TargetId.load();
        if (GetTickCount64() - g_TargetAt.load() > 500) return nullptr;
        return EntityFromId(id);
    }
}

bool GrabHand::Install()
{
    if (g_ThrowInstalled || !GameInfo::IsSupportedBuild()) return g_ThrowInstalled;
    const uintptr_t base = GameInfo::Base();
    g_FakeCam = (uint8_t*)VirtualAlloc(nullptr, kCamCopySize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_RealSubmit = (FnThis)(base + kInteractSubmit);
    g_RealResults = (FnThis)(base + kInteractResults);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_RealSubmit, SubmitHook);
    DetourAttach(&(PVOID&)g_RealResults, ResultsHook);
    LOGI("GrabHand: interaction sensor submit exe+0x{:X} + results exe+0x{:X} detour={} (camera copy {})", kInteractSubmit, kInteractResults,
         DetourTransactionCommit(), (void*)g_FakeCam);
    uint8_t* call = (uint8_t*)(base + kThrowSite);
    const uint8_t* pre = call - (sizeof kThrowSiteBytes - 1);
    if (memcmp(pre, kThrowSiteBytes, sizeof kThrowSiteBytes) || (uintptr_t)(call + 5 + *(int32_t*)(call + 1)) != base + kThrowCallee)
    { LOGI("GrabHand: the held object's throw is not what it was measured to be; not installed"); return false; }
    uint8_t* block = AllocNear(base, 0x1000);
    if (!block) { LOGI("GrabHand: no memory within 2 GB of the exe"); return false; }
    EmitThrowThunk(block, base + kThrowCallee);
    const int64_t rel = (int64_t)block - (int64_t)(call + 5);
    if (rel != (int32_t)rel) { LOGI("GrabHand: thunk out of rel32 range"); return false; }
    DWORD old = 0;
    if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    InterlockedExchange((volatile LONG*)(call + 1), (LONG)(int32_t)rel);
    VirtualProtect(call + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_ThrowInstalled = true;
    LOGI("GrabHand: the held object's throw velocity (exe+0x{:X}) redirected", kThrowSite);
    return true;
}

void GrabHand::OnPresent()
{
    FlightLog();
    LearnFist();
    CarryArms();
    void* st = PlayerState::Running("HoldObject") ? PlayerState::StateObject("HoldObject") : nullptr;
    const int id = st && Readable((uint8_t*)st + kHeldIdInState, 4) ? *(int*)((uint8_t*)st + kHeldIdInState) : 0;
    if (id <= 0)
    {
        if (void* was = g_Held.load())
        {
            const uint8_t* e = (const uint8_t*)was;
            if (Readable(e + kEntityOrigin, 0x48))
            {
                const float* o = (const float*)(e + kEntityOrigin); const float* v = (const float*)(e + kEntityVelocity);
                LOGI("GrabHand: the hold ends: object at ({:.2f},{:.2f},{:.2f}) v ({:.2f},{:.2f},{:.2f}) {:.2f} m/s", o[0], o[1], o[2], v[0], v[1], v[2], sqrtf(Dot(v, v)));
                g_FlightEnt = was; g_FlightVt = *(const uintptr_t*)e; memcpy(g_FlightFrom, o, 12); g_FlightStart = (double)GetTickCount64() * 1e-3;
            }
            else LOGI("GrabHand: the hold ends");
        }
        g_Held = nullptr; g_OffPtr = nullptr; g_LastId = 0;
        // not holding: remember where the picked object sits, for the moment it is taken
        if (void* t = TargetEntity())
        {
            std::lock_guard lk(g_M);
            g_PreEnt = t; memcpy(g_PrePos, (uint8_t*)t + kEntityOrigin, 12); memcpy(g_PreRot, (uint8_t*)t + kEntityAxis, 36); g_PreAt = GetTickCount64();
        }
        return;
    }
    ShowArms();
    if (id == g_LastId) return;
    void* ent = BodyHide::EntityById(id);
    if (!ent || !Readable((uint8_t*)ent + kEntityOrigin, 0x30)) return;
    float cw[3] = {}; const bool haveCenter = ObjectCenter(ent, cw);
    float bmn[3], bmx[3]; const bool haveBox = LocalBounds(ent, bmn, bmx);
    if (haveBox)
        LOGI("GrabHand: the object's box, in its axes: ({:.3f},{:.3f},{:.3f}) .. ({:.3f},{:.3f},{:.3f}) -> size {:.3f} x {:.3f} x {:.3f}, centre ({:.3f},{:.3f},{:.3f}) from its origin",
             bmn[0], bmn[1], bmn[2], bmx[0], bmx[1], bmx[2], bmx[0] - bmn[0], bmx[1] - bmn[1], bmx[2] - bmn[2],
             0.5f * (bmn[0] + bmx[0]), 0.5f * (bmn[1] + bmx[1]), 0.5f * (bmn[2] + bmx[2]));
    else LOGI("GrabHand: the object's box is unknown");
    {
        std::lock_guard lk(g_M); g_RelValid = false; g_Snapping = false; g_CenterValid = haveCenter;
        g_BoxValid = haveBox; if (haveBox) { memcpy(g_BoxMin, bmn, 12); memcpy(g_BoxMax, bmx, 12); }
        if (haveCenter)
        {   // in the object's own axes: a constant, so it does not matter that the game is already pulling it away
            const uint8_t* e = (const uint8_t*)ent;
            const float* o = (const float*)(e + kEntityOrigin); const float* ax = (const float*)(e + kEntityAxis);
            const float d[3] = { cw[0] - o[0], cw[1] - o[1], cw[2] - o[2] };
            for (int i = 0; i < 3; i++) g_CenterLocal[i] = Dot(d, ax + 3 * i);
        }
        else memset(g_CenterLocal, 0, 12);
    }
    LOGI("GrabHand: the object's centre {}: ({:.3f},{:.3f},{:.3f}) from its origin, in its axes", haveCenter ? "(the game's)" : "unknown, its origin used",
         g_CenterLocal[0], g_CenterLocal[1], g_CenterLocal[2]);
    g_OffPtr = nullptr; g_LastId = id; g_Held = ent; g_Holds++;
    LOGI("GrabHand: the player holds entity {} (id {}, vtable exe+0x{:X})", ent, id, *(uintptr_t*)ent - GameInfo::Base());
}

bool GrabHand::OnJointQuery(void* bindEnt, void* obj, const float* offPos, float* outPos, float* outAxis)
{
    void* ent = g_Held.load();
    if (!ent || !offPos || !g_On.load() || HandAimHook::Mode() < 2) return false;
    if (bindEnt == ent) g_OffPtr = offPos;
    else if (offPos != g_OffPtr.load()) return false;
    g_Matches++;
    // The controller as the hand is drawn from it (HandNow), not as the game camera has it: the object belongs in the
    // drawn hand, and the two differ by the head's offset in the room.
    const int hand = g_Hand.load();
    Pose hp;
    if (!HandNow(hand, hp) && !HandAimHook::ControllerPose(hand, hp.o, hp.a)) { g_NoPose++; return false; }
    const float* co = hp.o; const float* ca = hp.a;
    const uint8_t* model = obj ? *(const uint8_t* const*)((const uint8_t*)obj + HandFx::kObjModel) : nullptr;
    if (!model || !Readable(model + HandFx::kModelOrigin, 48)) { g_NoModel++; return false; }
    const float* mo = (const float*)(model + HandFx::kModelOrigin);
    const float* mx = (const float*)(model + HandFx::kModelAxis);   // rows: the model's basis in world

    float relPos[3], relRot[9];
    {
        std::lock_guard lk(g_M);
        if (!g_RelValid)
        {   // the object's pose in the controller's frame where it was taken: the pre-hold pose if it is this object
            // and fresh, else wherever it is now
            const bool pre = g_PreEnt == ent && GetTickCount64() - g_PreAt < 1000;
            const float* eo = pre ? g_PrePos : (const float*)((const uint8_t*)ent + kEntityOrigin);
            const float* ea = pre ? g_PreRot : (const float*)((const uint8_t*)ent + kEntityAxis);
            const float d[3] = { eo[0] - co[0], eo[1] - co[1], eo[2] - co[2] };
            for (int k = 0; k < 3; k++) g_RelPos[k] = Dot(d, ca + 3 * k);
            for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) g_RelRot[i * 3 + k] = Dot(ea + 3 * i, ca + 3 * k);
            g_CapturedDist = sqrtf(Dot(g_RelPos, g_RelPos)); g_CapturedPre = pre;
            g_Snapping = C.snap > 0.f;
            if (!g_Snapping && C.maxRel > 0.f && g_CapturedDist > C.maxRel) for (float& x : g_RelPos) x *= C.maxRel / g_CapturedDist;
            g_RelValid = true;
            LOGI("GrabHand: hold begins {:.3f} m from the {} controller ({}) -> {} ({:.3f} fwd, {:.3f} left, {:.3f} up)",
                 g_CapturedDist, HandName(hand), pre ? "where it was picked" : "where it is now", g_Snapping ? "glides to the palm from" : "kept at",
                 g_RelPos[0], g_RelPos[1], g_RelPos[2]);
            if (g_Snapping)
            {   // what was up in the world, in the controller's frame now (the controller's rows are its axes in world),
                // and in the object's own axes as it rested (the third column of its axis rows)
                g_SnapUpCtrl[0] = ca[2]; g_SnapUpCtrl[1] = ca[5]; g_SnapUpCtrl[2] = ca[8];
                for (int i = 0; i < 3; i++) g_SnapUpObj[i] = ea[3 * i + 2];
                LARGE_INTEGER q; QueryPerformanceCounter(&q); g_SnapT0 = q.QuadPart;
                const bool fit = C.snap >= 2.f && g_BoxValid;
                if (fit) FitTarget(); else SnapTarget(hand);
                LOGI("GrabHand: snap: world up was ({:.2f},{:.2f},{:.2f}) in the controller's frame; {} -> origin at ({:.3f} fwd, {:.3f} left, {:.3f} up)",
                     g_SnapUpCtrl[0], g_SnapUpCtrl[1], g_SnapUpCtrl[2],
                     fit ? g_FitRule : g_CenterValid ? "held at the game's centre" : "held snapheight up from the base", g_SnapPos[0], g_SnapPos[1], g_SnapPos[2]);
            }
        }
        if (g_Snapping)
        {   // the glide: smoothstep from where it was to its place in the hand. The place is recomputed every query, so a
            // grabcfg snap*/fit* change, and the palm's measured place, show at once on the object in the hand.
            if (C.snap >= 2.f && g_BoxValid) FitTarget(); else SnapTarget(hand);
            LARGE_INTEGER f, q; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&q);
            const float el = (float)((double)(q.QuadPart - g_SnapT0) / (double)f.QuadPart);
            float a = C.snapTime > 0.f ? el / C.snapTime : 1.f; a = a < 0.f ? 0.f : a > 1.f ? 1.f : a;
            a = a * a * (3.f - 2.f * a);
            for (int k = 0; k < 3; k++) relPos[k] = g_RelPos[k] + (g_SnapPos[k] - g_RelPos[k]) * a;
            if (a >= 1.f) memcpy(relRot, g_SnapRot, 36); else SlerpRot(g_RelRot, g_SnapRot, a, relRot);
        }
        else { memcpy(relPos, g_RelPos, 12); memcpy(relRot, g_RelRot, 36); }
    }
    // world target = controller * rel
    float p[3], R[9];
    for (int j = 0; j < 3; j++) p[j] = co[j] + relPos[0] * ca[j] + relPos[1] * ca[3 + j] + relPos[2] * ca[6 + j];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[i * 3 + j] = relRot[i * 3] * ca[j] + relRot[i * 3 + 1] * ca[3 + j] + relRot[i * 3 + 2] * ca[6 + j];
    memcpy(g_LastTarget, p, 12);
    if (bindEnt == ent)
    {   // the bind: what the physics body follows. The drawn object (the render placement) stays exactly on the hand.
        static float prev[3] = {}; static ULONGLONG prevT = 0; static int logged = 0;
        const ULONGLONG t = GetTickCount64();
        const float dj[3] = { p[0] - prev[0], p[1] - prev[1], p[2] - prev[2] };
        const float jump = sqrtf(Dot(dj, dj));
        if (prevT && t - prevT < 100 && jump > 0.3f)
        {   // the hand target should never jump; it did while the CamSwap race fed the camera snapshot the hand
            g_Jumps++;
            if (logged++ < 20)
                LOGI("GrabHand: target JUMP {:.2f} m in {} ms: ({:.2f},{:.2f},{:.2f}) -> ({:.2f},{:.2f},{:.2f})", jump, t - prevT, prev[0], prev[1], prev[2], p[0], p[1], p[2]);
        }
        memcpy(prev, p, 12); prevT = t;
        float eo[3], ea[9];
        if (C.eyeClear > 0.f && HandAimHook::RealCamera(eo, ea))
        {
            float e[3] = { p[0] - eo[0], p[1] - eo[1], p[2] - eo[2] };
            const float r = sqrtf(Dot(e, e));
            g_LastEyeDist = r;
            if (r < C.eyeClear)
            {
                if (r < 1e-3f) { memcpy(e, ea, 12); } else for (float& x : e) x /= r;
                for (int j = 0; j < 3; j++) p[j] = eo[j] + e[j] * C.eyeClear;
                g_EyeCleared++;
            }
        }
        // Not player+0x300 live: CamSwap replaces it with the controller pose while a hooked sensor runs on another
        // thread. BodyHide samples it once per frame on the game thread.
        BodyHide::Anchor an[BodyHide::aCount];
        BodyHide::GetAnchors(an);
        if (an[BodyHide::aPlayer].valid)
        {
            const float* po = an[BodyHide::aPlayer].org;
            float h[2] = { p[0] - po[0], p[1] - po[1] };
            const float r = sqrtf(h[0] * h[0] + h[1] * h[1]);
            g_LastAxisDist = r;
            if (C.clear > 0.f && r < C.clear)
            {
                if (r < 1e-3f) { h[0] = ca[0]; h[1] = ca[1]; const float n = sqrtf(h[0] * h[0] + h[1] * h[1]); if (n > 1e-4f) { h[0] /= n; h[1] /= n; } }
                else { h[0] /= r; h[1] /= r; }
                p[0] = po[0] + h[0] * C.clear; p[1] = po[1] + h[1] * C.clear;
                g_Cleared++;
            }
        }
    }
    else
    {   // the render placement: where the object is DRAWN (for the overlay and the measurements)
        std::lock_guard lk(g_DrawM); memcpy(g_DrawPos, p, 12); memcpy(g_DrawRot, R, 36); g_DrawAt = GetTickCount64();
    }
    // -> the model's space, as the query answers
    const float d[3] = { p[0] - mo[0], p[1] - mo[1], p[2] - mo[2] };
    for (int k = 0; k < 3; k++) outPos[k] = Dot(d, mx + 3 * k);
    for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) outAxis[i * 3 + k] = Dot(R + 3 * i, mx + 3 * k);
    g_Applied++;
    return true;
}

bool GrabHand::HandHasTarget()
{
    const int id = g_TargetId.load();
    return g_ReachMode.load() && HandAimHook::Mode() >= 2 && id > 0 && id != kNoTarget && GetTickCount64() - g_TargetAt.load() < 300;
}
bool GrabHand::ReleaseThrows(float* speed)
{
    float v[3], s = 0;
    const bool have = ThrowGesture::HandVelocity(g_Hand.load(), v, &s);
    if (speed) *speed = s;
    return have && s > C.dropSpeed;
}
void GrabHand::SetThrowMode(int m) { g_ThrowMode = m; LOGI("GrabHand: throw mode {}", m); }
bool GrabHand::TargetPoint(float out[3])
{
    void* e = TargetEntity();
    if (!e) return false;
    float b[6];
    if (EntityBounds(e, b)) { for (int i = 0; i < 3; i++) out[i] = 0.5f * (b[i] + b[3 + i]); }
    else memcpy(out, (uint8_t*)e + kEntityOrigin, 12);
    return true;
}
bool GrabHand::EntityCenter(int id, float out[3])
{
    void* e = BodyHide::EntityById(id);
    if (!e) return false;
    float b[6];
    if (EntityBounds(e, b)) { for (int i = 0; i < 3; i++) out[i] = 0.5f * (b[i] + b[3 + i]); }
    else memcpy(out, (uint8_t*)e + kEntityOrigin, 12);
    return true;
}
float GrabHand::PickRange() { return g_GameRange; }
void GrabHand::LockSensor(const float point[3])
{
    std::lock_guard lk(g_LockM);
    g_LockOn = true; memcpy(g_LockPt, point, 12); g_LockAt = GetTickCount64();
}
void GrabHand::UnlockSensor() { std::lock_guard lk(g_LockM); g_LockOn = false; }
int GrabHand::TargetId() { return GetTickCount64() - g_TargetAt.load() < 500 ? g_TargetId.load() : kNoTarget; }
int GrabHand::CarryTargetId() { return GetTickCount64() - g_TargetAt.load() < 500 ? g_CarryId.load() : kNoTarget; }
void GrabHand::SetReach(int mode, float back)
{
    g_ReachMode = mode; if (back >= 0.f) g_Back = back;
    LOGI("GrabHand: interaction {} (casts start {:.2f} m behind the {} controller)", mode ? "aimed with the hand's pointer" : "aimed as the game aims it",
         g_Back, HandName(g_Hand.load()));
}
void GrabHand::SetHand(int hand)
{
    if (g_Held.load()) { std::lock_guard lk(g_M); g_RelValid = false; }   // mid-hold: re-captured against the new hand
    g_Hand = hand ? 1 : 0;
    LOGI("GrabHand: the {} hand takes and holds", HandName(g_Hand.load()));
}
int GrabHand::Hand() { return g_Hand.load(); }
void GrabHand::SetReleaseMode(int m)
{
    g_ReleaseMode = m < 0 || m > 2 ? 2 : m;
    static const char* kWhat[3] = { "the trigger drops (slow hand) or throws (moving hand)", "letting go of the grip drops or throws, by the hand's speed",
                                    "another grip press drops, the trigger throws" };
    LOGI("GrabHand: release mode {}: {}", g_ReleaseMode.load(), kWhat[g_ReleaseMode.load()]);
}
int GrabHand::ReleaseMode() { return g_ReleaseMode.load(); }
void GrabHand::SetCarryGaze(bool on) { g_CarryGaze = on; LOGI("GrabHand: carrying a corpse, the interaction sensor aims {}", on ? "with the eyes" : "with the hidden hand's pointer (old)"); }
void GrabHand::SetCarryHide(int mode)
{
    g_CarryHide = mode < 0 || mode > 2 ? 1 : mode;
    LOGI("GrabHand: carrying a corpse, {}", g_CarryHide.load() == 0 ? "the arms stay as the game leaves them" : g_CarryHide.load() == 1 ? "the grabbing hand's arm is hidden" : "both arms are hidden");
}
bool GrabHand::CarryHidesArm(int side) { return g_CarryHid[side & 1].load(); }
void GrabHand::SetShowArms(int mode) { g_ShowArms = mode; LOGI("GrabHand: while holding, {}", mode == 0 ? "the arms stay as the game leaves them" : mode == 1 ? "the grabbing hand's arm is shown" : "both arms are shown"); }
void* GrabHand::Sensor() { return g_Sensor.load(); }
void GrabHand::SetEnabled(bool on) { g_On = on; LOGI("GrabHand: {}", on ? "on" : "off"); }
bool GrabHand::Enabled() { return g_On.load(); }
void* GrabHand::HeldEntity() { return g_Held.load(); }
bool GrabHand::Set(const std::string& key, float v)
{
    for (const Named& n : kCfg) if (key == n.name) { *n.v = v; return true; }
    return false;
}
std::string GrabHand::Config()
{
    std::ostringstream o;
    for (const Named& n : kCfg) o << n.name << " " << *n.v << "  ";
    o << "reach " << g_ReachMode.load() << "  back " << g_Back << "  hand " << HandName(g_Hand.load()) << "  showarms " << g_ShowArms.load() << "  release " << g_ReleaseMode.load();
    return o.str();
}
void GrabHand::Calibrate()
{
    const int hand = g_Hand.load();
    float t[3], r[9], fing[3], pn[3];
    Pose ctrlR, ctrlNow, ctrlGame;
    const bool haveR = GpuTrace::HandPoseWorld(hand, t, r); if (haveR) ctrlR = FromCols(t, r);
    const bool havePalm = GpuTrace::PalmFrame(hand, t, r, fing, pn); Pose palm; if (havePalm) palm = FromCols(t, r);
    const bool haveNow = HandNow(hand, ctrlNow), haveGame = HandAimHook::ControllerPose(hand, ctrlGame.o, ctrlGame.a);
    auto P = [](const char* what, const Pose& x) {
        LOGI("grabcal: {}: pos ({:.3f},{:.3f},{:.3f}) rows ({:.2f},{:.2f},{:.2f})({:.2f},{:.2f},{:.2f})({:.2f},{:.2f},{:.2f}) angle {:.1f}", what, x.o[0], x.o[1], x.o[2],
             x.a[0], x.a[1], x.a[2], x.a[3], x.a[4], x.a[5], x.a[6], x.a[7], x.a[8], AngleDeg(x.a));
    };
    LOGI("grabcal: {} hand: drawn controller {} palm {} now {} game {}", HandName(hand), haveR, havePalm, haveNow, haveGame);
    if (haveR && havePalm)
    {
        P("palm in the drawn controller", Rel(ctrlR, palm));
        float f[3], n[3]; for (int k = 0; k < 3; k++) { f[k] = Dot(fing, ctrlR.a + 3 * k); n[k] = Dot(pn, ctrlR.a + 3 * k); }
        LOGI("grabcal: fingers ({:.2f},{:.2f},{:.2f}) palm normal ({:.2f},{:.2f},{:.2f}) in the drawn controller", f[0], f[1], f[2], n[0], n[1], n[2]);
    }
    if (haveR && haveNow) P("controller now vs drawn", Rel(ctrlR, ctrlNow));
    if (haveR && haveGame) P("controller (game camera) vs drawn", Rel(ctrlR, ctrlGame));
    float wo[3], wa[9];
    if (havePalm && GpuTrace::WeaponTargetPose(hand, wo, wa))
    {
        Pose w; memcpy(w.o, wo, 12); memcpy(w.a, wa, 36);
        P("weapon in the palm", Rel(palm, w));
    }
    void* ent = g_Held.load();
    if (ent && havePalm)
    {
        Pose d; bool box; float mn[3], mx[3];
        { std::lock_guard lk(g_DrawM); memcpy(d.o, g_DrawPos, 12); memcpy(d.a, g_DrawRot, 36); }
        { std::lock_guard lk(g_M); box = g_BoxValid; memcpy(mn, g_BoxMin, 12); memcpy(mx, g_BoxMax, 12); }
        P("held object (drawn) in the palm", Rel(palm, d));
        if (box)
        {
            float c[3], cw[3]; for (int i = 0; i < 3; i++) c[i] = 0.5f * (mn[i] + mx[i]);
            for (int j = 0; j < 3; j++) cw[j] = d.o[j] + c[0] * d.a[j] + c[1] * d.a[3 + j] + c[2] * d.a[6 + j];
            const float dd[3] = { cw[0] - palm.o[0], cw[1] - palm.o[1], cw[2] - palm.o[2] };
            LOGI("grabcal: held object's box centre in the palm ({:.3f},{:.3f},{:.3f}), {:.3f} m from the palm", Dot(dd, palm.a), Dot(dd, palm.a + 3), Dot(dd, palm.a + 6), sqrtf(Dot(dd, dd)));
            // against the fist's hole: the centre's distance from the hole's axis and along it, and the angle between
            // the object's long axis and the hole's
            float guard[3], blade[3];
            { std::lock_guard lk(g_FitM); memcpy(guard, g_GuardInPalm, 12); memcpy(blade, g_BladeInPalm, 12); }
            float cp[3] = { Dot(dd, palm.a), Dot(dd, palm.a + 3), Dot(dd, palm.a + 6) }, hole[3];
            for (int i = 0; i < 3; i++) hole[i] = guard[i] - blade[i] * C.fitDepth;
            const float e[3] = { cp[0] - hole[0], cp[1] - hole[1], cp[2] - hole[2] };
            const float along = Dot(e, blade); float off[3]; for (int i = 0; i < 3; i++) off[i] = e[i] - along * blade[i];
            int L = 0; for (int i = 1; i < 3; i++) if (mx[i] - mn[i] > mx[L] - mn[L]) L = i;
            float lw[3]; for (int j = 0; j < 3; j++) lw[j] = d.a[L * 3 + j];
            float lp[3] = { Dot(lw, palm.a), Dot(lw, palm.a + 3), Dot(lw, palm.a + 6) };
            const float cosL = fabsf(Dot(lp, blade));
            LOGI("grabcal: FIT: box centre {:.1f} mm off the hole's axis, {:+.1f} mm along it (+ = thumb); long axis {:.1f} deg from the hole's; samples palm {} guard {}",
                 sqrtf(Dot(off, off)) * 1000.f, along * 1000.f, acosf(cosL > 1.f ? 1.f : cosL) * 57.29578f, g_PalmSamples, g_GuardSamples);
        }
    }
}
bool GrabHand::GetDebug(Debug& out)
{
    out = Debug{};
    if (!g_Held.load()) return false;
    Pose d; ULONGLONG at;
    { std::lock_guard lk(g_DrawM); memcpy(d.o, g_DrawPos, 12); memcpy(d.a, g_DrawRot, 36); at = g_DrawAt; }
    if (!at || GetTickCount64() - at > 300) return false;
    float mn[3], mx[3];
    { std::lock_guard lk(g_M); if (!g_BoxValid) return false; memcpy(mn, g_BoxMin, 12); memcpy(mx, g_BoxMax, 12); }
    for (int c = 0; c < 8; c++)
    {
        const float l[3] = { (c & 1) ? mx[0] : mn[0], (c & 2) ? mx[1] : mn[1], (c & 4) ? mx[2] : mn[2] };
        for (int j = 0; j < 3; j++) out.corners[c][j] = d.o[j] + l[0] * d.a[j] + l[1] * d.a[3 + j] + l[2] * d.a[6 + j];
    }
    const float lc[3] = { 0.5f * (mn[0] + mx[0]), 0.5f * (mn[1] + mx[1]), 0.5f * (mn[2] + mx[2]) };
    for (int j = 0; j < 3; j++) out.center[j] = d.o[j] + lc[0] * d.a[j] + lc[1] * d.a[3 + j] + lc[2] * d.a[6 + j];
    out.box = true;
    return true;
}
void GrabHand::Diag(int frames) { g_DiagFrames = frames < 0 ? 0 : frames; LOGI("GrabHand: grabdiag armed for {} pick(s)", frames); }
std::string GrabHand::Stats()
{
    std::ostringstream o;
    o << (g_On.load() ? "on" : "off") << " hand " << HandName(g_Hand.load()) << " held " << g_Held.load() << " holds " << g_Holds.load() << " target 0x" << std::hex << g_TargetId.load() << std::dec
      << " matches " << g_Matches.load() << " applied " << g_Applied.load() << " nopose " << g_NoPose.load() << " nomodel " << g_NoModel.load()
      << " submits " << g_Submits.load() << " results " << g_Results.load() << " aimed " << g_Aimed.load() << " gazeaimed " << g_GazeAimed.load() << (g_CarryGaze.load() ? "" : " (carrygaze off)") << " switches " << g_Switches.load()

      << " range " << g_GameRange << " radii " << g_Radii[0] << "/" << g_Radii[1] << "/" << g_Radii[2] << "/" << g_Radii[3] << "/" << g_Radii[4]
      << " throws " << g_Throws.load() << " byhand " << g_ThrowsByHand.load() << " lastgame " << g_LastGameV << " lasthand " << g_LastHandV
      << " captured " << g_CapturedDist << (g_CapturedPre ? " pre" : " now") << " cleared " << g_Cleared.load() << " eyecleared " << g_EyeCleared.load()
      << " armshows " << g_ArmShows.load() << " carryhide " << g_CarryHide.load() << " carryhid " << g_CarryHid[0].load() << g_CarryHid[1].load() << "/" << g_CarryHides.load() << " jumps " << g_Jumps.load() << " eyedist " << g_LastEyeDist << " axisdist " << g_LastAxisDist
      << " fit " << (C.snap >= 2.f) << " palmsamples " << g_PalmSamples << " guardsamples " << g_GuardSamples;
    return o.str();
}
