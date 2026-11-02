#pragma once
#include <string>

//! Records where the process dies. A mod that hooks the renderer and the game's entity system gets blamed for
//! every crash, and "it closed the game" is not something that can be debugged from a screenshot.
//!
//! On a crash -- and only then -- it writes crash_log-<yyyy-mm-dd_hh-mm>.log next to the game's exe (else in
//! %LOCALAPPDATA%\DishonoredVR), with everything needed to find the cause: the exception, the registers, the code at
//! the faulting instruction, the call stack unwound through the modules' unwind data, the stack's contents, the loaded
//! modules (with their timestamps, to resolve the addresses later), what the mod knew (version, system, GPU, headset,
//! map, settings) and the log's last lines; and a minidump of the same name (.dmp) that a debugger opens. Nothing of
//! it runs during play: the handlers only test an exception code, and the report is written by a thread that sleeps
//! until a crash wakes it (it has its own stack: a stack overflow or a corrupt stack can still be reported).
//!
//! What counts as a crash: an exception nobody handles (the unhandled-exception filter, kept first even when the game
//! sets its own -- that one is still called after ours), the codes that end the process whoever handles them (heap
//! corruption, a stack overflow, a fast fail raised as an exception, a non-continuable exception) and, in the mod's
//! own code, abort / std::terminate / a pure virtual call / an invalid CRT parameter. Faults the game catches itself
//! are not crashes; the first few are still noted in DishonoredVR.log (with the log on) as a hint.
namespace CrashLog
{
    void Install();     //!< DllMain: the handlers and the report's thread
    void Prepare();     //!< the init thread (outside the loader lock): dbghelp for the minidump, the filter kept first
    //! Note what the mod is doing, so the report says which subsystem was live.
    void SetPhase(const char* phase);
    //! "<module>+0x<rva>" for an address (or the absolute address outside any module)
    std::string Where(const void* addr);

    //! What the report lists about the session (copied now: nothing is built at the crash)
    enum Info { kMod, kSystem, kGpu, kRuntime, kHeadset, kMap, kRender, kSettings, kInfoCount };
    void SetInfo(Info what, const std::string& text);

    //! The newest crash report's file name ("" = none), and copying it (and its minidump) to the desktop: `result` says
    //! what was done, in a few words.
    std::string LatestReport();
    bool SaveLatestToDesktop(std::string& result);
}
