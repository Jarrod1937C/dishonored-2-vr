================================================================================================
  DISHONORED 2 VR  -  version {VERSION}  -  by The Cosmic Tangerine
================================================================================================

A mod that plays Dishonored 2 in a PC VR headset: the view follows your head, your hands are
your controllers (the sword strikes where you swing it, the pistol and crossbow fire where they
point, powers come out of your hand), and you can grab and throw things with your hands.

It is a free, open-source, unofficial fan mod, not affiliated with or endorsed by Arkane Studios
or Bethesda. Source code, updates and bug reports: {REPO_URL}
Support the development: patreon.com/CosmicTangerine  -  ko-fi.com/cosmictangerine


------------------------------------------------------------------------------------------------
 1. WHAT YOU NEED
------------------------------------------------------------------------------------------------

 * Dishonored 2 on STEAM, updated (Steam always installs the latest version).
   NOT supported: the GOG or Microsoft Store / Game Pass versions, and "Death of the Outsider".
   (On those, the mod shows a message at startup and stays off; the game runs normally.)
 * Windows 10 or 11, 64-bit, and a PC that can run VR.
 * A PC VR headset:
     - Meta Quest 2 / 3 / Pro with Link or Air Link ......... tested (Quest 3)
     - Meta Quest with Virtual Desktop (VDXR) ............... should work
     - Meta Quest through SteamVR ........................... should work
     - PlayStation VR2 through SteamVR (PC adapter) ......... experimental (see "PS VR2" below)
     - Valve Index and other SteamVR headsets ............... experimental
 * Nothing else to install. (No Visual C++ redistributable, no launcher, no script loader.)


------------------------------------------------------------------------------------------------
 2. INSTALL
------------------------------------------------------------------------------------------------

 1. In Steam: right-click Dishonored 2 > Manage > Browse local files.
    A folder opens; it contains Dishonored2.exe.
 2. Copy these files from the zip INTO THAT FOLDER, next to Dishonored2.exe:
        dinput8.dll
        DishonoredVR.dll
        DishonoredVR.ini
 3. That's it. If Windows or your antivirus deletes dinput8.dll or DishonoredVR.dll, restore them
    and add an exception: mods like this one load into the game, which antivirus programs
    sometimes dislike.

 Already using another mod that comes with its own dinput8.dll? Only one can be there.

 UNINSTALL: delete dinput8.dll, DishonoredVR.dll, DishonoredVR.ini, DishonoredVR*.log and the
 DVR_*.txt files from the game folder. The game itself is never modified.


------------------------------------------------------------------------------------------------
 3. ONE-TIME SETUP: THE "OPENXR RUNTIME"
------------------------------------------------------------------------------------------------

 The mod talks to your headset through OpenXR. Windows has ONE "active OpenXR runtime" at a time,
 and it must be the app your headset uses right now. This is the most common reason for "nothing
 shows up in the headset".

   Quest with Link / Air Link:  Meta Horizon Link app > Settings > General >
                                 OpenXR Runtime: "Set Meta Horizon Link as active"
   Quest with Virtual Desktop:  Virtual Desktop Streamer > OpenXR Runtime: VDXR
   SteamVR (PS VR2, Index...):  SteamVR > Settings > OpenXR > "Set SteamVR as OpenXR Runtime"

 GAME OPTIONS (Options > Visuals in the game):
   - Display mode: WINDOWED. Not fullscreen and not borderless: in those two the game renders at
     your monitor's size and the headset gets a blurry picture.
   - The game's own resolution setting does NOT change what you see in VR: the mod renders the
     game at the resolution your HEADSET asks for. For a sharper or faster picture change the
     resolution in the headset's PC app (Meta Horizon Link app: the headset's graphics
     preferences, render resolution; Virtual Desktop: Streaming tab; SteamVR: Settings > Video),
     or RenderScale in DishonoredVR.ini (section 6). Graphics quality (shadows, textures...)
     works as usual.
   - Leave the key bindings as you like them (the mod reads them); mouse sensitivity and
     "invert mouse" are handled by the mod.


------------------------------------------------------------------------------------------------
 4. PLAYING
------------------------------------------------------------------------------------------------

 1. Connect the headset (Link / Air Link / Virtual Desktop / SteamVR running).
 2. Start Dishonored 2 from Steam as usual. When it is running, CLICK THE GAME'S WINDOW ON THE PC
    ONCE so it is the window in front (see the box below), then put the headset on.
 3. Menus show on a big screen in front of you, fixed in the room: look around it freely. Use
    the controllers: sticks move, A selects, B goes back, grips change tab, triggers change
    sub-tab, a stick click brings the screen back in front of you. Loading screens show on the
    same screen.
 4. Load or start a game. A moment after you are in control of your character the VR mode
    switches on by itself: your hands appear and the view follows your head.
    Cutscenes happen around you: look wherever you like with your head (the right stick turns
    too). To skip one, hold A or X for about 3 seconds (the game's "Skip" ring fills up).

 +--------------------------------------------------------------------------------------------+
 | IMPORTANT: THE GAME MUST BE THE WINDOW IN FRONT ON THE PC (focused) WHILE YOU PLAY.        |
 | The mod turns the controllers into the game's keyboard and mouse, and Windows only gives   |
 | those to the window in front. With another window in front (browser, Discord, Steam...)    |
 | the controllers do nothing and the game pauses. The mod brings the game back to the front  |
 | by itself when you press a controller button or put the headset on, and closes that pause  |
 | (FocusGameWindow in DishonoredVR.ini). If Windows does not let it, click the game's window |
 | on the PC once (or Alt+Tab to it).                                                         |
 +--------------------------------------------------------------------------------------------+

 No headset connected? The game runs as the normal flat game. If you connect the headset
 later, VR switches on by itself. If the connection drops while playing, VR should come back by
 itself when the headset reconnects.

 Recenter the view with your headset's own recenter (Quest: hold the Meta button). Walking around
 your room walks the character with you (Roomscale), so a recenter is rarely needed: the game's
 collision stops the character at walls and furniture, and if your head goes on into a wall the
 headset fades to dark blue (WallFade) until you step back out.


------------------------------------------------------------------------------------------------
 5. CONTROLS
------------------------------------------------------------------------------------------------

 IN GAME
   Left stick ........................ move.  Click: sprint (on until you stop)
   Right stick ....................... turn (smooth; snap in DishonoredVR.ini).  Click: zoom
   Right trigger ..................... sword / right-hand weapon
   Swing your RIGHT ARM .............. strike with the sword (what the blade touches is hit; with
                                       SwordBladeDamage = 1 the blade itself strikes, harder the
                                       faster it moves; a guard can still block or parry it)
   Fist by your right ear, blade
     forward, then thrust ............ power attack (lower the blade to cancel it)
   Fist at your left shoulder ........ block (the right grip blocks too)
   Left trigger ...................... power or left-hand weapon (pistol, crossbow) - they fire
                                       where your hand points
   Grenades / springrazors ........... hold the left trigger, throw, and let go: a grenade leaves
                                       your hand at that moment, the way and as hard as it moved
   Right grip, pointing at something . take / use / open it (hold it to pick up a body).
                                       Holding an object: grip again = drop, right trigger = throw
                                       Carrying a body: grip again = put it down
   Right grip, pointing at nothing ... block
   Left grip (hold) .................. power wheel - pick with the stick, let go to equip
                                       (it opens on a panel in front of you; the world stays put)
       ...with the wheel open: B ..... drink a health elixir
                               A ..... drink a mana elixir
                               Y / X . assign the highlighted item to a quick slot
   A ................................. jump
   B ................................. crouch (toggle)
   Y ................................. quick slot 1 (hold 0.5 s: slot 3 - the controller buzzes)
   X ................................. quick slot 2 (hold 0.5 s: slot 4)
   Menu button (left controller) ..... short press: journal.  Hold: pause menu (save / load).
                                       The world stops and dims around the menu screen
   BOTH STICKS PRESSED TOGETHER ...... same as the menu button (for controllers without one)
   Look at the back of your LEFT
     wrist, like checking a watch .... health, mana and the equipped item with its ammo
                                       (hidden while the left trigger is held)
   Lean, peek, crouch for real ....... just do it with your body: the view follows your head.
                                       There is no lean button.
   Hand to your mouth, squeeze grip .. drink an elixir: left hand health, right hand Addermire
                                       Solution (mana) (MouthPotions; a light buzz at the mouth)
   Walk around your room ............. the character walks with you (Roomscale); small leans
                                       (under 10 cm) leave it where it is

 IN CUTSCENES
   Your head ......................... look around (the scene happens around you; a train or a
                                       boat you ride turns you with it)
   Right stick ....................... turn
   A or X, held about 3 s ............ skip the cutscene (also a mission's opening video)
   Menu button ....................... journal / pause, as in game

 With Reticle = 1 in DishonoredVR.ini (off in this build), two lines show where each hand points:
 the RIGHT one is what the right grip takes or uses, the LEFT one is where a power or shot goes,
 with a ring where it will land. MinimalReticle = 1 shows only a small white dot where each one
 ends (the right one grows when it is on something you can take or use); ReticleOpacity sets how
 see-through they are. When you can take a guard down from behind, the right one turns RED inside a
 thick BLUE border: swing the sword for the kill (red), or choke him (blue: the block button). With
 SwordBloodflyReach (0.3 m) makes bloodflies easier to hit with the blade; bloodfly nests can be struck too.
 KnockoutBlow = 1, hitting the back of his head hard (fist, the sword's pommel, the pistol) knocks him
 out at once instead. KnockoutSpeed sets how hard (in m/s; 4.5 by default), KnockoutAngle how squarely
 behind him you must be (degrees off straight behind; 60), KnockoutRadius how close to his head (m; 0.3).

 IN MENUS
   Sticks: move   A: select   B or menu: back   Grips: next tab   Triggers: previous / next sub-tab
   Click either stick: the screen comes in front of you (it also does by itself when you look
   away from it for a moment)
   The new game's character choice and a black market's counter are menus too, on the same
   screen: left / right picks Corvo or Emily, A confirms ("Play as ...? No / Yes"); in a shop
   A buys, the grips switch Ammunition / Items, B goes back out.

 VALVE INDEX (experimental)
   The left controller's A / B act as X / Y. Menu button: press the left trackpad down hard.
   The grips react to how hard you SQUEEZE them (their force sensor), not to your fingers resting
   on them: GripThresholdLeft / GripThresholdRight in VRControllerBindings\index.json set how firmly.
   If you changed the game's grip bindings in SteamVR, reset them there.
   Layout = index (in index.json, the Index's own profile) uses the trackpads instead (the rest is as above):
   Left trackpad, press its top / right / bottom / left .. quick slot 1 / 2 / 3 / 4
                                       (with the wheel open: assign the highlighted item to it;
                                       in menus: the arrows)
   Left trackpad, press its middle ... menu button (short: journal, hold: pause)
   Right trigger ..................... interact: use / open / take what your RIGHT hand points at.
                                       The sword strikes by swinging your arm
   Right grip ........................ physical only: take what your hand is AT (a bottle, a cup,
                                       a body; an item with PhysicalPickup), grab a drawer, cabinet
                                       or door to pull it open. An object stays in your hand while
                                       you hold the grip: open it to drop it (or throw it by moving
                                       your hand). With the sword out it blocks; otherwise it never
                                       draws a weapon
   Crouch for real ................... the character crouches (PhysicalCrouch); stand up to stand
   Doors and windows ................. push a door with either hand to open it, push an open one
                                       to close it; slide either hand up / down on a window
   Right trackpad, press ............. the sword's attack button (hold: power attack); in
                                       cutscenes: skip
   Left B (upper) / left A (lower) ... drink a health / mana elixir
   Right A / B ....................... jump / crouch, as above
   Your fingers (FingerTracking = 1) .. the in-game fingers of a hand with a power (or with
                                       nothing in it) open and close with yours. A hand that holds
                                       the sword, a gun or an object keeps its grip on it.

 PS VR2 (Sense controllers through SteamVR)
   Square / Triangle are X / Y, Cross / Circle are A / B. SteamVR does not pass the Create button
   to the game: use BOTH STICKS PRESSED TOGETHER for the journal and the pause menu, or assign
   Create in SteamVR > Settings > Controllers > Manage Controller Bindings > Dishonored 2 >
   "Menu: journal (short) / pause (hold)".


------------------------------------------------------------------------------------------------
 6. SETTINGS
------------------------------------------------------------------------------------------------

 DishonoredVR.ini (in the game folder) - open it with Notepad. Each setting is explained there.
 Changes made in the file apply the next time you start the game. 1 = on, 0 = off. Deleting a line (or
 the whole file) gives you the default back. Most of them can also be changed in the game itself: Options
 has VR tabs (VR Controls, VR Gameplay, VR Display, VR Performance, VR System/Cheats) after its own,
 each setting showing the value recommended for most players. Going back from a VR tab after changing
 something asks whether to apply the changes: Yes writes them into DishonoredVR.ini and they take effect
 at once, except the ones marked "restart" (used from the next start); No leaves everything as it was.
 The file is in main groups ([Display]) and subgroups
 ([Display.Fog]), and every setting is marked @user (for everyone) or @hidden (tuning, experiments
 and troubleshooting most players never need; last in each subgroup). A setting is found by its
 name in any group, so an older file still reads. The defaults, by main group:

   [Controls]  TurnMode = smooth (or snap)       SmoothTurnSpeed = 120 (deg/s)  SnapTurnAngle = 45
               SnapTurnBlank = 1 (snap turning: darker for the moment of each snap)
               SnapTurnBlankOpacity = 0.5 (how dark: 1 = black)
               ControllerBindings = auto (which controller profile, below)   Haptics = 1 (vibration)
               FocusGameWindow = 1 (the game is brought back to the front when you use the controllers)
   Controller profiles: VRControllerBindings\<name>.json (index.json: Valve Index), one per controller, hold what
               depends on the controller: Layout (default / classic / index), QuickSlotHoldTime = 0.5 (s),
               GripThresholdLeft / GripThresholdRight (how firmly each grip is squeezed to count; lower = more
               sensitive; Index: squeeze force), FingerTracking, ControllerPitchOffset (tilts the hands up / down,
               degrees), the trackpads' press force and centre. ControllerBindings = auto picks the profile made
               for the controllers in use, or the first file there is; a file's name (index) forces that one.
               Open them with Notepad; "notes" in each explains its settings.
   [Gameplay]  SwordGestures = 1   ThrowGestures = 1   GrabObjects = 1
               HandCollision = 0 / WeaponCollision = 0 (experimental: your hands / the sword and guns stop at
                                 walls, tables, props and, for the blade, people's bodies, instead of passing through)
               PushObjects = 1   PushStrength = 1 (with those: hands and weapons push loose objects; kg)
               SwordBodyDepth = 4 (cm the blade sinks into a body before it stops)
               PushDoors = 1 (with HandCollision: push a door with your hand to open it)
               PullToOpen = 1 (with HandCollision: grab a drawer / cabinet, pull it open, push it shut)
               SwipeDials = 1 (with HandCollision: slide your hand up / down a safe's number wheel to turn it)
               SlideWindows = 1 (with HandCollision: slide your right hand up / down a window to raise / lower it)
               HolsterWeapons = 1 (grip at a holster: left hand to the right hip = pistol, behind the
                                 back = crossbow, chest = last power; right hand to the left hip =
                                 sword. Again with it in the hand = put it away. A light buzz tells
                                 you the hand is at a holster. An emptied hand stays empty: the
                                 wheel, a quick slot, blocking or a stun no longer bring the sword back)
               PhysicalPickup = 0 (experimental, with HandCollision: grab items with the right hand and
                                 put them behind your back to keep them, or food to your mouth)
   [Display]   Reticle = 0 (1 = the two pointing lines) WristHud = 1  WristHudSize = 8.5 (cm)
               MinimalReticle = 0 (1 = only a dot where each hand points)  ReticleOpacity = 0.5
               StereoHud = 1 (3D: the game's interface on one layer for both eyes, not doubled)
               HudDistance = 2 (m, how far away that layer floats)
               HudMarkers = 1 (a guard's alert marker and the objective markers placed over them in the headset)
               HudMarkerSize = 0.07 (each marker shown at its character's distance; 0 = on the HUD's layer)
               HudScale = 0.8 (the HUD's share of your view: the edges brought inward)
               CameraPitchFix = 1 (the game's camera does not slide forward / back as you look down / up)
               DisableMetaASW = 1 (Link / Air Link: Meta's ASW off for this game, see section 7)
               VrGraphicsTweaks = 1 (in VR only: no depth of field; 0 = as the game has it)
               AmbientOcclusion = 1 (0 = no ambient occlusion in VR: faster)
               Fog = 1 (0 = no fog in VR: its low-resolution blocks show in front of the sky; faster)
               FogMaps = void (with Fog = 0, maps named so keep their fog: the Void)
               CheapFog = 1 (with Fog = 0: the mod's own light distance fog, so the background blends in)
               CheapFogStart = 30   CheapFogEnd = 300 (m)   CheapFogOpacity = 0.6
               CheapFogColor = auto (night / dusk / day colour by the map's upper sky; sky = the horizon's
                                     average; or R,G,B 0-255)
               CheapFogColorNight / CheapFogColorDusk / CheapFogColorDay = R,G,B[,brightness]
               CheapFogNightBelow = 0.1   CheapFogDayAbove = 0.5 (the upper sky's brightness)
               CheapFogBrightness = 1 (the fixed colour's brightness)
               CheapFogMinBrightness = 0.35 (never darker than this, in the game's light units)
               CheapFogFollowExposure = 1 (the fog changes with the eye adaptation as the scene does)
               CheapFogSkyHeight = auto (the sky fogged up to the far background's top; or degrees; 0 = none)
               ScreenSpaceShadows = 0 (experimental: sharp sun shadows of characters / small objects on screen)
               ScreenSpaceShadowCharacters = 1 (characters cast them)   ShadowMapCharacters = 0 (and leave the sun's shadow maps; needs ScreenSpaceShadows = 1)
               ScreenSpaceShadowSmallObjects = 1 (m across: smaller moving objects too, out of the shadow maps)
               ScreenSpaceShadowLength = 3 (m)   ScreenSpaceShadowContact = 0 (m, everything's contact shadows)
               ScreenSpaceShadowSteps = 24   ScreenSpaceShadowStrength = 1
               DisableMotionBlur = 1   Stereo3D = 1 (real depth, each eye drawn at the same instant,
                                 about half the frame rate; the mod is made for 1: leave it,
                                 0 / 2 are likely to break things)
               RenderScale = 1 (sharpness: 1 = what the headset asks for, lower = faster)
               MenuScreen = 1   MenuScreenSize = 62 (deg wide)   MenuScreenDistance = 2.4 (m)
               MenuBackground = 0.22 (how bright the paused world is around the menu screen)
               CutsceneLetterbox = 0 (1 = keep the black bars)   CutsceneArms = 0 (1 = the
                                 cutscene moves your hands, not the controllers)
   [Performance]
               ShadowCullDistance = 0 (m objects cast shadows to; 0 = the game's: Low 15, High 32, Ultra 75)
               ShadowUltraTrim = 1 (at Ultra: the costly settings that do not sharpen held at High's)
               ShadowLampFewerUpdates = 1 (lamps redraw moving things' shadows only when near / large: Low's)
               ShadowLampCacheSlices = 32 (the lamps' shadow cache, 16 MB a slice; 0 = the game's 12)
               StaticLodZoom = 1 (experimental: >1 = static models switch to simpler versions sooner; 1 = the game's)
               ViewDistance = -1 (the game's view-distance level instead of its menu's: 0 very low .. 5 ultra; -1 = the menu's)
               DepthPassDepthOnly = 0 (experimental: the depth pass's main shader drawn depth-only)
               DepthPassSkipRedundantCopies = 1 (depth copies that change nothing are not made)
               ShadowMapMinObjectSize = 0.3 (m across: smaller moving objects cast no sun shadow-map shadow)
               ShadowSunUpdateEvery = 2 (frame pairs between redraws of the sun's shadows beyond the nearest)
               OutOfViewMask = 0 (1 = the part of each eye's picture outside the lens's view is not shaded; breaks screen-space reflections, hair, glass; 2 = only each eye's outer side)
               OutOfViewMaskMargin = 2 (degrees beyond the lens's view still shaded)
               OutOfViewMaskLens = 1 (also the lens's hidden area, from the headset's runtime; OutOfViewMask = 1 only)
               OutOfViewMaskCorners = 4 (also each picture's two outer corners outside a rounded outline; 0 = off)
               FoveatedRendering = 0 (NVIDIA only: coarser shading away from each lens's centre; faster)
               FoveatedInner = 35   FoveatedOuter = 50 (deg: full rate, then 2x2, 4x4 beyond)
               Experimental, 0 by default (1 = the right eye's frame reuses the left eye's work):
                                 StereoOceanReuse   StereoDecalReuse   StereoStreamOncePerPair
                                 StereoLightShadowReuse (lamps' shadows)
                                 (StereoReflectionReuse is no longer used: it broke reflections)
               SkipDisabledEffects = 0 (experimental: 1 = the disabled SSAO's fill becomes a clear; 2 = also skip
                                      DOF's copy, which breaks water / electricity: do not use)
   [Cheats]    UnlockEverything = 1   InfiniteMana = 1   InfiniteAmmo = 0   InfiniteHealth = 0
               or, each on its own: UnlockOutsidersMark, UnlockPowers, UnlockPowerUpgrades,
               UnlockBlackMarketUpgrades, UnlockWeapons, FillAmmoAndGadgets, FillElixirs
   [Cheats.Powers] each power on its own: UnlockBlink, UnlockFarReach, UnlockDarkVision, UnlockPossession,
               UnlockDevouringSwarm, UnlockBendTime, UnlockWindblast, UnlockDomino, UnlockDoppelganger,
               UnlockMesmerize, UnlockShadowWalk
   [System]    KeepAliveFrames = 1 (while the game stops for a moment, the headset keeps the last
                                 picture instead of an hourglass)
               NewSessionAfter = 5 (s: VR restarts by itself when the headset's app stops it;
                                 Link / Air Link only)
   [Logging]   LogControllerInput = 1   WriteStatusFile = 0
               ProfileRenderPasses = 0 (1 = the GPU time of each render pass, to the log every 5 s; with
                                      the mod's own steps, hitches and a frame draw by draw)

 TWO CHEATS ARE ON by default, so every power and weapon can be tried in VR right away:
 UnlockEverything (every power, upgrade, bone charm crafting, black market upgrade and weapon, on
 every save you load, with full ammo once) and InfiniteMana. Ammo, gadgets, elixirs and health
 work as in the game (InfiniteAmmo / InfiniteHealth = 1 to change that). The cheats change your
 game for real: once the game saves or autosaves, the save keeps them. Use saves you do not mind
 changing, or set them to 0 before playing your own.

 Not in the file, always on: the view follows your head (and the left stick walks where you
 look), the hands and arms follow your controllers, and takedowns keep the view on your head.
 DisableMotionBlur also unticks motion blur in the game's own options (it stays off afterwards;
 tick it again there if you go back to the flat game).


------------------------------------------------------------------------------------------------
 7. IF SOMETHING GOES WRONG
------------------------------------------------------------------------------------------------

 Nothing in the headset
   - Is your headset's app the active OpenXR runtime? (section 3)
   - Was the headset connected and its PC app running? (Connecting later also works.)
   - Open DishonoredVR.log in the game folder and look for "OpenXR session not started (...)":
     the text in brackets says why.

 The game does not start at all on a PC with an AMD graphics card
   - Dishonored 2 itself (also without the mod) fails to start on some AMD setups and needs a
     replacement of its AMD AGS library (amd_ags_x64.dll). This is the game, not the mod.

 "Dishonored 2 VR is NOT active: this game build is not supported" at startup
   - Only the current Steam version of Dishonored 2 is supported (section 1).

 The controllers do nothing / the triggers, grips or the power wheel do nothing / the game paused
 by itself
   - The game window lost focus (section 4): another window is in front of it on the PC. Click
     the game's window on the PC once (or Alt+Tab to it), then put the headset back on.
   - A small window from the mod appeared on the PC screen and the game ignores the mouse and
     keyboard: the Insert key opens it. Press Insert again to close it.

 Hands and weapons point too high or too low compared to your real controllers
   - VRControllerBindings\<your controller>.json > ControllerPitchOffset (degrees, + = up), or in the
     game: Options > VR Controls. Try steps of 10.

 A laptop with two graphics cards: VR does not start, log mentions "different GPU"
   - Windows Settings > System > Display > Graphics > Dishonored2.exe > High performance.

 An hourglass in the headset / the picture freezes
   - The headset's app stopped taking frames (an Air Link / Link hiccup). The game keeps running
     on the PC and VR comes back by itself: on Link / Air Link at most NewSessionAfter seconds
     after the headset's app lets go; on SteamVR / Virtual Desktop when their app resumes.
     If it happens often on Air Link: 5 or 6 GHz Wi-Fi, the router close and in
     sight, nothing else downloading -- or a Link cable.
   - Please send DishonoredVR.log (section 8): it says what was stuck ("Watchdog: STALL").

 Low frame rate
   - Lower the resolution in SteamVR / the Meta Horizon app / Virtual Desktop, or set
     DishonoredVR.ini > RenderScale = 0.8. Lower the game's graphics settings. (Stereo3D = 1
     draws the world twice per frame, about half the frame rate, but leave it at 1: the mod is
     made for it and the other values are likely to break things.)

 Warped, "glitchy" edges around your hands or the world when you move fast
   - Your headset's app is inventing frames because the game runs below the headset's refresh.
     Meta (Link / Air Link): the mod turns ASW off for this game (DisableMetaASW = 1); if it is
     still on, Ctrl+Numpad1 on the PC keyboard turns it off. SteamVR: Settings > Video > Per-
     application video settings > Dishonored 2 > Motion Smoothing off. Virtual Desktop: its
     streaming settings > SSW off. Pimax: Pimax Play > Smart Smoothing off.

 Menus too small / too big, or too close
   - DishonoredVR.ini > MenuScreenSize (degrees: 70 = bigger) and MenuScreenDistance.

 Blurry picture
   - The game must be in a window (Options > Visuals > Display mode: windowed), not fullscreen
     and not borderless (both render at your monitor's size).
   - Raise the resolution in SteamVR / the Meta Horizon app / Virtual Desktop, or RenderScale.


------------------------------------------------------------------------------------------------
 8. REPORTING A PROBLEM  (thank you!)
------------------------------------------------------------------------------------------------

 Send, right after it happened (before starting the game again if you can):
   - DishonoredVR.log and DishonoredVR.prev.log from the game folder
     (if they are not there: %LOCALAPPDATA%\DishonoredVR)
   - your headset, and how it is connected (Link, Air Link, Virtual Desktop, SteamVR)
   - what you did, what you expected, what happened (a photo or video helps a lot)
 The log contains your PC's hardware and the mod's state; it does not contain personal data
 beyond your Windows user name in file paths.


------------------------------------------------------------------------------------------------
 Third-party software in this mod: see THIRD-PARTY-NOTICES.txt.
------------------------------------------------------------------------------------------------
