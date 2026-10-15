// Saving and exporting. Save writes the edited layers in place; the exports
// write new files and leave the open layers unchanged.
#include "actions.h"
#include "document.h"

#include "pxr/base/js/json.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/usd/sdf/fileFormat.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/usd/stage.h"

#include <algorithm>

namespace vela {

namespace {

ActionResult Done() { return ActionResult(); }

// Collects USD errors raised while running f into an ActionResult.
template <class F>
ActionResult Checked(const std::string &what, F f) {
    TfErrorMark mark;
    const bool ok = f();
    if (ok && mark.IsClean()) return Done();
    std::string message = what;
    for (const TfError &error : mark) message += "\n" + error.GetCommentary();
    mark.Clear();
    return ActionResult::Error(message);
}

// A path USD can write: a known layer file format by extension.
bool Writable(const std::string &path, std::string *error) {
    if (!SdfFileFormat::FindByExtension(path)) {
        *error = "Not a USD file extension: " + path;
        return false;
    }
    return true;
}

JsValue PathParameter() {
    return JsValue(JsObject{{"type", JsValue("string")},
                            {"description", JsValue("File to write; its extension "
                                                    "(.usda, .usdc, .usd) picks the format")}});
}

} // namespace

void RegisterFileActions(ActionRegistry &r) {
    r.Register({
        .name = "save", .title = "Save",
        .description = "Write every edited layer of the stage's layer stack to "
                       "its file, except the session layer. In-memory layers "
                       "cannot be saved; export them instead. Returns the "
                       "identifiers of the saved layers.",
        .menu = "File", .key = "s", .modifiers = ModifierCommand,
        .separatorBefore = true,
        .run = [](Document &d, const JsObject &) {
            JsArray saved;
            for (const SdfLayerHandle &layer : d.GetLayersToSave()) {
                ActionResult result = Checked("Could not save " + layer->GetIdentifier(),
                                              [&] { return layer->Save(); });
                if (!result.ok) return result;
                saved.push_back(JsValue(layer->GetIdentifier()));
            }
            d.LayersSaved();
            return ActionResult{true, JsValue(JsObject{{"saved", JsValue(saved)}}), ""};
        },
        .enabled = [](const Document &d) { return !d.GetLayersToSave().empty(); },
    });
    r.Register({
        .name = "export_flattened", .title = "Export Flattened…",
        .description = "Write the composed stage, flattened into one layer, to a "
                       "new file. The open stage is unchanged.",
        .parameters = ParameterSchema(JsWriteToString(JsValue(JsObject{
            {"properties", JsValue(JsObject{{"path", PathParameter()}})},
            {"required", JsValue(JsArray{JsValue("path")})}}))),
        .menu = "File",
        .savePathParam = "path",
        .run = [](Document &d, const JsObject &p) {
            const std::string &path = p.at("path").GetString();
            std::string error;
            if (!Writable(path, &error)) return ActionResult::Error(error);
            return Checked("Could not export " + path,
                           [&] { return d.GetStage()->Export(path); });
        },
    });
    r.Register({
        .name = "export_session_layer", .title = "Export Session Layer…",
        .description = "Write the session layer (overrides made with the session "
                       "layer as edit target) to a new file, as usdview's Save "
                       "Overrides As.",
        .parameters = ParameterSchema(JsWriteToString(JsValue(JsObject{
            {"properties", JsValue(JsObject{{"path", PathParameter()}})},
            {"required", JsValue(JsArray{JsValue("path")})}}))),
        .menu = "File",
        .savePathParam = "path",
        .run = [](Document &d, const JsObject &p) {
            const std::string &path = p.at("path").GetString();
            std::string error;
            if (!Writable(path, &error)) return ActionResult::Error(error);
            return Checked("Could not export " + path,
                           [&] { return d.GetStage()->GetSessionLayer()->Export(path); });
        },
    });
    r.Register({
        .name = "revert", .title = "Revert to Saved",
        .description = "Discard unsaved edits: reload every edited layer of the "
                       "layer stack from its file and clear the undo history. "
                       "In-memory layers are left as they are.",
        .menu = "File",
        .run = [](Document &d, const JsObject &) {
            for (const SdfLayerHandle &layer : d.GetLayersToSave()) {
                ActionResult result = Checked("Could not reload " + layer->GetIdentifier(),
                                              [&] { return layer->Reload(/* force = */ true); });
                if (!result.ok) return result;
            }
            d.GetUndoStack().Clear();
            d.LayersSaved();
            return Done();
        },
        .enabled = [](const Document &d) { return !d.GetLayersToSave().empty(); },
    });
    r.Register({
        .name = "set_layer_muted", .title = "Muted Layers",
        .description = "Mute or unmute a layer by identifier (usdview's --mute): a "
                       "muted layer's opinions are ignored, without editing any "
                       "layer, and the stage recomposes. Toggles when muted is "
                       "omitted. The root and session layers cannot be muted.",
        .parameters = ParameterSchema(R"({
            "properties": {"layer": {"type": "string"}, "muted": {"type": "boolean"}},
            "required": ["layer"]
        })"),
        .menu = "File", .separatorBefore = true,
        .dynamicChoices = [](const Document &d) {
            // The sublayers, and muted layers (no longer in the stack).
            const UsdStageRefPtr &stage = d.GetStage();
            std::vector<std::string> identifiers;
            for (const SdfLayerHandle &layer : stage->GetLayerStack(false)) {
                if (layer != stage->GetRootLayer()) identifiers.push_back(layer->GetIdentifier());
            }
            for (const std::string &muted : stage->GetMutedLayers()) {
                if (std::find(identifiers.begin(), identifiers.end(), muted) == identifiers.end()) {
                    identifiers.push_back(muted);
                }
            }
            std::vector<MenuChoice> choices;
            for (const std::string &identifier : identifiers) {
                choices.push_back({TfGetBaseName(identifier),
                                   JsObject{{"layer", JsValue(identifier)}}});
            }
            return choices;
        },
        .chosen = [](const Document &d, const JsObject &p) {
            return d.GetStage()->IsLayerMuted(p.at("layer").GetString());
        },
        .run = [](Document &d, const JsObject &p) {
            const UsdStageRefPtr &stage = d.GetStage();
            const std::string &identifier = p.at("layer").GetString();
            if (identifier == stage->GetRootLayer()->GetIdentifier() ||
                identifier == stage->GetSessionLayer()->GetIdentifier()) {
                return ActionResult::Error("The root and session layers cannot be muted");
            }
            auto given = p.find("muted");
            const bool mute = given == p.end() ? !stage->IsLayerMuted(identifier)
                                               : given->second.GetBool();
            if (mute) stage->MuteLayer(identifier);
            else stage->UnmuteLayer(identifier);
            return Done();
        },
    });
}

} // namespace vela
