#pragma once
#include <dxgi.h>
#include <string>

//! ImGui debug overlay drawn onto the backbuffer right before Present.
//! Toggle with INSERT. While visible, it takes over mouse/keyboard input.
namespace Overlay
{
    void OnPresent(IDXGISwapChain* sc);   //!< render thread
    void OnResize(IDXGISwapChain* sc);    //!< release RTV before ResizeBuffers
    void Shutdown();
    bool Visible();
    //! The overlay draws into an sRGB view of the backbuffer (the game's is R8G8B8A8_UNORM_SRGB): ImGui's colours are
    //! then taken as linear light and come out lighter. A colour meant as seen goes through Seen() first.
    bool SrgbTarget();
    unsigned Seen(int r, int g, int b, int a = 255);   //!< an IM_COL32 that shows as (r, g, b) on the overlay's target
    HWND Window();                        //!< the game's window (the swapchain's), null before the first Present
    //! The game window's CLIENT size in physical pixels (= the backbuffer the game renders to). A Quest eye is taller
    //! than it is wide and the render now covers its whole fov (vr fov auto), but Windows caps a window at the desktop,
    //! so the game could only get 1902x973 -- 6 px per degree vertically. This lifts the cap (WM_GETMINMAXINFO) and
    //! resizes the window, which may then extend past the screen's bottom edge; the game follows with ResizeBuffers.
    //! 0 0 = drop the override and go back to the size before the first change.
    void SetWindowClient(int w, int h);
    std::string WindowInfo();
}
