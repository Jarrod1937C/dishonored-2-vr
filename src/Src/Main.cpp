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
#include "StereoSync.h"
#include "VrTweaks.h"
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
#include "ScreenMode.h"
#include "HandAimHook.h"
#include "WeaponPose.h"
#include "HandFx.h"
#include "PlayerState.h"
#include "MeleeAim.h"
#include "Collision.h"
#include "SwordContact.h"
#include "ThrowGesture.h"
#include "GrabHand.h"
#include "PointerRay.h"
#include "GunRay.h"
#include "BinkVideo.h"
#include "Settings.h"
#include "Seeds.h"
#include "SysInfo.h"
#include "Version.h"

static void OnPresent(IDXGISwapChain* sc)
{
    Watchdog::OnPresent(sc);      // the heartbeat its thread watches (and vr stall's test hitch)
    WristHud::BeginPresent(sc);   // the game's interface for this frame is complete (and the mod's own draws start)
    static bool first = true;
    if (first)
    {
        first = false;
        LOGI("first Present on swapchain {}", (void*)sc);
        // The proxy loads before the exe's static initializers run, so the
        // idCVar objects are only valid once the game is actually rendering.
        CVars::Init();
        SysInfo::LogFirstPresent(sc);
        GpuTrace::Install(sc);   // needs the game's real device context vtable
        PlayerState::Install();  // counting stubs on the player-state vtables (what the player is doing)
    }
    if (!BinkVideo::Installed()) BinkVideo::Install();   // (normally done at init: the exe imports it)
    GpuTrace::OnPresent();
    StereoTaa::OnPresent(sc);
    StereoSync::OnPresent();
    PlayerState::OnPresent();
    MeleeAim::OnPresent();
    ThrowGesture::OnPresent();
    GrabHand::OnPresent();
    PointerRay::OnPresent();
    Commands::OnPresent();
    BodyView::OnPresent();        // vr fov auto: applied as soon as the headset gives its fov
    RenderRes::OnPresent(sc);
    Reticle::Tick();              // the game's crosshair off while the hand reticles are on
    VrTweaks::OnPresent();        // the ini's VR graphics tweaks, while the headset shows the game
    Overlay::OnPresent(sc);       // draws onto the backbuffer first...
    VRRuntime::OnPresent(sc);     // ...so the flat VR frame includes the overlay (debug aid)
    KbmInject::Tick();            // controllers -> keyboard/mouse (uses this frame's synced input)
    WristHud::EndPresent();       // cleared for the next frame's interface
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

static DWORD WINAPI InitThread(LPVOID)
{
    GameInfo::Init();
    SysInfo::LogStartup();
    if (!GameInfo::IsSupportedBuild())
    {
        // Every hook sits at an offset of the supported exe; on another build those bytes are other code. Touch nothing.
        LOGI("init: unsupported game build -- the mod stays inactive (no hooks, no VR)");
        CrashLog::SetPhase("inactive (unsupported build)");
        if (HANDLE h = CreateThread(nullptr, 0, UnsupportedBox, nullptr, 0, nullptr)) CloseHandle(h);
        return 0;
    }
    Settings::Load();
    LOGI("settings: {}", Settings::Summary());
    VrTweaks::SetEnabled(Settings::VrGraphicsTweaks());
    VrTweaks::SetAmbientOcclusion(Settings::AmbientOcclusion());
    VrTweaks::SetFog(Settings::Fog());
    KbmInject::SetSnapBlankOpacity(Settings::SnapTurnBlankOpacity());
    KbmInject::SetSnapBlank(Settings::SnapTurnBlank());
    KbmInject::SetGripThreshold(Settings::GripThreshold());
    GpuTrace::SetFingerTracking(Settings::FingerTracking());
    MeleeAim::SetBladeDamage(Settings::SwordBladeDamage(), Settings::SwordDamageScale());
    Collision::SetHands(Settings::HandCollision());
    Collision::SetWeapons(Settings::WeaponCollision());
    SwordContact::SetBodySink(Settings::SwordBodyDepth());
    Collision::SetPush(Settings::PushObjects(), Settings::PushStrength());
    ViewMatrixHook::SetPitchFix(Settings::CameraPitchFix());
    ScreenMode::SetHud(Settings::StereoHud(), Settings::HudDistance());
    Reticle::SetStyle(Settings::MinimalReticle(), Settings::ReticleOpacity());
    StereoSync::SetReuse(Settings::StereoReuse());   // before StereoSync::Install (experimental)
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
    StereoSync::Install();           // the game's frame: held on every second frame for synchronised stereo
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
