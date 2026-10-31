# Dishonored 2 engine reference (Void Engine, build 1.77.9.0)

What the VR mod in this repository learned about the game's engine by reverse engineering, organised by topic. It is
meant as a reference for anyone modding or reverse-engineering Dishonored 2. Everything here was measured on one build
(below); on any other build every address is wrong.

Sources: the measurements of the mod's development, consolidated here. The addresses the mod actually uses are `constexpr` constants in
[src/Src](../src/Src); where this document and a source comment disagree, the section "Superseded findings" at the end
says which one is right. The RE helpers used to find most of this are in [tools/](../tools) (`xref.py` string ->
code xrefs, `typeinfo.py` the id Tech runtime typeinfo, `rtti.py` MSVC RTTI -> vtables, `callers.py` call sites of a
function, `tirec.py` / `tiname.py` component typeinfo records, `scan_disp.py` displacement scans over `.pdata`).

Contents

1. [Target build and notation](#1-target-build-and-notation)
2. [Process, threads, input](#2-process-threads-input)
3. [Console commands and cvars](#3-console-commands-and-cvars)
4. [Rendering](#4-rendering)
5. [Camera, aim and the player](#5-camera-aim-and-the-player)
6. [Entities, components, models, joints](#6-entities-components-models-joints)
7. [Weapons and combat](#7-weapons-and-combat)
8. [Interaction, held objects, corpses](#8-interaction-held-objects-corpses)
9. [Interface, menus, videos, HUD](#9-interface-menus-videos-hud)
10. [Inventory, powers, upgrades, the mark](#10-inventory-powers-upgrades-the-mark)
11. [Consolidated address table](#11-consolidated-address-table)
12. [Known traps and dead ends](#12-known-traps-and-dead-ends)
13. [Superseded findings](#13-superseded-findings)

---

## 1. Target build and notation

| | |
|---|---|
| Game | Dishonored 2, Steam |
| Exe | `Dishonored2.exe`, file version **1.77.9.0**, **41,134,632 bytes**, PE link time stamp **`0x67A50CDE`** (2025-02-06), SHA256 `B671F8249422862BF557D00AAB2F4BB671ED4515CC10AD8A8A9DECAD84516180` |
| Engine | Void Engine = a heavily modified id Tech 5 (Rage lineage). D3D11 only |
| DRM / anti-cheat | Denuvo removed in December 2018 (Update 4); no anti-cheat. The exe is clean for IDA / Ghidra |
| Units | metres (walking ~2.9 m/s, gravity on a thrown grenade 9.4-9.7 m/s^2) |
| Axes | id Tech: an `idMat3` axis is stored as three ROWS = forward, left, up; Z is up |

**The version string does not identify the build.** Steam rebuilt the exe on 2025-02-06 without changing "1.77.9.0",
and the GOG build also calls itself 1.77.9 with a different exe (42,591,232 bytes). Check version + file size + link
time stamp together, as [GameInfo.h](../src/Src/GameInfo.h) does; on any mismatch the mod touches nothing.

**Notation.**
- `exe+0xRVA` = an address relative to the image base of `Dishonored2.exe` (code, globals, vtables, typeinfo records).
  The base is ASLR-randomised; the mod adds `GameInfo::Base()`.
- `+0x...` after an object name = a field offset inside that object (`cam+0xFC`, `entity+0x300`).
- `slot N` = the N-th entry of a vtable (byte offset `8*N`); `vtable +0x80` = a byte offset.
- "Return address exe+X" = the instruction after a `call`, used to recognise one call site of a shared Win32 or
  engine function from inside a hook (`_ReturnAddress()`).
- A *typeinfo* is the record `GetComponent` takes to name a component class (found with `tools/tirec.py`).
- Shader identifiers are the DXBC checksum (bytes 4..11 of the bytecode): shader object pointers change every run and
  every level load, the checksum does not.

---

## 2. Process, threads, input

### Load order
- `dinput8.dll` is a **static import** of the exe. A proxy `dinput8.dll` in the game folder (what the community
  "Dev Console Enabler" does too) runs its `DllMain` **before the exe's CRT static initialisers**: no game global (cvars
  included) exists yet. Read game state from the first `IDXGISwapChain::Present`, or later.
- `bink2w64.dll` (the video player) is an import too and is loaded before a proxy's init thread runs.

### Threads
- The **main thread is the render thread**: it pumps the window's messages, runs the WndProc and calls `Present`.
  Anything that blocks it (a long `xrWaitFrame`, a modal loop) freezes the window and queues input, which then plays
  out when it returns.
- Game logic (player, sensors, powers, physics queries) runs on **job threads**. Hardware watchpoints are per thread:
  a watchpoint armed on the main thread only never sees those readers (absence of hits proves nothing).
- The renderer records on **many deferred D3D11 contexts on many threads at once**; command lists of a view are
  finished and executed after the previous `Present`. See [Rendering](#41-d3d11-the-pipeline-and-deferred-contexts).
- A value read on the render thread can be caught mid-update by the game thread (the player's position in a moving
  cutscene alternates by ~0.1 m frame to frame): average, or read it on the game thread.
- The player camera's per-frame `UpdateAim` (exe+0x7F4B30) runs on the game thread once a frame; the mod hangs its
  game-thread work there (BodyHide, TestAids, GunRay ticks).

### Input
- Imports: `XINPUT9_1_0.dll!XInputGetState/XInputSetState` (gamepad), `DINPUT8.dll!DirectInput8Create` (keyboard),
  raw input for the mouse.
- **The keyboard is read through DirectInput = physical key position (DIK = set-1 scan codes).** The game's key names
  ("W", "LCTRL", "UPARROW") are DIK codes; the name table sits in the exe next to the string "UPARROW". Synthesised
  keys must be sent by scan code, never through the Windows layout (on AZERTY the layout's 'W' is the game's "Z").
  Table and parser: [KeyBinds.cpp](../src/Src/KeyBinds.cpp).
- **Modifiers need a scan code**: `SendInput` with `wVk = VK_LCONTROL, wScan = 0` reaches the window but the game
  ignores it (Ctrl/Shift/Alt = block, sprint, lean were dead). By scan code (0x1D) it works.
- **Key presses shorter than ~80 ms are lost about half the time** (a quick controller tap is 40-50 ms).
- **Synthesised mouse BUTTONS go to the window under the system cursor.** The game runs windowed and does not hold the
  cursor; a click over another window activates it, the game loses focus and pauses. A system prompt
  (`Shell_SystemDim`) covers the whole screen. A click on the game window's frame (x = 0 is its resize border when the
  window sits at (0,0)) most likely enters Windows' modal move/size loop on the game's own thread, which then waits for
  a button-up that thread was going to send: put the cursor in the CLIENT area (`GetWindowInfo`) before any button.
  Keys have no such problem (they go to the focused window).
- Mouse: ~**24.2 counts per degree** of yaw at `g_mouseSensitivity 0.808` (scales as `0.808 / g_mouseSensitivity`);
  `g_invertMouseYAxis` inverts pitch counts. The engine clamps very large per-frame deltas (~45 deg), so spread big
  turns at <= 300 counts a frame.
- **Bindings** live in `%USERPROFILE%\Saved Games\Arkane Studios\Dishonored2\base\dishonored2Config.cfg`, lines
  `arkBind "<key>" "<ACTION>"` / `arkBindSecondary ...` per binding set (the in-game set is `IN_GAME`). Default
  actions used: `MELEE` = MOUSE1, `INTERACT` = F (hold), `WEAP_1..0` quick slots, block = LCTRL.
- **Gamepad emulation is a dead end so far**: a faked XInput reaches `arkXInputControllerState::Poll` (exe+0xE0220;
  connection probe exe+0xE01D0, deadzone at exe+0x1AE0850, button map exe+0x22877B0) and becomes per-frame axis
  events (PostAxisEvent exe+0xE03D0, gated by a change flag at dev+0x2C; GenerateEvents exe+0xE0A80; InputSystem
  exe+0x3134140, pad = device 0, queue at +0x590), but the player ignores them (unknown consumer gate). Pads are only
  polled while "connected", so a hook must answer `ERROR_SUCCESS` from the first probe. Kept, disabled, in
  [XInputHook.cpp](../src/Src/XInputHook.cpp) / [PadProbe.cpp](../src/Src/PadProbe.cpp).

### Window focus and pausing
- The game **pauses itself when its window loses the desktop focus** (pause menu, `sim 0`). An unfocused game can also
  stop rendering entirely (0 draws) while still presenting. `win_pauseOnFocusLoss 0` does **not** reliably stop the
  pause (see [cvars](#33-notable-cvars-and-traps)). Measurements need the game focused and must check it is simulating.
- An ESC sent to a running game opens the pause menu: read "is it paused" twice before sending one.

---

## 3. Console commands and cvars

### 3.1 Command system
- `cmdSystem = *(exe+0x228BAE0)`. Vtable slots (measured): **2 `AddCommand(name, fn, flags, desc)`**, **8
  `ExecuteCommandText(text)`** (immediate, calling thread), **9 `BufferCommandText(text)`** (queued, main thread),
  10 `ExecuteCommandBuffer`. See [Game.h](../src/Src/Game.h).
- Known handlers: `Cmd_GetViewpos_f` exe+0x8861C0, `Cmd_SetViewpos_f` exe+0x886D10, `teleport` exe+0x886F50,
  `Cmd_LoadGame_f` exe+0x658D00 ("Quick load, or load the specified slot number"; with no quicksave it does nothing),
  `RestartFromCheckpoint` exe+0x65DA50, `writeConfig` exe+0x42920. Strings exist for `map`, `devmap`, `god`, `noclip`,
  `give`, `listCmds`, `listCvars`.
- `setviewpos` can drop the player out of the level (Game Over).
- `g_stopTime 1` freezes the world (particles and trails stay exactly as they were); `g_runFrames N` steps N frames.
  The mod's own "is it simulating" test reads 0 while frozen.

### 3.2 idCVar layout and access
- Prefixes: `r_` renderer, `g_` game, `pm_` player movement, `com_` common, `m_`/`in_` input, `rs_` dynamic resolution,
  `win_` window, `ark_` Arkane, `stereoRender_` id Tech 5 stereo leftovers. Dump: [cvars-1.77.9.0.txt](cvars-1.77.9.0.txt)
  (community gist, ~1600 cvars). **Its RVAs do NOT match this exe** (e.g. `com_allowConsole` dump +0x288F440 vs
  measured +0x25B2CF8): trust only its names and defaults.
- Every cvar is a **0x80-byte static object in `.data`**; the static object IS the live one (no separate internal cvar
  as in Doom 3). Measured layout ([CVars.cpp](../src/Src/CVars.cpp)):

  | off | field |
  |---|---|
  | +0x00 | `idStr value` (0x28: `{char* data; int len; int alloced; char base[24]}`; data -> +0x10 for short values) |
  | +0x28 | `int integerValue` |
  | +0x2C | `float floatValue` |
  | +0x30 | `const char* name` |
  | +0x38 | `const char* defaultValue` |
  | +0x40 | `const char* description` |
  | +0x48 | `int flags` (`0x40009` bool, `0x4000A` int, `0x4000C` float, + archive etc. in higher bits) |
  | +0x4C / +0x50 | `float valueMin` / `valueMax` (1.0 / -1.0 = no range) |
  | +0x58 | `valueStrings` |
  | +0x68 | `argCompletion` (e.g. exe+0x100B90 = boolean completion) or 0 |
  | +0x78 | `idCVar* next` (static registration list) |

- **Finding a cvar**: locate its NUL-terminated name in the image, then the 8-aligned qword pointing at it; that qword
  is `T+0x30`. ~33 ms for 14 names.
- **Registration**: each cvar's static constructor calls exe+0x1008F0 (or exe+0x100840 with a range) with the name
  in `rdx`, the default in `r8`, the type flags in `r9d` (1 bool, 2 int, 4 float; **0x10000 = saved in the config**:
  `r_swapInterval`, `r_skipDOF`, `r_skipLensFlares`, `r_skipCameraMotionBlur`, the quality presets) and the
  description on the stack. A cvar whose readers use `cmp [rip+obj+0x28]` reads its live value every frame; one with
  no such reader is read through another path (use the console for it).
- **Writing**: writing `+0x28/+0x2C` directly bypasses `OnChange` and the modified flag and leaves the string stale.
  Fine for plain tunables read every frame (`rs_enable`, `g_stopTime`, `ark_ui_show_master_hud`); useless for cvars
  whose effect lives in a callback. The console (`exec <cvar> <value>`, `toggle <cvar>`) goes through the engine.
- The cvar system object used by the renderer is `exe+0x228BAE8` (vtable +0x48 sets a cvar).

### 3.3 Notable cvars and traps

| cvar | notes |
|---|---|
| `com_allowConsole` | the dev console (what the Console Enabler flips) |
| `g_showHud`, `ark_ui_show_master_hud` (0 never, 1 contextual, 2 always), `ark_hud_enable_keyhole` | HUD. "Contextual" hides the health/mana ring after a few seconds |
| `pm_noBob`, `ark_playerBobAmount`, `g_drawBob` | head bob |
| `g_stopTime`, `g_runFrames` | freeze / step the world |
| `win_pauseOnFocusLoss` | **trap**: setting it to 0 does not keep the game from pausing on focus loss (the pause still happens) |
| `r_windowWidth`, `r_windowHeight` | set by the WndProc from `GetClientRect`; `exec r_windowWidth` does not stick |
| `r_fullscreen` | 0 windowed, 2 borderless. In borderless the engine renders at the MONITOR's size whatever `GetClientRect` says |
| `r_mode`, `ark_fieldOfView` | display mode, the options menu's fov |
| `rs_enable` (0 off, 1 horz, 2 vert, 3 both; default 3), `rs_VSyncBased 1`, `rs_minimumResolutionScale 0.85`, `rs_useHwUpsample 0` | dynamic resolution, aimed at the monitor's refresh rate; writing the int slot of `rs_enable` turns it off live |
| `r_AntiAliasingPreset` | 0 off, 1-2 FXAA, 3 TAA (default; the overall quality preset sets it, Low = FXAA) |
| `r_skipTemporalAA` | 1 skips the TAA pass. Changing the AA option in the menu rewrites it |
| `r_skipCameraMotionBlur` | 1 = no camera motion blur |
| `r_skipSurfaceCPUCulling` | does NOT stop a held weapon from being culled |
| `g_mouseSensitivity`, `g_invertMouseYAxis` | mouse |
| `ark_god_player`, `ark_god_npc` | writing the int does nothing; the `god` command toggles it; even on it did not keep the player alive in a sword fight. Both reset on a load |
| `r_prt_trailrenderer_budget`, `r_prt_particletrailrenderer_budget` | 0 does NOT remove the sword trail |
| `stereoRender_separation` / `_screenSeparation` / `_guiOffset` / `_swapEyes` | id Tech 5 stereo leftovers (1.5 / 0.25 / 0 / 0); no working stereo path found |

No `g_fov`/`r_fov`: the render fov is `renderView_t::fov_x/fov_y` (radians), see the projection builder.

---

## 4. Rendering

### 4.1 D3D11, the pipeline and deferred contexts
- Backbuffer: 2 buffers, a **TYPELESS** resource (format 27) viewed as `R8G8B8A8_UNORM` (28) and `_UNORM_SRGB` (29);
  the interface alternates both views draw by draw (see [Interface](#9-interface-menus-videos-hud)).
- **Gameplay renders on deferred contexts.** A deferred `ID3D11DeviceContext` has its own vtable in `d3d11.dll`;
  hooking the immediate context sees ~0 draws a frame in play (millions during loads). Create one deferred context on
  the game's device to get the second vtable (all deferred contexts share it): ~2300 draws a frame. Keep that context
  alive: with SteamVR another layer wraps it and, once released, its vtable is freed memory.
- Slots used: device `CreateInputLayout` 11, `CreateVertexShader` 12, `CreatePixelShader` 15, `CreateComputeShader` 18;
  context `Map` 14, `Unmap` 15, `UpdateSubresource` 48, deferred `FinishCommandList` 114, immediate
  `ExecuteCommandList` 58; swapchain `Present` 8, `ResizeBuffers` 13. ([GpuTrace.cpp](../src/Src/GpuTrace.cpp),
  [D3D11Hook.cpp](../src/Src/D3D11Hook.cpp))
- **The pipeline is deeper than one Present**: view N+1 is built before the Present that shows image N, and its
  command lists are finished/executed on worker threads after the previous Present. Which view an image belongs to
  must be read from the command lists (each inherits the serial of the view its draws used; the last executed before
  the Present names the image). Guessing by phase was right in one session and swapped the stereo eyes in another.
- The game's device is **not** multithread-protected; `ID3D11Multithread::SetMultithreadProtected(TRUE)` is needed
  before touching it from another thread (measured no slower).
- The game's own Present gaps while loading: 0.2-1.4 s, several per load.
- **Never `Map` a resource from inside a draw hook**, and never create one there: many contexts on many threads record
  at once; one DYNAMIC buffer mapped `WRITE_DISCARD` from several threads, or discarded while pending command lists
  reference it, crashed the driver. Allocate at Present, write with `UpdateSubresource`, restore every binding.

### 4.2 The view object, renderView_t and the main view
`renderView_t` (0x39C0 bytes, from the id Tech typeinfo):

| off | field |
|---|---|
| +0x44 | `viewID` (1 in gameplay, 3 in menus, for the main view) |
| +0x4C | `settings` (0x48) |
| +0x96 | `inhibitModelFovScale` |
| +0x9C / +0xA0 | `fov_x_RADIANS` / `fov_y_RADIANS` |
| +0xA4 | `cramZNear` |
| +0xA8 | `vieworg` (idVec3) |
| +0xB4 | `previousvieworg` |
| +0xC0 | `m_portalViewOrg` |
| +0xCC | `viewaxis` (idMat3) |
| +0xF0 | `customClipPlane` |
| +0x100 | `explicitProjectionMatrix` (4x4) |
| +0x140 | `useExplicitProjectionMatrix` (bool) |
| +0x15C | `forceIdentityViewMatrix` |
| +0x160 | `parmBlock` (0x260) |
| +0xBA0 | `mouseCursorPosition` |

The renderer's view object (constructors exe+0x373950 / exe+0x88B9F0; `renderView_t` ctor exe+0x43800):

| off | field |
|---|---|
| +0x0000 | `renderView_t m_gameRV` |
| +0x39C0 | `idRenderWorld* m_parentWorld` |
| +0x39D0 | `renderView_t m_renderRV` (vieworg +0x3A78, viewaxis +0x3A9C, fov_x +0x3A6C, fov_y +0x3A70, explicit proj +0x3AD0, useExplicit +0x3B10) |
| +0x3A84 | previous vieworg (12 bytes) |
| +0x7390 | `m_projectionMatrix` (jitter: p02 at +0x7398, p12 at +0x73A8) |
| +0x73D0 | inverse projection |
| +0x7410 | previous projection |
| +0x7450 | `m_viewMatrix` |
| +0x7490 | inverse view |
| +0x74D0 | previous view |
| +0x7510 | `m_viewProjectionMatrix` |
| +0x7550 | inverse VP |
| +0x7590 | previous VP |
| +0x77B0 | view determinant sign |
| +0x77B4 | engine frame number the "previous" copies were taken at |
| +0x7848 / +0x784C | `m_renderWidth` / `m_renderHeight` |
| +0x7850 / +0x7858 | current / previous TAA jitter |
| +0x7860 | `m_viewport` |
| +0x78C0 | `m_resolutionScale` |
| +0xBCFC | byte: the late path that makes previous = current (a camera cut) |

**View setup, per engine frame** (exe+0x375447..0x375700, chunk exe+0x375B7F): when the frame number (exe+0x35EB00)
differs from view+0x77B4, previous <- current for +0x3A84, +0x7410, +0x74D0, +0x7590; then the **projection builder
exe+0x3742B0** (called from exe+0x375AE4: copies the explicit matrix when `useExplicitProjectionMatrix`, else builds
from `fov_x/fov_y`; adds the jitter, see TAA), its inverse, then `forceIdentityViewMatrix ? identity :
**idRenderMatrix::CreateViewMatrix**(&rv->vieworg, &rv->viewaxis, &view->m_viewMatrix)` = **exe+0x109CC0**, inverse
(exe+0x10ACA0), proj x view (exe+0x10AD80) -> VP, inverse VP (exe+0x10A920). A second build within one frame skips
the copy.

- **CreateViewMatrix (exe+0x109CC0) is the hook that moves the scene.** `(const idVec3* org, const idMat3* axis,
  idRenderMatrix* out)`, out rows = (-left, up, -fwd) with dot-product translations. The main view is the call whose
  `org == mainView + 0x39D0 + 0xA8`; pass an edited copy of org/axis. Called once a frame for the main view. Yaw, pitch
  and position all move the scene; nothing accumulates because the renderView is untouched.
  ([ViewMatrixHook.cpp](../src/Src/ViewMatrixHook.cpp))
- **The projection builder (exe+0x3742B0) is where to change the fov**: write `fov_x/fov_y` into the main view's
  renderView there; everything downstream (inverse, VP, jitter, culling) stays consistent. The game's fov is **Hor+**
  (vertical kept, horizontal follows the aspect): 95.4 x 58.7 deg at 1902x973. There is **no separate viewmodel fov**:
  the player's arms and weapons use exactly the world projection. ([CameraHook.cpp](../src/Src/CameraHook.cpp))
- **exe+0x3586A0** ("SetViewConstants", `rcx` = constant block, `rdx` = view object), ~9 calls a frame on job threads,
  only UPLOADS already computed matrices and the view parms (it sends the previous matrices as
  `cb_previous*matrix`). Rewriting the renderView there moves nothing. Useful to identify the main view object
  (`m_renderWidth/Height >= 1024x576`, `viewID`) and to read the real fov.
- `exe+0x881640` builds render views for OTHER entities (NPC / security-camera eyes, via the wrapper exe+0x881D30),
  never for the local player. `exe+0x837A40` and `exe+0x67F970` are unrelated entity view / listener code.
- A spectator / cutscene camera path exists: if `*(gameLocal+0x1F52C0) & 0xFFFFFF` -> camera object via exe+0x7E7110 +
  exe+0x88E140(cam, &org, &angles). Not used.

### 4.3 Per-instance constant buffers (cb2) and the MVP factorisation
- Most mesh vertex shaders take a **`PerInstanceCB` at register b2** holding that draw's own model-view-projection,
  with vertices in MODEL space: `cb_mvpmatrix{x,y,z,w}` (each a COLUMN the shader `dp4`s the position against; z is
  multiplied by `cb_modeldepthhack`), optionally `cb_previousmvpmatrix{x,y,z,w}` (motion vectors) and
  `cb_modelmatrix{x,y,z}`.
- `cb_modeldepthhack` is what keeps the first-person arms in front of the world. A world draw moved into the hand (a
  carried item) must be given the arms' value, or the hand draws over the parts of it that are nearer the eye. Take
  the value from an arm draw with the same projection, so shadow and reflection passes keep their own.
- **There is no fixed layout**: every material compiles its own (~30 different cb2 sizes in one frame, up to at least
  1168 bytes). Read the offsets from the shader's reflection (`D3DReflect` on the bytecode kept from
  `CreateVertexShader`, cached per shader object) by those names. Use `__uuidof(ID3D11ShaderReflection)`, not
  `IID_ID3D11ShaderReflection`, when `d3dcompiler_47.dll` is loaded by hand.
- cb2 is written just before each draw (Map/Unmap or `UpdateSubresource` on the deferred context) and a deferred
  context cannot `Map` for reading: shadow those calls `thread_local`, keyed by buffer, reading the mapped memory
  before the real `Unmap`. A level load recycles shader object addresses: drop a cached layout when
  `CreateVertexShader` reuses an address.
- **Factoring the MVP without knowing the projection.** A perspective projection's last row is `(0,0,1,0)`, so the MVP's
  w row IS the third row of model->view. Per draw:
  ```
  s   = |w.xyz|            R2 = w.xyz/s      tz = w.w                       (uniform scale, view z row)
  p02 = dot(x.xyz/s, R2)   p00 = |x.xyz/s - p02*R2|   R0 = normalised        (same with y -> R1, p12, p11)
  tx  = (x.w - p02*tz)/p00                                                   (same with y)
  p2k = dot(z.xyz/s, Rk)                                                     (depth row, k = 0..2)
  ```
  Assumes only no x/y skew and no x/y clip offset; TAA jitter lives in p02/p12 and is carried through. Rebuilding with
  the same projection and the same pose reproduces the input bit for bit (the test to run before moving a pixel). With
  the view matrix this gives every draw's model->world, which matches the render model's world transform
  (`model+0x1860/+0x186C`) to 0.1 mm.
- Replacing a draw's pose = rewriting its columns; a constant buffer LARGER than the shader declares is legal, so one
  pool of one size (2048 bytes) serves every layout.
- **Other views (reflections, cube maps) go through the same path**: a projection taken from "any draw" is sometimes
  a mirrored one. Take it only from draws known to belong to the main view (the player's own models).
- The 800-byte buffer at slot 1 of these shaders is per-view data (view matrices at float 24 and 76, camera world
  matrix at float 136) that the mesh shaders do not read; zeroing it changes nothing. Bound is not used.

### 4.4 Skinned meshes (the first-person arms)
- Arm draws are skinned: `ro_matrices` = `Buffer<float4>`, 3 rows `(m00 m01 m02 tx)` per bone (72 bones on the arm
  palettes), index = SKININDICES + `cb_mtxoffset`, then the ordinary `cb_mvpmatrix*`. Palettes are written every
  frame via Map/Unmap.
- **Each arm is several meshes** (glove/skin, sleeves, rings), each with its OWN palette buffer, drawn in several
  passes recorded on worker threads; the palette sits in a different t-slot per pass. Tell left from right by the
  reflected `ro_matrices` slot of that draw, and pose each draw from its own palette and model matrix (meshes drawn
  before the "main" one in a frame would otherwise get last frame's pose).
- Vertex streams: position `R32G32B32_FLOAT` (stream 0, stride 12); stream 4 (stride 16) = weights 2x `R8G8B8A8_UNORM`
  at 0/4, indices 2x `R8G8B8A8_UINT` at 8/12; index buffer `R16_UINT`.
- Bones (left skin, 2595 vertices, Emily; Corvo / right arm the same indices, the right arm mirrored in X): 4 palm,
  5-7 thumb, 15-18 index, 26-29 middle, 37-40 ring, 48-51 pinky, 60-63 wrist helpers, 64-67 forearm twist, 1 shoulder
  cap, 2 upper arm, 3 and 68 elbow helpers, 0 no vertices. Bind pose: arm straight along +X, fingertips at x = 0.92,
  wrist ~0.22 m from the fingertip, elbow ~0.48, shoulder ~0.74-0.80. Pick the palm by the number of vertices a bone
  dominates, not by total weight. The skin and the big sleeve bind DIFFERENT palette buffers of the same skeleton.
- **A palette matrix is `M = W * B^-1`**: writing `M' = D * M` with a rigid delta `D` in model space moves the bone to
  `D * W` with no bind pose needed. Blending two palettes must be done about each bone's bind centroid: a palette
  matrix rotates about the model origin, half a metre from the fingers.
- Joints can be learned from the animation alone: the joint J between bones p and c satisfies `M_p J = M_c J` every
  frame (least squares over frames).
- The set of bones the hand uses is not fixed (52 <-> 54 bones as the wrist helpers move in and out).
- **Shadow Walk** swaps the arm for claws (2885 vertices, 69 bones, palm bone 2) on the same palettes: an analysis that
  keeps "the mesh with the most bones" takes the claws for the arm.
- **A level load builds new palettes while recycling vertex buffers**: anything keyed by palette pointer goes stale.
- Joint NAMES are in memory (`model+0x1FE8` -> resource, tables at +0xA0 and +0xEA0, stride 32, name at +0x10) but
  those are helper lists, not the skeleton in palette order. Skeleton bind poses were not found (`default.md6skl` is
  referenced from the same resource). The engine's joint index is NOT the palette index.

### 4.5 Temporal AA
- One compute shader, 16x16 threads (DXBC `e8b9140e89d71633`, variant `4e10dcb29d12f644`), found by diffing passes with
  `r_skipTemporalAA` 0/1. Bindings: `ro_taahistory_read` t0, `ro_motionvectors` t1 (R16G16_FLOAT, UV units),
  `ro_viewcolormap` t2 (a copy of the scene colour), t3 auto-exposure, `rw_taahistory_write` u0, `rw_taaresult` u1,
  PerViewCB b1, PerInstanceCB b2 (`cb_taaamount`, dither, tonemap). Find it by those resource names.
- History = ping-pong of two `R11G11B10` textures. Reprojection uses ONLY the motion vectors (the longest in the 3x3),
  history sampled Catmull-Rom, 3x3 variance clamp.
- Motion vectors (PS `48f2037fd43d86c8`): `(cur/w - prev/w - cb_jitterrelativetopreviousframe) * (0.5,-0.5)`. Static
  world: previous from PerViewCB `cb_previousviewprojectionmatrix` (= view+0x7590). Entities: `cb_previousmvpmatrix*`
  in cb2, **cached from the last frame** (a few objects build theirs from the view).
- Jitter: an 8-entry table at exe+0x1B35F10 indexed by `frame & 7`, scaled by 2/size; p02 = 2*jx, p12 = 2*jy.
- With stereo the previous frame is the other eye's: TAA ghosts. Per-eye history + per-eye previous matrices fix it
  ([StereoTaa.cpp](../src/Src/StereoTaa.cpp)). FXAA does not.
- An entity's cached previous MVP is the same draw's MVP in the last RENDERED frame (held frames included); the
  skinning palettes (`ro_matrices`, `ro_previousframeskinningmatrices`) are renewed only on simulated frames. The
  frame number view+0x77B4 advances on held frames too.
- **Holding a frame** (synchronised stereo, [StereoSync.cpp](../src/Src/StereoSync.cpp)): `idGameLocal::RunFrame`
  (exe+0x8ACDF0, vtable idGameLocal+0x18) replaced by the game's Draw alone (exe+0x8AC710, with this+0x340f08 pointed
  at the frame's output struct) draws the same world again and loses no game time (the next tick gets both frames'
  delta); `g_stopTime 1/2` for that frame loses time (0.55x). The engine reads DirectInput once a frame
  (arkDirectInputKeyboard exe+0xE1970, idMouseWin32 exe+0xE2930) into a list the next read clears and only a simulated
  frame consumes: the reads are skipped before a held frame. Holding while a pause menu opens crashed in Iggy
  (exe+0x100B891, freed memory): only hold in play.

- **The frame cap is a spin, set by `com_timeScaler`** (object exe+0x25B1E00, default 2, range 0.1-10): at the end of
  the frame (exe+0x359223) the renderer calls exe+0x462E0, which busy-waits (exe+0x1585A0) until
  `(1/30 s at exe+0x22813C8) * 1e6 / com_timeScaler` microseconds (timer exe+0x157F40, QPC in us) have passed since the
  previous frame: **30 fps x com_timeScaler** (120 = 4). Skipped when `com_disableFrameSpinning` (exe+0x25B1E80) is set,
  when `*(exe+0x3937288)` is non-zero, or when the renderer's vfunc +0x118 says no. Synchronised stereo switches the
  spin off while it runs (StereoSync, `com_disableFrameSpinning`): under the cap it could not pass 60 pairs a second.
- **V-Sync is the Present's sync interval**: exe+0x36B3A0 presents with `r_swapInterval` (int at exe+0x3937288) when it
  is above 0 (exe+0x36B3FA), and the frame-cap spin is skipped whenever it is non-zero (exe+0x35922C): with V-Sync on
  the cap is the monitor's vblank. The mod presents with interval 0 while synchronised stereo runs (D3D11Hook).
- **Physics runs in fixed 1/120 s substeps** (the game clocks, exe+0x59990): clock 2 (gameLocal+0x2E7428+0xA0) advances
  `n * (1/120) / scale` per game frame, `n = round(lag * 120 * scales)` against the real-time clock (catching up),
  at most 12, with `g_physicsStepTolerance` (0.2) keeping n from flipping and `g_limitPhysicStepVariation` (0) limiting
  it to one step of change per frame. Above 120 game frames a second some frames get no physics step (the community
  FPS patches' "physics issues"); at 90 ticks/s the steps go 1, 1, 2. Clock 0's step is the real time, clamped to 0.5 s.

### 4.5b Render passes (the frame's task list)
The renderer runs each view as a list of named passes (Forward+: depth prepass, tiled light culling, forward
lighting), registered in exe+0x34FF80..0x351160 through exe+0x2C2960(pass, name, task count, 2, ., function). A pass
is an array of 0x120-byte tasks: function at +0xD8, flags at +0xF4, the task's view object at +0x70 (its
`m_viewMatrix` at +0x7450), its D3D context wrapper at +0x108. The executor (exe+0x2C5D50, and a twin at
exe+0x2C5F90) skips a task whose flags have **0x100** (set by the frame's setup, e.g. exe+0x3536CA, exe+0x354C05): it
marks the context empty (`[+0x108]+0x12AD4 = 1`) instead of calling the function. Task counts in brackets:

| pass (`ARK_RENDERPASS_`) | function | pass (`ARK_RENDERPASS_`) | function |
|---|---|---|---|
| CLEAR | exe+0x3110E0 | SSAO | exe+0x305BE0 |
| UPDATE_NPC_DECALS | exe+0x340F10 | SHADOWS_WORLD [16] | thunk exe+0x32F390 -> exe+0x32D4B0(task, 0) |
| CULL_OCEAN | exe+0x31A320 | SHADOWS_DYNAMIC [16] | thunk exe+0x32F380 -> exe+0x32D4B0(task, 1) |
| GATHER_UMBRA | exe+0x31C710 | INDIRECT_LIGHTING_ACCUMULATE_FINAL_SH | exe+0x310190 |
| GATHER_LIGHTS | exe+0x31BCC0 | INDIRECT_LIGHTING_UPDATE_IBL | exe+0x3102A0 |
| GATHER_DYN_MODELS [6] | exe+0x31AD30 | INDIRECT_LIGHTING_UPDATE_PROBES | exe+0x310260 |
| SORT | exe+0x33D320 | SCREENSHADOWS | exe+0x32C7F0 |
| SHADOWS_PREPARE | exe+0x333550 | TLFLIGHTCULL | exe+0x2F7260 |
| COLLECT_DYNAMIC_CASTERS | exe+0x330020 | FORWARD_WORLD / _DYNAMIC | exe+0x3133D0 / exe+0x3133B0 |
| GATHER_CASTERS | exe+0x330C10 | FOG | exe+0x309430 |
| INDIRECT_LIGHTING_UPDATE_DYNAMIC_MODELS | exe+0x310230 | UPDATE_OCEAN_FFT / UPDATE_OCEAN | exe+0x3053B0 / exe+0x305370 |
| INDIRECT_LIGHTING_UPDATE_WORLD_STATE_AND_OCCLUDERS | exe+0x310200 | RENDER_WATER | exe+0x3045A0 |
| GPUCULLING | exe+0x2FC030 | DOF_AUTOFOCUS / DOF | exe+0x3150D0 / exe+0x315450 |
| DEPTHPASS [3] | exe+0x2FD8D0 | POSTPROC / POST_TONEMAP | exe+0x327270 / exe+0x323E70 |
| DEPTHMINMAX | exe+0x2FC130 | TEXTURE_STREAMER | exe+0x34FD00 |
| OCCLUSION_QUERIES | exe+0x31D7E0 | IGGY_FULLSCREEN | exe+0x31A8E0 |

- The shadow task exe+0x32D4B0: task+0xF8 below `r_shadowParallelSliceCnt` is a sun cascade (exe+0x32EE50), anything
  else a spot / omni light of the atlas (exe+0x32D6E0).
- Cascade time slicing (`r_shadowParallelPartialSliceUpdate`, exe+0x32EF08): slice 0 every frame, slices 1..n-2 on
  odd frames, slice n-1 on even ones (frame counter exe+0x32FC64C). A slice not updated is restored from a per-slice
  cache (a copy, exe+0x2BB270, from the caches at exe+0x3308DE8) -- not re-rendered. In synchronised stereo the frames
  alternate eyes, so each eye's distant cascades were fitted from the other eye's view.
- The light probes' update budget `r_ILProbeUpdate_MaxUpdatesPerFrame` is read per rendered frame at exe+0x2B32A1.
- The ocean passes (exe+0x3053B0, exe+0x305370) skip themselves when there is no ocean exactly as the executor skips a
  disabled task (context +0x12AD4 = 1, return 0); UPDATE_OCEAN's work reads the view at task+0x70 (+0x3A34).
  INDIRECT_LIGHTING_UPDATE_IBL reads the world through the view (task+0x70, then +0x39C0 = `m_parentWorld`).
- **INDIRECT_LIGHTING_UPDATE_IBL is bookkeeping, not relighting** (exe+0x3102A0 -> exe+0x2B4D30, indirect lighting
  object = world+0x39C0 -> vtable +0x50): the IBL reflections are baked cube maps streamed from disk
  (`generated/image/cubemaps/`). The pass copies the ones the streamer has just brought in (the list at +0x407D0,
  0x30-byte entries, count +0x407D8, emptied at the end) into the IBL cube map arrays (exe+0x2BB2A0 per face,
  exe+0x2922C0), then, when the probe set changed (IBL manager = +0x5A0, dirty flag +0x2ED), uploads the probes'
  parameter buffers (+0x60: +0x2D4 x 4 bytes, +0x80: +0x2D8 x 64 bytes) through the task's own context (exe+0x292220).
  Skipping it for one eye's frame leaves that frame's draws reading probe indices against stale buffers: wrong
  reflections in that eye only (glass, heads shining like chrome; measured 2026-10-29). Other reflections: there is
  no general screen-space reflection pass, only the ocean's (RENDER_WATER; `r_skipOceanSSR`,
  `r_water_ScreenSpaceMaxStepsCount` 64); `r_IBL_ParallaxCorrection` (1) box-projects the cube maps.
- **The task executors** exe+0x2C5D50 (deferred) and exe+0x2C5F60 (task flag 0x20: on the immediate context, called
  again while it returns true; otherwise it executes the task's command list +0x118): around `call [task+0xD8]` the
  context wrapper task+0x108 (+0 = the ID3D11DeviceContext; exe+0x2BAE70 before, exe+0x2BBA20 = finish -> +0x118 after).
  The immediate context is *(exe+0x32D4EF8), its wrapper *(exe+0x32D4EF0). PassProfile times each task there.
  TEXTURE_STREAMER (exe+0x34FD00) is a thunk to the streamer object at exe+0x32F0210 (exe+0x30D1E0).

### 4.6 Render resolution, the window and the cfg writer
- The engine renders at its window's **client size** (at most the desktop). What the size follows:
  - **WndProc exe+0xE9F80**: on **`WM_WINDOWPOSCHANGED` (0x47)** (not `WM_SIZE`, which goes to the default case) it calls
    `GetClientRect` (**return exe+0xE9FF1**) and sets `r_windowWidth/Height` through the cvar system. A `WM_SIZE` sent
    from outside does nothing.
  - **`GetHdcClientWindowSize` exe+0x346320** (`GetClientRect`, **return exe+0x346364**): the size the renderer resizes
    targets and the swapchain to (exe+0x36A794 -> exe+0x3726C0 -> ... -> `ResizeBuffers(0,0)` from exe+0x36ADA3).
  - **exe+0x32FAF80 / +0x84** (int, int): the engine's window size (posX/posY at -8/-4, a float 1.0 at +8, fullscreen
    flags at +0xC/+0xD); the render view's size follows it. Written by the virtual exe+0x348FA0; accessors
    exe+0x346750/0x346760. Writing it alone renders into targets of the OLD size: they are reallocated only on the
    resize path. A bigger swapchain alone is not enough either.
- **Where the size leaves the engine**:
  - **`WriteConfigToFile` exe+0x41770** `(common, const char* name, int, int mask, bool)`: FIVE arguments, the bool on
    the stack (callers write [rsp+0x20]). Called through common's vtable +0x98 from the renderer init (exe+0x369519) and
    the mode/resize path (exe+0x3520A8). A hook declared with four arguments made the game write a cfg with no cvars.
    The console `writeConfig` (exe+0x42920, mask 0x20000) writes a DIFFERENT, partial file: never run it on the
    player's cfg. Quitting does not write the cfg. Back the cfg up before touching anything that writes it.
  - **Window placement exe+0x347630** (rcx = display object exe+0x32FC580): `SetWindowPos` from
    min(`r_windowWidth/Height`, the struct). Called from the WndProc's `WM_SETFOCUS` (only when the byte at
    exe+0x32FAF8D is set: not windowed) and when video options change (exe+0x51198B, flag menu+0x309).
- **The interface cursor** is read at exe+0xE27B0 (`GetCursorPos` + `ScreenToClient`, **return exe+0xE27DF**), in client
  pixels taken 1:1 as frame pixels. `ClipCursor` exe+0xE2650 and centring exe+0xE25E0 use the real client rect.
- Answering the two `GetClientRect` call sites (and `ResizeBuffers`) with another size makes the engine render at that
  size while the window stays as it is; trigger with `SetWindowPos(SWP_FRAMECHANGED|NOSIZE|NOMOVE)`. Run the cfg writer
  and the window placement with the real window size in the cvars, and scale the cursor at exe+0xE27DF.
  ([RenderRes.cpp](../src/Src/RenderRes.cpp)) Not in borderless (`r_fullscreen 2` renders at the monitor's size) or
  fullscreen.
- Dynamic resolution (`rs_*`) defaults on and targets the monitor's refresh: it sat at 85% (software upsample).

### 4.7 Culling
- `arkSkinnedRenderModel` (vtable exe+0x1B0D628, ctor exe+0x25D090): world AABB at **+0x1840 (min) / +0x1850 (max)**,
  computed in exe+0x25F6D2 by exe+0xFB8C0(out, decl+0xE0 local bounds, +0x1860 origin, +0x186C axis, +0x1890);
  then **exe+0x237340(model)** derives the bounding sphere: centre at +0x1BD8, radius = max extent + margin(+0x18C0) at
  +0x1BE4 (margin <= 0: radius 32768, never culled). Readers of +0x1860 per frame: exe+0x109622 (render matrix, from
  exe+0x235735), exe+0xFBA2C (the box), the joint queries.
- **The engine culls against the render view using each model's REAL position.** Anything moved on the GPU only
  (rewritten cb2) is still culled where the engine thinks it is: a weapon in front of the character's body vanishes
  when the rendered view turns away from the body (all 26 draws at 0/45 deg, 6 at 90, 0 beyond), and the pistol's
  reload (the flat weapon goes 0.6 m below and behind the eye for ~0.8 s) submits no draw at all. Widening the box at
  exe+0x237340 to cover where the model is drawn keeps it.
- Arm models: the arm entity's origin (shared by both arms) is ~0.35 m below and behind the eye; a draw filter that
  rejects origins behind the camera misses the arms.

### 4.7b Spot / omni light shadows
- The shadow task exe+0x32D4B0(task, dynamic): task+0xF8 below `r_shadowParallelSliceCnt` (int at exe+0x397A2F8) is a
  sun cascade (exe+0x32EE50), else a spot / omni light: exe+0x32D6E0(task, params, index - slices, count - slices).
- Sun cascades (exe+0x32EE50): slice 0 every frame; with `r_shadowParallelPartialSliceUpdate` slices 1..n-2 on odd frames
  (exe+0x32FC64C), n-1 on even. A slice not updated is copied back from its cache (exe+0x3308DE8[slice-1], exe+0x2BB270);
  an updated one is copied into it. Render matrices from exe+0x3309020 + slice*0x40; the lighting binds exe+0x3308E20..
  (exe+0x32FD40).
- Job lists: the task parameter (task+0xE0 list of 16-byte entries, count task+0xE8) whose +8 is 0xA4719E7A holds at
  +0x10 an object with three idLists (ptr +0, count +8 & 0xFFFFFF) of job pointers: +0x26A030 world (shared by the
  tasks but the last), +0x267020 world (the last task), +0x264010 dynamic.
- Job: +0 the light (type at light+0x5C: 0/1 six faces, exe+0x32C630; 2 job+0x18 faces, exe+0x32D940), +0x10 face
  records of 0x100: +4 atlas page, +8 rect x0 y0 x1 y1 (x0 = -1: none), +0xA0 matrix, +0xE0 cache entry, +0xE8 pointer to
  a flag, +0xF0 DIRTY.
- SHADOWS_PREPARE (exe+0x333550) per face: the cache manager (exe+0x3309A08) lookup exe+0x2CD500; found = clean, else a
  new entry (exe+0x2CCA20) and DIRTY (exe+0x333401 / exe+0x33344F).
- World pass: the first spot task clears every atlas page (exe+0x3308D90 list, exe+0x2BAFC0); per face (exe+0x32CEA0)
  DIRTY = draw its static casters into the atlas rect, clean = copy its cache entry there (exe+0x32DB10). Dynamic pass:
  its first task copies every DIRTY face from the atlas into its entry (exe+0x334C90), then moving casters are drawn on top.

### 4.8 Fog and its upscale
- View render flags +0x3A36 (set each frame at exe+0x374F9F..): bit 0 `r_skipFog`, bit 2 `r_skipLightScattering` (the
  fog pass tests it at exe+0x3098BF), bit 3 `r_skipGodRays` (archived: the menu's god rays option).
- The fog is rendered at a reduced resolution (`atm/distanceFog/TexDepthHalf`) and brought to the screen by
  exe+0x30BB70: with `r_fogNearestUpscale` (int, default 1, read every frame at exe+0x30BC46) a depth-aware
  nearest-sample upscale with `r_fogNearestThreshold` (0.0002, exe+0x30BC5E), else a filtered 4-tap one. Related
  tunables (not archived): `r_fogBlurRadius` 6, `r_bilateralSharpnessNear` 0.1 / `Far` 0.01, `r_bilateralDepthRange` 10,
  `r_atmGodRaysDepthSharpness` 0.00001.
- In a headset the fog's low resolution shows as blocks in front of the sky; neither the filtered upscale nor
  `r_skipLightScattering` changed it visibly (tested on hardware). `r_skipFog` (flag cvar, read every frame at
  exe+0x374F9F into view+0x3A36 bit 0 and at exe+0x30644A) removes it: the mod's `Fog = 0`.

---

## 5. Camera, aim and the player

### 5.1 Globals
- `gameLocal = *(exe+0x39A5290)` (also the "game" pointer the entity table and body table hang off; **NULL during a
  level change**). `GetLocalPlayer(gameLocal)` = exe+0x8A8830 (null outside gameplay).
- Player (`idPlayer`): `firstPersonViewOrigin` +0x300 (idVec3), `firstPersonViewAxis` +0x30C (idMat3). This is the
  body's facing: **its pitch is always 0** (the axis is flattened); the view pitch lives on the camera.

### 5.2 arkCpntPlayerCamera
Vtable exe+0x1C72218, ctor exe+0x7EF5A0, update exe+0x7F38E0 (runs at load: a place to learn the component address).

| off | field | who reads it |
|---|---|---|
| +0x30 | `renderView_t` of the camera | `vieworg` = **+0xD8**, `viewaxis` = **+0xFC** |
| +0xD8 | eye origin | weapons, aim component, melee (via exe+0x417820), interaction rules, scene view |
| +0xFC | view axis (rows fwd/left/up; fwd.z = sin(pitch)) | same (via exe+0x8067C0) |
| +0x251B8 | `m_rightArmFollow` (bool) | |
| +0x25264 / +0x25268 / +0x25274 | `m_pitch` / `m_armsPitch` / `m_yaw` | |
| +0x2527C | `m_aimOrigin` (= eye) | powers, sensors (interaction, Far Reach, Blink), throwables |
| +0x25288 | `m_aimAxis` (idMat3) | same; **no weapon and not the renderer** |
| +0x25304 / +0x25328 | `m_leftArmAiming` / `m_rightArmAiming` (9 floats: [0..2] limits 1.22/2.61/0.349 rad, [3..5] yaw/pitch/roll of the arm IK relative to the view, [6..8] a second joint) | arm IK. Writing a matrix there corrupts the arms until a reload |
| +0x25440 / +0x25464 | copies of the renderView axis / origin (exe+0x7F2330 writes them) | |
| up to 0x252B0 | the component through the aim fields (size of a usable copy) | |

- **`UpdateAim` exe+0x7F4B30** (chunk exe+0x7F4B59, called from exe+0x7F7219): sin/cos of pitch/yaw -> `m_aimAxis`, then
  `m_aimOrigin`. Writing `m_aimOrigin/m_aimAxis` right after it holds for the rest of the frame (what every power and
  player sensor aims with). `GetAimOriginAndAxis(this, out, out)` exe+0x3C7CD0 (camera at this+0x40) is not used by
  firing.
- **The eye travels with the pitch.** UpdateAim takes the world position of the joint at cam+0x2519A (exe+0x53D940)
  and the eye (exe+0x417820), and puts `m_aimOrigin` at that joint + |eye - joint| * the view's forward: the eye is on
  a sphere round the head joint. exe+0x7F4CD0 bends the joints at cam+0x2519E / +0x2519C / +0x2519A by fractions of
  the pitch (+0x25268 / +0x2526C / +0x25270). So looking down carries the eye forward and down: a neck for a flat
  screen (the mod's `CameraPitchFix` takes it out in VR, measured rather than modelled).
- **The last writer of cam+0xD8/+0xFC each frame is exe+0x88C5E0** (adds shake and bob; rdx = cam+0x30): its return
  is the place to take a snapshot of the final camera.
- `renderView_t copy(dst, src)` = exe+0x444C0, called with src = cam+0x30; the destination feeds the scene (view setup
  at exe+0x3747A9).
- **The camera fields are read by many threads.** Temporarily swapping them (to run one engine call "as if" the camera
  were elsewhere) is visible to every other thread for its duration: the head-aim reader once took a swapped-in hand as
  the camera. Serialise every swap with the snapshot and the renderView copy, or better, hand the engine a COPY of the
  camera through the pointer the call reads it from (sensor+0x40, weapon+0x48).
- 30 code sites read `m_aimAxis`: sensors (exe+0x3A4665 Blink, 0x3B851C, 0x3B8DD2, 0x3C38E5, 0x3C7D0C), powers
  (0x61EE6B, 0x61FF2E, 0x6209EA, 0x620E60), throwables (0x632DD8, 0x633116, 0x639816, 0x639CF4).

### 5.3 The aim component
Player aim component (vtable exe+0x1C52BF8): `m_currentAimLocalDir` +0x20, `m_currentAimWorldDir` +0x2C,
`m_currentAimPosition` +0x38, camera +0x58. Per-frame update exe+0x726280 (world dir from cam+0xFC + aim assist).

### 5.4 The player state machine
- ~90 **`arkPlayerState*`** classes (RTTI `.?AVarkPlayerStateChoke@@` ...). Vtables from MSVC RTTI: TypeDescriptor (name
  at +0x10) <- CompleteObjectLocator (signature 1, TD rva at +0xC, self rva at +0x14) <- the qword before the vtable.
  Full table of 91 names and vtable RVAs in [PlayerState.cpp](../src/Src/PlayerState.cpp); e.g. Choke exe+0x1C77C18,
  Assassination exe+0x1C83468, Slash exe+0x1C85D28, Sneaking exe+0x1C84258, Cinematic exe+0x1C77F68, HoldObject
  exe+0x1C7A998, CarryCorpse exe+0x1C77748, Powerwheel exe+0x1C3FF18, CharacterChoice exe+0x1C3AAA8,
  BlackMarketInteract exe+0x1C3BD48.
- **Which state is running**: point every slot of every state vtable at a counting stub
  (`mov rax,&ctr; lock inc [rax]; mov rax,orig; jmp rax`; rax is free at a call in the x64 MSVC convention). Only
  vtable entries change, objects keep their vtable pointer. **Slot 13 is the per-tick update** and is called only on the
  running state(s): `Sneaking:2,13,24` becomes `Choke:2,13,24` on the frame the choke starts. **Slot 6 is polled on
  dozens of states every frame** and means nothing. The stubs also give each state's `this` (the state object).
- `Idle` never reports running (standing still reports `Running`); use the player's speed for "standing still".
- In the main menu a player runs too (`Running Slipping`). While the power wheel is up the player ticks every ~0.17 s.
- Some states measured:

| state | what the camera / body does |
|---|---|
| `Choke` (Ctrl held; 5.4-5.6 s, then the held Ctrl becomes `Block`) | eye jumps +0.66 m in one frame at the grab, lunges ~0.7 m, sways yaw +-12 / pitch -12..+22 / roll -10..+4; body yaw never moves; ignores the mouse |
| `Slash` | every sword swing; the kill from behind is usually a Slash with the kill outcome (~1 s, camera pitch +7 -> -33 in one frame, back 0.3 s later) |
| `Assassination` (~2 s) | chained from a Slash on an unaware guard: roll -14 -> +15, 1 m lunge, body yaw left 29 deg turned |
| `NpcFatality` | an NPC executing the player |
| `Cinematic` | an in-engine cutscene, see 5.6 |
| `CatFall` (~0.6 s after an ~8 m drop), then `EquipChange` | crouch 1-1.4 m, pitch -35..-78, roll to -72, yaw swing 70 |
| `Mantle` (~0.5 s) | pitch -25..-28, roll -2.5, eye up 1.1 m and up to 35 cm off the body axis; ignores the mouse; the weapon is holstered after (~1 s) |
| `Sliding` | eye down 1.2 m, camera rolls to -21; the "recovery" is `Running` undoing the roll in ~0.55 s. While sliding the mouse turns the CAMERA 1:1 and the body follows; at the end the body snaps to the camera (35 deg in one tick) |
| `Swimming` / `SwimMantle` | at the surface no first-person arms are drawn (a stroke shows them a frame or two) |
| `ShadowWalk` / `ShadowHide` | the arms become claws (see 4.4) |
| `EnterPossessedHost` | the leap into a host; nothing specific runs inside, no first-person draws at all; the camera is the host's (human eye 1.70 m, rat 0.18 m over the player origin), animating pitch and roll; the player entity rides at the host's feet with frozen yaw (~26 s at level 2) |
| `HoldObject`, `CarryCorpse` | see section 8 |
| `ThrowWeapon` | runs from the PRESS (cooking and the throw animation are one state) |
| `PlaceWeapon` / `PlaceWeaponReady` | mines, spring razors |
| `CharacterChoice`, `BlackMarketInteract` | see section 9 (menus over a running world) |

### 5.5 The servo problem (why mouse injection needs a closed loop)
- Turning the game with mouse counts is open loop and the counts-per-degree calibration is not repeatable (29.0 vs 35.4
  in the same save). Close the loop on the measured yaw (`player+0x30C`) and pitch (`cam+0xFC`).
- A level load, a slide's end, a cutscene's cut all make the body or camera jump in one tick. A loop that rejects
  "jumps" must re-seed after a run of rejections, or it latches shut.
- The camera ignores the mouse in takedowns, `Cinematic`, `Mantle`, `CatFall`; injecting against it overshoots on the
  way out (the hard landing's blend-out reached +32 deg with injection, +11 without).

### 5.6 Cutscenes
- An in-engine cutscene is the player's **`Cinematic`** state: the game keeps simulating and may draw the player's arms.
  The executable also names `MODE_CAMERA_CINEMATIC_WITH_ARMS / _WITHOUT_ARMS / _ANIMATED` (arkCpntPlayerCamera),
  `m_cinematicLookAt*`, `m_cinematicControlRecenter`, and arkGameLogicStateCinematic's `WAIT_SKIP / SKIPPING`.
- Its camera ignores the mouse, swings and rolls on its own, and can cut. In a ride (the train, the skiff) the BODY moves
  at the vehicle's speed (train ~14 m/s, skiff 3-11 m/s) and its yaw turns with the vehicle (train 22.5 -> 89.8 deg in
  7 s, skiff ~160 deg), while the camera looks around independently (up to 40 deg off the body). A choice dialog inside
  a cutscene stops the simulation (a real menu).
- **The letterbox is painted by the interface over a full render**: two `DrawIndexed` of 39 indices with the Scaleform
  vertex shader (DXBC `7ACC1A9A27C4D9E2`) and an untextured pixel shader into the backbuffer; skipping them shows the
  whole picture. The fov override still applies in a cutscene.
- **Skip = INTERACT held ~3 s** (the prompt with a ring); a tap does nothing.

### 5.7 Takedowns
- Prompts (default binding): `Ctrl` = choke (hold it; releasing early lets the guard go), left click = kill, `F` =
  pickpocket / carry the body. Sensors per takedown type: `arkCpntPlayerSensorChoke/Assassination/DropAssassination/
  WindowAssassination`.
- The assassination sensor: type exe+0x39B4750 (registered exe+0x13A7230; GetType exe+0x8C78F0, vtable exe+0x1B3CC38),
  a component of the player (exe+0x877720(player, type, 2)). Vtable slot 26 (exe+0x3A19A0) = "a kill is possible now":
  the target's id at +0x40 (exe+0x6DC560 -> entity), its state checks (exe+0x5BF440, the player's state machine at +0x20)
  and +0x48 clear. The choke sensor: type exe+0x39DD090 (registered exe+0x13D75F0, GetType exe+0x918D70).
- The player's state machine (the sensor's +0x20): +0x90 -> states at +0x10 + 8 * index (PLAYER_STATE_* enum, from
  0 = RUNNING: 6 SNEAKING, 7 SLIDING, 13 ASSASSINATION, 39 SLASH, 40 RECOIL, 41 CHOKE, 44 BLOCK; table of names at
  exe+0x2391BC8 - 13 * 0x18); exe+0x803180(sm, i) = state i running (state+0x20), exe+0x803400(sm, i) = may enter.
- **The choke's knockout** (exe+0x82D310, the choke state as `this`, target id at +0xB8): skipped when the NPC's +0xD1
  has 0x20 or 0x8; damage = exe+0x780260(0x90-byte record, the state's decl +0x18 -> +0x190), attacker id at +0x54
  (exe+0x854880(state) = the player), exe+0x882E20(tmp, record) -> exe+0x884920(npc, out, exe+0x39A9D20, tmp),
  exe+0x882FC0(out); then ARK_MSG_HAS_CHOKED (0x13E, payload the NPC's id) to the player. The Wall of Light's knockout
  is a damage too, followed by ARK_MSG_WALL_OF_LIGHT_STRIKE_UNCONSCIOUS (0x1B9).
- During a takedown the game places the arms in the world from its own animated camera: a VR view may take its rotation
  from the head and its position from the animation and the arms still land on the victim.

---

## 6. Entities, components, models, joints

### 6.1 Entity layout

| off | field |
|---|---|
| +0x00 | vtable (`arkPlayer` exe+0x1C5E428 for the arm entities, `arkWeapon` exe+0x1CE1E98, gadgets / weapon parts exe+0x1C87D58, `arkMovable` exe+0x1C5DE18) |
| +0xA8 | spawn id |
| +0xD0 | flags; bit 0 = hidden |
| +0xF0 | `arkSkinnedRenderModel*` |
| +0x300 | world origin (idVec3) |
| +0x30C | world axis (idMat3, rows fwd/left/up) |
| +0x330 | scale (per axis) |
| +0x33C | velocity (synced from Havok every frame by exe+0x753C10) |
| +0x348 | angular velocity |
| +0x360 | the skeleton / animation object (its +0x38 = the render model) |
| +0x368 | second render model (arms, body) |
| +0x480 | second render model (weapons) |

- Vtable **+0x80 = Hide**, **+0x88 = Show** (for the arm entities: exe+0x8784B0 / exe+0x87C590): they only set/clear
  +0xD0 bit 0 and tail-call a render refresh (exe+0x87E040); idempotent. Setting the bit by hand does not refresh.
- The world transform is produced by the transform component's update **exe+0x753C10** (`this` = transform component,
  entity = this+0x10, parent provider = this+0xD8 (== this+0x20 when unparented); writes entity+0x300/+0x30C/+0x330 from
  this+0x30/+0x3C/+0x60). [WeaponPose.cpp](../src/Src/WeaponPose.cpp)
- Teleport an entity: exe+0x87D1E0(entity, pos, axis, 0). Set velocity: exe+0x87C2A0 (entity+0x33C = *rdx); angular
  velocity exe+0x87C280.
- **World AABB**: exe+0x8780D0(entity, float out[6]) from the render model's local bounds (model vtable **+0x80** returns
  min xyz, max xyz), scaled per axis by +0x330 (exe+0xFB8C0) and turned by +0x300/+0x30C. For props the box encloses
  what is drawn (the render model is not rotated against the entity); origins are wherever the artist put them (bottles:
  at the base). Characters' boxes are their BIND pose (standing, arms out) whatever they are doing.

### 6.2 Looking entities up
- **Entity table**: `*(exe+0x39A5290) + 0x3130B0`, 0x4000 slots indexed by `spawnId >> 16`, valid when the entity's
  +0xA8 equals the id (lookup exe+0x7B5E00). `0x3FFF0010` = "no entity".
- **`GetEntityById(int)` exe+0x6DC560 faults during a level change**: it reads the table through the global, which is
  NULL then (crash at exe+0x7B5E14 with rcx = 0x3130B0, seen entering a level ~3 s after its cutscene). Check the
  global first ([BodyHide.cpp](../src/Src/BodyHide.cpp) `EntityById`).
- **Physics body -> entity** (what the game's target test does, exe+0x635B00 / exe+0x6350B0): body table at
  `game+0x60410` (records of 0xB0 at +0x28, count +0x30; a record's +0x70 must be the id and +0x44 & 3 set), then
  exe+0x17D10(record) -> +8 -> +0x10 = the entity. A ragdoll's bodies resolve to the NPC's entity.

### 6.3 Components
- **`GetComponent(entity, typeinfo*, int flags)` exe+0x877720.** flags 2 is the usual; some components need 0 (a
  grenade's `arkCpntGrenade` is not returned with 2).
- Typeinfo records used (`exe+`): body component `0x39CB7A0` (left/right arm entity ids at +0x370 / +0x680),
  `arkCpntInventory` `0x39DCD30` (the "equipment component"), `arkCpntMana` `0x39D13B0`, `arkCpntPlayerPowerSystem`
  `0x39E2BB0`, `arkCpntPlayerOutsiderMark` `0x39D15F0`, `arkCpntAttributes` `0x39EDB90`, `arkCpntHealthEvolved`
  `0x39BA160`, `arkCpntGrenade` `0x39E28E0`, `arkCpntMine` `0x39BFBA0`, the bound-FX joint component `0x39DC9D0`, the
  physics-centre component HoldObject asks for `0x39B4630`. Others seen: `arkCpntPickable_Simple` `0x39C5A30`,
  `arkCpntPhysicsBase` `0x39E85B0`.
- A component's owner entity is at **component+0x10**, its decl usually at **+0x8**.
- Item component vtables (inventory item +0x10, see 10): `arkCpntHeart` exe+0x1B99C68, `arkCpntWeaponThrowable`
  exe+0x1B9D8B8 (grenades), `arkCpntWeaponPlaceable` exe+0x1B54538 (stun mine, spring razor), `arkCpntWeaponGun`
  exe+0x1B53F58, `arkCpntWeaponCrossbow` exe+0x1B9CBF8, `arkCpntWeaponMeleePlayer` exe+0x1B9D508
  (`arkCpntWeaponMelee` exe+0x1B9DE18, `arkCpntWeaponMeleeNPC` exe+0x1B9CF48).

### 6.4 The player's entities
- The player is several entities: `PlayerBody` (the player entity), `PlayerLeftArm` / `PlayerRightArm` (class
  `arkPlayer`, one named "right_arm_2"), `PlayerLeftEquipment` / `PlayerRightEquipment`. Name atoms at
  exe+0x32CE5A0/5A8/5B0/5B8/5C0/5C8.
- Arms: body component (typeinfo 0x39CB7A0) +0x370 left / +0x680 right = id holders; exe+0x774A10(idHolder, show) =
  GetEntityById + Show/Hide (how the game hides an arm).
- Equipment: `arkCpntInventory` +0x70 = left item, +0x68 = right item; `item+0x8` = the entity's spawn id. Do not require
  the `arkWeapon` vtable: gadgets are another class.
- **The weapon's draws sit exactly at the equipment entity's world transform** (0.000 m; weapon mods ~0.13 m; everything
  else > 0.4 m), so a draw belongs to a hand's weapon when that equipment entity is the nearest first-person entity to
  its model origin. Upgrades (scope, drum, ...) are separate entities at the weapon's origin, and models bound to a
  weapon joint (the pistol's cartridge) also sit near it. No parent link to the weapon was found within 0x600 bytes.
- A **power's** equipment entity is NOT a hand socket: it sits on the forearm, ~0.36 m from the shoulder root.
- **Equip changes** (2026-10-31): every hand change is a player state queued by exe+0x8024D0(machine, state, params):
  state 0x35 = the right hand's equip change, 0x40 = the left's (arkPlayerStateEquipChange, vtable exe+0x1C79CF8;
  slot 18 exe+0x836F00 takes the params). Params (exe+0x835590 builds them, type 0x5C): +0x08 the inventory entry (0 =
  none / what the hand had), +0x18 flags (arkEquipChangeFlags_t: 1 LOCK, 2 UNLOCK, 4 REEQUIP, 8 REEQUIP_BOTH_HANDS,
  0x10 INSTANT_EQUIP, 0x20 INSTANT_UNEQUIP, 0x40 FAST_EQUIP, 0x80 FAST_UNEQUIP, 0x100 THIS_HAND_ONLY), +0x1C the source
  (0x5B). The message handlers: 0x9D equip exe+0x7F8D60 (its flags byte: 1 only into an empty hand, 2 instant -> 0x10,
  4 -> 0x100), 0x9E unequip exe+0x7F9140 (flags 0, no entry). The state's handling, exe+0x8363F0: an item put in a hand
  WITHOUT 0x100 queues the other hand's REEQUIP (no entry, flags 4, at exe+0x8366C6); a right-hand REEQUIP with no
  item remembered for it (the player's equip memory at machine+0x2F8, exe+0x7F88B0) equips the player decl's default
  weapon, +0x88 = the sword (exe+0x8365EC). That is how the game keeps the sword in the right hand.

### 6.5 Render models
`arkSkinnedRenderModel` (vtable exe+0x1B0D628): local bounds via vtable +0x80; world AABB +0x1840/+0x1850; **world
transform +0x1860 (origin) / +0x186C (idMat3)**, written by exe+0x808880; sphere +0x1BD8/+0x1BE4; `m_numVisibleMeshes`
+0x1F40; `m_transformSet` +0x1F48; skeleton resource +0x1FE8 (joint count =
`*(u16*)(*(*(model+0x1FE8)+0x50)+0x78)`); **`m_meshVisibilityMasks` +0x2100 (5 qwords)**: zeroing them makes the
engine recompute `m_numVisibleMeshes = 0` and the model disappears; restoring brings it back. Writes through a model
pointer must be revalidated the same frame: a level load frees models while they are still bound.

`arkTransformSet::SetOriginAndAxis(this, idVec3*, idMat3*)` exe+0x9D560: matrix +0x60, axis +0xA0, origin +0xC4. The
muzzle flash is placed from the weapon render model's transform set.

### 6.6 Joints, poses, binds
- **Model-space joint queries** (the two everything is built on; [HandFx.cpp](../src/Src/HandFx.cpp)):
  `bool exe+0x53D3F0(obj, int space, short joint, const idVec3* off, const idMat3* offAxis, idVec3* pos, idMat3* axis)`
  and `bool exe+0x53D6A0(obj, space, joint, pos, axis)`. `obj+0x38` = the render model; world = model+0x1860 +
  rows(model+0x186C) * pos. World wrappers: exe+0x53D800 (7 args: component, space 3, joint, offPos, offAxis, &pos,
  &axis), exe+0x53D940; by joint name/reference exe+0x53DAC0 (86 callers).
- **Pose array**: skeleton object `obj` (entity+0x360, check `obj+0x38 == entity+0xF0`): `obj+0x2D8` {ptr, count low 24
  bits at +0x2E0}, 64-byte entries in MODEL space (axis rows at +0x00/+0x10/+0x20, position at +0x30; exe+0x9B900
  converts one). Remaps per space at `obj+0x2E8 + space*16` {short* joint -> entry, count}: space 1 = identity = the
  BIND pose; space 3 = the animation's pose (what the engine hangs things on).
- **Ragdolls write the TAIL of the pose array** (the last J entries), while space 3 keeps the last animated pose
  standing. The array is rebuilt over time (667 = 3J-2 entries, then 530), so use "the last J", never an absolute
  index. Tell a ragdoll pose by its bone lengths matching the bind's and its position differing from space 3.
- No parent table was found in the skeleton definition; joining each joint to the nearest earlier joint in the bind
  pose gives usable bone capsules.
- **Entity binds**: exe+0x7506D0 places an entity bound to a joint. It takes **SEVEN** arguments (it reads the 5th at
  entry+0x28 and divides by the 7th at entry+0x38; hooked with six it crashed in a load); `self` = transform component,
  entity at +0x10; it calls exe+0x53D800 with space 3. Held objects, equipment and weapon attachments come through it.

---

## 7. Weapons and combat

### 7.1 The ranged shot
Fire sequence (gun exe+0x44EBD0, crossbow around exe+0x62B6xx):
1. **exe+0x639670**(weapon, out) = `GetAimPoint(provider #9, out, decl+0x198 range)`. `GetAimPoint` exe+0x3C3CC0: the
   provider is the player's `arkCpntPlayerSensorRangedTarget` (+0x40 camera, +0x48 crosshair/aim component, +0x54
   has-target, +0x58 point), else eye + aim world dir * dist.
2. **exe+0x639120 spread**: projects through the CAMERA's renderView (cam+0x30), adds a 2D offset, unprojects, keeps
   |aim - eye| (crossbow 0.0-0.07 deg, pistol 1.5-2.2 deg).
3. **`arkCpntWeaponRanged::ComputeAimTarget` exe+0x638AE0** (this = ranged component, rdx = out spawn, r8 = view
   axis, r9 = aim point, two byte flags on the stack). Shared by gun (call at exe+0x44ED59), crossbow (exe+0x62B7DB) and
   throwables (exe+0x633139). eye = exe+0x417820(this+0x48) = cam+0xD8, fwd = exe+0x8067C0 = cam+0xFC; **muzzle M =
   `ent+0x300 + rows(ent+0x30C) * this+0xF4`, ent = this+0x10 = the weapon's equipment entity**, +0xF4 =
   `m_projectileSpawnPos` (the muzzle joint in the weapon model's space, from the tag at decl+0x228; set by slot 3 =
   exe+0x44E2A0 gun / exe+0x62BA10 crossbow, both calling the ranged init exe+0x639F00). If this+0x118 (the projectile's
   collision shape) is null (the PISTOL) spawn = M. Otherwise: angle M->aim vs view against decl(+8)+0x19C, a ray eye->M
   (exe+0x638570, mask 0x800) and a sweep of the shape M->aim (exe+0x638800, mask 0x20000); if blocked the spawn slides
   along the eye's ray (this+0x10C, +0x100). The caller turns `aim - spawn` into the direction after the call.
4. exe+0x112000 (dir -> rotation) -> **exe+0x639970 launch**(this, entity, &spawn, &axis, speed, ...). A throwable
   passes speed 1.0 = "keep the velocity already set".

- `exe+0x638570(any, from, to, mask, float* dist)` = synchronous ray; `exe+0x638800(weapon, from, to, mask, dist)` = the
  same with the weapon's projectile shape (weapon+0x118 dereferenced: null for the pistol = crash). Both return true
  when CLEAR and see only STATIC collision with any mask (0x8 / 0x800 / 0x20000 / 0x408 find the world; 0xFFFFFFFF and
  0x443F find nothing). Safe on the game thread.
- **`arkCpntPlayerSensorRangedTarget`** (vtable exe+0x1B41D28): SUBMIT exe+0x3C3FD0 (dispatch exe+0x954AAE) posts an
  ASYNC ray (exe+0x95130 on the physics world +0x60280) from exe+0x417820(sensor+0x40) along (sensor+0x48)+0x2C, length
  decl+0x74 (400 m), mask decl+0x70 (0x443F, | bit 25 when sensor+0x80); handle +0x50. RESULTS exe+0x3C3E10 (dispatch
  exe+0x95028E): +0x54 has-target, +0x58 point, +0x64 normal, +0x70 dist^2, +0x74 entity handle; sees the DYNAMIC world
  too. Enabled flag +0x38. Both phases read the camera through sensor+0x40 / the aim component through sensor+0x48.
- **exe+0x63A8B0(this, projectile, &ent, &joint)**: before the aim, provider 16 (`arkCpntPlayerSensorAimAssist`, vtable
  exe+0x1B3C8C8; +0x8C/+0x90 has, +0x68 entity id, +0x70 joint) or provider 9's hit hands the projectile a target entity
  (exe+0x784FC0 stores it at a component's +0x364..).
- The projectile's visual effect and the AI noise cue (`arkAICue`, vtable exe+0x1B440A8) spawn at the weapon ENTITY's
  origin.

### 7.2 The pistol's blast and the muzzle effects
- **The gun's shoot** exe+0x44E0C0 (one per trigger pull): vfunc +0x140 (ammo), exe+0x44EE80 (muzzle FX),
  exe+0x63B3C0(this, 0) (muzzle entity), then -- when settings `this+0x98 -> +0x4A` -- **exe+0x44F030(this,
  spreadArea)**, then Fire (the projectile). spreadArea = gun decl +0x238 `m_spreadArea` (+0x240 reduced, +0x248
  upgraded by this+0xB8 bits 8/0x10), an `arkDeclExplosion`.
- **exe+0x44F030 is the pistol's BLAST**, a cone that breaks things on its own: centre = cam+0xD8 (through this+0x48) +
  rows(cam+0xFC) * decl+0x250 `m_spreadAreaOffset` (pistol: (-0.75,0,0), the apex 0.75 m BEHIND the eye), direction =
  camera forward, then game vtable +0x3A0. Explosion decl: +0x6C safe radius 1.0, +0x70 damage radius 4.5, +0x74 full
  damage 2.5, +0x84 angle 30 deg, +0x88 collision mask 0x4638, +0x8C occlusion 0x23, +0x9C impulse 10. Anything that
  moves the bullet must move this too.
- Weapon-attached things ask exe+0x53D800(component, space 3, joint, ...): the model-space query times the render
  model's world transform. Callers on the gun: exe+0x53DAC0 (from the bound-FX update exe+0x4E3090 and from
  exe+0x63B3C0) and the bind exe+0x7506D0 (e.g. the reload cartridge `m_attachedBullet` at gun+0x158, bound to decl+0x220
  `m_bulletAttachmentTag` with offset (0.109,0,0.068); its bind asks for joint 4, returning to exe+0x750757).
- **FX**: exe+0x4E2B00(slot, fxDecl, entity, &joint, ...) creates an instance with exe+0x19D100(fx world, entity id,
  decl), copies the entity's +0x300.. and +0x33C.. into instance +0x38.., starts it with exe+0x1A2FF0(world, inst, 0,
  0x1FFFF, 0.f), fills the slot (+0 handle, +4 entity id, +8 joint, +0x24 joint index or 0xFFFF). exe+0x4E3090(slot,
  posOvr, axisOvr, a4, a5, velOvr, angOvr) re-places it every frame from
  exe+0x53DAC0(GetComponent(entity, 0x39DC9D0, 2), 3, slot+8, ...).
- exe+0x63B3C0 spawns (game vtable +0x1C8, class exe+0x39CF460) an `idEntityFx` at the muzzle joint carrying
  `models/effects/weapons/gun/muzzle_1p_emily.fx` (flash, light, sparks, a paper wad). The gun's bound FX is
  `muzzle_1p_barrel_smoke.fx`; the projectile carries `std_gun_trail.fx`.
- Open: one mesh particle of `muzzle_1p_emily.fx` (instanced-mesh VS `e3ddf13f2df8e1a7`, world matrices from
  `ro_directinstancebuffer`) has its matrix rebuilt from the CAMERA every frame; the emitter decls (stride 0xAE0; +0 name,
  +8 enabled, +0x50 `m_position.m_screenParticle`, +0x51/+0x52 track owner position/orientation, instantiated at
  exe+0x2230A9..0x22310C via exe+0x2268F0 / exe+0x227CD0) show no screen-particle flag set. Cause not found.

### 7.3 Melee
- **The melee code never reads the camera fields itself**: everything goes through two one-line accessors,
  `GetViewAxis(cam)` exe+0x8067C0 (`lea rax,[rcx+0xFC]; ret`) and `GetViewOrigin(cam)` exe+0x417820
  (`lea rax,[rcx+0xD8]; ret`), called from 66 / 85 places in the whole game. Redirect only the 14 call sites inside the
  melee code (exe+0x60D000..0x636000; list in [MeleeAim.cpp](../src/Src/MeleeAim.cpp)) to register-saving thunks
  (callers were compiled against a leaf: save rcx/rdx/r8-r11/xmm0-5).
- `arkCpntWeaponMeleePlayer` (vtable exe+0x1B9D508): **slot 40 = attack direction** (exe+0x62F080: owner entity+0x30C
  if `+0x3428 & 2`, else GetViewAxis(comp+0x3438)); **slot 41 = the sweep segment** (exe+0x634990); **slot 42 = attack
  start** (exe+0x62F290: player origin, eye and axis, eye-based box centred 2 m ahead), both into the shape builder
  exe+0x634FB0. comp+0x3438 = the player camera component.
- **Timing** (MOUSE1 = MELEE): a click starts `Slash` on the next tick; ~0.27 s, then `Recoil` on a hit (~0.33 s in all).
  The direction is queried ~0.2 s after the click. A click DURING a Slash is dropped, not queued. Held: the charge is
  a phase of Slash; released before ~0.5 s it is a plain slash 0.23 s after the release, at >= 0.55 s the charged
  attack (direction queried 50 ms after release). Block (LCTRL, begins 11 ms after the key) with the attack held cancels
  the charge; a click on the tick Block is released is lost. A click right after the player's recoil starts nothing.
- **The attack window**: exe+0x634890(comp, active, b1, b2, float) writes comp+0x3428 bits 0..2 and comp+0x3424. An
  animation event (call at exe+0x84E26D) opens it ~0.20 s after the click, exe+0x637139 closes it at ~0.33 s, a hit
  closes it from the hit check (call at exe+0x631405). Bit 1 makes slot 40 return the body's axis: not usable for aim.
  Opening it from the hit check's own thread works at any time.
- **Candidates are async physics casts**: exe+0x637160 (window open) issues "MeleeWeaponWorld/Entities/Enemies" casts
  (handles comp+0x3414/0x3418/0x341C) of the decl box (decl+0x188 = 2.0, 1.5, 1.5; decl+0x1A4 = 1.5, 0.3, 1.0) oriented
  by slot 40, along slot 41's segment: from the owner entity's (the weapon as the FLAT game animates it) previous
  position (comp+0x33E0) minus 2.8 m along its motion, to its current position, both minus decl+0x17C (0.8) along the
  axis; exe+0x634CE0 clips it to slot 42's box. **exe+0x636B40** reads the previous frame's results into the lists
  comp+0x78 / +0x1188 / +0x2298 (entries 0x88: +0x18 physics body handle, +0x30 point, +0x3C normal, +0x54 a
  fraction), computes the owner's velocity with the frame dt (exe+0x877EB0), and stores the position in comp+0x33E0.
  exe+0x62EE20 only filters a list in place.
- **The strike**: **exe+0x631020** (hit check) runs every frame (bit 0 clear: zeroes the dwell comp+0x3B78 and returns).
  Each candidate goes through **exe+0x62F900(comp, point, shape)** = inside the 30 deg cone (shape+0x14 = 0.866) from the
  attack start. A damageable target (exe+0x635B00) must DWELL in the cone 0.09 s of game time (comp+0x3B78 += dt,
  threshold the float at exe+0x1B9D6F0) unless bit 1; exe+0x62EB20 vetoes while an enemy is in front within range. A
  non-damageable candidate in the cone (prop, world; its cone call returns to exe+0x63136A) is struck through
  exe+0x635140 (a bottle breaks, a wall makes the blade recoil).
- **A strike on a character** closes the window from the strike function: exe+0x6347ED calls the damage (component
  vtable +0x110), exe+0x630540(comp, target id), then (if the target accepts, vtable +0x158) SetActive(0), returning to
  **exe+0x634857**. Objects break with no window close. Corpses take no strike at all.
- So what is struck is what the invisible, flat-animated sword's cast sweeps in front of the camera ~0.25 s after the
  click, chosen by a cone around the slot-40 direction. Opening the window at a contact or ending the slot-41 sweep at
  the contact point makes the object a candidate on the next frame but the strike only PUSHES it (cause not found).
- **Where the damage is.** The per-frame hit check's character path (a candidate in the cone that dwelt 0.09 s) only
  sends the player's state machine events 0x27 / 0x28 (exe+0x630810), closes the window and plays the hit (slot 34,
  exe+0x62FEC0: blood, impact, a hit event). The DAMAGE is the strike, vtable **slot 39**: the player's exe+0x62E550
  (an actor target: slot 33 adjusts the scale), then the base exe+0x634320(comp, hit record, bool needs the window,
  bool close on a hit, float damage scale, qword 0). It skips a target already in comp+0x33D0 (count +0x33D8, cleared
  when the window closes, exe+0x634890), needs slot 35 (exe+0x630890) to accept it, builds the damage (exe+0x780260
  from the weapon's decl, slot 32; amount = the attack's table entry (comp+0x70, -1 = 1) x the scale x decl+0x5C,
  exe+0x780A80), sends it (exe+0x884920), plays the hit (slot 34), records the target (exe+0x630540) and, if asked and
  slot 43 agrees, closes the window. The game calls it from the swing's animation (exe+0x62CB80 / the per-type
  handlers exe+0x6355F0, exe+0x6357D0, which also test the target's block / parry) on a record of the sweep's lists.
  Hit records: 0x88 bytes, +0x18 the physics body, +0x20 -> +0x10 the entity, +0x28 non-null, +0x30 the point, +0x3C
  the normal, +0x48 the sweep's direction.
- **Block / parry.** exe+0x62CB80(comp, target) walks list comp+0x2298 for the target's record and picks the handler by
  the target's class id (its type info +0x60, from vtable slot 0): the range in the globals exe+0x39CE2C0 / +4 (NPCs)
  -> exe+0x6355F0(comp, record, entity); exe+0x39CE350 / +4 (the player class: an NPC's sword on the player) ->
  exe+0x6357D0. A strike that lands sends event 0xE7. exe+0x6355F0: the target's combat component (exe+0x877720(ent,
  &type exe+0x39B3F70, 2); null = strike) holds a state at +0xC8, the entity it is about at +0xCC (an id, exe+0x6DC560)
  and a block type at +0xD4. The attack's def (comp+8) +0x180 == 0 = unblockable: strike. State 3 about the attacker
  (exe+0x596D60): nothing at all. State 5 about the attacker (exe+0x596DA0) = BLOCKING: a d100 roll (exe+0x8AC110)
  against a chance read from the target's component exe+0x39EDB90 -> parry exe+0x636140(comp, ent), else the player's
  slot 36 (exe+0x6302E0, the blocked recoil: events 0x27 / 0x28 on the player), then the sparks exe+0x635F30(comp,
  record) and the window closes (SetActive 0); no damage. Otherwise the combat component's +0x210 set = nothing, else
  the strike (slot 39) with scale 1.0 (exe+0x1F697B0).
- A slash from behind on an unaware guard becomes `Assassination` (kills even with `ark_god_npc 1`).
- **The sword trail** is a ribbon along the animated sword's path: three non-indexed 26-vertex draws, vertex shaders
  DXBC `364e76313eb849ec` / `3fea51d57d8fff32` / `412fe65bec01fc06`. Skipping them removes it and nothing else.
- The sword makes no world-space joint queries during a Slash. Blade (Corvo): crossguard ~0.16 m, tip ~0.67 m along the
  model's forward.

### 7.4 NPC skeletons
See 6.6. Guard: 223 joints. Characters' render boxes are the bind pose (0.38 x 1.28 x 1.83 m for a guard); hit volumes
must come from the pose array ([NpcSkel.cpp](../src/Src/NpcSkel.cpp)).

### 7.5 Health
`arkCpntHealthEvolved` (typeinfo record exe+0x39BA160; class record exe+0x39AE8E0): health float +0x20, maximum +0x114.
`ark_god_player` does not keep the player alive in a fight; pinning +0x20 does.

### 7.6 Throwing grenades
- `arkCpntWeaponThrowable::Throw` exe+0x632F70 (virtual, no direct callers; this = rdi, decl at this+8): gets the
  projectile entity (exe+0x63B5F0 -> r14), ComputeAimTarget (call at exe+0x633139; spawn at its rsp+0x70, aim at
  rsp+0x60), then `v = speed * (cos a * dir + sin a * up)`, dir = normalize(aim - spawn), decl `m_throwAngle` +0x298 /
  `m_throwSpeed` +0x29C, or `m_lowThrowAngle` +0x2B8 / `m_lowThrowSpeed` +0x2BC when this+0x17C (looking down:
  `m_lowThrowRequiredCameraPitch` +0x2B0). A random fumble scales v by 0.1. Then **exe+0x63333C: `call exe+0x87C2A0`
  (entity+0x33C = v)** -- the projectile's ONLY initial velocity (bytes before it: `lea rdx,[rsp+0x50] ; mov rcx,r14`;
  spawn point at vel+0x20) -- angular velocity exe+0x87C280 (`m_throwAngularSpeed` +0x2A0, camera space if +0x2AC),
  and the launch exe+0x639970 with speed 1.0. Holding the button only COOKS (`m_cookingStages` +0x258).
- Measured: 17.2 m/s, 6 deg up, spawned 0.46 m ahead, launch 0.30 s after the release; flight has no drag. A grenade at
  your feet kills you even in god mode.

### 7.7 Placing mines and spring razors
Not throwables: item component `arkCpntWeaponPlaceable`, states `PlaceWeapon` / `PlaceWeaponReady`.
`arkCpntPlayerSensorPlacement::UpdateQueryPhysics` exe+0x3C1E40(this): one ray from exe+0x417820(sensor+0x40) along the
aim component's world dir (sensor+0x48, +0x2C), range sensor+0x58 (short: ~1.6 m for a mine, a razor only pointing
nearly straight down; a ray that lands on a placed mine places nothing).

### 7.8 Powers aiming
- Powers do NOT go through GetAimPoint/ComputeAimTarget. They aim with **`m_aimOrigin/m_aimAxis`** (cam+0x2527C /
  +0x25288).
- **Far Reach**: `arkCpntPlayerSensorGrapple` computes the landing. Its `UpdateQueryPhysics` exe+0x3B6C90 (dispatched
  from the sensor loop exe+0x954870) copies cam+0x2527C/+0x25288 into its ray (exe+0x3B6E98..0x3B6F69); the landing
  computation is the 2-argument wrapper exe+0x3B2280 (sensor, const idVec3* from). **Hook the wrapper, never the worker
  exe+0x3B4610** (six arguments, two on the stack: a register-only thunk crashed at exe+0x3B2951). The power component
  `arkCpntPowerGrapple`: main update exe+0x442850, targeting pass exe+0x4439F0 (copies the sensor's result +0x1CC.. into
  +0xF0), marker/FX update exe+0x4430D0 (asks its hand joint through exe+0x53DAC0, named joint at cpnt+0x194, into
  **+0x1B4 = the tendril's start**; +0x108 (copied to +0x270) = the END; +0x1C0 = the curve's middle control point;
  +0xA4 = targeting active). The FX update is dispatched once per active power component from exe+0x9534D0.
- Other sensors reading the camera: throw preview `arkCpntPlayerSensorThrowHit::UpdateQueryPhysics` exe+0x3CEC30 (reads
  player+0x300/+0x30C for the launch), search target `arkCpntPlayerSensorSearchTarget::UpdateQueryPhysics` exe+0x3C60E0.
  The sensor dispatch list is around exe+0x954900.
- **Effects anchored to an arm joint** (Far Reach's tendril and hand cloud, Blink's smoke, every power's aiming glow) are
  placed from the engine's skeleton through the joint queries of 6.6: an arm posed only on the GPU leaves them at the
  animated hand.

---

## 8. Interaction, held objects, corpses

### 8.0 Physics queries and impulses (Havok)

- **Synchronous ray cast**: `hknpWorld::castRay` exe+0xC89B00(world = *(game+0x60410), query, collector), as the
  melee line-of-sight check exe+0x636630 and the gun's trace exe+0x638570 build it. Query (0x70 bytes): +0x00 the
  world's shape tag codec (world+0x9D8), +0x08 the filter, +0x10 u16 material 0xFFFF, +0x14 u32 0, +0x18 u64 0, +0x20
  u32 2, +0x30 origin (w 0), +0x40 direction = to - from (w 1.0), +0x50 1/direction (0x7F7FFFEE where 0; w =
  0x3F000000 | bit k set when direction.k >= 0), +0x60 u32 0.
- **The game's query filter** (vtable exe+0x1AD4B68; +0x08 u64 0, +0x10 u32 0x1FFFF, +0x18 u8 4): +0x20 = the LAYERS
  mask, +0x24 = required flags. A body passes when bit (info & 0x1F) of the layers is set and ((info >> 5) & flags) ==
  flags (exe+0x996A0 / 0x996D0). The sword's casts (exe+0x637160, deferred through exe+0x95370 / 0x95130, read by
  exe+0x92630): "MeleeWeaponWorld" layers comp+0x3420 & 0x27, "MeleeWeaponEnemies" & 0x4480, "MeleeWeaponWorldRay"
  & 0xFFFFBB7F, all flags 0x100000. The gun's trace: layers 0x2F (global exe+0x1B989F8), flags = its mask argument.
  Cleanup: the filter's vtable set to exe+0x1AD40F8 and exe+0xA9E0D0(filter).
- **Collectors**: closest hit vtable exe+0x1AD5010 (0xA0 bytes: +0x0C hits, +0x20 early-out vector 0x5F7FFFF0 x4, the
  result at +0x30); all hits vtable exe+0x1AD2640 (+0x0C hits, +0x20 early-out, +0x30 an hkArray of results with
  inline storage at +0x50, capacity | 0x80000000 while inline; a grown array is freed through the allocator exe+0xA9DE90
  vtable +0x20; sort exe+0xC85470; cleanup vtable exe+0x1AD2610 + exe+0xA9E0B0).
- **A result** (hknpCollisionResult, 0x70): +0x00 point, +0x10 normal, +0x20 fraction along the direction, +0x28 the
  query body's info, +0x48 the hit body's id (& 0xFFFFFF), +0x4C its material, +0x54 its shape's filter info, +0x60
  its user data -> +0x08 -> +0x10 = the entity.
- **Entity messages**: exe+0x8112C0(queue, type, payload size, entity id) -> payload, the queue being exe+0x810240()
  (= game+0x1E8968) + 0xC8E8. Type 0xFA = an impulse {impulse xyz, world point xyz}: Windblast posts it (exe+0x4D7750,
  at the entity's bounds centre exe+0x878230, above the cvar `ark_windblast_minImpulseForPhysicEntities`) and so does
  the damage knockback (exe+0x7B2D19, impulse = direction x the damage decl's knockback), only to entities with
  entity+0xD1 bit 3. 0xE6 / 0xE7 are the melee block / hit events (exe+0x6357D0, exe+0x62CB80).
- **entity+0xD1 bit 3 is "being simulated", not "can move"**: set by exe+0x878D70, cleared by exe+0x879B20 once
  the body settles, so a prop at rest does not have it. Windblast does not test it: it asks the entity's
  arkCpntPhysicsBase (type exe+0x39E85B0) through its vtable +0x178 (exe+0x76A8F0: a body with motion type 2, dynamic).
  The 0xFA handler of that component (exe+0x809C90) applies a linear impulse when the point is (0,0,0), else a point
  impulse at the world point (exe+0x8519D0), which wakes the body.
- The entity velocity at +0x33C is a plain field (exe+0x87C2A0 only writes it): no use for pushing a body.
- **Message ids** are the ARK_MSG_* enum (names table at exe+0x2381F98, 0x18-byte entries, value = index - 1:
  ARK_MSG_NONE = -1). 0xE6 ATTACK_BLOCKED, 0xFA IMPULSE, 0x138 DOOR_PUSH, 0x149 VELOCITY_IMPULSE (payload 0x1C,
  ten senders, not yet decoded: a mass-independent push).
- **Items**: arkCpntLoot, type exe+0x39C5880 (registered exe+0x13BB850); arkCpntMissionItem, type exe+0x39AE970
  (exe+0x13A0430); also arkCpntPickableHolder, arkCpntInventory, arkCpntBoneCharmInventory. Taken by the player's interact.
- **Collision layers** (names from arkCollisionSettings' m_collisionLayers strings, exe+0x1D2F1C8..): 0 WORLD_LOW,
  1 WORLD_LOW_HIGH, 2 WORLD_HIGH, 3 RIGID_BODY, 4 DEBRIS, 5 MOVER, 6 PLAYER, 7 PLAYER_HITBOX, 8 PLAYER_DEAD, 9 NPC,
  10 NPC_BODY_ANIMATED, 11 PROJECTILE, 12 CLOTH, 13 CLOTH_COLLIDER, 14 NPC_BODY_RAGDOLL, 15 INTERACTABLE, 16 TRIGGER,
  17 VISION, 18 PLAYER_PHYSICS, 19 DECAL_QUERY, 20 WEAPON_MELEE, 21 CHAIN, 22 TRIGGER_WATER, 23 PICKUP, 24 (unnamed),
  25 GHOST, 26 HIDDEN.
- **Multi-stage usables** (safe dials, levers): arkCpntUsableWithStateList, type exe+0x39BA8B0 (registered
  exe+0x13AE680). Decl (+0x08): stages at +0x70 (0x80 bytes each), count +0x78, start +0x80. +0x28 = current stage,
  +0x30 bit 1 = transitioning. exe+0x77AEC0 = next (wraps; a click), exe+0x77B740 = previous (wraps), exe+0x77B920(u,
  stage, instant) = set; all through exe+0x77A880(u, stage, animate, 0), then entity+0xD4 |= 4. The Kiscule node
  ActionUsable (exe+0x797750 next, 0x797930 previous, 0x797C40 set stage, 0x797D30 enable) calls them.
- **Entity names**: the level's name of an entity is a char* at entity+0x20 (exe+0x1D9090 reads it for the game's own
  error messages, e.g. exe+0x762D41, 0x7C598E).
- **Closing an openable**: the scripts' Close (exe+0x793570) = if openable+0x1E8, exe+0x74E4C0(openable, false, true).
  Doors keep their open state there too (arkCpntOpenable's members include arkDoorDefaultState_t m_defaultState).
  Measured: +0x1E8 stays 0 on a door ARK_MSG_DOOR_PUSH swung open (every pushed door read "closed"), so the close
  no-ops on it. The mod reads the state machine instead (below) and closes a door with the player's own interact,
  aimed at the point the hand pushed (the interaction sensor locked on that point, then F).
- **Hand casts and props**: many props (fruit, cups, bottles) do not carry the melee sweeps' flag 0x100000 and were
  missed by a cast that requires it; a second cast on layers 3 / 4 (DEBRIS) / 5 / 15 / 23 with no flag finds them.
- **Finishing moves (generic fatality)**: arkPlayerStateSlash's method exe+0x86B050 (from vtable slot exe+0x86C630 and
  from exe+0x86BEC0) decides whether a blow becomes a synchronised kill: target not physics-driven (+0xD1 & 8), the
  attack outcome (+0x20 of the sensor's outcome: 3 rolls `rand % 100 < decl+0xEC`), a staggered target (exe+0x5BF440)
  or one on the ground (+0xD1 & 0x20), distance < decl+0xE8 when there is no outcome, the target's component
  exe+0x39E80A0 with decl +0xCC bit 0. All of it ends in one store, exe+0x86B381 `mov byte [rdi+0xD9], 1` (the rules'
  "slash.isGenericFatality"); every refusal continues at exe+0x86B38A. arkDeclPlayerStateSlash (0xF0 bytes, ctor
  exe+0x86A7C8): m_genericFatalityMaxDistance float +0xE8 (2.0), m_genericFatalityChance int +0xEC (10). The kill cam:
  cvar ark_killcam_chance_setting (default 1).
- **Takedown outcomes**: exe+0x3AFFC0(assassination sensor) returns the current offer's record (sensor+0x1D0 -> a
  component of type exe+0x39F0F50); its +0x20 is arkAssassinationOutcomeType_t (enum desc at exe+0x2398100: 0 NORMAL,
  1 FAST, 2 FATALITY_NORMAL, 3 FATALITY_FAST, 4 FATALITY_CHAIN, 5 FATALITY_GROUND, 6 FATALITY_GROUND_ON_RAGDOLL, 7 WINDOW,
  8 DROP, 9 SHADOWWALK, 10 SLIDE, 11 GRAPPLE). Its callers: the Slash (exe+0x86B0F0) and exe+0x3911C9. Measured: in a
  fight the sensor offers outcomes on standing guards (the kill animations), and on a knocked-out one.
- **Pickups**: arkPickup entities (notes, pouches, fruit, bathing salts) carry arkCpntPickable_Simple. The loot decl's
  m_itemDefs (decl+0x68) are arkDeclLootinfo {idDeclEntityDef* m_itemEntity @0, idAtomicString m_bindTagName @8} --
  entity defs, not inventory item decls. Interact on a pickup first undraws the weapons (~0.13 s) and loots after it,
  re-reading the sensor then.
- **Cabinet fronts**: some are plain arkUsable entities ('esc_env_door_middle_5'), no openable component.
- **Characters**: class ids between the globals exe+0x39CE2C0 / +4. An unconscious guard carries an arkCpntLoot (his
  pockets), so "has loot" alone takes him for an item.
- **Loot lists**: arkCpntLoot's own m_items (+0x28) is empty on items lying in the world; what they give is the loot
  decl's (loot+0x08, arkDeclCpntLoot) m_itemDefs, an idList at decl+0x68 of 0x10-byte arkDeclLootinfo (which of the
  first two fields is the item decl is not confirmed: the mod tries both, validated by RTTI).
- **Window names**: a sliding window's sash / handle has "window" in its level name ('ld_rich_small_gp_window_handle_1',
  layers 3 / 15); so does a glazed door ('ld_door_big_rich_int_window_350_0_4'), so names with "door" are excluded.
- **Openables** (drawers, cabinets, some doors): arkCpntOpenable, type exe+0x39C59A0 (registered exe+0x13BB9B0, GetType
  exe+0x8EB600, vtable exe+0x1C569C8); +0x1E8 = open; a state machine at +0x40 (current at +0x48: Closed / Animated /
  Opened, arkStateOpenable_*). The state objects are embedded: Closed at +0x60, Animated at +0x80 (+0x28 = phase:
  0 OPENED, 1 OPENING, 2 CLOSING, 3 CLOSED), Opened at +0xB8; told by their vtables exe+0x1C56908 / 0x1C56948 /
  0x1C56988 (the mod scans +0x40..+0x58 for a pointer to one: unverified in game). exe+0x74E4C0(openable, open, animate) sets it (sends the opened / closed event
  exe+0x39ACAC0 / 0x39ACB10 to the entity, starts the animation) without any lock check: the scripts' Open / Close
  (exe+0x7933D0 / 0x793570, a Kiscule node registered at exe+0x7932B0) call it; +0xD8 is a scripted state (0 / 1 / 2).
- **Doors**: arkDoor (RTTI vtable exe+0x1CB7C68) is told apart by its class id range at exe+0x39C0350 / +4 (tested by
  Windblast exe+0x4D89D5, and at exe+0x38FC51, 0x3B29E9 (interaction), 0x3CFF28, 0x4C7DA9, 0x5F9FBA, 0x6B7B07,
  0x801102, 0x86E91F, 0x8DC9A8). Windblast opens a door with exe+0x4D7E80: ARK_MSG_DOOR_PUSH {pusher's entity id, or
  0x3FFF0010}. Door states: ARK_DOOR_DEFAULT_OPENED / OPENED_CCW / CLOSED; phases OPENED / OPENING / CLOSING / CLOSED.

### 8.1 The interaction sensor (two phases, asynchronous)
`arkCpntPlayerSensorInteraction` ([GrabHand.cpp](../src/Src/GrabHand.cpp)):
- **SUBMIT exe+0x3B8D60** (once a frame from the sensor dispatch, exe+0x95491E): for slot i = 0..4, only if the slot's
  query handle (sensor+0x70+4i) is -1 and its shape (sensor+0x48+8i) exists, posts an ASYNCHRONOUS sphere cast
  (exe+0x95280 on the physics world +0x60280) of radius decl+0x74+4i from aimOrigin + aimAxis*radius to aimOrigin +
  aimAxis*decl+0x70. Radii **0.005 / 0.05 / 0.1 / 0.3 / 0.7 m**, reach **3 m**; filter mask 0x20CC38 for slots 0-2, 0x408
  for 3-4. Shapes made at init (exe+0x3B82C0, which also sets sensor+0x40 = the player's camera component; decl =
  sensor+0x8). Calling it again in the same frame posts nothing.
- **RESULTS exe+0x3B8430** (from exe+0x95000E): clears sensor+0x84/+0x88/+0x8C to 0x3FFF0010, reads the aim fields NOW,
  casts a SYNCHRONOUS ray (exe+0x6C6760, length decl+0x70) and takes an interactable it hits (component in
  sensor+0x98/+0xA8/+0xB0); then per slot, thinnest first, fetches the finished cast (exe+0x92630), walks hits by
  distance and takes the first that passes **exe+0x3B80A0** (the interactable's rules: hit within its decl+8 of the EYE
  cam+0xD8; |eye z - AABB centre z| within its decl+0xC; an optional facing test) and **exe+0x3B7B90** (line of sight:
  the slot's shape swept from the aim origin to the hit, mask 0xC43E, the object and its owner excluded). Outputs:
  **+0x84 USE target** (direct ray or slots 0-2; getter exe+0x3B8040), **+0x88 CARRY target** (pickable class range
  exe+0x39CE2C0..C4, slots 0-3; getter exe+0x3B8080), +0x8C the same incl. slot 4.
- **Never call RESULTS twice in a frame**: the first call retires the slots' query ids; the second asks exe+0x92630 for
  them again -> "Collision query accessed with outdated or corrupted id" (exe+0x92B2E -> exe+0x166830), a FATAL error:
  the game quits with no exception and no WER event.
- Both phases read the camera through sensor+0x40: aim them by swapping that pointer for a copy of the camera
  (0x252B0 bytes) with other aim fields, instead of writing the real camera.

### 8.2 HoldObject (bottles, glasses)
- `F` "carry" -> `arkPlayerStateHoldObject` (vtable exe+0x1C7A998); the object is an `arkMovable` (vtable exe+0x1C5DE18)
  with `arkCpntPickable_Simple` and `arkCpntPhysicsBase`. The state object holds the object's spawn id at **+0x68**; its
  tick (slot 13, exe+0x83E5D0) only checks the pickable component is still there.
- Enter (exe+0x83D770, reached from slot exe+0x83C460): stores the id, gets the component of typeinfo 0x39B4630 and calls
  **exe+0x731470(component, out)** = the mean WORLD position of its physics bodies (one exe+0x53D940 each), binds the
  object (exe+0x875E80); if the object's decl has +0xA8 set, hides the LEFT arm and sends event 0x40; always sends
  event 0x35 and hides the RIGHT arm. The right hand is UNEQUIPPED for the hold (its item empties, the sword entity is
  destroyed and rebuilt on release); the left item stays usable.
- The object is held by an **entity bind (exe+0x7506D0) to joint 8 of the player's body model** (origin at the feet) with
  local offset **(0.55, -0.30, -0.253)** and a tilt: rigid with the camera. Two queries a frame carry that offset through
  **exe+0x53D3F0**: one inside the bind (space 2, from exe+0x53DED2 <- exe+0x750748: what the physics body follows) and
  one placing the render model (space 3, from exe+0x808962 in exe+0x808880, which writes model+0x1860/+0x186C).
  Answering both with another pose moves the real body there, collisions included (it is a keyframed body: its
  velocity spikes to ~17 m/s for one frame when the head turns).
- **Throw of a held object** (LMB): exe+0x83DE90 (one caller, exe+0x845262): teleports the object (exe+0x87D1E0), builds
  the velocity towards the crosshair point (exe+0x83D460 = eye + view axis * a pitch curve) plus the player's own, scales
  by the pickable decl (+0xC0), posts it in event 0x149. The leaf `call exe+0x810240` at **exe+0x83E473** runs with that
  final velocity in `[rbp-0x50]` (x, y) and `r12d` (z), entity in rdi. A thrown object is a projectile at ANY speed (a
  bottle let go at zero velocity this way shattered on the one below). The drop (F) does not break it, but F goes to
  whatever interactable the sensor has picked meanwhile.

### 8.3 Carrying corpses
- `CarryCorpse` is NOT a HoldObject: the game leaves both arms visible and carries the body on the shoulder, stuck to
  the camera. Taking a body is a LONG press of F (an 80 ms tap fills a quarter of the bar); with a body on the
  shoulder F = drop, and F with no interactable picked also drops. To open a door while carrying, the sensor must be
  aimed at it.
- Hiding the arm with the game's own Hide (vtable +0x80) works: entity flags 0x8B hidden, 0x88 shown.

---

## 9. Interface, menus, videos, HUD

### 9.1 How the interface is drawn
- Straight into the BACKBUFFER from a deferred context, after a full-screen copy of the scene (no blend): ~38-45 quads a
  frame in play (the power wheel ~420, with a full-screen veil of alpha ~0.7), premultiplied alpha
  (`ONE, INV_SRC_ALPHA`), arcs clipped with STENCIL (draws with no pixel shader that only write stencil). Scaleform
  vertex shader DXBC `7ACC1A9A27C4D9E2`.
- The backbuffer is TYPELESS and the interface alternates UNORM (28) and sRGB (29) views draw by draw; through the UNORM
  view it blends in gamma space. Repeating its draws into another target needs the SAME view type per draw.
- **Layout**: menus (title, pause, load, options, journal) lay everything out in the centred 16:9 band (height W*9/16
  in a taller frame). The HUD, the power wheel (potions at the top edge, quick slots at the bottom) and the black market
  use the whole frame (the shop's item carousel sits on the bottom edge). HUD health/mana ring: in a centred 16:9 frame
  of height Hs = min(H, W*9/16), centre x = frame left + 0.1465*Hs, y = 0.133*Hs + 0.0327*(H - Hs) (< 1 px at five
  resolutions).

### 9.2 Menus and keys
- In menus the game simulates nothing (`sim 0`) and draws no first-person arms; a menu's controls are keys: arrows,
  ENTER, ESC, TAB = next tab (wraps; there is NO previous: SHIFT+TAB also advances), PGUP/PGDN = journal sub-tabs.
- Quick slots: `WEAP_1..0` equip a slot; with the wheel open they ASSIGN the highlighted item, after which the wheel
  stays open (releasing MOUSE3 no longer closes it; another MOUSE3 click closes it and equips). Potions with the wheel
  open: R / T. A held movement key keeps the game from closing the wheel.
- Loading a save creates autosaves (two per save space, rotating) and the load list opens on the last space loaded.
- **Menus over a running world**: the new game's **character choice** is the player state `CharacterChoice` (the game
  simulates and draws arms): two fixed third-person shots ~88 deg apart with a dutch angle (roll +12.9 / -10.5 deg), game fov
  22.4 x 23.1 deg; A/D or LEFT/RIGHT switch character, ENTER asks a yes/no dialog (that dialog stops the simulation and
  has NO back: ESC/BACKSPACE do nothing; ESC at the choice does nothing either). A **black market** is
  `BlackMarketInteract` (after a short `Cinematic` + `EquipChange`; `BlackMarketGoto` never ticked): arrows move, ENTER
  buys at once, TAB switches tabs, ESC goes back; the game draws its own mouse cursor there and mouse hover steals the
  selection.

### 9.3 Videos (Bink 2)
Mission videos are `base\video\*.bk2` played by `bink2w64.dll` (exports `BinkOpen`, `BinkOpenWithOptions`, `BinkClose`,
`BinkDoFrame`, `BinkDoFrameAsync`, `BinkNextFrame`; [BinkVideo.cpp](../src/Src/BinkVideo.cpp)). Seen: `legal.bk2`,
`warning.bk2` at start-up, a mission opening after the mission's "press a key". While one plays no player state ticks,
the game does not simulate; skip = INTERACT held (ENTER does nothing).

---

## 10. Inventory, powers, upgrades, the mark

### 10.1 The inventory
- `arkCpntInventory` (typeinfo exe+0x39DCD30) keeps a table of EVERY item the loadout knows, owned or not: +0x38 base,
  +0x40 count (low 24 bits; 816 on Corvo), entries of 0x38 bytes whose first qword is the decl
  (`arkDeclEquipmentUpgrade` or `arkDeclInventoryItem`, name at decl+0x8; names can exceed 80 characters); entry dword
  +0x24 = the BASE capacity. Hands: +0x70 left item, +0x68 right item; `item+0` = its decl, `item+8` = entity id,
  `item+0x10` = the item's component (class tells pistol / crossbow / grenade / mine / Heart apart: see 6.3).
- **The equipment list**: `arkCpntInventory+0x58` (count low 24 bits of +0x60) holds pointers to the entries that have an
  equipment entity (what exe+0x614DD0(inv, type, hand) walks to find "the sword": type 1, hand 0). An entry: +0x00 the
  decl, +0x08 the equipment entity's id, +0x10 the item component and +0x18 the equipment decl (both valid only while
  that entity exists: exe+0x63C890 / exe+0x63C8C0 return them after GetEntityById), +0x20 the quantity. The equipment
  decl: **+0x68 the slot** (0 right hand = inv+0x68, 1 left hand = inv+0x70, ...; the slots are inv+0x68+8*slot),
  +0x11C the item type (1 = the sword).
- **The player's equip messages** (handlers reached through thunks at exe+0x7FBE10 / 0x7FBE20): 0x9D -> exe+0x7F8D60:
  finds the item's entry, then flags bit 0 = only into an empty hand, bit 1 = instant (EQUIPCHANGE_INSTANT_EQUIP |
  INSTANT_UNEQUIP, else INSTANT_UNEQUIP only), **bit 2 = EQUIPCHANGE_THIS_HAND_ONLY** -- without it the change
  re-equips BOTH hands (measured from the headset: drawing the pistol brought the sword back); requests player state
  0x40 (left hand, slot 1) or 0x35 (right) with arkPlayerStateEquipChange params (exe+0x835590). 0x9E -> exe+0x7F9140:
  with m_decl null, m_slot 1 = the left hand (state 0x40), 4 a special slot, 0x10 nothing, anything else the right
  (0x35). The params' flags (enum desc at exe+0x23A01A8): LOCK 1, UNLOCK 2, REEQUIP 4, REEQUIP_BOTH_HANDS 8,
  INSTANT_EQUIP 16, INSTANT_UNEQUIP 32, FAST_EQUIP 64, FAST_UNEQUIP 128, THIS_HAND_ONLY 256 (the state's enter,
  exe+0x8363F0, reads them at params+0x18).
- **Equipping**: ARK_MSG_INVENTORY_EQUIP_ITEM (0x9D) to the owner, payload 0x10 {arkDeclInventoryItem*, flags (3 bits
  used)}. Senders: the script node ActionPlayerInventory's Equip (exe+0x534760: the sword with flags (x & ~2) | 5, then
  with `m_bothHands` its `m_leftHandEquipment` with the three bits clear) and the inventory's own update (exe+0x619B93:
  bit 2 set, bit 1 = inv+0x190 bit 1), for the requests queued at inv+0x128 (exe+0x6143C0 queues one: what the AddItem
  node's `m_autoEquip` does). Unequip: 0x9E
  (arkMsgInventoryUnequipItem: m_decl @0, arkEquipmentSlot_t m_slot @8, bool m_instant @0xC); the node sends m_decl 0
  with m_slot 1 and, for both hands, also 0. The enum arkEquipmentSlot_t reads EQUIP_NONE 0, EQUIP_LEFT_HAND 1,
  EQUIP_RIGHT_HAND 2, EQUIP_HEAD 3... while the inventory numbers its hand slots 0 right, 1 left: which numbering m_slot
  takes for the right hand is not confirmed (Holster tries 0, then 2). The player's equip change ends in
  exe+0x835E00 (unequips, exe+0x614B20 into the slot, then the script events equipLeftHandItem / equipRightHandItem);
  the path from the message to it is not traced. Done: ARK_MSG_INVENTORY_ITEM_EQUIPPED 0x9F {decl, equipment decl}.
- **`AddItem(inv, arkDeclInventoryItem*, int count)` exe+0x6156E0**: creates the power component for a power (the wheel
  and quick slots pick it up by themselves); adds only what fits (capacity includes storage upgrades: bolts and bullets
  20, grenades 4, mines 3, spring razors 5, elixirs 10) and returns how many. Setting a count by hand would not create
  the component. `GetQuantity(inv, decl)` exe+0x615310.
- **Infinite ammo is the game's own**: `arkCpntInventory+0x168` is an int counter (> 0 = on), incremented / decremented
  by the script node `arkKisculeNodeInfiniteAmmo` (pins exe+0x7A0BC0 / exe+0x7A0B80). Covers ammo, gadgets and elixirs.
- Reading the item table is slow if every byte is probed (816 decls, ~1.4 s per pass on the main thread): read it once
  per table (it moves with each load), one `VirtualQuery` per region.
- Item names: `models/powers/<x>/<x>.inventoryitem` for powers (blink, possession, bendtime, windblast, darkvision,
  devouring_swarm, grapple_corvo = Far Reach, doppelganger_corvo, mesmerize, shadow_walk, phantasmal_link = Domino);
  character-specific items end in `_corvo` / `_emily` (`crossbow_corvo`, `spyglass_corvo`); `gun_player` and
  `heart/heart` are shared.

### 10.2 Mana
`arkCpntMana` (typeinfo exe+0x39D13B0): current mana float **+0x20**; **+0x2C is the level mana regenerates to, and
each cast lowers it** (100 -> 30); a loaded save can bring it lowered already. +0x28 holds 5 after casts (unknown).
Blink costs 20.

### 10.3 The Outsider's mark
Found from the Kiscule "Give Mark" node (`arkKisculeNodePlayerOutsiderMarkControl`, vtable exe+0x1CD4238, pin callback
exe+0x642330): **`GiveMark(markCpnt)` exe+0x4C1E50** calls AddItem(..., 1) for each item of the mark decl's list
(+0x68, count +0x70: on Corvo the mark and Blink). `HasMark(markCpnt)` exe+0x4C1F20. Component typeinfo
`arkCpntPlayerOutsiderMark` exe+0x39D15F0.

### 10.4 Power upgrades and enhancements
- `arkCpntPlayerPowerSystem::AddPowerUpgrade(ps, id)` exe+0x455AB0 looks the id up in an inline array of the power
  system's decl (`ps+0x8` -> **+0x88**, 16-byte `{int id; decl*}`) in the order of the `arkPowerUpgradeId` enum desc at
  **`*(exe+0x32CDEA8)+0x10`** (`{char* name; int64 value}`, ended by an empty name), hands that decl to AddItem, then
  spends its rune cost (decl+0xC8, exe+0x614750). 39 ids, 34 with a decl. AddItem alone works (human possession needs
  its upgrade).
- Enhancements (Vitality, Agility, Strength, Reflexes, Bloodthirsty, Shadow Kill, bone charm crafting):
  `AddEnhancementUpgrade` exe+0x455660, array at decl+**0x2F8**, enum desc **`*(exe+0x32CDEB8)+0x10`** (24 with a decl).
  AddItem alone works.

### 10.5 Equipment upgrades (the black market's)
`AddEquipmentUpgrade(ps, index, variant)` **exe+0x4557A0** (found from its assert string, referenced at exe+0x455801).
The list is on the component: `ps+0x70` idList of 0x28-byte instances, count low 24 bits of `ps+0x78` (57 on Corvo).
Instance: +0x00 node index, +0x08 info*, +0x10/+0x18 children of a category node, +0x20 bool owned, +0x24 int variant.
info+0 = the item; info+0x30 = an alternative item used when byte info+0x38 is set and variant != 1 (the either/or
pairs: gun_snipe / gun_shotgun_corvo, armor / camouflage_corvo, ...). Category nodes have no item; some leaves are base
ammo / gadget items (giving them gives one). Callers: `arkCpntBlackMarketUpgrade` (exe+0x4993D3, shop entry +0x124 =
index<<2 | variant) and a loop at exe+0x514B34. exe+0x456AA0(ps, decl) = index of a decl in that list.

---

## 11. Consolidated address table

Every address below is a constant (or an inline offset) in the mod's source; the file named is where it is defined or
used. Offsets inside objects follow in 11.2.

### 11.1 Code, data, vtables, typeinfos (`exe+RVA`)

| RVA | what | source |
|---|---|---|
| **Globals and data** | | |
| 0x39A5290 | `gameLocal` / "game" pointer (entity table, body table); NULL during a level change | [Game.h](../src/Src/Game.h), [BodyHide.h](../src/Src/BodyHide.h), [MeleeAim.cpp](../src/Src/MeleeAim.cpp), [NpcSkel.cpp](../src/Src/NpcSkel.cpp), [SwordContact.cpp](../src/Src/SwordContact.cpp), [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x228BAE0 | `idCmdSystem**` | [Game.h](../src/Src/Game.h) |
| 0x32FAF80 | engine window size (int w, int h at +0x84 ...) | [RenderRes.cpp](../src/Src/RenderRes.cpp) |
| 0x32CDEA8 | `arkPowerUpgradeId` enum desc pointer | [TestAids.h](../src/Src/TestAids.h) |
| 0x32CDEB8 | enhancement enum desc pointer | [TestAids.h](../src/Src/TestAids.h) |
| 0x1B9D6F0 | float: melee dwell threshold (0.09 s) | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| **Game, player, commands** | | |
| 0x8A8830 | `GetLocalPlayer(gameLocal)` | [Game.h](../src/Src/Game.h) |
| 0x8861C0 / 0x886D10 | `Cmd_GetViewpos_f` / `Cmd_SetViewpos_f` | [Game.h](../src/Src/Game.h) |
| 0x6DC560 | `GetEntityById(int)` (faults with no world) | [BodyHide.h](../src/Src/BodyHide.h) |
| 0x877720 | `GetComponent(entity, typeinfo*, flags)` | [BodyHide.h](../src/Src/BodyHide.h), [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x17D10 | physics body record -> owner | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| 0x8780D0 | entity world AABB (entity, float[6]) | [GrabHand.cpp](../src/Src/GrabHand.cpp) |
| 0x753C10 | transform component update (writes entity +0x300/+0x30C/+0x330) | [WeaponPose.h](../src/Src/WeaponPose.h) |
| 0x9D560 | `arkTransformSet::SetOriginAndAxis` | [WeaponPose.h](../src/Src/WeaponPose.h) |
| 0x87C2A0 | set entity velocity (entity+0x33C) | [ThrowGesture.cpp](../src/Src/ThrowGesture.cpp) |
| **Rendering** | | |
| 0x109CC0 | `idRenderMatrix::CreateViewMatrix(org, axis, out)` | [ViewMatrixHook.h](../src/Src/ViewMatrixHook.h) |
| 0x3742B0 | projection builder (fov, jitter) | [CameraHook.h](../src/Src/CameraHook.h) |
| 0x3586A0 | view constants upload (identifies the main view; moves nothing) | [CameraHook.h](../src/Src/CameraHook.h) |
| 0x881640 | renderView builder for other entities (not the player) | [PlayerViewHook.h](../src/Src/PlayerViewHook.h) |
| 0x237340 | render model bounding sphere from its AABB (culling) | [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0xE9FF1 | return address: WndProc `WM_WINDOWPOSCHANGED` -> `GetClientRect` | [RenderRes.cpp](../src/Src/RenderRes.cpp) |
| 0x346364 | return address: `GetHdcClientWindowSize` -> `GetClientRect` | [RenderRes.cpp](../src/Src/RenderRes.cpp) |
| 0xE27DF | return address: interface cursor (`GetCursorPos` + `ScreenToClient`) | [RenderRes.cpp](../src/Src/RenderRes.cpp) |
| 0x41770 | `WriteConfigToFile(common, name, int, mask, bool)` (5 args) | [RenderRes.cpp](../src/Src/RenderRes.cpp) |
| 0x347630 | window placement (`SetWindowPos` from `r_windowWidth/Height`) | [RenderRes.cpp](../src/Src/RenderRes.cpp) |
| **Camera and aim** | | |
| 0x7F38E0 | camera component update (at load) | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x7F4B30 | `UpdateAim` (m_aimAxis / m_aimOrigin) | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x3C7CD0 | `GetAimOriginAndAxis` | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x726280 | aim component per-frame update | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x88C5E0 | camera shake/bob: last writer of cam+0xD8/+0xFC | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x7F2330 | camera: copies its renderView origin/axis to +0x25464/+0x25440 | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x444C0 | `renderView_t` copy (dst, src) | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x8067C0 | `GetViewAxis(cam)` = cam+0xFC | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| 0x417820 | `GetViewOrigin(cam)` = cam+0xD8 | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| **Ranged weapons** | | |
| 0x44EBD0 | `arkCpntWeaponGun::Fire` | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x44E0C0 | the gun's shoot (FX, blast, Fire) | [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x44E2A0 | gun slot 3: `m_projectileSpawnPos` from the muzzle tag | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x44F030 | the pistol's blast (`m_spreadArea` explosion) | [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x3C3CC0 | `GetAimPoint(provider, out, dist)` | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x638AE0 | `arkCpntWeaponRanged::ComputeAimTarget` | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x639F00 | ranged component init | [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x639670 | weapon aim point (GetAimPoint of provider #9, decl+0x198) | [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x638800 | sweep of the projectile shape (static world) | [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x638570 | synchronous ray (static world) | [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x639970 | projectile launch | [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x63A8B0 | the fire's target lock (aim assist / sensor hit) | [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x3C3FD0 / 0x3C3E10 | `arkCpntPlayerSensorRangedTarget` SUBMIT / RESULTS | [GunRay.cpp](../src/Src/GunRay.cpp) |
| **Joints, binds, FX** | | |
| 0x53D3F0 | model-space joint query with offset | [HandFx.h](../src/Src/HandFx.h) |
| 0x53D6A0 | model-space joint query | [HandFx.h](../src/Src/HandFx.h) |
| 0x53D800 | world-space joint query (7 args) | [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x53DAC0 | world-space joint query by joint reference | [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x7506D0 | entity bind transform (7 args) | [HandFx.h](../src/Src/HandFx.h) |
| 0x4E2B00 | FX spawn into a slot | [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x19D100 | FX instance create | [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x1A2FF0 | FX instance start | [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x4E3090 | bound FX per-frame update | [GunRay.cpp](../src/Src/GunRay.cpp) |
| **Melee** | | |
| 0x60D7C9, 0x61B64F, 0x62EC41, 0x62F0A5, 0x62F30A, 0x62F755, 0x62F99A, 0x631792 | melee call sites of `GetViewAxis` | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| 0x60D7BE, 0x61B641, 0x62EBD5, 0x62F2EF, 0x62F97D, 0x6308E2 | melee call sites of `GetViewOrigin` | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| 0x62F900 | 30 deg cone test (comp, point, shape) | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| 0x634890 | attack window SetActive(comp, active, b1, b2, f) | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| 0x631020 | per-frame hit check | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| 0x63140A | return address: a hit closes the window | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| 0x84E272 | return address: the animation event opens the window | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| 0x634857 | return address: a strike on a character closes the window | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| 0x63136A | return address: cone test of a prop / world candidate | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| 0x635B00 | candidate is a valid damageable target | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| 0x62EB20 | veto while an enemy is in front | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| 0x636B40 | gather last frame's cast results, store owner position | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| 0x877EB0 | frame dt of an entity | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| 0x62F290 | melee vtable slot 42: attack start / box | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| 0x634990 | melee vtable slot 41: sweep segment | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| **Powers, sensors, throwing** | | |
| 0x4430D0 | `arkCpntPowerGrapple` marker/FX update (also UpdateTarget) | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x442850 | power component main update | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x3B6C90 | `arkCpntPlayerSensorGrapple::UpdateQueryPhysics` | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x3B2280 | grapple landing computation (2-arg wrapper) | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x3CEC30 | `arkCpntPlayerSensorThrowHit::UpdateQueryPhysics` | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x3C1E40 | `arkCpntPlayerSensorPlacement::UpdateQueryPhysics` | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x3C60E0 | `arkCpntPlayerSensorSearchTarget::UpdateQueryPhysics` | [HandAimHook.h](../src/Src/HandAimHook.h) |
| 0x63333C | call site in `Throw`: `call exe+0x87C2A0` with the grenade's velocity | [ThrowGesture.cpp](../src/Src/ThrowGesture.cpp) |
| **Interaction, holding** | | |
| 0x3B8D60 / 0x3B8430 | interaction sensor SUBMIT / RESULTS | [GrabHand.cpp](../src/Src/GrabHand.cpp) |
| 0x731470 | mean world position of a component's physics bodies | [GrabHand.cpp](../src/Src/GrabHand.cpp) |
| 0x83E473 | call site in the held object's throw (final velocity in registers) | [GrabHand.cpp](../src/Src/GrabHand.cpp) |
| 0x810240 | its callee (a leaf returning a global) | [GrabHand.cpp](../src/Src/GrabHand.cpp) |
| **Inventory, powers, cheats** | | |
| 0x4C1E50 / 0x4C1F20 | `GiveMark` / `HasMark` | [TestAids.h](../src/Src/TestAids.h) |
| 0x6156E0 | `arkCpntInventory::AddItem(inv, decl, count)` | [TestAids.h](../src/Src/TestAids.h) |
| 0x615310 | `arkCpntInventory::GetQuantity(inv, decl)` | [TestAids.h](../src/Src/TestAids.h) |
| 0x4557A0 | `AddEquipmentUpgrade(ps, index, variant)` | [TestAids.h](../src/Src/TestAids.h) |
| **Gamepad research (disabled)** | | |
| 0xE0220 | `arkXInputControllerState::Poll` | [PadProbe.cpp](../src/Src/PadProbe.cpp) |
| 0xE03D0 | PostAxisEvent | [PadProbe.cpp](../src/Src/PadProbe.cpp) |
| 0xE0A80 | GenerateEvents | [PadProbe.cpp](../src/Src/PadProbe.cpp) |
| **Vtables** | | |
| 0x1B0D628 | `arkSkinnedRenderModel` | [BodyHide.h](../src/Src/BodyHide.h) |
| 0x1CE1E98 | `arkWeapon` (equipment entity) | [BodyHide.h](../src/Src/BodyHide.h) |
| 0x1B99C68 | `arkCpntHeart` | [BodyHide.h](../src/Src/BodyHide.h) |
| 0x1B9D8B8 | `arkCpntWeaponThrowable` | [BodyHide.h](../src/Src/BodyHide.h) |
| 0x1B54538 | `arkCpntWeaponPlaceable` | [BodyHide.h](../src/Src/BodyHide.h) |
| 0x1B53F58 | `arkCpntWeaponGun` | [BodyHide.h](../src/Src/BodyHide.h), [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x1B9CBF8 | `arkCpntWeaponCrossbow` | [BodyHide.h](../src/Src/BodyHide.h), [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x1B9D508 | `arkCpntWeaponMeleePlayer` | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| 0x1C3AAA8 .. 0x1C8DA78 | the 91 `arkPlayerState*` vtables (names in the source) | [PlayerState.cpp](../src/Src/PlayerState.cpp) |
| **Component typeinfo records** | | |
| 0x39CB7A0 | player body component (arm entity ids) | [BodyHide.h](../src/Src/BodyHide.h) |
| 0x39DCD30 | `arkCpntInventory` (equipment) | [BodyHide.h](../src/Src/BodyHide.h) |
| 0x39D13B0 | `arkCpntMana` | [TestAids.h](../src/Src/TestAids.h) |
| 0x39E2BB0 | `arkCpntPlayerPowerSystem` | [TestAids.h](../src/Src/TestAids.h) |
| 0x39D15F0 | `arkCpntPlayerOutsiderMark` | [TestAids.h](../src/Src/TestAids.h) |
| 0x39EDB90 | `arkCpntAttributes` | [TestAids.h](../src/Src/TestAids.h) |
| 0x39BA160 | `arkCpntHealthEvolved` | [TestAids.cpp](../src/Src/TestAids.cpp) |
| 0x39E28E0 | `arkCpntGrenade` | [ThrowGesture.cpp](../src/Src/ThrowGesture.cpp) |
| 0x39BFBA0 | `arkCpntMine` | [ThrowGesture.cpp](../src/Src/ThrowGesture.cpp) |
| 0x39DC9D0 | component the bound FX asks its joint from | [GunRay.cpp](../src/Src/GunRay.cpp) |
| 0x39B4630 | component HoldObject takes the physics centre of | [GrabHand.cpp](../src/Src/GrabHand.cpp) |

Shader identifiers (DXBC checksums) used in code: Scaleform interface VS `7ACC1A9A27C4D9E2`
([WristHud.cpp](../src/Src/WristHud.cpp)); sword trail VS `364e76313eb849ec`, `3fea51d57d8fff32`, `412fe65bec01fc06`
([GpuTrace.cpp](../src/Src/GpuTrace.cpp)). Build identity: link time stamp `0x67A50CDE`, size 41,134,632
([GameInfo.h](../src/Src/GameInfo.h)).

### 11.2 Object offsets used by the mod

| object | offset | field | source |
|---|---|---|---|
| idCVar | +0x00 / +0x28 / +0x2C / +0x30 / +0x48 | idStr value / int / float / name / flags | [CVars.cpp](../src/Src/CVars.cpp) |
| game (`*0x39A5290`) | +0x3130B0 | entity table (0x4000 slots, by id >> 16) | [BodyHide.h](../src/Src/BodyHide.h), [NpcSkel.cpp](../src/Src/NpcSkel.cpp), [SwordContact.cpp](../src/Src/SwordContact.cpp), [GunRay.cpp](../src/Src/GunRay.cpp) |
| game | +0x60410 | physics body table | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| entity | +0xA8 | spawn id | [NpcSkel.cpp](../src/Src/NpcSkel.cpp), [SwordContact.cpp](../src/Src/SwordContact.cpp), [GunRay.cpp](../src/Src/GunRay.cpp) |
| entity | +0xD0 | flags (bit 0 hidden) | [GrabHand.cpp](../src/Src/GrabHand.cpp), [SwordContact.cpp](../src/Src/SwordContact.cpp) |
| entity | +0xF0 / +0x368 / +0x480 | render models | [BodyHide.h](../src/Src/BodyHide.h) |
| entity | +0x300 / +0x30C | world origin / axis | [BodyHide.h](../src/Src/BodyHide.h), [GrabHand.cpp](../src/Src/GrabHand.cpp), [GunRay.cpp](../src/Src/GunRay.cpp), [WeaponPose.h](../src/Src/WeaponPose.h), [ThrowGesture.cpp](../src/Src/ThrowGesture.cpp) |
| entity | +0x330 | scale | [GrabHand.cpp](../src/Src/GrabHand.cpp), [SwordContact.cpp](../src/Src/SwordContact.cpp) |
| entity | +0x33C | velocity | [GrabHand.cpp](../src/Src/GrabHand.cpp), [ThrowGesture.cpp](../src/Src/ThrowGesture.cpp) |
| entity | +0x360 | skeleton object | [NpcSkel.cpp](../src/Src/NpcSkel.cpp) |
| entity vtable | +0x80 / +0x88 | Hide / Show | [GrabHand.cpp](../src/Src/GrabHand.cpp) |
| render model vtable | +0x80 | local bounds | [GrabHand.cpp](../src/Src/GrabHand.cpp), [SwordContact.cpp](../src/Src/SwordContact.cpp) |
| render model | +0x1840 / +0x1850 | world AABB min / max | [GunRay.cpp](../src/Src/GunRay.cpp) |
| render model | +0x1860 / +0x186C | world origin / axis | [HandFx.h](../src/Src/HandFx.h), [NpcSkel.cpp](../src/Src/NpcSkel.cpp), [GunRay.cpp](../src/Src/GunRay.cpp) |
| render model | +0x1F40 / +0x1F48 / +0x2100 | visible mesh count / transform set / visibility masks | [BodyHide.h](../src/Src/BodyHide.h) |
| render model | +0x1FE8 -> +0x50 -> +0x78 | skeleton definition, joint count (u16) | [NpcSkel.cpp](../src/Src/NpcSkel.cpp) |
| skeleton object | +0x38 / +0x2D8 / +0x2E8 | render model / pose array / remaps per space | [HandFx.h](../src/Src/HandFx.h), [NpcSkel.cpp](../src/Src/NpcSkel.cpp) |
| transform component | +0x10 / +0x30 / +0x3C / +0xD8 | entity / local origin / local axis / parent provider | [WeaponPose.h](../src/Src/WeaponPose.h) |
| player | +0x300 / +0x30C | first-person view origin / axis (body facing) | [Game.h](../src/Src/Game.h) |
| camera component | +0x30 / +0xD8 / +0xFC | renderView / eye / view axis | [HandAimHook.h](../src/Src/HandAimHook.h) |
| camera component | +0x251B8, +0x25264, +0x25268, +0x25274 | m_rightArmFollow, m_pitch, m_armsPitch, m_yaw | [HandAimHook.h](../src/Src/HandAimHook.h) |
| camera component | +0x2527C / +0x25288 | m_aimOrigin / m_aimAxis | [HandAimHook.h](../src/Src/HandAimHook.h) |
| camera component | +0x25304 / +0x25328 | m_leftArmAiming / m_rightArmAiming | [HandAimHook.h](../src/Src/HandAimHook.h) |
| camera component | +0x25440 / +0x25464 | renderView axis / origin copies | [HandAimHook.h](../src/Src/HandAimHook.h) |
| camera component | 0x252B0 bytes | a copy through the aim fields | [GrabHand.cpp](../src/Src/GrabHand.cpp) |
| aim component | +0x20 / +0x2C / +0x38 / +0x58 | local dir / world dir / position / camera | [HandAimHook.h](../src/Src/HandAimHook.h) |
| renderView_t | +0x9C / +0xA0 / +0xA8 / +0xCC / +0x100 / +0x140 | fov_x / fov_y / vieworg / viewaxis / explicit proj / use explicit | [CameraHook.h](../src/Src/CameraHook.h) |
| view object | +0x39D0 | m_renderRV | [CameraHook.h](../src/Src/CameraHook.h) |
| view object | +0x7848 / +0x784C | render width / height | [CameraHook.h](../src/Src/CameraHook.h) |
| view object | +0x3A84, +0x7390, +0x73D0, +0x7410, +0x74D0, +0x7590 | prev org, proj, inv proj, prev proj, prev view, prev VP | [StereoTaa.cpp](../src/Src/StereoTaa.cpp) |
| view object | +0x77B4 / +0x7850 / +0x7858 / +0xBCFC | frame number / jitter / prev jitter / cut flag | [StereoTaa.cpp](../src/Src/StereoTaa.cpp) |
| interaction sensor | +0x8 / +0x40 / +0x84 | decl / camera / USE pick (0x3FFF0010 none) | [GrabHand.cpp](../src/Src/GrabHand.cpp) |
| interaction decl | +0x70 / +0x74 | reach / slot radii | [GrabHand.cpp](../src/Src/GrabHand.cpp) |
| ranged-target sensor | +0x8 / +0x38 / +0x40 / +0x48 / +0x54 / +0x58; decl +0x74 | decl / enabled / camera / aim cpnt / has / point; range | [GunRay.cpp](../src/Src/GunRay.cpp) |
| ranged weapon component | +0x8 / +0x10 / +0x48 / +0xF4 / +0x118; decl +0x198 / +0x250 | decl / entity / camera / spawn pos / projectile shape; max range / blast offset | [GunRay.cpp](../src/Src/GunRay.cpp) |
| melee component | +0x3428 / +0x33E0 / +0x3B78 | window flags / owner's previous position / dwell | [MeleeAim.cpp](../src/Src/MeleeAim.cpp) |
| HoldObject state | +0x68 | held entity's spawn id | [GrabHand.cpp](../src/Src/GrabHand.cpp) |
| body component | +0x370 / +0x680 | left / right arm entity id | [BodyHide.h](../src/Src/BodyHide.h) |
| inventory | +0x38 / +0x40 (stride 0x38) | item table / count | [TestAids.h](../src/Src/TestAids.h) |
| inventory | +0x68 / +0x70 | right / left held item | [BodyHide.h](../src/Src/BodyHide.h) |
| inventory | +0x168 | infinite-ammo counter | [TestAids.h](../src/Src/TestAids.h) |
| inventory item | +0x8 / +0x10 | entity id / item component | [BodyHide.h](../src/Src/BodyHide.h) |
| mana | +0x20 / +0x2C | current / regen level | [TestAids.h](../src/Src/TestAids.h) |
| health | +0x20 / +0x114 | current / maximum | [TestAids.cpp](../src/Src/TestAids.cpp) |
| power system | +0x8 -> +0x88 (stride 0x10), +0x2F8 | power upgrade array, enhancement array | [TestAids.h](../src/Src/TestAids.h) |
| power system | +0x70 (stride 0x28) | equipment upgrade list | [TestAids.h](../src/Src/TestAids.h) |
| throw velocity block | +0x20 | spawn point | [ThrowGesture.cpp](../src/Src/ThrowGesture.cpp) |

---

## 12. Known traps and dead ends

Things that cost real time. Measured; listed so nobody repeats them.

**Dead ends**
- **Skipping the right eye's shadow passes in synchronised stereo** (exe+0x32D4B0 for the right eye's view): shadows
  broke in BOTH eyes. The sun's farther cascades are updated on alternate frames (frame parity, exe+0x32EF1D); in
  synchronised stereo alternate frames are alternate eyes, so some cascades are only ever updated in the right eye's
  frames. A pass may be skipped per eye only if the engine simply redoes it every frame.
- **Writing r_skipDOF / r_skipLensFlares / r_SSSBlurQuality / r_textureUploadTimesliceMs while in VR**: the game hung
  at its main menu (not narrowed down to one). r_skipDOF and r_skipLensFlares are the game's own menu options
  (ARK_VISUALSETTING_DOF / _LENS_FLARE); the menu code backs r_skipDOF up into r_skipDofBck and restores it through
  the setter (exe+0x4F2FC0, exe+0x50F9DE), so a live value can reach the config. Per view, use the render flags
  instead: view+0x3A37 bit 0 = no DOF (tested by the DOF pass, exe+0x315634), bit 1 = no SSAO (the SSAO pass,
  exe+0x305BF3), both set from the cvars in the view setup (exe+0x375075 / exe+0x37508F) before the passes run.
- **The game's ambient-occlusion option** (ARK_VISUALSETTING_HBAO_PLUS, `r_hbao`) chooses NVIDIA HBAO+ or the game's
  own AO (DWAO, `r_SsaoType 1`); it never turns AO off. Only `r_skipSSAO` (no menu) does.
- **Moving first-person arms/weapons from the CPU.** Writing the equipment entity's +0x300/+0x30C (or its transform
  component, or `arkTransformSet` of weapon and arm models), skipping `SetOriginAndAxis`, writing cam+0xD8/+0xFC after
  the shake hook, cam+0x25464, or the scene renderView copy ("FP anchor"): the image does not change beyond the idle
  animation's noise. The first-person models come from animation joints relative to the camera. The lever is the GPU:
  rewrite each draw's cb2 MVP (and the skinning palette for arms). Only the arm-aiming fields (`m_left/rightArmAiming`)
  move the arm IK, and then only in pitch.
- **Moving the equipment entity breaks weapon identification** (draws are matched to its real position): 26 draws fell
  to 5. Leave equipment and models bound to weapon joints at the flat pose; move only what is NOT a drawn model (FX,
  muzzle entity, blast, projectile) in the engine.
- **Overriding `m_aimAxis` to aim a gun**: weapons read cam+0xD8/+0xFC, not the aim fields.
- **Gamepad emulation through XInput**: events reach the queue, the player ignores them (see 2).
- **Patching the shared 800-byte per-view constant buffer** to move a weapon: bound but unused by those shaders.
- **Scanning memory for world-space bone matrices**: none exist; the only hits are `arkTransformSet`s.
- **Constancy of `M_i^-1 * socket`** cannot separate hand from forearm in idle animations (the whole arm is rigid).
- **"Instant" melee strikes** (opening the window at the contact, ending the sweep there, writing the contact's velocity
  into comp+0x33E0): the object is only pushed, not broken.
- **Removing the sword trail with `r_prt_*_budget 0`**: no effect. **`r_skipSurfaceCPUCulling 1`**: the reloading
  weapon is still culled.
- **Identifying the weapon by a hide/show scan diff, or by index counts**: not deterministic (animation and LOD change
  between the two scans) and index counts match world geometry after a reload.
- **Assigning a draw to the NEAREST controller**: wrong when the hands are together; use the owning equipment entity.
- **A weapon-relative grip that is re-learned in arbitrary frames**: the flat animation moves the palm relative to the
  weapon by up to 180 deg (crouched sword), 135-178 deg / 52-66 cm (mantle), 80 deg (re-equip).

**Traps**
- `GetEntityById` and any pointer learned from the world are invalid during a level change (NULL global, freed
  entities, freed render models, recycled vertex buffers and shader addresses, new palettes). Revalidate every frame;
  never write through a pointer validated an earlier frame.
- Never call the interaction sensor's RESULTS twice a frame: fatal, silent quit.
- Hook signatures must be exact: `WriteConfigToFile` has 5 arguments, the bind exe+0x7506D0 has 7, the grapple worker
  has 6 (2 on the stack). A short signature drops stack arguments: broken cfg, crash on load, null read.
- A fatal engine error quits with no exception and no WER event: a crash handler logs nothing. Look for the error
  string's code path instead.
- Camera fields swapped from one thread are seen by all threads.
- Synthesised mouse buttons go to the window under the cursor; keys need scan codes, modifiers too; presses need
  >= 80 ms; a button held across a mode change (menu <-> play) needs a release first in the mod's logic.
- `win_pauseOnFocusLoss 0` does not stop the focus-loss pause. Check the game is simulating before measuring.
- Keys sent with `keybd_event` + `KEYEVENTF_SCANCODE` (an external script) did not behave like the mod's `SendInput` by
  scan code (ESC neither paused nor left a shop section). Test menu keys the way the controllers send them. A
  synthesised SHIFT+TAB once left SHIFT logically held until a lone SHIFT.
- NaN passes every float comparison: `std::isfinite` before any range test in a memory scanner.
- cb2 contents and palettes are per draw: a snapshot at Present is another object's data.
- Bound is not used: a constant buffer or SRV bound at a slot may not be read by that shader. Read the shader
  (`D3DDisassemble` / `D3DReflect` of the bytecode kept from `CreateVertexShader`).
- Shader object pointers and index counts change per run/level; use DXBC checksums and reflection names.
- Bone/object matrices in these buffers are three `float4` rows: a 9-floats-in-a-row matrix detector finds none.
- `RenderRes`-style size changes must keep the real window size in the cfg; the console `writeConfig` writes a partial
  file. The game left running can rewrite the cfg with test settings: back it up.
- Borderless (`r_fullscreen 2`) renders at the monitor's size regardless of `GetClientRect`.
- The physics ray/sweep helpers (exe+0x638570 / exe+0x638800) see only static collision; the sensors' async casts see
  props and characters.
- A thrown held object is a projectile at any speed (it breaks what it hits); a dropped one is not.
- Cutscene cameras move up to 0.35 m per rendered frame (a train ride): "N metres in one frame is a cut" or "follow at
  most M m/s" rules must be relative to the camera's own motion. After a hitch the camera catches up in 1.25-1.45 m steps.
- The power wheel makes the player tick every ~0.17 s: "is it simulating" flaps while it is up.
- Possession, surface swimming and Shadow Walk have no (or foreign) first-person arm draws: "no arms drawn" is not
  "in a menu".
- `exec LoadGame` only quick-loads (nothing without a quicksave); menu automation by mouse is unreliable (hover changes
  the active item).

---

## 13. Superseded findings

Where an older note or a source comment says otherwise, this is what holds (the later measurement wins).

| earlier claim | what holds |
|---|---|
| The cvar dump's RVAs locate cvars (the `g_Known` table in [CVars.cpp](../src/Src/CVars.cpp) still lists them) | They do not match this exe; resolve by name scan. The table's RVAs are kept "for reference only" |
| `BufferCommandText` is probably cmdSystem slot 7 | Slot 9 (8 = ExecuteCommandText), measured ([Game.h](../src/Src/Game.h)) |
| exe+0x3586A0 builds the view/projection matrices (comment in [CameraHook.h](../src/Src/CameraHook.h)) | It only uploads them; the scene moves through CreateViewMatrix (exe+0x109CC0) and the fov through the projection builder (exe+0x3742B0) |
| exe+0x881640 builds the player's renderView (comment in [PlayerViewHook.h](../src/Src/PlayerViewHook.h)) | It builds other entities' views, never the local player's; the hook is installed disabled ([Main.cpp](../src/Src/Main.cpp)) |
| ComputeAimTarget's muzzle = `player+0x300 + player+0x30C * spawnPos` (comment in [HandAimHook.h](../src/Src/HandAimHook.h)) | The muzzle uses the WEAPON's equipment entity (this+0x10) |
| The grapple sensor reads the player's +0x300/+0x30C (comment in [HandAimHook.h](../src/Src/HandAimHook.h)) | It copies the camera's m_aimOrigin/m_aimAxis into its ray (exe+0x3B6E98..0x3B6F69, measured; [HandAimHook.cpp](../src/Src/HandAimHook.cpp)) |
| Writing the controller pose into the scene renderView copy (FP anchor) moves arms and weapons | Measured at the noise floor: no effect (dead end above) |
| The arm draws have no `cb_mvpmatrix*` | They do; they were missed because the arm entity's origin is behind the camera |
| The interaction sensor is one function (exe+0x3B8D60) | Two phases, SUBMIT and RESULTS, both read the aim fields |
| A held prop's render model is rotated against its entity | It is not; the local box is the render model's bounds times the entity scale |
| Pitch cannot be closed-loop (the view axis is flat) | The player's +0x30C is flat; the camera's +0xFC carries the pitch |
| Alternating-eye stereo: submit the backbuffer to `eye ^ 1` | The phase varies between sessions; identify the image's view by command-list serials |
| The presented image is the latest view (`vr posesource` before 2026-10-23) | It is the view that was the latest at the PREVIOUS Present (checked every frame against its TAA dispatch) |
| Stereo needs TAA off (`r_skipTemporalAA 1`) | Per-eye TAA history and previous matrices work ([StereoTaa.cpp](../src/Src/StereoTaa.cpp)) |
| `win_pauseOnFocusLoss 0` keeps the game running unfocused | The game still pauses on focus loss |
| `arkCpntMana+0x2C` is the maximum | It is the regen level, lowered by every cast |
| `ark_god_player` (via `god`) keeps the player alive | Not in a sword fight; pin `arkCpntHealthEvolved+0x20` |
| `exec LoadGame` is the robust way into gameplay | It quick-loads only; with no quicksave it does nothing |
| Entity +0x360 mirrors the render model pointer (comment in [BodyHide.h](../src/Src/BodyHide.h)) | +0x360 is the skeleton object, whose +0x38 is the render model ([NpcSkel.cpp](../src/Src/NpcSkel.cpp)) |
| Gadgets are told from weapons by their draw count (1-2) | The Heart draws 8 meshes and a reloading pistol 1-2: use the item component's class (item+0x10) |
