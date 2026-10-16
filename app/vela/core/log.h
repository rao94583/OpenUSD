// The app's log, for the Log tab: USD's diagnostics (TF_ERROR, TF_WARN,
// TF_STATUS from any thread), failed actions, and what MCP clients do.
// Thread-safe; keeps the most recent MaxEntries.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace vela {

struct LogEntry {
    enum class Level { Error, Warning, Info };
    uint64_t id = 0;        // increasing
    double time = 0.0;      // seconds since the epoch
    Level level = Level::Info;
    std::string source;     // e.g. "USD", "MCP", "Action"
    std::string message;
};

const char *LevelName(LogEntry::Level level);

class Log {
public:
    static constexpr size_t MaxEntries = 10000;
    static Log &Get();

    void Add(LogEntry::Level level, std::string source, std::string message);
    // Entries after id (0 for all), oldest first.
    std::vector<LogEntry> GetEntries(uint64_t after = 0) const;
    void Clear();
    // Called after entries are added or cleared, on the thread that did
    // it; the app hands it to the main thread.
    void SetChangedCallback(std::function<void()> callback);

    // Routes USD's diagnostics here (and still to stderr) from now on.
    void CaptureDiagnostics();

private:
    mutable std::mutex _mutex;
    std::vector<LogEntry> _entries;
    uint64_t _nextId = 1;
    std::function<void()> _changed;
};

// Entries of the shown levels whose source or message contain query,
// case-insensitively.
struct LogFilter {
    bool errors = true, warnings = true, info = true;
    std::string query;
    bool Matches(const LogEntry &entry) const;
};

} // namespace vela
