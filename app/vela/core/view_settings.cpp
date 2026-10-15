#include "view_settings.h"

#include "pxr/base/js/json.h"

#include <algorithm>

namespace vela {

namespace {

template <class Enum>
std::string Name(const std::vector<std::string> &names, Enum value) {
    return names[static_cast<size_t>(value)];
}

template <class Enum>
bool Find(const std::vector<std::string> &names, const std::string &name, Enum *value) {
    auto found = std::find(names.begin(), names.end(), name);
    if (found == names.end()) return false;
    *value = static_cast<Enum>(found - names.begin());
    return true;
}

} // namespace

bool IsShaded(DrawMode mode) {
    switch (mode) {
    case DrawMode::WireframeOnSurface:
    case DrawMode::ShadedFlat:
    case DrawMode::ShadedSmooth:
    case DrawMode::GeomFlat:
    case DrawMode::GeomSmooth:
        return true;
    default:
        return false;
    }
}

GfVec4f BackgroundColor(Background background) {
    switch (background) {
    case Background::Black: return GfVec4f(0.0f, 0.0f, 0.0f, 1.0f);
    case Background::DarkGrey: return GfVec4f(0.07074f, 0.07074f, 0.07074f, 1.0f);
    case Background::LightGrey: return GfVec4f(0.45626f, 0.45626f, 0.45626f, 1.0f);
    case Background::White: return GfVec4f(1.0f);
    }
    return GfVec4f(0.0f, 0.0f, 0.0f, 1.0f);
}

GfVec4f BBoxLineColor(Background background) {
    const GfVec4f clear = BackgroundColor(background);
    GfVec4f color(0.0f, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < 3; ++i) {
        color[i] = std::clamp(clear[i] > 0.5f ? clear[i] - 0.6f : clear[i] + 0.6f, 0.0f, 1.0f);
    }
    return color;
}

// In enum order.
const std::vector<std::string> &DrawModeNames() {
    static const std::vector<std::string> names{
        "points", "wireframe", "wireframeOnSurface", "shadedFlat",
        "shadedSmooth", "geomOnly", "geomFlat", "geomSmooth"};
    return names;
}

const std::vector<std::string> &BackgroundNames() {
    static const std::vector<std::string> names{
        "black", "darkGrey", "lightGrey", "white"};
    return names;
}

const std::vector<std::string> &HighlightModeNames() {
    static const std::vector<std::string> names{"never", "whenPaused", "always"};
    return names;
}

const std::vector<std::string> &ColorCorrectionNames() {
    static const std::vector<std::string> names{"disabled", "sRGB", "openColorIO"};
    return names;
}
std::string ToString(ColorCorrection mode) { return Name(ColorCorrectionNames(), mode); }
bool Parse(const std::string &name, ColorCorrection *mode) {
    return Find(ColorCorrectionNames(), name, mode);
}

const std::vector<std::string> &ComplexityNames() {
    static const std::vector<std::string> names{"low", "medium", "high", "veryhigh"};
    return names;
}

std::string ToString(DrawMode mode) { return Name(DrawModeNames(), mode); }
std::string ToString(Background background) { return Name(BackgroundNames(), background); }
std::string ToString(HighlightMode mode) { return Name(HighlightModeNames(), mode); }
bool Parse(const std::string &name, DrawMode *mode) { return Find(DrawModeNames(), name, mode); }
bool Parse(const std::string &name, Background *background) {
    return Find(BackgroundNames(), name, background);
}
bool Parse(const std::string &name, HighlightMode *mode) {
    return Find(HighlightModeNames(), name, mode);
}

namespace {

struct BoolField {
    const char *name;
    bool ViewSettings::*field;
};
const std::vector<BoolField> &BoolFields() {
    static const std::vector<BoolField> fields{
        {"showProxy", &ViewSettings::showProxy},
        {"showRender", &ViewSettings::showRender},
        {"showGuides", &ViewSettings::showGuides},
        {"cameraLight", &ViewSettings::cameraLight},
        {"domeLight", &ViewSettings::domeLight},
        {"domeLightTexturesVisible", &ViewSettings::domeLightTexturesVisible},
        {"sceneLights", &ViewSettings::sceneLights},
        {"sceneMaterials", &ViewSettings::sceneMaterials},
        {"cullBackfaces", &ViewSettings::cullBackfaces},
        {"showBBoxes", &ViewSettings::showBBoxes},
        {"showAlignedBBox", &ViewSettings::showAlignedBBox},
        {"showOrientedBBox", &ViewSettings::showOrientedBBox},
        {"showBBoxesDuringPlayback", &ViewSettings::showBBoxesDuringPlayback},
        {"showHUD", &ViewSettings::showHUD},
        {"hudInfo", &ViewSettings::hudInfo},
        {"hudComplexity", &ViewSettings::hudComplexity},
        {"hudPerformance", &ViewSettings::hudPerformance},
        {"hudGPUStats", &ViewSettings::hudGPUStats},
    };
    return fields;
}

bool IsNumber(const JsValue &value) {
    return value.IsReal() || value.IsInt() || value.IsUInt64();
}
double Number(const JsValue &value) {
    return value.IsReal() ? value.GetReal()
         : value.IsUInt64() ? double(value.GetUInt64()) : double(value.GetInt64());
}

} // namespace

JsObject ToJson(const ViewSettings &s) {
    JsObject json;
    for (const BoolField &f : BoolFields()) json[f.name] = JsValue(s.*(f.field));
    json["drawMode"] = JsValue(ToString(s.drawMode));
    json["complexity"] = JsValue(s.complexity);
    json["background"] = JsValue(ToString(s.background));
    json["highlightMode"] = JsValue(ToString(s.highlightMode));
    json["colorCorrection"] = JsValue(ToString(s.colorCorrection));
    json["ocioDisplay"] = JsValue(s.ocioDisplay);
    json["ocioView"] = JsValue(s.ocioView);
    json["ocioColorSpace"] = JsValue(s.ocioColorSpace);
    json["ocioLook"] = JsValue(s.ocioLook);
    json["renderer"] = JsValue(s.renderer);
    json["aov"] = JsValue(s.aov);
    JsObject rendererSettings;
    for (const auto &[key, value] : s.rendererSettings) rendererSettings[key] = value;
    json["rendererSettings"] = JsValue(rendererSettings);
    json["renderSettingsPrim"] = JsValue(s.renderSettingsPrim.GetString());
    json["renderPassPrim"] = JsValue(s.renderPassPrim.GetString());
    json["defaultMaterialAmbient"] = JsValue(s.defaultMaterialAmbient);
    json["defaultMaterialSpecular"] = JsValue(s.defaultMaterialSpecular);
    return json;
}

void FromJson(const JsObject &json, ViewSettings *s) {
    auto get = [&](const char *name) -> const JsValue * {
        auto found = json.find(name);
        return found == json.end() ? nullptr : &found->second;
    };
    for (const BoolField &f : BoolFields()) {
        if (const JsValue *v = get(f.name); v && v->IsBool()) s->*(f.field) = v->GetBool();
    }
    auto string = [&](const char *name, std::string *out) {
        if (const JsValue *v = get(name); v && v->IsString()) *out = v->GetString();
    };
    auto number = [&](const char *name, double *out) {
        if (const JsValue *v = get(name); v && IsNumber(*v)) *out = Number(*v);
    };
    auto path = [&](const char *name, SdfPath *out) {
        const JsValue *v = get(name);
        if (!v || !v->IsString()) return;
        const std::string &text = v->GetString();
        if (text.empty()) *out = SdfPath();
        else if (SdfPath::IsValidPathString(text)) *out = SdfPath(text);
    };
    auto named = [&](const char *name, auto *out) {
        std::string text;
        string(name, &text);
        if (!text.empty()) Parse(text, out);
    };
    named("drawMode", &s->drawMode);
    named("background", &s->background);
    named("highlightMode", &s->highlightMode);
    named("colorCorrection", &s->colorCorrection);
    number("complexity", &s->complexity);
    string("ocioDisplay", &s->ocioDisplay);
    string("ocioView", &s->ocioView);
    string("ocioColorSpace", &s->ocioColorSpace);
    string("ocioLook", &s->ocioLook);
    string("renderer", &s->renderer);
    string("aov", &s->aov);
    if (const JsValue *v = get("rendererSettings"); v && v->IsObject()) {
        s->rendererSettings.clear();
        // As the actions store them: integers as int64 (JSON gives
        // non-negative ones back as uint64).
        for (const auto &[key, value] : v->GetJsObject()) {
            s->rendererSettings[key] = value.IsUInt64()
                ? JsValue(int64_t(value.GetUInt64())) : value;
        }
    }
    path("renderSettingsPrim", &s->renderSettingsPrim);
    path("renderPassPrim", &s->renderPassPrim);
    number("defaultMaterialAmbient", &s->defaultMaterialAmbient);
    number("defaultMaterialSpecular", &s->defaultMaterialSpecular);
}

ViewSettings WithoutSceneSpecific(const ViewSettings &settings) {
    ViewSettings result = settings;
    const ViewSettings defaults;
    result.aov = defaults.aov;
    result.rendererSettings.clear();
    result.renderSettingsPrim = SdfPath();
    result.renderPassPrim = SdfPath();
    return result;
}

std::vector<std::string> DescribeChanges(const ViewSettings &settings) {
    const JsObject current = ToJson(settings), defaults = ToJson(ViewSettings());
    std::vector<std::string> changes;
    for (const auto &[name, value] : current) {
        if (value == defaults.at(name)) continue;
        std::string text = value.IsString() ? value.GetString() : JsWriteToString(value);
        changes.push_back(name + ": " + (text.empty() ? "(none)" : text));
    }
    return changes;
}

bool ParseComplexity(const std::string &name, double *complexity) {
    const auto &names = ComplexityNames();
    auto found = std::find(names.begin(), names.end(), name);
    if (found == names.end()) return false;
    *complexity = 1.0 + 0.1 * double(found - names.begin());
    return true;
}

} // namespace vela
