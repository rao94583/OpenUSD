#include "app_settings.h"

#include <algorithm>

namespace vela {

JsObject ToJson(const AppSettings &settings) {
    return JsObject{
        {"view", JsValue(ToJson(settings.view))},
        {"loadPayloads", JsValue(settings.loadPayloads)},
        {"usePrimaryCamera", JsValue(settings.usePrimaryCamera)},
        {"threadLimit", JsValue(settings.threadLimit)},
        {"mcpServer", JsValue(settings.mcpServer)},
        {"claudePath", JsValue(settings.claudePath)},
        {"claudeAsksBeforeEdits", JsValue(settings.claudeAsksBeforeEdits)},
        {"claudeModel", JsValue(settings.claudeModel)},
        {"imageGeneration", JsValue(settings.imageGeneration)},
        {"imageAsk", JsValue(settings.imageAsk)},
        {"imageHighQuality", JsValue(settings.imageHighQuality)},
        {"imageConnection", JsValue(settings.imageConnection)},
        {"imageRequestLimit", JsValue(settings.imageRequestLimit)},
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
    flag("mcpServer", &settings->mcpServer);
    flag("claudeAsksBeforeEdits", &settings->claudeAsksBeforeEdits);
    flag("imageGeneration", &settings->imageGeneration);
    flag("imageAsk", &settings->imageAsk);
    flag("imageHighQuality", &settings->imageHighQuality);
    if (const JsValue *v = get("imageRequestLimit"); v && (v->IsInt() || v->IsUInt64())) {
        const int64_t limit = v->IsUInt64() ? int64_t(v->GetUInt64()) : v->GetInt64();
        if (limit >= 1 && limit <= 100) settings->imageRequestLimit = int(limit);
    }
    auto text = [&](const char *name, std::string *out) {
        if (const JsValue *v = get(name); v && v->IsString()) *out = v->GetString();
    };
    text("claudePath", &settings->claudePath);
    text("claudeModel", &settings->claudeModel);
    text("imageConnection", &settings->imageConnection);
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
