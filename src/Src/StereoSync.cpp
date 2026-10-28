#include "pch.h"
#include "StereoSync.h"
#include "GameInfo.h"
#include "Game.h"
#include "CVars.h"
#include "KbmInject.h"
#include "ScreenMode.h"
#include "ViewMatrixHook.h"
#include "CameraHook.h"
#include <detours/detours.h>

namespace
{
    // idGameLocal::RunFrame(this, frameInput, a3) -> bool (vtable idGameLocal+0x18, exe+0x1C940A8). It branches on
    // g_stopTime / the pause byte this+0x340eb6 into the normal frame (exe+0x8AD260) or the frozen one (exe+0x8AD4E0).
    constexpr uint32_t kRunFrame = 0x8ACDF0;
    constexpr uint32_t kClocks = 0x2e7428;          // gameLocal + : clock entries of 0x50 bytes (+0x18 i64, +0x20 f, +0x28 i)
    using RunFrameFn = bool (*)(void*, void*, void*);
    // The game's Draw(this, out): the clocks, the current camera's render view -> out (+0x48 -> renderView). RunFrame
    // points this+0x340f08 at its third argument (or at this+0x340f10 when that is null) and resets it on the way out.
    constexpr uint32_t kDraw = 0x8AC710, kOutPtr = 0x340f08, kOutDefault = 0x340f10;
    using DrawFn = void (*)(void*, void*);
    RunFrameFn g_RunFrame = nullptr;
    std::atomic<bool> g_Installed{ false };

    // The engine's input devices read DirectInput once per frame (GetDeviceData, up to 256 events) into the device's own
    // event list, which the next read clears: arkDirectInputKeyboard (vtable exe+0x1AE0908, +0x28 = exe+0xE1970) and
    // idMouseWin32 (exe+0x1AE0AF8, +0x40 = exe+0xE2930). Only a simulated frame consumes that list, so whatever a held
    // frame read was gone: measured, a jump (A) or a walk (W) was lost about half the time, by whether its key-down fell
    // on a held frame, and the head-aim servo's mouse motion likewise. Before a held frame the devices are not read: the
    // events wait in DirectInput's own buffer for the next read, the simulated frame's.
    constexpr uint32_t kKbdPoll = 0xE1970, kMousePoll = 0xE2930;
    using PollFn = uintptr_t (*)(void*);
    PollFn g_KbdPoll = nullptr, g_MousePoll = nullptr;
    std::atomic<unsigned long long> g_Polls{ 0 }, g_PollsHeld{ 0 };
    std::atomic<bool> g_PollsHooked{ false };
    std::atomic<unsigned long long> g_Calls{ 0 };
    std::atomic<bool> g_SyncOn{ false }, g_PendingRight{ false };
    std::atomic<int> g_Dbg{ 0 };   // vr syncdbg <mask> (A/B and tests): 64 input read on held frames (as before the read hooks),
                                   // 256 no left image staged (VRRuntime: tests the stereo health check), 512 the gate ignored (holds in menus too, as before)

    // ---- When a frame may be held (the gate) ----
    // A held frame runs the rest of the engine's frame (the UI's tick and its input dispatch among it) without the game
    // simulation that owns what that UI shows. Holding is only for PLAY: never while paused or stopped, in any menu or on
    // the menu screen, while the player is changing, on the frame after a hitch, and not for kCooldown Presents after any
    // of those changed (a UI transition: a pause menu opening on focus loss was where the idle crash of 2026-10-23 sat).
    // The left view records when it was built; the next RunFrame is held only if it is THE next one (one Present later,
    // the same player): a left whose right never came used to keep `pending` set for ever and hold some later frame.
    constexpr uint32_t kPauseByte = 0x340eb6;      // gameLocal +: the game's pause flag (RunFrame branches on it)
    constexpr int kCooldown = 30;                  // Presents after a transition before holding again
    constexpr double kSpikeSec = 0.1;              // a frame this long: the next one is not held
    struct LeftMark { unsigned long long calls = 0, presents = 0; void* player = nullptr; } g_Mark;   // main thread
    std::atomic<unsigned long long> g_Presents{ 0 };
    std::atomic<bool> g_GateOpen{ false };         // decided at each Present (main thread)
    std::atomic<int> g_GateWhy{ 0 };
    std::atomic<void*> g_LastGl{ nullptr };
    bool g_PolledRead = false;                     // a device was read (not waited) since the last RunFrame: do not hold
    unsigned long long g_CooldownUntil = 0, g_LastSig = ~0ull;
    double g_LastPresentT = 0;
    enum { wNone, wPaused, wStopTime, wMenu, wScreen, wNoPlayer, wSpike, wCooldown, wNotSim, wCount };
    const char* kGateNames[wCount] = { "open", "paused", "stoptime", "menu", "screen", "noplayer", "spike", "cooldown", "notsim" };
    std::atomic<unsigned long long> g_GateShut[wCount];
    enum { xLate, xPresents, xPlayer, xPolled, xGate, xFlat, xCount };
    const char* kExpNames[xCount] = { "late", "presents", "player", "inputread", "gate", "flat" };
    std::atomic<unsigned long long> g_Expired[xCount];
    std::atomic<int> g_LoggedExp{ 0 };
    //! Is the RunFrame about to run (the `calls`-th) to be held for the pending right view? `why` = the expiry reason
    //! when not (-1 = nothing pending).
    bool PairHold(unsigned long long calls, int* why)
    {
        if (!(g_SyncOn.load() && g_PendingRight.load())) { if (why) *why = -1; return false; }
        int w = -1;
        if (calls != g_Mark.calls) w = xLate;
        else if (g_Presents.load() != g_Mark.presents + 1) w = xPresents;
        else if (!g_GateOpen.load() && !(g_Dbg.load() & 512)) w = xGate;   // (vr syncdbg 512: the gate ignored, as before it)
        else if (g_PolledRead) w = xPolled;
        else if (!g_Mark.player || Game::LocalPlayer() != g_Mark.player) w = xPlayer;
        if (why) *why = w;
        return w < 0;
    }
    //! The next RunFrame will be held (the mode's left view was just built).
    bool NextHeld() { return PairHold(g_Calls.load(), nullptr); }
    bool InputWaits() { return !(g_Dbg.load() & 64) && NextHeld(); }
    uintptr_t HkKbdPoll(void* d) { g_Polls++; if (InputWaits()) { g_PollsHeld++; return 0; } g_PolledRead = true; return g_KbdPoll(d); }
    uintptr_t HkMousePoll(void* d) { g_Polls++; if (InputWaits()) { g_PollsHeld++; return 0; } g_PolledRead = true; return g_MousePoll(d); }

    std::atomic<unsigned long long> g_Ticks{ 0 };
    std::atomic<unsigned long long> g_Lefts{ 0 }, g_Rights{ 0 }, g_SyncHeld{ 0 };
    std::atomic<bool> g_LastHeld{ false };

    // ---- the game's own frame cap ----
    // The game holds itself to 30 fps x com_timeScaler -- 120 at the options' highest setting (timeScaler 4) -- by
    // spinning at the end of each frame until that much time has passed (exe+0x462E0, called from exe+0x35924A unless
    // com_disableFrameSpinning is set; the timer is in microseconds: 33,333 / timeScaler). Synchronised stereo renders
    // TWO game frames per headset frame, so under that cap it never passes 60 pairs a second, below every headset's
    // refresh (72-120 Hz): whatever moves fast across the view -- close objects while strafing -- judders. While
    // synchronised stereo runs the spin is off: the VR compositor paces the game to one pair per display period (two
    // frames per refresh, whatever the headset's), and the simulation ticks once per pair -- at most the refresh, never
    // past the 120 Hz the game's 1/120 s physics substeps are built for. The player's value comes back when it ends.
    // The renderer's check (exe+0x359223) reads the cvar's integer value (+0x28), so that is what is written, and kept
    // written: the console command alone was measured not to stick (frames 8.334 ms apart with it sent).
    bool g_SpinForced = false; int g_SpinSaved = 0;   // main thread (OnPresent)
    void ApplyFrameCap()
    {
        const bool want = g_SyncOn.load();
        const bool have = CVars::Address("com_disableFrameSpinning") != 0;
        if (want && !g_SpinForced)
        {
            g_SpinSaved = have ? CVars::GetInt("com_disableFrameSpinning") : 0;
            if (have) CVars::SetInt("com_disableFrameSpinning", 1);
            g_SpinForced = true;
            const float ts = CVars::GetFloat("com_timeScaler", 0.f);
            LOGI("StereoSync: the game's frame cap ({:.0f} fps) {} while synchronised stereo runs", 30.f * ts, have ? "is off" : "could NOT be switched off (cvar not found)");
        }
        else if (want && have && CVars::GetInt("com_disableFrameSpinning") != 1) CVars::SetInt("com_disableFrameSpinning", 1);
        else if (!want && g_SpinForced)
        {
            if (have) CVars::SetInt("com_disableFrameSpinning", g_SpinSaved);
            g_SpinForced = false;
            LOGI("StereoSync: the game's frame cap is back");
        }
    }

    // ---- the light probes' update budget ----
    // The indirect lighting updates at most r_ILProbeUpdate_MaxUpdatesPerFrame probes (8) on every RENDERED frame
    // (the pass ARK_RENDERPASS_INDIRECT_LIGHTING_UPDATE_PROBES; the cap is read at exe+0x2B32A1). Synchronised stereo
    // renders two frames per tick, the second of the same world: half the budget a frame keeps the probes' rate per
    // second what it is in flat play, and the right eye's frame does not do the left's work again.
    bool g_ProbeForced = false; int g_ProbeSaved = 0;   // main thread (OnPresent)
    void ApplyProbeBudget()
    {
        const bool want = g_SyncOn.load();
        const char* kCvar = "r_ILProbeUpdate_MaxUpdatesPerFrame";
        if (want && !g_ProbeForced)
        {
            if (!CVars::Address(kCvar)) return;
            g_ProbeSaved = CVars::GetInt(kCvar);
            g_ProbeForced = true;
            LOGI("StereoSync: light probe updates {} -> {} a frame while synchronised stereo runs (two frames per tick)", g_ProbeSaved, std::max(1, g_ProbeSaved / 2));
        }
        if (want && g_ProbeForced)
        {
            const int v = std::max(1, g_ProbeSaved / 2);
            if (CVars::GetInt(kCvar) != v) CVars::SetInt(kCvar, v);
        }
        else if (!want && g_ProbeForced)
        {
            if (CVars::GetInt(kCvar) == std::max(1, g_ProbeSaved / 2)) CVars::SetInt(kCvar, g_ProbeSaved);
            g_ProbeForced = false;
        }
    }

    // ---- the right eye's repeated work (experimental: [Advanced] Stereo*Reuse) ----
    // The right eye's frame draws the world of the SAME tick again. The engine's renderer is a list of named passes run
    // as tasks (ENGINE.md, render passes); some of them do not depend on the view and so do, for the right eye, exactly
    // what the left eye's frame just did: the ocean's simulation, the reflection probes' relighting, the decals on NPCs,
    // the texture streamer's uploads. Each can be skipped for a task of the right eye's view (task+0x70
    // = the view object, one of the main view's; its m_viewMatrix at +0x7450 names the view) the way the engine skips a
    // disabled task (the executor, exe+0x2C5D6C, for task+0xF4 & 0x100) -- and the way the ocean passes skip themselves
    // when there is no ocean: the task's context marked empty (+0x12AD4), nothing recorded. Each one only if the ini
    // asks for it; experimental: whether what the left frame left behind is still intact when the right eye's frame
    // reads it has not been seen in a headset yet. Hooks only for what is asked.
    //
    // NOT the shadow maps (tried as StereoShadowReuse, exe+0x32D4B0: shadows broke in BOTH eyes). The engine updates
    // the sun's farther cascades on alternate frames (frame parity, exe+0x32EF1D) and keeps the rest in caches; in
    // synchronised stereo alternate frames are alternate eyes, so the right eye's frames are the ones that update some
    // cascades -- skipped, those were never updated for either eye. Any pass the engine schedules by frame parity has the
    // same trap: a pass skipped here must be one that is simply done again every frame.
    enum Reuse { rOcean, rReflections, rDecals, rStreaming, rGroups };
    const char* kReuseKey[rGroups] = { "StereoOceanReuse", "StereoReflectionReuse", "StereoDecalReuse", "StereoStreamOncePerPair" };
    std::atomic<bool> g_ReuseOn[rGroups];
    struct ReusePass
    {
        const char* name; uint32_t rva; int group;
        void* real = nullptr; std::atomic<unsigned long long> run{ 0 }, skipped{ 0 };
    };
    ReusePass g_Pass[] = {
        { "UPDATE_OCEAN_FFT", 0x3053B0, rOcean },
        { "UPDATE_OCEAN", 0x305370, rOcean },
        { "INDIRECT_LIGHTING_UPDATE_IBL", 0x3102A0, rReflections },
        { "UPDATE_NPC_DECALS", 0x340F10, rDecals },
        { "TEXTURE_STREAMER", 0x34FD00, rStreaming },
    };
    constexpr int kPasses = (int)(sizeof g_Pass / sizeof g_Pass[0]);
    using Task1Fn = bool(*)(void* task);
    //! The view a render task belongs to is the RIGHT eye of a synchronised pair.
    bool RightEyeTask(void* task)
    {
        const uint8_t* view = *(const uint8_t* const*)((const uint8_t*)task + 0x70);
        if (!CameraHook::IsMainViewObject(view)) return false;   // another view (a reflection, a camera screen), or not one
        float m[16]; memcpy(m, view + 0x7450, 64);
        const unsigned long long s = ViewMatrixHook::SerialOfViewMatrix(m);
        if (!s) return false;
        const ViewMatrixHook::XrPose p = ViewMatrixHook::RenderPoseXr(s);
        return p.valid && p.eye == 1 && p.pair && p.pair != s;
    }
    //! Skip this task? Then it is marked as the engine marks a task it skips.
    bool SkipTask(ReusePass& r, void* task)
    {
        if (!task || !g_ReuseOn[r.group].load(std::memory_order_relaxed) || !g_SyncOn.load(std::memory_order_relaxed) || !RightEyeTask(task))
        { r.run++; return false; }
        if (uint8_t* ctx = *(uint8_t**)((uint8_t*)task + 0x108)) ctx[0x12AD4] = 1;
        r.skipped++;
        return true;
    }
    template <int I> bool HkTask1(void* task)
    {
        ReusePass& r = g_Pass[I];
        if (SkipTask(r, task)) return false;
        return ((Task1Fn)r.real)(task);
    }
    void* HookOf(int i)
    {
        switch (i)
        {
        case 0: return (void*)&HkTask1<0>;
        case 1: return (void*)&HkTask1<1>;
        case 2: return (void*)&HkTask1<2>;
        case 3: return (void*)&HkTask1<3>;
        case 4: return (void*)&HkTask1<4>;
        }
        return nullptr;
    }
    static_assert(kPasses == 5, "HookOf lists every pass");

    struct Rec { double t; int held; long long c18[3]; float c20[3]; int c28[3]; float org[3]; int stop; uint32_t in[16]; };
    std::mutex g_LogM;
    std::vector<Rec> g_Log; size_t g_LogWant = 0;

    double Now()
    {
        static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
        LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart;
    }
    void ReadInput(const void* in, Rec& r)
    {
        __try { memcpy(r.in, in, sizeof r.in); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    void ReadClocks(void* gl, Rec& r)
    {
        __try
        {
            const uint8_t* b = (const uint8_t*)gl + kClocks;
            for (int i = 0; i < 3; i++)
            {
                r.c18[i] = *(const long long*)(b + i * 0x50 + 0x18);
                r.c20[i] = *(const float*)(b + i * 0x50 + 0x20);
                r.c28[i] = *(const int*)(b + i * 0x50 + 0x28);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ---- vr uistack: the game's UI stack (Iggy), logged when its shape changes ----
    // The idle stereo-2 crash of 2026-10-23 was in Iggy (the game's UI) resolving an AS3 path list of a UI movie that
    // pointed at freed heap, with a pause menu opening on a held frame (the gate stops that). The UI stack (disassembly
    // of the UI tick exe+0x35120, reached by a tail jump at exe+0x45790 with rcx = *(exe+0x2274270)): it passes rcx+0x18
    // -- an object EMBEDDED in the manager, not a pointer -- to exe+0x33740, which reads the layers at +8 (stride 0x20,
    // count +0x10 & 0xFFFFFF) and updates only the TOP one, (count-1)*0x20; a layer's movies at +0x10 (stride 0xDE8,
    // count +0x18 & 0xFFFFFF), a movie's first qword = its Iggy player. Measured: no layer in play and while loading, one
    // layer of one movie in the pause menu, journal, main menu, load list and black market.
    constexpr uint32_t kUiMgr = 0x2274270;
    // vr uistack 1: the stack's shape (movies per layer, bottom first), logged when it changes
    std::atomic<bool> g_UiStack{ false };
    std::mutex g_UiM; std::string g_UiShape = "?"; unsigned long long g_UiChanges = 0;   // under g_UiM
    bool Committed(const void* p, size_t n)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    //! The UI stack's shape: "<layers>:<movies of each layer, bottom first>", "-" = no manager yet, "?" = unreadable.
    int UiShapeRaw(int* per, int maxPer)
    {
        const uintptr_t base = GameInfo::Base();
        if (!Committed((void*)(base + kUiMgr), 8)) return -1;
        const uint8_t* mgr = *(uint8_t* const*)(base + kUiMgr);
        if (!mgr || !Committed(mgr + 0x18 + 8, 16)) return -1;
        const uint8_t* layers = *(uint8_t* const*)(mgr + 0x20); const int nl = *(const int*)(mgr + 0x28) & 0xFFFFFF;
        if (nl < 0 || nl > 64) return -2;
        if (nl && !Committed(layers, (size_t)nl * 0x20)) return -2;
        for (int l = 0; l < nl && l < maxPer; l++) per[l] = *(const int*)(layers + l * 0x20 + 0x18) & 0xFFFFFF;
        return nl;
    }
    int UiShapeSafe(int* per, int maxPer)
    {
        __try { return UiShapeRaw(per, maxPer); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
    }
    void UiStackTick()
    {
        if (!g_UiStack.load()) return;
        int per[16] = {}; const int nl = UiShapeSafe(per, 16);
        std::string s = nl == -1 ? "-" : nl < 0 ? "?" : std::to_string(nl) + ":";
        for (int l = 0; l < nl && l < 16; l++) s += (l ? "," : "") + std::to_string(per[l]);
        std::lock_guard lk(g_UiM);
        if (s == g_UiShape) return;
        const KbmInject::Stats ks = KbmInject::GetStats();
        LOGI("StereoSync: uistack {} -> {} (input mode {} sim {} scene {} screen {})", g_UiShape, s, ks.mode == 0 ? "play" : ks.mode == 1 ? "menu" : "wheel",
             ks.simulating ? 1 : 0, ks.scene, ScreenMode::FlatRender() ? 1 : 0);
        g_UiShape = s; g_UiChanges++;
    }

    int ReadPaused(void* gl)
    {
        // Checked first, not just caught: right after the intro videos RunFrame runs on an object that is not yet the
        // game's (it ends before +0x340EB6), and the read faulted on every Present until the world existed -- six
        // first-chance access violations at each boot that used up CrashLog's backtraces (measured 2026-10-23).
        const uint8_t* p = (const uint8_t*)gl + kPauseByte;
        if (!Committed(p, 1)) return 1;
        __try { return *p != 0 ? 1 : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
    }

    bool HkRunFrame(void* gl, void* in, void* a3)
    {
        const unsigned long long n = g_Calls++;
        // hold exactly the frame after a left view, and only while there is a game world to draw
        bool hold = false;
        if (g_SyncOn.load() && g_PendingRight.load())
        {
            int why = -1;
            hold = PairHold(n, &why) && gl;
            g_PendingRight = false;
            if (!hold && why >= 0)
            {
                g_Expired[why]++;
                if (g_LoggedExp.fetch_add(1) < 12)
                    LOGI("StereoSync: the right view of a pair was not held ({}): this frame runs, the left image goes alone", kExpNames[why]);
            }
        }
        if (gl) g_LastGl = gl;
        g_LastHeld = hold;
        bool r = true;
        if (hold)
        {   // no simulation, only the game's Draw (exe+0x8AC710): it publishes the camera's render view (and the clocks,
            // unchanged) into the frame's output struct, as RunFrame's frozen and normal paths both end by doing.
            uint8_t* g = (uint8_t*)gl;
            void* out = a3 ? a3 : g + kOutDefault;
            *(void**)(g + kOutPtr) = out;
            ((DrawFn)(GameInfo::Base() + kDraw))(gl, out);
            *(void**)(g + kOutPtr) = g + kOutDefault;
            g_SyncHeld++;
        }
        else { r = g_RunFrame(gl, in, a3); g_Ticks++; }
        g_PolledRead = false;     // (after the frame: device reads made inside it belong to it)
        {
            std::lock_guard lk(g_LogM);
            if (g_Log.size() < g_LogWant)
            {
                Rec rec{}; rec.t = Now(); rec.held = hold ? 1 : 0;
                ReadClocks(gl, rec);
                if (in) ReadInput(in, rec);
                float ax[9]; Game::PlayerView(rec.org, ax);
                g_Log.push_back(rec);
            }
        }
        return r;
    }

    void WriteLog()
    {
        std::vector<Rec> v;
        { std::lock_guard lk(g_LogM); v.swap(g_Log); g_LogWant = 0; }
        FILE* f = _wfopen((GameInfo::GameDir() + L"\\DVR_simlog.txt").c_str(), L"w");
        if (!f) return;
        fprintf(f, "# t_ms held stop c18_0 c20_0 c28_0 c18_1 c20_1 c28_1 c18_2 c20_2 c28_2 x y z\n");
        const double t0 = v.empty() ? 0 : v[0].t;
        for (auto& r : v)
        {
            fprintf(f, "%.3f %d %d %lld %.4f %d %lld %.4f %d %lld %.4f %d %.4f %.4f %.4f", (r.t - t0) * 1000.0, r.held, r.stop,
                    r.c18[0], r.c20[0], r.c28[0], r.c18[1], r.c20[1], r.c28[1], r.c18[2], r.c20[2], r.c28[2], r.org[0], r.org[1], r.org[2]);
            for (int i = 0; i < 16; i++) fprintf(f, " %08X", r.in[i]);
            fprintf(f, "\n");
        }
        fclose(f);
        LOGI("StereoSync: {} ticks logged to DVR_simlog.txt", v.size());
    }
}

void StereoSync::Install()
{
    if (g_Installed.exchange(true)) return;
    g_RunFrame = (RunFrameFn)(GameInfo::Base() + kRunFrame);
    g_KbdPoll = (PollFn)(GameInfo::Base() + kKbdPoll); g_MousePoll = (PollFn)(GameInfo::Base() + kMousePoll);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_RunFrame, HkRunFrame);
    DetourAttach(&(PVOID&)g_KbdPoll, HkKbdPoll);
    DetourAttach(&(PVOID&)g_MousePoll, HkMousePoll);
    const LONG e = DetourTransactionCommit();
    g_PollsHooked = e == NO_ERROR;
    LOGI("StereoSync: RunFrame + keyboard/mouse read hooks {}", e == NO_ERROR ? "installed" : "FAILED");
    for (int i = 0; i < kPasses; i++)
    {   // only what the ini asks for (experimental): a pass not asked for is not touched at all
        ReusePass& r = g_Pass[i];
        if (!g_ReuseOn[r.group].load()) continue;
        r.real = (void*)(GameInfo::Base() + r.rva);
        DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)r.real, HookOf(i));
        const LONG s = DetourTransactionCommit();
        if (s != NO_ERROR) { r.real = nullptr; g_ReuseOn[r.group] = false; }
        LOGI("StereoSync: {} (experimental): the right eye's frame skips ARK_RENDERPASS_{} (exe+0x{:X}): hook {}", kReuseKey[r.group], r.name, r.rva, s == NO_ERROR ? "installed" : "FAILED");
    }
}

void StereoSync::OnPresent()
{
    UiStackTick();
    ApplyFrameCap();
    ApplyProbeBudget();
    {   // the gate for the next frames (see PairHold)
        const unsigned long long np = ++g_Presents;
        LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
        const double t = (double)c.QuadPart / (double)f.QuadPart, dt = g_LastPresentT ? t - g_LastPresentT : 0.0;
        g_LastPresentT = t;
        int paused = 0;
        if (void* gl = g_LastGl.load()) paused = ReadPaused(gl);
        const int stop = CVars::GetInt("g_stopTime", 0);
        const KbmInject::Stats ks = KbmInject::GetStats();
        const bool flat = ScreenMode::FlatRender();
        void* player = Game::GameLocal() ? Game::LocalPlayer() : nullptr;
        // a transition = any of these changed since the last Present
        const unsigned long long sig = (unsigned long long)(uintptr_t)player ^ ((unsigned long long)ks.mode << 56) ^ ((unsigned long long)flat << 58) ^
                                       ((unsigned long long)paused << 59) ^ ((unsigned long long)(stop != 0) << 60) ^ ((unsigned long long)ks.simulating << 61);
        if (sig != g_LastSig) { if (g_LastSig != ~0ull) g_CooldownUntil = np + kCooldown; g_LastSig = sig; }
        int w = wNone;
        if (paused) w = wPaused;
        else if (stop) w = wStopTime;
        else if (flat) w = wScreen;
        else if (ks.mode != 0) w = wMenu;
        else if (!ks.simulating) w = wNotSim;
        else if (!player) w = wNoPlayer;
        else if (dt > kSpikeSec) { w = wSpike; if (g_CooldownUntil < np + 2) g_CooldownUntil = np + 2; }
        else if (np < g_CooldownUntil) w = wCooldown;
        if (g_SyncOn.load()) g_GateShut[w]++;
        g_GateWhy = w; g_GateOpen = w == wNone;
    }
    bool done = false;
    { std::lock_guard lk(g_LogM); done = g_LogWant && g_Log.size() >= g_LogWant; }
    if (done) WriteLog();
}

bool StereoSync::Command(const std::string& what, std::istringstream& ss)
{
    if (what == "uistack") { int m = 1; ss >> m; g_UiStack = m != 0; { std::lock_guard lk(g_UiM); g_UiShape = "?"; } LOGI("cmd vr uistack {}", m); return true; }
    if (what == "syncdbg") { int m = 0; ss >> m; g_Dbg = m; LOGI("cmd vr syncdbg {}", m); return true; }
    if (what == "simlog")
    {
        int n = 300; ss >> n;
        std::lock_guard lk(g_LogM); g_Log.clear(); g_Log.reserve(n); g_LogWant = n;
        LOGI("cmd vr simlog {}", n);
        return true;
    }
    return false;
}

bool StereoSync::LastFrameHeld() { return g_LastHeld.load(); }
void StereoSync::SetReuse(const ReuseFlags& f)
{
    g_ReuseOn[rOcean] = f.ocean; g_ReuseOn[rReflections] = f.reflections;
    g_ReuseOn[rDecals] = f.decals; g_ReuseOn[rStreaming] = f.streaming;
}

std::string StereoSync::Status()
{
    std::string gate, exp;
    for (int i = 1; i < wCount; i++) gate += fmt::format(" {} {}", kGateNames[i], g_GateShut[i].load());
    for (int i = 0; i < xCount; i++) exp += fmt::format(" {} {}", kExpNames[i], g_Expired[i].load());
    std::string ui; { std::lock_guard lk(g_UiM); ui = fmt::format("uistack {} {} changes {}", g_UiStack.load() ? 1 : 0, g_UiShape, g_UiChanges); }
    return fmt::format("stereosync {} | ", ui) +
           fmt::format("stereosync on {} lefts {} rights {} held {} | gate {} (open {}{}) | not held:{} | input reads {} waited {} | frames {} simulated {}{}",
                       g_SyncOn.load() ? 1 : 0, g_Lefts.load(), g_Rights.load(), g_SyncHeld.load(), kGateNames[g_GateWhy.load()], g_GateShut[0].load(), gate, exp,
                       g_Polls.load(), g_PollsHeld.load(), g_Calls.load(), g_Ticks.load(), g_Dbg.load() ? fmt::format(" | syncdbg {}", g_Dbg.load()) : std::string()) +
           [] { std::string s; for (const ReusePass& r : g_Pass) if (r.real) s += fmt::format(" | {} run {} skipped (right eye) {}", r.name, r.run.load(), r.skipped.load()); return s; }();
}
unsigned long long StereoSync::Tick() { return g_Ticks.load(); }
unsigned long long StereoSync::Frames() { return g_Calls.load(); }
bool StereoSync::NextFrameRuns()
{
    // The devices are not read before a held frame (InputWaits): what is sent now is read with the next simulated one.
    if (g_PollsHooked.load() && !(g_Dbg.load() & 64)) return true;
    return !NextHeld();
}
void StereoSync::SetActive(bool on)
{
    g_SyncOn = on; g_PendingRight = false;
    LOGI("StereoSync: synchronised stereo {}", on ? "ON (each pair of frames: the left eye on a simulated frame, the right on the next, held)" : "off");
}
bool StereoSync::Active() { return g_SyncOn.load(); }
void StereoSync::LeftBuilt()
{
    g_Lefts++;
    if (!g_SyncOn.load()) return;
    g_Mark.calls = g_Calls.load(); g_Mark.presents = g_Presents.load(); g_Mark.player = Game::LocalPlayer();
    g_PendingRight = true;
}
void StereoSync::RightBuilt() { g_Rights++; }
bool StereoSync::PendingRight() { return g_SyncOn.load() && g_PendingRight.load(); }
bool StereoSync::PairWanted() { return g_SyncOn.load() && (g_GateOpen.load() || (g_Dbg.load() & 512)); }
void StereoSync::CancelPending() { if (g_PendingRight.exchange(false)) g_Expired[xFlat]++; }
int StereoSync::Debug() { return g_Dbg.load(); }
