#include "pch.h"
#include "ControllerBindings.h"
#include "Settings.h"
#include "GameInfo.h"
#include <algorithm>
#include <map>
#include <mutex>

namespace
{
    // ---- a small JSON reader: objects, arrays, strings, numbers, true / false / null ----
    struct J
    {
        enum T { kNull, kBool, kNum, kStr, kArr, kObj } t = kNull;
        std::string s;                                     // a string, or a number / bool as written
        std::vector<J> a;
        std::vector<std::pair<std::string, J>> o;          // in file order
        size_t at = 0, end = 0;                            // where the value is in the text (for writing it back)
        const J* Get(const std::string& k) const { for (auto& p : o) if (p.first == k) return &p.second; return nullptr; }
    };
    struct Parser
    {
        const std::string& x; size_t i = 0; std::string err;
        explicit Parser(const std::string& t) : x(t) {}
        void Ws() { while (i < x.size() && (x[i] == ' ' || x[i] == '\t' || x[i] == '\r' || x[i] == '\n')) i++; }
        bool Fail(const char* what) { if (err.empty()) err = fmt::format("{} at character {}", what, i); return false; }
        bool Str(std::string& out)
        {
            if (i >= x.size() || x[i] != '"') return Fail("a string expected");
            i++;
            while (i < x.size() && x[i] != '"')
            {
                char ch = x[i++];
                if (ch == '\\' && i < x.size())
                {
                    const char e = x[i++];
                    ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
                    if (e == 'u') { i += 4; ch = '?'; }       // (no \u in these files)
                }
                out += ch;
            }
            if (i >= x.size()) return Fail("an unterminated string");
            i++;
            return true;
        }
        bool Val(J& v, int depth = 0)
        {
            if (depth > 32) return Fail("too deep");
            Ws(); v.at = i;
            if (i >= x.size()) return Fail("a value expected");
            const char ch = x[i];
            if (ch == '{')
            {
                v.t = J::kObj; i++; Ws();
                if (i < x.size() && x[i] == '}') { i++; v.end = i; return true; }
                for (;;)
                {
                    Ws(); std::string k; if (!Str(k)) return false;
                    Ws(); if (i >= x.size() || x[i] != ':') return Fail("':' expected");
                    i++; J e; if (!Val(e, depth + 1)) return false;
                    v.o.push_back({ k, std::move(e) });
                    Ws(); if (i < x.size() && x[i] == ',') { i++; continue; }
                    if (i < x.size() && x[i] == '}') { i++; v.end = i; return true; }
                    return Fail("',' or '}' expected");
                }
            }
            if (ch == '[')
            {
                v.t = J::kArr; i++; Ws();
                if (i < x.size() && x[i] == ']') { i++; v.end = i; return true; }
                for (;;)
                {
                    J e; if (!Val(e, depth + 1)) return false;
                    v.a.push_back(std::move(e));
                    Ws(); if (i < x.size() && x[i] == ',') { i++; continue; }
                    if (i < x.size() && x[i] == ']') { i++; v.end = i; return true; }
                    return Fail("',' or ']' expected");
                }
            }
            if (ch == '"') { v.t = J::kStr; if (!Str(v.s)) return false; v.end = i; return true; }
            size_t b = i;
            while (i < x.size() && (isalnum((unsigned char)x[i]) || x[i] == '-' || x[i] == '+' || x[i] == '.')) i++;
            v.s = x.substr(b, i - b); v.end = i;
            if (v.s == "true" || v.s == "false") { v.t = J::kBool; return true; }
            if (v.s == "null") { v.t = J::kNull; return true; }
            char* e = nullptr; strtod(v.s.c_str(), &e);
            if (v.s.empty() || *e) return Fail("a value expected");
            v.t = J::kNum; return true;
        }
    };
    std::string Lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); }); return s; }

    struct Profile
    {
        std::string file;                                   // name without .json, lower case
        std::wstring path;
        std::string name, label;                            // its "name", its "label" (for the VR options tabs)
        std::vector<std::string> match;                     // interaction profile paths
        std::vector<std::pair<std::string, std::string>> settings;   // (key as written, value as the ini would have it)
        std::vector<std::pair<std::string, std::string>> profiles;   // (interaction profile path, extension or "")
        std::vector<std::pair<std::string, std::string>> bindings;   // (action, input path)
    };
    std::mutex g_M;
    std::vector<Profile> g_Profiles;                        // by file name
    int g_Active = -1;
    std::string g_Left, g_Right;                            // the controllers the runtime reported
    void (*g_Apply)(const std::vector<std::string>&) = nullptr;
    std::wstring Dir() { return GameInfo::GameDir() + L"\\VRControllerBindings"; }

    std::string ReadFile(const std::wstring& p, bool& ok)
    {
        std::string t; ok = false;
        FILE* f = _wfopen(p.c_str(), L"rb");
        if (!f) return t;
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) t.append(buf, n);
        fclose(f); ok = true;
        if (t.size() >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) t.erase(0, 3);
        return t;
    }
    std::string Narrow(const std::wstring& w) { std::string s; for (wchar_t c : w) s += c < 128 ? (char)c : '_'; return s; }

    void Scan()
    {
        g_Profiles.clear();
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((Dir() + L"\\*.json").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) { LOGI("ControllerBindings: no VRControllerBindings\\*.json in the game folder: the ini's controller settings"); return; }
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            Profile p; p.path = Dir() + L"\\" + fd.cFileName;
            std::string fn = Narrow(fd.cFileName); p.file = Lower(fn.substr(0, fn.size() - 5));
            bool ok = false; const std::string text = ReadFile(p.path, ok);
            if (!ok) { LOGI("ControllerBindings: {} could not be read", fn); continue; }
            Parser ps(text); J root;
            if (!ps.Val(root) || root.t != J::kObj) { LOGI("ControllerBindings: {} is not valid JSON ({}): skipped", fn, ps.err.empty() ? "not an object" : ps.err); continue; }
            if (const J* n = root.Get("name"); n && n->t == J::kStr) p.name = n->s;
            if (const J* n = root.Get("label"); n && n->t == J::kStr) p.label = n->s;
            if (const J* m = root.Get("match"); m && m->t == J::kArr) for (const J& e : m->a) if (e.t == J::kStr) p.match.push_back(Lower(e.s));
            if (const J* pr = root.Get("profiles"); pr && pr->t == J::kArr)
                for (const J& e : pr->a)
                {
                    const J* path = e.t == J::kObj ? e.Get("path") : nullptr; const J* ext = e.t == J::kObj ? e.Get("extension") : nullptr;
                    if (!path || path->t != J::kStr) { LOGI("ControllerBindings: {}: a \"profiles\" entry without a \"path\": skipped", fn); continue; }
                    p.profiles.push_back({ path->s, ext && ext->t == J::kStr ? ext->s : std::string() });
                    p.match.push_back(Lower(path->s));
                }
            if (const J* bd = root.Get("bindings"); bd && bd->t == J::kObj)
                for (const auto& kv : bd->o)
                {
                    if (kv.second.t == J::kStr) p.bindings.push_back({ kv.first, kv.second.s });
                    else if (kv.second.t == J::kArr) { for (const J& e : kv.second.a) if (e.t == J::kStr) p.bindings.push_back({ kv.first, e.s }); }
                    else LOGI("ControllerBindings: {}: binding '{}' is not a path or a list of paths: skipped", fn, kv.first);
                }
            if (const J* st = root.Get("settings"); st && st->t == J::kObj)
                for (const auto& kv : st->o)
                {
                    if (kv.second.t == J::kObj || kv.second.t == J::kArr || kv.second.t == J::kNull) { LOGI("ControllerBindings: {}: {} is not a value: skipped", fn, kv.first); continue; }
                    const std::string v = kv.second.t == J::kBool ? (kv.second.s == "true" ? "1" : "0") : kv.second.s;
                    p.settings.push_back({ kv.first, v });
                }
            g_Profiles.push_back(std::move(p));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(g_Profiles.begin(), g_Profiles.end(), [](const Profile& a, const Profile& b) { return a.file < b.file; });
        std::string all;
        for (const Profile& p : g_Profiles) all += fmt::format(" {} ({}, {} settings, {} bindings, for {})", p.file, p.name, p.settings.size(), p.bindings.size(), p.match.empty() ? std::string("any") : p.match[0]);
        LOGI("ControllerBindings: {} profile(s) in VRControllerBindings:{}", g_Profiles.size(), all);
    }
    //! The profile to use now (g_M held): the ini's choice, else the reported controller's, else the first
    int Choose(std::string& why)
    {
        if (g_Profiles.empty()) { why = "none there"; return -1; }
        const std::string want = Lower(Settings::ControllerBindings());
        if (!want.empty() && want != "auto")
        {
            for (size_t i = 0; i < g_Profiles.size(); i++) if (g_Profiles[i].file == want) { why = "ControllerBindings = " + want; return (int)i; }
            why = fmt::format("ControllerBindings = {} is not there: the first one", want);
            return 0;
        }
        if (!g_Left.empty() || !g_Right.empty())
        {
            for (size_t i = 0; i < g_Profiles.size(); i++)
                for (const std::string& m : g_Profiles[i].match)
                    if (m == g_Right || m == g_Left) { why = "for the controllers the runtime bound (" + m + ")"; return (int)i; }
            why = fmt::format("no profile is for the controllers the runtime bound ({} / {}): the first one", g_Left, g_Right);
            return 0;
        }
        why = "the first one, until the runtime says which controllers these are";
        return 0;
    }
    //! Uses profile `idx` (g_M not held): its settings into Settings; after startup the modules are told what changed
    void Use(int idx, const std::string& why, bool startup)
    {
        std::vector<std::pair<std::string, std::string>> vals; std::string file;
        {
            std::lock_guard lk(g_M);
            const int was = g_Active; g_Active = idx;
            if (idx < 0) { if (was >= 0) LOGI("ControllerBindings: no profile in use"); return; }
            vals = g_Profiles[idx].settings; file = g_Profiles[idx].file;
            if (was == idx && !startup) return;
        }
        std::vector<std::string> changed;
        Settings::ApplyOverrides(vals, file + ".json", changed);
        std::string said; for (const auto& v : vals) said += fmt::format(" {}={}", v.first, v.second);
        LOGI("ControllerBindings: {}.json in use ({}):{}", file, why, said);
        if (!startup && !changed.empty() && g_Apply) g_Apply(changed);
    }
}

void ControllerBindings::Load()
{
    int idx; std::string why;
    { std::lock_guard lk(g_M); Scan(); idx = Choose(why); }
    Use(idx, why, true);
}
void ControllerBindings::SetApply(void (*apply)(const std::vector<std::string>&)) { g_Apply = apply; }
void ControllerBindings::OnProfile(const std::string& left, const std::string& right)
{
    int idx; std::string why;
    {
        std::lock_guard lk(g_M);
        std::string l = Lower(left), r = Lower(right);
        if (l == "none") l.clear();
        if (r == "none") r.clear();
        // "none" for both (the runtime sends it first at every session start, and when the controllers sleep): not a
        // controller -- the profile in use stays (else it flipped to the first file and back, re-applying both)
        if (l.empty() && r.empty()) return;
        if (l == g_Left && r == g_Right) return;
        g_Left = l; g_Right = r;
        idx = Choose(why);
    }
    Use(idx, why, false);
}
void ControllerBindings::Reselect()
{
    int idx; std::string why;
    { std::lock_guard lk(g_M); idx = Choose(why); }
    Use(idx, why, false);
}
std::string ControllerBindings::Active() { std::lock_guard lk(g_M); return g_Active >= 0 ? g_Profiles[g_Active].file : std::string(); }
std::vector<std::string> ControllerBindings::Names()
{
    std::lock_guard lk(g_M);
    std::vector<std::string> n; for (const Profile& p : g_Profiles) n.push_back(p.file);
    return n;
}
std::vector<std::pair<std::string, std::string>> ControllerBindings::Choices()
{
    std::lock_guard lk(g_M);
    std::vector<std::pair<std::string, std::string>> c;
    for (const Profile& p : g_Profiles) c.push_back({ p.file, !p.label.empty() ? p.label : p.name.substr(0, p.name.find(" (")) });   // ("Valve Index (Knuckles)": "Valve Index")
    return c;
}
bool ControllerBindings::Owns(const std::string& bare)
{
    std::lock_guard lk(g_M);
    if (g_Active < 0) return false;
    const std::string b = Lower(bare);
    for (const auto& kv : g_Profiles[g_Active].settings) if (Lower(kv.first) == b) return true;
    return false;
}
std::vector<ControllerBindings::Suggestion> ControllerBindings::Suggestions()
{
    std::lock_guard lk(g_M);
    std::vector<Suggestion> out;
    for (const Profile& p : g_Profiles)
        if (!p.bindings.empty())
            for (const auto& pr : p.profiles) out.push_back({ p.file, pr.first, pr.second, p.bindings });
    return out;
}
std::vector<std::string> ControllerBindings::Extensions()
{
    std::lock_guard lk(g_M);
    std::vector<std::string> out;
    for (const Profile& p : g_Profiles)
        for (const auto& pr : p.profiles)
            if (!pr.second.empty() && std::find(out.begin(), out.end(), pr.second) == out.end()) out.push_back(pr.second);
    return out;
}
bool ControllerBindings::Write(const std::vector<std::pair<std::string, std::string>>& values, std::string& err)
{
    std::lock_guard lk(g_M);
    if (g_Active < 0) { err = "no controller profile in use"; return false; }
    Profile& p = g_Profiles[g_Active];
    bool ok = false; std::string text = ReadFile(p.path, ok);
    if (!ok) { err = "VRControllerBindings\\" + p.file + ".json could not be read"; return false; }
    Parser ps(text); J root;
    if (!ps.Val(root) || root.t != J::kObj) { err = p.file + ".json is not valid JSON any more (" + ps.err + ")"; return false; }
    const J* st = root.Get("settings");
    if (!st || st->t != J::kObj) { err = p.file + ".json has no \"settings\""; return false; }
    // the values replaced from the end of the file back, so the earlier positions stay right
    struct Rep { size_t at, end; std::string with; };
    std::vector<Rep> reps;
    for (const auto& v : values)
        for (const auto& kv : st->o)
            if (Lower(kv.first) == Lower(v.first))
            {
                std::string with;
                if (kv.second.t == J::kStr) with = "\"" + v.second + "\"";
                else if (kv.second.t == J::kBool) with = (v.second == "1" || v.second == "true") ? "true" : "false";
                else
                {   // a number: written as a number when it is one, else as a string
                    char* e = nullptr; strtod(v.second.c_str(), &e);
                    with = (!v.second.empty() && !*e) ? v.second : "\"" + v.second + "\"";
                }
                reps.push_back({ kv.second.at, kv.second.end, with });
            }
    std::sort(reps.begin(), reps.end(), [](const Rep& a, const Rep& b) { return a.at > b.at; });
    for (const Rep& r : reps) text.replace(r.at, r.end - r.at, r.with);
    const std::wstring tmp = p.path + L".new";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) { err = p.file + ".json could not be written"; return false; }
    const bool wrote = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    if (!wrote || !MoveFileExW(tmp.c_str(), p.path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    { DeleteFileW(tmp.c_str()); err = p.file + ".json could not be replaced"; return false; }
    for (const auto& v : values)
        for (auto& kv : p.settings) if (Lower(kv.first) == Lower(v.first)) kv.second = v.second;
    return true;
}
