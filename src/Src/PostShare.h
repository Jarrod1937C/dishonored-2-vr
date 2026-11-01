#pragma once
#include <string>

struct ID3D11DeviceContext;

//! Two savings in the eyes' late passes, each on by default and with no visible change:
//!
//! [Performance] SkipEmptyOutlines: the game's outline effect (EMISSIVE_ONLY: the objects to outline drawn into an R8
//! mask, PS 075df32c84f09477; then two full-screen passes over the mask, PS 2d11c3a17106ea42, and a full-screen
//! composite into the scene, PS d18f70002e2ca13a: ~0.17 ms per eye at 16 Mpx) runs every frame whether or not anything
//! is outlined. The mask draws are counted by an occlusion predicate and the three full-screen draws are predicated on
//! it (D3D11 SetPredication): the GPU itself skips them in a frame where the mask got no pixel, with no delay and no
//! read-back. With no mask draw at all in the task they are not made.
//!
//! [Performance] StereoExposureReuse: the auto-exposure (POSTPROC: a histogram of the picture, CS dc6d8bbce02a1266 and
//! 9ba51c0d902365b7, then the exposure adapted from it, CS 577946c9f0ee49c9) is measured on the left eye's picture
//! only: the right eye's frame skips the histogram and adapts from the left's (copied into its own when the two eyes
//! do not share it). Both eyes then aim at the same exposure, always; the adaptation itself still runs every frame,
//! so it is as fast as before. The first right frames (until the histogram's resource is known) are left alone.
namespace PostShare
{
    void SetOutlines(bool on);        //!< from the ini (any time)
    void SetExposure(bool on);
    bool OutlinesOn();
    bool ExposureOn();
    //! PassProfile's executor, around an eye's EMISSIVE_ONLY (what 1) or POSTPROC (what 2) task on its context
    void Enter(ID3D11DeviceContext* c, int what, int eye);
    void Leave(ID3D11DeviceContext* c);
    bool Active();                    //!< such a task runs on this thread (GpuTrace's hooks)
    bool AtDraw(ID3D11DeviceContext* c);       //!< before a draw: true = not to be made
    bool AtDispatch(ID3D11DeviceContext* c);   //!< before a dispatch: true = not to be made
    std::string Status();
}
