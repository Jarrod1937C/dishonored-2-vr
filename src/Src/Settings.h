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
    bool FingerTracking();                        //!< [Controls] FingerTracking, with Layout = index (XRSession, GpuTrace)
    bool SwordBladeDamage();                      //!< [Gameplay] SwordBladeDamage: the blade strikes what it touches itself (MeleeAim)
    float SwordDamageScale();                     //!< [Gameplay] SwordDamageScale: x the blade's damage
    bool HandCollision();                         //!< [Gameplay] HandCollision: the drawn hands stop at the world (Collision)
    bool WeaponCollision();                       //!< [Gameplay] WeaponCollision: the held weapons too, and at bodies (Collision)
    bool PushObjects();                           //!< [Gameplay] PushObjects: hands / weapons push loose props (Collision)
    bool PushDoors();                             //!< [Gameplay] PushDoors: a hand pushing a door opens it (Collision)
    bool KnockoutBlow();                          //!< [Gameplay] KnockoutBlow: a blow to the back of the head knocks out (MeleeAim)
    bool FinisherAnimations();
    float KnockoutSoundVolume();
    bool SkipIntroVideos();                       //!< [Advanced] SkipIntroVideos: the start-up videos end at once (BinkVideo)                  //!< [Gameplay] KnockoutSoundVolume: the knockout blow's hit sound, 0..1                    //!< [Gameplay] FinisherAnimations: 0 = the game's finishing moves and kill cam off (MeleeAim)
    bool PhysicalCrouch();                        //!< [Controls] PhysicalCrouch: crouching for real crouches the character (KbmInject)
    float PhysicalCrouchDepth();                  //!< [Controls] PhysicalCrouchDepth: m (the ini says cm) the head goes down to crouch
    bool PullToOpen();                            //!< [Gameplay] PullToOpen: grab and pull drawers / cabinets open, push to close
    bool PhysicalPickup();                        //!< [Gameplay] PhysicalPickup: items grabbed by hand, stored behind the back
    bool SwipeDials();                            //!< [Gameplay] SwipeDials: slide a hand up / down a safe dial to turn it
    bool SlideWindows();                          //!< [Gameplay] SlideWindows: slide a hand up / down a window to raise / lower it
    bool HolsterWeapons();                        //!< [Gameplay] HolsterWeapons: left trigger at the left hip draws the pistol, behind the back the crossbow
    float SwordBodyDepth();                       //!< [Gameplay] SwordBodyDepth: m (the ini says cm) the blade sinks into a body
    float PushStrength();                         //!< [Gameplay] PushStrength: the hand's effective mass, kg
    //! [Controls] GripThresholdLeft / GripThresholdRight (hand 0 / 1): how far that grip is pressed to count (KbmInject).
    //! An ini without them that still has the old GripThreshold uses that for both.
    float GripThreshold(int hand);
    bool SnapTurnBlank();                         //!< [Controls] SnapTurnBlank: black during a snap turn (KbmInject)
    bool StereoHud();                             //!< [Display] StereoHud: the game's HUD on one panel for both eyes (ScreenMode)
    float HudDistance();                          //!< [Display] HudDistance: that panel's distance, m
    bool CameraPitchFix();                        //!< [Display] CameraPitchFix: the game's neck out of the view (ViewMatrixHook)
    float SnapTurnBlankOpacity();                 //!< [Controls] SnapTurnBlankOpacity: 0..1, 1 = black
    bool MinimalReticle();                        //!< [Display] MinimalReticle: the reticle is only a dot (Reticle)
    float ReticleOpacity();                       //!< [Display] ReticleOpacity: 0.05..1 (Reticle)
    bool VrGraphicsTweaks();                      //!< [Display] VrGraphicsTweaks: no depth of field in VR (VrTweaks)
    bool AmbientOcclusion();                      //!< [Display] AmbientOcclusion: 0 = none in VR (VrTweaks)
    bool Fog();                                   //!< [Display] Fog: 0 = no fog in VR (VrTweaks)
    std::string FogMaps();                        //!< [Display] FogMaps: words of map names where Fog = 0 does not apply
    StereoSync::ReuseFlags StereoReuse();         //!< [Advanced] Stereo*Reuse, StereoStreamOncePerPair (experimental, StereoSync)
    std::string Summary();                        //!< one line for the log
}
