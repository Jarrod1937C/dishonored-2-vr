# Dishonored 2 VR: architecture

This document explains how the mod works, end to end, one subsystem at a time. It is written for a developer who has
never seen the code and wants to understand it or contribute to it. It describes the code as it is; where a source
comment disagrees with the code, the code is right.

Companion documents:

- [ENGINE.md](ENGINE.md): the engine reference (structures, function addresses, measurements, traps). Read it before
  touching engine internals.
- [DEVELOPMENT.md](DEVELOPMENT.md): building, the mock runtime, testing, the `vr` commands, debugging.
- [STATUS.md](STATUS.md): what works, how well it is verified, known issues.
- `package/README.txt` and `package/DishonoredVR.ini`: what the player sees and can configure.

Conventions used below:

- **`exe+0x...`** is an offset (RVA) into `Dishonored2.exe`. Every such offset belongs to ONE build of the game
  (the Steam build, file version 1.77.9.0, identified by size and PE time stamp, see [GameInfo.h](../src/Src/GameInfo.h)).
  On any other build the mod installs nothing.
- **Engine space** (id Tech convention): +X forward, +Y left, +Z up, in metres. An `idMat3` axis is stored as three
  rows: forward, left, up. An entity's world transform is its origin at `entity+0x300` and its axis at `entity+0x30C`.
- **XR space** (OpenXR LOCAL): +X right, +Y up, -Z forward, metres. [VRMath.h](../src/Src/VRMath.h) converts between
  the two (`XrToId`) and composes a head pose onto a game camera (`ComposeHead`).
- Almost every feature can be switched and tuned at run time with a `vr <name> ...` console command (see
  "Support and diagnostics"). Command names are quoted here because they are the handle you will use to test a
  subsystem in isolation, and most features keep their old behaviour behind a switch for A/B comparison.

---

## 1. Overview

### What the player gets

Dishonored 2 is a flat, first-person, keyboard-and-mouse (or gamepad) game. With the mod installed and an OpenXR
headset connected, the player:

- sees the game world in the headset, with 6DOF head tracking, either mono or in stereo (both eyes of one instant);
- aims by looking (the game's camera is turned to follow the head) and moves and acts with the motion controllers;
- sees their own arms reaching to where the controllers really are, with the weapon, power or gadget in the hand that
  holds it;
- fires the pistol and crossbow along the barrel of the gun as drawn in the hand, swings the sword by swinging the
  controller (it strikes what the drawn blade touches), throws grenades with the velocity of the hand, aims powers and
  mines with the hand, and picks up, holds and throws objects with the right hand;
- gets menus, loading screens and videos on a screen placed in the room, the power wheel on a panel, and the
  health/mana HUD as a watch on the left wrist.

### The big idea

The game is never asked to "be a VR game". It keeps rendering what it always renders: one flat frame per Present, from
one camera. The mod changes the inputs and the outputs of that pipeline, and nothing in between:

```
  headset + controllers (OpenXR)
        |                      \
        | head pose             \ controller poses, buttons
        v                        v
  ViewMatrixHook            KbmInject --------------------> SendInput (keyboard / mouse)
  (the main view's matrix   (play / menu / wheel modes,        |
   gets the head's offset;   head-aim servo turns the game's   v
   rotation comes from the   camera to the head via mouse   the game simulates as if a
   game camera, which the    counts)                        player were at the keyboard
   servo keeps on the head)                                    |
        |                                                      v
        +--------------------------> the game renders one flat frame (D3D11, deferred contexts)
                                         |
                                         |  GpuTrace: per draw, the first-person draws (arms, weapon,
                                         |  gadget) get their model-view-projection and skinning palette
                                         |  rewritten so they follow the controllers
                                         v
                                    Present  --->  VRRuntime / XRSession: the backbuffer is drawn into
                                                   each eye at its true angles and submitted with the
                                                   pose it was rendered from; menus go to a screen layer
```

Three ideas carry most of the weight:

1. **The view.** The engine builds the main view's matrix in one small function (`idRenderMatrix::CreateViewMatrix`,
   exe+0x109CC0). Hooking that single call and handing it a different origin and axis moves the whole scene without
   touching any game state. The rotation stays the game's own camera axis (so culling, aiming and everything the game
   computes from the camera stay consistent); a mouse servo keeps that axis on the head. Only the head's position
   offset is added in the view.
2. **Input.** The engine ignores synthesized gamepad input (see [XInputHook.h](../src/Src/XInputHook.h), parked), so
   the controllers are translated into keyboard keys (by scan code) and mouse motion and buttons, exactly as a player at
   a desk would produce them.
3. **The body on the GPU.** No CPU-side pose the engine exposes moves the first-person models (measured: entity
   transforms, transform sets, the scene render view, all without effect). So the mod rewrites, per draw call, the
   per-instance model-view-projection matrix and the skinning palette that the vertex shaders receive. The engine's
   own skeleton is never touched; whatever the engine anchors to that skeleton (effects, held objects, the muzzle) is
   then corrected separately by answering the engine's joint queries with the drawn pose.

Some design rules recur throughout and explain choices that may look odd:

- **Answer a query rather than write state.** Where the engine asks "where is joint N" or "where is the camera eye",
  the mod returns a different answer to that one call (a hook, a call-site thunk, or a temporary copy of a component)
  instead of writing into engine structures that other threads read.
- **One source of truth per pose.** The drawn weapon pose (`GpuTrace::WeaponTargetPose`), the render pose of each view
  (`ViewMatrixHook::RenderPoseXr`) and the room-to-game yaw mapping (`KbmInject::WorldYawOffset` /
  `ViewMatrixHook::AnimYawOffset`) are each computed once and read by everything that must agree with them.
- **Everything is a command.** The ini file and the automatic "VR package" are turned into the same `vr ...` commands
  the test channel uses, so a setting does exactly what its command does.
- **Measure, then change.** Most code paths carry counters that end up in the heartbeat file, and most features keep
  the previous behaviour behind a switch.

---

## 2. Repository layout

| Path | What is there |
|---|---|
| `src/Proxy/` | the `dinput8.dll` proxy (one export, loads the mod) |
| `src/Src/` | the mod, `DishonoredVR.dll` (about 50 modules, one subsystem each) |
| `src/CMakeLists.txt`, `src/vcpkg.json` | build (MSVC x64, C++20, static CRT `/MT`; Detours, fmt, Dear ImGui, OpenXR loader) |
| `mockxr/` | a mock OpenXR runtime used for headless testing (fake HMD and controllers driven from a text file, real D3D11 swapchains, a compositor emulation that writes what each eye would see) |
| `scripts/` | `build.ps1`, `deploy.ps1`, `undeploy.ps1`, `package.ps1`, `run-test.ps1`, `build-mockxr.ps1`; `scripts/testing/` (not published; see DEVELOPMENT.md section 6) holds the test harness (`Play.ps1` drives the game through the command channel) and one folder of test scripts per subsystem |
| `tools/` | Python reverse-engineering helpers (RTTI, typeinfo, xref, call-site and displacement scanners, `symbolize.py` for crash backtraces, `make_seeds.py`) |
| `package/` | files shipped to players next to the DLLs (`DishonoredVR.ini`, `README.txt`) |
| `docs/` | this file, ENGINE.md, DEVELOPMENT.md, STATUS.md, the cvar dump |

---

## 3. Startup and injection

### The proxy

`Dishonored2.exe` imports `DINPUT8.dll` statically. Windows looks for it in the game folder first, so a
`dinput8.dll` placed there is loaded instead of the system one. The proxy
([dinput8.cpp](../src/Proxy/dinput8.cpp)) forwards its single export, `DirectInput8Create`, to the real system DLL
and, from its `DllMain`, loads `DishonoredVR.dll` from the same folder.

### Load-order constraints

Because the proxy is a static import, the mod's `DllMain` runs **before the executable's C++ static initializers**.
At that point no engine global (cvars, the game-local object, the command system) exists yet. Two rules follow:

- `DllMain` ([Main.cpp](../src/Src/Main.cpp)) only opens the log, installs the crash handler and starts an init thread
  (doing real work under the loader lock is unsafe).
- Anything that reads engine data waits for the **first Present**: `CVars::Init`, `SysInfo::LogFirstPresent`,
  `GpuTrace::Install` (it needs the game's real device and contexts) and `PlayerState::Install` (it patches vtables of
  objects that exist by then).

### The init thread

`InitThread` in [Main.cpp](../src/Src/Main.cpp) runs, in this order:

1. `GameInfo::Init` and the build check. If the executable is not the supported build (version string, file size and
   link time stamp must all match), nothing is hooked: a message box explains it and the game runs flat.
2. `Settings::Load` (the ini, below), `Watchdog::Install`, `Seeds::Install`.
3. Development switches read from one-line files in the game folder: `DVR_usemock.txt` (a runtime manifest path put in
   `XR_RUNTIME_JSON`, which is how tests select the mock runtime under a Steam launch) and `DVR_validate.txt` (loads
   the OpenXR core validation layer).
4. The hooks: `D3D11Hook` (Present and ResizeBuffers, found through a dummy device's swapchain vtable), `RenderRes`,
   `CameraHook`, `PlayerViewHook` (installed but disabled), `ViewMatrixHook`, `XInputHook` (disabled), `PadProbe`
   (research), `KbmInject` (enabled), `HandAimHook`, `GunRay`, `MeleeAim`, `ThrowGesture`, `WeaponPose`, `HandFx`,
   `GrabHand`, `BinkVideo`.
5. `XRProbe::Run` **last**. It creates a throw-away OpenXR instance to log the runtime and system, and
   `xrCreateInstance` can block for seconds. Run earlier, the game kept booting meanwhile and created its vertex
   shaders before `GpuTrace` could see them, and then no arm or weapon could ever be recognised. The real session is
   only created after the probe has finished (Meta's runtime allows one instance per process).

### Settings

[Settings.cpp](../src/Src/Settings.cpp) reads `<game folder>\DishonoredVR.ini` once. Every key has a default in code,
so a missing file, key or unreadable value leaves the mod working (and the log says which). Values are validated
(booleans, clamped floats with comma-or-point decimals, choices). Settings are not applied directly: they are turned
into `vr ...` commands in two batches:

- `Settings::StartupCommands()`, run on the first frame: turning (smooth/snap, speed), quick-slot hold time, layout,
  auto-focus, the menu screen and its size/distance/backdrop, cutscene letterbox and arms, keep-alive frames and
  the new-session delay.
- `Settings::PackageCommands()`, run when the VR package switches on (below): sword and throw gestures, grabbing, the
  reticle, the wrist HUD and its size, stereo, disabling camera motion blur, and the cheats (unlock everything, infinite
  mana, ammo, health).

A few settings are read directly by the module that needs them: `RenderScale` (RenderRes), `ControllerPitchOffset`
and `LogControllerInput` (XRSession), `Haptics` (VRRuntime), `WriteStatusFile` (the heartbeat), `VrGraphicsTweaks`
`AmbientOcclusion` and `Fog` (VrTweaks), `SnapTurnBlank` (KbmInject), `CameraPitchFix` (ViewMatrixHook), `MinimalReticle`
and `ReticleOpacity` (Reticle) and the experimental `StereoOceanReuse`, `StereoReflectionReuse`, `StereoDecalReuse`
and `StereoStreamOncePerPair` (StereoSync).

**VrTweaks** ([VrTweaks.cpp](../src/Src/VrTweaks.cpp)): while the headset shows the game, the main view's own render
flags (view+0x3A37, set by the engine every frame from `r_skipDOF` / `r_skipSSAO` before the passes run) get "no
depth of field" (ini `VrGraphicsTweaks`, default 1: in a headset the eyes focus, not the camera) and, if the ini asks,
"no ambient occlusion" (`AmbientOcclusion = 0`: the game's own option only switches between HBAO+ and its own AO).
`Fog = 0` writes the live `r_skipFog` 1 while the headset shows the game (put back after; a flag cvar, not saved to the
config): the fog is drawn at a fraction of the screen's resolution and shows as blocks in front of the sky in a headset.
A filtered upscale (`r_fogNearestUpscale 0`) and no light scattering (view+0x3A36 bit 2) were tried first and made no
visible difference.
ViewMatrixHook sets them as the main view is built. No cvar is written: the game's options, its config and its own
backup of the DOF setting (`r_skipDofBck`) stay the player's. A first version wrote cvars (`r_skipDOF`,
`r_skipLensFlares`, `r_SSSBlurQuality`, `r_textureUploadTimesliceMs`) and the game hung at its main menu; DOF and lens
flares are also options in the game's own Visuals menu.

### The VR package and its automatic activation

The game must stay a normal flat game until there is both a headset and a player. [Commands.cpp](../src/Src/Commands.cpp)
watches for that: once the OpenXR session is running AND a player is being simulated (a player state is running, the
camera update is ticking and the player's view can be read) for 90 consecutive frames, it runs, once per launch:

1. `vr weapon6dof 1`, the umbrella switch for the whole motion-control package;
2. a recentre of the view and of the head-aim reference;
3. `Settings::PackageCommands()`;
4. every line of `<game folder>\DVR_autoexec.txt`, if present (a development aid; lines starting with `#` are skipped).

`vr weapon6dof 1` switches on, together: per-draw identification of first-person geometry (`GpuTrace::SetNear`,
`SetIdMode(1)`, `SetOnlyWeapon`), the deterministic hold (`SetHoldMode(1)`), the 6DOF rewrite (`SetSixDof(1)`), the
controller fire ray (`HandAimHook::SetMode(2)`), head aim (`KbmInject::SetHeadAim`, `ViewMatrixHook::SetKeepGameAxis`),
the takedown/cutscene view (`SetAnimCam(2)`), feeding the drawn weapon pose back to the engine (`WeaponPose` mode 1),
the hands and full arms with IK (`SetHandFollow`, `SetArmMode`, `SetBodyFollow`), the reticle, and the render fov that
covers the headset's whole field of view (`BodyView::FovAuto`). `vr auto 0` disables the automatic activation. The
package then survives loads and level changes.

If fewer than 50 vertex shaders were captured at that point, the log warns that the mod hooked D3D11 too late and the
arms and weapons cannot be recognised.

### Shipped seeds

[Seeds.cpp](../src/Src/Seeds.cpp) writes `DVR_grips.txt` and `DVR_hands.txt` (learned hand data, see "The body") into
the game folder if they are not there yet, from data compiled into the DLL (`SeedsData.inc`, generated by
`tools/make_seeds.py`). The data is keyed by skeleton and mesh, so it is valid on any machine with the same game build;
what the player's game learns later replaces it.

---

## 4. Frame flow

### Threads

| Thread | What runs on it |
|---|---|
| Game main thread | the window procedure, the player camera's per-frame update (`UpdateAim`, hooked by HandAimHook, from which the mod's "game-thread ticks" run), Present, and therefore the mod's whole per-frame loop. In this game the render thread that presents and the main thread are the same thread. |
| Engine job threads | most gameplay logic, the player sensors and power components (so a camera swap done for a sensor is visible to other threads, see "CamSwap"), and the recording of draw calls on many D3D11 deferred contexts at once |
| OpenXR frame thread | `xrWaitFrame`, the event loop, begin/end session, and keep-alive frames (XRSession) |
| Watchdog thread | watches the Present heartbeat and reports stalls |
| Init thread | startup only |

### What one Present does

`OnPresent` in [Main.cpp](../src/Src/Main.cpp) is called by the Present hook before the real Present. In order:

1. `Watchdog::OnPresent` (heartbeat), `WristHud::BeginPresent` (the frame's interface is complete).
2. On the first call only: `CVars::Init`, `SysInfo`, `GpuTrace::Install`, `PlayerState::Install`.
3. `BinkVideo::Install` if not done yet.
4. Per-frame bookkeeping: `GpuTrace::OnPresent` (closes the frame's draw statistics, rebuilds pooled buffers),
   `StereoTaa`, `PlayerState` (which states ticked), `MeleeAim`, `ThrowGesture`, `GrabHand`, `PointerRay`,
   `Commands` (the command channel, the heartbeat, the package activation), `BodyView` (applies `fov auto` once the
   headset's fov is known), `RenderRes`.
5. `Overlay::OnPresent`: the ImGui debug overlay (toggled with INSERT) and the per-hand reticle are drawn onto the
   backbuffer. This runs BEFORE VRRuntime, so anything drawn here reaches the headset.
6. `VRRuntime::OnPresent`: the OpenXR frame (below).
7. `KbmInject::Tick`: the controllers' input of this frame becomes keys and mouse motion.
8. `WristHud::EndPresent`.

### VRRuntime and XRSession

[VRRuntime.cpp](../src/Src/VRRuntime.cpp) owns one `XRSession` and drives it from Present:

- **Starting and restarting.** It creates the session on the game's D3D11 device. If that fails in a way that may pass
  (no headset yet, Link not connected), it retries every 3 s; until it succeeds the game stays flat and the VR package
  waits. If the connection or runtime is lost, the session is shut down and recreated. A session the runtime ends with
  EXITING is not recreated.
- **Per frame:** it sets the source extents of the flat frame from the fov the main view was actually built with
  (`CameraHook::BuiltFov`), calls `BeginFrame` (which gets a frame from the frame thread and locates the eyes),
  publishes the head pose and both eye poses (with the recentre generation, see below), syncs the controllers, lets
  `WristHud` and `ScreenMode` add their layers, and submits.
- **Which pose the image was rendered from.** The engine's pipeline is deeper than one Present: the image being
  presented was not necessarily rendered from the latest view. Every main view the mod builds gets a serial and is kept
  in a ring (`ViewMatrixHook::RenderPoseXr`). GpuTrace tags the command lists recorded with that view's draws, and when
  the immediate context executes a command list the serial travels with it (`GpuTrace::ExecutedViewSerial`). The layer
  then claims exactly the pose that image was rendered from, so the compositor's reprojection corrects the true
  difference to the head's current pose. Fallbacks: the serial the draws took, then the latest view (`vr posesource`).
- **Mono vs stereo** (`vr stereo 0|1|2`, ini `Stereo3D` 0 / 2 / 1). In mono (`0`, the default) the backbuffer goes
  to both eyes. Both stereo modes render one eye per game frame (the engine draws one view a frame) and the view
  record says which eye an image belongs to:
  - **synchronised** (`vr stereo 2`, ini `Stereo3D = 1`): [StereoSync.cpp](../src/Src/StereoSync.cpp) hooks
    `idGameLocal::RunFrame` (exe+0x8ACDF0). A frame that simulated builds the LEFT eye's view; the next RunFrame is
    HELD -- no simulation, only the game's Draw (exe+0x8AC710), which publishes the camera's render view -- and builds
    the RIGHT eye's view of the same world instant (the game clock loses nothing: the next tick gets both frames'
    delta). Both views come from one set of poses located for the pair's display time (`VRRuntime::LocateForView`), so
    hands, pointer and servo agree with the picture. The left image is only staged (`XRSession::StagePairLeft`, no
    OpenXR frame); the right one's Present submits both eyes in one frame (`SubmitPair`), each at the pose it was
    rendered from. Before a held frame the engine's DirectInput reads are skipped (the events wait for the simulated
    frame: otherwise about half the key presses were lost). The game's own frame cap (30 fps x `com_timeScaler`, 120 at
    most from its options) would hold the pairs to 60 a second, under any headset's refresh: it is switched off while
    synchronised stereo runs (`com_disableFrameSpinning`, restored after), and the headset's frame wait (once per pair)
    is the limit -- two game frames per display period, whatever the refresh. The game's swapchain gets a third back
    buffer (D3D11Hook, at its resize when VR starts): the compositor holds the game's GPU after each submitted pair
    until it takes it, and with the game's 2 buffers the left image's Present had no buffer and blocked the render
    thread that long (measured on SteamVR with a Pimax: ~10 ms a pair, ending 0.6 ms before the pair's display), so the
    next pair always missed the next display period: half the refresh. The game's V-Sync is ignored too while
    synchronised stereo runs (D3D11Hook: the Present's sync interval 0): with it on, the game skips its frame cap and
    waits for the monitor's vblank instead, which would hold the pairs to half the MONITOR's refresh. The light probes'
    update budget is halved meanwhile (`r_ILProbeUpdate_MaxUpdatesPerFrame`, per rendered frame: the same rate per
    second as flat play). Experimental, each off by default in the ini: the right eye's frame skips render passes
    that only repeat what the left eye's frame of the same tick just did, the way the engine skips a disabled task
    (its context marked empty): the ocean's simulation (`StereoOceanReuse`, UPDATE_OCEAN_FFT / UPDATE_OCEAN), the reflection probes' relighting (`StereoReflectionReuse`,
    INDIRECT_LIGHTING_UPDATE_IBL), the decals on NPCs (`StereoDecalReuse`) and the texture streamer
    (`StereoStreamOncePerPair`), and the spot / omni lights' shadow maps (`StereoLightShadowReuse`, below). A task is the right eye's when its view (task+0x70) is a main view object and that
    view's matrix names the right view of a pair. Hooks exist only for what the ini asks for. NOT the shadows: skipping
    them for the right eye (exe+0x32D4B0) broke shadows in both eyes, because the engine updates the sun's farther
    cascades on alternate frames, which in synchronised stereo are always the same eye's. The spot / omni lights' faces
    (the shadow task exe+0x32D4B0 for task+0xF8 at or above `r_shadowParallelSliceCnt`) are skipped for the right eye only
    when that frame's shadow job lists hash the same as the left frame's (lights, faces, atlas pages and rects, light
    matrices, cache entries) and no face is DIRTY: SHADOWS_PREPARE gives a face it does not find in its cache a new entry
    and marks it, the world pass draws its static casters into the atlas and the dynamic pass copies them into the entry;
    a skipped dirty face left an empty entry that was found from then on (the walls that stopped casting). Otherwise the
    right frame draws them. Counts on the heartbeat's `lightshadows`. A gate holds only in play: not paused or stopped, no menu,
    menu screen or player change, no hitch, and 30 Presents after any of those changed (holding a frame while a pause
    menu opened crashed the game's UI). Anything else is a mono view; a stereo health check falls back to mono (logged,
    retried with backoff) when no pair reaches the headset (`vr stereohealth`).
  - **alternating** (`vr stereo 1`, ini `Stereo3D = 2`, the 3D of 1.0): every frame simulates and is submitted to its
    own eye only; the eyes see two instants of the world, so depth shimmers in motion.
  The eye whose image is presented is checked every frame against the view the TAA dispatch of that image used
  (`vr phaseloop`): an image that disagrees is dropped, and two in a row shift the frame-order rule.
- **Per-eye frustum** (`vr eyeproj 1`, default): each stereo eye's view is TURNED towards the centre of its own frustum
  and rendered with the smallest symmetric fov that covers it (Quest 3: 6.6 deg out, 5.4 down, 99.7 x 105 deg instead
  of the cover of both eyes): about 78% of the pixels. An asymmetric projection matrix would be exact but breaks the
  engine's tiled lighting (`eyeproj 2`, research). The projection layer declares each image's own pose and fov
  (`vr layerfov 1`, default); `layerfov 0` keeps each eye's native pose and fov and warps the turned image into it.
  The head's orientation is the VIEW space, not the left eye (canted displays).

[XRSession.cpp](../src/Src/XRSession.cpp) is the OpenXR layer proper (a port of the sister project's session class,
much extended):

- **Instance, system, session, swapchains** bound to the game's device; eye swapchains grow as needed; controller
  actions with suggested bindings for the Oculus Touch and Valve Index profiles (other runtimes remap these); a
  vibration action for haptics.
- **The OpenXR frame thread.** `xrWaitFrame` does not run on the game's thread. A runtime that stops releasing frames
  (observed for tens of seconds with a stalled wireless stream) used to freeze the game on the PC. Now a thread of its
  own calls `xrWaitFrame`, polls events and begins/ends the session; `BeginFrame` on the render thread waits for it at
  most `vr xrwaitbound` ms (default 50) and otherwise lets the game go on without submitting that Present.
- **Keep-alive frames.** While the game sends no frame (a hitch, a load), the frame thread resubmits the last frame's
  layers once per display period, so the headset shows the last picture held in the room instead of the runtime's
  "not responding" indicator (`vr keepalive`, ini `KeepAliveFrames`). Because two threads then use the device, D3D11
  multithread protection is switched on for the game's device.
- **New session instead of waiting.** Meta's runtime can take a running session away and only offer it back minutes
  later, while a NEW session is ready at once. On that runtime (or any, with `vr xrkick <s> all`) a stopped session
  that is not offered back within `NewSessionAfter` seconds is replaced.
- **Bounded swapchain waits**: `xrWaitSwapchainImage` never waits forever; a timed-out image stays acquired and is
  waited for again.
- **Tracking loss** holds the last good pose of the head and each hand (`held` flag, `LostFrames` counters).
- **Recentring.** When the runtime moves the reference space, the change takes effect at the event's `changeTime`. The
  session bumps a *recentre generation* on the first frame whose poses are in the new space; every pose handed out
  carries it, so consumers (the servo, the view reference) re-seed on exactly the frame the poses jump.
- **Layers it can submit**: the projection layer (the game's picture, or the menu backdrop), two room-locked quad
  panels (slot 0 the menu screen, slot 1 the power wheel), the wrist HUD quad in the left hand's action space, and a
  legacy head-locked HUD quad.

### Placing the flat frame: orthoscopic, at its true angles

The game renders a symmetric frustum; each eye has its own asymmetric field of view. The flat frame is **drawn**
(scaled, filtered) into each eye's swapchain so that it covers exactly the tangent extents it was rendered for
(`XRSession::SetOrtho`, `SetSourceTans`, default on). An earlier 1:1 pixel copy around the optical axis gave the image
the wrong angular size (measured 24% squeezed horizontally), which made the hands look closer to the centre than the
controllers. `vr ortho 0` restores the old placement for comparison.

### The render field of view

By default the game renders about 95 x 59 degrees, so relaxed hands fall outside the picture. CameraHook hooks the
engine's projection builder (exe+0x3742B0, called before the inverse projection and the view-projection are derived,
so a changed fov is consistent downstream) and can override the main view's fov. `BodyView::FovAuto` (part of the
package; `vr fov auto|off`) computes the fov that covers both eyes of the headset and keeps it applied as a mode: it is
applied as soon as the headset reports its fov, and again if it changes.

### Render resolution

[RenderRes.cpp](../src/Src/RenderRes.cpp) decouples the engine's render size from its window, the way general-purpose
VR injectors do. With the frame drawn at its true angles, the right render size is the eye's own pixel density over
the angles the frame covers (about 2266 x 2348 for a Quest 3 eye buffer of 1824 x 1968 with the auto fov), while a
16:9 window gave 1902 x 973, stretched 2.4 times vertically.

The engine learns its window size from `GetClientRect` in two places: its window procedure on WM_WINDOWPOSCHANGED
(call returning to exe+0xE9FF1, sets `r_windowWidth/Height`) and `GetHdcClientWindowSize` (returning to exe+0x346364,
the render targets and swapchain). The `GetClientRect` hook answers those two call sites with the target size,
`ResizeBuffers` gets it too, and the window stays as the player has it. The size leaves the engine through two
functions, and both are hooked to see the real window while they run: the config writer `WriteConfigToFile`
(exe+0x41770, so the player's `dishonored2Config.cfg` keeps their own window size) and the window placement
(exe+0x347630). The interface cursor (exe+0xE27DF, `ScreenToClient`) is scaled into the render size. The game's
dynamic resolution (`rs_enable`, which aims at the monitor's refresh rate) is held off while the headset shows the
game. The target is capped (4096 per side, 16 megapixels) and scaled by the ini's `RenderScale`; it is not applied in
fullscreen. Never run the console's `writeConfig` on the player's config: it writes a different, partial file.

### Temporal anti-aliasing per eye

The game's TAA is one compute shader that blends each frame with a history texture, reprojected by motion vectors.
In stereo the previous frame is the OTHER eye's, which ghosts, so stereo used to switch TAA off.
[StereoTaa.cpp](../src/Src/StereoTaa.cpp) gives each eye its own TAA:

- the TAA dispatch (found by its resource names) gets that eye's own history texture bound, and what it writes is
  copied out for that eye;
- the engine's "previous" view, projection, view-projection and jitter, copied each frame when the view is built, are
  replaced in the CreateViewMatrix hook with those of the same eye's last frame;
- the cached previous MVP that each entity draw carries in its per-instance buffer (the engine's: the same draw's MVP
  in the last RENDERED frame, held frames included) is replaced per draw (`vr aaprevrule 1`, GpuTrace calls
  `FixPrevRows`): a table keyed by every draw's current rows finds the draw of the previous frame whose current rows
  are this draw's cached previous, and takes THAT draw's original previous -- this eye's last view with the model of
  its own last frame. (A correction of the view alone kept the model of the held frame: the right eye of synchronised
  stereo lost all object motion.) Without a match, the view correction `PrevCorrection`;
- skinned characters need nothing more: the engine renews the skinning palettes (current and previous) only on
  simulated frames, so a held frame draws the left frame's current pose with the previous step's as previous
  (measured);
- which eye a dispatch belongs to is read from the GPU's own data: the view matrix in the PerViewCB that dispatch uses,
  matched against the views ViewMatrixHook built.

`vr stereotaa 0|1|2`; if per-eye TAA cannot run, it falls back to forcing `r_skipTemporalAA 1` in stereo. The mono
views a gated synchronised stereo makes do not count as failures (`vr aamonowin`).

---

## 5. Head and view

### CameraHook: the main view and its fov

[CameraHook.cpp](../src/Src/CameraHook.cpp) hooks exe+0x3586A0, called several times a frame with a view object.
Despite the name `ComputeMatrices` in its header, that function only uploads already-computed view constants, so
rewriting the render view there does not move the scene; the hook is kept in pass-through mode to identify the MAIN
view object (largest render size) and to read the true fov. The projection-builder hook (exe+0x3742B0) implements the
fov override, including the menu screen's widened "Hor+" projection (see "Interface").

### ViewMatrixHook: the 6DOF hook

[ViewMatrixHook.cpp](../src/Src/ViewMatrixHook.cpp) detours `idRenderMatrix::CreateViewMatrix(org, axis, out)` at
exe+0x109CC0. When `org` is the main view's `renderView.vieworg` (main view + 0x39D0 + 0xA8), it hands the engine an
HMD-composed copy. The render view itself is never written, so nothing accumulates and the game's values stay intact.

Two compositions exist:

- **Head-aim mode** (`SetKeepGameAxis`, the package's mode): the axis stays the game camera's (which the servo keeps
  on the head) and only the head's positional offset from a reference is added. The offset is expressed in the ROOM's
  frame and rotated into the world by the room-to-game yaw mapping (below), not by the camera's current yaw: otherwise
  a lean, and stereo's +/- half IPD, would rotate with the head. The game eye sits about 5 cm in front of the body's
  vertical axis and would swing round a circle as the servo turns the body; `vr pivotfix` holds that offset fixed in
  the room. The game also moves its eye with its PITCH (a flat screen's neck: the pitch is spread over the body's joints
  and the eye sits ahead of the head joint), forward and down when looking down; with the servo putting the head's
  pitch on the game, that came on top of the real neck the headset tracks, and the world slid to and fro with each nod.
  `CameraPitchFix` (ini, default 1) measures it while the player stands still (the change of the eye's forward and
  vertical offset from the player per degree of game pitch, averaged per 3-degree band) and takes the integral from
  level out of play's eye (`PfStep`; the curve is on the heartbeat's `pitchfix` line). Inside a possessed host, pitch comes from the head with level roll, and the head translation is scaled by
  the host's eye height (a rat's eye is 0.18 m off the floor).
- **Full HMD mode** (head aim off): the HMD rotation replaces the game's on top of the game's yaw (`ComposeHead`).

The reference position re-centres on every runtime recentre, every new player (load or level change) and on request.

Each view built is published with a serial, its origin/axis/matrix, the game eye it was composed from, the XR pose
used, the eye (stereo) and a `flat` flag (a picture for the menu screen). Consumers:
`RenderView` (GpuTrace places the controllers in the same view the weapon's MVP lives in), `ViewAnchor` (lets the game
thread place something where the NEXT view will draw it: GunRay), `RenderPoseXr` (the layer pose).

**The room-to-game yaw mapping.** "Game yaw = room yaw + offset" is the single mapping that the layer pose, the head
offset, the hands, the pointer and the servo all use. In play it is `KbmInject::WorldYawOffset` (the yaw the servo is
DRIVING the game to minus the head's yaw: it only changes with a stick turn or a recentre, unlike the camera's yaw,
which lags the head by the servo's error). While a takedown or cutscene view is on, the view owns it
(`ViewMatrixHook::AnimYawOffset`) and the servo adopts it.

### The head-aim servo

The game's camera has to face where the head looks, because the engine culls the first-person weapon against the
render view using the weapon's real position in front of the character's BODY: with the body's yaw decoupled from the
head, the weapon's draws stop arriving as soon as the head turns 90 degrees away. Head aim is therefore not optional
in the package. It is implemented in `HeadAimServo` in [KbmInject.cpp](../src/Src/KbmInject.cpp) as a closed loop on
injected mouse motion:

- **Yaw** is a target that accumulates the head's yaw deltas (and the stick's turns); each game tick the error between
  that target and the player's actual yaw (`Game::PlayerView`, player+0x30C) is injected as mouse counts with a gain of
  0.7 (so an over-estimated counts-per-degree damps instead of oscillating), at most 300 counts per frame (the engine
  clamps large deltas). An earlier open-loop version banked its calibration error turn after turn.
- **Pitch** is absolute: the game's view pitch is driven to equal the HMD's physical pitch, read back from the real
  camera axis (cam+0xFC, forward.z = sin pitch), clamped at 80 degrees.
- Mouse counts are scaled by the player's `g_mouseSensitivity` (the counts-per-degree constant was measured at 0.808)
  and flipped with `g_invertMouseYAxis`.
- **Is the game simulating?** The only reliable signal is the camera's aim update ticking (`HandAimHook::AimTicks`); it
  stops in the pause menu, loading screens and some cutscenes. When it stops for 10 Presents, the servo freezes and
  re-seeds from the body's yaw when the game comes back (head turns made in a menu are not paid out as a snap).
- **Guards with a way out.** A yaw jump over 25 degrees, or a pitch jump over 20, in one tick is rejected (it is usually
  a camera swap from another thread caught mid-flight), but 30 rejections in a row mean the stored reference is the
  stale one, and the servo re-seeds. Without that exit the guard once latched shut for good.
- **Recentres** are recognised by the pose's recentre generation and taken as a new reference, not a turn.
- In a slide or a possessed host the servo measures the CAMERA's yaw instead of the body's (the mouse turns the camera
  there). In takedowns, cutscenes and hard landings it holds its target and injects nothing.

Smooth turning (default 120 deg/s with a 0.25 dead zone) and snap turning add to the servo's target.
A snap moves the room-to-game mapping (`WorldYawOffset`) at once, while the frames already in flight were rendered
facing the old way: below the full frame rate the compositor turned them into place and the edge they do not cover
showed as a black border swinging back. `SnapTurnBlank` (ini, default 1) blacks the headset's picture from the snap
until the servo has the game within 1.5 deg of the new heading and an image built after that is submitted
(`KbmInject::SnapBlankFade`, applied as `XRSession::DimFrame`), then fades it back in over 60 ms; 0.5 s at most.
`SnapTurnBlankOpacity` (default 0.5) sets how dark: the picture's brightness is 1 - opacity at the blank's deepest.

### PlayerState: what the player is doing

[PlayerState.cpp](../src/Src/PlayerState.cpp) tells which of the player's ~90 `arkPlayerState*` states (Sneaking,
Choke, Slash, Cinematic, Mantle, CarryCorpse, Powerwheel, ...) are running, without knowing where the player controller
stores its current state. The state vtables are found by RTTI; every slot of every one is pointed at a tiny generated
stub that records `this`, atomically increments a counter and jumps to the original (rax is free at a call in the
MSVC x64 convention). No function is patched and objects keep their vtable pointer. **Slot 13 is the per-tick update**
and is called only on running states; slot 6 is polled on dozens of states and means nothing.

Each state can carry a mask (`vr animstate <Name> <mask>`) that the rest of the mod consults:

| Bit | Name | Meaning | Default states |
|---|---|---|---|
| 1 | kView | the view takes its rotation from the head | all below |
| 2 | kTakeover | the animation owns the arms, the weapon and the mouse | Choke, DropChoke, Assassination, NpcFatality, BloodthirstyFatality, SyncAnim |
| 4 | kCine | an in-engine cutscene | Cinematic |
| 8 | kHold | the servo holds and injects nothing | CatFall, KnockedDown, Mantle, Vault, Climb |
| 16 | kLoco | the player moving: only the rotation is the head's, the eye stays where play puts it | Mantle, Vault, Climb, Sliding |
| 32 | kCamYaw | the servo measures the camera's yaw | Sliding |

Slash has kView only (its kill variant jolts the camera 40 degrees). `AnimActiveNow` also reports a state whose update
ran in THIS frame's simulation, so the render hooks catch the first frame of a takedown (which already carries the
camera pop). `Running(name)` and `StateObject(name)` serve many modules (the wheel, carrying, holding, possession,
character choice, black market, Shadow Walk, swimming).

### Takedown, cutscene and climbing views

`AnimViewStep` in ViewMatrixHook is a small state machine that runs on every main-view call in head-aim mode while a
kView state runs (`vr animcam 2`):

- rotation from the head; position from the animated camera, rate-limited (4 m/s by default) so the animation's pops
  become a quick glide;
- in a cutscene the eye rides the cutscene camera however fast it moves; a *cut* is a jump the camera's own motion does
  not explain (`vr cinecut`), framed where the head looks; in a cutscene that carries the player (a train, a skiff) the
  view turns with the player's body when both move faster than a run (`vr cinevehicle`);
- for kLoco states the eye stays where play puts it (with play's pivot), only the rotation changes;
- on the way out, the view waits for the servo to bring the game camera within 1.5 degrees / 2 cm, or glides the rest.

During a takeover, `GpuTrace::SetAnimSuspend` leaves the arms and the weapon as the animation draws them (they are
placed in the world from the game's own animated camera, so they stay on the victim). In a cutscene the arms stay on
the controllers by default (`vr cinearms`).

`PlayerViewHook` (exe+0x881640) builds render views for other entities (NPC eyes, cameras), never the local player;
it is installed disabled.

---

## 6. Input

### KbmInject

[KbmInject.cpp](../src/Src/KbmInject.cpp) turns each frame's controller state into `SendInput` events. Its rules:

- **Keys go by scan code.** The game reads DirectInput, which sees physical key positions; a virtual-key event with
  scan code 0 is not seen at all (that once made block, sprint and lean dead). The arrows and PGUP/PGDN share scan codes
  with the keypad and need the extended bit.
- **Keys come from the player's own bindings.** [KeyBinds.cpp](../src/Src/KeyBinds.cpp) reads the IN_GAME set of the
  game's `dishonored2Config.cfg` and maps each action (JUMP, CROUCH, INTERACT, WEAP_1, ...) to the key the player bound,
  by physical position, so AZERTY/QWERTZ layouts and rebinds work. It re-reads the file when it changes. Menu keys
  (ESC, ENTER, arrows, TAB, PGUP/PGDN) are fixed.
- **Only while the game is in front.** `SendInput` goes to the foreground window. If the game is behind another window,
  it is brought to the front when the player uses a controller or puts the headset on (`vr autofocus`, ini
  `FocusGameWindow`; the attach-input-queue technique, never a synthesized Alt). The game pauses itself when it loses
  the foreground; if it was being played, that pause is closed again with ESC when it comes back. The press that
  brought it back is latched.
- **Mouse buttons only onto the game's client area.** A synthesized click goes to the window under the system cursor,
  which the servo's motion moves around. `CursorOnGame` moves the cursor onto a visible point of the game's client
  area before a button press, or does not click. A click on the window frame starts Windows' modal sizing loop and
  freezes the game; a click on another window (a firewall prompt was observed) steals the focus.
- **Lost input releases everything.** If the runtime stops delivering controller input for 0.3 s, held keys are
  released (a stick held at a stall used to walk the player into a wall for good).
- **Latching.** Buttons held across a mode change are ignored until released, so a press meant for one context does
  not act in the next. Keys have a minimum press length so the game sees them.

### Modes

Every tick decides one of three modes:

- **menu**: the game is not simulating, OR no first-person draws are drawn (`GpuTrace::FirstPersonDraws() <= 0`,
  the positive "someone is playing" signal: the main menu keeps a player state ticking behind it), OR a *scene* is
  running. Exceptions that count as play even without first-person draws: possession, Shadow Walk, swimming at the
  surface, and cutscenes. *Scenes* are menus over a running world: the new game's `CharacterChoice` and a black market
  (`BlackMarketInteract`), `vr scenemenus`.
- **wheel**: the power wheel is held open (left grip held with MOUSE3 down, or the game's `Powerwheel` state running).
  The player then ticks only every ~0.17 s, so "simulating" flaps; the wheel mode keeps the controls stable.
- **play**: everything else.

### The controller layout (default)

| Control | Play | Menu | Wheel |
|---|---|---|---|
| Left stick | move (8 directions, digital) | arrows (with repeat) | steers the wheel |
| Left stick click | sprint, latched until you stop | screen in front | |
| Right stick X | smooth (or snap) turn | arrows | steers the wheel |
| Right stick click | zoom | screen in front | |
| A / B | jump / crouch (crouch while sprinting = slide) | ENTER / ESC | mana / health potion |
| Y / X | quick slots 1 and 2 (short) or 3 and 4 (held 0.5 s, with a buzz) | | assign the highlighted item to that slot |
| Right trigger | melee (sword) | next sub-tab (PGDN) | |
| Left trigger | power / gun (left hand); grenades are thrown on release | previous sub-tab (PGUP) | |
| Right grip | take what the pointer picked (F held); otherwise block | next tab (TAB) | |
| Left grip | power wheel | next tab (TAB) | held = wheel open |
| Menu button | short = journal, held 0.6 s = pause | ESC | |
| Both stick clicks | the menu button (for controllers without one) | | |

The grips are decided on `XRInputState::gripPress` (the `grippress_l/r` actions): the squeeze value on Touch, the
squeeze FORCE on a Valve Index (whose squeeze value is the fingers' curl: a hand resting on the grip read ~1 and held
the grip down). Pressed past `GripThreshold` (ini, default 0.6), released 0.15 below it. The analog `gripL/gripR` (the
curl on an Index) still closes the drawn hand. A runtime that binds nothing to `grippress` falls back to the value.

`vr quickslots 2` (ini `Layout = index`) is the Valve Index layout, active when the runtime binds the Index trackpads
(`XRInputState::pad`, the `pad_l/pad_r/padpress_l/padpress_r` actions, suggested for the Index profile only; any other
controller keeps the default layout). A trackpad press is force past 0.5 (released below 0.3), its meaning fixed by
where it began (`PadBits`): the left pad's middle (radius 0.45) is the menu button -- replacing the force-anywhere
binding the other layouts use -- and its top / right / bottom / left are quick slots 1-4 at once (`PadQuick`; with the
wheel open they assign, in menus they are the arrows). The right TRIGGER is F (interact on what the right hand's
pointer picked, held like a key; not while it lets a held object go), and the right pad is the sword's button (MELEE,
held = the power attack's charge; in a cutscene, skip): the sword's strikes are the gestures and the blade's contacts. The left B / A (X / Y on an Index) are the health / mana potions (`PadPotions`), the
wheel's B / A without the wheel.

`vr quickslots 0` (ini `Layout = classic`) restores the older layout (Y lean, X interact, menu short = pause). In a
cutscene only A/X (held = skip, the game's F), the menu button and the stick work; while a pre-rendered video plays
(BinkVideo), A/X held = F skips it. The sword, throw and grab gestures add to this table (sections 8 and 9).

Two input routes are kept but disabled: [XInputHook.cpp](../src/Src/XInputHook.cpp) synthesizes an Xbox pad (it
reaches the engine's event queue, but the player ignores it, cause unknown), and [PadProbe.cpp](../src/Src/PadProbe.cpp)
is its research probe.

---

## 7. The body: arms, hands and weapons on the GPU

### Why the GPU

The first-person arms and equipment are animated relative to the camera, and every CPU-side pose the engine exposes was
measured to have no effect on where they are drawn. Their vertex shaders, however, receive per draw a
`PerInstanceCB` (cb2) holding that draw's own model-view-projection matrix, with vertices in MODEL space, and the
skinned arms also read a joint palette buffer (`ro_matrices`). Rewriting those, per draw, moves exactly that geometry
and nothing else on screen.

### GpuTrace: hooking the renderer

[GpuTrace.cpp](../src/Src/GpuTrace.cpp) is the largest module. `Install` (first Present):

- device hooks: `CreateVertexShader`, `CreatePixelShader`, `CreateComputeShader`, `CreateInputLayout`. Shader bytecode
  is kept per shader object and reflected (`D3DReflect`) to find, BY NAME, where `cb_mvpmatrix{x,y,z,w}`,
  `cb_previousmvpmatrix*` and `cb_modelmatrix*` live: every material compiles its own cb2 layout (about 30 sizes in one
  frame), so a table of sizes misses passes. Shaders are also named by a DXBC-header hash, which is stable across runs
  (pointers are not);
- context hooks on BOTH the immediate context and a deferred context of the same device (deferred contexts have their
  own vtable, shared by all of them, and gameplay is recorded on deferred contexts: the immediate context sees no draws
  in play): the six draw entry points, `Map`/`Unmap`/`UpdateSubresource` (cb2 is written just before each draw and a
  deferred context cannot be read back, so its contents are shadowed per thread), `Dispatch` and copies (for
  StereoTaa), and `FinishCommandList`/`ExecuteCommandList` (to carry the view serial to the presented image).

**The per-draw path is hot.** Every draw of every recording thread (~2300 a frame, two frames per headset frame in
synchronised stereo) passes the hooks, and with the weapon package or per-eye TAA every one of them enters
`BeginSixDof`. What it does before it knows the draw is not the player's must not take a lock or touch a shared cache
line: the newest transform is read from published mirrors (`EnsureTransform`, `XfPublish`), the command list's view tag
is remembered per thread (`CtxSerialSet`), a draw whose model origin is out of reach of the player is rejected from its
own rows (`FarFromPlayer`) before its view is looked up, the view lookups (`StereoTaa::ViewMatrixOf`,
`ViewMatrixHook::SerialOfViewMatrix`, StereoTaa's `FindDrawView`) keep their last answer per thread until a PerViewCB
is written or a view is published, and statistics are counted per thread and published every 64. Only the few dozen
near draws reach the locked state copy in `TransformInstanceCb`.

Replacement constant buffers come from pools created at Present, never inside a draw hook (creating or mapping shared
buffers from the recording threads crashed the driver), written with `UpdateSubresource`, bound around the draw, and
the original binding is always restored.

The draw hooks also serve WristHud (the interface draws are repeated into a texture, the cutscene letterbox is
dropped) and the sword-trail removal (three vertex shaders, by hash, are skipped around the player's Slash:
`vr notrail`).

### The MVP factorisation

No engine view or projection is needed. A perspective projection's last row is (0,0,1,0), so the MVP's `w` row is
the third row of model-to-view. From it the product factors per draw into a rigid model-to-view pose, a uniform scale
and every coefficient of the projection (TAA jitter included). Rebuilding with the SAME projection and a different
pose moves the model and nothing else, and since every pass (depth, velocity, colour) is factored with its own
projection, all passes receive the same pose. The recovered projection of the player's own draws is also what the
reticle projects with (`ProjectWorld`): it needs no fov, aspect or letterbox calibration. Only draws identified as the
player's own publish it: reflections and cube maps go through the same path with other projections.

### Which draws are the weapon

The player's equipment entities (`PlayerLeftEquipment` / `PlayerRightEquipment`, from the equipment component's items:
component at `GetComponent(player, exe+0x39DCD30)`, left item +0x70, right +0x68, spawn id at item+0x8) carry their own
world transform. [BodyHide](../src/Src/BodyHide.cpp) samples them, the arms and the player every frame
(`GetAnchors`). A draw belongs to a hand's equipment when that equipment entity is the nearest first-person entity to
the draw's recovered model origin (within an anchor radius). Measured: the weapon's draws sit at 0.000 m from their
entity, everything else 0.4 m or more away. The test is redone per draw, per frame: no scan, no index counts, nothing
to redo on a weapon change. Index-count and scan/hide/scan identification are kept only as research tools.

### Anchoring the weapon to the controller

For each hand: `delta = hand * gripBase * grip * entity^-1`, with `entity` read fresh every frame. Every draw of that
hand's equipment gets the same rigid delta, so the weapon's body and its parts (upgrades are separate entities that ride
the same delta) stay together. With the deterministic hold (`gpuhold 1`, default) `gripBase` is the identity: the
weapon's forward is the controller's forward, the same every session. `grip` is a per-hand trim in the controller's
frame (`gpugrip`, `gputrim`): a pistol points along the controller, the sword stands at -90 degrees of pitch out of the
fist. The anchor is retaken when the equipment entity changes (weapon swap, load), after a 45-frame settle so the equip
animation is not captured. `WeaponTargetPose` is the drawn pose; `WeaponFromHand` gives the pose for a controller pose
NOW (used by the game thread when firing); `WeaponClaim` is the identification rule, lock-free, for other modules.

**Each draw with its own view's transform.** `entity` must be the transform of the same game
tick as the draw's model matrix, or `entity^-1 * model` carries the player's movement over one tick (4 m/s at 90 ticks/s:
4.4 cm, sideways when strafing). It used to be the game thread's latest sample, read whenever the first moved draw of a
view was recorded, and the transform was the NEWEST view's for every draw -- but draws are recorded on worker threads
after the next view is built (in synchronised stereo the right eye's a whole game frame after the left's). Now every
view records the first-person entities and the game camera when it is built (`ViewMatrixHook`, `BodyHide::SampleNow`;
the right eye of a pair takes its left eye's, no tick lies between them), `EnsureTransform` builds one transform per view
(a ring of 8, `XfState`), and each moved draw takes the one of the view its own PerViewCB names (the newest view's when
that cannot be read: per-eye TAA off). The eye pivot (`vr pivotfix`) takes the player from the same record.

### The arms: palette rewrite and IK

The arm meshes (glove, sleeves, rings) are skinned with a palette per mesh. Writing `M_i' = D_i * M_i`, with `D_i` a
rigid delta in model space per SEGMENT (hand, forearm, upper arm, root), moves each bone without ever knowing the bind
pose; segments meet at the joints by construction.

- **Side**: which arm a palette belongs to comes from the reflected `ro_matrices` slot of the draw.
- **Segments**: which bone is what comes from where each bone's vertices sit along the bind axis, over every mesh of
  the arm (vertex streams read once per mesh).
- **Joints** are learned from the animation: the joint J between bones p and c satisfies `M_p J = M_c J` in every
  frame, solved by least squares with anatomical priors.
- **IK**: a two-bone IK from an anatomical shoulder hung under the head (yaw-only body frame) through the elbow (towards
  a pole vector) to the wrist target, with the arm lengthened to a configurable length (`vr armlen`, `gpushoulder`,
  `gpupole`); forearm roll is distributed over the twist bones.
- **Per draw, not per frame**: each arm draw is posed from its OWN palette and model matrix (`SolveArm`); the
  once-per-frame `LearnArm` only learns. Posing meshes drawn before the main palette with the previous frame's deltas
  was the "hand tremor while running". The previous-frame palette (for motion vectors) gets the exact rewrite that
  content was drawn with (`PrevStore`/`PrevFind`, by content hash).
- **Loads** build new palettes while recycling vertex buffers: a side that has not seen its main palette for 60 frames
  while still drawing adopts the largest palette it sees (`readopt`); the wanted-palette list is an LRU.
- **Foreign arms**: while the arms are the game's (Shadow Walk's claws, takedowns), no arm identification or analysis
  runs (`g_ArmForeignUntil`), or the claws would be learned as the skin.

### Hand poses

Which pose the palm takes depends on what the hand holds (what it holds comes from the item's component class,
`BodyHide::HeldItemClass`, `HoldsGun`, `HoldsGadget`):

- **On a weapon**: one learned grip per item class, side, skeleton and skin, the palm in the weapon entity's frame plus
  the fingers, learned only when no state moves the hand on the weapon (1.5 s clean, 45 steady frames), saved in
  `DVR_grips.txt` and applied always (crouched, blocking, reloading). `vr weapongrip`.
- **Empty hand / a power**: anatomical, from the mesh itself: the hand's long axis along the controller and the palm
  (the direction of least spread of its bind centroids) facing the body's midline (`gpuhandhold 2`). With nothing
  equipped, the fingers keep the game's open hand learned standing still, closed towards the sprint fist by a curl
  amount and by the controller's grip (`vr handrest`, `RestPass`; blends are done about each bone's bind centroid).
- **The player's fingers** (`FingerTracking`, with `Layout = index` on an Index): XRSession reads each finger's curl
  (`XRInputState::fingerCurl`, 0 straight .. 1 fist) from `XR_EXT_hand_tracking` when the runtime offers it (the
  bends at each finger's three joints, motion range unobstructed; SteamVR builds the skeleton from the Index's finger
  sensors), else from the controller's own sensors (the trigger finger by its touch and pull, the other three by the
  squeeze's curl, the thumb by its touch on any control). An empty hand, and a hand with a power while no power is
  being used, then takes `RestPass` with each bone blended by ITS finger's curl. Which bone is which finger is read off
  the open hand and the fist (`ClassifyFingers`): the four fingers bend about one axis, across the knuckles, and the
  thumb about another; the fingers' bones split into four groups along that axis (the index finger the group nearest
  the thumb), and the bones that barely turn join the nearest group. A hand with a power learns its open hand from its
  own idle (standing still, no power in use); its fist is the empty hand's sprinting fist, else the weapon's grip
  (`g_Fist`, or the fingers saved with a grip in `DVR_grips.txt`). The heartbeat's `handrest` line shows the curls,
  their source and the bones per finger.
- **Gadgets** (grenades, mines, the Heart): the hand takes the anatomical pose and the gadget is kept in the palm where
  it sits at the closest point of the equip animation (`gpugadget 2`).
- **Holding an object**: the fist learned from the weapon grip (`vr grabfist`).
- `DVR_hands.txt` keeps the hand socket and hold learned per side.

### BodyHide

[BodyHide.cpp](../src/Src/BodyHide.cpp) resolves the player's sub-entities (`PlayerBody`, `PlayerLeftArm`,
`PlayerRightArm`, the two equipment entities) on the game thread (from HandAimHook's UpdateAim detour), samples their
transforms, and can hide models by zeroing `arkSkinnedRenderModel` mesh visibility masks (model+0x2100). Two safety
rules live here:

- **`BodyHide::EntityById`** wraps `GetEntityById` (exe+0x6DC560), whose table is read through a global that is NULL
  during a level change; calling the raw function then faults. Always use the wrapper.
- Render-model pointers are revalidated on the same frame they are written through, and re-bound as soon as the player
  pointer changes: a load frees the old models while pointers to them are still held (`stale` counter).

### WeaponPose and HandFx: the engine's own idea of the pose

The GPU rewrite does not change where the ENGINE thinks things are. Two modules close that gap:

- [WeaponPose.cpp](../src/Src/WeaponPose.cpp) feeds the drawn weapon pose back into the `arkTransformSet` of the weapon
  render models (`SetOriginAndAxis`, exe+0x9D560; transform update exe+0x753C10), mode 1 in the package. Moving the
  equipment ENTITY too (bit 16) breaks the identification and is off.
- [HandFx.cpp](../src/Src/HandFx.cpp) hooks the engine's two model-space joint queries (exe+0x53D3F0 with a local
  offset, exe+0x53D6A0 without; every world-space and by-name joint query is built on them) and the entity bind
  (exe+0x7506D0). When the model is one of the player's arms, the answer gets the same per-segment delta the palette
  rewrite gives that point of the arm (chosen by position along the arm: the engine's joint indices are not the
  palette's). So Far Reach's tendril, Blink's smoke and power glows come out of the drawn hand. Entities bound to an
  arm joint (the equipment) are left alone, because the weapon identification matches draws against their real
  position. GrabHand answers held-object queries through the same hook.

### BodyView

[BodyView.cpp](../src/Src/BodyView.cpp) is an instrument (`vr bodyview <tag>`: the drawn arm's joints next to where a
real arm with the configured anatomy would be, for comparison with the mock compositor's per-eye picture) and owns the
`fov auto` mode described above.

---

## 8. Weapons and combat

### HandAimHook: the camera fields everything reads

[HandAimHook.cpp](../src/Src/HandAimHook.cpp) hooks the player camera component (`arkCpntPlayerCamera`): its update
(learns the component), `UpdateAim` (exe+0x7F4B30, once per frame: the mod's game-thread tick, which runs
`BodyHide::Tick`, `GunRay::Tick`, `TestAids::Tick`), the shake/bob application (exe+0x88C5E0, the last writer of the
camera's eye cam+0xD8 and axis cam+0xFC; its snapshot is `RealCamera`), the renderView copy, the weapon fire chain
(`GetAimPoint` exe+0x3C3CC0, `ComputeAimTarget` exe+0x638AE0), and several sensors and power updates.

- **CamSwap.** To make one engine call aim with a hand, the camera's (and player's, and aim component's) fields are
  replaced with the controller pose for the duration of that call only, and restored. These calls run on job threads,
  so one recursive mutex covers every CamSwap, the shake hook and the renderView copy; readers on other threads use the
  post-shake snapshot and reject one-frame jumps.
- **Power aim.** Powers and sensors aim with the camera's `m_aimOrigin`/`m_aimAxis` (cam+0x2527C/+0x25288), which no
  weapon reads. With `vr aimoverride 1`, after `UpdateAim` those hold the power hand's (left) controller pose for the
  rest of the frame. `vr poweraim <mask>` wraps further calls in a CamSwap (power targeting, throw preview, search
  target, the Far Reach sensor's query and compute wrapper, and the mine placement sensor exe+0x3C1E40).
- **`ControllerPose` vs `ControllerPoseAt`.** The first anchors the controller to the game camera; the second to the
  view the frame is actually drawn from (with the head's room offset). Anything that must sit in the DRAWN hand uses
  the second.

### GunRay: one ray for the gun

[GunRay.cpp](../src/Src/GunRay.cpp) makes everything about a ranged shot agree on one ray, from the muzzle of the
weapon as drawn, along its barrel (`vr gunray <mask>`, default 63):

- the game's ranged aim sensor (submit exe+0x3C3FD0, results exe+0x3C3E10) traces this ray, run with a copy of its
  camera/aim fields, so the engine decides what the gun points at with its own mask and range;
- `ComputeAimTarget` places the muzzle from the weapon component's entity transform times the muzzle joint; during the
  call that entity reads the drawn weapon pose, so the projectile leaves from the drawn muzzle and flies to the
  sensor's hit;
- the pistol's blast (an explosion cone placed from the head camera, exe+0x44F030) is fired along the barrel;
- effects and entities the engine places on the gun (muzzle flash, smoke, sparks; world joint query exe+0x53D800)
  are placed on the gun as drawn, except models bound to a gun joint (the reload cartridge), whose draws already ride
  the weapon's GPU delta;
- the culling sphere of every model drawn with a held weapon's delta (exe+0x237340) is widened to include the drawn
  pose, or the gun vanishes during the reload animation;
- each frame the projectile's shape is swept along the ray (exe+0x638800): the reticle's end point.

The game's own spread can be re-applied about the barrel (`vr gunspread 1`; default exact).

### MeleeAim: the sword strikes along the hand

The player's melee code (exe+0x62E000..0x636000) reads the view direction and eye only through two one-line accessors
(exe+0x8067C0 = cam+0xFC, exe+0x417820 = cam+0xD8) that are called from dozens of places elsewhere, so they are NOT
hooked. [MeleeAim.cpp](../src/Src/MeleeAim.cpp) redirects only their call sites inside the melee code to thunks that
save every volatile register and return the right controller's axis (or a gesture's frozen direction) when the camera
passed in is the player's. Nothing is written to the camera. It also hooks the melee internals that the contact strike
needs: the 30-degree cone test (exe+0x62F900), the attack window's activation (exe+0x634890), the per-frame hit check
(exe+0x631020), the attack sweep and start shape (vtable slots 41/42), the candidates gather (exe+0x636B40) and a veto.

### SwordGesture and SwordContact: swinging the sword

- [SwordGesture.cpp](../src/Src/SwordGesture.cpp) recognises gestures in XR space from the right controller: a SLASH is
  the blade's tip moving across the blade fast while the hand itself moves (which rejects wrist flicks), closing on
  where the head looks, with re-arm and cooldown; the strike direction is frozen at the swing's peak and handed to
  MeleeAim (the game asks for it about 0.2 s after the click). A fist by the right ear with the blade forward charges
  the power attack (holds the button), a thrust releases it, lowering the blade cancels it. The fist at the left
  shoulder blocks. The trigger and grip keep working.
- [SwordContact.cpp](../src/Src/SwordContact.cpp) makes the sword hit what the DRAWN blade touches. Every frame the
  blade (from `WeaponTargetPose`) is swept against the boxes of nearby entities (render-model bounds times the entity
  scale) and, for skinned characters, against bone capsules from their current skeleton
  ([NpcSkel.cpp](../src/Src/NpcSkel.cpp): the pose array at the skeleton object +0x2D8; a ragdoll writes the array's
  tail, so a corpse is struck where it lies). A contact by a real arm swing (hand speed and distance measured on the
  controller in the room) clicks the attack, aims it at the contact, and restricts what that Slash may hit to the
  touched entity (MeleeAim's cone filter). A swing that touches nothing still plays the Slash and its sound but hits
  nothing. Haptics mark touch, contact and impact.
- **The blade's own strikes** (`SwordBladeDamage`, experimental, `SwordDamageScale`): a contact on a character does not
  click the attack. MeleeAim strikes it on the game thread, in the hit check's hook: it opens the attack window (so the
  physics sweep runs, steered by slot 41 to end at the contact along the blade's motion, the attack axis aimed at the
  contact), keeps the game's own hit check out meanwhile (it would only play the hit, the "push", and close the window),
  and as soon as the sweep's lists hold a record of the touched character calls the component's strike (vtable slot 39)
  on it with the damage scale `clamp(blade speed / 4 m/s, 0.4, 2) x SwordDamageScale`. The strike closes the window
  itself. No record within 0.35 s, or a refused strike: the game's attack is clicked instead, as before.
  An NPC still defends itself: the strike goes through the game's own per-NPC handler (exe+0x6355F0, the one the
  swing's exe+0x62CB80 uses), so a guard that is blocking the player blocks or parries the blade (the recoil, the
  sparks, no damage), and a hook on the player's strike (exe+0x62E550) swaps the handler's x1 for the blade's scale. A
  defended strike is final (no fallback click).

### ThrowGesture: grenades by hand

The game's throw builds the velocity from constants and hands it to the projectile with one setter call at
exe+0x63333C. [ThrowGesture.cpp](../src/Src/ThrowGesture.cpp) redirects that call site: for a projectile with a grenade
component, the velocity becomes gain times the left hand's fastest velocity over the last moments before the trigger
opened, plus the player's own velocity, capped, and the spawn point is the hand. Holding the trigger still cooks the
grenade as in the game. Mines and spring razors keep the game's placement, aimed with the hand (power aim above).
`HandVelocity` is also used by GrabHand's throw.

---

## 9. Interaction

- **Stealth kills** (MeleeAim::TickTakedown, game thread): the player's assassination sensor is asked once a frame
  whether a kill is on offer (its slot 26, what the "kill" prompt shows). While it is, a blade contact on a character
  clicks the game's attack instead of the blade's own strike (SwordContact), so the game turns it into its
  Assassination. An air swing already clicks the attack.

### Collision: solid hands and weapons

[Collision.cpp](../src/Src/Collision.cpp) (`HandCollision`, `WeaponCollision`, `PushObjects`, all experimental). The
real controller cannot be stopped, so the DRAWN hand is, with whatever it holds as one rigid piece:

- **Game thread** (`Collision::Tick`, once a frame from HandAimHook's aim update next to GunRay): each collision point
  (the palm; with weapons, 5 samples through the long axis of the held weapon model's box, plus the ends of a wide
  second axis) is swept with the game's own Havok ray cast from where it was DRAWN last frame to where the controller
  puts it now, sliding along what it hits (up to 3 legs). Each surface met is published as a contact plane tied to that
  point in the hand's frame. After a snap turn or a teleport, or when a point is new, the sweep starts at the eye.
  The poses come from GpuTrace (`RawHandPose` / `RawItemPose`: the latest view's poses before the push).
- **Render thread** (`Collision::Apply`, called by GpuTrace where each view builds a hand's world pose): the hand's
  translation is pushed out of those planes using this view's own controller pose, so the stop is exact and does not
  trail a frame. A push larger than 0.35 m lets go (the hand passes through and follows the real one) until the eye
  sees the palm in the open again.
- **Characters' bodies** (weapons): SwordContact turns the RAW blade's penetration of the bone capsules (the ones the
  strike uses) into contact planes (`SetBodyContacts`), so the drawn blade halts at the body while the strike, made
  with the raw blade (`GpuTrace::WeaponTargetPoseRaw`), still lands. Each plane sits `SwordBodyDepth` (default 4 cm, at most
  80% of the bone's radius) inside the capsule's surface, so the blade sinks a little into the body before it stops.
  Deeper than 0.22 m: left alone (it went through).
- **Pushing**: a contact with a physics-driven entity (entity+0xD1 bit 3) gets the game's impulse message 0xFA with
  `PushStrength` (the hand's mass) x the speed at which the raw point moves into it, relative to the object's own
  velocity (entity+0x33C), so a box being pushed comes along with the hand.
- **Doors** (`PushDoors`): a contact with an arkDoor (class id within exe+0x39C0350 / +4) where the raw point moves
  into it faster than 0.35 m/s, or is pressed 4 cm past its surface, posts the game's own door push,
  ARK_MSG_DOOR_PUSH (0x138, payload the player's entity id) -- what Windblast sends, and the push NPCs walk through
  doors with. At most once per 1.5 s per door. Whether the door refuses the push while locked is the game's handler's
  decision (not yet confirmed: the log line `pushed door` says each push).
- A buzz on each new contact, stronger the faster the hand came in. Heartbeat line `collision`.

### GrabHand: holding objects in the hand

The game holds an object (a bottle, a glass: the `HoldObject` state) by binding its entity to joint 8 of the player's
body model with a fixed offset ahead of the eye; the physics body is keyframed to that bound transform and the render
model is placed by a second query with the same offset. [GrabHand.cpp](../src/Src/GrabHand.cpp) answers both queries
(through HandFx's joint-query hook) with the grabbing hand's pose times the object's pose relative to the controller
captured when the hold began (with a distance limit, so an object picked from afar comes to the hand), fitted into the
drawn fist by the object's own box. Nothing is written to the engine.

- **Selection** uses the game's own interaction sensor, which is two-phase and asynchronous: SUBMIT (exe+0x3B8D60)
  posts sphere casts, RESULTS (exe+0x3B8430) reads them and picks. Both phases are detoured and run with the sensor's
  camera pointer swapped for a COPY of the camera whose aim fields hold the grabbing hand's pointer ray; the real
  camera is never written.
- **Throwing** a held object redirects the one call in the game's throw (exe+0x83E473) that carries the final velocity.
- **Release** modes (`vr grabrelease`): by default another grip press drops, the trigger throws with the hand's
  velocity.
- The game hides the arm(s) while holding; GrabHand shows them again (`vr grabarm`). Carrying a corpse (`CarryCorpse`)
  hides the carrying hand's arm and its pointer (`vr carryarm`), the grip holds F for as long as it is held (picking up
  a body is a long press), and the sensor aims where the eyes look so F opens the door looked at (`vr carrygaze`).

### PointerRay

[PointerRay.cpp](../src/Src/PointerRay.cpp) computes one ray per hand per frame that everything aimed with a hand at the
world agrees on (the reticle draws it, the interaction sensor picks along it). Its origin is the controller where the
hand is drawn; its direction is turned by the servo's TARGET yaw (the room-to-game mapping), not the camera's lagging
yaw, and One Euro filtered, so head sway and hand tremor do not shake a ray that reaches metres out.

### Reticle

[Reticle.cpp](../src/Src/Reticle.cpp) draws, per hand, a line along the pointer and a ring where the engine says the
shot or power will land (the gun ray's hit, the power's target, the grab pick), or at a fixed range. It is drawn with
ImGui into the backbuffer from `Overlay::OnPresent` and projected with the projection the player's own draws used, so
it lands exactly where the drawn hand is. `MinimalReticle = 1` draws only the end of each ray, a small white dot ringed
in black (visible on any background), three times the size while the grab hand's ray is on something it can take;
`ReticleOpacity` (default 0.5) scales the reticle's alpha. While the reticles are on and the headset shows the game, the
game's own crosshair (the centre of the game camera's view, which marks nothing when the hands aim) is off:
`Reticle::Tick` writes `ark_ui_crosshair_style` 0 and `ark_ui_crosshair_opacity` 0 (live values only, as WristHud does
with the HUD option, so the config keeps the player's) and puts the player's values back otherwise. `vr handaxes 1` adds palm, controller, blade and box axes: the instrument
for judging a pose from a screenshot.

---

## 10. Interface

### ScreenMode: menus on a screen in the room

The game draws its menus, loading screens and pre-rendered videos as one flat picture. Across the eyes at 108 x 110
degrees that picture went wherever the head went and its corners could not be looked at.
[ScreenMode.cpp](../src/Src/ScreenMode.cpp), whenever KbmInject is in menu mode:

- makes the engine render that picture as the flat game would: ViewMatrixHook passes the game camera through
  unchanged, CameraHook widens the game's own (Hor+) projection so the picture's centred 16:9 band is exactly a 16:9
  monitor's view, GpuTrace leaves the arms as the game draws them, StereoTaa is idle (mono). Each view record carries
  `flat`, so a picture rendered flat is shown on the screen even after play resumed (the pipeline is deeper than one
  Present);
- submits, through `XRSession::SubmitScreen`, a projection layer drawn per eye for its current pose (a dim, room-locked
  environment plus the last game picture frozen where it was seen, darkened) and the 16:9 band on a quad panel fixed in
  the room in front of where the head looked when the menu opened (default 62 degrees wide at 2.4 m; ini `MenuScreen*`,
  `MenuBackground`). A stick click, or looking away for 0.8 s, brings it in front again;
- fades in, and fades the game back in from black on the way out; `Expect` brings the screen up the same frame the
  controllers ask for the pause or journal.

The shop of a black market lays its interface out against the frame's edges, so its panel crop runs from the band down
to the frame's bottom (`vr screencfg shopcrop`). The **power wheel** is play: the world stays, the view follows the head,
and the game's whole interface is lifted out of the picture (WristHud's copy, kept out of the backbuffer) onto panel
slot 1, fixed where it was opened.

**The HUD in stereo** (`ScreenMode::PlayHud`, ini `StereoHud`, default 1, and `HudDistance`, default 2 m). The game draws
its interface into each eye's picture at the same pixels, but the same pixel of the two pictures is not the same
direction: each eye's picture is rendered for its own frustum, turned towards that frustum's centre (wider on the nose
side), so every HUD element came out at two directions, diverging. In stereo play the interface is lifted the same way as
for the wheel (WristHud's copy only) and shown on panel slot 1 for both eyes: the plane of the picture it was drawn over,
at HudDistance, in the frame the game camera rendered that picture as (the eye's turn and its offset from the head taken
back out, the head's position from the view's own poses), in the room. It covers what the picture covered at any distance;
the distance only sets where the eyes converge on it. The left image of a pair is drawn without its interface; the
right image's is the one shown.

### WristHud: the HUD as a watch

The game draws its 2D interface straight into the backbuffer (premultiplied alpha, stencil-masked arcs).
[WristHud.cpp](../src/Src/WristHud.cpp), from GpuTrace's draw hooks, issues every interface draw a second time, just
before the game's own, into a transparent texture of the same size (same shaders, the game's depth-stencil with writes
off, the game's blend for colour). The backbuffer is typeless and the interface alternates UNORM and sRGB views draw by
draw, so each repeat goes through the same kind of view the game used. That texture then serves three purposes:

- **the watch**: the health/mana/item cluster, cropped, shown as a quad in the left controller's aim space on the back
  of the left wrist when the wrist is turned to the eyes and looked at (`vr wristhud`, `vr wristcfg`). While it is on,
  the game's HUD is forced to "always shown" (`ark_ui_show_master_hud 2`);
- **the wheel's panel** (ScreenMode, above);
- **the cutscene letterbox**: the bars are two untextured interface fills; in a cutscene they are not issued
  (`vr cinebars`, ini `CutsceneLetterbox`).

Draws with no pixel shader, no blending, or issued by the mod itself are not repeated.

### BinkVideo

Mission videos are Bink 2 files played by `bink2w64.dll`, outside the player's state machine (they read as a menu).
[BinkVideo.cpp](../src/Src/BinkVideo.cpp) hooks its open/decode/close exports to know when a video is on screen, so
KbmInject can map A/X held to the game's skip key.

### Overlay

[Overlay.cpp](../src/Src/Overlay.cpp) is a Dear ImGui overlay drawn onto the backbuffer (INSERT toggles it; while
visible it takes the mouse and keyboard). It subclasses the game window's procedure, exposes the window handle, and can
resize the window beyond the desktop (`vr window`, research). It also hosts the reticle.

---

## 11. Support and diagnostics

| Module | Role |
|---|---|
| [Log.cpp](../src/Src/Log.cpp) | `<game folder>\DishonoredVR.log` (truncated at start; the previous run's is kept as `DishonoredVR.prev.log`), `LOGI(...)` with fmt formatting |
| [CrashLog.cpp](../src/Src/CrashLog.cpp) | installed first: a vectored handler (sees faults the engine swallows) and the unhandled filter write the faulting instruction, address, registers, module+RVA backtrace and a stack scan to the log. `SetPhase` names what the mod was doing. Backtraces are symbolized with `tools/symbolize.py` and the PDBs kept by `package.ps1` |
| [Watchdog.cpp](../src/Src/Watchdog.cpp) | a thread watching Present; after 2 s without one it logs where the render thread is (the OpenXR call it is inside, from `Watchdog::Mark` scopes, its instruction pointer and stack return addresses sampled for microseconds), the foreground window and the session state, and again later; `DVR_watchdog.txt` is rewritten twice a second; `vr stall <ms>` creates a test hitch |
| [SysInfo.cpp](../src/Src/SysInfo.cpp) | machine diagnostics in the log: Windows, CPU/RAM, GPUs, displays, keyboard layout, the OpenXR runtimes, other DLLs in the game folder, and at the first Present the GPU used and overlays that hooked it |
| [GameInfo.cpp](../src/Src/GameInfo.cpp) | the executable's base, version, folder and the supported-build check |
| [CVars.cpp](../src/Src/CVars.cpp) | engine cvars by name: the name string is found in the image, then the pointer to it, which sits at +0x30 of the 0x80-byte cvar object (int at +0x28, float at +0x2C). Direct writes bypass change callbacks; for cvars with side effects the mod uses the engine console instead |
| [Game.cpp](../src/Src/Game.cpp) | game-local and local-player accessors, the player's first-person view, and `BufferCommandText` (the engine console, queued, run by the main thread) |
| [Commands.cpp](../src/Src/Commands.cpp) | the command channel and the console: polls `<game folder>\DVR_cmd*.txt` at ~10 Hz (processed in name order, then deleted), writes `DVR_heartbeat.txt` once a second (one status line per subsystem) when `WriteStatusFile` is on, runs startup and package commands, recentres on runtime recentres and new players. Commands: `vr <name> ...` (about 150 of them, dispatched to the modules), `exec <console text>`, `set`/`get <cvar>`, `capture <tag>`, plus research commands |
| [TestAids.cpp](../src/Src/TestAids.cpp) | testing aids that call the game's own functions on the game thread: the Outsider's mark, powers, power upgrades and enhancements, black market upgrades, any inventory item, the game's own infinite-ammo counter, infinite mana/health (`vr giveall`, `vr autoall`, `vr mana inf`, `vr hp inf`, ...); also the ini's cheats. What is given lives in memory; a load forgets it (`autoall` re-gives on every new player) |
| [Seeds.cpp](../src/Src/Seeds.cpp) | shipped learned hand data (section 3) |
| [PipeTrace.cpp](../src/Src/PipeTrace.cpp) | `vr pipetrace <presents>`: an ordered trace of view builds, draws, command lists, executes, Presents and submits |
| [Watchpoint.cpp](../src/Src/Watchpoint.cpp) | research: hardware read/write watchpoints (debug registers, one thread or all) with a histogram of accessing instructions |
| [XRCapture.cpp](../src/Src/XRCapture.cpp), `VRRuntime::RequestCapture/RequestBurst` | PNG captures of the backbuffer and the eye images, single or bursts of consecutive frames |
| [XRProbe.cpp](../src/Src/XRProbe.cpp) | the startup OpenXR probe (section 3) |

### Testing without a headset

The mock runtime in `mockxr/` is selected per process with `DVR_usemock.txt` (always point it back to the real runtime
afterwards, or the headset gets nothing). It reports a fake HMD (geometry configurable with `DVR_MockHmd.txt`, e.g. a
Quest 3's eye size and fov), reads head and controller poses, buttons and scripted motions (sword swings, throws,
tracking loss, recentres, runtime stalls and session loss) from `DVR_MockInput.txt` every frame, logs haptics, draws
quad layers and writes what each eye would see. `scripts/testing/Play.ps1` launches or attaches to the game and plays a
list of steps (keys, mock input fields, mod commands, sleeps, captures). Two traps worth knowing: the game pauses
itself when its window loses focus (check `sim` in the heartbeat before trusting a capture), and keys sent by the
harness's own key helper are not seen by the game's menus the way the mod's scan-code input is (`vr tapkey` sends a key
the controllers' way).

---

## 12. Module reference

Engine hooks are Detours detours unless marked "call site" (a `call rel32` redirected to a thunk) or "vtable" (vtable
entries replaced).

| File | Responsibility | Main engine hooks / addresses |
|---|---|---|
| [Proxy/dinput8.cpp](../src/Proxy/dinput8.cpp) | forwards `DirectInput8Create`, loads the mod | - |
| [Main.cpp](../src/Src/Main.cpp) | DllMain, init order, the per-Present loop | - |
| [D3D11Hook](../src/Src/D3D11Hook.cpp) | `IDXGISwapChain::Present` / `ResizeBuffers` via a dummy device | DXGI vtable |
| [RenderRes](../src/Src/RenderRes.cpp) | render size decoupled from the window | `GetClientRect` (callers exe+0xE9FF1, exe+0x346364), `ScreenToClient` (exe+0xE27DF), WriteConfigToFile exe+0x41770, window placement exe+0x347630, `rs_enable` |
| [Overlay](../src/Src/Overlay.cpp) | ImGui overlay, window procedure, window size | window subclass |
| [VRRuntime](../src/Src/VRRuntime.cpp) | drives the session from Present, layer pose, mono/stereo submit | - |
| [XRSession](../src/Src/XRSession.cpp) | OpenXR session, frame thread, keep-alive, layers, input, haptics | OpenXR |
| [XRProbe](../src/Src/XRProbe.cpp) | startup runtime probe | OpenXR |
| [XRCapture](../src/Src/XRCapture.cpp) | textures to PNG | - |
| [CameraHook](../src/Src/CameraHook.cpp) | main view identification, fov override | exe+0x3586A0 (view constants upload), projection builder exe+0x3742B0 |
| [ViewMatrixHook](../src/Src/ViewMatrixHook.cpp) | 6DOF view composition, takedown/cutscene views, view ring | CreateViewMatrix exe+0x109CC0 |
| [PlayerViewHook](../src/Src/PlayerViewHook.cpp) | (disabled) render views of other entities | exe+0x881640 |
| [BodyView](../src/Src/BodyView.cpp) | body-vs-real instrument, `fov auto` mode | - |
| [StereoTaa](../src/Src/StereoTaa.cpp) | per-eye TAA in stereo | via GpuTrace context hooks and ViewMatrixHook |
| [StereoSync](../src/Src/StereoSync.cpp) | synchronised stereo: every second game frame held (Draw only), its gate, input reads deferred | idGameLocal::RunFrame exe+0x8ACDF0, Draw exe+0x8AC710 (called), keyboard / mouse reads exe+0xE1970 / exe+0xE2930, right-eye pass skips (`Stereo*Reuse`: exe+0x3053B0, exe+0x305370, exe+0x3102A0, exe+0x340F10, exe+0x34FD00), `r_ILProbeUpdate_MaxUpdatesPerFrame` |
| [PipeTrace](../src/Src/PipeTrace.cpp) | frame pipeline trace | - |
| [KbmInject](../src/Src/KbmInject.cpp) | controllers to keyboard/mouse, modes, head-aim servo, focus | `SendInput`; reads `g_mouseSensitivity`, `g_invertMouseYAxis` |
| [KeyBinds](../src/Src/KeyBinds.cpp) | the player's key bindings, physical key positions | reads `dishonored2Config.cfg` |
| [XInputHook](../src/Src/XInputHook.cpp) | (disabled) gamepad emulation | `XInputGetState` |
| [PadProbe](../src/Src/PadProbe.cpp) | (research) the engine's pad poll | exe+0xE0220 |
| [PlayerState](../src/Src/PlayerState.cpp) | which player states run; per-state masks | vtable slot stubs on ~90 `arkPlayerState*` vtables (RTTI) |
| [GpuTrace](../src/Src/GpuTrace.cpp) | draw hooks, MVP factorisation, weapon identification and anchoring, arm palette rewrite and IK, hand poses and grips, reticle projection | D3D11 device and context vtables (draws, Map/Unmap/UpdateSubresource, Dispatch, copies, Finish/ExecuteCommandList, shader creation) |
| [BodyHide](../src/Src/BodyHide.cpp) | player sub-entities, anchors, hiding models, what each hand holds, safe `EntityById` | calls GetComponent exe+0x877720, GetEntityById exe+0x6DC560 |
| [WeaponPose](../src/Src/WeaponPose.cpp) | drawn weapon pose into the engine's transform sets | transform update exe+0x753C10, SetOriginAndAxis exe+0x9D560 |
| [HandFx](../src/Src/HandFx.cpp) | arm joint queries answered with the drawn arm | exe+0x53D3F0, exe+0x53D6A0, entity bind exe+0x7506D0 |
| [HandAimHook](../src/Src/HandAimHook.cpp) | camera component, fire chain, CamSwap, power aim, game-thread tick | camera update exe+0x7F38E0, UpdateAim exe+0x7F4B30, shake exe+0x88C5E0, renderView copy exe+0x444C0, GetAimPoint exe+0x3C3CC0, ComputeAimTarget exe+0x638AE0, power/sensor updates (exe+0x4430D0, exe+0x3CEC30, exe+0x3C60E0, exe+0x3B6C90, exe+0x3B2280, exe+0x442850, exe+0x3C1E40) |
| [GunRay](../src/Src/GunRay.cpp) | the gun ray: sensor, muzzle, blast, effects, culling, sweep | ranged sensor exe+0x3C3FD0 / exe+0x3C3E10, weapon aim point exe+0x639670, ranged init exe+0x639F00, launch exe+0x639970, fire target exe+0x63A8B0, blast exe+0x44F030, gun shoot exe+0x44E0C0, FX exe+0x4E2B00 / exe+0x4E3090 / exe+0x19D100 / exe+0x1A2FF0, world joint query exe+0x53D800, model sphere exe+0x237340 |
| [MeleeAim](../src/Src/MeleeAim.cpp) | melee direction from the hand, contact-strike plumbing | call sites of exe+0x8067C0 / exe+0x417820 in exe+0x60D000..0x636000; cone test exe+0x62F900, SetActive exe+0x634890, hit check exe+0x631020, slots 41/42 (exe+0x634990, exe+0x62F290), gather exe+0x636B40 |
| [SwordGesture](../src/Src/SwordGesture.cpp) | slash / power attack / block gestures | - |
| [SwordContact](../src/Src/SwordContact.cpp) | the drawn blade's contacts | via MeleeAim |
| [NpcSkel](../src/Src/NpcSkel.cpp) | characters' current skeletons (ragdolls included) | reads entity+0x360 -> pose array +0x2D8 |
| [ThrowGesture](../src/Src/ThrowGesture.cpp) | grenades thrown with the hand's velocity | call site exe+0x63333C |
| [GrabHand](../src/Src/GrabHand.cpp) | held objects in the hand, selection, throw, carrying | interaction sensor exe+0x3B8D60 / exe+0x3B8430, call site exe+0x83E473, joint queries via HandFx |
| [PointerRay](../src/Src/PointerRay.cpp) | the stable per-hand pointer | - |
| [Reticle](../src/Src/Reticle.cpp) | per-hand reticle and debug axes (ImGui) | - |
| [ScreenMode](../src/Src/ScreenMode.cpp) | menus on a room screen, wheel panel | via ViewMatrixHook, CameraHook, WristHud, XRSession |
| [WristHud](../src/Src/WristHud.cpp) | interface copy, wrist watch, letterbox removal | via GpuTrace draw hooks; `ark_ui_show_master_hud` |
| [BinkVideo](../src/Src/BinkVideo.cpp) | is a pre-rendered video playing | `bink2w64.dll` exports (BinkOpen, BinkOpenWithOptions, BinkClose, BinkDoFrame, BinkDoFrameAsync, BinkNextFrame) |
| [Settings](../src/Src/Settings.cpp) | the ini, turned into commands | - |
| [VrTweaks](../src/Src/VrTweaks.cpp) | VR-only render changes through the main view's render flags (no depth of field, optionally no AO) | view+0x3A37 bits 0 / 1 (DOF pass exe+0x315634, SSAO pass exe+0x305BF3) |
| [Commands](../src/Src/Commands.cpp) | command channel, console, heartbeat, package activation | engine console (`BufferCommandText`) |
| [TestAids](../src/Src/TestAids.cpp) | cheats and test aids via the game's own functions | GiveMark exe+0x4C1E50, AddItem exe+0x6156E0, AddEquipmentUpgrade exe+0x4557A0, component typeinfos |
| [Game](../src/Src/Game.cpp), [GameInfo](../src/Src/GameInfo.cpp), [CVars](../src/Src/CVars.cpp) | engine accessors, build facts, cvars | game local exe+0x39A5290, GetLocalPlayer exe+0x8A8830, command system exe+0x228BAE0 |
| [CrashLog](../src/Src/CrashLog.cpp), [Watchdog](../src/Src/Watchdog.cpp), [SysInfo](../src/Src/SysInfo.cpp), [Log](../src/Src/Log.cpp) | diagnostics | - |
| [Seeds](../src/Src/Seeds.cpp) | shipped hand data | - |
| [Watchpoint](../src/Src/Watchpoint.cpp) | research watchpoints | debug registers |
| [VRMath.h](../src/Src/VRMath.h) | XR/engine conversions, head composition | - |

---

## 13. Threads and safety rules

Most of what can go wrong in this mod is a race, a stale pointer or a call the engine does not expect. These rules
were each learned from a crash, a freeze or a silent quit.

**Where things run**

- The game's main thread runs the window procedure, the player camera's update and Present: the whole per-frame loop
  of the mod (`KbmInject::Tick`, the OpenXR submit, the command channel) runs there. Anything slow on it is a stall the
  headset sees (an item-table scan once cost 1.4 s per call and froze the game after every load). Keep it cheap.
- Draw calls are recorded on many deferred contexts on many threads at once. A draw hook must not allocate D3D
  resources, must not map a resource another context may be using, and must restore every binding it changes
  (including "nothing was bound"). State shared between draw hooks is per thread (`thread_local`) or lock-free.
- OpenXR calls that may block (`xrWaitFrame`, session begin/end, event polling) belong to the frame thread. The render
  thread waits for it with a bound; never call `xrWaitFrame` from Present. The keep-alive frames use the device from the
  frame thread, which is why D3D11 multithread protection is enabled.
- Player sensors and power components run on job threads. A CamSwap is visible to every other thread for its duration:
  take the CamSwap mutex, and readers on other threads use `HandAimHook::RealCamera` and reject one-frame jumps.

**Never do**

- Never read engine globals from `DllMain` or the init thread: wait for the first Present.
- Never call the raw `GetEntityById` (exe+0x6DC560): during a level change the world global it reads is NULL and it
  faults. Use `BodyHide::EntityById`.
- Never write through an entity or render-model pointer kept from a previous frame without revalidating it the same
  frame: a load frees them while pointers are still held.
- Never call the interaction sensor's RESULTS phase (exe+0x3B8430) twice in one frame: the second call asks for query
  ids the first retired, and the engine raises a fatal "outdated or corrupted id" error and quits with no exception and
  no crash log.
- Never detour a function with fewer arguments than it really has: stack arguments are lost. Several hooks here
  (the Far Reach landing computation, the entity bind with seven arguments, the five-argument config writer) were
  first written wrong and crashed or corrupted output. Hook the wrapper rather than a worker with stack arguments when
  in doubt.
- Never hook the one-line camera accessors (exe+0x8067C0, exe+0x417820) themselves: they are shared by the whole game.
  Redirect specific call sites.
- Never write the camera's fields to aim something for one call outside a CamSwap, and never leave them written.
- Never move the player's equipment ENTITY: the weapon identification matches draws against its real position.
- Never answer a joint query of a model bound to a held weapon with the drawn pose: its draws already ride the
  weapon's GPU delta, and it would be moved twice.
- Never send a mouse button unless `CursorOnGame` says the cursor is over the game's client area.
- Never send keys or mouse motion into a menu except the menu controls: in a save screen a stray click overwrites a
  save.
- Never run the console's `writeConfig` on the player's config, and back up `dishonored2Config.cfg` before testing
  anything that writes it.
- Never trust a hook that "had no effect" until a counter proves it ran: many dead ends in the research were hooks that
  were never called, or buffers that were bound but not read.

**Floating-point and file hygiene**

- Test NaN explicitly (`std::isfinite`) before range checks on memory scanned from the game: NaN passes every
  comparison.
- The C runtime is linked statically, so the game's locale cannot change how the mod formats or parses decimals; the
  ini parser accepts a comma as a decimal point anyway.
- Line endings are mixed in the repository (a few files are LF, most CRLF): an editor that rewrites a whole file buries
  the real change. Check diffs with whitespace ignored.
