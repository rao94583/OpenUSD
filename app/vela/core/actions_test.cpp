#include "actions.h"
#include "document.h"
#include "test_support.h"

#include "pxr/base/js/json.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usdGeom/cube.h"
#include "pxr/usd/usdRender/pass.h"
#include "pxr/usd/usdRender/settings.h"

#include <set>

using namespace vela;

namespace {

UsdStageRefPtr MakeStage() {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    stage->SetStartTimeCode(1);
    stage->SetEndTimeCode(10);
    UsdGeomCube::Define(stage, SdfPath("/A"));
    UsdGeomCube::Define(stage, SdfPath("/B"));
    return stage;
}

JsObject Params(const std::string &json) {
    return JsParseString(json).GetJsObject();
}

SdfPathVector Paths(std::initializer_list<const char *> paths) {
    SdfPathVector result;
    for (const char *p : paths) result.emplace_back(p);
    return result;
}

void TestValidation() {
    const JsObject schema = ParameterSchema(R"({
        "properties": {
            "name": {"type": "string"},
            "count": {"type": "integer"},
            "scale": {"type": "number"},
            "mode": {"type": "string", "enum": ["a", "b"]},
            "point": {"type": "array", "items": {"type": "number"},
                      "minItems": 3, "maxItems": 3}
        },
        "required": ["name"]
    })");
    auto check = [&](const char *json) {
        return ActionRegistry::Validate(schema, Params(json));
    };
    CHECK(check(R"({"name": "x"})").empty());
    CHECK(check(R"({"name": "x", "count": 2, "scale": 2, "mode": "b",
                    "point": [1, 2.5, 3]})").empty());
    CHECK(check(R"({})").find("Missing parameter 'name'") == 0);
    CHECK(check(R"({"name": 1})").find("must be of type string") != std::string::npos);
    CHECK(check(R"({"name": "x", "count": 1.5})").find("integer") != std::string::npos);
    CHECK(check(R"({"name": "x", "mode": "c"})").find("one of") != std::string::npos);
    CHECK(check(R"({"name": "x", "point": [1, 2]})").find("number of items") != std::string::npos);
    CHECK(check(R"({"name": "x", "point": [1, "2", 3]})").find("point[1]") != std::string::npos);
    CHECK(check(R"({"name": "x", "extra": 1})").find("Unknown parameter") == 0);
}

void TestRegistry() {
    ActionRegistry registry;
    int runs = 0;
    registry.Register({
        .name = "count", .title = "Count",
        .run = [&](Document &, const JsObject &) { ++runs; return ActionResult(); },
    });
    registry.Register({
        .name = "count", .title = "Count Twice",
        .run = [&](Document &, const JsObject &) { runs += 2; return ActionResult(); },
    });
    CHECK(registry.GetActions().size() == 1);
    CHECK(registry.Find("count")->title == "Count Twice");

    Document d(MakeStage());
    CHECK(registry.Run(d, "count").ok);
    CHECK(runs == 2);
    ActionResult missing = registry.Run(d, "nope");
    CHECK(!missing.ok && missing.error == "Unknown action 'nope'");
    ActionResult extra = registry.Run(d, "count", Params(R"({"x": 1})"));
    CHECK(!extra.ok && extra.error.find("Unknown parameter") != std::string::npos);

    // An undoable action's edits are one undo step named by its title.
    registry.Register({
        .name = "define_two", .title = "Define Two", .undoable = true,
        .run = [](Document &doc, const JsObject &) {
            UsdGeomCube::Define(doc.GetStage(), SdfPath("/C"));
            UsdGeomCube::Define(doc.GetStage(), SdfPath("/D"));
            return ActionResult();
        },
    });
    CHECK(registry.Run(d, "define_two").ok);
    CHECK(d.GetUndoStack().GetUndoName() == "Define Two");
    CHECK(d.GetUndoStack().Undo());
    CHECK(!d.GetStage()->GetPrimAtPath(SdfPath("/C")));
    CHECK(!d.GetStage()->GetPrimAtPath(SdfPath("/D")));
    CHECK(!d.GetUndoStack().CanUndo());
}

void TestStandardActions() {
    ActionRegistry &registry = ActionRegistry::Get();
    Document d(MakeStage());

    // Every action has a description, and names and shortcuts are unique.
    std::set<std::string> names, shortcuts;
    for (const Action &action : registry.GetActions()) {
        CHECK(!action.description.empty());
        CHECK(names.insert(action.name).second);
        if (!action.key.empty()) {
            CHECK(shortcuts.insert(action.key + "/" + std::to_string(action.modifiers)).second);
        }
    }
    CHECK(registry.FindShortcut("f", 0)->name == "frame_selection");
    CHECK(registry.FindShortcut("z", ModifierCommand | ModifierShift)->name == "redo");
    CHECK(registry.FindShortcut(Keys::RightArrow, 0)->name == "next_frame");
    CHECK(!registry.FindShortcut("q", 0));

    // select
    ActionResult result = registry.Run(d, "select", Params(R"({"paths": ["/A"]})"));
    CHECK(result.ok);
    CHECK(result.value == JsParseString(R"({"selection": ["/A"]})"));
    registry.Run(d, "select", Params(R"({"paths": ["/B"], "mode": "add"})"));
    CHECK(d.GetSelection() == Paths({"/A", "/B"}));
    registry.Run(d, "select", Params(R"({"paths": ["/A", "/B"], "mode": "toggle"})"));
    CHECK(d.GetSelection().empty());
    registry.Run(d, "select", Params(R"({"paths": ["/A", "/B"], "mode": "add"})"));
    registry.Run(d, "select", Params(R"({"paths": ["/A"], "mode": "remove"})"));
    CHECK(d.GetSelection() == Paths({"/B"}));
    result = registry.Run(d, "select", Params(R"({"paths": ["/Missing"]})"));
    CHECK(!result.ok && result.error == "No prim at '/Missing'");
    CHECK(d.GetSelection() == Paths({"/B"}));
    CHECK(!registry.Run(d, "select", Params(R"({"paths": ["A"]})")).ok);
    CHECK(!registry.Run(d, "select", Params(R"({"paths": ["/A"], "mode": "xor"})")).ok);
    CHECK(registry.IsEnabled(*registry.Find("clear_selection"), d));
    registry.Run(d, "clear_selection");
    CHECK(d.GetSelection().empty());
    CHECK(!registry.IsEnabled(*registry.Find("clear_selection"), d));
    CHECK(!registry.Run(d, "clear_selection").ok);

    // Time and playback
    registry.Run(d, "next_frame");
    CHECK(d.GetTime() == UsdTimeCode(2));
    registry.Run(d, "previous_frame");
    registry.Run(d, "previous_frame");
    CHECK(d.GetTime() == UsdTimeCode(10));
    registry.Run(d, "first_frame");
    CHECK(d.GetTime() == UsdTimeCode(1));
    registry.Run(d, "last_frame");
    CHECK(d.GetTime() == UsdTimeCode(10));
    CHECK(registry.Run(d, "set_time", Params(R"({"time": 3.5})")).ok);
    CHECK(d.GetTime() == UsdTimeCode(3.5));
    CHECK(registry.Run(d, "set_time", Params(R"({"time": 7})")).ok);
    CHECK(d.GetTime() == UsdTimeCode(7));
    CHECK(!registry.Run(d, "set_time").ok);
    const Action &play = *registry.Find("toggle_playback");
    CHECK(registry.GetTitle(play, d) == "Play");
    registry.Run(d, "toggle_playback");
    CHECK(d.IsPlaying() && registry.GetTitle(play, d) == "Pause");
    registry.Run(d, "toggle_playback");
    CHECK(!d.IsPlaying());

    // Camera
    CHECK(registry.Run(d, "set_camera",
                       Params(R"({"eye": [0, 0, 10], "target": [0, 0, 0], "fov": 40})")).ok);
    CHECK((d.GetCamera().GetCenter() - GfVec3d(0.0)).GetLength() < 1e-9);
    CHECK(std::abs(d.GetCamera().GetDistance() - 10.0) < 1e-9);
    CHECK(std::abs(d.GetCamera().GetFieldOfView() - 40.0) < 1e-4);
    const GfMatrix4d transform = d.GetCamera().ComputeTransform();
    CHECK((transform.ExtractTranslation() - GfVec3d(0, 0, 10)).GetLength() < 1e-9);
    // Straight down still works.
    CHECK(registry.Run(d, "set_camera",
                       Params(R"({"eye": [0, 10, 0], "target": [0, 0, 0]})")).ok);
    CHECK((d.GetCamera().ComputeTransform().ExtractTranslation() -
           GfVec3d(0, 10, 0)).GetLength() < 1e-6);
    CHECK(!registry.Run(d, "set_camera",
                        Params(R"({"eye": [1, 1, 1], "target": [1, 1, 1]})")).ok);
    CHECK(!registry.Run(d, "set_camera", Params(R"({"eye": [1, 1], "target": [0, 0, 0]})")).ok);
    registry.Run(d, "select", Params(R"({"paths": ["/B"]})"));
    registry.Run(d, "frame_selection");
    CHECK((d.GetCamera().GetCenter() - GfVec3d(0.0)).GetLength() < 1e-9);
    registry.Run(d, "frame_all");

    // Undo and redo, with titles naming the step.
    const Action &undo = *registry.Find("undo");
    const Action &redo = *registry.Find("redo");
    CHECK(!registry.IsEnabled(undo, d));
    CHECK(registry.GetTitle(undo, d) == "Undo");
    {
        UndoBlock block(d.GetUndoStack(), "Add Cube");
        UsdGeomCube::Define(d.GetStage(), SdfPath("/E"));
    }
    CHECK(registry.GetTitle(undo, d) == "Undo Add Cube");
    CHECK(registry.Run(d, "undo").ok);
    CHECK(!d.GetStage()->GetPrimAtPath(SdfPath("/E")));
    CHECK(registry.GetTitle(redo, d) == "Redo Add Cube");
    CHECK(registry.Run(d, "redo").ok);
    CHECK(d.GetStage()->GetPrimAtPath(SdfPath("/E")));
    CHECK(!registry.Run(d, "redo").ok);

    // Edit target
    const std::string root = d.GetStage()->GetRootLayer()->GetIdentifier();
    const std::string session = d.GetStage()->GetSessionLayer()->GetIdentifier();
    CHECK(registry.Run(d, "set_edit_target",
                       JsObject{{"layer", JsValue(session)}}).ok);
    CHECK(d.GetEditTarget() == d.GetStage()->GetSessionLayer());
    CHECK(!registry.Run(d, "set_edit_target", Params(R"({"layer": "nope.usda"})")).ok);
    CHECK(registry.Run(d, "set_edit_target", JsObject{{"layer", JsValue(root)}}).ok);

    // Describe lists every action, with its schema and shortcut.
    const JsArray described = registry.Describe().GetJsArray();
    CHECK(described.size() == registry.GetActions().size());
    bool sawRedo = false;
    for (const JsValue &entry : described) {
        const JsObject &o = entry.GetJsObject();
        CHECK(o.at("parameters").GetJsObject().at("type").GetString() == "object");
        if (o.at("name").GetString() == "redo") {
            sawRedo = true;
            CHECK(o.at("shortcut").GetString() == "Shift+Command+Z");
            CHECK(o.at("menu").GetString() == "Edit");
        }
    }
    CHECK(sawRedo);
}

void TestViewActions() {
    ActionRegistry &registry = ActionRegistry::Get();
    Document d(MakeStage());
    std::string before;
    d.GetStage()->GetRootLayer()->ExportToString(&before);
    unsigned seen = 0;
    d.AddObserver([&](unsigned changes) { seen |= changes; });
    const ViewSettings defaults;
    CHECK(d.GetViewSettings() == defaults);
    CHECK(d.GetViewSettings().background == Background::DarkGrey);
    CHECK(BackgroundColor(Background::DarkGrey)[0] < 0.1f);  // usdview's, linear

    // Draw mode: choices, the chosen one, and an invalid name.
    const Action &drawMode = *registry.Find("set_draw_mode");
    CHECK(drawMode.choices.size() == DrawModeNames().size());
    CHECK(drawMode.chosen(d, Params(R"({"mode": "shadedSmooth"})")));
    CHECK(registry.Run(d, "set_draw_mode", Params(R"({"mode": "wireframe"})")).ok);
    CHECK(d.GetViewSettings().drawMode == DrawMode::Wireframe);
    CHECK(seen == Document::Display);
    CHECK(drawMode.chosen(d, Params(R"({"mode": "wireframe"})")));
    CHECK(!drawMode.chosen(d, Params(R"({"mode": "shadedSmooth"})")));
    CHECK(!registry.Run(d, "set_draw_mode", Params(R"({"mode": "hidden"})")).ok);
    CHECK(!IsShaded(DrawMode::Wireframe) && IsShaded(DrawMode::GeomSmooth));

    // Options toggle, or take an explicit value.
    CHECK(registry.Run(d, "set_view_option", Params(R"({"option": "guide"})")).ok);
    CHECK(d.GetViewSettings().showGuides);
    CHECK(registry.Find("set_view_option")->chosen(d, Params(R"({"option": "guide"})")));
    CHECK(registry.Run(d, "set_view_option", Params(R"({"option": "guide"})")).ok);
    CHECK(!d.GetViewSettings().showGuides);
    CHECK(registry.Run(d, "set_view_option",
                       Params(R"({"option": "cameraLight", "enabled": false})")).ok);
    CHECK(!d.GetViewSettings().cameraLight);
    CHECK(registry.Run(d, "set_view_option",
                       Params(R"({"option": "cameraLight", "enabled": false})")).ok);
    CHECK(!d.GetViewSettings().cameraLight);
    CHECK(!registry.Run(d, "set_view_option", Params(R"({"option": "fog"})")).ok);

    // Complexity, background and highlighting.
    CHECK(registry.Run(d, "set_complexity", Params(R"({"level": "high"})")).ok);
    CHECK(std::abs(d.GetViewSettings().complexity - 1.2) < 1e-9);
    CHECK(registry.Find("set_complexity")->chosen(d, Params(R"({"level": "high"})")));
    CHECK(registry.Run(d, "set_background", Params(R"({"color": "white"})")).ok);
    CHECK(d.GetViewSettings().background == Background::White);
    CHECK(registry.Run(d, "set_selection_highlighting", Params(R"({"mode": "always"})")).ok);
    CHECK(d.GetViewSettings().highlightMode == HighlightMode::Always);

    // View settings are not scene edits.
    CHECK(!d.GetUndoStack().CanUndo());
    std::string after;
    d.GetStage()->GetRootLayer()->ExportToString(&after);
    CHECK(after == before);
}

void TestRendererActions() {
    ActionRegistry &registry = ActionRegistry::Get();
    UsdStageRefPtr stage = MakeStage();
    UsdRenderSettings::Define(stage, SdfPath("/Render/Settings"));
    UsdRenderSettings::Define(stage, SdfPath("/Render/Other"));
    UsdRenderPass pass = UsdRenderPass::Define(stage, SdfPath("/Render/Pass"));
    pass.CreateRenderSourceRel().AddTarget(SdfPath("/Render/Other"));
    UsdRenderPass::Define(stage, SdfPath("/Render/Sourceless"));
    Document d(stage);
    unsigned seen = 0;
    d.AddObserver([&](unsigned changes) { seen |= changes; });

    // Before a viewport publishes what the renderer offers: any renderer
    // name is accepted (the renderer resolves it), settings are unknown.
    CHECK(d.GetRendererInfo().plugins.empty());
    CHECK(registry.Run(d, "set_renderer", Params(R"({"renderer": "Embree"})")).ok);
    CHECK(d.GetViewSettings().renderer == "Embree");
    CHECK(!registry.Run(d, "set_renderer_setting", Params(R"({"key": "x"})")).ok);

    RendererInfo info;
    info.plugins = {{"HdStormRendererPlugin", "GL"}, {"HdEmbreeRendererPlugin", "Embree"}};
    info.current = "HdEmbreeRendererPlugin";
    info.aovs = {"color", "depth"};
    info.settings = {
        {"spp", "Samples per pixel", RendererSetting::Type::Int, JsValue(int64_t(1))},
        {"ao", "Ambient occlusion", RendererSetting::Type::Flag, JsValue(true)},
        {"gamma", "Gamma", RendererSetting::Type::Float, JsValue(2.2)},
    };
    seen = 0;
    d.SetRendererInfo(info);
    CHECK(seen == Document::Renderer);
    seen = 0;
    d.SetRendererInfo(info);
    CHECK(seen == 0);

    // Renderers: menu choices by display name, chosen by the current id.
    const Action &renderer = *registry.Find("set_renderer");
    const std::vector<MenuChoice> renderers = renderer.dynamicChoices(d);
    CHECK(renderers.size() == 2 && renderers[1].title == "Embree");
    CHECK(renderer.chosen(d, renderers[1].params));
    CHECK(!renderer.chosen(d, renderers[0].params));
    CHECK(!registry.Run(d, "set_renderer", Params(R"({"renderer": "Cycles"})")).ok);

    // AOVs, and settings: flags toggle, others take values of their type.
    CHECK(registry.Find("set_aov")->dynamicChoices(d).size() == 2);
    CHECK(registry.Run(d, "set_aov", Params(R"({"aov": "primvars:st"})")).ok);
    CHECK(d.GetViewSettings().aov == "primvars:st");
    const Action &setting = *registry.Find("set_renderer_setting");
    CHECK(setting.dynamicChoices(d).size() == 1);  // the flag
    CHECK(setting.chosen(d, Params(R"({"key": "ao"})")));
    CHECK(registry.Run(d, "set_renderer_setting", Params(R"({"key": "ao"})")).ok);
    CHECK(d.GetRendererSetting("ao") == JsValue(false));
    CHECK(!setting.chosen(d, Params(R"({"key": "ao"})")));
    CHECK(registry.Run(d, "set_renderer_setting", Params(R"({"key": "spp", "value": 8})")).ok);
    CHECK(d.GetRendererSetting("spp") == JsValue(int64_t(8)));
    CHECK(registry.Run(d, "set_renderer_setting", Params(R"({"key": "gamma", "value": 1})")).ok);
    CHECK(!registry.Run(d, "set_renderer_setting", Params(R"({"key": "spp", "value": 1.5})")).ok);
    CHECK(!registry.Run(d, "set_renderer_setting", Params(R"({"key": "spp"})")).ok);
    CHECK(!registry.Run(d, "set_renderer_setting", Params(R"({"key": "nope", "value": 1})")).ok);
    // Back to the default (by value or null) drops the override.
    CHECK(registry.Run(d, "set_renderer_setting", Params(R"({"key": "ao"})")).ok);
    CHECK(registry.Run(d, "set_renderer_setting", Params(R"({"key": "spp", "value": 1})")).ok);
    CHECK(d.GetViewSettings().rendererSettings.size() == 1);  // gamma

    // get_renderer_info reports it all.
    ActionResult result = registry.Run(d, "get_renderer_info");
    CHECK(result.ok);
    const JsObject &reported = result.value.GetJsObject();
    CHECK(reported.at("current").GetString() == "HdEmbreeRendererPlugin");
    CHECK(reported.at("renderers").GetJsArray().size() == 2);
    CHECK(reported.at("aov").GetString() == "primvars:st");
    const JsObject &gamma = reported.at("settings").GetJsArray()[2].GetJsObject();
    CHECK(gamma.at("type").GetString() == "float");
    CHECK(gamma.at("value") == JsValue(1.0));

    CHECK(registry.Run(d, "set_renderer_setting",
                       Params(R"({"key": "gamma", "value": null})")).ok);
    CHECK(d.GetViewSettings().rendererSettings.empty());
    CHECK(registry.Run(d, "set_renderer_setting", Params(R"({"key": "spp", "value": 4})")).ok);

    // Several at once, as the form sends them: checked first, so a bad
    // value changes nothing.
    const Action &settings = *registry.Find("set_renderer_settings");
    const std::vector<FormField> fields = settings.form(d);
    CHECK(fields.size() == 3);
    CHECK(fields[0].type == FormField::Type::Int && fields[0].value == JsValue(int64_t(4)));
    CHECK(fields[1].type == FormField::Type::Bool && fields[2].type == FormField::Type::Float);
    CHECK(!registry.Run(d, "set_renderer_settings",
                        Params(R"({"settings": {"ao": false, "spp": "many"}})")).ok);
    CHECK(d.GetRendererSetting("ao") == JsValue(true));
    CHECK(registry.Run(d, "set_renderer_settings",
                       Params(R"({"settings": {"ao": false, "spp": 2, "gamma": 2.5}})")).ok);
    CHECK(d.GetRendererSetting("ao") == JsValue(false));
    CHECK(d.GetRendererSetting("spp") == JsValue(int64_t(2)));
    CHECK(d.GetViewSettings().rendererSettings.size() == 3);
    // A float default round-tripped through text (2.2f prints as
    // 2.200000048) is still the default; null restores one.
    CHECK(registry.Run(d, "set_renderer_settings", JsObject{{"settings", JsValue(JsObject{
        {"gamma", JsValue(double(2.2f))}, {"ao", JsValue()}})}}).ok);
    CHECK(d.GetViewSettings().rendererSettings.size() == 1);  // spp

    // Switching renderer resets the AOV and settings.
    CHECK(registry.Run(d, "set_renderer", Params(R"({"renderer": "GL"})")).ok);
    CHECK(d.GetViewSettings().aov == "color");
    CHECK(d.GetViewSettings().rendererSettings.empty());

    // Render settings and passes; a pass brings its render source.
    const Action &settingsPrim = *registry.Find("set_render_settings_prim");
    CHECK(settingsPrim.dynamicChoices(d).size() == 3);  // stage default + 2
    CHECK(settingsPrim.chosen(d, Params(R"({"path": ""})")));
    CHECK(registry.Run(d, "set_render_settings_prim",
                       Params(R"({"path": "/Render/Settings"})")).ok);
    CHECK(d.GetViewSettings().renderSettingsPrim == SdfPath("/Render/Settings"));
    CHECK(!registry.Run(d, "set_render_settings_prim", Params(R"({"path": "/A"})")).ok);
    CHECK(!registry.Run(d, "set_render_settings_prim", Params(R"({"path": "bad path"})")).ok);
    CHECK(registry.Run(d, "set_render_pass_prim", Params(R"({"path": "/Render/Pass"})")).ok);
    CHECK(d.GetViewSettings().renderPassPrim == SdfPath("/Render/Pass"));
    CHECK(d.GetViewSettings().renderSettingsPrim == SdfPath("/Render/Other"));
    CHECK(registry.Run(d, "set_render_pass_prim",
                       Params(R"({"path": "/Render/Sourceless"})")).ok);
    CHECK(d.GetViewSettings().renderSettingsPrim.IsEmpty());
    CHECK(registry.Run(d, "set_render_pass_prim", Params(R"({"path": ""})")).ok);
    CHECK(d.GetViewSettings().renderPassPrim.IsEmpty());
    CHECK(!d.GetUndoStack().CanUndo());
}

} // namespace

int main() {
    test::RegisterPlugins();
    TestValidation();
    TestRegistry();
    TestStandardActions();
    TestViewActions();
    TestRendererActions();
    return test::Finish("action");
}
