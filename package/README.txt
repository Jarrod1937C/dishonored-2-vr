================================================================================================
  DISHONORED 2 VR  -  version {VERSION}  -  by The Cosmic Tangerine
================================================================================================

Play Dishonored 2 in a PC VR headset: the view follows your head, your hands are your
controllers, the sword strikes where you swing it, the pistol, crossbow and powers fire where
your hand points, and you take, throw, drink and climb with your own hands.

It is a free, open-source, unofficial fan mod, not affiliated with or endorsed by Arkane Studios
or Bethesda. Source code, updates and bug reports: {REPO_URL}
Support the development: patreon.com/CosmicTangerine  -  ko-fi.com/cosmictangerine

 CONTENTS
   1. What you need          5. What you can do         9. If something goes wrong
   2. Install                6. Settings               10. Reporting a problem
   3. Playing                7. Cheats
   4. Controls               8. Frame rate


------------------------------------------------------------------------------------------------
 1. WHAT YOU NEED
------------------------------------------------------------------------------------------------

 * Dishonored 2 on STEAM, up to date (Steam always installs the latest version).
   NOT supported: the GOG or Microsoft Store / Game Pass versions, and "Death of the Outsider".
   (On those the mod shows a message at startup and stays off; the game runs normally.)
 * Windows 10 or 11, 64-bit, and a PC that can run VR (see section 8 about the frame rate).
 * A PC VR headset:
     - Meta Quest 3 with Link or Air Link ............................. tested
     - Bigscreen Beyond 2, Pimax, on SteamVR with Index controllers ..... tested
     - Other Quests, Virtual Desktop (VDXR), other SteamVR headsets ..... should work
     - PlayStation VR2 through SteamVR (PC adapter) ................... experimental
 * Nothing else to install: no Visual C++ redistributable, no launcher, no script loader.


------------------------------------------------------------------------------------------------
 2. INSTALL
------------------------------------------------------------------------------------------------

 1. In Steam: right-click Dishonored 2 > Manage > Browse local files.
    A folder opens; it contains Dishonored2.exe.
 2. Copy these from the zip INTO THAT FOLDER, next to Dishonored2.exe:
        dinput8.dll
        DishonoredVR.dll
        DishonoredVR.ini
        the VRControllerBindings folder (one profile per kind of controller)
 3. Make your headset's app the ACTIVE OPENXR RUNTIME. Windows has one at a time, and it must be
    the app your headset uses right now (the most common reason for "nothing in the headset"):
      Quest with Link / Air Link:  Meta Horizon Link app > Settings > General >
                                   OpenXR Runtime: "Set Meta Horizon Link as active"
      Quest with Virtual Desktop:  Virtual Desktop Streamer > OpenXR Runtime: VDXR
      SteamVR (Index, Beyond, Pimax, PS VR2...):
                                   SteamVR > Settings > OpenXR > "Set SteamVR as OpenXR Runtime"
 4. In the game: Options > Visuals > Display mode: WINDOWED. Not fullscreen and not borderless:
    those render at your monitor's size and the headset gets a blurry picture.
    The game's resolution setting does not change what you see in VR: the mod renders at the
    resolution your HEADSET asks for (change that in the headset's PC app, or with RenderScale).

 If Windows or your antivirus deletes dinput8.dll or DishonoredVR.dll, restore them and add an
 exception: mods like this one load into the game, which antivirus programs sometimes dislike.
 Already using another mod with its own dinput8.dll? Only one can be there.

 UNINSTALL: delete dinput8.dll, DishonoredVR.dll, DishonoredVR.ini, the VRControllerBindings
 folder, DishonoredVR*.log and the DVR_*.txt files from the game folder. The game itself is
 never modified. (DVR_hands.txt and DVR_grips.txt are not logs: they keep how your hands grip
 each weapon between sessions.)


------------------------------------------------------------------------------------------------
 3. PLAYING
------------------------------------------------------------------------------------------------

 1. Connect the headset (Link / Air Link / Virtual Desktop / SteamVR running).
 2. Start Dishonored 2 from Steam as usual. CLICK THE GAME'S WINDOW ON THE PC ONCE so it is the
    window in front (see the box below), then put the headset on.
 3. Menus show on a big screen in front of you, fixed in the room. Use the controllers: sticks
    move, A selects, B goes back, grips change tab, triggers change sub-tab, a stick click brings
    the screen back in front of you.
 4. Load or start a game. A moment after you are in control of your character VR switches on by
    itself: your hands appear and the view follows your head.
 5. Cutscenes happen around you: look wherever you like. To skip one, hold A or X for about
    3 seconds (the game's "Skip" ring fills up).

 +--------------------------------------------------------------------------------------------+
 | THE GAME MUST BE THE WINDOW IN FRONT ON THE PC WHILE YOU PLAY.                             |
 | The mod turns the controllers into the game's keyboard and mouse, and Windows only gives   |
 | those to the window in front. The mod brings the game back to the front by itself when you |
 | press a controller button or put the headset on. If Windows does not let it, click the     |
 | game's window on the PC once (or Alt+Tab to it).                                           |
 | IF YOUR LEFT CONTROLLER DOESN'T SEEM TO REGISTER INPUT, LIKELY THE WINDOW NEEDS FOCUS      |
 +--------------------------------------------------------------------------------------------+

 * Walk around your room and the character walks with you, so you rarely need to recenter
   (use your headset's own recenter: Quest, hold the Meta button). Walls stop the character;
   if your head goes on into a wall, the headset fades to dark blue until you step back out.
 * No headset connected? The game runs as the normal flat game. Connect it later and VR
   switches on by itself; if the connection drops, VR comes back when it reconnects.
 * The VR settings are in the game's Options, in the VR tabs after the game's own (section 6).


------------------------------------------------------------------------------------------------
 4. CONTROLS
------------------------------------------------------------------------------------------------

 Every controller plays the same way: the RIGHT TRIGGER INTERACTS, THE GRIPS ARE YOUR HANDS,
 THE SWORD STRIKES BY SWINGING YOUR ARM. The profile for your controllers is picked by itself
 (ControllerBindings = auto in DishonoredVR.ini; the profiles are in VRControllerBindings).

 GESTURES (every controller)
   Swing your right arm .............. strike with the sword: the blade hits what it touches,
                                       harder the faster it moves
   Fist by your right ear, blade
     forward, then thrust ............ power attack (lower the blade to cancel it)
   Right fist at your left shoulder,
     blade up (or the right grip) .... block
   Hit the back of a guard's head .... from behind, when the blue marker shows: knock him out
   Left trigger held, swing, let go .. throw a grenade or springrazor
   A grip at a holster ............... draw / put away: left hand at the right hip = pistol,
                                       behind your back = crossbow, chest = last power;
                                       right hand at the left hip = sword
   Right grip on an item, then reach
     over your shoulder .............. into your backpack (inventory), with a firm buzz
   Hand to your mouth, squeeze grip .. eat what it holds, or drink an elixir: left hand health,
                                       right hand mana
   Right grip on a body .............. drag it by what you took hold of; bring that hand up over
                                       your shoulder to put it on your back
   Push a door / drawer / window ..... it moves as far as you push it; grip a drawer and pull
                                       to open it; slide a hand up / down a window
   Press a button, grip a lever ...... press it, or squeeze the grip on it and pull
   Both hands on a ledge, push down .. climb it
   Sweep an arm back, in the water ... swim forward
   Tap your right hand against the
     side of your head ............... zoom (the spyglass); again: more; again: back to normal
   Look at the back of your left
     wrist, like a watch ............. health, mana, ammo, and a pointer on its rim to each
                                       objective (the top of the watch = straight ahead)
   Crouch, lean, walk for real ....... the character does too (there is no lean button)

 BUTTONS - Quest / Rift / Pico / Reverb G2 / Vive Cosmos / Focus 3 (A / B, X / Y, two sticks)
   Left stick ........................ move (click: sprint)
   Right stick ....................... turn (snap by default)   click: the sword's own attack
                                       button (hold: power attack)
   Right trigger ..................... interact: use / open / take what your right hand points at
   Left trigger ...................... power or left-hand weapon (pistol, crossbow)
   Right grip ........................ your hand: take, hold, grab, pull; with the sword out: block
   Left grip, held ................... power wheel (pick with the stick, let go to equip);
                                       with the wheel open: B / A drink a health / mana elixir,
                                       Y / X assign the highlighted item to a quick slot
   A / B ............................. jump / crouch (toggle; held while crouched: sit down)
   Y / X ............................. quick slot 1 / 2 (held half a second: slot 3 / 4)
   Menu button (left) ................ short: journal; held: pause menu (save / load)
   Both sticks pressed together ...... the same as the menu button

 BUTTONS - Valve Index
   As above, except:
   Left trackpad, press its edges .... quick slot 1 / 2 / 3 / 4 (top / right / bottom / left)
   Left trackpad, press its middle ... menu button (short: journal, held: pause)
   Right trackpad, press ............. the sword's own attack button (hold: power attack);
                                       in cutscenes: skip
   Left B / left A ................... drink a health / mana elixir
   Right A / right B ................. jump / crouch
   Your fingers ...................... the in-game fingers of a hand with a power or nothing in
                                       it move with yours
   The grips count how hard you SQUEEZE them, not fingers resting on them (GripThresholdLeft /
   GripThresholdRight in VRControllerBindings\index.json). If you changed the game's bindings in
   SteamVR, reset them there.

 BUTTONS - other controllers
   PS VR2 (through SteamVR): Triangle / Square = Y / X, Circle / Cross = B / A, L1 / R1 = grips,
     L2 / R2 = triggers. SteamVR does not pass the Create button to the game: press both sticks
     together for the journal and pause menu, or bind Create in SteamVR's controller bindings.
   Windows Mixed Reality / Samsung Odyssey: the trackpads work as the Index's; right stick click
     = jump, right menu button = crouch, left menu button (or the left trackpad's middle) = menu.
   HTC Vive / Vive Pro wands: the left trackpad walks, the right one turns (press: jump); right
     menu button = crouch, left menu button = menu. No quick slots: use the power wheel, the
     holsters and your mouth.

 IN MENUS
   Sticks: move   A: select   B or menu: back   Grips: next tab   Triggers: previous / next sub-tab
   Click either stick: the screen comes in front of you (it also does when you look away).
   In a shop: A buys, the grips switch Ammunition / Items, B goes back out.

 THE POINTERS: the right one shows what the right trigger uses or takes, the left one where a
 power or shot goes (ReticleOpacity sets how see-through they are).
 TAKEDOWNS: behind a guard who has not seen you, a small RED-AND-BLUE MARKER shows on the back of
 his neck. Swing the sword at him for the kill (red), or choke him with the block button (the
 game's own). Or hit the marker hard with your fist, the sword's pommel or the pistol: KNOCKOUT
 BLOW (blue), he is knocked out at once, with no animation.


------------------------------------------------------------------------------------------------
 5. WHAT YOU CAN DO
------------------------------------------------------------------------------------------------

 YOUR BODY
   * 6DOF: the view follows your head; walk around your room, lean, peek and crouch for real.
   * Sit down: hold B while crouched and standing still and the view goes down to sitting height
     to take in the view (SitHeight). B again, or walking off, stands up.
   * Comfort: snap or smooth turning, a vignette while the character moves (MovementVignette),
     Far Reach that takes you there at once like Blink, no head bob.

 TWO INDEPENDENT HANDS
   * The sword in the right hand; the pistol, crossbow, powers and grenades in the left, each
     aimed exactly where that hand points. Powers come out of your hand.
   * Holsters on your body for each hand, and a backpack over your shoulder for items.

 THE SWORD
   * The blade itself hits what it touches, harder the faster you swing (SwordDamageScale).
   * Gestures for the power attack and the block. Finishing moves and the kill cam are off, so
     the view never leaves your head (stealth takedowns keep theirs).
   * Bloodflies are easier to hit, and their nests can be struck.

 YOUR HANDS IN THE WORLD
   * Hands and weapons are solid: they stop at walls and push loose objects.
   * Doors swing only as far as you push them (push slowly to peek through a gap).
   * Open drawers and cabinets by pulling, slide windows, turn safe dials, press buttons and pull
     levers, take items, eat food and drink elixirs at your mouth, throw what you hold.
   * People's bodies give where you press on them, then spring back.
   * Drag bodies by an arm or a leg and lift them onto your back.
   * Climb ledges with both hands, swim with your arms.

 HUD, MENUS AND CUTSCENES
   * The HUD floats in 3D; health, mana, ammo and your objectives are on a watch on your wrist.
   * Menus and loading screens on a big screen in the room; the world stays around it, dimmed.
   * Cutscenes play around you, without black bars.


------------------------------------------------------------------------------------------------
 6. SETTINGS
------------------------------------------------------------------------------------------------

 IN THE GAME: Options has VR tabs after the game's own: VR Controls, VR Gameplay, VR Display,
 VR Performance and VR System/Cheats. Each setting shows the value recommended for most players.
 Going back from a VR tab after a change asks whether to apply it: Yes saves it and it takes
 effect at once (except those marked "restart"); No leaves everything as it was.

 IN THE FILE: DishonoredVR.ini in the game folder, opened with Notepad, explains every setting.
 Changes there apply the next time you start the game. 1 = on, 0 = off; deleting a line (or the
 whole file) gives you the default back. Settings marked @hidden are for tuning and
 troubleshooting: most players never need them.

 The settings most players may want to change:
   VR Controls     TurnMode = snap / smooth   SmoothTurnSpeed = 120 (deg/s)   SnapTurnAngle = 30
                   SnapTurnBlank = 1 (the view darkens for the moment of each snap)
                   MovementVignette = 0.6 (0 = off, 1 = strongest)   Haptics = 1
                   PhysicalCrouchDepth = 30 (cm your head goes down to crouch)   WallFade = 1
                   ControllerBindings = auto (or a profile's name: touch, index, wmr, ...)
   VR Gameplay     SwordDamageScale = 1   KnockoutSpeed = 8 (m/s a knockout blow needs)
                   KnockoutSoundVolume = 0.8 (your own DVR_knockout.wav in the game folder
                   replaces the sound)   FarReachVRComfortOption = 1
                   SitHoldTime = 0.8 (s)   SitHeight = 80 (cm)
                   DoorsFollowHand, SoftCharacters, DragBodies, TouchToUse, ClimbByHand,
                   SwimStrokes = 1 (each of the hand features above; 0 = off)
   VR Display      RenderScale = 1 (sharpness: lower = faster)   TaaSharpening (with the game's
                   TAA)   AmbientOcclusion = 0   ReticleOpacity = 0.4
                   HudDistance = 2 (m)   HudScale = 0.8   WristHudSize = 8.5 (cm)
                   WatchObjectives = 1   MenuScreenSize = 62 (deg)   MenuScreenDistance = 2.4 (m)
                   MenuBackground = 0.22   DisableMetaASW = 1 (Link / Air Link)
   VR Performance  FpsCounter = off (or a corner of your view)   ViewDistance = 3 (0 - 5)
                   ShadowCullDistance = 24 (m)   StaticLodZoom = 1.5   HudResolution = 0.5
                   FoveatedRendering = 0 (NVIDIA only)   OutOfViewMask = 2
   VR System       SkipIntroVideos = 1   DebugLogging = 0 (for problem reports, section 10)

 Each controller's own settings (grip sensitivity, finger tracking, the hands' pitch, the
 quick slots' hold time) are in its profile, VRControllerBindings\<name>.json, and in VR
 Controls: "notes" in each file explains them.


------------------------------------------------------------------------------------------------
 7. CHEATS
------------------------------------------------------------------------------------------------

 All off by default, in Options > VR System/Cheats or in DishonoredVR.ini ([Cheats]):
   InfiniteHealth, InfiniteMana, InfiniteAmmo (ammo, gadgets and elixirs are not used up)
   Undetected ........ guards and other hostile people never treat you as an enemy: walk among
                       them in the open without being attacked or hunted. Everyone else sees you
                       and reacts as usual
   UnlockEverything .. the Outsider's mark, every upgrade, bone charm crafting, every black market
                       upgrade and weapon (or each on its own: UnlockOutsidersMark,
                       UnlockPowerUpgrades, UnlockBlackMarketUpgrades, UnlockWeapons,
                       FillAmmoAndGadgets, FillElixirs)
   [Cheats.Powers] ... the powers one by one: Blink, Far Reach, Dark Vision, Possession, Devouring
                       Swarm, Bend Time, Windblast, Domino, Doppelganger, Mesmerize, Shadow Walk

 The cheats change your game for real: once the game saves, the save keeps them. Use saves you
 do not mind changing.


------------------------------------------------------------------------------------------------
 8. FRAME RATE
------------------------------------------------------------------------------------------------

 VR asks far more of the game than a monitor: every frame is drawn twice, once per eye, at the
 headset's high resolution. The mod does a lot to make the game faster; an NVIDIA RTX 5090 with an
 AMD Ryzen 7 9800X3D holds a steady 90 fps at 3700 x 3700 per eye. On other PCs:
   * Turn on the frame-rate counter (VR Performance > FpsCounter): green at the headset's refresh
     rate, amber below it, red under three quarters of it.
   * The biggest lever is the resolution: lower it in the headset's PC app (SteamVR's
     per-application resolution, the Meta Horizon Link app, Virtual Desktop), or RenderScale 0.8.
   * Then the VR Performance and VR Display tabs and the game's own graphics options (shadows,
     view distance, detail), watching the counter.


------------------------------------------------------------------------------------------------
 9. IF SOMETHING GOES WRONG
------------------------------------------------------------------------------------------------

 Nothing in the headset
   - Is your headset's app the active OpenXR runtime? (section 2, step 3)
   - Was the headset connected and its PC app running? (Connecting later also works.)
   - With Debug logging on (section 10), DishonoredVR.log says "OpenXR session not started (...)":
     the text in brackets says why.

 "Dishonored 2 VR is NOT active: this game build is not supported" at startup
   - Only the current Steam version of Dishonored 2 is supported (section 1).

 The game does not start at all on a PC with an AMD graphics card
   - Dishonored 2 itself (also without the mod) fails to start on some AMD setups and needs a
     replacement of its AMD AGS library (amd_ags_x64.dll). This is the game, not the mod.

 The controllers do nothing / the game paused by itself
   - Another window is in front of the game on the PC (section 3). Click the game's window once
     (or Alt+Tab to it), then put the headset back on.
   - A small window from the mod appeared on the PC screen and the game ignores the controllers:
     the Insert key opens it. Press Insert again to close it.

 Hands and weapons point too high or too low
   - VR Controls > ControllerPitchOffset (degrees, + = up), or in your controller's profile.
     Try steps of 10.

 Blurry picture
   - The game must be WINDOWED (Options > Visuals > Display mode), not fullscreen or borderless.
   - Raise the resolution in the headset's PC app, or RenderScale. With the game's TAA
     anti-aliasing: raise TaaSharpening (VR Display).

 Warped, "glitchy" edges around your hands or the world when you move fast
   - Your headset's app is inventing frames because the game runs below the headset's refresh.
     Meta (Link / Air Link): the mod turns ASW off (DisableMetaASW = 1); if it is still on,
     Ctrl+Numpad1 on the PC keyboard turns it off. SteamVR: Settings > Video > Per-application
     video settings > Dishonored 2 > Motion Smoothing off. Virtual Desktop: SSW off. Pimax:
     Pimax Play > Smart Smoothing off.

 An hourglass in the headset / the picture freezes
   - The headset's app stopped taking frames (often an Air Link hiccup). The game keeps running
     and VR comes back by itself. On Air Link: 5 or 6 GHz Wi-Fi, the router close and in sight,
     nothing else downloading -- or a Link cable.

 A laptop with two graphics cards: VR does not start, the log mentions "different GPU"
   - Windows Settings > System > Display > Graphics > Dishonored2.exe > High performance.

 Menus too small, too big or too close
   - VR Display > MenuScreenSize (degrees) and MenuScreenDistance.


------------------------------------------------------------------------------------------------
 10. REPORTING A PROBLEM  (thank you!)
------------------------------------------------------------------------------------------------

 First switch on the log: Options > VR System/Cheats > Troubleshooting > Debug logging
 (DebugLogging = 1 in DishonoredVR.ini; it lowers the frame rate a little, so turn it off again
 for playing), and make the problem happen again. Then send, right after it happened:
   - DishonoredVR.log and DishonoredVR.prev.log from the game folder
     (if they are not there: %LOCALAPPDATA%\DishonoredVR)
   - after a CRASH: the newest crash_log-<date>_<time>.log and its .dmp from the same folder.
     Options > VR System/Cheats > Crash log > "Copy to desktop" copies both to your desktop.
   - your headset and controllers, and how the headset is connected (Link, Air Link, Virtual
     Desktop, SteamVR)
   - what you did, what you expected, what happened (a photo or video helps a lot)
 The log contains your PC's hardware and the mod's state; no personal data beyond your Windows
 user name in file paths.


------------------------------------------------------------------------------------------------
 Third-party software in this mod: see THIRD-PARTY-NOTICES.txt.
------------------------------------------------------------------------------------------------
