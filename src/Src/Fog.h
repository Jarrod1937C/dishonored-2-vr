#pragma once
#include <string>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

//! A cheap distance fog of the mod's own ([Display] CheapFog), for when the game's fog is off ([Display] Fog = 0, and not
//! in a FogMaps map): distant hills and buildings fade into the colour of the sky near them, so the background blends
//! in again, without the game's low-resolution fog and its blocks.
//!
//! Where: right before the game's TAA (temporal anti-aliasing) compute pass, which reads the lit scene
//! (ro_viewcolormap) at full resolution. Three small compute passes on the same context: the sky's colour (the average
//! of the pixels with no geometry, weighted towards the bottom of the screen, smoothed over time), then the fog into a
//! texture of the mod's own, which the TAA is handed in place of the scene for that one dispatch. The game's textures
//! are never written. The distance comes from the scene's depth buffer (learned from the draws that render into the
//! scene colour target) and the eye's own projection (the TAA's PerViewCB, cb_projectionmatrix).
//! Without TAA (r_skipTemporalAA 1) there is no fog.
namespace Fog
{
    struct Params { bool on = true; float start = 30.f, end = 300.f, opacity = 0.6f; bool sky = true; float rgb[3] = { 0.6f, 0.65f, 0.7f }; };
    void SetParams(const Params& p);          //!< from the ini
    void SetGameFogOff(bool off);             //!< VrTweaks: the game's own fog is off now (r_skipFog set by the mod)
    //! GpuTrace's draw hooks: learns which depth buffer goes with the scene colour target
    void OnDraw(ID3D11DeviceContext* c);
    //! StereoTaa, at a TAA dispatch: fog the scene into the mod's texture and bind it at `colorSlot` for this dispatch.
    //! Returns the engine's view that was there (AddRef'd; the caller puts it back after the dispatch), or null.
    ID3D11ShaderResourceView* BeforeTaa(ID3D11DeviceContext* c, int colorSlot, int viewCbSlot, int projOffset);
    std::string Status();
}
