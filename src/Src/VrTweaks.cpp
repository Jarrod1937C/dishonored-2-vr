#include "pch.h"
#include "VrTweaks.h"
#include "VRRuntime.h"
#include "CVars.h"
#include "Game.h"
#include "Fog.h"
#include <vector>
#include <mutex>

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
                    Fog::SetMap(m);
                    g_MapFog = false;
                    const char* hit = nullptr;
                    for (const std::string& w : g_FogMaps) if (m.find(w) != std::string::npos) { g_MapFog = true; hit = w.c_str(); break; }
                    if (g_MapFog) LOGI("VrTweaks: map '{}': its name holds '{}' (FogMaps): the game's fog stays on here", buf, hit);
                    else LOGI("VrTweaks: map '{}'", buf);
                }
            }
        }
        const bool want = active && !g_Fog.load() && !g_MapFog;
        Fog::SetGameFogOff(want);                 // the mod's own cheap fog stands in while the game's is off
        const int cur = CVars::GetInt("r_skipFog", -1);
        if (want && cur == 0) { g_UserSkipFog = cur; CVars::SetInt("r_skipFog", 1); LOGI("VrTweaks: fog off (r_skipFog 1) while the headset shows the game"); }
        else if (!want && g_UserSkipFog >= 0) { CVars::SetInt("r_skipFog", g_UserSkipFog); g_UserSkipFog = -1; LOGI("VrTweaks: fog back{}", g_MapFog ? " (FogMaps)" : ""); }
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
