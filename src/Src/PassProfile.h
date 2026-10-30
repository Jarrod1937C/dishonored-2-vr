#pragma once
#include <string>

struct IDXGISwapChain;

//! The frame's cost, pass by pass ([Advanced] ProfileRenderPasses, off by default), written to the log every 5 s:
//! for each of the engine's named render passes (ARK_RENDERPASS_*, ENGINE.md "Render passes"), separately for the left
//! eye's view, the right eye's and the other views (reflections, cube maps, shadow-only): GPU time (average and the
//! slowest single task), task count, CPU time spent recording it, draws and dispatches submitted, the GPU's pipeline
//! statistics (primitives in and rasterised, pixels shaded, compute threads) and the render target it drew into.
//! With it: GPU and CPU time per Present, the mod's own GPU work at Present, VRAM, the render settings (once), the
//! slowest frame of each window pass by pass, and every 30 s one frame task by task in GPU order with the gaps
//! between them (GPU idle). What to optimise is decided from this, not guessed.
//!
//! How: the two task executors (exe+0x2C5D50, exe+0x2C5F60) call each task's function at task+0xD8 with the task's
//! context wrapper at task+0x108 (its ID3D11DeviceContext at +0): around that call GPU timestamps and a pipeline
//! statistics query go into the same context (deferred contexts record them into the pass's own command list), read
//! back a few frames later; GpuTrace's draw / dispatch hooks count into the task running on their thread.
namespace PassProfile
{
    constexpr int kSections = 3;    //!< the mod's own work at Present: 0 the whole hook, 1 StereoTaa, 2 overlay + VR submit
    void SetEnabled(bool on);       //!< from the ini, before Install
    void Install();                 //!< hooks only when enabled
    bool On();
    void OnPresent(IDXGISwapChain* sc);
    void BeginSection(int s);       //!< render thread, around the mod's own work at Present (immediate context)
    void EndSection(int s);
    //! GpuTrace's draw / dispatch hooks: counted into the render task running on this thread, if any
    void NoteDraw(unsigned elements, unsigned instances, bool indirect);
    void NoteDispatch(unsigned x, unsigned y, unsigned z, bool indirect);
    std::string Status();
}
