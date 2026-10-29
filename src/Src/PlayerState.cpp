#include "pch.h"
#include "PlayerState.h"
#include "GameInfo.h"
#include "Game.h"
#include "HandAimHook.h"
#include "GpuTrace.h"
#include "KbmInject.h"
#include "ViewMatrixHook.h"
#include "ScreenMode.h"
#include "CameraHook.h"
#include "WristHud.h"
#include <cmath>
#include <vector>
#include <sstream>

namespace
{
    struct StateClass { const char* name; uint32_t vt; };
    // arkPlayerState subclasses, vtable RVAs from the RTTI complete-object locators of exe 1.77.9.0 (the parser
    // that produced them finds arkCpntPlayerCamera at exe+0x1C72218, the value measured by hand long before).
    // *SaveData / *Params are not states and are left out, as are secondary vtables (Peeking/Spyglass +0x30).
    const StateClass kStates[] = {
        { "Assassination", 0x1c83468 }, { "AutoCrouch", 0x1c812b8 },
        { "BlackMarketGoto", 0x1c3b8a8 }, { "BlackMarketInteract", 0x1c3bd48 },
        { "Blinking", 0x1c815d8 }, { "Block", 0x1c81a68 },
        { "BloodthirstyFatality", 0x1c3c108 }, { "BriskEye", 0x1c3c558 },
        { "Cancel", 0x1c81de8 }, { "CancelClimb", 0x1c786c8 },
        { "CancelFireWeapon", 0x1c84c48 }, { "CancelGrapple", 0x1c7a558 },
        { "CarryCorpse", 0x1c77748 }, { "CatFall", 0x1c82158 },
        { "CharacterChoice", 0x1c3aaa8 }, { "Choke", 0x1c77c18 },
        { "Cinematic", 0x1c77f68 }, { "Climb", 0x1c78498 },
        { "Crank", 0x1c3c9c8 }, { "CureChoke", 0x1c3cd48 },
        { "Dead", 0x1c82478 }, { "DoppelgangerSwap", 0x1c3d068 },
        { "Draw", 0x1c78a68 }, { "DropChoke", 0x1c78d68 },
        { "DropCorpse", 0x1c790c8 }, { "DropReady", 0x1c79478 },
        { "EatRat", 0x1c3aea8 }, { "EnterPossessedHost", 0x1c79878 },
        { "EquipChange", 0x1c79cf8 }, { "Falling", 0x1c845c8 },
        { "FireWeapon", 0x1c849d8 }, { "FishSwimming", 0x1c3b2b8 },
        { "Flying", 0x1c3f448 }, { "GrabGrappledEntity", 0x1c3f8e8 },
        { "Grapple", 0x1c7a328 }, { "HoldObject", 0x1c7a998 },
        { "Idle", 0x1c7af38 }, { "InAir", 0x1c7b238 },
        { "InBendTime", 0x1c7b568 }, { "Interact", 0x1c86ab8 },
        { "Journal", 0x1c7b898 }, { "Jumping", 0x1c85028 },
        { "KillCam", 0x1c3fc08 }, { "KnockedDown", 0x1c827b8 },
        { "Leaning", 0x1c82b28 }, { "LockHeartTarget", 0x1c3d4c8 },
        { "LootObject", 0x1c7bcc8 }, { "Mantle", 0x1c82f68 },
        { "Navigation", 0x1c3f198 }, { "NpcFatality", 0x1c3d878 },
        { "Pathing", 0x1c83988 }, { "Peeking", 0x1c7c4a8 },
        { "PlaceWeapon", 0x1c7c098 }, { "PlaceWeaponReady", 0x1c3db88 },
        { "PlayHeartBark", 0x1c7c908 }, { "Powerwheel", 0x1c3ff18 },
        { "ReadNote", 0x1c7ccd8 }, { "Recoil", 0x1c83c98 },
        { "ReleaseObject", 0x1c7cfe8 }, { "ReloadWeapon", 0x1c7d608 },
        { "RequestReloadWeapon", 0x1c7d3d8 }, { "Running", 0x1c85628 },
        { "ShadowHide", 0x1c7e978 }, { "ShadowWalk", 0x1c7ed28 },
        { "Slash", 0x1c85d28 }, { "Sliding", 0x1c86108 },
        { "Slipping", 0x1c86448 }, { "Sneaking", 0x1c84258 },
        { "Sprinting", 0x1c85948 }, { "Spyglass", 0x1c7e378 },
        { "SpyglassZoom", 0x1c7e5e8 }, { "StoneShroud", 0x1c3df18 },
        { "Stumble", 0x1c3e228 }, { "Stun", 0x1c86768 },
        { "SwimMantle", 0x1c7d958 }, { "Swimming", 0x1c7dc68 },
        { "SyncAnim", 0x1c3e568 }, { "ThrowWeapon", 0x1c7f118 },
        { "Undraw", 0x1c7df88 }, { "UnfoldTimeShiftDevice", 0x1c3e878 },
        { "UnfoldTimeShiftLoop", 0x1c3eb98 }, { "UseHeart", 0x1c80b78 },
        { "UsePhantasmalLink", 0x1c3eeb8 }, { "UsePower", 0x1c80ed8 },
        { "UseTimeShiftDevice", 0x1c415f8 }, { "UseWindblast", 0x1c40228 },
        { "Vault", 0x1c86e18 }, { "Versus", 0x1c405e8 },
        { "VersusGrab", 0x1c409b8 }, { "WallCrouch", 0x1c8d738 },
        { "Whisper", 0x1c8da78 },
    };
    constexpr int kNumStates = (int)(sizeof kStates / sizeof kStates[0]);
    constexpr int kMaxSlots = 96;

    struct Patched { int slots = 0; };
    Patched g_Class[kNumStates];
    // One counter per (class, slot), bumped by the stub; the snapshot is what the previous frame saw.
    volatile uint64_t* g_Count = nullptr;          // [kNumStates * kMaxSlots], lives in the stub block
    // The object each state's methods were last called on (rcx of any slot: they are all virtual methods).
    volatile uint64_t* g_This = nullptr;           // [kNumStates], after the counters
    uint64_t g_Prev[kNumStates * kMaxSlots] = {};
    uint64_t g_Accum[kNumStates * kMaxSlots] = {}; // Probe window
    bool g_Installed = false;
    int g_ProbeLeft = 0, g_ProbeTotal = 0;
    int g_TimelineLeft = 0; unsigned long long g_TimelineFrame = 0;
    unsigned long long g_LastAim = 0;
    std::string g_LastStates;
    std::mutex g_M;

    // Scripted (animation-driven) states, each with two bits:
    //   kView     the view takes its rotation from the head (ViewMatrixHook's takedown view);
    //   kTakeover the animation also owns the arms and the weapon (GpuTrace draws them as the game does) and the
    //             head-aim servo stops injecting (the animation ignores the mouse).
    // Choke is measured in the prologue save; the rest of the takedown family is there by name and cannot be
    // triggered in it (DropChoke/drop assassination need a ledge, the fatalities a fight). Slash is VIEW ONLY: the
    // prologue's lethal takedown IS a Slash with the kill outcome, and it pitches the camera +7 -> -33 deg in one
    // frame and back 0.3 s later; but Slash is also every ordinary sword swing -- same slots, measured, and the
    // camera moving <= 1.3 deg -- so the sword keeps following the controller and only the camera's jolt goes.
    // Left out: KillCam (a free camera that is not the player's eyes; the head cannot be put in it).
    //   kCine     a cutscene in the engine (the player's Cinematic state, the development notes): the view as for a takedown
    //             (the head turns, the cutscene moves the eye), the servo holds its target without injecting (the
    //             cutscene camera ignores the mouse: measured, two runs with the head still and turned 50 deg gave the
    //             same camera), the stick still turns the view, the controls are the cutscene's (KbmInject: skip), the
    //             letterbox bars are not drawn (WristHud). The arms stay on the controllers unless `vr cinearms 1`.
    //             Measured on the Outsider's shrine (Espacio 02, 14/10 19:33:08): the camera swings 66 deg in 3 s round
    //             a player who does not move, rolls 7-11 deg for seconds, and the bars hid the top and bottom quarters.
    constexpr int kUpdateSlot = 13;
    //   kHold     the servo holds its target and injects nothing while the state runs (the animation owns the camera:
    //             what the mouse did then was paid out after it, see CatFall). A takeover and a cutscene imply it.
    //   kLoco     the state is the player MOVING (climbing onto something, sliding): the view takes only its rotation from
    //             the head and the eye stays exactly where play puts it -- no rate limit (a slide runs at 6 m/s, and the
    //             takedown view's 4 m/s left the eye behind it), the eye pivot held in the room as in play
    //             (ViewMatrixHook). The hand-back also waits for the game camera's ROLL (a slide's -21 deg is blended out
    //             after the state, in Running).
    //   kCamYaw   the servo measures the CAMERA's yaw, not the body's: in a slide the mouse turns the camera 1:1 and the
    //             body only follows it, and the game snaps the body onto the camera when the slide ends.
    constexpr int kView = 1, kTakeover = 2, kCine = 4, kHold = 8, kLoco = 16, kCamYaw = 32;
    constexpr int kAllBits = kView | kTakeover | kCine | kHold | kLoco | kCamYaw;
    int g_Anim[kNumStates] = {};
    std::atomic<uint64_t> g_AnimRun[2] = {};     // the scripted states running at the last Present (one bit per state)
    std::atomic<bool> g_AnimLoco{ false }, g_ServoCam{ false };
    unsigned long long g_AnimSeenAim[kNumStates] = {};   // aim tick at which that state's update last ran
    bool g_AnimSeen[kNumStates] = {};
    std::atomic<bool> g_AnimActive{ false }, g_AnimTakeover{ false }, g_AnimCine{ false }, g_AnimHold{ false };
    std::atomic<bool> g_AnimArms{ true };
    std::atomic<bool> g_CineArms{ false };       //!< vr cinearms: the game's arms in a cutscene (0: the controllers')
    std::atomic<bool> g_ShadowArms{ true };      //!< vr shadowarms: the game's arms during Shadow Walk
    std::atomic<int> g_AnimIdx{ -1 };
    // Every state, not only the scripted ones: which ran recently (slot 13 within the last few aim ticks). Read by
    // KbmInject (is the power wheel open? is there a player at all?) and by the auto-activation.
    unsigned long long g_SeenAim[kNumStates] = {};
    bool g_Seen[kNumStates] = {};
    std::atomic<uint64_t> g_RunMask[2] = {};
    std::atomic<int> g_RunCount{ 0 };
    int FindState(const char* name)
    {
        for (int c = 0; c < kNumStates; c++) if (!_stricmp(kStates[c].name, name)) return c;
        return -1;
    }
    struct AnimInit
    {
        AnimInit()
        {
            for (const char* n : { "Choke", "DropChoke", "Assassination", "NpcFatality", "BloodthirstyFatality", "SyncAnim" })
                if (int c = FindState(n); c >= 0) g_Anim[c] = kView | kTakeover;
            // (kLoco: the eye stays where play puts it -- with play's eye pivot and pitch correction. As a plain view-only
            // state the eye came from the animation, without those: it jumped a few cm at every swing's start and end.)
            if (int c = FindState("Slash"); c >= 0) g_Anim[c] = kView | kLoco;
            if (int c = FindState("Cinematic"); c >= 0) g_Anim[c] = kView | kCine;
            // The hard landing (the development notes, Espacio 02 14/10 19:26:29, an 8 m drop): CatFall crouches the camera
            // 1 m, pitches it to -35 with 8 deg of roll and turns it 14 deg, and its blend-out (already EquipChange,
            // the weapon coming back) pitches it UP to +32 -- with the game's axis that was the picture flung up the
            // room and the head doing nothing for a second. The view follows the head, the arms stay on the
            // controllers, and the servo holds its target (kHold): it was injecting against the -35 dip, which is
            // what the +32 looked like. The hand-back hold covers the blend-out. KnockedDown is the same family by
            // name (not measured).
            for (const char* n : { "CatFall", "KnockedDown" })
                if (int c = FindState(n); c >= 0) g_Anim[c] = kView | kHold;
            // Climbing onto a table or a ledge (the development notes, Espacio 02 14/10 20:57): Mantle pitches the camera to
            // -25 deg and back in 0.5 s with 2.5 deg of roll, and IGNORES the mouse (the head turned 40 deg: the body
            // stayed put for the whole mantle and took the turn after it). With the game's axis that was the picture
            // dipping in front of the eyes and the head doing nothing. Vault and Climb are the same family by name (not
            // measured: no ledge for them in the save).
            for (const char* n : { "Mantle", "Vault", "Climb" })
                if (int c = FindState(n); c >= 0) g_Anim[c] = kView | kHold | kLoco;
            // A slide drops the eye 1.2 m and ROLLS the camera to -21 deg (blended out over 0.5 s after the state ends),
            // and the mouse turns the camera there, not the body: the servo, measuring the body, turned the camera 75
            // deg for a 40 deg head turn, the slide ended with the body snapped 35 deg onto the camera, and the servo's
            // jump guard then stalled 30 ticks and re-seeded -- the "blocked" camera at the end of the recovery. The
            // head keeps turning the body through it (no kHold), measured on the camera.
            if (int c = FindState("Sliding"); c >= 0) g_Anim[c] = kView | kLoco | kCamYaw;
        }
    } g_AnimInit;

    bool InText(uintptr_t p, uintptr_t lo, uintptr_t hi) { return p >= lo && p < hi; }

    //! Emits: mov rax, &self ; mov [rax], rcx ; mov rax, &counter ; lock inc qword [rax] ; mov rax, orig ; jmp rax.
    //! rax is volatile and carries no argument in the MSVC x64 convention, so the original sees exactly the registers
    //! and stack it was called with.
    uint8_t* EmitStub(uint8_t* p, volatile uint64_t* self, volatile uint64_t* counter, uintptr_t orig)
    {
        *p++ = 0x48; *p++ = 0xB8; memcpy(p, &self, 8); p += 8;
        *p++ = 0x48; *p++ = 0x89; *p++ = 0x08;
        *p++ = 0x48; *p++ = 0xB8; memcpy(p, &counter, 8); p += 8;
        *p++ = 0xF0; *p++ = 0x48; *p++ = 0xFF; *p++ = 0x00;
        *p++ = 0x48; *p++ = 0xB8; memcpy(p, &orig, 8); p += 8;
        *p++ = 0xFF; *p++ = 0xE0;
        return p;
    }

    std::string TickedThisFrame(bool withSlots)
    {
        std::ostringstream o;
        for (int c = 0; c < kNumStates; c++)
        {
            bool any = false;
            for (int s = 0; s < g_Class[c].slots; s++)
            {
                const int i = c * kMaxSlots + s;
                const uint64_t now = g_Count[i];
                if (now == g_Prev[i]) continue;
                if (!any) { o << (o.tellp() > 0 ? " " : "") << kStates[c].name; any = true; if (withSlots) o << ":"; }
                if (withSlots) o << s << (now - g_Prev[i] > 1 ? "x" + std::to_string(now - g_Prev[i]) : "") << ",";
            }
        }
        return o.str();
    }
}

void PlayerState::Install()
{
    if (g_Installed || !GameInfo::IsSupportedBuild()) return;
    const uintptr_t base = GameInfo::Base();
    auto* dos = (IMAGE_DOS_HEADER*)base; auto* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    uintptr_t textLo = 0, textHi = 0;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (!memcmp(sec->Name, ".text", 5)) { textLo = base + sec->VirtualAddress; textHi = textLo + sec->Misc.VirtualSize; }
    if (!textLo) { LOGI("PlayerState: no .text section"); return; }

    const size_t counterBytes = sizeof(uint64_t) * (kNumStates * kMaxSlots + kNumStates);
    const size_t stubBytes = 48ull * kNumStates * kMaxSlots;
    uint8_t* block = (uint8_t*)VirtualAlloc(nullptr, counterBytes + stubBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!block) { LOGI("PlayerState: VirtualAlloc failed"); return; }
    g_Count = (volatile uint64_t*)block;
    g_This = g_Count + kNumStates * kMaxSlots;
    uint8_t* stub = block + counterBytes;
    int total = 0;
    for (int c = 0; c < kNumStates; c++)
    {
        uintptr_t* vt = (uintptr_t*)(base + kStates[c].vt);
        int n = 0;
        while (n < kMaxSlots && InText(vt[n], textLo, textHi)) n++;
        DWORD old = 0;
        if (!n || !VirtualProtect(vt, n * 8, PAGE_READWRITE, &old)) continue;
        for (int s = 0; s < n; s++)
        {
            uint8_t* entry = stub; stub += 48;
            EmitStub(entry, &g_This[c], &g_Count[c * kMaxSlots + s], vt[s]);
            vt[s] = (uintptr_t)entry;
        }
        VirtualProtect(vt, n * 8, old, &old);
        g_Class[c].slots = n; total += n;
    }
    FlushInstructionCache(GetCurrentProcess(), block, counterBytes + stubBytes);
    g_Installed = true;
    LOGI("PlayerState: {} state vtables, {} slots redirected through counters", kNumStates, total);
}

bool PlayerState::Installed() { return g_Installed; }
void PlayerState::Probe(int frames) { std::lock_guard lk(g_M); memset(g_Accum, 0, sizeof g_Accum); g_ProbeLeft = g_ProbeTotal = frames; }
void PlayerState::Timeline(int frames) { std::lock_guard lk(g_M); g_TimelineLeft = frames; g_TimelineFrame = 0; }
std::string PlayerState::Stats()
{
    std::lock_guard lk(g_M);
    if (!g_Installed) return "off";
    const int c = g_AnimIdx.load();
    return (g_LastStates.empty() ? std::string("-") : g_LastStates) + " | anim " + (c >= 0 ? kStates[c].name : "-");
}

void PlayerState::OnPresent()
{
    if (!g_Installed) return;
    std::lock_guard lk(g_M);
    g_LastStates = TickedThisFrame(false);
    if (g_TimelineLeft > 0)
    {
        g_TimelineLeft--; g_TimelineFrame++;
        const unsigned long long aim = HandAimHook::AimTicks();
        float co[3] = {}, ca[9] = {}, po[3] = {}, pa[9] = {};
        const bool haveCam = HandAimHook::RealCamera(co, ca), havePl = Game::PlayerView(po, pa);
        const float camYaw = atan2f(ca[1], ca[0]) * 57.29578f, camPitch = asinf(ca[2] > 1 ? 1 : ca[2] < -1 ? -1 : ca[2]) * 57.29578f;
        const float camRoll = atan2f(ca[5], ca[8]) * 57.29578f;
        const float bodyYaw = atan2f(pa[1], pa[0]) * 57.29578f;
        auto ks = KbmInject::GetStats();
        float vm[16], vo[3] = {}, va[9] = {}; ViewMatrixHook::RenderView(vm, vo, va);   // what was actually rendered
        const float vYaw = atan2f(va[1], va[0]) * 57.29578f, vPitch = asinf(va[2] > 1 ? 1 : va[2] < -1 ? -1 : va[2]) * 57.29578f;
        const float vRoll = atan2f(va[5], va[8]) * 57.29578f;
        float gfx = 0, gfy = 0, bfx = 0, bfy = 0; unsigned long long nov = 0;
        CameraHook::GameFov(gfx, gfy); CameraHook::BuiltFov(bfx, bfy, nov);
        LOGI("td {} aim+{} fp {} sim {} cam {} ({:.3f},{:.3f},{:.3f}) y {:.1f} p {:.1f} r {:.1f} body {} ({:.3f},{:.3f},{:.3f}) y {:.1f} yawerr {:.1f} view ({:.3f},{:.3f},{:.3f}) y {:.1f} p {:.1f} r {:.1f} av {} sus {} fov {:.1f}x{:.1f} built {:.1f}x{:.1f} ov {} expl {} mode {} | {}",
             g_TimelineFrame, aim - g_LastAim, GpuTrace::FirstPersonDraws(), ks.simulating,
             haveCam, co[0], co[1], co[2], camYaw, camPitch, camRoll, havePl, po[0], po[1], po[2], bodyYaw, ks.yawError,
             vo[0], vo[1], vo[2], vYaw, vPitch, vRoll, ViewMatrixHook::AnimViewOn(), GpuTrace::AnimSuspended(),
             gfx, gfy, bfx, bfy, nov, CameraHook::ExplicitBuilds(), ks.mode,
             TickedThisFrame(true));
        if (!g_TimelineLeft) LOGI("td end");
    }
    g_LastAim = HandAimHook::AimTicks();
    if (g_ProbeLeft > 0)
    {
        for (int i = 0; i < kNumStates * kMaxSlots; i++) g_Accum[i] += g_Count[i] - g_Prev[i];
        if (--g_ProbeLeft == 0)
        {
            LOGI("stateprobe: {} frames", g_ProbeTotal);
            for (int c = 0; c < kNumStates; c++)
            {
                std::ostringstream o; bool any = false;
                for (int s = 0; s < g_Class[c].slots; s++)
                    if (uint64_t n = g_Accum[c * kMaxSlots + s]) { o << " " << s << ":" << n; any = true; }
                if (any) LOGI("stateprobe: {:22} {}", kStates[c].name, o.str());
            }
        }
    }
    {   // scripted-state detection. The update runs once per simulation tick, not per Present (the mock renders
        // ~140 fps against a slower tick, so every few frames show no update at all): a state stays "running" for a
        // few ticks after its last update, and a frozen tick counter (pause menu mid-choke) keeps it running.
        const unsigned long long aim = HandAimHook::AimTicks();
        int running = -1; int mask = 0; bool viewNotLoco = false;
        uint64_t run[2] = {};
        for (int c = 0; c < kNumStates; c++)
        {
            if (!g_Anim[c] || g_Class[c].slots <= kUpdateSlot) continue;
            const int i = c * kMaxSlots + kUpdateSlot;
            if (g_Count[i] != g_Prev[i]) { g_AnimSeenAim[c] = aim; g_AnimSeen[c] = true; }
            if (g_AnimSeen[c] && aim - g_AnimSeenAim[c] <= 3)
            {   // report the state that takes over, if one does: it is the one that decides the most
                if (running < 0 || ((g_Anim[c] & kTakeover) && !(g_Anim[running] & kTakeover))) running = c;
                mask |= g_Anim[c];
                if ((g_Anim[c] & kView) && !(g_Anim[c] & kLoco)) viewNotLoco = true;
                run[c >> 6] |= 1ull << (c & 63);
            }
        }
        g_AnimRun[0] = run[0]; g_AnimRun[1] = run[1];
        g_AnimLoco = (mask & kView) && !viewNotLoco;
        g_ServoCam = (mask & kCamYaw) != 0;
        {   // all states: the same "ran in the last few ticks" test, for Running()/RunningCount()
            uint64_t m[2] = {}; int n = 0;
            for (int c = 0; c < kNumStates; c++)
            {
                if (g_Class[c].slots <= kUpdateSlot) continue;
                const int i = c * kMaxSlots + kUpdateSlot;
                if (g_Count[i] != g_Prev[i]) { g_SeenAim[c] = aim; g_Seen[c] = true; }
                if (g_Seen[c] && aim - g_SeenAim[c] <= 3) { m[c >> 6] |= 1ull << (c & 63); n++; }
            }
            g_RunMask[0] = m[0]; g_RunMask[1] = m[1]; g_RunCount = n;
            // The log: every state that starts or stops (not Idle / Running, the walk's own on and off), at most 8
            // lines a second -- what the player was doing around anything reported (a crouch, a kill animation, a
            // kill cam, a knockout, a door). KillCam is not a scripted state, so this is where it shows.
            static uint64_t logged[2] = {}; static int quiet[2] = { -2, -2 };
            if (quiet[0] == -2) { quiet[0] = FindState("Idle"); quiet[1] = FindState("Running"); }
            uint64_t now[2] = { m[0], m[1] };
            for (int q : quiet) if (q >= 0) { now[q >> 6] &= ~(1ull << (q & 63)); }
            if (now[0] != logged[0] || now[1] != logged[1])
            {
                static ULONGLONG winAt = 0; static int inWin = 0, dropped = 0;
                const ULONGLONG t = GetTickCount64();
                if (t - winAt > 1000) { if (dropped) LOGI("PlayerState: ({} state changes not logged)", dropped); winAt = t; inWin = 0; dropped = 0; }
                if (inWin++ < 8)
                {
                    std::string on, off;
                    for (int c = 0; c < kNumStates; c++)
                    {
                        const bool a = (now[c >> 6] >> (c & 63)) & 1, b = (logged[c >> 6] >> (c & 63)) & 1;
                        if (a && !b) { on += " +"; on += kStates[c].name; }
                        if (!a && b) { off += " -"; off += kStates[c].name; }
                    }
                    LOGI("PlayerState:{}{}", on, off);
                }
                else dropped++;
                logged[0] = now[0]; logged[1] = now[1];
            }
        }
        const int was = g_AnimIdx.load();
        if (running != was)
        {
            if (running >= 0) LOGI("PlayerState: scripted state {} begins{} ({}{})", kStates[running].name, was >= 0 ? fmt::format(" after {}", kStates[was].name) : "",
                                   (mask & kCine) ? "cutscene" : (mask & kTakeover) ? "view + arms" : g_AnimLoco.load() ? "the head's rotation, play's eye" : "view only",
                                   (mask & kCamYaw) ? ", servo on the camera" : (mask & kHold) ? ", servo holds" : "");
            else LOGI("PlayerState: scripted state {} ends", kStates[was].name);
            g_AnimIdx = running;
        }
        g_AnimActive = (mask & kView) != 0;
        g_AnimTakeover = (mask & kTakeover) != 0;
        g_AnimCine = (mask & kCine) != 0;
        g_AnimHold = (mask & (kTakeover | kCine | kHold)) != 0;
        // (not on the menu screen: a pause during a cutscene keeps the state "running", and a menu's panels may be the
        // same untextured fills)
        WristHud::SetDropBars(g_AnimCine.load() && !ScreenMode::FlatRender());
        // The arms and the weapon go to the animation for exactly as long as it runs. Shadow Walk too: its arms are
        // shadow claws on a rig of their own, and pinning them to the controllers stretched them across the view
        // (measured reach 0.59 of a 0.60 arm); the user asked for the game's arms there. The view and the servo stay
        // as they are, the form walks and turns like the player.
        static bool shadowWas = false;
        const bool shadow = g_ShadowArms.load() && Running("ShadowWalk");
        if (shadow != shadowWas) { LOGI("PlayerState: Shadow Walk {}: arms {}", shadow ? "begins" : "ends", shadow ? "to the animation" : "back to the controllers"); shadowWas = shadow; }
        // and on the menu screen (ScreenMode): its picture is the flat game's, arms included (the pause menu draws the
        // world behind it), not arms reaching for controllers from a camera that no longer follows the head
        GpuTrace::SetAnimSuspend((g_AnimTakeover.load() && g_AnimArms.load()) || (g_AnimCine.load() && g_CineArms.load()) || shadow || ScreenMode::FlatRender());
    }
    for (int i = 0; i < kNumStates * kMaxSlots; i++) g_Prev[i] = g_Count[i];
}

bool PlayerState::AnimActive() { return g_AnimActive.load(); }
bool PlayerState::AnimTakeover() { return g_AnimTakeover.load(); }
bool PlayerState::Cinematic() { return g_AnimCine.load(); }
bool PlayerState::AnimHoldServo() { return g_AnimHold.load(); }
void PlayerState::SetCineArms(bool on) { g_CineArms = on; LOGI("PlayerState: arms in a cutscene: {}", on ? "the game's (the animation)" : "on the controllers"); }
bool PlayerState::CineArms() { return g_CineArms.load(); }
bool PlayerState::AnimActiveNow()
{
    if (g_AnimActive.load()) return true;
    if (!g_Installed) return false;
    // Has a scripted state's update run since the last Present? That is THIS frame's simulation, which the render
    // that is calling us is about to show. g_Prev is written at Present on this same (main = render) thread.
    for (int c = 0; c < kNumStates; c++)
        if ((g_Anim[c] & kView) && g_Class[c].slots > kUpdateSlot && g_Count[c * kMaxSlots + kUpdateSlot] != g_Prev[c * kMaxSlots + kUpdateSlot]) return true;
    return false;
}
bool PlayerState::AnimLocoNow()
{
    if (!g_Installed) return false;
    // The states AnimActiveNow counts (running at the last Present, or updated since): all of the viewing ones kLoco.
    // Asked on the same frame AnimActiveNow first says yes, so a mantle's first frame is already treated as one.
    const uint64_t run[2] = { g_AnimRun[0].load(), g_AnimRun[1].load() };
    bool any = false;
    for (int c = 0; c < kNumStates; c++)
    {
        if (!(g_Anim[c] & kView)) continue;
        const bool on = ((run[c >> 6] >> (c & 63)) & 1) ||
                        (g_Class[c].slots > kUpdateSlot && g_Count[c * kMaxSlots + kUpdateSlot] != g_Prev[c * kMaxSlots + kUpdateSlot]);
        if (!on) continue;
        if (!(g_Anim[c] & kLoco)) return false;
        any = true;
    }
    return any;
}
bool PlayerState::ServoOnCamera() { return g_ServoCam.load(); }
void PlayerState::SetAnimArms(bool on) { g_AnimArms = on; }
void PlayerState::SetShadowArms(bool on) { g_ShadowArms = on; }
bool PlayerState::AnimArms() { return g_AnimArms.load(); }
const char* PlayerState::AnimName() { const int c = g_AnimIdx.load(); return c >= 0 ? kStates[c].name : ""; }
bool PlayerState::SetAnimState(const char* name, int mask)
{
    const int c = FindState(name); if (c < 0) return false;
    std::lock_guard lk(g_M); g_Anim[c] = mask & kAllBits; if (!g_Anim[c]) g_AnimSeen[c] = false;
    return true;
}
bool PlayerState::Running(const char* name)
{
    const int c = FindState(name); if (c < 0) return false;
    return (g_RunMask[c >> 6].load() >> (c & 63)) & 1;
}
int PlayerState::RunningCount() { return g_RunCount.load(); }
std::string PlayerState::RunningNames()
{
    const uint64_t m[2] = { g_RunMask[0].load(), g_RunMask[1].load() };
    std::string s;
    for (int c = 0; c < kNumStates; c++) if ((m[c >> 6] >> (c & 63)) & 1) { if (!s.empty()) s += ","; s += kStates[c].name; }
    return s.empty() ? "-" : s;
}
bool PlayerState::RunningOutside(const char* const* names, int n)
{
    uint64_t m[2] = { g_RunMask[0].load(), g_RunMask[1].load() };
    for (int i = 0; i < n; i++) if (const int c = FindState(names[i]); c >= 0) m[c >> 6] &= ~(1ull << (c & 63));
    return (m[0] | m[1]) != 0;
}
void* PlayerState::StateObject(const char* name)
{
    const int c = FindState(name);
    return c >= 0 && g_This ? (void*)g_This[c] : nullptr;
}
std::string PlayerState::AnimStates()
{
    std::lock_guard lk(g_M); std::string s;
    for (int c = 0; c < kNumStates; c++) if (g_Anim[c]) { if (!s.empty()) s += ","; s += kStates[c].name; s += ":" + std::to_string(g_Anim[c]); }
    return s;
}
