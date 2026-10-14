// View settings: how the viewport draws, not what the stage contains. None
// of these author scene data or create undo steps.
#include "actions.h"
#include "document.h"

#include "pxr/base/js/json.h"

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
    };
    return options;
}
const Option *FindOption(const std::string &name) {
    for (const Option &option : Options()) {
        if (name == option.name) return &option;
    }
    return nullptr;
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
                           "sceneMaterials, cullBackfaces, and the purposes drawn "
                           "besides default: proxy, render and guide.",
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
}

} // namespace vela
