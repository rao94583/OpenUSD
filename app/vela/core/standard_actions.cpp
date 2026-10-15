// The actions Vela offers. Menus are built from these, in this order.
#include "actions.h"
#include "document.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/usd/usd/prim.h"

#include <algorithm>

namespace vela {

namespace {

double Number(const JsValue &value) {
    return value.IsReal() ? value.GetReal()
         : value.IsUInt64() ? double(value.GetUInt64()) : double(value.GetInt64());
}

GfVec3d Vector(const JsValue &value) {
    const JsArray &a = value.GetJsArray();
    return GfVec3d(Number(a[0]), Number(a[1]), Number(a[2]));
}

JsValue PathArray(const SdfPathVector &paths) {
    JsArray array;
    for (const SdfPath &path : paths) array.push_back(JsValue(path.GetString()));
    return JsValue(array);
}

ActionResult Done() { return ActionResult(); }

void RegisterEdit(ActionRegistry &r) {
    r.Register({
        .name = "undo",
        .title = "Undo",
        .description = "Revert the last change to the stage. Each action, "
                       "script or Claude request is one step.",
        .menu = "Edit", .key = "z", .modifiers = ModifierCommand,
        .run = [](Document &d, const JsObject &) {
            d.GetUndoStack().Undo();
            return Done();
        },
        .enabled = [](const Document &d) {
            return d.GetUndoStack().CanUndo();
        },
        .dynamicTitle = [](const Document &d) {
            const std::string name = d.GetUndoStack().GetUndoName();
            return name.empty() ? std::string("Undo") : "Undo " + name;
        },
    });
    r.Register({
        .name = "redo",
        .title = "Redo",
        .description = "Reapply the last change that was undone.",
        .menu = "Edit", .key = "z", .modifiers = ModifierCommand | ModifierShift,
        .run = [](Document &d, const JsObject &) {
            d.GetUndoStack().Redo();
            return Done();
        },
        .enabled = [](const Document &d) {
            return d.GetUndoStack().CanRedo();
        },
        .dynamicTitle = [](const Document &d) {
            const std::string name = d.GetUndoStack().GetRedoName();
            return name.empty() ? std::string("Redo") : "Redo " + name;
        },
    });
    r.Register({
        .name = "select",
        .title = "Select",
        .description = "Change the selection. mode is replace (the default), "
                       "add, remove or toggle. Returns the new selection.",
        .parameters = ParameterSchema(R"({
            "properties": {
                "paths": {"type": "array", "items": {"type": "string"},
                          "description": "Absolute prim paths"},
                "mode": {"type": "string",
                         "enum": ["replace", "add", "remove", "toggle"]}
            },
            "required": ["paths"]
        })"),
        .run = [](Document &d, const JsObject &params) {
            SdfPathVector paths;
            for (const JsValue &value : params.at("paths").GetJsArray()) {
                const SdfPath path(value.GetString());
                if (!path.IsAbsolutePath() || !path.IsPrimPath() ||
                    !d.GetStage()->GetPrimAtPath(path)) {
                    return ActionResult::Error("No prim at '" + value.GetString() + "'");
                }
                paths.push_back(path);
            }
            auto mode = params.find("mode");
            const std::string m = mode == params.end() ? "replace" : mode->second.GetString();
            SdfPathVector selection = m == "replace" ? SdfPathVector() : d.GetSelection();
            for (const SdfPath &path : paths) {
                auto found = std::find(selection.begin(), selection.end(), path);
                if (m == "remove" || (m == "toggle" && found != selection.end())) {
                    if (found != selection.end()) selection.erase(found);
                } else if (found == selection.end()) {
                    selection.push_back(path);
                }
            }
            d.SetSelection(selection);
            return ActionResult{true, JsValue(JsObject{
                {"selection", PathArray(d.GetSelection())}}), ""};
        },
    });
    r.Register({
        .name = "clear_selection",
        .title = "Select None",
        .description = "Deselect everything.",
        .menu = "Edit", .key = "a", .modifiers = ModifierCommand | ModifierShift,
        .separatorBefore = true,
        .run = [](Document &d, const JsObject &) {
            d.SetSelection({});
            return Done();
        },
        .enabled = [](const Document &d) { return !d.GetSelection().empty(); },
    });
    r.Register({
        .name = "select_path",
        .title = "Select Path…",
        .description = "Select one prim by absolute path and frame it, as usdview's "
                       "--select does.",
        .parameters = ParameterSchema(R"({
            "properties": {"path": {"type": "string"}},
            "required": ["path"]
        })"),
        .menu = "Edit",
        .form = [](const Document &d) {
            const SdfPathVector &selection = d.GetSelection();
            return std::vector<FormField>{{
                .key = "path", .label = "Prim path", .type = FormField::Type::String,
                .value = JsValue(selection.empty() ? std::string("/")
                                                   : selection.front().GetString())}};
        },
        .run = [](Document &d, const JsObject &params) {
            const std::string &text = params.at("path").GetString();
            if (!SdfPath::IsValidPathString(text) || !SdfPath(text).IsAbsolutePath() ||
                !SdfPath(text).IsPrimPath() || !d.GetStage()->GetPrimAtPath(SdfPath(text))) {
                return ActionResult::Error("No prim at '" + text + "'");
            }
            d.SetSelection({SdfPath(text)});
            d.FrameSelection();
            return Done();
        },
    });
    r.Register({
        .name = "set_edit_target",
        .title = "Set Edit Target",
        .description = "Send edits to a layer in the stage's layer stack, by "
                       "identifier (as listed by the layer stack).",
        .parameters = ParameterSchema(R"({
            "properties": {"layer": {"type": "string"}},
            "required": ["layer"]
        })"),
        .run = [](Document &d, const JsObject &params) {
            const std::string &id = params.at("layer").GetString();
            for (const SdfLayerHandle &layer : d.GetStage()->GetLayerStack()) {
                if (layer->GetIdentifier() == id) {
                    std::string error;
                    return d.SetEditTarget(layer, &error)
                        ? Done() : ActionResult::Error(error);
                }
            }
            return ActionResult::Error("No layer '" + id + "' in the layer stack");
        },
    });
}

void RegisterView(ActionRegistry &r) {
    r.Register({
        .name = "frame_selection",
        .title = "Frame Selected",
        .description = "Move the camera to fit the selection in view, or "
                       "everything when nothing is selected.",
        .menu = "View", .key = "f",
        .run = [](Document &d, const JsObject &) {
            d.FrameSelection();
            return Done();
        },
    });
    r.Register({
        .name = "frame_all",
        .title = "Frame All",
        .description = "Move the camera to fit the whole stage in view.",
        .menu = "View", .key = "a",
        .run = [](Document &d, const JsObject &) {
            d.FrameAll();
            return Done();
        },
    });
    r.Register({
        .name = "set_camera",
        .title = "Set Camera",
        .description = "Place the viewport's free camera at eye, looking at "
                       "target (world coordinates), which becomes the center "
                       "of interest. fov is the vertical field of view in "
                       "degrees.",
        .parameters = ParameterSchema(R"({
            "properties": {
                "eye": {"type": "array", "items": {"type": "number"},
                        "minItems": 3, "maxItems": 3},
                "target": {"type": "array", "items": {"type": "number"},
                           "minItems": 3, "maxItems": 3},
                "fov": {"type": "number"}
            },
            "required": ["eye", "target"]
        })"),
        .run = [](Document &d, const JsObject &params) {
            const GfVec3d eye = Vector(params.at("eye"));
            const GfVec3d target = Vector(params.at("target"));
            const double distance = (target - eye).GetLength();
            if (distance <= 0.0) {
                return ActionResult::Error("eye and target must differ");
            }
            const bool zUp = d.GetCamera().IsZUp();
            GfVec3d up = zUp ? GfVec3d::ZAxis() : GfVec3d::YAxis();
            if (GfCross(target - eye, up).GetLength() < 1e-9 * distance) {
                // Looking straight up or down.
                up = zUp ? GfVec3d::YAxis() : -GfVec3d::ZAxis();
            }
            GfCamera camera;
            auto fov = params.find("fov");
            camera.SetPerspectiveFromAspectRatioAndFieldOfView(
                1.0, fov == params.end() ? d.GetCamera().GetFieldOfView()
                                         : Number(fov->second),
                GfCamera::FOVVertical);
            camera.SetTransform(GfMatrix4d().SetLookAt(eye, target, up).GetInverse());
            camera.SetFocusDistance(distance);
            d.SetCamera(FreeCamera::FromGfCamera(camera, zUp, distance));
            return Done();
        },
    });
}

void RegisterPlayback(ActionRegistry &r) {
    auto animated = [](const Document &d) { return d.IsAnimated(); };
    r.Register({
        .name = "toggle_playback",
        .title = "Play",
        .description = "Start or stop playing the stage's time range.",
        .menu = "Playback", .key = " ",
        .run = [](Document &d, const JsObject &) {
            d.SetPlaying(!d.IsPlaying());
            return Done();
        },
        .enabled = animated,
        .dynamicTitle = [](const Document &d) {
            return std::string(d.IsPlaying() ? "Pause" : "Play");
        },
    });
    r.Register({
        .name = "next_frame",
        .title = "Next Frame",
        .description = "Step forward one frame, wrapping at the end.",
        .menu = "Playback", .key = Keys::RightArrow, .separatorBefore = true,
        .run = [](Document &d, const JsObject &) {
            d.StepTime(1);
            return Done();
        },
        .enabled = animated,
    });
    r.Register({
        .name = "previous_frame",
        .title = "Previous Frame",
        .description = "Step back one frame, wrapping at the start.",
        .menu = "Playback", .key = Keys::LeftArrow,
        .run = [](Document &d, const JsObject &) {
            d.StepTime(-1);
            return Done();
        },
        .enabled = animated,
    });
    r.Register({
        .name = "first_frame",
        .title = "First Frame",
        .description = "Go to the start of the time range.",
        .menu = "Playback", .key = Keys::LeftArrow, .modifiers = ModifierOption,
        .run = [](Document &d, const JsObject &) {
            d.SetTime(UsdTimeCode(d.GetStartTime()));
            return Done();
        },
        .enabled = animated,
    });
    r.Register({
        .name = "last_frame",
        .title = "Last Frame",
        .description = "Go to the end of the time range.",
        .menu = "Playback", .key = Keys::RightArrow, .modifiers = ModifierOption,
        .run = [](Document &d, const JsObject &) {
            d.SetTime(UsdTimeCode(d.GetEndTime()));
            return Done();
        },
        .enabled = animated,
    });
    r.Register({
        .name = "set_playback_range",
        .title = "Set Playback Range…",
        .description = "Play and step between start and end time codes instead "
                       "of the stage's range (usdview's --ff and --lf). The "
                       "current time moves into the range if it is outside.",
        .parameters = ParameterSchema(R"({
            "properties": {"start": {"type": "number"}, "end": {"type": "number"}},
            "required": ["start", "end"]
        })"),
        .menu = "Playback", .separatorBefore = true,
        .form = [](const Document &d) {
            return std::vector<FormField>{
                {.key = "start", .label = "Start", .type = FormField::Type::Float,
                 .value = JsValue(d.GetStartTime())},
                {.key = "end", .label = "End", .type = FormField::Type::Float,
                 .value = JsValue(d.GetEndTime())},
            };
        },
        .run = [](Document &d, const JsObject &params) {
            const double start = Number(params.at("start")), end = Number(params.at("end"));
            if (end < start) return ActionResult::Error("The end is before the start");
            d.SetPlaybackRange(start, end);
            const double time = d.GetTime().IsDefault() ? start : d.GetTime().GetValue();
            if (time < start || time > end) d.SetTime(UsdTimeCode(start));
            return Done();
        },
    });
    r.Register({
        .name = "reset_playback_range",
        .title = "Reset Playback Range",
        .description = "Play the stage's own time range again.",
        .menu = "Playback",
        .run = [](Document &d, const JsObject &) {
            d.ClearPlaybackRange();
            return Done();
        },
        .enabled = [](const Document &d) { return d.HasPlaybackRange(); },
    });
    r.Register({
        .name = "set_time",
        .title = "Set Time",
        .description = "Go to a time code. Times outside the stage's range "
                       "are allowed.",
        .parameters = ParameterSchema(R"({
            "properties": {"time": {"type": "number"}},
            "required": ["time"]
        })"),
        .run = [](Document &d, const JsObject &params) {
            d.SetTime(UsdTimeCode(Number(params.at("time"))));
            return Done();
        },
    });
}

} // namespace

void RegisterEditingActions(ActionRegistry &registry);
void RegisterViewActions(ActionRegistry &registry);
void RegisterFileActions(ActionRegistry &registry);

void RegisterStandardActions(ActionRegistry &registry) {
    RegisterFileActions(registry);
    RegisterEditingActions(registry);
    RegisterEdit(registry);
    RegisterView(registry);
    RegisterViewActions(registry);
    RegisterPlayback(registry);
}

} // namespace vela
