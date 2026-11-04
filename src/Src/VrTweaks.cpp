#include "pch.h"
#include "CrashLog.h"
#include "VrTweaks.h"
#include "VRRuntime.h"
#include "CVars.h"
#include "Game.h"
#include "Fog.h"
#include <vector>
#include <mutex>
#include <cmath>
#include "GameInfo.h"
#include <detours/detours.h>
#include "Settings.h"
#include "StereoSync.h"
#include "Reticle.h"

namespace
{
    // The main view object's render flags (in its renderView's settings), set by the engine every frame from cvars
    // before the view's passes run: +0x3A37 bit 0 = no depth of field, bit 1 = no SSAO.
    constexpr uint32_t kViewFlags = 0x3A37;
    constexpr uint8_t kNoDof = 0x01, kNoAo = 0x02;
    std::atomic<bool> g_Enabled{ true }, g_Ao{ true }, g_Fog{ true }, g_Active{ false };
    int g_UserSkipFog = -1;     // the game's r_skipFog, to put back (render thread)
    std::atomic<unsigned long long> g_Views{ 0 };
    bool g_WasActive = false;   // render thread only
    // [Display] FogMaps: the fog is kept in maps whose name holds one of these words (lower case)
    std::vector<std::string> g_FogMaps;
    std::mutex g_MapM; std::string g_Map;   // the current map's name, lower case (written on the render thread)
    std::atomic<bool> g_MapFog{ false };   // the current map is one of FogMaps
    std::atomic<bool> g_GameFogOff{ false };   // r_skipFog is 1 now (the game's fog pass does not run)
    std::atomic<int> g_FogMapsShift{ -1 };    // FogMapsResolution as r_downsampleQuality (0 full, 1 half, 2 quarter; -1 the game's)
    std::atomic<float> g_LodZoom{ 1.f };   // StaticLodZoom (1 = the game's own, never written)
    std::atomic<bool> g_NoBob{ true };     // DisableHeadBob
    int g_UserNoBob = -1;                  // the game's pm_noBob, to put back (render thread)

    // ViewDistance: the game's view-distance preset (ARK_VISUALSETTING_VIEW_DISTANCE, r_viewDistPreset; set by the
    // menu's environment detail with the others) is applied by exe+0x5105A0(settings, level) -- reached from
    // exe+0x50FD30 (setting 11), at start and at every change. It sets exactly two cvars from the game's own table
    // (settings+0x78; the levels' list at *(exe+0x32CDD00)+0x10, 16-byte entries { name, level }; the values at
    // +(0x96E + i) * 4 and +(0x91A + i) * 4): r_lodZoomMultiplicator (the static models' level of detail, see
    // StaticLodZoom) and r_umbraGateVisibilityRatio (Umbra's occlusion culling: how far is seen through the level's
    // gates -- doorways, windows, portals). The hook gives it ViewDistance's level instead of the game's.
    using ViewDistFn = void(*)(void* settings, int level);
    ViewDistFn g_ViewDistReal = nullptr;
    std::atomic<int> g_ViewDist{ -1 };   // -1 = the game's
    struct ViewDistLevel { char name[24]; long long level; float lod, gate; };
    //! the game's table (no C++ objects here: __try)
    int ReadViewDistTable(void* settings, ViewDistLevel* out, int max)
    {
        int n = 0;
        __try
        {
            const uintptr_t base = GameInfo::Base();
            const uint8_t* tbl = *(const uint8_t* const*)((const uint8_t*)settings + 0x78);
            const uint8_t* lst = *(const uint8_t* const*)(*(const uint8_t* const*)(base + 0x32CDD00) + 0x10);
            for (int i = 0; i < max; i++)
            {
                const char* nm = *(const char* const*)(lst + i * 16);
                if (!nm || !nm[0]) break;
                ViewDistLevel& L = out[n];
                for (int k = 0; k < 23; k++) { L.name[k] = nm[k]; if (!nm[k]) break; }
                L.name[23] = 0;
                L.level = *(const long long*)(lst + i * 16 + 8);
                L.lod = *(const float*)(tbl + (0x96E + i) * 4); L.gate = *(const float*)(tbl + (0x91A + i) * 4);
                n++;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        return n;
    }
    void LogViewDistTable(void* settings)
    {
        static void* logged = nullptr;
        if (logged == settings) return;
        logged = settings;
        ViewDistLevel lv[12] = {};
        const int n = ReadViewDistTable(settings, lv, 12);
        std::string s;
        for (int i = 0; i < n; i++) s += fmt::format(" | {} ({}): lod zoom {:g}, gate visibility {:g}", lv[i].name, lv[i].level, lv[i].lod, lv[i].gate);
        LOGI("VrTweaks: the game's view-distance levels:{}", n < 0 ? std::string(" (unreadable)") : s);
    }
    void HkViewDist(void* settings, int level)
    {
        if (settings) LogViewDistTable(settings);
        const int want = g_ViewDist.load();
        if (want >= 0 && want != level) LOGI("VrTweaks: view distance: the game asked for level {}, ViewDistance {} applied instead", level, want);
        g_ViewDistReal(settings, want >= 0 ? want : level);
        LOGI("VrTweaks: view distance level {} applied: r_lodZoomMultiplicator {:g}{}, r_umbraGateVisibilityRatio {:g}", want >= 0 ? want : level,
             CVars::GetFloat("r_lodZoomMultiplicator", -1.f), fabsf(g_LodZoom.load() - 1.f) > 1e-3f ? fmt::format(" (StaticLodZoom {:g} holds it instead)", g_LodZoom.load()) : std::string(),
             CVars::GetFloat("r_umbraGateVisibilityRatio", -1.f));
    }
    // The map's name: idGameLocal's vtable +0x88 (exe+0x8A89E0) returns gameLocal+0x337128, the map's file record,
    // whose idStr at +0x10 is the map file name -- what the game's own crash report prints as "Map:" (exe+0x427BD).
    constexpr size_t kMapNameStr = 0x337128 + 0x10;
    bool ReadMapName(const uint8_t* gl, char* out, size_t n)
    {
        __try
        {
            const char* s = *(const char* const*)(gl + kMapNameStr);
            if (!s) return false;
            size_t i = 0;
            for (; i + 1 < n && s[i]; i++) out[i] = s[i];
            out[i] = 0;
            return i > 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

void VrTweaks::SetFogMaps(const std::string& words)
{
    g_FogMaps.clear();
    size_t a = 0;
    while (a <= words.size())
    {
        size_t b = words.find(',', a); if (b == std::string::npos) b = words.size();
        std::string w = words.substr(a, b - a);
        while (!w.empty() && (w.front() == ' ' || w.front() == '\t')) w.erase(w.begin());
        while (!w.empty() && (w.back() == ' ' || w.back() == '\t')) w.pop_back();
        for (char& c : w) c = (char)tolower((unsigned char)c);
        if (!w.empty()) g_FogMaps.push_back(w);
        a = b + 1;
    }
    LOGI("VrTweaks: FogMaps = '{}' ({} words): with Fog = 0 the fog still stays on in maps named so", words, g_FogMaps.size());
}

std::string VrTweaks::MapName() { std::lock_guard lk(g_MapM); return g_Map; }

void VrTweaks::SetEnabled(bool on)
{
    g_Enabled = on;
    LOGI("VrTweaks: depth of field {} in VR (VrGraphicsTweaks = {})", on ? "off" : "as the game has it", on ? 1 : 0);
}

void VrTweaks::SetAmbientOcclusion(bool on)
{
    g_Ao = on;
    if (!on) LOGI("VrTweaks: ambient occlusion off in VR (AmbientOcclusion = 0)");
}

void VrTweaks::SetLodZoom(float z)
{
    g_LodZoom = z;
    if (fabsf(z - 1.f) > 1e-3f) LOGI("VrTweaks: StaticLodZoom {:g}: the game's r_lodZoomMultiplicator held at it (the static models' level of detail; experimental)", z);
}
void VrTweaks::SetHeadBobOff(bool off) { g_NoBob = off; }
bool VrTweaks::GameFogOff() { return g_GameFogOff.load(std::memory_order_relaxed); }
bool VrTweaks::DofOff() { return g_Active.load(std::memory_order_relaxed) && g_Enabled.load(std::memory_order_relaxed); }
void VrTweaks::SetViewDistance(int level)
{
    g_ViewDist = level < 0 ? -1 : (level > 5 ? 5 : level);
}
// ---- the game's options menus without what the mod decides in VR ----
// Each options section (main menu and pause menu alike) lists its settings in a loop over the setting enum (e.g.
// exe+0x5089B0 for gameplay, 26 settings): for each, the section's descriptor (decl [this+0x78] + 0xA0 + index * 0x120,
// the index being the enum entry's position in its descriptor table, entries { const char* name; int64 value }) is
// given to the section's virtual slot 13 (this+0x68 in the vtable: "list this one?"), and only a yes adds it. Slot 13 of
// arkMenuSectionOptionVisuals (vtable exe+0x1B71478; exe+0x50E1A0) and of arkMenuSectionOptionGameplay (vtable
// exe+0x1B70CA8; the base's exe+0x5034E0) is replaced: the setting is named from the descriptor's position, and the
// ones the mod holds, overrides or ignores while the headset session runs are answered no. The enum tables: visuals
// *(exe+0x32CDCE8), gameplay *(exe+0x32CDD48). Nothing is written to the game's settings.
namespace
{
    constexpr uint32_t kVisualsVtable = 0x1B71478, kGameplayVtable = 0x1B70CA8, kVisualsEnum = 0x32CDCE8, kGameplayEnum = 0x32CDD48;
    constexpr int kListSlot = 13;
    using FnList = bool (*)(void* section, void* desc);
    FnList g_RealList[2] = {};
    std::atomic<int> g_MenuListed{ 0 }, g_MenuHidden{ 0 };
    std::mutex g_MenuM; std::vector<std::string> g_MenuSeen;   // names already logged
    //! the enum name of a descriptor (nullptr if it is not one of the section's)
    const char* SettingName(int which, void* section, void* desc)
    {
        __try
        {
            const uint8_t* decl = *(const uint8_t* const*)((const uint8_t*)section + 0x78);
            if (!decl) return nullptr;
            const intptr_t d = (const uint8_t*)desc - (decl + 0xA0);
            if (d < 0 || d % 0x120) return nullptr;
            const intptr_t idx = d / 0x120;
            const uint8_t* en = *(const uint8_t* const*)(GameInfo::Base() + (which ? kGameplayEnum : kVisualsEnum));
            if (!en) return nullptr;
            const uint8_t* e = *(const uint8_t* const*)(en + 0x10);
            for (intptr_t k = 0; e && k < 128; k++, e += 16)
            {
                const char* n = *(const char* const*)e;
                if (!n || !n[0]) return nullptr;
                if (k == idx) return n;
            }
            return nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    //! why the mod hides a setting now ("" = it does not)
    const char* HideReason(const char* n)
    {
        if (!VRRuntime::GetStatus().running) return "";
        auto is = [&](const char* s) { return !strcmp(n, s); };
        if (is("ARK_VISUALSETTING_FOV")) return "the headset's own field of view";
        if (is("ARK_VISUALSETTING_RESOLUTION_SCALE") || is("ARK_VISUALSETTING_RESOLUTION_SCALE_QUALITY")) return "adaptive resolution held off (RenderRes)";
        if (is("ARK_VISUALSETTING_SWAPBUFFERCOUNT")) return "triple buffering held off (RenderRes)";
        if ((is("ARK_VISUALSETTING_VSYNC") || is("ARK_VISUALSETTING_FRAMERATE_LIMITER")) && StereoSync::Active()) return "the headset paces the frames (StereoSync)";
        if (is("ARK_VISUALSETTING_DOF") && Settings::VrGraphicsTweaks()) return "no depth of field in VR (VrGraphicsTweaks)";
        if (is("ARK_VISUALSETTING_CAM_MOTION_BLUR") && Settings::DisableMotionBlur()) return "no motion blur (DisableMotionBlur)";
        if (is("ARK_VISUALSETTING_FOG_BLUR") && !Settings::Fog()) return "the game's fog off in VR (Fog = 0)";
        if (is("ARK_VISUALSETTING_HBAO_PLUS")) return Settings::AmbientOcclusion() ? "HBAO+ held off in VR: the game's own ambient occlusion (RenderRes)" : "no ambient occlusion in VR (AmbientOcclusion = 0)";
        if (is("ARK_VISUALSETTING_VIEW_DISTANCE") && Settings::ViewDistance() >= 0) return "the VR options' ViewDistance";
        if ((is("ARK_GPSETTING_CROSSHAIR_STYLE") || is("ARK_GPSETTING_CROSSHAIR_OPACITY")) && Reticle::Mode() != 0) return "the hands' reticles instead (Reticle)";
        if (is("ARK_GPSETTING_KILLCAM") && !Settings::FinisherAnimations()) return "no kill cam (FinisherAnimations = 0)";
        if (is("ARK_GPSETTING_HEADBOB") && Settings::DisableHeadBob()) return "no head bob in VR (DisableHeadBob)";
        return "";
    }
    bool ListHook(int which, void* section, void* desc)
    {
        const bool r = g_RealList[which](section, desc);
        const char* n = SettingName(which, section, desc);
        const char* why = n ? HideReason(n) : "";
        {
            std::lock_guard lk(g_MenuM);
            const std::string key = n ? n : "?";
            if (std::find(g_MenuSeen.begin(), g_MenuSeen.end(), key) == g_MenuSeen.end() && g_MenuSeen.size() < 128)
            {
                g_MenuSeen.push_back(key);
                LOGI("VrTweaks: game options menu: {} {}{}", n ? n : "(unnamed setting)", !r ? "not listed by the game" : why[0] ? "HIDDEN: " : "listed", r && why[0] ? why : "");
            }
        }
        if (r && why[0]) { g_MenuHidden++; return false; }
        if (r) g_MenuListed++;
        return r;
    }
    bool ListVisuals(void* s, void* d) { return ListHook(0, s, d); }
    bool ListGameplay(void* s, void* d) { return ListHook(1, s, d); }
}
void VrTweaks::InstallMenuFilter()
{
    if (!GameInfo::IsSupportedBuild()) return;
    const uintptr_t base = GameInfo::Base();
    const uint32_t vts[2] = { kVisualsVtable, kGameplayVtable };
    const uint32_t expect[2] = { 0x50E1A0, 0x5034E0 };
    FnList hooks[2] = { &ListVisuals, &ListGameplay };
    for (int i = 0; i < 2; i++)
    {
        uintptr_t* slot = (uintptr_t*)(base + vts[i] + kListSlot * 8);
        const uintptr_t cur = *slot;
        // (PlayerState-style counting stubs are not put on these vtables; anything else there is not what was measured)
        if (cur != base + expect[i]) { LOGI("VrTweaks: options section {} slot {} is exe+0x{:X}, not exe+0x{:X}: its menu is left as it is", i ? "gameplay" : "visuals", kListSlot, cur - base, expect[i]); continue; }
        g_RealList[i] = (FnList)cur;
        DWORD old = 0;
        if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { g_RealList[i] = nullptr; continue; }
        *slot = (uintptr_t)hooks[i];
        VirtualProtect(slot, 8, old, &old);
    }
    LOGI("VrTweaks: the game's options menus leave out what the mod decides in VR (visuals {}, gameplay {})", g_RealList[0] ? "on" : "off", g_RealList[1] ? "on" : "off");
}

void VrTweaks::InstallViewDistance()
{
    if (g_ViewDistReal) return;
    const uintptr_t base = GameInfo::Base();
    static const uint8_t kExpect[15] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((const void*)(base + 0x5105A0), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || memcmp((const void*)(base + 0x5105A0), kExpect, sizeof kExpect)) { LOGI("VrTweaks: the view-distance setter exe+0x5105A0 is not as expected (another game version?): ViewDistance not applied"); return; }
    g_ViewDistReal = (ViewDistFn)(base + 0x5105A0);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_ViewDistReal, (PVOID)&HkViewDist);
    const LONG e = DetourTransactionCommit();
    if (e != NO_ERROR) { g_ViewDistReal = nullptr; LOGI("VrTweaks: the view-distance hook failed ({})", e); return; }
    const int v = g_ViewDist.load();
    LOGI("VrTweaks: ViewDistance {}", v < 0 ? std::string("-1: the game's own view distance (its levels are logged when it applies one)")
                                          : fmt::format("{}: the game's view-distance level {} (0 very low .. 5 ultra) whatever its menu says", v, v));
}
void VrTweaks::SetFogMapsResolution(float share)
{
    const int s = share <= 0.f ? -1 : share >= 0.75f ? 0 : share >= 0.375f ? 1 : 2;
    g_FogMapsShift = s;
    LOGI("VrTweaks: FogMapsResolution {:g}: {}", share, s < 0 ? std::string("the game's own size for its fog in FogMaps maps") : fmt::format("in FogMaps maps the game's fog (and its other reduced-size buffers) at 1/{} of the eye's size", 1 << s));
}
void VrTweaks::SetFog(bool on)
{
    g_Fog = on;
    if (!on) LOGI("VrTweaks: no fog in VR (Fog = 0)");
}

void VrTweaks::OnPresent()
{
    const bool running = VRRuntime::GetStatus().running;
    const bool active = (g_Enabled.load() || !g_Ao.load() || !g_Fog.load()) && running;
    g_Active = active;
    if (active != g_WasActive)
    {
        g_WasActive = active;
        LOGI("VrTweaks: {}", active ? "the headset shows the game: VR graphics changes on" : "VR graphics changes off");
    }
    // No fog ([Display] Fog = 0): the engine's r_skipFog, written live while the headset shows the game and put back
    // after. The fog is rendered at a fraction of the screen's resolution (atm/distanceFog/TexDepthHalf), blurred and
    // scaled up (exe+0x30BB70): at the headset's density its blocks show over everything in front of the sky, the
    // game's "high" fog blur only softens them. r_skipFog is a plain flag cvar (flags 1: not saved to the config), read
    // every frame -- into the view's render flags (view+0x3A36 bit 0, exe+0x374F9F) and by the fog pass's setup
    // (exe+0x30644A). Written as a cvar, not a view flag, so the engine's own copy of it is the one in force.
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now >= next && CVars::Available())
    {
        next = now + 1000;
        {   // which map: logged at each change, and whether it is one where the fog stays (FogMaps)
            char buf[160];
            const uint8_t* gl = (const uint8_t*)Game::GameLocal();
            if (gl && ReadMapName(gl, buf, sizeof buf))
            {
                std::string m = buf;
                for (char& c : m) c = (char)tolower((unsigned char)c);
                if (m != g_Map)
                {
                    { std::lock_guard lk(g_MapM); g_Map = m; }
                    CrashLog::SetInfo(CrashLog::kMap, buf);
                    Fog::SetMap(m);
                    g_MapFog = false;
                    const char* hit = nullptr;
                    for (const std::string& w : g_FogMaps) if (m.find(w) != std::string::npos) { g_MapFog = true; hit = w.c_str(); break; }
                    if (g_MapFog) LOGI("VrTweaks: map '{}': its name holds '{}' (FogMaps): the game's fog stays on here", buf, hit);
                    else LOGI("VrTweaks: map '{}'", buf);
                }
            }
        }
        {   // StaticLodZoom: r_lodZoomMultiplicator, the zoom the static models' level of detail is chosen for (fewer, larger
            // triangles in the depth pass when the models' coarser levels are picked sooner). Held while it differs.
            const float z = g_LodZoom.load();
            static int told = 0;
            if (fabsf(z - 1.f) > 1e-3f)
            {
                const float now = CVars::GetFloat("r_lodZoomMultiplicator", -1.f);
                if (now > 0.f && fabsf(now - z) > 1e-3f && CVars::SetFloat("r_lodZoomMultiplicator", z) && told++ < 8)
                    LOGI("VrTweaks: r_lodZoomMultiplicator {:g} -> {:g} (StaticLodZoom)", now, z);
            }
        }
        {   // DisableHeadBob: pm_noBob held at 1 while the headset shows the game (read by the player's body every frame),
            // the game's own value back after
            const int cur = CVars::GetInt("pm_noBob", -1);
            if (running && g_NoBob.load() && cur == 0)
            {
                if (g_UserNoBob < 0) g_UserNoBob = cur;
                if (CVars::SetInt("pm_noBob", 1)) LOGI("VrTweaks: head bob off (pm_noBob 1) while the headset shows the game (DisableHeadBob)");
            }
            else if (!(running && g_NoBob.load()) && g_UserNoBob >= 0)
            {
                CVars::SetInt("pm_noBob", g_UserNoBob); g_UserNoBob = -1;
                LOGI("VrTweaks: head bob back (pm_noBob as the game had it)");
            }
        }
        const bool want = active && !g_Fog.load() && !g_MapFog;
        Fog::SetGameFogOff(want);                 // the mod's own cheap fog stands in while the game's is off
        const int cur = CVars::GetInt("r_skipFog", -1);
        if (want && cur == 0) { g_UserSkipFog = cur; CVars::SetInt("r_skipFog", 1); LOGI("VrTweaks: fog off (r_skipFog 1) while the headset shows the game"); }
        else if (!want && g_UserSkipFog >= 0) { CVars::SetInt("r_skipFog", g_UserSkipFog); g_UserSkipFog = -1; LOGI("VrTweaks: fog back{}", g_MapFog ? " (FogMaps)" : ""); }
        g_GameFogOff = CVars::GetInt("r_skipFog", 0) != 0;   // after the change above: whatever set it (the mod, the player's config)
        {   // FogMapsResolution: in the maps whose fog stays (the Void) the game's reduced-size buffers -- the fog's, and the
            // depth downsample it measures with -- at this share of the eye's size. r_downsampleQuality is the shift the
            // renderer divides the size by ((w + 1) >> q, exe+0x30776A for the fog); its "modified" flag makes it rebuild
            // those targets the next frame (exe+0x3519A4). Put back on leaving such a map. Also sizes the depth of
            // field's, the distortion's and the bloom's buffers, in those maps only.
            static int saved = -1;
            const int shift = g_FogMapsShift.load();
            const int cur = CVars::GetInt("r_downsampleQuality", -1);
            if (active && g_MapFog && shift >= 0 && cur >= 0)
            {
                if (saved < 0) saved = cur;
                if (cur != shift && CVars::SetIntModified("r_downsampleQuality", shift))
                    LOGI("VrTweaks: FogMapsResolution: the game's reduced-size buffers (its fog's among them) at 1/{} of the eye's size here (r_downsampleQuality {} -> {})", 1 << shift, cur, shift);
            }
            else if (saved >= 0)
            {
                if (cur != saved && CVars::SetIntModified("r_downsampleQuality", saved))
                    LOGI("VrTweaks: FogMapsResolution: r_downsampleQuality back to {}", saved);
                saved = -1;
            }
        }
    }
}

void VrTweaks::OnMainView(void* view)
{
    if (!view || !g_Active.load(std::memory_order_relaxed)) return;
    uint8_t add = 0;
    if (g_Enabled.load(std::memory_order_relaxed)) add |= kNoDof;
    if (!g_Ao.load(std::memory_order_relaxed)) add |= kNoAo;
    if (!add) return;
    ((uint8_t*)view)[kViewFlags] |= add;
    g_Views.fetch_add(1, std::memory_order_relaxed);
}

std::string VrTweaks::Status()
{
    return fmt::format("vrtweaks active {} dof {} ao {} fog {} (r_skipFog now {}) views flagged {} map '{}'{}", g_Active.load() ? 1 : 0,
                       g_Enabled.load() ? "off" : "game", g_Ao.load() ? "game" : "off", g_Fog.load() ? "game" : "off",
                       CVars::GetInt("r_skipFog", -1), g_Views.load(), MapName(), g_MapFog ? " (fog kept: FogMaps)" : "");
}
