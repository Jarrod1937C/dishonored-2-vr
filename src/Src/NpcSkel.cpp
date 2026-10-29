#include "pch.h"
#include "NpcSkel.h"
#include "GameInfo.h"
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <algorithm>

namespace
{
    constexpr uint32_t kGamePtr = 0x39A5290, kEntTable = 0x3130B0, kEntSlots = 0x4000, kEntId = 0xA8, kEntModel = 0xF0;
    constexpr uint32_t kObjModel = 0x38, kObjPose = 0x2D8, kModelOrigin = 0x1860, kModelAxis = 0x186C;
    constexpr uint32_t kModelSkel = 0x1FE8, kSkelDef = 0x50, kDefJoints = 0x78;

    bool SafeCopy(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    template <class T> bool Rd(const void* p, T& v) { return SafeCopy(&v, p, sizeof(T)); }
    bool HeapPtr(uintptr_t p) { return p > 0x10000 && p < 0x7FF000000000ull && !(p & 7); }
    bool Finite(const float* v, int n) { for (int i = 0; i < n; i++) if (!std::isfinite(v[i]) || fabsf(v[i]) > 1e5f) return false; return true; }

    void* EntityById(uint32_t id)
    {
        uint8_t* game = nullptr;
        if (!Rd((void*)(GameInfo::Base() + kGamePtr), game) || !game) return nullptr;
        uint8_t** table = (uint8_t**)(game + kEntTable);
        for (uint32_t i = 0; i < kEntSlots; i++)
        {
            uint8_t* e = nullptr; uint32_t eid = 0;
            if (!Rd(table + i, e) || !e || !Rd(e + kEntId, eid)) continue;
            if (eid == id) return e;
        }
        return nullptr;
    }
    unsigned long long g_Gets = 0, g_Found = 0, g_NoObj = 0, g_Odd = 0;
    std::atomic<int> g_Mode{ 1 };

    bool Readable(uintptr_t p, size_t n)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!HeapPtr(p) || !VirtualQuery((void*)p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        return p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    // The pose array (64-byte entries, model space) starts with the bind pose [0, J); each "space" has a remap (obj+0x2E8
    // + space*16: {short*, count}) from joint to entry, and space 3 is what the engine hangs things on a character with
    // (exe+0x7506D0 binds through it): the ANIMATION's pose. A ragdoll writes its own J entries at the END of the array --
    // measured on a corpse: space 3 still stood 1.8 m tall where the animation left it, the last J entries lay on the floor
    // where the body is drawn (667 entries = 3J-2 before a strike, 530 after it: the array is rebuilt, the ragdoll set is
    // always the tail). So: the tail when it is a real pose of this skeleton (its bones keep their bind lengths) and lies
    // elsewhere than the animation's, else space 3.
    constexpr uint32_t kEntSkelObj = 0x360, kObjRemap = 0x2E8;
    // What a model's skeleton never changes: the bone capsules and the radius around each joint, from the bind pose.
    struct Shape { int n = 0; short link[NpcSkel::kMaxJoints]; float r[NpcSkel::kMaxJoints]; float len[NpcSkel::kMaxJoints]; bool humanoid = false; };
    unsigned long long g_Ragdoll = 0, g_Anim = 0;
    float g_LastLenErr = -1, g_LastDisp = -1;
    std::unordered_map<uintptr_t, Shape> g_Shapes;   // by skeleton definition

    const Shape* ShapeOf(uintptr_t def, uintptr_t pose, int J)
    {
        auto it = g_Shapes.find(def);
        if (it != g_Shapes.end() && it->second.n == J) return &it->second;
        if (g_Shapes.size() > 64) g_Shapes.clear();
        Shape s; s.n = J;
        static float b[NpcSkel::kMaxJoints][3];
        float top = -1e9f;
        for (int j = 0; j < J; j++)
        {
            float m[16]; if (!Rd((void*)(pose + (size_t)j * 64), m) || !Finite(m + 12, 3)) return nullptr;
            memcpy(b[j], m + 12, 12); top = fmaxf(top, b[j][2]);
        }
        // A humanoid stands 1.4-2.3 m tall in its bind pose, z up from the feet, pelvis near 1 m.
        s.humanoid = top > 1.4f && top < 2.3f && J > 2 && b[2][2] > 0.7f && b[2][2] < 1.3f;
        for (int j = 0; j < J; j++)
        {
            // Joined to the NEAREST earlier joint (a skeleton lists a parent before its children): not always the true
            // parent, but a bone between two joints of the same body part either way, which is all a capsule needs.
            short best = -1; float bd = 0.6f * 0.6f;
            for (int p = 0; p < j; p++)
            {
                const float d[3] = { b[j][0] - b[p][0], b[j][1] - b[p][1], b[j][2] - b[p][2] };
                const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                if (dd < bd && dd > 1e-6f) { bd = dd; best = (short)p; }
            }
            s.link[j] = best;
            s.len[j] = best >= 0 ? sqrtf(bd) : 0.f;
            float r = 0.08f;
            if (s.humanoid)
            {   // the trunk around the spine is wide; the head; the limbs
                const float ax = fabsf(b[j][0]), ay = fabsf(b[j][1]), z = b[j][2];
                if (ax < 0.16f && ay < 0.16f && z > 0.85f && z < 1.50f) r = 0.15f;
                else if (ax < 0.16f && ay < 0.16f && z >= 1.50f) r = 0.11f;
                else if (z < 0.85f) r = 0.09f;        // legs
                else r = 0.07f;                       // arms, hands
            }
            s.r[j] = r;
        }
        return &g_Shapes.insert_or_assign(def, s).first->second;
    }
}

void NpcSkel::SetMode(int m) { g_Mode = m; LOGI("NpcSkel: {}", m ? "characters are struck by their skeleton" : "off (boxes)"); }
int NpcSkel::Mode() { return g_Mode.load(); }

bool NpcSkel::Get(void* entity, Skel& out)
{
    // SwordContact reads skeletons on the input thread, MeleeAim's knockout blow on the game thread: one at a time
    // (the scratch buffers and the shape cache are shared)
    static std::mutex s_M; std::lock_guard lk(s_M);
    out.n = 0; g_Gets++;
    if (!g_Mode.load() || !entity) return false;
    uint8_t* e = (uint8_t*)entity;
    uintptr_t model = 0, obj = 0, objModel = 0, skel = 0, def = 0, pose = 0; uint16_t J = 0; uint32_t cnt = 0;
    if (!Rd(e + kEntModel, model) || !Readable(model, 0x2000) || !Rd(e + kEntSkelObj, obj) || !Readable(obj, 0x300)) { g_NoObj++; return false; }
    if (!Rd((void*)(obj + kObjModel), objModel) || objModel != model) { g_NoObj++; return false; }
    if (!Rd((void*)(model + kModelSkel), skel) || !Readable(skel, 0x58) || !Rd((void*)(skel + kSkelDef), def) || !Readable(def, 0x80) || !Rd((void*)(def + kDefJoints), J)) { g_NoObj++; return false; }
    if (!Rd((void*)(obj + kObjPose), pose) || !Rd((void*)(obj + kObjPose + 8), cnt)) { g_NoObj++; return false; }
    cnt &= 0xFFFFFF;
    if (J < 3 || J > kMaxJoints || !Readable(pose, (size_t)cnt * 64)) { g_Odd++; return false; }
    if (cnt < (uint32_t)J) { g_Odd++; return false; }
    const Shape* sh = ShapeOf(def, pose, J);
    if (!sh) { g_Odd++; return false; }
    float org[3], ax[9];
    if (!Rd((void*)(model + kModelOrigin), org) || !Rd((void*)(model + kModelAxis), ax) || !Finite(org, 3) || !Finite(ax, 9)) return false;
    // the animation's pose through space 3's remap (the bind pose if there is none)
    static float A[kMaxJoints][3], B[kMaxJoints][3];
    uintptr_t rp = 0; uint32_t rc = 0;
    Rd((void*)(obj + kObjRemap + 3 * 16), rp); Rd((void*)(obj + kObjRemap + 3 * 16 + 8), rc); rc &= 0xFFFFFF;
    const bool remap = rc == J && Readable(rp, (size_t)J * 2);
    for (int j = 0; j < J; j++)
    {
        int16_t idx = (int16_t)j;
        if (remap && (!Rd((void*)(rp + (size_t)j * 2), idx) || idx < 0 || (uint32_t)idx >= cnt)) idx = (int16_t)j;
        float m[16];
        if (!Rd((void*)(pose + (size_t)idx * 64), m) || !Finite(m + 12, 3)) { g_Odd++; return false; }
        memcpy(A[j], m + 12, 12);
    }
    // the tail: a ragdoll's pose, if it is one
    bool rag = false;
    if (cnt >= 2u * J)
    {
        static float errs[kMaxJoints]; int nl = 0; double disp = 0; bool ok = true;
        for (int j = 0; j < J && ok; j++)
        {
            float m[16];
            if (!Rd((void*)(pose + (size_t)(cnt - J + j) * 64), m) || !Finite(m + 12, 3)) { ok = false; break; }
            memcpy(B[j], m + 12, 12);
        }
        for (int j = 0; j < J && ok; j++)
        {
            const float d[3] = { B[j][0] - A[j][0], B[j][1] - A[j][1], B[j][2] - A[j][2] };
            disp += sqrt((double)d[0] * d[0] + (double)d[1] * d[1] + (double)d[2] * d[2]);
            if (sh->link[j] < 0) continue;
            const float* q = B[sh->link[j]];
            const float e[3] = { B[j][0] - q[0], B[j][1] - q[1], B[j][2] - q[2] };
            errs[nl++] = fabsf(sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]) - sh->len[j]);
        }
        if (ok && nl)
        {   // the MEDIAN bone: the joined-to-the-nearest pairs are not all rigid (measured mean 2.5 cm on a true ragdoll pose)
            std::nth_element(errs, errs + nl / 2, errs + nl);
            const float med = errs[nl / 2];
            disp /= J; rag = med < 0.01f && disp > 0.10;
            g_LastLenErr = med; g_LastDisp = (float)disp;
        }
    }
    if (rag) g_Ragdoll++; else g_Anim++;
    out.ragdoll = rag;
    for (int k = 0; k < 3; k++) { out.lo[k] = 1e9f; out.hi[k] = -1e9f; }
    for (int j = 0; j < J; j++)
    {
        const float* t = rag ? B[j] : A[j];
        for (int k = 0; k < 3; k++) out.p[j][k] = org[k] + t[0] * ax[k] + t[1] * ax[3 + k] + t[2] * ax[6 + k];
        for (int k = 0; k < 3; k++) { out.lo[k] = fminf(out.lo[k], out.p[j][k]); out.hi[k] = fmaxf(out.hi[k], out.p[j][k]); }
        out.link[j] = sh->link[j]; out.r[j] = sh->r[j];
    }
    out.n = J; out.humanoid = sh->humanoid;
    g_Found++;
    return true;
}

void NpcSkel::Probe(uint32_t id)
{
    uint8_t* e = (uint8_t*)EntityById(id);
    if (!e) { LOGI("skelprobe: entity id {:#x} not found", id); return; }
    uintptr_t model = 0; Rd(e + kEntModel, model);
    uintptr_t vt = 0; Rd(e, vt);
    LOGI("skelprobe: entity {} id {:#x} vt exe+{:#x} model {:#x}", (void*)e, id, vt - GameInfo::Base(), model);
    if (!model) return;
    // depth 1 and 2: who holds a pointer whose +0x38 is this model
    int hits = 0;
    std::vector<uintptr_t> objs;
    for (uint32_t off = 0; off < 0x1000 && hits < 24; off += 8)
    {
        uintptr_t p = 0;
        if (!Rd(e + off, p) || !HeapPtr(p)) continue;
        uintptr_t m = 0;
        if (Rd((void*)(p + kObjModel), m) && m == model) { LOGI("skelprobe: entity+{:#x} -> obj {:#x} (obj+0x38 == model)", off, p); objs.push_back(p); hits++; }
        if (off < 0x800)
            for (uint32_t o2 = 0; o2 < 0x400 && hits < 24; o2 += 8)
            {
                uintptr_t q = 0;
                if (!Rd((void*)(p + o2), q) || !HeapPtr(q) || q == p) continue;
                if (Rd((void*)(q + kObjModel), m) && m == model) { LOGI("skelprobe: entity+{:#x} -> {:#x} +{:#x} -> obj {:#x}", off, p, o2, q); objs.push_back(q); hits++; }
            }
    }
    // the model itself: a back pointer to its skeleton object?
    for (uint32_t off = 0; off < 0x2100; off += 8)
    {
        uintptr_t p = 0, m = 0;
        if (Rd((void*)(model + off), p) && HeapPtr(p) && p != model && Rd((void*)(p + kObjModel), m) && m == model)
            LOGI("skelprobe: model+{:#x} -> obj {:#x}", off, p);
    }
    uintptr_t skel = 0, def = 0; uint16_t nj = 0;
    Rd((void*)(model + kModelSkel), skel); if (skel) Rd((void*)(skel + kSkelDef), def); if (def) Rd((void*)(def + kDefJoints), nj);
    LOGI("skelprobe: model+0x1FE8 {:#x} -> +0x50 {:#x} -> joints {}", skel, def, nj);
    if (def)
    {   // the skeleton definition: look for a parent table (shorts, p[0] < 0, p[i] < i)
        std::string s;
        for (uint32_t off = 0; off < 0x100; off += 8) { uint64_t q = 0; Rd((void*)(def + off), q); s += fmt::format(" +{:02x}:{:x}", off, q); }
        LOGI("skelprobe: def{}", s);
        for (uint32_t off = 0; off < 0x100; off += 8)
        {
            uintptr_t p = 0; if (!Rd((void*)(def + off), p) || !HeapPtr(p)) continue;
            for (int stride : { 2, 4 })
            {
                int ok = 0; int first = 0;
                for (int i = 0; i < nj && i < 200; i++)
                {
                    int v = 0;
                    if (stride == 2) { int16_t x; if (!Rd((void*)(p + i * 2), x)) break; v = x; } else { int32_t x; if (!Rd((void*)(p + i * 4), x)) break; v = x; }
                    if (i == 0) first = v;
                    if ((i == 0 && v < 0) || (i > 0 && v >= 0 && v < i)) ok++; else break;
                }
                if (ok == nj && nj > 2) LOGI("skelprobe: def+{:#x} -> {:#x}: a PARENT table ({}-byte entries), root {}", off, p, stride, first);
            }
            // arrays of structs: parent short at a fixed field
            for (int stride : { 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50 })
                for (int field = 0; field < stride; field += 2)
                {
                    int ok = 0;
                    for (int i = 0; i < nj && i < 200; i++)
                    {
                        int16_t x; if (!Rd((void*)(p + (size_t)i * stride + field), x)) break;
                        if ((i == 0 && x < 0) || (i > 0 && x >= 0 && x < i)) ok++; else break;
                    }
                    if (ok == nj && nj > 2) LOGI("skelprobe: def+{:#x} -> {:#x}: parent short at +{:#x} of {:#x}-byte records", off, p, field, stride);
                }
        }
    }
    float org[3] = {}, ax[9] = {};
    Rd((void*)(model + kModelOrigin), org); Rd((void*)(model + kModelAxis), ax);
    {   // boxes kept in the model / the skeleton object: 6 floats min..max, 0.15-3 m a side (the culling bounds follow the pose?)
        auto scan = [&](const char* what, uintptr_t base, uint32_t len) {
            for (uint32_t off = 0; off + 24 <= len; off += 4)
            {
                float b[6]; if (!Rd((void*)(base + off), b) || !Finite(b, 6)) continue;
                bool ok = true; float d[3];
                for (int k = 0; k < 3; k++) { d[k] = b[3 + k] - b[k]; if (d[k] < 0.15f || d[k] > 3.f) ok = false; }
                if (!ok) continue;
                const bool world = fabsf(b[0] - org[0]) < 3 && fabsf(b[1] - org[1]) < 3 && fabsf(b[2] - org[2]) < 3;
                const bool local = fabsf(b[0]) < 3 && fabsf(b[1]) < 3 && fabsf(b[2]) < 3;
                if (world || local) LOGI("skelprobe: {}+{:#x} box ({:.2f},{:.2f},{:.2f})..({:.2f},{:.2f},{:.2f}) dims {:.2f} {:.2f} {:.2f} {}", what, off, b[0], b[1], b[2], b[3], b[4], b[5], d[0], d[1], d[2], world ? "WORLD" : "local");
            }
        };
        scan("model", model, 0x2100);
        uintptr_t o = 0; if (Rd(e + 0x360, o) && o) scan("obj", o, 0x600);
        scan("entity", (uintptr_t)e, 0x600);
    }
    LOGI("skelprobe: model world origin ({:.2f},{:.2f},{:.2f}) | entity origin ({:.2f},{:.2f},{:.2f})", org[0], org[1], org[2],
         *(float*)(e + 0x300), *(float*)(e + 0x304), *(float*)(e + 0x308));
    for (uintptr_t obj : objs)
    {
        uintptr_t pose = 0; uint32_t cnt = 0;
        Rd((void*)(obj + kObjPose), pose); Rd((void*)(obj + kObjPose + 8), cnt);
        std::string rm;
        for (int sp = 0; sp < 4; sp++) { uintptr_t rp = 0; uint32_t rc = 0; Rd((void*)(obj + 0x2E8 + sp * 16), rp); Rd((void*)(obj + 0x2E8 + sp * 16 + 8), rc); rm += fmt::format(" space{} {:#x}:{}", sp, rp, rc & 0xFFFFFF); }
        LOGI("skelprobe: obj {:#x} pose {:#x} count {} ({}){}", obj, pose, cnt & 0xFFFFFF, cnt, rm);
        const int n = (int)(cnt & 0xFFFFFF);
        float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
        for (int j = 0; j < n && j < 200; j++)
        {
            float m[16];
            if (!Rd((void*)(pose + (size_t)j * 64), m)) break;
            const float* t = m + 12;
            float w[3]; for (int k = 0; k < 3; k++) w[k] = org[k] + t[0] * ax[k] + t[1] * ax[3 + k] + t[2] * ax[6 + k];
            for (int k = 0; k < 3; k++) { lo[k] = fminf(lo[k], w[k]); hi[k] = fmaxf(hi[k], w[k]); }
            if (j < 6 || j % 10 == 0) LOGI("skelprobe:   joint {} model ({:.3f},{:.3f},{:.3f}) world ({:.2f},{:.2f},{:.2f}) row0 ({:.2f},{:.2f},{:.2f}) w {:.2f} {:.2f} {:.2f} {:.2f}",
                                        j, t[0], t[1], t[2], w[0], w[1], w[2], m[0], m[1], m[2], m[3], m[7], m[11], m[15]);
        }
        LOGI("skelprobe: obj {:#x} joints span world ({:.2f},{:.2f},{:.2f}) .. ({:.2f},{:.2f},{:.2f})", obj, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
        // the same array in thirds, and through each space's remap
        for (int third = 0; third < 3; third++)
        {
            float l2[3] = { 1e9f, 1e9f, 1e9f }, h2[3] = { -1e9f, -1e9f, -1e9f };
            const int per = n / 3;
            for (int j = third * per; j < (third + 1) * per; j++)
            {
                float m[16]; if (!Rd((void*)(pose + (size_t)j * 64), m)) break;
                float w[3]; for (int k = 0; k < 3; k++) w[k] = org[k] + m[12] * ax[k] + m[13] * ax[3 + k] + m[14] * ax[6 + k];
                for (int k = 0; k < 3; k++) { l2[k] = fminf(l2[k], w[k]); h2[k] = fmaxf(h2[k], w[k]); }
            }
            LOGI("skelprobe:   third {} [{}..{}) span ({:.2f},{:.2f},{:.2f}) .. ({:.2f},{:.2f},{:.2f})", third, third * per, (third + 1) * per, l2[0], l2[1], l2[2], h2[0], h2[1], h2[2]);
        }
        if (n % 3 == 1 && nj && n == 3 * nj - 2)
        {   // the animation's pose (set 2) against the final one (set 3): equal unless something (a ragdoll) overrides it
            float worst = 0.f; int wj = -1;
            for (int j = 2; j < nj; j++)
            {
                float a[16], b[16];
                if (!Rd((void*)(pose + (size_t)(nj - 2 + j) * 64), a) || !Rd((void*)(pose + (size_t)(2 * nj - 2 + j) * 64), b)) break;
                const float d[3] = { a[12] - b[12], a[13] - b[13], a[14] - b[14] };
                const float dd = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (dd > worst) { worst = dd; wj = j; }
            }
            LOGI("skelprobe:   set 2 vs set 3: worst joint {} differs {:.3f} m", wj, worst);
        }
        for (int sp = 0; sp < 4; sp++)
        {
            uintptr_t rp = 0; uint32_t rc = 0; Rd((void*)(obj + 0x2E8 + sp * 16), rp); Rd((void*)(obj + 0x2E8 + sp * 16 + 8), rc); rc &= 0xFFFFFF;
            float l2[3] = { 1e9f, 1e9f, 1e9f }, h2[3] = { -1e9f, -1e9f, -1e9f }; std::string idx; int mn = 1 << 30, mx = -1;
            for (uint32_t j = 0; j < rc && j < 256; j++)
            {
                int16_t r = 0; if (!Rd((void*)(rp + j * 2), r)) break;
                if (j < 8) idx += fmt::format(" {}", r);
                if (r < 0 || r >= n) continue;
                mn = std::min(mn, (int)r); mx = std::max(mx, (int)r);
                float m[16]; if (!Rd((void*)(pose + (size_t)r * 64), m)) break;
                float w[3]; for (int k = 0; k < 3; k++) w[k] = org[k] + m[12] * ax[k] + m[13] * ax[3 + k] + m[14] * ax[6 + k];
                for (int k = 0; k < 3; k++) { l2[k] = fminf(l2[k], w[k]); h2[k] = fmaxf(h2[k], w[k]); }
            }
            LOGI("skelprobe:   space {} remap{} .. (range {}..{}) span ({:.2f},{:.2f},{:.2f}) .. ({:.2f},{:.2f},{:.2f})", sp, idx, mn, mx, l2[0], l2[1], l2[2], h2[0], h2[1], h2[2]);
        }
    }
}

std::string NpcSkel::Stats()
{
    std::ostringstream o;
    o << "mode " << g_Mode.load() << " gets " << g_Gets << " found " << g_Found << " (ragdoll " << g_Ragdoll << " anim " << g_Anim << " lenerr " << g_LastLenErr << " disp " << g_LastDisp << ") noobj " << g_NoObj << " odd " << g_Odd << " shapes " << g_Shapes.size();
    return o.str();
}
