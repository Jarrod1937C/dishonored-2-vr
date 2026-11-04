#pragma once
#include <string>

struct IDXGISwapChain;

//! Says WHERE the game's render thread is when it stops presenting. The user's Quest 3 froze with the
//! hourglass for 33-169 s and the log only showed silence: the mod's whole loop hangs off the game's Present, so a
//! render thread stuck anywhere -- in the game's code, or in an OpenXR call the runtime does not return from -- leaves
//! nothing behind. A thread of its own watches the Present heartbeat; after 2 s without one it logs what the render
//! thread is doing (the OpenXR call it is in, if any, from the marks below; its instruction pointer and the return
//! addresses on its stack, sampled with the thread suspended for microseconds), which window has the foreground, the
//! session state, and the same again 10, 30 and 90 s on; and when Present comes back, how long it was gone.
//! DVR_watchdog.txt (game folder) is rewritten twice a second, so a test can read it while the render thread is stuck.
namespace Watchdog
{
    void Install();                           //!< starts the watching thread (from the init thread)
    void OnPresent(IDXGISwapChain* sc);       //!< every Present, first thing (the render thread registers itself)
    void SetXrState(int state);               //!< the OpenXR session state, as XRSession learns it
    void RegisterFrameThread();               //!< the OpenXR frame thread (XRSession's pump) registers itself
    //! RAII: "this thread is inside <what>" (a static string), for the stall report. A no-op on threads not registered.
    struct Mark
    {
        explicit Mark(const char* what);
        ~Mark();
        int slot; const char* prev; long long prevSince;
    };
    //! vr stall <ms>: the render thread sleeps that long at the next Present -- a game hitch on demand (tests)
    void Stall(int ms);
    std::string Status();                     //!< the heartbeat line
    unsigned long RenderThreadId();           //!< the thread that calls Present (0 before the first); CpuProfile's labels
    unsigned long FrameThreadId();            //!< the mod's OpenXR frame thread (0 = none)
}
