#pragma once
#include <cstdint>

struct ID3D11DeviceContext;

//! The headset's frame pacing, measured ([Logging] ProfileRenderPasses only; nothing runs with it off). Every 5 s:
//! - how many refreshes apart the submitted frames are shown (their predicted display times), and the time between
//!   submits as a histogram (1 ms bins): pairs that snap to whole refreshes, or spread between them;
//! - how long xrWaitFrame held the frame and how far ahead its display time was when it returned;
//! - on the GPU's own timeline (a D3D11 fence signalled at three points of each frame, its completion timed by a thread
//!   waiting on it): when the GPU finished the left eye's image, when it finished the right one (everything before
//!   xrEndFrame), and when it got past xrEndFrame's own work (SteamVR's hand-over) -- each against the frame's display
//!   time, the hand-over's end also as a phase of the refresh (SteamVR releasing the GPU at the same point of every
//!   refresh = it waits for the refresh; spread out = it is doing work).
namespace PaceLog
{
    //! XRSession, after the instance is made: xrConvertTimeToWin32PerformanceCounterKHR (null when not offered)
    void SetTimeConverter(void* instance, void* convertFn);
    //! the frame thread, around an xrWaitFrame the game asked for
    void WaitBegin();
    void WaitEnd(int64_t displayTime, int64_t period);
    //! the render thread: the left image of a pair staged (its GPU work is all queued)
    void AtLeft();
    //! the render thread, right before / after the frame's xrEndFrame
    void BeforeEnd(ID3D11DeviceContext* immediate, int64_t displayTime, int64_t period);
    void AfterEnd();

    //! The game's frame sync (exe+0x3591B0, once per engine frame on the render thread) flushes the immediate context;
    //! the render thread was seen blocked there ~4 ms a pair, inside the driver. `profile` (ProfileRenderPasses): each of
    //! those Flushes timed and classed by where the GPU was when it began (still on the last frame, in SteamVR's
    //! hand-over, past it), in the 5 s report. `skip` ([Performance.SkippedWork] SkipFrameSyncFlush, hidden, default 1):
    //! that Flush is not made (the work goes to the GPU with the next Present or command list). The
    //! game's call is replaced by a call into the mod only while either is on.
    void SetFrameSync(bool profile, bool skip);
}
