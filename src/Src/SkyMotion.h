#pragma once
#include <string>

struct ID3D11DeviceContext;

//! The sky's motion vectors for the game's TAA ([Display.Picture] TaaSkyMotion, hidden, on by default).
//!
//! The TAA (ENGINE.md 4.5) moves its history only by the motion vectors (the RG16F target of the depth pass), and only
//! the depth pass's geometry writes them: the sky is never drawn there, so its pixels keep the target's clear value
//! (no motion). Turning the head then drags the sky's history along with the view until the TAA's neighbourhood clamp
//! stops it: the sky and the clouds follow the head by a little and stop ("play", reported 2026-11-03; gone with TAA
//! off). At the end of each eye's DEPTHPASS task the mod draws one full-screen triangle into those motion vectors where
//! the depth is still the cleared far value (nothing drawn: the sky): the motion of a point at infinity between this
//! eye's previous view and its current one (rotation only; the TAA jitter taken out, as the engine's own are).
//! Only while the game's TAA runs; the context's state is put back exactly as it was.
namespace SkyMotion
{
    void SetEnabled(bool on);
    bool Wanted();
    //! PassProfile, at the start of an eye's DEPTHPASS task: true = watch its draws for the targets
    bool Begin(ID3D11DeviceContext* c);
    //! at a draw of that task (before it is made): true = the targets are noted (stop watching)
    bool AtDraw(ID3D11DeviceContext* c);
    //! at the end of that task: the sky's motion vectors drawn (if the targets were seen)
    void End(ID3D11DeviceContext* c, const void* view);
    std::string Status();
}
