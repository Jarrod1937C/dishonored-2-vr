#include "pch.h"
#include "PassProfile.h"
#include "GameInfo.h"
#include "CameraHook.h"
#include "ViewMatrixHook.h"
#include "StereoSync.h"
#include "CVars.h"
#include "Fog.h"
#include "Foveation.h"
#include "VrTweaks.h"
#include <d3d11.h>
#include <dxgi1_4.h>
#include <detours/detours.h>
#include <algorithm>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <thread>

namespace
{
    // The passes, by the function each registers for its tasks (exe+0x2C2960(pass, name, tasks, 2, ., function) in
    // exe+0x34FF80..0x351160, read statically 2026-10-29). A task's +0xD8 is that function.
    struct PassName { uint32_t fn; const char* name; };
    const PassName kPasses[] = {
        { 0x3110E0, "CLEAR" }, { 0x34FBD0, "ASYNC_KICKOFF/WAIT" }, { 0x310FB0, "ASYNC_CLEAR" }, { 0x340F10, "UPDATE_NPC_DECALS" },
        { 0x31A320, "CULL_OCEAN" }, { 0x31C710, "GATHER_UMBRA" }, { 0x31BCC0, "GATHER_LIGHTS" }, { 0x3101D0, "IL_GATHER_LIGHTS" },
        { 0x31AD30, "GATHER_DYN_MODELS" }, { 0x31A150, "GATHER_DYN_MODELS_FINALIZE" }, { 0x33D320, "SORT" },
        { 0x333550, "SHADOWS_PREPARE" }, { 0x330020, "COLLECT_DYNAMIC_CASTERS" }, { 0x330C10, "GATHER_CASTERS" },
        { 0x310230, "IL_UPDATE_DYNAMIC_MODELS" }, { 0x310200, "IL_UPDATE_WORLD_STATE_AND_OCCLUDERS" }, { 0x2FC030, "GPUCULLING" },
        { 0x2FD8D0, "DEPTHPASS" }, { 0x2FC130, "DEPTHMINMAX" }, { 0x305BE0, "SSAO" }, { 0x32F390, "SHADOWS_WORLD" },
        { 0x32F380, "SHADOWS_DYNAMIC" }, { 0x310190, "IL_ACCUMULATE_FINAL_SH" }, { 0x3102A0, "IL_UPDATE_IBL" },
        { 0x310260, "IL_UPDATE_PROBES" }, { 0x32C7F0, "SCREENSHADOWS" }, { 0x2F7260, "TLFLIGHTCULL" }, { 0x3133D0, "FORWARD_WORLD" },
        { 0x3133B0, "FORWARD_DYNAMIC" }, { 0x309430, "FOG" }, { 0x3053B0, "UPDATE_OCEAN_FFT" }, { 0x305370, "UPDATE_OCEAN" },
        { 0x3045A0, "RENDER_WATER" }, { 0x3222A0, "DEPTHALPHA_LIT" }, { 0x3150D0, "DOF_AUTOFOCUS" }, { 0x314B10, "DOF_ALPHA" },
        { 0x315450, "DOF" }, { 0x31F220, "EMISSIVE" }, { 0x31DCE0, "ALPHA_LIT" }, { 0x323210, "EMISSIVE_ONLY" },
        { 0x31F960, "DARKVISION" }, { 0x3222F0, "DISTORTION" }, { 0x327270, "POSTPROC" }, { 0x323E70, "POST_TONEMAP" },
        { 0x34FD00, "TEXTURE_STREAMER" }, { 0x31A690, "IGGY_MANAGE_TEXTURES_PACKS" }, { 0x31A710, "IGGY_3D" },
        { 0x31A8E0, "IGGY_FULLSCREEN" }, { 0x31AC40, "IGGY_TEXTURES_SUBSTITUTIONS" }, { 0x31AB10, "POST_IGGY" },
        { 0x31D7E0, "OCCLUSION_QUERIES" },
    };
    constexpr int kKnown = (int)(sizeof kPasses / sizeof kPasses[0]);
    // after the passes: a task whose function is not in the table, then the mod's own work at Present
    constexpr int kOtherPass = kKnown;
    constexpr int kSecBase = kKnown + 1;
    const char* kSecName[PassProfile::kSections] = { "mod: Present hook (all of it)", "mod: StereoTaa at Present", "mod: overlay + VR submit" };
    constexpr int kRows = kSecBase + PassProfile::kSections;
    const char* RowName(int r) { return r < kKnown ? kPasses[r].name : r == kOtherPass ? "(other function)" : kSecName[r - kSecBase]; }

    enum Kind { kLeft, kRight, kOtherView, kNoView, kKinds };
    const char* kKindName[kKinds] = { "L", "R", "other", "noview" };

    std::atomic<bool> g_On{ false };              // profiling
    std::atomic<bool> g_FogHook{ false };         // Fog's anchors (FORWARD_DYNAMIC end, ALPHA_LIT start) need the hook too
    int g_RowOpaque = -1, g_RowTransparent = -1;  // FORWARD_DYNAMIC, ALPHA_LIT
    std::atomic<bool> g_FovHook{ false };         // Foveation: the scene passes, at the eye's resolution
    bool g_FovRow[64] = {};
    // [Advanced] SkipDisabledEffects (experimental): with the view's own "no SSAO" / "no DOF" flags set (VrTweaks), the
    // game's SSAO pass still fills its targets with a neutral value (two full-screen draws, exe+0x305C06..: 0.1 ms per
    // eye measured) and the DOF pass still makes its half-resolution copy (exe+0x31562F, before its flag test: 0.07 ms).
    // DOF (SkipDisabledEffects = 2) is skipped like a disabled task once it has run for a while in the map; that broke the
    // ocean, electricity and faces (its half-size copy is read elsewhere: 2026-10-31). SSAO (= 1) is not skipped but
    // REPLACED: skipped outright, its targets kept what they last held and later passes drawing into them (what looked
    // like a fixed shading mask in the view, 2026-10-31) were never reset. So per eye, near the end of the warm-up, the
    // targets its draws bind are noted and five pixels of each read back after it twice: when every one holds the same
    // value both times (a constant fill), from then on the task only clears those targets to that value (a clear costs
    // next to nothing). Anything else -- not constant, a format not read here -- and the pass keeps running as ever.
    // Every 2000 runs it runs once more to check it still binds the same targets (else learned again).
    std::atomic<bool> g_SkipEffects{ false };
    std::atomic<bool> g_SkipDof{ false };   // SkipDisabledEffects = 2: DOF's too (its half-res copy of the scene may be read elsewhere)
    int g_RowSsao = -1, g_RowDof = -1;
    std::mutex g_EffM; std::string g_EffMap; unsigned long long g_EffRuns[2] = {}, g_EffSkipped[2] = {};
    constexpr int kFillMax = 4, kFillPix = 5;
    struct FillTarget { ID3D11Texture2D* tex = nullptr; ID3D11RenderTargetView* rtv = nullptr; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN; UINT w = 0, h = 0;
                        ID3D11Texture2D* stage[2] = {}; float value[4] = {}; };
    struct Fill
    {
        int state = 0;          // 0 learning, 1 read-backs recorded (waiting), 2 learned: cleared, 3 not a constant fill: run
        unsigned long long runs = 0, recordedAt = 0, clears = 0; int samples = 0; uintptr_t ret = 1;
        FillTarget t[kFillMax]; int n = 0;
    };
    Fill g_Fill[2];            // per eye; under g_EffM
    std::atomic<unsigned long long> g_FillPresent{ 0 };
    // the targets bound by the draws of the SSAO task running on this thread (while one is being learned)
    struct Caps { ID3D11DeviceContext* c = nullptr; bool on = false; ID3D11Texture2D* tex[kFillMax] = {}; DXGI_FORMAT fmt[kFillMax] = {}; int n = 0; bool over = false, odd = false; };
    thread_local Caps t_Caps;
    constexpr unsigned long long kEffWarmup = 240;
    uintptr_t g_Base = 0;
    using ExecFn = uintptr_t (*)(void* task);
    ExecFn g_Exec = nullptr, g_ExecImm = nullptr;          // exe+0x2C5D50, exe+0x2C5F60
    using TaskFn = uintptr_t (*)(void* task);
    thread_local TaskFn t_Real = nullptr;
    thread_local int t_Pass = -1;
    // what the task now running on this thread submits (GpuTrace's draw / dispatch hooks report here)
    struct Acc { bool on = false; uint32_t draws = 0, dispatches = 0, indirect = 0; uint64_t elems = 0, groups = 0; };
    thread_local Acc t_Acc;

    // ---- one record per timed task ----
    constexpr int kSlots = 8192;                            // ring of task records (a frame has a few hundred tasks)
    struct Rec
    {
        std::atomic<int> state{ 0 };                       // 0 free, 1 being recorded, 2 issued
        ID3D11Query* b = nullptr; ID3D11Query* e = nullptr; ID3D11Query* ps = nullptr;
        unsigned long long frame = 0; int row = 0; int kind = 0; const void* view = nullptr; DWORD tid = 0;
        double cpuUs = 0; Acc acc;
        // the targets the task drew into (sampled for the first task of a pass in a report window)
        bool rtSampled = false; uint32_t rtW = 0, rtH = 0, rtFmt = 0, rtN = 0, rtSamples = 0, rtArray = 0, dsW = 0, dsH = 0, uavW = 0, uavH = 0;
    };
    Rec g_Rec[kSlots];
    std::atomic<unsigned long long> g_Next{ 0 }, g_Frame{ 1 };
    std::atomic<unsigned long long> g_Dropped{ 0 }, g_Lost{ 0 }, g_Timed{ 0 }, g_NoCtx{ 0 }, g_NoStats{ 0 };
    ID3D11Device* g_Dev = nullptr;                           // taken at the first Present
    std::atomic<bool> g_DevReady{ false };
    std::atomic<bool> g_WantRt[kRows][kKinds];              // sample this row's targets on its next task
    LARGE_INTEGER g_QpcF{};

    // contexts confirmed to be ID3D11DeviceContexts (the wrapper's +0 is; checked once each, SEH-guarded)
    std::mutex g_CtxM; std::vector<void*> g_CtxOk, g_CtxBad;
    bool QiContext(void* p)
    {
        __try
        {
            IUnknown* u = (IUnknown*)p; void* out = nullptr;
            if (FAILED(u->QueryInterface(__uuidof(ID3D11DeviceContext), &out)) || !out) return false;
            ((IUnknown*)out)->Release();
            return out == p;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void* ReadPtr(const void* p)
    {
        __try { return *(void* const*)p; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    ID3D11DeviceContext* ContextOf(void* task)
    {
        uint8_t* wrap = (uint8_t*)ReadPtr((uint8_t*)task + 0x108);
        if ((uintptr_t)wrap < 0x10000) return nullptr;
        void* c = ReadPtr(wrap);
        if ((uintptr_t)c < 0x10000) return nullptr;
        {
            std::lock_guard lk(g_CtxM);
            if (std::find(g_CtxOk.begin(), g_CtxOk.end(), c) != g_CtxOk.end()) return (ID3D11DeviceContext*)c;
            if (std::find(g_CtxBad.begin(), g_CtxBad.end(), c) != g_CtxBad.end()) return nullptr;
        }
        const bool ok = QiContext(c);
        std::lock_guard lk(g_CtxM);
        auto& v = ok ? g_CtxOk : g_CtxBad;
        if (v.size() > 256) v.clear();
        v.push_back(c);
        if (!ok) LOGI("PassProfile: task {} context wrapper {} holds {} -- not a D3D11 context: not timed", task, (void*)wrap, c);
        return ok ? (ID3D11DeviceContext*)c : nullptr;
    }
    int KindOf(void* task, const void** viewOut)
    {
        const uint8_t* view = (const uint8_t*)ReadPtr((uint8_t*)task + 0x70);
        *viewOut = view;
        if ((uintptr_t)view < 0x10000) { *viewOut = nullptr; return kNoView; }
        if (!CameraHook::IsMainViewObject(view)) return kOtherView;
        float m[16];
        __try { memcpy(m, view + 0x7450, 64); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return kOtherView; }
        const unsigned long long s = ViewMatrixHook::SerialOfViewMatrix(m);
        if (!s) return kLeft;
        const ViewMatrixHook::XrPose p = ViewMatrixHook::RenderPoseXr(s);
        return p.valid && p.eye == 1 ? kRight : kLeft;
    }
    bool MakeQueries(Rec& r)
    {
        if (r.b) return true;
        D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP, 0 }, sd = { D3D11_QUERY_PIPELINE_STATISTICS, 0 };
        if (FAILED(g_Dev->CreateQuery(&qd, &r.b)) || FAILED(g_Dev->CreateQuery(&qd, &r.e)))
        {
            if (r.b) { r.b->Release(); r.b = nullptr; }
            return false;
        }
        if (FAILED(g_Dev->CreateQuery(&sd, &r.ps))) r.ps = nullptr;
        return true;
    }
    int Open(ID3D11DeviceContext* c, int row, int kind, const void* view)
    {
        if (!g_DevReady.load(std::memory_order_acquire)) return -1;
        const int i = (int)(g_Next.fetch_add(1) % kSlots);
        Rec& r = g_Rec[i];
        int expect = 0;
        if (!r.state.compare_exchange_strong(expect, 1)) { g_Dropped++; return -1; }
        if (!MakeQueries(r)) { r.state = 0; g_Dropped++; return -1; }
        r.frame = g_Frame.load(); r.row = row; r.kind = kind; r.view = view; r.tid = GetCurrentThreadId();
        r.rtSampled = false; r.acc = Acc{};
        c->End(r.b);
        if (r.ps) c->Begin(r.ps);
        return i;
    }
    uint32_t TexSize(ID3D11Resource* res, uint32_t* h, uint32_t* fmt, uint32_t* samples, uint32_t* array)
    {
        if (!res) return 0;
        D3D11_RESOURCE_DIMENSION dim{}; res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        {
            D3D11_TEXTURE2D_DESC d{}; static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
            if (h) *h = d.Height; if (fmt) *fmt = d.Format; if (samples) *samples = d.SampleDesc.Count; if (array) *array = d.ArraySize;
            return d.Width;
        }
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
        {
            D3D11_TEXTURE3D_DESC d{}; static_cast<ID3D11Texture3D*>(res)->GetDesc(&d);
            if (h) *h = d.Height; if (fmt) *fmt = d.Format; if (array) *array = d.Depth;
            return d.Width;
        }
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER)
        {
            D3D11_BUFFER_DESC d{}; static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
            if (h) *h = 1; if (fmt) *fmt = 0;
            return d.ByteWidth;
        }
        return 0;
    }
    void SampleTargets(ID3D11DeviceContext* c, Rec& r)
    {   // what is bound when the task ends: the targets its draws wrote (or the compute's first UAV)
        ID3D11RenderTargetView* rtv[8] = {}; ID3D11DepthStencilView* dsv = nullptr;
        c->OMGetRenderTargets(8, rtv, &dsv);
        for (int i = 0; i < 8; i++)
            if (rtv[i])
            {
                r.rtN++;
                if (r.rtN == 1) { ID3D11Resource* res = nullptr; rtv[i]->GetResource(&res); r.rtW = TexSize(res, &r.rtH, &r.rtFmt, &r.rtSamples, &r.rtArray); if (res) res->Release(); }
                rtv[i]->Release();
            }
        if (dsv) { ID3D11Resource* res = nullptr; dsv->GetResource(&res); r.dsW = TexSize(res, &r.dsH, nullptr, nullptr, nullptr); if (res) res->Release(); dsv->Release(); }
        ID3D11UnorderedAccessView* uav = nullptr; c->CSGetUnorderedAccessViews(0, 1, &uav);
        if (uav) { ID3D11Resource* res = nullptr; uav->GetResource(&res); r.uavW = TexSize(res, &r.uavH, nullptr, nullptr, nullptr); if (res) res->Release(); uav->Release(); }
        r.rtSampled = true;
    }
    void ClearEmpty(void* task)
    {   // the pass marked its context empty (nothing of its own to draw): the fog drawn in it must still be executed
        __try { uint8_t* w = *(uint8_t**)((uint8_t*)task + 0x108); if (w) w[0x12AD4] = 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    //! one byte, SEH-guarded (out of functions with destructors: C2712); returned + 1 as a pointer-sized value, 0 = unreadable
    const void* ReadByteAt(const uint8_t* p)
    {
        __try { return (const void*)(uintptr_t)(p[0] + 1u); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    //! SkipDisabledEffects, what to do with this SSAO / DOF task (see g_SkipEffects)
    enum { kEffRun, kEffSkip, kEffSample, kEffCheck, kEffClear };
    void FillReset(Fill& F)
    {
        for (FillTarget& t : F.t) { if (t.rtv) t.rtv->Release(); if (t.tex) t.tex->Release(); for (auto*& st : t.stage) if (st) { st->Release(); st = nullptr; } t = FillTarget{}; }
        const unsigned long long runs = F.runs; F = Fill{}; F.runs = runs;
    }
    int SkipEffect(void* task, int row, int eye)
    {
        if (!g_SkipEffects.load(std::memory_order_relaxed) || (row != g_RowSsao && (row != g_RowDof || !g_SkipDof.load(std::memory_order_relaxed)))) return kEffRun;
        const uint8_t* view = (const uint8_t*)ReadPtr((uint8_t*)task + 0x70);
        if ((uintptr_t)view < 0x10000 || !CameraHook::IsMainViewObject(view)) return kEffRun;
        const uint8_t* fp = (const uint8_t*)ReadByteAt(view + 0x3A37);
        if (!fp) return kEffRun;
        const uint8_t flags = (uint8_t)(uintptr_t)fp - 1;
        const int k = row == g_RowSsao ? 0 : 1;
        const bool off = (flags & (k == 0 ? 0x02 : 0x01)) != 0;
        std::lock_guard lk(g_EffM);
        const std::string map = VrTweaks::MapName();
        if (map != g_EffMap) { g_EffMap = map; g_EffRuns[0] = g_EffRuns[1] = 0; for (Fill& F : g_Fill) { FillReset(F); F.runs = 0; } }
        if (!off) { g_EffRuns[k] = 0; if (k == 0 && eye >= 0 && eye < 2 && g_Fill[eye].runs) { FillReset(g_Fill[eye]); g_Fill[eye].runs = 0; } return kEffRun; }
        if (k == 1)
        {
            if (++g_EffRuns[k] <= kEffWarmup) return kEffRun;
            if (g_EffSkipped[k]++ == 0) LOGI("PassProfile: SkipDisabledEffects: the DOF pass's neutral work (its effect is off) is skipped from now on");
            return kEffSkip;
        }
        if (eye < 0 || eye > 1) return kEffRun;
        Fill& F = g_Fill[eye];
        const unsigned long long r = ++F.runs;
        switch (F.state)
        {
        case 0: return r == kEffWarmup - 30 || r == kEffWarmup - 10 ? kEffSample : kEffRun;
        case 2: if (r % 2000 == 0) return kEffCheck; F.clears++; return kEffClear;
        default: return kEffRun;
        }
    }
    void CaptureTargets()
    {   // GpuTrace's draw hook, in a task being learned: what its draws bind
        Caps& C = t_Caps;
        ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        C.c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, nullptr);
        for (ID3D11RenderTargetView* v : rtv)
        {
            if (!v) continue;
            D3D11_RENDER_TARGET_VIEW_DESC d{}; v->GetDesc(&d);
            ID3D11Resource* res = nullptr; v->GetResource(&res);
            D3D11_RESOURCE_DIMENSION dim{}; if (res) res->GetType(&dim);
            if (d.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || d.Texture2D.MipSlice != 0 || dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) C.odd = true;
            else
            {
                ID3D11Texture2D* tex = static_cast<ID3D11Texture2D*>(res);
                bool have = false; for (int i = 0; i < C.n; i++) have |= C.tex[i] == tex;
                if (!have) { if (C.n < kFillMax) { C.tex[C.n] = tex; C.fmt[C.n] = d.Format; C.n++; } else C.over = true; }
            }
            if (res) res->Release();
            v->Release();
        }
    }
    //! The values a constant fill left, as clear colours; false for a format not read here
    float Half(uint16_t h)
    {
        const uint32_t sgn = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
        float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf((float)(m | 1024), (int)e - 25);
        return sgn ? -v : v;
    }
    float Small(uint32_t bits, int mbits)
    {   // the unsigned 11 / 10-bit floats of R11G11B10 (5-bit exponent)
        const uint32_t e = bits >> mbits, m = bits & ((1u << mbits) - 1);
        return e == 0 ? ldexpf((float)m, -14 - mbits) : e == 31 ? INFINITY : ldexpf((float)(m | (1u << mbits)), (int)e - 15 - mbits);
    }
    int PixelBytes(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R32_FLOAT: return 4;
        case DXGI_FORMAT_R8_UNORM: return 1;
        case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_FLOAT: return 2;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT: return 8;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
        default: return 0;
        }
    }
    void Decode(DXGI_FORMAT f, const uint8_t* p, float o[4])
    {
        o[0] = o[1] = o[2] = 0; o[3] = 1;
        auto lin = [](float x) { return x <= 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f); };
        uint32_t u = 0; memcpy(&u, p, std::min(4, PixelBytes(f)));
        switch (f)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM: for (int i = 0; i < 4; i++) o[i] = p[i] / 255.f; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: for (int i = 0; i < 3; i++) o[i] = lin(p[i] / 255.f); o[3] = p[3] / 255.f; break;
        case DXGI_FORMAT_B8G8R8A8_UNORM: o[0] = p[2] / 255.f; o[1] = p[1] / 255.f; o[2] = p[0] / 255.f; o[3] = p[3] / 255.f; break;
        case DXGI_FORMAT_R10G10B10A2_UNORM: o[0] = (u & 1023) / 1023.f; o[1] = ((u >> 10) & 1023) / 1023.f; o[2] = ((u >> 20) & 1023) / 1023.f; o[3] = (u >> 30) / 3.f; break;
        case DXGI_FORMAT_R11G11B10_FLOAT: o[0] = Small(u & 2047, 6); o[1] = Small((u >> 11) & 2047, 6); o[2] = Small(u >> 22, 5); break;
        case DXGI_FORMAT_R16G16_FLOAT: o[0] = Half((uint16_t)u); o[1] = Half((uint16_t)(u >> 16)); break;
        case DXGI_FORMAT_R32_FLOAT: memcpy(o, p, 4); break;
        case DXGI_FORMAT_R8_UNORM: o[0] = p[0] / 255.f; break;
        case DXGI_FORMAT_R8G8_UNORM: o[0] = p[0] / 255.f; o[1] = p[1] / 255.f; break;
        case DXGI_FORMAT_R16_FLOAT: o[0] = Half((uint16_t)u); break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: for (int i = 0; i < 4; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); o[i] = Half(h); } break;
        case DXGI_FORMAT_R32G32_FLOAT: memcpy(o, p, 8); break;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: memcpy(o, p, 16); break;
        default: break;
        }
    }
    //! After a learned SSAO task: the targets its draws bound, and (kEffSample) five pixels of each copied for the
    //! read-back -- recorded into the task's own command list, so right after its fill
    void AfterLearn(ID3D11DeviceContext* c, int eye, int what, uintptr_t ret)
    {
        Caps C = t_Caps; t_Caps = Caps{};
        std::lock_guard lk(g_EffM);
        Fill& F = g_Fill[eye];
        auto fail = [&](const std::string& why)
        {
            LOGI("PassProfile: SkipDisabledEffects: the {} eye's SSAO pass is not a fill that can be replaced ({}): it keeps running", eye ? "right" : "left", why);
            FillReset(F); F.state = 3;
        };
        if (what == kEffCheck)
        {
            bool same = C.n == F.n && !C.odd && !C.over;
            for (int i = 0; same && i < C.n; i++) same = C.tex[i] == F.t[i].tex;
            if (!same)
            {
                LOGI("PassProfile: SkipDisabledEffects: the {} eye's SSAO pass now draws into other targets: learned again", eye ? "right" : "left");
                FillReset(F); F.runs = kEffWarmup - 40;
            }
            return;
        }
        if (F.state != 0) return;
        if (C.odd || C.over) { fail(C.odd ? "a target that is not a whole 2D texture" : "more than four targets"); return; }
        if (!C.n) { fail("it drew nothing"); return; }
        if (F.samples == 0)
        {
            ID3D11Device* dev = nullptr; c->GetDevice(&dev);
            if (!dev) return;
            F.n = C.n; F.ret = ret;
            for (int i = 0; i < C.n; i++)
            {
                FillTarget& t = F.t[i];
                t.tex = C.tex[i]; t.tex->AddRef(); t.fmt = C.fmt[i];
                D3D11_TEXTURE2D_DESC td{}; t.tex->GetDesc(&td); t.w = td.Width; t.h = td.Height;
                if (!PixelBytes(t.fmt)) { dev->Release(); fail(fmt::format("target {} format {} not read here", i, (int)t.fmt)); return; }
                D3D11_TEXTURE2D_DESC sd{}; sd.Width = kFillPix; sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = t.fmt; sd.SampleDesc.Count = 1;
                sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (td.SampleDesc.Count > 1 || FAILED(dev->CreateTexture2D(&sd, nullptr, &t.stage[0])) || FAILED(dev->CreateTexture2D(&sd, nullptr, &t.stage[1])))
                { dev->Release(); fail("staging copy"); return; }
            }
            dev->Release();
        }
        else
        {
            bool same = C.n == F.n;
            for (int i = 0; same && i < C.n; i++) same = C.tex[i] == F.t[i].tex;
            if (!same) { fail("its targets changed between two frames"); return; }
        }
        const int si = F.samples;
        for (int i = 0; i < F.n; i++)
        {
            const FillTarget& t = F.t[i];
            const UINT px[kFillPix][2] = { { t.w / 2, t.h / 2 }, { t.w / 4, t.h / 4 }, { 3 * t.w / 4, t.h / 4 }, { t.w / 4, 3 * t.h / 4 }, { 3 * t.w / 4, 3 * t.h / 4 } };
            for (int p = 0; p < kFillPix; p++)
            {
                const D3D11_BOX b{ px[p][0], px[p][1], 0, px[p][0] + 1, px[p][1] + 1, 1 };
                c->CopySubresourceRegion(t.stage[si], 0, p, 0, 0, t.tex, 0, &b);
            }
        }
        if (++F.samples == 2) { F.state = 1; F.recordedAt = g_FillPresent.load(); }
    }
    //! At Present (immediate context): the read-backs recorded a few frames ago
    void ResolveFill(IDXGISwapChain* sc)
    {
        const unsigned long long now = ++g_FillPresent;
        std::lock_guard lk(g_EffM);
        for (int eye = 0; eye < 2; eye++)
        {
            Fill& F = g_Fill[eye];
            if (F.state != 1 || now < F.recordedAt + 12) continue;
            ID3D11Device* dev = nullptr;
            if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return;
            ID3D11DeviceContext* imm = nullptr; dev->GetImmediateContext(&imm);
            bool ok = imm != nullptr, wait = false; std::string vals, why;
            for (int i = 0; ok && i < F.n; i++)
            {
                FillTarget& t = F.t[i];
                float v[2][kFillPix][4] = {};
                for (int si = 0; ok && si < 2; si++)
                {
                    D3D11_MAPPED_SUBRESOURCE m{};
                    const HRESULT hr = imm->Map(t.stage[si], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
                    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { wait = true; ok = false; break; }
                    if (FAILED(hr)) { ok = false; why = "the read-back failed"; break; }
                    for (int p = 0; p < kFillPix; p++) Decode(t.fmt, (const uint8_t*)m.pData + p * PixelBytes(t.fmt), v[si][p]);
                    imm->Unmap(t.stage[si], 0);
                }
                if (!ok) break;
                for (int si = 0; si < 2; si++) for (int p = 0; p < kFillPix; p++) for (int ch = 0; ch < 4; ch++)
                    if (!(v[si][p][ch] == v[0][0][ch])) { ok = false; why = fmt::format("target {} ({}x{} format {}) holds {:.3f} / {:.3f} at two of its pixels", i, t.w, t.h, (int)t.fmt, v[0][0][ch], v[si][p][ch]); }
                if (!ok) break;
                memcpy(t.value, v[0][0], 16);
                vals += fmt::format(" [{}x{} format {} = {:.3f},{:.3f},{:.3f},{:.3f}]", t.w, t.h, (int)t.fmt, t.value[0], t.value[1], t.value[2], t.value[3]);
                D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = t.fmt; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                if (FAILED(dev->CreateRenderTargetView(t.tex, &rd, &t.rtv))) { ok = false; why = "a view of its target"; }
            }
            if (imm) imm->Release();
            dev->Release();
            if (wait) { if (now > F.recordedAt + 240) { LOGI("PassProfile: SkipDisabledEffects: the SSAO read-back never arrived: it keeps running"); FillReset(F); F.state = 3; } continue; }
            if (!ok)
            {
                LOGI("PassProfile: SkipDisabledEffects: the {} eye's SSAO pass is not a constant fill ({}): it keeps running", eye ? "right" : "left", why);
                FillReset(F); F.state = 3; continue;
            }
            for (FillTarget& t : F.t) for (auto*& st : t.stage) if (st) { st->Release(); st = nullptr; }
            F.state = 2;
            LOGI("PassProfile: SkipDisabledEffects: the {} eye's SSAO pass fills {} target(s) with one value each:{} -- from now on they are only cleared to it",
                 eye ? "right" : "left", F.n, vals);
        }
    }
    void MarkEmpty(void* task)
    {   // what the executor does for a disabled task: the context recorded nothing
        __try { uint8_t* w = *(uint8_t**)((uint8_t*)task + 0x108); if (w) w[0x12AD4] = 1; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    uintptr_t Thunk(void* task)
    {
        const TaskFn real = t_Real; const int row = t_Pass;
        const bool prof = g_On.load(std::memory_order_relaxed);
        ID3D11DeviceContext* c = ContextOf(task);
        const void* view = nullptr;
        const int kind = KindOf(task, &view);
        const int eye = kind == kLeft ? 0 : kind == kRight ? 1 : -1;
        const int eff = SkipEffect(task, row, eye);
        if (eff == kEffSkip) { MarkEmpty(task); return 0; }
        if (eff == kEffClear && c)
        {   // the SSAO pass's constant fill, as clears (see g_SkipEffects)
            std::lock_guard lk(g_EffM);
            const Fill& F = g_Fill[eye];
            if (F.state == 2)
            {
                for (int i = 0; i < F.n; i++) c->ClearRenderTargetView(F.t[i].rtv, F.t[i].value);
                if (g_EffSkipped[0]++ == 0) LOGI("PassProfile: SkipDisabledEffects: the SSAO pass's neutral fill is replaced by clears from now on");
                return F.ret;
            }
        }
        const bool learn = (eff == kEffSample || eff == kEffCheck) && c;
        if (learn) { t_Caps = Caps{}; t_Caps.c = c; t_Caps.on = true; }
        const int slot = c && prof ? Open(c, row, kind, view) : -1;
        if (!c) g_NoCtx++;
        const bool mainView = kind == kLeft || kind == kRight;
        // Fog: drawn at the start of the transparent pass (counted in its GPU time when profiling)
        const bool fogged = c && mainView && row == g_RowTransparent && g_FogHook.load(std::memory_order_relaxed) && Fog::BeforeTransparent(c, view);
        const bool fovRow = c && mainView && row >= 0 && row < 64 && g_FovRow[row] && g_FovHook.load(std::memory_order_relaxed);
        const bool foveated = fovRow && Foveation::Begin(c, view, kind == kRight ? 1 : 0);
        const Acc saved = t_Acc;
        t_Acc = Acc{}; t_Acc.on = prof;
        LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
        const uintptr_t ret = real(task);
        LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
        const Acc mine = t_Acc;
        t_Acc = saved;
        if (learn) AfterLearn(c, eye, eff, ret);
        if (fovRow) Foveation::End(c, foveated);
        if (fogged) ClearEmpty(task);
        if (c && mainView && row == g_RowOpaque && g_FogHook.load(std::memory_order_relaxed)) Fog::AfterOpaque(c, view);
        if (slot >= 0)
        {
            Rec& r = g_Rec[slot];
            r.cpuUs = (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)g_QpcF.QuadPart;
            r.acc = mine;
            if (g_WantRt[row][kind].exchange(false)) SampleTargets(c, r);
            if (r.ps) c->End(r.ps);
            c->End(r.e);
            r.state = 2;
        }
        return ret;
    }
    int PassOf(uintptr_t fn)
    {
        const uint32_t rva = (uint32_t)(fn - g_Base);
        for (int i = 0; i < kKnown; i++) if (kPasses[i].fn == rva) return i;
        static std::atomic<int> told{ 0 };
        if (told.load() < 12 && told.fetch_add(1) < 12) LOGI("PassProfile: a task runs exe+0x{:X}, not a known pass function: counted as '(other function)'", rva);
        return kOtherPass;
    }
    template <int W> uintptr_t HkExec(void* task)
    {
        const ExecFn real = W ? g_ExecImm : g_Exec;
        const bool prof = g_On.load(std::memory_order_relaxed);
        const bool fog = g_FogHook.load(std::memory_order_relaxed), fov = g_FovHook.load(std::memory_order_relaxed), eff = g_SkipEffects.load(std::memory_order_relaxed);
        if (!task || (!prof && !fog && !fov && !eff)) return real(task);
        uintptr_t* slot = (uintptr_t*)((uint8_t*)task + 0xD8);
        const uintptr_t fn = (uintptr_t)ReadPtr(slot);
        if (!fn || fn == (uintptr_t)&Thunk) return real(task);
        const int pass = PassOf(fn);
        const bool wanted = prof || (fog && (pass == g_RowOpaque || pass == g_RowTransparent)) || (fov && pass >= 0 && pass < 64 && g_FovRow[pass])
                         || (eff && (pass == g_RowSsao || (pass == g_RowDof && g_SkipDof.load(std::memory_order_relaxed))));
        if (!wanted) return real(task);
        const TaskFn savedReal = t_Real; const int savedPass = t_Pass;
        t_Real = (TaskFn)fn; t_Pass = pass;
        *slot = (uintptr_t)&Thunk;
        const uintptr_t ret = real(task);
        if (*slot == (uintptr_t)&Thunk) *slot = fn;
        t_Real = savedReal; t_Pass = savedPass;
        return ret;
    }

    // ---- the frame: a disjoint query per Present (the timestamps' frequency) and a stamp at each Present ----
    struct FrameQ { ID3D11Query* disj = nullptr; ID3D11Query* ts = nullptr; unsigned long long frame = 0; bool issued = false; };
    constexpr int kFrames = 64;
    FrameQ g_FQ[kFrames];
    struct FrameInfo { unsigned long long frame = 0; UINT64 freq = 0; bool bad = true; UINT64 ts = 0; double gpuMs = -1; };
    FrameInfo g_FI[kFrames];                                  // by frame % kFrames, filled a few Presents later
    double g_CpuMs[kFrames]; unsigned long long g_CpuFrame[kFrames];   // Present-to-Present CPU time, by frame % kFrames
    double CpuMsOf(unsigned long long f) { return g_CpuFrame[f % kFrames] == f ? g_CpuMs[f % kFrames] : -1.0; }
    UINT64 g_Freq = 0;
    ID3D11DeviceContext* g_Imm = nullptr;
    int g_SecOpen[PassProfile::kSections];                    // record slot of an open section, -1 none
    LARGE_INTEGER g_PresentT0{}, g_LastPresent{};

    // ---- accumulated over the report window (render thread only) ----
    struct Cell
    {
        double gpu = 0, gpuMax = 0, cpuUs = 0; unsigned long long n = 0;
        unsigned long long draws = 0, dispatches = 0, indirect = 0, elems = 0, groups = 0;
        unsigned long long iaPrims = 0, vsInv = 0, cPrims = 0, psInv = 0, csInv = 0, statsN = 0;
        bool rt = false; uint32_t rtW = 0, rtH = 0, rtFmt = 0, rtN = 0, rtSamples = 0, rtArray = 0, dsW = 0, dsH = 0, uavW = 0, uavH = 0;
        std::unordered_set<DWORD> threads;
    };
    Cell g_Cell[kRows][kKinds];
    unsigned long long g_FramesKind[kKinds] = {}, g_LastFrameKind[kKinds] = {};
    std::unordered_set<const void*> g_Views[kKinds];
    double g_FrameGpuSum = 0, g_FrameGpuMax = 0, g_FrameCpuSum = 0, g_FrameCpuMax = 0, g_HookCpuSum = 0; unsigned long long g_FrameGpuN = 0, g_FrameCpuN = 0, g_Presents = 0;
    // per frame, per row: for the worst frame's breakdown
    struct FrameRows { unsigned long long frame = 0; float ms[kRows] = {}; };
    FrameRows g_FR[kFrames];
    // one frame's tasks in GPU order, now and then
    struct TaskLine { UINT64 tb, te; int row, kind; double cpuUs; uint32_t draws, dispatches; DWORD tid; const void* view; };
    std::vector<TaskLine> g_Dump; unsigned long long g_DumpFrame = 0, g_DumpUntil = 0; ULONGLONG g_NextDumpAt = 0;
    ULONGLONG g_WindowStart = 0;
    unsigned long long g_Reports = 0;
    bool g_CvarsLogged = false;

    std::string Fmt(uint32_t f)
    {
        switch (f)
        {
        case 0: return "?"; case 2: return "RGBA32F"; case 10: return "RGBA16F"; case 11: return "RGBA16"; case 16: return "RG32F";
        case 24: return "RGB10A2"; case 26: return "R11G11B10F"; case 27: case 28: return "RGBA8"; case 29: return "RGBA8_SRGB";
        case 34: return "RG16F"; case 35: return "RG16"; case 39: case 40: case 41: return "R32F"; case 44: case 45: return "D24S8";
        case 49: return "RG8"; case 54: return "R16F"; case 55: case 56: return "R16"; case 61: return "R8"; case 87: return "BGRA8"; case 91: return "BGRA8_SRGB";
        default: return fmt::format("fmt{}", f);
        }
    }
    void LogCvars()
    {   // the settings that decide what a frame costs, as they are now
        static const char* names[] = {
            "r_renderQualityPresets", "r_AntiAliasingPreset", "r_AAMode", "r_TAAAmount", "r_modelsQualityPreset", "r_shadowQualityPreset",
            "r_textureDetailsPreset", "r_viewDistPreset", "r_oceanQualityPreset", "r_downsampleQualityPreset", "r_downsampleQuality",
            "r_maxDynamicDecalsPreset", "r_lodZoomMultiplicator", "lod_dist_level0", "lod_dist_level1", "lod_dist_level2", "lod_dist_level3",
            "lod_dist_far", "r_shadowParallelSliceCnt", "r_shadowParallelSliceSize", "r_shadowCascadeFars", "r_shadowCullDistance",
            "r_maxShadowMapResolution", "r_maxShadowMapExtraResolution", "r_shadowAtlasSliceCnt", "r_shadowParallelPartialSliceUpdate",
            "r_shadowSpotAndOmniLowResolutionDividerEnabled", "r_shadows32Bits", "r_hbao", "r_SsaoType", "r_skipSSAO", "r_skipFog",
            "r_skipLightScattering", "r_skipGodRays", "r_skipOceanSSR", "r_water_ScreenSpaceMaxStepsCount", "r_IBL_ParallaxCorrection",
            "r_ILProbeUpdate_MaxUpdatesPerFrame", "r_skipDOF", "r_skipBloom", "r_skipLensFlares", "r_SSSBlurQuality", "r_skipCameraMotionBlur",
            "r_textureStreamingPerFrameBudgetKB", "r_textureStreamerMemoryBudgetMB", "r_maxDynamicDecals", "r_skipTemporalAA" };
        std::string s;
        for (const char* n : names)
        {
            if (!CVars::Address(n)) { s += fmt::format(" {}=?", n); continue; }
            s += fmt::format(" {}={}", n, CVars::GetString(n));
        }
        LOGI("PassProfile: render settings now:{}", s);
    }
    std::string Vram()
    {
        if (!g_Dev) return "?";
        IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIAdapter3* a3 = nullptr; std::string out = "?";
        if (SUCCEEDED(g_Dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && dd && SUCCEEDED(dd->GetAdapter(&ad)) && ad
            && SUCCEEDED(ad->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&a3)) && a3)
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO li{}, nl{};
            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &li)))
            {
                a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nl);
                out = fmt::format("{} of {} MB (system {} MB)", li.CurrentUsage >> 20, li.Budget >> 20, nl.CurrentUsage >> 20);
            }
        }
        if (a3) a3->Release(); if (ad) ad->Release(); if (dd) dd->Release();
        return out;
    }

    void Report(double secs, unsigned long long curFrame)
    {
        if (!g_CvarsLogged && CVars::Available()) { g_CvarsLogged = true; std::thread(LogCvars).detach(); }   // name scans: off the render thread
        double all[kKinds] = {};
        struct Row { int row; double total; };
        std::vector<Row> rows;
        for (int p = 0; p < kRows; p++)
        {
            double t = 0;
            for (int k = 0; k < kKinds; k++) { t += g_Cell[p][k].gpu; all[k] += g_Cell[p][k].gpu; }
            if (t > 0 || g_Cell[p][0].n || g_Cell[p][1].n) rows.push_back({ p, t });
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.total > b.total; });
        const double fL = (double)std::max<unsigned long long>(1, g_FramesKind[kLeft]), fR = (double)std::max<unsigned long long>(1, g_FramesKind[kRight]);
        const double fAll = (double)std::max<unsigned long long>(1, g_Presents);
        auto perFrames = [&](int k) { return (double)std::max<unsigned long long>(1, k == kLeft ? g_FramesKind[kLeft] : k == kRight ? g_FramesKind[kRight] : g_Presents); };
        LOGI("PassProfile: ===== {:.1f} s: {} Presents ({:.1f}/s), {} left-eye and {} right-eye frames; stereo sync {} | GPU per Present {:.2f} ms avg, {:.2f} max | "
             "CPU Present-to-Present {:.2f} ms avg, {:.2f} max; the mod's Present hook {:.2f} ms CPU | VRAM {}",
             secs, g_Presents, g_Presents / secs, g_FramesKind[kLeft], g_FramesKind[kRight], StereoSync::Active() ? "on" : "off",
             g_FrameGpuN ? g_FrameGpuSum / g_FrameGpuN : 0.0, g_FrameGpuMax, g_FrameCpuN ? g_FrameCpuSum / g_FrameCpuN : 0.0, g_FrameCpuMax,
             g_Presents ? g_HookCpuSum / g_Presents : 0.0, Vram());
        LOGI("PassProfile: timed GPU work per frame: L {:.2f} ms over {} views, R {:.2f} ms over {} views, other views {:.2f} ms per Present over {} views, "
             "no view {:.2f} ms per Present; {} tasks timed, {} dropped, {} lost, {} without a context, {} without pipeline statistics",
             all[kLeft] / fL, g_Views[kLeft].size(), all[kRight] / fR, g_Views[kRight].size(), all[kOtherView] / fAll, g_Views[kOtherView].size(),
             all[kNoView] / fAll, g_Timed.load(), g_Dropped.load(), g_Lost.load(), g_NoCtx.load(), g_NoStats.load());
        LOGI("PassProfile: per pass, per frame of that eye (other / noview: per Present): GPU ms (max of one task) | tasks | CPU ms | draws (indirect) | "
             "dispatches | M elements submitted | M primitives in / rasterised | M pixels shaded | M compute threads | target");
        int shown = 0;
        for (const Row& r : rows)
        {
            if (shown++ >= 40) break;
            for (int k = 0; k < kKinds; k++)
            {
                const Cell& c = g_Cell[r.row][k];
                if (!c.n) continue;
                const double f = perFrames(k);
                std::string target;
                if (c.rt)
                {
                    if (c.rtN) target = fmt::format("{}x {}x{} {}{}{}", c.rtN, c.rtW, c.rtH, Fmt(c.rtFmt), c.rtSamples > 1 ? fmt::format(" MSAA{}", c.rtSamples) : "",
                                                    c.rtArray > 1 ? fmt::format(" [{}]", c.rtArray) : "");
                    if (c.dsW) target += fmt::format("{}depth {}x{}", target.empty() ? "" : " + ", c.dsW, c.dsH);
                    if (c.uavW) target += fmt::format("{}uav {}x{}", target.empty() ? "" : " + ", c.uavW, c.uavH);
                    if (target.empty()) target = "none bound";
                }
                const double st = c.statsN ? (double)c.n / (double)c.statsN : 0.0;   // scale stats if some tasks lacked them
                LOGI("PassProfile:   {:<34} {:<6} {:7.3f} ({:6.3f}) | {:5.1f} | {:6.3f} | {:6.0f} ({:.0f}) | {:5.0f} | {:7.3f} | {:7.3f} / {:7.3f} | {:7.2f} | {:7.3f} | {} | thr {}",
                     RowName(r.row), kKindName[k], c.gpu / f, c.gpuMax, c.n / f, c.cpuUs / 1000.0 / f, c.draws / f, c.indirect / f, c.dispatches / f,
                     c.elems / f / 1e6, c.iaPrims * st / f / 1e6, c.cPrims * st / f / 1e6, c.psInv * st / f / 1e6, c.csInv * st / f / 1e6,
                     target.empty() ? "-" : target, c.threads.size());
            }
        }
        // the worst frame of the window, pass by pass
        int worst = -1; double worstMs = 0;
        for (int i = 0; i < kFrames; i++)
        {
            const FrameRows& fr = g_FR[i];
            if (!fr.frame || fr.frame + 8 > curFrame) continue;
            const FrameInfo& fi = g_FI[fr.frame % kFrames];
            if (fi.frame != fr.frame || fi.gpuMs < 0) continue;
            if (fi.gpuMs > worstMs) { worstMs = fi.gpuMs; worst = i; }
        }
        if (worst >= 0)
        {
            const FrameRows& fr = g_FR[worst];
            std::vector<std::pair<float, int>> v;
            for (int p = 0; p < kRows; p++) if (fr.ms[p] > 0) v.push_back({ fr.ms[p], p });
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
            std::string s;
            for (size_t i = 0; i < v.size() && i < 8; i++) s += fmt::format(" {} {:.2f}", RowName(v[i].second), v[i].first);
            LOGI("PassProfile: the slowest recent frame (#{}): GPU {:.2f} ms Present to Present, CPU {:.2f} ms; its passes:{}", fr.frame, worstMs, CpuMsOf(fr.frame), s);
        }
        g_Reports++;
    }
    void DumpFrame()
    {
        if (g_Dump.empty()) return;
        std::sort(g_Dump.begin(), g_Dump.end(), [](const TaskLine& a, const TaskLine& b) { return a.tb < b.tb; });
        const FrameInfo& prev = g_FI[(g_DumpFrame - 1) % kFrames];
        const UINT64 t0 = (prev.frame == g_DumpFrame - 1 && !prev.bad && prev.ts && prev.ts < g_Dump.front().tb) ? prev.ts : g_Dump.front().tb;
        LOGI("PassProfile: ----- frame #{} task by task, in GPU order (ms from the previous Present on the GPU; the gap before each task is GPU time "
             "no task covers: waiting for the CPU, or the mod's / the driver's own work) -----", g_DumpFrame);
        UINT64 lastEnd = t0;
        for (const TaskLine& t : g_Dump)
        {
            const double at = (double)(t.tb - t0) * 1000.0 / g_Freq, ms = (double)(t.te - t.tb) * 1000.0 / g_Freq;
            const double gap = t.tb > lastEnd ? (double)(t.tb - lastEnd) * 1000.0 / g_Freq : 0.0;
            LOGI("PassProfile:   {:8.3f} +{:6.3f} gap {:6.3f}  {:<34} {:<6} view {} cpu {:.3f} ms draws {} dispatches {} thread {}",
                 at, ms, gap, RowName(t.row), kKindName[t.kind], t.view, t.cpuUs / 1000.0, t.draws, t.dispatches, t.tid);
            if (t.te > lastEnd) lastEnd = t.te;
        }
        LOGI("PassProfile: ----- end of frame #{} ({} tasks) -----", g_DumpFrame, g_Dump.size());
        g_Dump.clear();
    }
    void ResetWindow()
    {
        for (auto& row : g_Cell) for (auto& c : row) c = Cell{};
        for (int k = 0; k < kKinds; k++) { g_FramesKind[k] = 0; g_Views[k].clear(); }
        g_FrameGpuSum = g_FrameGpuMax = g_FrameCpuSum = g_FrameCpuMax = g_HookCpuSum = 0; g_FrameGpuN = g_FrameCpuN = g_Presents = 0;
        for (int p = 0; p < kRows; p++) for (int k = 0; k < kKinds; k++) g_WantRt[p][k] = true;
    }
    void Take(Rec& r, UINT64 tb, UINT64 te)
    {
        const double ms = (double)(te - tb) * 1000.0 / (double)g_Freq;
        if (ms >= 100.0) return;
        Cell& c = g_Cell[r.row][r.kind];
        c.gpu += ms; c.gpuMax = std::max(c.gpuMax, ms); c.n++; c.cpuUs += r.cpuUs;
        c.draws += r.acc.draws; c.dispatches += r.acc.dispatches; c.indirect += r.acc.indirect; c.elems += r.acc.elems; c.groups += r.acc.groups;
        if (c.threads.size() < 64) c.threads.insert(r.tid);
        if (r.rtSampled && !c.rt)
        {
            c.rt = true; c.rtW = r.rtW; c.rtH = r.rtH; c.rtFmt = r.rtFmt; c.rtN = r.rtN; c.rtSamples = r.rtSamples; c.rtArray = r.rtArray;
            c.dsW = r.dsW; c.dsH = r.dsH; c.uavW = r.uavW; c.uavH = r.uavH;
        }
        g_Timed++;
        if (g_LastFrameKind[r.kind] != r.frame) { g_LastFrameKind[r.kind] = r.frame; g_FramesKind[r.kind]++; }
        if (r.view && g_Views[r.kind].size() < 256) g_Views[r.kind].insert(r.view);
        FrameRows& fr = g_FR[r.frame % kFrames];
        if (fr.frame != r.frame) { fr = FrameRows{}; fr.frame = r.frame; }
        fr.ms[r.row] += (float)ms;
        if (g_DumpFrame && r.frame == g_DumpFrame && g_Dump.size() < 4000)
            g_Dump.push_back({ tb, te, r.row, r.kind, r.cpuUs, r.acc.draws, r.acc.dispatches, r.tid, r.view });
    }
}

void PassProfile::SetEnabled(bool on)
{
    g_On = on;
    if (on) LOGI("PassProfile: GPU time, work and targets per render pass written to the log every 5 s, one frame task by task every 30 s "
                 "(ProfileRenderPasses = 1; costs some GPU and CPU time itself)");
}

void PassProfile::Install()
{
    for (int i = 0; i < kKnown; i++)
    {
        if (!strcmp(kPasses[i].name, "FORWARD_DYNAMIC")) g_RowOpaque = i;
        if (!strcmp(kPasses[i].name, "ALPHA_LIT")) g_RowTransparent = i;
        if (!strcmp(kPasses[i].name, "SSAO")) g_RowSsao = i;
        if (!strcmp(kPasses[i].name, "DOF")) g_RowDof = i;
        // Foveation: what shades the scene at the eye's resolution
        static const char* fov[] = { "DEPTHPASS", "DEPTHALPHA_LIT", "FORWARD_WORLD", "FORWARD_DYNAMIC", "RENDER_WATER", "ALPHA_LIT", "EMISSIVE_ONLY" };
        for (const char* n : fov) if (!strcmp(kPasses[i].name, n) && i < 64) g_FovRow[i] = true;
    }
    g_FogHook = Fog::Wanted();
    g_FovHook = Foveation::Wanted();
    if ((!g_On.load() && !g_FogHook.load() && !g_FovHook.load() && !g_SkipEffects.load()) || g_Exec) return;
    QueryPerformanceFrequency(&g_QpcF);
    for (int i = 0; i < kSections; i++) g_SecOpen[i] = -1;
    for (int p = 0; p < kRows; p++) for (int k = 0; k < kKinds; k++) g_WantRt[p][k] = true;
    g_Base = GameInfo::Base();
    g_Exec = (ExecFn)(g_Base + 0x2C5D50);
    g_ExecImm = (ExecFn)(g_Base + 0x2C5F60);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_Exec, (PVOID)&HkExec<0>);
    DetourAttach(&(PVOID&)g_ExecImm, (PVOID)&HkExec<1>);
    const LONG e = DetourTransactionCommit();
    LOGI("PassProfile: task executors exe+0x2C5D50 / exe+0x2C5F60 hooked (detour={})", e);
    if (e != NO_ERROR) { g_On = false; g_FogHook = false; g_FovHook = false; g_SkipEffects = false; }
}

void PassProfile::SetSkipEffects(int mode)
{
    g_SkipEffects = mode >= 1; g_SkipDof = mode >= 2;
    if (mode >= 1) LOGI("PassProfile: SkipDisabledEffects = {} (experimental): {} leftover work is skipped while {} off", mode,
                        mode >= 2 ? "the SSAO and DOF passes'" : "the SSAO pass's", mode >= 2 ? "those effects are" : "it is");
}

bool PassProfile::On() { return g_On.load(std::memory_order_relaxed); }

void PassProfile::NoteDraw(unsigned elements, unsigned instances, bool indirect)
{
    if (t_Caps.on) CaptureTargets();
    if (!t_Acc.on) return;
    t_Acc.draws++; if (indirect) t_Acc.indirect++;
    t_Acc.elems += (uint64_t)elements * (instances ? instances : 1);
}
void PassProfile::NoteDispatch(unsigned x, unsigned y, unsigned z, bool indirect)
{
    if (!t_Acc.on) return;
    t_Acc.dispatches++; if (indirect) t_Acc.indirect++;
    t_Acc.groups += (uint64_t)x * y * z;
}

void PassProfile::BeginSection(int s)
{   // the mod's own GPU work at Present, on the immediate context (render thread)
    if (!g_On.load() || !g_Imm || s < 0 || s >= kSections || g_SecOpen[s] >= 0) return;
    g_SecOpen[s] = Open(g_Imm, kSecBase + s, kNoView, nullptr);
    if (s == 0) QueryPerformanceCounter(&g_PresentT0);
}
void PassProfile::EndSection(int s)
{
    if (!g_On.load() || !g_Imm || s < 0 || s >= kSections || g_SecOpen[s] < 0) return;
    Rec& r = g_Rec[g_SecOpen[s]];
    if (s == 0)
    {
        LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
        r.cpuUs = (double)(t1.QuadPart - g_PresentT0.QuadPart) * 1e6 / (double)g_QpcF.QuadPart;
        g_HookCpuSum += r.cpuUs / 1000.0;
    }
    if (r.ps) g_Imm->End(r.ps);
    g_Imm->End(r.e);
    r.state = 2;
    g_SecOpen[s] = -1;
}

void PassProfile::OnPresent(IDXGISwapChain* sc)
{
    if (g_SkipEffects.load(std::memory_order_relaxed) && sc) ResolveFill(sc);
    if (!g_On.load() || !g_Exec) return;
    if (!g_Dev)
    {
        ID3D11Device* d = nullptr;
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&d)) || !d) return;
        g_Dev = d;                                            // kept for the process's life
        g_Dev->GetImmediateContext(&g_Imm);                   // likewise
        g_DevReady.store(true, std::memory_order_release);
        ResetWindow();
    }
    ID3D11DeviceContext* imm = g_Imm;
    if (!imm) return;
    const unsigned long long f = g_Frame.load();
    g_Presents++;
    {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (g_LastPresent.QuadPart)
        {
            const double ms = (double)(now.QuadPart - g_LastPresent.QuadPart) * 1000.0 / (double)g_QpcF.QuadPart;
            if (ms < 500) { g_FrameCpuSum += ms; g_FrameCpuN++; g_FrameCpuMax = std::max(g_FrameCpuMax, ms); g_CpuMs[f % kFrames] = ms; g_CpuFrame[f % kFrames] = f; }
        }
        g_LastPresent = now;
    }

    // the frame queries: read the ones a few Presents old, then open this Present's
    for (int i = 0; i < kFrames; i++)
    {
        FrameQ& q = g_FQ[i];
        if (!q.issued || q.frame + 6 > f) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{}; UINT64 t = 0;
        if (imm->GetData(q.disj, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && imm->GetData(q.ts, &t, sizeof t, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
        {
            FrameInfo& fi = g_FI[q.frame % kFrames];
            fi = FrameInfo{}; fi.frame = q.frame; fi.freq = dj.Frequency; fi.bad = dj.Disjoint || !dj.Frequency; fi.ts = t;
            if (!fi.bad) g_Freq = dj.Frequency;
            const FrameInfo& pv = g_FI[(q.frame - 1) % kFrames];
            if (!fi.bad && !pv.bad && pv.frame + 1 == fi.frame && fi.ts > pv.ts && g_Freq)
            {
                const double ms = (double)(fi.ts - pv.ts) * 1000.0 / (double)g_Freq;
                if (ms < 200) { fi.gpuMs = ms; g_FrameGpuSum += ms; g_FrameGpuN++; g_FrameGpuMax = std::max(g_FrameGpuMax, ms); }
            }
            q.issued = false;
        }
        else if (q.frame + kFrames - 2 <= f) q.issued = false;   // never came: let the slot go
    }
    {
        FrameQ& q = g_FQ[f % kFrames];
        if (!q.disj)
        {
            D3D11_QUERY_DESC dd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, td = { D3D11_QUERY_TIMESTAMP, 0 };
            g_Dev->CreateQuery(&dd, &q.disj); g_Dev->CreateQuery(&td, &q.ts);
        }
        if (q.disj && q.ts && !q.issued)
        {
            static ID3D11Query* open = nullptr;
            if (open) imm->End(open);                         // the previous frame's disjoint ends here
            imm->Begin(q.disj); imm->End(q.ts);
            open = q.disj; q.issued = true; q.frame = f;
        }
    }
    g_Frame = f + 1;

    // a frame task by task every 30 s
    const ULONGLONG nowMs = GetTickCount64();
    if (!g_NextDumpAt) g_NextDumpAt = nowMs + 12000;
    if (!g_DumpFrame && nowMs >= g_NextDumpAt) { g_DumpFrame = f + 1; g_DumpUntil = f + 1 + 12; g_Dump.clear(); }

    // read the task records that are old enough
    if (g_Freq)
    {
        int reads = 0;
        for (int i = 0; i < kSlots && reads < 6000; i++)
        {
            Rec& r = g_Rec[i];
            if (r.state.load(std::memory_order_acquire) != 2) continue;
            if (r.frame + 4 > f) continue;
            UINT64 tb = 0, te = 0; reads++;
            const bool in = imm->GetData(r.b, &tb, sizeof tb, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
                         && imm->GetData(r.e, &te, sizeof te, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            if (in)
            {
                D3D11_QUERY_DATA_PIPELINE_STATISTICS ps{};
                const bool stats = r.ps && imm->GetData(r.ps, &ps, sizeof ps, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
                if (te >= tb)
                {
                    Take(r, tb, te);
                    Cell& c = g_Cell[r.row][r.kind];
                    if (stats) { c.iaPrims += ps.IAPrimitives; c.vsInv += ps.VSInvocations; c.cPrims += ps.CPrimitives; c.psInv += ps.PSInvocations; c.csInv += ps.CSInvocations; c.statsN++; }
                    else g_NoStats++;
                }
                r.state = 0;
            }
            else if (r.frame + 40 < f) { g_Lost++; r.state = 0; }   // never executed (a command list the engine dropped)
        }
    }
    if (g_DumpFrame && f >= g_DumpUntil) { DumpFrame(); g_DumpFrame = 0; g_NextDumpAt = nowMs + 30000; }

    if (!g_WindowStart) g_WindowStart = nowMs;
    if (nowMs - g_WindowStart >= 5000)
    {
        Report((nowMs - g_WindowStart) / 1000.0, f);
        ResetWindow();
        g_WindowStart = nowMs;
    }
}

std::string PassProfile::Status()
{
    return fmt::format("passprofile on {} reports {} timed {} dropped {} lost {}", g_On.load() ? 1 : 0, g_Reports, g_Timed.load(), g_Dropped.load(), g_Lost.load());
}
