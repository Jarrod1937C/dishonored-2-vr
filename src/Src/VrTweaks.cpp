#include "pch.h"
#include "VrTweaks.h"
#include "VRRuntime.h"

namespace
{
    // The main view object's render flags (in its renderView's settings), set by the engine every frame from cvars
    // before the view's passes run: +0x3A37 bit 0 = no depth of field, bit 1 = no SSAO.
    constexpr uint32_t kViewFlags = 0x3A37;
    constexpr uint8_t kNoDof = 0x01, kNoAo = 0x02;
    std::atomic<bool> g_Enabled{ true }, g_Ao{ true }, g_Active{ false };
    std::atomic<unsigned long long> g_Views{ 0 };
    bool g_WasActive = false;   // render thread only
}

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

void VrTweaks::OnPresent()
{
    const bool active = (g_Enabled.load() || !g_Ao.load()) && VRRuntime::GetStatus().running;
    g_Active = active;
    if (active != g_WasActive)
    {
        g_WasActive = active;
        LOGI("VrTweaks: {}", active ? "the headset shows the game: VR graphics changes on" : "VR graphics changes off");
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
    return fmt::format("vrtweaks active {} dof {} ao {} views flagged {}", g_Active.load() ? 1 : 0,
                       g_Enabled.load() ? "off" : "game", g_Ao.load() ? "game" : "off", g_Views.load());
}
