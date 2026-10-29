#pragma once
#include <sstream>
#include <string>
//! Stage 3 fallback: drives the game with synthesized KEYBOARD/MOUSE input (SendInput)
//! from the VR controllers. Works only while the game window is in the foreground —
//! which is the normal VR situation. Digital movement (WASD, with analog handled by
//! pulse-width modulation), mouse deltas for turning (smooth or snap), buttons -> keys.
namespace KbmInject
{
    void Tick();                 //!< call once per rendered frame (render thread)
    void SetEnabled(bool on);
    bool Enabled();
    void SetSnapAngle(float deg);          //!< 0 = smooth turning
    void SetMouseCountsPerDegree(float c); //!< calibration for snap turns (default from g_mouseSensitivity 0.808)
    void ReleaseAll();
    //! Head aim: the game's yaw/pitch follow the HMD (mouse deltas), so aiming = looking.
    void SetHeadAim(bool on);
    bool HeadAim();
    //! Head aim: the game yaw the servo is driving towards minus the head's XR yaw (degrees, both positive = left),
    //! i.e. where XR space sits in the game world. It changes only with a stick turn or a recentre -- unlike the
    //! camera's yaw, which lags the head by the servo's error. False when head aim is off or not yet seeded.
    //! Present thread (the servo's own).
    bool WorldYawOffset(float& deg);
    //! Degrees of turn the stick has added to the head-aim target since start (+ = left). Read by the takedown view.
    float StickTurnTotal();
    //! [Controls] SnapTurnBlank: black from a snap turn until the headset shows an image of the new heading.
    void SetSnapBlank(bool on);
    void SetGripThreshold(int hand, float t);   //!< [Controls] GripThresholdLeft / Right (hand 0 / 1): 0..1, how far that grip is pressed to count
    void SetSnapBlankOpacity(float k);     //!< [Controls] SnapTurnBlankOpacity: 1 = black, 0.3 = 30 % darker (before SetSnapBlank)
    //! Brightness for the headset image built from view `serial` (a pair's left serial; 0 = unknown): 1 normally, 0 while
    //! a snap turn is blanked, then a short fade in. Render thread (VRRuntime::OnPresent).
    float SnapBlankFade(unsigned long long serial);
    unsigned long long SnapBlanks();       //!< snap turns blanked so far (the heartbeat)
    void Calibrate();
    //! The HMD's yaw jumped without the head moving (a recenter): re-seed the servo instead of turning the game.
    void ResetHeadRef();            //!< measure mouse counts/degree in-game (logs the result)
    //! Whether the controllers may drive the game's MENUS (0 = no, the default: see the note in the .cpp).
    void SetMenuInput(int mode);
    int  MenuInput();
    void SetTurnSpeed(float degPerSec);    //!< smooth turn rate at full deflection (default 120)
    float TurnSpeed();
    void SetMovePwm(int on);               //!< 1 = the old pulse-width-modulated move keys
    void SetPotions(int on);               //!< right stick up/down = health/mana potion (the old way; default off)
    //! Y / X = quick slots 1-4 (short / held), potions on B / A with the wheel open, menu button short = journal and
    //! held = pause (default 1); 0 = the old layout (Y lean, X interact, menu short = pause, held = journal).
    void SetQuickSlots(int on);
    //! 1 (default) = bring the game to the front when a controller is used, or the headset put on, while it is behind
    //! another window, and close the pause the game opened itself on losing the front; 0 = leave the windows alone.
    void SetAutoFocus(int on);
    void SetQuickKeys(const std::string& keys);   //!< the game keys of slots 1-4, e.g. "1234" (default) or "6789"
    void SetQuickHold(float seconds);             //!< how long Y / X are held to mean slot 3 / 4 (default 0.5)
    std::string ControlsInfo();                   //!< heartbeat line: the layout and what each control has sent
    //! `vr cinecontrols 0|1`: in a cutscene A / X skip it (F), the menu button and the stick work, nothing else goes out (1,
    //! default); 0 = the play controls, as before.
    void SetCineControls(bool on);
    //! Send melee as this keyboard key (a virtual-key code) instead of MOUSE1; 0 = the mouse button. The game must
    //! have the key bound to MELEE. A key cannot land on another window the way a click under the cursor can.
    void SetMeleeKey(int vk);
    struct Stats { unsigned long long ticks = 0, inputTicks = 0, fgTicks = 0, keyEvents = 0, mouseEvents = 0; unsigned heldMask = 0; float lastDx = 0, maxTurnX = 0, yawError = 0, pitchError = 0, bodyYaw = 0, headYaw = 0; bool simulating = true; bool menuSafe = false; int fpDraws = -1; int mode = 1; bool sprint = false; bool possessing = false;
                   int scene = 0;      //!< scene: a menu over the world (0 none, 1 the character choice, 2 a black market)
                   int playRule = 1; bool playLatched = false; };   //!< vr playrule; this player has been seen playing
    Stats GetStats();
    //! Inside a possessed host (EnterPossessedHost began it; the arms coming back, death or a load end it).
    bool Possessing();
    //! [Controls] PhysicalCrouch / PhysicalCrouchDepth: the real head this far (m) under its standing height crouches the
    //! character (the game's crouch key), standing up stands it.
    void SetPhysicalCrouch(bool on, float depth);
    //! The game's crouch now is the one the real head made (ViewMatrixHook keeps the view at the head's height then).
    bool PhysicalCrouchOwns();
    //! `vr scenemenus 0|1` (menus over the world: the character choice, a black market; the development notes) and
    //! `vr tapkey <KEY> [ms]` (one key by the game's name, sent the way the controllers send theirs: a test aid --
    //! Focus-Key.ps1's keybd_event ESC is NOT seen the same way, measured). True when `what` was one of them.
    bool Command(const std::string& what, std::istringstream& ss);
}
