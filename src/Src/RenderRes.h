#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <sstream>
#include <string>

//! The resolution the game renders at, decoupled from its window, the way UEVR and REFramework give an engine the size
//! the headset needs. The flat frame is drawn into each eye at its true angles (XRSession's orthoscopic blit), so the
//! right size is the eye's own pixel density over the angles the frame covers: with the default fov that is about
//! square (2266x2348 for a Quest 3 at 1824x1968 per eye), where a 16:9 window gave 1902x973 stretched 2.4x vertically.
//!
//! The engine learns its window size in two places, both from GetClientRect: its window procedure on
//! WM_WINDOWPOSCHANGED (into r_windowWidth/r_windowHeight, which drive the render size) and GetHdcClientWindowSize (the
//! render targets and the swapchain). Those two calls are answered with the target, ResizeBuffers gets it too, and the
//! window itself stays as the player has it (Windows shows the frame scaled into it). The size only leaves the engine
//! through two functions, and both see the real window while they run: the config writer (the cvars are archived; the
//! player's dishonored2Config.cfg keeps their own window size) and the window placement (applying video options would
//! otherwise resize the window to the VR size). The cursor the interface reads is mapped from the window into the
//! render size, the way the window shows the frame. The game's dynamic resolution (rs_enable, aimed at the MONITOR's
//! refresh rate) is held off while the headset shows the game: it rendered at 85% and upscaled.
namespace RenderRes
{
    void Install();
    //! From ResizeBuffers, before the real call: the size the buffers get (0 = the window's client size, DXGI's rule).
    void OnResizeBuffers(IDXGISwapChain* sc, UINT& w, UINT& h);
    //! Every Present, on the window's thread: works out the target and applies it when it changes.
    void OnPresent(IDXGISwapChain* sc);
    //! The client size the PLAYER's window has (never the target), for code of the mod that maps into the window.
    bool RealClient(int& w, int& h);
    bool Command(const std::string& what, std::istringstream& ss);
    std::string Status();   //!< one heartbeat line
    //! The headset runtime's largest swapchain image (XRSession, from xrGetSystemProperties): the render size's limit.
    void SetRuntimeLimit(int w, int h);
}
