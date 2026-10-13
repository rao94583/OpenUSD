#include "actions.h"
#include "document.h"
#include "test_support.h"

#include "pxr/base/js/json.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usdGeom/cube.h"

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

} // namespace

int main() {
    test::RegisterPlugins();
    TestValidation();
    TestRegistry();
    TestStandardActions();
    return test::Finish("action");
}
