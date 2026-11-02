#include "pch.h"
#include "HandFx.h"
#include "GameInfo.h"
#include "GpuTrace.h"
#include "BodyHide.h"
#include "GrabHand.h"
#include <detours/detours.h>
#include <cmath>
#include <map>
#include <tuple>

namespace
{
    using FnOff = bool (*)(void* obj, int space, short joint, const float* offPos, const float* offAxis, float* outPos, float* outAxis);
    using FnPlain = bool (*)(void* obj, int space, short joint, float* outPos, float* outAxis);
    // exe+0x7506D0 takes SEVEN arguments (it reads the 5th at entry+0x28 and divides by the 7th at entry+0x38):
    // passing six left the 7th as garbage, NaN poses and a dead game during a load. Eight are passed through as they
    // came; the first four are integers/pointers, so no argument travels in an xmm register.
    using FnBind = uintptr_t (*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    FnOff g_RealOff = nullptr;
    FnPlain g_RealPlain = nullptr;
    FnBind g_RealBind = nullptr;
    // Inside the entity-bind transform (an entity hung off a joint: the equipment on the hand, the power's entity on
    // the forearm). GpuTrace identifies a weapon's draws by that entity's REAL position, so these stay the engine's.
    thread_local int t_InBind = 0;
    thread_local void* t_BindEnt = nullptr;      // the entity being placed (transform component + 0x10)
    std::atomic<int> g_Mode{ 2 };
    // The research bookkeeping (a stack walk per arm query) runs only once `vr handfxlog` has been asked for.
    std::atomic<bool> g_Research{ false };
    std::atomic<unsigned long long> g_Calls{ 0 }, g_ArmCalls{ 0 }, g_Corrected{ 0 }, g_NoFrame{ 0 };
    std::atomic<unsigned long long> g_ItemCalls{ 0 }, g_ItemCorrected{ 0 };
    // Research: one entity's binds and the joint queries inside them (HandFx::ProbeEntity).
    std::atomic<void*> g_ProbeEnt{ nullptr };
    std::atomic<int> g_ProbeLeft{ 0 };
    bool Readable(const void* p, size_t n);
    void ProbeJoint(int fn, void* obj, int space, short joint, const float* offPos, const float* offAxis, const float* outPos, const float* outAxis)
    {
        if (!g_ProbeEnt.load() || g_ProbeLeft.load() <= 0) return;
        // inside that entity's bind, or any query with the held object's offset (measured (0.55,-0.30,-0.253))
        const bool offMatch = offPos && fabsf(offPos[0] - 0.55f) < 0.002f && fabsf(offPos[1] + 0.30f) < 0.002f;
        if (t_BindEnt != g_ProbeEnt.load() && !offMatch) return;
        if (offMatch && t_BindEnt != g_ProbeEnt.load()) g_ProbeLeft--;
        void* frames[8] = {}; const USHORT n = RtlCaptureStackBackTrace(1, 8, frames, nullptr);
        const uintptr_t base = GameInfo::Base(); std::string ra;
        for (USHORT i = 0; i < n; i++) { const uintptr_t a = (uintptr_t)frames[i]; if (a >= base && a < base + 0x4000000) ra += fmt::format(" 0x{:X}", a - base); }
        void* model = obj ? *(void**)((uint8_t*)obj + HandFx::kObjModel) : nullptr;
        int side = -1, k = -1;
        for (int s = 0; s < 2; s++) for (int j = 0; j < 2; j++) if (model && model == BodyHide::ArmModel(s, j)) { side = s; k = j; }
        const float* mo = model && Readable((uint8_t*)model + HandFx::kModelOrigin, 48) ? (const float*)((uint8_t*)model + HandFx::kModelOrigin) : nullptr;
        LOGI("HandFx probe: {} obj {} model {} (arm side {} idx {}) space {} joint {} off ({},{}) -> pos ({:.3f},{:.3f},{:.3f}) axis ({:.2f},{:.2f},{:.2f})({:.2f},{:.2f},{:.2f})({:.2f},{:.2f},{:.2f}) model org ({:.3f},{:.3f},{:.3f}) | callers{}",
             fn ? "plain" : "off", obj, model, side, k, space, joint,
             offPos ? fmt::format("{:.3f},{:.3f},{:.3f}", offPos[0], offPos[1], offPos[2]) : "-", offAxis ? fmt::format("ax0 {:.2f},{:.2f},{:.2f}", offAxis[0], offAxis[1], offAxis[2]) : "-",
             outPos[0], outPos[1], outPos[2], outAxis[0], outAxis[1], outAxis[2], outAxis[3], outAxis[4], outAxis[5], outAxis[6], outAxis[7], outAxis[8],
             mo ? mo[0] : 0.f, mo ? mo[1] : 0.f, mo ? mo[2] : 0.f, ra);
    }

    // ---- research: who asks for which arm joint, and do the two model spaces agree ----
    struct Key { int fn, side, joint; uintptr_t ra[4]; uintptr_t bindVt; int equip; bool operator<(const Key& o) const { return std::tie(fn, side, joint, ra[0], ra[1], ra[2], ra[3], bindVt, equip) < std::tie(o.fn, o.side, o.joint, o.ra[0], o.ra[1], o.ra[2], o.ra[3], o.bindVt, o.equip); } };
    struct Seen { unsigned long long n = 0, corrected = 0; float pos[3] = {}; float moved = 0; int seg = 0; bool haveB = false; float B[12] = {}; double maxDB = 0; unsigned long long nB = 0;
                   // world distance from the DRAWN palm: of the joint as corrected, and as the animation had it
                   unsigned long long nP = 0; double sumCorr = 0, maxCorr = 0, sumAnim = 0; };
    struct Check
    {
        unsigned long long n = 0; double sumDw = 0, maxDw = 0;   // |engine world - GPU m2w world| of the same joint
    };
    std::mutex g_M;
    std::map<Key, Seen> g_Seen;
    Check g_Check[2];

    bool Readable(const void* p, size_t n)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    //! Which arm this skeleton object draws, or -1. The render model pointer is compared against the arm entities'
    //! (resolved on the game thread by BodyHide), so nothing is dereferenced beyond obj+0x38.
    int ArmSide(void* obj)
    {
        if (!obj) return -1;
        void* model = *(void**)((uint8_t*)obj + HandFx::kObjModel);
        if (!model) return -1;
        for (int s = 0; s < 2; s++) for (int k = 0; k < 2; k++) if (model == BodyHide::ArmModel(s, k)) return s;
        return -1;
    }
    bool IsEquipment(void* ent) { return ent && (ent == BodyHide::LeftWeaponEntity() || ent == BodyHide::RightWeaponEntity()); }
    // 3x4 rows (r0 t0)(r1 t1)(r2 t2): p' = R p + t
    void RtFromJoint(const float pos[3], const float axis[9], float o[12])
    {   // engine axis rows are the joint's basis vectors in model space -> they are the COLUMNS of R
        for (int i = 0; i < 3; i++) { for (int k = 0; k < 3; k++) o[i * 4 + k] = axis[k * 3 + i]; o[i * 4 + 3] = pos[i]; }
    }
    void RtInvMul(const float a[12], const float b[12], float o[12])   // o = a^-1 * b, a rigid
    {
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 3; j++) o[i * 4 + j] = a[0 * 4 + i] * b[0 * 4 + j] + a[1 * 4 + i] * b[1 * 4 + j] + a[2 * 4 + i] * b[2 * 4 + j];
            float d[3] = { b[3] - a[3], b[7] - a[7], b[11] - a[11] };
            o[i * 4 + 3] = a[0 * 4 + i] * d[0] + a[1 * 4 + i] * d[1] + a[2 * 4 + i] * d[2];
        }
    }

    void Research(int fn, int side, int joint, void* obj, const float before[3], const float axis[9], const float moved[3], bool corrected, int seg)
    {
        // The first two return addresses inside the executable are the engine callers (how many of our own frames
        // come first depends on what the compiler inlined).
        void* frames[12] = {};
        const USHORT n = RtlCaptureStackBackTrace(0, 12, frames, nullptr);
        const uintptr_t base = GameInfo::Base();
        Key k{ fn, side, joint, {}, 0, 0 }; int got = 0;
        if (t_InBind && t_BindEnt && Readable(t_BindEnt, 8)) k.bindVt = *(uintptr_t*)t_BindEnt - base;
        k.equip = t_InBind ? (IsEquipment(t_BindEnt) ? 1 : 2) : 0;
        for (USHORT i = 0; i < n && got < 4; i++)
        {
            const uintptr_t a = (uintptr_t)frames[i];
            if (a >= base && a < base + 0x4000000) k.ra[got++] = a - base;
        }
        float m2w[12], M[12]; int bones = 0; unsigned long long snapFrame = 0;
        const bool snap = GpuTrace::ArmSnapshot(side, m2w, joint, M, &bones, &snapFrame);
        uint8_t* model = *(uint8_t**)((uint8_t*)obj + HandFx::kObjModel);
        std::lock_guard lk(g_M);
        if (g_Seen.size() < 512 || g_Seen.count(k))
        {
            Seen& s = g_Seen[k]; s.n++; if (corrected) s.corrected++; memcpy(s.pos, before, 12); s.seg = seg;
            if (seg) { const float d[3] = { moved[0] - before[0], moved[1] - before[1], moved[2] - before[2] }; s.moved = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); }
        }
        if (!snap || !Readable(model + HandFx::kModelOrigin, 48)) return;
        {   // how far the effect's anchor lands from the hand the player sees
            float porg[3], prot[9], pf[3];
            if (seg && GpuTrace::PalmFrame(side, porg, prot, pf))
            {
                // In MODEL space: the drawn palm taken back through the m2w of the frame it was drawn in. In world
                // space the player's own motion between that frame and this query (7 cm a frame walking) would be
                // counted as error, but the engine places the effect with its CURRENT model transform, so what has
                // to agree is the model-space position.
                float pm[3]; { const float d[3] = { porg[0] - m2w[3], porg[1] - m2w[7], porg[2] - m2w[11] };
                               for (int i = 0; i < 3; i++) pm[i] = m2w[0 * 4 + i] * d[0] + m2w[1 * 4 + i] * d[1] + m2w[2 * 4 + i] * d[2]; }
                auto dist = [&](const float* p) {
                    return sqrt((double)(p[0] - pm[0]) * (p[0] - pm[0]) + (double)(p[1] - pm[1]) * (p[1] - pm[1]) + (double)(p[2] - pm[2]) * (p[2] - pm[2])); };
                Seen& s = g_Seen[k];
                const double dc = dist(moved), da = dist(before);
                s.nP++; s.sumCorr += dc; s.sumAnim += da; if (dc > s.maxCorr) s.maxCorr = dc;
                static int outliers = 0;
                if (s.nP > 20 && dc > s.sumCorr / s.nP + 0.08 && outliers < 40)   // far from where this joint usually sits
                {
                    outliers++;
                    LOGI("HandFx outlier: side {} joint {} seg {} palm dist {:.3f} (animated {:.3f}) snap frame {} now {} | anim ({:.3f},{:.3f},{:.3f}) corr ({:.3f},{:.3f},{:.3f}) m2w t ({:.2f},{:.2f},{:.2f}) palm model ({:.3f},{:.3f},{:.3f})",
                         side, joint, seg, dc, da, snapFrame, GpuTrace::PresentFrame(), before[0], before[1], before[2], moved[0], moved[1], moved[2],
                         m2w[3], m2w[7], m2w[11], pm[0], pm[1], pm[2]);
                }
            }
        }
        // (1) the engine's model->world against the GPU's: the same joint, both ways, should land on the same point
        const float* org = (const float*)(model + HandFx::kModelOrigin); const float* ax = (const float*)(model + HandFx::kModelAxis);
        float we[3], wg[3];
        for (int i = 0; i < 3; i++) we[i] = org[i] + before[0] * ax[0 * 3 + i] + before[1] * ax[1 * 3 + i] + before[2] * ax[2 * 3 + i];
        for (int i = 0; i < 3; i++) wg[i] = m2w[i * 4] * before[0] + m2w[i * 4 + 1] * before[1] + m2w[i * 4 + 2] * before[2] + m2w[i * 4 + 3];
        const double dw = sqrt((double)(we[0] - wg[0]) * (we[0] - wg[0]) + (double)(we[1] - wg[1]) * (we[1] - wg[1]) + (double)(we[2] - wg[2]) * (we[2] - wg[2]));
        Check& c = g_Check[side];
        c.n++; c.sumDw += dw; if (dw > c.maxDw) c.maxDw = dw;
        // (2) is engine joint j the palette's bone j? Then Inv(M_j) * W_j = B_j (times a constant local offset for
        //     the offset query) is the same every frame, however the arm animates. Per caller and joint.
        if (joint >= 0 && joint < bones && std::isfinite(M[0]))
        {
            Seen& s = g_Seen[k];
            float W[12], B[12]; RtFromJoint(before, axis, W); RtInvMul(M, W, B);
            if (!s.haveB) { memcpy(s.B, B, 48); s.haveB = true; }
            else { double d = 0; for (int i = 0; i < 12; i++) d = std::max(d, (double)fabsf(B[i] - s.B[i])); if (d > s.maxDB) s.maxDB = d; }
            s.nB++;
        }
    }

    //! Which hand's held item this skeleton object draws, or -1 (only items whose effects hang off their own joints:
    //! the Timepiece, whose light spins inside its glass dome).
    int ItemSide(void* obj)
    {
        if (!obj) return -1;
        void* model = *(void**)((uint8_t*)obj + HandFx::kObjModel);
        if (!model) return -1;
        for (int s = 0; s < 2; s++)
            if (BodyHide::HeldItemClass(s) == BodyHide::kTimeShiftCpntVtable)
                for (int k = 0; k < 2; k++) if (model == BodyHide::WeaponModel(s, k)) return s;
        return -1;
    }
    //! A joint of a held item's model, asked for by something hung off it (an effect, a light): the GPU draws the item at
    //! the hand (GpuTrace::WeaponTargetPose) while the engine keeps it where the animation holds it, so whatever the
    //! engine places by that joint floated where the flat game's item would be. The answer is the joint as the DRAWN
    //! item has it, given back in the model's current frame: p' = M^-1 * D * p (M the model's transform now, D the drawn
    //! pose; the item's draws sit at its entity, so its model frame is the entity's). When WeaponPose already put the
    //! drawn pose into the model's transform, M = D and nothing changes. The item's own bind (the equipment entity on the
    //! hand) is left alone: the draws are identified by that entity's real position.
    bool CorrectItem(void* obj, int side, float* outPos, float* outAxis)
    {
        g_ItemCalls++;
        if (g_Mode.load() < 2 || (t_InBind && IsEquipment(t_BindEnt))) return false;
        uint8_t* model = *(uint8_t**)((uint8_t*)obj + HandFx::kObjModel);
        if (!Readable(model + HandFx::kModelOrigin, 48)) return false;
        float dorg[3], dax[9];
        if (!GpuTrace::WeaponTargetPose(side, dorg, dax)) return false;
        const float* mo = (const float*)(model + HandFx::kModelOrigin); const float* ma = (const float*)(model + HandFx::kModelAxis);
        // world (drawn) = dorg + sum_k p_k dax[k]; back into the model's frame: p'_k = dot(ma[k], world - mo)
        float w[3];
        for (int i = 0; i < 3; i++) w[i] = dorg[i] + outPos[0] * dax[0 * 3 + i] + outPos[1] * dax[1 * 3 + i] + outPos[2] * dax[2 * 3 + i];
        const float d[3] = { w[0] - mo[0], w[1] - mo[1], w[2] - mo[2] };
        float p[3], r[9];
        for (int k = 0; k < 3; k++) p[k] = ma[k * 3] * d[0] + ma[k * 3 + 1] * d[1] + ma[k * 3 + 2] * d[2];
        // the joint's axis rows: in the world by the drawn frame, back by the model's
        for (int i = 0; i < 3; i++)
        {
            float rw[3];
            for (int c = 0; c < 3; c++) rw[c] = outAxis[i * 3] * dax[c] + outAxis[i * 3 + 1] * dax[3 + c] + outAxis[i * 3 + 2] * dax[6 + c];
            for (int k = 0; k < 3; k++) r[i * 3 + k] = rw[0] * ma[k * 3] + rw[1] * ma[k * 3 + 1] + rw[2] * ma[k * 3 + 2];
        }
        for (float v : p) if (!std::isfinite(v)) return false;
        const float moved = sqrtf((p[0] - outPos[0]) * (p[0] - outPos[0]) + (p[1] - outPos[1]) * (p[1] - outPos[1]) + (p[2] - outPos[2]) * (p[2] - outPos[2]));
        static std::atomic<int> logged{ 0 };
        if (logged.fetch_add(1) < 12)
            LOGI("HandFx: held item (side {}) joint asked {} -> moved {:.3f} m to the drawn item (bound entity {} vt exe+0x{:X})", side,
                 t_InBind ? "in a bind" : "directly", moved, t_InBind ? t_BindEnt : nullptr,
                 t_InBind && t_BindEnt && Readable(t_BindEnt, 8) ? *(uintptr_t*)t_BindEnt - GameInfo::Base() : 0);
        memcpy(outPos, p, 12); memcpy(outAxis, r, 36);
        g_ItemCorrected++;
        return true;
    }
    bool Correct(int fn, void* obj, short joint, float* outPos, float* outAxis)
    {
        g_Calls++;
        const int mode = g_Mode.load();
        if (!mode || !outPos || !outAxis) return false;
        if (const int is = ItemSide(obj); is >= 0) return CorrectItem(obj, is, outPos, outAxis);
        const int side = ArmSide(obj);
        if (side < 0) return false;
        g_ArmCalls++;
        // What the correction gives, computed in both modes (1 = measure only, 2 = hand it to the engine).
        float pos[3] = { outPos[0], outPos[1], outPos[2] }, axis[9]; memcpy(axis, outAxis, 36);
        int seg = 0;
        const bool have = GpuTrace::ArmCorrectJoint(side, joint, pos, axis, &seg);
        if (!have) g_NoFrame++;
        // Mode 2 leaves the player's equipment entities where the animation hangs them: GpuTrace identifies a
        // weapon's draws by that entity's real position (moving it took a pistol from 26 draws to 5).
        const bool apply = have && (mode >= 3 || (mode == 2 && !(t_InBind && IsEquipment(t_BindEnt))));
        if (apply) g_Corrected++;
        if (g_Research.load()) Research(fn, side, joint, obj, outPos, outAxis, pos, apply, have ? seg : 0);
        if (apply) { memcpy(outPos, pos, 12); memcpy(outAxis, axis, 36); }
        return apply;
    }

    bool HookOff(void* obj, int space, short joint, const float* offPos, const float* offAxis, float* outPos, float* outAxis)
    {
        const bool r = g_RealOff(obj, space, joint, offPos, offAxis, outPos, outAxis);
        if (r)
        {
            ProbeJoint(0, obj, space, joint, offPos, offAxis, outPos, outAxis);
            // a held object (its bind and its render placement both ask with the bind's offset) goes to the hand
            if (GrabHand::OnJointQuery(t_BindEnt, obj, offPos, outPos, outAxis)) return r;
            Correct(0, obj, joint, outPos, outAxis);
        }
        return r;
    }
    uintptr_t HookBind(void* self, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g)
    {
        void* const prevEnt = t_BindEnt;
        // self is the engine's own transform component (the caller just used it); no VirtualQuery on this hot path,
        // every bound entity in the level comes through here every frame.
        t_BindEnt = self ? *(void**)((uint8_t*)self + 0x10) : nullptr;
        t_InBind++;
        const bool probe = g_ProbeEnt.load() && t_BindEnt == g_ProbeEnt.load() && g_ProbeLeft.load() > 0;
        if (probe)
        {
            void* frames[6] = {}; const USHORT n = RtlCaptureStackBackTrace(1, 6, frames, nullptr);
            const uintptr_t base = GameInfo::Base(); std::string ra;
            for (USHORT i = 0; i < n; i++) { const uintptr_t x = (uintptr_t)frames[i]; if (x >= base && x < base + 0x4000000) ra += fmt::format(" 0x{:X}", x - base); }
            LOGI("HandFx probe: BIND self {} entity {} args {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} | callers{}", self, t_BindEnt, a, b, c, d, e, f, g, ra);
        }
        const uintptr_t r = g_RealBind(self, a, b, c, d, e, f, g);
        if (probe) { LOGI("HandFx probe: BIND returns {:#x}", r); g_ProbeLeft--; }
        t_InBind--;
        t_BindEnt = prevEnt;
        return r;
    }
    bool HookPlain(void* obj, int space, short joint, float* outPos, float* outAxis)
    {
        const bool r = g_RealPlain(obj, space, joint, outPos, outAxis);
        if (r) { ProbeJoint(1, obj, space, joint, nullptr, nullptr, outPos, outAxis); Correct(1, obj, joint, outPos, outAxis); }
        return r;
    }
}

bool HandFx::Install()
{
    if (!GameInfo::IsSupportedBuild()) return false;
    g_RealOff = (FnOff)(GameInfo::Base() + kJointModelOff);
    g_RealPlain = (FnPlain)(GameInfo::Base() + kJointModel);
    g_RealBind = (FnBind)(GameInfo::Base() + kBindTransform);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_RealOff, HookOff);
    DetourAttach(&(PVOID&)g_RealPlain, HookPlain);
    DetourAttach(&(PVOID&)g_RealBind, HookBind);
    const LONG r = DetourTransactionCommit();
    LOGI("HandFx: joint queries exe+0x{:X} / exe+0x{:X} detour={}", kJointModelOff, kJointModel, r);
    return r == NO_ERROR;
}
void HandFx::Uninstall()
{
    if (!g_RealOff) return;
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)g_RealOff, HookOff); DetourDetach(&(PVOID&)g_RealPlain, HookPlain); DetourDetach(&(PVOID&)g_RealBind, HookBind);
    DetourTransactionCommit();
}
void HandFx::SetMode(int mode) { g_Mode = mode; LOGI("HandFx: mode {}", mode); }
int HandFx::Mode() { return g_Mode.load(); }
void HandFx::LogReport()
{
    if (!g_Research.exchange(true)) { LOGI("HandFx: research on (collecting from now; ask again for the report)"); return; }
    std::lock_guard lk(g_M);
    LOGI("HandFx report: mode {} calls {} arm {} corrected {} noframe {} | {} caller/joint keys", g_Mode.load(), g_Calls.load(), g_ArmCalls.load(), g_Corrected.load(), g_NoFrame.load(), g_Seen.size());
    for (auto& [k, s] : g_Seen)
        LOGI("HandFx:  fn {} side {} joint {:3} bind {} vt 0x{:X} callers exe+0x{:X} <- 0x{:X} <- 0x{:X} <- 0x{:X}  n {} corrected {} seg {} moved {:.3f} m  pos ({:.3f},{:.3f},{:.3f})  bind drift {:.4f} over {}  palm dist corrected {:.3f} (max {:.3f}) animated {:.3f} over {}",
             k.fn ? "plain" : "off", k.side, k.joint, k.equip == 1 ? "EQUIP" : k.equip == 2 ? "other" : "-", k.bindVt, k.ra[0], k.ra[1], k.ra[2], k.ra[3], s.n, s.corrected, s.seg, s.moved, s.pos[0], s.pos[1], s.pos[2], s.maxDB, s.nB,
             s.nP ? s.sumCorr / s.nP : 0.0, s.maxCorr, s.nP ? s.sumAnim / s.nP : 0.0, s.nP);
    for (int s = 0; s < 2; s++)
    {
        const Check& c = g_Check[s];
        double lm = 0, lx = 0; unsigned long long ln = 0; GpuTrace::ArmFxLag(s, lm, lx, ln);
        LOGI("HandFx:  side {} space check: {} samples, engine vs GPU model->world mean {:.4f} max {:.4f} m | one-frame lag at the wrist mean {:.4f} max {:.4f} m over {} frames",
             s, c.n, c.n ? c.sumDw / c.n : 0.0, c.maxDw, lm, lx, ln);
    }
    g_Seen.clear(); g_Check[0] = Check(); g_Check[1] = Check();
}
void* HandFx::BoundEntity() { return t_InBind ? t_BindEnt : nullptr; }
void HandFx::ProbeEntity(void* entity, int n) { g_ProbeLeft = n; g_ProbeEnt = entity; LOGI("HandFx: probing {} binds of entity {}", n, entity); }
std::string HandFx::Stats()
{
    return fmt::format("mode {} calls {} arm {} corrected {} noframe {} item {} itemcorrected {}", g_Mode.load(), g_Calls.load(), g_ArmCalls.load(), g_Corrected.load(), g_NoFrame.load(),
                       g_ItemCalls.load(), g_ItemCorrected.load());
}
