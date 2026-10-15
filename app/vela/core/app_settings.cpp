#include "app_settings.h"

#include <algorithm>

namespace vela {

JsObject ToJson(const AppSettings &settings) {
    return JsObject{
        {"view", JsValue(ToJson(settings.view))},
        {"loadPayloads", JsValue(settings.loadPayloads)},
        {"usePrimaryCamera", JsValue(settings.usePrimaryCamera)},
        {"unloadedAsBounds", JsValue(settings.unloadedAsBounds)},
        {"threadLimit", JsValue(settings.threadLimit)},
    };
}

void FromJson(const JsObject &json, AppSettings *settings) {
    auto get = [&](const char *name) -> const JsValue * {
        auto found = json.find(name);
        return found == json.end() ? nullptr : &found->second;
    };
    if (const JsValue *v = get("view"); v && v->IsObject()) {
        FromJson(v->GetJsObject(), &settings->view);
    }
    auto flag = [&](const char *name, bool *out) {
        if (const JsValue *v = get(name); v && v->IsBool()) *out = v->GetBool();
    };
    flag("loadPayloads", &settings->loadPayloads);
    flag("usePrimaryCamera", &settings->usePrimaryCamera);
    flag("unloadedAsBounds", &settings->unloadedAsBounds);
    if (const JsValue *v = get("threadLimit"); v && (v->IsInt() || v->IsUInt64())) {
        settings->threadLimit = int(v->IsUInt64() ? int64_t(v->GetUInt64()) : v->GetInt64());
    }
}

AppSettingsStore &AppSettingsStore::Get() {
    static AppSettingsStore *store = new AppSettingsStore;
    return *store;
}

void AppSettingsStore::SetSettings(const AppSettings &settings) {
    if (settings == _settings) return;
    _settings = settings;
    // Observers may remove themselves.
    const auto observers = _observers;
    for (const auto &[id, observer] : observers) observer();
}

int AppSettingsStore::AddObserver(std::function<void()> observer) {
    _observers.emplace_back(_nextObserver, std::move(observer));
    return _nextObserver++;
}

void AppSettingsStore::RemoveObserver(int id) {
    _observers.erase(std::remove_if(_observers.begin(), _observers.end(),
                                    [&](const auto &entry) { return entry.first == id; }),
                     _observers.end());
}

} // namespace vela
