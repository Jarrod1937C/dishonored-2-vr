#pragma once
#include <string>

//! The comfort vignette ([Controls.Comfort] MovementVignette, 0 - 1): moving with the stick, the edges of the view go
//! dark -- the faster the character goes, the further in -- and open again as it slows. Your own walking in the room,
//! leaning and turning the head do not count, only the movement the game makes (the stick, Blink, falling).
//!
//! The speed is the character proxy's own (Roomscale's hook on arkCharacterProxy::integrate, exe+0x85A40): each physics
//! tick, how far the proxy moved across the floor, less the part roomscale asked of it (the head's), over the tick's
//! time. Drawn by the eye blit (XRSession's blit shaders), round each eye's own forward direction: no layer, no extra
//! pass, the same picture the compositor gets.
namespace Vignette
{
    void SetStrength(float s);                      //!< 0 = off, 1 = strongest (the setting)
    float Strength();
    //! the physics' thread (Roomscale's hook), each tick of the player's proxy: the game's own horizontal move (m) in `dt` s
    void OnStep(float dx, float dy, float dt);
    //! The XR thread, once per submitted frame: the vignette for it -- the clear radius and where it is black (angles off
    //! each eye's forward, radians). False = none this frame.
    bool Frame(float& inner, float& outer);
    std::string Status();
}
