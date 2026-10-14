// Saving and exporting. Save writes the edited layers in place; the exports
// write new files and leave the open layers unchanged.
#include "actions.h"
#include "document.h"

#include "pxr/base/js/json.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/usd/sdf/fileFormat.h"
#include "pxr/usd/sdf/layer.h"

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
}

} // namespace vela
