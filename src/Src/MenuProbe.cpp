#include "pch.h"
#include "MenuProbe.h"
#include "GameInfo.h"
#include <detours/detours.h>
#include <atomic>
#include <mutex>
#include <map>
#include <string>

// See MenuProbe.h. Every hook passes all four register arguments on unchanged (2026-11-01: the first version passed
// three -- the array constructor exe+0x27F60 takes a FOURTH, the element count, so the options screen's tab list came
// out empty); the logging only reads, through SEH-guarded helpers, and only pointers that look like user-space ones.
namespace
{
    using Fn3 = uintptr_t (*)(void*, void*, void*, void*);
    using Fn2 = Fn3;
    constexpr uintptr_t kHub = 0x34800, kObjCtor = 0x28E50, kArrCtor = 0x27F60;
    constexpr uintptr_t kFastName = 0x1009960;   // Iggy's IggyPlayerCreateFastName(player, utf8, length): the member names' ids
    constexpr uintptr_t kSetInt = 0x2A120, kSetUint = 0x2A1F0, kSetFloat = 0x2A2C0, kSetBool = 0x2A640, kSetStr = 0x2A720, kSetStrId = 0x2A530;
    constexpr uintptr_t kSetObj = 0x2A3A0, kSetArr = 0x2A460;
    constexpr uintptr_t kSectionsPause = 0x516ED0, kSectionsMain = 0x518F10, kShowSection = 0x501BB0;

    Fn3 g_Hub, g_ObjCtor, g_ArrCtor, g_SetInt, g_SetUint, g_SetFloat, g_SetBool, g_SetStr, g_SetStrId, g_SetObj, g_SetArr;
    Fn2 g_SectionsPause, g_SectionsMain, g_ShowSection;
    using NameFn = uint64_t (*)(void*, const char*, int, void*);
    NameFn g_FastName;
    std::mutex g_NM; std::map<uint64_t, std::string> g_Names;   // a member name's id -> its text (made while capturing)
    std::atomic<bool> g_On{ false };
    std::atomic<int> g_Lines{ 0 };
    constexpr int kMaxLines = 6000;
    thread_local int t_Capture = 0;                 // inside one of the options screen's builders or a menu callback
    std::mutex g_M; std::map<std::string, int> g_CallCount;
    std::atomic<unsigned long long> g_Calls{ 0 }, g_Writes{ 0 };

    bool Plausible(const void* p) { const uintptr_t v = (uintptr_t)p; return v >= 0x10000 && v < 0x7FFFFFFF0000ull; }   // user space
    bool Text(const void* p, char* out, size_t cap)
    {   // a NUL-terminated, printable string at p (else false)
        if (!Plausible(p)) return false;
        __try
        {
            const char* s = (const char*)p; size_t i = 0;
            for (; i + 1 < cap && s[i]; i++) { const unsigned char c = (unsigned char)s[i]; if (c < 0x20 && c != '\t') return false; out[i] = s[i]; }
            out[i] = 0;
            return i > 0 || !s[0];
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool ReadQ(const void* p, uint64_t& v) { if (!Plausible(p)) return false; __try { v = *(const uint64_t*)p; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
    bool ReadD(const void* p, uint32_t& v) { if (!Plausible(p)) return false; __try { v = *(const uint32_t*)p; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
    bool Line() { return g_Lines.fetch_add(1) < kMaxLines; }
    //! a member's name: the member is { IggyName, file } (exe+0x296C0); the name's text as it was made (HFastName)
    std::string Member(const void* m)
    {
        uint64_t id = 0;
        if (!ReadQ(m, id)) return "?";
        { std::lock_guard lk(g_NM); auto it = g_Names.find(id); if (it != g_Names.end()) return it->second; }
        return fmt::format("#{:X}", id);
    }
    uint64_t HFastName(void* player, const char* utf8, int len, void* r9)
    {
        const uint64_t r = g_FastName(player, utf8, len, r9);
        if (t_Capture)
        {
            char b[128];
            if (Text(utf8, b, sizeof b))
            {
                std::string n = len >= 0 && (size_t)len < strlen(b) ? std::string(b, (size_t)len) : std::string(b);
                std::lock_guard lk(g_NM);
                if (g_Names.size() < 20000) g_Names[r] = n;
            }
        }
        return r;
    }
    void Write(const char* kind, void* obj, void* value, void* member)
    {
        if (!t_Capture || !Line()) return;
        g_Writes++;
        std::string v;
        uint32_t d = 0; uint64_t q = 0; char b[160];
        if (!strcmp(kind, "int") && ReadD(value, d)) v = fmt::format("{}", (int)d);
        else if (!strcmp(kind, "uint") && ReadD(value, d)) v = fmt::format("{}", d);
        else if (!strcmp(kind, "float") && ReadD(value, d)) { float f; memcpy(&f, &d, 4); v = fmt::format("{}", f); }
        else if (!strcmp(kind, "bool") && ReadD(value, d)) v = (d & 0xFF) ? "true" : "false";
        else if (!strcmp(kind, "str")) v = Text(value, b, sizeof b) ? fmt::format("'{}'", b) : "?";
        else if (!strcmp(kind, "strId") && ReadD(value, d)) v = fmt::format("strId {}", d);
        else v = fmt::format("{}", value);
        LOGI("MenuProbe:   set {} obj {} .{} = {}", kind, obj, Member(member), v);
    }
#define SETTER(name, kind) uintptr_t H##name(void* o, void* v, void* m, void* x) { Write(kind, o, v, m); return g_##name(o, v, m, x); }
    SETTER(SetInt, "int") SETTER(SetUint, "uint") SETTER(SetFloat, "float") SETTER(SetBool, "bool") SETTER(SetStr, "str") SETTER(SetStrId, "strId")
    SETTER(SetObj, "object") SETTER(SetArr, "array")
#undef SETTER
    uintptr_t HObjCtor(void* o, void* file, void* path, void* x)
    {
        if (t_Capture && Line()) { char b[200]; LOGI("MenuProbe:   object {} = '{}'", o, Text(path, b, sizeof b) ? b : "?"); }
        return g_ObjCtor(o, file, path, x);
    }
    uintptr_t HArrCtor(void* o, void* file, void* path, void* count)
    {
        if (t_Capture && Line()) { char b[200]; LOGI("MenuProbe:   array {} = '{}' ({} elements)", o, Text(path, b, sizeof b) ? b : "?", (int)(intptr_t)count); }
        return g_ArrCtor(o, file, path, count);
    }
    //! the options screen's 4 section slots (this+8..): each one's vtable and its decl's (+0x78) label / class fields
    void Sections(void* self)
    {
        if (!Line()) return;
        std::string s;
        for (int i = 0; i < 4; i++)
        {
            uint64_t sec = 0, vt = 0, decl = 0;
            if (!ReadQ((const uint8_t*)self + 8 + 8 * i, sec) || !Plausible((const void*)sec)) { s += " -"; continue; }
            ReadQ((const void*)sec, vt); ReadQ((const uint8_t*)sec + 0x78, decl);
            uint32_t type = 0; ReadD((const uint8_t*)sec + 8, type);
            s += fmt::format(" [{}: {:X} vt exe+0x{:X} type {} decl {:X}]", i, sec, vt - (uint64_t)GameInfo::Base(), type, decl);
        }
        LOGI("MenuProbe: the options screen {}: sections{}", self, s);
    }
    uintptr_t HSectionsPause(void* self, void* file, void* a, void* b)
    {
        if (Line()) LOGI("MenuProbe: === the PAUSE menu's options screen builds its tabs (optionsData.sections)");
        Sections(self);
        t_Capture++; const uintptr_t r = g_SectionsPause(self, file, a, b); t_Capture--;
        return r;
    }
    uintptr_t HSectionsMain(void* self, void* file, void* a, void* b)
    {
        if (Line()) LOGI("MenuProbe: === the MAIN menu's options screen builds its tabs (optionsData.sections)");
        Sections(self);
        t_Capture++; const uintptr_t r = g_SectionsMain(self, file, a, b); t_Capture--;
        return r;
    }
    uintptr_t HShowSection(void* self, void* file, void* a, void* b)
    {
        if (Line())
        {
            uint64_t vt = 0, decl = 0; uint32_t type = 0;
            ReadQ(self, vt); ReadQ((const uint8_t*)self + 0x78, decl); ReadD((const uint8_t*)self + 8, type);
            LOGI("MenuProbe: === an options section is shown (optionsData.currentSection): {} vt exe+0x{:X} type {} decl {:X}", self, vt - (uint64_t)GameInfo::Base(), type, decl);
        }
        t_Capture++; const uintptr_t r = g_ShowSection(self, file, a, b); t_Capture--;
        if (Line()) LOGI("MenuProbe: === (section shown)");
        return r;
    }
    //! AS3 -> C++: the call's name (IggyExternalFunctionCallUTF8: name, length, +0x10 count, +0x18 arguments of 0x20 bytes:
    //! type at +0 (5 = a UTF-8 string at +0x10, length +0x18), the value at +0x10)
    uintptr_t HHub(void* self, void* player, void* call, void* x)
    {
        char name[128]; uint64_t np = 0;
        const bool named = call && ReadQ(call, np) && Text((const void*)np, name, sizeof name);
        bool menu = false;
        if (named)
        {
            g_Calls++;
            int n;
            { std::lock_guard lk(g_M); n = ++g_CallCount[name]; }
            menu = strstr(name, "Option") || strstr(name, "Setting") || strstr(name, "_C_") || strstr(name, "Menu") || strstr(name, "Slider")
                || strstr(name, "Stepper") || strstr(name, "Apply") || strstr(name, "Back") || strstr(name, "Default") || strstr(name, "Value");
            if ((n <= 40 || menu) && Line())
            {
                uint32_t cnt = 0; ReadD((const uint8_t*)call + 0x10, cnt);
                std::string a;
                for (uint32_t i = 0; i < cnt && i < 6; i++)
                {
                    const uint8_t* e = (const uint8_t*)call + 0x18 + 0x20 * i;
                    uint32_t t = 0; uint64_t v = 0; char b[128];
                    ReadD(e, t); ReadQ(e + 0x10, v);
                    double dv; memcpy(&dv, &v, 8);
                    if (t == 5 && Text((const void*)v, b, sizeof b)) a += fmt::format(" '{}'", b);
                    else a += fmt::format(" (type {}: {:X} = {})", t, v, dv);
                }
                LOGI("MenuProbe: AS3 calls {}({}){}", name, a, n == 40 && !menu ? " (no more of these logged)" : "");
            }
        }
        if (menu) t_Capture++;
        const uintptr_t r = g_Hub(self, player, call, x);
        if (menu) t_Capture--;
        return r;
    }
}

void MenuProbe::Install(bool on)
{
    if (!on || g_On.exchange(true)) return;
    const uintptr_t b = (uintptr_t)GameInfo::Base();
    g_Hub = (Fn3)(b + kHub); g_ObjCtor = (Fn3)(b + kObjCtor); g_ArrCtor = (Fn3)(b + kArrCtor);
    g_SetInt = (Fn3)(b + kSetInt); g_SetUint = (Fn3)(b + kSetUint); g_SetFloat = (Fn3)(b + kSetFloat); g_SetBool = (Fn3)(b + kSetBool);
    g_SetStr = (Fn3)(b + kSetStr); g_SetStrId = (Fn3)(b + kSetStrId); g_SetObj = (Fn3)(b + kSetObj); g_SetArr = (Fn3)(b + kSetArr);
    g_SectionsPause = (Fn2)(b + kSectionsPause); g_SectionsMain = (Fn2)(b + kSectionsMain); g_ShowSection = (Fn2)(b + kShowSection);
    g_FastName = (NameFn)(b + kFastName);
    // the prologues, as read from the exe (a different build: nothing is hooked)
    struct P { uintptr_t rva; uint8_t bytes[4]; } const pro[] = {
        { kHub, { 0x48, 0x89, 0x5C, 0x24 } }, { kObjCtor, { 0x48, 0x89, 0x4C, 0x24 } }, { kArrCtor, { 0x48, 0x89, 0x4C, 0x24 } },
        { kSetInt, { 0x41, 0x56, 0x48, 0x83 } }, { kSetUint, { 0x41, 0x56, 0x48, 0x83 } }, { kSetFloat, { 0x40, 0x57, 0x48, 0x83 } },
        { kSetBool, { 0x40, 0x57, 0x48, 0x83 } }, { kSetStr, { 0x48, 0x8B, 0xC4, 0x57 } }, { kSetStrId, { 0x48, 0x8B, 0xC4, 0x57 } },
        { kSetObj, { 0x40, 0x57, 0x48, 0x83 } }, { kSetArr, { 0x40, 0x57, 0x48, 0x83 } },
        { kFastName, { 0x48, 0x89, 0x5C, 0x24 } }, { kSectionsPause, { 0x48, 0x8B, 0xC4, 0x57 } }, { kSectionsMain, { 0x48, 0x8B, 0xC4, 0x57 } }, { kShowSection, { 0x48, 0x8B, 0xC4, 0x55 } } };
    for (const P& p : pro)
        // (a jmp there: another of the mod's hooks on the same function, VrMenu / MenuProbe -- fine)
        if (*(const uint8_t*)(b + p.rva) != 0xE9 && memcmp((const void*)(b + p.rva), p.bytes, 4) != 0)
        {
            LOGI("MenuProbe: exe+0x{:X} is not the expected function (another game build?): the menu probe is off", p.rva);
            return;
        }
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_Hub, HHub); DetourAttach(&(PVOID&)g_ObjCtor, HObjCtor); DetourAttach(&(PVOID&)g_ArrCtor, HArrCtor);
    DetourAttach(&(PVOID&)g_SetInt, HSetInt); DetourAttach(&(PVOID&)g_SetUint, HSetUint); DetourAttach(&(PVOID&)g_SetFloat, HSetFloat);
    DetourAttach(&(PVOID&)g_SetBool, HSetBool); DetourAttach(&(PVOID&)g_SetStr, HSetStr); DetourAttach(&(PVOID&)g_SetStrId, HSetStrId);
    DetourAttach(&(PVOID&)g_SetObj, HSetObj); DetourAttach(&(PVOID&)g_SetArr, HSetArr);
    DetourAttach(&(PVOID&)g_FastName, HFastName);
    DetourAttach(&(PVOID&)g_SectionsPause, HSectionsPause); DetourAttach(&(PVOID&)g_SectionsMain, HSectionsMain); DetourAttach(&(PVOID&)g_ShowSection, HShowSection);
    const LONG r = DetourTransactionCommit();
    LOGI("MenuProbe: the options screen's Flash traffic is logged (LogMenuData = 1; at most {} lines): detour={}", kMaxLines, r);
}

std::string MenuProbe::Stats()
{
    return fmt::format("menuprobe {} calls {} writes logged {} lines {}", g_On.load() ? 1 : 0, g_Calls.load(), g_Writes.load(), g_Lines.load());
}
