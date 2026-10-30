#pragma once
#include <string>

//! The game's pre-rendered videos (Bink 2, base\video\*.bk2: a mission's opening, the intro). The game plays them with
//! bink2w64.dll, outside the player's state machine: no player state runs, nothing simulates, and for the mod that is
//! a menu (they go to the screen in the room, ScreenMode). But the game's own prompt there is "F Skip" -- the interact
//! key held -- and a menu's A is ENTER, so a video could not be skipped from the controllers.
//! The exports that open, decode and close a video are hooked: a video is PLAYING while one is open and its frames
//! still advance (a paused or finished one does not count).
namespace BinkVideo
{
    //! Hooks bink2w64.dll if it is loaded; safe to call again (from Present) until it is.
    void Install();
    bool Installed();
    //! [Advanced] SkipIntroVideos: the start-up videos end on their first frame.
    void SetSkipIntros(bool on);
    //! A video is on screen now: open, and a frame decoded in the last half second.
    bool Playing();
    //! The video playing (its file name), or empty.
    std::string Current();
    //! Heartbeat line.
    std::string Status();
}
