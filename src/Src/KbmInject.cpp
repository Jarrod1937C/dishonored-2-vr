#include "pch.h"
#include "KbmInject.h"
#include "VRRuntime.h"
#include "XRSession.h"
#include "Game.h"
#include "HandAimHook.h"
#include "GpuTrace.h"
#include "PlayerState.h"
#include "SwordGesture.h"
#include "SwordContact.h"
#include "MeleeAim.h"
#include "ThrowGesture.h"
#include "GrabHand.h"
#include "Collision.h"
#include "Holster.h"
#include "VRMath.h"
#include "BodyHide.h"
#include "KeyBinds.h"
#include "CVars.h"
#include "ScreenMode.h"
#include "ViewMatrixHook.h"
#include "BinkVideo.h"
#include "Overlay.h"
#include "StereoSync.h"
#include "CameraHook.h"
#include <cmath>
#include <algorithm>

namespace
{
    std::atomic<bool> g_Enabled{ false };
    std::atomic<float> g_SnapDeg{ 0.f };          //!< 0 = smooth turning (the user's choice); > 0 = snap by this many degrees
    std::atomic<float> g_CountsPerDeg{ 24.2f };   //!< mouse counts per degree of yaw AT kRefSensitivity (measured: 450 counts = 18.6 deg at g_mouseSensitivity 0.808)
    // The player's mouse settings, which the counts above were not measured with. The servo's loop gain is
    // kYawGain * (real counts/deg) / (counts/deg it assumes): at the developer's sensitivity 0.808 it is 0.7, at 2.0 it
    // would be 1.7 (overshoot, ringing) and at 3.0 past 2 (the view oscillates without end); at 0.3 it is 0.26 (the view
    // trails the head). The game turns by counts * sensitivity, so the counts are scaled by 0.808 / sensitivity, read
    // from the game every tick (the options menu can change it mid-game). And "invert mouse Y" flips which way a count
    // turns the pitch: unflipped, the pitch servo would push the view AWAY from the head and run it into the clamp.
    constexpr float kRefSensitivity = 0.808f;
    float g_SensScale = 1.f; bool g_InvertY = false;
    void ReadMouseSettings()
    {
        static float lastSens = -1.f; static int lastInv = -1;
        float sens = CVars::GetFloat("g_mouseSensitivity", kRefSensitivity);
        if (!(sens > 0.01f)) sens = kRefSensitivity;          // also catches NaN
        if (sens > 50.f) sens = 50.f;
        const int inv = CVars::GetInt("g_invertMouseYAxis", 0) != 0;
        g_SensScale = kRefSensitivity / sens; g_InvertY = inv != 0;
        if (sens != lastSens || inv != lastInv)
        {
            LOGI("KbmInject: game mouse sensitivity {:.3f} (head aim scales its mouse counts by {:.2f}), invert Y {}", sens, g_SensScale, inv ? "ON (pitch counts flipped)" : "off");
            lastSens = sens; lastInv = inv;
        }
    }
    float CountsPerDeg() { return g_CountsPerDeg.load() * g_SensScale; }
    std::atomic<bool> g_HeadAim{ false };
    float g_LastHeadYaw = 0, g_LastHeadPitch = 0; bool g_HeadValid = false;
    // Head aim drives the body yaw as a SERVO, not as a scaled feed-forward. Feeding the head's delta through a
    // measured counts/degree is open loop, and the measurement is not repeatable: two runs of the auto-calibration
    // in the same save returned 29.0 and 35.4 counts/deg. At 35.4 the body over-turned by ~45% (45 deg of head ->
    // 65 deg of body, 90 -> 129) and, because the input is incremental, the error ACCUMULATED - after five turns
    // the body was 136 deg away from the head. Tracking the absolute target instead makes the gain a convergence
    // rate rather than a calibration: any error is corrected on the following frames instead of being banked.
    float g_DesiredYaw = 0, g_DesiredPitch = 0; bool g_ServoValid = false;
    // Every turn the STICK has added to the servo's target, in degrees (game yaw convention, + = left). The takedown
    // view takes its rotation from the head, so during a view-only state (a Slash still accepts turning) it has to
    // add the stick's share itself, or the game would turn away under it and the handover would wait for nothing.
    std::atomic<float> g_StickTurn{ 0.f };
    constexpr float kYawGain = 0.7f;            // < 1 so an over-estimated counts/deg damps instead of oscillating
    constexpr float kPitchLimit = 80.f;         // the game clamps near vertical; do not wind the target past it
    float g_YawAccum = 0, g_PitchAccum = 0;       //!< pending mouse counts (spread over frames: the game clamps ~45 deg/frame)
    bool g_Simulating = true;                     //!< false in a menu / loading screen: see the note in Tick()
    constexpr int kMaxCountsPerFrame = 300;
    // Whether the controllers drive the game's MENUS. They must not, by default. The servo is frozen while the game
    // is not simulating, but the rest of this function was not: in the pause / save / load screen the right stick
    // still fell through to the open-loop snap branch (45 deg * 24.2 counts/deg = 1089 mouse counts per flick, which
    // throws the cursor across the screen) and the right trigger still went out as a LEFT MOUSE BUTTON, which is
    // what activates whatever the cursor is over. Reported from hardware as "I cannot save or load once I am in the
    // game": the entry you aim at is not the entry that gets clicked. It is worse than an annoyance -- in the save
    // screen a stray click lands on an existing save, and the next one confirms overwriting it.
    // 1 restores the old behaviour, for driving a menu from the controllers on purpose.
    std::atomic<int> g_MenuInput{ 0 };
    bool g_MenuSafe = false;                      //!< this tick is inside a menu and injection is suppressed
    // Auto-calibration: inject a known delta and measure the player's yaw change. It RETRIES, because the useful
    // moment to ask for it is when the VR mode is switched on, which is often a menu or a load screen -- there is
    // no player to measure yet, and a single attempt would silently leave the default 24.2 counts/deg in place.
    // Measured cost of getting this wrong: the body turns ~85% of the head's angle, and since head aim is
    // incremental the shortfall accumulates over every turn.
    int g_CalState = 0; float g_CalYaw0 = 0; int g_CalFrames = 0; int g_CalTries = 0; int g_CalDelay = 0;
    const int kCalCounts = 200, kCalMaxTries = 20, kCalRetryFrames = 45;
    std::mutex g_M; KbmInject::Stats g_Stats;

    // Key slots we may hold down. Bindings from the user's IN_GAME set (dishonored2Config.cfg).
    // IN_GAME bindings: W/S/A/D move, LSHIFT sprint, SPACE jump, C crouch, F interact, LALT lean,
    // LCTRL block, Z zoom, ESC pause, MOUSE1 melee, MOUSE2 power/weapon, MOUSE3 power wheel,
    // R/T quick health/mana potion, J journal, 1..0 the quick slots (WEAP_n). ENTER and the arrows drive the menus;
    // TAB is the menus' next tab (the load list's Espacio 01/02/03, the journal's MISION/PODERES/...: it wraps, and
    // there is no previous -- SHIFT+TAB went forward too, measured) and PGUP/PGDN the journal's sub-tabs (Objetivos /
    // Pistas / Objetos), shown on the menus' own key hints.
    enum Slot { kW, kS, kA, kD, kShift, kSpace, kC, kF, kAlt, kCtrl, kZ, kEsc, kEnter, kUp, kDown, kLeft, kRight,
                kR, kT, kJ, kTab, kPgUp, kPgDn, kQuick1, kQuick2, kQuick3, kQuick4, kLMB, kRMB, kMMB, kCount };
    // What each slot presses, by the GAME's name for the key (KeyBinds: a physical key position, sent by scan code,
    // so the layout -- AZERTY, QWERTZ -- does not change which key the game sees) and, for a game action, the action
    // whose key the player may have rebound in the game's options (read from its config; the default key otherwise).
    // The menu keys (ESC, ENTER, arrows, TAB, PGUP/PGDN) are fixed: the menus answer to them whatever the bindings.
    struct SlotDef { const char* key; const char* action; };
    const SlotDef kSlotDef[kCount] = {
        { "W", "MOVE_FORWARD" }, { "S", "MOVE_BACKWARD" }, { "A", "MOVE_LEFT" }, { "D", "MOVE_RIGHT" },
        { "LSHIFT", "SPRINT" }, { "SPACE", "JUMP" }, { "C", "CROUCH" }, { "F", "INTERACT" }, { "LALT", "LEAN" },
        { "LCTRL", "BLOCK" }, { "Z", "ZOOM" }, { "ESCAPE", nullptr }, { "ENTER", nullptr }, { "UPARROW", nullptr },
        { "DOWNARROW", nullptr }, { "LEFTARROW", nullptr }, { "RIGHTARROW", nullptr },
        { "R", "QUICK_USE_HEALTH_POTION" }, { "T", "QUICK_USE_MANA_POTION" }, { "J", "JOURNAL" }, { "TAB", nullptr },
        { "PGUP", nullptr }, { "PGDN", nullptr }, { "1", "WEAP_1" }, { "2", "WEAP_2" }, { "3", "WEAP_3" }, { "4", "WEAP_4" },
        { "MOUSE1", "MELEE" }, { "MOUSE2", "POWER_WEAPON" }, { "MOUSE3", "POWERWHEEL" } };
    KeyBinds::Bind g_Bind[kCount];                //!< what each slot sends right now (Rebind)
    unsigned g_BindGen = ~0u;                     //!< the KeyBinds generation g_Bind was resolved from
    std::string g_QuickKeys = "1234";             //!< `vr quickkeys`: the game's quick slot (digit) or key (letter) of Y/X
    bool g_Held[kCount] = {};
    double g_TapUntil[kCount] = {};               //!< a tapped key goes up at this time (0 = not a tap)
    unsigned g_Frame = 0;
    bool g_SnapArmed = true;
    // [Controls] SnapTurnBlank: the headset's picture goes black from a snap until an image rendered after the game
    // turned is shown. Below the full frame rate the frames already in flight were rendered facing the old way and are
    // claimed at the new heading (the turn is in WorldYawOffset at once): the compositor turned them into place, and
    // the edge they do not cover showed as a black border swinging back. 0 none, 1 the game is still turning, 2 turned:
    // black until the view serial g_SnapUntil (or, with no serial to go by, the time g_SnapUntilT), then a short fade in.
    std::atomic<bool> g_SnapBlankOn{ false };
    std::atomic<float> g_SnapBlankDepth{ 1.f };    //!< SnapTurnBlankOpacity: how dark the blank is (1 = black, 0.3 = 30 % darker)
    std::atomic<int> g_SnapState{ 0 };
    std::atomic<unsigned long long> g_SnapUntil{ 0 };
    std::atomic<double> g_SnapT0{ 0 }, g_SnapUntilT{ 0 }, g_SnapShownT{ 0 };
    std::atomic<unsigned long long> g_SnapBlanks{ 0 };
    constexpr double kSnapTimeout = 0.5, kSnapFadeIn = 0.06;
    constexpr float kSnapSettledDeg = 1.5f;
    double QpcNow() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart; }
    void SnapBlankStart() { if (!g_SnapBlankOn.load()) return; g_SnapT0 = QpcNow(); g_SnapShownT = 0; g_SnapState = 1; g_SnapBlanks++; }
    //! The game has turned (or was told to, without the servo): black until a view built from now on is shown.
    void SnapBlankSettled(unsigned long long extraViews)
    {
        if (g_SnapState.load() != 1) return;
        const unsigned long long latest = ViewMatrixHook::LatestSerial();
        g_SnapUntil = latest ? latest + extraViews : 0;
        g_SnapUntilT = QpcNow() + 0.035 * (double)extraViews;
        g_SnapState = 2;
    }
    double g_Now = 0, g_Dt = 1.0 / 90;            //!< seconds, from QPC; dt clamped (a hitch must not fling the view)

    // Console-style layout (the user's choice, 2026-09-30): smooth turning, crouch toggles on B (the game's C is a
    // toggle itself), sprint LATCHES on a left-stick click until you stop. In a menu the sticks are the arrows, A is
    // ENTER and B / menu are ESC -- keys that only move the highlight and confirm it, never a cursor or a click.
    // Revised 2026-10-05 (the user's choice, after the console version): the left menu button opens the JOURNAL with a
    // short press and pauses when held; potions are drunk from the power wheel (B health, A mana: the console's
    // Circle / Cross with L1 held), no longer by flicking the right stick (too easy to set off by mistake); Y and X are
    // four quick slots like the console's d-pad (short / held), assigned from the wheel the same way.
    std::atomic<float> g_TurnSpeed{ 120.f };      //!< smooth turn, degrees per second at full deflection
    std::atomic<int> g_MovePwm{ 0 };              //!< 1 = the old 8-frame pulse-width modulation of the move keys
    std::atomic<int> g_Potions{ 0 };              //!< 1 = the old right-stick potions (up health, down mana)
    // [Controls] GripThresholdLeft / GripThresholdRight: how far each grip is pressed (0..1: the squeeze on Touch, the
    // squeeze FORCE on an Index) before it counts, and it lets go 0.15 below that (at least 0.02), so a grip held right
    // at the line does not flicker.
    std::atomic<float> g_GripOn[2] = { 0.6f, 0.6f };
    bool GripPressed(int h, float v)
    {
        static bool down[2] = {};
        const float on = g_GripOn[h].load(), off = std::max(0.02f, on - 0.15f);
        down[h] = down[h] ? v > off : v > on;
        return down[h];
    }
    std::atomic<int> g_QuickSlots{ 1 };           //!< 0 = the old Y (lean) and X (interact); 2 = the Index layout (trackpads, PadBits)
    std::atomic<float> g_QuickHold{ 0.5f };       //!< seconds held for Y / X to mean slot 3 / 4
    bool g_SprintLatch = false; double g_StillSince = 0;
    bool g_MoveKeys[4] = {};                      //!< 8-way digital move state, with hysteresis
    double g_MenuBtnDown = -1; bool g_PauseSent = false;
    bool g_PotionArmed = true;
    // Y and X: a press shorter than g_QuickHold is slot 1 / 2, sent when the button comes up (only then is it known to
    // be short); held that long it is slot 3 / 4, sent at once with a buzz in the left hand so the player knows which
    // one went out without looking. With the wheel open the same key ASSIGNS the highlighted item to that slot
    // (measured 2026-10-01: a number with the wheel open assigns it and leaves the wheel open).
    struct QuickBtn { unsigned bit; Slot shortSlot, longSlot; double downAt; bool down, longSent; };
    QuickBtn g_QuickBtn[2] = { { kXRBtnY, kQuick1, kQuick3, 0, false, false }, { kXRBtnX, kQuick2, kQuick4, 0, false, false } };
    struct { unsigned long long quick[4], potions[2], tabs, menuJournal, menuPause, wheelCloses, skips, videoSkips; } g_Count = {};
    std::atomic<bool> g_CineControls{ true };     //!< vr cinecontrols: the cutscene's own controls (0 = play's, as before)
    bool g_InCine = false;
    // Menus over the running WORLD: the character choice of a new game (CharacterChoice) and a black
    // market's counter (BlackMarketInteract). The game keeps simulating there and draws the first-person arms, so
    // nothing else said "menu": the controllers were play's -- A jumped and nothing sent ENTER (the choice switched with
    // the stick and could never be confirmed), B crouched (a shop section could not be left), and the head servo's mouse
    // drove the shop's cursor. Each is a player state of its own; while one runs it is a menu, on the menu screen.
    enum Scene { kSceneNone = 0, kSceneChoice = 1, kSceneShop = 2, kSceneCount };
    const char* SceneName(int s) { return s == kSceneChoice ? "character choice" : s == kSceneShop ? "black market" : "none"; }
    std::atomic<int> g_SceneMenus{ 1 };           //!< vr scenemenus 0|1 (0 = play's controls there, as before)
    std::atomic<int> g_PlayRule{ 1 };             //!< vr playrule 0|1 (0 = the list of armless states only, as before 2026-10-23)
    bool g_PlayLatched = false;                   //!< (Tick) this player has been seen playing: no arms is not a menu
    int g_Scene = kSceneNone;
    // B and the menu button stay ESC there, as in every menu (measured, sent the controllers' way: at a black market ESC
    // leaves a section and then the counter; at the character choice it does nothing, and its "Play as ...? No / Yes"
    // has no back at all -- neither ESC nor BACKSPACE closes it -- so No is picked with the stick and A, as on a pad).
    //! A black market draws the game's own mouse cursor over its counter, wherever the mouse last left it (the head
    //! servo's counts put it on the shopkeeper's chest, measured). On the way in it is pushed into the top right corner,
    //! out of the picture, over this many seconds of mouse motion.
    double g_ParkCursorUntil = -1;
    // Test aid (`vr tapkey <KEY> [ms]`): one key by the game's name, sent exactly the way the controllers send theirs.
    KeyBinds::Bind g_TestBind; double g_TestMs = 0; bool g_TestPending = false; double g_TestUp = -1;
    // A number pressed with the wheel open assigns the slot AND makes the game keep the wheel open: letting go of
    // MOUSE3 no longer closes it (measured: it stayed up with the grip released; one more MOUSE3 click closed it and
    // equipped the highlighted item). So a wheel that assigned something gets that click when the grip opens.
    bool g_WheelAssigned = false; double g_WheelCloseAt = -1;
    // Input modes. A button held across a change of mode is LATCHED -- ignored until released -- so the A that
    // confirmed "Continue" does not land as a jump, and the menu button that paused does not also close the menu.
    enum Mode { kModePlay, kModeMenu, kModeWheel };
    Mode g_Mode = kModeMenu;
    unsigned g_Latched = 0, g_PrevBtn = 0;
    bool g_StickLatch = false;   //!< both sticks ignored until they rest (set on leaving the power wheel)
    std::atomic<bool> g_Possessing{ false };   //!< inside a possessed host (see Tick)
    bool g_PossessionEdge = false;             //!< it began or ended this tick: the servo re-seeds
    enum : unsigned { kVTrigL = 1u << 8, kVTrigR = 1u << 9, kVGripL = 1u << 10, kVGripR = 1u << 11 };
    // The Index layout's trackpad presses ([Controls] Layout = index, `vr quickslots 2`): the left pad's edges are a
    // d-pad (up / right / down / left = quick slots 1-4, as the console's d-pad), its middle is the menu button, and the
    // right pad is the interact key -- on the hand whose ray picks what is interacted with.
    enum : unsigned { kVPadUp = 1u << 12, kVPadRight = 1u << 13, kVPadDown = 1u << 14, kVPadLeft = 1u << 15, kVPadUse = 1u << 16 };
    constexpr unsigned kVPadDirs = kVPadUp | kVPadRight | kVPadDown | kVPadLeft;
    //! The trackpads as presses (force past 0.5, let go below 0.3), each press's meaning fixed by where it began.
    unsigned PadBits(const XRInputState& in)
    {
        static bool down[2] = {}; static unsigned what[2] = {};
        for (int h = 0; h < 2; h++)
        {
            const float f = in.padForce[h];
            if (!down[h] && f > 0.5f)
            {
                down[h] = true;
                if (h == 1) what[h] = kVPadUse;
                else
                {
                    const float x = in.padX[0], y = in.padY[0];
                    what[h] = x * x + y * y < 0.45f * 0.45f ? kXRBtnMenu
                            : fabsf(y) >= fabsf(x) ? (y > 0 ? kVPadUp : kVPadDown) : (x > 0 ? kVPadRight : kVPadLeft);
                }
            }
            else if (down[h] && f < 0.3f) down[h] = false;
        }
        return (down[0] ? what[0] : 0u) | (down[1] ? what[1] : 0u);
    }
    int g_MenuDir = -1; double g_MenuDirNext = 0;   //!< menu arrow held (slot) and when it repeats
    const char* ModeName(Mode m) { return m == kModePlay ? "play" : m == kModeMenu ? "menu" : "wheel"; }

    //! A synthesized mouse BUTTON goes to the window under the system cursor, not to the foreground window. The game
    //! runs in a window and does not hold the cursor during play (it reads raw mouse deltas), so the cursor can sit
    //! over anything. Measured 2026-09-30: a Windows Firewall prompt popped up over the game, the cursor was on it,
    //! and every sword strike clicked the prompt instead -- the game lost the focus (and paused) on each one, and
    //! the cursor was a few pixels above the prompt's "Allow" button. So before a button goes out, the cursor must be
    //! over the game: if it is not, move it to a point of the game's client area that the game itself is visible at;
    //! if there is none, the click is not sent at all.
    bool CursorOnGame()
    {
        HWND fg = GetForegroundWindow(); if (!fg) return false;
        POINT pt; if (!GetCursorPos(&pt)) return false;
        // By PROCESS, not by window: the foreground window and the one under the cursor are both the game's but need
        // not be the same top-level window (measured: comparing the roots refused every click on the game itself).
        auto ownedByGame = [&](POINT p)
        {
            HWND w = WindowFromPoint(p); if (!w) return false;
            DWORD pid = 0; GetWindowThreadProcessId(GetAncestor(w, GA_ROOT), &pid);
            return pid == GetCurrentProcessId();
        };
        // ...and inside its CLIENT area, not on the frame. The servo's mouse motion walks the system cursor to the screen's
        // edge, and with the window at (0,0) x = 0 is the game's own left sizing border: a click there starts Windows'
        // size loop inside the game's DefWindowProc, which waits for the button-up -- that this thread would send on a
        // later tick that never comes. The game froze until ESC (measured 2026-10-18: a 6.6 s stall, the render thread
        // in USER32/uxtheme under the game's WndProc, the cursor at x = 0). The window's client rectangle comes from
        // GetWindowInfo, in screen coordinates: GetClientRect is answered by RenderRes with the headset's render size.
        WINDOWINFO wi{}; wi.cbSize = sizeof wi;
        if (!GetWindowInfo(fg, &wi)) return false;
        const RECT cl = wi.rcClient;
        if (cl.right - cl.left < 16 || cl.bottom - cl.top < 16) return false;
        auto inClient = [&](POINT p) { return p.x >= cl.left + 4 && p.x < cl.right - 4 && p.y >= cl.top + 4 && p.y < cl.bottom - 4; };
        if (inClient(pt) && ownedByGame(pt)) return true;
        const float fx[] = { 0.5f, 0.12f, 0.88f, 0.12f, 0.88f, 0.5f, 0.5f }, fy[] = { 0.5f, 0.12f, 0.12f, 0.88f, 0.88f, 0.1f, 0.9f };
        for (int i = 0; i < 7; i++)
        {
            POINT c{ cl.left + (LONG)((cl.right - cl.left) * fx[i]), cl.top + (LONG)((cl.bottom - cl.top) * fy[i]) };
            if (ownedByGame(c))
            {
                SetCursorPos(c.x, c.y);
                LOGI("KbmInject: the cursor was {} ({},{}): moved onto the game at ({},{}) before a click",
                     ownedByGame(pt) ? "on the game window's frame" : "over another window", pt.x, pt.y, c.x, c.y);
                return true;
            }
        }
        static int logged = 0;
        if (logged++ < 20) LOGI("KbmInject: mouse click NOT sent: no point of the game window is visible under the cursor");
        return false;
    }

    //! The controllers only reach the game while its window is the foreground one (SendInput goes there), and the game
    //! pauses when it loses it. A player in the headset cannot see that the game went behind another window -- the
    //! controllers just stop. So the game is brought to the front when the player shows they are playing: a controller
    //! button or a pushed stick while it is behind, or the headset being put on (the session turning FOCUSED). Not at
    //! any other time: someone at the desktop with the headset off keeps their window.
    //! Windows only lets the foreground process hand the foreground over, so the plain call usually fails from here;
    //! the standard way round is to attach to the foreground window's input queue for the call. The other well-known
    //! trick, a synthesized Alt tap, is NOT used: a lone Alt landing on the game opens its system menu's modal loop.
    std::atomic<int> g_AutoFocus{ 1 };   // `vr autofocus 0|1`
    struct { unsigned long long tries = 0, done = 0, failed = 0, unpaused = 0; } g_Focus;
    bool BringGameToFront(const char* why)
    {
        HWND w = Overlay::Window();
        if (!w || !IsWindow(w)) return false;
        g_Focus.tries++;
        if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
        bool ok = SetForegroundWindow(w) && GetForegroundWindow() == w;
        if (!ok)
        {
            // Not onto a window that is not responding: sharing its input queue could stall this thread -- the game's
            // own. (Across integrity levels, an elevated window in front, AttachThreadInput simply fails.)
            HWND fg = GetForegroundWindow();
            const DWORD fgThread = fg && !IsHungAppWindow(fg) ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
            if (fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE))
            {
                BringWindowToTop(w);
                SetForegroundWindow(w);
                AttachThreadInput(me, fgThread, FALSE);
            }
            ok = GetForegroundWindow() == w;
        }
        if (ok) g_Focus.done++; else g_Focus.failed++;
        // every success; the first refusals and then one in 30 (it is retried every 2 s while the controllers are used)
        if (ok || g_Focus.failed <= 3 || g_Focus.failed % 30 == 0)
            LOGI("KbmInject: the game was not the window in front ({}): {}", why,
                 ok ? "brought to the front" : fmt::format("Windows refused to bring it to the front ({} times; click the game window on the PC)", g_Focus.failed));
        return ok;
    }

    // The melee button as a KEYBOARD key instead of MOUSE1 (0 = the mouse button). A key goes to the focused window
    // whatever the cursor is over, so a melee bound to a key (arkBind "<key>" "MELEE") can never click another
    // window. Research knob for now: it needs that binding in the game, which the user has to agree to.
    std::atomic<int> g_MeleeKey{ 0 };

    //! One key or button, down or up. Keys go by SCANCODE: the game reads DirectInput, which sees scan codes, and a
    //! bare virtual key (wScan 0) is not seen at all -- measured on the choke prompt ("Ctrl" = choke): Ctrl sent that
    //! way counted two key events and choked nothing, the same key by scancode (0x1D) choked at once. The arrows and
    //! PGUP/PGDN share their scancodes with the keypad; only the extended bit makes them arrows (KeyBinds::Bind::ext).
    void SendBind(const KeyBinds::Bind& b, bool down)
    {
        INPUT in{};
        if (b.mouse)
        {
            // A release always goes out (a held button must never be left down); a press only onto the game.
            if (down && !CursorOnGame()) return;
            in.type = INPUT_MOUSE;
            switch (b.mouse)
            {
            case 1: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
            case 2: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
            case 3: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
            case 4: case 5: in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; in.mi.mouseData = b.mouse == 4 ? XBUTTON1 : XBUTTON2; break;
            default:   // the wheel has no "up": one notch on the press
                if (!down) return;
                in.mi.dwFlags = MOUSEEVENTF_WHEEL; in.mi.mouseData = (DWORD)(b.mouse == 6 ? WHEEL_DELTA : -WHEEL_DELTA); break;
            }
        }
        else if (b.scan)
        {
            in.type = INPUT_KEYBOARD;
            in.ki.wScan = b.scan;
            in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP) | (b.ext ? KEYEVENTF_EXTENDEDKEY : 0);
        }
        else return;
        SendInput(1, &in, sizeof in);
        std::lock_guard lk(g_M); g_Stats.keyEvents++;
    }
    void SendKey(Slot s, bool down)
    {
        if (s == kLMB && g_MeleeKey.load())
        {   // research knob (`vr meleekey <vk>`): melee as a keyboard key, by the layout's scancode of that virtual key
            KeyBinds::Bind b; b.scan = (uint16_t)MapVirtualKeyW((UINT)g_MeleeKey.load(), MAPVK_VK_TO_VSC); b.name = "meleekey";
            SendBind(b, down); return;
        }
        SendBind(g_Bind[s], down);
    }
    //! Resolve every slot to a key: the player's binding for its action, else the game's default key. A slot whose key
    //! changes while held is released with its OLD key first, or the game would see that key stuck down.
    void Rebind()
    {
        std::string changed, missing;
        static char quickAction[4][8];
        for (int i = 0; i < kCount; i++)
        {
            KeyBinds::Bind b;
            const char* action = kSlotDef[i].action;
            std::string want = kSlotDef[i].key;
            if (i >= kQuick1 && i <= kQuick4)
            {   // `vr quickkeys`: a digit is the game's quick slot WEAP_<digit>; a letter is that key itself
                const int q = i - kQuick1;
                const char c = (size_t)q < g_QuickKeys.size() ? g_QuickKeys[q] : (char)('1' + q);
                want = std::string(1, c);
                if (c >= '0' && c <= '9') { snprintf(quickAction[q], sizeof quickAction[q], "WEAP_%c", c); action = quickAction[q]; }
                else action = nullptr;
            }
            const bool bound = action && KeyBinds::ForAction(action, b);
            if (!bound)
            {
                KeyBinds::FromName(want, b);
                if (action) missing += fmt::format(" {}", action);
            }
            if (action && b.name != want) changed += fmt::format(" {}={}", action, b.name);
            if (b.name != g_Bind[i].name || b.scan != g_Bind[i].scan || b.mouse != g_Bind[i].mouse)
            {
                if (g_Held[i] && g_Bind[i].valid()) { SendBind(g_Bind[i], false); g_Held[i] = false; g_TapUntil[i] = 0; }
                g_Bind[i] = b;
            }
        }
        LOGI("KbmInject: keys resolved ({}){}{}", KeyBinds::Info(),
             changed.empty() ? ": every action on the game's default key" : "; rebound by the player:" + changed,
             missing.empty() ? "" : "; no key/mouse binding found, the default key is used for:" + missing);
    }
    double g_DownAt[kCount] = {}, g_UpAt[kCount] = {};
    constexpr double kMinDown = 0.08;
    // A gesture strike waits until block has been up this long. Measured: an attack pressed on the same tick block is
    // released is LOST (the Block state was still running for that tick and the game wants a fresh press after it),
    // while one pressed 70 ms after the release strikes. A backhand swing starts at the left shoulder, where the fist
    // blocks, so this is the common case, not a corner.
    constexpr double kAfterBlock = 0.06;
    double g_PendingStrike = -1;                  //!< when a gesture strike was asked for, while waiting for block
    bool g_GripTakes = false;                     //!< this press of the grabbing hand's grip took/interacts (no wheel/block)
    bool g_GripF = false;                         //!< ...and is the game's interact key, held down with the grip
    bool g_TrigLets = false;                      //!< this press of the grabbing hand's trigger let the object go
    bool g_WasHolding = false; double g_HoldEndedAt = -1;
    // PullToOpen: the grip closed on a drawer / cabinet the hand touches waits for the pull before it presses interact
    struct Pull { bool armed = false; float start[3] = {}, n[3] = {}; double at = 0; uint32_t id = 0; } g_Pull;
    // PhysicalPickup: an item grabbed by hand, on its way to the back (stored) or the mouth (eaten)
    struct Carry { bool on = false; uint32_t id = 0; double at = 0, zoneAt = -1; } g_Carry;
    struct Verify { uint32_t id = 0; double at = -1; } g_Verify;   // the store's interact sent: did the game take it?
    double g_CarryClearAt = -1;                                     // when a returning item's drawing is handed back
    //! The item goes back where it was taken from (refused, let go, out of reach): it flies there, then is drawn as is.
    void CarryReturn()
    {
        GpuTrace::ReturnCarry(); g_CarryClearAt = g_Now + 0.4;
        GrabHand::UnlockSensor();
    }
    //! Where the hand is, relative to the head, in the room: behind the back (stores) or at the mouth (eats).
    int CarryZone(const XRInputState& in, int hand)
    {
        const XRHandPose& h = in.hand[hand & 1];
        const VRRuntime::HeadPose hp = VRRuntime::GetHeadPose();
        if (!h.valid || !hp.valid) return 0;
        float hf[3], hr[3], hu[3]; VRMath::QuatBasis(hp.quat, hf, hr, hu);
        float f[3] = { hf[0], 0.f, hf[2] }; float n = sqrtf(f[0] * f[0] + f[2] * f[2]); if (n < 1e-3f) { f[0] = 0; f[2] = -1; n = 1; }
        f[0] /= n; f[2] /= n;
        const float right[3] = { -f[2], 0.f, f[0] };
        const float rel[3] = { h.posX - hp.pos[0], h.posY - hp.pos[1], h.posZ - hp.pos[2] };
        const float rx = rel[0] * right[0] + rel[2] * right[2], ry = rel[1], rz = rel[0] * f[0] + rel[2] * f[2];
        if (rz < -0.12f && ry > -0.9f && ry < 0.35f) return 1;                         // behind the back / over the shoulder
        if (fabsf(rx) < 0.12f && ry > -0.22f && ry < 0.02f && rz > -0.02f && rz < 0.20f) return 2;   // at the mouth
        return 0;
    }
    // [Gameplay] HolsterWeapons: where the LEFT hand is on the body -- 1 at the left hip (the pistol), 2 behind the back
    // (the crossbow), 3 on the chest (the last power), 0 elsewhere. Measured from the NECK (the eyes less a head-and-neck offset in the head's own frame,
    // so looking down at the hip does not move it, and sitting or standing changes nothing) in the TORSO's frame: the
    // head's heading, which the torso follows only once the head is turned more than 35 degrees from it (and slowly
    // after that), so a glance to the side does not swing the hip round to the back.
    float g_TorsoYaw = 0; bool g_TorsoOk = false;
    void TorsoTick(const VRRuntime::HeadPose& hp)
    {
        if (!hp.valid) return;
        float hf[3], hr[3], hu[3]; VRMath::QuatBasis(hp.quat, hf, hr, hu);
        if (hf[0] * hf[0] + hf[2] * hf[2] < 0.04f) return;           // looking straight up / down: keep the last heading
        const float head = atan2f(hf[0], -hf[2]);
        if (!g_TorsoOk) { g_TorsoYaw = head; g_TorsoOk = true; return; }
        auto wrap = [](float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; };
        const float lim = 35.f * 0.01745329f;
        float d = wrap(head - g_TorsoYaw);
        if (d > lim) g_TorsoYaw = wrap(head - lim); else if (d < -lim) g_TorsoYaw = wrap(head + lim);
        d = wrap(head - g_TorsoYaw);
        g_TorsoYaw = wrap(g_TorsoYaw + d * (float)std::min(1.0, g_Dt / 2.0));
    }
    int HolsterZone(const XRInputState& in, float rel[3])
    {
        rel[0] = rel[1] = rel[2] = 0;
        const XRHandPose& h = in.hand[0];
        const VRRuntime::HeadPose hp = VRRuntime::GetHeadPose();
        if (!h.valid || !hp.valid || !g_TorsoOk) return 0;
        float hf[3], hr[3], hu[3]; VRMath::QuatBasis(hp.quat, hf, hr, hu);
        float neck[3];
        for (int i = 0; i < 3; i++) neck[i] = hp.pos[i] - hu[i] * 0.11f - hf[i] * 0.09f;
        const float f[3] = { sinf(g_TorsoYaw), 0.f, -cosf(g_TorsoYaw) };
        const float right[3] = { -f[2], 0.f, f[0] };
        const float d[3] = { h.posX - neck[0], h.posY - neck[1], h.posZ - neck[2] };
        rel[0] = d[0] * right[0] + d[2] * right[2]; rel[1] = d[1]; rel[2] = d[0] * f[0] + d[2] * f[2];
        const float rx = rel[0], ry = rel[1], rz = rel[2];
        if (rz < -0.09f && ry > -0.85f && ry < 0.35f && fabsf(rx) < 0.45f) return 2;                 // behind the back / over the shoulder
        if (ry > -0.42f && ry < -0.05f && fabsf(rx) < 0.20f && rz >= -0.09f && rz < 0.30f) return 3;   // on the chest
        if (ry > -0.85f && ry < -0.28f && rx < -0.10f && rx > -0.55f && rz >= -0.09f && rz < 0.22f) return 1;   // the left hip
        return 0;
    }
    bool g_HolsterGrip = false;                   //!< this squeeze of the left grip drew a weapon: it does nothing else
    int g_HolsterZoneWas = 0;
    double g_HolsterLogAt = -10;
    void Hold(Slot s, bool want)
    {
        if (g_TapUntil[s] > 0) { if (want) g_TapUntil[s] = 0; else return; }   // a tap in flight ends on its own
        if (g_Held[s] == want) return;
        // A key must stay down for at least kMinDown. The game samples its keys once per simulation tick, and a quick
        // tap of a Touch button can be over in 40-50 ms: measured on the mock, 40 ms presses of A were missed by the
        // menus about half the time. A release that comes too early is deferred, as a tap.
        if (!want && g_Now - g_DownAt[s] < kMinDown) { g_TapUntil[s] = g_DownAt[s] + kMinDown; return; }
        g_Held[s] = want; SendKey(s, want);
        if (want) g_DownAt[s] = g_Now; else g_UpAt[s] = g_Now;
    }
    //! Press and release. The release waits kMinDown: the game samples its keys once per simulation tick, and a key
    //! that is up again by the next tick was never pressed as far as it is concerned.
    void Tap(Slot s)
    {
        if (g_Held[s]) return;
        g_Held[s] = true; SendKey(s, true); g_DownAt[s] = g_Now; g_TapUntil[s] = g_Now + kMinDown;
    }
    void FlushTaps()
    {
        for (int i = 0; i < kCount; i++)
            if (g_TapUntil[i] > 0 && g_Now >= g_TapUntil[i]) { g_TapUntil[i] = 0; g_Held[i] = false; SendKey((Slot)i, false); g_UpAt[i] = g_Now; }
    }
    void MouseMove(int dx, int dy)
    {
        if (!dx && !dy) return;
        INPUT in{}; in.type = INPUT_MOUSE; in.mi.dx = dx; in.mi.dy = dy; in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof in);
        std::lock_guard lk(g_M); g_Stats.mouseEvents++; g_Stats.lastDx = (float)dx;
    }

    //! Analog -> digital with 8-frame pulse-width modulation (full stick = held every frame). OFF by default: at a
    //! partial deflection it released W every few frames, and the character stuttered -- worst when crouched or
    //! sprinting (a released W drops the sprint). Reported from the headset as "crouched it goes absurdly slow and
    //! in jerks, slower than the base game".
    bool Pwm(float v)
    {
        float a = fabsf(v);
        if (a < 0.25f) return false;
        if (a > 0.9f) return true;
        int duty = (int)(a * 8.f + 0.5f);
        return (int)(g_Frame % 8) < duty;
    }
    //! The left stick as eight directions, like the game's own keyboard: past 0.35 of deflection (released below
    //! 0.25) the keys of the nearest of 8 directions are held steadily. Order W S D A.
    void MoveKeys(float x, float y, bool out[4])
    {
        const float m = sqrtf(x * x + y * y);
        const bool any = out[0] || out[1] || out[2] || out[3];
        if (m < (any ? 0.25f : 0.35f)) { out[0] = out[1] = out[2] = out[3] = false; return; }
        const float s = 0.3827f * m;                  // sin 22.5 deg: the edge between a pure and a diagonal sector
        out[0] = y > s; out[1] = y < -s; out[2] = x > s; out[3] = x < -s;
    }

    //! Y / X as the four quick slots (see g_QuickBtn). `assign`: the wheel is open, so the key assigns instead of equips.
    void QuickButtons(unsigned live, unsigned pressed, bool assign)
    {
        for (QuickBtn& q : g_QuickBtn)
        {
            const bool on = (live & q.bit) != 0;
            if (on && !q.down && (pressed & q.bit)) { q.down = true; q.longSent = false; q.downAt = g_Now; }
            if (!q.down) continue;
            Slot send = kCount;
            if (on && !q.longSent && g_Now - q.downAt >= g_QuickHold.load())
            {
                send = q.longSlot; q.longSent = true;
                VRRuntime::Vibrate(0, 0.6f, 0.05f);
            }
            else if (!on)
            {
                if (!q.longSent) send = q.shortSlot;
                q.down = false;
            }
            if (send == kCount) continue;
            Tap(send);
            if (assign) g_WheelAssigned = true;
            const int n = send - kQuick1;
            g_Count.quick[n]++;
            LOGI("KbmInject: {} {} -> quick slot {} (key {}){}", q.bit == kXRBtnY ? "Y" : "X", send == q.longSlot ? "held" : "short",
                 n + 1, g_Bind[send].name, assign ? ": ASSIGNS the item highlighted in the wheel" : "");
        }
    }
    void QuickReset() { for (QuickBtn& q : g_QuickBtn) q.down = false; }
    //! The Index layout's quick slots: a press on the left pad's edge is that slot at once (no short / held: four
    //! directions are four slots). `assign`: the wheel is open, so it assigns the highlighted item instead.
    void PadQuick(unsigned pressed, bool assign)
    {
        static const struct { unsigned bit; Slot slot; const char* name; } kPad[4] = {
            { kVPadUp, kQuick1, "up" }, { kVPadRight, kQuick2, "right" }, { kVPadDown, kQuick3, "down" }, { kVPadLeft, kQuick4, "left" } };
        for (const auto& p : kPad)
        {
            if (!(pressed & p.bit)) continue;
            Tap(p.slot);
            if (assign) g_WheelAssigned = true;
            const int n = p.slot - kQuick1;
            g_Count.quick[n]++;
            LOGI("KbmInject: left trackpad {} -> quick slot {} (key {}){}", p.name, n + 1, g_Bind[p.slot].name, assign ? ": ASSIGNS the item highlighted in the wheel" : "");
        }
    }
    //! The Index layout's potions: the LEFT hand's B (upper) health, A (lower) mana -- the wheel's B / A on the right,
    //! without the wheel. (On an Index the left A / B arrive as X / Y.)
    void PadPotions(unsigned pressed)
    {
        if (pressed & kXRBtnY) { Tap(kR); g_Count.potions[0]++; LOGI("KbmInject: health potion (left B)"); }
        if (pressed & kXRBtnX) { Tap(kT); g_Count.potions[1]++; LOGI("KbmInject: mana potion (left A)"); }
    }
    //! Menu button (play and cutscenes): a short press opens the journal (J on release), held 0.6 s it pauses (ESC, with
    //! a buzz). Inside any menu the button is ESC at once (Tick), so the same short press closes the journal again.
    //! (`vr quickslots 0`: the old way round, short = pause, held = journal.)
    void MenuButton(unsigned live, bool quick)
    {
        if (live & kXRBtnMenu)
        {
            if (g_MenuBtnDown < 0) { g_MenuBtnDown = g_Now; g_PauseSent = false; }
            else if (!g_PauseSent && g_Now - g_MenuBtnDown > 0.6)
            {
                Tap(quick ? kEsc : kJ); g_PauseSent = true;
                ScreenMode::Expect(quick ? "pause" : "journal");   // the menu screen comes up with it, not ~10 frames later
                if (quick) { VRRuntime::Vibrate(0, 0.6f, 0.05f); g_Count.menuPause++; LOGI("KbmInject: menu button held: pause"); }
            }
        }
        else if (g_MenuBtnDown >= 0)
        {
            if (!g_PauseSent) { Tap(quick ? kJ : kEsc); ScreenMode::Expect(quick ? "journal" : "pause"); if (quick) { g_Count.menuJournal++; LOGI("KbmInject: menu button: journal"); } }
            g_MenuBtnDown = -1;
        }
    }
}

namespace
{
    //! Head aim: the servo that turns the game to the head, the stick's turns, the calibration, and the mouse counts
    //! that carry them. Every mode that plays (play, the wheel, a cutscene) ends here; a menu does not.
    void HeadAimServo(const XRInputState& in)
    {
        // Head aim: make the game's look angles follow the HMD (yaw & pitch) via mouse deltas, so the
        // crosshair / weapons aim where the player looks. ViewMatrixHook then keeps the game axis.
        float bodyYaw = 0; bool haveBody = false; float headYawNow = 0; bool haveHeadYaw = false;
        {
            float po[3], pa[9];
            if (Game::PlayerView(po, pa)) { bodyYaw = atan2f(pa[1], pa[0]) * 57.29578f; haveBody = true; }
            // In a host the player entity rides along inside it and its yaw stays frozen (measured 90.5 for the whole
            // possession); what the mouse turns is the host's view, i.e. the camera. Servo on that instead.
            // In a slide too: the mouse turns the camera there and the body only follows it (measured: a 40 deg head
            // turn gave the body 40 and the camera 75 while the servo watched the body, and the slide ended with the
            // body snapped 35 deg onto the camera). The two are equal when the slide starts and when it ends.
            if (g_Possessing || PlayerState::ServoOnCamera())
            {
                float co[3], ca[9];
                if (HandAimHook::RealCamera(co, ca)) { bodyYaw = atan2f(ca[1], ca[0]) * 57.29578f; haveBody = true; }
                else haveBody = false;
            }
            static float lastBodyYaw = 0.f; static bool haveLastYaw = false;   // same guard: player+0x30C is swapped too
            if (g_PossessionEdge)
            {   // the yaw source changed (body <-> camera, and the camera jumps into / out of the host): start over from it
                g_PossessionEdge = false; haveLastYaw = false; g_ServoValid = false; g_HeadValid = false;
            }
            // The guard rejects a yaw that jumped more than 25 deg in a frame, because the power's CamSwap windows
            // substitute player+0x300/+0x30C and this reads them mid-flight. But rejecting a tick used to ALSO skip the
            // refresh below, so one stale reference latched the guard shut for good: every later tick was >25 deg from a
            // yaw that no longer existed, haveBody stayed false and the servo never ran again. Reported from hardware as
            // "sometimes turning my head moved the view and sometimes it did nothing", with yawerr frozen while ticks
            // rose. A real jump lasts one tick (the swap window is inside one call); a reference that keeps disagreeing
            // for kStaleTicks in a row is the stale one, so believe the game and re-seed from it.
            static int rejected = 0;
            constexpr int kStaleTicks = 30;
            if (haveBody && haveLastYaw)
            {
                float d = bodyYaw - lastBodyYaw; while (d > 180) d -= 360; while (d < -180) d += 360;
                // Not while a takedown / cutscene view is on, or handing back: then a jump is the game's own (a cutscene
                // turns the body 30 deg in one tick as it ends, measured on the shrine's second one), and the view is
                // waiting for the servo to bring the game to it -- rejecting the jump stalled the servo 30 ticks and
                // the view glided instead. No power's CamSwap window runs in an animation.
                if (fabsf(d) > 25.f && ViewMatrixHook::AnimViewActive() && !g_Possessing)
                {
                    static int logged = 0;
                    if (logged++ < 20) LOGI("KbmInject: body yaw jumped {:.1f} -> {:.1f} deg during an animation view: the game's, taken", lastBodyYaw, bodyYaw);
                }
                else if (fabsf(d) > 25.f)
                {
                    haveBody = false;
                    if (++rejected >= kStaleTicks)
                    {   // the stored yaw is the wrong one: adopt what the game reports and let the servo re-seed
                        LOGI("KbmInject: body yaw reference was stale ({:.1f} -> {:.1f} deg for {} ticks): re-seeding", lastBodyYaw, bodyYaw, rejected);
                        lastBodyYaw = bodyYaw; haveLastYaw = true; rejected = 0;
                        g_ServoValid = false; g_HeadValid = false;
                    }
                }
                else rejected = 0;
            }
            if (haveBody) { lastBodyYaw = bodyYaw; haveLastYaw = true; rejected = 0; }
        }
        // The player's view axis is flattened, so the actual PITCH has to come from the camera the weapons read
        // (cam+0xFC). Its forward's z is sin(pitch), positive up -- the same convention as the HMD pitch below.
        float camPitch = 0; bool havePitch = false;
        if (g_HeadAim.load())
        {
            float co[3], ca[9];
            if (HandAimHook::RealCamera(co, ca))
            { float z = ca[2] > 1.f ? 1.f : (ca[2] < -1.f ? -1.f : ca[2]); camPitch = asinf(z) * 57.29578f; havePitch = true; }
            // A camera that pitched more than 20 deg in one frame is not the camera, it is a hooked call's swapped-in
            // controller pose caught mid-flight on another thread. Skip the servo for this tick rather than chase it.
            // The same latch the yaw guard had (see above), and it bit twice in the mock. A save load resets the
            // camera pitch (measured -12.6 -> 16.8, and once a view left at -80 until something else moved it), and in
            // one run the game's pitch went 0.1 -> -50.7 in one frame after a choke (next to a 0.56 s capture stall;
            // not reproduced without captures, cause unknown) and stayed. Either way every later tick was >20 deg from
            // the stored reference, the pitch servo never ran again and the view sat at that angle for good. A swap
            // window lasts one tick; a pitch that disagrees for kStalePitchTicks in a row is the camera, so adopt it.
            static float lastCamPitch = 0.f; static bool haveLast = false; static int pitchRejected = 0;
            constexpr int kStalePitchTicks = 30;
            // (not while a takedown / cutscene view is on: an animated camera pitches 20+ deg in a frame for real, and the
            // view does not take the game's pitch then anyway)
            if (havePitch && haveLast && fabsf(camPitch - lastCamPitch) > 20.f && ViewMatrixHook::AnimViewActive()) { lastCamPitch = camPitch; pitchRejected = 0; }
            else if (havePitch && haveLast && fabsf(camPitch - lastCamPitch) > 20.f)
            {
                havePitch = false;
                if (++pitchRejected >= kStalePitchTicks)
                {
                    LOGI("KbmInject: camera pitch reference was stale ({:.1f} -> {:.1f} deg for {} ticks): re-seeding", lastCamPitch, camPitch, pitchRejected);
                    lastCamPitch = camPitch; pitchRejected = 0; havePitch = true;
                }
            }
            else if (havePitch) { lastCamPitch = camPitch; haveLast = true; pitchRejected = 0; }
        }
        if (g_HeadAim.load())
        {
            // the head where it will be when the view built from the mouse sent now is shown (vr predictservo)
            VRRuntime::HeadPose hp = VRRuntime::GetServoHeadPose();
            if (hp.valid)
            {
                // XR quaternion -> yaw (about +Y, positive = left) and pitch (about +X, positive = up)
                float x = hp.quat[0], y = hp.quat[1], z = hp.quat[2], w = hp.quat[3];
                float fwdX = -(2 * (x * z + y * w)), fwdY = -(2 * (y * z - x * w)), fwdZ = -(1 - 2 * (x * x + y * y)); // R*(0,0,-1)
                float yaw = atan2f(-fwdX, -fwdZ) * 57.29578f;           // 0 = facing -Z
                headYawNow = yaw; haveHeadYaw = true;
                float pitch = asinf(fwdY < -1 ? -1 : (fwdY > 1 ? 1 : fwdY)) * 57.29578f;
                // A recentre turns the whole space by the head's yaw at that instant: the pose jumps without the head
                // moving. Accumulated as a head turn it turned the game -- measured 12 deg of a 40 deg recentre, the part
                // the servo managed to inject before the request to re-seed (a Present later) arrived. The generation
                // travels WITH the pose, so the jump is recognised on the very tick it shows up.
                static unsigned lastGen = 0;
                const bool recentred = hp.gen != lastGen;
                if (recentred && g_HeadValid) LOGI("KbmInject: recentre (generation {}): head yaw {:.1f} -> {:.1f} deg taken as the new reference, not a turn", hp.gen, g_LastHeadYaw, yaw);
                lastGen = hp.gen;
                if (g_HeadValid && g_ServoValid)
                {
                    float dyaw = yaw - g_LastHeadYaw; if (dyaw > 180) dyaw -= 360; if (dyaw < -180) dyaw += 360;
                    if (!recentred) g_DesiredYaw += dyaw;   // where the body should be facing; the servo below gets it there
                }
                else if (haveBody && havePitch) { g_DesiredYaw = bodyYaw; g_ServoValid = true; }
                // Pitch is ABSOLUTE, not accumulated: your head's pitch is a physical angle against gravity, so the
                // game's view pitch should simply BE it. Seeding a relative target from the camera's pitch at arm time
                // would keep the very fault this is fixing -- where the view sat vertically depended on how high or low
                // the player happened to be looking when the mode came on.
                g_DesiredPitch = pitch > kPitchLimit ? kPitchLimit : (pitch < -kPitchLimit ? -kPitchLimit : pitch);
                (void)0;
                g_LastHeadYaw = yaw; g_LastHeadPitch = pitch; g_HeadValid = true;
            }
        }
        else { g_HeadValid = false; g_ServoValid = false; }
        // While the takedown / cutscene view is on, the target IS that view: the game is to face where the view looks.
        // Accumulating the head's turns gave the same thing until the servo re-seeded on a body jump, and then the two
        // parted for good. One mapping, owned by the view (ViewMatrixHook::AnimYawOffset).
        {
            float off = 0.f;
            if (g_HeadAim.load() && g_HeadValid && ViewMatrixHook::AnimYawOffset(off))
            { g_DesiredYaw = g_LastHeadYaw + off; g_ServoValid = true; }
        }

        // A takedown owns the camera: its animation ignores the mouse (measured: the pitch servo injected against it for
        // the whole choke and the camera never moved). Inject nothing and do not bank a stick turn either -- the view
        // follows the head meanwhile (ViewMatrixHook's takedown view), and the head's turns keep accumulating into the
        // target above, so when the animation hands the camera back the servo turns the game to where the head is.
        // Only a takeover: a Slash (view-only) accepts the mouse, and the body must keep tracking the head through it.
        // A cutscene likewise injects nothing (its camera ignores the mouse, measured) but the stick still turns: its
        // turns go into the view (StickTurnTotal), which is how a seated player looks round at the scene.
        const bool cine = PlayerState::Cinematic();
        const bool anim = PlayerState::AnimHoldServo();    // a takedown, a cutscene, a hard landing (PlayerState kHold)
        const bool servo = g_HeadAim.load() && g_ServoValid && haveBody && g_Simulating;
        float snap = g_SnapDeg.load();
        if (anim && !cine) { if (fabsf(in.turnX) < 0.3f) g_SnapArmed = true; }
        else if (snap <= 0.f)
        {   // Smooth turn in degrees per SECOND (it used to be 0.5 deg per frame, so it turned at whatever the frame rate
            // was). A 0.25 dead zone, then linear, so a thumb resting on the stick or a push meant for a potion does not
            // turn the view.
            const float ax = fabsf(in.turnX);
            const float t = ax < 0.25f ? 0.f : (in.turnX > 0 ? 1.f : -1.f) * (ax - 0.25f) / 0.75f;
            const float deg = t * g_TurnSpeed.load() * (float)g_Dt;
            if (servo) { g_DesiredYaw += -deg; g_StickTurn = g_StickTurn.load() - deg; }
            else g_YawAccum += deg * CountsPerDeg();
        }
        else
        {
            if (g_SnapArmed && fabsf(in.turnX) > 0.6f)
            {
                g_SnapArmed = false;
                const float dir = in.turnX > 0 ? 1.f : -1.f;
                if (servo) { g_DesiredYaw += -dir * snap; g_StickTurn = g_StickTurn.load() - dir * snap; SnapBlankStart(); }
                else { g_YawAccum += dir * snap * CountsPerDeg(); SnapBlankStart(); SnapBlankSettled(4); }
            }
            else if (fabsf(in.turnX) < 0.3f) g_SnapArmed = true;
        }
        // Once per GAME tick. The error is measured against the game's angles, and those only move when the game ticks;
        // a present with no tick in between (a hitch, the capture of a screenshot) used to send the same correction a
        // second time, and the game then took both at once. Measured in a possessed host: pitch -0.3 -> +14.4 -> -77 in
        // three frames, one of them without a tick, right at a capture.
        static unsigned long long lastServoAim = ~0ull;
        const unsigned long long aimNow = HandAimHook::AimTicks();
        const bool freshTick = aimNow != lastServoAim;
        // Synchronised stereo holds every second game frame (StereoSync); mouse motion that a held frame reads is lost,
        // so the correction waits for the Present before a frame that runs (the angles are still this tick's).
        const bool canSend = StereoSync::NextFrameRuns();
        if (servo && (!freshTick || !canSend)) { g_YawAccum = 0; g_PitchAccum = 0; }
        else if (servo)
        {   // Close the loop: inject whatever is still missing to reach the target, every frame. Overshoot from a bad
            // counts/degree is corrected next frame instead of being kept.
            lastServoAim = aimNow;
            float err = g_DesiredYaw - bodyYaw;
            while (err > 180.f) err -= 360.f;
            while (err < -180.f) err += 360.f;
            g_YawAccum = -err * CountsPerDeg() * kYawGain;   // mouse +dx turns right; game yaw + is left
            // the game faces the snap's heading: the views built from here on show it (+1: the view of the state just read
            // may not be built yet -- the game thread runs ahead of this Present)
            if (fabsf(err) < kSnapSettledDeg) SnapBlankSettled(1);
            float perr = 0.f;
            // Not in a host: its camera pitch is animated (measured: -27 deg in one frame when the host stops walking,
            // then drifting back), so the servo only fought it, and nothing needs it there -- the view takes the head's
            // pitch (ViewMatrixHook) and powers aim with the hand.
            if (g_Possessing) g_PitchAccum = 0;
            else if (havePitch)
            {   // Same treatment for the up/down angle, and for the same reason: fed forward through counts/degree it
                // was left with whatever offset the mouse scale happened to produce, so where the view sat vertically
                // depended on how high or low the player was looking when the mode armed - reported from hardware.
                perr = g_DesiredPitch - camPitch;
                g_PitchAccum = -perr * CountsPerDeg() * kYawGain;   // mouse +dy looks down; game pitch + is up
            }
            std::lock_guard lk(g_M); g_Stats.yawError = err; g_Stats.pitchError = perr;
            g_Stats.bodyYaw = bodyYaw; if (haveHeadYaw) g_Stats.headYaw = headYawNow;
        }
        if (anim) { g_YawAccum = 0; g_PitchAccum = 0; }

        // Auto-calibration: frame 0 read yaw, inject kCalCounts; frame 4 read yaw again.
        if (g_CalState == 1 && g_Simulating)
        {
            float o[3], a[9];
            if (g_CalDelay > 0) g_CalDelay--;
            else if (Game::PlayerView(o, a)) { g_CalYaw0 = atan2f(a[1], a[0]) * 57.29578f; g_YawAccum += kCalCounts; g_CalState = 2; g_CalFrames = 0; }
            else if (--g_CalTries <= 0) { LOGI("KbmInject: calibration gave up - no player view (still in a menu?)"); g_CalState = 0; }
            else g_CalDelay = kCalRetryFrames;
        }
        else if (g_CalState == 2 && g_Simulating && ++g_CalFrames >= 4)
        {
            float o[3], a[9];
            bool done = false;
            if (Game::PlayerView(o, a))
            {
                float d = atan2f(a[1], a[0]) * 57.29578f - g_CalYaw0; if (d > 180) d -= 360; if (d < -180) d += 360;
                if (fabsf(d) > 0.5f)
                {
                    g_CountsPerDeg = kCalCounts / fabsf(d) / g_SensScale;   // stored at the reference sensitivity
                    LOGI("KbmInject: calibration {} counts -> {:.2f} deg => {:.2f} counts/deg ({:.2f} at sensitivity {})", kCalCounts, d, CountsPerDeg(), g_CountsPerDeg.load(), kRefSensitivity);
                    done = true;
                }
            }
            // A frozen yaw means the game was not accepting input yet (menu, load, cutscene), not a bad measurement.
            if (done || --g_CalTries <= 0) { if (!done) LOGI("KbmInject: calibration gave up - the yaw never moved"); g_CalState = 0; }
            else { g_CalState = 1; g_CalDelay = kCalRetryFrames; }
        }

        // Flush the accumulators, at most kMaxCountsPerFrame per axis per frame (engine clamps big deltas).
        if (StereoSync::NextFrameRuns())
        {
            // The cap is ~12 degrees a frame, whatever the sensitivity: kMaxCountsPerFrame was measured at the reference
            // sensitivity, and as raw counts it let a sensitivity of 3.0 turn the game 28 deg in one frame -- more than the
            // yaw guard above believes (25), so the tick was rejected, the servo stalled and re-seeded (measured).
            const int cap = std::max(8, (int)(kMaxCountsPerFrame * g_SensScale));
            int dx = (int)g_YawAccum, dy = (int)g_PitchAccum;
            if (dx > cap) dx = cap; if (dx < -cap) dx = -cap;
            if (dy > cap) dy = cap; if (dy < -cap) dy = -cap;
            g_YawAccum -= dx; g_PitchAccum -= dy;
            MouseMove(dx, g_InvertY ? -dy : dy);
        }
    }
}

namespace
{
    //! The controllers' input stopped while the GAME runs on (the runtime stalled or took the session away, the development notes
    //! part 51): every key and button the mod holds goes up, as on the way into a menu, and the buttons the controllers
    //! hold when the input comes back are ignored until released (nothing fires twice); the sticks simply resume. A
    //! stick held at that moment used to stay held in the game for as long as VR was gone (the player walked on).
    //! Not after a hitch of the game's own: the render thread was stuck too, the input is fresh on the next frame
    //! (a corpse carried with the grip must not drop on a 0.6 s hitch) -- so 0.3 s of the game TICKING with no input.
    bool g_InputLost = false, g_HadInput = false;
    double g_NoInputSince = -1;
    void InputLost()
    {
        if (g_InputLost) return;
        g_InputLost = true;
        int held = 0;
        for (int i = 0; i < kCount; i++) if (g_Held[i]) { g_TapUntil[i] = 0; g_Held[i] = false; SendKey((Slot)i, false); held++; }
        g_Latched = ~0u; g_SprintLatch = false; g_MenuDir = -1; g_MenuBtnDown = -1; g_PendingStrike = -1;
        QuickReset();
        g_WheelCloseAt = -1;
        g_MoveKeys[0] = g_MoveKeys[1] = g_MoveKeys[2] = g_MoveKeys[3] = false;
        g_YawAccum = g_PitchAccum = 0;
        { std::lock_guard lk(g_M); g_Stats.heldMask = 0; }
        LOGI("KbmInject: the controllers' input stopped (no OpenXR frame): {} held key(s) released, the controllers latched until released", held);
    }
}

void KbmInject::Tick()
{
    g_Frame++;
    {
        static LARGE_INTEGER fq{}, last{}; LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (!fq.QuadPart) { QueryPerformanceFrequency(&fq); last = now; }
        const double dt = double(now.QuadPart - last.QuadPart) / fq.QuadPart; last = now;
        g_Dt = dt < 0.001 ? 0.001 : (dt > 0.1 ? 0.1 : dt);
        g_Now = double(now.QuadPart) / fq.QuadPart;
    }
    FlushTaps();
    {   // vr tapkey (SendBind takes g_M itself)
        KeyBinds::Bind b; int send = 0;
        {
            std::lock_guard lk(g_M);
            if (g_TestPending) { g_TestPending = false; b = g_TestBind; send = 1; g_TestUp = g_Now + g_TestMs / 1000.0; }
            else if (g_TestUp >= 0 && g_Now >= g_TestUp) { g_TestUp = -1; b = g_TestBind; send = 2; }
        }
        if (send) { SendBind(b, send == 1); LOGI("KbmInject: test key {} {}", b.name, send == 1 ? "down" : "up"); }
    }
    { std::lock_guard lk(g_M); g_Stats.ticks++; }
    XRInputState in;
    if (!g_Enabled.load()) { ReleaseAll(); return; }
    if (!VRRuntime::GetInput(in))
    {
        if (g_HadInput) { if (g_NoInputSince < 0) g_NoInputSince = g_Now; else if (g_Now - g_NoInputSince > 0.3) InputLost(); }
        return;
    }
    g_HadInput = true; g_NoInputSince = -1;
    if (g_InputLost) { g_InputLost = false; LOGI("KbmInject: the controllers' input is back"); }
    {   // the keys the game expects: read once, then again whenever the game rewrites its config (the options menu)
        static double nextPoll = 0;
        if (g_Now >= nextPoll) { nextPoll = g_Now + 2.0; KeyBinds::Poll(); }
        const unsigned gen = KeyBinds::Generation();
        if (gen != g_BindGen) { g_BindGen = gen; Rebind(); }
    }
    // Only inject when the game window owns the foreground (SendInput goes to the active window).
    HWND fg = GetForegroundWindow(); DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    { std::lock_guard lk(g_M); g_Stats.inputTicks++; if (fabsf(in.turnX) > g_Stats.maxTurnX) g_Stats.maxTurnX = fabsf(in.turnX); }
    const bool inFront = pid == GetCurrentProcessId();
    // Behind another window: bring the game to the front when the player shows they are playing (BringGameToFront).
    // The game pauses itself when it loses the front; if it was being PLAYED then, that pause is closed again once it is
    // back (ESC on its pause menu), so the player is not left in a menu they did not open. The press that brought it
    // back is latched: it must not also fire in the game (a trigger = an attack, B = ESC).
    static bool wasBehind = false, playWhenLost = false;
    static double unpauseFrom = -1, unpauseUntil = -1;
    {
        static int lastState = 0; static double nextTry = 0;
        const int st = VRRuntime::GetStatus().state;
        const bool headsetOn = st == 5 && lastState != 5;   // XR_SESSION_STATE_FOCUSED: the headset was put on
        lastState = st;
        const bool active = in.buttons != 0 || in.triggerL > 0.5f || in.triggerR > 0.5f || in.gripPressL > g_GripOn[0].load() || in.gripPressR > g_GripOn[1].load()
                         || fabsf(in.moveX) > 0.6f || fabsf(in.moveY) > 0.6f || fabsf(in.turnX) > 0.6f || fabsf(in.turnY) > 0.6f;
        if (!inFront && g_AutoFocus.load() && (headsetOn || (active && g_Now >= nextTry)))
        {
            nextTry = g_Now + 2.0;
            BringGameToFront(headsetOn ? "the headset was put on" : "a controller was used");
        }
    }
    if (!inFront)
    {
        if (!wasBehind) { wasBehind = true; playWhenLost = g_Mode == kModePlay && g_Simulating; unpauseUntil = -1; }
        ReleaseAll(); return;
    }
    if (wasBehind)
    {
        wasBehind = false; g_Latched = ~0u;
        if (playWhenLost && g_AutoFocus.load()) { unpauseFrom = g_Now + 0.4; unpauseUntil = g_Now + 2.5; }
    }
    { std::lock_guard lk(g_M); g_Stats.fgTicks++; }
    ReadMouseSettings();

    // Is the game actually simulating? The engine's player-camera aim update stops dead in the pause menu, in a
    // loading screen and during a cutscene (measured: frozen at 17453 calls for as long as the pause menu was up,
    // while Present kept running at full rate). That matters because the head-aim servo compares the HMD's yaw
    // against the game's, and while the game's cannot move the error just grows: with the head turned 50 deg in the
    // pause menu it sat at yawerr 50 and injected -300 mouse counts EVERY frame, indefinitely. Those counts go into
    // a menu, where the mouse is what picks the highlighted entry -- so "pause, load a game" turns into "pause, and
    // whatever the cursor lands on". Freeze the servo instead, and re-seed it from the body yaw on the way back so
    // the head turns made during the menu are not banked and paid out as one violent snap when play resumes.
    {
        static unsigned long long lastAim = 0;
        static int idleFrames = 0;
        const unsigned long long aim = HandAimHook::AimTicks();
        if (aim == lastAim) idleFrames++; else { idleFrames = 0; lastAim = aim; }
        g_Simulating = idleFrames < 10;     // ~10 presents of a frozen player update; a hitch must not trip it
        std::lock_guard lk(g_M); g_Stats.simulating = g_Simulating;
    }
    // Not through the power wheel, though: with it up the player ticks every ~0.17 s, so "simulating" flaps, and each
    // re-seed moved the servo's room anchor by the head's turn -- the world turned with the head while the wheel was up
    //. The wheel's view follows the head on its own (ViewMatrixHook) and the target keeps the head's
    // turns, so when the wheel closes the servo turns the game to where the head is.
    const bool wheelUp = g_Mode == kModeWheel || (g_Mode == kModePlay && PlayerState::Running("Powerwheel"));
    if (!g_Simulating && !wheelUp)
    {
        g_HeadValid = false; g_ServoValid = false;      // reseed from the body yaw when the game comes back
        g_YawAccum = 0; g_PitchAccum = 0;
    }
    else if (!g_Simulating) { g_YawAccum = 0; g_PitchAccum = 0; }
    // Hands off the menus. Everything below this point synthesizes input for a PLAYER; a menu reads the same events
    // as clicks and cursor moves on a screen where one wrong click overwrites a save. The menu button is the single
    // exception: it is how you got here and how you leave, and ESC cannot select anything.
    //
    // "Not simulating" alone does not find every menu. Measured at the MAIN menu: the engine's player-camera aim
    // update keeps ticking there (sim 1), so the pitch servo ran against it and dumped 9779 mouse counts into the
    // menu -- which is what moves the highlighted entry, and why an ENTER meant for "Continue" landed elsewhere.
    // The system mouse cursor was tried as the extra signal and measured USELESS: Dishonored 2 draws its own
    // cursor and keeps the system one hidden, so CURSOR_SHOWING is 0 on the main menu too.
    // What does separate them is a POSITIVE signal that the player is playing: the first-person draws. Gameplay
    // draws ~10 per arm plus the equipment; a menu, a loading screen and a cutscene draw none. -1 means nothing is
    // counting them (the hand/6DOF subsystems are off), and then only the simulating test applies.
    const int fpDraws = GpuTrace::FirstPersonDraws();
    // Possession: inside a host there are no first-person draws at all (Corvo's arms and weapons are gone), which is
    // exactly what a menu looks like -- measured, the whole possession ran as "menu", nothing was injected and the host
    // could neither walk nor turn. The player's state machine says when it starts (EnterPossessedHost runs for the
    // leap in); it ends when the arms are back for a moment, on death, or with a new player (a load).
    {
        static void* possPlayer = nullptr; static int armsBack = 0;
        void* player = Game::LocalPlayer();
        const bool entering = PlayerState::Running("EnterPossessedHost");
        if (entering && !g_Possessing) { g_Possessing = true; possPlayer = player; armsBack = 0; g_PossessionEdge = true; LOGI("KbmInject: possession begins (fpdraws {})", fpDraws); }
        if (g_Possessing && !entering)
        {
            const char* why = player != possPlayer ? "new player" : PlayerState::Running("Dead") ? "dead" : nullptr;
            if (!why && fpDraws > 0 && ++armsBack >= 10) why = "arms back";
            else if (fpDraws <= 0) armsBack = 0;
            if (why) { g_Possessing = false; g_PossessionEdge = true; LOGI("KbmInject: possession ends ({})", why); }
        }
        std::lock_guard lk(g_M); g_Stats.possessing = g_Possessing;
    }
    // Before the 6DOF package is on, nothing counts the first-person draws (-1). Then "is anybody playing" comes from
    // the player's state machine: at the main menu and on a loading screen no player state runs at all.
    // Before the package is on (fpDraws -1) nothing proves a player is playing -- the main menu runs a player state
    // behind it (measured: Running + Slipping at the title screen) -- so it is a menu until the draws say otherwise.
    // Shadow Walk likewise: its claws are not the player's arms and are no longer taken for them (GpuTrace's arm
    // analysis is held off while the arms are the game's: it learned the claws as the skin), so it draws no
    // first-person draws either -- measured, it read as "menu", the stick did not walk and B (crouch) paused the game.
    // Swimming at the surface too: the game hides the arms there (a stroke shows them for a frame or two), so it read
    // as "menu" -- measured on the 07/10 18:36 save, fpdraws 0 with Swimming the only state running: the servo and the
    // controllers stopped, the picture froze in the room and the mode flapped menu <-> play with every stroke.
    // And a cutscene: some draw the arms (the Outsider's shrine: fpdraws 29) and some may not; either way it is in the
    // world and in front of the eyes, not a picture for the menu screen.
    const bool armlessPlay = PlayerState::Running("ShadowWalk") || PlayerState::Running("ShadowHide")
                          || PlayerState::Running("Swimming") || PlayerState::Running("SwimMantle") || PlayerState::Cinematic();
    // A menu over the running world (see g_Scene): a menu whatever the draws and the simulation say. The state's tick
    // counter freezes with the game, so its own dialogs ("Play as Emily Kaldwin? No / Yes", a purchase) keep it.
    // vr playrule 1: the list above is a list of the cases found so far -- the tutorial's chain climb
    // (Climb with no arms drawn) was not on it and played as a menu (flat screen, mono, no controls). The general rule:
    // once THIS player has been seen playing (its first-person draws for 10 Presents, or any state but the title
    // screen's Running + Slipping for 15 while simulating), no arms is play -- except in the states that are a picture
    // rather than play (death, the kill camera, a note, the journal). Main menu and loading never latch (no player
    // state runs: RunningCount() 0); a new player (a load) starts unlatched. The title screen runs Running + Slipping
    // and its camera is a narrow 20 deg: the state route is not taken below 30 deg of vertical fov.
    bool uiState = false;
    {
        static void* latchPlayer = nullptr; static int fpRun = 0, stateRun = 0, noStates = 0;
        void* player = Game::LocalPlayer();
        if (player != latchPlayer)
        {
            if (g_PlayLatched) LOGI("KbmInject: play latch off (a new player)");
            latchPlayer = player; g_PlayLatched = false; fpRun = stateRun = 0;
        }
        if (PlayerState::RunningCount() == 0) { if (++noStates >= 5 && g_PlayLatched) { g_PlayLatched = false; LOGI("KbmInject: play latch off (no player state runs: a menu or a load)"); } }
        else noStates = 0;
        if (g_Simulating && !g_PlayLatched && player)
        {
            static const char* const kTitle[] = { "Running", "Slipping" };
            float fx = 0, fy = 0; CameraHook::GameFov(fx, fy);
            fpRun = fpDraws > 0 ? fpRun + 1 : 0;
            stateRun = fy >= 30.f && PlayerState::RunningOutside(kTitle, 2) ? stateRun + 1 : 0;
            if (fpRun >= 10 || stateRun >= 15)
            {
                g_PlayLatched = true;
                LOGI("KbmInject: play latch on ({}; fov {:.0f}x{:.0f}, states {})", fpRun >= 10 ? "first-person draws" : "player states", fx, fy, PlayerState::RunningNames());
            }
        }
        uiState = PlayerState::Running("Dead") || PlayerState::Running("KillCam") || PlayerState::Running("ReadNote") || PlayerState::Running("Journal");
    }
    const bool playRule = g_PlayRule.load() != 0;
    const int scene = !g_SceneMenus.load() ? kSceneNone : PlayerState::Running("CharacterChoice") ? kSceneChoice
                    : PlayerState::Running("BlackMarketInteract") ? kSceneShop : kSceneNone;
    if (scene != g_Scene)
    {
        LOGI("KbmInject: {} {}", SceneName(scene != kSceneNone ? scene : g_Scene), scene != kSceneNone ? "begins: a menu over the world (the menu screen, menu controls)" : "ends");
        g_Scene = scene;
        g_ParkCursorUntil = scene == kSceneShop ? g_Now + 0.25 : -1;
    }
    // (only the shop: at the character choice the mouse turns the camera, and there is no cursor)
    if (g_ParkCursorUntil >= 0 && g_Scene == kSceneShop) { if (g_Now < g_ParkCursorUntil) MouseMove(400, -400); else g_ParkCursorUntil = -1; }
    const bool noArmsMenu = fpDraws <= 0 && !g_Possessing && !armlessPlay && (!playRule || uiState || !g_PlayLatched);
    const bool menuNow = (!g_Simulating || noArmsMenu || scene != kSceneNone) && g_MenuInput.load() == 0;
    // Menu fallback: BOTH stick clicks together are the menu button (short = journal, held = pause, like the button).
    // Some controllers have no menu button the runtime hands us: SteamVR presents the PS VR2 Sense controllers as
    // Touch but its conversion drops the menu button (measured by another OpenXR mod), and without a menu there is
    // no pause, no save and no load. While the chord is held neither click does its own job.
    unsigned raw = in.buttons;
    // The Index layout (and an Index bound): the left pad's force -- the menu button of the other layouts, wherever it is
    // pressed -- is split by where it is pressed (PadBits). Any other controller keeps the default layout.
    const bool indexPads = g_QuickSlots.load() == 2 && in.pad;
    {
        static int was = -1;
        if (g_QuickSlots.load() == 2 && (int)in.pad != was)
        {
            was = in.pad ? 1 : 0;
            LOGI("KbmInject: Index layout {}", in.pad ? "on: right trigger = interact, right trackpad = sword, left trackpad = quick slots (edges) and menu (middle), left B / A = health / mana potion"
                                                      : "asked for, but no trackpads are bound (not an Index controller): the default layout");
        }
    }
    if (indexPads) raw = (raw & ~kXRBtnMenu) | PadBits(in);
    {
        static bool chord = false;
        constexpr unsigned kBoth = kXRBtnThumbL | kXRBtnThumbR;
        if ((raw & kBoth) == kBoth) { if (!chord) { chord = true; g_SprintLatch = false; LOGI("KbmInject: both stick clicks = menu button"); } }
        else if (!(raw & kBoth)) chord = false;
        if (chord) raw = (raw & ~kBoth) | kXRBtnMenu;
    }
    // Analog inputs as buttons, so the latch covers them too.
    const unsigned btn = raw | (in.triggerL > 0.5f ? kVTrigL : 0) | (in.triggerR > 0.5f ? kVTrigR : 0)
                                    | (GripPressed(0, in.gripPressL) ? kVGripL : 0) | (GripPressed(1, in.gripPressR) ? kVGripR : 0);
    // The power wheel: held open by the L grip (MOUSE3). The game stops simulating while it is up, which on its own
    // would read as a menu -- and releasing the grip's MOUSE3 on the way into "menu" closes the wheel at once. So
    // while the grip that opened it is still held, it is the wheel, whatever the rest says.
    Mode mode = menuNow ? kModeMenu : kModePlay;
    if (g_Mode != kModeMenu && g_Held[kMMB] && (btn & kVGripL) && !(g_Latched & kVGripL))
        mode = (g_Mode == kModeWheel || !g_Simulating || PlayerState::Running("Powerwheel")) ? kModeWheel : kModePlay;
    // And while the GAME still has the wheel up, whatever the grip does: with the wheel open the player ticks only
    // every ~0.17 s (measured), so "simulating" flaps and this used to flip menu <-> play ten times a second -- where
    // play walked with the stick and menu swallowed the grip, so nothing could close the wheel any more.
    else if (fpDraws > 0 && PlayerState::Running("Powerwheel"))
        mode = kModeWheel;
    if (mode != g_Mode)
    {
        const bool viaMenu = mode == kModeMenu || g_Mode == kModeMenu;
        if (viaMenu)
        {   // hands off everything, and ignore whatever is held until it is released
            for (int i = 0; i < kCount; i++) if (g_Held[i]) { g_TapUntil[i] = 0; g_Held[i] = false; SendKey((Slot)i, false); }
            g_Latched = btn; g_SprintLatch = false; g_MenuDir = -1; g_MenuBtnDown = -1; g_PendingStrike = -1;
            QuickReset();      // a Y / X held into a menu is latched: its "release" must not go out as a short press
            g_WheelCloseAt = -1;
            g_MoveKeys[0] = g_MoveKeys[1] = g_MoveKeys[2] = g_MoveKeys[3] = false;
        }
        if (mode == kModeWheel) g_WheelAssigned = false;   // a fresh opening of the wheel
        // Leaving the wheel, the sticks are latched too until they come back to rest. One picks a power by pushing a
        // stick and letting go of the grip with the stick still pushed -- and that stick then walked: measured, the
        // movement keys went down on the very tick the wheel's MOUSE3 came up, and with a key held the game left the
        // wheel OPEN (sim flapping for good, the mod flipping menu <-> play every 0.1 s and re-pressing the keys).
        if (g_Mode == kModeWheel) g_StickLatch = true;
        LOGI("KbmInject: input mode {} -> {} (sim {} fpdraws {} states {} [{}] play latch {}{} latched 0x{:X})", ModeName(g_Mode), ModeName(mode),
             g_Simulating, fpDraws, PlayerState::RunningCount(), PlayerState::RunningNames(), g_PlayLatched ? 1 : 0, uiState ? " ui" : "", viaMenu ? btn : 0u);
        g_Mode = mode;
    }
    if (unpauseUntil >= 0)
    {   // the pause the game opened itself when it lost the front (see inFront above)
        if (g_Now > unpauseUntil || g_Simulating) unpauseUntil = -1;   // it did not pause, or is running again
        else if (g_Now >= unpauseFrom && g_Mode == kModeMenu)
        {
            Tap(kEsc); unpauseUntil = -1; g_Focus.unpaused++;
            LOGI("KbmInject: back in front: the pause the game opened when it lost the front is closed (ESC)");
        }
    }
    if (g_StickLatch)
    {
        const float m = std::max(in.moveX * in.moveX + in.moveY * in.moveY, in.turnX * in.turnX + in.turnY * in.turnY);
        if (m < 0.25f * 0.25f) { g_StickLatch = false; LOGI("KbmInject: sticks back at rest after the wheel"); }
        else in.moveX = in.moveY = in.turnX = in.turnY = 0.f;
    }
    {   // every change of the controllers' buttons, for the log (a few lines per press; a tester's best friend)
        static unsigned lastBtn = 0;
        if (btn != lastBtn) { LOGI("KbmInject: buttons 0x{:X} -> 0x{:X} ({}, latched 0x{:X})", lastBtn, btn, ModeName(g_Mode), g_Latched); lastBtn = btn; }
    }
    g_Latched &= btn;                                     // released buttons are live again
    const unsigned live = btn & ~g_Latched;
    const unsigned pressed = live & ~g_PrevBtn;           // this tick's new presses
    const unsigned released = g_PrevBtn & ~live;          // ...and releases
    g_PrevBtn = live;
    g_MenuSafe = g_Mode == kModeMenu;
    if (g_Mode != kModePlay) { SwordGesture::Reset(); SwordContact::Reset(); }
    { std::lock_guard lk(g_M); g_Stats.fpDraws = fpDraws; g_Stats.menuSafe = g_MenuSafe; g_Stats.mode = (int)g_Mode; g_Stats.sprint = g_SprintLatch; g_Stats.scene = g_Scene; g_Stats.playRule = playRule ? 1 : 0; g_Stats.playLatched = g_PlayLatched; }
    if (g_Mode == kModeMenu)
    {
        g_YawAccum = 0; g_PitchAccum = 0; g_SnapArmed = fabsf(in.turnX) < 0.3f;
        // Console navigation: either stick is the arrows (a press, then a repeat after 0.45 s every 0.12 s), A is
        // ENTER, B and the menu button are ESC. No mouse at all: a cursor move or a click is what once picked the
        // wrong save; the arrows only move the highlight.
        float sx = in.moveX, sy = in.moveY;
        if (in.turnX * in.turnX + in.turnY * in.turnY > sx * sx + sy * sy) { sx = in.turnX; sy = in.turnY; }
        const float ax = fabsf(sx), ay = fabsf(sy);
        int dir = -1;
        if (std::max(ax, ay) > (g_MenuDir >= 0 ? 0.35f : 0.6f))
            dir = ay >= ax ? (sy > 0 ? kUp : kDown) : (sx > 0 ? kRight : kLeft);
        if (dir != g_MenuDir) { g_MenuDir = dir; if (dir >= 0) { Tap((Slot)dir); g_MenuDirNext = g_Now + 0.45; } }
        if (pressed & kVPadUp) Tap(kUp);                  // (Index layout: the left trackpad's edges are arrows too)
        if (pressed & kVPadDown) Tap(kDown);
        if (pressed & kVPadLeft) Tap(kLeft);
        if (pressed & kVPadRight) Tap(kRight);
        else if (dir >= 0 && g_Now >= g_MenuDirNext) { Tap((Slot)dir); g_MenuDirNext = g_Now + 0.12; }
        // A pre-rendered video (a mission's opening, BinkVideo) is a menu here -- nothing simulates, no player state
        // runs -- but the game's prompt over it is "F Skip", the interact key held, and ENTER does nothing there: the
        // video could not be skipped from the controllers. While one plays, A or X is F, held as long
        // as the button, as in a cutscene. A button still held when the video ends is latched: the screen after it (the
        // mission's "press a key") must not take it as a press.
        const bool video = g_CineControls.load() && BinkVideo::Playing();
        static bool wasVideo = false;
        if (video != wasVideo)
        {
            wasVideo = video;
            if (video) LOGI("KbmInject: a video ({}): A / X held = skip (F)", BinkVideo::Current());
            else { g_Latched |= btn & (kXRBtnA | kXRBtnX); LOGI("KbmInject: the video is over"); }
        }
        const bool skip = video && (live & (kXRBtnA | kXRBtnX)) != 0;
        if (skip && !g_Held[kF]) { g_Count.skips++; g_Count.videoSkips++; LOGI("KbmInject: video: skip (F held with {})", (live & kXRBtnA) ? "A" : "X"); }
        Hold(kF, skip);
        Hold(kEnter, !video && (live & kXRBtnA) != 0);
        Hold(kEsc, (live & (kXRBtnB | kXRBtnMenu)) != 0);
        // Tabs, like a console's bumpers and triggers: either grip = the next tab (TAB: the game has no previous, it
        // wraps), the triggers = the previous / next sub-tab (PGUP / PGDN). One per press, no repeat.
        if (pressed & (kVGripL | kVGripR)) { Tap(kTab); g_Count.tabs++; LOGI("KbmInject: menu: next tab (grip)"); }
        if (pressed & kVTrigL) { Tap(kPgUp); g_Count.tabs++; LOGI("KbmInject: menu: previous sub-tab (L trigger)"); }
        if (pressed & kVTrigR) { Tap(kPgDn); g_Count.tabs++; LOGI("KbmInject: menu: next sub-tab (R trigger)"); }
        // Either stick's click brings the menu screen in front of the eyes (ScreenMode). Both together are the menu
        // button (above), which never reaches here as a click.
        if (pressed & (kXRBtnThumbL | kXRBtnThumbR)) { ScreenMode::RequestRecenter("stick click"); LOGI("KbmInject: menu: stick click, the screen comes in front"); }
        std::lock_guard lk(g_M); unsigned m = 0; for (int i = 0; i < kCount; i++) if (g_Held[i]) m |= 1u << i; g_Stats.heldMask = m;
        return;
    }
    if (g_WheelCloseAt >= 0 && g_Now >= g_WheelCloseAt)
    {   // (wheel or play: the mode follows the game's wheel state, which is what this click changes)
        g_WheelCloseAt = -1;
        // Only while the game still has it up: on a wheel that did close, the click would open it again.
        if (!g_Held[kMMB] && PlayerState::Running("Powerwheel"))
        { Tap(kMMB); g_Count.wheelCloses++; LOGI("KbmInject: wheel: a quick slot was assigned, so the game keeps it open: MOUSE3 click to close it"); }
        else LOGI("KbmInject: wheel: a quick slot was assigned but the wheel closed by itself: no extra click");
    }
    if (g_Mode == kModeWheel)
    {   // The wheel picks by the direction the mouse moves, as on a console by a stick: either stick steers it.
        // Nothing else goes out, and the servo stays quiet (the game is not turning while the wheel is up).
        for (int i = 0; i < kCount; i++) if (i != kMMB && g_Held[i] && g_TapUntil[i] == 0) Hold((Slot)i, false);
        const bool gripL = (live & kVGripL) != 0;
        if (!gripL && g_Held[kMMB] && g_TapUntil[kMMB] == 0 && g_WheelAssigned)
        {   // the wheel will not close on this release (see g_WheelAssigned): click MOUSE3 once more, a moment later
            g_WheelCloseAt = g_Now + 0.1; g_WheelAssigned = false;
        }
        Hold(kMMB, gripL);                                 // the grip still opens / closes it
        g_MoveKeys[0] = g_MoveKeys[1] = g_MoveKeys[2] = g_MoveKeys[3] = false;
        float sx = in.moveX + in.turnX, sy = in.moveY + in.turnY;
        const float k = 1500.f * (float)g_Dt;              // counts per second at full deflection
        // NOTE, measured with all eleven powers: the slot picked is not always the one the stick points at -- the
        // wheel's cursor slides round the ring from the last pick, and the slots ~30 deg off the vertical (Domino,
        // Swarm, Far Reach) often give a neighbour. Scaling the vertical counts by the window's aspect made it worse
        // (Far Reach then gave Bend Time at every angle tried). The highlight follows the stick, so a player corrects
        // by eye; a direct pick (reading the wheel's slots) would be the real fix.
        MouseMove((int)(sx * k), (int)(-sy * k));
        g_YawAccum = 0; g_PitchAccum = 0;
        // Potions as on a console with the wheel up (L1 + Circle / Cross): B health, A mana. Measured: the game drinks
        // with the wheel open (T took a mana elixir, 10 -> 9). And Y / X assign the highlighted item to a quick slot.
        if (g_QuickSlots.load())
        {
            if (pressed & kXRBtnB) { Tap(kR); g_Count.potions[0]++; LOGI("KbmInject: wheel: health potion (B)"); }
            if (pressed & kXRBtnA) { Tap(kT); g_Count.potions[1]++; LOGI("KbmInject: wheel: mana potion (A)"); }
            if (indexPads) { PadQuick(pressed, true); PadPotions(pressed); }
            else QuickButtons(live, pressed, true);
        }
        std::lock_guard lk(g_M); unsigned m = 0; for (int i = 0; i < kCount; i++) if (g_Held[i]) m |= 1u << i; g_Stats.heldMask = m;
        return;
    }

    // A cutscene (the player's Cinematic state, the development notes). The game takes no movement, attack or power in it,
    // and a click or a key it does take there is the SKIP: its prompt ("F Omitir", bottom left) is the interact key,
    // which no button sent outside of reaching for an object -- the cutscene could not be skipped. Here A or X is F
    // (held as long as the button), the menu button pauses / opens the journal as always, the stick still turns the
    // view (HeadAimServo), and nothing else goes out (a sword swing or a grip used to click and block into it).
    const bool cine = PlayerState::Cinematic() && g_CineControls.load();
    if (cine != g_InCine)
    {
        g_InCine = cine;
        LOGI("KbmInject: cutscene {}", cine ? "begins: A / X = skip (F), menu button as ever, the rest stands down" : "ends: play controls");
        if (cine) { g_Latched |= btn; g_SprintLatch = false; QuickReset(); g_MoveKeys[0] = g_MoveKeys[1] = g_MoveKeys[2] = g_MoveKeys[3] = false; }
    }
    if (cine)
    {
        for (int i = 0; i < kCount; i++) if (i != kF && g_Held[i] && g_TapUntil[i] == 0) Hold((Slot)i, false);
        SwordGesture::Reset(); SwordContact::Reset();
        const bool skip = (live & (kXRBtnA | kXRBtnX | kVPadUse)) != 0;   // (and the Index layout's right trackpad)
        if (skip && !g_Held[kF]) { g_Count.skips++; LOGI("KbmInject: cutscene: skip (F held with {})", (live & kXRBtnA) ? "A" : (live & kXRBtnX) ? "X" : "the right trackpad"); }
        Hold(kF, skip);
        MenuButton(live, g_QuickSlots.load() != 0);
        HeadAimServo(in);
        std::lock_guard lk(g_M); unsigned m = 0; for (int i = 0; i < kCount; i++) if (g_Held[i]) m |= 1u << i; g_Stats.heldMask = m;
        return;
    }

    // Movement: eight steady directions (or the old PWM on request).
    if (g_MovePwm.load())
    {
        Hold(kW, in.moveY > 0 && Pwm(in.moveY));
        Hold(kS, in.moveY < 0 && Pwm(in.moveY));
        Hold(kD, in.moveX > 0 && Pwm(in.moveX));
        Hold(kA, in.moveX < 0 && Pwm(in.moveX));
    }
    else
    {
        MoveKeys(in.moveX, in.moveY, g_MoveKeys);
        Hold(kW, g_MoveKeys[0]); Hold(kS, g_MoveKeys[1]); Hold(kD, g_MoveKeys[2]); Hold(kA, g_MoveKeys[3]);
    }
    const bool moving = g_MoveKeys[0] || g_MoveKeys[1] || g_MoveKeys[2] || g_MoveKeys[3] || fabsf(in.moveX) > 0.25f || fabsf(in.moveY) > 0.25f;
    // Sprint latches on the stick click, like a console: pushing the stick AND holding its click is a strain on a
    // Touch controller. It ends when the stick rests for 0.3 s, on a crouch, or on another click.
    // A crouch while sprinting is the game's SLIDE, and it needs the sprint key still down when C arrives: releasing
    // Shift and pressing C on the same frame gave a plain crouch half the time (measured in the mock, 2 slides of 4).
    // So Shift stays down a moment past the crouch that ends the latch.
    static double slideShiftUntil = 0;
    if (pressed & kXRBtnThumbL) g_SprintLatch = !g_SprintLatch;
    if (pressed & kXRBtnB) { if (g_SprintLatch && moving) slideShiftUntil = g_Now + 0.25; g_SprintLatch = false; }
    if (moving) g_StillSince = g_Now; else if (g_Now - g_StillSince > 0.3) g_SprintLatch = false;
    Hold(kShift, (g_SprintLatch || g_Now < slideShiftUntil) && moving);   // L stick click: sprint (latched)
    // Touch layout -> game actions
    Hold(kSpace, (live & kXRBtnA) != 0);                  // A            : jump
    Hold(kC,     (live & kXRBtnB) != 0);                  // B            : crouch (the game's C toggles)
    const bool quick = g_QuickSlots.load() != 0;
    // Y / X: quick slots 1-4 (short / held). The old Y was the game's LEAN, which only leans together with a sideways
    // stick and stops the stick walking meanwhile -- alone it did nothing; leaning the head already moves the view.
    if (indexPads) { PadQuick(pressed, false); PadPotions(pressed); QuickReset(); }
    else if (quick) QuickButtons(live, pressed, false);
    else QuickReset();
    Hold(kAlt,   !quick && (live & kXRBtnY) != 0);        // (old) Y      : lean
    Hold(kZ,     (live & kXRBtnThumbR) != 0);             // R stick click: zoom
    // The grabbing hand (GrabHand, `vr grabhand`: the right by default) takes what it touches: its grip on something
    // the palm's sweep has picked is the game's interact -- take a bottle, eat the fruit, open a door -- and anywhere
    // else it does what it always did (right: block, left: the power wheel). While an object is held neither grip
    // blocks nor opens the wheel (the wheel would drop it). How it is let go (`vr grabrelease`):
    //   0 = the trigger: a moving hand throws it (the game's "Lanzar", the left button, which GrabHand sends off with
    //       the hand's velocity), a slow hand drops it (the game's "Soltar", F: a thrown bottle shatters on its first
    //       contact, however slow);
    //   1 = hold the grip to hold it: letting go of the grip drops or throws it, by the hand's speed as in 0;
    //   2 = another press of the grip drops it, the trigger throws it with the hand's velocity (default).
    const bool holding = GrabHand::HeldEntity() != nullptr;
    const int grabHand = GrabHand::Hand(), relMode = GrabHand::ReleaseMode();
    const uint32_t grabGrip = grabHand ? kVGripR : kVGripL, grabTrig = grabHand ? kVTrigR : kVTrigL;
    const char* grabSide = grabHand ? "right" : "left";
    auto letGo = [&](bool throwIt, const char* how, float speed) {
        Tap(throwIt ? kLMB : kF);
        LOGI("KbmInject: {} {} with an object held: {} (hand {:.2f} m/s)", grabSide, how, throwIt ? "throw" : "drop", speed);
    };
    // The grip that takes is the F key for as long as it is held, not a tap: several interactions are a long press. A
    // body is picked up by holding F until the game's bar fills; the 80 ms tap this used to send filled a quarter of it
    // and stopped (reported from the headset, on the corpses save). Held like a keyboard player holds F, the game
    // itself decides tap or long press. A carried corpse is dropped with F too, and it is not a HoldObject.
    const bool carrying = PlayerState::Running("CarryCorpse");
    // Pull to open (Collision): the hand ON a closed drawer / cabinet / door the pointer has picked does not interact at
    // the grip; the pull does (the hand drawn back ~6 cm from the front while the grip stays closed), as a tap.
    Collision::Touch touch;
    const bool onOpenable = !holding && !carrying && Collision::Touched(grabHand, touch) && touch.openable && !touch.open
                         && GrabHand::TargetId() == (int)touch.id;
    float handNow[3], handRot[9];
    const bool handOk = GpuTrace::RawHandPose(grabHand, handNow, handRot);
    // Physical pickup (Collision::ItemsOn): the grip closing on an item the hand touches (and the pointer has) takes it
    // in the hand; it is stored when the hand goes behind the back (or eaten at the mouth) -- the game's interact then.
    const bool itemsByHand = Collision::ItemsOn();
    const bool onItem = itemsByHand && !holding && !carrying && Collision::Touched(grabHand, touch) && touch.item
                     && GrabHand::TargetId() == (int)touch.id;
    if (pressed & grabGrip)
    {
        g_GripTakes = holding || carrying || GrabHand::HandHasTarget();
        g_GripF = g_GripTakes && !holding;
        if (g_GripF && onItem)
        {
            g_GripF = false;
            const float in2[3] = { touch.p[0] - touch.n[0] * 0.02f, touch.p[1] - touch.n[1] * 0.02f, touch.p[2] - touch.n[2] * 0.02f };
            GrabHand::LockSensor(in2);
            g_Carry = Carry{ true, touch.id, g_Now, -1 };
            g_CarryClearAt = -1; g_Verify = Verify{};
            BodyHide::SetCarryEntity(touch.ent); GpuTrace::SetCarry(grabHand);   // drawn in the hand from now on
            VRRuntime::Vibrate(grabHand, 0.25f, 0.03f);
            LOGI("KbmInject: {} hand took item id {:#x}: put it behind your back to keep it (or to your mouth to eat it)", grabSide, touch.id);
        }
        else if (g_GripF && itemsByHand && Collision::PointerItem() && Collision::PointerItem() == (uint32_t)GrabHand::TargetId())
        {   // an item pointed at from afar is not taken by the grip: reach for it
            g_GripF = false;
            LOGI("KbmInject: {} grip on item id {:#x} from afar: not taken (PhysicalPickup: grab it by hand)", grabSide, Collision::PointerItem());
        }
        else if (g_GripF && onOpenable && handOk)
        {
            g_GripF = false;
            g_Pull = Pull{ true, { handNow[0], handNow[1], handNow[2] }, { touch.n[0], touch.n[1], touch.n[2] }, g_Now, touch.id };
            LOGI("KbmInject: {} grip on a closed drawer / cabinet (id {:#x}): pull it to open", grabSide, touch.id);
        }
        else if (g_GripF) LOGI("KbmInject: {} grip {}: interact, F held with the grip", grabSide, carrying ? "carrying a corpse" : "on a picked interactable");
        else if (holding && relMode == 2) letGo(false, "grip", 0.f);
    }
    else if (!(live & grabGrip))
    {
        if (g_GripF) LOGI("KbmInject: {} grip released: F up", grabSide);
        if (g_Pull.armed) LOGI("KbmInject: {} grip let go of the drawer / cabinet without pulling it", grabSide);
        if (g_Carry.on) { LOGI("KbmInject: {} hand let go of item id {:#x} before storing it: it goes back", grabSide, g_Carry.id); CarryReturn(); }
        g_GripTakes = g_GripF = false; g_Pull.armed = false; g_Carry.on = false;
    }
    if (g_Carry.on)
    {
        const int zone = CarryZone(in, grabHand);
        if (zone && g_Carry.zoneAt < 0) g_Carry.zoneAt = g_Now;
        if (!zone) g_Carry.zoneAt = -1;
        if (zone && GrabHand::TargetId() == (int)g_Carry.id && !g_Held[kF])
        {
            Tap(kF);
            LOGI("KbmInject: item id {:#x} {} ({:.1f} s after taking it): interact", g_Carry.id, zone == 1 ? "put behind the back: storing" : "brought to the mouth: eating", g_Now - g_Carry.at);
            g_Verify = Verify{ g_Carry.id, g_Now }; Collision::WatchItem(g_Carry.id);
            g_Carry.on = false; GrabHand::UnlockSensor();
        }
        else if (zone && g_Now - g_Carry.zoneAt > 0.6)
        {
            LOGI("KbmInject: item id {:#x} could not be taken: the sensor no longer finds it (out of reach?) -- it goes back", g_Carry.id);
            g_Carry.on = false; CarryReturn();
        }
        else if (g_Now - g_Carry.at > 7.0) { g_Carry.on = false; CarryReturn(); LOGI("KbmInject: item id {:#x} carried too long: it goes back", g_Carry.id); }
    }
    if (g_Verify.at >= 0)
    {   // the game took it (its entity left the world) or refused it (still there: a full inventory, as the game says)
        const int st = Collision::ItemPresent();
        if (st == 0)
        {
            VRRuntime::Vibrate(grabHand, 0.6f, 0.06f);
            LOGI("KbmInject: item id {:#x} taken by the game", g_Verify.id);
            GpuTrace::EndCarry(); BodyHide::SetCarryEntity(nullptr); Collision::WatchItem(0); g_Verify = Verify{};
        }
        else if (g_Now - g_Verify.at > (st == 1 ? 0.7 : 1.5))
        {
            VRRuntime::Vibrate(grabHand, 0.25f, 0.15f);
            LOGI("KbmInject: item id {:#x} REFUSED by the game (still in the world {:.1f} s later: inventory full?): it goes back", g_Verify.id, g_Now - g_Verify.at);
            CarryReturn(); Collision::WatchItem(0); g_Verify = Verify{};
        }
    }
    if (g_CarryClearAt >= 0 && g_Now >= g_CarryClearAt)
    {   // back in its place: drawn as the game has it again
        GpuTrace::EndCarry(); BodyHide::SetCarryEntity(nullptr); g_CarryClearAt = -1;
    }
    if (g_Pull.armed && handOk)
    {
        const float d[3] = { handNow[0] - g_Pull.start[0], handNow[1] - g_Pull.start[1], handNow[2] - g_Pull.start[2] };
        const float out = d[0] * g_Pull.n[0] + d[1] * g_Pull.n[1] + d[2] * g_Pull.n[2];
        if (out >= 0.06f)
        {
            Tap(kF); g_Pull.armed = false;
            VRRuntime::Vibrate(grabHand, 0.4f, 0.05f);
            LOGI("KbmInject: {} hand pulled the drawer / cabinet {:.0f} cm: interact (the game opens it)", grabSide, out * 100.f);
        }
        else if (g_Now - g_Pull.at > 4.0 || sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) > 0.5f) g_Pull.armed = false;
    }
    {   // a push on an open one: the interact that closes it, when the pointer has it
        uint32_t cid = 0;
        if (Collision::TakeCloseRequest(grabHand, &cid) && !holding && GrabHand::TargetId() == (int)cid && !g_Held[kF])
        {
            Tap(kF);
            LOGI("KbmInject: {} hand moved drawer / cabinet / window id {:#x}: interact", grabSide, cid);
        }
    }
    // Once the press has become a hold the key has done its job: F is the hold's "drop", and `vr grabrelease 1` taps it
    // when the grip opens, which needs the key up beforehand.
    if (holding && g_GripF) { g_GripF = false; LOGI("KbmInject: {} grip took an object: F up", grabSide); }
    // Index layout: the right TRIGGER is interact (on what the right hand's pointer picked), held like a key; the sword
    // strikes by gesture and by the blade's contact (its button is the right trackpad). Not while it lets an object go.
    // PhysicalPickup: the interact buttons do not take an item from afar either (the zone's tap above is how)
    const bool itemAimed = itemsByHand && Collision::PointerItem() && Collision::PointerItem() == (uint32_t)GrabHand::TargetId();
    const bool trigUse = indexPads && (live & kVTrigR) != 0 && !holding && !(grabHand == 1 && g_TrigLets) && !itemAimed;
    Hold(kF, (!quick && (live & kXRBtnX) != 0 && !itemAimed) || g_GripF || trigUse);   // the grabbing grip (above), (old) X, Index trigger : interact
    if (holding && relMode == 1 && (released & grabGrip))
    {
        float speed = 0; const bool thr = GrabHand::ReleaseThrows(&speed);
        letGo(thr, "grip released", speed);
    }
    const bool takesL = grabHand == 0 && g_GripTakes, takesR = grabHand == 1 && g_GripTakes;
    // The press that lets the object go is spent on that: the hold ends ~25 ms into it, and the same trigger still down
    // would then be the sword's attack (measured: a Slash 8 ms after the drop, shattering every bottle in reach). And
    // a throw IS a swing: the sword gesture stays out for a moment after the hold ends.
    if (holding && (pressed & grabTrig))
    {
        g_TrigLets = true;
        float speed = 0; const bool thr = GrabHand::ReleaseThrows(&speed);
        if (relMode == 0) letGo(thr, "trigger", speed);
        else if (relMode == 2) letGo(true, "trigger", speed);
    }
    else if (!(live & grabTrig)) g_TrigLets = false;
    if (g_WasHolding && !holding) g_HoldEndedAt = g_Now;
    g_WasHolding = holding;
    const bool afterHold = g_HoldEndedAt >= 0 && g_Now - g_HoldEndedAt < 0.8;
    const bool letsL = grabHand == 0 && g_TrigLets, letsR = grabHand == 1 && g_TrigLets;
    // [Gameplay] HolsterWeapons: the left GRIP squeezed with the left hand at the left hip draws the pistol, behind the
    // back the crossbow, on the chest the last power held (Holster: the game's own equip). That squeeze does nothing else -- no power wheel -- until the
    // grip opens. A weapon already in the hand is not drawn again: the grip then opens the wheel as anywhere else (a
    // hand hanging at the side is in the hip zone). A light buzz as the hand reaches a holster whose weapon it can draw.
    {
        TorsoTick(VRRuntime::GetHeadPose());
        float rel[3] = {};
        const bool usable = Holster::Enabled() && !holding && !takesL;
        const int hz = usable ? HolsterZone(in, rel) : 0;
        const int kind = hz == 1 ? Holster::kPistol : hz == 2 ? Holster::kCrossbow : Holster::kMagic;
        const char* where = hz == 1 ? "left hip" : hz == 2 ? "back" : "chest";
        const char* what = hz == 1 ? "pistol" : hz == 2 ? "crossbow" : "last power";
        if (hz != g_HolsterZoneWas)
        {
            if (hz && Holster::Owns(kind) && !Holster::InHand(kind)) VRRuntime::Vibrate(0, 0.15f, 0.02f);
            g_HolsterZoneWas = hz;
        }
        if (pressed & kVGripL)
        {
            if (hz && Holster::Owns(kind) && !Holster::InHand(kind))
            {
                g_HolsterGrip = true;
                Holster::Request(kind);
                VRRuntime::Vibrate(0, 0.5f, 0.04f);
                LOGI("KbmInject: left grip at the {} (hand {:.2f} {:.2f} {:.2f} m from the neck: right, up, forward): draw the {}",
                     where, rel[0], rel[1], rel[2], what);
            }
            else if (hz && !Holster::Owns(kind)) LOGI("KbmInject: left grip at the {}, but there is no {} to draw: power wheel as usual", where, what);
            else if (hz) LOGI("KbmInject: left grip at the {}: {} already in the hand: power wheel as usual", where, hz == 3 ? "a power is" : what);
            else if (usable && (rel[1] < -0.2f || rel[2] < 0.f) && g_Now - g_HolsterLogAt > 2.0)
            {   // near a holster but not in one: where the hand was, for tuning the zones
                g_HolsterLogAt = g_Now;
                LOGI("KbmInject: left grip with the hand {:.2f} {:.2f} {:.2f} m from the neck (right, up, forward): no holster there", rel[0], rel[1], rel[2]);
            }
        }
        if (!(live & kVGripL)) g_HolsterGrip = false;
    }
    Hold(kMMB,   (live & kVGripL) != 0 && !takesL && !holding && !g_HolsterGrip);   // L grip : power wheel (or take / draw, above)
    // The sword by gesture (SwordGesture): a swing taps the attack, the fist by the right ear holds it (the power
    // attack's charge) until a thrust lets go, the fist at the left shoulder blocks. The trigger and the grip keep
    // doing all of it as well.
    // Holding an object the game puts the sword away and its attack button is the object's throw: the right hand stays
    // out of it (a swing would throw the bottle).
    // (the sword's button: the right trigger, or with the Index layout the right trackpad)
    const bool trigR = indexPads ? ((live & kVPadUse) != 0 && !holding) : ((live & kVTrigR) != 0 && !holding && !letsR);
    const bool gripR = (live & kVGripR) != 0 && !holding && !takesR;
    // The blade strikes what it touches (SwordContact): before the gestures, so a contact during a charged thrust
    // releases it this same tick.
    MeleeAim::NoteTrigger(trigR);
    const SwordContact::Out contact = SwordContact::Tick(in, !holding && !afterHold && !SwordGesture::StandsDown());
    if (holding || afterHold) SwordGesture::Reset();
    SwordGesture::Out sword = SwordGesture::Tick(in, trigR, gripR);
    if (holding || afterHold) sword = SwordGesture::Out{};
    Hold(kCtrl,  gripR || sword.block);                   // R grip / fist at the left shoulder : block
    Hold(kLMB,   trigR || sword.hold);                    // R trigger / charge pose            : melee (right hand)
    // a swing: one strike -- with SwordContact, once the swing has touched something (aimed at it) or is over (in the air)
    if (sword.tap && !SwordContact::DeferAirTap()) g_PendingStrike = g_Now;
    if (contact.tap) g_PendingStrike = g_Now;             // the blade touched something        : one strike
    if (g_PendingStrike >= 0)
    {   // ...once block is up and has been for kAfterBlock (a press under block, or on its last tick, is lost)
        if (!g_Held[kCtrl] && g_Now - g_UpAt[kCtrl] >= kAfterBlock)
        {
            if (g_Now - g_PendingStrike > 0.004) LOGI("KbmInject: gesture strike sent {:.0f} ms late, after block let go", (g_Now - g_PendingStrike) * 1000.0);
            Tap(kLMB); g_PendingStrike = -1;
        }
        else if (g_Now - g_PendingStrike > 0.4) { LOGI("KbmInject: gesture strike dropped: block is still held"); g_PendingStrike = -1; }
    }
    // L trigger: power / weapon (left hand) -- not while the left hand holds an object.
    Hold(kRMB, (live & kVTrigL) != 0 && !(holding && grabHand == 0) && !letsL);
    ThrowGesture::Tick(in, (live & kVTrigL) != 0);        // ...and a grenade leaves with the hand's velocity when it opens
    MenuButton(live, quick);
    // Right stick up / down: health / mana potion. Only a clearly vertical push, and once per push.
    if (g_Potions.load())
    {
        if (g_PotionArmed && fabsf(in.turnY) > 0.8f && fabsf(in.turnX) < 0.45f)
        { Tap(in.turnY > 0 ? kR : kT); g_PotionArmed = false; LOGI("KbmInject: {} potion", in.turnY > 0 ? "health" : "mana"); }
        else if (fabsf(in.turnY) < 0.3f) g_PotionArmed = true;
    }

    HeadAimServo(in);
    { std::lock_guard lk(g_M); unsigned m = 0; for (int i = 0; i < kCount; i++) if (g_Held[i]) m |= 1u << i; g_Stats.heldMask = m; }
}

bool KbmInject::Possessing() { return g_Possessing.load(); }
void KbmInject::ReleaseAll() { for (int i = 0; i < kCount; i++) if (g_Held[i]) { g_TapUntil[i] = 0; g_Held[i] = false; SendKey((Slot)i, false); } g_SprintLatch = false; }
void KbmInject::SetEnabled(bool on) { g_Enabled = on; if (!on) ReleaseAll(); }
bool KbmInject::Enabled() { return g_Enabled; }
void KbmInject::SetSnapAngle(float d) { g_SnapDeg = d; }
void KbmInject::SetMouseCountsPerDegree(float c) { g_CountsPerDeg = c / g_SensScale; }   // c: at the current sensitivity
void KbmInject::SetHeadAim(bool on) { g_HeadAim = on; g_HeadValid = false; }
void KbmInject::ResetHeadRef() { g_HeadValid = false; }
void KbmInject::Calibrate() { g_CalState = 1; g_CalTries = kCalMaxTries; g_CalDelay = 0; }
bool KbmInject::HeadAim() { return g_HeadAim; }
bool KbmInject::WorldYawOffset(float& deg)
{
    if (!g_HeadAim.load() || !g_ServoValid || !g_HeadValid) return false;
    deg = g_DesiredYaw - g_LastHeadYaw;
    return true;
}
float KbmInject::StickTurnTotal() { return g_StickTurn.load(); }
void KbmInject::SetSnapBlank(bool on)
{
    g_SnapBlankOn = on; if (!on) g_SnapState = 0;
    LOGI("KbmInject: snap turns {}", on ? fmt::format("darken the headset ({:.0f} %) until the game has turned (SnapTurnBlank = 1)", g_SnapBlankDepth.load() * 100.f) : std::string("do not blank the headset"));
}
namespace { float SnapBlankRaw(unsigned long long serial); }
float KbmInject::SnapBlankFade(unsigned long long serial)
{
    const float raw = SnapBlankRaw(serial);      // 0 = blanked .. 1 = clear
    return raw >= 1.f ? 1.f : 1.f - g_SnapBlankDepth.load() * (1.f - raw);
}
void KbmInject::SetSnapBlankOpacity(float k) { g_SnapBlankDepth = k < 0.f ? 0.f : (k > 1.f ? 1.f : k); }
namespace { float SnapBlankRaw(unsigned long long serial)
{
    const int st = g_SnapState.load();
    if (!st) return 1.f;
    const double now = QpcNow();
    if (now - g_SnapT0.load() > kSnapTimeout + kSnapFadeIn) { g_SnapState = 0; return 1.f; }   // never settled (a takedown took the camera)
    if (st == 1 && now - g_SnapT0.load() < kSnapTimeout) return 0.f;
    if (st == 2)
    {
        const unsigned long long until = g_SnapUntil.load();
        const bool before = serial && until ? serial < until : now < g_SnapUntilT.load();
        if (before && now - g_SnapT0.load() < kSnapTimeout) return 0.f;
    }
    // the first image of the new heading: a short fade in, not a flash
    double shown = g_SnapShownT.load();
    if (shown <= 0) { shown = now; g_SnapShownT = now; }
    const float k = (float)((now - shown) / kSnapFadeIn);
    if (k >= 1.f) { g_SnapState = 0; return 1.f; }
    return k < 0.25f ? 0.25f : k;
} }
unsigned long long KbmInject::SnapBlanks() { return g_SnapBlanks.load(); }
void KbmInject::SetMenuInput(int m) { g_MenuInput = m; }
int KbmInject::MenuInput() { return g_MenuInput; }
void KbmInject::SetTurnSpeed(float d) { g_TurnSpeed = d; }
float KbmInject::TurnSpeed() { return g_TurnSpeed; }
void KbmInject::SetMovePwm(int m) { g_MovePwm = m; }
void KbmInject::SetPotions(int m) { g_Potions = m; }
void KbmInject::SetQuickSlots(int m) { g_QuickSlots = m; }
void KbmInject::SetGripThreshold(int hand, float t)
{
    if (hand < 0 || hand > 1) return;
    g_GripOn[hand] = t < 0.05f ? 0.05f : (t > 0.98f ? 0.98f : t);
    LOGI("KbmInject: the {} grip counts as pressed past {:.2f} (GripThreshold{}; the squeeze force on an Index), released below {:.2f}",
         hand ? "right" : "left", g_GripOn[hand].load(), hand ? "Right" : "Left", std::max(0.02f, g_GripOn[hand].load() - 0.15f));
}
void KbmInject::SetQuickKeys(const std::string& keys)
{
    std::string k = g_QuickKeys;
    for (int i = 0; i < 4 && i < (int)keys.size(); i++)
    {
        const char c = (char)toupper((unsigned char)keys[i]);
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) k[i] = c;
    }
    g_QuickKeys = k;
    g_BindGen = ~0u;   // re-resolved on the next tick (Rebind releases a held slot with its old key first)
}
void KbmInject::SetQuickHold(float s) { g_QuickHold = s < 0.15f ? 0.15f : (s > 2.f ? 2.f : s); }
void KbmInject::SetAutoFocus(int on) { g_AutoFocus = on ? 1 : 0; LOGI("KbmInject: bring the game to the front {}", on ? "on" : "off"); }
std::string KbmInject::ControlsInfo()
{
    return fmt::format("controls autofocus {} tries {} done {} failed {} unpaused {} | quickslots {} keys {},{},{},{} hold {:.2f} stickpotions {} | quick {}/{}/{}/{} potions health {} mana {} tabs {} menubtn journal {} pause {} wheelcloses {} | cine {} incine {} skips {} videoskips {}",
        g_AutoFocus.load(), g_Focus.tries, g_Focus.done, g_Focus.failed, g_Focus.unpaused,
        g_QuickSlots.load(), g_Bind[kQuick1].name, g_Bind[kQuick2].name, g_Bind[kQuick3].name, g_Bind[kQuick4].name, g_QuickHold.load(), g_Potions.load(),
        g_Count.quick[0], g_Count.quick[1], g_Count.quick[2], g_Count.quick[3], g_Count.potions[0], g_Count.potions[1], g_Count.tabs,
        g_Count.menuJournal, g_Count.menuPause, g_Count.wheelCloses, g_CineControls.load() ? 1 : 0, g_InCine ? 1 : 0, g_Count.skips, g_Count.videoSkips);
}
bool KbmInject::Command(const std::string& what, std::istringstream& ss)
{
    if (what == "playrule") { int m = 1; ss >> m; g_PlayRule = m ? 1 : 0; LOGI("cmd vr playrule {}: {}", m, m ? "a player seen playing stays in play without arms (death, kill camera, notes, journal excepted)" : "only the listed armless states are play, as before"); return true; }
    if (what == "scenemenus") { int m = 1; ss >> m; g_SceneMenus = m ? 1 : 0; LOGI("cmd vr scenemenus {}: {}", m, m ? "the character choice and a black market are menus (screen, menu controls)" : "they are play, as before"); return true; }
    if (what == "tapkey")
    {
        std::string k; double ms = 100; ss >> k >> ms;
        KeyBinds::Bind b;
        if (!KeyBinds::FromName(k, b) || !b.valid()) { LOGI("cmd vr tapkey {}: unknown key", k); return true; }
        { std::lock_guard lk(g_M); g_TestBind = b; g_TestMs = std::clamp(ms, 20.0, 5000.0); g_TestPending = true; }
        LOGI("cmd vr tapkey {} {} ms", b.name, ms);
        return true;
    }
    return false;
}
void KbmInject::SetCineControls(bool on) { g_CineControls = on; LOGI("KbmInject: cutscene controls {}", on ? "on (A / X skip)" : "off (play's, as before)"); }
void KbmInject::SetMeleeKey(int vk) { if (g_Held[kLMB]) { SendKey(kLMB, false); g_Held[kLMB] = false; g_TapUntil[kLMB] = 0; } g_MeleeKey = vk; }
KbmInject::Stats KbmInject::GetStats() { std::lock_guard lk(g_M); return g_Stats; }
