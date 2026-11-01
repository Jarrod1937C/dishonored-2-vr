#pragma once
#include <string>

struct ID3D11DeviceContext;

//! Screen-space sun shadows ([Display] ScreenSpaceShadows, on by default), and a log of the game's shadow settings.
//!
//! The game resolves the sun's cascaded shadow maps into a screen-space mask in its SCREENSHADOWS pass, before the
//! forward passes light the scene with it. At the END of that pass (the mask still bound as its render target), each
//! pixel's position is rebuilt from the scene's depth and a short ray is marched towards the sun through the depth
//! buffer: where something on screen is in the way, the mask is lowered (blend MIN, its first channel only). What is
//! on screen gets a sharp contact shadow whatever the shadow quality; what is off screen keeps the game's own shadow
//! maps (so a low shadow setting plus this is the intended compromise). The sun's direction comes from cascade 0's
//! render matrix (exe+0x3309020). Only the sun: the lamps are shaded inside the game's own lighting shaders.
//!
//! The shadow settings log: every shadow-related cvar (and the quality presets) at start and whenever one changes
//! (polled once a second on a thread of its own), so the game's Low / Ultra presets can be told apart.
namespace ScreenShadows
{
    //! from the ini, before PassProfile::Install. `lengthM`: how far the ray goes towards the sun (m); `steps`: samples
    void SetParams(bool on, float lengthM, int steps, float strength);
    bool Wanted();
    //! PassProfile's executor hook: the end of a SCREENSHADOWS task of a main view (`eye` 0/1, -1 mono)
    void AfterMask(ID3D11DeviceContext* c, const void* view, int eye);
    //! the shadow settings log (a thread): from Main once the cvars are known
    void StartSettingsLog();
    std::string Status();
}
