#pragma once
#include <sstream>
#include <string>

class XRSession;
struct ID3D11Texture2D;

//! Menus on a screen in the room, and the power wheel on a panel.
//!
//! The game draws its menus (the title, pause, load, options, journal), its loading screens and its cutscenes as one
//! flat picture in screen space. Shown across the eyes like the game world, that picture covered 108 x 110 degrees and
//! went wherever the head went: the corners of a menu were 54 degrees out and could not be looked at. The precedent in
//! VR games and flat-to-VR mods is a SCREEN: a 16:9 panel at a comfortable size and distance, fixed in the room where
//! it was opened, with dark surroundings. That is what this does whenever the controls are in menu mode
//! (KbmInject::GetStats().mode, the same detection the controls use):
//!  - the engine renders that picture as the flat game would (its own camera and fov: no head pose, no 108-degree
//!    projection -- the title screen's camera is a 19-degree shot of the two characters, and it is shown as that);
//!  - the picture's 16:9 band (where every menu lays itself out in a taller frame, measured) goes on a quad layer,
//!    placed in front of where the head looks when the menu opens, level, facing the eyes; it stays there; a click of
//!    either stick brings it in front again, and it comes back by itself when it has been out of sight for a moment;
//!  - around it, the last picture of the game frozen where it was seen in the room, darkened (paused in the world),
//!    over a dim room-locked environment; after a load or with nothing to show, the environment alone.
//! Fades in, and the game fades back in from black on the way out.
//!
//! The power wheel is part of play (the game runs on, slowed): the world stays in the headset and the view follows the
//! head while the wheel is up (the servo cannot turn the game then: the mouse picks the slot), and the wheel itself --
//! the game's whole interface, taken out of the picture (WristHud's copy of the interface draws, kept out of the
//! backbuffer) -- is a panel fixed where it was opened.
namespace ScreenMode
{
    //! Render side (any thread). The main view is the flat game's: its own camera and fov (ViewMatrixHook,
    //! CameraHook) and the arms as the game has them (GpuTrace's animation suspend).
    bool FlatRender();
    //! The wheel is up: the view's rotation follows the head (ViewMatrixHook's takedown view does it).
    bool WheelView();
    //! The interface goes only into the interface texture (WristHud), for the wheel's panel.
    bool LiftUi();

    //! Once per XR frame, render thread, after the poses are synced and before anything is submitted. `bb` is the
    //! picture about to be presented; `imageFlat` that it was rendered as a flat view (its view record says so), with
    //! `imageKnown` false when there is no record. True = the frame was submitted here (the menu screen); false = the
    //! caller submits the game's picture as usual (the wheel's panel and the fade back in are already set on `xr`).
    bool OnXrFrame(XRSession& xr, ID3D11Texture2D* bb, bool imageFlat, bool imageKnown);
    //! Stereo play ([Display] StereoHud): the game's interface, lifted out of both eyes' pictures, on one panel for both
    //! eyes -- the plane of the picture it was drawn over at HudDistance, seen from where that picture was rendered:
    //! `quat`/`pos` the head's frame the game camera rendered it as (no eye turn), `tan` its tangent extents (l r u d).
    //! Render thread, in the XR frame, before the frame is submitted.
    void PlayHud(XRSession& xr, const float quat[4], const float pos[3], const float tan[4]);
    void SetHud(bool on, float dist);        //!< from the ini, before the first frame
    //! Bring the screen in front of the eyes (a stick click in a menu).
    void RequestRecenter(const char* why);
    //! The controllers just asked the game for a menu (the pause, the journal): the screen comes up now instead of
    //! when the controls recognise the menu (~10 frames of a paused game later, while the menu is already drawn across
    //! the eyes). If no menu follows within a moment, play goes on.
    void Expect(const char* what);

    void SetMode(int m);                     //!< `vr screen 0|1`: 0 = the picture across the eyes, as before
    int Mode();
    bool Set(const std::string& key, float v);   //!< `vr screencfg <key> <value>`
    std::string Config();
    bool Command(const std::string& what, std::istringstream& ss);
    std::string Status();                    //!< heartbeat line
}
