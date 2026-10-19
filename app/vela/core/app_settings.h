// The app's settings (the Settings window): defaults for newly opened
// documents and the options that replace usdview's launch flags. Plain
// C++; the app persists them (NSUserDefaults) by observing the store.
#pragma once

#include "view_settings.h"

#include "pxr/pxr.h"
#include "pxr/base/js/value.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

struct AppSettings {
    // View settings newly opened documents start with (see AsDefaults),
    // including drawing unloaded prims as bounding boxes (--bboxStandin).
    ViewSettings view;
    // Opening files: load payloads (usdview's --unloaded turns this off),
    // and look through the stage's primary camera (main_cam) when it has
    // one.
    bool loadPayloads = true;
    bool usePrimaryCamera = true;
    // Worker threads: 0 for all, negative for all but that many
    // (--numThreads). Applies at the next launch.
    int threadLimit = 0;
    // Accept MCP clients (Claude Code through vela-mcp) on the app's
    // socket.
    bool mcpServer = true;
    // The Claude panel: the claude CLI to run ("" finds it), whether
    // Claude's edits need approval first, the model ("" for the CLI's
    // default, or an alias such as "opus") and the effort ("" for the
    // CLI's default, or low, medium, high, xhigh or max).
    std::string claudePath;
    bool claudeAsksBeforeEdits = false;
    std::string claudeModel;
    std::string claudeEffort;

    // Provider secrets live in Keychain, never in these persisted settings.
    bool imageGeneration = false;
    bool imageAsk = true;
    bool imageHighQuality = false;
    std::string imageConnection = "openai";
    int imageRequestLimit = 20;

    bool operator==(const AppSettings &other) const = default;
};

// {"view": {...}, "loadPayloads": true, ...}. Reading is lenient, as for
// view settings: unknown keys and wrong types keep the current values.
JsObject ToJson(const AppSettings &settings);
void FromJson(const JsObject &json, AppSettings *settings);

// The process's settings. Observers are called after each change.
class AppSettingsStore {
public:
    static AppSettingsStore &Get();

    const AppSettings &GetSettings() const { return _settings; }
    void SetSettings(const AppSettings &settings);
    // Restores every default.
    void Reset() { SetSettings(AppSettings()); }

    int AddObserver(std::function<void()> observer);
    void RemoveObserver(int id);

private:
    AppSettings _settings;
    std::vector<std::pair<int, std::function<void()>>> _observers;
    int _nextObserver = 1;
};

} // namespace vela
