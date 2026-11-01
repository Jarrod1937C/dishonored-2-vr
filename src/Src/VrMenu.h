#pragma once
#include <string>
#include <vector>

//! VR tabs in the game's own Options screen ([System.Startup] VrOptionsMenu, 2026-11-01): after the game's four tabs
//! (Controls, Graphics, Audio, Gameplay), one per main group of DishonoredVR.ini (VR Controls, VR Gameplay, VR Display,
//! VR Performance), System and Cheats sharing the last (VR System/Cheats: six tabs did not fit the list). A VR tab's page has the group's subgroups as its section headers and every
//! @user setting as the game's own widget (on/off stepper, choice stepper, slider), showing its value from the ini.
//! The protocol (claude/vr-options-menu-research.md, MenuProbe's log): the tab list is the AS3 array
//! optionsData.sections; picking a tab calls OnOptionSectionSelected(index), the page is optionsData.currentSection
//! { label, screenClassName, categories[], settings[] { label, categoryIndex, <widget> } } shown by the AS3 method
//! _C_ShowOptionSection; Back calls OnBackFromSettingsList(index), answered by _C_BackFromOptionSection.
//! Back from a VR page reads every widget back (selectedIndex / value); when any differs, the game's own "apply the
//! changes?" box asks (its message box, with a callback object of ours): Yes = Settings::Save (the values written into
//! DishonoredVR.ini in place; those that can change at once are used at once through `apply`, the others are labelled
//! "restart" on the page and used next time), No = dropped. Then the page closes (_C_BackFromOptionSection).
namespace VrMenu
{
    //! apply: told the bare lower-case names of saved settings that changed at once (Main.cpp gives them to the modules)
    void Install(bool on, void (*apply)(const std::vector<std::string>& changed));
    std::string Stats();
}
