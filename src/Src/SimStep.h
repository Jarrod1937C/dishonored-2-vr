#pragma once
#include <string>

//! The game's simulation step at the headset's refresh ([Performance.FrameRate] SimStepAtRefresh, hidden, default 1).
//!
//! The game moves its world in fixed steps of 1/120 s (ENGINE.md 4.5: the game clocks, exe+0x59990): each game frame
//! takes as many whole steps as real time has passed, n = round(lag * 120), and physics (arkPhysicsStep exe+0x97BC0),
//! the player's movement and the animations advance by them. At the headset's 90 frames a second that is 1, 1, 2 steps
//! -- 8.3, 8.3, 16.7 ms of motion in frames 11.1 ms apart: the camera and everything moving judder at 30 Hz, a lateral
//! move most of all (reported 2026-11-04: "a solid 90, but lateral motion is jittery"; measured 2026-11-04 on a door's
//! animation, steps of 1/120 and 2/120 s in turn). Set to the headset's refresh, every frame takes one step of exactly
//! a frame. Both of the clock's 120s are changed: the step it adds (1/120 s, the float at exe+0x2280A54) and the rate
//! it counts steps with (the 120.0 at exe+0x1AC9C28, read at exe+0x59E0D -- shared with other code, so that one
//! instruction is pointed at a float of the mod's own instead). Havok is given the frame's time and its own substep
//! count (round(dt * 120)), so it simulates the same time either way. Back to 120 when the headset is not running.
namespace SimStep
{
    void SetEnabled(bool on);
    void OnPresent();          //!< render thread, every frame: follows the headset's refresh
    std::string Status();
}
