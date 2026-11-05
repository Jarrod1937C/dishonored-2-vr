#include "pch.h"
#include "GameInfo.h"
#include "CrashLog.h"
#include "Watchdog.h"
#include "CVars.h"
#include "D3D11Hook.h"
#include "RenderRes.h"
#include "Overlay.h"
#include "XRProbe.h"
#include "GpuTrace.h"
#include "WristHud.h"
#include "StereoTaa.h"
#include "TaaSharpen.h"
#include "StereoSync.h"
#include "VrTweaks.h"
#include "PassProfile.h"
#include "CpuProfile.h"
#include "Fog.h"
#include "Foveation.h"
#include "FovMask.h"
#include "SkyMotion.h"
#include "PaceLog.h"
#include "Probes.h"
#include "DoorHold.h"
#include "SoftBody.h"
#include "SimStep.h"
#include "ObjectiveWatch.h"
#include "ScreenShadows.h"
#include "BodyView.h"
#include "VRRuntime.h"
#include "Commands.h"
#include "CameraHook.h"
#include "PlayerViewHook.h"
#include "ViewMatrixHook.h"
#include "XInputHook.h"
#include "PadProbe.h"
#include "KbmInject.h"
#include "Reticle.h"
#include "MenuProbe.h"
#include "VrMenu.h"
#include "ScreenMode.h"
#include "HandAimHook.h"
#include "WeaponPose.h"
#include "HandFx.h"
#include "PlayerState.h"
#include "MeleeAim.h"
#include "Holster.h"
#include "Collision.h"
#include "SwordContact.h"
#include "ThrowGesture.h"
#include "GrabHand.h"
#include "PointerRay.h"
#include "GunRay.h"
#include "BinkVideo.h"
#include "Settings.h"
#include "PostShare.h"
#include "Roomscale.h"
#include "Vignette.h"
#include "BodyDrag.h"
#include "NpcGestures.h"
#include "AiResearch.h"
#include "Undetected.h"
#include "ControllerBindings.h"
#include "TestAids.h"
#include "Seeds.h"
#include "SysInfo.h"
#include "Version.h"

static void OnPresent(IDXGISwapChain* sc)
{
    PassProfile::BeginSection(0); // [Advanced] ProfileRenderPasses: the mod's own GPU / CPU work at Present
    Watchdog::OnPresent(sc);      // the heartbeat its thread watches (and vr stall's test hitch)
    SimStep::OnPresent();         // the game's simulation step follows the headset's refresh
    WristHud::BeginPresent(sc);   // the game's interface for this frame is complete (and the mod's own draws start)
    static bool first = true;
    if (first)
    {
        first = false;
        LOGI("first Present on swapchain {}", (void*)sc);
        // The proxy loads before the exe's static initializers run, so the
        // idCVar objects are only valid once the game is actually rendering.
        CVars::Init();
        ScreenShadows::StartSettingsLog();   // the game's shadow settings, now and whenever one changes
        SysInfo::LogFirstPresent(sc);
        GpuTrace::Install(sc);   // needs the game's real device context vtable
        PlayerState::Install();  // counting stubs on the player-state vtables (what the player is doing)
    }
    if (!BinkVideo::Installed()) BinkVideo::Install();   // (normally done at init: the exe imports it)
    BinkVideo::BlankStartup(sc);  // SkipIntroVideos: the skipped intros' placeholder picture, black instead
    PassProfile::BeginSection(PassProfile::kSecGpuTrace);
    GpuTrace::OnPresent();
    PassProfile::EndSection(PassProfile::kSecGpuTrace);
    PassProfile::BeginSection(1);
    StereoTaa::OnPresent(sc);
    TaaSharpen::OnPresent(sc);
    PassProfile::EndSection(1);
    PassProfile::BeginSection(PassProfile::kSecTicks);
    StereoSync::OnPresent();
    PlayerState::OnPresent();
    MeleeAim::OnPresent();
    ThrowGesture::OnPresent();
    GrabHand::OnPresent();
    PointerRay::OnPresent();
    Commands::OnPresent();
    BodyView::OnPresent();        // vr fov auto: applied as soon as the headset gives its fov
    RenderRes::OnPresent(sc);
    PassProfile::EndSection(PassProfile::kSecTicks);
    PassProfile::BeginSection(PassProfile::kSecProfiler);
    PassProfile::OnPresent(sc);   // [Advanced] ProfileRenderPasses: GPU ms per render pass
    PassProfile::EndSection(PassProfile::kSecProfiler);
    Fog::OnPresent(sc);           // [Display] CheapFog: its colour and the frame's distances, to the log every 10 s
    Probes::OnPresent();          // research logging (ProfileRenderPasses): 30 s summaries
    Reticle::Tick();              // the game's crosshair off while the hand reticles are on
    VrTweaks::OnPresent();        // the ini's VR graphics tweaks, while the headset shows the game
    PassProfile::BeginSection(2);
    PassProfile::BeginSection(PassProfile::kSecOverlay);
    Overlay::OnPresent(sc);       // draws onto the backbuffer first...
    PassProfile::EndSection(PassProfile::kSecOverlay);
    VRRuntime::OnPresent(sc);     // ...so the flat VR frame includes the overlay (debug aid)
    PassProfile::EndSection(2);
    PassProfile::BeginSection(PassProfile::kSecAfter);
    KbmInject::Tick();            // controllers -> keyboard/mouse (uses this frame's synced input)
    WristHud::EndPresent();       // cleared for the next frame's interface
    PassProfile::EndSection(PassProfile::kSecAfter);
    PassProfile::EndSection(0);
}

//! The game build is not the one the mod was made for: say so where the player will see it. Its own thread, so the
//! game starts normally behind the box (the mod has installed nothing at this point and will not).
static DWORD WINAPI UnsupportedBox(LPVOID)
{
    const std::string& why = GameInfo::Mismatch();
    std::wstring text = L"Dishonored 2 VR is NOT active: this game build is not supported.\n\n";
    text += std::wstring(why.begin(), why.end());
    text += L"\n\nThe game will run normally, without VR. To remove the mod, delete dinput8.dll and DishonoredVR.dll "
            L"from the game folder. Details: DishonoredVR.log in the game folder.";
    MessageBoxW(nullptr, text.c_str(), L"Dishonored 2 VR", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    return 0;
}

//! A one-line text file in the game folder (a path), or empty.
static std::string ReadOneLine(const wchar_t* name)
{
    std::string s;
    if (FILE* f = _wfopen((GameInfo::GameDir() + L"\\" + name).c_str(), L"r"))
    {
        char buf[1024] = {};
        if (fgets(buf, sizeof buf, f)) s = buf;
        fclose(f);
    }
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

//! After the VR options tabs saved settings that take effect at once (VrMenu; `changed` = their bare lower-case
//! names): the modules that were given those settings at startup (InitThread, below) are given them again, and the
//! settings' commands that came out different run at the next frame. Only what changed, so a module is not touched
//! for nothing (MeleeAim::SetFinishers patches code). The setters store atomics / take their own locks.
static void ApplySettings(const std::vector<std::string>& changed)
{
    Log::SetFileEnabled(Settings::WriteLog());
    CrashLog::SetInfo(CrashLog::kSettings, Settings::Summary());
    auto has = [&](std::initializer_list<const char*> keys)
    {
        for (const char* k : keys) for (const std::string& c : changed) if (c == k) return true;
        return false;
    };
    bool fog = false; for (const std::string& c : changed) if (c.rfind("cheapfog", 0) == 0) fog = true;
    if (has({ "vrgraphicstweaks" })) VrTweaks::SetEnabled(Settings::VrGraphicsTweaks());
    if (has({ "ambientocclusion" })) VrTweaks::SetAmbientOcclusion(Settings::AmbientOcclusion());
    if (has({ "fog" })) VrTweaks::SetFog(Settings::Fog());
    if (has({ "staticlodzoom" })) VrTweaks::SetLodZoom(Settings::StaticLodZoom());
    if (has({ "disableheadbob" })) VrTweaks::SetHeadBobOff(Settings::DisableHeadBob());
    if (has({ "fpscounter", "fpscountersize", "fpscounterangle" })) VRRuntime::SetFpsCounter(Settings::FpsCounter(), Settings::FpsCounterSize(), Settings::FpsCounterAngle());
    if (fog) Fog::SetParams(Settings::CheapFog());
    if (has({ "snapturnblank" })) KbmInject::SetSnapBlank(Settings::SnapTurnBlank());
    if (has({ "movementvignette" })) Vignette::SetStrength(Settings::MovementVignette());
    if (has({ "dragbodies" })) BodyDrag::SetEnabled(Settings::DragBodies());
    if (has({ "touchtouse" })) Collision::SetUsables(Settings::TouchToUse());
    if (has({ "climbbyhand" })) Collision::SetClimb(Settings::ClimbByHand());
    if (has({ "swimstrokes" })) KbmInject::SetSwimStrokes(Settings::SwimStrokes());
    if (has({ "undetected" })) Undetected::SetEnabled(Settings::Undetected());
    if (has({ "gripthresholdleft", "gripthreshold" })) KbmInject::SetGripThreshold(0, Settings::GripThreshold(0));
    if (has({ "gripthresholdright", "gripthreshold" })) KbmInject::SetGripThreshold(1, Settings::GripThreshold(1));
    if (has({ "layout", "fingertracking" })) GpuTrace::SetFingerTracking(Settings::FingerTracking());
    if (has({ "layout", "fingertracking", "controllerpitchoffset" })) VRRuntime::SetControllerOptions(Settings::ControllerPitchOffset(), Settings::FingerTracking());
    if (has({ "padpressforce", "padreleaseforce", "padcenterradius" })) KbmInject::SetPadParams(Settings::PadPressForce(), Settings::PadReleaseForce(), Settings::PadCenterRadius());
    if (has({ "controllerbindings" })) ControllerBindings::Reselect();   // (its own changes come back through here)
    if (has({ "swordbladedamage", "sworddamagescale" })) MeleeAim::SetBladeDamage(Settings::SwordBladeDamage(), Settings::SwordDamageScale());
    if (has({ "knockoutblow" })) MeleeAim::SetKnockoutBlow(Settings::KnockoutBlow());
    if (has({ "knockoutspeed" })) MeleeAim::SetKnockoutSpeed(Settings::KnockoutSpeed());
    if (has({ "knockoutsoundvolume" })) MeleeAim::SetKnockoutSound(Settings::KnockoutSoundVolume());
    if (has({ "finisheranimations" })) MeleeAim::SetFinishers(Settings::FinisherAnimations());
    if (has({ "physicalcrouch", "physicalcrouchdepth" })) KbmInject::SetPhysicalCrouch(Settings::PhysicalCrouch(), Settings::PhysicalCrouchDepth());
    if (has({ "sitholdtime", "sitheight" })) KbmInject::SetSitting(Settings::SitHoldTime(), Settings::SitHeight());
    if (has({ "roomscale", "roomscaledeadzone", "roomscalemaxspeed" })) Roomscale::SetParams(Settings::Roomscale(), Settings::RoomscaleDeadzone(), Settings::RoomscaleMaxSpeed());
    if (has({ "wallfade", "wallfadecolor" })) { float c[3]; Settings::WallFadeColor(c); Roomscale::SetWallFade(Settings::WallFade(), c); }
    if (has({ "handcollision" })) Collision::SetHands(Settings::HandCollision());
    if (has({ "weaponcollision" })) Collision::SetWeapons(Settings::WeaponCollision());
    if (has({ "pushobjects" })) Collision::SetPush(Settings::PushObjects(), Settings::PushStrength());
    if (has({ "pushdoors" })) Collision::SetDoors(Settings::PushDoors());
    if (has({ "doorsfollowhand" })) DoorHold::SetEnabled(Settings::DoorsFollowHand());
    if (has({ "softcharacters" })) SoftBody::SetEnabled(Settings::SoftCharacters());
    if (has({ "watchobjectives" })) ObjectiveWatch::SetEnabled(Settings::WatchObjectives());
    if (has({ "pulltoopen" })) Collision::SetOpenables(Settings::PullToOpen());
    if (has({ "physicalpickup" })) Collision::SetItems(Settings::PhysicalPickup());
    if (has({ "swipedials" })) Collision::SetDials(Settings::SwipeDials());
    if (has({ "slidewindows" })) Collision::SetWindows(Settings::SlideWindows());
    if (has({ "holsterweapons" })) Holster::SetEnabled(Settings::HolsterWeapons());
    if (has({ "mouthpotions" })) KbmInject::SetMouthPotions(Settings::MouthPotions());
    if (has({ "templezoom" })) KbmInject::SetTempleZoom(Settings::TempleZoom());
    if (has({ "debuglogging" })) { PassProfile::SetEnabled(Settings::ProfileRenderPasses()); CpuProfile::SetEnabled(Settings::ProfileRenderPasses()); }
    if (has({ "debuglogging", "profilerenderpasses", "skipframesyncflush" })) PaceLog::SetFrameSync(Settings::ProfileRenderPasses(), Settings::SkipFrameSyncFlush());   // (the log file: above, every time)
    if (has({ "farreachvrcomfortoption" })) HandAimHook::SetFarReachBlink(Settings::FarReachVRComfortOption());
    if (has({ "unlockeverything", "unlockoutsidersmark", "unlockpowers", "unlockpowerupgrades", "unlockblackmarketupgrades", "unlockweapons",
              "fillammoandgadgets", "fillelixirs" }))
        TestAids::SetAutoUnlock(Settings::Unlocks());   // the current player gets the new ones in a few seconds
    {
        bool power = false;
        for (const char* k : { "unlockblink", "unlockfarreach", "unlockdarkvision", "unlockpossession", "unlockdevouringswarm", "unlockbendtime", "unlockwindblast", "unlockdomino", "unlockdoppelganger", "unlockmesmerize", "unlockshadowwalk" }) if (has({ k })) power = true;
        if (power) { const std::string p = Settings::PowerUnlocks(); TestAids::SetAutoPowers(p.empty() ? "off" : p); }
    }
    if (has({ "huddistance", "hudscale" })) ScreenMode::SetHud(Settings::StereoHud(), Settings::HudDistance(), Settings::HudScale(), Settings::HudMarkerSize());
    if (has({ "hudresolution" })) ScreenMode::SetHudResolution(Settings::HudResolution());
    if (has({ "hudskipempty" })) ScreenMode::SetHudSkipEmpty(Settings::HudSkipEmpty());
    if (has({ "skipemptyoutlines" })) PostShare::SetOutlines(Settings::SkipEmptyOutlines());
    if (has({ "stereoexposurereuse" })) PostShare::SetExposure(Settings::StereoExposureReuse());
    if (has({ "stereosunshadowreuse" })) StereoSync::SetReuse(Settings::StereoReuse());   // (on: only if it was on at start)
    if (has({ "minimalreticle", "reticleopacity" })) Reticle::SetStyle(Settings::MinimalReticle(), Settings::ReticleOpacity());
    if (has({ "foveatedrendering" })) Foveation::SetParams(Settings::Foveated());
    if (has({ "outofviewmask" })) FovMask::SetParams(Settings::OutOfViewMask(), Settings::OutOfViewMaskMargin(), Settings::OutOfViewMaskLens());
    if (has({ "screenspaceshadows" }))
        ScreenShadows::SetParams(Settings::ScreenSpaceShadows(), Settings::ScreenSpaceShadowLength(), Settings::ScreenSpaceShadowSteps(),
                                 Settings::ScreenSpaceShadowStrength());
    if (has({ "shadowsunupdateevery" })) ScreenShadows::SetSunUpdateEvery(Settings::ShadowSunUpdateEvery());
    if (has({ "shadowculldistance" })) ScreenShadows::SetCullDistance(Settings::ShadowCullDistance());
    if (has({ "shadowultratrim" })) ScreenShadows::SetUltraTrim(Settings::ShadowUltraTrim());
    if (has({ "shadowlampfewerupdates" })) ScreenShadows::SetLampFewerUpdates(Settings::ShadowLampFewerUpdates());
    if (has({ "shadowlampkeepcache" })) ScreenShadows::SetLampKeepCache(Settings::ShadowLampKeepCache());
    if (has({ "taaskymotion" })) SkyMotion::SetEnabled(Settings::TaaSkyMotion());
    if (has({ "handdepth" })) GpuTrace::SetHandDepth(Settings::HandDepth());
    if (has({ "simstepatrefresh" })) SimStep::SetEnabled(Settings::SimStepAtRefresh());
    Commands::SettingsChanged();   // turning, layout, menu screen, cutscenes, gestures, reticle, wrist HUD, cheats ...
}

static DWORD WINAPI InitThread(LPVOID)
{
    CrashLog::Prepare();             // outside the loader lock: dbghelp for the minidump, our crash filter kept first
    GameInfo::Init();
    SysInfo::LogStartup();
    if (!GameInfo::IsSupportedBuild())
    {
        // Every hook sits at an offset of the supported exe; on another build those bytes are other code. Touch nothing.
        LOGI("init: unsupported game build -- the mod stays inactive (no hooks, no VR)");
        CrashLog::SetPhase("inactive (unsupported build)");
        Log::SetFileEnabled(true);   // the ini is not read on an unsupported build: the log says why nothing happens
        if (HANDLE h = CreateThread(nullptr, 0, UnsupportedBox, nullptr, 0, nullptr)) CloseHandle(h);
        return 0;
    }
    Settings::Load();
    Log::SetFileEnabled(Settings::WriteLog());   // [Logging.Files] WriteLog = 0: no DishonoredVR.log at all
    ControllerBindings::Load();      // VRControllerBindings\*.json: the controller's settings over the ini's
    ControllerBindings::SetApply(ApplySettings);
    {
        const std::string s = Settings::Summary();
        LOGI("settings: {}", s);
        CrashLog::SetInfo(CrashLog::kSettings, s);
    }
    VrTweaks::SetEnabled(Settings::VrGraphicsTweaks());
    VrTweaks::SetAmbientOcclusion(Settings::AmbientOcclusion());
    VrTweaks::SetFog(Settings::Fog());
    VrTweaks::SetFogMaps(Settings::FogMaps());
    VrTweaks::SetFogMapsResolution(Settings::FogMapsResolution());
    VrTweaks::SetLodZoom(Settings::StaticLodZoom());
    VrTweaks::SetHeadBobOff(Settings::DisableHeadBob());
    VRRuntime::SetFpsCounter(Settings::FpsCounter(), Settings::FpsCounterSize(), Settings::FpsCounterAngle());
    VrTweaks::SetViewDistance(Settings::ViewDistance());
    Fog::SetParams(Settings::CheapFog());
    KbmInject::SetSnapBlankOpacity(Settings::SnapTurnBlankOpacity());
    KbmInject::SetSnapBlank(Settings::SnapTurnBlank());
    Vignette::SetStrength(Settings::MovementVignette());
    BodyDrag::SetEnabled(Settings::DragBodies());
    Collision::SetUsables(Settings::TouchToUse());
    Collision::SetClimb(Settings::ClimbByHand());
    KbmInject::SetSwimStrokes(Settings::SwimStrokes());
    Undetected::SetEnabled(Settings::Undetected());
    KbmInject::SetGripThreshold(0, Settings::GripThreshold(0));
    KbmInject::SetGripThreshold(1, Settings::GripThreshold(1));
    KbmInject::SetPadParams(Settings::PadPressForce(), Settings::PadReleaseForce(), Settings::PadCenterRadius());
    GpuTrace::SetFingerTracking(Settings::FingerTracking());
    MeleeAim::SetBladeDamage(Settings::SwordBladeDamage(), Settings::SwordDamageScale());
    MeleeAim::SetKnockoutBlow(Settings::KnockoutBlow());
    MeleeAim::SetKnockoutSpeed(Settings::KnockoutSpeed());
    MeleeAim::SetKnockoutZone(Settings::KnockoutAngle(), Settings::KnockoutRadius());
    MeleeAim::SetKnockoutSound(Settings::KnockoutSoundVolume());
    BinkVideo::SetSkipIntros(Settings::SkipIntroVideos());
    MeleeAim::SetFinishers(Settings::FinisherAnimations());
    KbmInject::SetPhysicalCrouch(Settings::PhysicalCrouch(), Settings::PhysicalCrouchDepth());
    KbmInject::SetSitting(Settings::SitHoldTime(), Settings::SitHeight());
    Roomscale::SetParams(Settings::Roomscale(), Settings::RoomscaleDeadzone(), Settings::RoomscaleMaxSpeed());
    { float c[3]; Settings::WallFadeColor(c); Roomscale::SetWallFade(Settings::WallFade(), c); }
    Collision::SetHands(Settings::HandCollision());
    Collision::SetWeapons(Settings::WeaponCollision());
    SwordContact::SetBodySink(Settings::SwordBodyDepth());
    SwordContact::SetBloodflyReach(Settings::SwordBloodflyReach());
    Collision::SetPush(Settings::PushObjects(), Settings::PushStrength());
    Collision::SetDoors(Settings::PushDoors());
    DoorHold::SetEnabled(Settings::DoorsFollowHand());          // (hooks the animation players' update when on)
    SoftBody::SetEnabled(Settings::SoftCharacters());           // (hooks the pose commit when on)
    ObjectiveWatch::SetEnabled(Settings::WatchObjectives());
    Collision::SetOpenables(Settings::PullToOpen());
    Collision::SetItems(Settings::PhysicalPickup());
    Collision::SetDials(Settings::SwipeDials());
    Collision::SetWindows(Settings::SlideWindows());
    Holster::SetEnabled(Settings::HolsterWeapons());
    KbmInject::SetMouthPotions(Settings::MouthPotions());
    KbmInject::SetTempleZoom(Settings::TempleZoom());
    HandAimHook::SetFarReachBlink(Settings::FarReachVRComfortOption());
    TestAids::SetAutoUnlock(Settings::Unlocks());   // [Cheats]: given to every save loaded, ~3 s into play
    if (const std::string p = Settings::PowerUnlocks(); !p.empty()) TestAids::SetAutoPowers(p);   // [Cheats.Powers]: the mark and those
    ViewMatrixHook::SetPitchFix(Settings::CameraPitchFix());
    ScreenMode::SetHud(Settings::StereoHud(), Settings::HudDistance(), Settings::HudScale(), Settings::HudMarkerSize());
    ScreenMode::SetHudResolution(Settings::HudResolution());
    ScreenMode::SetHudSkipEmpty(Settings::HudSkipEmpty());
    Reticle::SetStyle(Settings::MinimalReticle(), Settings::ReticleOpacity());
    StereoSync::SetReuse(Settings::StereoReuse());   // before StereoSync::Install (experimental)
    PassProfile::SetEnabled(Settings::ProfileRenderPasses());   // before PassProfile::Install
    CpuProfile::SetEnabled(Settings::ProfileRenderPasses());    // the game's threads' CPU time, sampled (profiling only)
    PaceLog::SetFrameSync(Settings::ProfileRenderPasses(), Settings::SkipFrameSyncFlush());   // the game's frame-sync Flush: timed / skipped
    PassProfile::SetSkipEffects(Settings::SkipDisabledEffects());
    PassProfile::SetDepthOnly(Settings::DepthPassDepthOnly());
    PassProfile::SetSkipSneakOverlay(Settings::SkipSneakOverlay());
    PassProfile::SetDepthCopies(Settings::DepthPassSkipRedundantCopies());
    PostShare::SetOutlines(Settings::SkipEmptyOutlines());      // before PassProfile::Install
    PostShare::SetExposure(Settings::StereoExposureReuse());
    Foveation::SetParams(Settings::Foveated());
    FovMask::SetParams(Settings::OutOfViewMask(), Settings::OutOfViewMaskMargin(), Settings::OutOfViewMaskLens());
    FovMask::SetCorners(Settings::OutOfViewMaskCorners());
    ScreenShadows::SetParams(Settings::ScreenSpaceShadows(), Settings::ScreenSpaceShadowLength(), Settings::ScreenSpaceShadowSteps(),
                             Settings::ScreenSpaceShadowStrength());
    ScreenShadows::SetObjects(Settings::ScreenSpaceShadowCharacters(), Settings::ShadowMapCharacters(), Settings::ScreenSpaceShadowContact());
    ScreenShadows::SetSmallObjects(Settings::ScreenSpaceShadowSmallObjects());
    ScreenShadows::SetShadowMapMinSize(Settings::ShadowMapMinObjectSize());
    ScreenShadows::SetSunUpdateEvery(Settings::ShadowSunUpdateEvery());
    ScreenShadows::SetCullDistance(Settings::ShadowCullDistance());
    ScreenShadows::SetUltraTrim(Settings::ShadowUltraTrim());
    ScreenShadows::SetLampFewerUpdates(Settings::ShadowLampFewerUpdates());
    ScreenShadows::SetLampKeepCache(Settings::ShadowLampKeepCache());
    SkyMotion::SetEnabled(Settings::TaaSkyMotion());
    GpuTrace::SetHandDepth(Settings::HandDepth());
    SimStep::SetEnabled(Settings::SimStepAtRefresh());   // (points the game clock's step rate at the mod's own float)
    ScreenShadows::SetLampCacheSlices(Settings::ShadowLampCacheSlices());
    Watchdog::Install();          // a render thread that stops presenting is reported with where it is
    Seeds::Install();
    // Test hook: <game>\DVR_usemock.txt holding a runtime manifest path selects
    // that OpenXR runtime for this process (Steam launches don't pass env vars).
    if (std::string s = ReadOneLine(L"DVR_usemock.txt"); !s.empty())
    {
        SetEnvironmentVariableA("XR_RUNTIME_JSON", s.c_str());
        LOGI("XR_RUNTIME_JSON overridden by DVR_usemock.txt: {}", s);
    }
    // Development aid: <game>\DVR_validate.txt holding the folder of the OpenXR core validation layer
    // (XrApiLayer_core_validation.json + .dll, built from the Khronos OpenXR-SDK-Source repository) loads that layer:
    // it checks every OpenXR call against the specification and reports what a strict runtime could reject.
    if (std::string s = ReadOneLine(L"DVR_validate.txt"); !s.empty())
    {
        const std::wstring& dir = GameInfo::GameDir();
        SetEnvironmentVariableA("XR_API_LAYER_PATH", s.c_str());
        SetEnvironmentVariableA("XR_ENABLE_API_LAYERS", "XR_APILAYER_LUNARG_core_validation");
        SetEnvironmentVariableA("XR_CORE_VALIDATION_EXPORT_TYPE", "html");
        SetEnvironmentVariableA("XR_CORE_VALIDATION_FILE_NAME", (std::string(dir.begin(), dir.end()) + "\\DVR_xrvalidation.html").c_str());
        LOGI("OpenXR core validation layer enabled from {} (report: DVR_xrvalidation.html, written at once; empty of messages = no violations)", s);
    }
    D3D11Hook::Callbacks cb; cb.onPresent = OnPresent; cb.onResize = Overlay::OnResize;
    D3D11Hook::Install(cb);
    RenderRes::Install();
    CameraHook::Install();
    CameraHook::SetMode(0);          // render-side hook proved ineffective for the scene; keep for stats only
    PlayerViewHook::Install();
    PlayerViewHook::SetEnabled(false);   // the local player never goes through that builder
    ViewMatrixHook::Install();
    Roomscale::Install();            // walking in the room walks the character: its character proxy follows the head
    AiResearch::Install();           // (log on only) research for the gestures at people: the AI's messages, what NPCs hear
    StereoSync::Install();           // the game's frame: held on every second frame for synchronised stereo
    ScreenShadows::InstallCasterLog();   // research: the shadow casters by size, every 10 s
    ScreenShadows::InstallSmallObjects();   // ScreenSpaceShadowSmallObjects: small moving objects out of the sun's maps
    ScreenShadows::InstallSunUpdateRate();  // ShadowSunUpdateEvery: the sun's far cascades redrawn less often
    VrTweaks::InstallMenuFilter();          // the game's options menus without what the mod decides in VR
    VrTweaks::InstallViewDistance();        // ViewDistance: the game's view-distance level, whatever its menu says
    PassProfile::Install();          // [Advanced] ProfileRenderPasses only
    if (Settings::ProfileRenderPasses()) Probes::Install();   // research logging: doors, hit reactions, objective markers
    XInputHook::Install();
    XInputHook::SetEnabled(false);       // pad emulation reaches the engine's event queue but not the player yet (see ENGINE.md)
    PadProbe::Install();
    KbmInject::SetEnabled(true);
    HandAimHook::Install();
    GunRay::Install();               // guns/crossbow: sensor, aim point, muzzle and reticle on the drawn weapon's ray
    MeleeAim::Install();             // the sword strikes along the right controller (only active with vr hand 2)
    ThrowGesture::Install();         // grenades leave with the left hand's velocity at the trigger's release
    WeaponPose::Install();
    HandFx::Install();               // effects anchored to an arm joint come out of the hand as it is drawn
    GrabHand::Install();             // a held object's throw leaves with the left hand's velocity
    Holster::Install();              // a hand put away stays empty: the game's pairing of the hands' equips is held back
    ScreenMode::InstallMarkers(Settings::HudMarkers());   // a guard's awareness marker over his head in the headset
    MenuProbe::Install(Settings::LogMenuData());   // research: the options screen's Flash traffic (for a VR options tab)
    VrMenu::Install(Settings::VrOptionsMenu(), ApplySettings);   // VR tabs in the game's Options screen
    BinkVideo::Install();            // the pre-rendered videos: A / X held skips them as in a cutscene
    // The probe goes LAST: it only feeds the overlay, and xrCreateInstance can block for seconds (measured 6.1 s with
    // Meta's runtime on a game launched from Air Link). Run first, the game kept booting meanwhile and created its
    // vertex shaders before GpuTrace could see them (4 captured instead of hundreds): no arm or weapon was ever
    // recognised, fpdraws stayed 0, the controls stayed in "menu" mode and nothing moved with the controllers.
    XRProbe::Run();
    CrashLog::SetPhase("running");
    LOGI("init done");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(self);
        wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring dir(path); dir = dir.substr(0, dir.find_last_of(L'\\'));
        Log::Init(dir);
        LOGI("DishonoredVR {} loaded", DVR_VERSION_FULL);
        // Before anything else hooks anything: a crash we cannot see is a crash we cannot fix.
        CrashLog::Install();
        CrashLog::SetInfo(CrashLog::kMod, fmt::format("Dishonored 2 VR {} (built {} {})", DVR_VERSION_FULL, __DATE__, __TIME__));
        // Do the real work outside the loader lock.
        HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        KbmInject::ReleaseAll();
        HandAimHook::Uninstall();
        WeaponPose::Uninstall();
        PadProbe::Uninstall();
        XInputHook::Uninstall();
        ViewMatrixHook::Uninstall();
        PlayerViewHook::Uninstall();
        CameraHook::Uninstall();
        VRRuntime::Shutdown(reserved != nullptr);   // non-null = the process is exiting: its other threads are gone
        Overlay::Shutdown();
    }
    return TRUE;
}
