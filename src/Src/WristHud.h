#pragma once
#include <string>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11BlendState;
struct ID3D11DepthStencilState;
struct IDXGISwapChain;
class XRSession;

//! The game's HUD (health, mana, the equipped item and its count: the cluster in the top-left corner) as a watch on the
//! BACK of the LEFT wrist, shown when the back of the wrist faces the eyes and is looked at.
//!
//! Capture: the game draws its 2D interface straight into the backbuffer (measured with `vr hudprobe`: ~45 quads per
//! frame from one deferred context, premultiplied alpha ONE/INV_SRC_ALPHA, stencil-masked arcs). Every such draw is
//! issued a second time, just BEFORE the game's own, into a transparent texture of the backbuffer's size: same
//! shaders, same depth-stencil buffer (the arcs' masks) with its writes switched off, the game's blend for colour and
//! "over" for alpha. The texture then holds the interface alone, premultiplied, and the game's frame is untouched.
//! Draws with no pixel shader (stencil masks) or no blending (the scene's own full-screen copy) are not repeated, and
//! neither is anything drawn from inside the mod's Present (the ImGui reticle).
namespace WristHud
{
    //! From GpuTrace's draw hooks, before anything else (probe only).
    void OnDraw(ID3D11DeviceContext* ctx, int kind, int drawType, unsigned count);
    //! From GpuTrace's draw hooks: true = the caller issues the draw once more right now (into the HUD texture), then
    //! calls MirrorEnd with the same token. `kind` 0 = immediate, 1 = deferred context.
    struct Mirror
    {
        ID3D11RenderTargetView* rtv[8] = {}; ID3D11DepthStencilView* dsv = nullptr;
        ID3D11BlendState* bs = nullptr; float factor[4] = {}; unsigned sampleMask = 0;
        ID3D11DepthStencilState* ds = nullptr; unsigned ref = 0;
        //! The interface is lifted out of the picture (ScreenMode::LiftUi, the power wheel's panel): the caller does
        //! NOT issue the game's own draw after MirrorEnd -- the interface exists only in the texture.
        bool lift = false;
    };
    bool MirrorBegin(ID3D11DeviceContext* ctx, int kind, Mirror& m);
    void MirrorEnd(ID3D11DeviceContext* ctx, Mirror& m);
    //! The letterbox of a cutscene. The game renders the whole frame and its INTERFACE then paints the
    //! top and bottom bands opaque black (measured with `vr hudshot` on the Outsider's shrine: rows 0-580 and 1770-2348
    //! of 2348, two DrawIndexed of 39 indices, Scaleform's vertex shader with an untextured fill -- the only untextured
    //! fills into the backbuffer in that frame). In a headset the bars cut the world above and below the eyes; while
    //! `SetDropBars` is on (PlayerState: a cutscene, not on the menu screen) such draws are not issued. From GpuTrace's
    //! draw hooks: true = skip the game's draw.
    bool DropDraw(ID3D11DeviceContext* ctx, int kind);
    void SetDropBars(bool on);
    void SetBarsMode(int m);                  //!< `vr cinebars 0|1`: 1 (default) removes the letterbox in a cutscene
    int BarsMode();

    void BeginPresent(IDXGISwapChain* sc);    //!< top of the mod's Present: the frame's interface is complete
    void EndPresent();                        //!< bottom of the mod's Present: clear for the next frame
    void RequestProbe(int frames);            //!< `vr hudprobe [frames]`
    void RequestShot(const std::string& tag); //!< `vr hudshot [tag]`: the HUD texture as a PNG (with alpha)

    void SetCapture(bool on);                 //!< `vr hudcap 0|1`
    bool Capture();
    //! The HUD texture of the frame being presented (render thread, during the Present), or null. `gamma`: its colour is
    //! gamma-encoded and premultiplied in gamma space (the game draws its interface through a UNORM view), so a reader
    //! that wants linear light decodes it (XRSession's wrist and panel shaders do).
    ID3D11ShaderResourceView* Texture(unsigned& w, unsigned& h, bool* gamma = nullptr);
    std::string Status();                     //!< heartbeat line

    //! The watch. Once per XR frame, after the poses are synced and before the frame is submitted: decides whether the
    //! back of the left wrist is turned to the eyes and being looked at, and if so hands XRSession the layer (the
    //! interface's health/mana/item cluster, cropped, on the wrist, upright to the head, faded in and out).
    //! `inMenu`: the controls are in menu mode (nothing to show; the game's own menus are full-screen).
    void OnXrFrame(XRSession& xr, bool inMenu);
    void SetWatch(int on);                    //!< `vr wristhud 0|1`
    //! The health / mana / item cluster's square in the HUD texture (tw x th, px: x0 y0 x1 y1) while the watch holds the
    //! game's HUD on against the player's own setting (contextual / off): the watch shows it, so the HUD's panel leaves
    //! it out (ScreenMode, HudSkipEmpty). False when the player's own HUD setting is always-on, or the watch is off.
    bool ClusterHole(unsigned tw, unsigned th, float r[4]);
    int Watch();
    bool Set(const std::string& key, float v);   //!< `vr wristcfg <key> <value>`
    std::string Config();
    std::string WatchStatus();                //!< heartbeat line
}
