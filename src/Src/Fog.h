#pragma once
#include <string>

struct ID3D11DeviceContext;
struct IDXGISwapChain;

//! A cheap distance fog of the mod's own ([Display] CheapFog), for when the game's fog is off ([Display] Fog = 0, and not
//! in a FogMaps map): distant hills and buildings fade into the colour of the sky near them, so the background blends
//! in again, without the game's low-resolution fog and its blocks. Works with any anti-aliasing (TAA, FXAA, none).
//!
//! Where (from the frame's pass order, measured with ProfileRenderPasses 2026-10-31): the opaque scene is drawn by
//! FORWARD_WORLD and FORWARD_DYNAMIC into one R11G11B10 colour target with its depth buffer, then the water
//! (RENDER_WATER), then the transparent surfaces (ALPHA_LIT). At the end of FORWARD_DYNAMIC's task those two targets
//! are still bound: they are noted, with the view's projection and frame number. At the START of the ALPHA_LIT task --
//! so after the water, before glass, rain and particles -- the fog is drawn over the scene on that task's own (fresh)
//! context: the sky's colour first (two small compute passes: the pixels with no geometry, weighted towards the bottom
//! of the screen, smoothed over time), then one full-screen triangle blended into the colour target by distance. Its
//! state is put back exactly as it was before the game's own ALPHA_LIT work starts. The fog's colour is that of what is
//! ONE colour per map: the average of the sky near the horizon all the way round (64 world directions, each learned from
//! 5 s into the map as the player looks that way), settled once every direction has been seen (or 30 s in with 8 of
//! them) and kept for the map; the fog fades in then. Or a fixed colour (CheapFogColor = R,G,B, CheapFogBrightness). The sky is fogged too, as much as the far
//! distance at the horizon, thinning to none at CheapFogSkyHeight above it. Not in the main menu's 3D scene. Every 10 s the log has the colour
//! and the frame's distances (how much was sky, in the fog's range, beyond it), read back from the GPU.
//! The tasks are reached through PassProfile's hook on the render task executors.
namespace Fog
{
    struct Params { bool on = true; float start = 30.f, end = 300.f, opacity = 0.6f; bool sky = true; float rgb[3] = { 0.6f, 0.65f, 0.7f };
                    float brightness = 1.f;      //!< the fixed colour's multiplier (the scene is HDR, before the exposure)
                    float skyHeight = 10.f;      //!< deg above the horizon (with skyAuto: above the far background) where the sky's fog has thinned to none (0: sky not fogged)
                    bool skyAuto = true;         //!< the sky's fog reaches up to the top of the far background seen in the map (then thins out)
                    //! CheapFogColor = auto: one of three colours by how bright the upper sky is in the map (night, dusk, day);
                    //! cat[k] = linear rgb + brightness (<= 0: the map's own horizon brightness)
                    bool category = false; float cat[3][4] = { { 0.06f, 0.08f, 0.13f, 0 }, { 0.75f, 0.6f, 0.45f, 0 }, { 0.95f, 1.f, 0.97f, 0 } };
                    float nightBelow = 0.1f, dayAbove = 0.5f;
                    float minBrightness = 0.35f;  //!< CheapFogMinBrightness: the fog's least luminance (x the game's EngineLuminanceFactor)
                    bool followExposure = true;   //!< CheapFogFollowExposure: drawn divided by the frame's EngineLuminanceFactor, as the scene is
    };
    void SetParams(const Params& p);          //!< from the ini (before PassProfile::Install)
    bool Wanted();                            //!< CheapFog is on in the ini (the executor hook is needed)
    void SetGameFogOff(bool off);             //!< VrTweaks: the game's own fog is off now (r_skipFog set by the mod)
    void SetMap(const std::string& name);     //!< VrTweaks, at each map change: no fog in the menu; the colour learned again
    void OnPresent(IDXGISwapChain* sc);       //!< the render thread's Present: the log's readback
    //! PassProfile's executor hook, on the task's own context: the end of a FORWARD_DYNAMIC task (targets still bound)...
    void AfterOpaque(ID3D11DeviceContext* c, const void* view);
    //! ...the end of a RENDER_WATER task: its depth buffer (a copy of the scene's, with the water's surface) is noted...
    void AfterWater(ID3D11DeviceContext* c, const void* view);
    //! ...and the start of an ALPHA_LIT task (a fresh context): the fog is drawn there.
    //! Returns true when it drew (the task's context must then be finished even if the pass marks it empty).
    bool BeforeTransparent(ID3D11DeviceContext* c, const void* view);
    std::string Status();
    //! GpuTrace's shader-creation hook: a pixel / compute shader that binds the game's auto-exposure buffer is noted
    void OnShader(void* sh, const void* code, size_t len, bool cs);
    //! GpuTrace's draw / dispatch hooks, only while ExposureSearching(): the exposure buffer bound there is kept
    bool ExposureSearching();
    void AtWork(ID3D11DeviceContext* c, bool dispatch);
}
