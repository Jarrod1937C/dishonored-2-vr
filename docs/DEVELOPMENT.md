# Development guide

How to build, run, test, debug and release the Dishonored 2 VR mod. Organised by task. Read
[ARCHITECTURE.md](ARCHITECTURE.md) for how the mod works and [ENGINE.md](ENGINE.md) before touching engine internals.

Placeholders used below:

| Placeholder | Means |
|---|---|
| `<repo>` | your clone of this repository |
| `<Steam>` | your Steam library, by default `C:\Program Files (x86)\Steam` |
| `<game folder>` | `<Steam>\steamapps\common\Dishonored2` (where `Dishonored2.exe` is) |
| `<saves>` | `%USERPROFILE%\Saved Games\Arkane Studios\Dishonored2\base` (the game's `savegame\` folder and `dishonored2Config.cfg`) |

> Many scripts still hard-code the game folder and the original developer's repository path (see
> [Known machine-specific paths](#known-machine-specific-paths)). Until they are parameterised, edit the `$repo` /
> `$game` lines at the top of a script before running it.

---

## 1. Requirements

| What | Version / notes |
|---|---|
| Windows | 10/11 x64. Everything is driven from Windows PowerShell 5.1 (the scripts use `Add-Type`, `System.Windows.Forms`, `keybd_event`). |
| Visual Studio 2022 | MSVC x64 toolset, C++20, Windows SDK ("Desktop development with C++"). The scripts use the CMake generator `Visual Studio 17 2022`. `build.ps1` and `build-mockxr.ps1` look for the CMake bundled with **VS 2022 Build Tools** at its default path and fall back to a `cmake` on `PATH`; `package.ps1` looks for `dumpbin.exe` under the Build Tools path only. |
| CMake | 3.25 or newer (`cmake_minimum_required` in `src/CMakeLists.txt`). |
| vcpkg | Manifest mode (`src/vcpkg.json`, with a `builtin-baseline`, so a git clone of vcpkg is needed). Triplet **`x64-windows-static`**: the dependencies and the mod are linked with the static CRT (`/MT`), so the DLLs need no Visual C++ redistributable. (Older notes say `x64-windows-static-md`: that is out of date; `build.ps1` reconfigures a build tree made for another triplet from scratch.) Dependencies: `detours`, `fmt`, `imgui[dx11-binding,win32-binding]`, `openxr-loader`. |
| Python 3 | For `tools\` and the analysis scripts in `scripts\testing\`. Packages: `numpy`, `Pillow` (image analysis), `capstone` (the static RE tools). |
| Git | The build stamps `git describe --always --dirty` into the DLLs. |
| Dishonored 2 | **Steam build 1.77.9.0**, the current Steam executable. The mod checks version + file size + link timestamp (`GameInfo`) and stays inactive (a message box, no hooks) on anything else: the GOG exe and Death of the Outsider are different executables. Steam app id `403640`. |
| OpenXR runtime | Any (Meta Horizon Link, SteamVR, Virtual Desktop...). Not needed for most development: the [mock runtime](#5-the-mock-openxr-runtime) stands in for a headset. |

The community *Dev Console Enabler* is also a `dinput8.dll`: the two cannot be installed together.

### First-time setup

```powershell
git clone <this repository> <repo>
cd <repo>
git clone https://github.com/microsoft/vcpkg local\vcpkg     # local\ is git-ignored
local\vcpkg\bootstrap-vcpkg.bat
powershell -ExecutionPolicy Bypass -File scripts\build.ps1   # first configure builds the vcpkg dependencies (slow)
```

`build.ps1` finds vcpkg in this order: `-Vcpkg <dir>`, `<repo>\local\vcpkg`, then `%VCPKG_ROOT%`.

---

## 2. Repository layout

| Path | What |
|---|---|
| `src/CMakeLists.txt` | The build: both DLLs, the version number, `/MT`, PDBs in Release. |
| `src/vcpkg.json` | vcpkg manifest. Its `version-string` is unrelated to the mod's version. |
| `src/CMake/FindDetours.cmake` | Find module for Detours. |
| `src/Proxy/` | `dinput8.dll`: forwards `DirectInput8Create` to the system DLL and `LoadLibrary`s `DishonoredVR.dll` from the game folder. |
| `src/Src/` | The mod. `Main.cpp` (init order, the per-Present call list), `Commands.cpp` (command files, heartbeat, the `vr` command chain), one module per feature (`XRSession`, `VRRuntime`, `KbmInject`, `GpuTrace`, `ViewMatrixHook`, `GunRay`, `SwordContact`, `ScreenMode`, `WristHud`, `RenderRes`, `StereoTaa`...), `Settings` (the ini), `CrashLog`, `Watchdog`, `Log`, `SysInfo`, `TestAids`, `Seeds` + generated `SeedsData.inc`, `Version.h.in`/`Version.rc.in`. |
| `mockxr/` | The mock OpenXR runtime (`mock_runtime.cpp`, one file). Test tool only. |
| `package/` | Sources of the release zip's text files: `DishonoredVR.ini` and `README.txt` (`{VERSION}` and `{REPO_URL}` are substituted). |
| `scripts/` | `build.ps1`, `build-mockxr.ps1`, `deploy.ps1`, `undeploy.ps1`, `run-test.ps1`, `package.ps1`. |
| `scripts/testing/` | Not in the public repository (see section 6): the headless driver `Play.ps1` and the test batteries. |
| `tools/` | Python: `symbolize.py` (crash lines -> functions), `make_seeds.py` (regenerate `SeedsData.inc`), static RE helpers for `Dishonored2.exe`. |
| `docs/` | `ARCHITECTURE.md` (how the mod works), `ENGINE.md` (engine reference), `STATUS.md` (features and known issues), this file, `cvars-1.77.9.0.txt` (cvar dump). |
| `build/` | Mod build tree (ignored). Output in `build\bin`, generated headers in `build\generated`. |
| `build-mockxr/` | Mock build tree (ignored). |
| `dist/` | Release zips (ignored). |
| `local/` | Per-machine scratch (ignored): vcpkg, `shots\` (captures), `releases\` (PDBs per release), `mockin_state.txt`, backups. |

No game assets or Arkane/Bethesda code belong in the repo, and nothing is ever built inside the game folder.

---

## 3. Build, deploy, undeploy

```powershell
scripts\build.ps1 [-Config Release|RelWithDebInfo] [-Vcpkg <dir>]
scripts\deploy.ps1 [-GameDir <game folder>]
scripts\undeploy.ps1 [-GameDir <game folder>]
scripts\build-mockxr.ps1          # after the first build.ps1 (it uses the mod's vcpkg OpenXR headers)
```

- `build.ps1` configures `build\` on first use (and whenever the triplet changed), otherwise re-runs the configure step
  so `Version.h` picks up the current commit, then builds. Outputs: `build\bin\dinput8.dll`, `DishonoredVR.dll` and
  their `.pdb` files.
- `deploy.ps1` copies `dinput8.dll`, `DishonoredVR.dll` and `DishonoredVR.pdb` into `<game folder>`. Nothing else
  (the ini, the autoexec and the test files are yours to create).
- `undeploy.ps1` removes those three files and `DishonoredVR.log`. It leaves `DishonoredVR.ini`, `DishonoredVR.prev.log`
  and every `DVR_*` file; delete them by hand for a truly vanilla folder.
- `build-mockxr.ps1` builds `build-mockxr\bin\DVR_MockXR.dll` and writes its loader manifest
  `build-mockxr\DVR_MockXR.json`.

**Close the game before building and before deploying.** A running game keeps the DLLs (and `build\bin\dinput8.pdb`)
open; a deploy onto a locked DLL fails and the next run silently uses the old build. Confirm the build you are running
from the first log line: `DishonoredVR <version> (<commit>[-dirty]) loaded`.

`run-test.ps1 [-Screenshot <png>] [-TimeoutSec 90] [-SettleSec 8] [-KeepRunning]` is a smoke test: it starts
`Dishonored2.exe` directly with `DVR_OVERLAY_AUTOSHOW=1`, waits for `first Present` in the log, optionally screenshots
the primary screen, prints the log and kills the game. A direct exe launch skips Steam's ownership check (the game then
has no saves), so for anything beyond "does it load", use `Play.ps1`, which launches through `steam://rungameid/403640`.

---

## 4. Files in the game folder

The mod reads and writes everything next to `Dishonored2.exe` (the game's working directory when Steam launches it).

| File | Who writes it | Purpose |
|---|---|---|
| `DishonoredVR.ini` | you | Player settings (see below). Optional: every key has a default in code. |
| `DVR_autoexec.txt` | you (dev only) | `vr ...` / engine lines run once per launch, after the ini's package settings. `#` comments a line out. |
| `DVR_usemock.txt` | you / scripts | One line: an OpenXR runtime manifest path. Sets `XR_RUNTIME_JSON` for this process. |
| `DVR_validate.txt` | you (dev only) | One line: the folder of the Khronos core validation layer. Loads it; report in `DVR_xrvalidation.html`. |
| `DVR_cmd.txt`, `DVR_cmd_<n>.txt` | scripts | Command files: executed and deleted (section 7). |
| `DVR_MockHmd.txt`, `DVR_MockInput.txt`, `DVR_MockCompose.txt`, `DVR_MockRuntimeName.txt` | scripts | Read by the mock runtime only (section 5). |
| `DishonoredVR.log`, `DishonoredVR.prev.log` | mod | The log, and the previous session's (section 8). |
| `DVR_heartbeat.txt` | mod | Status snapshot, once a second, when `WriteStatusFile = 1`. |
| `DVR_watchdog.txt` | mod | Watchdog state, twice a second, even while the render thread is stuck. |
| `DVR_grips.txt`, `DVR_hands.txt` | mod | Learned weapon grips / hand data. Seeded from the DLL when absent (`Seeds`). |
| `DVR_<tag>_frame.png`, `_eye<n>.png`, `_b<nn>.png` | mod | Captures (`capture`, `vr burst`); `Play.ps1 shot:` moves them to `<repo>\local\shots\`. |
| `DVR_MockXR.log`, `DVR_seen_<tag>_eye<n>.bmp` / `.txt` | mock | Mock runtime log; compositor output. |
| `DVR_cpnt_*.txt`, `DVR_model_*.txt`, `DVR_shader*.txt`, `DVR_vb*`, ... | mod | Output of research commands. |

### DishonoredVR.ini and how it maps to commands

The shipped file is [`package/DishonoredVR.ini`](../package/DishonoredVR.ini); the key table and its defaults are in
[`src/Src/Settings.cpp`](../src/Src/Settings.cpp) (`kKeys`). Parsing: sections + `Key = value`, case-insensitive,
`;` or `#` comments (also after a value), UTF-8 with or without BOM (UTF-16 is rejected with a log line), decimal comma
accepted, floats clamped to their range. A missing file, key or bad value falls back to the default and the log says so
(`Settings: ...`). The effective values are logged once as `settings: ...`.

Settings are not a separate code path: each one turns into the same `vr` commands a developer types.

- `Settings::StartupCommands()` run on the first Present: turning, quick-slot hold, layout, `FocusGameWindow`
  (`vr autofocus`), the menu screen (`vr screen`, `vr screencfg`), cutscene bars/arms, `KeepAliveFrames`,
  `NewSessionAfter` (`vr xrkick`).
- `Settings::PackageCommands()` run when the VR package switches itself on (below): sword/throw/grab gestures,
  reticle, wrist HUD, `Stereo3D`, motion blur (`exec r_skipCameraMotionBlur 1`), cheats (`vr autoall`, `vr mana`,
  `vr ammo`, `vr infitems`, `vr hp`).
- `RenderScale`, `ControllerPitchOffset`, `Haptics`, `LogControllerInput`, `WriteStatusFile`, `VrGraphicsTweaks`
  (VrTweaks) and the experimental `Stereo*Reuse` / `StereoStreamOncePerPair` (StereoSync, read before its hooks are
  installed) are read directly.

`WriteStatusFile` defaults to **1 in code** (a dev setup has no ini, so the heartbeat is on) and is **0 in the shipped
ini**. The shipped ini may also carry non-default values chosen for testers (e.g. `Reticle`, `[Cheats]`): read it before
copying it into your game folder.

To add a setting: add a row to `kKeys`, emit the command in `StartupCommands`/`PackageCommands` (or read it with a
getter), and document it in `package/DishonoredVR.ini` and `package/README.txt`.

### Auto-activation and DVR_autoexec.txt

With `vr auto 1` (default), once a player has been simulated for ~90 frames **and the OpenXR session is running**, the
mod runs `vr weapon6dof 1` (the whole VR package), recentres, runs the ini's package commands, then every line of
`DVR_autoexec.txt`. This happens once per launch; the package survives loads and level changes. Without a running
session the game stays the flat game and the package switches on when the headset appears. Typing
`vr weapon6dof ...` yourself marks the auto step as done.

`DVR_autoexec.txt` applies to every save you load, including a real playthrough, and an autosave keeps whatever a
cheat gave. Remove it (or comment its lines) when you play for real.

### DVR_usemock.txt: choosing the runtime

The OpenXR loader in this build reads the active runtime from `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` and ignores
`HKCU`, so a machine whose HKLM entry points at another runtime than the one you want needs `DVR_usemock.txt` to pin it.
`Play.ps1 -UseMock` writes the mock's manifest there; `Play.ps1` without `-UseMock` (and without `-Attach`) writes Meta's
manifest if it is installed at its default path, or deletes the file.

**After any mock test, point `DVR_usemock.txt` back to your real runtime's manifest (or delete it).** Otherwise the next
launch from Steam (yours, with the headset on) talks to the mock and the headset gets nothing.

### DVR_validate.txt: the OpenXR validation layer

Build the Khronos `core_validation` API layer from OpenXR-SDK-Source yourself (no script for it is in the repo). Put the folder holding
`XrApiLayer_core_validation.json` + `.dll` on the file's single line. The report is `DVR_xrvalidation.html`; empty of
messages = no violations.

---

## 5. The mock OpenXR runtime

`DVR_MockXR.dll` is a minimal OpenXR runtime: a fake stereo HMD, a real D3D11 session and swapchains, Touch-like
controller actions, haptics (logged), and a frame loop with real display times. It lets the whole OpenXR path (instance,
session, swapchains, frames, input, recenter, session loss) run in the real game with no headset. It is how almost
everything in this mod was measured; it is not a guarantee of how a real runtime behaves.

Use it: `scripts\build-mockxr.ps1`, then `scripts\testing\Play.ps1 -UseMock ...`. It reads its files from the game
folder (the process's working directory):

| File | Content |
|---|---|
| `DVR_MockHmd.txt` | `w h fovL fovR fovU fovD`: per-eye pixels and the LEFT eye's fov in degrees (the right eye is mirrored). Absent = 1024x1024, symmetric 90 deg. The Quest 3 geometry the tests use: `1824 1968 -54 40 44 -55` (`body\boot.ps1` writes it). |
| `DVR_MockInput.txt` | One line of up to 62 space-separated numbers (below). Re-read every frame. Until it exists, the head does a slow yaw sweep. |
| `DVR_MockCompose.txt` | One word (a tag), optionally followed by `world`. The next `xrEndFrame` composes every submitted layer (projection + quads) into each eye's full fov, on the CPU, with a rotation-only reprojection: `DVR_seen_<tag>_eye<i>.bmp` + `DVR_seen_<tag>.txt`. Pixels no layer reaches are dark blue. `world` = compose for a world-fixed eye (world-swim tests). The file is deleted after use. |
| `DVR_MockRuntimeName.txt` | The runtime name to report (e.g. `SteamVR/OpenXR`) to exercise runtime-specific branches. |

`DVR_MockXR.log` is the mock's log (truncated once per process); every gap of more than 1 s between two frames of a
running session is logged as `HOURGLASS`.

### DVR_MockInput.txt fields

Angles in degrees, positions in metres. The head sits at (0, 1.6, 0) plus `headX/Y/Z`; the right controller at
(0.2, 1.4, -0.3) plus `handX/Y/Z`; the left at (-0.2, 1.4, -0.3) plus `lhandX/Y/Z`. Buttons are 0/1. Rotations are
yaw * pitch * roll (roll about the controller's own pointing axis).

| # | Fields | Meaning |
|---|---|---|
| 1-4 | `turnX trigR handYaw handPitch` | right stick X, right trigger, right controller yaw/pitch |
| 5-9 | `headYaw headPitch headX headY headZ` | head pose (offsets) |
| 10-14 | `moveX moveY handX handY handZ` | left stick, right controller position offset |
| 15-18 | `btnA btnB btnX btnY` | face buttons |
| 19-25 | `turnY trigL gripL gripR thumbL thumbR menu` | right stick Y, left trigger, grips, stick clicks, menu |
| 26-30 | `lhandYaw lhandPitch lhandX lhandY lhandZ` | left controller pose |
| 31-32 | `handRoll lhandRoll` | wrist roll |
| 33-35 | `rhandLost lhandLost headLost` | 1 = tracking lost (valid bits cleared, zero pose: the worst a runtime may return) |
| 36 | `recenter` | counter: each change = a runtime recenter (moves the LOCAL space) |
| 37-48 | `swing swKind swAx swAy swAz swFrom swTo swDur swPivX swPivY swPivZ swRet` | swing generator: each change of `swing` starts one smooth minimum-jerk motion of a hand on top of its pose. `swKind` 0 = rotation by an angle about the axis through a pivot, 1 = translation along the axis. Timeline: 0 -> `swFrom` in `swRet` s, `swFrom` -> `swTo` in `swDur` s, hold 0.3 s, back in `swRet` s. |
| 49-50 | `swHand swRelL` | 1 = the swing moves the LEFT hand; left-trigger release time into the gesture (scripted throws) |
| 51-52 | `headSway handJit` | head sway amplitude (deg), hand tremor amplitude (deg, 5-10 Hz) |
| 53-54 | `sessionLoss noHeadset` | counter: each change = `LOSS_PENDING` then `SESSION_LOST`; 1 = `xrGetSystem` reports no headset |
| 55 | `headSpin` | steady head turn, deg/s |
| 56-62 | `paceHz xrStall stallSec stallAll xrStop readyAfter starve` | runtime trouble: compositor pacing; counter = a stall of `stallSec` s then the session is stopped (negative at startup = the first frames stall); `stallAll` 1 = every call blocks; counter = session taken away; seconds before a stopped session is offered back (< 0 never); stop the session after that many seconds without frames |

Traps:

- **Decimal commas.** A Spanish/German PowerShell formats `0.5` as `0,5`; the mock's `sscanf` stops there and silently
  drops every later field. Always format with `InvariantCulture`. `Play.ps1 mi:` does this for you.
- **Unspecified fields keep their previous values.** `mockin:` writes a raw line (fields you omit at the end keep what
  the mock last read); `mi:name=value~name=value` keeps the full state in `<repo>\local\mockin_state.txt` and rewrites
  every field; `mi:reset` zeroes it.
- **`headY` is an offset** added to the 1.6 m standing height, not the height.
- `hand*` is the RIGHT controller, `lhand*` the LEFT.
- Counters (`swing`, `recenter`, `sessionLoss`, `xrStall`, `xrStop`) act on a **change**; the first read only takes
  them as the baseline.
- Moving a hand with `mi:` jumps it between two frames, i.e. at infinite speed: with the sword drawn that can count as
  a strike. Use `vr swing 0` while repositioning, and the swing generator for real motions.
- The file is written by write-then-rename; write it the same way from your own scripts.

### Captures

`capture <tag>` (or `Play.ps1 shot:<tag>`) saves the backbuffer (`_frame.png`) and, while OpenXR runs, each eye's
swapchain image (`_eye0.png`, `_eye1.png`). These show what the mod rendered; to see what lands in the eye, use the
mock compositor (`DVR_MockCompose.txt`, wrapped by `body\seen.ps1`, `menu\compose.ps1`, `wrist\watch.ps1`).
`vr burst <n> [tag] [every]` saves n consecutive frames.

---

## 6. Automated testing

> **Not in the public repository.** The test harness described here (`scripts/testing/`: `Play.ps1` and one folder of
> test batteries per area) is tied to the author's machine and save games, so it is not published. It is documented
> because the techniques carry over: the mock runtime (section 5), the `vr` commands (section 7) and the command files
> are all you need to script your own tests.

### Play.ps1

```powershell
scripts\testing\Play.ps1 [-UseMock] [-KeepAlive] [-Attach] [-BootTimeoutSec 90] -Steps <step>,<step>,...
```

Without `-Attach` it kills any running game, deletes the heartbeat and old captures, launches through Steam and waits
for the heartbeat (so the heartbeat must be enabled: no ini, or `WriteStatusFile = 1`). With `-Attach` it drives the
running game; if the game is not running, it **launches a new one** (and the log rotates to `.prev.log`): a "still
working" after a crash may be a fresh game at the title screen. Without `-KeepAlive` the game is killed at the end.

| Step | Does |
|---|---|
| `sleep:<s>` | wait, in SECONDS with decimals (`sleep:0.4`; `sleep:1000` is 17 minutes) |
| `key:<KEY>[xN]` | focus the game and press a key N times (`Focus-Key.ps1`; `SHIFT+TAB` style combos) |
| `hold:<KEY>~<ms>` | hold a key |
| `click:<n>` / `clickat:<fx>x<fy>` | left click(s) / click at a fraction of the client area |
| `mouse:<dx>x<dy>` | relative mouse motion |
| `focus` | bring the game window to the front |
| `cmd:<text>` | send a mod command (writes `DVR_cmd_<n>.txt`) |
| `mockin:<values>` / `mi:<name>=<v>~...` / `mi:reset` | mock input, raw / by name |
| `shot:<name>` | mod-side capture -> `local\shots\<name>_frame.png` (+ `_eyeN.png`) |
| `screen:<name>` | desktop screenshot -> `local\shots\<name>.png` |
| `hb` / `log:<n>` | print the heartbeat / the last n log lines |

Known limits and traps:

- PowerShell splits `-Steps` on commas, so pairs use `x` or `~` (`clickat:0.7x0.6`, `hold:W~1500`).
- **Long step lists have lost steps silently** (seen from about 4-8 steps on). Keep lists short and chain several
  `-Attach -KeepAlive` invocations.
- Commands are processed asynchronously (polled about 10 times a second from the game's Present). A `cmd:` and a key in
  the same invocation race (e.g. `cmd:set g_stopTime 0` then a key: the key may land in a frozen game). Several
  `Play.ps1` calls in the same second can collide on command file numbers: one command per call, or wait.
- The heartbeat lags about 1 s: wait before reading it.
- `Focus-Key.ps1` only knows the key names in its table; **an unknown name sends SPACE without an error** (`ESC`, not
  `ESCAPE`). Synthesised keyboard events are also not received like the mod's own scancode `SendInput`: measure menu
  keys with `cmd:vr tapkey <KEY> [ms]`.
- A `SHIFT+TAB` from `Focus-Key.ps1` once left Shift logically held in the game until a lone Shift was sent.
- In PowerShell, `$p` and `$P` are the same variable, and `MI` is an alias of `Move-Item`.

### The game pauses when it loses focus

The game pauses itself when its window is not in front, and a capture then shows the pause menu. `win_pauseOnFocusLoss 0`
does **not** prevent it (the mod's `set` does not know that cvar; `exec` accepts it and the game pauses anyway). Batteries
detect the pause (`sim 0` in the heartbeat while time runs) and dismiss it before measuring. Check `sim` twice before
sending ESC: ESC on a running game opens the pause menu. Any window that steals focus (a firewall prompt, the headset's PC
app) does the same. The controllers also only reach the game while it is the foreground window.

To freeze the game for still captures: `cmd:set g_stopTime 1` (the mod then sees `sim 0` and its reticle self-gates: add
`vr menuinput 1`); `exec g_runFrames <n>` steps it. Never judge a hand or weapon from captures of an animating game.

### Comparing captures

`scripts\testing\Diff-Shots.ps1 -Pairs "a,a2","a,b" [-Full]`: mean absolute pixel difference (bottom-left quadrant, or
the whole frame with `-Full`). Always measure a control pair first: idle animation alone gives 15-30. Look at the PNGs.

### Test batteries (`scripts\testing\<area>\`)

Most areas have a `boot.ps1` (close the game, deploy, boot on the mock, load a specific save with the controllers or
menu keys, enable god mode / cheats) and one or more measuring scripts that print PASS/FAIL or numbers and leave
captures in `local\shots\`. Many print which captures to look at before trusting a result.

| Area | Scripts | Covers |
|---|---|---|
| `aa` | `boot`, `mv`, `quality`, `robust` (+ `.py`) | per-eye TAA in stereo (motion vectors, jaggies, flicker, robustness) |
| `stereo` | `motionall`, `jumps`, `soak`, `loadsave`, `servo`, `walk`, `simlog` (+ `.py`) | stereo modes in motion (disparity vs a still reference, eye timing), input in held frames, long soaks, loads |
| `aids` | `boot` | load a save with god mode and the testing aids |
| `body` | `boot`, `seen`, `armposes`, `swim`, `statyaw`, `swimbattery` (+ `.py`) | what each eye sees (mock compositor), arm poses vs a real arm, world swim |
| `cine` | `boot -Save altar\|ledge\|table\|skiff\|doctor\|market`, `cine`, `move`, `ride` | cutscenes, hard landing, mantle/slide, cutscenes that carry the player |
| `controls` | `boot`, `controls -Part journal\|pause\|quick\|wheel\|potions` | the controller layout |
| `freeze` | `freeze [-Boot] -Part stall\|stop\|hitch\|all` | runtime stalls, sessions taken away, game hitches (mock fields 56-62) |
| `grab` | `boot`, `right`, `compare`, `fitcheck`, `abcheck`, `push`, `continuar` | grabbing with the pointer ray, fit in the fist |
| `gun` | `boot -Save corvo\|emily`, `shoot`, `battery`, `aim`, `ringaim`, `headturn`, `motion`, `fxburst`, `parts`, `reloadparts`, `bisect` | crossbow/pistol ray, muzzle effects, parts on the drawn gun, which draw puts what on screen |
| `hands` | `grip`, `rest`, `shadowwalk`, `mantlecrouch` (+ `gripsum.py`) | hand on a weapon, empty-hand pose, Shadow Walk |
| `jitter` | `burst`, `phase` (+ `sheet.py`) | hand tremor while moving |
| `menu` | `boot`, `screen`, `compose`, `scenes -Part shop\|choice\|skiff`, `newgame` | the menu screen, wheel panel, menus over the world (`newgame` overwrites save space 03) |
| `powers` | `boot`, `bootdw`, `battery`, `aimtest`, `aimon`, `equip`, `fxbattery`, `fxdest`, `possess`, `rat`, `load`, `reload*`, `sample`, `hb` | powers aimed with the hand, effects out of the hand, possession |
| `release` | `quickboot -Runtime mock\|meta\|system\|none [-Ini f]`, `connection`, `servo`, `keybind`, `misc`, `fresh-install` | release hardening: runtime selection, ini parsing, headset loss, mouse settings, rebound keys, a tester's first run |
| `res` | `resolution` | render resolution vs window, title menu, saves, the player's cfg |
| `robustness` | `boot`, `corvo1`, `enter`, `reload1`, `reload2`, `reload2check` (+ `td.py`) | loads and reloads, arms re-identified after a load |
| `swim` | `surface` | swimming at the surface |
| `sword` | `boot -Save corpse\|shelf\|table\|guard`, `battery`, `contact`, `combat`, `power` (+ `tdstates.py`) | sword gestures, contact, combat, charged attack |
| `throw` | `boot`, `throw`, `handshots`, `wheelpick`, `wheelslot` | grenades by hand, picking from the power wheel |
| `wrist` | `layout`, `watch` | HUD crop rule, the wrist watch |

Scripts that change player files: `release\keybind.ps1` rebinds keys in `dishonored2Config.cfg` (backs it up and
restores it); `res\resolution.ps1` reads/compares it; `release\fresh-install.ps1` **deletes every `DVR_*` and
`DishonoredVR*` file in the game folder** (keeping only the mock's input files) and unpacks the newest zip there, without
restoring anything.

### Test saves

The batteries load specific saves by position in the load menu (`-Down`, `-Tabs`, `-Space`, `-Save`).
They were written against the author's own saves, and the positions shift with every new save: on another machine
you need your own saves at equivalent spots and must adjust those parameters. Back up `<saves>\savegame` before tests that save, and never pick "Save" in a scripted
menu sequence (positions differ between the pause menu and the load list).

---

## 7. The in-game console and commands

The mod has no typed console of its own. Commands reach it as text files: every `DVR_cmd*.txt` in the game folder is
read in name order, each line executed, and the file deleted (polled from the game's Present, so nothing runs while the
render thread is stuck). From a script: `Play.ps1 -Attach -KeepAlive -Steps "cmd:vr reticle 0"`, or write a file
yourself (write to a temp name and rename). The overlay (**INSERT** in the game window, or `DVR_OVERLAY_AUTOSHOW=1`)
shows the exe version, fps, OpenXR state, editable cvars and the log tail; it is drawn on the backbuffer, so it also
reaches the headset. Results of commands go to `DishonoredVR.log`; the `*cfg` commands print their whole configuration
when given no arguments.

### Top-level commands

| Command | Does |
|---|---|
| `capture <tag>` | backbuffer + eye images -> `DVR_<tag>_*.png` |
| `set <cvar> <value>` / `get <cvar>` | write/read a cvar the mod's resolver knows ("unknown cvar" otherwise: use `exec`) |
| `exec <text>` | queue a line into the engine's own console (`idCmdSystem::BufferCommandText`), e.g. `exec god`, `exec g_runFrames 1` |
| `log <text>` | write a marker into the log |

Engine notes: `ark_god_player` only holds via `exec god`; `vr hp inf` is the reliable infinite health. Do not use
`exec setviewpos` (it takes the player out of the map). **Never run the engine's `writeConfig`** (see section 10).

### `vr` commands that matter

Arguments in `<>`. The value used when a number is omitted differs per command (often 1, but e.g. `vr menuinput` 0,
`vr animcam` 2, `vr swing` 7, `vr gunray` 63). Check
[`Commands.cpp`](../src/Src/Commands.cpp) and the module `Command()` functions (`BodyView`, `RenderRes`, `StereoTaa`,
`ScreenMode`, `KbmInject`, `GunRay`) for the full set.

| Area | Commands |
|---|---|
| Package | `vr auto 0\|1` (auto-activation), `vr weapon6dof 0\|1` (the whole VR package: weapons and hands on the controllers, head aim, body, reticle, fov auto), `vr body 0\|1`, `vr hands 0\|1`, `vr hidearms`, `vr hidebody` |
| View / render | `vr stereo 0\|1\|2` (mono, alternating eyes, synchronised), `vr eyeproj 0\|1\|2 [margin]` (per-eye frustum), `vr layerfov 0\|1`, `vr stereohealth 0\|1`, `vr simlog <n>` (frames to DVR_simlog.txt), `vr syncdbg <mask>` (64 input read on held frames, 256 no pair staged, 512 no gate), `vr stereotaa 0\|1\|2` (0 TAA off in stereo, 1 per-eye TAA, 2 shared history), `vr fov auto [margin] \| off \| <x> <y>`, `vr ortho 0\|1`, `vr rendersize auto\|off\|<w> <h>`, `vr renderscale <s>` (0 = off), `vr renderres` (status), `vr window <w> <h>\|off`, `vr recenter`, `vr headaim 0\|1`, `vr vm 0\|1` (view matrix hook) |
| Scripted views | `vr animcam 0\|1\|2`, `vr animstate <State> <mask>`, `vr cinebars`, `vr cinearms`, `vr cinetrack`, `vr cinevehicle`, `vr cinecut <m> <deg>`, `vr cinecontrols` |
| Controls | `vr kbm 0\|1`, `vr menuinput 0\|1` (default 0: controllers do not drive game menus), `vr snap <deg>` (0 = smooth turn), `vr turnspeed <deg/s>`, `vr quickslots 0\|1`, `vr quickkeys <keys>`, `vr quickhold <s>`, `vr potions`, `vr autofocus 0\|1`, `vr scenemenus 0\|1`, `vr tapkey <KEY> [ms]` (a key sent the controllers' way), `vr mcal auto\|<counts/deg>`, `vr meleekey <vk>`, `vr buzz <hand> <amp> <s>` |
| Hands / body | `vr weapongrip 0\|1\|2\|forget\|forgetall`, `vr handrest <mode> [curl] [grip] \| forget`, `vr armlen <m>`, `vr armsolve 0\|1`, `vr pivotfix`, `vr carryarm`, `vr armforeign`, `vr handaxes 0\|1` (draws the palm and controller frames into the image) |
| Aim | `vr hand 0\|1\|2`, `vr aimoverride`, `vr poweraim <mask>`, `vr gunray <mask>` (default 63; 0 = old behaviour), `vr gunspread`, `vr guncull`, `vr reticle 0\|1\|2\|3`, `vr reticlerange <m>`, `vr pointer`, `vr pointercfg [k v]` |
| Melee | `vr meleeaim`, `vr meleeorigin 0\|1`, `vr swing <mask>` (1 slash, 2 power attack, 4 block; 0 = buttons only), `vr swingcfg [k v]`, `vr swordhit`, `vr swordcfg [k v]`, `vr notrail` |
| Throw / grab | `vr throw`, `vr throwcfg [k v]`, `vr grab`, `vr grabhand`, `vr grabrelease`, `vr grabreach`, `vr grabcfg [k v]`, `vr carrygaze` |
| Menus / HUD | `vr screen 0\|1`, `vr screencfg [k v]` (`size`, `dist`, `backdrop`, ...), `vr screenrecenter`, `vr wristhud 0\|1`, `vr wristcfg [k v]`, `vr hudshot <tag>`, `vr hudprobe [n]` |
| Frames / session | `vr keepalive 0\|1`, `vr xrwaitbound <ms>`, `vr xrkick <s> [all]` (new session for a stopped one; Meta's runtime only unless `all`), `vr d3dmt`, `vr stall <ms>` (a test hitch) |
| Testing aids | `vr giveall`, `vr autoall 0\|1`, `vr givemark`, `vr givepower <filter\|all>`, `vr giveupgrade <filter\|all\|list>`, `vr giveequip <filter\|all\|list>`, `vr giveitem <filter>[*n\|*max]`, `vr autopowers <list\|all\|off>`, `vr mana inf\|0\|<v>`, `vr hp inf\|0\|<v>`, `vr ammo inf\|0`, `vr infitems 0\|1`, `vr items [filter]`, `vr decls [filter]` (see the main [README](../README.md#testing-aids-unlock-everything-infinite-mana-ammo-and-elixirs)) |
| Instruments | `vr burst <n> [tag] [every]`, `vr bodyview <tag>`, `vr handjit [n]`, `vr handtrace <frames> [every] \| ref`, `vr pipetrace [n]`, `vr posesource <n>`, `vr swinglog [n]`, `vr raylog [n]`, `vr throwtrack <s> [ms]`, `vr gunburst <n> [stride]`, `vr gunparts [n]`, `vr gunpix <fx> <fy>`, `vr aamv`, `tdlog [n]` (player-state timeline), `stateprobe [n]` |

Most features keep their previous implementation behind a mode number (`vr gunray 0`, `vr armsolve 0`,
`vr weapongrip 0`, `vr stereotaa 2`...) for A/B comparison; the module's header names it.

### Research commands (brief)

Reverse-engineering helpers, mostly writing to the log or to `DVR_*.txt`:

- GPU: `gpudump <tag>`, `gpushader`, `gpucb`, `gpufind <m> [frames]`, `gpuskiphash <hash...>` (hide draws by vertex shader
  hash), `gputarget`, `gpuskip`, `gpudiag`, `gpu6dof 0|1|2` (2 = identity rebuild), `gpugrip`/`gputrim`/`gpuhandgrip`/
  `gpuhandtrim [l|r] <a> <b> <c>`, `gpuarminfo`, `gpuarmseg`, `vbdump`, `slashfreeze <ms>`.
- Memory: `peek`/`poke <addr|cam|player|gun|aimcp> [+off] ...`, `calli <rva> [args]` (calls game code on the command
  thread: pure lookups only), `findvt`, `scanvt`, `scanptr`, `scanvec`, `scanpose`, `findaim`, `findrv`, `cpnt`,
  `dumpmodel`, `nearmodels`, `modeljoints`, `stateobj`, `holdinfo`, `holdprobe`, `dumpcmdsys`.
- Watchpoints: `watch <aim|camorg|camaxis|aimorg|pitch|<hex>> [len] [hits] [all]` (write), `watchr` (read), `watch dump`
  (reader histogram), `watch off`. They run from a helper thread: never suspend or log on the target thread (the game's
  main thread is its render thread).

---

## 8. Debugging

### The log

`<game folder>\DishonoredVR.log` (falls back to `%LOCALAPPDATA%\DishonoredVR\` if the game folder is not writable).
Timestamped lines; capped at 64 MB per session. On start the previous log is renamed `DishonoredVR.prev.log`: after a
crash and relaunch (scripts relaunch by themselves), the fault is in the `.prev.log`. Read it first whenever the game
"just closes". Useful early lines: the version/commit line, `SysInfo` (Windows, GPUs, displays, keyboard layout, active
and installed OpenXR runtimes, implicit API layers, overlays loaded, window mode), `settings: ...`, `XR_RUNTIME_JSON
overridden by DVR_usemock.txt`, `first Present`, `auto: ...` (package activation, or why not yet).

### The heartbeat

`DVR_heartbeat.txt`, rewritten once a second (only with `WriteStatusFile = 1`). One line per subsystem; the ones used
most:

| Line | Read it for |
|---|---|
| `xr_init`, `xr_running`, `xr_state`, `xr_frames`, `xr_runtime`, `xr_error`, `xr_stereo` | the OpenXR session |
| `renderres ...`, `stereotaa ...`, `screen ...`, `watchdog ...` | render size, per-eye TAA, menu screen, stalls |
| `xr_stereo 1 mode 2 synchronised`, `stereosync ...`, `syncstereo ...`, `predict ...` | stereo mode; pairs held, gate state and why it is shut; pairs staged / submitted; pose prediction |
| `cam_pos`, `cam_fwd` | the game's camera (a load or a crash back to the logos shows here) |
| `kbm_ticks ...` | `sim` (is the game simulating), `mode play\|menu\|wheel`, `fpdraws`, `yawerr`/`pitcherr` (head-aim servo error), `menusafe`, `held` keys |
| `gpu6dof`, `gpu6dof_weapondraws entity L n R n`, `gpuhands`, `gpubody` | weapon and arm identification (`fulldraws`, `readopt`, `palmiss`) |
| `pstate` | running player states |
| `sword`, `swordhit`, `meleeaim`, `throw`, `grab`, `pointer`, `gunray`, `weapongrip`, `handrest` | each feature's counters |
| `aids` | cheats state, mana, items topped up |
| `hide ...` | hidden models, `stale` models, `playerchanges` (loads) |
| `hmd_pos`, `input ...` | head and controller poses from the runtime, lost frames, recenters |
| `cvar ...` | watched cvars |

A measurement only counts when the game is really in play: check `sim 1`, `mode play` and a rising `shakecalls` /
`fpdraws > 0` around it.

### Crashes: CrashLog and symbolize.py

`CrashLog` is installed in `DllMain` before any hook. The first faults are logged as `CRASH ...` lines: exception code,
address as `module+RVA`, the mod's current phase, the thread, registers, a backtrace and a stack scan. To turn the
`DishonoredVR.dll+0x...` frames into functions and source lines, you need the PDB of exactly that build:

```powershell
python tools\symbolize.py <DishonoredVR.log> [folder with DishonoredVR.dll + .pdb]   # default: build\bin
python tools\symbolize.py <tester's log> local\releases\<version>-<commit>
```

Only `DishonoredVR.dll` frames are resolved; `Dishonored2.exe+0x...` frames are game code (look them up with the RE
tools in `tools\`, which read the exe). The log's version line names the commit to check out.

Not every exit is a crash. The game has been seen to quit silently with no `CRASH` line and no Windows error event
(e.g. when the interaction sensor's results phase was called twice in one frame: "outdated query id"), and SteamVR
closes the game itself after its room-setup prompt. If `.prev.log` ends without a `CRASH`, suspect a deliberate exit.

### Stalls: the Watchdog

A thread of its own watches Present. After 2 s without one it logs
`Watchdog: <tag>: no Present for <s> s. Render thread <where>; OpenXR frame thread <...>; session <...>; foreground
<...>; game window <...>` (the OpenXR call the render thread is inside, its instruction pointer and recognisable return
addresses), again at 10, 30 and 90 s, then `STALL #n over: Present is back after <s> s`. Gaps over 1 s are logged as
hitches. `DVR_watchdog.txt` is rewritten twice a second while stuck. `vr stall <ms>` makes a test hitch;
`freeze\freeze.ps1` reproduces runtime stalls on the mock.

### OpenXR problems

Check `xr_*` in the heartbeat and the `XRSession` lines in the log (runtime name and version, the GPU the runtime asks
for vs the game's, accepted and rejected interaction profiles, the first `xrEndFrame` failures). Use `DVR_validate.txt`
for spec violations. A session going `FOCUSED -> ... -> STOPPING -> IDLE` is normal when the headset is taken off or the
game loses focus. SteamVR's null driver (`driver_null`, `forcedDriver null` in `steamvr.vrsettings`; back that file up
first) reaches FOCUSED but cannot be played on; it is good only for startup checks.

### Static RE tools (`tools\`)

`pe.py` (PE64 helper), `rtti.py` (MSVC RTTI -> vtable RVAs), `vt.py` (vtable explorer), `xref.py` (string xrefs +
disassembly, needs capstone), `disasm.py <rva> [len]`, `callers.py`, `scan_disp.py`, `typeinfo.py`, `tiname.py`,
`tirec.py` (id Tech typeinfo), `disparity.py`. They target `Dishonored2.exe` 1.77.9.0; the RVAs in the code and in
ENGINE.md are for that exe only (a cvar gist found online does not match it).

---

## 9. Packaging and releasing

1. Set the version in [`src/CMakeLists.txt`](../src/CMakeLists.txt) (`DVR_VERSION_MAJOR/MINOR/PATCH`). The build appends
   the commit from `git describe --always --dirty`; both appear in the log and in the DLLs' file properties.
2. If learned grips changed, regenerate the shipped seeds from your game folder's `DVR_grips.txt` / `DVR_hands.txt`:
   `python tools\make_seeds.py "<game folder>"` (writes `src\Src\SeedsData.inc`; the DLL writes them out only for players
   who have none).
3. Update `package\README.txt` and `package\DishonoredVR.ini`.
4. Commit, then run `scripts\package.ps1 [-SkipBuild] [-AllowDirty] [-RepoUrl <url>]` (`-RepoUrl` fills `{REPO_URL}` in
   the README; check its default before a public release).

`package.ps1`:

- builds (unless `-SkipBuild`) and **refuses a build whose commit is `-dirty`** unless `-AllowDirty` (test packages
  only: a tester's log must name a commit you can check out);
- checks with `dumpbin /dependents` that neither DLL imports the C runtime (`msvcp*`, `vcruntime*`, `ucrtbase`,
  `api-ms-win-crt*`, `concrt*`) or `openxr_loader`;
- stages `dist\DishonoredVR-<version>\` and zips it to `dist\DishonoredVR-<version>.zip`, containing `dinput8.dll`,
  `DishonoredVR.dll`, `DishonoredVR.ini`, `README.txt` (CRLF, `{VERSION}` and `{REPO_URL}` filled in) and
  `THIRD-PARTY-NOTICES.txt` (Detours, fmt, Dear ImGui, OpenXR loader, JsonCpp, from vcpkg's copyright files);
- copies the DLLs, their PDBs and the zip to `local\releases\<version>-<commit>\` (keep that folder: it is what
  `symbolize.py` needs for testers' crashes);
- prints the zip's contents and SHA-256 hashes.

`release\fresh-install.ps1` then simulates a tester's first run with the newest zip (it wipes the mod's files from the
game folder: see section 6).

---

## 10. Conventions and traps

### Repository

- **Line endings are mixed** and there is no `.gitattributes` (`core.autocrlf` false): many files are LF (including
  `Overlay.cpp`, `mockxr/mock_runtime.cpp`, most scripts), many are CRLF (`Commands.cpp`, `GpuTrace.cpp`, the CRLF
  docs...), a few are mixed (`ViewMatrixHook.h`, `Reticle.h`). Keep each file's endings: check with
  `git ls-files --eol <file>`, and compare `git diff --stat` with `git diff -w --stat` before committing. Tools that
  normalise a whole file (Git Bash `sed -i` turns CRLF into LF; some editors normalise mixed files) bury the real diff.
- Bash heredocs collapse `\\` into `\` (and can turn `\b` into a backspace): write C++, PowerShell and Python files with
  an editor, not `cat <<EOF`, and check for control characters after scripted edits.
- `Commands.cpp`'s main `else if` chain is at MSVC's nesting limit (`C1061`). Put new commands in a module's
  `Command(what, ss)` function or a helper like `FrameCommand`, called before the chain.
- The main branch is `master`. Commit builds you hand to testers, so their logs name a real commit.
- MSVC messages may come out localised (e.g. Spanish on a Spanish Windows: `C2001: nueva línea en constante` = newline
  in constant, usually a mangled backslash). Error codes are the same in every language.
- A git worktree in a deep temp folder may not build (path too long for CMake); use a short path such as
  `<repo>\local\old`.

### Engine and injection

- `dinput8.dll` loads before the exe's static initialisers run: never read game globals or cvars from `DllMain` or the
  init thread. `CVars::Init` and everything that needs the game happen on the first Present.
- Every hook is an offset into the supported exe. Keep `GameInfo`'s build check; never hook on an unknown build.
- Mid level change the world global is NULL: get entities with `BodyHide::EntityById`, never the raw `GetEntityById`.
- Pointers cached across a level load point at freed memory: revalidate before writing (see the `BodyHide` and
  `HandAimHook::RealCam` comments, and ENGINE.md's traps).
- The engine's console command system queues: `exec` runs later, on the main thread.
- The game must be **windowed** (not fullscreen, not borderless) for `RenderRes` to render at the headset's density;
  in borderless the engine renders at the monitor's size.
- The game ignores input while its window is not in front; the mod only clicks when the cursor is over the game's
  client area (a click on the frame starts Windows' modal move/resize loop on the game's thread).

### Player data

- **Never run the engine console's `writeConfig` on the player's config**: it writes a partial `dishonored2Config.cfg`.
  Back up `<saves>\dishonored2Config.cfg` before anything that may write it (option changes, `RenderRes` work, a game left
  running after a test that changed a preset rewrites it on exit).
- Back up `<saves>\savegame` before test sessions; scripted menu sequences can land one menu off and press the wrong
  entry.
- `DVR_autoexec.txt` and the cheat settings apply to real playthroughs too, and autosaves keep what they gave.
- After mock tests, restore `DVR_usemock.txt` to the real runtime (section 4).

### Measuring

- Prefer what can be verified from the log, the heartbeat, a capture or the mock compositor; a headset test is the last
  step, not the instrument. When the unknown is a rotation or a distance, log the number instead of judging a screenshot.
- Always run a control (the same state measured twice) before a before/after comparison.
- The mock's `headSway`/`handJit`/swing generator exist because still, file-driven poses hide timing and tremor bugs.

### Known machine-specific paths

These must be parameterised before publishing (as of this writing):

- Every script under `scripts\testing\<area>\` sets `$repo`, `$root` or `$P` to the original developer's absolute
  repository path, and most set `$game` to the default Steam folder.
- `scripts\deploy.ps1`, `undeploy.ps1`, `run-test.ps1` default `-GameDir` to the default Steam folder;
  `scripts\testing\Play.ps1` hard-codes it.
- `scripts\build.ps1`, `build-mockxr.ps1` and `package.ps1` look for CMake / `dumpbin` under the VS 2022 **Build Tools**
  install path.
- `Play.ps1` and `release\quickboot.ps1` assume Meta's runtime manifest at its default install path.
- `tools\make_seeds.py` and `tools\xref.py` default to the Steam game path.
