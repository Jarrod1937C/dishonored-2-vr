#pragma once
#include <string>
#include <vector>

//! Controller profiles ([Controls.Controllers] ControllerBindings, 2026-11-01): <game>\VRControllerBindings\*.json,
//! one per controller, each holding the settings that depend on the controller -- the button scheme (Layout), the
//! grips' press points, finger tracking, the pointing angle, the quick slots' hold time, the trackpads' press / release
//! force and centre. Any setting of DishonoredVR.ini may be given there; a profile's value wins over the ini's.
//!
//! A profile:
//!   { "name": "Valve Index", "match": [ "/interaction_profiles/valve/index_controller" ],
//!     "settings": { "Layout": "index", "GripThresholdLeft": 0.6, ... }, "about": { ... } }
//! `match`: the OpenXR interaction profiles it is for (what the runtime reports it bound: XRSession's "controllers
//! bound as" log line).
//!
//! Which one: ControllerBindings = auto (the default) takes the profile whose `match` has the controller the runtime
//! reports, once it reports one (the profile in use until then: the first file, by name); a file name (index, without
//! .json) forces that one. A name that is not there, or no profile for the controller, = the first file there is.
//! No folder or no file: the ini's own values. The VR options tabs save a profile's settings back into its file.
namespace ControllerBindings
{
    //! InitThread, after Settings::Load: reads the folder, applies the chosen profile (before anything is told a setting)
    void Load();
    //! Told the bare names of settings a profile changed after startup (Main.cpp's ApplySettings)
    void SetApply(void (*apply)(const std::vector<std::string>& changed));
    //! XRSession, when the runtime reports which controllers it bound (any thread)
    void OnProfile(const std::string& left, const std::string& right);
    //! ControllerBindings changed (the VR options tabs): choose again
    void Reselect();
    //! The profile in use ("" = none) and the files there are (names without .json), for the VR options tabs
    std::string Active();
    std::vector<std::string> Names();
    //! The profile in use holds this setting (bare name, any case): it is saved there, not in the ini
    bool Owns(const std::string& bare);
    //! Writes these (bare name, value as the ini would have it) into the profile's file, in place. false + err = not written
    bool Write(const std::vector<std::pair<std::string, std::string>>& values, std::string& err);
}
