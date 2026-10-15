// View settings: how the viewport draws, not what the stage contains. None
// of these author scene data or create undo steps.
#include "actions.h"
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
                           "hudPerformance, hudGPUStats.",
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
                if (value.IsNull() || value == Canonical(*setting, setting->defaultValue)) {
                    s.rendererSettings.erase(key);
                } else {
                    s.rendererSettings[key] = value;
                }
            });
        },
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
}

} // namespace vela
