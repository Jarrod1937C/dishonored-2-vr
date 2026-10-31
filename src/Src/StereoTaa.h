#pragma once
#include <d3d11.h>
#include <sstream>
#include <string>

//! The game's temporal anti-aliasing, one per eye, in alternating-eye stereo.
//!
//! TAA blends each frame with the previous one, reprojected along motion vectors. With alternating eyes the previous
//! frame is the OTHER eye's, so `vr stereo 1` used to switch TAA off -- and with it all the anti-aliasing the game has
//! (jagged edges, flickering highlights in the headset: the development notes). Here each eye gets its own TAA:
//!
//!  - its own HISTORY: the TAA compute shader (found by its resource names, ro_taahistory_read / rw_taahistory_write)
//!    reads a history texture of that eye instead of the engine's (which the other eye wrote), and what it writes is
//!    kept for that eye;
//!  - motion vectors measured against ITS OWN previous frame: the engine's "previous" view, projection, view-projection
//!    and jitter (copied from the last frame when a view is built) are replaced with those of the last frame of the same
//!    eye, in the view build (ViewMatrixHook), before anything reads them.
//!
//! Which eye a TAA dispatch belongs to is read from the GPU's own data: the view matrix in the PerViewCB that dispatch
//! uses, matched against the views ViewMatrixHook built (each tagged with its eye). Nothing is guessed from timing.
namespace StereoTaa
{
    //! What the hook before a pass decided, handed to the hook after it.
    struct Pass
    {
        int what = 0;                                  // 0 nothing, 1 = TAA dispatch with the history swapped
        int eye = -1, srvSlot = -1;
        ID3D11ShaderResourceView* savedSrv = nullptr;  // the engine's history view, restored after the dispatch
        ID3D11Resource* out = nullptr;                 // the engine's history write target (AddRef'd)
        ID3D11Texture2D* keep = nullptr;               // this eye's history, copied from `out` after the dispatch
    };
    //! From GpuTrace's context hooks, around the real call. `kind` 0 = immediate, 1 = deferred context.
    void OnDraw(ID3D11DeviceContext* c, int kind, int type, unsigned count, Pass& p);
    void OnDispatch(ID3D11DeviceContext* c, int kind, unsigned x, unsigned y, unsigned z, bool indirect, Pass& p);
    void AfterPass(ID3D11DeviceContext* c, int kind, Pass& p);
    void OnCopy(ID3D11DeviceContext* c, int kind, ID3D11Resource* dst, ID3D11Resource* src, bool region);
    //! Constant buffer writes (GpuTrace's Map / Unmap / UpdateSubresource hooks): the PerViewCB of the TAA is shadowed.
    void OnMap(ID3D11DeviceContext* c, ID3D11Resource* r, void* data);
    void OnUnmap(ID3D11DeviceContext* c, ID3D11Resource* r);
    void OnUpdate(ID3D11DeviceContext* c, ID3D11Resource* r, const void* data);
    //! From GpuTrace's CreateComputeShader hook.
    void OnComputeShader(ID3D11ComputeShader* cs, const void* code, size_t len);
    //! From ViewMatrixHook, right after the main view's matrix was built (before the engine derives anything from it).
    //! `eye` -1 = mono, or no pose.
    void OnMainView(void* view, int eye, const float viewMatrix[16]);
    void OnPresent(IDXGISwapChain* sc);
    //! True when the engine's "previous" matrices for the view built with this matrix are its own eye's last frame
    //! (GpuTrace then uses that eye's last delta for the previous pose of the draws it moves).
    bool PrevIsSameEye(const float viewMatrix[16]);
    //! Per-draw correction of the previous MVP an entity's draw carries in cb2 (cached by the engine from the last
    //! frame, i.e. the other eye): C for the view whose PerViewCB is `viewCb`, false when none applies.
    bool FixingPrev();
    //! The view matrix the PerViewCB `viewCb` held when last written (on this context if it wrote it), false if unknown.
    bool ViewMatrixOf(ID3D11DeviceContext* c, ID3D11Buffer* viewCb, float m[16]);
    //! A mesh draw's PerViewCB (VS b1, the same layout as the TAA's): shadowed from now on, so ViewMatrixOf can say which
    //! view the draw belongs to (GpuTrace places the player's arms and weapons with THAT view).
    void LearnViewCb(ID3D11Buffer* viewCb);
    //! The main view (ViewMatrixHook serial) of the latest TAA dispatch recorded since the last call, 0 = none.
    unsigned long long TakeTaaSerial();
    //! GpuTrace's command-list hooks: a deferred context's TAA view travels with its list to the immediate context.
    void OnFinishList(void* ctx, void* list);
    void OnExecuteList(void* list);
    bool PrevCorrection(ID3D11DeviceContext* c, ID3D11Buffer* viewCb, float C[16], const float* prevRows = nullptr, const float* curRows = nullptr);
    //! The previous MVP rows of an entity's draw (`prev`, 16 floats, row-major, rewritten in place) for the eye of the
    //! view its PerViewCB holds: `vr aaprevrule 1` = the engine's original previous of the draw that had these rows as
    //! its current ones one frame before (falls back to C), 0 = C only (as before). `cur` = its current MVP rows (null
    //! if the layout has none), `same` = the engine's previous equals the current (an object's first frame). True when
    //! `prev` changed.
    bool FixPrevRows(ID3D11DeviceContext* c, ID3D11Buffer* viewCb, const float* cur, float* prev, bool same);
    std::string PrevRuleStatus();
    void ResetPrevStats();

    //! 1 (default) = per-eye TAA in stereo; 0 = stereo switches TAA off (before 2026-10-14); 2 = research: TAA on with
    //! the engine's shared history (the ghosting that made us switch it off). Applies at once.
    void SetMode(int m);
    int Mode();
    bool KeepTaaInStereo();   //!< mode != 0
    //! `vr stereo`, before the switch: StereoTaa owns r_skipTemporalAA (forced to 1 in stereo only with TAA off: mode
    //! 0 or the safety net; the player's value back when that ends). `vr stereotaa` applies at once, stereo on or not.
    void OnStereoChange(bool on);

    bool Command(const std::string& what, std::istringstream& ss);
    std::string Status();   //!< one heartbeat line
}
