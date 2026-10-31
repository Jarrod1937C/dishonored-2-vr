#include "pch.h"
#include "GpuTrace.h"
#include "PassProfile.h"
#include "Collision.h"
#include "PipeTrace.h"
#include "PlayerState.h"
#include "GrabHand.h"
#include "WristHud.h"
#include "StereoTaa.h"
#include "ScreenMode.h"
#include <detours/detours.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <array>
#include <bitset>
#include <algorithm>
#include <d3dcommon.h>
#include <d3d11shader.h>
#include <cmath>
#include "GameInfo.h"
#include "ViewMatrixHook.h"
#include "HandAimHook.h"
#include "VRRuntime.h"
#include "BodyHide.h"
#include "XRSession.h"

namespace
{
    std::mutex g_M;
    // (vertex shader, index count) -> how many times it was drawn this frame. The vertex shader alone is too
    // coarse (one skinning shader draws every character) and the index count alone collides across meshes.
    std::map<std::pair<void*, UINT>, int> g_Sig;
    std::map<std::pair<void*, UINT>, unsigned> g_SigKinds;   // which draw calls (bit per Entry, +8 deferred) made it
    std::atomic<bool> g_Recording{ false };
    int g_Pending = 0;                 // frames left to wait for draws before giving up on a dump
    std::string g_Tag;
    unsigned long long g_LastPerFrame = 0;
    GpuTrace::Stats g_Stats;

    // Which draw entry the engine uses is not obvious, so hook them all and count each separately.
    enum Entry { eDrawIndexed, eDraw, eDrawIndexedInstanced, eDrawInstanced, eDrawIndexedInstancedIndirect, eDrawInstancedIndirect, eCount };
    const char* kNames[eCount] = { "DrawIndexed", "Draw", "DrawIndexedInstanced", "DrawInstanced", "DrawIndexedInstancedIndirect", "DrawInstancedIndirect" };
    const int   kSlots[eCount] = { 12, 13, 20, 21, 39, 40 };
    // [context kind][entry]: 0 = immediate, 1 = deferred (separate vtables, so separate trampolines)
    std::atomic<unsigned long long> g_Counts[2][eCount];

    // The weapon's draws, identified by index count (stable across runs; shader pointers are not).
    constexpr int kMaxTargets = 24;
    std::atomic<unsigned> g_Target[kMaxTargets];
    std::atomic<bool> g_SkipDraws{ false };

    bool IsTarget(UINT n)
    {
        if (!n) return false;
        for (int i = 0; i < kMaxTargets; i++) if (g_Target[i].load() == n) return true;
        return false;
    }
    bool Skipped(UINT n) { return g_SkipDraws.load() && IsTarget(n); }

    // --- constant buffer capture (stage 2: find the joint palette) ---
    constexpr int kMaxCB = 14;
    std::atomic<bool> g_WantConstants{ false };
    ID3D11Buffer* g_CapturedCB[kMaxCB] = {};      // AddRef'd in the draw hook, read+released at Present
    ID3D11Device* g_Device = nullptr;
    ID3D11DeviceContext* g_Immediate = nullptr;   // deferred contexts cannot Map for reading

    // The VS constant buffers turned out to be tiny (800 + 80 bytes, no matrix runs), so the skinning palette is
    // most likely a buffer bound as a shader resource — the usual id Tech 5 arrangement. Capture those too.
    constexpr int kMaxSRV = 8;
    ID3D11ShaderResourceView* g_CapturedSRV[kMaxSRV] = {};

    void CaptureConstants(ID3D11DeviceContext* ctx)
    {
        ID3D11Buffer* bufs[kMaxCB] = {};
        ctx->VSGetConstantBuffers(0, kMaxCB, bufs);            // AddRefs each non-null
        for (int i = 0; i < kMaxCB; i++)
        {
            if (g_CapturedCB[i]) g_CapturedCB[i]->Release();
            g_CapturedCB[i] = bufs[i];
        }
        ID3D11ShaderResourceView* srvs[kMaxSRV] = {};
        ctx->VSGetShaderResources(0, kMaxSRV, srvs);
        for (int i = 0; i < kMaxSRV; i++)
        {
            if (g_CapturedSRV[i]) g_CapturedSRV[i]->Release();
            g_CapturedSRV[i] = srvs[i];
        }
    }

    // --- stage 3: bind a patched copy of the view constant buffer around the weapon's draws ---
    // The weapon's vertices are already in world space; the only thing placing them on screen is the shared view
    // matrix in slot 1 (see ENGINE.md). Patching it for these draws alone moves the weapon and nothing else.
    // Which VS constant buffer slot to patch, and its size, are both discovered at run time: slot 1 turned out to
    // be *bound* but not *used* by the weapon's shader (zeroing all 800 bytes of it changed nothing on screen).
    constexpr size_t kMaxCBFloats = 512;
    std::atomic<int> g_Slot{ 1 };
    std::atomic<size_t> g_ShadowFloats{ 0 };
    constexpr size_t kViewMatrixA = 24, kViewMatrixB = 76; // the two view matrices in slot 1, translation row at +12
    std::atomic<float> g_ViewOff[3];
    // Bisect which part of the buffer the weapon's shader actually reads: zero a float range and look.
    std::atomic<int> g_ZeroFrom{ -1 }, g_ZeroCount{ 0 };
    std::atomic<bool> g_WantShader{ false };
    void* g_CapturedVS = nullptr;   // vertex shader of a target draw, disassembled at the next Present
    std::mutex g_ShaderM;
    std::map<void*, std::vector<uint8_t>> g_ShaderCode;   // bytecode kept from CreateVertexShader
    size_t g_ShaderBytes = 0;                             // what that costs (under g_ShaderM); level loads add to it
    std::set<void*> g_ShaderDumped;                       // so repeated requests walk to a new shader
    // A STABLE identity for a vertex shader: the checksum in its DXBC header (bytes 4..11). Pointers change on every
    // run and level load; this does not, so a draw can be named in a config line and found again next session.
    std::map<void*, uint64_t> g_VsHash;                   // under g_ShaderM
    std::map<void*, uint64_t> g_PsCsHash;                 // pixel and compute shaders, the same hash (under g_ShaderM)
    // Draws to hide by their vertex shader's hash (gpuskiphash), and the live pointers those hashes resolved to.
    constexpr int kMaxSkipVs = 8;
    std::atomic<uint64_t> g_SkipHash[kMaxSkipVs];
    std::atomic<void*> g_SkipVsPtr[kMaxSkipVs * 4];       // several live shaders can share one hash (reloads)
    std::atomic<bool> g_SkipVsOn{ false };
    std::atomic<unsigned long long> g_SkipVsHits{ 0 };
    uint64_t DxbcHash(const void* code, size_t len)
    {
        if (!code || len < 20 || memcmp(code, "DXBC", 4)) return 0;
        uint64_t h; memcpy(&h, (const uint8_t*)code + 4, 8); return h;
    }
    void AddSkipPtr(void* vs)
    {
        for (auto& p : g_SkipVsPtr) { void* e = nullptr; if (p.load() == vs) return; if (p.compare_exchange_strong(e, vs)) return; }
    }
    // The sword's TRAIL (the white swoosh of the swing). The game draws it along the ANIMATED sword's path, which in VR
    // is not where the sword is (the sword follows the controller), so it streaks across the middle of the view
    // whatever the arm did. Found on a still frame (slashfreeze 300, gpudump, diff against slashfreeze 20): three
    // 26-vertex non-indexed draws, one vertex shader each, that exist only while the trail does; hiding those three
    // shaders removes the trail and nothing else in the frame (A/B/A capture, 2026-09-30). The hashes are the DXBC
    // header checksums, stable across runs. Other trails in the game may share these shaders, so they are hidden
    // only while the player's Slash runs and for kTrailTail after it (the trail fades out after the swing).
    constexpr uint64_t kTrailHashes[] = { 0x364e76313eb849ecull, 0x3fea51d57d8fff32ull, 0x412fe65bec01fc06ull };
    std::atomic<void*> g_TrailVsPtr[16];
    std::atomic<bool> g_NoTrail{ true }, g_TrailWindow{ false };
    std::atomic<unsigned long long> g_TrailHits{ 0 };
    bool IsTrailHash(uint64_t h) { for (uint64_t t : kTrailHashes) if (t == h) return true; return false; }
    void AddTrailPtr(void* vs)
    {
        for (auto& p : g_TrailVsPtr) { void* e = nullptr; if (p.load() == vs) return; if (p.compare_exchange_strong(e, vs)) return; }
    }
    //! Hide this draw? Only asks the context for its shader while a skip list (or the trail window) is active.
    bool SkippedVs(ID3D11DeviceContext* c)
    {
        const bool research = g_SkipVsOn.load(std::memory_order_relaxed);
        const bool trail = g_TrailWindow.load(std::memory_order_relaxed) && g_NoTrail.load(std::memory_order_relaxed);
        if (!research && !trail) return false;
        ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr);
        if (!vs) return false;
        vs->Release();
        if (research) for (auto& p : g_SkipVsPtr) if (p.load(std::memory_order_relaxed) == (void*)vs) { g_SkipVsHits++; return true; }
        if (trail) for (auto& p : g_TrailVsPtr) if (p.load(std::memory_order_relaxed) == (void*)vs) { g_TrailHits++; return true; }
        return false;
    }
    std::mutex g_ShadowM;
    float g_Shadow[kMaxCBFloats] = {};                    // last known contents, refreshed at Present
    bool g_ShadowValid = false;
    // ONE shared buffer, written once per frame on the immediate context at Present and only ever *read* by the
    // recording contexts. Mapping is exclusive, reading is not � which is why the earlier designs crashed: the first
    // mapped one shared buffer from many threads, the second created buffers inside the draw hook mid-frame.
    std::atomic<ID3D11Buffer*> g_PatchedCB{ nullptr };
    std::atomic<bool> g_BindEnabled{ true };
    std::atomic<unsigned long long> g_SwapTried{ 0 }, g_SwapDone{ 0 }, g_SwapNoBuf{ 0 }, g_SwapSize{ 0 };   // bisect: separates the draw-hook binding from the Present-side readback

    std::atomic<bool> g_WantShadow{ false };
    ID3D11Buffer* g_ViewCB = nullptr;      // the game's slot-1 buffer, grabbed on a target draw, read at Present

    bool ViewOffsetOn()
    {
        return g_ViewOff[0].load() != 0.f || g_ViewOff[1].load() != 0.f || g_ViewOff[2].load() != 0.f
            || g_ZeroFrom.load() >= 0;
    }

    //! Bind the already-prepared patched buffer for this draw. Does no allocation and no mapping: everything that
    //! needs the device or a Map happened at Present, on one thread.
    //! Returns the buffer the caller must restore (AddRef'd), or null if nothing was swapped.
    //! Swap in the patched buffer for one draw. Two rules learned the hard way, both of which crashed the driver:
    //!  - only swap when the bound buffer is exactly the same size as ours. These index counts are drawn by four
    //!    different shaders across two passes, and handing a shader a constant buffer smaller than it declares is
    //!    an out-of-bounds read.
    //!  - the caller must ALWAYS restore, including restoring "nothing was bound"; leaving our small buffer behind
    //!    poisons every later draw in that context.
    //! Returns true when a swap happened; `prev` then holds the buffer to restore (may be null but still must be set
    //! back), AddRef'd only when non-null.
    bool BindPatchedView(ID3D11DeviceContext* ctx, ID3D11Buffer** prev)
    {
        *prev = nullptr;
        g_SwapTried++;
        ID3D11Buffer* mine = g_PatchedCB.load();
        if (!mine) { g_SwapNoBuf++; return false; }
        ID3D11Buffer* cur = nullptr;
        const int slot = g_Slot.load();
        ctx->VSGetConstantBuffers(slot, 1, &cur);           // AddRefs
        if (!cur) { g_SwapNoBuf++; return false; }          // nothing bound: not our draw layout, leave it alone
        D3D11_BUFFER_DESC bd{}; cur->GetDesc(&bd);
        if (bd.ByteWidth != (UINT)(g_ShadowFloats.load() * sizeof(float))) { g_SwapSize++; cur->Release(); return false; }
        ctx->VSSetConstantBuffers(slot, 1, &mine);
        g_SwapDone++;
        *prev = cur;
        return true;
    }

    void RestoreView(ID3D11DeviceContext* ctx, bool swapped, ID3D11Buffer* prev)
    {
        if (!swapped) return;
        ctx->VSSetConstantBuffers(g_Slot.load(), 1, &prev);
        if (prev) prev->Release();
    }

    // ============================ stage 5: the weapon in 6DOF ============================
    // What actually places the weapon is a full model-view-projection matrix handed to the shader per draw in
    // PerInstanceCB (cb2), with the vertices arriving in MODEL space. Rewriting that matrix is "put the model where
    // I say", and nothing else on screen is touched.
    //
    // We never read the engine's view or projection to do it. A perspective projection's last row is (0,0,1,0), so
    // the MVP's w row IS the third row of model->view; from that the whole product factors, per draw, into a rigid
    // model->view pose plus every coefficient of the projection. Rebuild with a new pose and the same projection and
    // the rebuild is exact -- with the identity pose it reproduces the original matrix bit for bit, which is the
    // test that proves the factorisation before a single pixel is asked to move. It also means each pass is
    // factored with its OWN projection, so the depth, velocity and material passes cannot disagree about where the
    // weapon is (that would clip the weapon against its own depth buffer).

    // Where the matrices sit inside PerInstanceCB. It is NOT one struct: every material compiles its own, and a
    // single frame binds some thirty different sizes at cb2. So the offsets are read from the shader's own
    // reflection data rather than from a table of sizes -- a table silently misses passes, and a weapon whose depth
    // pass is missed gets clipped against its own stale depth while the visible pass moves.
    //   cb_mvpmatrix{x,y,z,w}          the rows of the MVP (always, or the draw is not transformable)
    //   cb_previousmvpmatrix{x,y,z,w}  motion vectors: must move too or TAA smears the weapon
    //   cb_modelmatrix{x,y,z}          world position and normals for lighting: must move too
    // Measured 1152 in the development save, but the user's scene binds 1168-byte instance buffers, which were
    // rejected outright ("0 of them transformable" in gpufind) and so never moved. The ceiling only bounds how much
    // is worth shadowing, so make it generous rather than exact.
    constexpr UINT kInstMaxBytes = 2048;
    constexpr size_t kInstMaxFloats = kInstMaxBytes / 4;
    constexpr UINT kInstMinBytes = 64;
    struct InstLayout
    {
        bool ok = false;                       // has a complete MVP
        bool model = false;                    // has the model->world rows too
        bool prev = false;
        UINT size = 0;
        int x = -1, y = -1, z = -1, w = -1;
        int px = -1, py = -1, pz = -1, pw = -1;
        int mx = -1, my = -1, mz = -1;
        int dh = -1, dhn = 0;                  // cb_modeldepthhack: offset and size in floats (first-person depth)
        int matSlot = -1, prevMatSlot = -1;    // t-slots of ro_matrices / ro_previousframeskinningmatrices (skinned shaders)
    };
    using Reflect_t = HRESULT (WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    std::mutex g_LayoutM;
    std::map<void*, InstLayout> g_LayoutByVs;
    std::atomic<unsigned long long> g_NoBytecode{ 0 }, g_NoMvp{ 0 };

    InstLayout ReflectLayout(void* vs)
    {
        InstLayout L;
        std::vector<uint8_t> code;
        {
            std::lock_guard lk(g_ShaderM);
            auto it = g_ShaderCode.find(vs);
            if (it == g_ShaderCode.end()) { g_NoBytecode++; return L; }
            code = it->second;
        }
        static Reflect_t reflect = nullptr;
        static bool tried = false;
        if (!tried)
        {
            tried = true;
            HMODULE d = LoadLibraryA("d3dcompiler_47.dll");
            reflect = d ? (Reflect_t)GetProcAddress(d, "D3DReflect") : nullptr;
            if (!reflect) LOGI("gpu6dof: D3DReflect unavailable - the 6DOF weapon needs it");
        }
        if (!reflect) return L;
        ID3D11ShaderReflection* r = nullptr;
        // __uuidof, not IID_ID3D11ShaderReflection: that symbol lives in d3dcompiler.lib and we load the DLL by hand.
        if (FAILED(reflect(code.data(), code.size(), __uuidof(ID3D11ShaderReflection), (void**)&r)) || !r) return L;
        D3D11_SHADER_DESC sd{};
        const char* name = nullptr;
        if (SUCCEEDED(r->GetDesc(&sd)))
            for (UINT i = 0; i < sd.BoundResources; i++)
            {
                D3D11_SHADER_INPUT_BIND_DESC b{};
                if (FAILED(r->GetResourceBindingDesc(i, &b))) continue;
                if (b.Type == D3D_SIT_CBUFFER && b.BindPoint == 2 && !name) name = b.Name;
                // The skinned shaders read their joint palette (3 float4 rows per bone) from a Buffer<float4>.
                if (b.Type == D3D_SIT_TEXTURE && b.Name && !strcmp(b.Name, "ro_matrices")) L.matSlot = (int)b.BindPoint;
                if (b.Type == D3D_SIT_TEXTURE && b.Name && !strcmp(b.Name, "ro_previousframeskinningmatrices")) L.prevMatSlot = (int)b.BindPoint;
            }
        if (ID3D11ShaderReflectionConstantBuffer* cb = name ? r->GetConstantBufferByName(name) : nullptr)
        {
            D3D11_SHADER_BUFFER_DESC bd{};
            if (SUCCEEDED(cb->GetDesc(&bd)))
            {
                L.size = bd.Size;
                auto off = [&](const char* n) -> int
                {
                    ID3D11ShaderReflectionVariable* v = cb->GetVariableByName(n);
                    D3D11_SHADER_VARIABLE_DESC vd{};
                    if (!v || FAILED(v->GetDesc(&vd))) return -1;      // absent names return a dummy whose GetDesc fails
                    return (int)(vd.StartOffset / 4);
                };
                L.x = off("cb_mvpmatrixx"); L.y = off("cb_mvpmatrixy");
                L.z = off("cb_mvpmatrixz"); L.w = off("cb_mvpmatrixw");
                L.px = off("cb_previousmvpmatrixx"); L.py = off("cb_previousmvpmatrixy");
                L.pz = off("cb_previousmvpmatrixz"); L.pw = off("cb_previousmvpmatrixw");
                L.mx = off("cb_modelmatrixx"); L.my = off("cb_modelmatrixy"); L.mz = off("cb_modelmatrixz");
                if (ID3D11ShaderReflectionVariable* v = cb->GetVariableByName("cb_modeldepthhack"))
                {
                    D3D11_SHADER_VARIABLE_DESC vd{};
                    if (SUCCEEDED(v->GetDesc(&vd))) { L.dh = (int)(vd.StartOffset / 4); L.dhn = (int)std::min<UINT>(4, std::max<UINT>(1, vd.Size / 4)); }
                }
            }
        }
        r->Release();
        L.ok = L.x >= 0 && L.y >= 0 && L.z >= 0 && L.w >= 0;
        L.prev = L.px >= 0 && L.py >= 0 && L.pz >= 0 && L.pw >= 0;
        L.model = L.mx >= 0 && L.my >= 0 && L.mz >= 0;
        // Every offset here indexes a kInstMaxFloats buffer on the stack. Reflection reports whatever the shader
        // declares and nothing guarantees that is small, so a cb2 bigger than the shadow would be rebuilt straight
        // past the end of that array. Refuse the layout rather than trust the shader.
        auto fits = [](int off) { return off >= 0 && (size_t)off + 4 <= kInstMaxFloats; };
        if (!(fits(L.x) && fits(L.y) && fits(L.z) && fits(L.w))) L.ok = false;
        if (!(fits(L.px) && fits(L.py) && fits(L.pz) && fits(L.pw))) L.prev = false;
        if (!(fits(L.mx) && fits(L.my) && fits(L.mz))) L.model = false;
        if (!(L.dh >= 0 && (size_t)L.dh + L.dhn <= kInstMaxFloats)) { L.dh = -1; L.dhn = 0; }
        if (!L.ok) g_NoMvp++;
        return L;
    }

    // Both shader caches are keyed by the ID3D11VertexShader POINTER, and a level load destroys every shader the
    // old level used before creating the new level's -- which the allocator is free to place at exactly the same
    // addresses. A recycled address would then be handed the previous shader's constant-buffer layout: offsets from
    // a different cb2 entirely, used to rewrite the draw. This counter is bumped whenever CreateVertexShader lands
    // on an address we already have a layout for, which is precisely when that has happened.
    std::atomic<unsigned long long> g_LayoutGen{ 1 };

    //! Layout for one vertex shader, reflected once and then cached (globally, plus a small per-thread cache since
    //! draws come batched by shader and the draw hooks run on several threads at once). The per-thread cache holds
    //! COPIES, not pointers into the map: an entry that has to be dropped must not leave another thread reading a
    //! node through a dangling pointer.
    const InstLayout* LayoutFor(void* vs)
    {
        constexpr int kFast = 64;                 // hundreds of vertex shaders a frame: 8 slots missed (lock + map) often
        static thread_local void* fastVs[kFast] = {};
        static thread_local InstLayout fastL[kFast];
        static thread_local unsigned long long fastGen = 0;
        const unsigned long long gen = g_LayoutGen.load(std::memory_order_relaxed);
        if (fastGen != gen) { for (int i = 0; i < kFast; i++) fastVs[i] = nullptr; fastGen = gen; }
        const int slot = (int)((((uintptr_t)vs) >> 6) & (kFast - 1));
        if (fastVs[slot] == vs) return &fastL[slot];
        {
            std::lock_guard lk(g_LayoutM);
            auto it = g_LayoutByVs.find(vs);
            if (it == g_LayoutByVs.end()) it = g_LayoutByVs.emplace(vs, ReflectLayout(vs)).first;
            fastL[slot] = it->second;
        }
        fastVs[slot] = vs;
        return &fastL[slot];
    }

    // ---- rigid transforms (row-major rotation, p' = R*p + t) ----
    struct Rigid
    {
        float r[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        float t[3] = {};
    };
    Rigid Mul(const Rigid& a, const Rigid& b)          // a applied after b
    {
        Rigid o;
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 3; j++)
                o.r[i * 3 + j] = a.r[i * 3] * b.r[j] + a.r[i * 3 + 1] * b.r[3 + j] + a.r[i * 3 + 2] * b.r[6 + j];
            o.t[i] = a.r[i * 3] * b.t[0] + a.r[i * 3 + 1] * b.t[1] + a.r[i * 3 + 2] * b.t[2] + a.t[i];
        }
        return o;
    }
    Rigid Inv(const Rigid& a)
    {
        Rigid o;
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) o.r[i * 3 + j] = a.r[j * 3 + i];
        for (int i = 0; i < 3; i++) o.t[i] = -(o.r[i * 3] * a.t[0] + o.r[i * 3 + 1] * a.t[1] + o.r[i * 3 + 2] * a.t[2]);
        return o;
    }

    //! A rotation whose x axis is `a` and whose y axis is `b` made perpendicular to it. Two directions that are
    //! not parallel name a frame; this is how an anatomical frame (fingers, palm normal) becomes a rotation.
    Rigid BasisFromTwo(const float a[3], const float b[3])
    {
        auto dot = [](const float* u, const float* v) { return u[0] * v[0] + u[1] * v[1] + u[2] * v[2]; };
        auto norm = [&](float* v) { const float n = sqrtf(dot(v, v)); if (n > 1e-9f) { v[0] /= n; v[1] /= n; v[2] /= n; } return n; };
        float x[3] = { a[0], a[1], a[2] }; norm(x);
        float y[3] = { b[0], b[1], b[2] };
        const float k = dot(y, x);
        for (int i = 0; i < 3; i++) y[i] -= x[i] * k;
        if (norm(y) < 1e-4f)                     // degenerate: any perpendicular will do
        {
            const float alt[3] = { fabsf(x[0]) < 0.9f ? 1.f : 0.f, fabsf(x[0]) < 0.9f ? 0.f : 1.f, 0.f };
            float c = dot(alt, x);
            for (int i = 0; i < 3; i++) y[i] = alt[i] - x[i] * c;
            norm(y);
        }
        const float z[3] = { x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0] };
        Rigid o;
        for (int i = 0; i < 3; i++) { o.r[i * 3] = x[i]; o.r[i * 3 + 1] = y[i]; o.r[i * 3 + 2] = z[i]; }
        return o;
    }

    // ---- the factorisation ----
    struct Factored
    {
        Rigid pose;              // model -> view, rotation only; the model's uniform scale is kept apart
        float s = 1.f;
        float p[4][4] = {};      // the projection, in the recovered view basis
        float resid = 0.f;       // |dot(right, up)|: zero when the projection really has the assumed shape
        bool ok = false;
    };
    bool Finite4(const float* v)
    { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) && std::isfinite(v[3]); }

    //! Split one MVP into (projection, rigid model->view pose). `r*` are its four rows: clip.i = dot(row_i, modelPos).
    //! Assumes only what every perspective frustum satisfies: last row (0,0,1,0), no x/y skew, no x/y clip offset.
    //! TAA jitter lives in p[0][2]/p[1][2] and is carried through untouched.
    Factored Factor(const float* rx, const float* ry, const float* rz, const float* rw)
    {
        Factored f;
        if (!Finite4(rx) || !Finite4(ry) || !Finite4(rz) || !Finite4(rw)) return f;   // NaN passes every compare: check first
        const float s = sqrtf(rw[0] * rw[0] + rw[1] * rw[1] + rw[2] * rw[2]);
        if (!(s > 1e-8f)) return f;                       // orthographic or degenerate: leave that draw alone
        const float R2[3] = { rw[0] / s, rw[1] / s, rw[2] / s };
        // Each of the x/y rows is (projection scale)*axis + (clip offset)*depthAxis; the depth axis is known, so the
        // other two axes and the two scales fall out of it.
        float R0[3], R1[3];
        auto split = [&](const float* row, float out[3], float& along, float& scale)
        {
            float u[3] = { row[0] / s, row[1] / s, row[2] / s };
            along = u[0] * R2[0] + u[1] * R2[1] + u[2] * R2[2];
            for (int k = 0; k < 3; k++) u[k] -= along * R2[k];
            scale = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            if (!(scale > 1e-8f)) return false;
            for (int k = 0; k < 3; k++) out[k] = u[k] / scale;
            return true;
        };
        if (!split(rx, R0, f.p[0][2], f.p[0][0])) return f;
        if (!split(ry, R1, f.p[1][2], f.p[1][1])) return f;
        f.resid = fabsf(R0[0] * R1[0] + R0[1] * R1[1] + R0[2] * R1[2]);
        f.s = s;
        f.pose.t[2] = rw[3];
        f.pose.t[0] = (rx[3] - f.p[0][2] * f.pose.t[2]) / f.p[0][0];
        f.pose.t[1] = (ry[3] - f.p[1][2] * f.pose.t[2]) / f.p[1][1];
        memcpy(f.pose.r, R0, 12); memcpy(f.pose.r + 3, R1, 12); memcpy(f.pose.r + 6, R2, 12);
        // The depth row needs no assumption at all: (R0,R1,R2) is a complete basis, so project onto all three.
        for (int k = 0; k < 3; k++)
            f.p[2][k] = (rz[0] / s) * f.pose.r[k * 3] + (rz[1] / s) * f.pose.r[k * 3 + 1] + (rz[2] / s) * f.pose.r[k * 3 + 2];
        f.p[2][3] = rz[3] - (f.p[2][0] * f.pose.t[0] + f.p[2][1] * f.pose.t[1] + f.p[2][2] * f.pose.t[2]);
        f.p[3][2] = 1.f;
        f.ok = std::isfinite(f.pose.t[0]) && std::isfinite(f.pose.t[1]) && f.resid < 0.01f;
        return f;
    }

    //! The inverse of Factor: the same projection, a new pose. Pose = identity delta reproduces the input exactly.
    void Rebuild(const Factored& f, const Rigid& pose, float* rx, float* ry, float* rz, float* rw)
    {
        const float* R0 = pose.r; const float* R1 = pose.r + 3; const float* R2 = pose.r + 6;
        for (int k = 0; k < 3; k++)
        {
            rw[k] = f.s * R2[k];
            rx[k] = f.s * (f.p[0][0] * R0[k] + f.p[0][2] * R2[k]);
            ry[k] = f.s * (f.p[1][1] * R1[k] + f.p[1][2] * R2[k]);
            rz[k] = f.s * (f.p[2][0] * R0[k] + f.p[2][1] * R1[k] + f.p[2][2] * R2[k]);
        }
        rw[3] = pose.t[2];
        rx[3] = f.p[0][0] * pose.t[0] + f.p[0][2] * pose.t[2];
        ry[3] = f.p[1][1] * pose.t[1] + f.p[1][2] * pose.t[2];
        rz[3] = f.p[2][0] * pose.t[0] + f.p[2][1] * pose.t[1] + f.p[2][2] * pose.t[2] + f.p[2][3];
    }

    // ---- mode + per-frame transform ----
    std::atomic<int> g_SixDof{ 0 };            // 0 off, 1 follow the controller, 2 identity rebuild (plumbing test)
    // A scripted state (a takedown) is animating the arms and the weapon itself: draw them as the game does. The
    // draws are still identified and COUNTED -- FirstPersonDraws is what tells gameplay from a menu, and a zero there
    // would make KbmInject release every key, the choke's held Ctrl included.
    std::atomic<bool> g_AnimSuspend{ false };
    std::atomic<unsigned long long> g_AnimSuspended{ 0 };
    std::atomic<int> g_SixHand{ -1 };          // -1 = whatever hand the fire ray uses
    std::atomic<int> g_Basis{ 0 };
    std::atomic<bool> g_CalReq{ true };
    std::atomic<int> g_DiagLeft{ 0 };
    std::atomic<int> g_PartsLeft{ 0 };             // vr gunparts: log the next n weapon draws, where they land in the drawn weapon's frame
    std::atomic<int> g_ArmCbLeft{ 0 };         // research: dump the whole cb2 of this many arm draws (gpuarmcb)
    // PER HAND. One shared grip cannot describe both hands: a pistol points ALONG the controller (the barrel is
    // the pointing direction) while a sword stands VERTICAL out of the fist, so their holds differ by about a
    // right angle. Index 0 = left, 1 = right.
    std::atomic<float> g_OffR[2], g_OffU[2], g_OffF[2];
    std::atomic<float> g_GripYaw[2], g_GripRoll[2];
    // Right hand defaults to -90 deg of pitch: that is the sword, and a blade has to stand VERTICAL out of the
    // fist, not point along the controller the way a barrel does. The SIGN was wrong until 2026-09-05: +90 also
    // stands the blade up, but inverted -- tip down and edge up, which is what the user saw on the Quest and what
    // the mock could not judge (at that angle the blade sits at the edge of the frame and the spin about its own
    // axis is not visible). Confirmed on hardware at -90. The left hand (pistol, crossbow, gadgets) wants 0: those
    // DO point along the controller, which is also where the shot goes.
    std::atomic<float> g_GripPitch[2] = { 0.f, -90.f };               // degrees, in the controller's frame
    // The HAND's own grip, used when a POWER (not a weapon) is in that hand. It needs to be separate, and that is
    // not a convenience knob. With a weapon the hand is dragged along by the weapon's delta, which aligns the
    // BARREL with the controller -- right for a pistol or a crossbow, and reported as perfect on hardware. With a
    // power there is no weapon, so the hand is pinned by the socket constant Q instead, and Q was learned from a
    // weapon: the hand comes out oriented as if it were still gripping the crossbow. Reported from the Quest 3:
    // pointing the real hand forward put the palm facing forward "as if pushing against a wall", while the game's
    // own power pose holds the palm facing to the SIDE. One grip cannot serve both -- a barrel points along the
    // controller, a casting palm faces across it.
    std::atomic<float> g_HandGripYaw[2], g_HandGripPitch[2], g_HandGripRoll[2];
    std::atomic<float> g_HandOffR[2], g_HandOffU[2], g_HandOffF[2];

    //! Build a grip from yaw/pitch/roll (degrees) and a (right,up,fwd) offset, in the controller's own frame
    //! (columns fwd/left/up). Shared by the weapon grip and the hand grip so both mean the same thing.
    Rigid MakeGrip(float yawDeg, float pitchDeg, float rollDeg, float offR, float offU, float offF)
    {
        const float d2r = 3.14159265f / 180.f;
        const float y = yawDeg * d2r, p = pitchDeg * d2r, r = rollDeg * d2r;
        const float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p), cr = cosf(r), sr = sinf(r);
        // yaw about up (local z), pitch about left (local y), roll about fwd (local x)
        const float Rz[9] = { cy, -sy, 0,  sy, cy, 0,  0, 0, 1 };
        const float Ry[9] = { cp, 0, sp,   0, 1, 0,   -sp, 0, cp };
        const float Rx[9] = { 1, 0, 0,   0, cr, -sr,   0, sr, cr };
        Rigid a, b, o;
        memcpy(a.r, Rz, 36); memcpy(b.r, Ry, 36);
        o = Mul(a, b);
        memcpy(a.r, Rx, 36); a.t[0] = a.t[1] = a.t[2] = 0.f;
        o = Mul(o, a);
        o.t[0] = offF; o.t[1] = -offR; o.t[2] = offU;   // (right,up,fwd) -> (fwd,-right,up)
        return o;
    }
    //! The grip: where the weapon sits relative to the hand, in the controller's own frame.
    Rigid GripOffset(int hand)
    {
        const int h = hand & 1;
        return MakeGrip(g_GripYaw[h].load(), g_GripPitch[h].load(), g_GripRoll[h].load(),
                        g_OffR[h].load(), g_OffU[h].load(), g_OffF[h].load());
    }
    //! The grip of the HAND ITSELF, for when a power rather than a weapon is in it. See the note on g_HandGripYaw.
    Rigid HandGripOffset(int hand)
    {
        const int h = hand & 1;
        return MakeGrip(g_HandGripYaw[h].load(), g_HandGripPitch[h].load(), g_HandGripRoll[h].load(),
                        g_HandOffR[h].load(), g_HandOffU[h].load(), g_HandOffF[h].load());
    }

    std::mutex g_XfM;
    unsigned long long g_XfSerial = 0;         // the view this cached transform belongs to
    unsigned long long g_XfPresent = 0;        // the Present frame it was taken in
    // Per hand, because both arms are within arm's reach of the eye and would otherwise both follow one controller.
    // Each draw is assigned to the hand whose REST pose it was drawn next to, measured in the game camera's frame so
    // that walking and turning do not change the assignment.
    Rigid g_DeltaView[2], g_DeltaViewPrev[2], g_DeltaWorld[2], g_ViewRigid, g_ViewInv;
    // Alternating-eye stereo with per-eye TAA (StereoTaa): the engine's "previous" matrices are those of the same eye's
    // last frame, so the previous delta of a moved draw must be the one made in THAT view (a view-space delta depends on
    // the view: the other eye's, 6.4 cm aside, put the weapon's motion vectors ~190 px off). [eye][hand]
    Rigid g_DeltaViewEye[2][2]; bool g_DeltaEyeValid[2] = {};
    std::atomic<bool> g_PrevPerEye{ true };   // vr aaarms 0|1
    float g_GameCam[3] = {}, g_GameCamAxis[9] = {}; bool g_GameCamValid = false;
    // The body frame: the HEAD's position (not the eye's: the eyes alternate, and shoulders hung off an eye would
    // jump by an IPD every frame) with the yaw-only axis of the render view. And the same frame built from the game
    // camera: the difference is the HMD's positional offset (lean, crouch), which the torso and the shoulders follow.
    Rigid g_BodyVr, g_BodyDelta; bool g_BodyValid = false;
    BodyHide::Anchor g_Anchor[BodyHide::aCount];      // under g_XfM, refreshed once per view like the camera
    void* g_AnchoredEnt[2] = {};                     // the equipment entity each hand's rest pose belongs to
    bool g_XfValid = false, g_XfPrevValid = false, g_ViewValid = false;
    float g_XfViewMat[16] = {};                // the view matrix of g_XfSerial (vr aaxf: is it the moved draw's own view?)
    std::atomic<unsigned long long> g_XfSame{ 0 }, g_XfOther{ 0 }, g_XfUnknown{ 0 }, g_XfLag[4];
    // ---- Each moved draw placed with ITS OWN view's transform ----
    // EnsureTransform used to build ONE transform, from the newest view and from wherever the first-person entities were
    // at that moment (BodyHide samples them on the game thread once per tick), and every moved draw used it. But draws
    // are recorded on worker threads after the next view is built -- in synchronised stereo the right eye's a whole game
    // frame after the left's -- so a draw could be placed with another view's camera, and the weapon's correction
    // (target * entity^-1, applied to the draw's model matrix) with the entity of ANOTHER TICK: off by one tick of the
    // player's movement (4 m/s at 90 ticks/s = 4.4 cm), sideways when strafing, where it shows -- and the hand on the
    // weapon with it. Now each view carries the entities and the game camera it was built with, and each moved draw is
    // placed with the transform of the view its own PerViewCB names (kept here per view).
    constexpr int kXfRing = 8;
    struct XfState
    {
        unsigned long long serial = 0, present = 0, pair = 0; bool valid = false; int eye = -1;
        Rigid vw, vinv, dv[2], dvPrev[2], dw[2], target[2], hand[2], bodyVr, bodyDelta;
        bool targetValid[2] = {}, bodyValid = false, camValid = false;
        float cam[3] = {}, camAxis[9] = {}, viewMat[16] = {};
        BodyHide::Anchor anch[BodyHide::aCount];
    };
    XfState g_XfRing[kXfRing];                                 // under g_XfM, by serial % kXfRing
    // Lock-free mirrors for the per-draw fast path (EnsureTransform, BeginSixDof): g_XfSerial / g_XfValid, each ring
    // entry's serial * 2 + valid, and the game camera's distance from the newest render view (m, < 0 = unknown; see
    // FarFromPlayer). Written under g_XfM once a build is complete (XfPublish), never in the middle of one, so a draw
    // that sees a serial here sees that transform finished.
    std::atomic<unsigned long long> g_XfSerialPub{ 0 }; std::atomic<bool> g_XfValidPub{ false };
    std::atomic<unsigned long long> g_XfRingPub[kXfRing];
    std::atomic<float> g_CamEyePub{ -1.f };
    //! Under g_XfM: makes the state of g_XfSerial and of ring entry `serial` visible to the lock-free readers.
    void XfPublish(unsigned long long serial)
    {
        const XfState& r = g_XfRing[serial % kXfRing];
        g_XfRingPub[serial % kXfRing].store(r.serial * 2 + (r.valid ? 1 : 0), std::memory_order_release);
        g_XfValidPub.store(g_XfValid, std::memory_order_relaxed);
        g_XfSerialPub.store(g_XfSerial, std::memory_order_release);
    }
    thread_local unsigned long long t_DrawSerial = 0;          // the moved draw's own view (BeginSixDof); 0 = not known
    thread_local unsigned long long t_XfUsed = 0;              // the view whose transform it was placed with
    // Anchor. The reference the weapon is moved FROM is the equipment entity's own world transform, read fresh
    // every frame (BodyHide::GetAnchors) -- not a snapshot of where the model was when calibrated. Three things
    // that cost whole sessions all come from taking a snapshot:
    //  - a snapshot taken during the equip animation freezes a pose the weapon never returns to, so the weapon sits
    //    at a wrong offset from the hand for good (measured: re-anchoring on a weapon change landed mid-animation).
    //  - kept in world space a snapshot goes stale the moment the player walks: a measured delta of -200 m.
    //  - it has to be captured inside the draw hook, which races the identification: in the user's session the
    //    anchor was taken 0.5 s BEFORE the scan that decides what the weapon is had finished.
    // The live transform has none of those: it is correct on the frame it is read, it walks with the player, and it
    // is available before the first draw. Anchoring on the CONTROLLER instead is what does not work -- that only
    // moves the weapon by however much the hand has moved since calibration, so it never arrives at the hand and
    // every rotation pivots 40 cm away, turning a wrist flick into a 35 cm swing.
    Rigid g_GripBase[2];                        // controller->weapon rotation captured when anchoring
    Rigid g_TargetPose[2]; bool g_TargetValid[2] = {};   // where the model is being put, for WeaponPose to mirror
    Rigid g_HandPose[2];                        // the raw controller pose, so the hand can be posed without the weapon grip
    // Collision: the controller pose BEFORE it was pushed out of what it touches, and the push (world translation) the
    // latest view applied -- g_HandPose and g_TargetPose are the drawn (pushed) ones.
    Rigid g_HandRaw[2]; float g_CollOff[2][3] = {};
    // PhysicalPickup: the item carried in a hand is DRAWN there (its draws sit on the aCarry anchor, the entity's real
    // place); the game's entity never moves. rel = the item in the hand's frame, taken from its first draw after the
    // grab (so it starts where it lies under the fingers). Returning: it flies back to its place over kCarryBack s.
    struct CarryState { bool on = false; int hand = 1; bool relValid = false; Rigid rel; Rigid last; bool lastValid = false;
                        bool returning = false; double retStart = 0; Rigid retFrom; };
    std::mutex g_CarryM; CarryState g_Carry;
    constexpr double kCarryBack = 0.35;
    std::atomic<unsigned long long> g_CarryDraws{ 0 };
    // The first-person depth hack. The arms' cb2 carries cb_modeldepthhack, which the vertex shader applies to the
    // clip z so the player's own models win against the world. A carried item is a WORLD draw moved into the hand: it
    // kept the world's value, so the hand (pulled toward the eye in depth) drew over a coin or an apple held in front
    // of it. The carried draw is given the arms' value instead -- the same depth transform as the hand, so the two
    // sort by their real distance (and the item, like the hand, no longer sinks into a wall it is pushed against).
    // Keyed by the projection the arm draw used, so a pass of another view (shadow maps, reflections) keeps its own.
    struct ArmDepthHack { float p00 = 0, p11 = 0; float v[4] = {}; int n = 0; bool valid = false; };
    std::mutex g_DhM; ArmDepthHack g_ArmDh[6]; int g_ArmDhNext = 0;
    std::atomic<unsigned long long> g_CarryDhSet{ 0 }, g_CarryDhNone{ 0 }, g_CarryDhNoVar{ 0 };
    std::atomic<int> g_CarryDhLog{ 0 };          // log lines left for this carry
    bool SameProj(float a, float b) { return fabsf(a - b) <= 0.02f * std::fmax(fabsf(a), fabsf(b)); }
    void NoteArmDepthHack(const float p[4][4], const float* v, int n)
    {
        std::lock_guard lk(g_DhM);
        int slot = -1;
        for (int i = 0; i < 6; i++) if (g_ArmDh[i].valid && SameProj(g_ArmDh[i].p00, p[0][0]) && SameProj(g_ArmDh[i].p11, p[1][1])) { slot = i; break; }
        if (slot < 0) { slot = g_ArmDhNext; g_ArmDhNext = (g_ArmDhNext + 1) % 6; }
        ArmDepthHack& e = g_ArmDh[slot];
        e.p00 = p[0][0]; e.p11 = p[1][1]; e.n = n; memcpy(e.v, v, sizeof(float) * n); e.valid = true;
    }
    bool ArmDepthHackFor(const float p[4][4], float* v, int n)
    {
        std::lock_guard lk(g_DhM);
        for (int i = 0; i < 6; i++)
        {
            const ArmDepthHack& e = g_ArmDh[i];
            if (e.valid && e.n == n && SameProj(e.p00, p[0][0]) && SameProj(e.p11, p[1][1])) { memcpy(v, e.v, sizeof(float) * n); return true; }
        }
        return false;
    }
    double CarryNow() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart / (double)f.QuadPart; }
    // The projection recovered from a factored MVP, kept so the screen-space overlay (the reticle) can put a world
    // point on the screen. Using the engine's OWN projection instead of rebuilding one from the camera's FOV means
    // there is nothing to calibrate: `p` is exactly what the shader multiplies by, expressed in the same view basis
    // as g_ViewRigid, so letterboxing, the aspect the game actually renders at and the TAA jitter carried in
    // p[0][2]/p[1][2] are all already in it.
    // It must come from a draw of the MAIN view. It used to be taken from any draw that factored ("it is per view,
    // so any draw will do") -- but the frame also renders other views through the same path (reflection / cube-map
    // passes: measured 2026-10-03, markers at known world points came out mirrored in x and squashed into a band at
    // mid-height, the reticle pointing somewhere the hand did not), and whichever factored last won. Now only a draw
    // identified as the player's own arm or equipment -- whose recovered origin was matched to its entity in the
    // world, so it is certainly in the main view -- publishes its projection (t_DrawProj, committed by the caller).
    // vr handjit, weapons: distance from a weapon draw's model origin to the equipment entity it is placed by (under 3 cm)
    struct JitWeapon { unsigned long long n = 0; double sum = 0, max = 0; };
    std::mutex g_JitWM; JitWeapon g_JitW[2];
    extern std::atomic<bool> g_JitFrozen;
    std::mutex g_ProjM;
    std::atomic<int> g_ProjAny{ 0 };            // research: 1 = the old behaviour (any factored draw publishes), for comparison
    float g_Proj[4][4] = {}; bool g_ProjValid = false; Rigid g_ProjView; unsigned long long g_ProjFrame = 0;
    // ... and per main view (GpuTrace::ProjectForSerial): the last 4 views' projection and view, under g_ProjM
    struct ProjRec { unsigned long long serial = 0, frame = 0; float P[4][4] = {}; Rigid view; };
    ProjRec g_ProjRing[4];
    thread_local unsigned long long t_ProjWant = 0;
    std::atomic<unsigned long long> g_ProjForHit{ 0 }, g_ProjForMiss{ 0 };
    void ProjRemember(unsigned long long serial, const float P[4][4], const Rigid& view, unsigned long long frame)
    {   // (caller holds g_ProjM)
        if (!serial) return;
        ProjRec* slot = &g_ProjRing[0];
        for (ProjRec& r : g_ProjRing) { if (r.serial == serial) { slot = &r; break; } if (r.serial < slot->serial) slot = &r; }
        slot->serial = serial; slot->frame = frame; memcpy(slot->P, P, sizeof slot->P); slot->view = view;
    }
    // Projection census (GpuTrace::RequestProjCensus): does the first-person body use the world's projection? id Tech
    // engines classically draw the view model with its own fov (renderView_t has an inhibitModelFovScale flag here),
    // which on a monitor makes the arms bigger and closer -- and in VR puts them somewhere the real arms are not.
    struct CensusRow { float fx = 0, fy = 0, z2 = 0, z3 = 0; unsigned long long n = 0, own = 0; float dmin = 1e9f, dmax = 0; };
    std::mutex g_CensusM; std::vector<CensusRow> g_Census; std::string g_CensusText;
    std::atomic<int> g_CensusLeft{ 0 }; int g_CensusFrames = 0;
    void CensusAdd(const float p[4][4], float dist, bool own)
    {
        std::lock_guard lk(g_CensusM);
        for (auto& r : g_Census)
            if (fabsf(r.fx - p[0][0]) < 1e-3f * r.fx && fabsf(r.fy - p[1][1]) < 1e-3f * r.fy && fabsf(r.z2 - p[2][2]) < 1e-3f * (fabsf(r.z2) + 1e-3f))
            {
                if (own) { r.own++; return; }
                r.n++; r.dmin = std::min(r.dmin, dist); r.dmax = std::max(r.dmax, dist); return;
            }
        if (g_Census.size() >= 64) return;
        CensusRow r; r.fx = p[0][0]; r.fy = p[1][1]; r.z2 = p[2][2]; r.z3 = p[2][3];
        if (own) r.own = 1; else { r.n = 1; r.dmin = r.dmax = dist; }
        g_Census.push_back(r);
    }
    std::atomic<int> g_HoldMode{ 1 };           // 1 = weapon forward := controller forward, 0 = capture whatever it was
    bool g_RestModelValid[2] = {};
    // Frames the equipment entity has been the same one. The equip animation swings the weapon through poses it
    // never holds, so the grip rotation is captured only once the entity has settled.
    int g_EntStable[2] = {};
    constexpr int kSettleFrames = 45;
    std::atomic<unsigned long long> g_XfNoView{ 0 }, g_XfNoHand{ 0 }, g_CbMiss{ 0 }, g_CbHit{ 0 },
                                    g_FactorFail{ 0 }, g_Patched{ 0 }, g_NoLayout{ 0 }, g_NoPool{ 0 }, g_NotNear{ 0 }, g_NotWeapon{ 0 }, g_Anchored{ 0 };

    // Identifying the weapon without fixed index counts. A first-person weapon is a draw whose model origin sits
    // within arm's reach of the eye, and the factorisation reports exactly that, for free, for every draw. This
    // replaces diffing draw signatures against BodyHide: index counts are specific to one weapon in one save,
    // proximity is not.
    std::atomic<float> g_Near{ 0.f };          // metres; 0 = identify by index count only

    // --- identifying the weapon by the ENTITY it belongs to ---
    // The scan/hide/scan diff below was not deterministic: two runs of the same save with the same weapon produced
    // different sets (4 -> 13 of 14 draws one time, 6 -> 9 the next), because the two scans are taken frames apart
    // with animation and LOD moving in between, and because BodyHide only hides SOME of the weapon's models.
    // The engine already knows where the weapon is: the equipment entity's world transform (BodyHide::GetAnchors).
    // The factorisation already recovers each draw's model origin in world space. So a draw is the weapon when its
    // model origin sits nearer the weapon entity than to the arms or the player -- decided per draw, per frame,
    // with no scan, no flicker and nothing to go stale on a weapon change.
    std::atomic<int> g_IdMode{ 1 };            // 0 = legacy scan diff, 1 = entity anchors
    // A draw is the weapon's only within this of the weapon entity. Measured: a weapon's draws sit at 0.000 m, a
    // pistol part at 0.175, a mod at 0.128 (RESEARCH). At 0.75 a bottle on a table 0.68 m from the sword's entity was
    // claimed as the sword after a release and drawn wherever the sword's delta put it (its shadow stayed put).
    std::atomic<float> g_AnchorRadius{ 0.35f };
    std::atomic<unsigned long long> g_NoAnchor{ 0 };
    // Live count of the draws the entity test claimed, per hand, for the frame just submitted. This is what tells
    // the tester at a glance whether the weapon is whole: it should be steady, and it should not be zero.
    std::atomic<int> g_WeaponHits[2], g_WeaponHitsLive[2], g_WeaponHitsPeak[2];
    // Which of the near-eye draws are the WEAPON rather than the arms and torso. They share one model matrix (the
    // weapon is a submesh placed by skinning), but they are separate draws, and we rewrite the constants per draw --
    // so telling them apart by draw is enough, no bone palette needed. The set is found by diffing a scan against
    // one taken with BodyHide hiding the weapons: what disappears is the weapon.
    std::mutex g_WeaponM;
    std::set<UINT> g_WeaponIdx;
    std::atomic<bool> g_OnlyWeapon{ true };
    enum IdPhase { idIdle, idScanA, idHide, idScanB };
    std::atomic<int> g_IdPhase{ idIdle };
    std::atomic<int> g_CollectLeft{ 0 };
    int g_IdWait = 0;
    std::set<UINT> g_IdSet, g_IdSetA;
    // Pose of each draw seen while scanning. The weapon is a submesh of the arms' skinned model, so every draw of
    // that model shares one pose; BodyHide only hides PART of the weapon, so the draws that vanish are used as a
    // *marker* for which model is the weapon, and then the whole model's draws are taken.
    std::map<UINT, std::array<float, 3>> g_IdPose;

    // --- hands: the arm models follow their hand's weapon delta ---
    std::atomic<int> g_HandFollow{ 0 };
    unsigned long long g_PresentFrame = 0;
    bool PaletteWanted(ID3D11Resource* r);        // defined with the palette shadow below
    void WantPalette(ID3D11Resource* r);
    constexpr UINT kPalMinBytes = 1024, kPalMaxBytes = 8192;
    constexpr size_t kPalMaxFloats = kPalMaxBytes / 4;
    bool FindPalette(ID3D11Buffer* b, float* out, UINT* bytes);
    std::atomic<int> g_VbDumpWant{ 0 };           // research: index count of the arm draw whose vertex streams to dump
    std::atomic<int> g_VbDumpSide{ 0 };           // ... and which arm (0 left, 1 right)
    void CaptureVertexStreams(ID3D11DeviceContext* ctx, UINT indexCount);

    // ---- what the arm's skin mesh says about its bones ----
    // The joint palette alone cannot say which bone is the hand (a skinning matrix is bone-space * inverse bind
    // pose, its translation is not a joint position). The mesh can: every vertex carries its bind-pose position
    // and up to eight (bone, weight) pairs, so each bone gets a bind-pose centroid and a vertex count. Measured on
    // Emily's left arm (2595 vertices, 72 palette slots): bones 4-63 are hand and fingers, 60-63 the wrist
    // helpers, 64-67 the forearm skin, 0-3 and 68 have no skin vertices at all (the sleeve meshes use them).
    struct ArmBones
    {
        bool valid = false; int bones = 0; size_t verts = 0; int bonesUsed = 0;
        float c[256][3] = {}; float w[256] = {}; int n[256] = {};
        float s[256] = {};            // distance from the fingertip end along the arm, in the bind pose (m)
        int palm = -1, tip = -1;
        float axis[3] = {};           // bind-pose arm axis, unit, from the fingertip towards the shoulder
        float palmN[3] = {};          // bind-pose palm normal, unit, out of the PALM side (see AnalyseArmMesh)
        bool palmNValid = false;
        std::bitset<256> collapse;    // not the hand: collapsed onto the palm for the draw
    };
    std::mutex g_ArmBonesM;
    ArmBones g_ArmBones[2];
    std::atomic<float> g_HandCut{ 0.22f };        // metres from the fingertip: beyond this it is forearm
    std::atomic<int> g_MeshCaps{ 0 }, g_MeshAnalyzed{ 0 }, g_MeshRejected{ 0 };
    // Where the hand IS, independently of what is equipped: the palm bone's frame this frame, in world space, and
    // the constant that turns it into the equipment socket (learned while a weapon is held, when the equipment
    // entity really is in the hand; a power's equipment entity sits on the forearm -- measured 0.36 m from the
    // shoulder root -- so it cannot serve as the hand for the power hand).
    struct HandQ { bool valid = false; Rigid q; int bones = 0, palm = -1; };
    HandQ g_HandQ[2];
    // The HOLD: the palm frame relative to the CONTROLLER, learned while a weapon is in that hand. This is the one
    // hand pose that is known to be right -- with the pistol and the crossbow the user called it perfect on the
    // Quest 3 -- so for a power we replay it instead of rebuilding the pose out of the socket constant.
    //
    // Rebuilding it was the bug. On paper palm = controller * grip * Q^-1 in BOTH branches, so the power hand
    // should have come out identical to the crossbow hand. Measured with the reticle (same controller pose, the ray
    // drawn from the controller in both shots): with the crossbow the hand sits ~90 px from the ray's origin, with
    // Far Reach ~280 px away and the forearm stretched across the view (reach 0.44 of a 0.53 arm). So the two
    // constructions are NOT equivalent in practice, and the way out is not to reconstruct the pose at all.
    struct HandHold { Rigid h; bool valid = false; int bones = 0; int palm = -1; };
    HandHold g_HandHold[2];
    // ---- the hand ON a held weapon: one grip per weapon, not whatever the animation does (vr weapongrip) ----
    // The weapon is pinned to the controller (delta = controller * grip * entity^-1) and the hand used to ride the same
    // delta, so it sat on the weapon wherever the FLAT game's animation had the palm relative to the equipment entity.
    // That is the grip most of the time, but not always -- measured (vr handtrace, 2026-10-05): crouched the game turns
    // the palm 180 deg round the sword (reverse grip), blocking does the same, a mantle swings it 135-178 deg and 52-66 cm
    // off before the game holsters the weapon, the regrip of every crouch/stand opens the fingers up to 15 cm. The old
    // cure covered crouching only, by replaying a "standing hold" -- which was itself re-learned EVERY frame the player
    // was not crouched: during a mantle (measured 88 deg off), a block, a slash, an equip, a takedown or Shadow Walk (the
    // draws are still analysed while they are handed to the game), and saved to DVR_hands.txt. The next crouch then held
    // the hand at that pose for as long as it lasted: the user's "after crouching / climbing / sliding / Shadow Walk the
    // wrist is wrong and the hand does not hold the sword or the pistol where it should".
    // Now each weapon kind (the item component's class) has ONE grip per side and skeleton: the palm in the weapon
    // entity's frame and the fingers round the palm, the game's own idle grip, learned only when the player is in no
    // state that moves the hand on the weapon (GripClean) and the grip has been steady for kGripStableFrames, once per
    // session, kept in DVR_grips.txt so it is right from the first frame of the next load. It is applied ALWAYS while the
    // weapon is in the hand: crouched, sliding, blocking, reloading -- the weapon is on the controller, the hand on it.
    // While the arms are the game's -- Shadow Walk, a takedown -- and for a moment after, the meshes on the arm models
    // are not the player's arms: Shadow Walk draws shadow CLAWS on the arm entities (Emily: 2885 vertices, 62 bones
    // against her skin's 2282), and the arm analysis, which keeps the mesh with the most bones and vertices, took the
    // claws for the skin -- for the rest of the session the hand, the IK and the grips ran on the claw skeleton
    // (measured 2026-10-05: the sword hand 177 deg / 35 cm off its grip after one Shadow Walk; the user's "the arm stays
    // wrong after Shadow Walk", part 24: forearm L2 0.45). Nothing is identified or analysed until this frame.
    std::atomic<unsigned long long> g_ArmForeignUntil{ 0 };
    std::atomic<unsigned long long> g_ArmForeignSkipped{ 0 };
    std::atomic<int> g_ArmForeignOn{ 1 };         //!< vr armforeign: 0 = the old way, the claws get analysed (A/B)
    std::atomic<int> g_GripMode{ 1 };             //!< vr weapongrip: 1 = the learned grip always; 0 = the old way (the animation; crouched, the standing hold)
    std::atomic<bool> g_GripClean[2], g_StatesClean{ false };
    std::atomic<const char*> g_GripDirty[2] = { "", "" };
    constexpr int kGripStableFrames = 45;
    constexpr double kGripSettleSec = 1.5;        // seconds clean before anything is learned (see GripCleanTick)
    constexpr float kGripStableDeg = 1.5f, kGripStableCm = 0.5f;
    std::atomic<unsigned long long> g_GripUsed[2], g_GripLearned[2], g_GripFingUsed[2];
    struct GripLookup { int idx = -1; bool valid = false; Rigid rel; };
    GripLookup GripFind(int side, uint32_t cls, int bones, int palm, int verts);
    void GripObserve(int side, uint32_t cls, int bones, int palm, int verts, const Rigid& relA, bool learnable);
    // handrel on demand: the %180 gate fires whenever it likes, which is no use when a measurement has to be
    // taken at a known moment (a weapon equipped, the time frozen, this pose). `gpuhandrel [n]` asks for n lines.
    std::atomic<int> g_RelLogLeft{ 0 };
    // vr handtrace: where the DRAWN palm sits on the weapon (in the frame of the weapon as drawn = controller * grip)
    // and where the flat game's palm sits on its equipment entity, sampled per side by the arm draw and printed at
    // Present against a reference, with the player's states -- the instrument for "the hand comes off the grip".
    struct HandTraceS { bool valid = false; unsigned long long frame = 0; int whits = 0; bool weapon = false, gadget = false, crouchHold = false, haveA = false, grip = false; Rigid relD, relA; };
    std::mutex g_TraceM;
    HandTraceS g_TraceS[2];
    Rigid g_TraceRefD[2], g_TraceRefA[2], g_TraceRefHold[2]; bool g_TraceRefValid[2] = {};
    std::atomic<int> g_TraceLeft{ 0 }, g_TraceEvery{ 6 };
    std::atomic<bool> g_TraceRefReq{ false }, g_TraceFingReq[2];
    std::atomic<float> g_TraceFingMax[2], g_TraceFingMean[2];
    float RotAngleDeg(const Rigid& a, const Rigid& b)
    {
        float tr = 0; for (int i = 0; i < 9; i++) tr += a.r[i] * b.r[i];
        return acosf(std::clamp((tr - 1.f) * 0.5f, -1.f, 1.f)) * 57.2958f;
    }
    float PosDiffCm(const Rigid& a, const Rigid& b) { const float d[3] = { a.t[0] - b.t[0], a.t[1] - b.t[1], a.t[2] - b.t[2] }; return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * 100.f; }
    // The palm frame the draw path actually produced this frame, for the axes overlay (see GpuTrace::PalmFrame).
    std::mutex g_PalmM;
    Rigid g_PalmW[2]; float g_PalmFing[2][3] = {}, g_PalmNorm[2][3] = {}; unsigned long long g_PalmSeen[2] = {};
    // gpuhandhold: 2 (default) = the ANATOMICAL pose, fingers along the controller and palm towards the midline;
    // 1 = replay the hold learned from the last weapon in that hand (measured to be whichever weapon came last, so
    // the same power could look 90 degrees different from one minute to the next); 0 = rebuild it from the socket
    // constant, which is what it did before 2026-09-15.
    std::atomic<int> g_UseHold{ 2 };
    std::atomic<int> g_CrouchHoldOn{ 1 };         //!< crouched, the weapon hand keeps its standing hold (gpucrouchhold)
    constexpr float kHandHoldMax = 0.30f;         // metres: the palm is never farther than this from the controller
    constexpr float kHandSocketMax = 0.30f;       // metres: a weapon's socket is never farther than this from the palm (the bad one was 0.8)
    std::atomic<unsigned> g_HandQRejected{ 0 };
    std::atomic<bool> g_SaveQPending{ false };
    std::atomic<int> g_HandOwnDelta[2], g_HandOwnDeltaLive[2];
    // A GADGET (grenade, mine, the Heart) is held like a weapon -- in the fist, the game animates the hand round it --
    // but draws only one or two meshes, under the three that tell a weapon from a power's single FX draw. What does
    // tell them apart is WHERE the equipment entity is: a gadget sits in the palm, a power's entity on the forearm.
    std::atomic<float> g_GadgetPalmMax{ 0.15f };  // metres: an equipment entity this close to the (animated) palm is in the hand
    std::atomic<float> g_EntPalm[2] = { -1.f, -1.f };   // measured this frame, for the heartbeat
    // gpugadget 2 (default): the hand takes the ANATOMICAL pose of an empty hand -- fingers along the controller, palm
    // to the midline -- and the gadget rides with it, kept where the game's animation has it in the fist. 1 = the
    // gadget is aimed like a weapon (its axis along the controller) and the hand follows it: the user saw that on the
    // mine and called the hand "fatal" (2026-10-01) -- a grenade has no barrel to line up. 0 = the old rule (hits >= 3).
    std::atomic<int> g_GadgetOn{ 2 };
    // What mode 2 needs at the gadget's own draws, which may come before the arm's in a frame: the hand's anatomical
    // rotation (constant per mesh) and the gadget's pose in the ANIMATED palm's frame (constant while it is held). The
    // gadget's delta is then palmTarget * inPalm * entity^-1 from this frame's controller and entity -- the same pose
    // the hand gets, whichever draw comes first. Under g_ArmBonesM.
    // inPalm is learned only near the CLOSEST the entity has come to the palm since the item was equipped: the grip.
    // Measured: with anything up to the 15 cm "in the hand" bound it learned the sticky grenade mid-toss, 15 cm above an
    // open hand, and kept it floating there.
    struct GadgetHold { bool valid = false; Rigid rel, inPalm; unsigned long long frame = 0; float minPalm = 1e9f; };
    GadgetHold g_GadgetHold[2];
    std::atomic<unsigned long long> g_GadgetDraws[2];

    struct MeshCapture
    {
        bool have = false; int side = -1;
        ID3D11Buffer* pos = nullptr; ID3D11Buffer* skin = nullptr;
        UINT posStride = 0, posOff = 0, skinStride = 0, skinOff = 0;
        UINT posElemOff = 0, wOff0 = 0, wOff1 = 4, iOff0 = 8, iOff1 = 12, wFmt = 28, iFmt = 30, posFmt = 6;
        bool layoutKnown = false; void* key = nullptr;
        void* palKey = nullptr;                   // the joint-palette buffer the draw bound: which skeleton the mesh is rigged to
    };
    std::mutex g_MeshM;
    MeshCapture g_MeshCap;
    std::set<void*> g_MeshDone;                   // position VB pointers already analysed (or rejected)
    void CaptureArmMesh(ID3D11DeviceContext* ctx, int side, void* palKey);
    void AnalyseArmMesh();
    void LoadHandQ(); void SaveHandQ();

    // ---- the full arm (vr body 1) ----
    // A skinning matrix is M_i = W_i * B_i^-1: the bone's current pose times its inverse bind pose. To put bone i
    // somewhere else, write M_i' = W_i' * B_i^-1 = (W_i' * W_i^-1) * M_i = D_i * M_i -- the bind pose is never
    // needed, only the rigid delta D_i of that bone in MODEL space. So instead of collapsing everything that is not
    // the hand, every bone gets a delta: the hand the one that puts the palm on the controller (the same delta the
    // hand-only mode gives the whole draw), the forearm and the upper arm the ones a two-bone IK from a shoulder
    // under the head to the wrist produces, the torso's bits the HMD lean. Adjacent segments meet at the joints by
    // construction, so the skin stays continuous whether or not the joint estimates are exact.
    //
    // The joints: J (bind space) between parent bone p and child bone c satisfies M_p J = M_c J in every frame,
    // i.e. (R_p - R_c) J = t_c - t_p, a least-squares over frames that only gains information when the joint
    // actually bends. The prior is the arm's bind axis: the mesh is bound with the arm straight, so every joint
    // lies close to the line through the bone centroids, at a distance from the fingertip that anatomy bounds.
    // The segment each bone belongs to comes from where its bind-pose vertices sit along that axis (every mesh of
    // the arm model contributes -- the sleeves are what rig the upper-arm bones, the skin has no vertices there),
    // and a rigidity statistic against each segment's representative bone corrects the ambiguous ones over time.
    std::atomic<int> g_ArmMode{ 0 };            // 0 = hand only (collapse the rest), 1 = full arm with IK
    std::atomic<int> g_BodyFollow{ 0 };         // torso draws (player entity) get the HMD body delta: lean, crouch
    enum Seg : uint8_t { sgNone = 0, sgHand, sgFore, sgUpper, sgRoot, sgCount };
    const char* const kSegName[sgCount] = { "-", "hand", "fore", "upper", "root" };
    struct SegTable                              // one per joint palette (= per skeleton instance)
    {
        void* key = nullptr; int side = -1; int bones = 0;
        uint8_t seg[256] = {}; float twist[256] = {}; float s[256] = {}; float w[256] = {};
        float c[256][3] = {};                    // each bone's bind centroid (model space), from the heaviest mesh
        int rep[sgCount] = { -1, -1, -1, -1, -1 };   // heaviest bone of each segment, -1 when the segment has none
    };
    struct MeshBones { void* key = nullptr; int side = -1; size_t verts = 0; float cw[256] = {}; float c[256][3] = {}; };
    struct JointEst
    {
        double A[9] = {}; double b[3] = {}; double uu = 0, uv = 0; int n = 0;   // normal equations, and the 1-D line fit
        float sPrior = 0; float j[3] = {}; float s = 0;                          // solution: bind position and its axis coordinate
    };
    struct ArmRig
    {
        bool valid = false;
        float tip[3] = {}; float axis[3] = {};   // bind axis: from the fingertip towards the shoulder
        JointEst wrist, elbow, shoulder;
        uint8_t manual[256] = {};                 // gpuarmseg overrides for the main palette (0 = automatic, 255 = none)
        uint8_t autoSeg[256] = {};                // reassignments from the rigidity statistic
        int mainKeyBones = 0;                     // the main palette is the one with the most bones
        int palm = -1;                            // the skin's palm bone (its frame is the hand)
        void* mainKey = nullptr;                  // the skin's palette: the IK frame is computed from it alone, since the
                                                  // joints were learned in ITS bind space (a sleeve model may be bound differently)
        void* skinVb = nullptr;                   // the skin mesh's vertex buffer (a load recycles it: it names the skin afterwards)
        unsigned long long missFrame = 0; int missFrames = 0;   // frames this side drew WITHOUT its main palette (loading screens do not count)
        unsigned long long mainSeen = 0;          // last Present frame mainKey was actually bound: see the staleness
                                                  // check in SwapPaletteFull. A level load builds new palettes but
                                                  // RECYCLES the vertex buffers, so CaptureArmMesh's g_MeshDone
                                                  // short-circuit means the mesh is never re-analysed and this
                                                  // pointer names a buffer that no longer exists -- measured on the
                                                  // Quest 3 as the arms freezing for the rest of the session
                                                  // (fulldraws 0, noframe rising) the moment a save was loaded.
    };
    struct ArmFrame                              // this frame's deltas for one side, MODEL space of the arm draws
    {
        unsigned long long frame = 0; bool valid = false;
        Rigid dh, du, dbody, rf0; float ax[3] = {}; float twist = 0; float E[3] = {}, Ep[3] = {};
        float k = 1.f;                            // segment scale (g_ArmLen): upper arm about Sp along Ep-Sp, forearm about Ep along ax
        float S[3] = {}, Sp[3] = {}, W[3] = {}, Wp[3] = {}; float L1 = 0, L2 = 0, reach = 0; bool stretched = false;
        Rigid m2w; float head[3] = {}, ctrl[3] = {};    // for the log: model->world, the head and the controller in world space
        uint64_t srcHash = 0; unsigned long long srcSerial = 0;   // the palette content and the view it was computed from (vr handjit)
    };
    struct RelStat { double n = 0; double mean[3] = {}; double m2 = 0; };
    std::mutex g_RigM;
    ArmRig g_Rig[2];
    std::vector<MeshBones> g_MeshList;
    // One table per SIDE, not per palette: the arm model's skin and its sleeves bind different palette buffers, but
    // they are the same skeleton (measured: bone 64-67 centroids agree between the skin and the sleeve, the sleeve is
    // what carries bones 1-3 and 68), and a table per palette left each without the segments the other had.
    SegTable g_Seg[2];
    ArmFrame g_ArmFrame[2];                      // the main palette's latest frame, for the logs, HandFx and BodyView
    // vr armsolve: 1 (default) = every draw posed from its own palette (SolveArm per draw, PrevFind for motion vectors);
    // 0 = the old way, for comparison: one frame per Present from the first main draw, handed to every draw of the arm,
    // and the previous Present's to the motion-vector palette. Under g_RigM.
    std::atomic<int> g_ArmSolve{ 1 };
    ArmFrame g_LegacyF[2], g_LegacyPrev[2];
    RelStat g_Rel[2][3][256];                    // [side][vs 0 forearm rep / 1 upper rep / 2 model frame][bone]
    // fwd, outward, up: the shoulder joint relative to the head (m). Anatomy (BodyView, 2026-10-04): ~9 cm behind the
    // eyes, 18.5 to the side, 23 below. It was (-0.06, 0.17, -0.20): measured against a real arm in six poses, the drawn
    // shoulder sat 3 cm higher, 3 cm forward and 1.5 cm inward -- part of "the arms are closer to the face".
    std::atomic<float> g_ShoulderOff[3] = { -0.09f, 0.185f, -0.23f };
    // The drawn arm's length. The game's skeleton measures 0.49-0.52 m shoulder to wrist, a real adult arm ~0.56; when the
    // controller was further than the game arm reaches, the IK dragged the SHOULDER towards the hand -- 7-10 cm forward
    // and up, in front of the face (measured, arm forward and arm up). Now the segments are lengthened towards g_ArmLen
    // first (a scale along each bone, at most g_ArmScaleMax), and only beyond that does the shoulder follow.
    std::atomic<float> g_ArmLen{ 0.56f }, g_ArmScaleMax{ 1.15f };
    std::atomic<float> g_Pole[3] = { -0.2f, 0.4f, -0.8f };             // fwd, outward, up: which way the elbow points
    std::atomic<float> g_ForearmPrior{ 0.26f }, g_UpperPrior{ 0.30f };  // wrist->elbow, elbow->shoulder (m), until measured
    std::atomic<int> g_FullDrawsLive[2], g_FullDraws[2], g_BodyDrawsLive, g_BodyDraws, g_FullNoTableLive, g_FullNoTable, g_FullNoFrameLive, g_FullNoFrame;
    std::atomic<unsigned> g_MainReadopted{ 0 };   //!< times a side's main joint palette was replaced after a load
    std::atomic<unsigned> g_HoldUsed[2];          //!< power-hand draws posed from the learned hold rather than from Q
    std::atomic<unsigned> g_CrouchHoldUsed[2];    //!< weapon-hand draws posed from the standing hold while crouched
    void RebuildSegTables(int side);             // g_RigM held
    std::mutex g_ArmM;
    std::unordered_map<void*, int> g_ArmSrvSide;     // joint-palette SRV -> 0 left / 1 right
    std::set<void*> g_ArmSrvA, g_ArmSrvB;            // seen on arm draws: normally / with the left arm hidden
    std::set<void*> g_ArmSrvUnknown;                 // seen but not yet classified
    enum ArmIdPhase { aiIdle, aiCollectA, aiHide, aiCollectB };
    std::atomic<int> g_ArmIdPhase{ aiIdle };
    int g_ArmIdWait = 0; unsigned long long g_ArmIdLastFrame = 0, g_ArmIdRuns = 0;
    std::atomic<int> g_ArmHits[2], g_ArmHitsLive[2], g_ArmUnknown, g_ArmUnknownLive;
    //! Side of an arm draw from the joint palette it binds, or -1 while unknown. Unknown palettes are collected for
    //! the identification pass; while it runs, every arm draw's palettes are recorded into the current set.
    int ArmSideFromSrv(void* const srv[3])
    {
        std::lock_guard lk(g_ArmM);
        int side = -1;
        for (int i = 0; i < 3; i++)
        {
            if (!srv[i]) continue;
            auto it = g_ArmSrvSide.find(srv[i]);
            if (it != g_ArmSrvSide.end()) side = it->second; else g_ArmSrvUnknown.insert(srv[i]);
            const int ph = g_ArmIdPhase.load();
            if (ph == aiCollectA) g_ArmSrvA.insert(srv[i]);
            else if (ph == aiCollectB) g_ArmSrvB.insert(srv[i]);
        }
        return side;
    }
    //! Present-side state machine: collect the palettes the arm draws bind, hide the left arm (BodyHide, game
    //! thread, takes a few frames), collect again; what vanished is the left arm's, what stayed is the right's.
    //! Costs one short flicker of the left arm each time a new palette shows up (level load, character change).
    void TickArmIdentify()
    {
        g_PresentFrame++;
        const int ph = g_ArmIdPhase.load();
        if (ph == aiIdle)
        {
            if (!g_HandFollow.load()) return;
            bool want = false;
            { std::lock_guard lk(g_ArmM); want = !g_ArmSrvUnknown.empty(); }
            if (!want || g_PresentFrame - g_ArmIdLastFrame < 120 || g_PresentFrame < g_ArmForeignUntil.load()) return;
            { std::lock_guard lk(g_ArmM); g_ArmSrvA.clear(); g_ArmSrvB.clear(); }
            g_ArmIdWait = 10; g_ArmIdPhase = aiCollectA;
            return;
        }
        if (--g_ArmIdWait > 0) return;
        switch (ph)
        {
        case aiCollectA:
            BodyHide::SetHideArmSide(0, true);
            g_ArmIdWait = 15; g_ArmIdPhase = aiHide;
            break;
        case aiHide:
            g_ArmIdWait = 10; g_ArmIdPhase = aiCollectB;
            break;
        case aiCollectB:
        {
            BodyHide::SetHideArmSide(0, false);
            std::lock_guard lk(g_ArmM);
            int nl = 0, nr = 0;
            for (void* p : g_ArmSrvA)
            {
                const int side = g_ArmSrvB.count(p) ? 1 : 0;
                g_ArmSrvSide[p] = side; g_ArmSrvUnknown.erase(p);
                side ? nr++ : nl++;
            }
            for (void* p : g_ArmSrvB) if (!g_ArmSrvA.count(p)) { g_ArmSrvSide[p] = 1; g_ArmSrvUnknown.erase(p); nr++; }
            g_ArmIdLastFrame = g_PresentFrame; g_ArmIdRuns++;
            LOGI("gpuhands: identification #{}: {} palettes seen, {} vanished with the left arm hidden (left), {} stayed (right), {} still unknown",
                 g_ArmIdRuns, g_ArmSrvA.size(), nl, nr, g_ArmSrvUnknown.size());
            if (!nl) LOGI("gpuhands: WARNING - nothing vanished: was the left arm visible (vr hidearms 0) and in view?");
            g_ArmSrvUnknown.clear();          // whatever is left will be re-collected as it is drawn
            g_ArmIdPhase = aiIdle;
            break;
        }
        default: g_ArmIdPhase = aiIdle; break;
        }
    }

    std::atomic<int> g_FindLeft{ 0 };
    std::atomic<float> g_FindDist{ 2.0f };
    std::mutex g_FindM;
    struct FindHit
    {
        int hits = 0; float dist = 0; float sx = 0, sy = 0; float t[3] = {}; UINT bytes = 0;
        float w[3] = {};                       // model origin in world space
        int anchor = -1; float ad[BodyHide::aCount] = {};   // distance to each first-person entity
        void* srv[3] = {};                     // VS shader resources t0..t2 (a skinned draw binds its joint palette there)
    };
    std::map<UINT, FindHit> g_Find;            // by index count

    // What the weapon test claims, for readers on other threads (GpuTrace::WeaponClaim): the anchors of this frame
    // behind a sequence counter, so a reader never takes a lock on the hot paths that ask (every render model's bounds).
    std::atomic<unsigned> g_ClaimSeq{ 0 };
    BodyHide::Anchor g_ClaimAnch[BodyHide::aCount];
    std::atomic<bool> g_ClaimValid{ false };
    bool SixDofOn() { return g_SixDof.load() != 0; }
    bool Scanning() { return g_FindLeft.load() > 0 || g_CollectLeft.load() > 0; }
    bool ShadowWanted() { return g_SixDof.load() != 0 || g_DiagLeft.load() > 0 || Scanning() || StereoTaa::FixingPrev(); }

    //! Refresh the frame's transform, keyed on the view serial so the hand pose and the view it is expressed in
    //! always come from the same frame. Whichever recording thread reaches the weapon first does the work.
    //! XfState: the transform of the moved draw's OWN view is built too, from that view's own matrix,
    //! poses and recorded entities, when it is not the newest one.
    std::atomic<bool> g_HandsFromView{ true };
    std::atomic<unsigned long long> g_XfFromView{ 0 }, g_XfLatest{ 0 };
    //! One view's transform into the ring (and, for the newest view, into the globals the other readers use, with what is
    //! done once per new view: the weapon-change bookkeeping, anchoring a hand). Returns whether it is valid.
    bool BuildXf(unsigned long long serial, bool latest)
    {
        float vm[16], vorg[3], vaxis[9], haxis[9];
        // (vaxis: the axis the view was rendered with -- the basis the MVPs live in; haxis: the head's, without a stereo
        // eye's turn or cant -- the yaw the controllers are carried into the world with, the same for both eyes)
        if (!ViewMatrixHook::ViewBySerial(serial, vm, vorg, vaxis, haxis)) { g_XfNoView++; return false; }
        XfState st; st.serial = serial; st.present = g_PresentFrame; memcpy(st.viewMat, vm, 64);

        // The view basis the factorisation recovers: x = clip x = right, y = clip y = up, z = depth = forward.
        // Built from the pose the renderer ACTUALLY used (HMD composition included), which is why it comes from
        // ViewMatrixHook and not from the game camera.
        const int mask = g_Basis.load();
        const float sr = (mask & 1) ? -1.f : 1.f, su = (mask & 2) ? -1.f : 1.f, sf = (mask & 4) ? -1.f : 1.f;
        Rigid vw;
        for (int k = 0; k < 3; k++)
        {
            vw.r[k]     = -vaxis[3 + k] * sr;      // right = -left  (idMat3 rows are fwd/left/up)
            vw.r[3 + k] =  vaxis[6 + k] * su;      // up
            vw.r[6 + k] =  vaxis[k]     * sf;      // forward
        }
        for (int i = 0; i < 3; i++)
            vw.t[i] = -(vw.r[i * 3] * vorg[0] + vw.r[i * 3 + 1] * vorg[1] + vw.r[i * 3 + 2] * vorg[2]);

        st.vw = vw; st.vinv = Inv(vw);

        // The controller, anchored to that same render view: the MVP lives in the render view's space, which differs
        // from the game camera by the HMD positional offset -- anchoring to the camera makes the weapon drift on lean.
        // The eye this VIEW was built for (its record), not the eye current now: the pipeline is deeper than one frame,
        // and with the other eye's pose the hands were anchored an IPD aside on those frames.
        const ViewMatrixHook::XrPose viewRec = ViewMatrixHook::RenderPoseXr(serial);
        const int eye = viewRec.valid ? viewRec.eye : VRRuntime::CurrentEye();
        VRRuntime::HeadPose hp = eye >= 0 ? VRRuntime::GetEyePose(eye) : VRRuntime::GetHeadPose();
        // ... and the poses that view was BUILT from (head, eyes, controllers, located for when it is shown): both eyes of
        // a synchronised pair draw the hands from one instant; the latest located ones differed eye to eye (vr handsfromview)
        VRRuntime::PoseSet vset;
        const bool fromView = g_HandsFromView.load() && ViewMatrixHook::ViewPoses(serial, vset) && vset.inputValid;
        if (fromView) { hp = eye >= 0 ? vset.eye[eye] : vset.head; g_XfFromView++; } else g_XfLatest++;
        const bool prevSameEye = eye >= 0 && g_PrevPerEye.load() && StereoTaa::PrevIsSameEye(vm);
        st.eye = eye; st.pair = viewRec.valid ? viewRec.pair : 0;
        Rigid hw[2];                                // controller poses in world space; columns = fwd/left/up
        Rigid hwRaw[2]; float collOff[2][3] = {};   // ... before Collision pushed them out of the world, and the push
        bool haveHand[2] = {};
        for (int h = 0; h < 2; h++)
        {
            float horg[3], hxaxis[9];
            if (!hp.valid || !HandAimHook::ControllerPoseAt(h, vorg, haxis, hp.pos, horg, hxaxis, fromView ? &vset.input : nullptr,
                                                            fromView ? vset.head.quat : nullptr)) continue;
            haveHand[h] = true;
            for (int i = 0; i < 3; i++)
            { hw[h].r[i * 3] = hxaxis[i]; hw[h].r[i * 3 + 1] = hxaxis[3 + i]; hw[h].r[i * 3 + 2] = hxaxis[6 + i]; hw[h].t[i] = horg[i]; }
            // Solid hands and weapons: the drawn hand (and with it the weapon) stops at what it meets (Collision)
            hwRaw[h] = hw[h];
            Collision::Apply(h, hw[h].r, hw[h].t, collOff[h]);
        }
        if (!haveHand[0] && !haveHand[1]) g_XfNoHand++;


        // The game camera and the first-person entities this view was BUILT with: the draws it is placed with were made
        // from that game tick. Else as they are now (the game thread's last sample: the tick may differ).
        if (!ViewMatrixHook::ViewEntities(serial, st.anch, st.cam, st.camAxis, &st.camValid))
        {
            st.camValid = HandAimHook::RealCamera(st.cam, st.camAxis);
            BodyHide::GetAnchors(st.anch);
        }
        {   // the body frames (see the declaration): head position + yaw-only axis, from the render view and from the game camera
            auto yawFrame = [](const float org[3], const float axis[9], Rigid& o) {
                const float yaw = atan2f(axis[1], axis[0]), c = cosf(yaw), s = sinf(yaw);
                // columns = fwd/left/up of the frame, so R * local = world
                o.r[0] = c; o.r[1] = -s; o.r[2] = 0; o.r[3] = s; o.r[4] = c; o.r[5] = 0; o.r[6] = 0; o.r[7] = 0; o.r[8] = 1;
                memcpy(o.t, org, 12); };
            float headOrg[3]; memcpy(headOrg, vorg, 12);
            VRRuntime::HeadPose head = fromView ? vset.head : VRRuntime::GetHeadPose();
            if (hp.valid && head.valid)
            {
                const float d[3] = { head.pos[0] - hp.pos[0], head.pos[1] - hp.pos[1], head.pos[2] - hp.pos[2] };
                float dw[3]; HandAimHook::XrOffsetToWorld(d, haxis, dw, fromView ? vset.head.quat : nullptr);
                for (int i = 0; i < 3; i++) headOrg[i] += dw[i];
            }
            yawFrame(headOrg, haxis, st.bodyVr);
            st.bodyValid = st.camValid;
            if (st.bodyValid) { Rigid bg; yawFrame(st.cam, st.camAxis, bg); st.bodyDelta = Mul(st.bodyVr, Inv(bg)); }
        }

        std::lock_guard lk(g_XfM);
        struct Publish { unsigned long long s; ~Publish() { XfPublish(s); } } publish{ serial };   // on every way out, still under the lock
        if (latest)
        {
            if (serial <= g_XfSerial) return g_XfValid;              // another thread got here first
            if (g_XfValid) { for (int h = 0; h < 2; h++) g_DeltaViewPrev[h] = g_DeltaView[h]; g_XfPrevValid = true; }
            g_XfSerial = serial; g_XfPresent = g_PresentFrame; g_ViewRigid = vw; g_ViewInv = st.vinv; g_ViewValid = true; g_XfValid = false;
            memcpy(g_XfViewMat, vm, 64);
            PipeTrace::Event(PipeTrace::Xform, serial);
            // The engine places the weapon relative to the GAME camera, while the MVP is built from the rendered
            // view; with 6DOF head tracking the two differ by the HMD offset. Measuring "near the eye" against the
            // rendered view would drop the weapon out of range as soon as the player stands up or leans.
            g_GameCamValid = st.camValid;
            if (g_GameCamValid) { memcpy(g_GameCam, st.cam, 12); memcpy(g_GameCamAxis, st.camAxis, 36); }
            {   // how far the rendered eye is from the game camera (the HMD's offset): FarFromPlayer's bound
                const float d[3] = { st.cam[0] - vorg[0], st.cam[1] - vorg[1], st.cam[2] - vorg[2] };
                g_CamEyePub.store(st.camValid ? sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) : -1.f, std::memory_order_relaxed);
            }
            memcpy(g_Anchor, st.anch, sizeof g_Anchor);          // where the engine says the weapon and the arms are, this view
            g_ClaimSeq.fetch_add(1); memcpy(g_ClaimAnch, g_Anchor, sizeof g_ClaimAnch); g_ClaimSeq.fetch_add(1); g_ClaimValid = g_SixDof.load() == 1;
            if (g_PartsLeft.load() > 0 && g_GameCamValid && (serial % 4) == 0)
                for (int k = 0; k < 2; k++)
                {   // vr gunparts: where the flat game has each weapon, in the game camera's frame (fwd / left / up)
                    const BodyHide::Anchor& w = g_Anchor[k == 0 ? BodyHide::aWeaponL : BodyHide::aWeaponR];
                    if (!w.valid) continue;
                    const float d[3] = { w.org[0] - g_GameCam[0], w.org[1] - g_GameCam[1], w.org[2] - g_GameCam[2] };
                    float r[3]; for (int i = 0; i < 3; i++) r[i] = d[0] * g_GameCamAxis[i * 3] + d[1] * g_GameCamAxis[i * 3 + 1] + d[2] * g_GameCamAxis[i * 3 + 2];
                    LOGI("gunparts: ANCHOR weapon {} at fwd {:.3f} left {:.3f} up {:.3f} (axis fwd {:.2f},{:.2f},{:.2f})", k, r[0], r[1], r[2], w.axis[0], w.axis[1], w.axis[2]);
                }
            g_BodyVr = st.bodyVr; g_BodyValid = st.bodyValid; if (st.bodyValid) g_BodyDelta = st.bodyDelta;
            // A weapon swap (or a level load) builds a new equipment entity, and the grip rotation we captured belongs
            // to the weapon that is gone -- keeping it holds the new one at a wrong angle for the rest of the session.
            // Re-anchor instead, which needs no command from the player.
            for (int h = 0; h < 2; h++)
            {
                void* ent = g_Anchor[h == 0 ? BodyHide::aWeaponL : BodyHide::aWeaponR].ent;
                if (ent == g_AnchoredEnt[h]) { g_EntStable[h]++; continue; }
                if (g_RestModelValid[h]) LOGI("gpu6dof: hand {} equipment changed ({} -> {}), re-anchoring", h, g_AnchoredEnt[h], ent);
                g_AnchoredEnt[h] = ent;
                g_EntStable[h] = 0;
                g_RestModelValid[h] = false;
            }
            if (g_CalReq.exchange(false)) { g_RestModelValid[0] = g_RestModelValid[1] = false; }

        }
        else
        {
            const XfState& r = g_XfRing[serial % kXfRing];
            if (r.serial == serial) return r.valid;                  // another thread got here first
        }
        for (int h = 0; h < 2; h++) st.hand[h] = g_HandPose[h];

        Rigid deltaWorld[2];                                     // identity unless a controller is driving it
        if (latest) g_TargetValid[0] = g_TargetValid[1] = false;
        if (g_SixDof.load() == 1)
        {
            if ((!haveHand[0] && !haveHand[1]) || !st.camValid) { g_XfRing[serial % kXfRing] = st; return false; }
            for (int h = 0; h < 2; h++)
            {
                const Rigid grip = GripOffset(h);
                const BodyHide::Anchor& wa = st.anch[h == 0 ? BodyHide::aWeaponL : BodyHide::aWeaponR];
                if (!haveHand[h]) continue;
                if (!wa.valid)
                {   // An empty hand: no equipment to move (the delta stays the identity), but the hand itself still has
                    // a controller to follow -- the arm draws pose it from the palm (see the empty-hand branch).
                    st.target[h] = Mul(hw[h], grip); st.targetValid[h] = true; st.hand[h] = hw[h];
                    continue;
                }
                Rigid ent;                                       // equipment entity -> world, as this view has it
                for (int i = 0; i < 3; i++)
                {
                    ent.r[i * 3] = wa.axis[i]; ent.r[i * 3 + 1] = wa.axis[3 + i]; ent.r[i * 3 + 2] = wa.axis[6 + i];
                    ent.t[i] = wa.org[i];
                }
                if (!g_RestModelValid[h])
                {
                    if (!latest) continue;               // a hand is anchored on the newest view only
                    if (g_HoldMode.load() == 1)
                    {   // Deterministic hold: gripBase is the identity, so the weapon's frame BECOMES the
                        // controller's frame and the barrel points where the controller points. The same hold every
                        // session, and it agrees with the fire ray, which already leaves along the controller's
                        // forward. Nothing to settle for: the rotation does not depend on the moment, and the
                        // reference is the entity's live transform, so anchoring mid-equip is harmless.
                        g_GripBase[h] = Rigid();
                    }
                    else
                    {   // Legacy: capture the controller->weapon rotation as it is right now, which makes switching
                        // the mode on a pure translation. The cost is that the hold is whatever the wrist happened
                        // to be doing at that instant -- a different, often visibly rotated hold on every load.
                        if (g_EntStable[h] < kSettleFrames) continue;    // let the equip animation finish first
                        Rigid base = Mul(Inv(hw[h]), ent);
                        base.t[0] = base.t[1] = base.t[2] = 0.f;        // rotation only
                        g_GripBase[h] = base;
                    }
                    g_RestModelValid[h] = true;
                    g_Anchored++;
                    float dx = ent.t[0] - hw[h].t[0], dy = ent.t[1] - hw[h].t[1], dz = ent.t[2] - hw[h].t[2];
                    LOGI("gpu6dof: anchored hand {} ({} hold) - weapon entity at ({:.3f},{:.3f},{:.3f}), controller at "
                         "({:.3f},{:.3f},{:.3f}), jump {:.3f} m", h, g_HoldMode.load() == 1 ? "controller-aligned" : "captured",
                         ent.t[0], ent.t[1], ent.t[2],
                         hw[h].t[0], hw[h].t[1], hw[h].t[2], sqrtf(dx * dx + dy * dy + dz * dz));
                }
                // Put the weapon IN the hand: delta = hand * gripBase * grip * entity^-1. Every draw of this hand's
                // equipment gets the same rigid delta, so the body of the weapon and the mods that hang off it at
                // their own offsets stay together -- what came apart in the previous design.
                const Rigid target = Mul(Mul(hw[h], g_GripBase[h]), grip);
                deltaWorld[h] = Mul(target, Inv(ent));
                st.target[h] = target; st.targetValid[h] = true; st.hand[h] = hw[h];
            }
        }
        for (int h = 0; h < 2; h++) { st.dw[h] = deltaWorld[h]; st.dv[h] = Mul(vw, Mul(deltaWorld[h], st.vinv)); }
        if (latest)
        {
            for (int h = 0; h < 2; h++)
            {
                g_TargetPose[h] = st.target[h]; g_TargetValid[h] = st.targetValid[h]; if (st.targetValid[h]) g_HandPose[h] = st.hand[h];
                if (st.targetValid[h] && haveHand[h]) { g_HandRaw[h] = hwRaw[h]; memcpy(g_CollOff[h], collOff[h], 12); }
                g_DeltaWorld[h] = st.dw[h];
                g_DeltaView[h] = st.dv[h];
                if (!g_XfPrevValid) g_DeltaViewPrev[h] = g_DeltaView[h];
            }
            if (eye >= 0)
            {   // stereo: this eye's last delta is the previous one when the engine's previous matrices are this eye's too
                if (prevSameEye && g_DeltaEyeValid[eye]) for (int h = 0; h < 2; h++) g_DeltaViewPrev[h] = g_DeltaViewEye[eye][h];
                for (int h = 0; h < 2; h++) g_DeltaViewEye[eye][h] = g_DeltaView[h];
                g_DeltaEyeValid[eye] = true;
            }
            else g_DeltaEyeValid[0] = g_DeltaEyeValid[1] = false;
            g_XfPrevValid = true;
            g_XfValid = true;
            for (int h = 0; h < 2; h++) st.dvPrev[h] = g_DeltaViewPrev[h];
        }
        else
        {   // the previous delta (motion vectors): the view before this one in the ring -- of the same eye when the
            // engine's previous matrices are that eye's (see g_DeltaViewEye)
            const XfState* p = nullptr;
            for (const XfState& r : g_XfRing)
                if (r.valid && r.serial < serial && (!prevSameEye || r.eye == eye) && (!p || r.serial > p->serial)) p = &r;
            for (int h = 0; h < 2; h++) st.dvPrev[h] = p ? p->dv[h] : st.dv[h];
        }
        st.valid = true;
        g_XfRing[serial % kXfRing] = st;
        return true;
    }
    //! The transform for a moved draw: the newest view's (built if new), and the draw's own view's
    //! (`want`, from its PerViewCB; 0 = not known) when that is an older one. True when the draw can be moved.
    bool EnsureTransform(unsigned long long want = 0)
    {
        // Asked for every draw: once a view's transform is built, answered from the published mirrors without a lock.
        const unsigned long long latest = ViewMatrixHook::LatestSerial();
        if (!latest) { g_XfNoView++; return false; }
        bool ok;
        if (latest <= g_XfSerialPub.load(std::memory_order_acquire)) ok = g_XfValidPub.load(std::memory_order_relaxed);
        else ok = BuildXf(latest, true);
        if (!want || want >= latest) return ok;
        const unsigned long long tag = g_XfRingPub[want % kXfRing].load(std::memory_order_acquire);
        bool own = false;
        if ((tag >> 1) == want) own = (tag & 1) != 0;
        else if (latest - want < kXfRing) own = BuildXf(want, false);
        return own || ok;
    }

    //! Research: everything the factorisation believes about one draw, plus -- for the 384-byte layout, which is
    //! handed the model->world matrix as well -- the pose predicted from the engine's own view. If the predicted and
    //! the recovered pose agree, the factorisation, the view basis and the axis signs are all confirmed at once.
    void LogDraw(const InstLayout* L, const float* in, const Factored& f)
    {
        LOGI("gpu6dof: cb{} prev={} model={} s={:.4f} resid={:.5f} fx={:.4f} fy={:.4f} cx={:.4f} cy={:.4f} z=({:.5f},{:.5f},{:.5f},{:.3f})",
             L->size, L->prev, L->model, f.s, f.resid, f.p[0][0], f.p[1][1], f.p[0][2], f.p[1][2],
             f.p[2][0], f.p[2][1], f.p[2][2], f.p[2][3]);
        LOGI("gpu6dof:   view pose t=(r {:.3f}, u {:.3f}, f {:.3f}) rows=({:.3f},{:.3f},{:.3f})({:.3f},{:.3f},{:.3f})({:.3f},{:.3f},{:.3f})",
             f.pose.t[0], f.pose.t[1], f.pose.t[2],
             f.pose.r[0], f.pose.r[1], f.pose.r[2], f.pose.r[3], f.pose.r[4], f.pose.r[5],
             f.pose.r[6], f.pose.r[7], f.pose.r[8]);
        // Where the model origin lands on screen: clip = (p00*tx + p02*tz, p11*ty + p12*tz, -, tz). Compare with the
        // PNG and the whole chain -- signs included -- is confirmed against something visible.
        if (f.pose.t[2] > 1e-4f)
            LOGI("gpu6dof:   model origin at screen ({:.3f}, {:.3f}) of 0..1",
                 0.5f + 0.5f * (f.p[0][0] * f.pose.t[0] + f.p[0][2] * f.pose.t[2]) / f.pose.t[2],
                 0.5f - 0.5f * (f.p[1][1] * f.pose.t[1] + f.p[1][2] * f.pose.t[2]) / f.pose.t[2]);
        if (!L->model) return;
        Rigid vw; bool have;
        { std::lock_guard lk(g_XfM); vw = g_ViewRigid; have = g_ViewValid; }
        if (!have) return;
        Rigid w;                                     // model->world, straight from the shader's own constants
        const float* rows[3] = { in + L->mx, in + L->my, in + L->mz };
        for (int i = 0; i < 3; i++) { memcpy(w.r + i * 3, rows[i], 12); w.t[i] = rows[i][3]; }
        Rigid pred = Mul(vw, w);
        LOGI("gpu6dof:   model->world t=({:.3f},{:.3f},{:.3f}); predicted view t=(r {:.3f}, u {:.3f}, f {:.3f}) [should match the pose above]",
             w.t[0], w.t[1], w.t[2], pred.t[0], pred.t[1], pred.t[2]);
        LOGI("gpu6dof:   predicted rows=({:.3f},{:.3f},{:.3f})({:.3f},{:.3f},{:.3f})({:.3f},{:.3f},{:.3f})",
             pred.r[0], pred.r[1], pred.r[2], pred.r[3], pred.r[4], pred.r[5], pred.r[6], pred.r[7], pred.r[8]);
    }

    //! Which first-person entity a draw belongs to: the nearest of the sampled anchors to its model origin, or -1
    //! when nothing is close enough to be sure. Nearest-of-all rather than "within X of the weapon", so a draw that
    //! sits equally near the arm is left alone instead of being claimed by whichever test ran first.
    int NearestAnchor(const BodyHide::Anchor* a, const float w[3], float* distOut, float* marginOut)
    {
        int best = -1, second = -1; float bd = 1e30f, sd = 1e30f;
        for (int i = 0; i < BodyHide::aCount; i++)
        {
            if (!a[i].valid) continue;
            const float dx = w[0] - a[i].org[0], dy = w[1] - a[i].org[1], dz = w[2] - a[i].org[2];
            const float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d < bd) { sd = bd; second = best; bd = d; best = i; }
            else if (d < sd) { sd = d; second = i; }
        }
        if (distOut) *distOut = best >= 0 ? bd : -1.f;
        if (marginOut) *marginOut = second >= 0 ? sd - bd : 1e30f;
        return best;
    }

    //! Rewrite one PerInstanceCB. False = leave this draw alone. `byIndex` is true when the draw was picked by its
    //! index count, which then overrides the proximity filter.
    //! Filled for arm-model draws: which arm, the hand socket in MODEL space (the collapse point of the hand-only
    //! mode) and, for the full-arm mode, the model->world of the draw and the hand's delta in model space.
    struct ArmInfo
    {
        int side = -1; float socket[3] = {};
        bool full = false; Rigid m2w, dh; void* palKey = nullptr;
        void* vb0 = nullptr;                     // the draw's first vertex buffer: identifies the MESH (skin vs sleeve) across a load
        Rigid bodyW; bool bodyValid = false; Rigid bodyVr; float ctrl[3] = {};   // ctrl = this hand's target (controller) in world space
        // vr handjit: what this draw's OWN data says the palm should be, to compare with where it is drawn
        Rigid pose, vw, palmTgt; bool palmTgtValid = false; int palm = -1; float palmC[3] = {}; unsigned long long serial = 0;
        uint64_t palHash = 0;                    // the palette content the hand delta was computed from
        Rigid prevPose; bool prevValid = false;  // the draw's PREVIOUS-frame model->view (its motion-vector matrix)
        int grip = -1; bool gripApplied = false; // the weapon grip of this hand (g_Grips index) and whether the palm was posed from it
    };
    //! FNV-1a over a palette's bone matrices: tells two reads of the same buffer apart when the game rewrote it between.
    uint64_t PalHash(const float* pal, UINT bones)
    {
        uint64_t h = 1469598103934665603ull;
        const uint64_t* p = (const uint64_t*)pal; const size_t n = (size_t)bones * 6;   // 48 bytes a bone, 8 at a time
        for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; h ^= h >> 29; }
        return h;
    }
    thread_local float t_DrawProj[4][4]; thread_local Rigid t_DrawProjView; thread_local bool t_DrawProjValid = false; thread_local unsigned long long t_DrawProjSerial = 0;
    bool TransformInstanceCb(ID3D11DeviceContext* ctx, const InstLayout* L, const float* in, UINT bytes, float* out, bool byIndex, UINT draw, ArmInfo* ai = nullptr)
    {
        t_DrawProjValid = false;
        if (!L || !L->ok) { g_NoLayout++; return false; }
        const bool byAnchor = g_IdMode.load() == 1;
        if (!byIndex && !byAnchor && g_OnlyWeapon.load())
        {   // legacy: move only the draws the scan diff kept. Arms and torso share the weapon's model matrix, so
            // without this a wrist turn drags the whole first-person body around.
            std::lock_guard lk(g_WeaponM);
            if (!g_WeaponIdx.empty() && !g_WeaponIdx.count(draw)) { g_NotWeapon++; return false; }
        }
        Rigid dvH[2], dvPrevH[2], dwH[2], vinv, vw, targetH[2], handH[2]; bool targetValid[2];
        float cam[3], camAxis[9]; bool haveCam;
        BodyHide::Anchor anch[BodyHide::aCount];
        Rigid bodyW, bodyVr; bool bodyValid; int entStable[2]; unsigned long long xfSerial;
        {
            std::lock_guard lk(g_XfM);
            const XfState* own = nullptr;        // the transform of the view this draw belongs to
            if (t_DrawSerial)
            { const XfState& r = g_XfRing[t_DrawSerial % kXfRing]; if (r.serial == t_DrawSerial && r.valid) own = &r; }
            if (own)
            {
                xfSerial = own->serial;
                for (int h = 0; h < 2; h++)
                { dvH[h] = own->dv[h]; dvPrevH[h] = own->dvPrev[h]; dwH[h] = own->dw[h]; targetH[h] = own->target[h]; targetValid[h] = own->targetValid[h]; handH[h] = own->hand[h]; }
                vinv = own->vinv; vw = own->vw; haveCam = own->camValid; memcpy(cam, own->cam, 12); memcpy(camAxis, own->camAxis, 36);
                memcpy(anch, own->anch, sizeof anch);
                bodyW = own->bodyDelta; bodyVr = own->bodyVr; bodyValid = own->bodyValid;
            }
            else
            {
                if (!g_XfValid) return false;
                xfSerial = g_XfSerial;
                for (int h = 0; h < 2; h++)
                { dvH[h] = g_DeltaView[h]; dvPrevH[h] = g_DeltaViewPrev[h]; dwH[h] = g_DeltaWorld[h]; targetH[h] = g_TargetPose[h]; targetValid[h] = g_TargetValid[h]; handH[h] = g_HandPose[h]; }
                vinv = g_ViewInv; vw = g_ViewRigid; haveCam = g_GameCamValid; memcpy(cam, g_GameCam, 12); memcpy(camAxis, g_GameCamAxis, 36);
                memcpy(anch, g_Anchor, sizeof anch);
                bodyW = g_BodyDelta; bodyVr = g_BodyVr; bodyValid = g_BodyValid;
            }
            entStable[0] = g_EntStable[0]; entStable[1] = g_EntStable[1];
            t_XfUsed = xfSerial;
        }
        Rigid ownDv; bool useOwnDv = false;      // an arm draw whose hand pose comes from the palette, not the equipment entity
        bool carryDraw = false;                  // a draw of the item carried in a hand (PhysicalPickup)
        bool torso = false;                      // a draw of the player entity's own models (torso, legs): follows the HMD body frame
        const bool fullArm = g_ArmMode.load() == 1;
        const int diag = g_DiagLeft.load();

        // The layout comes from the shader, the size from the buffer the engine bound to it. They normally agree,
        // but nothing in the API forces them to, and a row reaching past the bound buffer would be rebuilt out of
        // whatever the shadow happened to hold there.
        const size_t floats = bytes / 4;
        auto within = [&](int off) { return off >= 0 && (size_t)off + 4 <= floats; };
        if (!(within(L->x) && within(L->y) && within(L->z) && within(L->w))) { g_NoLayout++; return false; }
        const bool usePrev = L->prev && within(L->px) && within(L->py) && within(L->pz) && within(L->pw);
        const bool useModel = L->model && within(L->mx) && within(L->my) && within(L->mz);

        Factored f = Factor(in + L->x, in + L->y, in + L->z, in + L->w);
        if (diag > 0 && g_DiagLeft.fetch_sub(1) > 0) LogDraw(L, in, f);
        if (!f.ok) { g_FactorFail++; return false; }
        // this draw's projection, for the reticle if the draw turns out to be the player's (see g_Proj)
        memcpy(t_DrawProj, f.p, sizeof t_DrawProj); t_DrawProjView = vw; t_DrawProjValid = true; t_DrawProjSerial = xfSerial;
        if (g_ProjAny.load())
        {
            std::lock_guard lk(g_ProjM);
            memcpy(g_Proj, f.p, sizeof g_Proj); g_ProjView = vw; g_ProjValid = true; g_ProjFrame = g_PresentFrame;
        }
        // Where this draw sits relative to the game camera. Measuring against the GAME camera rather than the
        // rendered view keeps head tracking from pushing the weapon out of range when the player leans or stands.
        if (!haveCam) { g_NotNear++; return false; }
        float rel[3] = {};                        // in the camera frame: fwd / left / up
        float worldOrg[3];                        // the draw's model origin in world space
        {
            for (int i = 0; i < 3; i++)
                worldOrg[i] = vinv.r[i * 3] * f.pose.t[0] + vinv.r[i * 3 + 1] * f.pose.t[1]
                            + vinv.r[i * 3 + 2] * f.pose.t[2] + vinv.t[i];
            const float w[3] = { worldOrg[0] - cam[0], worldOrg[1] - cam[1], worldOrg[2] - cam[2] };
            for (int k = 0; k < 3; k++) rel[k] = w[0] * camAxis[k * 3] + w[1] * camAxis[k * 3 + 1] + w[2] * camAxis[k * 3 + 2];
        }
        // vr gunparts: a draw rejected before the weapon test although it sits by a held weapon (research)
        auto partsReject = [&](const char* why) {
            if (g_PartsLeft.load() <= 0) return;
            float best = 1e9f; int wk = -1;
            for (int k = 0; k < 2; k++) { const BodyHide::Anchor& w = anch[k == 0 ? BodyHide::aWeaponL : BodyHide::aWeaponR]; if (!w.valid) continue; float q = 0; for (int i = 0; i < 3; i++) q += (worldOrg[i] - w.org[i]) * (worldOrg[i] - w.org[i]); if (q < best) { best = q; wk = k; } }
            if (wk < 0 || best > 0.8f * 0.8f) return;
            g_PartsLeft--;
            LOGI("gunparts: REJECTED ({}) draw n={} cb{} | engine origin ({:.3f},{:.3f},{:.3f}), {:.3f} m from weapon {}", why, draw, bytes, worldOrg[0], worldOrg[1], worldOrg[2], sqrtf(best), wk);
        };
        if (g_CensusLeft.load() > 0) CensusAdd(f.p, sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]), false);
        {   // a draw of the carried item: its model origin is the item entity's (wherever it was taken from)
            const BodyHide::Anchor& ca = anch[BodyHide::aCarry];
            if (ca.valid && byAnchor)
            {
                const float dx = worldOrg[0] - ca.org[0], dy = worldOrg[1] - ca.org[1], dz = worldOrg[2] - ca.org[2];
                if (dx * dx + dy * dy + dz * dz < 0.03f * 0.03f) { std::lock_guard lk(g_CarryM); carryDraw = g_Carry.on; }
            }
        }
        if (!byIndex && !carryDraw)
        {
            float n = g_Near.load();
            if (n > 0.f && g_BodyFollow.load() && n < 2.5f) n = 2.5f;   // the player entity's origin is at the feet, farther than any weapon
            const float d2 = rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2];
            if (!(n > 0.f) || d2 > n * n) { partsReject("not near"); g_NotNear++; return false; }
        }
        // Which controller drives this draw. With the entity anchors on it is simply which equipment entity the draw
        // belongs to; otherwise, the side of the view it is DRAWN on. Matching it to whichever controller was
        // nearest at calibration time fails outright when both hands hang together -- measured, with the two
        // controllers 1.7 cm apart in X, which handed the left-hand weapon to the right controller.
        int hand = g_SixHand.load();
        if (byAnchor && g_OnlyWeapon.load())
        {   // the entity test: this draw is the weapon only if the weapon entity is the nearest thing to it. The
            // radius is a sanity bound (a weapon plus its mods spans far less than that), not the discriminator.
            float d = 0.f, margin = 0.f;
            const int a = NearestAnchor(anch, worldOrg, &d, &margin);
            if (a < 0) { partsReject("no anchor"); g_NoAnchor++; return false; }
            if (carryDraw)
            {   // the carried item: drawn at hand * rel (or on its way back), the entity left where it is
                const BodyHide::Anchor& ca = anch[BodyHide::aCarry];
                Rigid ent; for (int i = 0; i < 3; i++) { ent.r[i * 3] = ca.axis[i]; ent.r[i * 3 + 1] = ca.axis[3 + i]; ent.r[i * 3 + 2] = ca.axis[6 + i]; ent.t[i] = ca.org[i]; }
                Rigid target = ent; bool moved = false;
                {
                    std::lock_guard lk(g_CarryM);
                    const int ch = g_Carry.hand & 1;
                    if (g_Carry.returning)
                    {
                        const double u = (CarryNow() - g_Carry.retStart) / kCarryBack;
                        if (u < 1.0 && g_Carry.lastValid)
                        {   // smoothstep from where it was in the hand back to its place: positions lerped, the rotation
                            // blended and re-orthonormalised (short and small: good enough for a quarter second)
                            const float s = (float)(u * u * (3.0 - 2.0 * u));
                            const Rigid& a0 = g_Carry.retFrom;
                            for (int i = 0; i < 3; i++) target.t[i] = a0.t[i] + (ent.t[i] - a0.t[i]) * s;
                            float c[3][3];
                            for (int col = 0; col < 3; col++) for (int i = 0; i < 3; i++) c[col][i] = a0.r[i * 3 + col] + (ent.r[i * 3 + col] - a0.r[i * 3 + col]) * s;
                            auto nrm = [](float* v) { const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-6f) for (int k = 0; k < 3; k++) v[k] /= l; };
                            nrm(c[0]);
                            const float dp = c[1][0] * c[0][0] + c[1][1] * c[0][1] + c[1][2] * c[0][2];
                            for (int k = 0; k < 3; k++) c[1][k] -= c[0][k] * dp;
                            nrm(c[1]);
                            c[2][0] = c[0][1] * c[1][2] - c[0][2] * c[1][1]; c[2][1] = c[0][2] * c[1][0] - c[0][0] * c[1][2]; c[2][2] = c[0][0] * c[1][1] - c[0][1] * c[1][0];
                            for (int col = 0; col < 3; col++) for (int i = 0; i < 3; i++) target.r[i * 3 + col] = c[col][i];
                            moved = true;
                        }
                    }
                    else if (targetValid[ch])
                    {
                        if (!g_Carry.relValid) { g_Carry.rel = Mul(Inv(handH[ch]), ent); g_Carry.relValid = true; }
                        target = Mul(handH[ch], g_Carry.rel);
                        g_Carry.last = target; g_Carry.lastValid = true;
                        moved = true;
                    }
                    hand = ch;
                }
                if (!moved) { g_NotWeapon++; return false; }      // drawn where it lies
                const Rigid gdw = Mul(target, Inv(ent));
                ownDv = Mul(vw, Mul(gdw, Inv(vw))); useOwnDv = true;
                g_CarryDraws++;
            }
            else if (a == BodyHide::aPlayer && d < 0.05f)
            {   // The torso (PlayerBody): its models have their origin at the player entity. With body follow on
                // they get the HMD body delta, so leaning and crouching move the chest with the head.
                if (!g_BodyFollow.load() || !bodyValid) { g_NotWeapon++; return false; }
                torso = true; g_BodyDrawsLive++;
            }
            else if ((a == BodyHide::aArmL || a == BodyHide::aArmR) && d < 0.05f)
            {   // An ARM model: its origin coincides with the arm entity (both arms share one). Which arm it is
                // comes from the joint palette it binds; the draw then follows that hand's weapon delta, so the
                // hand stays attached to whatever it is holding.
                if (!g_HandFollow.load() || !ctx) { g_NotWeapon++; return false; }
                ID3D11ShaderResourceView* sr[3] = {};
                ctx->VSGetShaderResources(0, 3, sr);
                // The side comes from the JOINT PALETTE alone -- the slot the shader's reflection names ro_matrices
                // (and its previous-frame twin), never from whatever else sits in t0..t2. Each pass of the same arm
                // mesh binds its palette at a different slot (measured: t1 in the 160-byte pass, t2 in the 768, t0
                // in the 240), and the other slots hold resources the identification pass had also filed under a
                // side. With "the last known slot wins" the right arm's skin was drawn in its 240-byte pass with the
                // LEFT arm's delta: that pass landed somewhere else and the hand came out black with only its
                // highlights. Which arm it hit changed from one boot to the next with what the identification saw.
                void* keys[3] = {};
                const bool byPal = L && L->matSlot >= 0 && L->matSlot < 3;
                if (byPal)
                {   // matSlot last, so it decides if the two ever disagree
                    if (L->prevMatSlot >= 0 && L->prevMatSlot < 3) keys[0] = sr[L->prevMatSlot];
                    keys[1] = sr[L->matSlot];
                }
                else for (int i = 0; i < 3; i++) keys[i] = sr[i];
                for (int i = 0; i < 3; i++)
                    if (sr[i])
                    {
                        ID3D11Resource* res = nullptr; sr[i]->GetResource(&res);
                        if (res) { WantPalette(res); res->Release(); }
                        sr[i]->Release();
                    }
                const int side = ArmSideFromSrv(keys);
                if (side < 0) { partsReject("arm model, side unknown"); g_ArmUnknownLive++; return false; }
                g_ArmHitsLive[side]++;
                if (L && L->dh >= 0 && within(L->dh)) NoteArmDepthHack(f.p, in + L->dh, L->dhn);
                if (side == g_VbDumpSide.load() && g_VbDumpWant.load() && ctx && draw == (UINT)g_VbDumpWant.load()) { CaptureVertexStreams(ctx, draw); g_VbDumpWant = 0; }
                if (hand < 0) hand = side;
                // The joint palette this draw binds: the shadow copy is read through it, and it keys the skeleton
                // the mesh is rigged to (the arm model has more than one).
                ID3D11Buffer* pb = nullptr;
                if (L && L->matSlot >= 0)
                {
                    ID3D11ShaderResourceView* sv = nullptr; ctx->VSGetShaderResources(L->matSlot, 1, &sv);
                    if (sv) { ID3D11Resource* res = nullptr; sv->GetResource(&res); if (res) { res->QueryInterface(__uuidof(ID3D11Buffer), (void**)&pb); res->Release(); } sv->Release(); }
                }
                // The mesh behind this draw, once: bind-pose centroids per bone tell hand from forearm.
                if (g_PresentFrame < g_ArmForeignUntil.load()) g_ArmForeignSkipped++;   // not the player's arms (Shadow Walk's claws)
                else { std::lock_guard lk(g_MeshM); if (!g_MeshCap.have && ctx) CaptureArmMesh(ctx, side, pb); }
                if (ai)
                {
                    {   // which mesh this is: the vertex buffer survives a level load, the palette does not
                        ID3D11Buffer* vb = nullptr; UINT st = 0, of = 0; ctx->IAGetVertexBuffers(0, 1, &vb, &st, &of);
                        ai->vb0 = vb; if (vb) vb->Release();
                    }
                    ai->palKey = pb; ai->bodyW = bodyW; ai->bodyValid = bodyValid; ai->bodyVr = bodyVr;
                    ai->pose = f.pose; ai->vw = vw; ai->serial = xfSerial;
                    if (usePrev) { const Factored fp = Factor(in + L->px, in + L->py, in + L->pz, in + L->pw); if (fp.ok) { ai->prevPose = fp.pose; ai->prevValid = true; } }
                    if (targetValid[side]) memcpy(ai->ctrl, targetH[side].t, 12);
                    const BodyHide::Anchor& wa = anch[side == 0 ? BodyHide::aWeaponL : BodyHide::aWeaponR];
                    const Rigid modelToWorld = Mul(vinv, f.pose);
                    const Rigid modelInv = Inv(modelToWorld);            // world -> model
                    // Default collapse point: the equipment socket in model space (right when a weapon is held).
                    for (int i = 0; i < 3; i++)
                        ai->socket[i] = modelInv.r[i * 3] * wa.org[0] + modelInv.r[i * 3 + 1] * wa.org[1] + modelInv.r[i * 3 + 2] * wa.org[2] + modelInv.t[i];
                    // An EMPTY hand has no equipment entity at all (Corvo's left in chapter 1: entL 0), and requiring
                    // one left that arm out of everything -- drawn where the animation had it, not on the controller.
                    // With the controller tracked the empty-hand branch below poses it; the check after it drops the
                    // arm again if nothing did.
                    ai->side = (wa.valid || targetValid[side]) ? side : -1;
                    Rigid handDeltaW = dwH[side];                        // world delta that puts this hand where the controller is
                    // The palm from the palette: its frame in world space, and its centroid as the collapse point.
                    ArmBones ab; { std::lock_guard lk(g_ArmBonesM); ab = g_ArmBones[side]; }
                    if (ab.valid && ab.palm >= 0 && L && L->matSlot >= 0)
                    {
                        static thread_local float pal[kPalMaxFloats]; UINT pbytes = 0;
                        const bool got = pb && FindPalette(pb, pal, &pbytes) && (int)(pbytes / 48) > ab.palm;
                        if (got)
                        {
                            ai->palHash = PalHash(pal, pbytes / 48);
                            const float* m = pal + ab.palm * 12;

                            Rigid P;                                       // palm frame in MODEL space: R = M.R, t = M * centroid
                            for (int r = 0; r < 3; r++) { P.r[r * 3] = m[r * 4]; P.r[r * 3 + 1] = m[r * 4 + 1]; P.r[r * 3 + 2] = m[r * 4 + 2];
                                                          P.t[r] = m[r * 4] * ab.c[ab.palm][0] + m[r * 4 + 1] * ab.c[ab.palm][1] + m[r * 4 + 2] * ab.c[ab.palm][2] + m[r * 4 + 3]; }
                            memcpy(ai->socket, P.t, 12);                   // collapse onto the palm, whatever is equipped
                            const Rigid Pw = Mul(modelToWorld, P);         // palm frame in world space
                            // handrel: the palm frame RELATIVE TO THE CONTROLLER, after this frame's delta. It is
                            // the one quantity the weapon case and the power case must agree on -- the weapon case
                            // is the pose the user validated, so if replaying the hold works, the two print the
                            // same rows. Measured here rather than judged from a screenshot: the hand is a fanned
                            // black glove and its apparent "direction" is not readable when the fingers splay.
                            // (on request only: `vr gpuhandrel <n>`; it used to log every 180th frame by default)
                            bool relLog = false;
                            if (g_RelLogLeft.load() > 0) { relLog = true; g_RelLogLeft--; }
                            const int whits = g_WeaponHits[side].load();
                            float entPalm = -1.f;
                            if (wa.valid) { float d2 = 0; for (int i = 0; i < 3; i++) { const float x = wa.org[i] - Pw.t[i]; d2 += x * x; } entPalm = sqrtf(d2); }
                            g_EntPalm[side] = entPalm;
                            // a power shows a single FX draw at its entity, on the forearm; a weapon is many draws; a
                            // gadget is one or two, with its entity in the palm (see g_GadgetPalmMax)
                            // ...or the inventory says so: the Heart draws 8 meshes, as many as a pistol, and was aimed
                            // like one -- floating above an open hand (measured). Its item component is arkCpntHeart.
                            // With the item known to be one, it stays a gadget even when the entity leaves the palm: the
                            // idle animation tosses it about (measured: a sticky grenade 0.2 m out of an open hand), and
                            // the gadget then keeps the last pose it had IN the palm (see GadgetHold) instead of falling
                            // back to the weapon branch and floating next to the hand.
                            const bool palmNear = entPalm >= 0.f && entPalm < g_GadgetPalmMax.load();
                            const bool itemGadget = g_GadgetOn.load() == 2 && BodyHide::HoldsGadget(side) && whits >= 1 && entPalm >= 0.f;
                            // ...but never a firearm: the pistol's reload brings it into the palm drawing two meshes,
                            // and as a 'gadget' it was posed into the anatomical palm and vanished from the hand for
                            // ~0.85 s of every reload (measured, the development notes)
                            const bool gadget = g_GadgetOn.load() && !BodyHide::HoldsGun(side) && (((whits >= 1 && whits < 3) && palmNear) || itemGadget);
                            const bool gadgetAnat = gadget && g_GadgetOn.load() == 2 && g_UseHold.load() == 2 && ab.palmNValid && wa.valid && targetValid[side];
                            const bool weaponHeld = ((whits >= 3 || (whits >= 1 && BodyHide::HoldsGun(side))) && !gadgetAnat) || (gadget && !gadgetAnat);
                            // Crouched, the game turns the hand round the weapon: the sword goes to a reverse grip by
                            // rotating the WRIST inside the arm model, while the equipment entity (what the weapon
                            // branch pins to the controller) stays put. The hand rode the weapon delta, so it flipped
                            // with the animation -- reported from the headset: standing, the right hand's fingers point
                            // left; crouched they turned over. The blade may turn (the user likes the reverse grip),
                            // the hand may not: crouched, the hand keeps the hold it had standing, and nothing is
                            // learned from the crouched pose. On standing up it stays on that hold until the
                            // animation has come back within 12 deg of it (at most 1.5 s), so there is no pop.
                            const bool crouch = PlayerState::Running("Sneaking") || PlayerState::Running("AutoCrouch") || PlayerState::Running("WallCrouch");
                            static unsigned long long crouchSeen[2] = {}; static bool crouchHold[2] = {};
                            if (crouch && g_CrouchHoldOn.load()) { crouchSeen[side] = g_PresentFrame; crouchHold[side] = true; }
                            if (!g_CrouchHoldOn.load()) crouchHold[side] = false;
                            std::lock_guard lk(g_ArmBonesM);
                            if (crouchHold[side] && !crouch && targetValid[side] && g_HandHold[side].valid)
                            {
                                const Rigid live = Mul(Inv(handH[side]), Mul(dwH[side], Pw));
                                const Rigid& h = g_HandHold[side].h;
                                float tr = 0; for (int i = 0; i < 9; i++) tr += live.r[i] * h.r[i];   // trace(h^T live)
                                const float ang = acosf(std::clamp((tr - 1.f) * 0.5f, -1.f, 1.f)) * 57.2958f;
                                if (ang < 12.f || g_PresentFrame - crouchSeen[side] > 135) crouchHold[side] = false;
                            }
                            if (!g_HandHold[side].valid) crouchHold[side] = false;
                            // The weapon's grip (see g_GripMode): the palm in the weapon entity's frame, learned only
                            // while nothing moves the hand on the weapon, and then the hand is ALWAYS posed from it.
                            const uint32_t cls = BodyHide::HeldItemClass(side);
                            const bool gripKind = g_GripMode.load() && weaponHeld && !gadget && wa.valid && cls != 0;
                            const bool learnClean = g_GripClean[side].load() && entStable[side] >= kSettleFrames && whits >= 3;
                            GripLookup gl;
                            if (gripKind)
                            {
                                Rigid entW; for (int i = 0; i < 3; i++) { entW.r[i * 3] = wa.axis[i]; entW.r[i * 3 + 1] = wa.axis[3 + i]; entW.r[i * 3 + 2] = wa.axis[6 + i]; entW.t[i] = wa.org[i]; }
                                const Rigid relA = Mul(Inv(entW), Pw);
                                const float relAd = sqrtf(relA.t[0] * relA.t[0] + relA.t[1] * relA.t[1] + relA.t[2] * relA.t[2]);
                                GripObserve(side, cls, ab.bones, ab.palm, (int)ab.verts, relA, learnClean && relAd < kHandSocketMax);
                                gl = GripFind(side, cls, ab.bones, ab.palm, (int)ab.verts);
                            }
                            const bool gripApply = gl.valid && targetValid[side] && g_GripMode.load() == 1;
                            ai->grip = gl.valid ? gl.idx : -1; ai->gripApplied = gripApply;
                            if (gripApply)
                            {   // the palm where the weapon AS DRAWN holds it: target * (palm in the entity's frame)
                                const Rigid dw = Mul(Mul(targetH[side], gl.rel), Inv(Pw));
                                ownDv = Mul(vw, Mul(dw, Inv(vw)));
                                useOwnDv = true; handDeltaW = dw;
                                g_GripUsed[side]++;
                            }
                            else if (weaponHeld && wa.valid && crouchHold[side] && targetValid[side])
                            {
                                const Rigid dw = Mul(Mul(handH[side], g_HandHold[side].h), Inv(Pw));
                                ownDv = Mul(vw, Mul(dw, Inv(vw)));
                                useOwnDv = true; handDeltaW = dw;
                                g_CrouchHoldUsed[side]++;
                            }
                            // Q and the hold are learned from the animated palm too, so under the same rule: only while
                            // nothing moves the hand on the weapon (they were learned in every frame that was not
                            // crouched, and the hold was what a crouch then replayed).
                            const bool learnQ = weaponHeld && wa.valid && entStable[side] >= kSettleFrames && whits >= 3 &&   // not from a gun's reload (fewer draws, the gun in the palm)
                                                (g_GripMode.load() ? learnClean : !(crouchHold[side] && targetValid[side]));
                            if (learnQ)
                            {   // learn: socket = palm * Q, constant while the weapon is in the hand. Only once the
                                // equipment entity has been the same for a while: at a weapon swap the draw count
                                // still says "weapon" for a few frames after the entity has become the POWER's, which
                                // sits on the forearm -- measured, that learned a Q of 0.8 m, saved it, and every
                                // power hand afterwards flew 0.6 m past the controller.
                                Rigid ent; for (int i = 0; i < 3; i++) { ent.r[i * 3] = wa.axis[i]; ent.r[i * 3 + 1] = wa.axis[3 + i]; ent.r[i * 3 + 2] = wa.axis[6 + i]; ent.t[i] = wa.org[i]; }
                                HandQ& hq = g_HandQ[side];
                                const Rigid q = Mul(Inv(Pw), ent);
                                const float qd = sqrtf(q.t[0] * q.t[0] + q.t[1] * q.t[1] + q.t[2] * q.t[2]);
                                if (qd < kHandSocketMax)             // a hand's socket is within a hand's breadth of the palm
                                {
                                    if (!hq.valid) LOGI("gpuhands: side {} hand socket learned from the weapon: palm bone {}, offset ({:.3f},{:.3f},{:.3f}) m", side, ab.palm, q.t[0], q.t[1], q.t[2]);
                                    hq.q = q; hq.valid = true; hq.bones = ab.bones; hq.palm = ab.palm;
                                    g_SaveQPending = true;               // written at Present: SaveHandQ takes this lock
                                    // And the hold: where this same weapon puts the palm relative to the controller.
                                    // dwH is the delta the weapon branch applies, so dwH * Pw IS the palm frame the
                                    // user validated; referred to the controller it is a constant of the hand, not
                                    // of the weapon, and a power can be given the very same pose.
                                    if (targetValid[side] && !(crouch && g_CrouchHoldOn.load()))
                                    {
                                        const Rigid hold = Mul(Inv(handH[side]), Mul(dwH[side], Pw));
                                        const float hd = sqrtf(hold.t[0] * hold.t[0] + hold.t[1] * hold.t[1] + hold.t[2] * hold.t[2]);
                                        HandHold& hh = g_HandHold[side];
                                        if (hd < kHandHoldMax)
                                        {
                                            if (!hh.valid) LOGI("gpuhands: side {} hand HOLD learned from the weapon: palm is ({:.3f},{:.3f},{:.3f}) m from the controller", side, hold.t[0], hold.t[1], hold.t[2]);
                                            hh.h = hold; hh.valid = true; hh.bones = ab.bones; hh.palm = ab.palm;
                                        }
                                    }
                                }
                                else
                                {
                                    g_HandQRejected++;
                                    static std::atomic<unsigned long long> lastRejLog[2];
                                    if (g_PresentFrame - lastRejLog[side].load() > 600)
                                    {
                                        lastRejLog[side] = g_PresentFrame;
                                        LOGI("gpuhands: side {} hand socket NOT learned: the equipment entity is {:.2f} m from the palm (q t {:.2f},{:.2f},{:.2f}; kept {})",
                                             side, qd, q.t[0], q.t[1], q.t[2], hq.valid ? "the previous one" : "nothing");
                                    }
                                }
                            }
                            if (!weaponHeld && targetValid[side])
                            {   // a power (or nothing) in this hand: the equipment entity is not on the hand, the palm is.
                                // The hand's own grip, not the weapon's: targetH aligns the BARREL with the
                                // controller, and there is no barrel here. See the note on g_HandGripYaw.
                                // With no socket constant yet, the palm itself is the socket (Q = identity): that puts
                                // the palm centroid on the controller, which is about where a controller sits in a real
                                // hand. Anything is better than falling through to the branch below, which moves this
                                // hand by the WEAPON delta -- and a power's equipment entity is on the FOREARM,
                                // measured 1.61 m from the palm, so the hand left the controller altogether.
                                const Rigid Q = g_HandQ[side].valid ? g_HandQ[side].q : Rigid();
                                const Rigid entEst = Mul(Pw, Q);
                                Rigid dw;
                                const int holdMode = g_UseHold.load();
                                if (holdMode == 2 && ab.palmNValid)
                                {   // ANATOMY, not a weapon's grip. In the controller's own frame (columns
                                    // fwd/left/up) a hand holding a controller has its long axis along the pointing
                                    // direction and its palm towards the body's midline -- to the RIGHT for the left
                                    // hand. That is the user's spec in so many words, and unlike the hold below it
                                    // does not depend on which weapon was last in that hand: measured, the pistol and
                                    // the crossbow teach holds 90 degrees apart (rel rows (0.96,0.18,0.23)... against
                                    // (-0.00,-0.14,0.99)...), and a power replayed whichever came last -- so the same
                                    // power looked one way before picking up the crossbow and another way after.
                                    const float bf[3] = { -ab.axis[0], -ab.axis[1], -ab.axis[2] };   // bind: wrist -> fingertips
                                    const Rigid bind = BasisFromTwo(bf, ab.palmN);
                                    const float wantF[3] = { 1.f, 0.f, 0.f };                        // controller forward
                                    const float wantN[3] = { 0.f, side == 0 ? -1.f : 1.f, 0.f };     // palm to the right / left
                                    const Rigid want = BasisFromTwo(wantF, wantN);
                                    Rigid rel = Mul(want, Inv(bind));
                                    rel.t[0] = rel.t[1] = rel.t[2] = 0.f;                            // rotation only
                                    const Rigid palmTarget = Mul(Mul(handH[side], HandGripOffset(side)), rel);
                                    dw = Mul(palmTarget, Inv(Pw));
                                    g_HoldUsed[side]++;
                                    if (gadgetAnat)
                                    {   // the gadget goes where the hand goes: keep its pose in the animated palm
                                        Rigid ent; for (int i = 0; i < 3; i++) { ent.r[i * 3] = wa.axis[i]; ent.r[i * 3 + 1] = wa.axis[3 + i]; ent.r[i * 3 + 2] = wa.axis[6 + i]; ent.t[i] = wa.org[i]; }
                                        GadgetHold& gh = g_GadgetHold[side];
                                        // Learned only while it is IN the palm, and never from a pose left over from the
                                        // previous item (an equip settles for kSettleFrames).
                                        if (entStable[side] < kSettleFrames) { gh.valid = false; gh.minPalm = 1e9f; }
                                        else if (palmNear && entPalm <= gh.minPalm + 0.015f)
                                        {
                                            gh.minPalm = std::min(gh.minPalm, entPalm);
                                            if (!gh.valid || g_PresentFrame - gh.frame > 30)
                                                LOGI("gpuhands: side {} gadget held in the hand (anatomical pose), {:.2f} m from the palm", side, entPalm);
                                            gh.inPalm = Mul(Inv(Pw), ent); gh.valid = true;
                                        }
                                        gh.rel = rel; gh.frame = g_PresentFrame;
                                    }
                                }
                                else if (g_HandHold[side].valid && holdMode == 1)
                                {   // replay the validated hold: put the palm exactly where a weapon would put it.
                                    // The trim has to rotate the hand IN PLACE. Applied as controller*trim*hold it
                                    // pivots the palm around the CONTROLLER, and the hold puts the palm up to 30 cm
                                    // away from it -- measured in the mock, a 90 deg roll swung the hand ~40 cm and
                                    // dragged the IK's whole arm with it, so the knob changed direction as well as
                                    // orientation and was useless for searching one. Orientation from the trimmed
                                    // product, POSITION from the untrimmed one (plus the trim's own offset).
                                    Rigid palmTarget = Mul(Mul(handH[side], HandGripOffset(side)), g_HandHold[side].h);
                                    Rigid offOnly; memcpy(offOnly.t, HandGripOffset(side).t, 12);   // rotation identity
                                    const Rigid pos = Mul(Mul(handH[side], offOnly), g_HandHold[side].h);
                                    memcpy(palmTarget.t, pos.t, 12);
                                    dw = Mul(palmTarget, Inv(Pw));
                                    g_HoldUsed[side]++;
                                }
                                else dw = Mul(Mul(handH[side], HandGripOffset(side)), Inv(entEst));
                                ownDv = Mul(vw, Mul(dw, Inv(vw)));
                                useOwnDv = true; handDeltaW = dw;
                                g_HandOwnDeltaLive[side]++;
                                static std::atomic<unsigned long long> lastPowerLog{ 0 };
                                if (g_PresentFrame - lastPowerLog.load() > 300)
                                {
                                    lastPowerLog = g_PresentFrame;
                                    LOGI("gpuhands: side {} power hand: palm world ({:.2f},{:.2f},{:.2f}) entEst ({:.2f},{:.2f},{:.2f}) target ({:.2f},{:.2f},{:.2f}) dw t ({:.2f},{:.2f},{:.2f}) Q t ({:.3f},{:.3f},{:.3f})",
                                         side, Pw.t[0], Pw.t[1], Pw.t[2], entEst.t[0], entEst.t[1], entEst.t[2], targetH[side].t[0], targetH[side].t[1], targetH[side].t[2],
                                         dw.t[0], dw.t[1], dw.t[2], Q.t[0], Q.t[1], Q.t[2]);
                                }
                            }
                            if (g_TraceLeft.load() > 0 && targetValid[side])
                            {
                                HandTraceS s; s.valid = true; s.frame = g_PresentFrame; s.whits = whits; s.weapon = weaponHeld; s.gadget = gadget; s.crouchHold = crouchHold[side]; s.grip = gripApply;
                                s.relD = Mul(Inv(targetH[side]), Mul(handDeltaW, Pw));
                                if (wa.valid)
                                {
                                    Rigid ent; for (int i = 0; i < 3; i++) { ent.r[i * 3] = wa.axis[i]; ent.r[i * 3 + 1] = wa.axis[3 + i]; ent.r[i * 3 + 2] = wa.axis[6 + i]; ent.t[i] = wa.org[i]; }
                                    s.relA = Mul(Inv(ent), Pw); s.haveA = true;
                                }
                                std::lock_guard lt(g_TraceM); g_TraceS[side] = s;
                            }
                            {   // Where the hand ENDS UP, kept for the axes overlay. Judging a hand from its
                                // silhouette does not work (a fanned black glove has no readable direction, and it
                                // cost two wrong conclusions), so the palm frame gets drawn into the image instead.
                                const Rigid pf = Mul(handDeltaW, Pw);
                                ai->palmTgt = pf; ai->palmTgtValid = true; ai->palm = ab.palm; memcpy(ai->palmC, ab.c[ab.palm], 12);
                                std::lock_guard lp(g_PalmM);

                                g_PalmW[side] = pf; g_PalmSeen[side] = g_PresentFrame;
                                // The fingers: the bind-pose arm axis runs from the fingertip towards the shoulder.
                                for (int i = 0; i < 3; i++)
                                    g_PalmFing[side][i] = -(pf.r[i * 3] * ab.axis[0] + pf.r[i * 3 + 1] * ab.axis[1] + pf.r[i * 3 + 2] * ab.axis[2]);
                                // And the way the palm faces, which is the half of the question a silhouette never answers.
                                for (int i = 0; i < 3; i++)
                                    g_PalmNorm[side][i] = pf.r[i * 3] * ab.palmN[0] + pf.r[i * 3 + 1] * ab.palmN[1] + pf.r[i * 3 + 2] * ab.palmN[2];
                            }
                            if (relLog && ab.palmNValid && weaponHeld && wa.valid)
                            {   // Which way is the PALM? The hand grips the weapon, so the weapon is on the palm's
                                // side: the sign of (weapon - palm) . palmN settles a question that a screenshot of
                                // a glove cannot. Positive = the normal computed from the mesh points out of the
                                // palm, negative = it points out of the back of the hand.
                                const Rigid pf = Mul(handDeltaW, Pw);
                                float nw[3]; for (int i = 0; i < 3; i++) nw[i] = pf.r[i * 3] * ab.palmN[0] + pf.r[i * 3 + 1] * ab.palmN[1] + pf.r[i * 3 + 2] * ab.palmN[2];
                                float d[3]; for (int i = 0; i < 3; i++) d[i] = wa.org[i] - pf.t[i];
                                const float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                                // ACROSS the weapon, not along it: the entity origin sits 20 cm down the barrel, so
                                // the raw direction is mostly the barrel and says nothing about which face of the
                                // hand touches the stock. Taking the barrel component out leaves the offset that does.
                                const float* wf = wa.axis;                                   // the weapon's forward
                                const float along = d[0] * wf[0] + d[1] * wf[1] + d[2] * wf[2];
                                float dp[3]; for (int i = 0; i < 3; i++) dp[i] = d[i] - wf[i] * along;
                                const float pl = sqrtf(dp[0] * dp[0] + dp[1] * dp[1] + dp[2] * dp[2]);
                                const float c = pl > 1e-4f ? (dp[0] * nw[0] + dp[1] * nw[1] + dp[2] * nw[2]) / pl : 0.f;
                                LOGI("palmside side {} weapon {:.3f} m from the palm ({:.3f} m of it across the barrel), cos to the palm normal {:.2f} ({})",
                                     side, dl, pl, c, c > 0 ? "normal points at the weapon = out of the PALM" : "normal points AWAY: it is the back of the hand");
                            }
                            if (relLog && targetValid[side])
                            {   // The comparison that settles it: both cases must print the same rows.
                                const Rigid rel = Mul(Inv(handH[side]), Mul(handDeltaW, Pw));
                                const Rigid& H = g_HandHold[side].h;
                                LOGI("handrel side {} {} whits {} rel r ({:.2f},{:.2f},{:.2f})({:.2f},{:.2f},{:.2f})({:.2f},{:.2f},{:.2f}) t ({:.3f},{:.3f},{:.3f}) | hold({}) r ({:.2f},{:.2f},{:.2f})({:.2f},{:.2f},{:.2f})({:.2f},{:.2f},{:.2f}) t ({:.3f},{:.3f},{:.3f})",
                                     side, weaponHeld ? "WEAPON" : "power ", g_WeaponHits[side].load(),
                                     rel.r[0], rel.r[1], rel.r[2], rel.r[3], rel.r[4], rel.r[5], rel.r[6], rel.r[7], rel.r[8], rel.t[0], rel.t[1], rel.t[2],
                                     g_HandHold[side].valid ? 1 : 0,
                                     H.r[0], H.r[1], H.r[2], H.r[3], H.r[4], H.r[5], H.r[6], H.r[7], H.r[8], H.t[0], H.t[1], H.t[2]);
                            }
                        }
                    }
                    if (!wa.valid && !useOwnDv) ai->side = -1;           // no equipment and no pose of its own: leave it be
                    // Full arm: the hand's delta in the draw's model space, for the per-bone rewrite. Only once the
                    // rig of this side is known; until then the draw is treated as in the hand-only mode.
                    if (fullArm && ai->side >= 0)
                    {
                        std::lock_guard lr(g_RigM);
                        if (g_Rig[side].valid)
                        {
                            ai->full = true; ai->m2w = modelToWorld;
                            ai->dh = Mul(Inv(modelToWorld), Mul(handDeltaW, modelToWorld));
                        }
                    }
                    static std::atomic<unsigned long long> lastLog{ 0 };
                    const unsigned long long fr = g_PresentFrame;
                    if (fr - lastLog.load() > 120 && side == 0)
                    {
                        lastLog = fr;
                        const Rigid mw = Mul(vinv, f.pose);
                        float back[3];
                        for (int i = 0; i < 3; i++) back[i] = mw.r[i * 3] * ai->socket[0] + mw.r[i * 3 + 1] * ai->socket[1] + mw.r[i * 3 + 2] * ai->socket[2] + mw.t[i];
                        LOGI("gpuhands: side {} socket world=({:.3f},{:.3f},{:.3f}) model origin=({:.3f},{:.3f},{:.3f}) socket model=({:.3f},{:.3f},{:.3f}) roundtrip=({:.3f},{:.3f},{:.3f}) s={:.3f}",
                             side, wa.org[0], wa.org[1], wa.org[2], worldOrg[0], worldOrg[1], worldOrg[2],
                             ai->socket[0], ai->socket[1], ai->socket[2], back[0], back[1], back[2], f.s);
                    }
                }
                if (pb) pb->Release();
            }
            else
            {
                if ((a != BodyHide::aWeaponL && a != BodyHide::aWeaponR) || d > g_AnchorRadius.load())
                {
                    if (g_PartsLeft.load() > 0)
                    {   // vr gunparts: a draw left alone although it sits by a held weapon -- drawn where the flat game has it
                        float dw[2] = { -1.f, -1.f };
                        for (int k = 0; k < 2; k++) { const BodyHide::Anchor& w = anch[k == 0 ? BodyHide::aWeaponL : BodyHide::aWeaponR]; if (w.valid) { float q = 0; for (int i = 0; i < 3; i++) q += (worldOrg[i] - w.org[i]) * (worldOrg[i] - w.org[i]); dw[k] = sqrtf(q); } }
                        if ((dw[0] >= 0.f && dw[0] < 0.6f) || (dw[1] >= 0.f && dw[1] < 0.6f))
                            { g_PartsLeft--; LOGI("gunparts: UNCLAIMED draw n={} cb{} | engine origin ({:.3f},{:.3f},{:.3f}) nearest anchor {} at {:.3f} m | weapon L {:.3f} R {:.3f} m", draw, bytes, worldOrg[0], worldOrg[1], worldOrg[2], a, d, dw[0], dw[1]); }
                    }
                    g_NotWeapon++; return false;
                }
                const int owner = a == BodyHide::aWeaponL ? 0 : 1;
                g_WeaponHitsLive[owner]++;
                if (d < 0.03f && !g_JitFrozen.load())
                {   // vr handjit: the weapon's own draws sit ON its entity -- unless the entity was sampled in another frame
                    std::lock_guard lk(g_JitWM); JitWeapon& s = g_JitW[owner]; s.n++; s.sum += d; if (d > s.max) s.max = d;
                }
                if (hand < 0) hand = owner;
                // A gadget held in the anatomical hand (gpugadget 2): it takes the hand's pose, not a barrel's.
                const BodyHide::Anchor& wa = anch[owner == 0 ? BodyHide::aWeaponL : BodyHide::aWeaponR];
                GadgetHold gh; { std::lock_guard lk(g_ArmBonesM); gh = g_GadgetHold[owner]; }
                if (g_GadgetOn.load() == 2 && gh.valid && g_PresentFrame - gh.frame <= 3 && wa.valid && targetValid[owner])
                {
                    Rigid ent; for (int i = 0; i < 3; i++) { ent.r[i * 3] = wa.axis[i]; ent.r[i * 3 + 1] = wa.axis[3 + i]; ent.r[i * 3 + 2] = wa.axis[6 + i]; ent.t[i] = wa.org[i]; }
                    const Rigid palmTarget = Mul(Mul(handH[owner], HandGripOffset(owner)), gh.rel);
                    const Rigid gdw = Mul(Mul(palmTarget, gh.inPalm), Inv(ent));
                    ownDv = Mul(vw, Mul(gdw, Inv(vw))); useOwnDv = true;
                    g_GadgetDraws[owner]++;
                    if (g_PartsLeft.load() > 0) LOGI("gunparts: hand {} draw n={} posed as a GADGET (palm)", owner, draw);
                }
            }
        }
        // rel is in the camera frame (fwd/left/up), so rel[1] > 0 means drawn to the player's left.
        if (hand < 0) hand = rel[1] > 0.f ? 0 : 1;

        Rigid dv, dvPrev, dw;
        if (torso)
        {   // the torso follows the HMD body frame: the same rigid delta for every one of its draws
            dw = bodyW; dv = Mul(vw, Mul(bodyW, vinv)); dvPrev = dv;
        }
        else
        {
            // No anchoring here any more: it is done once per frame in EnsureTransform, from the equipment entity's
            // own transform. Doing it from inside the draw hook meant the anchor could be taken before the code that
            // decides which draws are the weapon had finished, and from whichever draw happened to arrive first.
            {
                std::lock_guard lk(g_XfM);
                // still settling after a weapon change -- which only matters to a draw that rides the weapon's delta
                if (!carryDraw && !g_RestModelValid[hand] && !(ai && ai->side >= 0 && (ai->full || useOwnDv))) { g_NotWeapon++; return false; }
            }
            if (ai && ai->full)
            {   // Full arm: the draw's own matrix stays exactly as the game made it (the model origin is the shoulder
                // root, which does not move); everything happens per bone in the palette, in BeginSixDof.
                memset(out, 0, kInstMaxBytes); memcpy(out, in, bytes);
                return true;
            }
            dv = useOwnDv ? ownDv : dvH[hand]; dvPrev = useOwnDv ? ownDv : dvPrevH[hand];
            dw = useOwnDv ? Mul(vinv, Mul(ownDv, vw)) : dwH[hand];
        }
        memset(out, 0, kInstMaxBytes);            // our buffer is the largest supported size; the tail must be defined
        memcpy(out, in, bytes);
        Rebuild(f, Mul(dv, f.pose), out + L->x, out + L->y, out + L->z, out + L->w);
        if (carryDraw)
        {   // the carried item takes the hand's depth transform (see g_ArmDh)
            float dh[4] = {};
            const bool var = L->dh >= 0 && within(L->dh);
            const bool got = var && ArmDepthHackFor(f.p, dh, L->dhn);
            if (got) { memcpy(out + L->dh, dh, sizeof(float) * L->dhn); g_CarryDhSet++; }
            else if (var) g_CarryDhNone++; else g_CarryDhNoVar++;
            if (g_CarryDhLog.load() > 0 && g_CarryDhLog.fetch_sub(1) > 0)
            {
                const float* o = var ? in + L->dh : dh;
                if (got) LOGI("carry depth: draw n={} cb{} depth hack ({:.4f},{:.4f},{:.4f},{:.4f}) -> the arms' ({:.4f},{:.4f},{:.4f},{:.4f}) [{} floats]",
                              draw, bytes, o[0], L->dhn > 1 ? o[1] : 0.f, L->dhn > 2 ? o[2] : 0.f, L->dhn > 3 ? o[3] : 0.f, dh[0], dh[1], dh[2], dh[3], L->dhn);
                else if (var) LOGI("carry depth: draw n={} cb{} depth hack ({:.4f}) left as it is: no arm draw seen with this projection (p00 {:.3f} p11 {:.3f})", draw, bytes, o[0], f.p[0][0], f.p[1][1]);
                else LOGI("carry depth: draw n={} cb{} has no cb_modeldepthhack in its shader", draw, bytes);
            }
        }
        if (!torso && !(ai && ai->side >= 0) && targetValid[hand] && g_PartsLeft.load() > 0 && g_PartsLeft.fetch_sub(1) > 0)
        {   // where this weapon draw is DRAWN, in the frame of the pose the weapon is drawn at: the body of the weapon sits
            // at its origin, a model bound to one of its joints at that joint's offset -- anything else is drawn wrong
            const Rigid drawnW = Mul(vinv, Mul(dv, f.pose));
            const Rigid loc = Mul(Inv(targetH[hand]), drawnW);
            const float tr = loc.r[0] + loc.r[4] + loc.r[8];
            const float ang = acosf(std::fmax(-1.f, std::fmin(1.f, (tr - 1.f) * 0.5f))) * 57.2958f;
            float d = 0.f; const int an = NearestAnchor(anch, worldOrg, &d, nullptr);
            LOGI("gunparts: hand {} draw n={} cb{} | engine origin ({:.3f},{:.3f},{:.3f}) {:.3f} m from anchor {} | drawn in the weapon's frame t ({:.4f},{:.4f},{:.4f}) rot {:.2f} deg",
                 hand, draw, bytes, worldOrg[0], worldOrg[1], worldOrg[2], d, an, loc.t[0], loc.t[1], loc.t[2], ang);
        }
        if (usePrev)
        {   // motion vectors: the same treatment with the previous frame's delta, or the weapon smears under TAA
            Factored p = Factor(in + L->px, in + L->py, in + L->pz, in + L->pw);
            if (p.ok) Rebuild(p, Mul(dvPrev, p.pose), out + L->px, out + L->py, out + L->pz, out + L->pw);
        }
        if (useModel)
        {   // world position and normals for lighting: rows of (delta * model->world)
            const int idx[3] = { L->mx, L->my, L->mz };
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 4; j++)
                    out[idx[i] + j] = dw.r[i * 3] * in[idx[0] + j] + dw.r[i * 3 + 1] * in[idx[1] + j]
                                    + dw.r[i * 3 + 2] * in[idx[2] + j] + (j == 3 ? dw.t[i] : 0.f);
        }
        return true;
    }

    // ---- watching the writes to PerInstanceCB ----
    // cb2 is filled just before each draw, so a snapshot taken at Present is the wrong object's data, and a deferred
    // context cannot Map for reading. So catch the contents on the way in: Map/Unmap and UpdateSubresource are
    // hooked and every constant buffer of a known layout size is cached. Contexts record from several threads, so
    // the cache is thread_local -- no lock, and no chance of reading another thread's in-flight instance.
    constexpr int kShadowSlots = 64;
    struct ShadowEntry { ID3D11Buffer* buf = nullptr; UINT bytes = 0; float f[kInstMaxFloats]; };
    thread_local ShadowEntry t_Shadow[kShadowSlots];
    constexpr int kMapSlots = 8;
    thread_local ID3D11Resource* t_MapRes[kMapSlots] = {};
    thread_local void* t_MapPtr[kMapSlots] = {};
    thread_local UINT t_MapBytes[kMapSlots] = {};
    int SlotOf(const void* p) { return (int)((((uintptr_t)p) >> 5) & (kShadowSlots - 1)); }
    thread_local bool t_InSwap = false;      // guards our own writes out of the shadow cache

    //! Size of `r` when it is a constant buffer that could be a PerInstanceCB, else 0. The bound size no longer
    //! selects the layout (the shader does), so this only bounds how much is worth remembering.
    UINT InstanceBufferBytes(ID3D11Resource* r)
    {
        D3D11_RESOURCE_DIMENSION dim{};
        r->GetType(&dim);
        if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return 0;
        D3D11_BUFFER_DESC bd{};
        static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);            // safe: D3D11 objects are single-inheritance
        if (!(bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return 0;
        return (bd.ByteWidth >= kInstMinBytes && bd.ByteWidth <= kInstMaxBytes) ? bd.ByteWidth : 0;
    }
    void StoreShadow(ID3D11Resource* r, const void* data, UINT bytes)
    {
        ShadowEntry& e = t_Shadow[SlotOf(r)];
        e.buf = static_cast<ID3D11Buffer*>(r); e.bytes = bytes;
        memcpy(e.f, data, bytes);
    }
    const ShadowEntry* FindShadow(ID3D11Buffer* b, UINT bytes)
    {
        const ShadowEntry& e = t_Shadow[SlotOf(b)];
        return (e.buf == b && e.bytes == bytes) ? &e : nullptr;
    }
    void RememberMap(ID3D11Resource* r, void* p)
    {
        UINT bytes = InstanceBufferBytes(r);
        if (!bytes) return;
        for (int i = 0; i < kMapSlots; i++)
            if (!t_MapRes[i] || t_MapRes[i] == r) { t_MapRes[i] = r; t_MapPtr[i] = p; t_MapBytes[i] = bytes; return; }
        // Full: a slot whose Unmap never came through here (see Hooks::Unmap). Recycle one rather than stop
        // shadowing on this thread for good -- that would leave every later cb2 unknown and the 6DOF dead.
        static thread_local int next = 0; const int i = next++ % kMapSlots;
        t_MapRes[i] = r; t_MapPtr[i] = p; t_MapBytes[i] = bytes;
    }
    bool MapPending() { for (int i = 0; i < kMapSlots; i++) if (t_MapRes[i]) return true; return false; }
    void CaptureMap(ID3D11Resource* r)
    {
        for (int i = 0; i < kMapSlots; i++)
            if (t_MapRes[i] == r)
            {   // read before the real Unmap: after it the driver may have taken the memory back
                StoreShadow(r, t_MapPtr[i], t_MapBytes[i]);
                t_MapRes[i] = nullptr; t_MapPtr[i] = nullptr;
                return;
            }
    }

    // ---- the joint palettes ----
    // A skinned draw reads 72 bones x 3 float4 (3456 bytes) through a shader resource. To draw only the HAND of an
    // arm model the forearm's bones have to be collapsed, which means handing the draw a modified copy: shadow the
    // game's writes (same Map/Unmap/UpdateSubresource path as cb2, but global -- the palette is written once per
    // frame per model, not per draw, possibly on another thread than the one that draws with it).
    constexpr int kPalMax = 16;
    struct PalShadow { ID3D11Buffer* buf = nullptr; UINT bytes = 0; unsigned long long frame = 0; float f[kPalMaxFloats]; };
    std::mutex g_PalM;
    PalShadow g_Pal[kPalMax];
    std::atomic<unsigned long long> g_PalWrites{ 0 }, g_PalSwaps{ 0 }, g_PalMiss{ 0 }, g_PalIgnored{ 0 };
    // Only the buffers behind the ARM draws' palette views are shadowed. The game writes hundreds of small
    // shader-resource buffers a frame, and with them all competing for the slots the arm palettes were evicted
    // before their draws came (measured: 7000 misses to 3 swaps).
    // It is an LRU, not an append-only list. It used to fill up and stay full: every load builds the arms new
    // palettes (4 more pointers each time), so on the fourth reload of a session the 16 slots held only dead buffers,
    // the new palettes were never shadowed (palmiss climbing by thousands a second) and both arms stopped following
    // the controllers for the rest of the session -- measured on Corvo after a death and "last checkpoint".
    constexpr int kPalWantedMax = 16;
    void* volatile g_PalWanted[kPalWantedMax] = {}; std::atomic<int> g_PalWantedCount{ 0 };
    unsigned long long g_PalWantedSeen[kPalWantedMax] = {};   // last Present frame an arm draw bound it
    std::atomic<unsigned> g_PalWantedEvicted{ 0 };
    bool PaletteWanted(ID3D11Resource* r)
    {
        const int n = g_PalWantedCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; i++) if (g_PalWanted[i] == r) return true;
        return false;
    }
    //! Called for every palette an arm draw binds: refreshes it, or takes a slot for it -- the one unused longest once
    //! the list is full, provided nothing drew with it in the last second (the arms bind these every frame).
    void WantPalette(ID3D11Resource* r)
    {
        const unsigned long long now = g_PresentFrame;
        {
            const int n = g_PalWantedCount.load(std::memory_order_acquire);
            for (int i = 0; i < n; i++) if (g_PalWanted[i] == r) { g_PalWantedSeen[i] = now; return; }   // fast path, no lock
        }
        std::lock_guard lk(g_PalM);
        const int n = g_PalWantedCount.load();
        for (int i = 0; i < n; i++) if (g_PalWanted[i] == r) { g_PalWantedSeen[i] = now; return; }
        if (n < kPalWantedMax) { g_PalWantedSeen[n] = now; g_PalWanted[n] = r; g_PalWantedCount.store(n + 1, std::memory_order_release); return; }
        int old = 0;
        for (int i = 1; i < kPalWantedMax; i++) if (g_PalWantedSeen[i] < g_PalWantedSeen[old]) old = i;
        if (now - g_PalWantedSeen[old] < 60) return;       // all in use: an arm cannot bind 16 palettes, so this is noise
        g_PalWantedSeen[old] = now; g_PalWanted[old] = r; g_PalWantedEvicted++;
    }
    constexpr int kPalMapSlots = 4;
    thread_local ID3D11Resource* t_PalMapRes[kPalMapSlots] = {};
    thread_local void* t_PalMapPtr[kPalMapSlots] = {};
    thread_local UINT t_PalMapBytes[kPalMapSlots] = {};

    UINT PaletteBytes(ID3D11Resource* r)
    {
        if (!PaletteWanted(r)) { return 0; }
        D3D11_RESOURCE_DIMENSION dim{};
        r->GetType(&dim);
        if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return 0;
        D3D11_BUFFER_DESC bd{};
        static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);
        if (!(bd.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return 0;
        return (bd.ByteWidth >= kPalMinBytes && bd.ByteWidth <= kPalMaxBytes) ? bd.ByteWidth : 0;
    }
    void StorePalette(ID3D11Resource* r, const void* data, UINT bytes)
    {
        std::lock_guard lk(g_PalM);
        int slot = -1, oldest = 0;
        for (int i = 0; i < kPalMax; i++)
        {
            if (g_Pal[i].buf == r) { slot = i; break; }
            if (!g_Pal[i].buf) { if (slot < 0) slot = i; }
            else if (g_Pal[i].frame < g_Pal[oldest].frame) oldest = i;
        }
        if (slot < 0) slot = oldest;
        g_Pal[slot].buf = static_cast<ID3D11Buffer*>(r); g_Pal[slot].bytes = bytes; g_Pal[slot].frame = g_PresentFrame;
        memcpy(g_Pal[slot].f, data, bytes);
        g_PalWrites++;
    }
    bool FindPalette(ID3D11Buffer* b, float* out, UINT* bytes)
    {
        std::lock_guard lk(g_PalM);
        for (int i = 0; i < kPalMax; i++)
            if (g_Pal[i].buf == b) { memcpy(out, g_Pal[i].f, g_Pal[i].bytes); *bytes = g_Pal[i].bytes; return true; }
        return false;
    }
    void RememberPalMap(ID3D11Resource* r, void* p)
    {
        UINT bytes = PaletteBytes(r);
        if (!bytes) return;
        for (int i = 0; i < kPalMapSlots; i++)
            if (!t_PalMapRes[i] || t_PalMapRes[i] == r) { t_PalMapRes[i] = r; t_PalMapPtr[i] = p; t_PalMapBytes[i] = bytes; return; }
        static thread_local int next = 0; const int i = next++ % kPalMapSlots;   // full: recycle (see RememberMap)
        t_PalMapRes[i] = r; t_PalMapPtr[i] = p; t_PalMapBytes[i] = bytes;
    }
    bool PalMapPending() { for (int i = 0; i < kPalMapSlots; i++) if (t_PalMapRes[i]) return true; return false; }
    void CapturePalMap(ID3D11Resource* r)
    {
        for (int i = 0; i < kPalMapSlots; i++)
            if (t_PalMapRes[i] == r)
            {
                StorePalette(r, t_PalMapPtr[i], t_PalMapBytes[i]);
                t_PalMapRes[i] = nullptr; t_PalMapPtr[i] = nullptr;
                return;
            }
    }
    // Replacement palettes: DEFAULT buffers with a float4 view, big enough for any palette, rotated like the cb pool.
    constexpr int kPalPoolSize = 256;
    struct PalBuf { ID3D11Buffer* b = nullptr; ID3D11ShaderResourceView* v = nullptr; };
    PalBuf g_PalPool[kPalPoolSize];
    std::atomic<unsigned> g_PalPoolNext{ 0 };
    std::atomic<bool> g_PalPoolReady{ false };
    // Which bones of each arm are NOT the hand and get collapsed to the hand socket. Research knob until the
    // identification fills it in.
    std::mutex g_BoneM;
    std::bitset<256> g_Collapse[2];
    std::atomic<int> g_HandCollapse{ 1 };
    // Which bones ARE the hand: the ones that carry the hand socket rigidly. For bone i, C_i = M_i^-1 * socket is
    // where the socket sits in that bone's own frame; over frames it is constant for the hand and its fingers and
    // wanders for the forearm and everything above it, by however much the wrist moves. Welford per bone.
    struct BoneStat { double n = 0; double mean[3] = {}; double m2 = 0; };
    BoneStat g_BoneStat[2][256]; unsigned long long g_BoneStatFrame[2] = {}; int g_BoneCount[2] = {};
    void AccumulateBones(int side, const float* pal, UINT bytes, const float socket[3])
    {
        std::lock_guard lk(g_BoneM);
        if (g_BoneStatFrame[side] == g_PresentFrame) return;       // one sample per frame per arm
        g_BoneStatFrame[side] = g_PresentFrame;
        const int bones = (int)(bytes / 48); g_BoneCount[side] = bones;
        for (int b = 0; b < bones && b < 256; b++)
        {
            const float* m = pal + b * 12;
            // rows (r0 t0)(r1 t1)(r2 t2): C = R^T (socket - t)
            const float d[3] = { socket[0] - m[3], socket[1] - m[7], socket[2] - m[11] };
            const float c[3] = { m[0] * d[0] + m[4] * d[1] + m[8] * d[2],
                                 m[1] * d[0] + m[5] * d[1] + m[9] * d[2],
                                 m[2] * d[0] + m[6] * d[1] + m[10] * d[2] };
            if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2])) continue;
            BoneStat& st = g_BoneStat[side][b];
            st.n += 1;
            double dd = 0;
            for (int k = 0; k < 3; k++) { const double delta = c[k] - st.mean[k]; st.mean[k] += delta / st.n; dd += delta * (c[k] - st.mean[k]); }
            st.m2 += dd;
        }
    }

    void CreatePalPool()
    {
        if (g_PalPoolReady.load() || !g_Device) return;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kPalMaxBytes; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        vd.Buffer.FirstElement = 0; vd.Buffer.NumElements = kPalMaxBytes / 16;
        for (int k = 0; k < kPalPoolSize; k++)
        {
            if (FAILED(g_Device->CreateBuffer(&bd, nullptr, &g_PalPool[k].b))) { LOGI("gpuhands: palette pool creation failed"); return; }
            if (FAILED(g_Device->CreateShaderResourceView(g_PalPool[k].b, &vd, &g_PalPool[k].v))) { LOGI("gpuhands: palette view creation failed"); return; }
        }
        g_PalPoolReady = true;
        LOGI("gpuhands: {} replacement palettes of {} bytes ready", kPalPoolSize, kPalMaxBytes);
    }
    const PalBuf& PalPoolTake() { return g_PalPool[g_PalPoolNext++ % kPalPoolSize]; }

    // ---- replacement buffers ----
    // Created at Present and handed out round-robin. Never created inside a draw hook: doing that mid-frame from
    // several threads was one of the three causes of the driver crash. DEFAULT + UpdateSubresource rather than
    // DYNAMIC + Map(WRITE_DISCARD), which is the renaming hazard that killed the first design.
    // One size for all of them: a constant buffer LARGER than the shader declares is legal (it reads only what it
    // declared), while a smaller one is the out-of-bounds read that has to be avoided. So the pool is kInstMaxBytes
    // wide and the tail is zeroed.
    constexpr int kPoolSize = 192;
    ID3D11Buffer* g_Pool[kPoolSize] = {};
    std::atomic<unsigned> g_PoolNext{ 0 };
    std::atomic<bool> g_PoolReady{ false };

    void CreatePools()
    {
        if (g_PoolReady.load() || !g_Device) return;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kInstMaxBytes;
        bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        for (int k = 0; k < kPoolSize; k++)
            if (FAILED(g_Device->CreateBuffer(&bd, nullptr, &g_Pool[k]))) { LOGI("gpu6dof: pool creation failed"); return; }
        g_PoolReady = true;
        LOGI("gpu6dof: {} replacement buffers of {} bytes ready", kPoolSize, kInstMaxBytes);
    }
    ID3D11Buffer* PoolTake() { return g_Pool[g_PoolNext++ % kPoolSize]; }

    //! Research: factor every draw in a frame and collect the ones drawn within `gpufind`'s radius of the eye.
    //! Reading that list off the log is how the weapon gets identified, with no dependence on a save or a weapon.
    // Histogram of what is actually bound at cb2, and how much of it we can read. A pass whose size is missing from
    // the layout table, or whose contents we never saw written, is a pass that silently will not move -- and a
    // weapon whose depth pass stays behind gets clipped against its own old depth.
    std::map<UINT, std::pair<int, int>> g_CbSizes;      // bytes -> (draws, of which shadowed)

    void ScanDraw(ID3D11DeviceContext* ctx, UINT n)
    {
        EnsureTransform();                               // gpufind must work with the feature off, so seed the view here
        ID3D11Buffer* cur = nullptr;
        ctx->VSGetConstantBuffers(2, 1, &cur);
        if (!cur) return;
        D3D11_BUFFER_DESC bd{}; cur->GetDesc(&bd);
        const ShadowEntry* e = FindShadow(cur, bd.ByteWidth);
        cur->Release();
        ID3D11VertexShader* vs = nullptr; ctx->VSGetShader(&vs, nullptr, nullptr);
        const InstLayout* L = vs ? LayoutFor(vs) : nullptr;
        if (vs) vs->Release();
        if (g_FindLeft.load() > 0)
        { std::lock_guard lk(g_FindM); auto& s = g_CbSizes[bd.ByteWidth]; s.first++; if (e && L && L->ok) s.second++; }
        if (!e || !L || !L->ok) return;
        Factored f = Factor(e->f + L->x, e->f + L->y, e->f + L->z, e->f + L->w);
        // No depth test here: the first-person ARM models have their origin at the character root, which sits
        // below and a little behind the eye, so a "must be in front of the camera" filter hid them from every scan.
        if (!f.ok) return;
        Rigid vinv; float cam[3]; bool haveCam; BodyHide::Anchor anch[BodyHide::aCount];
        {
            std::lock_guard lk(g_XfM);
            if (!g_ViewValid) return;
            vinv = g_ViewInv; haveCam = g_GameCamValid; memcpy(cam, g_GameCam, 12); memcpy(anch, g_Anchor, sizeof anch);
        }
        if (!haveCam) return;
        float world[3], d2 = 0.f;                        // measured from the game camera, like the runtime filter
        for (int i = 0; i < 3; i++)
        {
            world[i] = vinv.r[i * 3] * f.pose.t[0] + vinv.r[i * 3 + 1] * f.pose.t[1]
                     + vinv.r[i * 3 + 2] * f.pose.t[2] + vinv.t[i];
            const float dd = world[i] - cam[i];
            d2 += dd * dd;
        }
        const float d = sqrtf(d2);
        if (d > g_FindDist.load()) return;
        if (g_CollectLeft.load() > 0)
        {
            std::lock_guard lk(g_WeaponM);
            g_IdSet.insert(n);
            g_IdPose[n] = { f.pose.t[0], f.pose.t[1], f.pose.t[2] };
        }
        if (g_FindLeft.load() <= 0) return;
        std::lock_guard lk(g_FindM);
        FindHit& h = g_Find[n];
        if (!h.hits || d < h.dist)
        {
            h.dist = d; h.bytes = bd.ByteWidth; memcpy(h.t, f.pose.t, 12); memcpy(h.w, world, 12);
            h.anchor = NearestAnchor(anch, world, nullptr, nullptr);
            for (int i = 0; i < BodyHide::aCount; i++)
            {
                if (!anch[i].valid) { h.ad[i] = -1.f; continue; }
                const float dx = world[0] - anch[i].org[0], dy = world[1] - anch[i].org[1], dz = world[2] - anch[i].org[2];
                h.ad[i] = sqrtf(dx * dx + dy * dy + dz * dz);
            }
            h.sx = 0.5f + 0.5f * (f.p[0][0] * f.pose.t[0] + f.p[0][2] * f.pose.t[2]) / f.pose.t[2];
            h.sy = 0.5f - 0.5f * (f.p[1][1] * f.pose.t[1] + f.p[1][2] * f.pose.t[2]) / f.pose.t[2];
            ID3D11ShaderResourceView* sr[3] = {};
            ctx->VSGetShaderResources(0, 3, sr);
            for (int i = 0; i < 3; i++) { h.srv[i] = sr[i]; if (sr[i]) sr[i]->Release(); }
        }
        h.hits++;
    }

    //! Identify the weapon's draws automatically: scan, hide the weapons with BodyHide, scan again, and keep what
    //! disappeared. Runs as a few-frame sequence from Present because BodyHide only takes effect on the game thread.
    //! Costs a brief flicker of the weapon, once, and unlike a list of index counts it re-runs when the weapon changes.
    void TickIdentify()
    {
        if (g_IdPhase.load() == idIdle) return;
        if (g_CollectLeft.load() > 0) { g_CollectLeft--; return; }
        if (--g_IdWait > 0) return;
        switch (g_IdPhase.load())
        {
        case idScanA:
            { std::lock_guard lk(g_WeaponM); g_IdSetA = g_IdSet; g_IdSet.clear(); }
            BodyHide::SetHideWeapon(0, true); BodyHide::SetHideWeapon(1, true);
            g_IdWait = 15;                                   // let the game thread apply the visibility masks
            g_IdPhase = idHide;
            break;
        case idHide:
            g_CollectLeft = 10; g_IdWait = 2;
            g_IdPhase = idScanB;
            break;
        case idScanB:
        {
            BodyHide::SetHideWeapon(0, false); BodyHide::SetHideWeapon(1, false);
            std::lock_guard lk(g_WeaponM);
            g_WeaponIdx.clear();
            // Draws that vanished when the weapon was hidden. BodyHide covers only part of the weapon models, so
            // these are markers, not the answer.
            std::vector<std::array<float, 3>> marks;
            int vanished = 0;
            for (UINT i : g_IdSetA)
                if (!g_IdSet.count(i))
                {
                    vanished++;
                    auto it = g_IdPose.find(i);
                    if (it != g_IdPose.end()) marks.push_back(it->second);
                }
            // Everything drawn with the same model matrix belongs to the same model: take all of its draws, or the
            // weapon comes apart with half of it moving and half staying behind.
            for (UINT i : g_IdSetA)
            {
                auto it = g_IdPose.find(i);
                if (it == g_IdPose.end()) continue;
                for (const auto& m : marks)
                {
                    const float dx = it->second[0] - m[0], dy = it->second[1] - m[1], dz = it->second[2] - m[2];
                    if (dx * dx + dy * dy + dz * dz < 0.06f * 0.06f) { g_WeaponIdx.insert(i); break; }
                }
            }
            std::string list;
            for (UINT i : g_WeaponIdx) list += " " + std::to_string(i);
            LOGI("gpuweapon: {} of {} near-eye draws are the weapon ({} vanished with BodyHide, grown to whole models):{}",
                 g_WeaponIdx.size(), g_IdSetA.size(), vanished, list);
            // A weapon is drawn by several passes; one or two means the scan caught a bad moment (a menu, a
            // transition) and moving that set would tear the weapon in half. Say so rather than doing it.
            if (!vanished || g_WeaponIdx.size() < 3)
                LOGI("gpuweapon: WARNING - that looks wrong ({} near-eye draws seen). Re-run gpuweapon while in "
                     "gameplay with the weapon in view, or the weapon will come apart.", g_IdSetA.size());
            g_IdSet.clear(); g_IdSetA.clear(); g_IdPose.clear();
            g_IdPhase = idIdle;
            break;
        }
        default: g_IdPhase = idIdle; break;
        }
    }

    void FlushFind()
    {
        if (g_FindLeft.load() <= 0) return;
        if (g_FindLeft.fetch_sub(1) > 1) return;                 // keep collecting across the requested frames
        std::lock_guard lk(g_FindM);
        LOGI("gpufind: {} draw signatures within {} m of the eye", g_Find.size(), g_FindDist.load());
        {   // where the engine says the first-person entities are, so the per-draw distances below can be read
            BodyHide::Anchor a[BodyHide::aCount]; BodyHide::GetAnchors(a);
            for (int i = 0; i < BodyHide::aCount; i++)
                LOGI("gpufind: anchor {} valid={} org=({:.3f},{:.3f},{:.3f})",
                     BodyHide::AnchorName(i), a[i].valid ? 1 : 0, a[i].org[0], a[i].org[1], a[i].org[2]);
        }
        for (auto& e : g_Find)
            LOGI("gpufind: idx={} cb{} n={} dist={:.3f} t=(r {:.3f}, u {:.3f}, f {:.3f}) screen=({:.3f},{:.3f}) "
                 "world=({:.3f},{:.3f},{:.3f}) near={} d(wL {:.3f} wR {:.3f} aL {:.3f} aR {:.3f} pl {:.3f}) srv={} {} {}",
                 e.first, e.second.bytes, e.second.hits, e.second.dist,
                 e.second.t[0], e.second.t[1], e.second.t[2], e.second.sx, e.second.sy,
                 e.second.w[0], e.second.w[1], e.second.w[2], BodyHide::AnchorName(e.second.anchor),
                 e.second.ad[0], e.second.ad[1], e.second.ad[2], e.second.ad[3], e.second.ad[4],
                 e.second.srv[0], e.second.srv[1], e.second.srv[2]);
        for (auto& s : g_CbSizes)
            LOGI("gpufind: cb2 size {} on {} draws, {} of them transformable", s.first, s.second.first, s.second.second);
        LOGI("gpufind: shaders reflected {} (no bytecode {}, no mvp {})",
             g_LayoutByVs.size(), g_NoBytecode.load(), g_NoMvp.load());
        LOGI("gpufind: end");
        g_Find.clear(); g_CbSizes.clear();
    }

    // ================= the full arm: rig, joints, two-bone IK, per-bone deltas =================
    Rigid BoneRigid(const float* m)             // palette rows (r0 t0)(r1 t1)(r2 t2) -> Rigid
    {
        Rigid o;
        for (int r = 0; r < 3; r++) { o.r[r * 3] = m[r * 4]; o.r[r * 3 + 1] = m[r * 4 + 1]; o.r[r * 3 + 2] = m[r * 4 + 2]; o.t[r] = m[r * 4 + 3]; }
        return o;
    }
    void BoneWrite(float* m, const Rigid& a)
    {
        for (int r = 0; r < 3; r++) { m[r * 4] = a.r[r * 3]; m[r * 4 + 1] = a.r[r * 3 + 1]; m[r * 4 + 2] = a.r[r * 3 + 2]; m[r * 4 + 3] = a.t[r]; }
    }
    void Xform(const Rigid& a, const float p[3], float o[3]) { for (int i = 0; i < 3; i++) o[i] = a.r[i * 3] * p[0] + a.r[i * 3 + 1] * p[1] + a.r[i * 3 + 2] * p[2] + a.t[i]; }
    void Rotate(const Rigid& a, const float p[3], float o[3]) { for (int i = 0; i < 3; i++) o[i] = a.r[i * 3] * p[0] + a.r[i * 3 + 1] * p[1] + a.r[i * 3 + 2] * p[2]; }
    float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    void Cross3(const float a[3], const float b[3], float o[3]) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
    float Norm3(float v[3]) { const float n = sqrtf(Dot3(v, v)); if (n > 1e-9f) { v[0] /= n; v[1] /= n; v[2] /= n; } return n; }
    void Sub3(const float a[3], const float b[3], float o[3]) { for (int i = 0; i < 3; i++) o[i] = a[i] - b[i]; }
    //! Rotation about a unit axis (Rodrigues), no translation.
    Rigid AxisAngle(const float k[3], float ang)
    {
        Rigid o; const float c = cosf(ang), s = sinf(ang), v = 1.f - c;
        o.r[0] = c + k[0] * k[0] * v;        o.r[1] = k[0] * k[1] * v - k[2] * s; o.r[2] = k[0] * k[2] * v + k[1] * s;
        o.r[3] = k[1] * k[0] * v + k[2] * s; o.r[4] = c + k[1] * k[1] * v;        o.r[5] = k[1] * k[2] * v - k[0] * s;
        o.r[6] = k[2] * k[0] * v - k[1] * s; o.r[7] = k[2] * k[1] * v + k[0] * s; o.r[8] = c + k[2] * k[2] * v;
        return o;
    }
    //! A unit vector perpendicular to a (unit): deterministic, continuous except near the vertical.
    void AnyPerp(const float a[3], float o[3])
    {
        const float up[3] = { 0, 0, 1 }, fwd[3] = { 1, 0, 0 };
        Cross3(a, fabsf(a[2]) < 0.9f ? up : fwd, o); Norm3(o);
    }
    //! Orthonormal frame with x along a and y towards r; F holds the basis vectors as columns.
    void MakeFrame(const float a[3], const float r[3], float F[9])
    {
        float x[3] = { a[0], a[1], a[2] }; Norm3(x);
        const float rx = Dot3(r, x);
        float y[3] = { r[0] - x[0] * rx, r[1] - x[1] * rx, r[2] - x[2] * rx };
        if (Norm3(y) < 1e-4f) AnyPerp(x, y);
        float z[3]; Cross3(x, y, z);
        for (int i = 0; i < 3; i++) { F[i * 3] = x[i]; F[i * 3 + 1] = y[i]; F[i * 3 + 2] = z[i]; }
    }
    //! The rotation taking frame (a0, r0) onto frame (a1, r1): R = F1 * F0^T. No translation.
    Rigid FrameRot(const float a0[3], const float r0[3], const float a1[3], const float r1[3])
    {
        float F0[9], F1[9]; MakeFrame(a0, r0, F0); MakeFrame(a1, r1, F1);
        Rigid o;
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
            o.r[i * 3 + j] = F1[i * 3] * F0[j * 3] + F1[i * 3 + 1] * F0[j * 3 + 1] + F1[i * 3 + 2] * F0[j * 3 + 2];
        return o;
    }

    //! One frame of evidence for the joint between bones p (null = the model frame itself) and c:
    //! (R_p - R_c) J = t_c - t_p. Frames where the joint did not bend carry no information and are skipped.
    void JointAccumulate(JointEst& je, const Rigid* Mp, const Rigid& Mc, const ArmRig& rig)
    {
        float R[9], t[3];
        for (int i = 0; i < 9; i++) R[i] = (Mp ? Mp->r[i] : ((i % 4) == 0 ? 1.f : 0.f)) - Mc.r[i];
        for (int i = 0; i < 3; i++) t[i] = Mc.t[i] - (Mp ? Mp->t[i] : 0.f);
        float mag = 0.f; for (int i = 0; i < 9; i++) mag += R[i] * R[i];
        if (!(mag > 1e-6f) || !std::isfinite(mag) || !std::isfinite(t[0] + t[1] + t[2])) return;
        // R J = t: normal equations A J = b with A = R^T R, b = R^T t
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 3; j++) je.A[i * 3 + j] += (double)R[i] * R[j] + (double)R[3 + i] * R[3 + j] + (double)R[6 + i] * R[6 + j];
            je.b[i] += (double)R[i] * t[0] + (double)R[3 + i] * t[1] + (double)R[6 + i] * t[2];
        }
        // on the line J = tip + axis * s: (R axis) s = t - R tip
        float u[3], v[3];
        for (int i = 0; i < 3; i++)
        {
            u[i] = R[i * 3] * rig.axis[0] + R[i * 3 + 1] * rig.axis[1] + R[i * 3 + 2] * rig.axis[2];
            v[i] = t[i] - (R[i * 3] * rig.tip[0] + R[i * 3 + 1] * rig.tip[1] + R[i * 3 + 2] * rig.tip[2]);
        }
        je.uu += Dot3(u, u); je.uv += Dot3(u, v); je.n++;
    }
    //! Solve: the 1-D fit along the bind axis gives the line prior, the 3-D normal equations refine it where the
    //! data has information (a bend pins the joint in the plane of the bend, the prior holds it along the hinge),
    //! and anatomy bounds how far from the prior it may wander.
    //! `reach` bounds how far along the arm the joint may move from its prior: 12 cm for the elbow and the shoulder,
    //! 5 cm for the wrist. Every arm that works learned its wrist within 1 cm of the prior (0.22-0.23 m); on Corvo's
    //! empty left hand the animation pushed it to the end of a 12 cm bound (0.34 m), leaving a 12 cm forearm.
    void JointSolve(JointEst& je, const ArmRig& rig, float reach = 0.12f)
    {
        const double lam = 0.2, mu = 0.2;
        double s = (lam * je.sPrior + je.uv) / (lam + je.uu);
        const double smin = je.sPrior - reach, smax = je.sPrior + reach;
        if (!(s >= smin)) s = smin;
        if (!(s <= smax)) s = smax;
        double jl[3]; for (int i = 0; i < 3; i++) jl[i] = rig.tip[i] + rig.axis[i] * s;
        double M[9], rhs[3];
        for (int i = 0; i < 9; i++) M[i] = je.A[i] + ((i % 4) == 0 ? mu : 0.0);
        for (int i = 0; i < 3; i++) rhs[i] = je.b[i] + mu * jl[i];
        auto det3 = [](const double m[9]) { return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]); };
        const double det = det3(M);
        double J[3] = { jl[0], jl[1], jl[2] };
        if (fabs(det) > 1e-12)
            for (int c = 0; c < 3; c++)
            {
                double m[9]; memcpy(m, M, sizeof m); m[c] = rhs[0]; m[3 + c] = rhs[1]; m[6 + c] = rhs[2];
                J[c] = det3(m) / det;
            }
        // bound the wander: along the axis and away from it
        float d[3]; for (int i = 0; i < 3; i++) d[i] = (float)(J[i] - jl[i]);
        if (!std::isfinite(d[0] + d[1] + d[2])) d[0] = d[1] = d[2] = 0.f;
        float along = Dot3(d, rig.axis); along = std::clamp(along, -0.10f, 0.10f);
        if (reach < 0.12f) { const float sl = (float)s + along; if (sl < je.sPrior - reach) along = je.sPrior - reach - (float)s; if (sl > je.sPrior + reach) along = je.sPrior + reach - (float)s; }
        float perp[3]; for (int i = 0; i < 3; i++) perp[i] = d[i] - rig.axis[i] * Dot3(d, rig.axis);
        const float pn = sqrtf(Dot3(perp, perp)); if (pn > 0.06f) for (int i = 0; i < 3; i++) perp[i] *= 0.06f / pn;
        for (int i = 0; i < 3; i++) je.j[i] = (float)jl[i] + rig.axis[i] * along + perp[i];
        const float dj[3] = { je.j[0] - rig.tip[0], je.j[1] - rig.tip[1], je.j[2] - rig.tip[2] };
        je.s = Dot3(dj, rig.axis);
    }
    void JointReset(JointEst& je, float sPrior, const ArmRig& rig)
    {
        je = JointEst(); je.sPrior = sPrior; je.s = sPrior;
        for (int i = 0; i < 3; i++) je.j[i] = rig.tip[i] + rig.axis[i] * sPrior;
    }
    //! The segment a bind-axis coordinate falls in.
    uint8_t SegOf(const ArmRig& rig, float s)
    {
        return s <= rig.wrist.s + 0.005f ? sgHand : s <= rig.elbow.s ? sgFore : s <= rig.shoulder.s + 0.05f ? sgUpper : sgRoot;
    }
    //! Rebuild this side's per-palette segment tables from every mesh seen on it. g_RigM held.
    void RebuildSegTables(int side)
    {
        ArmRig& rig = g_Rig[side]; if (!rig.valid) return;
        SegTable& T = g_Seg[side]; T = SegTable(); T.side = side;
        for (const MeshBones& mb : g_MeshList)
        {
            if (mb.side != side) continue;
            for (int b = 0; b < 256; b++)
            {
                if (mb.cw[b] <= 0.f) continue;
                const float d[3] = { mb.c[b][0] - rig.tip[0], mb.c[b][1] - rig.tip[1], mb.c[b][2] - rig.tip[2] };
                if (mb.cw[b] > T.w[b]) { T.w[b] = mb.cw[b]; T.s[b] = Dot3(d, rig.axis); memcpy(T.c[b], mb.c[b], 12); }   // the heaviest mesh decides where the bone is
                if (b + 1 > T.bones) T.bones = b + 1;
            }
        }
        rig.mainKeyBones = T.bones;
        const float sw = rig.wrist.s, se = rig.elbow.s;
        for (int i = 0; i < sgCount; i++) T.rep[i] = -1;
        for (int b = 0; b < T.bones; b++)
        {
            if (T.w[b] <= 0.f) { T.seg[b] = sgNone; continue; }
            uint8_t seg = SegOf(rig, T.s[b]);
            if (rig.autoSeg[b]) seg = rig.autoSeg[b];
            if (rig.manual[b]) seg = rig.manual[b] == 255 ? (uint8_t)sgNone : rig.manual[b];
            T.seg[b] = seg;
            T.twist[b] = se > sw + 0.01f ? std::clamp((se - T.s[b]) / (se - sw), 0.f, 1.f) : 1.f;
        }
        // The representative of a segment is the heaviest bone more than 3 cm short of its shoulder-side joint;
        // any bone of the segment only when none is. The heaviest outright went wrong on Corvo's left arm: bone 1 (a
        // shoulder bone, root on the right arm at 0.80 m) sat at 0.78 m, just inside the upper arm's margin, and
        // out-weighed the real upper arm bone through a small mesh skinned to it alone -- the elbow was then learned
        // between the clavicle and the forearm, drifted to 0.70 m, and the IK never produced a frame (L1 < 5 cm).
        const float hi[sgCount] = { 1e9f, sw, se, rig.shoulder.s, 1e9f };   // each segment's shoulder-side joint
        bool interior[sgCount] = {};
        for (int pass = 0; pass < 2; pass++)
        {
            for (int b = 0; b < T.bones; b++)
            {
                const uint8_t seg = T.seg[b];
                if (seg == sgNone || seg >= sgCount || T.w[b] <= 0.f) continue;
                // the margin is on the SHOULDER side only: the forearm's best bones hug the wrist (66/67 at 0.23-0.26
                // m, wrist 0.22) and excluding them made a twist bone by the elbow the forearm's rep (wrist -> 0.31 m)
                if (pass == 0 && seg != sgHand && !(T.s[b] < hi[seg] - 0.03f)) continue;
                // and the forearm's rep is one of its DISTAL bones: the wrist is learned between it and the palm, and a
                // twist bone by the elbow (Corvo's left: 64/65 carry the sleeve's weight, k 0.3-0.6) turns with the
                // pronation, which pushed the wrist to the end of its bound (0.34 m, prior 0.22, L2 0.12)
                if (pass == 0 && seg == sgFore && T.twist[b] < 0.8f) continue;
                if (pass == 1 && interior[seg]) continue;                  // the interior already gave one
                if (T.rep[seg] < 0 || T.w[b] > T.w[T.rep[seg]]) T.rep[seg] = b;
            }
            if (pass == 0) for (int i = 0; i < sgCount; i++) interior[i] = T.rep[i] >= 0;
        }
    }
    //! The rigidity statistic corrects the geometric guess for the bones near a joint: a bone whose transform
    //! relative to the forearm's representative never changes while the one relative to the upper arm's does is
    //! forearm, whatever its centroid said. Conservative thresholds, main palette only, and never the hand.
    void AutoSegment(int side, const SegTable& T)
    {
        ArmRig& rig = g_Rig[side]; bool changed = false;
        for (int b = 0; b < T.bones; b++)
        {
            if (T.w[b] <= 0.f || rig.manual[b]) continue;
            const uint8_t geo = SegOf(rig, T.s[b]);
            if (geo == sgHand) continue;
            const RelStat* st[3] = { &g_Rel[side][0][b], &g_Rel[side][1][b], &g_Rel[side][2][b] };
            if (st[0]->n < 300) continue;
            double sd[3]; for (int k = 0; k < 3; k++) sd[k] = sqrt(st[k]->m2 / st[k]->n) * 100.0;
            uint8_t want = 0;
            if (sd[0] < 0.3 && sd[1] > 1.5) want = sgFore;
            else if (sd[1] < 0.3 && sd[0] > 1.5) want = sgUpper;
            else if (sd[2] < 0.2 && sd[0] > 1.5 && sd[1] > 1.5) want = sgRoot;
            if (want && want != geo && rig.autoSeg[b] != want)
            {
                LOGI("gpubody: side {} bone {} moves with the {} (sd fore {:.2f} upper {:.2f} model {:.2f} cm), centroid said {}",
                     side, b, kSegName[want], sd[0], sd[1], sd[2], kSegName[geo]);
                rig.autoSeg[b] = want; changed = true;
            }
        }
        if (changed) RebuildSegTables(side);
    }
    //! The representatives this side's rig poses a palette from (upper arm, forearm, palm); false when it cannot.
    bool ArmReps(int side, UINT bones, const SegTable& T, int& u, int& f, int& p)
    {
        const ArmRig& rig = g_Rig[side];
        if (!rig.valid) return false;
        u = T.rep[sgUpper]; f = T.rep[sgFore];
        p = (rig.palm >= 0 && rig.palm < T.bones && T.seg[rig.palm] == sgHand) ? rig.palm : T.rep[sgHand];
        return !(u < 0 || f < 0 || p < 0 || (UINT)u >= bones || (UINT)f >= bones || (UINT)p >= bones);
    }
    //! What the animation teaches, once per frame from the main palette: the joints from the articulation, the
    //! rigidity of every bone against the representatives, and every half second the joint solve. g_RigM held.
    //! Learning only: nothing here poses a draw (SolveArm does, per draw).
    void LearnArm(int side, const float* pal, UINT bones, const SegTable& T)
    {
        static unsigned long long learned[2] = {};
        int u, f, p;
        if (learned[side] == g_PresentFrame || !ArmReps(side, bones, T, u, f, p)) return;
        learned[side] = g_PresentFrame;
        ArmRig& rig = g_Rig[side];
        const Rigid Mu = BoneRigid(pal + u * 12), Mf = BoneRigid(pal + f * 12), Mp = BoneRigid(pal + p * 12);
        JointAccumulate(rig.elbow, &Mu, Mf, rig); JointAccumulate(rig.wrist, &Mf, Mp, rig); JointAccumulate(rig.shoulder, nullptr, Mu, rig);
        {
            const Rigid iMf = Inv(Mf), iMu = Inv(Mu);
            for (UINT b = 0; b < bones && b < 256; b++)
            {
                const Rigid Mb = BoneRigid(pal + b * 12);
                const Rigid rel[3] = { Mul(iMf, Mb), Mul(iMu, Mb), Mb };
                for (int k = 0; k < 3; k++)
                {
                    RelStat& st = g_Rel[side][k][b]; const float* x = rel[k].t;
                    if (!std::isfinite(x[0] + x[1] + x[2])) continue;
                    st.n += 1; double dd = 0;
                    for (int i = 0; i < 3; i++) { const double d = x[i] - st.mean[i]; st.mean[i] += d / st.n; dd += d * (x[i] - st.mean[i]); }
                    st.m2 += dd;
                }
            }
        }
        // every half second: re-solve the joints (and re-cut the segments if a threshold moved), check the rigidity
        if ((g_PresentFrame % 30) == 0)
        {
            const float before[3] = { rig.wrist.s, rig.elbow.s, rig.shoulder.s };
            JointSolve(rig.wrist, rig, 0.05f); JointSolve(rig.elbow, rig); JointSolve(rig.shoulder, rig);
            if (fabsf(before[0] - rig.wrist.s) > 0.01f || fabsf(before[1] - rig.elbow.s) > 0.01f || fabsf(before[2] - rig.shoulder.s) > 0.01f)
            {
                LOGI("gpubody: side {} joints along the bind axis: wrist {:.3f} elbow {:.3f} shoulder {:.3f} m from the fingertip ({} {} {} bending frames)",
                     side, rig.wrist.s, rig.elbow.s, rig.shoulder.s, rig.wrist.n, rig.elbow.n, rig.shoulder.n);
                RebuildSegTables(side);        // rewrites T in place: the caller's reference sees the new table
            }
            else if ((g_PresentFrame % 600) == 0 && T.bones >= rig.mainKeyBones) AutoSegment(side, T);
        }
    }
    //! The deltas of ONE draw, from that draw's own palette and its own model matrix. g_RigM held.
    //!
    //! This used to be computed once per Present frame, from whichever draw of the main palette came first, and handed
    //! to every draw of that arm. But an arm is several meshes (the glove, the sleeves) with a palette each, drawn in
    //! several passes, recorded on worker threads across the Present boundary: a draw that came before the main one in
    //! a frame got the PREVIOUS frame's deltas, computed from an animation pose one frame older than the bones it was
    //! applied to. The hand was then off by however far the animation had moved it in one frame -- measured while
    //! sprinting with both hands empty (2026-10-04, `vr handjit`): 33-64 mm on every such draw, 0.00 mm on the main
    //! ones, i.e. the arm's meshes alternating between two poses. The swinging empty hand of the sprint is the worst
    //! case, which is what the user saw on the Quest 3. Now nothing is shared between draws except what is a constant
    //! of the skeleton (joints, segments): the hand goes where the target is whatever pose the animation is in.
    //!
    //! `m2r` takes the model into the reference space the targets are given in: world for a current palette (the
    //! targets EnsureTransform built), the draw's previous view for the motion-vector palette (see PrevFind).
    bool SolveArm(int side, const float* pal, UINT bones, const SegTable& T, const ArmInfo& ai,
                  const Rigid& m2r, const Rigid& palmT, const Rigid& bodyVr, const Rigid& bodyW, ArmFrame& F)
    {
        int u, f, p;
        if (!ArmReps(side, bones, T, u, f, p)) return false;
        const ArmRig& rig = g_Rig[side];
        F = ArmFrame(); F.frame = g_PresentFrame;
        const Rigid Mu = BoneRigid(pal + u * 12), Mf = BoneRigid(pal + f * 12), Mp = BoneRigid(pal + p * 12);
        // The hand: the delta that puts THIS palette's palm on the palm target, D * P = m2r^-1 * target. The same
        // bone and centroid the target was defined with (TransformInstanceCb), so the palm lands on it exactly.
        Rigid dh = ai.dh;
        if (ai.palmTgtValid && ai.palm >= 0 && (UINT)ai.palm < bones)
        {
            const Rigid Mpb = BoneRigid(pal + ai.palm * 12);
            Rigid P = Mpb; Xform(Mpb, ai.palmC, P.t);                    // palm frame in model space: R = M.R, t = M * centroid
            dh = Mul(Mul(Inv(m2r), palmT), Inv(P));
        }
        // the joints in the current pose
        float S[3], E[3], W[3]; Xform(Mu, rig.shoulder.j, S); Xform(Mf, rig.elbow.j, E); Xform(Mp, rig.wrist.j, W);
        float se[3], ew[3]; Sub3(E, S, se); Sub3(W, E, ew);
        const float L1 = sqrtf(Dot3(se, se)), L2 = sqrtf(Dot3(ew, ew));
        if (!(L1 > 0.05f) || !(L2 > 0.05f) || !std::isfinite(L1 + L2)) return false;
        float Wp[3]; Xform(dh, W, Wp);                                  // the wrist goes with the hand
        // the drawn arm towards the real arm's length (g_ArmLen), never shorter than the game's own
        float ks = 1.f;
        { const float want = g_ArmLen.load(); if (want > 0.f) ks = std::clamp(want / (L1 + L2), 1.f, std::max(1.f, g_ArmScaleMax.load())); }
        const float L1k = L1 * ks, L2k = L2 * ks;
        // the shoulder under the head, and the elbow hint, in model space
        const Rigid m2wInv = Inv(m2r);
        const float sgn = side == 0 ? 1.f : -1.f;
        const float offL[3] = { g_ShoulderOff[0].load(), sgn * g_ShoulderOff[1].load(), g_ShoulderOff[2].load() };
        float Sw[3], Sp[3]; Xform(bodyVr, offL, Sw); Xform(m2wInv, Sw, Sp);
        const float poleL[3] = { g_Pole[0].load(), sgn * g_Pole[1].load(), g_Pole[2].load() };
        float poleW[3], pole[3]; Rotate(bodyVr, poleL, poleW); Rotate(m2wInv, poleW, pole); Norm3(pole);
        // two-bone IK: the elbow on the circle where both segments reach, towards the hint
        float along[3]; Sub3(Wp, Sp, along); float d = Norm3(along);
        if (d < 1e-4f) { along[0] = -pole[0]; along[1] = -pole[1]; along[2] = -pole[2]; }
        const float maxr = 0.995f * (L1k + L2k), minr = fabsf(L1k - L2k) + 0.01f;
        bool stretched = false;
        if (d > maxr) { for (int i = 0; i < 3; i++) Sp[i] = Wp[i] - along[i] * maxr; d = maxr; stretched = true; }   // the shoulder follows the hand
        if (d < minr) { for (int i = 0; i < 3; i++) Sp[i] = Wp[i] - along[i] * minr; d = minr; }
        const float ca = std::clamp((L1k * L1k + d * d - L2k * L2k) / (2.f * L1k * d), -1.f, 1.f), sa = sqrtf(std::max(0.f, 1.f - ca * ca));
        float perp[3]; const float pa = Dot3(pole, along);
        for (int i = 0; i < 3; i++) perp[i] = pole[i] - along[i] * pa;
        if (Norm3(perp) < 1e-3f) AnyPerp(along, perp);
        float Ep[3]; for (int i = 0; i < 3; i++) Ep[i] = Sp[i] + along[i] * (L1k * ca) + perp[i] * (L1k * sa);
        // the deltas: upper arm S->S', E->E' (roll from the elbow plane); forearm E->E', W->W' with the roll the
        // elbow hinge inherits from the upper arm at the elbow end and the hand's own at the wrist end
        float sep[3], ewp[3]; Sub3(Ep, Sp, sep); Sub3(Wp, Ep, ewp);
        Rigid du = FrameRot(se, ew, sep, ewp);
        { float t[3]; Rotate(du, S, t); for (int i = 0; i < 3; i++) du.t[i] = Sp[i] - t[i]; }
        float ax[3] = { ewp[0], ewp[1], ewp[2] }; Norm3(ax);
        float q0[3]; { const float es[3] = { -se[0], -se[1], -se[2] }; Cross3(ew, es, q0); if (Norm3(q0) < 1e-4f) { float ewn[3] = { ew[0], ew[1], ew[2] }; Norm3(ewn); AnyPerp(ewn, q0); } }
        float re[3], rh[3]; Rotate(du, q0, re); Rotate(dh, q0, rh);
        for (float* v : { re, rh }) { const float k = Dot3(v, ax); for (int i = 0; i < 3; i++) v[i] -= ax[i] * k; if (Norm3(v) < 1e-4f) AnyPerp(ax, v); }
        Rigid rf0 = FrameRot(ew, q0, ewp, re);
        { float t[3]; Rotate(rf0, E, t); for (int i = 0; i < 3; i++) rf0.t[i] = Ep[i] - t[i]; }
        float cr[3]; Cross3(re, rh, cr);
        float twist = atan2f(Dot3(cr, ax), Dot3(re, rh)); if (!std::isfinite(twist)) twist = 0.f;
        F.dbody = ai.bodyValid ? Mul(m2wInv, Mul(bodyW, m2r)) : Rigid();
        F.dh = dh; F.du = du; F.rf0 = rf0; memcpy(F.ax, ax, 12); F.twist = twist;
        memcpy(F.E, E, 12); memcpy(F.Ep, Ep, 12); memcpy(F.S, S, 12); memcpy(F.Sp, Sp, 12); memcpy(F.W, W, 12); memcpy(F.Wp, Wp, 12);
        F.L1 = L1k; F.L2 = L2k; F.reach = d; F.stretched = stretched; F.k = ks;
        F.m2w = m2r; memcpy(F.head, bodyVr.t, 12); memcpy(F.ctrl, ai.ctrl, 12);
        F.srcSerial = ai.serial;
        F.valid = true;
        return true;
    }
    //! The delta bone `b` gets this frame: its segment's. False for a bone the rewrite leaves alone.
    //! p' = P + (I + (k-1) u u^T)(p - P): a stretch by k along the unit axis u through P (the lengthened arm, g_ArmLen).
    Rigid StretchAbout(const float P[3], const float uIn[3], float k)
    {
        Rigid S; float u[3] = { uIn[0], uIn[1], uIn[2] }; Norm3(u);
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) S.r[i * 3 + j] = (i == j ? 1.f : 0.f) + (k - 1.f) * u[i] * u[j];
        for (int i = 0; i < 3; i++) S.t[i] = P[i] - (S.r[i * 3] * P[0] + S.r[i * 3 + 1] * P[1] + S.r[i * 3 + 2] * P[2]);
        return S;
    }
    bool BoneDelta(const SegTable& T, const ArmFrame& F, UINT b, Rigid& D)
    {
        switch (T.seg[b])
        {
        case sgHand:  D = F.dh; return true;
        case sgUpper:
        {
            D = F.du;
            if (F.k != 1.f) { float u[3]; Sub3(F.Ep, F.Sp, u); D = Mul(StretchAbout(F.Sp, u, F.k), D); }
            return true;
        }
        case sgRoot:  D = F.dbody; return true;
        case sgFore:
        {
            D = Mul(AxisAngle(F.ax, T.twist[b] * F.twist), F.rf0);
            float t[3]; Rotate(D, F.E, t); for (int i = 0; i < 3; i++) D.t[i] = F.Ep[i] - t[i];
            if (F.k != 1.f) D = Mul(StretchAbout(F.Ep, F.ax, F.k), D);
            return true;
        }
        default: return false;
        }
    }
    //! Rewrite a palette in place: M_i' = D_seg(i) * M_i.
    void ApplyFull(float* pal, UINT bones, const SegTable& T, const ArmFrame& F)
    {
        const UINT n = std::min<UINT>(bones, (UINT)T.bones);
        for (UINT b = 0; b < n && b < 256; b++)
        {
            Rigid D;
            if (!BoneDelta(T, F, b, D)) continue;
            float* m = pal + b * 12;
            BoneWrite(m, Mul(D, BoneRigid(m)));
        }
    }

    // ---- the FIST while an object is held (GrabHand) ----
    // The rewrite moves every hand bone with the palm's delta, so the fingers are whatever the game animates -- an
    // open hand while holding (the game hides that arm and does not care). With a weapon in the hand the animation
    // closes it round the grip: each hand bone RELATIVE TO THE PALM, R_b = M_palm^-1 * M_b, is learned from those
    // frames (every 30 frames, never mid-Slash) and, while an object is held in that hand, the fingers are rebuilt as
    // M_b' = M_palm * R_b before the hand's delta goes on. M = W * B^-1, so R_b = B_palm (W_palm^-1 W_b) B_b^-1: the
    // finger pose relative to the palm, the bind matrices cancelling -- valid for any palette of the same skeleton.
    std::atomic<int> g_FistMode{ 1 };             // vr grabfist: 0 = the game's (open) hand, 1 = the weapon's fist
    struct FistPose { bool valid = false; UINT bones = 0; int palm = -1; unsigned long long frame = 0; std::bitset<256> has; Rigid rel[256]; };
    FistPose g_Fist[2];                           // under g_RigM
    std::atomic<unsigned long long> g_FistLearned[2], g_FistApplied[2], g_FistNoPose[2];

    // ---- the EMPTY hand: one pose of its own, not the locomotion's ----
    // With nothing equipped the fingers were the flat game's walk cycle: standing both hands open, walking the left
    // one a fist, sprinting both -- in VR, where the hand is wherever the controller is, the fingers changed with the
    // legs. The empty hand now keeps ONE pose, built like the fist above (each hand bone relative to the palm):
    // the game's own OPEN hand, learned once standing still (the player not moving for kStillMs), blended towards a closed
    // fist by `curl` (the rest pose) and by the controller's grip (squeeze to close). The fist is the game's own
    // sprinting fist when it has been seen, else the weapon's grip above. Until the open hand has been seen the
    // fingers are left as the game animates them. Learned once per skeleton: re-learning every few frames would
    // replay the idle animation's finger fidget as jumps. An empty hand = no equipment entity in it (a power has one,
    // so a hand with a power keeps the game's fingers), nothing held, no takedown animation.
    constexpr unsigned long long kStillMs = 1000, kSprintMs = 700;
    std::atomic<int> g_RestMode{ 1 };             // vr handrest: 0 = the game's fingers, 1 = the fixed pose, 2 = also a hand with a power (research)
    std::atomic<float> g_RestCurl{ 0.f };         // 0 = the learned open hand .. 1 = the fist
    std::atomic<int> g_RestGrip{ 1 };             // the grip closes the empty hand
    std::atomic<float> g_GripNow[2];              // the controllers' grip, 0..1 (Present)
    std::atomic<unsigned long long> g_StillMs{ 0 }, g_SprintMs{ 0 };   // how long the player has stood still / sprinted, ms (Present)
    std::atomic<float> g_PlayerSpeed{ 0.f };
    std::atomic<bool> g_RestForget{ false };
    FistPose g_RestOpen[2], g_RestFist[2];        // under g_RigM
    std::atomic<unsigned long long> g_RestApplied[2], g_RestNoPose[2];
    std::atomic<float> g_RestT[2];
    bool EmptyHand(int side)
    {
        void* ent = side ? BodyHide::RightWeaponEntity() : BodyHide::LeftWeaponEntity();
        return !ent && g_WeaponHits[side].load() == 0 && !ViewMatrixHook::AnimViewOn();
    }
    //! A power in the hand (research, vr handrest 2): an equipment entity that is neither a gun nor a gadget and draws
    //! fewer meshes than a weapon (a power is a single FX draw on the forearm, a sword many).
    bool PowerHand(int side)
    {
        void* ent = side ? BodyHide::RightWeaponEntity() : BodyHide::LeftWeaponEntity();
        return ent && !BodyHide::HoldsGun(side) && !BodyHide::HoldsGadget(side) && g_WeaponHits[side].load() < 3 && !ViewMatrixHook::AnimViewOn();
    }
    //! Rotation matrix (row-major) <-> quaternion (w, x, y, z), for blending two hand poses.
    void RotToQuat(const float* r, float q[4])
    {
        const float tr = r[0] + r[4] + r[8];
        if (tr > 0.f) { const float s = sqrtf(tr + 1.f) * 2.f; q[0] = 0.25f * s; q[1] = (r[7] - r[5]) / s; q[2] = (r[2] - r[6]) / s; q[3] = (r[3] - r[1]) / s; }
        else if (r[0] > r[4] && r[0] > r[8]) { const float s = sqrtf(1.f + r[0] - r[4] - r[8]) * 2.f; q[0] = (r[7] - r[5]) / s; q[1] = 0.25f * s; q[2] = (r[1] + r[3]) / s; q[3] = (r[2] + r[6]) / s; }
        else if (r[4] > r[8]) { const float s = sqrtf(1.f + r[4] - r[0] - r[8]) * 2.f; q[0] = (r[2] - r[6]) / s; q[1] = (r[1] + r[3]) / s; q[2] = 0.25f * s; q[3] = (r[5] + r[7]) / s; }
        else { const float s = sqrtf(1.f + r[8] - r[0] - r[4]) * 2.f; q[0] = (r[3] - r[1]) / s; q[1] = (r[2] + r[6]) / s; q[2] = (r[5] + r[7]) / s; q[3] = 0.25f * s; }
    }
    void QuatToRot(const float q[4], float* r)
    {
        const float w = q[0], x = q[1], y = q[2], z = q[3];
        r[0] = 1 - 2 * (y * y + z * z); r[1] = 2 * (x * y - w * z);     r[2] = 2 * (x * z + w * y);
        r[3] = 2 * (x * y + w * z);     r[4] = 1 - 2 * (x * x + z * z); r[5] = 2 * (y * z - w * x);
        r[6] = 2 * (x * z - w * y);     r[7] = 2 * (y * z + w * x);     r[8] = 1 - 2 * (x * x + y * y);
    }
    //! a..b by t, about `pivot`: the rotation by normalised lerp of the quaternions, and the pivot's image by lerp.
    //! The pivot matters: a palette matrix is W * B^-1, so it turns about the MODEL origin, half a metre and more from
    //! the finger it moves -- blending its rotation and translation apart swung the fingers off through the air
    //! mid-blend (measured: curl 0.3-0.7 drew triangles across both hands, 0 and 1 were right). About the bone's own
    //! bind centroid the chord error is a finger's length times the angle's, a few millimetres.
    Rigid BlendRigid(const Rigid& a, const Rigid& b, float t, const float pivot[3])
    {
        float qa[4], qb[4]; RotToQuat(a.r, qa); RotToQuat(b.r, qb);
        if (qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3] < 0.f) for (float& v : qb) v = -v;
        float q[4], n = 0.f;
        for (int i = 0; i < 4; i++) { q[i] = qa[i] + (qb[i] - qa[i]) * t; n += q[i] * q[i]; }
        n = sqrtf(n); if (n > 1e-9f) for (float& v : q) v /= n;
        Rigid o; QuatToRot(q, o.r);
        float pa[3], pb[3], rp[3]; Xform(a, pivot, pa); Xform(b, pivot, pb); Rotate(o, pivot, rp);
        for (int i = 0; i < 3; i++) o.t[i] = pa[i] + (pb[i] - pa[i]) * t - rp[i];
        return o;
    }
    void LearnPose(FistPose& fp, const float* pal, UINT bones, UINT n, const SegTable& T, int p, const Rigid& Mp)
    {
        const Rigid iMp = Inv(Mp);
        fp.has.reset();
        for (UINT b = 0; b < n && b < 256; b++)
            if (T.seg[b] == sgHand && (int)b != p) { fp.rel[b] = Mul(iMp, BoneRigid(pal + b * 12)); fp.has.set(b); }
        fp.valid = true; fp.bones = bones; fp.palm = p; fp.frame = g_PresentFrame;
    }
    //! How much of the hand, as the segment table NOW draws it, a pose covers: 0 = not this skeleton / palm. The table
    //! is not fixed: it was still growing when a pose was first learned (measured: the left open hand learned with 26
    //! of 52 hand bones, and the other 26 fingers kept the walk cycle's fist), and the learned wrist moves a couple of
    //! bones by it in and out of the hand (52 <-> 54) -- so full coverage is what gets learned, 90% is what gets used.
    float PoseCover(const FistPose& fp, UINT bones, int p, const SegTable& T, UINT n)
    {
        if (!fp.valid || fp.bones != bones || fp.palm != p) return 0.f;
        int hand = 0, have = 0;
        for (UINT b = 0; b < n && b < 256; b++) if (T.seg[b] == sgHand && (int)b != p) { hand++; have += fp.has.test(b); }
        return hand ? (float)have / (float)hand : 0.f;
    }
    bool PoseFits(const FistPose& fp, UINT bones, int p, const SegTable& T, UINT n) { return PoseCover(fp, bones, p, T, n) >= 0.9f; }

    // ---- the fingers from the controller ([Controls] FingerTracking, Valve Index, Layout = index) ----
    // Each of the player's fingers closes its own fingers of the drawn hand: every hand bone blended from the game's open
    // hand towards its fist (RestPass's two poses) by THAT finger's curl (XRInputState::fingerCurl). Which bone belongs to
    // which finger is read off the two poses themselves: from the open hand to the fist the four fingers all bend about
    // one axis -- across the knuckles -- and the thumb about another; so the bones that turn are split by their axis
    // (thumb or not) and the fingers' bones by where they sit along that axis (four groups; the index finger the one
    // nearest the thumb). Bones that barely turn (the palm's) join the nearest group. A hand whose bones cannot be
    // split so (too few, a group empty) closes all its fingers by their mean curl.
    std::atomic<bool> g_FingersOn{ false };
    std::atomic<float> g_FingerCurl[2][5];        // smoothed, Present
    std::atomic<int> g_FingerSrc[2];              // XRInputState::fingerSrc
    struct FingerMap { bool valid = false, tried = false; UINT bones = 0; int palm = -1; unsigned long long openFrame = 0, fistFrame = 0; int8_t f[256]; int count[5] = {}; };
    FingerMap g_FMap[2];                          // under g_RigM (RestPass)
    std::atomic<unsigned long long> g_FingerApplied[2];
    void PowerIter(const double M[9], double v[3])
    {
        for (int it = 0; it < 40; it++)
        {
            double w[3] = { M[0] * v[0] + M[1] * v[1] + M[2] * v[2], M[3] * v[0] + M[4] * v[1] + M[5] * v[2], M[6] * v[0] + M[7] * v[1] + M[8] * v[2] };
            const double nn = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]); if (nn < 1e-12) return;
            for (int i = 0; i < 3; i++) v[i] = w[i] / nn;
        }
    }
    //! Which finger each hand bone is (0 thumb .. 4 little), from the open hand and the fist (see above).
    void ClassifyFingers(int side, const FistPose& open, const FistPose& fist, const SegTable& T, UINT n, int p, FingerMap& fm)
    {
        fm = FingerMap{}; fm.tried = true; fm.bones = open.bones; fm.palm = p; fm.openFrame = open.frame; fm.fistFrame = fist.frame;
        memset(fm.f, -1, sizeof fm.f);
        struct Bn { int b; float d[3]; float ax[3]; float ang; };
        std::vector<Bn> bs;
        for (UINT b = 0; b < n && b < 256; b++)
        {
            if (T.seg[b] != sgHand || (int)b == p || !open.has.test(b) || !fist.has.test(b)) continue;
            Bn x{}; x.b = (int)b;
            float q[3]; Xform(open.rel[b], T.c[b], q);
            for (int i = 0; i < 3; i++) x.d[i] = q[i] - T.c[p][i];
            // the turn from the open hand to the fist: R = fist * open^T (row-major), its axis and angle
            const float* A = fist.rel[b].r; const float* O = open.rel[b].r; float R[9];
            for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[i * 3 + j] = A[i * 3] * O[j * 3] + A[i * 3 + 1] * O[j * 3 + 1] + A[i * 3 + 2] * O[j * 3 + 2];
            const float tr = std::clamp((R[0] + R[4] + R[8] - 1.f) * 0.5f, -1.f, 1.f);
            x.ang = acosf(tr) * 57.29578f;
            float ax[3] = { R[7] - R[5], R[2] - R[6], R[3] - R[1] };
            const float na = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
            for (int i = 0; i < 3; i++) x.ax[i] = na > 1e-6f ? ax[i] / na : 0.f;
            bs.push_back(x);
        }
        if (bs.size() < 10) { LOGI("gpuhands: side {} fingers: only {} hand bones in both poses: the whole hand closes together", side, bs.size()); return; }
        // the knuckles' axis: the dominant turn axis of the bones that turn
        double M[9] = {};
        for (const Bn& x : bs) if (x.ang > 15.f) for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M[i * 3 + j] += x.ang * x.ax[i] * x.ax[j];
        double K[3] = { 1, 0.3, 0.1 }; PowerIter(M, K);
        // thumb: a bone that turns about another axis
        std::vector<int> thumb, fing; float thumbA = 0.f;
        auto lat = [&](const Bn& x) { return (float)(x.d[0] * K[0] + x.d[1] * K[1] + x.d[2] * K[2]); };
        for (int i = 0; i < (int)bs.size(); i++)
        {
            const Bn& x = bs[i];
            if (x.ang <= 15.f) continue;
            const float c = fabsf((float)(x.ax[0] * K[0] + x.ax[1] * K[1] + x.ax[2] * K[2]));
            if (c < 0.8f) { thumb.push_back(i); thumbA += lat(x); } else fing.push_back(i);
        }
        if (thumb.empty() || fing.size() < 4) { LOGI("gpuhands: side {} fingers: {} thumb / {} finger bones turn: cannot tell the fingers apart, the whole hand closes together", side, thumb.size(), fing.size()); return; }
        thumbA /= (float)thumb.size();
        // the four fingers: the three widest gaps along the knuckles' axis
        std::vector<std::pair<float, int>> v; for (int i : fing) v.push_back({ lat(bs[i]), i });
        std::sort(v.begin(), v.end());
        std::vector<std::pair<float, int>> gaps; for (size_t i = 1; i < v.size(); i++) gaps.push_back({ v[i].first - v[i - 1].first, (int)i });
        std::sort(gaps.begin(), gaps.end(), [](auto& a, auto& b) { return a.first > b.first; });
        if (gaps.size() < 3) return;
        int cut[3] = { gaps[0].second, gaps[1].second, gaps[2].second }; std::sort(cut, cut + 3);
        float centre[4] = {}; int cnt[4] = {};
        for (size_t i = 0; i < v.size(); i++) { const int g = (int)(i >= (size_t)cut[0]) + (int)(i >= (size_t)cut[1]) + (int)(i >= (size_t)cut[2]); centre[g] += v[i].first; cnt[g]++; }
        for (int g = 0; g < 4; g++) { if (!cnt[g]) return; centre[g] /= (float)cnt[g]; }
        // the index finger is the group nearest the thumb: number them from that end
        const bool fromLow = fabsf(centre[0] - thumbA) < fabsf(centre[3] - thumbA);
        auto fingerOfGroup = [&](int g) { return 1 + (fromLow ? g : 3 - g); };
        for (size_t i = 0; i < v.size(); i++) { const int g = (int)(i >= (size_t)cut[0]) + (int)(i >= (size_t)cut[1]) + (int)(i >= (size_t)cut[2]); fm.f[bs[v[i].second].b] = (int8_t)fingerOfGroup(g); }
        for (int i : thumb) fm.f[bs[i].b] = 0;
        // the bones that barely turn: the nearest group along the axis (or the thumb's)
        for (const Bn& x : bs)
        {
            if (fm.f[x.b] >= 0) continue;
            const float a = lat(x); int best = 0; float bd = fabsf(a - thumbA);
            for (int g = 0; g < 4; g++) if (fabsf(a - centre[g]) < bd) { bd = fabsf(a - centre[g]); best = fingerOfGroup(g); }
            fm.f[x.b] = (int8_t)best;
        }
        for (const Bn& x : bs) fm.count[fm.f[x.b]]++;
        fm.valid = true;
        LOGI("gpuhands: side {} fingers told apart (palm {}, {} bones): thumb {} index {} middle {} ring {} little {} hand bones",
             side, p, open.bones, fm.count[0], fm.count[1], fm.count[2], fm.count[3], fm.count[4]);
    }
    bool SavedGripFist(int side, UINT bones, int p, const SegTable& T, UINT n, FistPose& out);   // below (the weapon grips)
    void RestPass(int side, float* pal, UINT bones, UINT n, const SegTable& T, int p, const Rigid& Mp, bool learnHere, bool powerHand = false)
    {
        FistPose& open = g_RestOpen[side]; FistPose& fist = g_RestFist[side];
        const bool fingers = g_FingersOn.load() && g_FingerSrc[side].load() != 0;
        if (g_RestForget.exchange(false)) { g_RestOpen[0].valid = g_RestOpen[1].valid = g_RestFist[0].valid = g_RestFist[1].valid = false; LOGI("gpuhands: empty-hand poses forgotten"); }
        if (learnHere)
        {
            const bool crouch = PlayerState::Running("Sneaking") || PlayerState::Running("AutoCrouch");
            // (a hand with a power: its idle open hand, only while no power is being used)
            if (PoseCover(open, bones, p, T, n) < 1.f && !crouch && g_StillMs.load() >= kStillMs && (!g_GripMode.load() || g_StatesClean.load())
                && (!powerHand || !PlayerState::Running("UsePower")))
            {
                LearnPose(open, pal, bones, n, T, p, Mp);
                LOGI("gpuhands: side {} empty hand: the game's OPEN hand learned standing still: {} hand bones round palm {} ({} bones)", side, open.has.count(), p, bones);
            }
            if (!powerHand && PoseCover(fist, bones, p, T, n) < 1.f && g_SprintMs.load() >= kSprintMs)
            {
                LearnPose(fist, pal, bones, n, T, p, Mp);
                LOGI("gpuhands: side {} empty hand: the game's sprinting FIST learned: {} hand bones round palm {} ({} bones)", side, fist.has.count(), p, bones);
            }
        }
        if (!g_RestMode.load() || !PoseFits(open, bones, p, T, n)) { g_RestNoPose[side]++; return; }
        const FistPose* B = PoseFits(fist, bones, p, T, n) ? &fist : PoseFits(g_Fist[side], bones, p, T, n) ? &g_Fist[side] : nullptr;
        static thread_local FistPose saved;
        if (!B && fingers && SavedGripFist(side, bones, p, T, n, saved)) B = &saved;
        float t = std::clamp(g_RestCurl.load(), 0.f, 1.f);
        if (g_RestGrip.load()) { const float g = std::clamp(g_GripNow[side].load(), 0.f, 1.f); t += (1.f - t) * g; }
        if (!B) t = 0.f;
        // the fingers from the controller: each bone by its finger's curl
        float curl[5] = {}; const FingerMap* fm = nullptr;
        if (fingers && B)
        {
            float mean = 0.f;
            for (int f = 0; f < 5; f++) { curl[f] = std::clamp(g_FingerCurl[side][f].load(), 0.f, 1.f); mean += curl[f] * 0.2f; }
            FingerMap& m = g_FMap[side];
            if (!m.tried || m.bones != open.bones || m.palm != p || m.openFrame != open.frame || m.fistFrame != B->frame) ClassifyFingers(side, open, *B, T, n, p, m);
            if (m.valid) fm = &m; else for (float& c : curl) c = mean;
            t = mean;
            g_FingerApplied[side]++;
        }
        g_RestT[side] = t;
        for (UINT b = 0; b < n && b < 256; b++)
        {
            if (!open.has.test(b) || T.seg[b] != sgHand || (int)b == p) continue;   // only what moves with the palm now
            const float tb = fingers && B ? (fm && fm->f[b] >= 0 ? curl[fm->f[b]] : t) : t;
            const Rigid R = (tb > 1e-3f && B->has.test(b)) ? BlendRigid(open.rel[b], B->rel[b], tb, T.c[b]) : open.rel[b];
            BoneWrite(pal + b * 12, Mul(Mp, R));
        }
        g_RestApplied[side]++;
    }

    // ---- the weapon grips (see g_GripMode) ----
    struct WeaponGrip
    {
        // the key: the item component's class, the hand, its skeleton -- and the skin mesh's vertex count, because Corvo
        // and Emily share the skeleton (68 bones, palm 4) but not the grip (measured: the sword's 2.2 cm apart)
        uint32_t cls = 0; int side = -1, bones = 0, palm = -1, verts = 0;
        bool valid = false; Rigid rel;                // the palm in the weapon entity's frame: where the game's idle grip has it
        bool session = false;                         // learned in this session (else restored from DVR_grips.txt)
        FistPose fing; bool fingSession = false;      // the fingers round the palm, the same moment
    };
    std::mutex g_GripM;                               // never held while taking another lock
    std::vector<WeaponGrip> g_Grips;                  // a handful: one per (side, weapon kind, skeleton); never shrinks (indices are kept in ArmInfo)
    struct GripTrack { uint32_t cls = 0; int bones = 0, n = 0; Rigid s0; unsigned long long frame = 0; };
    GripTrack g_GripTrack[2];                         // how long the grip has been steady, per side (under g_GripM)
    std::atomic<bool> g_GripSavePending{ false };
    const char* GripKindName(uint32_t cls)
    {
        switch (cls)
        {
        case BodyHide::kGunCpntVtable: return "pistol";
        case BodyHide::kCrossbowCpntVtable: return "crossbow";
        case BodyHide::kHeartCpntVtable: return "heart";
        case BodyHide::kThrowableCpntVtable: return "throwable";
        case BodyHide::kPlaceableCpntVtable: return "placeable";
        default: return "melee/other";
        }
    }
    int GripIndex(int side, uint32_t cls, int bones, int palm, int verts, bool create)
    {
        for (size_t i = 0; i < g_Grips.size(); i++)
        {
            const WeaponGrip& g = g_Grips[i];
            if (g.side == side && g.cls == cls && g.bones == bones && g.palm == palm && g.verts == verts) return (int)i;
        }
        if (!create) return -1;
        WeaponGrip g; g.side = side; g.cls = cls; g.bones = bones; g.palm = palm; g.verts = verts;
        g_Grips.push_back(g);
        return (int)g_Grips.size() - 1;
    }
    GripLookup GripFind(int side, uint32_t cls, int bones, int palm, int verts)
    {
        GripLookup r;
        std::lock_guard lk(g_GripM);
        const int i = GripIndex(side, cls, bones, palm, verts, false);
        if (i >= 0 && g_Grips[i].valid) { r.idx = i; r.valid = true; r.rel = g_Grips[i].rel; }
        return r;
    }
    //! Called once per frame per side with the flat game's palm in the weapon entity's frame. Learns the grip when it
    //! has stayed within kGripStableDeg / kGripStableCm for kGripStableFrames frames in which `learnable` held -- once
    //! per session (a file grip is replaced by the session's own, which heals a file from an older build).
    void GripObserve(int side, uint32_t cls, int bones, int palm, int verts, const Rigid& relA, bool learnable)
    {
        std::lock_guard lk(g_GripM);
        GripTrack& tr = g_GripTrack[side & 1];
        if (tr.frame == g_PresentFrame) return;       // several arm draws a frame: one sample
        tr.frame = g_PresentFrame;
        if (!learnable) { tr.n = 0; return; }
        if (tr.n == 0 || tr.cls != cls || tr.bones != bones || RotAngleDeg(relA, tr.s0) > kGripStableDeg || PosDiffCm(relA, tr.s0) > kGripStableCm)
        { tr.cls = cls; tr.bones = bones; tr.s0 = relA; tr.n = 1; return; }
        if (++tr.n < kGripStableFrames) return;
        WeaponGrip& g = g_Grips[GripIndex(side, cls, bones, palm, verts, true)];
        if (g.session) return;
        const float dA = g.valid ? RotAngleDeg(g.rel, relA) : -1.f, dP = g.valid ? PosDiffCm(g.rel, relA) : -1.f;
        g.rel = relA; g.valid = true; g.session = true;
        g_GripLearned[side & 1]++; g_GripSavePending = true;
        LOGI("weapongrip: side {} {} ({:#x}) grip learned: skeleton {} bones, palm {}, mesh {} vertices, palm at ({:.3f},{:.3f},{:.3f}) m on the weapon{}", side, GripKindName(cls), cls, bones, palm, verts,
             relA.t[0], relA.t[1], relA.t[2], dA < 0.f ? std::string() : fmt::format("; the saved one was {:.1f} deg / {:.1f} cm off", dA, dP));
    }
    //! The fingers of a hand on a weapon: learned once the grip is (in the same steady, clean frames), drawn from it
    //! whenever the palm is. g_RigM held; the palette is still the animated one here.
    void GripFingers(int side, float* pal, UINT bones, UINT n, const SegTable& T, int p, const Rigid& Mp, bool learnHere, int idx, bool applied)
    {
        std::lock_guard lk(g_GripM);
        if (idx < 0 || idx >= (int)g_Grips.size()) return;
        WeaponGrip& g = g_Grips[idx];
        // until it covers every bone the segment table calls hand (the table still grows after the first frames, and the
        // learned wrist moves a bone or two in and out: see PoseCover), then never again this session
        if (learnHere && g.session && (!g.fingSession || PoseCover(g.fing, bones, p, T, n) < 1.f) && g_GripClean[side & 1].load() && g_GripTrack[side & 1].n >= kGripStableFrames)
        {
            const bool first = !g.fingSession;
            LearnPose(g.fing, pal, bones, n, T, p, Mp);
            g.fingSession = true; g_GripSavePending = true;
            if (first) LOGI("weapongrip: side {} {} fingers learned: {} hand bones round palm {} ({} bones)", side, GripKindName(g.cls), g.fing.has.count(), p, bones);
        }
        if (!applied || g_GripMode.load() != 1 || !PoseFits(g.fing, bones, p, T, n)) return;
        for (UINT b = 0; b < n && b < 256; b++)
            if (g.fing.has.test(b) && T.seg[b] == sgHand && (int)b != p) BoneWrite(pal + b * 12, Mul(Mp, g.fing.rel[b]));
        g_GripFingUsed[side & 1]++;
    }
    //! The weapon grip fingers saved for this side and skeleton (DVR_grips.txt): a closed hand when no fist was seen yet
    //! (a hand that always holds a power never shows the game's empty fist). Copied: g_GripM is not held meanwhile.
    bool SavedGripFist(int side, UINT bones, int p, const SegTable& T, UINT n, FistPose& out)
    {
        std::lock_guard lk(g_GripM);
        for (const WeaponGrip& g : g_Grips)
            if (g.side == side && g.fing.valid && PoseFits(g.fing, bones, p, T, n)) { out = g.fing; return true; }
        return false;
    }
    std::wstring GripPath() { return GameInfo::GameDir() + L"\\DVR_grips.txt"; }
    //! DVR_grips.txt, one grip a line: class (hex) side bones palm verts (of the skin mesh's analysis), the palm (9 rotation +
    //! 3 translation), the number of finger bones, the palette's bone count and palm bone they were learned on, and per
    //! finger bone its index and its 12 numbers. Written at Present.
    void SaveGrips()
    {
        std::vector<WeaponGrip> gs; { std::lock_guard lk(g_GripM); gs = g_Grips; }
        FILE* f = _wfopen(GripPath().c_str(), L"w"); if (!f) return;
        int n = 0;
        for (const WeaponGrip& g : gs)
        {
            if (!g.valid) continue;
            fprintf(f, "%x %d %d %d %d", g.cls, g.side, g.bones, g.palm, g.verts);
            for (int i = 0; i < 9; i++) fprintf(f, " %.6f", g.rel.r[i]);
            for (int i = 0; i < 3; i++) fprintf(f, " %.6f", g.rel.t[i]);
            const bool fv = g.fing.valid;
            fprintf(f, " %d %u %d", fv ? (int)g.fing.has.count() : 0, fv ? g.fing.bones : 0u, fv ? g.fing.palm : -1);
            if (fv)
                for (int b = 0; b < 256; b++)
                    if (g.fing.has.test(b))
                    {
                        fprintf(f, " %d", b);
                        for (int i = 0; i < 9; i++) fprintf(f, " %.6f", g.fing.rel[b].r[i]);
                        for (int i = 0; i < 3; i++) fprintf(f, " %.6f", g.fing.rel[b].t[i]);
                    }
            fprintf(f, "\n"); n++;
        }
        fclose(f);
        LOGI("weapongrip: {} grips written to DVR_grips.txt", n);
    }
    void LoadGrips()
    {
        FILE* f = _wfopen(GripPath().c_str(), L"r");
        if (!f) { LOGI("weapongrip: no DVR_grips.txt yet (each grip is learned standing with the weapon in the hand)"); return; }
        int loaded = 0;
        for (;;)
        {
            WeaponGrip g; unsigned cls = 0, fb = 0; int nf = 0, fp = -1;
            if (fscanf_s(f, "%x %d %d %d %d", &cls, &g.side, &g.bones, &g.palm, &g.verts) != 5) break;
            g.cls = cls; bool ok = g.side >= 0 && g.side <= 1 && g.bones > 0 && g.bones <= 256 && g.palm >= 0 && g.palm < g.bones;
            for (int i = 0; i < 9; i++) ok &= fscanf_s(f, "%f", &g.rel.r[i]) == 1;
            for (int i = 0; i < 3; i++) ok &= fscanf_s(f, "%f", &g.rel.t[i]) == 1;
            ok &= fscanf_s(f, "%d %u %d", &nf, &fb, &fp) == 3 && nf >= 0 && nf <= 256;
            if (!ok) break;
            for (int k = 0; k < nf && ok; k++)
            {
                int b = -1; ok &= fscanf_s(f, "%d", &b) == 1 && b >= 0 && b < 256;
                if (!ok) break;
                Rigid r; for (int i = 0; i < 9; i++) ok &= fscanf_s(f, "%f", &r.r[i]) == 1;
                for (int i = 0; i < 3; i++) ok &= fscanf_s(f, "%f", &r.t[i]) == 1;
                g.fing.rel[b] = r; g.fing.has.set(b);
            }
            if (!ok) break;
            if (sqrtf(g.rel.t[0] * g.rel.t[0] + g.rel.t[1] * g.rel.t[1] + g.rel.t[2] * g.rel.t[2]) >= kHandSocketMax) continue;
            g.valid = true;
            if (nf > 0 && fb > 0 && fb <= 256 && fp >= 0) { g.fing.valid = true; g.fing.bones = fb; g.fing.palm = fp; }
            std::lock_guard lk(g_GripM);
            const int i = GripIndex(g.side, g.cls, g.bones, g.palm, g.verts, true);
            if (!g_Grips[i].session) { g_Grips[i] = g; loaded++; LOGI("weapongrip: side {} {} grip restored from DVR_grips.txt (skeleton {} bones, palm {}, mesh {} vertices, {} finger bones)", g.side, GripKindName(g.cls), g.bones, g.palm, g.verts, nf); }
        }
        fclose(f);
        if (!loaded) LOGI("weapongrip: DVR_grips.txt has no usable grip");
    }
    //! At Present: may each side's grip be learned from what the game draws now? Only with the weapon in the hand and the
    //! player in no state that moves the hand on it (measured, or plainly animated; a plain walk is fine, the steadiness
    //! test covers its bob), nor in a takedown or Shadow Walk, whose arms are the game's.
    void GripCleanTick()
    {
        static const char* const kDirty[] = {
            "Sneaking", "AutoCrouch", "WallCrouch", "Sliding", "Slipping", "Sprinting", "Jumping", "InAir", "Falling", "CatFall",
            "Mantle", "Vault", "Climb", "CancelClimb", "SwimMantle", "Swimming", "ShadowWalk", "ShadowHide", "Slash", "Block",
            "Draw", "Undraw", "EquipChange", "ReloadWeapon", "RequestReloadWeapon", "FireWeapon", "CancelFireWeapon", "Recoil",
            "ThrowWeapon", "PlaceWeapon", "PlaceWeaponReady", "Choke", "DropChoke", "CureChoke", "Assassination", "NpcFatality",
            "BloodthirstyFatality", "SyncAnim", "KnockedDown", "Stun", "Stumble", "Leaning", "Peeking", "CarryCorpse", "DropCorpse",
            "HoldObject", "ReleaseObject", "Interact", "LootObject", "EnterPossessedHost", "UsePower", "Blinking", "Grapple",
            "GrabGrappledEntity", "Dead", "Cinematic", "Spyglass", "SpyglassZoom", "Powerwheel", "UseHeart", "Journal", "ReadNote",
            "EatRat", "Crank", "InBendTime", "KillCam", "Versus", "VersusGrab", "Whisper", "Flying", "FishSwimming" };
        const char* why = nullptr;
        if (!PlayerState::Installed() || PlayerState::RunningCount() == 0) why = "nosim";
        else if (g_AnimSuspend.load()) why = "anim";
        else if (ViewMatrixHook::AnimViewOn()) why = "animview";
        else for (const char* n : kDirty) if (PlayerState::Running(n)) { why = n; break; }
        // ...and not for a while after one: the animation outlives its state. Measured: standing up from a crouch,
        // Sneaking ends and the palm stays in the reverse grip, steady, for 33 more frames before the regrip -- a
        // steadiness test alone would have learned it.
        const double now = (double)GetTickCount64() * 1e-3;
        static double dirtyAt = -1e9, sideDirtyAt[2] = { -1e9, -1e9 };
        if (why) dirtyAt = now;
        else if (now - dirtyAt < kGripSettleSec) why = "settling";
        g_StatesClean = why == nullptr;
        for (int s = 0; s < 2; s++)
        {
            const char* w = nullptr;
            { const uint32_t c = BodyHide::HeldItemClass(s); if (!c) w = "noitem"; else if (BodyHide::HoldsGadget(s)) w = "gadget"; }
            if (!w && GrabHand::HeldEntity() && GrabHand::Hand() == s) w = "holding";
            if (w) sideDirtyAt[s] = now;
            else if (now - sideDirtyAt[s] < kGripSettleSec) w = "settling";
            if (why) w = why;
            g_GripClean[s] = w == nullptr; g_GripDirty[s] = w ? w : "";
        }
    }

    void FistPass(int side, float* pal, UINT bones, const SegTable& T, const ArmRig& rig, bool learnHere, const ArmInfo* ai = nullptr)
    {
        const int p = (rig.palm >= 0 && rig.palm < T.bones && T.seg[rig.palm] == sgHand) ? rig.palm : T.rep[sgHand];
        if (p < 0 || (UINT)p >= bones) return;
        FistPose& fp = g_Fist[side];
        const bool holding = g_FistMode.load() && GrabHand::HeldEntity() && GrabHand::Hand() == side;
        const UINT n = std::min<UINT>(bones, (UINT)T.bones);
        const Rigid Mp = BoneRigid(pal + p * 12);
        if (!holding)
        {
            if (EmptyHand(side)) { RestPass(side, pal, bones, n, T, p, Mp, learnHere); return; }
            if (g_RestMode.load() == 2 && PowerHand(side)) { RestPass(side, pal, bones, n, T, p, Mp, false); return; }   // nothing learned from a power's hand
            // the fingers from the controller close a hand with a power too (its open hand learned from its own idle)
            if (g_FingersOn.load() && g_FingerSrc[side].load() != 0 && PowerHand(side) && !PlayerState::Running("UsePower"))
            { RestPass(side, pal, bones, n, T, p, Mp, learnHere, true); return; }
            // the fist an object is held with is the weapon's grip: learned under the weapon grip's rule (a regrip opens it)
            if (learnHere && g_WeaponHits[side].load() >= 3 && !PlayerState::Running("Slash") && g_PresentFrame - fp.frame >= 30 && (!g_GripMode.load() || g_GripClean[side].load()))
            {
                const bool first = !fp.valid || fp.bones != bones;
                LearnPose(fp, pal, bones, n, T, p, Mp);
                if (first) LOGI("gpuhands: side {} fist learned from the weapon's grip: {} hand bones round palm {} ({} bones)", side, fp.has.count(), p, bones);
                g_FistLearned[side]++;
            }
            if (ai && ai->grip >= 0) GripFingers(side, pal, bones, n, T, p, Mp, learnHere, ai->grip, ai->gripApplied);
            return;
        }
        if (!fp.valid || fp.bones != bones || fp.palm != p) { g_FistNoPose[side]++; return; }
        for (UINT b = 0; b < n && b < 256; b++)
            if (fp.has.test(b)) BoneWrite(pal + b * 12, Mul(Mp, fp.rel[b]));
        g_FistApplied[side]++;
    }
    //! vr handtrace: the fingers as they will be drawn (after FistPass), relative to the palm, against a reference pose:
    //! the largest and the mean displacement of a hand bone's bind centroid, in cm. Main palette only. g_RigM held.
    FistPose g_TraceFing[2];
    void TraceFingers(int side, const float* pal, UINT bones, const SegTable& T, const ArmRig& rig)
    {
        if (g_TraceLeft.load() <= 0) return;
        const int p = (rig.palm >= 0 && rig.palm < T.bones && T.seg[rig.palm] == sgHand) ? rig.palm : T.rep[sgHand];
        if (p < 0 || (UINT)p >= bones) return;
        const UINT n = std::min<UINT>(bones, (UINT)T.bones);
        const Rigid Mp = BoneRigid(pal + p * 12);
        FistPose& ref = g_TraceFing[side];
        if (g_TraceFingReq[side].exchange(false) || !ref.valid || ref.bones != bones || ref.palm != p) LearnPose(ref, pal, bones, n, T, p, Mp);
        const Rigid iMp = Inv(Mp);
        double sum = 0, mx = 0; int k = 0;
        for (UINT b = 0; b < n && b < 256; b++)
        {
            if (!ref.has.test(b) || T.seg[b] != sgHand || (int)b == p) continue;
            const Rigid R = Mul(iMp, BoneRigid(pal + b * 12));
            float a[3], c[3]; Xform(R, T.c[b], a); Xform(ref.rel[b], T.c[b], c);
            const double d = sqrt((double)(a[0] - c[0]) * (a[0] - c[0]) + (double)(a[1] - c[1]) * (a[1] - c[1]) + (double)(a[2] - c[2]) * (a[2] - c[2])) * 100.0;
            sum += d; if (d > mx) mx = d; k++;
        }
        g_TraceFingMax[side] = (float)mx; g_TraceFingMean[side] = k ? (float)(sum / k) : -1.f;
    }

    // ---- HandFx: the same deltas, for joints the ENGINE asks the animated skeleton for (effects on the hand) ----
    // Copied once per frame per side when that side's frame is computed, so the game threads never touch g_RigM
    // (held by the render thread across every arm draw).
    constexpr int kFxPalBones = 128;
    struct FxSnap
    {
        bool valid = false; unsigned long long frame = 0;
        ArmFrame F; SegTable T; int bones = 0;
        float pal[kFxPalBones * 12] = {};            // the ANIMATED main palette (research: joint index vs palette index)
    };
    std::mutex g_FxM;
    FxSnap g_Fx[2];
    // The price of the frame of latency: an effect placed with frame N-1's hand delta and drawn in frame N sits
    // |dh_N * p - dh_(N-1) * p| from the drawn hand, p the animated palm. Measured here, on the render thread, from
    // two consecutive frames of the same arm -- no mixing of frames from different threads.
    struct FxLag { unsigned long long n = 0; double sum = 0, max = 0; };
    FxLag g_FxLag[2];
    void FxPublish(int side, const float* pal, UINT bones, const SegTable& T, const ArmFrame& F)
    {
        std::lock_guard lk(g_FxM);
        FxSnap& s = g_Fx[side & 1];
        const unsigned long long gap = g_PresentFrame - s.frame;
        if (s.valid && s.F.valid && gap >= 1 && gap <= 3 && F.valid)
        {
            const float* w = F.W;                 // the animated wrist: the hand's own end of the forearm
            float a[3], b[3]; Xform(F.dh, w, a); Xform(s.F.dh, w, b);
            const double d = sqrt((double)(a[0] - b[0]) * (a[0] - b[0]) + (double)(a[1] - b[1]) * (a[1] - b[1]) + (double)(a[2] - b[2]) * (a[2] - b[2])) / (double)gap;
            FxLag& L = g_FxLag[side & 1]; L.n++; L.sum += d; if (d > L.max) L.max = d;
        }
        if (s.frame == g_PresentFrame && s.valid) return;     // several draws per frame share one delta
        s.valid = true; s.frame = g_PresentFrame; s.F = F; s.T = T; s.bones = (int)std::min<UINT>(bones, kFxPalBones);
        memcpy(s.pal, pal, s.bones * 48);
    }

    // ---- vr handjit: where each arm draw REALLY puts the palm, against where its own data says it should be ----
    // The palm drawn = the draw's model->view * (rewritten palm bone) * palm centroid; the target = the palm pose the hand
    // delta was aimed at, in the same draw's view. Both are in the view the draw is rendered with, so this is the error
    // in the image itself, per draw. Under g_RigM.
    struct JitStat { unsigned long long n = 0, bad1 = 0, bad10 = 0, hashDiff = 0, readDiff = 0, elbowN = 0; double sum = 0, max = 0, angMax = 0, elbowMax = 0; };
    JitStat g_Jit[2][2];                          // [side][0 the main palette / 1 another palette of that arm]
    std::atomic<int> g_JitTraceLeft{ 0 };
    std::atomic<bool> g_JitFrozen{ false };       // vr handjit -1: keep the statistics as they are (end of a measured phase)
    // The motion-vector palette: where the palm was drawn LAST frame, through the previous-frame matrix, against where
    // the target is now (the mock holds the head and the hands still, so the two should agree). Per side.
    struct JitPrev { unsigned long long n = 0, hit = 0, solved = 0, miss = 0, bad1 = 0; double sum = 0, max = 0; };
    JitPrev g_JitP[2];
    void JitMeasurePrev(int side, const ArmInfo& ai, const float* palAfter, UINT bones)
    {
        if (g_JitFrozen.load() || !ai.prevValid || !ai.palmTgtValid || ai.palm < 0 || (UINT)ai.palm >= bones) return;
        float pm[3], pv[3], tv[3]; Xform(BoneRigid(palAfter + ai.palm * 12), ai.palmC, pm); Xform(ai.prevPose, pm, pv); Xform(ai.vw, ai.palmTgt.t, tv);
        const float d[3] = { pv[0] - tv[0], pv[1] - tv[1], pv[2] - tv[2] };
        const double err = sqrt((double)Dot3(d, d));
        JitPrev& s = g_JitP[side & 1]; s.n++; s.sum += err; if (err > s.max) s.max = err; if (err > 0.001) s.bad1++;
    }
    // ---- the motion-vector palette ----
    // Each arm draw also binds the PREVIOUS frame's palette, which the engine pairs with the previous frame's matrix to
    // compute motion vectors (TAA reprojects with them: wrong ones smear and shimmer exactly what they are wrong about).
    // The right rewrite of it is the one this very mesh was DRAWN with last frame -- so every rewrite is kept, keyed by
    // the game's palette content it was made from, and the previous-frame palette is looked up by its content. A palette
    // is rewritten for the frame it belongs to, never with another frame's deltas (which is what a "previous ArmFrame"
    // amounted to). Under g_RigM.
    constexpr int kPrevCache = 48;
    struct PrevEntry { uint64_t h = 0; unsigned long long serial = 0, stamp = 0; UINT bytes = 0; float f[kPalMaxFloats]; };
    PrevEntry g_PrevCache[kPrevCache]; unsigned long long g_PrevStamp = 0;
    void PrevStore(uint64_t h, unsigned long long serial, const float* pal, UINT bytes)
    {
        int slot = 0;
        for (int i = 0; i < kPrevCache; i++)
        {
            if (g_PrevCache[i].h == h && g_PrevCache[i].serial == serial) { slot = i; break; }   // another pass of the same draw
            if (g_PrevCache[i].stamp < g_PrevCache[slot].stamp) slot = i;
        }
        PrevEntry& e = g_PrevCache[slot];
        e.h = h; e.serial = serial; e.stamp = ++g_PrevStamp; e.bytes = bytes; memcpy(e.f, pal, bytes);
    }
    //! The rewrite made for this content in an EARLIER view (the latest such), else for any view.
    bool PrevFind(uint64_t h, unsigned long long serial, float* pal, UINT bytes)
    {
        int best = -1, any = -1;
        for (int i = 0; i < kPrevCache; i++)
        {
            const PrevEntry& e = g_PrevCache[i];
            if (e.h != h || e.bytes != bytes || !e.stamp) continue;
            if (e.serial < serial && (best < 0 || e.serial > g_PrevCache[best].serial)) best = i;
            if (any < 0 || e.stamp > g_PrevCache[any].stamp) any = i;
        }
        const int i = best >= 0 ? best : any;
        if (i < 0) return false;
        memcpy(pal, g_PrevCache[i].f, bytes);
        return true;
    }

    std::vector<std::string> g_JitLines;
    void JitMeasure(int side, bool main, const ArmInfo& ai, const float* palAfter, UINT bones, const float palmBefore[12], uint64_t hashHere, const ArmFrame& F)
    {
        if (g_JitFrozen.load() || !ai.palmTgtValid || ai.palm < 0 || (UINT)ai.palm >= bones) return;
        if (!main && F.valid)
        {   // do the arm's other meshes pose the same arm? Their elbow against the main palette's latest (meaningful
            // with the animation still: the two are solved from palettes of neighbouring frames)
            const ArmFrame& M = g_ArmFrame[side & 1];
            if (M.valid && g_PresentFrame - M.frame <= 1)

            {
                float a[3], b[3]; Xform(F.m2w, F.Ep, a); Xform(M.m2w, M.Ep, b);
                const float e[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
                const double ed = sqrt((double)Dot3(e, e));
                JitStat& s = g_Jit[side & 1][1]; s.elbowN++; if (ed > s.elbowMax) s.elbowMax = ed;
            }
        }
        const Rigid Ma = BoneRigid(palAfter + ai.palm * 12), Mb = BoneRigid(palmBefore);
        float pm[3], pv[3], tv[3]; Xform(Ma, ai.palmC, pm); Xform(ai.pose, pm, pv); Xform(ai.vw, ai.palmTgt.t, tv);
        const float d[3] = { pv[0] - tv[0], pv[1] - tv[1], pv[2] - tv[2] };
        const double err = sqrt((double)Dot3(d, d));
        const Rigid Rd = Mul(ai.pose, Ma), Rt = Mul(ai.vw, ai.palmTgt);
        float tr = 0; for (int i = 0; i < 9; i++) tr += Rd.r[i] * Rt.r[i];
        const double ang = acos(std::clamp((tr - 1.0) * 0.5, -1.0, 1.0)) * 57.2958;
        // what this draw's OWN hand delta would have done with the palette it is drawn with
        float po[3], pov[3]; { float pb[3]; Xform(Mb, ai.palmC, pb); Xform(ai.dh, pb, po); } Xform(ai.pose, po, pov);
        const float d2[3] = { pov[0] - tv[0], pov[1] - tv[1], pov[2] - tv[2] };
        const double ownErr = sqrt((double)Dot3(d2, d2));
        JitStat& s = g_Jit[side & 1][main ? 0 : 1];
        s.n++; s.sum += err; if (err > s.max) s.max = err; if (ang > s.angMax) s.angMax = ang;
        if (err > 0.001) s.bad1++; if (err > 0.01) s.bad10++;
        if (hashHere != F.srcHash) s.hashDiff++;
        if (hashHere != ai.palHash) s.readDiff++;
        if (g_JitTraceLeft.load() > 0 && g_JitTraceLeft.fetch_sub(1) > 0 && g_JitLines.size() < 4000)
            g_JitLines.push_back(fmt::format("hj pf {} ser {} side {} {} Ffr {} Fser {} hash {} read {} err {:.1f}mm own {:.1f}mm ang {:.2f} pv ({:.3f},{:.3f},{:.3f}) tv ({:.3f},{:.3f},{:.3f}) vb {}",
                g_PresentFrame, ai.serial, side, main ? "main" : "other", F.frame, F.srcSerial, hashHere == F.srcHash ? "=F" : "!=F", hashHere == ai.palHash ? "=" : "!=",
                err * 1000.0, ownErr * 1000.0, ang, pv[0], pv[1], pv[2], tv[0], tv[1], tv[2], ai.vb0));
    }

    //! Swap a rewritten PerInstanceCB in for one draw. The caller must ALWAYS restore.
    struct Swap
    {
        bool on = false; ID3D11Buffer* vs = nullptr; ID3D11Buffer* ps = nullptr; bool psOn = false;
        int palSlot[2] = { -1, -1 }; ID3D11ShaderResourceView* palPrev[2] = {};   // joint palettes swapped for this draw
    };
    //! Hand a draw a collapsed copy of the palette bound at `slot`: every bone in `mask` becomes "everything to the
    //! socket point", the rest is left as the game wrote it. False when the palette was never seen written.
    bool SwapPalette(ID3D11DeviceContext* ctx, int slot, const std::bitset<256>& mask, const float socket[3], Swap& sw, int which, int side)
    {
        ID3D11ShaderResourceView* cur = nullptr;
        ctx->VSGetShaderResources(slot, 1, &cur);
        if (!cur) return false;
        ID3D11Resource* res = nullptr; cur->GetResource(&res);
        ID3D11Buffer* buf = nullptr;
        if (res) { res->QueryInterface(__uuidof(ID3D11Buffer), (void**)&buf); res->Release(); }
        float pal[kPalMaxFloats]; UINT bytes = 0;
        const bool found = buf && FindPalette(buf, pal, &bytes);
        if (buf) buf->Release();
        if (!found) { cur->Release(); g_PalMiss++; return false; }
        const UINT bones = bytes / 48;
        if (which == 0) AccumulateBones(side, pal, bytes, socket);
        for (UINT b = 0; b < bones && b < 256; b++)
        {
            if (!mask.test(b)) continue;
            float* m = pal + b * 12;
            for (int r = 0; r < 3; r++) { m[r * 4] = m[r * 4 + 1] = m[r * 4 + 2] = 0.f; m[r * 4 + 3] = socket[r]; }
        }
        memset(pal + bytes / 4, 0, kPalMaxBytes - bytes);
        const PalBuf& mine = PalPoolTake();
        t_InSwap = true;
        ctx->UpdateSubresource(mine.b, 0, nullptr, pal, 0, 0);
        t_InSwap = false;
        ctx->VSSetShaderResources(slot, 1, &mine.v);
        sw.palSlot[which] = slot; sw.palPrev[which] = cur;         // restored (and released) by EndSixDof
        g_PalSwaps++;
        return true;
    }
    //! The full-arm counterpart: every bone of the palette bound at `slot` gets its segment's delta, solved from that
    //! palette itself (SolveArm). `which` 1 is the previous-frame palette (motion vectors): it gets the rewrite that
    //! same content was drawn with in its own frame (PrevFind).
    bool SwapPaletteFull(ID3D11DeviceContext* ctx, int slot, const ArmInfo& ai, Swap& sw, int which)
    {
        ID3D11ShaderResourceView* cur = nullptr;
        ctx->VSGetShaderResources(slot, 1, &cur);
        if (!cur) return false;
        ID3D11Resource* res = nullptr; cur->GetResource(&res);
        ID3D11Buffer* buf = nullptr;
        if (res) { res->QueryInterface(__uuidof(ID3D11Buffer), (void**)&buf); res->Release(); }
        static thread_local float pal[kPalMaxFloats]; UINT bytes = 0;
        const bool found = buf && FindPalette(buf, pal, &bytes);
        void* key = buf;
        if (buf) buf->Release();
        if (!found) { cur->Release(); g_PalMiss++; return false; }
        const UINT bones = bytes / 48;
        bool applied = false;
        float palmBefore[12] = {};
        if (which == 0 && ai.palm >= 0 && (UINT)ai.palm < bones) memcpy(palmBefore, pal + ai.palm * 12, 48);
        const uint64_t hashHere = PalHash(pal, bones);
        {
            std::lock_guard lk(g_RigM);
            const int side = ai.side & 1;
            const SegTable& T = g_Seg[side];
            if (T.bones == 0) g_FullNoTableLive++;
            else
            {
                ArmRig& rig = g_Rig[side];
                if (which == 0 && T.rep[sgUpper] >= 0 && T.rep[sgFore] >= 0)
                {
                    // Has the main palette gone missing? Its pointer dies with a level load (see ArmRig::mainSeen).
                    // Adopt a replacement only after it has been absent for kMainStale frames while this side kept
                    // drawing: within one frame the skin and its sleeves bind several palettes of the same size, and
                    // adopting on the first mismatch would pick a different one every frame. Requiring a long
                    // absence means the stored palette really is gone, and once adopted the new one matches from
                    // then on, so this runs once per load and not again.
                    constexpr unsigned long long kMainStale = 60;
                    if (key == rig.mainKey) { rig.mainSeen = g_PresentFrame; rig.missFrames = 0; }
                    else if (rig.missFrame != g_PresentFrame) { rig.missFrame = g_PresentFrame; rig.missFrames++; }
                    else if (rig.mainSeen && g_PresentFrame - rig.mainSeen > kMainStale && (int)bones >= rig.mainKeyBones)
                    {
                        // WHICH palette replaces it. By size alone it went wrong after a reload, measured: the right
                        // side adopted a 72-bone palette, and 0.8 s later the LEFT side adopted the very same one --
                        // the right arm (69 bones) was then driven by the left arm's skeleton, its learned joints
                        // slid 10 cm (L1/L2 0.26 -> 0.36 m) and the hand vanished from view. The skin's own draw is
                        // the one to follow: its vertex buffer is recycled by the load, so it still names the skin.
                        // Never the palette the other arm is using. And if the skin's buffer does not come back
                        // within ~10 s (a load that did rebuild the vertex buffers), fall back to the size rule
                        // rather than leave the arm frozen.
                        const bool otherSide = key == g_Rig[side ^ 1].mainKey;
                        const bool skin = !rig.skinVb || ai.vb0 == rig.skinVb;
                        // "Late" counts frames in which this arm DREW without its palette: a loading screen draws no
                        // arm, and measured from mainSeen the whole load counted, so the fallback fired at once.
                        const bool late = rig.missFrames > 300;
                        if (!otherSide && (skin || late))
                        {
                            LOGI("gpubody: side {} main palette {} has not been bound for {} frames (a level load recycles them): adopting {} with {} bones ({})",
                                 side, rig.mainKey, g_PresentFrame - rig.mainSeen, key, bones, skin ? "the skin's own draw" : "no skin draw for 10 s: by size");
                            rig.mainKey = key; rig.mainSeen = g_PresentFrame; rig.missFrames = 0; g_MainReadopted++;
                            if (!skin) rig.skinVb = ai.vb0;
                            g_ArmFrame[side] = ArmFrame();   // a frame of the old palette
                        }
                    }
                    if (key == rig.mainKey) LearnArm(side, pal, bones, T);   // pal is still the animated one here
                }
                if (!g_ArmSolve.load())
                {   // legacy, for comparison (see g_ArmSolve)
                    if (which == 0 && key == rig.mainKey && g_LegacyF[side].frame != g_PresentFrame)
                    {
                        ArmFrame F;
                        if (SolveArm(side, pal, bones, T, ai, ai.m2w, ai.palmTgt, ai.bodyVr, ai.bodyW, F))
                        { F.srcHash = hashHere; g_LegacyPrev[side] = g_LegacyF[side]; g_LegacyF[side] = F; g_ArmFrame[side] = F; FxPublish(side, pal, bones, T, F); }
                    }
                    const ArmFrame& F = which == 1 && g_LegacyPrev[side].valid ? g_LegacyPrev[side] : g_LegacyF[side];
                    if (F.valid && g_PresentFrame - F.frame <= 2)
                    {
                        FistPass(side, pal, bones, T, rig, which == 0 && key == rig.mainKey, &ai);
                        ApplyFull(pal, bones, T, F); applied = true;
                        if (which == 0) JitMeasure(side, key == rig.mainKey, ai, pal, bones, palmBefore, hashHere, F);
                        else JitMeasurePrev(side, ai, pal, bones);
                    }
                    else g_FullNoFrameLive++;
                }
                else if (which == 0)
                {
                    ArmFrame F;
                    if (SolveArm(side, pal, bones, T, ai, ai.m2w, ai.palmTgt, ai.bodyVr, ai.bodyW, F))
                    {
                        F.srcHash = hashHere;
                        if (key == rig.mainKey) { g_ArmFrame[side] = F; FxPublish(side, pal, bones, T, F); }   // still animated here
                        FistPass(side, pal, bones, T, rig, key == rig.mainKey, &ai);
                        if (key == rig.mainKey) TraceFingers(side, pal, bones, T, rig);
                        ApplyFull(pal, bones, T, F); applied = true;
                        JitMeasure(side, key == rig.mainKey, ai, pal, bones, palmBefore, hashHere, F);
                        PrevStore(hashHere, ai.serial, pal, bytes);
                    }
                    else g_FullNoFrameLive++;
                }
                else
                {
                    JitPrev& jp = g_JitP[side];
                    if (PrevFind(hashHere, ai.serial, pal, bytes)) { applied = true; if (!g_JitFrozen.load()) jp.hit++; }
                    else if (ai.prevValid)
                    {   // never drawn (the first frame of a mesh): solve it in its previous view with this frame's targets
                        // -- they are head-locked, so the hand's motion relative to the head is the one thing it misses
                        const Rigid iv = Inv(ai.vw);
                        ArmFrame F;
                        if (SolveArm(side, pal, bones, T, ai, ai.prevPose, Mul(ai.vw, ai.palmTgt), Mul(ai.vw, ai.bodyVr), Mul(ai.vw, Mul(ai.bodyW, iv)), F))
                        {
                            FistPass(side, pal, bones, T, rig, false, &ai);
                            ApplyFull(pal, bones, T, F); applied = true; if (!g_JitFrozen.load()) jp.solved++;
                        }
                    }
                    if (!applied) { if (!g_JitFrozen.load()) jp.miss++; }
                    else JitMeasurePrev(side, ai, pal, bones);
                }
            }
        }

        if (!applied) { cur->Release(); return false; }
        memset(pal + bytes / 4, 0, kPalMaxBytes - bytes);
        const PalBuf& mine = PalPoolTake();
        t_InSwap = true;
        ctx->UpdateSubresource(mine.b, 0, nullptr, pal, 0, 0);
        t_InSwap = false;
        ctx->VSSetShaderResources(slot, 1, &mine.v);
        sw.palSlot[which] = slot; sw.palPrev[which] = cur;
        g_PalSwaps++;
        if (which == 0) g_FullDrawsLive[ai.side & 1]++;
        return true;
    }
    // ---- which view the PRESENTED image was drawn from ----
    // The game records gameplay into deferred contexts and executes the command lists on the immediate context before
    // Present. A draw rewritten on context C used view serial S (EnsureTransform); C's command list carries S; the list
    // executed last before a Present is the image being presented. That is a fact about this frame, not a guess about
    // the pipeline's depth (which differed between sessions: PipeTrace).
    struct CtxSer { void* k = nullptr; unsigned long long s = 0; };
    std::mutex g_CtxSerM; CtxSer g_CtxSer[32]; CtxSer g_ListSer[64]; int g_ListNext = 0;
    std::atomic<unsigned long long> g_ExecSerial{ 0 }, g_ExecFrame{ 0 };
    std::atomic<unsigned long long> g_CtxSerGen{ 1 };   // bumped whenever a context's tag is taken (FinishCommandList)
    //! Called for every draw: a context already tagged with at least `s` since the last FinishCommandList anywhere is
    //! left alone without the lock (the thread remembers what it tagged).
    void CtxSerialSet(void* ctx, unsigned long long s)
    {
        thread_local void* t_Ctx = nullptr; thread_local unsigned long long t_S = 0, t_Gen = 0;
        const unsigned long long gen = g_CtxSerGen.load(std::memory_order_acquire);
        if (t_Ctx == ctx && t_Gen == gen && s <= t_S) return;
        bool stored = false;
        {
            std::lock_guard lk(g_CtxSerM);
            CtxSer* freeSlot = nullptr;
            for (auto& e : g_CtxSer) { if (e.k == ctx) { if (s > e.s) e.s = s; stored = true; break; } if (!e.k && !freeSlot) freeSlot = &e; }
            if (!stored && freeSlot) { freeSlot->k = ctx; freeSlot->s = s; stored = true; }
        }
        if (stored) { t_Ctx = ctx; t_S = s; t_Gen = gen; }
    }
    using FinishCL_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
    using ExecCL_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
    FinishCL_t g_RealFinishCL = nullptr; ExecCL_t g_RealExecCL = nullptr;
    HRESULT STDMETHODCALLTYPE HkFinishCL(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** pp)
    {
        const HRESULT hr = g_RealFinishCL(c, restore, pp);
        unsigned long long s = 0;
        {
            std::lock_guard lk(g_CtxSerM);
            for (auto& e : g_CtxSer) if (e.k == c) { s = e.s; e.k = nullptr; e.s = 0; break; }
            g_CtxSerGen.fetch_add(1, std::memory_order_acq_rel);
            if (SUCCEEDED(hr) && pp && *pp && s) { g_ListSer[g_ListNext] = { *pp, s }; g_ListNext = (g_ListNext + 1) % 64; }
        }
        StereoTaa::OnFinishList(c, SUCCEEDED(hr) && pp ? *pp : nullptr);
        PipeTrace::Event(PipeTrace::Finish, (uint64_t)(uintptr_t)c & 0xFFFFF, s);
        return hr;
    }
    void NoteExecuted(ID3D11CommandList* l)
    {
        StereoTaa::OnExecuteList(l);
        unsigned long long s = 0;
        {
            std::lock_guard lk(g_CtxSerM);
            for (auto& e : g_ListSer) if (e.k == l) { s = e.s; e.k = nullptr; break; }
        }
        if (s) { g_ExecSerial = s; g_ExecFrame = g_PresentFrame; }
        PipeTrace::Event(PipeTrace::Execute, (uint64_t)(uintptr_t)l & 0xFFFFF, s);
    }
    void STDMETHODCALLTYPE HkExecCL(ID3D11DeviceContext* c, ID3D11CommandList* l, BOOL restore) { NoteExecuted(l); g_RealExecCL(c, l, restore); }
    ExecCL_t g_RealExecCLMT = nullptr;   // the thread-safe immediate context's (see RehookImmediate)
    void STDMETHODCALLTYPE HkExecCLMT(ID3D11DeviceContext* c, ID3D11CommandList* l, BOOL restore) { NoteExecuted(l); g_RealExecCLMT(c, l, restore); }

    //! Alternating-eye stereo with per-eye TAA: an entity's draw carries the previous MVP the engine cached from the
    //! last frame -- the OTHER eye's view (measured: arms, weapons, doors, props ~190 px of false motion with the head
    //! still). StereoTaa knows, for the view this draw belongs to (its PerViewCB at b1), the matrix C that takes a clip
    //! position of the other eye's last view to this eye's last view: applied to the previous rows of `f`.
    std::atomic<unsigned long long> g_EyeFixed{ 0 };
    bool EyePrevFix(ID3D11DeviceContext* ctx, const InstLayout* L, float* f, UINT bytes)
    {
        if (!L || !L->prev || !StereoTaa::FixingPrev()) return false;
        const int rows[4] = { L->px, L->py, L->pz, L->pw }, cur[4] = { L->x, L->y, L->z, L->w };
        for (int i = 0; i < 4; i++) if (rows[i] < 0 || (UINT)(rows[i] + 4) * 4 > bytes) return false;
        bool same = true;   // first frame of an object: the engine's previous IS the current one (no motion), leave it
        for (int i = 0; i < 4 && same; i++) if (cur[i] >= 0 && (UINT)(cur[i] + 4) * 4 <= bytes) same = !memcmp(f + rows[i], f + cur[i], 16);
        float pr[16], cr[16]; bool haveCur = true;
        for (int i = 0; i < 4; i++) { memcpy(pr + i * 4, f + rows[i], 16); if (cur[i] < 0 || (UINT)(cur[i] + 4) * 4 > bytes) haveCur = false; else memcpy(cr + i * 4, f + cur[i], 16); }
        // (the current rows and the engine's own previous are also kept for the draws of the next frame: vr aaprevrule)
        ID3D11Buffer* vcb = nullptr; ctx->VSGetConstantBuffers(1, 1, &vcb);
        const bool ok = vcb && StereoTaa::FixPrevRows(ctx, vcb, haveCur ? cr : nullptr, pr, same);
        if (vcb) vcb->Release();
        if (!ok) return false;
        for (int i = 0; i < 4; i++) memcpy(f + rows[i], pr + i * 4, 16);
        g_EyeFixed++;
        return true;
    }

    //! Certainly not one of the player's own draws: its model origin is farther from the eye it is drawn for than the near
    //! radius (TransformInstanceCb's test, from the game camera) plus the game camera's distance from the rendered eye (the
    //! HMD's offset) and a margin for the other eye and a tick of motion. Read from the draw's own rows only -- no lock,
    //! no shared state but two atomics -- so the world's ~2300 draws a frame never reach the locked part. False whenever
    //! in doubt (no layout, no camera, a research tool that wants every draw): TransformInstanceCb then decides.
    bool FarFromPlayer(const InstLayout* L, const float* in, UINT bytes)
    {
        if (!L || !L->ok) return false;
        if (g_DiagLeft.load(std::memory_order_relaxed) > 0 || g_PartsLeft.load(std::memory_order_relaxed) > 0
            || g_CensusLeft.load(std::memory_order_relaxed) > 0 || g_ProjAny.load(std::memory_order_relaxed)) return false;
        const size_t floats = bytes / 4;
        auto within = [&](int off) { return off >= 0 && (size_t)off + 4 <= floats; };
        if (!(within(L->x) && within(L->y) && within(L->z) && within(L->w))) return false;
        const float camEye = g_CamEyePub.load(std::memory_order_relaxed);
        if (camEye < 0.f) return false;
        float n = g_Near.load(std::memory_order_relaxed);
        if (n > 0.f && g_BodyFollow.load() && n < 2.5f) n = 2.5f;      // as TransformInstanceCb
        if (!(n > 0.f)) return false;
        const Factored f = Factor(in + L->x, in + L->y, in + L->z, in + L->w);
        if (!f.ok) return false;
        const float d2 = f.pose.t[0] * f.pose.t[0] + f.pose.t[1] * f.pose.t[1] + f.pose.t[2] * f.pose.t[2];
        const float reach = n + camEye + 0.5f;
        return d2 > reach * reach;
    }

    void BeginSixDof(ID3D11DeviceContext* ctx, Swap& sw, bool byIndex, UINT draw, bool allowMove = true)
    {
        if (!g_PoolReady.load()) return;
        t_DrawSerial = 0; t_XfUsed = 0;
        bool xf = allowMove && SixDofOn() && EnsureTransform();    // the newest view's transform (lock-free once built)
        const bool fixing = StereoTaa::FixingPrev();
        if (!xf && !fixing) return;
        if (xf) CtxSerialSet(ctx, g_XfSerialPub.load(std::memory_order_acquire));
        ID3D11Buffer* cur = nullptr;
        ctx->VSGetConstantBuffers(2, 1, &cur);                  // AddRefs
        if (!cur) { if (xf) g_CbMiss++; return; }
        D3D11_BUFFER_DESC bd{}; cur->GetDesc(&bd);
        const ShadowEntry* e = FindShadow(cur, bd.ByteWidth);
        if (!e) { if (xf) g_CbMiss++; cur->Release(); return; }   // never saw it written: the contents are unknown
        if (xf) g_CbHit++;
        ID3D11VertexShader* vs = nullptr; ctx->VSGetShader(&vs, nullptr, nullptr);
        const InstLayout* L = vs ? LayoutFor(vs) : nullptr;
        if (vs) vs->Release();
        float in[kInstMaxFloats];
        memcpy(in, e->f, std::min<size_t>(bd.ByteWidth, sizeof in));
        const bool eyeFixed = fixing && EyePrevFix(ctx, L, in, bd.ByteWidth);
        // Nearly every draw is the world, far from anything the player holds: rejected here from its own rows, before
        // the lookup of its view and TransformInstanceCb's locked copy of the transform state (FarFromPlayer).
        if (xf && !byIndex && FarFromPlayer(L, in, bd.ByteWidth)) { xf = false; thread_local unsigned t_Far = 0; if (++t_Far == 64) { g_NotNear.fetch_add(64, std::memory_order_relaxed); t_Far = 0; } }
        if (xf)
        {   // the view this draw belongs to, from the view matrix in its own PerViewCB, and that view's own transform when
            // it is not the newest one (the left eye of a synchronised pair, recorded after the right view was built)
            ID3D11Buffer* vcb = nullptr; ctx->VSGetConstantBuffers(1, 1, &vcb);
            if (vcb)
            {
                StereoTaa::LearnViewCb(vcb);
                float dm[16];
                if (StereoTaa::ViewMatrixOf(ctx, vcb, dm)) t_DrawSerial = ViewMatrixHook::SerialOfViewMatrix(dm);
                vcb->Release();
            }
            if (t_DrawSerial) xf = EnsureTransform(t_DrawSerial);
        }
        float out[kInstMaxFloats]; ArmInfo ai;
        const bool transformed = xf && TransformInstanceCb(ctx, L, in, bd.ByteWidth, out, byIndex, draw, &ai);
        if (!transformed) t_DrawProjValid = false;
        bool moved = transformed;
        if (moved && g_AnimSuspend.load()) { g_AnimSuspended++; moved = false; }   // identified and counted, drawn as the game has it
        if (t_DrawProjValid)
        {   // a draw of the player's own arm / equipment: certainly the main view (see g_Proj)
            std::lock_guard lk(g_ProjM);
            memcpy(g_Proj, t_DrawProj, sizeof g_Proj); g_ProjView = t_DrawProjView; g_ProjValid = true; g_ProjFrame = g_PresentFrame;
            ProjRemember(t_DrawProjSerial, t_DrawProj, t_DrawProjView, g_PresentFrame);
        }
        if (t_DrawProjValid && g_CensusLeft.load() > 0) CensusAdd(t_DrawProj, 0.f, true);
        if (!moved)
        {
            if (!eyeFixed) { cur->Release(); return; }
            memset(out, 0, kInstMaxBytes); memcpy(out, in, std::min<size_t>(bd.ByteWidth, kInstMaxBytes));   // only the previous rows changed
            ai = ArmInfo();
        }
        ID3D11Buffer* mine = PoolTake();
        if (!mine) { g_NoPool++; cur->Release(); return; }
        t_InSwap = true;
        ctx->UpdateSubresource(mine, 0, nullptr, out, 0, 0);
        t_InSwap = false;
        ctx->VSSetConstantBuffers(2, 1, &mine);
        sw.on = true; sw.vs = cur;
        // The material pass reads the same buffer in the pixel shader; leaving that one behind would light the
        // weapon from its old position.
        ID3D11Buffer* ps = nullptr;
        ctx->PSGetConstantBuffers(2, 1, &ps);
        if (ps == cur) { ctx->PSSetConstantBuffers(2, 1, &mine); sw.psOn = true; sw.ps = ps; }
        else if (ps) ps->Release();
        if (!moved) return;                                       // the eye correction alone: no arm, no palettes
        {   // research (heartbeat stereotaa line): was this moved draw placed with ITS OWN view's transform? Its view is the
            // one its PerViewCB holds (learned here, shadowed from then on); `lag` = how many views older than the one the
            // transform was built from.
            ID3D11Buffer* vcb = nullptr; ctx->VSGetConstantBuffers(1, 1, &vcb);
            if (vcb) StereoTaa::LearnViewCb(vcb);
            float dm[16];
            unsigned long long xs = t_XfUsed; if (!xs) { std::lock_guard lk(g_XfM); xs = g_XfSerial; }
            const unsigned long long ds = vcb && StereoTaa::ViewMatrixOf(ctx, vcb, dm) ? ViewMatrixHook::SerialOfViewMatrix(dm) : 0;
            if (!ds) g_XfUnknown++;
            else if (ds == xs) g_XfSame++;
            else { g_XfOther++; const unsigned long long lag = xs > ds ? xs - ds : 0; g_XfLag[lag < 3 ? lag : 3]++; }
            if (vcb) vcb->Release();
        }
        g_Patched++;
        if (ai.side >= 0 && g_ArmCbLeft.load() > 0 && g_ArmCbLeft.fetch_sub(1) > 0)
        {
            std::string v; char b[32];
            for (UINT i = 0; i < bd.ByteWidth / 4 && i < kInstMaxFloats; i++) { snprintf(b, sizeof b, "%s%.4g", (i % 4) ? "," : " |", e->f[i]); v += b; }
            ID3D11ShaderResourceView* psr[8] = {}; ctx->PSGetShaderResources(0, 8, psr);
            std::string ps8; for (auto* r : psr) { snprintf(b, sizeof b, " %p", (void*)r); ps8 += b; if (r) r->Release(); }
            LOGI("armcb side {} draw {} vb0 {} bytes {} psSame {} full {} L x{} y{} z{} w{} px{} m{} mat{} prev{} psSRV{} :{}", ai.side, draw, ai.vb0, bd.ByteWidth, sw.psOn ? 1 : 0, ai.full ? 1 : 0,
                 L ? L->x : -1, L ? L->y : -1, L ? L->z : -1, L ? L->w : -1, L ? L->px : -1, L ? L->mx : -1, L ? L->matSlot : -1, L ? L->prevMatSlot : -1, ps8, v);
        }
        // An arm draw. Full arm: every bone gets its segment's delta (hand on the controller, IK for the rest).
        if (ai.side >= 0 && ai.full && g_PalPoolReady.load() && L)
        {
            if (L->matSlot >= 0) SwapPaletteFull(ctx, L->matSlot, ai, sw, 0);
            if (L->prevMatSlot >= 0) SwapPaletteFull(ctx, L->prevMatSlot, ai, sw, 1);
        }
        // Hand only: collapse the bones that are not the hand, so only the hand is left where the controller is.
        else if (ai.side >= 0 && g_HandCollapse.load() && g_PalPoolReady.load() && L)
        {
            std::bitset<256> mask; { std::lock_guard lk(g_BoneM); mask = g_Collapse[ai.side]; }
            if (mask.none()) { std::lock_guard lk(g_ArmBonesM); if (g_ArmBones[ai.side].valid) mask = g_ArmBones[ai.side].collapse; }
            if (mask.any() || g_HandCollapse.load() == 2)   // 2 = statistics only, nothing collapsed
            {
                if (L->matSlot >= 0) SwapPalette(ctx, L->matSlot, mask, ai.socket, sw, 0, ai.side);
                if (L->prevMatSlot >= 0) SwapPalette(ctx, L->prevMatSlot, mask, ai.socket, sw, 1, ai.side);
            }
        }
    }
    void EndSixDof(ID3D11DeviceContext* ctx, Swap& sw)
    {
        if (!sw.on) return;
        for (int k = 0; k < 2; k++)
            if (sw.palSlot[k] >= 0) { ctx->VSSetShaderResources(sw.palSlot[k], 1, &sw.palPrev[k]); if (sw.palPrev[k]) sw.palPrev[k]->Release(); sw.palSlot[k] = -1; sw.palPrev[k] = nullptr; }
        ctx->VSSetConstantBuffers(2, 1, &sw.vs);
        if (sw.psOn) { ctx->PSSetConstantBuffers(2, 1, &sw.ps); if (sw.ps) sw.ps->Release(); }
        if (sw.vs) sw.vs->Release();
        sw.on = false;
    }

    //! Both constant-buffer paths for one target draw: the 6DOF rewrite and the legacy slot-1 view offset, which is
    //! kept because it is the scaffolding the swap mechanism was debugged on. Always paired with EndTarget.
    struct TargetSwap { Swap six; bool legacy = false; ID3D11Buffer* legacyPrev = nullptr; };
    void BeginTarget(ID3D11DeviceContext* c, UINT n, TargetSwap& t)
    {
        if (Scanning()) ScanDraw(c, n);
        const bool byIndex = IsTarget(n);
        const bool candidate = byIndex || g_Near.load() > 0.f;
        if (!candidate && !StereoTaa::FixingPrev()) return;
        if (SixDofOn() || StereoTaa::FixingPrev()) { BeginSixDof(c, t.six, byIndex, n, candidate); return; }
        if (byIndex && g_BindEnabled.load() && ViewOffsetOn()) t.legacy = BindPatchedView(c, &t.legacyPrev);
    }
    void EndTarget(ID3D11DeviceContext* c, TargetSwap& t)
    {
        EndSixDof(c, t.six);
        RestoreView(c, t.legacy, t.legacyPrev);
    }

    void Record(ID3D11DeviceContext* ctx, int kind, Entry e, UINT indexCount)
    {
        {   // every draw of every recording thread passes here: count per thread, publish every 64 (one shared cache line
            // fought over by all of them, twice a draw, otherwise)
            thread_local unsigned t_N[2][eCount] = {};
            if (++t_N[kind][e] == 64) { g_Counts[kind][e].fetch_add(64, std::memory_order_relaxed); g_Stats.draws.fetch_add(64, std::memory_order_relaxed); t_N[kind][e] = 0; }
        }
        if (g_WantConstants.load() && IsTarget(indexCount)) { CaptureConstants(ctx); g_WantConstants = false; }
        if (g_WantShader.load() && IsTarget(indexCount))
        {
            // These index counts are drawn by several shaders (depth pass, colour pass, ...), so keep asking until
            // we have seen a new one: the first capture was the depth-only pass, which is not the visible weapon.
            ID3D11VertexShader* vs = nullptr; ctx->VSGetShader(&vs, nullptr, nullptr);
            if (vs)
            {
                std::lock_guard lk(g_ShaderM);
                if (!g_ShaderDumped.count((void*)vs)) { g_ShaderDumped.insert((void*)vs); g_CapturedVS = vs; g_WantShader = false; }
                vs->Release();                                   // pointer kept only as a map key
            }
        }
        if (g_WantShadow.load() && IsTarget(indexCount))
        {   // grab the buffer this draw reads; its contents are copied out at Present (no reads on a deferred context)
            ID3D11Buffer* cb = nullptr; ctx->VSGetConstantBuffers(g_Slot.load(), 1, &cb);
            if (cb) { if (g_ViewCB) g_ViewCB->Release(); g_ViewCB = cb; g_WantShadow = false; }
        }
        if (!g_Recording.load()) return;
        ID3D11VertexShader* vs = nullptr;
        ctx->VSGetShader(&vs, nullptr, nullptr);               // AddRefs; only while recording
        { std::lock_guard lk(g_M); g_Sig[{ (void*)vs, indexCount }]++; g_SigKinds[{ (void*)vs, indexCount }] |= 1u << (e + 8 * kind); }
        if (vs) vs->Release();
    }

    using DrawIndexed_t  = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
    using Draw_t         = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
    using DrawIdxInst_t  = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
    using DrawInst_t     = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
    using DrawIndirect_t = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
    using Map_t          = HRESULT (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
    using Unmap_t        = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
    using UpdateSub_t    = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
    // Vtable slots for the three ways a constant buffer's contents can arrive.
    constexpr int kMapSlot = 14, kUnmapSlot = 15, kUpdateSubSlot = 48;
    // Compute dispatches and resource copies: StereoTaa (the TAA pass and its history)
    constexpr int kDispatchSlot = 41, kDispatchIndSlot = 42, kCopyRegionSlot = 46, kCopySlot = 47;
    constexpr int kExtraSlots[] = { kMapSlot, kUnmapSlot, kUpdateSubSlot, kDispatchSlot, kDispatchIndSlot, kCopyRegionSlot, kCopySlot };
    constexpr int kNumExtra = (int)(sizeof kExtraSlots / sizeof kExtraSlots[0]);
    using Dispatch_t     = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
    using DispatchInd_t  = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
    using CopyRegion_t   = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
    using Copy_t         = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);

    //! One set of trampolines + hooks per context kind, so the same code serves both vtables.
    //! Can Detours patch this function safely? Detours follows a jump at a function's entry (an import thunk, or
    //! another program's hook) to where the code really starts, and reads through that jump's pointer without asking
    //! whether it is valid. Measured with SteamVR as the OpenXR runtime: the game died at the first frame inside
    //! DetourAttach on a deferred-context function, reading address 0xFFFFFFFFFFFFFFFF. So walk the same jumps here,
    //! with reads that cannot fault, and say what is there: an entry that leads nowhere valid is NOT hooked (the
    //! feature that needs it degrades; the game keeps running), and one already hooked by someone else is logged.
    bool SafeRead(const void* p, void* out, size_t n)
    {
        __try { memcpy(out, p, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool Executable(const void* p)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
        return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    }
    std::string ModuleOf(const void* p)
    {
        HMODULE m = nullptr; char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &m) && m)
        {
            GetModuleFileNameA(m, name, MAX_PATH);
            const char* s = strrchr(name, 0x5C);   // the last backslash
            return fmt::format("{}+0x{:X}", s ? s + 1 : name, (uintptr_t)p - (uintptr_t)m);
        }
        return fmt::format("{} (no module)", p);
    }
    bool Hookable(const void* fn, const char* what)
    {
        const uint8_t* pc = (const uint8_t*)fn;
        std::string path = ModuleOf(pc);
        for (int hop = 0; hop < 6; hop++)
        {
            uint8_t b[16];
            if (!Executable(pc) || !SafeRead(pc, b, sizeof b))
            {
                LOGI("GpuTrace: NOT hooking {}: its code is not readable ({}) -- another program patched it?", what, path);
                return false;
            }
            const uint8_t* next = nullptr;
            if (b[0] == 0xE9) next = pc + 5 + *(const int32_t*)(b + 1);                 // jmp rel32
            else if (b[0] == 0xEB) next = pc + 2 + (int8_t)b[1];                           // jmp rel8
            else if (b[0] == 0xFF && b[1] == 0x25)                                         // jmp [rip+rel32]
            {
                const uint8_t* slot = pc + 6 + *(const int32_t*)(b + 2);
                if (!SafeRead(slot, &next, sizeof next)) next = (const uint8_t*)~0ull;
            }
            else
            {
                if (hop > 0) LOGI("GpuTrace: {} was already patched by another program: {}", what, path);
                return true;
            }
            path += " -> " + (Executable(next) ? ModuleOf(next) : fmt::format("{} (invalid)", (const void*)next));
            pc = next;
        }
        LOGI("GpuTrace: NOT hooking {}: its entry is a chain of jumps ({})", what, path);
        return false;
    }

    //! VARIANT: a second set of the same hooks (own trampolines) for another implementation of the same context kind --
    //! the immediate context's thread-safe one, which D3D11 switches to once multithread protection is on (Hooks<0, 1>).
    template <int KIND, int VARIANT = 0>
    struct Hooks
    {
        static inline DrawIndexed_t  realDrawIndexed = nullptr;
        static inline Draw_t         realDraw = nullptr;
        static inline DrawIdxInst_t  realDrawIdxInst = nullptr;
        static inline DrawInst_t     realDrawInst = nullptr;
        static inline DrawIndirect_t realDrawIdxInstIndirect = nullptr;
        static inline DrawIndirect_t realDrawInstIndirect = nullptr;
        static inline Map_t          realMap = nullptr;
        static inline Unmap_t        realUnmap = nullptr;
        static inline UpdateSub_t    realUpdateSub = nullptr;
        static inline Dispatch_t     realDispatch = nullptr;
        static inline DispatchInd_t  realDispatchInd = nullptr;
        static inline CopyRegion_t   realCopyRegion = nullptr;
        static inline Copy_t         realCopy = nullptr;

        static void STDMETHODCALLTYPE Dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z)
        {
            PassProfile::NoteDispatch(x, y, z, false);
            StereoTaa::Pass p; StereoTaa::OnDispatch(c, KIND, x, y, z, false, p);
            realDispatch(c, x, y, z);
            StereoTaa::AfterPass(c, KIND, p);
        }
        static void STDMETHODCALLTYPE DispatchInd(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o)
        {
            PassProfile::NoteDispatch(0, 0, 0, true);
            StereoTaa::Pass p; StereoTaa::OnDispatch(c, KIND, 0, 0, 0, true, p);
            realDispatchInd(c, b, o);
            StereoTaa::AfterPass(c, KIND, p);
        }
        static void STDMETHODCALLTYPE CopyRegion(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z,
                                                 ID3D11Resource* s, UINT ss, const D3D11_BOX* box)
        {
            StereoTaa::OnCopy(c, KIND, d, s, true);
            realCopyRegion(c, d, ds, x, y, z, s, ss, box);
        }
        static void STDMETHODCALLTYPE Copy(ID3D11DeviceContext* c, ID3D11Resource* d, ID3D11Resource* s)
        {
            StereoTaa::OnCopy(c, KIND, d, s, false);
            realCopy(c, d, s);
        }

        //! PerInstanceCB is written just before its draw; these three are how the contents get there. Everything is
        //! gated on the feature being on, so the cost is one atomic load per Map when it is off.
        static HRESULT STDMETHODCALLTYPE Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type,
                                             UINT flags, D3D11_MAPPED_SUBRESOURCE* out)
        {
            HRESULT hr = realMap(c, r, sub, type, flags, out);
            if (SUCCEEDED(hr) && !sub && out && out->pData)
            {
                StereoTaa::OnMap(c, r, out->pData);
                if (ShadowWanted()) RememberMap(r, out->pData);
                if (g_HandFollow.load()) RememberPalMap(r, out->pData);
            }
            return hr;
        }
        static void STDMETHODCALLTYPE Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub)
        {
            if (!sub)
            {   // StereoTaa first: the PerViewCB of the TAA
                StereoTaa::OnUnmap(c, r);
                // read it out before the driver reclaims the memory. A Map remembered while the feature was on is
                // released here even if it has been switched off since: gated on the flag, that slot leaked for good.
                if (ShadowWanted() || MapPending()) CaptureMap(r);
                if (g_HandFollow.load() || PalMapPending()) CapturePalMap(r);
            }
            realUnmap(c, r, sub);
        }
        static void STDMETHODCALLTYPE UpdateSub(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub,
                                                const D3D11_BOX* box, const void* data, UINT rp, UINT dp)
        {
            // t_InSwap: our own rewrite goes through this very hook and would cache OUR buffer, evicting the game's
            // entry from the same slot and starving the next draw.
            if (!sub && !box && data && !t_InSwap)
            {
                StereoTaa::OnUpdate(c, r, data);
                if (ShadowWanted()) { if (UINT bytes = InstanceBufferBytes(r)) StoreShadow(r, data, bytes); }
                if (g_HandFollow.load()) { if (UINT pb = PaletteBytes(r)) StorePalette(r, data, pb); }
            }
            realUpdateSub(c, r, sub, box, data, rp, dp);
        }

        static void STDMETHODCALLTYPE DrawIndexed(ID3D11DeviceContext* c, UINT n, UINT s, INT b)
        {
            Record(c, KIND, eDrawIndexed, n);
            PassProfile::NoteDraw(n, 1, false);
            WristHud::OnDraw(c, KIND, eDrawIndexed, n);
            if (Skipped(n) || SkippedVs(c)) return;
            if (WristHud::DropDraw(c, KIND)) return;      // a cutscene's letterbox bar
            { WristHud::Mirror wm; if (WristHud::MirrorBegin(c, KIND, wm)) { realDrawIndexed(c, n, s, b); WristHud::MirrorEnd(c, wm); if (wm.lift) return; } }
            TargetSwap t; BeginTarget(c, n, t);
            StereoTaa::Pass sp; StereoTaa::OnDraw(c, KIND, eDrawIndexed, n, sp);
            realDrawIndexed(c, n, s, b);
            StereoTaa::AfterPass(c, KIND, sp);
            EndTarget(c, t);
        }
        static void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* c, UINT n, UINT s)
        {   // proximity targeting works off the constants, not the count, so the non-indexed paths qualify too --
            // and a weapon pass left out here would stay behind and clip the moved geometry against its own depth
            Record(c, KIND, eDraw, n);
            PassProfile::NoteDraw(n, 1, false);
            WristHud::OnDraw(c, KIND, eDraw, n);
            if (SkippedVs(c)) return;
            if (WristHud::DropDraw(c, KIND)) return;      // a cutscene's letterbox bar
            { WristHud::Mirror wm; if (WristHud::MirrorBegin(c, KIND, wm)) { realDraw(c, n, s); WristHud::MirrorEnd(c, wm); if (wm.lift) return; } }
            TargetSwap t; BeginTarget(c, n, t);
            StereoTaa::Pass sp; StereoTaa::OnDraw(c, KIND, eDraw, n, sp);
            realDraw(c, n, s);
            StereoTaa::AfterPass(c, KIND, sp);
            EndTarget(c, t);
        }
        static void STDMETHODCALLTYPE DrawIdxInst(ID3D11DeviceContext* c, UINT n, UINT i, UINT s, INT b, UINT si)
        {
            Record(c, KIND, eDrawIndexedInstanced, n);
            PassProfile::NoteDraw(n, i, false);
            WristHud::OnDraw(c, KIND, eDrawIndexedInstanced, n);
            if (Skipped(n) || SkippedVs(c)) return;
            if (WristHud::DropDraw(c, KIND)) return;      // a cutscene's letterbox bar
            { WristHud::Mirror wm; if (WristHud::MirrorBegin(c, KIND, wm)) { realDrawIdxInst(c, n, i, s, b, si); WristHud::MirrorEnd(c, wm); if (wm.lift) return; } }
            TargetSwap t; BeginTarget(c, n, t);
            StereoTaa::Pass sp; StereoTaa::OnDraw(c, KIND, eDrawIndexedInstanced, n, sp);
            realDrawIdxInst(c, n, i, s, b, si);
            StereoTaa::AfterPass(c, KIND, sp);
            EndTarget(c, t);
        }
        static void STDMETHODCALLTYPE DrawInst(ID3D11DeviceContext* c, UINT n, UINT i, UINT s, UINT si)
        {
            Record(c, KIND, eDrawInstanced, n);
            PassProfile::NoteDraw(n, i, false);
            WristHud::OnDraw(c, KIND, eDrawInstanced, n);
            if (SkippedVs(c)) return;
            if (WristHud::DropDraw(c, KIND)) return;      // a cutscene's letterbox bar
            { WristHud::Mirror wm; if (WristHud::MirrorBegin(c, KIND, wm)) { realDrawInst(c, n, i, s, si); WristHud::MirrorEnd(c, wm); if (wm.lift) return; } }
            TargetSwap t; BeginTarget(c, n, t);
            StereoTaa::Pass sp; StereoTaa::OnDraw(c, KIND, eDrawInstanced, n, sp);
            realDrawInst(c, n, i, s, si);
            StereoTaa::AfterPass(c, KIND, sp);
            EndTarget(c, t);
        }
        static void STDMETHODCALLTYPE DrawIdxInstIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o)
        { Record(c, KIND, eDrawIndexedInstancedIndirect, 0); PassProfile::NoteDraw(0, 0, true); if (SkippedVs(c)) return; realDrawIdxInstIndirect(c, b, o); }
        static void STDMETHODCALLTYPE DrawInstIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o)
        { Record(c, KIND, eDrawInstancedIndirect, 0); PassProfile::NoteDraw(0, 0, true); if (SkippedVs(c)) return; realDrawInstIndirect(c, b, o); }

        //! Attach to one vtable. Entries already detoured for the other kind (shared implementation) are skipped.
        static int Attach(void** vt, void** taken, int nTaken)
        {
            if (!vt) return 0;
            auto isTaken = [&](void* f) { for (int i = 0; i < nTaken; i++) if (taken[i] == f) return true; return false; };
            realDrawIndexed         = isTaken(vt[kSlots[eDrawIndexed]]) ? nullptr : (DrawIndexed_t)vt[kSlots[eDrawIndexed]];
            realDraw                = isTaken(vt[kSlots[eDraw]]) ? nullptr : (Draw_t)vt[kSlots[eDraw]];
            realDrawIdxInst         = isTaken(vt[kSlots[eDrawIndexedInstanced]]) ? nullptr : (DrawIdxInst_t)vt[kSlots[eDrawIndexedInstanced]];
            realDrawInst            = isTaken(vt[kSlots[eDrawInstanced]]) ? nullptr : (DrawInst_t)vt[kSlots[eDrawInstanced]];
            realDrawIdxInstIndirect = isTaken(vt[kSlots[eDrawIndexedInstancedIndirect]]) ? nullptr : (DrawIndirect_t)vt[kSlots[eDrawIndexedInstancedIndirect]];
            realDrawInstIndirect    = isTaken(vt[kSlots[eDrawInstancedIndirect]]) ? nullptr : (DrawIndirect_t)vt[kSlots[eDrawInstancedIndirect]];
            realMap                 = isTaken(vt[kMapSlot]) ? nullptr : (Map_t)vt[kMapSlot];
            realUnmap               = isTaken(vt[kUnmapSlot]) ? nullptr : (Unmap_t)vt[kUnmapSlot];
            realUpdateSub           = isTaken(vt[kUpdateSubSlot]) ? nullptr : (UpdateSub_t)vt[kUpdateSubSlot];
            realDispatch            = isTaken(vt[kDispatchSlot]) ? nullptr : (Dispatch_t)vt[kDispatchSlot];
            realDispatchInd         = isTaken(vt[kDispatchIndSlot]) ? nullptr : (DispatchInd_t)vt[kDispatchIndSlot];
            realCopyRegion          = isTaken(vt[kCopyRegionSlot]) ? nullptr : (CopyRegion_t)vt[kCopyRegionSlot];
            realCopy                = isTaken(vt[kCopySlot]) ? nullptr : (Copy_t)vt[kCopySlot];
            // every entry checked first (Hookable): one Detours cannot follow is left alone instead of crashing the game
            const char* kind = KIND ? "deferred" : VARIANT ? "immediate (thread-safe)" : "immediate";
            auto ok = [&](auto& real, const char* name) -> bool
            {
                if (!real) return false;
                if (Hookable((const void*)real, fmt::format("{} {}", kind, name).c_str())) return true;
                real = nullptr; return false;
            };
            ok(realDrawIndexed, "DrawIndexed"); ok(realDraw, "Draw"); ok(realDrawIdxInst, "DrawIndexedInstanced");
            ok(realDrawInst, "DrawInstanced"); ok(realDrawIdxInstIndirect, "DrawIndexedInstancedIndirect");
            ok(realDrawInstIndirect, "DrawInstancedIndirect"); ok(realMap, "Map"); ok(realUnmap, "Unmap"); ok(realUpdateSub, "UpdateSubresource");
            ok(realDispatch, "Dispatch"); ok(realDispatchInd, "DispatchIndirect"); ok(realCopyRegion, "CopySubresourceRegion"); ok(realCopy, "CopyResource");
            int n = 0;
            if (realDrawIndexed)         { DetourAttach(&(PVOID&)realDrawIndexed, DrawIndexed); n++; }
            if (realDraw)                { DetourAttach(&(PVOID&)realDraw, Draw); n++; }
            if (realDrawIdxInst)         { DetourAttach(&(PVOID&)realDrawIdxInst, DrawIdxInst); n++; }
            if (realDrawInst)            { DetourAttach(&(PVOID&)realDrawInst, DrawInst); n++; }
            if (realDrawIdxInstIndirect) { DetourAttach(&(PVOID&)realDrawIdxInstIndirect, DrawIdxInstIndirect); n++; }
            if (realDrawInstIndirect)    { DetourAttach(&(PVOID&)realDrawInstIndirect, DrawInstIndirect); n++; }
            if (realMap)                 { DetourAttach(&(PVOID&)realMap, Map); n++; }
            if (realUnmap)               { DetourAttach(&(PVOID&)realUnmap, Unmap); n++; }
            if (realUpdateSub)           { DetourAttach(&(PVOID&)realUpdateSub, UpdateSub); n++; }
            if (realDispatch)            { DetourAttach(&(PVOID&)realDispatch, Dispatch); n++; }
            if (realDispatchInd)         { DetourAttach(&(PVOID&)realDispatchInd, DispatchInd); n++; }
            if (realCopyRegion)          { DetourAttach(&(PVOID&)realCopyRegion, CopyRegion); n++; }
            if (realCopy)                { DetourAttach(&(PVOID&)realCopy, Copy); n++; }
            return n;
        }
    };
    using Imm = Hooks<0>;
    using Def = Hooks<1>;
    using ImmMT = Hooks<0, 1>;
    // every context slot the hooks cover (draws, Map/Unmap/UpdateSubresource, dispatches, copies, ExecuteCommandList), and
    // every function already detoured in them (RehookImmediate)
    constexpr int kHookedImmSlots[] = { 12, 13, 20, 21, 39, 40, kMapSlot, kUnmapSlot, kUpdateSubSlot, kDispatchSlot, kDispatchIndSlot,
                                        kCopyRegionSlot, kCopySlot, 58 };
    std::vector<void*> g_HookedFns;
    bool g_ImmMTHooked = false;
    bool g_Installed = false;

    // --- stage 4: read the shader instead of guessing which constants matter ---
    // ID3D11VertexShader exposes no bytecode, so keep a copy from CreateVertexShader and disassemble on demand.
    using CreateVS_t = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
    using CreateIL_t = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**);
    CreateIL_t g_RealCreateIL = nullptr;
    struct ILElem { std::string semantic; UINT index, format, slot, offset; };
    std::mutex g_ILM;
    std::map<void*, std::vector<ILElem>> g_Layouts;
    HRESULT STDMETHODCALLTYPE HkCreateInputLayout(ID3D11Device* dev, const D3D11_INPUT_ELEMENT_DESC* d, UINT n, const void* sig, SIZE_T len, ID3D11InputLayout** out)
    {
        HRESULT hr = g_RealCreateIL(dev, d, n, sig, len, out);
        if (SUCCEEDED(hr) && out && *out && d)
        {
            std::vector<ILElem> v;
            for (UINT i = 0; i < n; i++) v.push_back({ d[i].SemanticName ? d[i].SemanticName : "", d[i].SemanticIndex, (UINT)d[i].Format, d[i].InputSlot, d[i].AlignedByteOffset });
            std::lock_guard lk(g_ILM);
            g_Layouts[(void*)*out] = std::move(v);
        }
        return hr;
    }
    // ---- research: capture the vertex streams of one arm draw and write them out at Present ----
    std::mutex g_VbM;
    struct VbCapture
    {
        bool have = false; ID3D11InputLayout* il = nullptr; ID3D11Buffer* vb[8] = {}; UINT stride[8] = {}, offset[8] = {};
        ID3D11Buffer* ib = nullptr; DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN; UINT ibOff = 0; UINT indexCount = 0, startIndex = 0; INT baseVertex = 0;
    } g_Vb;
    void CaptureVertexStreams(ID3D11DeviceContext* ctx, UINT indexCount)
    {
        std::lock_guard lk(g_VbM);
        if (g_Vb.have) return;
        ctx->IAGetInputLayout(&g_Vb.il);
        ctx->IAGetVertexBuffers(0, 8, g_Vb.vb, g_Vb.stride, g_Vb.offset);
        ctx->IAGetIndexBuffer(&g_Vb.ib, &g_Vb.ibFmt, &g_Vb.ibOff);
        g_Vb.indexCount = indexCount; g_Vb.have = true;
    }
    void WriteBufferFile(ID3D11Buffer* b, const std::wstring& path)
    {
        D3D11_BUFFER_DESC bd{}; b->GetDesc(&bd);
        D3D11_BUFFER_DESC sd = bd; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0; sd.StructureByteStride = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Buffer* st = nullptr;
        if (FAILED(g_Device->CreateBuffer(&sd, nullptr, &st)) || !st) { LOGI("vbdump: staging failed for {} bytes", bd.ByteWidth); return; }
        g_Immediate->CopyResource(st, b);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_Immediate->Map(st, 0, D3D11_MAP_READ, 0, &m)) && m.pData)
        {
            if (FILE* f = _wfopen(path.c_str(), L"wb")) { fwrite(m.pData, 1, bd.ByteWidth, f); fclose(f); }
            g_Immediate->Unmap(st, 0);
            LOGI("vbdump: wrote {} bytes", bd.ByteWidth);
        }
        st->Release();
    }
    //! Remember the vertex streams of this arm draw for analysis at Present. Formats come from the input layout
    //! when we saw it created; the engine's skinned layout (position R32G32B32_FLOAT in stream 0, weights as two
    //! R8G8B8A8_UNORM and indices as two R8G8B8A8_UINT in a 16-byte stream) is assumed when we did not.
    void CaptureArmMesh(ID3D11DeviceContext* ctx, int side, void* palKey)
    {
        ID3D11Buffer* vb[8] = {}; UINT stride[8] = {}, off[8] = {};
        ctx->IAGetVertexBuffers(0, 8, vb, stride, off);
        if (!vb[0] || g_MeshDone.count((void*)vb[0])) { for (auto b : vb) if (b) b->Release(); return; }
        ID3D11InputLayout* il = nullptr; ctx->IAGetInputLayout(&il);
        MeshCapture mc; mc.side = side; mc.key = (void*)vb[0]; mc.palKey = palKey;
        int posSlot = 0, skinSlot = -1;
        {
            std::lock_guard lk(g_ILM);
            auto it = il ? g_Layouts.find((void*)il) : g_Layouts.end();
            if (it != g_Layouts.end())
            {
                mc.layoutKnown = true; int found = 0;
                for (auto& e : it->second)
                {
                    if (e.semantic == "VERTEX_POSITION" && e.index == 0) { posSlot = (int)e.slot; mc.posElemOff = e.offset; mc.posFmt = e.format; found |= 1; }
                    if (e.semantic == "VERTEX_SKINWEIGHTS" && e.index == 0) { skinSlot = (int)e.slot; mc.wOff0 = e.offset; mc.wFmt = e.format; found |= 2; }
                    if (e.semantic == "VERTEX_SKINWEIGHTS" && e.index == 1) mc.wOff1 = e.offset;
                    if (e.semantic == "VERTEX_SKININDICES" && e.index == 0) { mc.iOff0 = e.offset; mc.iFmt = e.format; found |= 4; }
                    if (e.semantic == "VERTEX_SKININDICES" && e.index == 1) mc.iOff1 = e.offset;
                }
                if (found != 7) skinSlot = -1;
            }
        }
        if (il) il->Release();
        if (skinSlot < 0)
        {   // unknown layout: the observed engine format, if the strides agree
            for (int i = 0; i < 8; i++) if (vb[i] && stride[i] == 16) { skinSlot = i; break; }
            if (stride[posSlot] != 12) skinSlot = -1;
        }
        if (skinSlot < 0 || !vb[skinSlot]) { g_MeshDone.insert((void*)vb[0]); g_MeshRejected++; for (auto b : vb) if (b) b->Release(); return; }
        mc.pos = vb[posSlot]; mc.posStride = stride[posSlot]; mc.posOff = off[posSlot];
        mc.skin = vb[skinSlot]; mc.skinStride = stride[skinSlot]; mc.skinOff = off[skinSlot];
        mc.pos->AddRef(); mc.skin->AddRef();
        for (auto b : vb) if (b) b->Release();
        mc.have = true; g_MeshCap = mc; g_MeshDone.insert((void*)mc.key); g_MeshCaps++;
    }
    bool ReadBuffer(ID3D11Buffer* b, std::vector<uint8_t>& out)
    {
        D3D11_BUFFER_DESC bd{}; b->GetDesc(&bd);
        D3D11_BUFFER_DESC sd = bd; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0; sd.StructureByteStride = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Buffer* st = nullptr;
        if (FAILED(g_Device->CreateBuffer(&sd, nullptr, &st)) || !st) return false;
        g_Immediate->CopyResource(st, b);
        D3D11_MAPPED_SUBRESOURCE m{}; bool ok = false;
        if (SUCCEEDED(g_Immediate->Map(st, 0, D3D11_MAP_READ, 0, &m)) && m.pData) { out.assign((uint8_t*)m.pData, (uint8_t*)m.pData + bd.ByteWidth); g_Immediate->Unmap(st, 0); ok = true; }
        st->Release();
        return ok;
    }
    //! Present: turn the captured streams into per-bone bind centroids and the hand/forearm split.
    void AnalyseArmMesh()
    {
        MeshCapture mc;
        { std::lock_guard lk(g_MeshM); if (!g_MeshCap.have) return; mc = g_MeshCap; g_MeshCap = MeshCapture(); }
        std::vector<uint8_t> pos, skin;
        const bool ok = g_Device && g_Immediate && ReadBuffer(mc.pos, pos) && ReadBuffer(mc.skin, skin);
        mc.pos->Release(); mc.skin->Release();
        if (!ok || mc.posFmt != 6 || mc.wFmt != 28 || mc.iFmt != 30) { g_MeshRejected++; LOGI("gpuhands: mesh rejected (fmt pos {} w {} i {})", mc.posFmt, mc.wFmt, mc.iFmt); return; }
        const size_t nv = (pos.size() - mc.posOff) / mc.posStride;
        const size_t ns = (skin.size() - mc.skinOff) / mc.skinStride;
        const size_t n = nv < ns ? nv : ns;
        ArmBones ab; ab.bones = 0;
        double cw[256] = {}; double cs[256][3] = {};
        double dp[256][3] = {}, dp2[256] = {};      // dominated vertices: sum of positions and of |p|^2 (their spread)
        for (size_t v = 0; v < n; v++)
        {
            const float* p = (const float*)(pos.data() + mc.posOff + v * mc.posStride + mc.posElemOff);
            const uint8_t* sk = skin.data() + mc.skinOff + v * mc.skinStride;
            const uint8_t* wgt[2] = { sk + mc.wOff0, sk + mc.wOff1 }; const uint8_t* idx[2] = { sk + mc.iOff0, sk + mc.iOff1 };
            for (int k = 0; k < 8; k++)
            {
                const float w = wgt[k / 4][k % 4] / 255.f; const int b = idx[k / 4][k % 4];
                if (w <= 0.f || b >= 256) continue;
                cw[b] += w; for (int i = 0; i < 3; i++) cs[b][i] += w * p[i];
                if (w > 0.25f) { ab.n[b]++; for (int i = 0; i < 3; i++) dp[b][i] += p[i]; dp2[b] += p[0] * p[0] + p[1] * p[1] + p[2] * p[2]; }
                if (b + 1 > ab.bones) ab.bones = b + 1;
            }
        }
        int palm = -1; double best = 0;
        for (int b = 0; b < ab.bones; b++)
        {
            ab.w[b] = (float)cw[b];
            if (cw[b] > 0) ab.bonesUsed++;
            if (cw[b] > 0) for (int i = 0; i < 3; i++) ab.c[b][i] = (float)(cs[b][i] / cw[b]);
            // The palm is the bone that DOMINATES the most vertices, not the one with the most total weight: on the
            // right arm the ring fingertip out-weighed the palm (161 vs 133) while dominating fewer vertices (187 vs
            // 217), and a fingertip as the hand's frame makes the whole hand pivot on that finger.
            if (ab.n[b] > best || (ab.n[b] == best && palm >= 0 && cw[b] > cw[palm])) { best = ab.n[b]; palm = b; }
        }
        if (palm < 0) { g_MeshRejected++; return; }
        {   // The palm is a broad PLATE; a fingertip is a thimble. On Corvo's left skin the dominant-vertex rule picked
            // a fingertip (bone 40, 187 vertices, against the real palm's 177): its centroid sat at the end of the
            // fingers, the arm axis came out ACROSS the hand, and the rig had no forearm or upper arm to drive
            // (reps 4/1/-1/-1, fulldraws L 0). So among the bones that dominate nearly as many vertices, the one whose
            // vertices spread widest is the palm. It only overrides when the rule's pick is clearly the smaller of the
            // two, so the arms whose palm was already right keep it.
            auto spread = [&](int b) {
                if (ab.n[b] < 8) return 0.0;
                double m[3], m2 = dp2[b] / ab.n[b]; for (int i = 0; i < 3; i++) m[i] = dp[b][i] / ab.n[b];
                const double v = m2 - (m[0] * m[0] + m[1] * m[1] + m[2] * m[2]); return v > 0 ? sqrt(v) : 0.0; };
            int wide = palm; double ws = spread(palm);
            for (int b = 0; b < ab.bones; b++) if (ab.n[b] >= 0.8 * ab.n[palm] && spread(b) > ws) { ws = spread(b); wide = b; }
            if (wide != palm && spread(palm) < 0.6 * ws)
            {
                LOGI("gpuhands: side {} palm bone {} ({} vertices, spread {:.1f} cm) is a fingertip-sized bone: bone {} ({} vertices, spread {:.1f} cm) is the palm",
                     mc.side, palm, ab.n[palm], spread(palm) * 100.0, wide, ab.n[wide], ws * 100.0);
                palm = wide;
            }
        }
        // Which end is the fingers: the one with more bones within 8 cm of the extreme point. A hand has fifteen
        // finger bones plus helpers crowded into a few centimetres; the forearm end has two or three.
        auto dist = [&](int a, int b) { float d = 0; for (int i = 0; i < 3; i++) { const float x = ab.c[a][i] - ab.c[b][i]; d += x * x; } return sqrtf(d); };
        int e0 = palm; for (int b = 0; b < ab.bones; b++) if (cw[b] > 0 && dist(b, palm) > dist(e0, palm)) e0 = b;      // farthest from the palm
        int e1 = e0;   for (int b = 0; b < ab.bones; b++) if (cw[b] > 0 && dist(b, e0) > dist(e1, e0)) e1 = b;          // farthest from that
        auto crowd = [&](int e) { int c = 0; for (int b = 0; b < ab.bones; b++) if (cw[b] > 0 && dist(b, e) < 0.08f) c++; return c; };
        ab.tip = crowd(e0) >= crowd(e1) ? e0 : e1;
        const int prox = ab.tip == e0 ? e1 : e0;
        float axis[3]; float al = 0; for (int i = 0; i < 3; i++) { axis[i] = ab.c[prox][i] - ab.c[ab.tip][i]; al += axis[i] * axis[i]; }
        al = sqrtf(al); if (al < 1e-3f) { g_MeshRejected++; return; }
        for (int i = 0; i < 3; i++) axis[i] /= al;
        {   // research: every bone's bind centroid and dominant-vertex count, to see what an arm mesh looks like
            std::string o; char t[64];
            for (int bb = 0; bb < ab.bones; bb++) if (cw[bb] > 0) { snprintf(t, sizeof t, " %d:(%.3f,%.3f,%.3f)n%d", bb, ab.c[bb][0], ab.c[bb][1], ab.c[bb][2], ab.n[bb]); o += t; }
            LOGI("gpuhands: side {} mesh {} bones (palm {}, tip {}, prox {}):{}", mc.side, n, palm, ab.tip, prox, o);
        }
        const float cut = g_HandCut.load();
        for (int b = 0; b < ab.bones; b++)
        {
            ab.s[b] = cw[b] > 0 ? (ab.c[b][0] - ab.c[ab.tip][0]) * axis[0] + (ab.c[b][1] - ab.c[ab.tip][1]) * axis[1] + (ab.c[b][2] - ab.c[ab.tip][2]) * axis[2] : 1e9f;
            if (cw[b] <= 0 || ab.s[b] > cut) ab.collapse.set(b);
        }
        for (int b = ab.bones; b < 256; b++) ab.collapse.set(b);
        ab.palm = palm; ab.valid = true; ab.verts = n; memcpy(ab.axis, axis, 12);
        {   // The PALM NORMAL, in the bind pose: a hand is a flat plate, so the direction of least spread of its
            // bones IS the normal of the palm. Which of the two ways it points is decided by the THUMB -- the one
            // bone that leaves the plate, and it leaves it on the palm's side. Two power iterations give the two
            // directions the plate spreads along; their cross product is the normal.
            // This is what lets an empty hand be pointed at something anatomically ("fingers along the controller,
            // palm towards the body") instead of by replaying whatever grip the last weapon happened to teach --
            // measured, a pistol and a crossbow teach holds 90 degrees apart, and a power got whichever came last.
            double C[3] = {}, W = 0;
            for (int b = 0; b < ab.bones; b++)
                if (cw[b] > 0 && ab.s[b] <= cut) { W += cw[b]; for (int i = 0; i < 3; i++) C[i] += cw[b] * ab.c[b][i]; }
            if (W > 0)
            {
                for (int i = 0; i < 3; i++) C[i] /= W;
                double M[3][3] = {};
                for (int b = 0; b < ab.bones; b++)
                {
                    if (!(cw[b] > 0) || ab.s[b] > cut) continue;
                    const double d[3] = { ab.c[b][0] - C[0], ab.c[b][1] - C[1], ab.c[b][2] - C[2] };
                    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M[i][j] += cw[b] * d[i] * d[j];
                }
                auto power = [&](double A[3][3], double v[3]) {
                    v[0] = 0.577; v[1] = 0.577; v[2] = 0.577;
                    for (int it = 0; it < 40; it++)
                    {
                        double o[3] = {};
                        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) o[i] += A[i][j] * v[j];
                        const double n = sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
                        if (!(n > 1e-12)) return false;
                        for (int i = 0; i < 3; i++) v[i] = o[i] / n;
                    }
                    return true;
                };
                double v1[3], v2[3];
                bool ok1 = power(M, v1);
                if (ok1)
                {   // deflate and take the second spread direction
                    double lam = 0; { double o[3] = {}; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) o[i] += M[i][j] * v1[j]; lam = o[0] * v1[0] + o[1] * v1[1] + o[2] * v1[2]; }
                    double D[3][3];
                    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) D[i][j] = M[i][j] - lam * v1[i] * v1[j];
                    if (power(D, v2))
                    {
                        float nrm[3] = { (float)(v1[1] * v2[2] - v1[2] * v2[1]), (float)(v1[2] * v2[0] - v1[0] * v2[2]), (float)(v1[0] * v2[1] - v1[1] * v2[0]) };
                        const float nl = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
                        if (nl > 1e-6f)
                        {
                            for (int i = 0; i < 3; i++) nrm[i] /= nl;
                            // WHICH WAY it points is decided by the one bone that leaves the plate -- and it leaves
                            // it on the BACK, not on the palm: the normal is taken away from it. That sign is not a
                            // guess, it is what two independent measurements said. Turning the hand the other way
                            // made the forearm carry 176.5 degrees of twist where this way carries -50.8 (an arm
                            // cannot pronate 176 degrees, so the palm was upside down), and the user, looking at
                            // the same build, reported seeing the back of the hand and not the palm. The weapon
                            // test that was tried first -- is the weapon on the palm's side -- decided nothing:
                            // the equipment entity sits ~18 cm across the barrel from the palm, so its direction
                            // came out nearly perpendicular to the normal (cos 0.09 to 0.29) for all three weapons.
                            int offBone = -1; float offPlane = 0;      // `far` is a windows.h macro
                            for (int b = 0; b < ab.bones; b++)
                            {
                                if (!(cw[b] > 0) || ab.s[b] > cut) continue;
                                const float d = (ab.c[b][0] - (float)C[0]) * nrm[0] + (ab.c[b][1] - (float)C[1]) * nrm[1] + (ab.c[b][2] - (float)C[2]) * nrm[2];
                                if (fabsf(d) > fabsf(offPlane)) { offPlane = d; offBone = b; }
                            }
                            if (offBone >= 0 && offPlane > 0) for (int i = 0; i < 3; i++) nrm[i] = -nrm[i];
                            memcpy(ab.palmN, nrm, 12); ab.palmNValid = offBone >= 0 && fabsf(offPlane) > 0.002f;
                            LOGI("gpuhands: side {} palm normal (bind) ({:.3f},{:.3f},{:.3f}), away from bone {} which is {:.1f} mm off the palm plane, valid {}",
                                 mc.side, nrm[0], nrm[1], nrm[2], offBone, fabsf(offPlane) * 1000.f, ab.palmNValid ? 1 : 0);
                        }
                    }
                }
            }
        }
        bool main = false, rekey = false;
        {   // several meshes share the arm's palette (skin, sleeve pieces, rings); the skin has by far the most
            // vertices and is the one whose palm/fingertip mean anything, so a smaller mesh never replaces a bigger
            // The skin is the mesh that touches the most bones (a hand alone is thirty-odd); a sleeve or a
            // wristband touches a few, however many vertices it has.
            std::lock_guard lk(g_ArmBonesM);
            const ArmBones& have = g_ArmBones[mc.side];
            if (have.valid && (have.bonesUsed > ab.bonesUsed || (have.bonesUsed == ab.bonesUsed && have.verts >= n)))
            {
                // The SAME skin (same vertex and bone count) on a new vertex buffer: a load that rebuilt the buffers.
                // It is still this arm's skin, so the rig follows it to its new palette -- everything learned from it
                // (bind-space joints, palm, hold) stays valid, it is the same mesh.
                rekey = have.bonesUsed == ab.bonesUsed && have.verts == n;
                LOGI("gpuhands: side {} mesh with {} vertices / {} bones {} (have {} / {})", mc.side, n, ab.bonesUsed,
                     rekey ? "is the skin again on a new buffer: the rig follows it" : "ignored", have.verts, have.bonesUsed);
            }
            else
            {
                g_ArmBones[mc.side] = ab;
                g_HandQ[mc.side] = HandQ();          // the socket constant belongs to the palm bone of THIS mesh
                g_HandHold[mc.side] = HandHold();    // and so does the hold
                main = true;
            }
        }
        {   // The full-arm rig: EVERY mesh contributes where its bones sit in the bind pose (the sleeves are the
            // only meshes rigged to the upper-arm bones), the skin defines the axis and the palm.
            std::lock_guard lk(g_RigM);
            MeshBones mb; mb.key = mc.palKey; mb.side = mc.side; mb.verts = n;
            for (int b = 0; b < 256 && b < ab.bones; b++) { mb.cw[b] = (float)cw[b]; memcpy(mb.c[b], ab.c[b], 12); }
            if (g_MeshList.size() >= 48) g_MeshList.erase(g_MeshList.begin());
            g_MeshList.push_back(mb);
            if (rekey && g_Rig[mc.side].valid && mc.palKey && mc.palKey != g_Rig[mc.side ^ 1].mainKey)
            {
                ArmRig& r = g_Rig[mc.side];
                LOGI("gpubody: side {} skin re-keyed: palette {} -> {}, vertex buffer {} -> {}", mc.side, r.mainKey, mc.palKey, r.skinVb, mc.key);
                r.mainKey = mc.palKey; r.skinVb = mc.key; r.mainSeen = g_PresentFrame; r.missFrames = 0;
                g_ArmFrame[mc.side] = ArmFrame();
            }
            if (main)
            {
                ArmRig& r = g_Rig[mc.side]; r = ArmRig(); r.valid = true; memcpy(r.tip, ab.c[ab.tip], 12); memcpy(r.axis, axis, 12); r.palm = palm; r.mainKey = mc.palKey; r.mainSeen = g_PresentFrame; r.skinVb = mc.key;
                const float cut = g_HandCut.load();
                JointReset(r.wrist, cut, r); JointReset(r.elbow, cut + g_ForearmPrior.load(), r); JointReset(r.shoulder, cut + g_ForearmPrior.load() + g_UpperPrior.load(), r);
                for (int k = 0; k < 3; k++) for (int b = 0; b < 256; b++) g_Rel[mc.side][k][b] = RelStat();
                g_ArmFrame[mc.side] = ArmFrame();
            }
            if (g_Rig[mc.side].valid)
            {
                RebuildSegTables(mc.side);
                int cnt[sgCount] = {}; const SegTable& T = g_Seg[mc.side];
                for (int b = 0; b < T.bones; b++) cnt[T.seg[b]]++;
                LOGI("gpubody: side {} mesh {} on palette {}: rig now has {} bones: hand {} fore {} upper {} root {} (reps {} {} {} {})",
                     mc.side, n, mc.palKey, T.bones, cnt[sgHand], cnt[sgFore], cnt[sgUpper], cnt[sgRoot], T.rep[sgHand], T.rep[sgFore], T.rep[sgUpper], T.rep[sgRoot]);
            }
        }
        if (!main) return;
        g_MeshAnalyzed++;
        LOGI("gpuhands: side {} mesh analysed: {} vertices, {} bones ({} used), palm bone {} ({} vertices), fingertip bone {}, arm length {:.3f} m, {} bones collapse beyond {:.2f} m",
             mc.side, n, ab.bones, ab.bonesUsed, palm, ab.n[palm], ab.tip, al, (int)ab.collapse.count() - (256 - ab.bones), cut);
        LoadHandQ();
    }
    //! The hand-socket constant survives the session: one line per side, keyed by bone count and palm bone.
    void SaveHandQ()
    {
        std::wstring path = GameInfo::GameDir() + L"\\DVR_hands.txt";
        FILE* f = _wfopen(path.c_str(), L"w"); if (!f) return;
        std::lock_guard lk(g_ArmBonesM);
        for (int sd = 0; sd < 2; sd++)
        {
            const HandQ& h = g_HandQ[sd]; if (!h.valid) continue;
            fprintf(f, "%d %d %d", sd, h.bones, h.palm);
            for (int i = 0; i < 9; i++) fprintf(f, " %.6f", h.q.r[i]);
            for (int i = 0; i < 3; i++) fprintf(f, " %.6f", h.q.t[i]);
            // The hold is appended to the same line: a file written before it existed simply has 15 numbers and
            // the loader stops there, so an old DVR_hands.txt keeps working and the hold is learned again.
            const HandHold& hh = g_HandHold[sd];
            if (hh.valid && hh.bones == h.bones && hh.palm == h.palm)
            {
                for (int i = 0; i < 9; i++) fprintf(f, " %.6f", hh.h.r[i]);
                for (int i = 0; i < 3; i++) fprintf(f, " %.6f", hh.h.t[i]);
            }
            fprintf(f, "\n");
        }
        fclose(f);
    }
    //! Re-read whenever a side still has no socket constant. This used to be a one-shot (`static bool loaded`),
    //! and the latch quietly cost the power hand its pose: the meshes are analysed one at a time, so on the single
    //! pass that ran, the OTHER side's ArmBones were still invalid and its line was skipped -- and the file was
    //! then never looked at again. Measured in the Dreadful Whale save, where the left hand starts with Far Reach
    //! and so never holds a weapon to learn from: q L stayed false for the whole session, the power branch never
    //! ran, and the left hand was driven by the weapon delta against the POWER's entity, which sits 1.6 m away on
    //! the forearm. A side that already has a Q is never overwritten, so a learned one still wins.
    void LoadHandQ()
    {
        std::wstring path = GameInfo::GameDir() + L"\\DVR_hands.txt";
        FILE* f = _wfopen(path.c_str(), L"r"); if (!f) return;
        int sd, bones, palm; float v[12]; float hv[12];
        // 15 numbers = socket only (a file written before the hold existed); 27 = socket and hold.
        int n;
        while ((n = fscanf_s(f, "%d %d %d %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f",
                             &sd, &bones, &palm, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11],
                             &hv[0], &hv[1], &hv[2], &hv[3], &hv[4], &hv[5], &hv[6], &hv[7], &hv[8], &hv[9], &hv[10], &hv[11])) >= 15)
        {
            const bool haveHold = n == 27;
            if (sd < 0 || sd > 1) continue;
            std::lock_guard lk(g_ArmBonesM);
            if (g_HandQ[sd].valid) continue;                                   // learned or already restored
            const ArmBones& ab = g_ArmBones[sd];
            if (!ab.valid || ab.bones != bones || ab.palm != palm) continue;   // a different skeleton: do not reuse
            if (sqrtf(v[9] * v[9] + v[10] * v[10] + v[11] * v[11]) >= kHandSocketMax)
            {
                static bool warned[2] = {};
                if (!warned[sd]) { warned[sd] = true; LOGI("gpuhands: side {} hand socket in DVR_hands.txt is {:.2f} m from the palm: ignored (learned at a weapon swap)", sd, sqrtf(v[9] * v[9] + v[10] * v[10] + v[11] * v[11])); }
                continue;
            }
            HandQ& h = g_HandQ[sd]; memcpy(h.q.r, v, 36); memcpy(h.q.t, v + 9, 12); h.bones = bones; h.palm = palm; h.valid = true;
            LOGI("gpuhands: side {} hand socket restored from DVR_hands.txt", sd);
            if (haveHold && sqrtf(hv[9] * hv[9] + hv[10] * hv[10] + hv[11] * hv[11]) < kHandHoldMax)
            {
                HandHold& hh = g_HandHold[sd]; memcpy(hh.h.r, hv, 36); memcpy(hh.h.t, hv + 9, 12);
                hh.bones = bones; hh.palm = palm; hh.valid = true;
                LOGI("gpuhands: side {} hand hold restored from DVR_hands.txt: palm ({:.3f},{:.3f},{:.3f}) m from the controller", sd, hh.h.t[0], hh.h.t[1], hh.h.t[2]);
            }
        }
        fclose(f);
    }

    void FlushVbDump()
    {
        std::lock_guard lk(g_VbM);
        if (!g_Vb.have) return;
        std::wstring dir = GameInfo::GameDir();
        if (FILE* f = _wfopen((dir + L"\\DVR_vb_layout.txt").c_str(), L"w"))
        {
            fprintf(f, "indexCount %u ibFormat %u ibOffset %u\n", g_Vb.indexCount, (UINT)g_Vb.ibFmt, g_Vb.ibOff);
            {
                std::lock_guard lk2(g_ILM);
                auto it = g_Layouts.find((void*)g_Vb.il);
                if (it != g_Layouts.end())
                    for (auto& e : it->second) fprintf(f, "elem %s %u format %u slot %u offset %u\n", e.semantic.c_str(), e.index, e.format, e.slot, e.offset);
                else fprintf(f, "layout %p unknown\n", (void*)g_Vb.il);
            }
            for (int i = 0; i < 8; i++) if (g_Vb.vb[i]) { D3D11_BUFFER_DESC bd{}; g_Vb.vb[i]->GetDesc(&bd); fprintf(f, "vb %d stride %u offset %u bytes %u\n", i, g_Vb.stride[i], g_Vb.offset[i], bd.ByteWidth); }
            fclose(f);
        }
        for (int i = 0; i < 8; i++) if (g_Vb.vb[i]) { WriteBufferFile(g_Vb.vb[i], dir + L"\\DVR_vb" + std::to_wstring(i) + L".bin"); g_Vb.vb[i]->Release(); g_Vb.vb[i] = nullptr; }
        if (g_Vb.ib) { WriteBufferFile(g_Vb.ib, dir + L"\\DVR_ib.bin"); g_Vb.ib->Release(); g_Vb.ib = nullptr; }
        if (g_Vb.il) { g_Vb.il->Release(); g_Vb.il = nullptr; }
        g_Vb.have = false;
        LOGI("vbdump: done");
    }
    CreateVS_t g_RealCreateVS = nullptr;
    HRESULT STDMETHODCALLTYPE HkCreateVertexShader(ID3D11Device* dev, const void* code, SIZE_T len,
                                                   ID3D11ClassLinkage* linkage, ID3D11VertexShader** out)
    {
        HRESULT hr = g_RealCreateVS(dev, code, len, linkage, out);
        if (SUCCEEDED(hr) && out && *out && code && len)
        {
            {
                std::lock_guard lk(g_ShaderM);
                auto& slot = g_ShaderCode[(void*)*out];
                g_ShaderBytes += len - slot.size();      // an address reused after a level load replaces its entry
                slot.assign((const uint8_t*)code, (const uint8_t*)code + len);
                const uint64_t h = DxbcHash(code, len);
                g_VsHash[(void*)*out] = h;
                g_PsCsHash.erase((void*)*out);
                // an address reused by another shader must not inherit the old one's skip
                for (auto& p : g_SkipVsPtr) { void* e = (void*)*out; p.compare_exchange_strong(e, nullptr); }
                for (auto& p : g_TrailVsPtr) { void* e = (void*)*out; p.compare_exchange_strong(e, nullptr); }
                if (h) for (auto& s : g_SkipHash) if (s.load() == h) { AddSkipPtr((void*)*out); break; }
                if (IsTrailHash(h)) AddTrailPtr((void*)*out);
            }
            {   // A new shader at an address we already reflected means the old one was destroyed: its layout
                // describes a constant buffer that no longer exists and must not be applied to this one.
                std::lock_guard lk(g_LayoutM);
                if (g_LayoutByVs.erase((void*)*out)) g_LayoutGen++;
            }
        }
        return hr;
    }

    //! Pixel and compute shaders: their DXBC hash (StereoTaa finds the TAA pass by it) and bytecode (gpushaderhash).
    using CreatePS_t = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
    using CreateCS_t = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**);
    CreatePS_t g_RealCreatePS = nullptr; CreateCS_t g_RealCreateCS = nullptr;
    void KeepShader(void* sh, const void* code, SIZE_T len)
    {
        std::lock_guard lk(g_ShaderM);
        auto& slot = g_ShaderCode[sh];
        g_ShaderBytes += len - slot.size();
        slot.assign((const uint8_t*)code, (const uint8_t*)code + len);
        g_PsCsHash[sh] = DxbcHash(code, len);
        g_VsHash.erase(sh);                               // the address was a vertex shader before (a level load)
    }
    HRESULT STDMETHODCALLTYPE HkCreatePixelShader(ID3D11Device* dev, const void* code, SIZE_T len, ID3D11ClassLinkage* l, ID3D11PixelShader** out)
    {
        HRESULT hr = g_RealCreatePS(dev, code, len, l, out);
        if (SUCCEEDED(hr) && out && *out && code && len) KeepShader(*out, code, len);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE HkCreateComputeShader(ID3D11Device* dev, const void* code, SIZE_T len, ID3D11ClassLinkage* l, ID3D11ComputeShader** out)
    {
        HRESULT hr = g_RealCreateCS(dev, code, len, l, out);
        if (SUCCEEDED(hr) && out && *out && code && len) { KeepShader(*out, code, len); StereoTaa::OnComputeShader(*out, code, len); }
        return hr;
    }

    //! Disassemble a captured shader to <game>\DVR_shader.txt (d3dcompiler loaded on demand).
    void DumpShader(void* vs)
    {
        std::vector<uint8_t> code;
        {
            std::lock_guard lk(g_ShaderM);
            auto it = g_ShaderCode.find(vs);
            if (it == g_ShaderCode.end()) { LOGI("gpushader: no bytecode for {} ({} shaders known)", vs, g_ShaderCode.size()); return; }
            code = it->second;
        }
        using Disasm_t = HRESULT (WINAPI*)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);
        HMODULE d3dc = LoadLibraryA("d3dcompiler_47.dll");
        auto disasm = d3dc ? (Disasm_t)GetProcAddress(d3dc, "D3DDisassemble") : nullptr;
        if (!disasm) { LOGI("gpushader: D3DDisassemble unavailable"); return; }
        ID3DBlob* blob = nullptr;
        if (FAILED(disasm(code.data(), code.size(), 0, nullptr, &blob)) || !blob)
        { LOGI("gpushader: disassembly failed"); return; }
        static int seq = 0;
        std::wstring path = GameInfo::GameDir() + L"\\DVR_shader" + std::to_wstring(seq++) + L".txt";
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(h, blob->GetBufferPointer(), (DWORD)blob->GetBufferSize(), &written, nullptr);
            CloseHandle(h);
            LOGI("gpushader: {} bytes of bytecode for {} -> DVR_shader.txt ({} bytes of asm)", code.size(), vs, written);
        }
        blob->Release();
    }

    //! Vtables of the game's immediate context and of a deferred context on the same device.
    //! Deferred contexts are a different implementation class in d3d11.dll with their OWN vtable, and this engine
    //! records gameplay rendering into them: the immediate context saw 1.7M draws during the load screens and then
    //! exactly zero per frame while the game was demonstrably drawing. All deferred contexts share one vtable, so
    //! creating our own is enough to reach the ones the game uses.
    bool GetContextVTables(IDXGISwapChain* sc, void*** immediate, void*** deferred)
    {
        *immediate = *deferred = nullptr;
        ID3D11Device* dev = nullptr;
        if (!sc || FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev)
        { LOGI("GpuTrace: swapchain has no ID3D11Device"); return false; }
        ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
        if (ctx) { *immediate = *(void***)ctx; g_Immediate = ctx; }   // kept (owns a ref) for staging readbacks
        g_Device = dev; dev->AddRef();
        // The deferred context is KEPT (never released): the vtable is read through it and hooked afterwards, and with
        // SteamVR as the runtime the context handed back is not d3d11's own object (another layer wraps it) -- released
        // at once, its vtable was freed memory and every entry garbage (measured: 0x5F706D730072656C, i.e. ASCII text).
        static ID3D11DeviceContext* s_Def = nullptr;
        ID3D11DeviceContext* def = s_Def;
        HRESULT hr = def ? S_OK : dev->CreateDeferredContext(0, &def);
        if (SUCCEEDED(hr) && def) { s_Def = def; *deferred = *(void***)def; }
        else LOGI("GpuTrace: CreateDeferredContext failed hr=0x{:08X}", (unsigned)hr);
        g_RealCreateVS = (CreateVS_t)(*(void***)dev)[12];
        g_RealCreateIL = (CreateIL_t)(*(void***)dev)[11];      // ID3D11Device::CreateVertexShader
        g_RealCreatePS = (CreatePS_t)(*(void***)dev)[15];
        g_RealCreateCS = (CreateCS_t)(*(void***)dev)[18];
        dev->Release();
        return *immediate || *deferred;
    }
}

bool GpuTrace::Install(IDXGISwapChain* sc)
{
    if (g_Installed) return true;
    void** vtImm = nullptr; void** vtDef = nullptr;
    if (!GetContextVTables(sc, &vtImm, &vtDef)) return false;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (g_RealCreateVS && !Hookable((const void*)g_RealCreateVS, "CreateVertexShader")) g_RealCreateVS = nullptr;
    if (g_RealCreateIL && !Hookable((const void*)g_RealCreateIL, "CreateInputLayout")) g_RealCreateIL = nullptr;
    if (g_RealCreateVS) DetourAttach(&(PVOID&)g_RealCreateVS, HkCreateVertexShader);
    if (g_RealCreateIL) DetourAttach(&(PVOID&)g_RealCreateIL, HkCreateInputLayout);
    if (g_RealCreatePS && !Hookable((const void*)g_RealCreatePS, "CreatePixelShader")) g_RealCreatePS = nullptr;
    if (g_RealCreateCS && !Hookable((const void*)g_RealCreateCS, "CreateComputeShader")) g_RealCreateCS = nullptr;
    if (g_RealCreatePS) DetourAttach(&(PVOID&)g_RealCreatePS, HkCreatePixelShader);
    if (g_RealCreateCS) DetourAttach(&(PVOID&)g_RealCreateCS, HkCreateComputeShader);
    int nImm = Imm::Attach(vtImm, nullptr, 0);
    void* taken[eCount + kNumExtra] = {};
    if (vtImm)
    {
        for (int i = 0; i < eCount; i++) taken[i] = vtImm[kSlots[i]];
        for (int i = 0; i < kNumExtra; i++) taken[eCount + i] = vtImm[kExtraSlots[i]];
    }
    int nDef = Def::Attach(vtDef, taken, vtImm ? eCount + kNumExtra : 0);
    if (vtDef && vtImm)
    {   // command lists: which view the presented image was drawn from (see g_ExecSerial)
        g_RealFinishCL = (FinishCL_t)vtDef[114]; g_RealExecCL = (ExecCL_t)vtImm[58];
        if (Hookable((const void*)g_RealFinishCL, "FinishCommandList")) DetourAttach(&(PVOID&)g_RealFinishCL, HkFinishCL); else g_RealFinishCL = nullptr;
        if (Hookable((const void*)g_RealExecCL, "ExecuteCommandList")) DetourAttach(&(PVOID&)g_RealExecCL, HkExecCL); else g_RealExecCL = nullptr;
    }
    LONG r = DetourTransactionCommit();
    g_Installed = (r == NO_ERROR);
    g_Stats.installed = g_Installed;
    if (vtImm) for (int s : kHookedImmSlots) g_HookedFns.push_back(vtImm[s]);
    if (vtDef) for (int s : kHookedImmSlots) g_HookedFns.push_back(vtDef[s]);
    LOGI("GpuTrace: immediate vt={} [{}] ({} hooks), deferred vt={} [{}] ({} hooks), detour={}",
         (void*)vtImm, vtImm ? ModuleOf(vtImm) : "-", nImm, (void*)vtDef, vtDef ? ModuleOf(vtDef) : "-", nDef, r);
    return g_Installed;
}

//! The immediate context changes implementation when multithread protection is switched on (XRSession does, for the
//! keep-alive frames): D3D11 then calls a thread-safe set of functions that never reach the hooks put
//! on the first set. Measured 2026-10-23: from then on no ExecuteCommandList and no write of the TAA's PerViewCB was
//! seen -- the presented image was named one view late (the stereo eyes swapped) and per-eye TAA fell back to off.
//! Checked every Present (a few compares): a set of functions not hooked yet gets its own hooks (Hooks<0, 1>).
void GpuTrace::RehookImmediate()
{
    if (!g_Installed || !g_Immediate || g_ImmMTHooked) return;
    void** vt = *(void***)g_Immediate;
    if (!vt) return;
    bool fresh = false;
    for (int s : kHookedImmSlots)
        if (std::find(g_HookedFns.begin(), g_HookedFns.end(), vt[s]) == g_HookedFns.end()) { fresh = true; break; }
    if (!fresh) return;
    g_ImmMTHooked = true;      // one attempt: a third implementation is logged by the next check's absence of effect
    std::vector<void*> taken = g_HookedFns;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    const int n = ImmMT::Attach(vt, taken.data(), (int)taken.size());
    bool exec = false;
    if (std::find(taken.begin(), taken.end(), vt[58]) == taken.end() && Hookable(vt[58], "immediate (thread-safe) ExecuteCommandList"))
    { g_RealExecCLMT = (ExecCL_t)vt[58]; DetourAttach(&(PVOID&)g_RealExecCLMT, HkExecCLMT); exec = true; }
    const LONG r = DetourTransactionCommit();
    for (int s : kHookedImmSlots) g_HookedFns.push_back(vt[s]);
    LOGI("GpuTrace: the immediate context switched implementation (multithread protection): vt={} [{}], {} hooks + ExecuteCommandList {} added, detour={}",
         (void*)vt, ModuleOf(vt), n, exec ? 1 : 0, r);
}

void GpuTrace::Uninstall()
{
    if (!g_Installed) return;
    g_Recording = false;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (Imm::realDrawIndexed)         DetourDetach(&(PVOID&)Imm::realDrawIndexed, Imm::DrawIndexed);
    if (Imm::realDraw)                DetourDetach(&(PVOID&)Imm::realDraw, Imm::Draw);
    if (Imm::realDrawIdxInst)         DetourDetach(&(PVOID&)Imm::realDrawIdxInst, Imm::DrawIdxInst);
    if (Imm::realDrawInst)            DetourDetach(&(PVOID&)Imm::realDrawInst, Imm::DrawInst);
    if (Imm::realDrawIdxInstIndirect) DetourDetach(&(PVOID&)Imm::realDrawIdxInstIndirect, Imm::DrawIdxInstIndirect);
    if (Imm::realDrawInstIndirect)    DetourDetach(&(PVOID&)Imm::realDrawInstIndirect, Imm::DrawInstIndirect);
    if (Def::realDrawIndexed)         DetourDetach(&(PVOID&)Def::realDrawIndexed, Def::DrawIndexed);
    if (Def::realDraw)                DetourDetach(&(PVOID&)Def::realDraw, Def::Draw);
    if (Def::realDrawIdxInst)         DetourDetach(&(PVOID&)Def::realDrawIdxInst, Def::DrawIdxInst);
    if (Def::realDrawInst)            DetourDetach(&(PVOID&)Def::realDrawInst, Def::DrawInst);
    if (Def::realDrawIdxInstIndirect) DetourDetach(&(PVOID&)Def::realDrawIdxInstIndirect, Def::DrawIdxInstIndirect);
    if (Def::realDrawInstIndirect)    DetourDetach(&(PVOID&)Def::realDrawInstIndirect, Def::DrawInstIndirect);
    if (Imm::realMap)                 DetourDetach(&(PVOID&)Imm::realMap, Imm::Map);
    if (Imm::realUnmap)               DetourDetach(&(PVOID&)Imm::realUnmap, Imm::Unmap);
    if (Imm::realUpdateSub)           DetourDetach(&(PVOID&)Imm::realUpdateSub, Imm::UpdateSub);
    if (Def::realMap)                 DetourDetach(&(PVOID&)Def::realMap, Def::Map);
    if (Def::realUnmap)               DetourDetach(&(PVOID&)Def::realUnmap, Def::Unmap);
    if (Def::realUpdateSub)           DetourDetach(&(PVOID&)Def::realUpdateSub, Def::UpdateSub);
    if (Imm::realDispatch)            DetourDetach(&(PVOID&)Imm::realDispatch, Imm::Dispatch);
    if (Imm::realDispatchInd)         DetourDetach(&(PVOID&)Imm::realDispatchInd, Imm::DispatchInd);
    if (Imm::realCopyRegion)          DetourDetach(&(PVOID&)Imm::realCopyRegion, Imm::CopyRegion);
    if (Imm::realCopy)                DetourDetach(&(PVOID&)Imm::realCopy, Imm::Copy);
    if (Def::realDispatch)            DetourDetach(&(PVOID&)Def::realDispatch, Def::Dispatch);
    if (Def::realDispatchInd)         DetourDetach(&(PVOID&)Def::realDispatchInd, Def::DispatchInd);
    if (Def::realCopyRegion)          DetourDetach(&(PVOID&)Def::realCopyRegion, Def::CopyRegion);
    if (Def::realCopy)                DetourDetach(&(PVOID&)Def::realCopy, Def::Copy);
    DetourTransactionCommit();
    g_Installed = false;
}

void GpuTrace::RequestDump(const char* tag)
{
    std::lock_guard lk(g_M);
    g_Sig.clear(); g_Tag = tag ? tag : "dump";
    g_Pending = 600;      // ~10 s: the game stops drawing whenever the window loses focus, so wait for a real frame
    g_Recording = true;
}

namespace
{
    //! Read one captured constant buffer through a staging copy and log what looks like a joint palette.
    void DumpOneConstantBuffer(int slot, ID3D11Buffer* cb)
    {
        D3D11_BUFFER_DESC bd{}; cb->GetDesc(&bd);
        D3D11_BUFFER_DESC sd = bd;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0;
        sd.StructureByteStride = 0;      // must go with MiscFlags, or CreateBuffer rejects a structured buffer
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Buffer* staging = nullptr;
        if (FAILED(g_Device->CreateBuffer(&sd, nullptr, &staging)) || !staging)
        { LOGI("gpucb: slot {} size {} - staging buffer failed", slot, bd.ByteWidth); return; }
        g_Immediate->CopyResource(staging, cb);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_Immediate->Map(staging, 0, D3D11_MAP_READ, 0, &m)) && m.pData)
        {
            const float* f = (const float*)m.pData;
            size_t n = bd.ByteWidth / 4;
            // A bone matrix is packed as three float4 rows: (m00,m01,m02,tx)(m10,m11,m12,ty)(m20,m21,m22,tz),
            // so the rotation is NOT 9 contiguous floats — checking it that way finds nothing.
            auto ortho = [](const float* v) {
                for (int i = 0; i < 12; i++) if (!std::isfinite(v[i])) return false;
                for (int r = 0; r < 3; r++)
                {
                    const float* u = v + r * 4;
                    float q = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
                    if (q < 0.9f || q > 1.1f) return false;      // looser than the memory scan: bones may be scaled
                }
                return true;
            };
            int hits = 0;
            LOGI("gpucb: slot {} size {} floats {}", slot, bd.ByteWidth, n);
            for (size_t i = 0; i + 12 <= n && hits < 16; i += 4)  // constant buffers are float4-aligned
            {
                if (!ortho(f + i)) continue;
                hits++;
                LOGI("gpucb:   matrix @float{} t=({:.3f},{:.3f},{:.3f}) row0=({:.3f},{:.3f},{:.3f})",
                     i, f[i + 3], f[i + 7], f[i + 11], f[i], f[i + 1], f[i + 2]);
            }
            // Small buffers get dumped whole: the object transform is in here somewhere and eyeballing beats guessing.
            for (size_t i = 0; i < n && n <= 256; i += 4)
                LOGI("gpucb:   [float{:3}] {:9.3f} {:9.3f} {:9.3f} {:9.3f}", i,
                     f[i], i + 1 < n ? f[i + 1] : 0.f, i + 2 < n ? f[i + 2] : 0.f, i + 3 < n ? f[i + 3] : 0.f);
            // Big ones (a joint palette is 72 bones x 12 floats) go to a file, one float4 per line, so the packing
            // can be worked out offline instead of guessed at from a detector that assumes one.
            {
                std::wstring path = GameInfo::GameDir() + L"\\DVR_cb" + std::to_wstring(slot) + L".txt";
                if (FILE* fo = _wfopen(path.c_str(), L"w"))
                {
                    for (size_t i = 0; i < n; i += 4)
                        fprintf(fo, "%4zu %12.6f %12.6f %12.6f %12.6f\n", i, f[i], i + 1 < n ? f[i + 1] : 0.f, i + 2 < n ? f[i + 2] : 0.f, i + 3 < n ? f[i + 3] : 0.f);
                    fclose(fo);
                }
            }
            g_Immediate->Unmap(staging, 0);
        }
        else LOGI("gpucb: slot {} size {} - Map failed", slot, bd.ByteWidth);
        staging->Release();
    }

    void FlushConstantDump()
    {
        bool any = false;
        for (int i = 0; i < kMaxCB; i++) if (g_CapturedCB[i]) any = true;
        for (int i = 0; i < kMaxSRV; i++) if (g_CapturedSRV[i]) any = true;
        if (!any || !g_Device || !g_Immediate) return;
        for (int i = 0; i < kMaxCB; i++)
        {
            if (!g_CapturedCB[i]) continue;
            DumpOneConstantBuffer(i, g_CapturedCB[i]);
            g_CapturedCB[i]->Release(); g_CapturedCB[i] = nullptr;
        }
        for (int i = 0; i < kMaxSRV; i++)
        {
            if (!g_CapturedSRV[i]) continue;
            ID3D11Resource* res = nullptr; g_CapturedSRV[i]->GetResource(&res);
            ID3D11Buffer* buf = nullptr;
            if (res) res->QueryInterface(__uuidof(ID3D11Buffer), (void**)&buf);
            if (buf) { LOGI("gpucb: --- VS SRV slot {} is a buffer ---", i); DumpOneConstantBuffer(100 + i, buf); buf->Release(); }
            else LOGI("gpucb: VS SRV slot {} is not a buffer (texture)", i);
            if (res) res->Release();
            g_CapturedSRV[i]->Release(); g_CapturedSRV[i] = nullptr;
        }
        LOGI("gpucb: end");
    }
}

namespace
{
    //! Refresh the CPU-side copy of the view constant buffer and make sure our patched buffer exists.
    //! One frame stale, which is invisible for a hand-held object and avoids shadowing every Map/Unmap.
    ID3D11Buffer* g_Staging = nullptr;      // reused: creating one per frame is itself a stall
    int g_RefreshCountdown = 0;

    void RefreshShadow()
    {
        if (!g_Device || !g_Immediate || !g_ViewCB) { g_WantShadow = true; return; }
        // CopyResource + Map(READ) on the immediate context is a full CPU-GPU sync. Doing it every frame stalls the
        // game badly, so refresh rarely � the view matrix only matters here as a base to offset from.
        if (--g_RefreshCountdown > 0) return;
        g_RefreshCountdown = 60;
        D3D11_BUFFER_DESC bd{}; g_ViewCB->GetDesc(&bd);
        if (!g_Staging)
        {
            D3D11_BUFFER_DESC sd = bd;
            sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0; sd.StructureByteStride = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(g_Device->CreateBuffer(&sd, nullptr, &g_Staging))) { g_Staging = nullptr; return; }
        }
        g_Immediate->CopyResource(g_Staging, g_ViewCB);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_Immediate->Map(g_Staging, 0, D3D11_MAP_READ, 0, &m)) && m.pData)
        {
            size_t floats = bd.ByteWidth / 4; if (floats > kMaxCBFloats) floats = kMaxCBFloats;
            { std::lock_guard lk(g_ShadowM); memcpy(g_Shadow, m.pData, floats * sizeof(float)); g_ShadowValid = true; }
            g_ShadowFloats.store(floats);
            g_Immediate->Unmap(g_Staging, 0);
        }
        g_WantShadow = true;      // re-grab the source buffer, the engine may cycle through several
    }

    // A fresh IMMUTABLE buffer per frame, retired after a few frames.
    // Not DYNAMIC + Map(WRITE_DISCARD): discarding on the immediate context a buffer that pending deferred command
    // lists still reference is exactly the renaming hazard D3D11 forbids, and it crashed inside nvwgf2umx.dll �
    // measured, and reproducible even with an offset small enough to change no pixel, which is what pinned the fault
    // on the mechanism rather than on the values. An immutable buffer is read-only, so any number of contexts may
    // reference it safely; we just must not free it while a recorded command list can still run.
    constexpr int kRetire = 8;
    ID3D11Buffer* g_Ring[kRetire] = {};
    int g_RingIdx = 0;

    void UpdatePatchedBuffer()
    {
        if (!g_Device) return;
        size_t n = g_ShadowFloats.load();
        if (!n) return;
        float data[kMaxCBFloats];
        {
            std::lock_guard lk(g_ShadowM);
            if (!g_ShadowValid) return;
            memcpy(data, g_Shadow, n * sizeof(float));
        }
        const float dx = g_ViewOff[0].load(), dy = g_ViewOff[1].load(), dz = g_ViewOff[2].load();
        if (kViewMatrixB + 15 < n)
            for (size_t base : { kViewMatrixA, kViewMatrixB })
            {   // view-space translation row of each view matrix (slot 1 layout)
                data[base + 12] += dx; data[base + 13] += dy; data[base + 14] += dz;
            }
        int zf = g_ZeroFrom.load(), zc = g_ZeroCount.load();
        if (zf >= 0 && zc > 0)
            for (int i = zf; i < zf + zc && i < (int)n; i++) data[i] = 0.f;
        g_RingIdx = (g_RingIdx + 1) % kRetire;
        if (g_Ring[g_RingIdx]) { g_Ring[g_RingIdx]->Release(); g_Ring[g_RingIdx] = nullptr; }   // kRetire frames old
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (UINT)(n * sizeof(float));
        bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{}; init.pSysMem = data;
        ID3D11Buffer* buf = nullptr;
        if (FAILED(g_Device->CreateBuffer(&bd, &init, &buf)) || !buf)
        { LOGI("GpuTrace: could not create the patched view buffer"); return; }
        g_Ring[g_RingIdx] = buf;
        static bool once = false;
        if (!once) { once = true; LOGI("GpuTrace: patched view buffer ring live (first {})", (void*)buf); }
        g_PatchedCB.store(buf);
    }
}

void GpuTrace::OnPresent()
{
    RehookImmediate();
    if (g_CapturedVS) { DumpShader(g_CapturedVS); g_CapturedVS = nullptr; }
    FlushConstantDump();
    CreatePools();          // on one thread, outside any draw: never create resources inside the draw hook
    CreatePalPool();
    TickIdentify();
    TickArmIdentify();
    {   // for the empty hand's pose (RestPass, render thread = this thread). Standing still is read from the player's
        // own speed: "Idle" never reports running (its per-tick slot is shared with a dozen other states, and
        // "Running" reports running standing still), so the state machine cannot tell standing from walking.
        static double lastT = 0; static float lastOrg[3]; static bool haveOrg = false; static double stillSince = 0, sprintSince = 0;
        const double now = (double)GetTickCount64() * 1e-3;
        BodyHide::Anchor an[BodyHide::aCount]; BodyHide::GetAnchors(an);
        const BodyHide::Anchor& pl = an[BodyHide::aPlayer];
        if (!pl.valid) { stillSince = now; haveOrg = false; }
        else if (now - lastT >= 0.1)
        {
            if (haveOrg)
            {
                const float d[3] = { pl.org[0] - lastOrg[0], pl.org[1] - lastOrg[1], pl.org[2] - lastOrg[2] };
                const float speed = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / (float)(now - lastT);
                g_PlayerSpeed = speed;
                if (speed > 0.05f) stillSince = now;
            }
            memcpy(lastOrg, pl.org, 12); lastT = now; haveOrg = true;
        }
        if (!PlayerState::Running("Sprinting")) sprintSince = now;
        g_StillMs = (unsigned long long)((now - stillSince) * 1000.0);
        g_SprintMs = (unsigned long long)((now - sprintSince) * 1000.0);
        XRInputState in;
        if (VRRuntime::GetInput(in))
        {
            g_GripNow[0] = in.gripL; g_GripNow[1] = in.gripR;
            for (int h = 0; h < 2; h++)
            {   // smoothed a little (hand tracking twitches by a few degrees)
                g_FingerSrc[h] = in.fingerSrc[h];
                for (int f = 0; f < 5; f++) { const float o = g_FingerCurl[h][f].load(); g_FingerCurl[h][f] = o + (in.fingerCurl[h][f] - o) * 0.5f; }
            }
        }
    }
    FlushVbDump();
    AnalyseArmMesh();
    if (g_SaveQPending.exchange(false)) SaveHandQ();
    {   // the weapon grips: may they be learned this frame, the file once, a new grip written
        static bool loaded = false;
        if (!loaded) { loaded = true; LoadGrips(); }
        GripCleanTick();
        // (UsePower too: Shadow Walk's claws can be on screen the frame its cast starts, before ShadowWalk reports)
        // (not the menu screen's suspend: the arms there are the player's own, drawn as the flat game draws them)
        if ((g_AnimSuspend.load() && !ScreenMode::FlatRender()) || PlayerState::Running("ShadowWalk") || PlayerState::Running("ShadowHide") || PlayerState::Running("UsePower") ||
            PlayerState::Running("EnterPossessedHost")) { if (g_ArmForeignOn.load()) g_ArmForeignUntil = g_PresentFrame + 135; }
        if (g_GripSavePending.exchange(false)) SaveGrips();
    }
    if (g_CensusLeft.load() > 0 && g_CensusLeft.fetch_sub(1) == 1)
    {
        std::lock_guard lk(g_CensusM);
        std::sort(g_Census.begin(), g_Census.end(), [](const CensusRow& a, const CensusRow& b) { return a.n + a.own > b.n + b.own; });
        std::string t = fmt::format("projcensus over {} frames: {} distinct projections\n", g_CensusFrames, g_Census.size());
        for (const auto& r : g_Census)
            t += fmt::format("  fx {:.4f} fy {:.4f} (fov {:.2f} x {:.2f} deg) z {:.5f} {:.5f}  draws {} own {}  origin {:.2f}..{:.2f} m\n",
                             r.fx, r.fy, 2 * atanf(1.f / r.fx) * 57.29578f, 2 * atanf(1.f / r.fy) * 57.29578f, r.z2, r.z3,
                             r.n, r.own, r.n ? r.dmin : 0.f, r.n ? r.dmax : 0.f);
        g_CensusText = t;
        LOGI("{}", t);
    }
    {
        std::vector<std::string> lines;
        { std::lock_guard lk(g_RigM); lines.swap(g_JitLines); }
        if (!lines.empty()) { std::string t; for (auto& l : lines) { t += l; t += '\n'; } LOGI("handjit trace, {} draws:\n{}", lines.size(), t); }
    }
    if (g_TraceLeft.load() > 0)
    {   // vr handtrace: one line every g_TraceEvery frames
        HandTraceS s[2]; { std::lock_guard lt(g_TraceM); s[0] = g_TraceS[0]; s[1] = g_TraceS[1]; }
        const bool refNow = g_TraceRefReq.exchange(false);
        for (int h = 0; h < 2; h++)
            if (s[h].valid && g_PresentFrame - s[h].frame <= 3 && (refNow || !g_TraceRefValid[h]))
            {
                g_TraceRefD[h] = s[h].relD; g_TraceRefA[h] = s[h].relA; g_TraceRefValid[h] = true; g_TraceFingReq[h] = true;
                std::lock_guard lk(g_ArmBonesM); g_TraceRefHold[h] = g_HandHold[h].h;
            }
        if ((g_PresentFrame % (unsigned)std::max(1, g_TraceEvery.load())) == 0)
        {
            std::string st;
            for (const char* n : { "Sneaking", "AutoCrouch", "WallCrouch", "Sprinting", "Sliding", "Jumping", "InAir", "Falling", "CatFall", "Mantle", "Vault", "Climb", "SwimMantle",
                                   "ShadowWalk", "Slash", "Block", "Draw", "Undraw", "EquipChange", "ReloadWeapon", "FireWeapon", "Recoil", "Leaning", "Peeking", "Choke", "Assassination", "UsePower", "Blinking", "CarryCorpse", "HoldObject", "Interact" })
                if (PlayerState::Running(n)) { if (!st.empty()) st += ' '; st += n; }
            std::string t = fmt::format("htrace f {} spd {:.2f} anim {} st [{}]", g_PresentFrame, g_PlayerSpeed.load(), g_AnimSuspend.load() ? 1 : 0, st);
            for (int h = 0; h < 2; h++)
            {
                if (!s[h].valid || g_PresentFrame - s[h].frame > 3) { t += h ? " | R -" : " | L -"; continue; }
                HandHold hh; { std::lock_guard lk(g_ArmBonesM); hh = g_HandHold[h]; }
                float ws = 0, es = 0; int hb = 0;
                { std::lock_guard lk(g_RigM); ws = g_Rig[h].wrist.s; es = g_Rig[h].elbow.s; for (int b = 0; b < g_Seg[h].bones && b < 256; b++) hb += g_Seg[h].seg[b] == sgHand; }
                t += fmt::format(" | {} {} w{} ch{} gr{} cl{} drawn {:.1f}d {:.1f}cm anim {:.1f}d {:.1f}cm hold{} {:.1f}d {:.1f}cm fing {:.1f}/{:.1f}cm wrist {:.3f} elbow {:.3f} hb {}",
                                 h ? 'R' : 'L', s[h].weapon ? "WPN" : s[h].gadget ? "GDG" : "emp", s[h].whits, s[h].crouchHold ? 1 : 0, s[h].grip ? 1 : 0,
                                 g_GripClean[h].load() ? std::string("1") : std::string("0:") + g_GripDirty[h].load(),
                                 RotAngleDeg(s[h].relD, g_TraceRefD[h]), PosDiffCm(s[h].relD, g_TraceRefD[h]),
                                 s[h].haveA ? RotAngleDeg(s[h].relA, g_TraceRefA[h]) : -1.f, s[h].haveA ? PosDiffCm(s[h].relA, g_TraceRefA[h]) : -1.f,
                                 hh.valid ? 1 : 0, RotAngleDeg(hh.h, g_TraceRefHold[h]), PosDiffCm(hh.h, g_TraceRefHold[h]),
                                 g_TraceFingMax[h].load(), g_TraceFingMean[h].load(), ws, es, hb);
            }
            LOGI("{}", t);
        }
        g_TraceLeft--;
    }
    for (int h = 0; h < 2; h++) g_HandOwnDelta[h].store(g_HandOwnDeltaLive[h].exchange(0));
    for (int h = 0; h < 2; h++) g_FullDraws[h].store(g_FullDrawsLive[h].exchange(0));
    g_BodyDraws.store(g_BodyDrawsLive.exchange(0)); g_FullNoTable.store(g_FullNoTableLive.exchange(0)); g_FullNoFrame.store(g_FullNoFrameLive.exchange(0));
    FlushFind();
    for (int h = 0; h < 2; h++) g_ArmHits[h].store(g_ArmHitsLive[h].exchange(0));
    g_ArmUnknown.store(g_ArmUnknownLive.exchange(0));
    for (int h = 0; h < 2; h++)
    {   // one frame's worth of the entity test, latched so the heartbeat reads a whole frame and not a partial one
        const int n = g_WeaponHitsLive[h].exchange(0);
        g_WeaponHits[h].store(n);
        if (n > g_WeaponHitsPeak[h].load()) g_WeaponHitsPeak[h].store(n);
    }
    if (ViewOffsetOn()) { RefreshShadow(); UpdatePatchedBuffer(); }
    static unsigned long long lastDraws = 0;
    unsigned long long now = g_Stats.draws.load();
    unsigned long long perFrame = now - lastDraws; lastDraws = now;
    { std::lock_guard lk(g_M); g_Stats.frames++; g_LastPerFrame = perFrame; }
    if (!g_Recording.load()) return;
    std::lock_guard lk(g_M);
    // Not "one frame": the request arrives from inside Present, so the first boundary we see can be the same one,
    // with nothing recorded yet. Keep recording until draws show up (bounded, so it always terminates).
    if (g_Sig.empty() && --g_Pending > 0) return;
    g_Recording = false;
    std::string counts;
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < eCount; i++)
            if (g_Counts[k][i].load()) counts += fmt::format(" {}{}={}", k ? "def." : "imm.", kNames[i], g_Counts[k][i].load());
    LOGI("gpudump '{}': {} signatures, drawsThisFrame={}, totals:{}", g_Tag, g_Sig.size(), g_LastPerFrame, counts);
    std::map<void*, uint64_t> hashes;
    { std::lock_guard lk2(g_ShaderM); hashes = g_VsHash; }
    for (auto& e : g_Sig)
    {
        auto h = hashes.find(e.first.first);
        const unsigned km = g_SigKinds[e.first];
        std::string kinds;
        for (int k = 0; k < 2; k++) for (int i = 0; i < eCount; i++) if (km & (1u << (i + 8 * k))) kinds += fmt::format("{}{} ", k ? "def." : "imm.", kNames[i]);
        LOGI("gpudump '{}': vs={} hash={:016x} idx={} n={} {}", g_Tag, e.first.first, h != hashes.end() ? h->second : 0ull, e.first.second, e.second, kinds);
    }
    g_SigKinds.clear();
    LOGI("gpudump '{}': end", g_Tag);
}

void GpuTrace::SetTargets(const std::vector<unsigned>& idx)
{
    std::string s;
    for (int i = 0; i < kMaxTargets; i++)
    {
        const unsigned v = i < (int)idx.size() ? idx[i] : 0u;
        g_Target[i] = v;
        if (v) s += " " + std::to_string(v);
    }
    LOGI("GpuTrace: target index counts{}", s.empty() ? " (none)" : s);
}
//! Load the automatically identified weapon draws into the target set (so gpuskip can prove what they are).
void GpuTrace::TargetsFromWeapon()
{
    std::vector<unsigned> v;
    { std::lock_guard lk(g_WeaponM); for (UINT i : g_WeaponIdx) v.push_back(i); }
    SetTargets(v);
}

void GpuTrace::SetSkip(bool on) { g_SkipDraws = on; LOGI("GpuTrace: skip target draws {}", on); }

void GpuTrace::SetSkipVsHashes(const std::vector<uint64_t>& hashes)
{
    for (auto& s : g_SkipHash) s = 0;
    for (auto& p : g_SkipVsPtr) p = nullptr;
    int n = 0;
    for (uint64_t h : hashes) if (h && n < kMaxSkipVs) g_SkipHash[n++] = h;
    int live = 0;
    {
        std::lock_guard lk(g_ShaderM);
        for (auto& e : g_VsHash) for (int i = 0; i < n; i++) if (e.second == g_SkipHash[i].load()) { AddSkipPtr(e.first); live++; break; }
    }
    g_SkipVsOn = n > 0;
    LOGI("GpuTrace: hiding the draws of {} vertex shader hash(es), {} live shader(s) matched", n, live);
}
unsigned long long GpuTrace::SkipVsHits() { return g_SkipVsHits.load(); }
void GpuTrace::SetNoTrail(bool on) { g_NoTrail = on; }
bool GpuTrace::NoTrail() { return g_NoTrail; }
void GpuTrace::SetTrailWindow(bool active) { g_TrailWindow = active; }
std::string GpuTrace::TrailStats()
{
    int live = 0; for (auto& p : g_TrailVsPtr) if (p.load()) live++;
    return fmt::format("{} shaders {} window {} hidden {}", g_NoTrail.load() ? "on" : "off", live, g_TrailWindow.load() ? 1 : 0, g_TrailHits.load());
}

void GpuTrace::DumpShaderByHash(uint64_t hash)
{
    void* vs = nullptr;
    {
        std::lock_guard lk(g_ShaderM);
        for (auto& e : g_VsHash) if (e.second == hash) { vs = e.first; break; }
        if (!vs) for (auto& e : g_PsCsHash) if (e.second == hash) { vs = e.first; break; }   // pixel / compute shaders too
    }
    if (!vs) { LOGI("gpushader: no vertex shader with hash {:016x}", hash); return; }
    LOGI("gpushader: hash {:016x} is vs {}", hash, vs);
    DumpShader(vs);
}
void GpuTrace::RequestShaderDump() { g_WantShader = true; LOGI("GpuTrace: will disassemble the next target draw's vertex shader"); }

void GpuTrace::SetSlot(int slot)
{ g_Slot = slot; g_ShadowValid = false; g_ShadowFloats = 0; g_WantShadow = true; LOGI("GpuTrace: patch VS constant slot {}", slot); }

void GpuTrace::SetZeroRange(int from, int count)
{ g_ZeroFrom = from; g_ZeroCount = count; LOGI("GpuTrace: zero floats [{}, {})", from, from + count); }

void GpuTrace::SetBind(bool on) { g_BindEnabled = on; LOGI("GpuTrace: draw-hook binding {}", on); }

void GpuTrace::SetViewOffset(float x, float y, float z)
{ g_ViewOff[0] = x; g_ViewOff[1] = y; g_ViewOff[2] = z; g_WantShadow = true; LOGI("GpuTrace: weapon view offset {} {} {}", x, y, z); }

void GpuTrace::RequestConstantDump() { g_WantConstants = true; LOGI("GpuTrace: will capture VS constants of the next target draw"); }

unsigned long long GpuTrace::Draws() { return g_Stats.draws.load(); }
unsigned long long GpuTrace::Frames() { std::lock_guard lk(g_M); return g_Stats.frames; }
bool GpuTrace::Installed() { return g_Stats.installed; }

void GpuTrace::SetSixDof(int mode)
{
    g_SixDof = mode;
    if (mode) g_CalReq = true;
    { std::lock_guard lk(g_XfM); g_XfSerial = 0; g_XfValid = false; g_XfPrevValid = false; g_XfValidPub = false; g_XfSerialPub = 0; }
    LOGI("gpu6dof: mode {} ({})", mode, mode == 0 ? "off" : mode == 1 ? "follow the controller" : "identity rebuild");
}
int  GpuTrace::SixDof() { return g_SixDof.load(); }
void GpuTrace::Calibrate() { g_CalReq = true; LOGI("gpu6dof: re-anchoring the weapon on the controller"); }
void GpuTrace::SetGrip(int hand, float yawDeg, float pitchDeg, float rollDeg)
{
    for (int h = 0; h < 2; h++)
        if (hand < 0 || h == (hand & 1)) { g_GripYaw[h] = yawDeg; g_GripPitch[h] = pitchDeg; g_GripRoll[h] = rollDeg; }
    LOGI("gpu6dof: grip rotation hand {} yaw {} pitch {} roll {}", hand < 0 ? "both" : (hand & 1 ? "R" : "L"), yawDeg, pitchDeg, rollDeg);
}
void GpuTrace::SetHandGrip(int hand, float yawDeg, float pitchDeg, float rollDeg)
{
    for (int h = 0; h < 2; h++)
        if (hand < 0 || h == (hand & 1)) { g_HandGripYaw[h] = yawDeg; g_HandGripPitch[h] = pitchDeg; g_HandGripRoll[h] = rollDeg; }
    LOGI("gpuhands: hand grip (power hand) {} yaw {} pitch {} roll {}", hand < 0 ? "both" : (hand & 1 ? "R" : "L"), yawDeg, pitchDeg, rollDeg);
}
void GpuTrace::SetHandOffset(int hand, float r, float u, float f)
{
    for (int h = 0; h < 2; h++)
        if (hand < 0 || h == (hand & 1)) { g_HandOffR[h] = r; g_HandOffU[h] = u; g_HandOffF[h] = f; }
    LOGI("gpuhands: hand trim (power hand) {} right {} up {} fwd {}", hand < 0 ? "both" : (hand & 1 ? "R" : "L"), r, u, f);
}
void GpuTrace::HandGripOf(int hand, float ypr[3], float off[3])
{
    const int h = hand & 1;
    ypr[0] = g_HandGripYaw[h].load(); ypr[1] = g_HandGripPitch[h].load(); ypr[2] = g_HandGripRoll[h].load();
    off[0] = g_HandOffR[h].load(); off[1] = g_HandOffU[h].load(); off[2] = g_HandOffF[h].load();
}
void GpuTrace::RequestHandRelLog(int lines) { g_RelLogLeft = lines > 0 ? lines : 0; }
void GpuTrace::HandTrace(int frames, int every) { g_TraceEvery = every > 0 ? every : 6; g_TraceLeft = frames > 0 ? frames : 0; LOGI("handtrace: {} frames, a line every {}", frames, g_TraceEvery.load()); }
void GpuTrace::SetPrevPerEye(bool on) { g_PrevPerEye = on; }
unsigned long long GpuTrace::EyeFixedDraws() { return g_EyeFixed.load(); }
std::string GpuTrace::XfViewStats() { return fmt::format("moved draws placed with their own view {} another view {} (older by 0:{} 1:{} 2:{} 3+:{}) unknown {}", g_XfSame.load(), g_XfOther.load(), g_XfLag[0].load(), g_XfLag[1].load(), g_XfLag[2].load(), g_XfLag[3].load(), g_XfUnknown.load()); }
void GpuTrace::SetArmForeign(int on) { g_ArmForeignOn = on; if (!on) g_ArmForeignUntil = 0; LOGI("armforeign: {}", on ? "no arm analysis while the arms are the game's (Shadow Walk, takedowns)" : "OLD: the arm meshes are analysed during Shadow Walk too"); }
void GpuTrace::SetWeaponGrip(int mode) { g_GripMode = mode; LOGI("weapongrip: mode {} ({})", mode, mode == 1 ? "the learned grip, always" : mode == 2 ? "learn only, the animation drawn" : "the animation; crouched, the standing hold (old)"); }
void GpuTrace::WeaponGripForget(bool file)
{
    { std::lock_guard lk(g_GripM); for (auto& g : g_Grips) { g.valid = false; g.session = false; g.fing = FistPose(); g.fingSession = false; } g_GripTrack[0].n = g_GripTrack[1].n = 0; }
    if (file) { _wremove(GripPath().c_str()); LOGI("weapongrip: every grip forgotten, DVR_grips.txt deleted"); }
    else LOGI("weapongrip: this session's grips forgotten (relearned at the next steady moment)");
}
std::string GpuTrace::WeaponGripStats()
{
    std::string t = fmt::format("mode {}", g_GripMode.load());
    {
        std::lock_guard lk(g_GripM);
        int n = 0; for (auto& g : g_Grips) n += g.valid;
        t += fmt::format(" grips {}", n);
        for (auto& g : g_Grips) if (g.valid) t += fmt::format(" [{} {} {}b {}v{}{}]", g.side ? 'R' : 'L', GripKindName(g.cls), g.bones, g.verts, g.session ? " session" : " file", g.fing.valid ? " fing" : "");
    }
    t += fmt::format(" foreignskip {}", g_ArmForeignSkipped.load());
    for (int h = 0; h < 2; h++)
        t += fmt::format(" | {} clean {} used {} fing {} learned {} steady {}", h ? 'R' : 'L', g_GripClean[h].load() ? std::string("1") : std::string("0 ") + g_GripDirty[h].load(),
                         g_GripUsed[h].load(), g_GripFingUsed[h].load(), g_GripLearned[h].load(), [&] { std::lock_guard lk(g_GripM); return g_GripTrack[h].n; }());
    return t;
}
void GpuTrace::HandTraceRef() { g_TraceRefReq = true; if (g_TraceLeft.load() <= 0) g_TraceLeft = 10; }
void GpuTrace::SetUseHandHold(int on)
{
    g_UseHold = on;
    LOGI("gpuhands: power hand posed from {}", on ? "the hold learned while a weapon was held" : "the socket constant (the old way)");
}
int GpuTrace::UseHandHold() { return g_UseHold.load(); }
void GpuTrace::SetCrouchHold(int on) { g_CrouchHoldOn = on; LOGI("gpuhands: crouched weapon hand {}", on ? "keeps the standing hold" : "follows the animation (old)"); }
void GpuTrace::SetSixDofHand(int hand) { g_SixHand = hand; LOGI("gpu6dof: hand {}", hand); }
void GpuTrace::SetSixDofOffset(int hand, float r, float u, float f)
{
    for (int h = 0; h < 2; h++)
        if (hand < 0 || h == (hand & 1)) { g_OffR[h] = r; g_OffU[h] = u; g_OffF[h] = f; }
    LOGI("gpu6dof: trim hand {} right {} up {} fwd {}", hand < 0 ? "both" : (hand & 1 ? "R" : "L"), r, u, f);
}
void GpuTrace::SetArmCbDump(int draws) { g_ArmCbLeft = draws; }
void GpuTrace::SetGadget(int on, float palmMax) { g_GadgetOn = on; if (palmMax > 0.f) g_GadgetPalmMax = palmMax; LOGI("gpuhands: gadgets {} (entity within {:.2f} m of the palm)", on == 2 ? "in the anatomical hand" : on == 1 ? "aimed like weapons" : "off", g_GadgetPalmMax.load()); }
void GpuTrace::SetDiag(int draws) { g_DiagLeft = draws; LOGI("gpu6dof: will log the factorisation of {} target draws", draws); }
void GpuTrace::SetBasis(int mask) { g_Basis = mask; LOGI("gpu6dof: view basis sign mask {}", mask); }
void GpuTrace::SetNear(float metres) { g_Near = metres; LOGI("gpu6dof: pick draws within {} m of the eye", metres); }
float GpuTrace::Near() { return g_Near.load(); }
void GpuTrace::IdentifyWeapon()
{
    if (g_IdMode.load() == 1)
    {   // nothing to identify: under the entity test every draw is classified as it is drawn.
        LOGI("gpuweapon: identification is by equipment entity (gpuid 1); the scan is not needed. "
             "Use 'gpuid 0' first if you really want the old scan diff.");
        return;
    }
    { std::lock_guard lk(g_WeaponM); g_IdSet.clear(); g_IdSetA.clear(); }
    g_CollectLeft = 10; g_IdWait = 2; g_IdPhase = idScanA;
    LOGI("gpuweapon: identifying the weapon's draws (brief flicker)");
}
void GpuTrace::SetOnlyWeapon(bool on) { g_OnlyWeapon = on; LOGI("gpu6dof: move only the weapon's draws {}", on); }
void GpuTrace::SetIdMode(int mode) { g_IdMode = mode; LOGI("gpu6dof: weapon identification = {}", mode == 1 ? "entity anchors" : "scan diff"); }
int  GpuTrace::IdMode() { return g_IdMode.load(); }
void GpuTrace::SetAnchorRadius(float m) { g_AnchorRadius = m; LOGI("gpu6dof: anchor radius {} m", m); }
void GpuTrace::SetHoldMode(int mode)
{
    g_HoldMode = mode;
    { std::lock_guard lk(g_XfM); g_RestModelValid[0] = g_RestModelValid[1] = false; }   // re-anchor with the new rule
    LOGI("gpu6dof: hold = {}", mode == 1 ? "controller-aligned (deterministic)" : "captured at anchor time");
}
int GpuTrace::HoldMode() { return g_HoldMode.load(); }
bool GpuTrace::WeaponTargetPose(int hand, float org[3], float axis[9])
{
    const int h = hand & 1;
    if (g_AnimSuspend.load()) return false;     // the weapon is where the takedown animation puts it, not in the hand
    std::lock_guard lk(g_XfM);
    if (!g_TargetValid[h]) return false;
    const Rigid& t = g_TargetPose[h];
    memcpy(org, t.t, 12);
    // Rigid keeps the basis in its COLUMNS (fwd/left/up); an idMat3 wants them as rows.
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) axis[j * 3 + i] = t.r[i * 3 + j];
    return true;
}

bool GpuTrace::WeaponTargetPoseRaw(int hand, float org[3], float axis[9])
{
    const int h = hand & 1;
    if (g_AnimSuspend.load()) return false;
    std::lock_guard lk(g_XfM);
    if (!g_TargetValid[h]) return false;
    const Rigid& t = g_TargetPose[h];
    for (int i = 0; i < 3; i++) org[i] = t.t[i] - g_CollOff[h][i];       // a pure translation: the item moved with the hand
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) axis[j * 3 + i] = t.r[i * 3 + j];
    return true;
}
bool GpuTrace::RawItemPose(int hand, float org[3], float axis[9], void** ent)
{
    const int h = hand & 1;
    if (g_AnimSuspend.load() || g_SixDof.load() != 1) return false;
    std::lock_guard lk(g_XfM);
    const BodyHide::Anchor& wa = g_Anchor[h == 0 ? BodyHide::aWeaponL : BodyHide::aWeaponR];
    if (!g_TargetValid[h] || !g_RestModelValid[h] || !wa.valid) return false;
    const Rigid& t = g_TargetPose[h];
    for (int i = 0; i < 3; i++) org[i] = t.t[i] - g_CollOff[h][i];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) axis[j * 3 + i] = t.r[i * 3 + j];
    if (ent) *ent = wa.ent;
    return true;
}
void GpuTrace::SetCarry(int hand)
{
    std::lock_guard lk(g_CarryM);
    g_Carry = CarryState{}; g_Carry.on = true; g_Carry.hand = hand & 1;
    g_CarryDhLog = 8;
    LOGI("carry depth: so far {} carried draws given the arms' depth, {} without an arm match, {} without the variable",
         g_CarryDhSet.load(), g_CarryDhNone.load(), g_CarryDhNoVar.load());
}
void GpuTrace::ReturnCarry()
{
    std::lock_guard lk(g_CarryM);
    if (!g_Carry.on || g_Carry.returning) return;
    g_Carry.returning = true; g_Carry.retStart = CarryNow(); g_Carry.retFrom = g_Carry.last;
}
void GpuTrace::EndCarry() { std::lock_guard lk(g_CarryM); g_Carry = CarryState{}; }
unsigned long long GpuTrace::CarryDraws() { return g_CarryDraws.load(); }
bool GpuTrace::RawHandPose(int hand, float org[3], float rot[9])
{
    const int h = hand & 1;
    if (g_AnimSuspend.load() || g_SixDof.load() != 1) return false;
    std::lock_guard lk(g_XfM);
    if (!g_XfValid || !g_TargetValid[h]) return false;
    memcpy(org, g_HandRaw[h].t, 12); memcpy(rot, g_HandRaw[h].r, 36);
    return true;
}

int GpuTrace::WeaponClaim(const float w[3])
{
    if (!g_ClaimValid.load() || g_IdMode.load() != 1) return -1;
    BodyHide::Anchor a[BodyHide::aCount];
    for (int tries = 0; tries < 4; tries++)
    {
        const unsigned s0 = g_ClaimSeq.load();
        if (s0 & 1) continue;
        memcpy(a, g_ClaimAnch, sizeof a);
        if (g_ClaimSeq.load() != s0) continue;
        float d = 0.f; const int k = NearestAnchor(a, w, &d, nullptr);
        if ((k != BodyHide::aWeaponL && k != BodyHide::aWeaponR) || d > g_AnchorRadius.load()) return -1;   // the weapon test's rule
        return k == BodyHide::aWeaponL ? 0 : 1;
    }
    return -1;
}
bool GpuTrace::WeaponFromHand(int hand, const float horg[3], const float haxis[9], float org[3], float axis[9], void** ent)
{
    const int h = hand & 1;
    if (g_AnimSuspend.load() || g_SixDof.load() != 1) return false;
    Rigid hw;
    for (int i = 0; i < 3; i++) { hw.r[i * 3] = haxis[i]; hw.r[i * 3 + 1] = haxis[3 + i]; hw.r[i * 3 + 2] = haxis[6 + i]; hw.t[i] = horg[i]; }
    Rigid t;
    {
        std::lock_guard lk(g_XfM);
        const BodyHide::Anchor& wa = g_Anchor[h == 0 ? BodyHide::aWeaponL : BodyHide::aWeaponR];
        if (!g_TargetValid[h] || !g_RestModelValid[h] || !wa.valid) return false;
        t = Mul(Mul(hw, g_GripBase[h]), GripOffset(h));      // exactly what EnsureTransform draws the weapon at
        if (ent) *ent = wa.ent;
    }
    memcpy(org, t.t, 12);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) axis[j * 3 + i] = t.r[i * 3 + j];
    return true;
}

std::string GpuTrace::WeaponDraws()
{
    if (g_IdMode.load() == 1)
        return fmt::format("entity L {} (peak {}) R {} (peak {})",
                           g_WeaponHits[0].load(), g_WeaponHitsPeak[0].load(),
                           g_WeaponHits[1].load(), g_WeaponHitsPeak[1].load());
    std::lock_guard lk(g_WeaponM);
    std::string s = fmt::format("{}", g_WeaponIdx.size());
    for (UINT i : g_WeaponIdx) s += " " + std::to_string(i);
    return s;
}

void GpuTrace::RequestFind(float metres, int frames)
{
    g_FindDist = metres;
    { std::lock_guard lk(g_FindM); g_Find.clear(); }
    g_FindLeft = frames > 0 ? frames : 1;
    LOGI("gpufind: scanning {} frames for draws within {} m of the eye", frames, metres);
}

std::string GpuTrace::SixDofStats()
{
    bool xf, prev; float l[3], r[3];
    { std::lock_guard lk(g_XfM); xf = g_XfValid; prev = g_XfPrevValid; memcpy(l, g_DeltaView[0].t, 12); memcpy(r, g_DeltaView[1].t, 12); }
    BodyHide::Anchor a[BodyHide::aCount]; BodyHide::GetAnchors(a);
    return fmt::format("mode {} id {} hold {} hand {} near {} pool {} xf {}/{} cbhit {} cbmiss {} nolayout {} factorfail {} notnear {} notweapon {} noanchor {} entL {} entR {} anchored {} patched {} noview {} nohand {} dtL ({:.3f},{:.3f},{:.3f}) dtR ({:.3f},{:.3f},{:.3f})",
                       g_SixDof.load(), g_IdMode.load(), g_HoldMode.load(), g_SixHand.load(), g_Near.load(), g_PoolReady.load() ? 1 : 0, xf ? 1 : 0, prev ? 1 : 0,
                       g_CbHit.load(), g_CbMiss.load(), g_NoLayout.load(), g_FactorFail.load(), g_NotNear.load(), g_NotWeapon.load(), g_NoAnchor.load(),
                       a[BodyHide::aWeaponL].valid ? 1 : 0, a[BodyHide::aWeaponR].valid ? 1 : 0, g_Anchored.load(),
                       g_Patched.load(), g_XfNoView.load(), g_XfNoHand.load(), l[0], l[1], l[2], r[0], r[1], r[2]);
}

std::string GpuTrace::ShaderCache()
{
    std::lock_guard lk(g_ShaderM);
    std::lock_guard lk2(g_LayoutM);
    return fmt::format("{} shaders {} KB, {} layouts, gen {}",
                       g_ShaderCode.size(), g_ShaderBytes / 1024, g_LayoutByVs.size(), g_LayoutGen.load());
}

size_t GpuTrace::ShaderCount() { std::lock_guard lk(g_ShaderM); return g_ShaderCode.size(); }
uint64_t GpuTrace::ShaderHashOf(const void* sh)
{
    std::lock_guard lk(g_ShaderM);
    if (auto it = g_VsHash.find((void*)sh); it != g_VsHash.end()) return it->second;
    if (auto it = g_PsCsHash.find((void*)sh); it != g_PsCsHash.end()) return it->second;
    return 0;
}
uint64_t GpuTrace::VsHashOf(const void* vs)
{ std::lock_guard lk(g_ShaderM); auto it = g_VsHash.find((void*)vs); return it == g_VsHash.end() ? 0 : it->second; }

std::string GpuTrace::SwapStats()
{ return fmt::format("tried {} done {} nobuf {} sizemismatch {} shadow {}", g_SwapTried.load(), g_SwapDone.load(), g_SwapNoBuf.load(), g_SwapSize.load(), g_ShadowValid ? 1 : 0); }

void GpuTrace::SetHandFollow(int on) { g_HandFollow = on; }
void GpuTrace::RequestVertexDump(unsigned indexCount, int side) { g_VbDumpSide = side & 1; g_VbDumpWant = (int)indexCount; }

// ---- the full arm / body ----
void GpuTrace::SetArmMode(int mode) { g_ArmMode = mode; }
void GpuTrace::SetAnimSuspend(bool on)
{
    if (g_AnimSuspend.exchange(on) == on) return;
    if (!on)
    {   // The arm rig's stale-palette test reads "main palette not bound for 60 frames" as a level load, and while
        // suspended nothing refreshed it: a 9 s choke would have adopted whichever same-sized sleeve palette came
        // first. The palette never went anywhere, so mark it seen.
        std::lock_guard lk(g_RigM);
        for (auto& rig : g_Rig) if (rig.mainSeen) rig.mainSeen = g_PresentFrame;
    }
    LOGI("gpu6dof: arms and weapon {} (scripted animation)", on ? "handed to the game" : "back on the controllers");
}
bool GpuTrace::AnimSuspended() { return g_AnimSuspend.load(); }
unsigned long long GpuTrace::AnimSuspendedDraws() { return g_AnimSuspended.load(); }
int  GpuTrace::ArmMode() { return g_ArmMode; }
void GpuTrace::SetBodyFollow(int on) { g_BodyFollow = on; }
void GpuTrace::SetArmLength(float metres, float maxScale) { g_ArmLen = metres; if (maxScale > 0.f) g_ArmScaleMax = maxScale; LOGI("gpubody: drawn arm lengthened towards {:.2f} m (at most x{:.2f})", g_ArmLen.load(), g_ArmScaleMax.load()); }
void GpuTrace::SetShoulder(float fwd, float out, float up) { g_ShoulderOff[0] = fwd; g_ShoulderOff[1] = out; g_ShoulderOff[2] = up; }
void GpuTrace::SetPole(float fwd, float out, float up) { g_Pole[0] = fwd; g_Pole[1] = out; g_Pole[2] = up; }
void GpuTrace::SetArmPriors(float forearm, float upper)
{
    g_ForearmPrior = forearm; g_UpperPrior = upper;
    std::lock_guard lk(g_RigM);
    for (int sd = 0; sd < 2; sd++) { ArmRig& r = g_Rig[sd]; if (!r.valid) continue; r.elbow.sPrior = r.wrist.sPrior + forearm; r.shoulder.sPrior = r.elbow.sPrior + upper; JointSolve(r.wrist, r, 0.05f); JointSolve(r.elbow, r); JointSolve(r.shoulder, r); RebuildSegTables(sd); }
}
void GpuTrace::SetArmSegment(int side, const std::vector<int>& bones, int seg)
{
    std::lock_guard lk(g_RigM);
    ArmRig& r = g_Rig[side & 1];
    for (int b : bones) if (b >= 0 && b < 256) r.manual[b] = seg == 0 ? 0 : seg == 5 ? 255 : (uint8_t)seg;
    RebuildSegTables(side & 1);
}
void GpuTrace::ResetArmJoints(int side)
{
    std::lock_guard lk(g_RigM);
    for (int sd = 0; sd < 2; sd++)
    {
        if (side >= 0 && sd != side) continue;
        ArmRig& r = g_Rig[sd]; if (!r.valid) continue;
        const float cut = g_HandCut.load();
        JointReset(r.wrist, cut, r); JointReset(r.elbow, cut + g_ForearmPrior.load(), r); JointReset(r.shoulder, cut + g_ForearmPrior.load() + g_UpperPrior.load(), r);
        memset(r.autoSeg, 0, sizeof r.autoSeg);
        for (int k = 0; k < 3; k++) for (int b = 0; b < 256; b++) g_Rel[sd][k][b] = RelStat();
        RebuildSegTables(sd);
    }
}
void GpuTrace::LogArmRig(int side)
{
    std::lock_guard lk(g_RigM);
    const int sd = side & 1; const ArmRig& r = g_Rig[sd];
    if (!r.valid) { LOGI("gpubody: side {} rig not ready (skin mesh not analysed yet)", sd); return; }
    LOGI("gpubody: side {} tip ({:.3f},{:.3f},{:.3f}) axis ({:.3f},{:.3f},{:.3f}) palm {} main palette {} bones", sd, r.tip[0], r.tip[1], r.tip[2], r.axis[0], r.axis[1], r.axis[2], r.palm, r.mainKeyBones);
    auto joint = [&](const char* name, const JointEst& j) {
        LOGI("gpubody: side {} {}: s {:.3f} (prior {:.3f}) bind ({:.3f},{:.3f},{:.3f}) bending frames {} line uu {:.3f}", sd, name, j.s, j.sPrior, j.j[0], j.j[1], j.j[2], j.n, j.uu); };
    joint("wrist", r.wrist); joint("elbow", r.elbow); joint("shoulder", r.shoulder);
    {
        const SegTable& T = g_Seg[sd];
        LOGI("gpubody: side {} table: {} bones, reps hand {} fore {} upper {} root {}", sd, T.bones, T.rep[sgHand], T.rep[sgFore], T.rep[sgUpper], T.rep[sgRoot]);
        std::string line; int col = 0;
        for (int b = 0; b < T.bones; b++)
        {
            if (T.w[b] <= 0.f) continue;
            const RelStat* st = g_Rel[sd][0] + b; const RelStat* su = g_Rel[sd][1] + b; const RelStat* sm = g_Rel[sd][2] + b;
            auto sdev = [](const RelStat* s) { return s->n > 1 ? sqrt(s->m2 / s->n) * 100.0 : -1.0; };
            line += fmt::format(" {}:{}@{:.2f}(f{:.1f}/u{:.1f}/m{:.1f})", b, kSegName[T.seg[b]], T.s[b], sdev(st), sdev(su), sdev(sm));
            if (T.seg[b] == sgFore) line += fmt::format("k{:.2f}", T.twist[b]);
            if (++col % 6 == 0) { LOGI("gpubody: side {} bones{}", sd, line); line.clear(); }
        }
        if (!line.empty()) LOGI("gpubody: side {} bones{}", sd, line);
    }
    const ArmFrame& F = g_ArmFrame[sd];
    if (F.valid)
    {
        LOGI("gpubody: side {} frame: L1 {:.3f} L2 {:.3f} reach {:.3f}{} twist {:.1f} deg | model S ({:.2f},{:.2f},{:.2f})->({:.2f},{:.2f},{:.2f}) E ({:.2f},{:.2f},{:.2f})->({:.2f},{:.2f},{:.2f}) W ({:.2f},{:.2f},{:.2f})->({:.2f},{:.2f},{:.2f})",
             sd, F.L1, F.L2, F.reach, F.stretched ? " (stretched)" : "", F.twist * 57.2958f, F.S[0], F.S[1], F.S[2], F.Sp[0], F.Sp[1], F.Sp[2],
             F.E[0], F.E[1], F.E[2], F.Ep[0], F.Ep[1], F.Ep[2], F.W[0], F.W[1], F.W[2], F.Wp[0], F.Wp[1], F.Wp[2]);
        float w[6][3]; const float* m[6] = { F.S, F.Sp, F.E, F.Ep, F.W, F.Wp };
        for (int i = 0; i < 6; i++) Xform(F.m2w, m[i], w[i]);
        LOGI("gpubody: side {} frame world: S ({:.2f},{:.2f},{:.2f})->({:.2f},{:.2f},{:.2f}) E ({:.2f},{:.2f},{:.2f})->({:.2f},{:.2f},{:.2f}) W ({:.2f},{:.2f},{:.2f})->({:.2f},{:.2f},{:.2f}) | head ({:.2f},{:.2f},{:.2f}) controller ({:.2f},{:.2f},{:.2f}) model origin ({:.2f},{:.2f},{:.2f})",
             sd, w[0][0], w[0][1], w[0][2], w[1][0], w[1][1], w[1][2], w[2][0], w[2][1], w[2][2], w[3][0], w[3][1], w[3][2], w[4][0], w[4][1], w[4][2], w[5][0], w[5][1], w[5][2],
             F.head[0], F.head[1], F.head[2], F.ctrl[0], F.ctrl[1], F.ctrl[2], F.m2w.t[0], F.m2w.t[1], F.m2w.t[2]);
    }
    else LOGI("gpubody: side {} no frame computed yet", sd);
}
void GpuTrace::SetArmSolve(int mode) { g_ArmSolve = mode; LOGI("gpubody: arm solve {}", mode ? "per draw, from its own palette" : "LEGACY: one frame per Present, shared"); }
void GpuTrace::RequestParts(int draws) { g_PartsLeft = draws; LOGI("gunparts: logging the next {} weapon draws", draws); }
void GpuTrace::RequestHandJit(int draws)
{

    std::lock_guard lk(g_RigM);
    if (draws < 0) { g_JitFrozen = true; return; }
    for (auto& a : g_Jit) for (auto& s : a) s = JitStat();
    for (auto& s : g_JitP) s = JitPrev();
    { std::lock_guard lw(g_JitWM); for (auto& s : g_JitW) s = JitWeapon(); }
    g_JitTraceLeft = draws; g_JitFrozen = false;

}
std::string GpuTrace::HandJitStats()
{
    std::lock_guard lk(g_RigM);
    std::string s;
    for (int sd = 0; sd < 2; sd++)
        for (int k = 0; k < 2; k++)
        {
            const JitStat& j = g_Jit[sd][k];
            s += fmt::format("{}{} n {} mean {:.2f} max {:.2f} mm >1mm {} >1cm {} ang {:.2f} hash!=F {} reread {}", sd ? "R" : "L", k ? "o" : "m", j.n,
                             j.n ? j.sum / j.n * 1000.0 : 0.0, j.max * 1000.0, j.bad1, j.bad10, j.angMax, j.hashDiff, j.readDiff);
            if (k) s += fmt::format(" elbow {} max {:.2f} mm", j.elbowN, j.elbowMax * 1000.0);
            s += " | ";
        }
    for (int sd = 0; sd < 2; sd++)
    {
        const JitPrev& p = g_JitP[sd];
        s += fmt::format("{}prev n {} hit {} solved {} miss {} mean {:.2f} max {:.2f} mm >1mm {} | ", sd ? "R" : "L", p.n, p.hit, p.solved, p.miss,
                         p.n ? p.sum / p.n * 1000.0 : 0.0, p.max * 1000.0, p.bad1);
    }
    {
        std::lock_guard lw(g_JitWM);
        for (int sd = 0; sd < 2; sd++)
            s += fmt::format("{}weapon n {} mean {:.2f} max {:.2f} mm | ", sd ? "R" : "L", g_JitW[sd].n, g_JitW[sd].n ? g_JitW[sd].sum / g_JitW[sd].n * 1000.0 : 0.0, g_JitW[sd].max * 1000.0);
    }


    return s;
}
std::string GpuTrace::BodyStats()
{

    std::lock_guard lk(g_RigM);
    std::string s = fmt::format("mode {} follow {} fulldraws L {} R {} notable {} noframe {} torsodraws {} readopt {}", g_ArmMode.load(), g_BodyFollow.load(),
                                g_FullDraws[0].load(), g_FullDraws[1].load(), g_FullNoTable.load(), g_FullNoFrame.load(), g_BodyDraws.load(), g_MainReadopted.load());
    for (int sd = 0; sd < 2; sd++)
    {
        const ArmRig& r = g_Rig[sd]; const ArmFrame& F = g_ArmFrame[sd];
        const SegTable& T = g_Seg[sd];
        s += fmt::format(" | {} rig {} bones {} reps {}/{}/{}/{} joints {:.2f}/{:.2f}/{:.2f} bend {}/{}/{}", sd ? "R" : "L", r.valid ? 1 : 0, T.bones,
                         T.rep[sgHand], T.rep[sgFore], T.rep[sgUpper], T.rep[sgRoot], r.wrist.s, r.elbow.s, r.shoulder.s, r.wrist.n, r.elbow.n, r.shoulder.n);
        if (F.valid) s += fmt::format(" L1 {:.2f} L2 {:.2f} reach {:.2f}{} twist {:.0f}", F.L1, F.L2, F.reach, F.stretched ? "!" : "", F.twist * 57.2958f);
    }
    return s;
}
void GpuTrace::SetHandCollapse(int on) { g_HandCollapse = on; }
void GpuTrace::SetHandCut(float metres)
{
    g_HandCut = metres;
    {
        std::lock_guard lk(g_ArmBonesM);
        for (int sd = 0; sd < 2; sd++)
        {
            ArmBones& ab = g_ArmBones[sd]; if (!ab.valid) continue;
            ab.collapse.reset();
            for (int b = 0; b < ab.bones; b++) if (ab.w[b] <= 0 || ab.s[b] > metres) ab.collapse.set(b);
            for (int b = ab.bones; b < 256; b++) ab.collapse.set(b);
        }
    }
    ResetArmJoints(-1);          // the wrist prior is the cut; the other joints hang off it
}
void GpuTrace::LogArmBones(int side)
{
    std::lock_guard lk(g_ArmBonesM);
    const ArmBones& ab = g_ArmBones[side & 1];
    if (!ab.valid) { LOGI("gpuhands: side {} not analysed yet", side); return; }
    std::string line;
    for (int b = 0; b < ab.bones; b++)
    {
        if (ab.w[b] <= 0) line += fmt::format(" {}:-", b);
        else line += fmt::format(" {}:{:.2f}{}", b, ab.s[b], ab.collapse.test(b) ? "x" : "");
        if ((b % 12) == 11 || b == ab.bones - 1) { LOGI("gpuhands: side {} bone:tipdist{}", side, line); line.clear(); }
    }
    LOGI("gpuhands: side {} palm {} tip {} cut {:.2f} socketQ {}", side, ab.palm, ab.tip, g_HandCut.load(), g_HandQ[side & 1].valid);
}
void GpuTrace::LogBoneStats(int side)
{
    std::lock_guard lk(g_BoneM);
    const int n = g_BoneCount[side & 1];
    std::string line;
    for (int b = 0; b < n; b++)
    {
        const BoneStat& st = g_BoneStat[side & 1][b];
        const double sd = st.n > 1 ? sqrt(st.m2 / st.n) : -1.0;
        line += fmt::format(" {}:{:.1f}", b, sd * 100.0);
        if ((b % 12) == 11 || b == n - 1) { LOGI("gpuhands: side {} bone:sd(cm){}", side, line); line.clear(); }
    }
    LOGI("gpuhands: side {} {} bones, {} samples", side, n, n ? g_BoneStat[side & 1][0].n : 0.0);
}
void GpuTrace::AutoCollapse(int side, float thresholdCm)
{
    std::lock_guard lk(g_BoneM);
    const int n = g_BoneCount[side & 1];
    g_Collapse[side & 1].reset();
    int nc = 0;
    for (int b = 0; b < n; b++)
    {
        const BoneStat& st = g_BoneStat[side & 1][b];
        const double sd = st.n > 1 ? sqrt(st.m2 / st.n) * 100.0 : 1e9;
        if (sd > thresholdCm) { g_Collapse[side & 1].set(b); nc++; }
    }
    LOGI("gpuhands: side {} auto: {} of {} bones collapse (sd > {:.1f} cm)", side, nc, n, thresholdCm);
}
void GpuTrace::ResetBoneStats(int side)
{
    std::lock_guard lk(g_BoneM);
    for (int b = 0; b < 256; b++) g_BoneStat[side & 1][b] = BoneStat();
}
void GpuTrace::SetCollapseBones(int side, const std::vector<int>& bones, bool clear)
{
    std::lock_guard lk(g_BoneM);
    if (clear) g_Collapse[side & 1].reset();
    for (int b : bones) if (b >= 0 && b < 256) g_Collapse[side & 1].set(b);
}
int GpuTrace::HandFollow() { return g_HandFollow; }
void GpuTrace::SetGrabFist(int mode) { g_FistMode = mode; LOGI("gpuhands: grab fist {}", mode ? "on (the weapon's grip)" : "off (the game's hand)"); }
std::string GpuTrace::FistStats()
{
    std::lock_guard lk(g_RigM);
    return fmt::format("mode {} L learned {} applied {} nopose {} ({}) R learned {} applied {} nopose {} ({})", g_FistMode.load(),
                       g_FistLearned[0].load(), g_FistApplied[0].load(), g_FistNoPose[0].load(), g_Fist[0].valid ? g_Fist[0].has.count() : 0,
                       g_FistLearned[1].load(), g_FistApplied[1].load(), g_FistNoPose[1].load(), g_Fist[1].valid ? g_Fist[1].has.count() : 0);
}
void GpuTrace::SetHandRest(int mode, float curl, int grip)
{
    g_RestMode = mode; if (curl >= 0.f) g_RestCurl = std::clamp(curl, 0.f, 1.f); if (grip >= 0) g_RestGrip = grip;
    LOGI("gpuhands: empty hand {}, curl {:.2f}, grip {}", mode ? "keeps one pose" : "as the game animates it", g_RestCurl.load(), g_RestGrip.load() ? "closes it" : "ignored");
}
void GpuTrace::ForgetHandRest() { g_RestForget = true; }
void GpuTrace::SetFingerTracking(bool on)
{
    g_FingersOn = on;
    LOGI("gpuhands: the drawn fingers {}", on ? "follow the controller's fingers (FingerTracking, Index layout): an empty hand and a hand with a power" : "keep the game's / the grip's pose");
}
std::string GpuTrace::HandRestStats()
{
    std::lock_guard lk(g_RigM);
    return fmt::format("mode {} curl {:.2f} grip {} | still {}ms sprint {}ms speed {:.2f} | L open {} fist {} applied {} nopose {} t {:.2f} grip {:.2f} | R open {} fist {} applied {} nopose {} t {:.2f} grip {:.2f}",
                       g_RestMode.load(), g_RestCurl.load(), g_RestGrip.load(), g_StillMs.load(), g_SprintMs.load(), g_PlayerSpeed.load(),
                       g_RestOpen[0].valid ? (int)g_RestOpen[0].has.count() : 0, g_RestFist[0].valid ? (int)g_RestFist[0].has.count() : g_Fist[0].valid ? -1 : 0,
                       g_RestApplied[0].load(), g_RestNoPose[0].load(), g_RestT[0].load(), g_GripNow[0].load(),
                       g_RestOpen[1].valid ? (int)g_RestOpen[1].has.count() : 0, g_RestFist[1].valid ? (int)g_RestFist[1].has.count() : g_Fist[1].valid ? -1 : 0,
                       g_RestApplied[1].load(), g_RestNoPose[1].load(), g_RestT[1].load(), g_GripNow[1].load()) +
           fmt::format(" | fingers {} L src {} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} map {} applied {} | R src {} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} map {} applied {}",
                       g_FingersOn.load() ? 1 : 0,
                       g_FingerSrc[0].load(), g_FingerCurl[0][0].load(), g_FingerCurl[0][1].load(), g_FingerCurl[0][2].load(), g_FingerCurl[0][3].load(), g_FingerCurl[0][4].load(),
                       g_FMap[0].valid ? fmt::format("{}/{}/{}/{}/{}", g_FMap[0].count[0], g_FMap[0].count[1], g_FMap[0].count[2], g_FMap[0].count[3], g_FMap[0].count[4]) : std::string(g_FMap[0].tried ? "whole" : "-"),
                       g_FingerApplied[0].load(),
                       g_FingerSrc[1].load(), g_FingerCurl[1][0].load(), g_FingerCurl[1][1].load(), g_FingerCurl[1][2].load(), g_FingerCurl[1][3].load(), g_FingerCurl[1][4].load(),
                       g_FMap[1].valid ? fmt::format("{}/{}/{}/{}/{}", g_FMap[1].count[0], g_FMap[1].count[1], g_FMap[1].count[2], g_FMap[1].count[3], g_FMap[1].count[4]) : std::string(g_FMap[1].tried ? "whole" : "-"),
                       g_FingerApplied[1].load());
}
int GpuTrace::HandItemDraws(int hand)
{
    if (!g_SixDof.load()) return -1;
    return g_WeaponHits[hand & 1].load();
}
//! First-person draws seen in the last completed frame: the arm models plus each hand's equipment. It is the
//! mod's only POSITIVE "the player is actually playing" signal -- every menu, loading screen and cutscene draws
//! none of them, while gameplay draws ~10 per arm. -1 when neither subsystem that counts them is enabled, so a
//! caller can tell "not in game" from "not measured".
int GpuTrace::FirstPersonDraws()
{
    if (!g_HandFollow.load() && !g_SixDof.load()) return -1;
    return g_ArmHits[0].load() + g_ArmHits[1].load() + g_WeaponHits[0].load() + g_WeaponHits[1].load();
}
std::string GpuTrace::HandStats()
{
    std::lock_guard lk(g_ArmM);
    size_t nl, nr; { std::lock_guard lb(g_BoneM); nl = g_Collapse[0].count(); nr = g_Collapse[1].count(); }
    return fmt::format("on {} armdraws L {} R {} unknown {} palettes {} unknownpalettes {} idphase {} runs {} palwrites {} palswaps {} palmiss {} wanted {} evicted {} collapse {} bones L {} R {} mesh cap {} ok {} rej {} owndelta L {} R {} qrej {} q L {} R {} handgrip L {:.0f}/{:.0f}/{:.0f} R {:.0f}/{:.0f}/{:.0f} hold L {} R {} used L {} R {} usehold {} crouchhold L {} R {} entpalm L {:.2f} R {:.2f} gadget {} gadgetdraws L {} R {}",
                       g_HandFollow.load(), g_ArmHits[0].load(), g_ArmHits[1].load(), g_ArmUnknown.load(),
                       g_ArmSrvSide.size(), g_ArmSrvUnknown.size(), g_ArmIdPhase.load(), g_ArmIdRuns,
                       g_PalWrites.load(), g_PalSwaps.load(), g_PalMiss.load(), g_PalWantedCount.load(), g_PalWantedEvicted.load(), g_HandCollapse.load(), nl, nr,
                       g_MeshCaps.load(), g_MeshAnalyzed.load(), g_MeshRejected.load(), g_HandOwnDelta[0].load(), g_HandOwnDelta[1].load(), g_HandQRejected.load(),
                       [&]{ std::lock_guard lq(g_ArmBonesM); return g_HandQ[0].valid; }(), [&]{ std::lock_guard lq(g_ArmBonesM); return g_HandQ[1].valid; }(),
                       g_HandGripYaw[0].load(), g_HandGripPitch[0].load(), g_HandGripRoll[0].load(),
                       g_HandGripYaw[1].load(), g_HandGripPitch[1].load(), g_HandGripRoll[1].load(),
                       [&]{ std::lock_guard lq(g_ArmBonesM); return g_HandHold[0].valid; }(), [&]{ std::lock_guard lq(g_ArmBonesM); return g_HandHold[1].valid; }(),
                       g_HoldUsed[0].load(), g_HoldUsed[1].load(), g_UseHold.load(), g_CrouchHoldUsed[0].load(), g_CrouchHoldUsed[1].load(),
                       g_EntPalm[0].load(), g_EntPalm[1].load(), g_GadgetOn.load(), g_GadgetDraws[0].load(), g_GadgetDraws[1].load());
}

//! World point -> normalised device coordinates, through the projection the engine itself handed the shaders this
//! frame (see g_Proj). x/y in [-1,1] with +y up; false when the point is behind the eye or no view has been
//! factored recently. `depth` comes back as the view-space distance along the forward axis, so a caller can size a
//! marker by range.
void GpuTrace::SetProjAny(int on) { g_ProjAny = on; LOGI("GpuTrace: reticle projection from {}", on ? "ANY factored draw (old)" : "the player's own draws"); }
namespace
{
    //! The projection and view ProjectWorld / UnprojectNdc use on this thread (see GpuTrace::ProjectForSerial).
    bool ProjFor(float P[4][4], Rigid& view)
    {
        std::lock_guard lk(g_ProjM);
        if (t_ProjWant)
            for (const ProjRec& r : g_ProjRing)
                if (r.serial == t_ProjWant && g_PresentFrame - r.frame <= 4) { memcpy(P, r.P, sizeof r.P); view = r.view; g_ProjForHit++; return true; }
        if (t_ProjWant) g_ProjForMiss++;
        if (!g_ProjValid || g_PresentFrame - g_ProjFrame > 4) return false;
        memcpy(P, g_Proj, sizeof g_Proj); view = g_ProjView;
        return true;
    }
}
void GpuTrace::ProjectForSerial(unsigned long long serial) { t_ProjWant = serial; }
bool GpuTrace::ProjectWorld(const float w[3], float ndc[2], float* depth)
{
    float P[4][4]; Rigid vwLocal;
    if (!ProjFor(P, vwLocal)) return false;
    // world -> view, then clip.i = dot(P_row_i, view4)
    float v[4] = { 0, 0, 0, 1 };
    for (int i = 0; i < 3; i++)
        v[i] = vwLocal.r[i * 3] * w[0] + vwLocal.r[i * 3 + 1] * w[1] + vwLocal.r[i * 3 + 2] * w[2] + vwLocal.t[i];
    float clip[4];
    for (int i = 0; i < 4; i++) clip[i] = P[i][0] * v[0] + P[i][1] * v[1] + P[i][2] * v[2] + P[i][3] * v[3];
    if (!(clip[3] > 1e-4f)) return false;                  // behind the eye (or on it)
    ndc[0] = clip[0] / clip[3]; ndc[1] = clip[1] / clip[3];
    if (!std::isfinite(ndc[0]) || !std::isfinite(ndc[1])) return false;
    if (depth) *depth = clip[3];
    return true;
}
bool GpuTrace::UnprojectNdc(const float ndc[2], float org[3], float dir[3])
{
    float P[4][4]; Rigid vwLocal;
    if (!ProjFor(P, vwLocal)) return false;
    // a direction d (w = 0) with d.z = 1 projects to ndc when P0.d = ndc.x * P3.d and P1.d = ndc.y * P3.d
    const float a = P[0][0] - ndc[0] * P[3][0], b = P[0][1] - ndc[0] * P[3][1], c = -(P[0][2] - ndc[0] * P[3][2]);
    const float d = P[1][0] - ndc[1] * P[3][0], e = P[1][1] - ndc[1] * P[3][1], f = -(P[1][2] - ndc[1] * P[3][2]);
    const float det = a * e - b * d;
    if (fabsf(det) < 1e-9f) return false;
    const float v[3] = { (c * e - b * f) / det, (a * f - c * d) / det, 1.f };
    for (int i = 0; i < 3; i++)
    {
        dir[i] = vwLocal.r[i] * v[0] + vwLocal.r[3 + i] * v[1] + vwLocal.r[6 + i] * v[2];
        org[i] = -(vwLocal.r[i] * vwLocal.t[0] + vwLocal.r[3 + i] * vwLocal.t[1] + vwLocal.r[6 + i] * vwLocal.t[2]);
    }
    const float n = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    for (int i = 0; i < 3; i++) dir[i] /= n;
    return true;
}
bool GpuTrace::ArmCorrectJoint(int side, int /*joint*/, float pos[3], float axis[9], int* seg)
{
    // The engine's joint index is NOT the palette's (measured: Inv(M_j) * W_j drifts by 0.1-1.4 across frames for
    // the joints the effects ask for), so the segment is taken from WHERE the joint is along the animated arm, which
    // is also exactly the delta the skin at that point receives: beyond the wrist the hand's, between the elbow and
    // the wrist the forearm's (its twist interpolated by how far along it is, as the twist bones are), between the
    // shoulder and the elbow the upper arm's. Off the arm (a helper far from the chain) nothing.
    // A takedown (and anything else that suspends the rewrite) draws the arms as the game animates them: the
    // engine's own joints are then the right answer, already where the arms are drawn.
    if (g_AnimSuspend.load()) return false;
    ArmFrame F;
    {
        std::lock_guard lk(g_FxM);
        const FxSnap& s = g_Fx[side & 1];
        // Two frames of grace, as the palette rewrite itself allows: past that the arm is not being drawn (a menu, a
        // takedown's own arms, a load) and the animated pose is the right answer.
        if (!s.valid || !s.F.valid || g_PresentFrame - s.frame > 2) return false;
        F = s.F;
    }
    constexpr float kOffArm = 0.15f;             // metres from the bone line: not part of the arm
    float ew[3], se[3], pe[3], ps[3];
    Sub3(F.W, F.E, ew); Sub3(F.E, F.S, se); Sub3(pos, F.E, pe); Sub3(pos, F.S, ps);
    const float L2 = Norm3(ew), L1 = Norm3(se);
    if (!(L2 > 0.05f) || !(L1 > 0.05f)) return false;
    const float tf = Dot3(pe, ew), tu = Dot3(ps, se);
    auto radial = [](const float d[3], const float u[3], float t) { const float r[3] = { d[0] - u[0] * t, d[1] - u[1] * t, d[2] - u[2] * t }; return sqrtf(Dot3(r, r)); };
    Rigid D; int sg = 0;
    if (tf > L2)
    {   // the hand: within a hand's length of the wrist
        float pw[3]; Sub3(pos, F.W, pw);
        if (sqrtf(Dot3(pw, pw)) > 0.30f) return false;
        D = F.dh; sg = sgHand;
    }
    else if (tf >= 0.f && radial(pe, ew, tf) < kOffArm)
    {
        const float k = tf / L2;
        D = Mul(AxisAngle(F.ax, k * F.twist), F.rf0);
        float t[3]; Rotate(D, F.E, t); for (int i = 0; i < 3; i++) D.t[i] = F.Ep[i] - t[i];
        sg = sgFore;
    }
    else if (tu >= -0.05f && tu <= L1 && radial(ps, se, tu) < kOffArm) { D = F.du; sg = sgUpper; }
    else return false;
    if (seg) *seg = sg;
    float p[3] = { pos[0], pos[1], pos[2] };
    {   // the position goes with the lengthened arm (BoneDelta's stretch); the axes keep the rigid rotation
        Rigid Dp = D;
        if (F.k != 1.f && sg == sgUpper) { float u[3]; Sub3(F.Ep, F.Sp, u); Dp = Mul(StretchAbout(F.Sp, u, F.k), D); }
        if (F.k != 1.f && sg == sgFore) Dp = Mul(StretchAbout(F.Ep, F.ax, F.k), D);
        Xform(Dp, p, pos);
    }
    for (int k = 0; k < 3; k++) { const float row[3] = { axis[k * 3], axis[k * 3 + 1], axis[k * 3 + 2] }; Rotate(D, row, axis + k * 3); }
    return true;
}
unsigned long long GpuTrace::PresentFrame() { return g_PresentFrame; }
void GpuTrace::ArmFxLag(int side, double& mean, double& max, unsigned long long& n)
{
    std::lock_guard lk(g_FxM);
    FxLag& L = g_FxLag[side & 1];
    n = L.n; mean = L.n ? L.sum / L.n : 0.0; max = L.max; L = FxLag();
}
bool GpuTrace::ArmSnapshot(int side, float m2w[12], int bone, float boneRT[12], int* bones, unsigned long long* frame)
{
    std::lock_guard lk(g_FxM);
    const FxSnap& s = g_Fx[side & 1];
    if (!s.valid || g_PresentFrame - s.frame > 2) return false;
    if (frame) *frame = s.frame;
    BoneWrite(m2w, s.F.m2w);
    if (bones) *bones = s.bones;
    if (bone >= 0 && bone < s.bones) memcpy(boneRT, s.pal + bone * 12, 48); else memset(boneRT, 0, 48);
    return true;
}
bool GpuTrace::GetArmJoints(int side, ArmJoints& o)
{
    std::lock_guard lk(g_FxM);
    const FxSnap& s = g_Fx[side & 1];
    o.valid = false;
    if (!s.valid || !s.F.valid || g_PresentFrame - s.frame > 2) return false;
    const ArmFrame& F = s.F;
    Xform(F.m2w, F.S, o.S); Xform(F.m2w, F.E, o.E); Xform(F.m2w, F.W, o.W);
    Xform(F.m2w, F.Sp, o.Sp); Xform(F.m2w, F.Ep, o.Ep); Xform(F.m2w, F.Wp, o.Wp);
    memcpy(o.head, F.head, 12); memcpy(o.ctrl, F.ctrl, 12);
    o.L1 = F.L1; o.L2 = F.L2; o.reach = F.reach; o.stretched = F.stretched; o.frame = s.frame;
    o.valid = true;
    return true;
}
bool GpuTrace::GetRenderView(float r[9], float t[3], float org[3])
{
    std::lock_guard lk(g_XfM);
    if (!g_ViewValid) return false;
    memcpy(r, g_ViewRigid.r, 36); memcpy(t, g_ViewRigid.t, 12); memcpy(org, g_ViewInv.t, 12);
    return true;
}
bool GpuTrace::HandsFromView() { return g_HandsFromView.load(); }
void GpuTrace::SetHandsFromView(bool on) { g_HandsFromView = on; LOGI("GpuTrace: hands drawn from {} (transforms so far: from the view {}, latest {})", on ? "the poses their view was built from" : "the latest located poses (before)", g_XfFromView.load(), g_XfLatest.load()); }
bool GpuTrace::ExecutedViewSerial(unsigned long long& serial, unsigned long long& age)
{
    serial = g_ExecSerial.load(); age = g_PresentFrame - g_ExecFrame.load();
    return serial != 0;
}
bool GpuTrace::DrawViewSerial(unsigned long long& serial, unsigned long long& age)
{
    std::lock_guard lk(g_XfM);
    if (!g_XfValid || !g_XfSerial) return false;
    serial = g_XfSerial; age = g_PresentFrame - g_XfPresent;
    return true;
}
bool GpuTrace::GetPlayerProjection(float p[4][4])
{
    std::lock_guard lk(g_ProjM);
    if (!g_ProjValid || g_PresentFrame - g_ProjFrame > 4) return false;
    memcpy(p, g_Proj, sizeof g_Proj);
    return true;
}
void GpuTrace::RequestProjCensus(int frames)
{
    { std::lock_guard lk(g_CensusM); g_Census.clear(); g_CensusText.clear(); g_CensusFrames = frames > 0 ? frames : 1; }
    g_CensusLeft = frames > 0 ? frames : 1;
}
std::string GpuTrace::ProjCensusText() { std::lock_guard lk(g_CensusM); return g_CensusText; }
//! The palm frame the draw path produced this frame (see the header), for the axes overlay.
bool GpuTrace::PalmFrame(int hand, float org[3], float rot[9], float fingers[3], float normal[3])
{
    const int h = hand & 1;
    std::lock_guard lk(g_PalmM);
    if (!g_PalmSeen[h] || g_PresentFrame - g_PalmSeen[h] > 4) return false;
    memcpy(org, g_PalmW[h].t, 12); memcpy(rot, g_PalmW[h].r, 36); memcpy(fingers, g_PalmFing[h], 12);
    if (normal) memcpy(normal, g_PalmNorm[h], 12);
    return true;
}
//! This frame's controller ray in world space, as the RENDER view sees it: the same pose the weapon and the hand
//! are placed with, so the reticle cannot disagree with them. Columns of the pose are fwd/left/up.
bool GpuTrace::HandRay(int hand, float org[3], float fwd[3])
{
    const int h = hand & 1;
    std::lock_guard lk(g_XfM);
    if (!g_XfValid || !g_TargetValid[h]) return false;
    memcpy(org, g_HandPose[h].t, 12);
    fwd[0] = g_HandPose[h].r[0]; fwd[1] = g_HandPose[h].r[3]; fwd[2] = g_HandPose[h].r[6];
    return true;
}
//! The whole controller pose, not just its forward: the axes overlay needs the UP axis too, because roll about the
//! pointing axis is exactly what decides where the palm faces.
bool GpuTrace::HandPoseWorld(int hand, float org[3], float rot[9])
{
    const int h = hand & 1;
    std::lock_guard lk(g_XfM);
    if (!g_XfValid || !g_TargetValid[h]) return false;
    memcpy(org, g_HandPose[h].t, 12); memcpy(rot, g_HandPose[h].r, 36);
    return true;
}
