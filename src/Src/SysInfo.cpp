#include "pch.h"
#include "CrashLog.h"
#include "SysInfo.h"
#include "GameInfo.h"
#include "CVars.h"
#include "Version.h"
#include <dxgi1_2.h>
#pragma comment(lib, "dxgi.lib")

namespace
{
    std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
        return s;
    }
    std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value)
    {
        wchar_t buf[1024]; DWORD sz = sizeof buf;
        if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return {};
        return buf;
    }
    DWORD RegDword(HKEY root, const wchar_t* key, const wchar_t* value)
    {
        DWORD v = 0, sz = sizeof v;
        return RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS ? v : 0;
    }
    //! The value NAMES of a key (OpenXR lists its runtimes and layers as value names holding a manifest path).
    std::vector<std::wstring> RegValueNames(HKEY root, const wchar_t* key)
    {
        std::vector<std::wstring> out; HKEY h;
        if (RegOpenKeyExW(root, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return out;
        for (DWORD i = 0;; i++)
        {
            wchar_t name[1024]; DWORD len = 1024; DWORD data = 0, dsz = sizeof data, type = 0;
            const LONG r = RegEnumValueW(h, i, name, &len, nullptr, &type, (BYTE*)&data, &dsz);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r == ERROR_SUCCESS || r == ERROR_MORE_DATA)
                out.push_back(std::wstring(name) + (type == REG_DWORD && data ? L" (disabled)" : L""));
        }
        RegCloseKey(h);
        return out;
    }

    void LogWindows()
    {
        using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW v{ sizeof v };
        if (auto f = (RtlGetVersion_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) f(&v);
        const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
        LOGI("sys: Windows {}.{} build {}.{} ({}, {})", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber,
             RegDword(HKEY_LOCAL_MACHINE, cv, L"UBR"), Narrow(RegString(HKEY_LOCAL_MACHINE, cv, L"ProductName")),
             Narrow(RegString(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion")));
        MEMORYSTATUSEX ms{ sizeof ms }; GlobalMemoryStatusEx(&ms);
        SYSTEM_INFO si; GetNativeSystemInfo(&si);
        LOGI("sys: CPU {} ({} logical), RAM {:.1f} GB", Narrow(RegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString")),
             si.dwNumberOfProcessors, ms.ullTotalPhys / 1073741824.0);
        CrashLog::SetInfo(CrashLog::kSystem, fmt::format("Windows {}.{} build {}.{}, CPU {} ({} logical), RAM {:.1f} GB", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber,
            RegDword(HKEY_LOCAL_MACHINE, cv, L"UBR"), Narrow(RegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString")),
            si.dwNumberOfProcessors, ms.ullTotalPhys / 1073741824.0));
        wchar_t kl[KL_NAMELENGTH] = {}; GetKeyboardLayoutNameW(kl);
        LOGI("sys: keyboard layout {} (the mod sends keys by physical position, so the layout does not matter)", Narrow(kl));
    }

    void LogGpus()
    {
        IDXGIFactory1* f = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f)) || !f) { LOGI("sys: DXGI factory unavailable"); return; }
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++)
        {
            DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
            LOGI("sys: GPU {}: {} ({} MB, LUID {:08X}:{:08X}){}", i, Narrow(d.Description), d.DedicatedVideoMemory >> 20,
                 (unsigned)d.AdapterLuid.HighPart, (unsigned)d.AdapterLuid.LowPart, (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " software" : "");
            a->Release();
        }
        f->Release();
    }

    BOOL CALLBACK MonitorProc(HMONITOR m, HDC, LPRECT, LPARAM n)
    {
        MONITORINFOEXW mi{}; mi.cbSize = sizeof mi;
        if (GetMonitorInfoW(m, &mi))
            LOGI("sys: display {}: {}x{}{}", (*(int*)n)++, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                 (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "");
        return TRUE;
    }
    void LogDisplays()
    {
        int n = 0; EnumDisplayMonitors(nullptr, nullptr, MonitorProc, (LPARAM)&n);
        using GetDpiForSystem_t = UINT(WINAPI*)();
        if (auto f = (GetDpiForSystem_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"))
            LOGI("sys: Windows display scaling {}%", f() * 100 / 96);
    }

    void LogOpenXR()
    {
        const wchar_t* k = L"SOFTWARE\\Khronos\\OpenXR\\1";
        LOGI("sys: OpenXR active runtime: {}", SysInfo::ActiveOpenXRRuntime());
        for (const auto& r : RegValueNames(HKEY_LOCAL_MACHINE, (std::wstring(k) + L"\\AvailableRuntimes").c_str()))
            LOGI("sys: OpenXR runtime installed: {}", Narrow(r));
        // Implicit API layers load into EVERY OpenXR app (OpenXR Toolkit, overlays, eye-tracking drivers...): one that
        // misbehaves breaks us without being visible anywhere else.
        for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
            for (const auto& l : RegValueNames(root, (std::wstring(k) + L"\\ApiLayers\\Implicit").c_str()))
                LOGI("sys: OpenXR implicit layer{}: {}", root == HKEY_CURRENT_USER ? " (user)" : "", Narrow(l));
    }

    void LogGameFolder()
    {
        // Our two DLLs and the game's own are expected; anything else in the folder is worth knowing about (another
        // proxy DLL, ReShade, an ASI loader...). The game ships few DLLs, so listing them all keeps the line short.
        std::string dlls, others;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((GameInfo::GameDir() + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring n = fd.cFileName, l = n; for (auto& c : l) c = (wchar_t)towlower(c);
            const auto ends = [&](const wchar_t* e) { const size_t k = wcslen(e); return l.size() >= k && l.compare(l.size() - k, k, e) == 0; };
            if (ends(L".dll")) dlls += " " + Narrow(n);
            else if (ends(L".asi") || l.find(L"reshade") != std::wstring::npos || l == L"dxvk.conf" || l == L"specialk.ini") others += " " + Narrow(n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        LOGI("sys: DLLs in the game folder:{}", dlls);
        if (!others.empty()) LOGI("sys: other mod files in the game folder:{}", others);
    }
}

std::string SysInfo::ActiveOpenXRRuntime()
{
    wchar_t env[1024];
    if (GetEnvironmentVariableW(L"XR_RUNTIME_JSON", env, 1024)) return Narrow(env) + " (XR_RUNTIME_JSON)";
    const std::wstring r = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime");
    return r.empty() ? "NONE (no OpenXR runtime is set as active: open the headset's PC app - Meta Horizon Link, SteamVR, "
                       "Virtual Desktop, Pico Connect... - and set it as the OpenXR runtime)" : Narrow(r);
}

void SysInfo::LogStartup()
{
    LOGI("Dishonored 2 VR {} (built {} {})", DVR_VERSION_FULL, __DATE__, __TIME__);
    LogWindows();
    LogGpus();
    LogDisplays();
    LogOpenXR();
    LogGameFolder();
}

void SysInfo::LogFirstPresent(IDXGISwapChain* sc)
{
    // Which GPU the GAME renders on. On a laptop with two GPUs Windows may run the game on the integrated one, and the
    // headset's runtime then refuses the session (it composites on the discrete GPU) -- or works slowly through a copy.
    ID3D11Device* dev = nullptr; IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) && dev)
    {
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && dd && SUCCEEDED(dd->GetAdapter(&ad)) && ad)
        {
            DXGI_ADAPTER_DESC d{}; ad->GetDesc(&d);
            LOGI("sys: the game renders on {} (LUID {:08X}:{:08X}), feature level {:X}", Narrow(d.Description),
                 (unsigned)d.AdapterLuid.HighPart, (unsigned)d.AdapterLuid.LowPart, (unsigned)dev->GetFeatureLevel());
            CrashLog::SetInfo(CrashLog::kGpu, fmt::format("{} ({} MB, vendor {:04X} device {:04X})", Narrow(d.Description), d.DedicatedVideoMemory >> 20, d.VendorId, d.DeviceId));
            ad->Release();
        }
        if (dd) dd->Release();
        dev->Release();
    }
    DXGI_SWAP_CHAIN_DESC scd{};
    if (SUCCEEDED(sc->GetDesc(&scd)))
        LOGI("sys: game swapchain {}x{} format {} {} buffers {}", scd.BufferDesc.Width, scd.BufferDesc.Height, (int)scd.BufferDesc.Format,
             scd.Windowed ? "windowed" : "EXCLUSIVE FULLSCREEN", scd.BufferCount);
    // Overlays and other injectors that hook the same swapchain / input we do.
    const struct { const wchar_t* dll; const char* what; } kHooks[] = {
        { L"gameoverlayrenderer64.dll", "Steam overlay" }, { L"RTSSHooks64.dll", "RivaTuner / MSI Afterburner" },
        { L"DiscordHook64.dll", "Discord overlay" }, { L"graphics-hook64.dll", "OBS game capture" },
        { L"ReShade64.dll", "ReShade" }, { L"SpecialK64.dll", "Special K" }, { L"nvspcap64.dll", "NVIDIA overlay / ShadowPlay" },
        { L"EOSOVH-Win64-Shipping.dll", "Epic overlay" }, { L"overlay64.dll", "an overlay (overlay64.dll)" },
    };
    std::string found;
    for (const auto& k : kHooks) if (GetModuleHandleW(k.dll)) found += fmt::format(" [{}]", k.what);
    LOGI("sys: overlays loaded in the game:{}", found.empty() ? " none of the known ones" : found);
    if (CVars::Available())
        LOGI("sys: game settings: r_fullscreen {} window {}x{} r_mode {} fov {} mouse sensitivity {} invert mouse Y {} camera motion blur {}",
             CVars::GetString("r_fullscreen"), CVars::GetString("r_windowWidth"), CVars::GetString("r_windowHeight"),
             CVars::GetString("r_mode"), CVars::GetString("ark_fieldOfView"), CVars::GetString("g_mouseSensitivity"),
             CVars::GetString("g_invertMouseYAxis"), CVars::GetInt("r_skipCameraMotionBlur", 0) ? "off" : "ON");
}
