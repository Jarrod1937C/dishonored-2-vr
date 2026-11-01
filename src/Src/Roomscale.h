#pragma once
#include <string>

//! Roomscale ([Controls.Body] Roomscale, 2026-11-01): walking in the room walks the character. Without it the view is
//! the game's eye plus the head's offset from the XR reference (ViewMatrixHook), so a player who walked away from where
//! the view was centred had a view metres from the character: the game's body, its hands' aim, the guards' sight and
//! the collision all stayed behind until a recentre.
//!
//! The character is moved by the game's own physics, so walls, props, steps and slopes stop or carry it as they do
//! the stick. The player's movement is a Havok character proxy (arkCharacterProxy, a hknpCharacterProxy; vtable
//! exe+0x1AD26A0) that the player's physics (exe+0x1B4760, from arkCpntPlayerPhysics' update) gives the tick's
//! velocity at proxy+0xB0 and then integrates with collision: arkCharacterProxy::integrate exe+0x85A40(proxy,
//! stepInfo { start, end, delta +0x08, 1/delta +0x0C }, ...). Its position is proxy+0x80 (the transform's translation),
//! read back after it with the velocity. Hooked: the head's horizontal offset from the game's eye (published by
//! ViewMatrixHook each view) is added to that velocity for the tick, as much as the tick may take (MaxSpeed); after
//! the integration the part of it the character actually moved (along the request, the game's own part taken out) is
//! taken back out of the velocity -- the game's movement, animation and noise see only its own -- and handed to the
//! view, which moves its XR reference by it: the view does not move, the character catches up under it.
//!
//! A deadzone keeps small leans from moving the character: it starts following when the head is `deadzone` from the
//! game's eye and follows until it is under the head again (2 cm). Off in takedowns / animated views, inside a host,
//! in cutscenes and menus (no fresh offset from the view: the view only publishes in play).
//!
//! The wall fade ([Controls.Body] WallFade): the game stops the character at a wall, not the real head, which can walk
//! on into it (or lean into it, roomscale or not). Pushing the view back out is what makes people sick, so the headset
//! darkens instead: each game frame a ray from the game's own eye (always in the open) to the head's position in the
//! view (Collision::WorldRay: walls, floors, doors -- not props or characters); the head this far past the surface the
//! picture is: 5 cm before it, still clear; 10 cm through, all dark blue (WallFadeColor: black looked like the game
//! had stopped). Fades in 0.1 s, clears in 0.25 s.
namespace Roomscale
{
    void Install();
    void SetParams(bool on, float deadzone, float maxSpeed);
    void SetWallFade(bool on, const float srgb[3]);   //!< the colour faded to, sRGB 0..1
    void WallFadeColor(float srgb[3]);
    //! ViewMatrixHook, every play view: the game's eye and the head's position in the view (world)
    void PublishHead(const float gameEye[3], const float head[3]);
    //! game thread, once a frame (HandAimHook, after Collision::Tick): the wall fade's ray
    void Tick();
    //! VRRuntime, each submitted frame: how much of the picture the wall fade leaves (1 = clear, 0 = only its colour)
    float WallFade();
    bool Enabled();
    //! ViewMatrixHook, play's view (left / mono eye): the head's horizontal offset from the game's eye (world, m)
    void Publish(const float offset[2], bool allowed);
    //! ViewMatrixHook: the world distance the character was moved by roomscale since the last call (true if any)
    bool TakeMoved(float moved[2]);
    //! the XR reference was taken again (recentre): nothing pending, not following
    void Reset();
    std::string Stats();
}
