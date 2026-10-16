#include "log.h"

#include "pxr/pxr.h"
#include "pxr/base/tf/diagnosticMgr.h"
#include "pxr/base/tf/error.h"
#include "pxr/base/tf/status.h"
#include "pxr/base/tf/warning.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

const char *LevelName(LogEntry::Level level) {
    switch (level) {
    case LogEntry::Level::Error: return "Error";
    case LogEntry::Level::Warning: return "Warning";
    case LogEntry::Level::Info: break;
    }
    return "Info";
}

Log &Log::Get() {
    static Log *log = new Log;
    return *log;
}

void Log::Add(LogEntry::Level level, std::string source, std::string message) {
    // Trailing newlines read as blank rows.
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    std::function<void()> changed;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        LogEntry entry;
        entry.id = _nextId++;
        entry.time = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        entry.level = level;
        entry.source = std::move(source);
        entry.message = std::move(message);
        _entries.push_back(std::move(entry));
        if (_entries.size() > MaxEntries) {
            _entries.erase(_entries.begin(), _entries.begin() + (_entries.size() - MaxEntries));
        }
        changed = _changed;
    }
    if (changed) changed();
}

std::vector<LogEntry> Log::GetEntries(uint64_t after) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto first = std::upper_bound(_entries.begin(), _entries.end(), after,
        [](uint64_t id, const LogEntry &entry) { return id < entry.id; });
    return std::vector<LogEntry>(first, _entries.end());
}

void Log::Clear() {
    std::function<void()> changed;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _entries.clear();
        changed = _changed;
    }
    if (changed) changed();
}

void Log::SetChangedCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(_mutex);
    _changed = std::move(callback);
}

namespace {

// Formats a diagnostic as Tf prints it, e.g. "in Foo at line 12 of foo.cpp
// -- message".
std::string Describe(const TfDiagnosticBase &d) {
    const TfCallContext &context = d.GetContext();
    std::string text;
    if (context) {
        text = std::string("in ") + context.GetFunction() + " at line " +
            std::to_string(context.GetLine()) + " of " + context.GetFile() + " -- ";
    }
    return text + d.GetCommentary();
}

// For the log: the message first, then where it came from, e.g. "message
// (Foo, foo.cpp:12)".
std::string Summarize(const TfDiagnosticBase &d) {
    const TfCallContext &context = d.GetContext();
    std::string text = d.GetCommentary();
    if (context) {
        std::string file = context.GetFile();
        file = file.substr(file.rfind('/') + 1);
        text += std::string("  (") + context.GetFunction() + ", " + file + ":" +
            std::to_string(context.GetLine()) + ")";
    }
    return text;
}

class Delegate : public TfDiagnosticMgr::Delegate {
public:
    explicit Delegate(Log &log) : _log(log) {}

    void IssueError(TfError const &error) override {
        Report(LogEntry::Level::Error, "Error", error);
    }
    void IssueWarning(TfWarning const &warning) override {
        Report(LogEntry::Level::Warning, "Warning", warning);
    }
    void IssueStatus(TfStatus const &status) override {
        Report(LogEntry::Level::Info, "Status", status);
    }
    void IssueFatalError(TfCallContext const &context, std::string const &message) override {
        fprintf(stderr, "Fatal error: %s\n", message.c_str());
        _UnhandledAbort();
    }

private:
    // As Tf would print it without a delegate, and into the log.
    void Report(LogEntry::Level level, const char *kind, const TfDiagnosticBase &d) {
        if (!d.GetQuiet()) fprintf(stderr, "%s: %s\n", kind, Describe(d).c_str());
        _log.Add(level, "USD", Summarize(d));
    }
    Log &_log;
};

} // namespace

void Log::CaptureDiagnostics() {
    static Delegate *delegate = [this] {
        auto *d = new Delegate(*this);
        TfDiagnosticMgr::GetInstance().AddDelegate(d);
        return d;
    }();
    (void)delegate;
}

bool LogFilter::Matches(const LogEntry &entry) const {
    switch (entry.level) {
    case LogEntry::Level::Error: if (!errors) return false; break;
    case LogEntry::Level::Warning: if (!warnings) return false; break;
    case LogEntry::Level::Info: if (!info) return false; break;
    }
    if (query.empty()) return true;
    auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    };
    const std::string needle = lower(query);
    return lower(entry.source).find(needle) != std::string::npos ||
           lower(entry.message).find(needle) != std::string::npos;
}

} // namespace vela
