// The app's settings (the Settings window): defaults for newly opened
// documents and the options that replace usdview's launch flags. Plain
// C++; the app persists them (NSUserDefaults) by observing the store.
#pragma once

#include "view_settings.h"

#include "pxr/pxr.h"
#include "pxr/base/js/value.h"

#include <functional>
#include <utility>
#include <vector>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

struct AppSettings {
    // View settings newly opened documents start with (without the
    // scene-specific ones; see WithoutSceneSpecific).
    ViewSettings view;
    // Opening files: load payloads (usdview's --unloaded turns this off),
    // look through the stage's primary camera (main_cam) when it has one,
    // and draw unloaded prims as bounding boxes (--bboxStandin).
    bool loadPayloads = true;
    bool usePrimaryCamera = true;
    bool unloadedAsBounds = false;
    // Worker threads: 0 for all, negative for all but that many
    // (--numThreads). Applies at the next launch.
    int threadLimit = 0;

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
