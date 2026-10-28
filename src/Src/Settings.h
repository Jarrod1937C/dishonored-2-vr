#pragma once
#include <string>
#include <vector>
#include "StereoSync.h"

//! The player's settings: <game>\DishonoredVR.ini, read once at startup. Every key has a default, so a missing file,
//! a missing key or a value that cannot be read all leave the mod working (and the log says which). Settings are
//! turned into the same `vr ...` commands the test channel uses, so a setting does exactly what its command does.
namespace Settings
{
    void Load();                                  //!< InitThread, before anything asks for a setting
    std::vector<std::string> StartupCommands();   //!< run at the first Present (input layout, turning)
    std::vector<std::string> PackageCommands();   //!< run when the VR package switches on in a game
    bool StatusFile();                            //!< write DVR_heartbeat.txt once a second (diagnostics)
    bool LogInput();                              //!< log every controller button change (diagnostics)
    bool Haptics();                               //!< controller vibration
    float RenderScale();                          //!< the game's render size relative to the headset's pixel density (0 = the window's)
    float ControllerPitchOffset();                //!< degrees the controllers' pointing is tilted (+ = up)
    bool VrGraphicsTweaks();                      //!< [Display] VrGraphicsTweaks: no depth of field in VR (VrTweaks)
    bool AmbientOcclusion();                      //!< [Display] AmbientOcclusion: 0 = none in VR (VrTweaks)
    StereoSync::ReuseFlags StereoReuse();         //!< [Advanced] Stereo*Reuse, StereoStreamOncePerPair (experimental, StereoSync)
    std::string Summary();                        //!< one line for the log
}
