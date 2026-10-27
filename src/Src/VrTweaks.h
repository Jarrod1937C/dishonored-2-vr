#pragma once
#include <string>

//! [Display] VrGraphicsTweaks (ini, default on): a few of the game's graphics settings changed while the headset shows
//! the game, for speed and for how they look in a headset, and put back as soon as it does not (flat play, VR off).
//! They are a preference, not a fix: each one costs some of the flat game's look.
//!   - depth of field off (r_skipDOF): a blur at the depth the CAMERA focuses on; in a headset your eyes focus, and
//!     the blur sits on whatever you look at
//!   - lens flares off (r_skipLensFlares): a flat screen-space effect, wrong in stereo
//!   - skin blur at 23 taps instead of 92 (r_SSSBlurQuality 2): the faces' subsurface scattering, done per pixel
//!   - texture uploads 1.5 ms a frame instead of 3 (r_textureUploadTimesliceMs): at two frames per headset frame the
//!     game's own budget was half a frame while it streams; fewer hitches, textures sharpen a little later
//! The live values only: nothing of it reaches the game's config, so flat play keeps the player's own settings.
namespace VrTweaks
{
    void SetEnabled(bool on);       //!< from the ini (Settings), before the first Present
    void OnPresent();               //!< applies / restores (render thread)
    std::string Status();
}
