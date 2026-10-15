// View settings: how the viewport draws, not what the stage contains. None
// of these author scene data or create undo steps.
#include "actions.h"
#include "app_settings.h"
#include "color_management.h"
#include "document.h"

#include "pxr/base/js/json.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace vela {

namespace {

ActionResult Done() { return ActionResult(); }

// An enum parameter's schema, from a list of names.
std::string EnumSchema(const std::string &name, const std::vector<std::string> &values) {
    JsArray choices;
    for (const std::string &value : values) choices.push_back(JsValue(value));
    return JsWriteToString(JsValue(JsObject{
        {"properties", JsValue(JsObject{{name, JsValue(JsObject{
            {"type", JsValue("string")}, {"enum", JsValue(choices)}})}})},
        {"required", JsValue(JsArray{JsValue(name)})}}));
}

std::vector<MenuChoice> Choices(const std::string &param,
                                const std::vector<std::string> &values,
                                const std::vector<std::string> &titles) {
    std::vector<MenuChoice> choices;
    for (size_t i = 0; i < values.size(); ++i) {
        choices.push_back({titles[i], JsObject{{param, JsValue(values[i])}}});
    }
    return choices;
}

// Applies an edit to a copy of the document's view settings.
ActionResult Change(Document &d, const std::function<void(ViewSettings &)> &edit) {
    ViewSettings settings = d.GetViewSettings();
    edit(settings);
    d.SetViewSettings(settings);
    return Done();
}

// Boolean view options, toggled from the menu or set explicitly.
struct Option {
    const char *name;
    const char *title;
    bool ViewSettings::*field;
};
const std::vector<Option> &Options() {
    static const std::vector<Option> options{
        {"cameraLight", "Camera Light", &ViewSettings::cameraLight},
        {"domeLight", "Default Dome Light", &ViewSettings::domeLight},
        {"domeLightTextures", "Dome Light Textures Visible",
         &ViewSettings::domeLightTexturesVisible},
        {"sceneLights", "Scene Lights", &ViewSettings::sceneLights},
        {"sceneMaterials", "Scene Materials", &ViewSettings::sceneMaterials},
        {"cullBackfaces", "Cull Backfaces", &ViewSettings::cullBackfaces},
        {"proxy", "Proxy Purpose", &ViewSettings::showProxy},
        {"render", "Render Purpose", &ViewSettings::showRender},
        {"guide", "Guide Purpose", &ViewSettings::showGuides},
        {"bboxes", "Bounding Boxes", &ViewSettings::showBBoxes},
        {"alignedBBox", "Axis-Aligned Bounding Box", &ViewSettings::showAlignedBBox},
        {"orientedBBox", "Oriented Bounding Box", &ViewSettings::showOrientedBBox},
        {"bboxesDuringPlayback", "Bounding Boxes During Playback",
         &ViewSettings::showBBoxesDuringPlayback},
        {"hud", "Heads-Up Display", &ViewSettings::showHUD},
        {"hudInfo", "HUD Prim Info", &ViewSettings::hudInfo},
        {"hudComplexity", "HUD Complexity and Camera", &ViewSettings::hudComplexity},
        {"hudPerformance", "HUD Performance", &ViewSettings::hudPerformance},
        {"hudGPUStats", "HUD GPU Statistics", &ViewSettings::hudGPUStats},
        {"unloadedAsBounds", "Unloaded Prims as Bounding Boxes",
         &ViewSettings::unloadedAsBounds},
        {"pauseDrawing", "Pause Viewport Drawing", &ViewSettings::pauseDrawing},
    };
    return options;
}
const Option *FindOption(const std::string &name) {
    for (const Option &option : Options()) {
        if (name == option.name) return &option;
    }
    return nullptr;
}

const char *TypeName(RendererSetting::Type type) {
    switch (type) {
    case RendererSetting::Type::Flag: return "flag";
    case RendererSetting::Type::Int: return "int";
    case RendererSetting::Type::Float: return "float";
    case RendererSetting::Type::String: return "string";
    }
    return "";
}

const RendererSetting *FindSetting(const Document &d, const std::string &key) {
    for (const RendererSetting &setting : d.GetRendererInfo().settings) {
        if (setting.key == key) return &setting;
    }
    return nullptr;
}

// Whether value suits the setting; floats accept integers.
bool Suits(const RendererSetting &setting, const JsValue &value) {
    switch (setting.type) {
    case RendererSetting::Type::Flag: return value.IsBool();
    case RendererSetting::Type::Int: return value.IsInt();
    case RendererSetting::Type::Float: return value.IsReal() || value.IsInt();
    case RendererSetting::Type::String: return value.IsString();
    }
    return false;
}

// One representation per type, so values compare equal to defaults: JSON
// parsing gives non-negative integers as uint64, others as int64.
JsValue Canonical(const RendererSetting &setting, const JsValue &value) {
    switch (setting.type) {
    case RendererSetting::Type::Int:
        return JsValue(value.IsUInt64() ? int64_t(value.GetUInt64()) : value.GetInt64());
    case RendererSetting::Type::Float:
        return JsValue(value.IsReal() ? value.GetReal()
                       : value.IsUInt64() ? double(value.GetUInt64())
                       : double(value.GetInt64()));
    default:
        return value;
    }
}

// Whether value is the setting's default. Float settings are floats in
// Hydra, so they compare at float precision (a form shows 2.2f as
// 2.200000048).
bool IsDefault(const RendererSetting &setting, const JsValue &value) {
    const JsValue fallback = Canonical(setting, setting.defaultValue);
    if (setting.type == RendererSetting::Type::Float && value.IsReal() && fallback.IsReal()) {
        return float(value.GetReal()) == float(fallback.GetReal());
    }
    return value == fallback;
}

// A path parameter naming a prim of the given kind, or empty.
ActionResult CheckPrim(const Document &d, const std::string &path,
                       const SdfPathVector &prims, const char *kind) {
    if (path.empty()) return Done();
    if (!SdfPath::IsValidPathString(path)) {
        return ActionResult::Error("Not a valid path: " + path);
    }
    if (std::find(prims.begin(), prims.end(), SdfPath(path)) == prims.end()) {
        return ActionResult::Error(path + " is not a " + kind + " prim");
    }
    return Done();
}

} // namespace

void RegisterViewActions(ActionRegistry &r) {
    r.Register({
        .name = "set_draw_mode", .title = "Draw Mode",
        .description = "How the viewport draws geometry, as in usdview: points, "
                       "wireframe, wireframeOnSurface, shadedFlat, shadedSmooth "
                       "(the default), geomOnly, geomFlat or geomSmooth. Lighting "
                       "applies only to shaded modes.",
        .parameters = ParameterSchema(EnumSchema("mode", DrawModeNames())),
        .menu = "View", .separatorBefore = true,
        .choices = Choices("mode", DrawModeNames(),
            {"Points", "Wireframe", "Wireframe on Surface", "Flat Shaded",
             "Smooth Shaded", "Geom Only", "Geom Flat", "Geom Smooth"}),
        .chosen = [](const Document &d, const JsObject &p) {
            return ToString(d.GetViewSettings().drawMode) == p.at("mode").GetString();
        },
        .run = [](Document &d, const JsObject &p) {
            DrawMode mode;
            Parse(p.at("mode").GetString(), &mode);
            return Change(d, [&](ViewSettings &s) { s.drawMode = mode; });
        },
    });
    r.Register({
        .name = "set_complexity", .title = "Complexity",
        .description = "Mesh refinement level, as usdview's: low (the default), "
                       "medium, high or veryhigh.",
        .parameters = ParameterSchema(EnumSchema("level", ComplexityNames())),
        .menu = "View",
        .choices = Choices("level", ComplexityNames(),
                           {"Low", "Medium", "High", "Very High"}),
        .chosen = [](const Document &d, const JsObject &p) {
            double complexity = 0;
            ParseComplexity(p.at("level").GetString(), &complexity);
            return std::abs(d.GetViewSettings().complexity - complexity) < 1e-6;
        },
        .run = [](Document &d, const JsObject &p) {
            double complexity = 1.0;
            ParseComplexity(p.at("level").GetString(), &complexity);
            return Change(d, [&](ViewSettings &s) { s.complexity = complexity; });
        },
    });
    {
        std::vector<std::string> names, titles;
        for (const Option &option : Options()) {
            names.push_back(option.name);
            titles.push_back(option.title);
        }
        JsArray values;
        for (const std::string &name : names) values.push_back(JsValue(name));
        r.Register({
            .name = "set_view_option", .title = "Display",
            .description = "Turn a view option on or off, or toggle it when "
                           "enabled is omitted. Options: cameraLight, domeLight "
                           "(usdview's default dome light), domeLightTextures "
                           "(dome textures in the background), sceneLights, "
                           "sceneMaterials, cullBackfaces, the purposes drawn "
                           "besides default (proxy, render, guide; bounds follow "
                           "them too), and bounding boxes of the selection or "
                           "stage: bboxes, alignedBBox, orientedBBox, "
                           "bboxesDuringPlayback; the heads-up display: hud, "
                           "hudInfo (prim and geometry counts), hudComplexity, "
                           "hudPerformance, hudGPUStats; unloadedAsBounds "
                           "(unloaded prims as bounding boxes) and pauseDrawing "
                           "(the viewport stops drawing).",
            .parameters = ParameterSchema(JsWriteToString(JsValue(JsObject{
                {"properties", JsValue(JsObject{
                    {"option", JsValue(JsObject{{"type", JsValue("string")},
                                                {"enum", JsValue(values)}})},
                    {"enabled", JsValue(JsObject{{"type", JsValue("boolean")}})}})},
                {"required", JsValue(JsArray{JsValue("option")})}}))),
            .menu = "View",
            .choices = Choices("option", names, titles),
            .chosen = [](const Document &d, const JsObject &p) {
                const Option *option = FindOption(p.at("option").GetString());
                return option && d.GetViewSettings().*(option->field);
            },
            .run = [](Document &d, const JsObject &p) {
                const Option *option = FindOption(p.at("option").GetString());
                auto enabled = p.find("enabled");
                return Change(d, [&](ViewSettings &s) {
                    bool &value = s.*(option->field);
                    value = enabled == p.end() ? !value : enabled->second.GetBool();
                });
            },
        });
    }
    r.Register({
        .name = "set_view_camera", .title = "Camera",
        .description = "Look through a scene camera (a UsdGeomCamera prim path), "
                       "fitted to the viewport, or pass an empty path for the free "
                       "camera. Navigating or framing detaches to a free copy of "
                       "the scene camera; the scene camera itself never moves.",
        .parameters = ParameterSchema(R"({
            "properties": {"path": {"type": "string"}},
            "required": ["path"]
        })"),
        .menu = "View", .separatorBefore = true,
        .dynamicChoices = [](const Document &d) {
            std::vector<MenuChoice> choices{{"Free Camera", JsObject{{"path", JsValue("")}}}};
            for (const SdfPath &camera : d.GetSceneCameras()) {
                choices.push_back({camera.GetString(),
                                   JsObject{{"path", JsValue(camera.GetString())}}});
            }
            return choices;
        },
        .chosen = [](const Document &d, const JsObject &p) {
            return d.GetViewCamera().GetString() == p.at("path").GetString();
        },
        .run = [](Document &d, const JsObject &p) {
            const std::string &path = p.at("path").GetString();
            if (!path.empty() && !SdfPath::IsValidPathString(path)) {
                return ActionResult::Error("Not a valid path: " + path);
            }
            std::string error;
            return d.SetViewCamera(path.empty() ? SdfPath() : SdfPath(path), &error)
                ? Done() : ActionResult::Error(error);
        },
    });
    r.Register({
        .name = "set_color_correction", .title = "Color Correction",
        .description = "How rendered colors are converted for display: disabled, "
                       "sRGB (the default) or openColorIO, which uses the OCIO "
                       "config in $OCIO (or OCIO's default config) with the "
                       "display, view, color space and look from set_ocio.",
        .parameters = ParameterSchema(EnumSchema("mode", ColorCorrectionNames())),
        .menu = "View", .separatorBefore = true,
        .choices = Choices("mode", ColorCorrectionNames(),
                           {"Disabled", "sRGB", "OpenColorIO"}),
        .chosen = [](const Document &d, const JsObject &p) {
            return ToString(d.GetViewSettings().colorCorrection) == p.at("mode").GetString();
        },
        .run = [](Document &d, const JsObject &p) {
            ColorCorrection mode;
            Parse(p.at("mode").GetString(), &mode);
            return Change(d, [&](ViewSettings &s) { s.colorCorrection = mode; });
        },
    });
    r.Register({
        .name = "set_ocio", .title = "OCIO Display/View",
        .description = "OpenColorIO settings, which also turn on OpenColorIO color "
                       "correction. Each is optional; omitted ones are unchanged "
                       "and empty ones use the config's default.",
        .parameters = ParameterSchema(R"({"properties": {
            "display": {"type": "string"}, "view": {"type": "string"},
            "colorSpace": {"type": "string"}, "look": {"type": "string"}}})"),
        .menu = "View",
        .dynamicChoices = [](const Document &) {
            std::vector<MenuChoice> choices;
            for (const auto &[display, view] : OcioDisplayViews()) {
                choices.push_back({display + " / " + view, JsObject{
                    {"display", JsValue(display)}, {"view", JsValue(view)}}});
            }
            return choices;
        },
        .chosen = [](const Document &d, const JsObject &p) {
            const ViewSettings &s = d.GetViewSettings();
            return s.colorCorrection == ColorCorrection::OpenColorIO &&
                p.count("display") && s.ocioDisplay == p.at("display").GetString() &&
                p.count("view") && s.ocioView == p.at("view").GetString();
        },
        .run = [](Document &d, const JsObject &p) {
            return Change(d, [&](ViewSettings &s) {
                s.colorCorrection = ColorCorrection::OpenColorIO;
                auto set = [&](const char *key, std::string *field) {
                    auto found = p.find(key);
                    if (found != p.end()) *field = found->second.GetString();
                };
                set("display", &s.ocioDisplay);
                set("view", &s.ocioView);
                set("colorSpace", &s.ocioColorSpace);
                set("look", &s.ocioLook);
            });
        },
    });
    r.Register({
        .name = "set_background", .title = "Background",
        .description = "The viewport's background: black, darkGrey (the default), "
                       "lightGrey or white, as in usdview.",
        .parameters = ParameterSchema(EnumSchema("color", BackgroundNames())),
        .menu = "View",
        .choices = Choices("color", BackgroundNames(),
                           {"Black", "Dark Grey", "Light Grey", "White"}),
        .chosen = [](const Document &d, const JsObject &p) {
            return ToString(d.GetViewSettings().background) == p.at("color").GetString();
        },
        .run = [](Document &d, const JsObject &p) {
            Background background;
            Parse(p.at("color").GetString(), &background);
            return Change(d, [&](ViewSettings &s) { s.background = background; });
        },
    });
    r.Register({
        .name = "set_default_material", .title = "Default Material…",
        .description = "The material for geometry without one: ambient and specular "
                       "gray levels from 0 to 1 (usdview's defaults: 0.2 and 0.1). "
                       "Either may be omitted.",
        .parameters = ParameterSchema(R"({"properties": {
            "ambient": {"type": "number"}, "specular": {"type": "number"}}})"),
        .menu = "View",
        .form = [](const Document &d) {
            const ViewSettings &s = d.GetViewSettings();
            return std::vector<FormField>{
                {.key = "ambient", .label = "Ambient", .type = FormField::Type::Float,
                 .value = JsValue(s.defaultMaterialAmbient)},
                {.key = "specular", .label = "Specular", .type = FormField::Type::Float,
                 .value = JsValue(s.defaultMaterialSpecular)},
            };
        },
        .run = [](Document &d, const JsObject &p) {
            ViewSettings s = d.GetViewSettings();
            for (const auto &[key, field] : {std::pair{"ambient", &ViewSettings::defaultMaterialAmbient},
                                             std::pair{"specular", &ViewSettings::defaultMaterialSpecular}}) {
                auto found = p.find(key);
                if (found == p.end()) continue;
                const JsValue &v = found->second;
                const double value = v.IsReal() ? v.GetReal()
                    : v.IsUInt64() ? double(v.GetUInt64()) : v.IsInt() ? double(v.GetInt64()) : -1.0;
                if (value < 0.0 || value > 1.0) {
                    return ActionResult::Error(std::string(key) + " must be a number from 0 to 1");
                }
                s.*field = value;
            }
            d.SetViewSettings(s);
            return Done();
        },
    });
    r.Register({
        .name = "set_selection_highlighting", .title = "Selection Highlighting",
        .description = "When the viewport highlights selected prims: never, "
                       "whenPaused (the default; not during playback) or always.",
        .parameters = ParameterSchema(EnumSchema("mode", HighlightModeNames())),
        .menu = "View",
        .choices = Choices("mode", HighlightModeNames(),
                           {"Never", "Only When Paused", "Always"}),
        .chosen = [](const Document &d, const JsObject &p) {
            return ToString(d.GetViewSettings().highlightMode) == p.at("mode").GetString();
        },
        .run = [](Document &d, const JsObject &p) {
            HighlightMode mode;
            Parse(p.at("mode").GetString(), &mode);
            return Change(d, [&](ViewSettings &s) { s.highlightMode = mode; });
        },
    });
    r.Register({
        .name = "set_renderer", .title = "Renderer",
        .description = "The Hydra renderer, by plugin id or display name as in "
                       "usdview's --renderer (e.g. GL for Storm, the default, or "
                       "Embree). Switching drops renderer settings and shows "
                       "the color AOV. get_renderer_info lists the renderers.",
        .parameters = ParameterSchema(R"({
            "properties": {"renderer": {"type": "string"}},
            "required": ["renderer"]
        })"),
        .menu = "Renderer",
        .dynamicChoices = [](const Document &d) {
            std::vector<MenuChoice> choices;
            for (const RendererInfo::Plugin &plugin : d.GetRendererInfo().plugins) {
                choices.push_back({plugin.name, JsObject{{"renderer", JsValue(plugin.id)}}});
            }
            return choices;
        },
        .chosen = [](const Document &d, const JsObject &p) {
            return d.GetRendererInfo().current == p.at("renderer").GetString();
        },
        .run = [](Document &d, const JsObject &p) {
            const std::string &name = p.at("renderer").GetString();
            const auto &plugins = d.GetRendererInfo().plugins;
            if (!plugins.empty() && std::none_of(plugins.begin(), plugins.end(),
                    [&](const RendererInfo::Plugin &plugin) {
                        return plugin.id == name || plugin.name == name;
                    })) {
                return ActionResult::Error("Unknown renderer: " + name);
            }
            return Change(d, [&](ViewSettings &s) {
                if (s.renderer == name) return;
                s.renderer = name;
                s.aov = "color";
                s.rendererSettings.clear();
            });
        },
    });
    r.Register({
        .name = "set_aov", .title = "AOV",
        .description = "The AOV (render output) the viewport shows: color (the "
                       "default), or another the renderer offers, such as depth "
                       "or primId, or a primvar as \"primvars:NAME\".",
        .parameters = ParameterSchema(R"({
            "properties": {"aov": {"type": "string"}},
            "required": ["aov"]
        })"),
        .menu = "Renderer",
        .dynamicChoices = [](const Document &d) {
            std::vector<MenuChoice> choices;
            for (const std::string &aov : d.GetRendererInfo().aovs) {
                choices.push_back({aov, JsObject{{"aov", JsValue(aov)}}});
            }
            return choices;
        },
        .chosen = [](const Document &d, const JsObject &p) {
            return d.GetViewSettings().aov == p.at("aov").GetString();
        },
        .run = [](Document &d, const JsObject &p) {
            const std::string &aov = p.at("aov").GetString();
            if (aov.empty()) return ActionResult::Error("No AOV given");
            return Change(d, [&](ViewSettings &s) { s.aov = aov; });
        },
    });
    r.Register({
        .name = "set_renderer_setting", .title = "Renderer Settings",
        .description = "Set one of the current renderer's settings by key "
                       "(get_renderer_info lists them with types and values): "
                       "a boolean for flags, an integer, a number or a string. "
                       "Omit value to toggle a flag; pass null to restore the "
                       "default.",
        .parameters = ParameterSchema(R"({
            "properties": {"key": {"type": "string"}, "value": {}},
            "required": ["key"]
        })"),
        .menu = "Renderer",
        .dynamicChoices = [](const Document &d) {
            // Flags toggle from the menu; others need a value.
            std::vector<MenuChoice> choices;
            for (const RendererSetting &setting : d.GetRendererInfo().settings) {
                if (setting.type == RendererSetting::Type::Flag) {
                    choices.push_back({setting.name, JsObject{{"key", JsValue(setting.key)}}});
                }
            }
            return choices;
        },
        .chosen = [](const Document &d, const JsObject &p) {
            const JsValue value = d.GetRendererSetting(p.at("key").GetString());
            return value.IsBool() && value.GetBool();
        },
        .run = [](Document &d, const JsObject &p) {
            const std::string &key = p.at("key").GetString();
            const RendererSetting *setting = FindSetting(d, key);
            if (!setting) {
                return ActionResult::Error("Unknown renderer setting: " + key);
            }
            JsValue value;
            auto given = p.find("value");
            if (given == p.end()) {
                if (setting->type != RendererSetting::Type::Flag) {
                    return ActionResult::Error(key + " needs a value");
                }
                const JsValue current = d.GetRendererSetting(key);
                value = JsValue(!(current.IsBool() && current.GetBool()));
            } else if (!given->second.IsNull()) {
                if (!Suits(*setting, given->second)) {
                    return ActionResult::Error(key + " takes a " +
                                               TypeName(setting->type) + " value");
                }
                value = Canonical(*setting, given->second);
            }
            return Change(d, [&](ViewSettings &s) {
                if (value.IsNull() || IsDefault(*setting, value)) {
                    s.rendererSettings.erase(key);
                } else {
                    s.rendererSettings[key] = value;
                }
            });
        },
    });
    r.Register({
        .name = "set_renderer_settings", .title = "Renderer Settings…",
        .description = "Set several of the current renderer's settings at once: "
                       "settings maps keys to values, as for set_renderer_setting "
                       "(null restores a default). Every value is checked before "
                       "any is applied. From the menu, a form shows them all.",
        .parameters = ParameterSchema(R"({
            "properties": {"settings": {"type": "object"}},
            "required": ["settings"]
        })"),
        .menu = "Renderer",
        .form = [](const Document &d) {
            std::vector<FormField> fields;
            for (const RendererSetting &setting : d.GetRendererInfo().settings) {
                FormField field{.key = setting.key, .label = setting.name,
                                .value = d.GetRendererSetting(setting.key)};
                switch (setting.type) {
                case RendererSetting::Type::Flag: field.type = FormField::Type::Bool; break;
                case RendererSetting::Type::Int: field.type = FormField::Type::Int; break;
                case RendererSetting::Type::Float: field.type = FormField::Type::Float; break;
                case RendererSetting::Type::String: field.type = FormField::Type::String; break;
                }
                fields.push_back(std::move(field));
            }
            return fields;
        },
        .formParam = "settings",
        .run = [](Document &d, const JsObject &p) {
            std::map<std::string, JsValue> values;
            for (const auto &[key, value] : p.at("settings").GetJsObject()) {
                const RendererSetting *setting = FindSetting(d, key);
                if (!setting) {
                    return ActionResult::Error("Unknown renderer setting: " + key);
                }
                if (!value.IsNull() && !Suits(*setting, value)) {
                    return ActionResult::Error(key + " takes a " +
                                               TypeName(setting->type) + " value");
                }
                values[key] = value.IsNull() ? JsValue() : Canonical(*setting, value);
            }
            return Change(d, [&](ViewSettings &s) {
                for (const auto &[key, value] : values) {
                    const RendererSetting *setting = FindSetting(d, key);
                    if (value.IsNull() || IsDefault(*setting, value)) {
                        s.rendererSettings.erase(key);
                    } else {
                        s.rendererSettings[key] = value;
                    }
                }
            });
        },
        .enabled = [](const Document &d) { return !d.GetRendererInfo().settings.empty(); },
    });
    r.Register({
        .name = "set_render_settings_prim", .title = "Render Settings Prim",
        .description = "The active UsdRenderSettings prim, by path, or an empty "
                       "path for the stage's renderSettingsPrimPath metadata.",
        .parameters = ParameterSchema(R"({
            "properties": {"path": {"type": "string"}},
            "required": ["path"]
        })"),
        .menu = "Renderer", .separatorBefore = true,
        .dynamicChoices = [](const Document &d) {
            std::vector<MenuChoice> choices{{"Stage Default", JsObject{{"path", JsValue("")}}}};
            for (const SdfPath &path : d.GetRenderSettingsPrims()) {
                choices.push_back({path.GetString(), JsObject{{"path", JsValue(path.GetString())}}});
            }
            return choices;
        },
        .chosen = [](const Document &d, const JsObject &p) {
            return d.GetViewSettings().renderSettingsPrim.GetString() ==
                p.at("path").GetString();
        },
        .run = [](Document &d, const JsObject &p) {
            const std::string &path = p.at("path").GetString();
            ActionResult check = CheckPrim(d, path, d.GetRenderSettingsPrims(),
                                           "UsdRenderSettings");
            if (!check.ok) return check;
            return Change(d, [&](ViewSettings &s) {
                s.renderSettingsPrim = path.empty() ? SdfPath() : SdfPath(path);
            });
        },
    });
    r.Register({
        .name = "set_render_pass_prim", .title = "Render Pass Prim",
        .description = "The active UsdRenderPass prim, by path, or an empty path "
                       "for none. As in usdview, a pass also makes its render "
                       "source (renderSource relationship) the active render "
                       "settings prim.",
        .parameters = ParameterSchema(R"({
            "properties": {"path": {"type": "string"}},
            "required": ["path"]
        })"),
        .menu = "Renderer",
        .dynamicChoices = [](const Document &d) {
            std::vector<MenuChoice> choices{{"None", JsObject{{"path", JsValue("")}}}};
            for (const SdfPath &path : d.GetRenderPassPrims()) {
                choices.push_back({path.GetString(), JsObject{{"path", JsValue(path.GetString())}}});
            }
            return choices;
        },
        .chosen = [](const Document &d, const JsObject &p) {
            return d.GetViewSettings().renderPassPrim.GetString() ==
                p.at("path").GetString();
        },
        .run = [](Document &d, const JsObject &p) {
            const std::string &path = p.at("path").GetString();
            ActionResult check = CheckPrim(d, path, d.GetRenderPassPrims(),
                                           "UsdRenderPass");
            if (!check.ok) return check;
            return Change(d, [&](ViewSettings &s) {
                s.renderPassPrim = path.empty() ? SdfPath() : SdfPath(path);
                if (!path.empty()) {
                    s.renderSettingsPrim = RenderPassSource(d.GetStage(), s.renderPassPrim);
                }
            });
        },
    });
    r.Register({
        .name = "get_renderer_info", .title = "Renderer Info",
        .description = "The renderers available and the current one, its AOVs, "
                       "its settings (key, name, type, default and current "
                       "value), and the view's renderer choices. Empty lists "
                       "until a viewport has drawn the stage.",
        .query = true,
        .run = [](Document &d, const JsObject &) {
            const RendererInfo &info = d.GetRendererInfo();
            const ViewSettings &s = d.GetViewSettings();
            JsArray plugins, aovs, settings;
            for (const RendererInfo::Plugin &plugin : info.plugins) {
                plugins.push_back(JsValue(JsObject{{"id", JsValue(plugin.id)},
                                                   {"name", JsValue(plugin.name)}}));
            }
            for (const std::string &aov : info.aovs) aovs.push_back(JsValue(aov));
            for (const RendererSetting &setting : info.settings) {
                settings.push_back(JsValue(JsObject{
                    {"key", JsValue(setting.key)},
                    {"name", JsValue(setting.name)},
                    {"type", JsValue(TypeName(setting.type))},
                    {"default", setting.defaultValue},
                    {"value", d.GetRendererSetting(setting.key)}}));
            }
            return ActionResult{true, JsValue(JsObject{
                {"renderers", JsValue(plugins)},
                {"current", JsValue(info.current)},
                {"aov", JsValue(s.aov)},
                {"aovs", JsValue(aovs)},
                {"settings", JsValue(settings)},
                {"renderSettingsPrim", JsValue(s.renderSettingsPrim.GetString())},
                {"renderPassPrim", JsValue(s.renderPassPrim.GetString())}}), {}};
        },
    });
    r.Register({
        .name = "use_view_as_default", .title = "Use as Default View Settings",
        .description = "Make this window's view settings (draw mode, lights, HUD, "
                       "color correction, renderer and so on) the ones newly "
                       "opened files start with. Scene-specific choices (AOV, "
                       "renderer setting overrides, render settings and pass "
                       "prims) are not kept.",
        .menu = "View", .separatorBefore = true,
        .run = [](Document &d, const JsObject &) {
            AppSettingsStore &store = AppSettingsStore::Get();
            AppSettings settings = store.GetSettings();
            settings.view = AsDefaults(d.GetViewSettings());
            store.SetSettings(settings);
            return Done();
        },
    });
    r.Register({
        .name = "get_settings", .title = "Settings",
        .description = "The app's settings (the Settings window), as JSON: view "
                       "defaults for newly opened files under \"view\", and "
                       "loadPayloads, usePrimaryCamera and threadLimit (0 for "
                       "all threads; applies at next launch).",
        .query = true,
        .run = [](Document &, const JsObject &) {
            return ActionResult{true, JsValue(ToJson(AppSettingsStore::Get().GetSettings())), {}};
        },
    });
    r.Register({
        .name = "set_settings", .title = "Change Settings",
        .description = "Change the app's settings: an object with any of the keys "
                       "get_settings returns (a partial \"view\" object changes "
                       "only the view defaults it names). Unknown keys are errors.",
        .parameters = ParameterSchema(R"({
            "properties": {"settings": {"type": "object"}},
            "required": ["settings"]
        })"),
        .run = [](Document &, const JsObject &p) {
            const JsObject &given = p.at("settings").GetJsObject();
            const JsObject known = ToJson(AppSettings());
            for (const auto &[key, value] : given) {
                if (!known.count(key)) return ActionResult::Error("Unknown setting: " + key);
                if (key == "view") {
                    if (!value.IsObject()) return ActionResult::Error("view must be an object");
                    const JsObject knownView = known.at("view").GetJsObject();
                    for (const auto &[viewKey, unused] : value.GetJsObject()) {
                        if (!knownView.count(viewKey)) {
                            return ActionResult::Error("Unknown view setting: " + viewKey);
                        }
                    }
                }
            }
            AppSettings settings = AppSettingsStore::Get().GetSettings();
            FromJson(given, &settings);
            settings.view = AsDefaults(settings.view);
            AppSettingsStore::Get().SetSettings(settings);
            return Done();
        },
    });
    r.Register({
        .name = "reset_settings", .title = "Reset Settings",
        .description = "Restore every app setting to its default (usdview's "
                       "defaults for the view). Open windows keep their view "
                       "settings.",
        .run = [](Document &, const JsObject &) {
            AppSettingsStore::Get().Reset();
            return Done();
        },
    });
}

} // namespace vela
