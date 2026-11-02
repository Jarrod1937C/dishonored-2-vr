#include "pch.h"
#include "PassProfile.h"
#include "GameInfo.h"
#include "CameraHook.h"
#include "ViewMatrixHook.h"
#include "StereoSync.h"
#include "CVars.h"
#include "Fog.h"
#include "Foveation.h"
#include "FovMask.h"
#include "ScreenShadows.h"
#include "VrTweaks.h"
#include <d3d11.h>
#include <dxgi1_4.h>
#include <detours/detours.h>
#include <algorithm>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <thread>
#include <d3d11shader.h>
#include "GpuTrace.h"
#include "PostShare.h"
#include "PlayerState.h"
#include "VRRuntime.h"

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
        { 0x2FD8D0, "DEPTHPASS" }, { 0x2FC130, "DEPTHMINMAX" }, { 0x305BE0, "SSAO" }, { 0x32F390, "SHADOWS_WORLD lamps" },
        { 0x32F380, "SHADOWS_DYNAMIC lamps" }, { 0x310190, "IL_ACCUMULATE_FINAL_SH" }, { 0x3102A0, "IL_UPDATE_IBL" },
        { 0x310260, "IL_UPDATE_PROBES" }, { 0x32C7F0, "SCREENSHADOWS" }, { 0x2F7260, "TLFLIGHTCULL" }, { 0x3133D0, "FORWARD_WORLD" },
        { 0x3133B0, "FORWARD_DYNAMIC" }, { 0x309430, "FOG" }, { 0x3053B0, "UPDATE_OCEAN_FFT" }, { 0x305370, "UPDATE_OCEAN" },
        { 0x3045A0, "RENDER_WATER" }, { 0x3222A0, "DEPTHALPHA_LIT" }, { 0x3150D0, "DOF_AUTOFOCUS" }, { 0x314B10, "DOF_ALPHA" },
        { 0x315450, "DOF" }, { 0x31F220, "EMISSIVE" }, { 0x31DCE0, "ALPHA_LIT" }, { 0x323210, "EMISSIVE_ONLY" },
        { 0x31F960, "DARKVISION" }, { 0x3222F0, "DISTORTION" }, { 0x327270, "POSTPROC" }, { 0x323E70, "POST_TONEMAP" },
        { 0x34FD00, "TEXTURE_STREAMER" }, { 0x31A690, "IGGY_MANAGE_TEXTURES_PACKS" }, { 0x31A710, "IGGY_3D" },
        { 0x31A8E0, "IGGY_FULLSCREEN" }, { 0x31AC40, "IGGY_TEXTURES_SUBSTITUTIONS" }, { 0x31AB10, "POST_IGGY" },
        { 0x31D7E0, "OCCLUSION_QUERIES" },
        // the shadow passes' sun-cascade tasks, counted apart from their spot / omni (lamp) tasks: no function of their
        // own (both run exe+0x32D4B0, which takes task+0xF8 < r_shadowParallelSliceCnt as a cascade)
        { 0, "SHADOWS_WORLD sun" }, { 0, "SHADOWS_DYNAMIC sun" },
    };
    constexpr int kKnown = (int)(sizeof kPasses / sizeof kPasses[0]);
    // after the passes: a task whose function is not in the table, then the mod's own work at Present
    constexpr int kOtherPass = kKnown;
    constexpr int kSecBase = kKnown + 1;
    const char* kSecName[PassProfile::kSections] = { "mod: Present hook (all of it)", "mod: StereoTaa at Present", "mod: overlay + VR submit",
        "mod:   overlay draw", "mod:   VR stage left eye (copy)", "mod:   VR BeginFrame (all)", "mod:     xrBeginFrame + views", "mod:   VR wrist HUD",
        "mod:   VR screen mode / game HUD", "mod:   VR eye blits (DrawEye, all)", "mod:     swapchain acquire + wait", "mod:   VR layers + EndFrame (all)", "mod:     xrEndFrame call",
        "mod:   VR mirror copy", "mod:   VR menu backdrop copy", "mod: GpuTrace at Present", "mod: game-state ticks at Present",
        "mod: this profiler at Present", "mod: input / HUD after the submit" };
    constexpr int kRows = kSecBase + PassProfile::kSections;
    const char* RowName(int r) { return r < kKnown ? kPasses[r].name : r == kOtherPass ? "(other function)" : kSecName[r - kSecBase]; }

    enum Kind { kLeft, kRight, kOtherView, kNoView, kKinds };
    const char* kKindName[kKinds] = { "L", "R", "other", "noview" };

    std::atomic<bool> g_On{ false };              // profiling
    std::atomic<bool> g_FogHook{ false };         // Fog's anchors (FORWARD_DYNAMIC end, ALPHA_LIT start) need the hook too
    int g_RowOpaque = -1, g_RowTransparent = -1, g_RowWater = -1;  // FORWARD_DYNAMIC, ALPHA_LIT, RENDER_WATER
    std::atomic<bool> g_SssHook{ false };         // ScreenShadows: the end of SCREENSHADOWS (the sun's mask still bound)
    int g_RowScreenShadows = -1;
    std::atomic<bool> g_FovHook{ false };         // Foveation: the scene passes, at the eye's resolution
    std::atomic<bool> g_MaskHook{ false };        // FovMask: the depth pass of each eye's main view
    std::atomic<bool> g_DepthOnly{ false };       // DepthPassDepthOnly (experimental): its main shader drawn depth-only
    thread_local bool t_DepthOnly = false;
    std::atomic<bool> g_DepthCopies{ false };     // DepthPassSkipRedundantCopies: GpuTrace skips a depth copy that changes nothing
    thread_local bool t_DepthCopies = false;
    // SkipSneakOverlay: the POSTPROC draw that lays the sneak mode's screen overlay (PS 2ee2b9822f61d7d1, reading
    // ro_fx_opacity) over each eye's picture -- in the headset a tiny black square fixed in each eye's view
    std::atomic<bool> g_SneakFx{ false };
    thread_local bool t_PostFx = false;
    thread_local unsigned long long t_DepthSerial = 0;   // which depth task (GpuTrace's copy tracking starts over with each)
    std::atomic<unsigned long long> g_DepthSerial{ 0 };
    int g_RowDepth = -1;                          // DEPTHPASS
    // the DEPTHPASS task running on this thread, until its first draw into the scene's depth buffer (FovMask)
    struct MaskT { ID3D11DeviceContext* c = nullptr; const void* view = nullptr; int eye = -1, tries = 0; bool on = false; };
    thread_local MaskT t_Mask;
    // ScreenShadows: a SCREENSHADOWS task of a main view running on this thread (its draws looked at)
    thread_local ID3D11DeviceContext* t_SssCtx = nullptr;
    thread_local const void* t_SssView = nullptr;
    thread_local int t_SssEye = -1;
    int g_RowShWorld = -1, g_RowShDyn = -1, g_RowShWorldSun = -1, g_RowShDynSun = -1;
    // ScreenShadows' characters: 1 = a sun cascade's shadow task runs on this thread, 2 = an eye's main depth pass
    std::atomic<bool> g_CharHook{ false };
    thread_local int t_CharTask = 0, t_CharEye = -1;
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
    struct Acc { bool on = false; uint32_t draws = 0, dispatches = 0, indirect = 0; uint64_t elems = 0, groups = 0;
                 uint32_t mapDiscard = 0, mapNoOver = 0, mapRead = 0, mapOther = 0, updates = 0, copies = 0; };
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
    bool ReadInt(const void* p, int& v)
    {
        __try { v = *(const int*)p; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
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
    // ---- draw by draw (ProfileRenderPasses): one left-eye frame of a few passes, every draw timed with its own queries,
    // 40 s after the profile starts and every 2 minutes. DEPTHPASS is summed by shader pair (what makes it shade 8x the
    // screen); the full-screen passes (POSTPROC, EMISSIVE_ONLY, ...) are listed draw by draw with the textures they read.
    struct DRec
    {
        std::atomic<int> state{ 0 };                       // 0 free, 1 open, 2 issued
        ID3D11Query* b = nullptr; ID3D11Query* e = nullptr; ID3D11Query* ps = nullptr;
        unsigned long long frame = 0; int row = 0, idx = 0; DWORD tid = 0;
        uint64_t psHash = 0, vsHash = 0; const void* psPtr = nullptr; const void* cs = nullptr;
        uint32_t elems = 0, inst = 0, rtW = 0, rtH = 0, rtFmt = 0, rtN = 0; bool depth = false, indirect = false, dispatch = false;
        const void* srcRes = nullptr; const void* rtvRes = nullptr; const void* dsvRes = nullptr;   // t0, the first target, the depth
        uint32_t srcW = 0, srcH = 0, srcFmt = 0, dsW = 0, dsH = 0, dsFmt = 0;
    };
    constexpr int kDSlots = 8192;
    // Which passes read what (the draw-by-draw frame): every left-eye task's draws and dispatches, the resources behind
    // their pixel / compute shaders' views, so that what a DEPTHPASS copy writes can be followed to its readers.
    struct Rd { bool on = false; ID3D11DeviceContext* c = nullptr; int row = -1; };
    thread_local Rd t_Rd;
    std::mutex g_ReadM;
    std::unordered_map<const void*, std::pair<uint64_t, uint64_t>> g_Reads;   // resource -> the rows that read it (bits 0..127)
    std::unordered_map<const void*, std::pair<uint64_t, uint64_t>> g_Targets; // resource -> the rows that draw into it (colour or depth)
    void NoteReads(ID3D11DeviceContext* c, bool dispatch)
    {
        if (!c || c != t_Rd.c || t_Rd.row < 0 || t_Rd.row >= 128) return;
        ID3D11ShaderResourceView* v[16] = {};
        if (dispatch) c->CSGetShaderResources(0, 16, v); else c->PSGetShaderResources(0, 16, v);
        const void* res[16] = {}; int n = 0;
        for (int i = 0; i < 16; i++)
            if (v[i]) { ID3D11Resource* r = nullptr; v[i]->GetResource(&r); if (r) { res[n++] = r; r->Release(); } v[i]->Release(); }
        const void* tg[9] = {}; int nt = 0;
        if (!dispatch)
        {   // the targets it draws into (and depth-tests against): a buffer that is only a depth buffer later is used too
            ID3D11RenderTargetView* rtv[8] = {}; ID3D11DepthStencilView* dsv = nullptr;
            c->OMGetRenderTargets(8, rtv, &dsv);
            for (int i = 0; i < 8; i++) if (rtv[i]) { ID3D11Resource* r = nullptr; rtv[i]->GetResource(&r); if (r) { tg[nt++] = r; r->Release(); } rtv[i]->Release(); }
            if (dsv) { ID3D11Resource* r = nullptr; dsv->GetResource(&r); if (r) { tg[nt++] = r; r->Release(); } dsv->Release(); }
        }
        if (!n && !nt) return;
        std::lock_guard lk(g_ReadM);
        if (g_Reads.size() > 20000 || g_Targets.size() > 20000) return;
        auto mark = [&](std::pair<uint64_t, uint64_t>& b) { if (t_Rd.row < 64) b.first |= 1ull << t_Rd.row; else b.second |= 1ull << (t_Rd.row - 64); };
        for (int i = 0; i < n; i++) mark(g_Reads[res[i]]);
        for (int i = 0; i < nt; i++) mark(g_Targets[tg[i]]);
    }
    std::string ReadersOf(const void* res, bool targets = false)
    {
        std::lock_guard lk(g_ReadM);
        auto& m = targets ? g_Targets : g_Reads;
        auto it = m.find(res);
        if (it == m.end()) return "none in this frame";
        std::string out;
        for (int r = 0; r < 128; r++)
            if (r < 64 ? (it->second.first >> r) & 1 : (it->second.second >> (r - 64)) & 1) out += fmt::format("{}{}", out.empty() ? "" : ", ", RowName(r));
        return out;
    }
    DRec g_D[kDSlots];
    std::atomic<unsigned long long> g_DNext{ 0 };
    std::atomic<unsigned long long> g_DescFrame{ 0 };       // the frame being described (0 none)
    std::atomic<int> g_DescDrops{ 0 };
    int g_RowPost = -1, g_RowEmissive = -1, g_RowTonemap = -1, g_RowClear = -1, g_RowFogPass = -1;
    bool g_DescExtra[64] = {};   // more passes described draw by draw: the screen-space ones that could draw a square into an eye
    struct Desc { bool on = false; ID3D11DeviceContext* c = nullptr; int row = -1, open = -1, idx = 0; unsigned long long frame = 0; };
    thread_local Desc t_Desc;
    bool DescRow(int row)
    {
        return row >= 0 && (row == g_RowDepth || row == g_RowPost || row == g_RowEmissive || row == g_RowTonemap || row == g_RowSsao
                            || row == g_RowDof || row == g_RowClear || row == g_RowFogPass || (row < 64 && g_DescExtra[row]));   // FOG: what the game's fog reads (2026-11-01: none in the Void)
    }
    void DescClose()
    {
        if (t_Desc.open < 0) return;
        DRec& d = g_D[t_Desc.open];
        if (d.ps) t_Desc.c->End(d.ps);
        t_Desc.c->End(d.e);
        d.state = 2; t_Desc.open = -1;
    }
    //! at a draw or dispatch of a described task: the previous one's queries end, this one's begin
    void DescDraw(ID3D11DeviceContext* c, uint32_t elems, uint32_t inst, bool indirect, bool dispatch)
    {
        if (!c || c != t_Desc.c) return;
        DescClose();
        const int i = (int)(g_DNext.fetch_add(1) % kDSlots);
        DRec& d = g_D[i];
        int expect = 0;
        if (!d.state.compare_exchange_strong(expect, 1)) { g_DescDrops++; return; }
        if (!d.b)
        {
            D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP, 0 }, sd = { D3D11_QUERY_PIPELINE_STATISTICS, 0 };
            if (FAILED(g_Dev->CreateQuery(&qd, &d.b)) || FAILED(g_Dev->CreateQuery(&qd, &d.e))) { if (d.b) { d.b->Release(); d.b = nullptr; } d.state = 0; g_DescDrops++; return; }
            if (FAILED(g_Dev->CreateQuery(&sd, &d.ps))) d.ps = nullptr;
        }
        d.frame = t_Desc.frame; d.row = t_Desc.row; d.idx = t_Desc.idx++; d.tid = GetCurrentThreadId();
        d.elems = elems; d.inst = inst; d.indirect = indirect; d.dispatch = dispatch;
        d.psHash = d.vsHash = 0; d.psPtr = d.cs = nullptr; d.rtW = d.rtH = d.rtFmt = d.rtN = 0; d.depth = false;
        d.srcRes = d.rtvRes = d.dsvRes = nullptr; d.srcW = d.srcH = d.srcFmt = d.dsW = d.dsH = d.dsFmt = 0;
        if (dispatch)
        {
            ID3D11ComputeShader* cs = nullptr; c->CSGetShader(&cs, nullptr, nullptr);
            if (cs) { d.cs = cs; d.psHash = GpuTrace::ShaderHashOf(cs); cs->Release(); }
        }
        else
        {
            ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr);
            if (ps) { d.psPtr = ps; d.psHash = GpuTrace::ShaderHashOf(ps); ps->Release(); }
            ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr);
            if (vs) { d.vsHash = GpuTrace::ShaderHashOf(vs); vs->Release(); }
            ID3D11RenderTargetView* rtv[8] = {}; ID3D11DepthStencilView* dsv = nullptr;
            c->OMGetRenderTargets(8, rtv, &dsv);
            for (int k = 0; k < 8; k++)
                if (rtv[k])
                {
                    if (!d.rtN++)
                    {
                        ID3D11Resource* r = nullptr; rtv[k]->GetResource(&r);
                        d.rtW = TexSize(r, &d.rtH, &d.rtFmt, nullptr, nullptr);
                        d.rtvRes = r;
                        if (r) r->Release();
                    }
                    rtv[k]->Release();
                }
            if (dsv)
            {
                d.depth = true;
                ID3D11Resource* r = nullptr; dsv->GetResource(&r);
                if (r) { d.dsvRes = r; d.dsW = TexSize(r, &d.dsH, &d.dsFmt, nullptr, nullptr); r->Release(); }
                dsv->Release();
            }
            ID3D11ShaderResourceView* t0 = nullptr; c->PSGetShaderResources(0, 1, &t0);
            if (t0)
            {
                ID3D11Resource* r = nullptr; t0->GetResource(&r);
                if (r) { d.srcRes = r; d.srcW = TexSize(r, &d.srcH, &d.srcFmt, nullptr, nullptr); r->Release(); }
                t0->Release();
            }
        }
        c->End(d.b);
        if (d.ps) c->Begin(d.ps);
        t_Desc.open = i;
    }
    //! the textures a pixel / compute shader reads, by name (its reflection), cached by hash
    std::string ShaderReads(const void* sh, uint64_t hash)
    {
        static std::unordered_map<uint64_t, std::string> cache;
        if (!sh || !hash) return "?";
        if (auto it = cache.find(hash); it != cache.end()) return it->second;
        std::string out;
        std::vector<uint8_t> code;
        if (GpuTrace::ShaderHashOf(sh) == hash && GpuTrace::ShaderBytecode(sh, code) && code.size() > 32)
        {
            using Reflect_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
            static Reflect_t reflect = nullptr; static bool tried = false;
            if (!tried) { tried = true; if (HMODULE d = LoadLibraryA("d3dcompiler_47.dll")) reflect = (Reflect_t)GetProcAddress(d, "D3DReflect"); }
            ID3D11ShaderReflection* r = nullptr;
            if (reflect && SUCCEEDED(reflect(code.data(), code.size(), __uuidof(ID3D11ShaderReflection), (void**)&r)) && r)
            {
                D3D11_SHADER_DESC sd{};
                if (SUCCEEDED(r->GetDesc(&sd)))
                {
                    int n = 0;
                    for (UINT i = 0; i < sd.BoundResources && n < 10; i++)
                    {
                        D3D11_SHADER_INPUT_BIND_DESC b{};
                        if (FAILED(r->GetResourceBindingDesc(i, &b)) || !b.Name || b.Type == D3D_SIT_SAMPLER || b.Type == D3D_SIT_CBUFFER) continue;
                        const bool uav = b.Type >= D3D_SIT_UAV_RWTYPED && b.Type != D3D_SIT_TBUFFER && b.Type != D3D_SIT_TEXTURE && b.Type != D3D_SIT_STRUCTURED && b.Type != D3D_SIT_BYTEADDRESS;
                        out += fmt::format("{}{}{}", n++ ? " " : "", uav ? "w:" : "", b.Name);
                    }
                    out += fmt::format(" [{} instr]", sd.InstructionCount);
                }
                r->Release();
            }
        }
        if (out.empty()) out = "?";
        if (cache.size() < 4096) cache[hash] = out;
        return out;
    }

    // ---- FogProbe (ProfileRenderPasses): what the game's own FOG pass wrote, read back once in a while ----
    // Reported 2026-11-01: no game fog in the Void although r_skipFog is back to 0 there (FogMaps) and the FOG pass runs.
    // In the left eye's FOG task every target its draws write (colour and depth, up to 6) is noted at its draws; at the
    // end of the task each is copied (on the task's own context) into a staging texture, mapped a few Presents later and
    // summarised: an all-zero fog buffer (nothing to fog, or the fog's parameters zero) reads differently from a fog that
    // is there but never put on the scene. The view's own fog-skip flag (view+0x3A36 bit 0) and r_skipFog go with it.
    // (2026-11-01, first run: only the target bound at the task's end was read -- a 1/4-size RGBA16F, all zero.)
    namespace FogProbe
    {
        constexpr int kMax = 6;
        std::mutex m;
        struct Img { ID3D11Texture2D* st = nullptr; D3D11_TEXTURE2D_DESC d{}; int draws = 0; bool depth = false; };
        Img img[kMax]; int nImg = 0;
        std::atomic<int> state{ 0 };     // 0 idle, 1 copies recorded
        unsigned long long recordedAt = 0;
        std::atomic<unsigned long long> presentCount{ 0 };
        int count = 0, viewFlag = -1, skipFog = -1, draws = 0;
        ULONGLONG lastAt = 0, armedAt = 0;
        struct T { bool on = false; ID3D11Resource* res[kMax] = {}; bool depth[kMax] = {}; int uses[kMax] = {}; int n = 0, draws = 0; };
        thread_local T t_T;
        int ByteAt(const void* p) { __try { return *(const volatile uint8_t*)p; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; } }
        //! at the start of the left eye's FOG task: whether this one is read back
        bool Want()
        {
            if (state.load(std::memory_order_relaxed)) return false;
            const ULONGLONG now = GetTickCount64();
            std::lock_guard lk(m);
            if (count >= 12) return false;
            static std::string lastMap;
            if (std::string map = VrTweaks::MapName(); map != lastMap) { lastMap = map; armedAt = now; lastAt = 0; return false; }   // a new map: 15 s to settle
            if (now - armedAt < 15000 || (lastAt && now - lastAt < 90000)) return false;
            lastAt = now;
            return true;
        }
        void Begin() { t_T = T{}; t_T.on = true; }
        void Note(ID3D11Resource* r, bool depth)
        {
            for (int i = 0; i < t_T.n; i++) if (t_T.res[i] == r) { t_T.uses[i]++; return; }
            if (t_T.n >= kMax) return;
            r->AddRef(); t_T.res[t_T.n] = r; t_T.depth[t_T.n] = depth; t_T.uses[t_T.n] = 1; t_T.n++;
        }
        //! at each draw of that task: the targets it writes
        void AtDraw(ID3D11DeviceContext* c)
        {
            if (!t_T.on) return;
            t_T.draws++;
            ID3D11RenderTargetView* rtv[8] = {}; ID3D11DepthStencilView* dsv = nullptr;
            c->OMGetRenderTargets(8, rtv, &dsv);
            for (int k = 0; k < 8; k++) if (rtv[k]) { ID3D11Resource* r = nullptr; rtv[k]->GetResource(&r); if (r) { Note(r, false); r->Release(); } rtv[k]->Release(); }
            if (dsv) { ID3D11Resource* r = nullptr; dsv->GetResource(&r); if (r) { Note(r, true); r->Release(); } dsv->Release(); }
        }
        bool Copy(ID3D11DeviceContext* c, ID3D11Resource* r, Img& o)
        {
            D3D11_RESOURCE_DIMENSION dim{}; r->GetType(&dim);
            if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
            D3D11_TEXTURE2D_DESC d{}; static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
            if (d.SampleDesc.Count != 1) return false;
            D3D11_TEXTURE2D_DESC s = d; s.MipLevels = 1; s.ArraySize = 1; s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0;
            s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
            ID3D11Device* dev = nullptr; c->GetDevice(&dev);
            if (!dev) return false;
            const bool ok = SUCCEEDED(dev->CreateTexture2D(&s, nullptr, &o.st)) && o.st;
            dev->Release();
            if (!ok) { LOGI("FogProbe: no staging copy of a {}x{} format {} texture", d.Width, d.Height, (int)d.Format); return false; }
            c->CopySubresourceRegion(o.st, 0, 0, 0, 0, r, 0, nullptr);
            o.d = s;
            return true;
        }
        //! at the end of that task (the game's worker thread)
        void AtEnd(ID3D11DeviceContext* c, const void* view)
        {
            T tt = t_T; t_T = T{};
            std::lock_guard lk(m);
            nImg = 0;
            for (int i = 0; i < tt.n; i++)
            {
                Img& o = img[nImg];
                if (!state.load() && Copy(c, tt.res[i], o)) { o.draws = tt.uses[i]; o.depth = tt.depth[i]; nImg++; }
                tt.res[i]->Release();
            }
            if (state.load() || !nImg) return;
            draws = tt.draws;
            viewFlag = view ? ByteAt((const uint8_t*)view + 0x3A36) : -1;
            skipFog = CVars::GetInt("r_skipFog", -1);
            state = 1; recordedAt = presentCount.load();
        }
        float Half(uint16_t h)
        {
            const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, f = h & 1023;
            float v = e == 0 ? ldexpf((float)f, -24) : e == 31 ? (f ? NAN : INFINITY) : ldexpf((float)(f | 1024), (int)e - 25);
            return s ? -v : v;
        }
        std::string Summary(ID3D11DeviceContext* imm, const Img& o)
        {
            D3D11_MAPPED_SUBRESOURCE ms{};
            if (FAILED(imm->Map(o.st, 0, D3D11_MAP_READ, 0, &ms)) || !ms.pData) return "could not be read";
            const DXGI_FORMAT f = o.d.Format;
            const UINT W = o.d.Width, H = o.d.Height;
            std::string s;
            if (f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16G16B16A16_TYPELESS)
            {
                double sum[4] = {}; float mx[4] = { -1e30f, -1e30f, -1e30f, -1e30f }, mn[4] = { 1e30f, 1e30f, 1e30f, 1e30f };
                unsigned long long n = 0, zero = 0, bad = 0;
                for (UINT y = 0; y < H; y += 4)
                {
                    const uint16_t* row = (const uint16_t*)((const uint8_t*)ms.pData + (size_t)y * ms.RowPitch);
                    for (UINT x = 0; x < W; x += 4)
                    {
                        float v[4]; bool allZero = true, nan = false;
                        for (int k = 0; k < 4; k++) { v[k] = Half(row[x * 4 + k]); if (v[k] != 0.f) allZero = false; if (!std::isfinite(v[k])) nan = true; }
                        n++; if (allZero) zero++; if (nan) { bad++; continue; }
                        for (int k = 0; k < 4; k++) { sum[k] += v[k]; mx[k] = std::max(mx[k], v[k]); mn[k] = std::min(mn[k], v[k]); }
                    }
                }
                const double g = (double)std::max<unsigned long long>(1, n - bad);
                s = fmt::format("{}x{} RGBA16F: mean ({:.4f}, {:.4f}, {:.4f}, {:.4f}), min ({:.3f}, {:.3f}, {:.3f}, {:.3f}), max ({:.3f}, {:.3f}, {:.3f}, {:.3f}); {:.1f}% all zero, {:.1f}% not finite",
                                W, H, sum[0] / g, sum[1] / g, sum[2] / g, sum[3] / g, mn[0], mn[1], mn[2], mn[3], mx[0], mx[1], mx[2], mx[3],
                                100.0 * zero / std::max<unsigned long long>(1, n), 100.0 * bad / std::max<unsigned long long>(1, n));
            }
            else if (f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_D24_UNORM_S8_UINT)
            {
                double sum = 0; float mn = 2.f, mx = -1.f; unsigned long long n = 0, at0 = 0, at1 = 0;
                for (UINT y = 0; y < H; y += 4)
                {
                    const uint32_t* row = (const uint32_t*)((const uint8_t*)ms.pData + (size_t)y * ms.RowPitch);
                    for (UINT x = 0; x < W; x += 4)
                    {
                        const uint32_t raw = row[x] & 0xFFFFFF;
                        const float d = raw / 16777215.f;
                        n++; sum += d; mn = std::min(mn, d); mx = std::max(mx, d);
                        if (raw == 0) at0++; else if (raw == 0xFFFFFF) at1++;
                    }
                }
                s = fmt::format("{}x{} D24S8: depth mean {:.5f}, min {:.5f}, max {:.5f}; {:.1f}% exactly 0, {:.1f}% exactly 1", W, H, sum / std::max<unsigned long long>(1, n),
                                mn, mx, 100.0 * at0 / std::max<unsigned long long>(1, n), 100.0 * at1 / std::max<unsigned long long>(1, n));
            }
            else s = fmt::format("{}x{} format {}: not summarised", W, H, (int)f);
            imm->Unmap(o.st, 0);
            return s;
        }
        //! at Present (the render thread): the copies are read once their command lists have run
        void OnPresent(ID3D11DeviceContext* imm)
        {
            const unsigned long long presentsNow = presentCount.fetch_add(1) + 1;
            if (state != 1) return;   // (read unlocked: only this thread clears it)
            std::lock_guard lk(m);
            if (state != 1 || presentsNow < recordedAt + 4) return;
            count++;
            LOGI("FogProbe #{}: the left eye's FOG pass: {} draws into {} targets; the view's fog-skip flag (view+0x3A36 bit 0) {}, r_skipFog {}; map '{}'", count,
                 draws, nImg, viewFlag < 0 ? std::string("?") : std::to_string(viewFlag & 1), skipFog, VrTweaks::MapName());
            for (int i = 0; i < nImg; i++)
                LOGI("FogProbe #{}:   {} {} (drawn into by {} of its draws): {}", count, img[i].depth ? "depth" : "target", i, img[i].draws, Summary(imm, img[i]));
            for (Img& i : img) { if (i.st) i.st->Release(); i = Img{}; }
            nImg = 0;
            state = 0;
        }
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
        const MaskT savedMask = t_Mask;
        ID3D11DeviceContext* const savedSss = t_SssCtx; const void* const savedSssView = t_SssView; const int savedSssEye = t_SssEye;
        t_SssCtx = c && (kind == kLeft || kind == kRight) && row == g_RowScreenShadows && g_SssHook.load(std::memory_order_relaxed) ? c : nullptr;
        t_SssView = view; t_SssEye = eye;
        const int savedCharTask = t_CharTask, savedCharEye = t_CharEye;
        t_CharTask = !c || !g_CharHook.load(std::memory_order_relaxed) ? 0
                   : (row == g_RowShWorldSun || row == g_RowShDynSun) ? 1
                   : (row == g_RowDepth && (kind == kLeft || kind == kRight)) ? 2 : 0;
        t_CharEye = eye;
        const bool savedDepthOnly = t_DepthOnly, savedDepthCopies = t_DepthCopies; const unsigned long long savedDepthSerial = t_DepthSerial;
        const bool mainDepth = c && row == g_RowDepth && (kind == kLeft || kind == kRight);
        t_DepthOnly = mainDepth && g_DepthOnly.load(std::memory_order_relaxed);
        t_DepthCopies = mainDepth && g_DepthCopies.load(std::memory_order_relaxed);
        if (t_DepthCopies) t_DepthSerial = g_DepthSerial.fetch_add(1) + 1;
        if (c && eye >= 0 && row == g_RowDepth && g_MaskHook.load(std::memory_order_relaxed)) { t_Mask = MaskT{}; t_Mask.c = c; t_Mask.view = view; t_Mask.eye = eye; t_Mask.on = true; }
        else t_Mask.on = false;
        if (learn) { t_Caps = Caps{}; t_Caps.c = c; t_Caps.on = true; }
        const int slot = c && prof ? Open(c, row, kind, view) : -1;
        if (!c) g_NoCtx++;
        const bool mainView = kind == kLeft || kind == kRight;
        // Fog: drawn at the start of the transparent pass (counted in its GPU time when profiling)
        const bool fogged = c && mainView && row == g_RowTransparent && g_FogHook.load(std::memory_order_relaxed) && Fog::BeforeTransparent(c, view);
        const bool fovRow = c && mainView && row >= 0 && row < 64 && g_FovRow[row] && g_FovHook.load(std::memory_order_relaxed);
        const bool foveated = fovRow && Foveation::Begin(c, view, kind == kRight ? 1 : 0);
        // SkipEmptyOutlines / StereoExposureReuse (PostShare): GpuTrace's draw / dispatch hooks act inside these tasks
        const bool shared = c && mainView && ((row == g_RowEmissive && PostShare::OutlinesOn()) || (row == g_RowPost && PostShare::ExposureOn()));
        const bool savedPostFx = t_PostFx;
        t_PostFx = c && mainView && row == g_RowPost && g_SneakFx.load(std::memory_order_relaxed);
        if (shared) PostShare::Enter(c, row == g_RowEmissive ? 1 : 2, eye);
        const Acc saved = t_Acc;
        t_Acc = Acc{}; t_Acc.on = prof;
        const Desc savedDesc = t_Desc;
        const Rd savedRd = t_Rd;
        t_Desc = Desc{}; t_Rd = Rd{};
        if (slot >= 0 && (kind == kLeft || kind == kNoView))
        {
            const unsigned long long df = g_DescFrame.load(std::memory_order_relaxed);
            if (df && g_Frame.load() == df)
            {
                t_Rd.on = true; t_Rd.c = c; t_Rd.row = row;
                if (kind == kLeft && DescRow(row)) { t_Desc.on = true; t_Desc.c = c; t_Desc.row = row; t_Desc.frame = df; }
            }
        }
        const bool fogProbe = prof && c && kind == kLeft && row == g_RowFogPass && FogProbe::Want();
        if (fogProbe) FogProbe::Begin();
        LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
        const uintptr_t ret = real(task);
        LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
        const Acc mine = t_Acc;
        t_Acc = saved;
        if (t_Desc.on) DescClose();
        t_Desc = savedDesc; t_Rd = savedRd;
        t_Mask = savedMask;
        if (learn) AfterLearn(c, eye, eff, ret);
        if (fovRow) Foveation::End(c, foveated);
        if (fogProbe) FogProbe::AtEnd(c, view);
        if (shared) PostShare::Leave(c);
        if (fogged) ClearEmpty(task);
        if (c && mainView && row == g_RowOpaque && g_FogHook.load(std::memory_order_relaxed)) Fog::AfterOpaque(c, view);
        if (c && mainView && row == g_RowWater && g_FogHook.load(std::memory_order_relaxed)) Fog::AfterWater(c, view);
        t_SssCtx = savedSss; t_SssView = savedSssView; t_SssEye = savedSssEye;
        t_CharTask = savedCharTask; t_CharEye = savedCharEye;
        t_DepthOnly = savedDepthOnly; t_DepthCopies = savedDepthCopies; t_DepthSerial = savedDepthSerial;
        t_PostFx = savedPostFx;
        if (c && mainView && row == g_RowScreenShadows && g_SssHook.load(std::memory_order_relaxed)) ScreenShadows::AfterMask(c, view, eye);
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
        const bool mask = g_MaskHook.load(std::memory_order_relaxed) || g_DepthOnly.load(std::memory_order_relaxed) || g_DepthCopies.load(std::memory_order_relaxed);
        const bool sss = g_SssHook.load(std::memory_order_relaxed), chars = g_CharHook.load(std::memory_order_relaxed);
        const bool share = PostShare::OutlinesOn() || PostShare::ExposureOn() || g_SneakFx.load(std::memory_order_relaxed);
        if (!task || (!prof && !fog && !fov && !eff && !mask && !sss && !chars && !share)) return real(task);
        uintptr_t* slot = (uintptr_t*)((uint8_t*)task + 0xD8);
        const uintptr_t fn = (uintptr_t)ReadPtr(slot);
        if (!fn || fn == (uintptr_t)&Thunk) return real(task);
        int pass = PassOf(fn);
        if ((prof || chars) && (pass == g_RowShWorld || pass == g_RowShDyn))
        {   // a sun cascade's task or a lamp's
            int idx = -1, slices = 0;
            if (ReadInt((uint8_t*)task + 0xF8, idx) && ReadInt((const void*)(g_Base + 0x397A2F8), slices) && idx >= 0 && idx < slices)
                pass = pass == g_RowShWorld ? g_RowShWorldSun : g_RowShDynSun;
        }
        const bool wanted = prof || (fog && (pass == g_RowOpaque || pass == g_RowTransparent || pass == g_RowWater)) || (fov && pass >= 0 && pass < 64 && g_FovRow[pass])
                         || (eff && (pass == g_RowSsao || (pass == g_RowDof && g_SkipDof.load(std::memory_order_relaxed))))
                         || (mask && pass == g_RowDepth) || (sss && pass == g_RowScreenShadows)
                         || (chars && (pass == g_RowDepth || pass == g_RowShWorldSun || pass == g_RowShDynSun))
                         || (share && (pass == g_RowEmissive || pass == g_RowPost));
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
    LARGE_INTEGER g_SecT0[PassProfile::kSections];            // its CPU start
    double g_SecCpuLast[PassProfile::kSections];              // this Present's CPU ms per section, so far
    double g_SecCpuPrev[PassProfile::kSections];              // the last whole Present's (the CPU hitch lines)
    DWORD g_RenderTid = 0;                                    // the thread the Present hook runs on (sections only there)
    double g_RowAvg[256];                                     // per row: the running GPU ms of one task (the hitch lines)
    unsigned g_HitchLines = 0, g_CpuHitchLines = 0;           // this report window's
    double g_CpuAvg = 0;                                      // running Present-to-Present CPU ms
    LARGE_INTEGER g_PresentT0{}, g_LastPresent{};

    // ---- accumulated over the report window (render thread only) ----
    struct Cell
    {
        double gpu = 0, gpuMax = 0, cpuUs = 0, cpuMaxUs = 0; unsigned long long n = 0;
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
            const bool sec = p >= kSecBase;
            for (int k = 0; k < kKinds; k++) { t += g_Cell[p][k].gpu; if (!sec || p == kSecBase) all[k] += g_Cell[p][k].gpu; }
            if (!sec && (t > 0 || g_Cell[p][0].n || g_Cell[p][1].n)) rows.push_back({ p, t });
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
            if (shown++ >= 48) break;
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
        // the mod's own work at Present, step by step, in order (GPU: the timestamps' span on the immediate context, so
        // a step that waits on the CPU -- the headset's pacing in BeginFrame -- counts the GPU's idle time behind it too)
        LOGI("PassProfile: the mod's work at Present, per Present: GPU ms avg (max) | CPU ms avg (max) | times per Present");
        for (int sct = 0; sct < PassProfile::kSections; sct++)
        {
            const Cell& c = g_Cell[kSecBase + sct][kNoView];
            if (!c.n) continue;
            LOGI("PassProfile:   {:<36} {:7.3f} ({:6.3f}) | {:6.3f} ({:6.3f}) | {:4.2f}", kSecName[sct], c.gpu / fAll, c.gpuMax,
                 c.cpuUs / 1000.0 / fAll, c.cpuMaxUs / 1000.0, c.n / fAll);
        }
        if (g_DepthCopies.load()) LOGI("PassProfile: DepthPassSkipRedundantCopies so far: {}", GpuTrace::DepthCopyStats());
        if (g_HitchLines || g_CpuHitchLines) LOGI("PassProfile: {} slow tasks and {} slow Presents in this window (the 'hitch' lines above)", g_HitchLines, g_CpuHitchLines);
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
    // ---- the draw-by-draw frame: armed, then read and written to the log a few frames later ----
    std::atomic<int> g_DescCount{ 0 };                       // draws described in the armed frame
    unsigned long long g_DescPrintAt = 0, g_DescPrinted = 0; int g_DescTries = 0; ULONGLONG g_NextDescAt = 0;
    void DescReport(unsigned long long frame)
    {
        struct Out { int row, idx; DWORD tid; double ms; D3D11_QUERY_DATA_PIPELINE_STATISTICS st; bool stats; uint64_t ps, vs; const void* psPtr; const void* cs;
                     uint32_t elems, inst, rtW, rtH, rtFmt, rtN; bool depth, indirect, dispatch;
                     const void* srcRes; const void* rtvRes; const void* dsvRes; uint32_t srcW, srcH, srcFmt, dsW, dsH, dsFmt; };
        std::vector<Out> out; int notReady = 0;
        for (int i = 0; i < kDSlots; i++)
        {
            DRec& d = g_D[i];
            const int st = d.state.load(std::memory_order_acquire);
            if (st != 2) continue;
            if (d.frame != frame) { if (d.frame + 64 < frame || d.frame > frame + 64) d.state = 0; continue; }
            UINT64 tb = 0, te = 0; D3D11_QUERY_DATA_PIPELINE_STATISTICS ps{};
            const bool in = g_Imm->GetData(d.b, &tb, sizeof tb, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && g_Imm->GetData(d.e, &te, sizeof te, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            const bool stats = in && d.ps && g_Imm->GetData(d.ps, &ps, sizeof ps, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            if (in && te >= tb && g_Freq)
                out.push_back({ d.row, d.idx, d.tid, (double)(te - tb) * 1000.0 / (double)g_Freq, ps, stats, d.psHash, d.vsHash, d.psPtr, d.cs,
                                d.elems, d.inst, d.rtW, d.rtH, d.rtFmt, d.rtN, d.depth, d.indirect, d.dispatch,
                                d.srcRes, d.rtvRes, d.dsvRes, d.srcW, d.srcH, d.srcFmt, d.dsW, d.dsH, d.dsFmt });
            else notReady++;
            d.state = 0;
        }
        LOGI("PassProfile: ===== draw by draw, left eye, frame #{}: {} draws / dispatches timed ({} not read, {} dropped); "
             "each one's GPU time runs from its start to the next one's (its state changes included) =====", frame, out.size(), notReady, g_DescDrops.exchange(0));
        std::sort(out.begin(), out.end(), [](const Out& a, const Out& b) { return a.row != b.row ? a.row < b.row : a.tid != b.tid ? a.tid < b.tid : a.idx < b.idx; });
        // DEPTHPASS: summed by its shader pair, the costliest first
        {
            struct Agg { uint64_t vs, ps; const void* psPtr; int n = 0; double ms = 0; unsigned long long elems = 0, ia = 0, cp = 0, px = 0; uint32_t rtN = 0; bool depth = false; };
            std::map<std::pair<uint64_t, uint64_t>, Agg> agg;
            double total = 0; unsigned long long px = 0, cp = 0, ia = 0; int n = 0, noPs = 0;
            for (const Out& o : out)
            {
                if (o.row != g_RowDepth) continue;
                Agg& a = agg[{ o.vs, o.ps }];
                a.vs = o.vs; a.ps = o.ps; a.psPtr = o.psPtr; a.n++; a.ms += o.ms; a.elems += (unsigned long long)o.elems * (o.inst ? o.inst : 1);
                a.rtN = o.rtN; a.depth = o.depth;
                if (o.stats) { a.ia += o.st.IAPrimitives; a.cp += o.st.CPrimitives; a.px += o.st.PSInvocations; }
                total += o.ms; n++; if (!o.ps) noPs++;
                if (o.stats) { px += o.st.PSInvocations; cp += o.st.CPrimitives; ia += o.st.IAPrimitives; }
            }
            if (n)
            {
                std::vector<Agg> v; for (auto& kv : agg) v.push_back(kv.second);
                std::sort(v.begin(), v.end(), [](const Agg& a, const Agg& b) { return a.ms > b.ms; });
                LOGI("PassProfile: DEPTHPASS: {} draws in {} shader pairs, {:.3f} ms GPU, {:.2f} M primitives in, {:.2f} M rasterised, {:.2f} M pixels shaded; "
                     "{} draws with no pixel shader. By shader pair, the costliest first: draws | GPU ms | M prims in / rasterised | M pixels | pixels per rasterised prim | targets | what the pixel shader reads",
                     n, v.size(), total, ia / 1e6, cp / 1e6, px / 1e6, noPs);
                for (size_t i = 0; i < v.size() && i < 24; i++)
                {
                    const Agg& a = v[i];
                    LOGI("PassProfile:   VS {:016x} PS {:016x} | {:4d} | {:6.3f} | {:6.3f} / {:6.3f} | {:7.2f} | {:6.1f} | {} rt{} | {}", a.vs, a.ps, a.n, a.ms, a.ia / 1e6, a.cp / 1e6,
                         a.px / 1e6, a.cp ? (double)a.px / (double)a.cp : 0.0, a.rtN, a.depth ? " + depth" : "", a.ps ? ShaderReads(a.psPtr, a.ps) : std::string("(no pixel shader)"));
                }
                // the draws that shade the most pixels, one by one (overdraw: big surfaces drawn behind others)
                std::vector<const Out*> big;
                for (const Out& o : out) if (o.row == g_RowDepth && o.stats) big.push_back(&o);
                std::sort(big.begin(), big.end(), [](const Out* a, const Out* b) { return a->st.PSInvocations > b->st.PSInvocations; });
                std::string s;
                for (size_t i = 0; i < big.size() && i < 12; i++)
                    s += fmt::format(" | #{} {:.2f} Mpx {} prims {:.3f} ms PS {:016x}", big[i]->idx, big[i]->st.PSInvocations / 1e6, big[i]->st.CPrimitives, big[i]->ms, big[i]->ps);
                LOGI("PassProfile: DEPTHPASS: the draws that shade the most pixels:{}", s);
                // its full-screen draws (copies of depth, mostly): what each reads and writes, and who reads that
                {
                    std::map<const void*, int> rtvUse, dsvUse;   // the targets of its geometry draws
                    for (const Out& o : out)
                        if (o.row == g_RowDepth && o.stats && o.st.CPrimitives > 4) { if (o.rtvRes) rtvUse[o.rtvRes]++; if (o.dsvRes) dsvUse[o.dsvRes]++; }
                    const void* sceneRt = nullptr; const void* sceneDs = nullptr; int best = 0;
                    for (auto& kv : rtvUse) if (kv.second > best) { best = kv.second; sceneRt = kv.first; }
                    best = 0;
                    for (auto& kv : dsvUse) if (kv.second > best) { best = kv.second; sceneDs = kv.first; }
                    auto name = [&](const void* r) { return !r ? std::string("-") : r == sceneDs ? std::string("SCENE DEPTH") : r == sceneRt ? std::string("SCENE RG16F")
                                                                                   : fmt::format("{}", r); };
                    LOGI("PassProfile: DEPTHPASS's geometry writes its target {} (read by: {}; drawn into by: {}) and the depth {} (read by: {}; drawn / tested into by: {})",
                         sceneRt, sceneRt ? ReadersOf(sceneRt) : std::string("-"), sceneRt ? ReadersOf(sceneRt, true) : std::string("-"),
                         sceneDs, sceneDs ? ReadersOf(sceneDs) : std::string("-"), sceneDs ? ReadersOf(sceneDs, true) : std::string("-"));
                    for (const Out& o : out)
                    {
                        if (o.row != g_RowDepth || !o.stats || o.st.CPrimitives > 4 || o.st.PSInvocations < 1000000) continue;
                        const void* dst = o.rtvRes ? o.rtvRes : o.dsvRes;
                        LOGI("PassProfile:   full-screen #{} (thread {}) PS {:016x} {:.3f} ms {:.1f} Mpx: reads {} ({}x{} {}) -> {} {} ({}x{} {}); that target is read by: {}; drawn / tested into by: {}",
                             o.idx, o.tid, o.ps, o.ms, o.st.PSInvocations / 1e6, name(o.srcRes), o.srcW, o.srcH, Fmt(o.srcFmt), o.rtvRes ? "target" : "depth", name(dst),
                             o.rtvRes ? o.rtW : o.dsW, o.rtvRes ? o.rtH : o.dsH, Fmt(o.rtvRes ? o.rtFmt : o.dsFmt), dst ? ReadersOf(dst) : std::string("-"),
                             dst ? ReadersOf(dst, true) : std::string("-"));
                    }
                }
            }
        }
        // the full-screen passes, draw by draw
        int lines = 0;
        for (const Out& o : out)
        {
            if (o.row == g_RowDepth || lines >= 120) continue;
            lines++;
            LOGI("PassProfile:   {:<14} #{:<3} {:6.3f} ms | {} {:016x} {} | {} | M pixels {:.2f} | -> {}x {}x{} {}{} | reads {}", RowName(o.row), o.idx, o.ms,
                 o.dispatch ? "CS" : "PS", o.ps, o.dispatch ? fmt::format("{} groups", o.elems) : fmt::format("VS {:016x}", o.vs),
                 o.dispatch ? (o.indirect ? "indirect" : "") : fmt::format("{} x{}", o.elems, o.inst), o.stats ? (o.dispatch ? o.st.CSInvocations : o.st.PSInvocations) / 1e6 : -1.0,
                 o.rtN, o.rtW, o.rtH, Fmt(o.rtFmt), o.depth ? " + depth" : "", ShaderReads(o.dispatch ? o.cs : o.psPtr, o.ps));
            if (o.row == g_RowFogPass)   // where the game's fog goes: which passes read what the FOG pass wrote
                LOGI("PassProfile:     FOG #{} reads {} ({}x{} {}); writes target {} (read by: {}; drawn into by: {}); depth {} ({}x{} {}; read by: {}; tested into by: {})",
                     o.idx, o.srcRes, o.srcW, o.srcH, Fmt(o.srcFmt), o.rtvRes, o.rtvRes ? ReadersOf(o.rtvRes) : std::string("-"), o.rtvRes ? ReadersOf(o.rtvRes, true) : std::string("-"),
                     o.dsvRes, o.dsW, o.dsH, Fmt(o.dsFmt), o.dsvRes ? ReadersOf(o.dsvRes) : std::string("-"), o.dsvRes ? ReadersOf(o.dsvRes, true) : std::string("-"));
        }
        LOGI("PassProfile: ===== end of the draw-by-draw frame #{} =====", frame);
        { std::lock_guard lk(g_ReadM); g_Reads.clear(); g_Targets.clear(); }
    }
    void ResetWindow()
    {
        for (auto& row : g_Cell) for (auto& c : row) c = Cell{};
        for (int k = 0; k < kKinds; k++) { g_FramesKind[k] = 0; g_Views[k].clear(); }
        g_FrameGpuSum = g_FrameGpuMax = g_FrameCpuSum = g_FrameCpuMax = g_HookCpuSum = 0; g_FrameGpuN = g_FrameCpuN = g_Presents = 0;
        g_HitchLines = g_CpuHitchLines = 0;
        for (int p = 0; p < kRows; p++) for (int k = 0; k < kKinds; k++) g_WantRt[p][k] = true;
    }
    //! A task far slower than its pass usually is (GPU): what it did, to the log (at most 12 per report window). The
    //! uploads say whether it waited on data (a Map that reads stalls the CPU; many discards / updates are streaming);
    //! a slow task with ordinary draws and pixels is the GPU being taken away (the compositor) or a shader being built.
    void Hitch(const Rec& r, UINT64 tb, UINT64 te, const D3D11_QUERY_DATA_PIPELINE_STATISTICS* ps)
    {
        const double ms = (double)(te - tb) * 1000.0 / (double)g_Freq;
        if (r.row < 0 || r.row >= kRows || r.row >= 256) return;
        double& avg = g_RowAvg[r.row];
        const double before = avg;
        avg = avg <= 0 ? ms : avg + (ms - avg) * 0.02;
        if (r.row >= kSecBase || before <= 0 || ms < 2.0 || ms < before * 4.0 || g_HitchLines >= 12) return;
        // not a task's own time: one that draws and dispatches nothing (IGGY_3D: the GPU time between its two
        // timestamps is other command lists' work -- its "pixels" are theirs), or the draw-by-draw frame (its queries)
        if (!r.acc.draws && !r.acc.dispatches) return;
        if (r.frame == g_DescPrinted || r.frame == g_DescFrame.load()) return;
        g_HitchLines++;
        const Acc& a = r.acc;
        LOGI("PassProfile: hitch: frame #{} {} {} took {:.2f} ms GPU (usually {:.3f}), recorded in {:.2f} ms CPU on thread {} | draws {} dispatches {} "
             "M elements {:.2f} | maps: discard {} no-overwrite {} read {} other {}, updates {}, copies {}{}",
             r.frame, RowName(r.row), kKindName[r.kind], ms, before, r.cpuUs / 1000.0, r.tid, a.draws, a.dispatches, a.elems / 1e6,
             a.mapDiscard, a.mapNoOver, a.mapRead, a.mapOther, a.updates, a.copies,
             ps ? fmt::format(" | M prims in {:.3f} rasterised {:.3f} pixels {:.3f} compute {:.3f}", ps->IAPrimitives / 1e6, ps->CPrimitives / 1e6,
                              ps->PSInvocations / 1e6, ps->CSInvocations / 1e6) : std::string(" | no statistics"));
    }
    //! A Present far later than usual (CPU): how long the mod's own steps took in the last one (at most 8 per window)
    void CpuHitch(unsigned long long f, double ms)
    {
        const double before = g_CpuAvg;
        g_CpuAvg = g_CpuAvg <= 0 ? ms : g_CpuAvg + (ms - g_CpuAvg) * 0.02;
        if (before <= 0 || ms < 20.0 || ms < before * 1.8 || g_CpuHitchLines >= 8) return;
        g_CpuHitchLines++;
        std::string s;
        for (int i = 0; i < PassProfile::kSections; i++) if (g_SecCpuPrev[i] >= 0.5) s += fmt::format(" {} {:.2f}", kSecName[i], g_SecCpuPrev[i]);
        LOGI("PassProfile: hitch: Present #{} came {:.1f} ms after the last one (usually {:.1f}); the mod's steps over 0.5 ms CPU in the last Present:{}",
             f, ms, before, s.empty() ? " none" : s);
    }
    void Take(Rec& r, UINT64 tb, UINT64 te)
    {
        const double ms = (double)(te - tb) * 1000.0 / (double)g_Freq;
        if (ms >= 100.0) return;
        Cell& c = g_Cell[r.row][r.kind];
        c.gpu += ms; c.gpuMax = std::max(c.gpuMax, ms); c.n++; c.cpuUs += r.cpuUs; c.cpuMaxUs = std::max(c.cpuMaxUs, r.cpuUs);
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
        if (!strcmp(kPasses[i].name, "RENDER_WATER")) g_RowWater = i;
        if (!strcmp(kPasses[i].name, "SSAO")) g_RowSsao = i;
        if (!strcmp(kPasses[i].name, "DOF")) g_RowDof = i;
        if (!strcmp(kPasses[i].name, "DEPTHPASS")) g_RowDepth = i;
        if (!strcmp(kPasses[i].name, "POSTPROC")) g_RowPost = i;
        if (!strcmp(kPasses[i].name, "EMISSIVE_ONLY")) g_RowEmissive = i;
        if (!strcmp(kPasses[i].name, "POST_TONEMAP")) g_RowTonemap = i;
        if (!strcmp(kPasses[i].name, "CLEAR")) g_RowClear = i;
        if (!strcmp(kPasses[i].name, "FOG")) g_RowFogPass = i;
        for (const char* n : { "IGGY_3D", "IGGY_FULLSCREEN", "DARKVISION", "DISTORTION", "EMISSIVE", "POST_IGGY" })
            if (!strcmp(kPasses[i].name, n) && i < 64) g_DescExtra[i] = true;
        if (!strcmp(kPasses[i].name, "SCREENSHADOWS")) g_RowScreenShadows = i;
        if (!strcmp(kPasses[i].name, "SHADOWS_WORLD lamps")) g_RowShWorld = i;
        if (!strcmp(kPasses[i].name, "SHADOWS_DYNAMIC lamps")) g_RowShDyn = i;
        if (!strcmp(kPasses[i].name, "SHADOWS_WORLD sun")) g_RowShWorldSun = i;
        if (!strcmp(kPasses[i].name, "SHADOWS_DYNAMIC sun")) g_RowShDynSun = i;
        // Foveation: what shades the scene at the eye's resolution
        static const char* fov[] = { "DEPTHPASS", "DEPTHALPHA_LIT", "FORWARD_WORLD", "FORWARD_DYNAMIC", "RENDER_WATER", "ALPHA_LIT", "EMISSIVE_ONLY" };
        for (const char* n : fov) if (!strcmp(kPasses[i].name, n) && i < 64) g_FovRow[i] = true;
    }
    g_FogHook = Fog::Wanted();
    g_FovHook = Foveation::Wanted();
    g_MaskHook = FovMask::Wanted();
    g_SssHook = ScreenShadows::Wanted();
    g_CharHook = ScreenShadows::CharactersWanted();
    if ((!g_On.load() && !g_FogHook.load() && !g_FovHook.load() && !g_SkipEffects.load() && !g_MaskHook.load() && !g_SssHook.load() && !g_CharHook.load() && !g_DepthOnly.load() && !g_DepthCopies.load()
         && !PostShare::OutlinesOn() && !PostShare::ExposureOn() && !g_SneakFx.load()) || g_Exec) return;
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
    if (e != NO_ERROR) { g_On = false; g_FogHook = false; g_FovHook = false; g_SkipEffects = false; g_MaskHook = false; g_SssHook = false; g_CharHook = false; }
}

void PassProfile::SetSkipEffects(int mode)
{
    g_SkipEffects = mode >= 1; g_SkipDof = mode >= 2;
    if (mode >= 1) LOGI("PassProfile: SkipDisabledEffects = {} (experimental): {} leftover work is skipped while {} off", mode,
                        mode >= 2 ? "the SSAO and DOF passes'" : "the SSAO pass's", mode >= 2 ? "those effects are" : "it is");
}

bool PassProfile::On() { return g_On.load(std::memory_order_relaxed); }

int PassProfile::CharTask() { return t_CharTask; }
bool PassProfile::DepthOnlyNow() { return t_DepthOnly; }
bool PassProfile::DepthCopiesNow() { return t_DepthCopies; }
unsigned long long PassProfile::DepthTaskSerial() { return t_DepthSerial; }
void PassProfile::SetDepthCopies(bool on)
{
    g_DepthCopies = on;
    if (on) LOGI("PassProfile: DepthPassSkipRedundantCopies: in the eyes' depth pass, a depth copy whose target already holds what it would copy (the same copy "
                 "earlier in the task, nothing drawn, cleared or copied into either since) is skipped, and with the depth of field and the game's fog off the depth downsample only the autofocus then reads");
}
void PassProfile::SetSkipSneakOverlay(bool on)
{
    g_SneakFx = on;
    if (on) LOGI("PassProfile: SkipSneakOverlay: the sneak mode's screen overlay (PS 2ee2b9822f61d7d1) is not drawn over the eyes' pictures");
}
bool PassProfile::SneakOverlayNow() { return t_PostFx; }

void PassProfile::SetDepthOnly(bool on)
{
    g_DepthOnly = on;
    if (on) LOGI("PassProfile: DepthPassDepthOnly (experimental): the eyes' depth pass draws its main shader (48f2037fd43d86c8) depth-only: no pixel shader, no RG16F target (nothing in the frame reads it: the draw-by-draw log)");
}
int PassProfile::CharEye() { return t_CharEye; }

void PassProfile::NoteDraw(ID3D11DeviceContext* c, unsigned elements, unsigned instances, bool indirect)
{
    if (t_Mask.on)
    {   // FovMask: before the depth pass's first draw into the scene's depth (its own draw comes back through here: off first)
        t_Mask.on = false;
        if (!FovMask::AtDraw(t_Mask.c, t_Mask.view, t_Mask.eye) && ++t_Mask.tries < 64) t_Mask.on = true;
    }
    if (t_Caps.on) CaptureTargets();
    if (t_SssCtx) ScreenShadows::AtTaskDraw(t_SssCtx, t_SssView, t_SssEye);
    if (t_Desc.on) DescDraw(c, elements, instances, indirect, false);   // (after the mod's own draws above)
    if (FogProbe::t_T.on) FogProbe::AtDraw(c);
    if (t_Rd.on) NoteReads(c, false);
    if (!t_Acc.on) return;
    t_Acc.draws++; if (indirect) t_Acc.indirect++;
    t_Acc.elems += (uint64_t)elements * (instances ? instances : 1);
}
void PassProfile::NoteMap(int type)
{
    if (!t_Acc.on) return;
    if (type == 4) t_Acc.mapDiscard++; else if (type == 5) t_Acc.mapNoOver++; else if (type == 1 || type == 3) t_Acc.mapRead++; else t_Acc.mapOther++;
}
void PassProfile::NoteUpdate() { if (t_Acc.on) t_Acc.updates++; }
void PassProfile::NoteCopy() { if (t_Acc.on) t_Acc.copies++; }
void PassProfile::NoteDispatch(ID3D11DeviceContext* c, unsigned x, unsigned y, unsigned z, bool indirect)
{
    if (t_Desc.on) DescDraw(c, x * y * z, 0, indirect, true);
    if (t_Rd.on) NoteReads(c, true);
    if (!t_Acc.on) return;
    t_Acc.dispatches++; if (indirect) t_Acc.indirect++;
    t_Acc.groups += (uint64_t)x * y * z;
}

bool PassProfile::BeginSection(int s)
{   // the mod's own GPU work at Present, on the immediate context (render thread)
    if (!g_On.load() || !g_Imm || s < 0 || s >= kSections || g_SecOpen[s] >= 0) return false;
    if (s == 0) g_RenderTid = GetCurrentThreadId();
    else if (GetCurrentThreadId() != g_RenderTid) return false;   // (a step reached from another thread: not timed)
    g_SecOpen[s] = Open(g_Imm, kSecBase + s, kNoView, nullptr);
    if (g_SecOpen[s] < 0) { g_SecOpen[s] = -1; return false; }
    if (s == 0) for (int i = 0; i < kSections; i++) g_SecCpuLast[i] = 0;
    QueryPerformanceCounter(&g_SecT0[s]);
    if (s == 0) g_PresentT0 = g_SecT0[s];
    return true;
}
void PassProfile::EndSection(int s)
{
    if (!g_On.load() || !g_Imm || s < 0 || s >= kSections || g_SecOpen[s] < 0) return;
    if (GetCurrentThreadId() != g_RenderTid) return;
    Rec& r = g_Rec[g_SecOpen[s]];
    {
        LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
        r.cpuUs = (double)(t1.QuadPart - g_SecT0[s].QuadPart) * 1e6 / (double)g_QpcF.QuadPart;
        g_SecCpuLast[s] += r.cpuUs / 1000.0;
        if (s == 0) { g_HookCpuSum += r.cpuUs / 1000.0; for (int i = 0; i < kSections; i++) g_SecCpuPrev[i] = g_SecCpuLast[i]; }
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
    FogProbe::OnPresent(imm);
    {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (g_LastPresent.QuadPart)
        {
            const double ms = (double)(now.QuadPart - g_LastPresent.QuadPart) * 1000.0 / (double)g_QpcF.QuadPart;
            if (ms < 500) { g_FrameCpuSum += ms; g_FrameCpuN++; g_FrameCpuMax = std::max(g_FrameCpuMax, ms); g_CpuMs[f % kFrames] = ms; g_CpuFrame[f % kFrames] = f; }
            CpuHitch(f, ms);
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
                    Hitch(r, tb, te, stats ? &ps : nullptr);
                }
                r.state = 0;
            }
            else if (r.frame + 40 < f) { g_Lost++; r.state = 0; }   // never executed (a command list the engine dropped)
        }
    }
    if (g_DumpFrame && f >= g_DumpUntil) { DumpFrame(); g_DumpFrame = 0; g_NextDumpAt = nowMs + 30000; }
    // draw by draw: 40 s in, then every 2 minutes, and 20 s into every map; the armed frame moves on until one with
    // left-eye tasks is met
    {
        if (!g_NextDescAt) g_NextDescAt = nowMs + 40000;
        {
            static ULONGLONG checkAt = 0; static std::string map;
            if (nowMs >= checkAt)
            {
                checkAt = nowMs + 1000;
                std::string m = VrTweaks::MapName();
                if (m != map) { map = m; if (!m.empty() && g_NextDescAt > nowMs + 20000) g_NextDescAt = nowMs + 20000; }
            }
        }
        const unsigned long long df = g_DescFrame.load();
        if (!df && !g_DescPrintAt && nowMs >= g_NextDescAt) { g_DescCount = 0; g_DescTries = 0; g_DescFrame = f + 1; }
        else if (df && f > df)
        {
            bool seen = false;
            for (int i = 0; i < kDSlots && !seen; i++) if (g_D[i].state.load(std::memory_order_relaxed) == 2 && g_D[i].frame == df) seen = true;
            // the next try one frame later every other time: the game presents the left and the right eye's frames in
            // turn, and arming f + 1 each time checked only every other frame -- always the same eye's (2026-11-01: four
            // dumps in a row with 0 draws)
            if (!seen && ++g_DescTries < 40) g_DescFrame = f + 1 + (g_DescTries & 1);
            else { g_DescFrame = 0; g_DescPrinted = df; g_DescPrintAt = f + 10; }
        }
        if (g_DescPrintAt && f >= g_DescPrintAt) { DescReport(g_DescPrinted); g_DescPrintAt = 0; g_NextDescAt = nowMs + 120000; }
    }

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
