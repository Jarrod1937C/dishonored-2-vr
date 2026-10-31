#pragma once
#include <string>

struct ID3D11DeviceContext;

//! Fixed foveated rendering ([Display] FoveatedRendering, off by default; NVIDIA RTX / GTX 16 and later only): the
//! pixels far from the centre of each eye's lens are shaded at a coarser rate (2x2, then 4x4 pixels per shading), with
//! NVIDIA's Variable Rate Shading through NVAPI (nvapi64.dll, which the NVIDIA driver installs). Geometry, depth and
//! edges stay at full resolution; only the shading of surfaces in the periphery gets coarser, where the lens blurs
//! anyway. Not eye-tracked: the full-rate circle sits on each lens's centre (the eye's projection axis).
//!
//! Applied to the passes that shade the scene at the eye's resolution (depth prepass with its alpha-tested surfaces,
//! opaque, water, transparent, emissive): the shading rate is switched on at the start of each of their tasks on the
//! task's own context and off again at its end (PassProfile's executor hook). Post-processing and shadow maps are not
//! touched.
namespace Foveation
{
    struct Params { bool on = false; float innerDeg = 35.f, outerDeg = 50.f; };
    void SetParams(const Params& p);          //!< from the ini (before PassProfile::Install)
    bool Wanted();
    //! Around a task of a foveated pass of an eye's main view (eye 0 / 1)
    bool Begin(ID3D11DeviceContext* c, const void* view, int eye);
    //! At the task's end (always, for the render size it learns; `begun` = Begin returned true: switched off again)
    void End(ID3D11DeviceContext* c, bool begun);
    std::string Status();
}
