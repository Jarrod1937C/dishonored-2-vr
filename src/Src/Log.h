#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <fmt/format.h>

namespace Log
{
    inline std::atomic<bool> g_On{ true };   //!< false with [Logging.Files] WriteLog = 0 (set by SetFileEnabled)
    //! Whether log lines go anywhere. LOGI tests it first, so with WriteLog = 0 a line costs one load: its arguments are
    //! not evaluated and nothing is formatted.
    inline bool Enabled() { return g_On.load(std::memory_order_relaxed); }
    void Init(const std::wstring& dir);   //!< the folder of <dir>\DishonoredVR.log; lines are held until SetFileEnabled
    void SetFileEnabled(bool on);         //!< [Logging.Files] WriteLog: create the file (the held lines first) or write none
    void ForceFile();                     //!< a crash before the ini was read: write the file anyway
    void Write(const std::string& line);
    std::vector<std::string> Tail(size_t n); //!< last n lines (for the overlay)
    //! The kept last lines (up to 400), each handed to `out` -- for the crash report: never waits, allocates nothing.
    void CrashTail(void (*out)(const char*, size_t, void*), void* ctx);
    template <class... A> void Info(fmt::format_string<A...> f, A&&... a) { Write(fmt::format(f, std::forward<A>(a)...)); }
}
#define LOGI(...) (::Log::Enabled() ? ::Log::Info(__VA_ARGS__) : (void)0)
