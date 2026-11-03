#pragma once
#include <string>
struct XRInputState;

//! GRENADES thrown like a real one (the user's spec, 2026-10-01): hold the left trigger -- the game's own "hold the
//! power/weapon button", which pulls the pin and starts the fuse (cooking) -- and the grenade leaves when the trigger
//! is let go, with the VELOCITY the hand had at that instant: the direction the hand MOVES (not where it points) and
//! its speed. Let go without moving and it drops at your feet; a gentle push lobs it short; a hard throw sends it far.
//! Mines and spring razors are NOT affected (the user's call: they keep the game's placement, aimed with the hand).
//!
//! How the game throws (RESEARCH "Throwing"): arkCpntWeaponThrowable::Throw (exe+0x632F70) builds the velocity from
//! two decl constants -- m_throwSpeed (+0x29C) along the aim direction tilted up by m_throwAngle (+0x298), or the
//! m_lowThrow* pair when looking down -- and hands it to the projectile entity with a one-line setter
//! (exe+0x87C2A0: entity+0x33C = v) at exe+0x63333C, right before the launch (exe+0x639970), which for a throwable
//! is passed speed 1.0 and so leaves that velocity alone. Holding longer only cooks: the speed is a constant.
//! That one call site is redirected here; for a projectile with an arkCpntGrenade the velocity becomes
//!   gain * (hand velocity at the release) + inherit * (the player's own velocity)
//! capped at maxSpeed, and the spawn point the hand's position at the release.
//!
//! The release velocity is measured in the XR LOCAL space from the left controller's poses (display-time stamped,
//! like the sword): the fastest velocity over the last `peakWin` (0.15 s) before the trigger opened -- people let go a
//! little after the peak, not at it -- and mapped into the world the way the hand itself is drawn (HandAimHook).
//!
//! A REAL throw (2026-11-01, throwcfg `instant` 1): hold the trigger, throw, let go -- the grenade leaves at that
//! instant, from the hand. The game would launch it from its throw animation's event 0.4-0.7 s after the release
//! (the user's logs: from wherever the hand had got to, with the hand already stopped); the mod fires it itself at the
//! release and drops the animation's launch (see ThrowGesture.cpp, "the real throw").
namespace ThrowGesture
{
    bool Install();
    //! One tick, from KbmInject::Tick in PLAY mode (render thread). triggerHeld: the left trigger as it goes to the
    //! game (the RMB it holds).
    void Tick(const XRInputState& in, bool triggerHeld);
    //! The left trigger as it should reach the game (render thread, each tick, before Tick): while the game's throw
    //! cycle after a grenade is not over (throwcfg `ready`) a press is held back and passed on when it is; a press the
    //! game did not take (no grenade lit, `relight`) is let up and pressed again.
    bool GateTrigger(bool live);
    //! A grenade in the left hand being thrown: its trigger held (cooking), or let go less than 0.8 s ago (the fuse may
    //! still be lighting, the launch still to come). KbmInject ignores the left grip meanwhile (asked: a hand that
    //! swings and lets go of the trigger tends to squeeze the grip too). Render thread.
    bool GrenadeBusy(bool triggerHeld);
    //! Game thread, once a frame (HandAimHook): fires a released grenade (the real throw)
    void GameTick();
    //! Render thread, once per Present: logs the flight of the last thrown grenade (research / validation).
    void OnPresent();
    void SetEnabled(bool on);
    bool Enabled();
    bool Set(const std::string& key, float v);
    std::string Config();
    std::string Stats();             //!< heartbeat
    //! Research: log the position of the next thrown projectile every `ms` for `seconds` (0 = off).
    void Track(float seconds, int ms);
    //! A hand's velocity now (0 = left, 1 = right; the fastest over the last `peakwin`, as at a release), in world
    //! axes as the hand is drawn. Any thread. `speedXr` = its magnitude in the XR space. False without samples.
    //! GrabHand's throw uses it.
    bool HandVelocity(int hand, float world[3], float* speedXr = nullptr);
    void PlayerVelocity(float world[3]);   //!< the player's own velocity (m/s, world), as the grenade inherits it
    float Cfg(const char* name);           //!< a throwcfg value by name (gain, maxspeed, inherit...)
}
