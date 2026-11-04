#pragma once
#include <string>

//! Where the game's CPU time goes ([Logging] ProfileRenderPasses, off by default), written to the log every 5 s:
//! - every thread of the game's process with its CPU time in the window (QueryThreadCycleTime: cores, 1.00 = one core
//!   busy the whole time), labelled where the mod knows it (the render thread that calls Present, the threads that
//!   record the render tasks, the OpenXR frame thread, the process's first thread);
//! - for the busiest few (at most 6 using at least 0.15 of a core), a sampled profile: about 500 times a second each is
//!   suspended for the length of GetThreadContext and a copy of the top of its stack, then resumed -- everything else
//!   (module and function lookups, the log) runs after it resumed, so no lock it holds is ever waited on. Self time by
//!   module and by function (the function's start from its module's unwind table: exe+0x..., comparable between runs
//!   and with ENGINE.md), the share spent in system calls (waiting, or in the kernel), and for the time spent in the
//!   driver or Windows, the nearest caller in the game or the mod (the first return address into them on the stack).
//! Nothing runs, and no thread is touched, with profiling off.
namespace CpuProfile
{
    void SetEnabled(bool on);     //!< from the ini (ProfileRenderPasses / DebugLogging); starts the sampler the first time
    void NoteTaskThread();        //!< PassProfile, on a thread running a render task (the labels): cheap after the first
}
