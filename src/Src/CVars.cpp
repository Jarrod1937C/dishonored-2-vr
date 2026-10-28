#include "pch.h"
#include "CVars.h"
#include "GameInfo.h"
#include <unordered_map>

// Void Engine keeps its idCVar objects as 0x80-byte static globals in .data.
// Measured layout (1.77.9.0) — see docs/ENGINE.md:
//   T+00 idStr value   { char* data; int len; int alloced; char base[24]; }  (live string value)
//   T+28 int integerValue        T+2C float floatValue           (live numeric values)
//   T+30 const char* name        T+38 const char* defaultValue   T+40 const char* description
//   T+48 int flags               T+4C float valueMin (1.0 = none)  T+50 float valueMax (-1.0 = none)
//   T+58 valueStrings            T+68 argCompletion fn           T+70 ? (0)
//   T+78 idCVar* next (static list)
// We resolve a cvar by finding its name string in the image, then the qword
// that points at it (T+30).
namespace
{
    const std::vector<CVars::Entry> g_Known = {
        // rva = value from the community dump; NOT valid for this exe, kept for reference only.
        { "com_allowConsole",        0x288F440 },
        { "com_showFPS",             0x288F540 },
        { "g_showHud",               0x3BFC270 },
        { "ark_ui_show_master_hud",  0x3BEFBB0 },
        { "ark_hud_enable_keyhole",  0x3BE0EF0 },
        { "ark_playerBobAmount",     0x3BEF1F0 },
        { "pm_noBob",                0x3BFC0F0 },
        { "pm_thirdPerson",          0x3BFBFF0 },
        { "g_stopTime",              0x3BF9A70 },
        { "com_disableFrameSpinning", 0 },        // (measured: object at exe+0x25B1E80) StereoSync: the frame cap off
        { "com_timeScaler",          0 },        // (measured: object at exe+0x25B1E00) the frame cap is 30 fps x this
        { "r_ILProbeUpdate_MaxUpdatesPerFrame", 0 }, // (measured: object at exe+0x32D1C20) StereoSync: halved in synchronised stereo
        { "g_debugPlayer",           0x3BFDFE0 },
        { "stereoRender_separation",       0x2EC1430 },
        { "stereoRender_screenSeparation", 0x2EC14B0 },
        { "stereoRender_guiOffset",        0x2EC1530 },
        { "stereoRender_swapEyes",         0x2EC15B0 },
    };
    constexpr uint32_t kStrOff = 0x00, kIntOff = 0x28, kFloatOff = 0x2C, kNameOff = 0x30, kFlagsOff = 0x48;
    bool g_Available = false;
    std::unordered_map<std::string, uintptr_t> g_Resolved;   //!< name -> object (T)

    struct Section { uintptr_t begin, end; bool exec; };
    std::vector<Section> ImageSections()
    {
        std::vector<Section> out;
        uintptr_t base = GameInfo::Base();
        auto* dos = (IMAGE_DOS_HEADER*)base;
        auto* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        {
            uintptr_t b = base + sec->VirtualAddress;
            out.push_back({ b, b + sec->Misc.VirtualSize, (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 });
        }
        return out;
    }

    bool Readable(uintptr_t p, size_t n)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((void*)p, &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        return p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }

    //! Finds the NUL-terminated string (preceded by NUL) in non-exec sections.
    uintptr_t FindString(const std::vector<Section>& secs, const char* s)
    {
        size_t n = strlen(s);
        for (auto& sc : secs)
        {
            if (sc.exec || !Readable(sc.begin, 1)) continue;
            const char* p = (const char*)sc.begin;
            const char* end = (const char*)sc.end - n - 1;
            for (const char* q = p; q < end; q++)
                if (q[0] == s[0] && q[n] == 0 && memcmp(q, s, n) == 0 && (q == p || q[-1] == 0))
                    return (uintptr_t)q;
        }
        return 0;
    }

    //! Finds the (first) 8-aligned qword equal to `value` in non-exec sections.
    uintptr_t FindPointerTo(const std::vector<Section>& secs, uintptr_t value)
    {
        for (auto& sc : secs)
        {
            if (sc.exec || !Readable(sc.begin, 8)) continue;
            uintptr_t b = (sc.begin + 7) & ~uintptr_t(7);
            for (uintptr_t p = b; p + 8 <= sc.end; p += 8)
                if (*(uintptr_t*)p == value) return p;
        }
        return 0;
    }
}

void CVars::Init()
{
    g_Available = false;
    g_Resolved.clear();
    auto secs = ImageSections();
    uintptr_t base = GameInfo::Base();
    LARGE_INTEGER t0, t1, f; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&f);
    for (auto& e : g_Known)
    {
        uintptr_t str = FindString(secs, e.name);
        uintptr_t slot = str ? FindPointerTo(secs, str) : 0;
        if (!slot) { LOGI("CVars: {:32} NOT FOUND (str=0x{:X})", e.name, str); continue; }
        uintptr_t T = slot - kNameOff;
        // Sanity: the idStr data pointer at T+00 must point into the object or the heap, never be garbage.
        uintptr_t strData = *(uintptr_t*)(T + kStrOff);
        if (!strData || !Readable(strData, 1)) { LOGI("CVars: {:32} layout check failed (idStr data=0x{:X})", e.name, strData); continue; }
        g_Resolved[e.name] = T;
    }
    QueryPerformanceCounter(&t1);
    LOGI("CVars: resolved {}/{} by name scan in {:.0f} ms", g_Resolved.size(), g_Known.size(),
         1000.0 * (t1.QuadPart - t0.QuadPart) / f.QuadPart);
    if (g_Resolved.empty()) return;
    g_Available = true;
    for (auto& e : g_Known)
        if (g_Resolved.count(e.name))
            LOGI("CVars: {:32} exe+0x{:07X} flags=0x{:X} int={} float={} str=\"{}\"", e.name, g_Resolved[e.name] - base,
                 *(uint32_t*)(g_Resolved[e.name] + kFlagsOff), GetInt(e.name), GetFloat(e.name), GetString(e.name));
}

void CVars::DumpObject(const char* name)
{
    uintptr_t T = Address(name); if (!T) return;
    std::string d;
    for (int i = 0; i < 16; i++) d += fmt::format(" [+{:02X}]={:016X}", i * 8, *(uintptr_t*)(T + i * 8));
    LOGI("CVars: dump {} exe+0x{:X}:{}", name, T - GameInfo::Base(), d);
    // Follow every pointer-looking qword into the image and dump 0x40 bytes of the target.
    for (int i = 0; i < 16; i++)
    {
        uintptr_t q = *(uintptr_t*)(T + i * 8);
        if (q < GameInfo::Base() || q > GameInfo::Base() + 0x6000000 || !Readable(q, 0x40) || (q >= T && q < T + 0x80)) continue;
        std::string e;
        for (int j = 0; j < 8; j++) e += fmt::format(" [+{:02X}]={:016X}", j * 8, *(uintptr_t*)(q + j * 8));
        LOGI("CVars:   +{:02X} -> exe+0x{:X}:{}", i * 8, q - GameInfo::Base(), e);
    }
}

bool CVars::Available() { return g_Available; }
const std::vector<CVars::Entry>& CVars::Known() { return g_Known; }

uintptr_t CVars::Address(const char* name)
{
    if (!g_Available) return 0;
    auto it = g_Resolved.find(name);
    if (it != g_Resolved.end()) return it->second;
    // Any other cvar: the same name scan, on demand, cached (0 = not a cvar, so a typo is not rescanned).
    static std::mutex m; std::lock_guard lk(m);
    static std::unordered_map<std::string, uintptr_t> extra;
    auto e = extra.find(name);
    if (e != extra.end()) return e->second;
    auto secs = ImageSections();
    uintptr_t str = FindString(secs, name), slot = str ? FindPointerTo(secs, str) : 0, T = slot ? slot - kNameOff : 0;
    if (T && (!Readable(T, 0x80) || !*(uintptr_t*)(T + kStrOff) || !Readable(*(uintptr_t*)(T + kStrOff), 1))) T = 0;
    LOGI("CVars: {} resolved on demand -> {}", name, T ? fmt::format("exe+0x{:X}", T - GameInfo::Base()) : std::string("not found"));
    extra[name] = T;
    return T;
}
int CVars::GetInt(const char* n, int fb)       { uintptr_t a = Address(n); return a ? *(int*)(a + kIntOff) : fb; }
float CVars::GetFloat(const char* n, float fb) { uintptr_t a = Address(n); return a ? *(float*)(a + kFloatOff) : fb; }
std::string CVars::GetString(const char* n)
{
    uintptr_t a = Address(n); if (!a) return {};
    const char* s = *(const char**)(a + kStrOff);
    return (s && Readable((uintptr_t)s, 1)) ? std::string(s) : std::string();
}
// Note: direct writes bypass the engine's OnChange/modified flags; good enough
// for plain tunables, not for cvars whose side effects live in a callback.
bool CVars::SetInt(const char* n, int v)
{
    uintptr_t a = Address(n); if (!a) return false;
    *(int*)(a + kIntOff) = v; *(float*)(a + kFloatOff) = (float)v;
    return true;
}
bool CVars::SetFloat(const char* n, float v)
{
    uintptr_t a = Address(n); if (!a) return false;
    *(float*)(a + kFloatOff) = v; *(int*)(a + kIntOff) = (int)v;
    return true;
}
