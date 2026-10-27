#include "pch.h"
#include "VrTweaks.h"
#include "CVars.h"
#include "Game.h"
#include "VRRuntime.h"

namespace
{
    //! One setting. `console`: set through the game's console (the engine reads it through its own path; these two are
    //! not saved in the game's config) instead of the live value (read where it is used, every frame -- r_skipDOF at
    //! exe+0x375073, r_skipLensFlares at exe+0x375264 -- and saved in the config: writing only the live value keeps the
    //! player's own setting out of it).
    struct Tweak
    {
        const char* name; float value; bool isFloat; bool console;
        float saved = 0.f; bool held = false;
    };
    Tweak g_T[] = {
        { "r_skipDOF",                  1.f,  false, false },
        { "r_skipLensFlares",           1.f,  false, false },
        { "r_SSSBlurQuality",           2.f,  false, true },
        { "r_textureUploadTimesliceMs", 1.5f, true,  true },
    };
    std::atomic<bool> g_Enabled{ true };
    bool g_Active = false;            // render thread only
    unsigned g_Since = 0;
    std::mutex g_SM; std::string g_Status = "vrtweaks off";

    float Get(const Tweak& t) { return t.isFloat ? CVars::GetFloat(t.name) : (float)CVars::GetInt(t.name); }
    std::string Text(const Tweak& t, float v) { return t.isFloat ? fmt::format("{} {}", t.name, v) : fmt::format("{} {}", t.name, (int)v); }
    void Set(const Tweak& t, float v)
    {
        if (t.console) Game::BufferCommandText(Text(t, v).c_str());
        else if (t.isFloat) CVars::SetFloat(t.name, v);
        else CVars::SetInt(t.name, (int)v);
    }
    void SetStatus(std::string s) { std::lock_guard lk(g_SM); g_Status = std::move(s); }
}

void VrTweaks::SetEnabled(bool on)
{
    g_Enabled = on;
    LOGI("VrTweaks: VR graphics tweaks {}", on ? "on while the headset shows the game (VrGraphicsTweaks = 1)" : "off (VrGraphicsTweaks = 0)");
}

void VrTweaks::OnPresent()
{
    if (!CVars::Available()) return;
    const bool want = g_Enabled.load() && VRRuntime::GetStatus().running;
    if (want && !g_Active)
    {
        std::string s;
        for (Tweak& t : g_T)
        {
            t.held = CVars::Address(t.name) != 0;
            if (!t.held) { s += fmt::format(" {} (not found)", t.name); continue; }
            t.saved = Get(t);
            Set(t, t.value);
            s += fmt::format(" {} {} -> {}", t.name, t.saved, t.value);
        }
        g_Active = true; g_Since = 0;
        LOGI("VrTweaks: the headset shows the game: graphics tweaks on:{}", s);
        SetStatus("vrtweaks on:" + s);
    }
    else if (want)
    {
        // The options menu (a quality preset) can write the live ones again: they are held. The console ones are set
        // once (the command runs on a later frame); their live values are logged once, as a check.
        for (Tweak& t : g_T) if (t.held && !t.console && Get(t) != t.value) Set(t, t.value);
        if (++g_Since == 300)
        {
            std::string s;
            for (const Tweak& t : g_T) if (t.held) s += fmt::format(" {}={}", t.name, Get(t));
            LOGI("VrTweaks: live values:{}", s);
        }
    }
    else if (g_Active)
    {
        for (Tweak& t : g_T)
        {
            if (!t.held) continue;
            if (t.console || Get(t) == t.value) Set(t, t.saved);    // a value the player changed meanwhile is left
            t.held = false;
        }
        g_Active = false;
        LOGI("VrTweaks: graphics tweaks off: the game's own values are back");
        SetStatus("vrtweaks off");
    }
}

std::string VrTweaks::Status() { std::lock_guard lk(g_SM); return g_Status; }
