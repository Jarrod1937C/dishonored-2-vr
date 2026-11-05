#include "pch.h"
#include "SoftBody.h"
#include "GameInfo.h"
#include "VRRuntime.h"
#include "GrabHand.h"
#include <detours/detours.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    // exe+0x540A90(skeleton object, space): the posed joints (obj+0x198) copied into the pose array as that space
    using Commit_t = uintptr_t(*)(void* obj, int space);
    constexpr uintptr_t kCommit = 0x540A90;
    constexpr uint8_t kCommitBytes[8] = { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57 };
    // The spaces' remaps: obj+0x2E8 {pointer, count} to an array of {short* joint -> entry, count} (kept inline at
    // obj+0x2F8): space 3 is the last and the drawn one (exe+0x25EFE0 is given its block, [[obj+0x2E8]+0x30][0]);
    // space 2 the animation's before the last stage. (NpcSkel's "space 3", obj+0x2E8 + 3*16, is this space 2.)
    constexpr int kDrawnSpace = 3, kAnimSpace = 2;
    constexpr size_t kEntModel = 0xF0, kEntSkelObj = 0x360, kObjModel = 0x38, kObjPose = 0x2D8, kObjCount = 0x2E0, kObjRemaps = 0x2E8;
    constexpr size_t kModelOrigin = 0x1860, kModelAxis = 0x186C, kModelSkel = 0x1FE8, kSkelDef = 0x50, kDefJoints = 0x78;
    constexpr int kMaxJ = 256, kChars = 8;

    // how the body gives
    constexpr float kPushGain = 1.6f;      // the pushed point moves this many times as far as the real hand is in
    constexpr float kMaxDepth = 0.15f;     // m of the hand's depth that counts
    constexpr float kMaxMove = 0.25f;      // m the touched point moves at most
    constexpr float kPullGain = 1.3f;      // a pull: times the hand's own move
    constexpr float kMaxPull = 0.30f;      // m it is pulled at most
    constexpr float kMaxAngle = 0.60f;     // rad a part turns at most (34 deg)
    constexpr float kPivotReach = 0.18f;   // m: the part bends at the first joint at least this far up from the touched one
    constexpr float kHoldHz = 5.f, kHoldZeta = 0.9f;     // while pressed: follows the hand closely
    constexpr float kFreeHz = 2.6f, kFreeZeta = 0.22f;   // let go: springs back with a sway
    constexpr int kBends = 4;              // per character: each hand's, and the ones still springing back

    std::atomic<bool> g_On{ false };
    bool g_Installed = false;
    Commit_t g_Real = nullptr;
    std::atomic<unsigned long long> g_Applied{ 0 }, g_Bends{ 0 }, g_Faults{ 0 };

    // ---- POD helpers (__try) ----
    bool Rd(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    uintptr_t P(uintptr_t base, size_t off) { uintptr_t p = 0; if (!base || !Rd(&p, (const void*)(base + off), 8)) return 0; return p; }
    bool Finite(const float* v, int n) { for (int i = 0; i < n; i++) if (!std::isfinite(v[i]) || fabsf(v[i]) > 1e5f) return false; return true; }
    float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    void Cross(const float a[3], const float b[3], float o[3]) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
    float Len(const float a[3]) { return sqrtf(Dot(a, a)); }
    //! Rodrigues: the rotation matrix (row-major, acts on column vectors) of rotation vector w
    void RotOf(const float w[3], float R[9])
    {
        const float a = Len(w);
        if (a < 1e-7f) { const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; memcpy(R, I, 36); return; }
        const float x = w[0] / a, y = w[1] / a, z = w[2] / a, c = cosf(a), s = sinf(a), t = 1.f - c;
        R[0] = t * x * x + c;     R[1] = t * x * y - s * z; R[2] = t * x * z + s * y;
        R[3] = t * x * y + s * z; R[4] = t * y * y + c;     R[5] = t * y * z - s * x;
        R[6] = t * x * z - s * y; R[7] = t * y * z + s * x; R[8] = t * z * z + c;
    }
    void Mul(const float R[9], const float v[3], float o[3]) { o[0] = R[0] * v[0] + R[1] * v[1] + R[2] * v[2]; o[1] = R[3] * v[0] + R[4] * v[1] + R[5] * v[2]; o[2] = R[6] * v[0] + R[7] * v[1] + R[8] * v[2]; }

    // ---- what the hook applies (published under g_PM) ----
    struct PubBend { int pivot = -1; bool rowsAxes = true; uint64_t mask[4] = {}; float R[9] = {}; };
    struct Pub { uintptr_t obj = 0; int n = 0, slot = -1; PubBend b[kBends]; };
    std::atomic<unsigned long long> g_AppliedAt[8];   // per character slot: when the drawn pose was last bent (ms)
    std::mutex g_PM;
    Pub g_Pub[kChars]; int g_PubN = 0;
    std::atomic<int> g_PubCount{ 0 };

    //! one bend applied to the drawn space's entries: the pivot's subtree turned about the pivot (POD)
    bool ApplyBend(uintptr_t obj, const PubBend& b)
    {
        __try
        {
            const uintptr_t pose = *(const uintptr_t*)(obj + kObjPose);
            const uint32_t cnt = *(const uint32_t*)(obj + kObjCount) & 0xFFFFFF;
            const uintptr_t table = *(const uintptr_t*)(obj + kObjRemaps);
            const uint32_t spaces = *(const uint32_t*)(obj + kObjRemaps + 8) & 0xFFFFFF;
            if (!table || spaces <= (uint32_t)kDrawnSpace) return false;
            const uintptr_t rp = *(const uintptr_t*)(table + kDrawnSpace * 16);
            const uint32_t rc = *(const uint32_t*)(table + kDrawnSpace * 16 + 8) & 0xFFFFFF;
            if (!pose || !rp || (uint32_t)b.pivot >= rc || rc > (uint32_t)kMaxJ) return false;
            const int16_t pi = ((const int16_t*)rp)[b.pivot];
            if (pi < 0 || (uint32_t)pi >= cnt) return false;
            const float* pm = (const float*)(pose + (size_t)pi * 64);
            const float piv[3] = { pm[12], pm[13], pm[14] };
            for (uint32_t j = 0; j < rc; j++)
            {
                if (!(b.mask[j >> 6] & (1ull << (j & 63)))) continue;
                const int16_t ix = ((const int16_t*)rp)[j];
                if (ix < 0 || (uint32_t)ix >= cnt) continue;
                float* m = (float*)(pose + (size_t)ix * 64);
                const float r0[3] = { m[0], m[1], m[2] }, r1[3] = { m[4], m[5], m[6] }, r2[3] = { m[8], m[9], m[10] };
                if (b.rowsAxes)
                {   // each row is one of the joint's axes: each turns
                    float o[3];
                    Mul(b.R, r0, o); m[0] = o[0]; m[1] = o[1]; m[2] = o[2];
                    Mul(b.R, r1, o); m[4] = o[0]; m[5] = o[1]; m[6] = o[2];
                    Mul(b.R, r2, o); m[8] = o[0]; m[9] = o[1]; m[10] = o[2];
                }
                else
                {   // the columns are the axes: R * M
                    const float* R = b.R;
                    for (int c = 0; c < 3; c++)
                    {
                        const float a = r0[c], bb = r1[c], cc = r2[c];
                        m[0 + c] = R[0] * a + R[1] * bb + R[2] * cc;
                        m[4 + c] = R[3] * a + R[4] * bb + R[5] * cc;
                        m[8 + c] = R[6] * a + R[7] * bb + R[8] * cc;
                    }
                }
                const float d[3] = { m[12] - piv[0], m[13] - piv[1], m[14] - piv[2] };
                float o[3]; Mul(b.R, d, o);
                m[12] = piv[0] + o[0]; m[13] = piv[1] + o[1]; m[14] = piv[2] + o[2];
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    uintptr_t HkCommit(void* obj, int space)
    {
        const uintptr_t r = g_Real(obj, space);
        if (space != kDrawnSpace || g_PubCount.load(std::memory_order_relaxed) == 0) return r;
        Pub p; bool found = false;
        {
            std::lock_guard lk(g_PM);
            for (int i = 0; i < g_PubN; i++) if (g_Pub[i].obj == (uintptr_t)obj) { p = g_Pub[i]; found = true; break; }
        }
        if (!found) return r;
        bool any = false;
        for (int i = 0; i < p.n; i++) { if (ApplyBend((uintptr_t)obj, p.b[i])) { g_Applied++; any = true; } else g_Faults++; }
        if (any && p.slot >= 0 && p.slot < 8) g_AppliedAt[p.slot] = GetTickCount64();
        static std::atomic<bool> told{ false };
        if (any && !told.exchange(true)) LOGI("SoftBody: a character's drawn pose bent (the first time)");
        return r;
    }

    void Install()
    {
        if (g_Installed) return;
        g_Installed = true;
        const uintptr_t b = GameInfo::Base();
        uint8_t have[8] = {};
        if (!Rd(have, (const void*)(b + kCommit), 8) || memcmp(have, kCommitBytes, 8) != 0)
        {
            LOGI("SoftBody: the pose commit exe+0x{:X} is not as expected (bytes {:02X} {:02X} {:02X} {:02X}): characters stay as the game poses them",
                 kCommit, have[0], have[1], have[2], have[3]);
            return;
        }
        g_Real = (Commit_t)(b + kCommit);
        DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_Real, HkCommit);
        const LONG e = DetourTransactionCommit();
        if (e != NO_ERROR) g_Real = nullptr;
        LOGI("SoftBody: the pose commit exe+0x{:X} hooked (detour={}): characters give where a hand presses", kCommit, e);
    }

    // ---- skeletons: the parents, worked out once per definition (SwordContact's thread) ----
    struct Skel { int J = 0; bool ok = false, rowsAxes = true; short parent[kMaxJ]; short size[kMaxJ]; float bind[kMaxJ][3]; float err = 0; int neck = -1; };
    std::unordered_map<uintptr_t, Skel> g_Skels;
    std::unordered_map<uintptr_t, double> g_SkelTried;
    struct Mat { float m[16]; };
    bool Remap(uintptr_t obj, int space, uintptr_t& rp, uint32_t& rc)
    {
        const uintptr_t table = P(obj, kObjRemaps); uint32_t spaces = 0;
        if (!table || !Rd(&spaces, (const void*)(obj + kObjRemaps + 8), 4) || (spaces & 0xFFFFFF) <= (uint32_t)space) return false;
        rp = P(table, space * 16);
        if (!rp || !Rd(&rc, (const void*)(table + space * 16 + 8), 4)) return false;
        rc &= 0xFFFFFF;
        return true;
    }
    bool ReadSpace(uintptr_t obj, int space, int J, Mat* out)
    {
        const uintptr_t pose = P(obj, kObjPose); uint32_t cnt = 0; Rd(&cnt, (const void*)(obj + kObjCount), 4); cnt &= 0xFFFFFF;
        uintptr_t rp = 0; uint32_t rc = 0;
        if (!Remap(obj, space, rp, rc)) return false;
        if (!pose || !rp || rc != (uint32_t)J) return false;
        for (int j = 0; j < J; j++)
        {
            int16_t ix = -1;
            if (!Rd(&ix, (const void*)(rp + j * 2), 2) || ix < 0 || (uint32_t)ix >= cnt) return false;
            if (!Rd(out[j].m, (const void*)(pose + (size_t)ix * 64), 64) || !Finite(out[j].m, 3) || !Finite(out[j].m + 12, 3)) return false;
        }
        return true;
    }
    const Skel* SkelOf(uintptr_t obj, uintptr_t def, int J, double t)
    {
        auto it = g_Skels.find(def);
        if (it != g_Skels.end()) return it->second.ok ? &it->second : nullptr;
        auto tr = g_SkelTried.find(def);
        if (tr != g_SkelTried.end() && t - tr->second < 2.0) return nullptr;
        g_SkelTried[def] = t;
        // The bind pose: the skinning's inverse binds, def+0x120 (3x4, 0x30 bytes each: exe+0x24C340 -> exe+0x12E1D00
        // multiplies the posed joints by them). The posed joints (the animation's space): the entries' rows are the
        // joint's axes in model space, +0x30 its position (exe+0x9B980 turns them into the same 3x4 layout, the rows
        // as its columns). The pose array's spaces 1-3 share one block, the animation's -- not a bind pose.
        static Mat B[2][kMaxJ], A[kMaxJ];
        float ib[kMaxJ][12];
        const uintptr_t inv = P(def, 0x120);
        if (J < 3 || J > kMaxJ || !inv || !Rd(ib, (const void*)inv, (size_t)J * 48) || !ReadSpace(obj, kAnimSpace, J, A)) return nullptr;
        for (int j = 0; j < J; j++)
        {
            const float* f = ib[j];
            Mat& x = B[0][j]; Mat& y = B[1][j]; memset(&x, 0, sizeof x); memset(&y, 0, sizeof y);
            for (int k = 0; k < 3; k++)
                for (int c = 0; c < 3; c++)
                {
                    x.m[k * 4 + c] = f[k * 4 + c];   // the inverse of [Q | u]: axis k = row k of Q
                    y.m[k * 4 + c] = f[c * 4 + k];   // [Q | u] taken as the bind itself: axis k = column k
                }
            for (int c = 0; c < 3; c++)
            {
                x.m[12 + c] = -(f[0 * 4 + c] * f[3] + f[1 * 4 + c] * f[7] + f[2 * 4 + c] * f[11]);
                y.m[12 + c] = f[c * 4 + 3];
            }
        }
        // the true parent keeps the child at the same offset in its own frame, in the bind pose and the animated one
        Skel s; s.J = J;
        short par[2][kMaxJ]; double tot[2] = { 0, 0 };
        for (int v = 0; v < 2; v++)
        {
            par[v][0] = -1;
            for (int j = 1; j < J; j++)
            {
                float best = 1e9f; short bi = -1;
                for (int i = 0; i < j; i++)
                {
                    float lb[3], la[3];
                    const float* mb = B[v][i].m; const float* ma = A[i].m;
                    const float db[3] = { B[v][j].m[12] - mb[12], B[v][j].m[13] - mb[13], B[v][j].m[14] - mb[14] };
                    const float da[3] = { A[j].m[12] - ma[12], A[j].m[13] - ma[13], A[j].m[14] - ma[14] };
                    for (int k = 0; k < 3; k++) { lb[k] = mb[k * 4] * db[0] + mb[k * 4 + 1] * db[1] + mb[k * 4 + 2] * db[2]; la[k] = ma[k * 4] * da[0] + ma[k * 4 + 1] * da[1] + ma[k * 4 + 2] * da[2]; }
                    const float e[3] = { lb[0] - la[0], lb[1] - la[1], lb[2] - la[2] };
                    const float err = Len(e) + 0.01f * Len(db);   // (ties: the nearer)
                    if (err < best) { best = err; bi = (short)i; }
                }
                par[v][j] = bi;
                tot[v] += best;
            }
        }
        const int c = tot[0] <= tot[1] ? 0 : 1;
        s.rowsAxes = true;
        memcpy(s.parent, par[c], sizeof(short) * J);
        s.err = (float)(tot[c] / (J - 1));
        for (int j = 0; j < J; j++) { s.bind[j][0] = B[c][j].m[12]; s.bind[j][1] = B[c][j].m[13]; s.bind[j][2] = B[c][j].m[14]; }
        s.ok = s.err < 0.02f;
        int roots = 0; for (int j = 0; j < J; j++) if (s.parent[j] < 0) roots++;
        // each joint's subtree, and the neck: the joint holding the most of the body within the top 13 % of its height
        // (the head and the face hang from it), not the spine below (half the body or more)
        for (int j = 0; j < J; j++) s.size[j] = 1;
        for (int j = J - 1; j > 0; j--) if (s.parent[j] >= 0) s.size[s.parent[j]] += s.size[j];
        float top = -1e9f, bottom = 1e9f; for (int j = 0; j < J; j++) { top = std::max(top, s.bind[j][2]); bottom = std::min(bottom, s.bind[j][2]); }
        const float band = 0.13f * (top - bottom);
        for (int j = 0; j < J; j++)
            if (s.bind[j][2] > top - band && s.size[j] >= 10 && s.size[j] < J / 2 && (s.neck < 0 || s.size[j] > s.size[s.neck])) s.neck = j;
        LOGI("SoftBody: skeleton {:#x} ({} joints): parents worked out from its {} (mean offset change {:.1f} mm; read the other way {:.1f} mm){}; the neck joint {} ({} joints hang from it)",
             def, J, c == 0 ? "inverse binds inverted" : "binds as stored", s.err * 1000.f, tot[1 - c] / (J - 1) * 1000.0, s.ok ? "" : ": too large, not used",
             s.neck, s.neck >= 0 ? s.size[s.neck] : 0);
        {
            std::string pl; for (int j = 0; j < J && j < 64; j++) pl += fmt::format(" {}", s.parent[j]);
            LOGI("SoftBody:   parents of joints 0-63:{}", pl);
        }
        (void)roots;
        auto ins = g_Skels.insert_or_assign(def, s);
        return ins.first->second.ok ? &ins.first->second : nullptr;
    }

    // ---- the characters being bent (SwordContact's thread) ----
    struct Bend
    {
        bool on = false, pressing = false, grab = false;
        int hand = 0, pivot = -1; uint64_t mask[4] = {}; int size = 0;
        float lever[3] = {};          // model space: pivot -> the touched point, when the bend began
        float anchor[3] = {};         // model space: where the grip took hold
        float w[3] = {}, v[3] = {}, target[3] = {};
        double lastPress = 0;
    };
    struct Char
    {
        void* ent = nullptr; uintptr_t obj = 0, def = 0, model = 0; const Skel* sk = nullptr;
        Bend b[kBends]; double seen = 0; char name[64] = {};
        bool Idle() const { for (const Bend& x : b) if (x.on) return false; return true; }
    };
    Char g_C[kChars];
    double g_LastTick = 0;
    bool g_GripWas[2] = {};

    void Name(const void* ent, char* out, size_t cap)
    {
        out[0] = 0;
        const uintptr_t s = P((uintptr_t)ent, 0x20);
        char tmp[64] = {};
        if (!s || !Rd(tmp, (const void*)s, sizeof tmp - 1)) return;
        size_t i = 0; for (; i + 1 < cap && i < sizeof tmp - 1 && tmp[i] >= 32 && tmp[i] < 127; i++) out[i] = tmp[i];
        out[i] = 0;
    }
    bool ModelFrame(uintptr_t model, float org[3], float ax[9])
    {
        return Rd(org, (const void*)(model + kModelOrigin), 12) && Rd(ax, (const void*)(model + kModelAxis), 36) && Finite(org, 3) && Finite(ax, 9);
    }
    void ToModelDir(const float ax[9], const float w[3], float o[3]) { o[0] = Dot(ax, w); o[1] = Dot(ax + 3, w); o[2] = Dot(ax + 6, w); }
    void ToModel(const float org[3], const float ax[9], const float w[3], float o[3]) { const float d[3] = { w[0] - org[0], w[1] - org[1], w[2] - org[2] }; ToModelDir(ax, d, o); }
    bool JointPos(uintptr_t obj, int space, int j, float o[3])
    {
        const uintptr_t pose = P(obj, kObjPose);
        uintptr_t rp = 0; uint32_t rc = 0;
        int16_t ix = -1;
        if (!pose || !Remap(obj, space, rp, rc) || (uint32_t)j >= rc || !Rd(&ix, (const void*)(rp + j * 2), 2) || ix < 0) return false;
        return Rd(o, (const void*)(pose + (size_t)ix * 64 + 0x30), 12) && Finite(o, 3);
    }
    Char* CharOf(void* ent, double t)
    {
        for (Char& c : g_C) if (c.ent == ent) return &c;
        Char* slot = nullptr;
        for (Char& c : g_C) if (!c.ent || (c.Idle() && t - c.seen > 2.0)) { slot = &c; break; }
        if (!slot) return nullptr;
        *slot = Char{};
        slot->ent = ent;
        slot->obj = P((uintptr_t)ent, kEntSkelObj);
        slot->model = P((uintptr_t)ent, kEntModel);
        if (!slot->obj || !slot->model || P(slot->obj, kObjModel) != slot->model) { slot->ent = nullptr; return nullptr; }
        slot->def = P(P(slot->model, kModelSkel), kSkelDef);
        uint16_t J = 0; if (!slot->def || !Rd(&J, (const void*)(slot->def + kDefJoints), 2)) { slot->ent = nullptr; return nullptr; }
        slot->sk = SkelOf(slot->obj, slot->def, J, t);
        if (!slot->sk) { slot->ent = nullptr; return nullptr; }
        Name(ent, slot->name, sizeof slot->name);
        return slot;
    }
    bool Under(const Skel& s, int j, int a) { for (int k = 0; j >= 0 && k < kMaxJ; k++) { if (j == a) return true; j = s.parent[j]; } return false; }
    //! Where a press on joint t bends. Anything of the head (the face's many small joints -- an ear, a cheek, a brow):
    //! the whole head at the neck, so the face keeps its shape. Elsewhere: the first joint up the chain at least
    //! kPivotReach away, never one holding (nearly) the whole body.
    int PivotFor(const Skel& s, int t)
    {
        if (t < 0 || t >= s.J) return -1;
        if (s.neck >= 0 && Under(s, t, s.neck)) return s.neck;
        int a = t, steps = 0;
        while (s.parent[a] >= 0 && steps < 12)
        {
            const float d[3] = { s.bind[t][0] - s.bind[a][0], s.bind[t][1] - s.bind[a][1], s.bind[t][2] - s.bind[a][2] };
            if (a != t && Len(d) >= kPivotReach && s.size[a] >= 3) break;
            if (s.size[s.parent[a]] > (s.J * 9) / 10) break;   // its parent holds the whole body: stop here
            a = s.parent[a]; steps++;
        }
        return a;
    }
    bool StartBend(Char& c, Bend& b, int h, int t, int pivot, const float contactM[3], bool grab)
    {
        const Skel& s = *c.sk;
        b = Bend{};
        b.hand = h; b.pivot = pivot;
        for (int j = 0; j < s.J; j++)
            if (j == pivot || (s.parent[j] >= 0 && (b.mask[s.parent[j] >> 6] & (1ull << (s.parent[j] & 63))))) { b.mask[j >> 6] |= 1ull << (j & 63); b.size++; }
        float pv[3];
        if (!JointPos(c.obj, kAnimSpace, pivot, pv)) return false;
        for (int k = 0; k < 3; k++) { b.lever[k] = contactM[k] - pv[k]; b.anchor[k] = contactM[k]; }
        if (Len(b.lever) < 0.03f) return false;
        b.on = true; b.grab = grab;
        g_Bends++;
        if (g_Bends.load() <= 80)
            LOGI("SoftBody: '{}' {} by the {} hand: touched at joint {}, it bends at joint {}{} ({} joints move, {:.0f} cm from the touch)", c.name,
                 grab ? "pulled (grip)" : "pushed", h ? "right" : "left", t, pivot, pivot == s.neck ? " (the neck: the head)" : "", b.size, Len(b.lever) * 100.f);
        return true;
    }
    void TargetFrom(Bend& b, const float disp[3], float maxD)
    {
        float d[3] = { disp[0], disp[1], disp[2] };
        const float l = Len(d); if (l > maxD) for (float& x : d) x *= maxD / l;
        const float r2 = Dot(b.lever, b.lever);
        float w[3]; Cross(b.lever, d, w); for (float& x : w) x /= std::max(r2, 1e-4f);
        const float a = Len(w); if (a > kMaxAngle) for (float& x : w) x *= kMaxAngle / a;
        memcpy(b.target, w, 12);
    }
    //! the bend this hand's press drives: its own on the same part, else a new one (a part already springing back
    //! is taken over) -- the hand moving on to another part lets the one it left spring back
    Bend* BendFor(Char& c, int h, int t, int pivot, const float contactM[3], bool grip)
    {
        for (Bend& b : c.b) if (b.on && b.hand == h && b.pivot == pivot) return &b;
        for (Bend& b : c.b) if (b.on && b.hand == h && b.pressing) { b.pressing = false; b.grab = false; }
        for (Bend& b : c.b)
            if (b.on && b.pivot == pivot && !b.pressing) { b.hand = h; for (int k = 0; k < 3; k++) b.anchor[k] = contactM[k]; return &b; }   // (its lever kept)
        Bend* slot = nullptr;
        for (Bend& b : c.b) if (!b.on) { slot = &b; break; }
        if (!slot)
        {   // all four busy: the one nearest rest that no hand holds
            float least = 1e9f;
            for (Bend& b : c.b) if (!b.pressing && Len(b.w) < least) { least = Len(b.w); slot = &b; }
        }
        if (!slot || !StartBend(c, *slot, h, t, pivot, contactM, grip)) return nullptr;
        return slot;
    }
}

void SoftBody::SetEnabled(bool on)
{
    g_On = on;
    if (on) Install();
    LOGI("SoftBody: {}", on && g_Real ? "characters give where a hand presses and spring back (SoftCharacters)" : "characters are solid (SoftCharacters off)");
}
bool SoftBody::Enabled() { return g_On.load() && g_Real != nullptr; }

float SoftBody::Press(int hand, void* ent, int joint, const float surface[3], const float n[3], float depth, const float handPos[3], double t)
{
    const int h = hand & 1;
    if (!Enabled()) return 0.f;
    XRInputState in{};
    const bool gotIn = VRRuntime::GetInput(in);
    const bool grip = gotIn && (h ? in.gripPressR : in.gripPressL) > 0.6f && !GrabHand::HeldEntity();
    const bool gripNew = grip && !g_GripWas[h];
    g_GripWas[h] = grip;
    // a pull already held keeps following the hand while the grip is (the hand may leave the body)
    for (Char& c : g_C)
        for (Bend& b : c.b)
        {
            if (!c.ent || !b.on || !b.grab || b.hand != h || (ent && c.ent == ent)) continue;
            float org[3], ax[9], hm[3];
            if (!grip || !ModelFrame(c.model, org, ax)) { b.grab = false; continue; }
            ToModel(org, ax, handPos, hm);
            float disp[3] = { hm[0] - b.anchor[0], hm[1] - b.anchor[1], hm[2] - b.anchor[2] };
            if (Len(disp) > 0.5f) { b.grab = false; continue; }
            for (float& x : disp) x *= kPullGain;
            TargetFrom(b, disp, kMaxPull); b.pressing = true; b.lastPress = t;
        }
    if (!ent) return 0.f;
    Char* c = CharOf(ent, t);
    if (!c) return 0.f;
    c->seen = t;
    float org[3], ax[9];
    if (!ModelFrame(c->model, org, ax)) return 0.f;
    float sm[3], nm[3], hm[3];
    ToModel(org, ax, surface, sm); ToModelDir(ax, n, nm); ToModel(org, ax, handPos, hm);
    // a pull holds on to its part, wherever the hand goes over the body
    Bend* b = nullptr;
    for (Bend& x : c->b) if (x.on && x.hand == h && x.grab && grip) b = &x;
    if (!b)
    {
        const int pivot = PivotFor(*c->sk, joint);
        if (pivot < 0) return 0.f;
        b = BendFor(*c, h, joint, pivot, sm, grip);
        if (!b) return 0.f;
    }
    if (gripNew && !b->grab) { b->grab = true; memcpy(b->anchor, sm, 12); }
    if (b->grab)
    {
        float disp[3] = { hm[0] - b->anchor[0], hm[1] - b->anchor[1], hm[2] - b->anchor[2] };
        for (float& x : disp) x *= kPullGain;
        TargetFrom(*b, disp, kMaxPull);
        b->pressing = true; b->lastPress = t;
        return 0.f;
    }
    const float push[3] = { -nm[0], -nm[1], -nm[2] };
    const float d = kPushGain * std::clamp(depth, 0.f, kMaxDepth);
    const float disp[3] = { push[0] * d, push[1] * d, push[2] * d };
    TargetFrom(*b, disp, kMaxMove);
    b->pressing = true; b->lastPress = t;
    // how far the body has given at the touch, along the push: the hand's stop moves in by that (never past the real
    // hand; only while the drawn pose is really being bent: else the hand would sink into a body that did not move)
    if (GetTickCount64() - g_AppliedAt[c - g_C].load() > 250) return 0.f;
    float moved[3]; Cross(b->w, b->lever, moved);
    return std::clamp(Dot(moved, push), 0.f, depth);
}

void SoftBody::Tick(double t)
{
    const float dt = g_LastTick > 0 ? (float)std::clamp(t - g_LastTick, 0.0, 0.05) : 0.f;
    g_LastTick = t;
    if (!Enabled()) { if (g_PubCount.load()) { std::lock_guard lk(g_PM); g_PubN = 0; g_PubCount = 0; } return; }
    Pub pub[kChars]; int np = 0;
    for (Char& c : g_C)
    {
        if (!c.ent) continue;
        if (P((uintptr_t)c.ent, kEntSkelObj) != c.obj) { c = Char{}; continue; }   // gone, or no longer the same body
        uint8_t f = 0; Rd(&f, (const uint8_t*)c.ent + 0xD1, 1);
        Pub& p = pub[np]; p = Pub{}; p.obj = c.obj; p.slot = (int)(&c - g_C);
        for (Bend& b : c.b)
        {
            if (!b.on) continue;
            if (f & 0x28) { b = Bend{}; continue; }                        // down (a ragdoll): left to the physics
            if (b.pressing && t - b.lastPress > 0.08) b.pressing = false;   // the hand left
            if (!b.pressing) { b.grab = false; memset(b.target, 0, 12); }
            const float hz = b.pressing ? kHoldHz : kFreeHz, zeta = b.pressing ? kHoldZeta : kFreeZeta;
            const float k = (6.2831853f * hz) * (6.2831853f * hz), cd = 2.f * zeta * sqrtf(k);
            const int steps = dt > 0.012f ? 2 : 1; const float sdt = dt / steps;
            for (int s = 0; s < steps; s++)
                for (int i = 0; i < 3; i++) { b.v[i] += (k * (b.target[i] - b.w[i]) - cd * b.v[i]) * sdt; b.w[i] += b.v[i] * sdt; }
            if (!b.pressing && Len(b.w) < 0.002f && Len(b.v) < 0.02f) { b = Bend{}; continue; }
            PubBend& pb = p.b[p.n++];
            pb.pivot = b.pivot; pb.rowsAxes = c.sk->rowsAxes; memcpy(pb.mask, b.mask, 32); RotOf(b.w, pb.R);
        }
        if (p.n) np++;
    }
    std::lock_guard lk(g_PM);
    g_PubN = np; for (int i = 0; i < np; i++) g_Pub[i] = pub[i];
    g_PubCount = np;
}

std::string SoftBody::Status()
{
    int bent = 0; for (const Char& c : g_C) if (c.ent && !c.Idle()) bent++;
    return fmt::format("soft characters {} (bent now {}, bends {}, entries turned {}, faults {}, skeletons {})",
                       g_On.load() ? (g_Real ? "on" : "on, hook not installed") : "off", bent, g_Bends.load(), g_Applied.load(), g_Faults.load(), g_Skels.size());
}
