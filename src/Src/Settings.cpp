#include "pch.h"
#include "Settings.h"
#include "GameInfo.h"
#include "StereoSync.h"
#include "ControllerBindings.h"
#include <map>
#include <algorithm>
#include <cmath>

namespace
{
    enum class Kind { Bool, Float, Choice, Text };
    struct Key
    {
        const char* name;          //!< "section.key", lower case
        Kind kind;
        const char* def;           //!< default, as it would be written in the file
        float lo, hi;              //!< range of a Float
        const char* choices;       //!< "a|b" for a Choice
        bool hidden = false;       //!< @hidden in the ini: most players never need to change it (else @user)
    };
    // The keys the file may hold ("group.key": the group here is only part of the name -- the file's own groups,
    // [Main.Sub], are found by the key alone). hidden: @hidden in the shipped file, else @user. The core VR features
    // (physical sword, reticles, roomscale, hand interactions, ...) are hidden too: always on, as for a native VR game;
    // their other value is left in for troubleshooting and development only. Their defaults here are the VR ones.
    // A missing key takes its default; the shipped DishonoredVR.ini lists them all, with
    // the same defaults, except WriteStatusFile ([Logging.Files]), which defaults to 1 here (a development setup has no ini)
    // and is 0 in the file the players get.
    const Key kKeys[] = {
        { "controls.turnmode",          Kind::Choice, "smooth", 0, 0, "smooth|snap" },
        { "controls.smoothturnspeed",   Kind::Float,  "120", 30, 360, nullptr },
        { "controls.snapturnangle",     Kind::Float,  "45", 10, 90, nullptr },
        { "controls.snapturnblank",     Kind::Bool,   "1", 0, 0, nullptr },   // KbmInject: black during a snap turn
        { "controls.snapturnblankopacity", Kind::Float, "0.5", 0, 1, nullptr, true },   // how dark: 1 = black
        { "controls.quickslotholdtime", Kind::Float,  "0.5", 0.15f, 2, nullptr },
        { "controls.layout",            Kind::Choice, "default", 0, 0, "default|classic|index", true },
        { "controls.haptics",           Kind::Bool,   "1", 0, 0, nullptr },
        { "controls.fingertracking",    Kind::Bool,   "1", 0, 0, nullptr, true },   // Index layout: the drawn fingers follow the controller's
        { "controls.gripthresholdleft", Kind::Float,  "0.6", 0.05f, 0.95f, nullptr },   // KbmInject: the left grip's press point
        { "controls.gripthresholdright", Kind::Float, "0.45", 0.05f, 0.95f, nullptr },  // ...the right grip's (more sensitive)
        { "controls.gripthreshold",     Kind::Float,  "0.6", 0.05f, 0.95f, nullptr, true },   // (old: both grips) used when the two above are absent
        { "controls.controllerpitchoffset", Kind::Float, "0", -60, 60, nullptr },
        // ControllerBindings: the profile in VRControllerBindings\ (auto = the controller's; or a file name) and the
        // trackpads' press (force past, let go below) and the left pad's centre (the menu button), a radius 0..1
        { "controls.controllerbindings", Kind::Text,  "auto", 0, 0, nullptr },
        { "controls.padpressforce",     Kind::Float,  "0.5", 0.05f, 1, nullptr, true },
        { "controls.padreleaseforce",   Kind::Float,  "0.3", 0.02f, 1, nullptr, true },
        { "controls.padcenterradius",   Kind::Float,  "0.45", 0.1f, 0.9f, nullptr, true },
        { "controls.focusgamewindow",   Kind::Bool,   "1", 0, 0, nullptr, true },
        { "controls.physicalcrouch",    Kind::Bool,   "1", 0, 0, nullptr, true },   // KbmInject: crouching for real crouches the character
        { "controls.physicalcrouchdepth", Kind::Float, "30", 10, 80, nullptr }, // cm the head goes under its standing height to crouch
        { "controls.roomscale",         Kind::Bool,   "1", 0, 0, nullptr, true },   // Roomscale: walking in the room walks the character
        { "controls.wallfade",          Kind::Bool,   "1", 0, 0, nullptr },   // Roomscale: the headset darkens with the head in a wall
        { "controls.wallfadecolor",     Kind::Text,   "20,34,82", 0, 0, nullptr, true },   // its colour, R,G,B 0-255
        { "controls.roomscaledeadzone", Kind::Float,  "10", 0, 50, nullptr, true },   // cm the head may be off the character before it follows
        { "controls.roomscalemaxspeed", Kind::Float,  "3", 0.5f, 10, nullptr, true }, // m/s at most
        { "gameplay.swordgestures",     Kind::Bool,   "1", 0, 0, nullptr, true },
        { "gameplay.swordbladedamage",  Kind::Bool,   "1", 0, 0, nullptr, true },   // MeleeAim: the blade's contact strikes by itself
        { "gameplay.sworddamagescale",  Kind::Float,  "1", 0.1f, 5, nullptr },
        { "gameplay.handcollision",     Kind::Bool,   "1", 0, 0, nullptr, true },   // Collision: the drawn hands stop at the world
        { "gameplay.weaponcollision",   Kind::Bool,   "1", 0, 0, nullptr, true },   // Collision: the held weapons too, and at characters' bodies
        { "gameplay.swordbodydepth",    Kind::Float,  "4", 0, 15, nullptr, true },  // cm the blade sinks into a body before it stops
        { "gameplay.pushobjects",       Kind::Bool,   "1", 0, 0, nullptr, true },   // Collision: loose props are pushed
        { "gameplay.pushdoors",         Kind::Bool,   "1", 0, 0, nullptr, true },   // Collision: a hand pushing a door opens it
        { "gameplay.knockoutblow",      Kind::Bool,   "1", 0, 0, nullptr, true },   // MeleeAim: a blow to the back of the head knocks a guard out
        { "gameplay.knockoutspeed",     Kind::Float,  "4.5", 1, 20, nullptr },   // MeleeAim: m/s a knockout blow needs
        { "gameplay.swordbloodflyreach", Kind::Float, "0.3", 0, 1, nullptr, true },   // SwordContact: how much further from a bloodfly the blade touches it
        { "gameplay.knockoutangle",     Kind::Float,  "60", 10, 120, nullptr, true },  // MeleeAim: deg off straight behind the target a knockout is possible from
        { "gameplay.knockoutradius",    Kind::Float,  "0.3", 0.1f, 0.8f, nullptr, true },   // MeleeAim: m around the head's centre a blow must land
        { "gameplay.knockoutsoundvolume", Kind::Float, "0.8", 0, 1, nullptr },   // MeleeAim: the hit sound of a knockout blow (0 = none)
        { "advanced.skipintrovideos",   Kind::Bool,   "0", 0, 0, nullptr },   // BinkVideo: the start-up videos end at once
        { "gameplay.finisheranimations", Kind::Bool,  "0", 0, 0, nullptr, true },   // MeleeAim: 0 = no finishing-move kill animations, no kill cam
        { "gameplay.pulltoopen",        Kind::Bool,   "1", 0, 0, nullptr, true },   // Collision + KbmInject: grab and pull drawers / cabinets open
        { "gameplay.physicalpickup",    Kind::Bool,   "1", 0, 0, nullptr, true },   // items taken by hand, stored behind the back
        { "gameplay.mouthpotions",      Kind::Bool,   "1", 0, 0, nullptr, true },   // KbmInject: a grip at the mouth drinks (left health, right Addermire)
        { "gameplay.swipedials",        Kind::Bool,   "1", 0, 0, nullptr, true },   // Collision: slide a hand up / down a safe dial to turn it
        { "gameplay.slidewindows",      Kind::Bool,   "1", 0, 0, nullptr, true },   // Collision: slide a hand up / down a window to raise / lower it
        { "gameplay.holsterweapons",    Kind::Bool,   "1", 0, 0, nullptr, true },   // Holster: left trigger at the left hip = pistol, behind the back = crossbow
        { "gameplay.pushstrength",      Kind::Float,  "1", 0.1f, 10, nullptr, true },   // the hand's effective mass, kg
        { "gameplay.throwgestures",     Kind::Bool,   "1", 0, 0, nullptr, true },
        { "gameplay.grabobjects",       Kind::Bool,   "1", 0, 0, nullptr, true },
        { "display.reticle",            Kind::Bool,   "1", 0, 0, nullptr, true },
        { "display.minimalreticle",     Kind::Bool,   "0", 0, 0, nullptr },   // Reticle: only a small dot where each hand points
        { "display.reticleopacity",     Kind::Float,  "0.5", 0.05f, 1, nullptr },
        { "display.stereohud",          Kind::Bool,   "1", 0, 0, nullptr, true },   // ScreenMode::PlayHud: the game's HUD on one panel in stereo
        { "display.huddistance",        Kind::Float,  "2", 0.5f, 10, nullptr },
        { "display.hudmarkers",         Kind::Bool,   "1", 0, 0, nullptr, true },
        { "display.screenspaceshadows", Kind::Bool,   "0", 0, 0, nullptr, true },   // ScreenShadows: the sun's shadows of what is on screen, traced in the depth buffer
        { "display.screenspaceshadowlength", Kind::Float, "3", 0.2f, 20, nullptr, true },   // m towards the sun
        { "display.screenspaceshadowsteps", Kind::Float, "24", 4, 64, nullptr, true },
        { "display.screenspaceshadowstrength", Kind::Float, "1", 0, 1, nullptr, true },
        { "display.screenspaceshadowcharacters", Kind::Bool, "1", 0, 0, nullptr, true },   // ScreenShadows: characters cast them
        { "display.shadowmapcharacters", Kind::Bool, "0", 0, 0, nullptr, true },           // ... and stay in the sun's shadow maps too
        { "display.screenspaceshadowcontact", Kind::Float, "0", 0, 2, nullptr, true },      // m: everything's contact shadows (0 = off)
        { "display.screenspaceshadowsmallobjects", Kind::Float, "1", 0, 4, nullptr, true }, // m across: smaller moving objects screen-space only (0 = none)
        { "display.shadowculldistance", Kind::Float, "0", 0, 200, nullptr },
        { "display.shadowmapminobjectsize", Kind::Float, "0.3", 0, 2, nullptr, true },
        { "display.shadowsunupdateevery", Kind::Float, "2", 1, 4, nullptr },   // frame pairs: the sun's far cascades' redraw interval   // m across: smaller moving objects leave the sun's maps (0 = none)   // ScreenShadows: r_shadowCullDistance held at this (0 = the game's)
        { "display.shadowultratrim",    Kind::Bool,   "1", 0, 0, nullptr },   // ScreenShadows: Ultra's costly non-sharpening settings held at High's
        { "display.shadowlampfewerupdates", Kind::Bool, "1", 0, 0, nullptr }, // ScreenShadows: the lamps' moving-caster updates at Low's thresholds
        { "display.shadowlampcacheslices", Kind::Float, "32", 0, 128, nullptr, true }, // ScreenShadows: r_shadowCacheSliceCnt (0 = the game's 12)
        { "display.staticlodzoom",      Kind::Float,  "1", 0.1f, 4, nullptr },  // VrTweaks: r_lodZoomMultiplicator held at this (1 = the game's)
        { "display.viewdistance",       Kind::Float,  "-1", -1, 5, nullptr },   // VrTweaks: the game's view-distance level instead of its menu's (-1 = the game's)
        { "display.depthpassdepthonly", Kind::Bool,   "0", 0, 0, nullptr, true },     // PassProfile/GpuTrace (experimental): the depth pass's main shader drawn depth-only
        { "display.depthpassskipredundantcopies", Kind::Bool, "1", 0, 0, nullptr, true }, // GpuTrace: a depth copy that changes nothing is not made
        { "display.skipemptyoutlines",  Kind::Bool,   "1", 0, 0, nullptr, true },   // PostShare: the outline effect's full-screen passes only with something outlined
        { "advanced.stereoexposurereuse", Kind::Bool, "1", 0, 0, nullptr, true },   // PostShare: the right eye adapts its exposure from the left eye's histogram
        { "display.hudscale",           Kind::Float,  "0.8", 0.3f, 1, nullptr },
        { "display.hudresolution",      Kind::Float,  "1", 0.25f, 1, nullptr },   // ScreenMode: the HUD panel's resolution, share of the picture's per side
        { "display.hudskipempty",       Kind::Bool,   "1", 0, 0, nullptr, true },     // ScreenMode / XRSession: the HUD panel only where it shows something
        { "display.hudmarkersize",      Kind::Float,  "0.07", 0, 0.25f, nullptr, true },   // ScreenMode::PlayHud: the HUD square around a marker shown at its character's distance   // ScreenMode::PlayHud: the HUD panel's share of the view (inward from the edges)   // ScreenMode: markers over characters / objectives placed for the headset
        { "display.camerapitchfix",     Kind::Bool,   "1", 0, 0, nullptr, true },   // ViewMatrixHook: no game neck on top of the real one
        { "display.maxrendermegapixels", Kind::Float, "0", 0, 200, nullptr, true },   // RenderRes: a budget for the render size (0 = only the headset's own limit)
        { "display.wristhud",           Kind::Bool,   "1", 0, 0, nullptr, true },
        { "display.wristhudsize",       Kind::Float,  "8.5", 4, 15, nullptr },
        // 0 mono, 1 synchronised stereo, 2 the alternating stereo of 1.0 (whose ini had 1 / true for that one: it gets
        // the synchronised one now, which is what 1 meant to the player)
        { "display.stereo3d",           Kind::Choice, "1", 0, 0, "0|1|2|true|false|on|off|yes|no", true },   // hidden: the mod is made for 1
        { "display.renderscale",        Kind::Float,  "1", 0, 2, nullptr },
        { "display.highresolution",     Kind::Bool,   "0", 0, 0, nullptr, true },   // 0.1.0's ini; replaced by RenderScale
        { "display.disablemotionblur",  Kind::Bool,   "1", 0, 0, nullptr, true },
        { "display.disablemetaasw",     Kind::Bool,   "1", 0, 0, nullptr },   // Meta's ASW off for this game (MetaAsw)
        { "display.vrgraphicstweaks",   Kind::Bool,   "1", 0, 0, nullptr, true },   // VrTweaks: no depth of field in VR
        { "display.skipsneakoverlay",   Kind::Bool,   "1", 0, 0, nullptr, true },   // PassProfile / GpuTrace: the sneak mode's screen overlay not drawn in the eyes
        { "display.ambientocclusion",   Kind::Bool,   "1", 0, 0, nullptr },   // VrTweaks: 0 = no ambient occlusion in VR
        { "display.fog",                Kind::Bool,   "0", 0, 0, nullptr, true },   // VrTweaks: 0 = no game fog in VR (but in FogMaps); hidden, core VR
        { "display.fogmaps",            Kind::Text,   "void", 0, 0, nullptr, true },   // VrTweaks: ...except in maps named so
        { "display.fogmapsresolution",  Kind::Float,  "1", 0, 1, nullptr, true },      // VrTweaks: the game's fog buffers there (1 full, 0.5, 0.25, 0 = the game's)
        { "display.cheapfog",           Kind::Bool,   "1", 0, 0, nullptr },      // Fog: the mod's own distance fog while the game's is off
        { "display.cheapfogstart",      Kind::Float,  "30", 0, 5000, nullptr },  // m where it begins
        { "display.cheapfogend",        Kind::Float,  "300", 1, 20000, nullptr },// m where it is at full opacity
        { "display.cheapfogopacity",    Kind::Float,  "0.6", 0, 1, nullptr },    // its strength at the far end
        { "display.cheapfogcolor",      Kind::Text,   "auto", 0, 0, nullptr },   // "auto" (night / dusk / day), "sky" or R,G,B (0-255)
        { "display.cheapfogcolornight", Kind::Text,   "70,80,100", 0, 0, nullptr, true },   // R,G,B[,brightness]
        { "display.cheapfogcolordusk",  Kind::Text,   "225,205,180", 0, 0, nullptr, true },
        { "display.cheapfogcolorday",   Kind::Text,   "250,255,252", 0, 0, nullptr, true },
        { "display.cheapfognightbelow", Kind::Float,  "0.1", 0, 100, nullptr, true },  // the upper sky's brightness under which a map is night
        { "display.cheapfogdayabove",   Kind::Float,  "0.5", 0, 100, nullptr, true },  // ...and over which it is day (dusk between)
        { "display.cheapfogbrightness", Kind::Float,  "1", 0, 100, nullptr, true },    // the fixed colour's multiplier (HDR)
        { "display.cheapfogminbrightness", Kind::Float, "0.35", 0, 100, nullptr, true }, // the fog's least brightness as seen (never dark)
        { "display.cheapfogfollowexposure", Kind::Bool, "1", 0, 0, nullptr, true },    // the fog follows the game's exposure (EngineLuminanceFactor)
        { "display.cheapfogskyheight",  Kind::Text,   "auto", 0, 0, nullptr, true },   // "auto" (up to the far background, then 10 deg more) or deg above the horizon (0: none)
        { "display.foveatedrendering",  Kind::Bool,   "0", 0, 0, nullptr },      // Foveation: NVIDIA VRS, coarser shading away from the lens centre
        { "display.foveatedinner",      Kind::Float,  "35", 5, 80, nullptr, true },    // deg from the centre at full shading
        { "display.foveatedouter",      Kind::Float,  "50", 5, 89, nullptr, true },    // deg to which 2x2 shading reaches (4x4 beyond)
        { "display.outofviewmask",      Kind::Float,  "0", 0, 2, nullptr },      // FovMask: 0 off, 1 all outside each eye's native fov not shaded, 2 only its outer side
        { "display.outofviewmaskmargin", Kind::Float, "2", 0, 15, nullptr, true },     // ...plus this many degrees round it (the compositor's reprojection)
        { "display.outofviewmasklens",  Kind::Bool,   "1", 0, 0, nullptr, true },      // ...and the lens's hidden area (XR_KHR_visibility_mask)
        { "display.outofviewmaskcorners", Kind::Float, "4", 0, 16, nullptr, true },    // ...and the fov's corners outside a superellipse of this exponent (0 = none)
        { "display.fpscounter",         Kind::Choice, "off", 0, 0, "off|top-left|top-right|bottom-left|bottom-right" },   // XRSession: a frame-rate counter in the headset
        { "display.fpscountersize",     Kind::Float,  "1", 0.5f, 2, nullptr, true },   // ...its size (1 = 20 cm wide at 1 m)
        { "display.fpscounterangle",    Kind::Float,  "15", 5, 30, nullptr, true },    // ...degrees from the view's centre, sideways (0.8 of it up / down)
        { "display.menuscreen",         Kind::Bool,   "1", 0, 0, nullptr, true },
        { "display.menuscreensize",     Kind::Float,  "62", 30, 110, nullptr },
        { "display.menuscreendistance", Kind::Float,  "2.4", 0.8f, 10, nullptr },
        { "display.menubackground",     Kind::Float,  "0.22", 0, 1, nullptr },
        { "display.cutsceneletterbox",  Kind::Bool,   "0", 0, 0, nullptr, true },
        { "display.cutscenearms",       Kind::Bool,   "0", 0, 0, nullptr, true },
        { "cheats.unlockeverything",    Kind::Bool,   "0", 0, 0, nullptr },
        // the parts of it, each on its own (TestAids::Unlock)
        { "cheats.unlockoutsidersmark", Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.unlockpowers",        Kind::Bool,   "0", 0, 0, nullptr, true },   // hidden (2026-11-01): the powers are chosen one by one ([Cheats.Powers])
        { "cheats.unlockpowerupgrades", Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.unlockblackmarketupgrades", Kind::Bool, "0", 0, 0, nullptr },
        { "cheats.unlockweapons",       Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.fillammoandgadgets",  Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.fillelixirs",         Kind::Bool,   "0", 0, 0, nullptr },
        // each power on its own ([Cheats.Powers]; Settings::PowerUnlocks: the parts of their item names)
        { "cheats.unlockblink",           Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.unlockfarreach",        Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.unlockdarkvision",      Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.unlockpossession",      Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.unlockdevouringswarm",  Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.unlockbendtime",        Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.unlockwindblast",       Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.unlockdomino",          Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.unlockdoppelganger",    Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.unlockmesmerize",       Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.unlockshadowwalk",      Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.infinitemana",        Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.infiniteammo",        Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.infinitehealth",      Kind::Bool,   "0", 0, 0, nullptr },
        { "advanced.keepaliveframes",   Kind::Bool,   "1", 0, 0, nullptr, true },
        { "advanced.newsessionafter",   Kind::Float,  "5", 0, 120, nullptr, true },
        // experimental (StereoSync): passes the right eye's frame skips, using what the left eye's frame of the same tick did
        { "advanced.stereooceanreuse",  Kind::Bool,   "0", 0, 0, nullptr, true },
        { "advanced.stereoreflectionreuse", Kind::Bool, "0", 0, 0, nullptr, true },
        { "advanced.stereodecalreuse",  Kind::Bool,   "0", 0, 0, nullptr, true },
        { "advanced.stereostreamonceperpair", Kind::Bool, "0", 0, 0, nullptr, true },
        { "advanced.stereolightshadowreuse", Kind::Bool, "0", 0, 0, nullptr, true },
        { "advanced.stereosunshadowreuse", Kind::Bool, "1", 0, 0, nullptr, true },   // StereoSync: the right eye keeps the left's nearest sun cascade
        { "advanced.skipdisabledeffects", Kind::Float, "0", 0, 2, nullptr, true },   // PassProfile: 1 = SSAO's leftover work skipped, 2 = also DOF's (experimental)
        // [Logging] (2026-11-01: moved from [Advanced]; the old place is still read: kMoved)
        { "logging.writelog",           Kind::Bool,   "1", 0, 0, nullptr, true },   // Log: DishonoredVR.log at all (0 = no file)
        { "logging.logcontrollerinput", Kind::Bool,   "1", 0, 0, nullptr, true },
        { "logging.writestatusfile",    Kind::Bool,   "1", 0, 0, nullptr, true },
        { "logging.profilerenderpasses", Kind::Bool,  "0", 0, 0, nullptr, true },
        { "advanced.vroptionsmenu",     Kind::Bool,   "1", 0, 0, nullptr, true },   // VrMenu: VR tabs in the game's Options screen
        { "logging.logmenudata",        Kind::Bool,   "0", 0, 0, nullptr, true },   // MenuProbe: the options screen's Flash traffic, to the log   // PassProfile: GPU ms per render pass, to the log
    };

    std::map<std::string, std::string> g_Val;     //!< validated values, every key present (never inserted into after Load)
    std::map<std::string, std::string> g_Pending; //!< saved from the VR options tabs, used after a restart (kRestart)
    std::map<std::string, std::string> g_Source;  //!< bare name -> the controller profile that set it ("index.json")
    SRWLOCK g_Lock = SRWLOCK_INIT;                //!< g_Val's values / g_Pending: the VR options tabs change them while others read
    std::map<std::string, bool> g_Given;          //!< the keys the file set (validly)
    bool g_FileFound = false;
    int g_Read = 0, g_Bad = 0;

    std::string Lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); }); return s; }
    std::string Trim(const std::string& s)
    {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }
    bool ParseBool(const std::string& v, bool& out)
    {
        const std::string l = Lower(v);
        if (l == "1" || l == "true" || l == "on" || l == "yes") { out = true; return true; }
        if (l == "0" || l == "false" || l == "off" || l == "no") { out = false; return true; }
        return false;
    }
    //! The value for a key as the file has it, checked against the key's type; false = not usable.
    bool Validate(const Key& k, const std::string& raw, std::string& out)
    {
        if (k.kind == Kind::Bool) { bool b; if (!ParseBool(raw, b)) return false; out = b ? "1" : "0"; return true; }
        if (k.kind == Kind::Text) { out = Lower(Trim(raw)); return true; }      // free text (may be empty)
        if (k.kind == Kind::Float)
        {
            // Decimals with a point; a comma (how a Spanish or German Windows writes them) is taken as a point too.
            std::string r = raw; std::replace(r.begin(), r.end(), ',', '.');
            char* end = nullptr; const double d = strtod(r.c_str(), &end);
            if (end == r.c_str() || Trim(end).size()) return false;
            const float f = (float)(d < k.lo ? k.lo : d > k.hi ? k.hi : d);
            if (f != (float)d) LOGI("Settings: {} = {} is outside {}..{}: using {}", k.name, raw, k.lo, k.hi, f);
            out = fmt::format("{}", f); return true;
        }
        const std::string l = Lower(raw), all = std::string("|") + k.choices + "|";
        if (l.empty() || l.find('|') != std::string::npos || all.find("|" + l + "|") == std::string::npos) return false;
        out = l; return true;
    }
    //! Several threads ask, and the VR options tabs may change a value meanwhile (Settings::Save): a copy, under the lock.
    //! Never inserts.
    std::string V(const char* key)
    {
        AcquireSRWLockShared(&g_Lock);
        auto it = g_Val.find(key);
        std::string v = it != g_Val.end() ? it->second : std::string();
        const bool found = it != g_Val.end();
        ReleaseSRWLockShared(&g_Lock);
        if (found) return v;
        for (const Key& k : kKeys) if (!strcmp(k.name, key)) return k.def;
        return std::string();
    }
    bool B(const char* key) { return V(key) == "1"; }
    float F(const char* key) { return (float)atof(V(key).c_str()); }
}

void Settings::Load()
{
    for (const Key& k : kKeys) g_Val[k.name] = k.def;
    const std::wstring path = GameInfo::GameDir() + L"\\DishonoredVR.ini";
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) { LOGI("Settings: no DishonoredVR.ini in the game folder: every setting at its default"); return; }
    g_FileFound = true;
    std::string text; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.erase(0, 3);
    if (text.size() >= 2 && (unsigned char)text[0] == 0xFF && (unsigned char)text[1] == 0xFE)
        LOGI("Settings: DishonoredVR.ini is saved as UTF-16 and cannot be read: save it as UTF-8 or ANSI (Notepad: Save As > Encoding)");
    std::string section; int lineNo = 0; size_t pos = 0;
    while (pos <= text.size())
    {
        size_t eol = text.find('\n', pos); if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos); pos = eol + 1; lineNo++;
        // A comment starts with ; or #, at the start of a line or after the value.
        const size_t c = line.find_first_of(";#"); if (c != std::string::npos) line.erase(c);
        line = Trim(line);
        if (line.empty()) continue;
        if (line.front() == '[')
        {
            const size_t close = line.find(']');
            section = Lower(Trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) { LOGI("Settings: line {} ignored (no '='): {}", lineNo, line); g_Bad++; continue; }
        // A setting is found by its name alone, whichever group it is under (2026-11-01: the file is in main groups and
        // subgroups, [Display.Fog]; the names are unique): an older file, with the old sections, still reads
        const std::string bare = Lower(Trim(line.substr(0, eq)));
        const std::string raw = Trim(line.substr(eq + 1));
        const Key* k = nullptr;
        for (const Key& kk : kKeys) if (bare == strchr(kk.name, '.') + 1) { k = &kk; break; }
        if (!k) { LOGI("Settings: line {}: unknown setting '{}' (in [{}]) ignored", lineNo, bare, section); g_Bad++; continue; }
        std::string v;
        if (!Validate(*k, raw, v)) { LOGI("Settings: line {}: {} = '{}' is not a valid value: keeping the default '{}'", lineNo, k->name, raw, k->def); g_Bad++; continue; }
        g_Val[k->name] = v; g_Given[k->name] = true; g_Read++;
    }
    int hid = 0; for (const Key& kk : kKeys) hid += kk.hidden ? 1 : 0;
    LOGI("Settings: DishonoredVR.ini read: {} settings, {} lines ignored ({} settings known: {} @user, {} @hidden)", g_Read, g_Bad,
         (int)(sizeof kKeys / sizeof kKeys[0]), (int)(sizeof kKeys / sizeof kKeys[0]) - hid, hid);
}

std::vector<std::string> Settings::StartupCommands()
{
    std::vector<std::string> c;
    c.push_back(V("controls.turnmode") == "snap" ? fmt::format("vr snap {}", F("controls.snapturnangle")) : "vr snap 0");
    c.push_back(fmt::format("vr turnspeed {}", F("controls.smoothturnspeed")));
    c.push_back(fmt::format("vr quickhold {}", F("controls.quickslotholdtime")));
    // default = Touch-style (Y / X quick slots), classic = 1.0's (Y lean, X interact), index = the Index controllers'
    // trackpads (left: quick slots and menu, right: interact; left B / A potions); index on any other controller = default
    {
        const std::string& l = V("controls.layout");
        c.push_back(l == "classic" ? "vr quickslots 0" : l == "index" ? "vr quickslots 2" : "vr quickslots 1");
        // index: an object the game holds (a bottle, a cup) stays in the hand while the grip is held and is dropped (or
        // thrown, by the hand's speed) when it opens -- reported, a second squeeze to let go felt like a toggle
        c.push_back(l == "index" ? "vr grabrelease 1" : "vr grabrelease 2");
    }
    // the game brought to the front when the controllers are used while it is behind another window (KbmInject)
    c.push_back(B("controls.focusgamewindow") ? "vr autofocus 1" : "vr autofocus 0");
    // the menu screen is there from the title screen on, before any game is loaded (ScreenMode)
    c.push_back(B("display.menuscreen") ? "vr screen 1" : "vr screen 0");
    c.push_back(fmt::format("vr screencfg size {}", F("display.menuscreensize")));
    c.push_back(fmt::format("vr screencfg dist {}", F("display.menuscreendistance")));
    c.push_back(fmt::format("vr screencfg backdrop {}", F("display.menubackground")));
    // cutscenes in the engine: the game's letterbox, and whose arms
    c.push_back(B("display.cutsceneletterbox") ? "vr cinebars 0" : "vr cinebars 1");
    c.push_back(B("display.cutscenearms") ? "vr cinearms 1" : "vr cinearms 0");
    // the headset's frames when the game or the runtime stalls
    c.push_back(B("advanced.keepaliveframes") ? "vr keepalive 1" : "vr keepalive 0");
    c.push_back(fmt::format("vr xrkick {}", F("advanced.newsessionafter")));
    c.push_back(B("display.disablemetaasw") ? "vr metaasw 1" : "vr metaasw 0");
    return c;
}

std::vector<std::string> Settings::PackageCommands()
{
    std::vector<std::string> c;
    c.push_back(B("gameplay.swordgestures") ? "vr swing 7" : "vr swing 0");
    c.push_back(B("gameplay.throwgestures") ? "vr throw 1" : "vr throw 0");
    c.push_back(B("gameplay.grabobjects") ? "vr grab 1" : "vr grab 0");
    c.push_back(B("display.reticle") ? "vr reticle 1" : "vr reticle 0");
    c.push_back(B("display.wristhud") ? "vr wristhud 1" : "vr wristhud 0");
    c.push_back(fmt::format("vr wristcfg size {}", F("display.wristhudsize") / 100.f));   // cm in the file, m in the command
    {
        const std::string& st = V("display.stereo3d");
        if (st == "1" || st == "true" || st == "on" || st == "yes") c.push_back("vr stereo 2");   // both eyes of one instant
        else if (st == "2") c.push_back("vr stereo 1");                                              // alternating eyes
    }
    // Camera motion blur smears the whole view each time the head-driven camera turns: off in VR (the game keeps it
    // in its own config afterwards, as if it had been unticked in the options).
    if (B("display.disablemotionblur")) c.push_back("exec r_skipCameraMotionBlur 1");
    // (UnlockEverything and its parts: TestAids::SetAutoUnlock, from Main -- since 2026-11-01 no longer `vr autoall 1`,
    // which also switched the infinites on before the lines below switched them as the ini says)
    c.push_back(B("cheats.infinitemana") ? "vr mana inf" : "vr mana 0");
    c.push_back(B("cheats.infiniteammo") ? "vr ammo inf" : "vr ammo 0");   // the game's own counter: ammo, gadgets and elixirs
    c.push_back(B("cheats.infiniteammo") ? "vr infitems 1" : "vr infitems 0");   // + the top-up of what was held
    if (B("cheats.infinitehealth")) c.push_back("vr hp inf");
    return c;
}

bool Settings::StatusFile() { return B("logging.writestatusfile"); }
bool Settings::WriteLog() { return B("logging.writelog"); }
bool Settings::LogInput() { return B("logging.logcontrollerinput"); }
bool Settings::Haptics() { return B("controls.haptics"); }
float Settings::RenderScale() { return F("display.renderscale"); }
float Settings::ControllerPitchOffset() { return F("controls.controllerpitchoffset"); }
bool Settings::FingerTracking() { return B("controls.fingertracking") && V("controls.layout") == "index"; }
bool Settings::SwordBladeDamage() { return B("gameplay.swordbladedamage"); }
float Settings::SwordDamageScale() { return F("gameplay.sworddamagescale"); }
bool Settings::HandCollision() { return B("gameplay.handcollision"); }
bool Settings::WeaponCollision() { return B("gameplay.weaponcollision"); }
bool Settings::PushObjects() { return B("gameplay.pushobjects"); }
bool Settings::PushDoors() { return B("gameplay.pushdoors"); }
bool Settings::KnockoutBlow() { return B("gameplay.knockoutblow"); }
bool Settings::FinisherAnimations() { return B("gameplay.finisheranimations"); }
float Settings::KnockoutSoundVolume() { return F("gameplay.knockoutsoundvolume"); }
float Settings::KnockoutSpeed() { return F("gameplay.knockoutspeed"); }
float Settings::KnockoutAngle() { return F("gameplay.knockoutangle"); }
float Settings::SwordBloodflyReach() { return F("gameplay.swordbloodflyreach"); }
float Settings::KnockoutRadius() { return F("gameplay.knockoutradius"); }
bool Settings::SkipIntroVideos() { return B("advanced.skipintrovideos"); }
bool Settings::PhysicalCrouch() { return B("controls.physicalcrouch"); }
bool Settings::Roomscale() { return B("controls.roomscale"); }
std::string Settings::ControllerBindings() { return V("controls.controllerbindings"); }
float Settings::PadPressForce() { return F("controls.padpressforce"); }
float Settings::PadReleaseForce() { return std::min(F("controls.padreleaseforce"), F("controls.padpressforce") - 0.02f); }
float Settings::PadCenterRadius() { return F("controls.padcenterradius"); }
bool Settings::WallFade() { return B("controls.wallfade"); }
void Settings::WallFadeColor(float srgb[3])
{
    int r = 20, g = 34, b = 82;
    const std::string v = V("controls.wallfadecolor");
    if (sscanf_s(v.c_str(), "%d , %d , %d", &r, &g, &b) != 3) { LOGI("Settings: WallFadeColor = '{}' is not R,G,B: 20,34,82", v); r = 20; g = 34; b = 82; }
    const int c[3] = { r, g, b };
    for (int i = 0; i < 3; i++) srgb[i] = std::min(255, std::max(0, c[i])) / 255.f;
}
float Settings::RoomscaleDeadzone() { return F("controls.roomscaledeadzone") / 100.f; }
float Settings::RoomscaleMaxSpeed() { return F("controls.roomscalemaxspeed"); }
float Settings::PhysicalCrouchDepth() { return F("controls.physicalcrouchdepth") / 100.f; }
bool Settings::PullToOpen() { return B("gameplay.pulltoopen"); }
bool Settings::PhysicalPickup() { return B("gameplay.physicalpickup"); }
bool Settings::MouthPotions() { return B("gameplay.mouthpotions"); }
std::string Settings::PowerUnlocks()
{
    // the part of each power's inventory item name (models/powers/<x>/<x>.inventoryitem; ENGINE.md 10.1)
    static const std::pair<const char*, const char*> kPowers[] = {
        { "cheats.unlockblink", "blink" },
        { "cheats.unlockfarreach", "grapple" },
        { "cheats.unlockdarkvision", "darkvision" },
        { "cheats.unlockpossession", "possession" },
        { "cheats.unlockdevouringswarm", "devouring_swarm" },
        { "cheats.unlockbendtime", "bendtime" },
        { "cheats.unlockwindblast", "windblast" },
        { "cheats.unlockdomino", "phantasmal_link" },
        { "cheats.unlockdoppelganger", "doppelganger" },
        { "cheats.unlockmesmerize", "mesmerize" },
        { "cheats.unlockshadowwalk", "shadow_walk" },
    };
    std::string s;
    for (const auto& p : kPowers) if (B(p.first)) s += (s.empty() ? "" : ",") + std::string(p.second);
    return s;
}
unsigned Settings::Unlocks()
{
    // UnlockEverything: everything but the powers (2026-11-01: one switch for all powers next to a list of the single
    // powers was confusing -- the powers come from [Cheats.Powers] only; the hidden UnlockPowers still adds them all)
    if (B("cheats.unlockeverything")) return (127u & ~2u) | (B("cheats.unlockpowers") ? 2u : 0u);
    return (B("cheats.unlockoutsidersmark") ? 1u : 0u) | (B("cheats.unlockpowers") ? 2u : 0u) | (B("cheats.unlockpowerupgrades") ? 4u : 0u)
         | (B("cheats.unlockblackmarketupgrades") ? 8u : 0u) | (B("cheats.unlockweapons") ? 16u : 0u) | (B("cheats.fillammoandgadgets") ? 32u : 0u)
         | (B("cheats.fillelixirs") ? 64u : 0u);
}
bool Settings::SwipeDials() { return B("gameplay.swipedials"); }
bool Settings::SlideWindows() { return B("gameplay.slidewindows"); }
bool Settings::HolsterWeapons() { return B("gameplay.holsterweapons"); }
float Settings::SwordBodyDepth() { return F("gameplay.swordbodydepth") / 100.f; }
float Settings::PushStrength() { return F("gameplay.pushstrength"); }
float Settings::GripThreshold(int hand)
{
    const char* key = hand ? "controls.gripthresholdright" : "controls.gripthresholdleft";
    if (!g_Given.count(key) && g_Given.count("controls.gripthreshold")) return F("controls.gripthreshold");
    return F(key);
}
bool Settings::SnapTurnBlank() { return B("controls.snapturnblank"); }
float Settings::SnapTurnBlankOpacity() { return F("controls.snapturnblankopacity"); }
bool Settings::StereoHud() { return B("display.stereohud"); }
float Settings::HudDistance() { return F("display.huddistance"); }
bool Settings::HudMarkers() { return B("display.hudmarkers"); }
bool Settings::ScreenSpaceShadows() { return B("display.screenspaceshadows"); }
float Settings::ScreenSpaceShadowLength() { return F("display.screenspaceshadowlength"); }
int Settings::ScreenSpaceShadowSteps() { return (int)std::lround(F("display.screenspaceshadowsteps")); }
float Settings::ScreenSpaceShadowStrength() { return F("display.screenspaceshadowstrength"); }
bool Settings::ScreenSpaceShadowCharacters() { return B("display.screenspaceshadowcharacters"); }
bool Settings::ShadowMapCharacters() { return B("display.shadowmapcharacters"); }
float Settings::ScreenSpaceShadowContact() { return F("display.screenspaceshadowcontact"); }
float Settings::ScreenSpaceShadowSmallObjects() { return F("display.screenspaceshadowsmallobjects"); }
float Settings::ShadowCullDistance() { return F("display.shadowculldistance"); }
bool Settings::ShadowUltraTrim() { return B("display.shadowultratrim"); }
bool Settings::ShadowLampFewerUpdates() { return B("display.shadowlampfewerupdates"); }
int Settings::ShadowLampCacheSlices() { return (int)std::lround(F("display.shadowlampcacheslices")); }
float Settings::StaticLodZoom() { return F("display.staticlodzoom"); }
int Settings::ViewDistance() { return (int)std::lround(F("display.viewdistance")); }
bool Settings::DepthPassDepthOnly() { return B("display.depthpassdepthonly"); }
bool Settings::DepthPassSkipRedundantCopies() { return B("display.depthpassskipredundantcopies"); }
bool Settings::SkipEmptyOutlines() { return B("display.skipemptyoutlines"); }
bool Settings::StereoExposureReuse() { return B("advanced.stereoexposurereuse"); }
float Settings::ShadowMapMinObjectSize() { return F("display.shadowmapminobjectsize"); }
int Settings::ShadowSunUpdateEvery() { return (int)std::lround(F("display.shadowsunupdateevery")); }
float Settings::HudScale() { return F("display.hudscale"); }
float Settings::HudResolution() { return F("display.hudresolution"); }
bool Settings::HudSkipEmpty() { return B("display.hudskipempty"); }
float Settings::HudMarkerSize() { return F("display.hudmarkersize"); }
bool Settings::CameraPitchFix() { return B("display.camerapitchfix"); }
float Settings::MaxRenderMegapixels() { return F("display.maxrendermegapixels"); }
bool Settings::SkipSneakOverlay() { return B("display.skipsneakoverlay"); }
float Settings::FogMapsResolution() { return F("display.fogmapsresolution"); }
bool Settings::MinimalReticle() { return B("display.minimalreticle"); }
float Settings::ReticleOpacity() { return F("display.reticleopacity"); }
bool Settings::VrGraphicsTweaks() { return B("display.vrgraphicstweaks"); }
bool Settings::AmbientOcclusion() { return B("display.ambientocclusion"); }
bool Settings::Fog() { return B("display.fog"); }
bool Settings::ProfileRenderPasses() { return B("logging.profilerenderpasses"); }
bool Settings::LogMenuData() { return B("logging.logmenudata"); }
bool Settings::VrOptionsMenu() { return B("advanced.vroptionsmenu"); }
int Settings::SkipDisabledEffects() { return (int)std::lround(F("advanced.skipdisabledeffects")); }
int Settings::OutOfViewMask() { return (int)std::lround(F("display.outofviewmask")); }
float Settings::OutOfViewMaskMargin() { return F("display.outofviewmaskmargin"); }
bool Settings::OutOfViewMaskLens() { return B("display.outofviewmasklens"); }
float Settings::OutOfViewMaskCorners() { return F("display.outofviewmaskcorners"); }
int Settings::FpsCounter()
{
    const std::string v = V("display.fpscounter");
    return v == "top-left" ? 1 : v == "top-right" ? 2 : v == "bottom-left" ? 3 : v == "bottom-right" ? 4 : 0;
}
float Settings::FpsCounterSize() { return F("display.fpscountersize"); }
float Settings::FpsCounterAngle() { return F("display.fpscounterangle"); }
Foveation::Params Settings::Foveated()
{
    Foveation::Params p;
    p.on = B("display.foveatedrendering");
    p.innerDeg = F("display.foveatedinner"); p.outerDeg = std::max(p.innerDeg, F("display.foveatedouter"));
    return p;
}
std::string Settings::FogMaps() { return V("display.fogmaps"); }
Fog::Params Settings::CheapFog()
{
    Fog::Params p;
    p.on = B("display.cheapfog");
    p.start = F("display.cheapfogstart"); p.end = std::max(p.start + 1.f, F("display.cheapfogend")); p.opacity = F("display.cheapfogopacity");
    p.brightness = F("display.cheapfogbrightness");
    p.minBrightness = F("display.cheapfogminbrightness"); p.followExposure = B("display.cheapfogfollowexposure");
    {
        const std::string& h = V("display.cheapfogskyheight");
        float deg = 0;
        if (h.empty() || _stricmp(h.c_str(), "auto") == 0) { p.skyAuto = true; p.skyHeight = 10.f; }
        else if (sscanf_s(h.c_str(), "%f", &deg) == 1) { p.skyAuto = false; p.skyHeight = std::clamp(deg, 0.f, 90.f); }
        else { p.skyAuto = true; p.skyHeight = 10.f; }
    }
    // "R,G,B[,brightness]": sRGB 0-255 to linear (the scene is in linear light); brightness 0 = not given
    auto colour = [](const std::string& s, float out[4]) -> bool
    {
        int r = 0, g = 0, b = 0; float k = 0;
        const int n = sscanf_s(s.c_str(), "%d , %d , %d , %f", &r, &g, &b, &k);
        if (n < 3) return false;
        const int v[3] = { r, g, b };
        for (int i = 0; i < 3; i++)
        {
            const float x = std::min(255, std::max(0, v[i])) / 255.f;
            out[i] = x <= 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f);
        }
        out[3] = n >= 4 ? std::max(0.f, k) : 0.f;
        return true;
    };
    const char* catKeys[3] = { "display.cheapfogcolornight", "display.cheapfogcolordusk", "display.cheapfogcolorday" };
    for (int k = 0; k < 3; k++)
        if (!colour(V(catKeys[k]), p.cat[k])) LOGI("Settings: {} = '{}' is not R,G,B[,brightness]: its default", catKeys[k], V(catKeys[k]));
    p.nightBelow = F("display.cheapfognightbelow"); p.dayAbove = std::max(p.nightBelow, F("display.cheapfogdayabove"));
    const std::string& c = V("display.cheapfogcolor");
    float fixed[4];
    p.sky = false; p.category = false;
    if (_stricmp(c.c_str(), "sky") == 0) p.sky = true;
    else if (colour(c, fixed)) { memcpy(p.rgb, fixed, 12); if (fixed[3] > 0) p.brightness = fixed[3]; }
    else
    {
        if (_stricmp(c.c_str(), "auto") != 0) LOGI("Settings: CheapFogColor = '{}' is not auto, sky or R,G,B: auto", c);
        p.category = true;
    }
    return p;
}
StereoSync::ReuseFlags Settings::StereoReuse()
{
    StereoSync::ReuseFlags f;
    f.ocean = B("advanced.stereooceanreuse");
    f.reflections = B("advanced.stereoreflectionreuse"); f.decals = B("advanced.stereodecalreuse");
    f.streaming = B("advanced.stereostreamonceperpair");
    f.lightShadows = B("advanced.stereolightshadowreuse");
    f.sunShadows = B("advanced.stereosunshadowreuse");
    return f;
}

std::string Settings::Summary()
{
    std::string s = g_FileFound ? "DishonoredVR.ini:" : "defaults (no DishonoredVR.ini):";
    for (const Key& k : kKeys) s += fmt::format(" {}={}", strchr(k.name, '.') + 1, V(k.name));
    return s;
}

bool Settings::IsHidden(const std::string& name)
{
    const std::string n = Lower(name);
    for (const Key& k : kKeys) if (n == k.name || n == strchr(k.name, '.') + 1) return k.hidden;
    return false;
}

namespace
{
    // The value recommended for most players, per @user setting: the shipped ini says it on each one's tag line
    // ("; @user  recommended: ..."), the VR options tabs show it (VrMenu). Keep the two the same.
    const std::pair<const char*, const char*> kRecommended[] = {
        { "turnmode", "snap" },
        { "smoothturnspeed", "120" },
        { "snapturnangle", "20" },
        { "snapturnblank", "1" },
        { "quickslotholdtime", "0.5" },
        { "haptics", "1" },
        { "gripthresholdleft", "0.6" },
        { "gripthresholdright", "0.45" },
        { "controllerpitchoffset", "0" },
        { "physicalcrouchdepth", "30" },
        { "wallfade", "1" },
        { "sworddamagescale", "1" },
        { "knockoutspeed", "8" },
        { "knockoutsoundvolume", "0.8" },
        { "renderscale", "1" },
        { "disablemetaasw", "1" },
        { "ambientocclusion", "0" },
        { "minimalreticle", "1" },
        { "reticleopacity", "0.4" },
        { "huddistance", "2" },
        { "hudscale", "0.8" },
        { "wristhudsize", "8.5" },
        { "menuscreensize", "62" },
        { "menuscreendistance", "2.4" },
        { "menubackground", "0.22" },
        { "cheapfog", "1" },
        { "cheapfogstart", "160" },
        { "cheapfogend", "400" },
        { "cheapfogopacity", "0.6" },
        { "cheapfogcolor", "auto" },
        { "shadowculldistance", "32" },
        { "shadowsunupdateevery", "2" },
        { "shadowultratrim", "1" },
        { "shadowlampfewerupdates", "1" },
        { "viewdistance", "3" },
        { "staticlodzoom", "1.5" },
        { "hudresolution", "0.5" },
        { "foveatedrendering", "0" },
        { "outofviewmask", "2" },
        { "fpscounter", "off" },
        { "unlockeverything", "0" },
        { "unlockoutsidersmark", "0" },
        { "unlockpowerupgrades", "0" },
        { "unlockblackmarketupgrades", "0" },
        { "unlockweapons", "0" },
        { "fillammoandgadgets", "0" },
        { "fillelixirs", "0" },
        { "unlockblink", "0" },
        { "unlockfarreach", "0" },
        { "unlockdarkvision", "0" },
        { "unlockpossession", "0" },
        { "unlockdevouringswarm", "0" },
        { "unlockbendtime", "0" },
        { "unlockwindblast", "0" },
        { "unlockdomino", "0" },
        { "unlockdoppelganger", "0" },
        { "unlockmesmerize", "0" },
        { "unlockshadowwalk", "0" },
        { "infinitemana", "1" },
        { "infiniteammo", "1" },
        { "infinitehealth", "1" },
        { "skipintrovideos", "1" },
        { "controllerbindings", "auto" }
    };
    // The @user settings a change of which only takes effect when the game is started again (VrMenu labels them, and
    // Settings::Save keeps their new value for then): read once when the headset's session or the swapchain is made
    // (Stereo3D: the VR package's command, run once a game is first entered; RenderScale), a hook installed or not at
    // startup (HudMarkers), only used while starting (SkipIntroVideos), or applied by the game when it applies its own
    // view distance (ViewDistance). (ControllerPitchOffset and FingerTracking: given to the session at once since
    // 2026-11-01, as a controller profile can change them after it started.)
    const char* const kRestart[] = { "stereo3d", "renderscale", "hudmarkers", "skipintrovideos", "viewdistance" };
    bool Restart(const std::string& bare)
    {
        for (const char* r : kRestart) if (bare == r) return true;
        return false;
    }
}

bool Settings::Info(const std::string& name, KeyInfo& out)
{
    const std::string n = Lower(name);
    for (const Key& k : kKeys)
        if (n == k.name || n == strchr(k.name, '.') + 1)
        {
            out.name = strchr(k.name, '.') + 1;
            out.kind = k.kind == Kind::Bool ? 0 : k.kind == Kind::Float ? 1 : k.kind == Kind::Choice ? 2 : 3;
            out.value = V(k.name);
            AcquireSRWLockShared(&g_Lock);
            if (auto it = g_Pending.find(k.name); it != g_Pending.end()) out.value = it->second;   // saved, for after a restart
            ReleaseSRWLockShared(&g_Lock);
            out.restart = Restart(out.name);
            AcquireSRWLockShared(&g_Lock);
            if (auto it = g_Source.find(out.name); it != g_Source.end()) out.source = it->second;
            ReleaseSRWLockShared(&g_Lock);
            out.def = k.def; out.lo = k.lo; out.hi = k.hi; out.choices = k.choices ? k.choices : ""; out.hidden = k.hidden;
            out.rec.clear();
            for (const auto& r : kRecommended) if (out.name == r.first) { out.rec = r.second; break; }
            return true;
        }
    return false;
}

bool Settings::Save(const std::vector<std::pair<std::string, std::string>>& changes, std::vector<std::string>& live, std::string& err)
{
    live.clear(); err.clear();
    struct Change { const Key* k; std::string name, value; bool written = false, json = false; };
    std::vector<Change> todo;
    for (const auto& c : changes)
    {
        const std::string bare = Lower(c.first);
        const Key* k = nullptr;
        for (const Key& kk : kKeys) if (bare == strchr(kk.name, '.') + 1) { k = &kk; break; }
        std::string v;
        if (!k || !Validate(*k, c.second, v)) { LOGI("Settings: save: {} = '{}' is not a setting / not a valid value: not saved", c.first, c.second); continue; }
        todo.push_back({ k, c.first, v });
    }
    if (todo.empty()) return true;
    // A controller profile's settings go back into its file (ControllerBindings), the others into the ini
    std::vector<std::pair<std::string, std::string>> toJson;
    for (Change& c : todo)
        if (ControllerBindings::Owns(strchr(c.k->name, '.') + 1)) { toJson.push_back({ c.name, c.value }); c.written = c.json = true; }
    if (!toJson.empty() && !ControllerBindings::Write(toJson, err)) return false;
    bool anyIni = false; for (const Change& c : todo) if (!c.json) anyIni = true;
    if (anyIni)
    {
    // The file, its lines changed in place: only the value on each one's line (what is around it, comments and line
    // ends included, stays); a setting the file does not have yet goes at its end (it is found by its name alone).
    const std::wstring path = GameInfo::GameDir() + L"\\DishonoredVR.ini";
    std::string text;
    if (FILE* f = _wfopen(path.c_str(), L"rb"))
    {
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        fclose(f);
    }
    if (text.size() >= 2 && (unsigned char)text[0] == 0xFF && (unsigned char)text[1] == 0xFE) { err = "DishonoredVR.ini is saved as UTF-16"; return false; }
    const bool crlf = text.find("\r\n") != std::string::npos || text.empty();
    std::string out; out.reserve(text.size() + 256);
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t eol = text.find('\n', pos); const bool last = eol == std::string::npos; if (last) eol = text.size();
        std::string line = text.substr(pos, eol - pos); pos = eol + 1;
        size_t end = line.size(); if (end && line[end - 1] == '\r') end--;
        const size_t first = line.find_first_not_of(" \t");
        const size_t eq = line.find('=');
        if (first != std::string::npos && first < end && line[first] != ';' && line[first] != '#' && line[first] != '[' && eq != std::string::npos && eq < end)
        {
            const std::string bare = Lower(Trim(line.substr(0, eq)));
            for (Change& c : todo)
                if (!c.json && bare == strchr(c.k->name, '.') + 1)
                {
                    size_t vs = eq + 1; while (vs < end && (line[vs] == ' ' || line[vs] == '\t')) vs++;
                    size_t ce = line.find_first_of(";#", vs); if (ce == std::string::npos || ce > end) ce = end;
                    size_t ve = ce; while (ve > vs && (line[ve - 1] == ' ' || line[ve - 1] == '\t')) ve--;
                    if (vs == eq + 1) { line.insert(vs, " "); vs++; ve++; }   // "Key=v" -> "Key= v": the file's usual look
                    line.replace(vs, ve - vs, c.value);
                    c.written = true;
                    break;
                }
        }
        out += line;
        if (!last) out += '\n';
    }
    for (Change& c : todo)
        if (!c.written)
        {
            if (!out.empty() && out.back() != '\n') out += crlf ? "\r\n" : "\n";
            out += c.name + " = " + c.value + (crlf ? "\r\n" : "\n");
        }
    const std::wstring tmp = path + L".new";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) { err = "DishonoredVR.ini could not be written (read-only folder?)"; return false; }
    const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(tmp.c_str());
        err = fmt::format("DishonoredVR.ini could not be replaced (error {})", GetLastError());
        return false;
    }
    }
    // Now the values themselves: at once, or (kRestart) kept for the tabs and used after a restart
    AcquireSRWLockExclusive(&g_Lock);
    for (const Change& c : todo)
    {
        const std::string bare = strchr(c.k->name, '.') + 1;
        if (Restart(bare)) { if (g_Val[c.k->name] == c.value) g_Pending.erase(c.k->name); else g_Pending[c.k->name] = c.value; }
        else { g_Val[c.k->name] = c.value; live.push_back(bare); }
    }
    ReleaseSRWLockExclusive(&g_Lock);
    std::string said;
    for (const Change& c : todo) said += fmt::format(" {}={}{}{}", c.name, c.value, c.json ? " (in VRControllerBindings\\" + ControllerBindings::Active() + ".json)" : "",
                                                    Restart(strchr(c.k->name, '.') + 1) ? " (after a restart)" : "");
    LOGI("Settings: saved:{}", said);
    return true;
}

void Settings::ApplyOverrides(const std::vector<std::pair<std::string, std::string>>& values, const std::string& source, std::vector<std::string>& changed)
{
    changed.clear();
    for (const auto& kv : values)
    {
        const std::string bare = Lower(Trim(kv.first));
        const Key* k = nullptr;
        for (const Key& kk : kKeys) if (bare == strchr(kk.name, '.') + 1) { k = &kk; break; }
        if (!k) { LOGI("Settings: {}: unknown setting '{}' ignored", source, kv.first); continue; }
        if (bare == "controllerbindings") { LOGI("Settings: {}: ControllerBindings is chosen in DishonoredVR.ini, not in a profile: ignored", source); continue; }
        std::string v;
        if (!Validate(*k, kv.second, v)) { LOGI("Settings: {}: {} = '{}' is not a valid value: ignored", source, kv.first, kv.second); continue; }
        AcquireSRWLockExclusive(&g_Lock);
        const std::string was = g_Val[k->name];
        const bool fromIni = g_Given.count(k->name) && !g_Source.count(bare);
        g_Val[k->name] = v; g_Given[k->name] = true; g_Source[bare] = source;
        g_Pending.erase(k->name);
        ReleaseSRWLockExclusive(&g_Lock);
        if (fromIni && was != v) LOGI("Settings: {} = {} in DishonoredVR.ini: {} has {}, which is used", strchr(k->name, '.') + 1, was, source, v);
        if (was != v) changed.push_back(bare);
    }
}
