#pragma once
#include <string>
#include <sstream>

//! Synchronised stereo: both eyes rendered from the SAME simulation instant. The game renders one view per frame; to
//! give each eye its own view without the other eye's image being one frame of world motion older (the ghosting of
//! alternating-eye stereo), the game's own frame (idGameLocal::RunFrame, exe+0x8ACDF0) is held on every second frame,
//! so two consecutive frames render one world state from the two eyes.
//!
//! Tools: `vr simlog <n>` logs n frames (real time, held, the game clocks, the player's origin) to DVR_simlog.txt;
//! `vr syncdbg <mask>` (A/B and tests, see StereoSync.cpp); `vr uistack 1` logs the game's UI stack when it changes.
namespace StereoSync
{
    //! The mode itself (`vr stereo 2`): ViewMatrixHook builds the LEFT eye's view on a frame that simulated and calls
    //! LeftBuilt(); the next RunFrame is then held (no simulation, only the game's Draw) and its view is the RIGHT eye
    //! of the same instant (RightBuilt()). Nothing is held unless a left view was just built, so menus, loading screens
    //! and anything that is not a stereo play view run as before.
    void SetActive(bool on);
    bool Active();
    void LeftBuilt();
    void RightBuilt();
    bool PendingRight();        //!< a left view was built and the next frame will be held for the right one
    //! May the view being built start a pair? Only in play (not paused, no menu, no menu screen, not right after any of
    //! those changed or after a hitch): otherwise the view is a mono one (one picture for both eyes, submitted at once).
    bool PairWanted();
    void CancelPending();       //!< a flat (menu screen) view: no right view follows the last left
    int Debug();                //!< vr syncdbg (A/B and tests)
    void Install();
    void OnPresent();
    bool Command(const std::string& what, std::istringstream& ss);
    std::string Status();
    unsigned long long Tick();      //!< game frames run (advanced) so far
    unsigned long long Frames();    //!< RunFrame calls so far (held ones too)
    //! Will the game's next frame simulate? Input read by a held frame is lost (the engine turns the frame's mouse into
    //! a user command that only a simulated frame consumes): mouse motion is sent only before a frame that runs.
    bool NextFrameRuns();
    bool LastFrameHeld();       //!< the latest RunFrame call was held (on RunFrame's thread: the frame being built)
    //! [Advanced] Stereo*Reuse (experimental, set before Install): passes the right eye's frame skips, using what the left
    //! eye's frame of the same tick just did: the ocean's simulation, the reflection probes' relighting, the decals on
    //! NPCs, and the texture streamer's uploads (once per pair instead of once per frame). Not the shadow maps: see
    //! StereoSync.cpp.
    struct ReuseFlags { bool ocean = false, reflections = false, decals = false, streaming = false; };
    void SetReuse(const ReuseFlags& f);
}
