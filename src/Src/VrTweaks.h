#pragma once
#include <string>

//! Graphics changes made only while the headset shows the game, through the main view's own render flags -- the
//! per-view bits the engine itself derives from r_skipDOF / r_skipSSAO when it sets the view up (exe+0x375075 /
//! exe+0x37508F) and the passes test when they run. No cvar is written: the game's own options (and its config file,
//! and its own backup of the DOF setting, r_skipDofBck) are never touched, so flat play is exactly as the player set it.
//!   - [Display] VrGraphicsTweaks (default 1): no depth of field. It blurs at the depth the CAMERA focuses on; in a
//!     headset your eyes focus, and the blur sits on whatever you look at. (The DOF pass tests view+0x3A37 bit 0 at
//!     exe+0x315634 / exe+0x3158F8.)
//!   - [Display] Fog (default 1): 0 = no fog in VR (r_skipFog: its low-resolution image showed as blocks in a headset).
//!     [Display] FogMaps (default "void"): comma-separated words; in a map whose name holds one of them the game's fog
//!     stays on even with Fog = 0 (the Void is made of its fog). The map's name is logged at every map change.
//!   - [Display] AmbientOcclusion (default 1): 0 = no ambient occlusion in VR. The game's own option only chooses
//!     NVIDIA HBAO+ or the game's own AO (ARK_VISUALSETTING_HBAO_PLUS): with it off, the game's AO still runs. (The SSAO
//!     pass tests view+0x3A37 bit 1 at exe+0x305BF3.)
//! An earlier version wrote cvars (r_skipDOF, r_skipLensFlares, r_SSSBlurQuality, r_textureUploadTimesliceMs) and hung
//! the game at its main menu; r_skipDOF and r_skipLensFlares are also the game's own menu options.
namespace VrTweaks
{
    void SetEnabled(bool on);               //!< VrGraphicsTweaks, from the ini before the first Present
    void SetAmbientOcclusion(bool on);      //!< AmbientOcclusion, from the ini (false = off in VR)
    //! [Display] Fog (default 1): 0 = no fog while the headset shows the game (r_skipFog, live value only).
    void SetFog(bool on);
    void SetFogMaps(const std::string& words);   //!< [Display] FogMaps, from the ini
    void SetFogMapsResolution(float share);      //!< [Display] FogMapsResolution: 1 full, 0.5 half, 0.25 quarter, 0 the game's
    //! [Display] StaticLodZoom (experimental, default 1 = untouched): the game's r_lodZoomMultiplicator ("zoom
    //! multiplicator for static LODs") held at this, checked every second
    void SetLodZoom(float z);
    //! [Display] ViewDistance: the game's view-distance level used instead of the menu's (0 very low .. 5 ultra; -1 =
    //! the game's); InstallViewDistance hooks where the game applies it (at init, before the game sets its settings up)
    void SetViewDistance(int level);
    bool GameFogOff();                      //!< r_skipFog is 1 now: the game's FOG pass does not run (Fog = 0 outside FogMaps)
    bool DofOff();                          //!< the main views' depth of field is off now (VrGraphicsTweaks, headset showing the game)
    void InstallViewDistance();
    //! The game's own options menus (main and pause) without the settings the mod holds or overrides in VR: each options
    //! section asks itself, per setting, whether to list it (vtable slot 13 of arkMenuSectionOptionVisuals /
    //! ...Gameplay); a setting the mod decides is answered "no" (VrTweaks.cpp, HideReason)
    void InstallMenuFilter();
    std::string MapName();                  //!< the loaded map's name as the game has it ("" between maps)
    void OnPresent();                       //!< follows whether the headset shows the game (render thread)
    void OnMainView(void* view);            //!< the main view being set up (ViewMatrixHook): its render flags
    std::string Status();
}
