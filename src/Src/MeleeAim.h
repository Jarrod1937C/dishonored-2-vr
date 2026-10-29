#pragma once
#include <string>

//! The sword strikes where the RIGHT controller points, not where the head looks.
//!
//! The player's melee code (arkCpntWeaponMeleePlayer, exe+0x62E000..0x636000, plus two helpers just below) never
//! reads the camera fields directly: every direction and eye position it uses comes from two one-line accessors,
//! `GetViewAxis(cam) = cam+0xFC` (exe+0x8067C0) and `GetViewOrigin(cam) = cam+0xD8` (exe+0x417820). Measured with a
//! read watchpoint on cam+0xFC: idle, 15 readers, none of them melee; during four sword swings, 7 more, all in that
//! range and all fed by those accessors (the attack direction is vtable slot 40 = exe+0x62F080, the attack start
//! point slot 42 = exe+0x62F290, which hands both to the shape builder exe+0x634FB0).
//!
//! Those accessors are called from 66/85 places across the game, so they are NOT hooked. Only their call sites
//! inside the melee code are redirected, each to a small thunk that saves every volatile register and asks
//! `Resolve` for the pointer: the right controller's axis (and, with `SetOrigin(1)`, its position) when the camera
//! passed in is the player's and the hand mode is on, the camera's own fields otherwise. Nothing in the camera is
//! ever written, so no other thread can see the hand as the camera.
namespace MeleeAim
{
    bool Install();                  //!< patch the call sites (idempotent); safe before the game has spawned anything
    void SetEnabled(bool on);        //!< default on; also requires HandAimHook::Mode() >= 2 (vr hand 2 / weapon6dof)
    bool Enabled();
    //! Where the strike starts: 0 (default) the head, i.e. the game's own eye; 1 the controller.
    void SetOrigin(int mode);
    int  Origin();
    //! A gesture swing's direction (world, rows fwd/left/up), in force for `seconds` from now: the melee code asks
    //! for its direction ~0.2 s after the click, when the arm has already finished the arc and the controller points
    //! somewhere else, so SwordGesture freezes the direction at the swing's peak and hands it over here. While it is
    //! in force it replaces the controller's axis; a trigger press (ClearGestureAxis) goes back to the controller.
    void SetGestureAxis(const float axis[9], float seconds);
    void ClearGestureAxis();
    //! --- contact strikes (SwordContact, the development notes) ---
    //! What the drawn blade touched: the entity, and its box (entity axes, rows fwd/left/up; local min/max). For the next
    //! 0.8 s the strike may hit a candidate whose physics body belongs to that entity (or, when the body cannot be resolved,
    //! that lies inside the box + `swordcfg boxmargin`).
    struct HitBox { float org[3]; float axis[9]; float mn[3]; float mx[3]; void* ent; };
    void AddHitBox(const HitBox& b);
    //! The touched point and the blade's velocity there: for 0.3 s the attack's physics sweep (slot 41) ends at that point,
    //! arriving along that direction, instead of following the canned animation's weapon.
    void SetContactSweep(const float point[3], const float vel[3]);
    //! Open the attack window on the next hit check of a running Slash that has not hit yet (the contact's strike), for
    //! up to `maxWait` seconds (a click that starts the Slash may still be on its way).
    void RequestOpen(float maxWait);
    //! [Gameplay] SwordBladeDamage / SwordDamageScale: a blade contact on a character strikes it directly (the game's own
    //! strike, on a record of its physics sweep run into the contact), damage scaled by the blade's speed -- no Slash.
    void SetBladeDamage(bool on, float scale);
    bool BladeDamage();
    //! Strike `ent` (an entity the blade touched) as soon as the sweep has a record of it: returns the request's serial.
    unsigned BladeStrike(void* ent, float bladeSpeed);
    //! 0 none, 1 pending, 2 landed, 3 ended without landing (no record in time, or the game refused it), 4 the target
    //! defended itself (blocked or parried, as against the game's swing).
    int BladeStrikeState(unsigned serial);
    //! Game thread, once a frame: asks the player's assassination sensor whether a stealth kill is on offer.
    void TickTakedown();
    //! [Gameplay] KnockoutBlow: while the takedown is offered, a hard blow to the back of the target's head knocks him
    //! out (the game's choke knockout, without its animation).
    void SetKnockoutBlow(bool on);
    //! [Gameplay] FinisherAnimations: false = a finishing blow is an ordinary blow (no synchronised kill animation) and
    //! no kill cam; true = the game's.
    void SetFinishers(bool on);
    //! A knockout blow would count now: the takedown is offered, or was within the last 1.5 s on a target still there.
    bool KnockoutReady();
    //! A stealth kill is on offer now (the game's "kill" prompt): the game's attack would be the takedown.
    bool KillOffered(void** target = nullptr);
    bool SlashRunning();             //!< the player's Slash state is running (seen at the last Present)
    bool HitThisSlash();             //!< this Slash already hit something (a click now would be dropped, and one hit per swing)
    //! The running Slash's attack window: 0 = not opened yet (it opens ~0.2 s after the click), 1 = open, 2 = closed.
    int  WindowPhase();
    uint64_t Hits();                 //!< strikes that landed so far (the game closed the window on a hit)
    unsigned SlashSerial();          //!< counts the Slashes that began (seen at the Present)
    uint64_t Accepted();             //!< candidates of a touched entity let through (objects break this way: no window close)
    void NoteTrigger(bool held);     //!< KbmInject, per tick: a Slash the trigger starts is not filtered by contact
    //! A Slash the trigger did not start may only hit what the blade touched: 2 (default) every candidate of a touched
    //! entity counts, inside the game's 30 deg cone or not; 1 = touched AND inside the cone; 0 = never filtered.
    void SetFilterMode(int mode);
    int  FilterMode();
    void SetBoxMargin(float m);
    //! 1 (default) = the trigger aims along the blade or the knuckles, whichever is closer to the gaze; 0 = the controller.
    void SetBladeAim(int on);
    void OnPresent();                //!< per frame: logs one line per sword swing (hand vs view direction)
    //! Research: set g_stopTime 1 this many ms after the next Slash begins (one shot), to look at a still swing.
    void FreezeAfterSlash(int ms);
    std::string Stats();             //!< heartbeat
    void DumpSites();                //!< research: per call site, calls and redirections so far
    void SetContactVel(int on);      //!< research, default 0: a contact's candidates carry the drawn blade's velocity
    void SetSweepMode(int m);        //!< research: 0 game sweep (default), 1 game start + contact end, 2 along the blade
    void ShapeLog(int n);            //!< research: log the next n attack shapes (slot 42)
    void OpenAt(int ms);             //!< research: force the attack window open this long after the next Slash begins
    void Trace(int n);               //!< research: log the next n accessor / cone-test calls (meleetrace)
}
