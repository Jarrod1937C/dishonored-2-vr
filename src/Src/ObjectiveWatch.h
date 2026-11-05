#pragma once
#include <string>

//! The objectives' directions on the watch ([Display.Hud] WatchObjectives, with WristHud): pointers on the rim of the
//! dial, the top of the dial = straight ahead of your head, the nearest objective's pointer the largest and brightest.
//! The game's own objective markers are then left off the HUD, and its option to show them is hidden from its menu.
//!
//! The game keeps its objective markers only while ark_ui_show_objective_markers is on (measured 2026-11-04: with it
//! off they are released at once, and come back when it is on) -- so it is held on while the watch shows them. Each
//! frame the game projects every marker's world point to the HUD (exe+0x6DDA50, ScreenMode's hook): an objective
//! marker is told by its class (arkEntityMarkerObjective, vtable exe+0x1B7E748; the point is the marker's +0x3C), its
//! point kept for the watch, and its place on the HUD moved to one fixed spot (the parking spot: inside the HUD, near
//! its bottom-right corner, where the game draws nothing else) which the HUD's panel leaves out.
namespace ObjectiveWatch
{
    void SetEnabled(bool on);
    bool Enabled();
    //! Shown on the watch now (enabled, the watch on, the headset running): the game's markers held on and kept off the HUD.
    bool Active();
    //! ScreenMode's marker projection hook (game thread), before the projection: true = an objective marker (its world
    //! point is kept for the watch).
    bool OnMarker(const float* world);
    //! After the projection of an objective marker, while Active(): its place on the HUD (w x h px) moved to the
    //! parking spot.
    void Park(float* out, int w, int h);
    //! The HUD's panel (ScreenMode): the parking spot's square in the HUD texture (tw x th px), to leave out.
    bool ParkHole(unsigned tw, unsigned th, float r[4]);
    struct Needle { float x, y, strength; };   //!< direction on the dial (x right, y down: up = ahead), 0..1
    //! The watch (XR thread): the objectives seen in the last second as directions from the head, nearest first (4 at most).
    int Needles(Needle out[4]);
    std::string Status();
}
