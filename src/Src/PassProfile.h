#pragma once
#include <string>

struct IDXGISwapChain;
struct ID3D11DeviceContext;

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
    //! the mod's own work at Present, GPU and CPU time each: the whole hook, StereoTaa, overlay + VR submit, and that last
    //! one step by step (some nest: the wait is BeginFrame without xrBeginFrame, the layers are EndFrame without xrEndFrame)
    enum Section
    {
        kSecHook, kSecTaa, kSecSubmit, kSecOverlay, kSecStage, kSecBegin, kSecXrBegin, kSecWrist, kSecScreen, kSecEyes,
        kSecAcquire, kSecEnd, kSecXrEnd, kSecMirror, kSecSnapshot, kSecGpuTrace, kSecTicks, kSecProfiler, kSecAfter, kSections
    };
    void SetEnabled(bool on);       //!< from the ini, before Install
    void SetSkipEffects(int mode);  //!< [Advanced] SkipDisabledEffects (experimental): 0 off, 1 SSAO, 2 SSAO + DOF; before Install
    void Install();                 //!< hooks only when enabled
    bool On();
    void OnPresent(IDXGISwapChain* sc);
    bool BeginSection(int s);       //!< render thread, around the mod's own work at Present (immediate context)
    void EndSection(int s);
    struct Scope                    //!< a section for the rest of a block (early returns included)
    {
        int s; bool on;
        explicit Scope(int sec) : s(sec), on(BeginSection(sec)) {}
        ~Scope() { if (on) EndSection(s); }
        Scope(const Scope&) = delete; Scope& operator=(const Scope&) = delete;
    };
    //! ScreenShadows' characters, for the draw hooks: 1 = a sun cascade's shadow task runs on this thread, 2 = an
    //! eye's main depth pass (CharEye 0 / 1), 0 = neither
    int CharTask();
    //! DepthPassDepthOnly (experimental): an eye's main DEPTHPASS task runs on this thread (GpuTrace's draw hooks)
    bool DepthOnlyNow();
    void SetDepthOnly(bool on);   //!< from the ini, before Install
    //! DepthPassSkipRedundantCopies: an eye's main DEPTHPASS task runs on this thread, and which one (a serial)
    bool DepthCopiesNow();
    unsigned long long DepthTaskSerial();
    void SetDepthCopies(bool on); //!< from the ini, before Install
    int CharEye();
    //! GpuTrace's draw / dispatch hooks: counted into the render task running on this thread, if any
    void NoteDraw(ID3D11DeviceContext* c, unsigned elements, unsigned instances, bool indirect);
    void NoteDispatch(ID3D11DeviceContext* c, unsigned x, unsigned y, unsigned z, bool indirect);
    //! ... and the uploads / copies: what a slow task did (the hitch lines)
    void NoteMap(int type);         //!< D3D11_MAP: 1 read, 3 read-write, 4 discard, 5 no-overwrite, 2 write
    void NoteUpdate();
    void NoteCopy();
    std::string Status();
}
