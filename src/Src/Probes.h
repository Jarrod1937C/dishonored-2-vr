#pragma once

//! Research logging for the next features ([Logging] ProfileRenderPasses only: nothing is hooked otherwise), 2026-11-04:
//!   - doors: how a door's swing is driven -- each animating openable (arkCpntOpenable's Animated state, update
//!     exe+0x74E7A0) followed for 3 s: the state, its component, the door entity's orientation, its animation component
//!     and that animation's player, every dword that changes logged per frame; and every call of exe+0x540D30 (thought
//!     to set an animation's speed);
//!   - hit reactions: each damage an NPC's hit-react component handles (exe+0x591DF0) and each hit react it plays
//!     (exe+0x593860): the target, the damage record and its declaration, who called;
//!   - objective markers: each arkEntityMarkerObjective seen projected (the marker projection exe+0x6DDA50, through
//!     ScreenMode's hook): where in the marker its world point is kept, its type, and whether the game's objective
//!     markers option (ark_ui_show_objective_markers) is on.
namespace Probes
{
    void Install();                          //!< at start, when ProfileRenderPasses is on
    void OnMarkerProj(const float* world);   //!< ScreenMode's marker projection hook, before anything else
    void OnPresent();                        //!< the 30 s summaries, the marker pool every 5 s, a pushed door's motion
    //! Collision, a hand pushing a closed door: the first 3 times (profiling only) a physics impulse is sent instead of the
    //! game's door push (20, 60, 150) and the door's rotation and velocities logged for 1.5 s. True = the push was taken.
    bool DoorImpulseTest(void* door, const float point[3], const float normal[3], int hand);
    //! SwordContact, a hand touching a standing character's body at a joint (profiling only, round 7): twice, 4 s apart, a
    //! read / write watchpoint on that joint's pose entry (the animation's, then the array's tail) for 1.5 s, and the
    //! skeleton object's pose array and remaps logged: where the pose goes to be drawn. (Any thread: queued.)
    void NpcPushTest(void* npc, const float point[3], const float normal[3], float into, int hand, int joint);
    void GameTick();                         //!< Collision::Tick, the game thread: a queued NpcPushTest is run
}
