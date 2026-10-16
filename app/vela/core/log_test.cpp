#include "log.h"
#include "test_support.h"

#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/errorMark.h"

#include <thread>

using namespace vela;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {

void TestLog() {
    Log &log = Log::Get();
    int changes = 0;
    log.SetChangedCallback([&] { ++changes; });
    log.Add(LogEntry::Level::Info, "MCP", "run_python\n");
    log.Add(LogEntry::Level::Error, "Action", "No prim at /Nope");
    auto entries = log.GetEntries();
    CHECK(entries.size() == 2 && changes == 2);
    CHECK(entries[0].message == "run_python" && entries[0].id < entries[1].id);
    CHECK(log.GetEntries(entries[0].id).size() == 1);

    // USD's diagnostics, from any thread; errors caught by a mark aren't
    // reported.
    log.CaptureDiagnostics();
    log.CaptureDiagnostics();  // once only
    std::thread([] { TF_WARN("a warning from a thread"); }).join();
    TF_STATUS("a status");
    TF_RUNTIME_ERROR("an error");
    {
        PXR_NS::TfErrorMark mark;
        TF_RUNTIME_ERROR("a handled error");
        mark.Clear();
    }
    entries = log.GetEntries();
    CHECK(entries.size() == 5);
    CHECK(entries[2].level == LogEntry::Level::Warning && entries[2].source == "USD" &&
          entries[2].message.find("a warning from a thread") != std::string::npos);
    CHECK(entries[3].level == LogEntry::Level::Info);
    CHECK(entries[4].level == LogEntry::Level::Error);

    LogFilter filter;
    filter.query = "PRIM";
    CHECK(filter.Matches(entries[1]) && !filter.Matches(entries[0]));
    filter.query = "mcp";
    CHECK(filter.Matches(entries[0]));
    filter.info = false;
    CHECK(!filter.Matches(entries[0]));

    for (size_t i = 0; i < Log::MaxEntries + 5; ++i) log.Add(LogEntry::Level::Info, "Test", "x");
    CHECK(log.GetEntries().size() == Log::MaxEntries);
    log.Clear();
    CHECK(log.GetEntries().empty());
    log.SetChangedCallback(nullptr);
}

} // namespace

int main() {
    TestLog();
    return test::Finish("log");
}
