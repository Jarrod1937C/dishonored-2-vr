#pragma once
#include <string>

struct ID3D11DeviceContext; struct ID3D11RenderTargetView; struct ID3D11DepthStencilView; struct ID3D11PixelShader;
struct ID3D11DepthStencilState; struct ID3D11BlendState;

//! Screen-space sun shadows ([Display] ScreenSpaceShadows, experimental, off by default), and a log of the game's
//! shadow settings.
//!
//! The game resolves the sun's cascaded shadow maps into a screen-space mask in its SCREENSHADOWS pass (G = the sun),
//! before the forward passes light the scene with it. Right after that resolve, at half resolution: the moving shadow
//! casters' world boxes (from the sun cascades' caster lists, exe+0x328260) are drawn into a box index; then a ray from
//! each pixel towards the sun, sampled evenly on screen, is blocked where a sample lies behind a caster's visible
//! surface (a character, marked during the eye's depth pass; or a small object) and inside that caster's box. The
//! result is upsampled by depth and MINed into the mask's sun channel. Characters and small objects can then be left
//! out of the sun's shadow maps. Only the sun: the lamps are shaded inside the game's own lighting shaders.
//!
//! The shadow settings log: every shadow-related cvar (and the quality presets) at start and whenever one changes
//! (polled once a second on a thread of its own), so the game's Low / Ultra presets can be told apart.
namespace ScreenShadows
{
    //! from the ini, before PassProfile::Install. `lengthM`: how far the ray goes towards the sun (m); `steps`: samples
    void SetParams(bool on, float lengthM, int steps, float strength);
    //! what casts them: `characters` (skinned models; their rays reach `lengthM`, a body's thickness behind what is
    //! visible blocks the sun), kept in the sun's shadow maps too or not, and contact shadows of everything else within
    //! `contactM` (a thin shell behind what is visible; 0 = none)
    void SetObjects(bool characters, bool charactersInShadowMaps, float contactM);
    //! [Display] ScreenSpaceShadowSmallObjects: moving objects under `acrossM` (their world box's diagonal) leave the sun
    //! cascades' caster lists and are drawn into the mask as boxes (0 = none); then InstallSmallObjects
    void SetSmallObjects(float acrossM);
    //! [Display] ShadowMapMinObjectSize: moving objects under `acrossM` (their world box's diagonal) are left out of the
    //! sun cascades' caster lists whatever ScreenSpaceShadows says (0 = none); then InstallSmallObjects
    void SetShadowMapMinSize(float acrossM);
    //! [Display] ShadowSunUpdateEvery (1, 2 or 4): the sun's cascades beyond the nearest redrawn every Nth frame pair
    //! (a patch of the cascade task's frame test, exe+0x32EF1D); then InstallSunUpdateRate
    void SetSunUpdateEvery(int pairs);
    void InstallSunUpdateRate();
    void InstallSmallObjects();
    bool Wanted();
    bool CharactersWanted();
    bool CharactersInShadowMaps();
    void NoteCharacterShadowDraw();          //!< GpuTrace: a skinned draw met in a sun cascade's shadow task
    //! GpuTrace's draw hooks, in an eye's main depth pass: a skinned draw is drawn once more between these, into the
    //! eye's characters' mask (depth EQUAL, no depth write)
    struct Mark
    {
        ID3D11RenderTargetView* rtv[8] = {}; ID3D11DepthStencilView* dsv = nullptr; ID3D11PixelShader* ps = nullptr;
        ID3D11DepthStencilState* dss = nullptr; unsigned ref = 0; ID3D11BlendState* bs = nullptr; float bf[4] = {}; unsigned sampleMask = 0xFFFFFFFF;
    };
    bool MarkBegin(ID3D11DeviceContext* c, Mark& m);
    void MarkEnd(ID3D11DeviceContext* c, Mark& m);
    //! PassProfile's executor hook: the end of a SCREENSHADOWS task of a main view (`eye` 0/1, -1 mono): the
    //! screen-space shadows still pending (a resolve that was the task's last draw) are drawn
    void AfterMask(ID3D11DeviceContext* c, const void* view, int eye);
    //! the shadow settings log (a thread): from Main once the cvars are known
    void StartSettingsLog();
    //! [Display] ShadowCullDistance: the game's r_shadowCullDistance held at this (m) while it differs; 0 = the game's own
    void SetCullDistance(float m);
    //! [Display] ShadowUltraTrim: at Ultra, r_shadowParallelPartialDynaUpdate and the lamps' partial-update thresholds
    //! held at High's values (they cost, and do not make the shadows sharper)
    void SetUltraTrim(bool on);
    //! [Display] ShadowLampFewerUpdates: the lamps' partial-update thresholds held at Low's (0.7 screen cover / 8 m) at
    //! any preset: fewer redraws of the moving casters in the spot / omni shadow maps (before StartSettingsLog)
    void SetLampFewerUpdates(bool on);
    //! [Display] ShadowLampCacheSlices: r_shadowCacheSliceCnt held at this (0 = the game's 12), the atlas and the cache
    //! made again by the game's own path (before StartSettingsLog)
    void SetLampCacheSlices(int n);
    //! research: the shadow casters by size every 10 s (the dynamic ones' bounding radii, the static lists' sizes)
    void InstallCasterLog();
    //! each draw of a SCREENSHADOWS task of a main view (before it). The pass: the game's resolve of its sun shadow maps
    //! into the screen's mask (the sun in G; R holds the ambient occlusion, BA the packed depth), then bilateral blurs.
    //! After the resolve (at the next draw, or the task's end) the screen-space shadows are MINed into the channels the
    //! resolve wrote, reading the depth it read
    void AtTaskDraw(ID3D11DeviceContext* c, const void* view, int eye);
    //! GpuTrace's shader creation: the resolve's pixel shaders are recognised (they read ro_screenshadows_depthbuffer
    //! and ro_lightdepthcomparemap)
    void OnShader(void* sh, const void* code, size_t len);
    std::string Status();
}
