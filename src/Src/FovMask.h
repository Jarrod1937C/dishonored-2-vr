#pragma once
#include <string>

struct ID3D11DeviceContext;

//! The out-of-view mask ([Display] OutOfViewMask, off by default: 1 was confirmed 2026-10-31 to break screen-space reflections, hair and glass; 2 = only each eye's outer side). The game renders only symmetric views, so each eye's
//! view is turned towards the centre of its lopsided native fov and rendered with the smallest symmetric fov that
//! covers it (CameraHook::EyeTans): measured on a Pimax, 8-9 % of that rectangle lies outside the eye's native fov
//! (2026-10-31), and the lens hides more inside it. All of it was shaded in every pass and never seen.
//!
//! Before the first draw of each DEPTHPASS task of an eye's main view (after the pass's depth clear), the part of the
//! main depth buffer outside the eye's native fov (plus OutOfViewMaskMargin degrees, for the compositor's
//! reprojection) is written with the NEAR plane's depth: every later depth-tested draw there (the depth prepass,
//! opaque, water, transparent, emissive) is rejected before shading. A few triangles: the render rectangle minus the
//! native fov's corners projected into it. With OutOfViewMaskLens also the lens's own hidden area inside the fov, as the
//! runtime gives it (XR_KHR_visibility_mask's hidden triangle mesh, XRVisMask). Nothing is drawn into the colour
//! targets, and the context's state is put back exactly as it was.
namespace FovMask
{
    //! from the ini, before PassProfile::Install; `lens` = the lens's hidden area too (XR_KHR_visibility_mask)
    //! `mode` 0 off, 1 everything outside the fov, 2 only each eye's outer side (left of the left eye, right of the right)
    void SetParams(int mode, float marginDeg, bool lens);
    //! [Display] OutOfViewMaskCorners: also the fov's corners outside the superellipse of this exponent (0 = none);
    //! after SetParams
    void SetCorners(float n);
    bool Wanted();
    //! PassProfile's executor hook, at a draw of a DEPTHPASS task of eye `eye`'s main view (before the draw is made):
    //! true = done for this task (drawn, or not to be: no fov yet, not the main depth buffer...), false = try again at
    //! the task's next draw (the depth buffer bound is not the scene's yet).
    bool AtDraw(ID3D11DeviceContext* c, const void* view, int eye);
    std::string Status();
}
