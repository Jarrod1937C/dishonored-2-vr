#include "pch.h"
#include "VrMenu.h"
#include "Settings.h"
#include "GameInfo.h"
#include "ControllerBindings.h"
#include "CrashLog.h"
#include <detours/detours.h>
#include <atomic>
#include <climits>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

// See VrMenu.h. Everything the page needs is written through the game's own AS3 helpers (arkIggyAS3Object and
// friends, exe+0x27xxx-0x2Axxx; their layout: claude/vr-options-menu-research.md), on the thread that runs the menu.
namespace
{
    struct VrRow { const char* main; const char* sub; const char* key; };
    struct VrGroup { const char* name; const char* desc; };
#include "VrMenuLayout.inc"
    // The VR tabs: one per ini main group, except System and Cheats, which share one (with six, the last tab was cut
    // off at the bottom of the screen's tab list). A tab of two groups names each subgroup with its group's name.
    struct VrTab { const char* label; const char* groups[2]; };
    constexpr VrTab kVrTabList[] = { { "Controls", { "Controls" } }, { "Gameplay", { "Gameplay" } }, { "Display", { "Display" } },
                                     { "Performance", { "Performance" } }, { "System/Cheats", { "System", "Cheats" } } };
    constexpr int kVrTabs = (int)(sizeof kVrTabList / sizeof kVrTabList[0]);

    // ---- the game's helpers (exe-relative) ----
    uintptr_t B() { return (uintptr_t)GameInfo::Base(); }
    template <class F> F At(uintptr_t rva) { return (F)(B() + rva); }
    using V1 = void* (*)(void*);
    using V2 = void* (*)(void*, void*);
    using V3 = void* (*)(void*, void*, const void*);
    using VI = void* (*)(void*, void*, int);
    using VN = void* (*)(void*, int);
    using VPathN = void* (*)(void*, void*, const char*, int);
    using VArgs = void* (*)(void*, void*, void*, int);
    using Set = bool (*)(void*, const void*, const void*);
    using CStrFn = const char* (*)(const void*);
    using Get = bool (*)(void*, void*, const void*);
    constexpr uintptr_t kObjPath = 0x28E50, kArrPath = 0x27F60, kElemCtor = 0x28140, kElemBind = 0x28450, kElemBegin = 0x283F0,
        kElemNext = 0x281A0, kElemOver = 0x280B0, kArrFresh = 0x27DF0, kArrSize = 0x29470, kDtor = 0x28FD0, kChildDtor = 0x28180,
        kMember = 0x296C0, kSetInt = 0x2A120, kSetUint = 0x2A1F0, kSetBool = 0x2A640, kSetStr = 0x2A720, kSetStrId = 0x2A530,
        kSetArr = 0x2A460, kMethod = 0x28B70, kRoot = 0x28EE0, kArgs = 0x28980, kCall = 0x28A90, kCStr = 0x1D9090,
        kGetInt = 0x29740;
    // the game's confirmation box (what its own tabs ask on Back with changes: exe+0x519500 / 0x517470): idStr ctor / dtor,
    // an AS3 object's ctor, a localized string id from its name
    constexpr uintptr_t kIdStr = 0x13F9D0, kIdStrDtor = 0x13FD10, kObjCtor = 0x28F80, kStrId = 0x165C80;
    constexpr uintptr_t kTabsPause = 0x516ED0, kTabsMain = 0x518F10, kSelPause = 0x5171F0, kSelMain = 0x519230,
        kBackPause = 0x517110, kBackMain = 0x519150;
    constexpr size_t kObj = 0x600;     // room for any of them (an object 0x2A8, an element 0x2D0, an array 0x570)

    //! an engine object in a zeroed buffer of our own
    struct Buf { std::unique_ptr<uint8_t[]> p{ new uint8_t[kObj]() }; uint8_t* get() const { return p.get(); } };
    struct Member { uint64_t name = 0; void* parent = nullptr; };
    Member Mem(void* parent, const char* name) { Member m; At<V3>(kMember)(parent, &m, name); return m; }
    void* Player(void* file) { return *(void**)((uint8_t*)file + 0x1608); }

    //! an AS3 object at a path
    struct Obj
    {
        Buf b; bool live = false;
        Obj(void* file, const std::string& path) { At<V3>(kObjPath)(b.get(), file, path.c_str()); live = true; }
        ~Obj() { if (live) At<V1>(kDtor)(b.get()); }
        void* get() const { return b.get(); }
        Obj(const Obj&) = delete; Obj& operator=(const Obj&) = delete;
    };
    //! an AS3 array object at a path (data / length), with an element walking its data
    struct PathArray
    {
        Buf a, e;
        PathArray(void* file, const char* path, int count)
        {
            At<V1>(kElemCtor)(e.get());
            At<V2>(kElemBind)(e.get(), file);
            At<VPathN>(kArrPath)(a.get(), file, path, count);
            At<VI>(kElemBegin)(e.get(), a.get() + 0x2C8, 0);
        }
        ~PathArray() { At<V1>(kChildDtor)(a.get() + 0x2C8); At<V1>(kDtor)(a.get()); At<V1>(kDtor)(e.get()); }
        void* elem() const { return e.get(); }
        void Next() { At<V1>(kElemNext)(e.get()); }
    };
    //! a new AS3 array of strings, then set as a member of `owner`
    void StringArray(void* file, void* owner, const Member& m, const std::vector<std::string>& v)
    {
        Buf arr, e;
        At<V2>(kArrFresh)(arr.get(), Player(file));
        At<VN>(kArrSize)(arr.get(), (int)v.size());
        At<VI>(kElemOver)(e.get(), arr.get(), 0);
        for (const std::string& s : v) { At<Set>(kSetStr)(e.get(), s.c_str(), nullptr); At<V1>(kElemNext)(e.get()); }
        At<Set>(kSetArr)(owner, arr.get(), &m);
        At<V1>(kDtor)(e.get());
        At<V1>(kDtor)(arr.get());
    }
    void SetStr(void* o, const Member& m, const std::string& s) { At<Set>(kSetStr)(o, s.c_str(), &m); }
    void SetInt(void* o, const Member& m, int v) { At<Set>(kSetInt)(o, &v, &m); }
    void SetUint(void* o, const Member& m, unsigned v) { At<Set>(kSetUint)(o, &v, &m); }
    void SetBool(void* o, const Member& m, bool v) { uint8_t b = v ? 1 : 0; At<Set>(kSetBool)(o, &b, &m); }
    //! an AS3 method of the file, called with no arguments
    bool CallAs3(void* file, const char* name)
    {
        Buf h, root; uint8_t args[0x18] = {}, vals[0x20] = {}, names[0x10] = {};
        At<V3>(kMethod)(h.get(), file, name);
        using RootFn = void* (*)(void*, void*, bool);
        At<RootFn>(kRoot)(root.get(), file, true);
        At<VArgs>(kArgs)(args, vals, names, 1);
        using CallFn = bool (*)(void*, void*, void*);
        const bool ok = At<CallFn>(kCall)(h.get(), root.get(), args);
        At<V1>(kDtor)(root.get());
        return ok;
    }

    // ---- what a setting shows as ----
    std::string Words(const char* camel)
    {   // "SmoothTurnSpeed" -> "Smooth turn speed"; runs of capitals (ASW, HUD) stay
        std::string out; const size_t n = strlen(camel);
        for (size_t i = 0; i < n; i++)
        {
            const char c = camel[i];
            const bool up = c >= 'A' && c <= 'Z';
            const bool prevLow = i > 0 && camel[i - 1] >= 'a' && camel[i - 1] <= 'z';
            const bool prevDigit = i > 0 && camel[i - 1] >= '0' && camel[i - 1] <= '9';
            const bool nextLow = i + 1 < n && camel[i + 1] >= 'a' && camel[i + 1] <= 'z';
            const bool prevUp = i > 0 && camel[i - 1] >= 'A' && camel[i - 1] <= 'Z';
            if (i > 0 && up && !prevDigit && (prevLow || (prevUp && nextLow))) out += ' ';
            const bool acronym = up && ((i + 1 < n && camel[i + 1] >= 'A' && camel[i + 1] <= 'Z') || (prevUp && !nextLow) || prevDigit);
            out += (i > 0 && up && !acronym) ? (char)(c - 'A' + 'a') : c;
        }
        // the usual short forms
        static const std::pair<const char*, const char*> kFix[] = { { "Hud", "HUD" }, { "hud", "HUD" }, { "Stereo3D", "Stereo 3D" },
            { "Vr ", "VR " }, { "vr ", "VR " }, { "Fov", "FOV" }, { "Far reach", "Far Reach" } };
        for (const auto& f : kFix)
            for (size_t at = out.find(f.first); at != std::string::npos; at = out.find(f.first, at + strlen(f.second)))
            {
                const size_t e = at + strlen(f.first);
                const bool wordStart = at == 0 || out[at - 1] == ' ', wordEnd = e >= out.size() || out[e] == ' ' || f.first[strlen(f.first) - 1] == ' ';
                if (wordStart && wordEnd) out.replace(at, strlen(f.first), f.second);
            }
        return out;
    }
    std::string Num(double v)
    {
        std::string s = fmt::format("{:.2f}", v);
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s == "-0" ? "0" : s;
    }
    struct Widget
    {
        enum { kBool, kStepper, kSlider } type = kBool;
        std::vector<std::string> shown, stored;   // a stepper's values as shown / as written to the ini
        int index = 0;                             // the stepper's / boolean's selection
        int lo = 0, hi = 100, step = 1, value = 0; // a slider (whole numbers)
    };
    bool Truthy(const std::string& v) { return v == "1" || v == "true" || v == "on" || v == "yes"; }
    Widget Make(const char* key, const Settings::KeyInfo& k)
    {
        Widget w;
        const std::string name = k.name;
        auto stepper = [&](std::vector<std::pair<std::string, std::string>> opts)
        {   // (stored, shown); the current value is kept even when it is none of them
            w.type = Widget::kStepper;
            int at = -1;
            for (size_t i = 0; i < opts.size(); i++)
                if (opts[i].first == k.value || (k.kind == 1 && fabs(atof(opts[i].first.c_str()) - atof(k.value.c_str())) < 1e-4)) at = (int)i;
            if (at < 0)
            {
                bool num = k.kind == 1; double cur = num ? atof(k.value.c_str()) : 0;
                size_t ins = opts.size();
                if (num) for (size_t i = 0; i < opts.size(); i++) if (atof(opts[i].first.c_str()) > cur) { ins = i; break; }
                opts.insert(opts.begin() + ins, { k.value, num ? Num(cur) : k.value });
                at = (int)ins;
            }
            for (auto& o : opts) { w.stored.push_back(o.first); w.shown.push_back(o.second); }
            w.index = at;
        };
        if (k.kind == 0) { w.type = Widget::kBool; w.index = Truthy(k.value) ? 1 : 0; return w; }
        if (name == "stereo3d") { stepper({ { "0", "Off (flat)" }, { "1", "On" }, { "2", "Alternating eyes (old)" } }); return w; }
        if (name == "viewdistance")
        {
            stepper({ { "-1", "As the game's menu" }, { "0", "Very low" }, { "1", "Low" }, { "2", "Medium" }, { "3", "High" }, { "4", "Very high" }, { "5", "Ultra" } });
            return w;
        }
        if (name == "outofviewmask") { stepper({ { "0", "Off" }, { "1", "Both sides" }, { "2", "Outer side only" } }); return w; }
        if (name == "hudresolution") { stepper({ { "0.25", "25%" }, { "0.5", "50%" }, { "0.75", "75%" }, { "1", "100%" } }); return w; }
        if (name == "fpscounter")
        {
            stepper({ { "off", "Off" }, { "top-left", "Top left" }, { "top-right", "Top right" }, { "bottom-left", "Bottom left" }, { "bottom-right", "Bottom right" } });
            return w;
        }
        if (name == "controllerbindings")
        {   // auto, or one of the profiles in VRControllerBindings, by a short name: the option box is narrow, and the long
            // form ("auto (for the controllers in use: now index)", "touch (Quest 1/2/3/Pro, Rift, PS VR2)") was too small
            // to read (reported 2026-11-01). "Auto (Index)" = auto, picking the Index profile now.
            auto shortName = [](const std::string& file, const std::string& label)
            {
                static const std::pair<const char*, const char*> kShort[] = {
                    { "index", "Index" }, { "touch", "Quest / Touch" }, { "pico", "Pico" }, { "hp_reverb_g2", "Reverb G2" },
                    { "vive_cosmos", "Vive Cosmos" }, { "vive_focus3", "Vive Focus 3" }, { "vive_wands", "Vive wands" }, { "wmr", "WMR" } };
                for (const auto& s : kShort) if (file == s.first) return std::string(s.second);
                return label.empty() || label.size() > 16 ? file : label;   // a profile added later: its label if short
            };
            std::string now = ControllerBindings::Active();
            std::string nowLabel;
            for (const auto& c : ControllerBindings::Choices()) if (c.first == now) nowLabel = c.second;
            std::vector<std::pair<std::string, std::string>> o{ { "auto", now.empty() ? std::string("Auto") : "Auto (" + shortName(now, nowLabel) + ")" } };
            for (const auto& c : ControllerBindings::Choices()) o.push_back({ c.first, shortName(c.first, c.second) });
            stepper(o); return w;
        }
        if (name == "renderscale")
        {
            std::vector<std::pair<std::string, std::string>> o{ { "0", "Window size" } };
            for (int i = 10; i <= 40; i++) o.push_back({ Num(i * 0.05), Num(i * 0.05) });
            stepper(o); return w;
        }
        if (k.kind == 2)
        {
            std::vector<std::pair<std::string, std::string>> o;
            size_t s = 0;
            while (s <= k.choices.size())
            {
                const size_t e = std::min(k.choices.find('|', s), k.choices.size());
                const std::string c = k.choices.substr(s, e - s);
                if (!c.empty()) o.push_back({ c, c });
                s = e + 1;
            }
            stepper(o); return w;
        }
        if (k.kind == 3)
        {
            if (name == "cheapfogcolor") { stepper({ { "auto", "auto (night / dusk / day)" }, { "sky", "sky (the horizon's colour)" } }); return w; }
            stepper({}); return w;
        }
        // a number: the coarsest step from this list that still gives enough positions, and that the value is on
        const double lo = k.lo, hi = k.hi, cur = atof(k.value.c_str()), r = hi - lo;
        static const double kSteps[] = { 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 25, 50, 100, 200, 500 };
        if (r <= 0) { stepper({}); return w; }
        const double def = atof(k.def.c_str());
        auto integral = [](double v) { return std::floor(v) == v; };
        const bool whole = integral(lo) && integral(hi) && integral(cur) && integral(def);
        double step = 0.05;
        for (double s : kSteps) if (r / s <= 50) { step = s; break; }
        if (whole && step < 1) step = 1;
        if (whole && r > 12)
        {   // a slider of whole numbers: about 100 positions at most, finer until the value is on one
            double s2 = 1;
            for (double s : kSteps) if (s >= 1 && r / s <= 100) { s2 = s; break; }
            while (s2 > 1 && std::fmod(cur, s2) != 0)
            {
                double next = 1;
                for (double s : kSteps) if (s >= 1 && s < s2) next = s;
                s2 = next;
            }
            w.type = Widget::kSlider; w.step = (int)s2;
            w.lo = (int)(std::floor(lo / s2) * s2); w.hi = (int)(std::ceil(hi / s2) * s2); w.value = (int)cur;
            return w;
        }
        std::vector<std::pair<std::string, std::string>> o;
        for (int i = 0; lo + i * step <= hi + 1e-6; i++) { const std::string v = Num(lo + i * step); o.push_back({ v, v }); }
        stepper(o);
        return w;
    }

    //! the recommended value as the control shows it ("" = none)
    std::string RecText(const Widget& w, const Settings::KeyInfo& k)
    {
        if (k.rec.empty()) return "";
        if (w.type == Widget::kBool) return Truthy(k.rec) ? "on" : "off";
        if (w.type == Widget::kSlider) return Num(atof(k.rec.c_str()));
        for (size_t i = 0; i < w.stored.size(); i++)
            if (w.stored[i] == k.rec || (k.kind == 1 && fabs(atof(w.stored[i].c_str()) - atof(k.rec.c_str())) < 1e-4))
                return w.shown[i].substr(0, w.shown[i].find(" ("));   // without its explanation
        return k.kind == 1 ? Num(atof(k.rec.c_str())) : k.rec;
    }

    // ---- the tabs ----
    std::atomic<unsigned long long> g_Tabs{ 0 }, g_Pages{ 0 }, g_Backs{ 0 }, g_Fail{ 0 };
    using Fn = uintptr_t (*)(void*, void*, void*, void*);
    Fn g_TabsPause, g_TabsMain, g_SelPause, g_SelMain, g_BackPause, g_BackMain;

    int GameTabs(void* ctx, size_t slots)
    {
        int n = 0;
        for (int i = 0; i < 4; i++) if (*(void**)((uint8_t*)ctx + slots + 8 * i)) n++;
        return n;
    }
    //! the tab list again: the game's own tabs, then the VR ones
    void Tabs(void* ctx, void* file, size_t slots)
    {
        const int nGame = GameTabs(ctx, slots);
        Buf e, a;
        At<V1>(kElemCtor)(e.get());
        At<V2>(kElemBind)(e.get(), file);
        const Member mLabel = Mem(e.get(), "label"), mClick = Mem(e.get(), "gameCallbackOnClick"), mClass = Mem(e.get(), "screenClassName");
        At<VPathN>(kArrPath)(a.get(), file, "optionsData.sections", nGame + kVrTabs);
        At<VI>(kElemBegin)(e.get(), a.get() + 0x2C8, 0);
        for (int i = 0; i < 4; i++)
        {
            uint8_t* sec = *(uint8_t**)((uint8_t*)ctx + slots + 8 * i);
            if (!sec) continue;
            uint8_t* decl = *(uint8_t**)(sec + 0x78);
            At<Set>(kSetStrId)(e.get(), decl + 0x60, &mLabel);
            SetStr(e.get(), mClass, At<CStrFn>(kCStr)(decl + 0x68));
            SetStr(e.get(), mClick, "OnOptionSectionSelected");
            At<V1>(kElemNext)(e.get());
        }
        for (int t = 0; t < kVrTabs; t++)
        {
            SetStr(e.get(), mLabel, std::string("VR ") + kVrTabList[t].label);
            SetStr(e.get(), mClass, "");
            SetStr(e.get(), mClick, "OnOptionSectionSelected");
            At<V1>(kElemNext)(e.get());
        }
        At<V1>(kChildDtor)(a.get() + 0x2C8); At<V1>(kDtor)(a.get()); At<V1>(kDtor)(e.get());
        if (g_Tabs++ == 0) LOGI("VrMenu: the Options screen's tabs: the game's {} and {} VR ones", nGame, kVrTabs);
    }
    // ---- the page shown, for Back ----
    struct Item { std::string label; unsigned cat; Widget w; std::string key, value; int kind; };
    const char* WidgetName(const Widget& w) { return w.type == Widget::kBool ? "optionStepperBoolean" : w.type == Widget::kSlider ? "slider" : "optionStepper"; }
    struct Shown { int tab = -1; std::vector<Item> items; } g_Shown;   // the menu's thread only
    void (*g_Apply)(const std::vector<std::string>&) = nullptr;
    // Actions: a row that is not a setting (its key starts with '@'), a switch turned on and confirmed on Back like a
    // change. "@savecrashlog": the newest crash report (CrashLog) and its minidump copied to the desktop.
    std::string g_ActionResult;   // what the last action did (the log has it)
    int g_ActionOk = -1;          // -1 not done yet, 0 failed, 1 done: shown on its row the next time the page opens
    void RunAction(const std::string& key)
    {
        if (key == "@savecrashlog") { g_ActionOk = CrashLog::SaveLatestToDesktop(g_ActionResult) ? 1 : 0; return; }
        LOGI("VrMenu: unknown action {}", key);
    }

    //! VR tab t's page, then the AS3 method that shows it
    void Page(void* file, int t)
    {
        const VrTab& tab = kVrTabList[t];
        const char* group = tab.label;
        const bool several = tab.groups[1] != nullptr;
        std::vector<std::string> cats, catSrc;
        std::vector<Item> items;
        for (const char* g : tab.groups)
        {
        for (const VrRow& r : kVrRows)
        {
            if (!g || strcmp(r.main, g) != 0) continue;
            Settings::KeyInfo k;
            if (!Settings::Info(r.key, k) || k.hidden) continue;
            const std::string sub = several ? std::string(g) + ": " + Words(r.sub) : Words(r.sub);
            if (cats.empty() || cats.back() != sub) { cats.push_back(sub); catSrc.push_back(""); }
            if (!k.source.empty()) catSrc.back() = k.source;   // a controller profile's: saved into that file
            Widget w = Make(r.key, k);
            const std::string rec = RecText(w, k);
            // "Render scale (restart, recommended: 1)": a change of a restart one is saved and used next time
            // what a setting needs, first ("Foveated rendering (NVIDIA GTX 16 / RTX only, recommended: off)")
            static const std::pair<const char*, const char*> kNeeds[] = { { "foveatedrendering", "NVIDIA GTX 16 / RTX only" } };
            std::string notes;
            for (const auto& n : kNeeds) if (k.name == n.first) notes = n.second;
            if (k.restart) notes += (notes.empty() ? "" : ", ") + std::string("restart");
            if (!rec.empty()) notes += (notes.empty() ? "" : ", ") + std::string("recommended: ") + rec;
            items.push_back({ Words(r.key) + (notes.empty() ? "" : " (" + notes + ")"), (unsigned)(cats.size() - 1), w, r.key, k.value, k.kind });
        }
        if (g && strcmp(g, "System") == 0)
        {   // the crash report: switch it on and confirm on Back to copy the newest one to the desktop
            cats.push_back(several ? std::string(g) + ": Crash log" : std::string("Crash log")); catSrc.push_back("");
            const std::string latest = CrashLog::LatestReport();
            Widget w; w.type = Widget::kBool; w.index = 0;
            // short (a long label is cut off in the row; the section's name says what it copies): the file's name and the
            // result are in the log
            std::string label = latest.empty() ? "Copy to desktop (none)" : g_ActionOk == 1 ? "Copy to desktop (copied)" : g_ActionOk == 0 ? "Copy to desktop (failed)" : "Copy to desktop";
            items.push_back({ label, (unsigned)(cats.size() - 1), w, "@savecrashlog", "0", 0 });
        }
        }
        {
            Obj s(file, "optionsData.currentSection");
            const Member mLabel = Mem(s.get(), "label"), mClass = Mem(s.get(), "screenClassName"), mCats = Mem(s.get(), "categories");
            SetStr(s.get(), mLabel, std::string("VR ") + group);
            SetStr(s.get(), mClass, "");
            // "Controllers (index.json)": the subgroup's settings come from (and are saved into) that controller profile
            for (size_t i = 0; i < cats.size(); i++) if (!catSrc[i].empty()) cats[i] += " (" + catSrc[i] + ")";
            StringArray(file, s.get(), mCats, cats);
            PathArray list(file, "optionsData.currentSection.settings", (int)items.size());
            const Member mL = Mem(list.elem(), "label"), mC = Mem(list.elem(), "categoryIndex");
            for (size_t i = 0; i < items.size(); i++)
            {
                const Item& it = items[i];
                SetStr(list.elem(), mL, it.label);
                SetUint(list.elem(), mC, it.cat);
                const char* kind = WidgetName(it.w);
                Obj w(file, fmt::format("optionsData.currentSection.settings.data[{}].{}", i, kind));
                SetBool(w.get(), Mem(w.get(), "refreshEnabled"), false);
                SetBool(w.get(), Mem(w.get(), "locked"), false);
                if (it.w.type == Widget::kSlider)
                {
                    SetInt(w.get(), Mem(w.get(), "minimum"), it.w.lo);
                    SetInt(w.get(), Mem(w.get(), "maximum"), it.w.hi);
                    SetInt(w.get(), Mem(w.get(), "interval"), it.w.step);
                    SetBool(w.get(), Mem(w.get(), "displayValue"), true);
                    SetInt(w.get(), Mem(w.get(), "value"), it.w.value);
                }
                else
                {
                    if (it.w.type == Widget::kStepper) StringArray(file, w.get(), Mem(w.get(), "values"), it.w.shown);
                    SetInt(w.get(), Mem(w.get(), "selectedIndex"), it.w.index);
                }
                list.Next();
            }
        }
        g_Shown.tab = t; g_Shown.items = std::move(items);
        const bool ok = CallAs3(file, "_C_ShowOptionSection");
        if (!ok) g_Fail++;
        if (g_Pages++ < 20) LOGI("VrMenu: page 'VR {}': {} settings in {} sections{}", group, g_Shown.items.size(), cats.size(), ok ? "" : " (the menu did not take _C_ShowOptionSection)");
    }

    // ---- Back: the values read back, Save / Cancel ----
    bool Same(const std::string& a, const std::string& b, int kind)
    {
        if (a == b) return true;
        if (kind == 0) return Truthy(a) == Truthy(b);
        if (kind == 1) return fabs(atof(a.c_str()) - atof(b.c_str())) < 1e-4;
        return false;
    }
    //! what the page's widgets hold now that differs from the values it was shown with: (key, value as the ini has it)
    std::vector<std::pair<std::string, std::string>> Changes(void* file, std::string& said)
    {
        std::vector<std::pair<std::string, std::string>> out;
        int bad = 0;
        for (size_t i = 0; i < g_Shown.items.size(); i++)
        {
            const Item& it = g_Shown.items[i];
            Obj w(file, fmt::format("optionsData.currentSection.settings.data[{}].{}", i, WidgetName(it.w)));
            const Member m = Mem(w.get(), it.w.type == Widget::kSlider ? "value" : "selectedIndex");
            int v = INT_MIN;
            if (!At<Get>(kGetInt)(w.get(), &v, &m)) { bad++; continue; }
            std::string now;
            if (it.w.type == Widget::kBool) now = v ? "1" : "0";
            else if (it.w.type == Widget::kSlider) now = std::to_string(v);
            else if (v >= 0 && v < (int)it.w.stored.size()) now = it.w.stored[v];
            else { bad++; continue; }
            if (!Same(now, it.value, it.kind)) { out.push_back({ it.key, now }); said += fmt::format(" {}: {} -> {}", it.key, it.value, now); }
        }
        if (bad) said += fmt::format(" ({} widgets unreadable: left as they were)", bad);
        return out;
    }
    struct Pending { bool waiting = false; void* file = nullptr; std::vector<std::pair<std::string, std::string>> changes; } g_Pending;
    std::atomic<unsigned long long> g_Saves{ 0 }, g_Cancels{ 0 }, g_Boxes{ 0 };
    void Leave(void* file) { if (!CallAs3(file, "_C_BackFromOptionSection")) g_Fail++; }
    void Commit(const std::vector<std::pair<std::string, std::string>>& all)
    {
        std::vector<std::pair<std::string, std::string>> changes;
        for (const auto& c : all)
            if (!c.first.empty() && c.first[0] == '@') { if (Truthy(c.second)) RunAction(c.first); }
            else changes.push_back(c);
        if (changes.empty()) return;
        std::vector<std::string> live; std::string err;
        if (!Settings::Save(changes, live, err)) { LOGI("VrMenu: the settings could not be saved: {}", err); return; }
        g_Saves++;
        if (!live.empty() && g_Apply) g_Apply(live);
    }
    //! the box's answer (the game's arkMessageBoxCallback: vtable { dtor, invoke }, invoke(this, ?, answer) -- its own
    //! one, exe+0xC8B10, calls a member function of the options screen with the same arguments, exe+0x5171B0: answer != 0
    //! = Yes, apply)
    struct Callback { void* const* vtbl; void* unused[2]; };
    void* CbDtor(Callback* self, unsigned) { return self; }
    void CbInvoke(Callback*, void*, int answer)
    {
        if (!g_Pending.waiting) return;
        g_Pending.waiting = false;
        LOGI("VrMenu: the confirmation box: {}", answer ? "Yes: saving" : "No: the changes are dropped");
        if (answer) Commit(g_Pending.changes); else g_Cancels++;
        g_Pending.changes.clear();
        Leave(g_Pending.file);
    }
    void* const kCbVtbl[2] = { (void*)&CbDtor, (void*)&CbInvoke };
    Callback g_Cb{ kCbVtbl, { nullptr, nullptr } };
    //! the game's message-box manager for this options screen (as exe+0x519500 / 0x517470 find it); no C++ objects (__try)
    void* BoxManager(void* ctx, bool pause)
    {
        __try
        {
            uint8_t* a = *(uint8_t**)((uint8_t*)ctx + (pause ? 0x40 : 0x08)); if (!a) return nullptr;
            uint8_t* b = *(uint8_t**)(a + 8); if (!b) return nullptr;
            void* x = *(void**)(b + 8); if (!x) return nullptr;
            using VF = void* (*)(void*);
            return (*(VF**)x)[0x70 / 8](x);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    //! the game's "apply the changes?" box (its text: ui_main_settings_confirmation), answered through g_Cb
    bool ShowBox(void* mgr)
    {
        __try
        {
            alignas(16) uint8_t req[0x300] = {};
            At<V2>(kIdStr)(req + 0x08, (void*)"");
            At<V1>(kObjCtor)(req + 0x38);
            At<V2>(kStrId)(req + 0x04, (void*)"ui_main_settings_confirmation");
            *(int*)(req + 0x30) = 2;   // two buttons
            using ShowFn = int (*)(void*, void*, void*, void*, void*);
            (*(ShowFn**)mgr)[0x18 / 8](mgr, req, nullptr, &g_Cb, nullptr);
            At<V1>(kDtor)(req + 0x38);
            At<V1>(kIdStrDtor)(req + 0x08);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    int Index(void* args)
    {   // the AS3 call's first argument (a number)
        if (!args || (*(uint32_t*)((uint8_t*)args + 8) & 0xFFFFFF) < 1) return -1;
        const uint8_t* a0 = *(const uint8_t* const*)args;
        return a0 ? (int)*(const double*)(a0 + 0x10) : -1;
    }
    uintptr_t HTabsPause(void* ctx, void* file, void* a, void* b) { const uintptr_t r = g_TabsPause(ctx, file, a, b); Tabs(ctx, file, 0x08); return r; }
    uintptr_t HTabsMain(void* ctx, void* file, void* a, void* b) { const uintptr_t r = g_TabsMain(ctx, file, a, b); Tabs(ctx, file, 0x30); return r; }
    uintptr_t Sel(Fn real, size_t slots, void* ctx, void* file, void* args, void* x)
    {
        const int i = Index(args), nGame = ctx ? GameTabs(ctx, slots) : 4;
        if (ctx && file && i >= nGame && i < nGame + kVrTabs) { Page(file, i - nGame); return 0; }
        return real(ctx, file, args, x);
    }
    uintptr_t Back(Fn real, size_t slots, void* ctx, void* file, void* args, void* x)
    {
        const int i = Index(args), nGame = ctx ? GameTabs(ctx, slots) : 4;
        if (ctx && file && i >= nGame && i < nGame + kVrTabs)
        {
            g_Backs++;
            std::string said;
            std::vector<std::pair<std::string, std::string>> changes;
            if (g_Shown.tab == i - nGame) changes = Changes(file, said);
            if (g_Backs <= 20 || !changes.empty())
                LOGI("VrMenu: back from 'VR {}': {} settings, {} changed{}", kVrTabList[i - nGame].label, g_Shown.items.size(), changes.size(), said);
            if (changes.empty()) { Leave(file); return 0; }
            // as the game's own tabs: "apply the changes?" -- Yes saves (DishonoredVR.ini, and the settings that can be
            // used at once are), No drops them; the page closes after the answer
            g_Pending.waiting = true; g_Pending.file = file; g_Pending.changes = std::move(changes);
            void* mgr = BoxManager(ctx, slots == 0x08);
            if (mgr && ShowBox(mgr)) { g_Boxes++; return 0; }
            LOGI("VrMenu: the game's confirmation box could not be shown: saving without asking");
            g_Pending.waiting = false;
            Commit(g_Pending.changes); g_Pending.changes.clear();
            Leave(file);
            return 0;
        }
        return real(ctx, file, args, x);
    }
    uintptr_t HSelPause(void* c, void* f, void* a, void* x) { return Sel(g_SelPause, 0x08, c, f, a, x); }
    uintptr_t HSelMain(void* c, void* f, void* a, void* x) { return Sel(g_SelMain, 0x30, c, f, a, x); }
    uintptr_t HBackPause(void* c, void* f, void* a, void* x) { return Back(g_BackPause, 0x08, c, f, a, x); }
    uintptr_t HBackMain(void* c, void* f, void* a, void* x) { return Back(g_BackMain, 0x30, c, f, a, x); }
}

void VrMenu::Install(bool on, void (*apply)(const std::vector<std::string>& changed))
{
    g_Apply = apply;
    if (!on) { LOGI("VrMenu: no VR tabs in the Options screen (VrOptionsMenu = 0)"); return; }
    const uintptr_t b = B();
    struct P { uintptr_t rva; uint8_t bytes[6]; } const pro[] = {
        { kTabsPause, { 0x48, 0x8B, 0xC4, 0x57, 0x48, 0x81 } }, { kTabsMain, { 0x48, 0x8B, 0xC4, 0x57, 0x48, 0x81 } },
        { kSelPause, { 0x48, 0x89, 0x74, 0x24, 0x10, 0x57 } }, { kSelMain, { 0x48, 0x89, 0x74, 0x24, 0x10, 0x57 } },
        { kBackPause, { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x20 } }, { kBackMain, { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x20 } },
        { kObjPath, { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x57 } }, { kArrPath, { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x57 } },
        { kMember, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57 } }, { kCStr, { 0x48, 0x8B, 0x01, 0xC3, 0xCC, 0xCC } },
        { kCall, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48 } }, { kMethod, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57 } },
        { kGetInt, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 } }, { kIdStr, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48 } },
        { kIdStrDtor, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30 } }, { kObjCtor, { 0x48, 0x8D, 0x05, 0x49, 0x42, 0xA9 } },
        { kStrId, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57 } } };
    for (const P& p : pro)
        // (a jmp there: another of the mod's hooks on the same function, VrMenu / MenuProbe -- fine)
        if (*(const uint8_t*)(b + p.rva) != 0xE9 && memcmp((const void*)(b + p.rva), p.bytes, 6) != 0)
        {
            LOGI("VrMenu: exe+0x{:X} is not the expected function (another game build?): no VR tabs", p.rva);
            return;
        }
    g_TabsPause = (Fn)(b + kTabsPause); g_TabsMain = (Fn)(b + kTabsMain); g_SelPause = (Fn)(b + kSelPause); g_SelMain = (Fn)(b + kSelMain);
    g_BackPause = (Fn)(b + kBackPause); g_BackMain = (Fn)(b + kBackMain);
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_TabsPause, HTabsPause); DetourAttach(&(PVOID&)g_TabsMain, HTabsMain);
    DetourAttach(&(PVOID&)g_SelPause, HSelPause); DetourAttach(&(PVOID&)g_SelMain, HSelMain);
    DetourAttach(&(PVOID&)g_BackPause, HBackPause); DetourAttach(&(PVOID&)g_BackMain, HBackMain);
    const LONG r = DetourTransactionCommit();
    LOGI("VrMenu: {} VR tabs in the Options screen (main menu and pause menu), {} settings: detour={}", kVrTabs,
         sizeof kVrRows / sizeof kVrRows[0], r);
}

std::string VrMenu::Stats()
{
    return fmt::format("vrmenu tabs built {} pages {} backs {} boxes {} saves {} cancels {} failed calls {}", g_Tabs.load(), g_Pages.load(),
                       g_Backs.load(), g_Boxes.load(), g_Saves.load(), g_Cancels.load(), g_Fail.load());
}
