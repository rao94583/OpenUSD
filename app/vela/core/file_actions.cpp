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
#include <cmath>

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

std::string Extension(const std::string &path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos || path.find('/', dot) != std::string::npos) return std::string();
    return TfStringToLower(path.substr(dot + 1));
}

// Optional parameters with defaults. JSON integers parse as uint64 when
// non-negative.
double NumberOr(const JsObject &p, const char *key, double fallback) {
    auto it = p.find(key);
    if (it == p.end()) return fallback;
    const JsValue &v = it->second;
    return v.IsUInt64() ? double(v.GetUInt64()) : v.IsInt() ? double(v.GetInt64())
         : v.IsReal() ? v.GetReal() : fallback;
}
bool BoolOr(const JsObject &p, const char *key, bool fallback) {
    auto it = p.find(key);
    return it != p.end() && it->second.IsBool() ? it->second.GetBool() : fallback;
}
std::string StringOr(const JsObject &p, const char *key, const std::string &fallback) {
    auto it = p.find(key);
    return it != p.end() && it->second.IsString() ? it->second.GetString() : fallback;
}

// Image and movie sizes: a resolution preset, or width and height (0 for
// the viewport's), at most 8K; and the framing.
bool ExportSize(const JsObject &p, int *width, int *height, Framing *framing, std::string *error) {
    const std::string preset = StringOr(p, "resolution", "");
    if (!preset.empty() && preset != "viewport" && preset != "custom") {
        const ResolutionPreset *found = FindResolutionPreset(preset);
        if (!found) {
            std::vector<std::string> ids;
            for (const ResolutionPreset &r : ResolutionPresets()) ids.push_back(r.id);
            *error = "Unknown resolution '" + preset + "' (use " + TfStringJoin(ids, ", ") +
                     ", or custom with width and height)";
            return false;
        }
        *width = found->width;
        *height = found->height;
    } else {
        const double w = preset == "viewport" ? 0 : NumberOr(p, "width", 0);
        const double h = preset == "viewport" ? 0 : NumberOr(p, "height", 0);
        if (w < 0 || h < 0 || w > 8192 || h > 8192) {
            *error = "width and height must be between 1 and 8192 pixels (or 0 for the viewport's)";
            return false;
        }
        *width = int(w);
        *height = int(h);
    }
    if (!ParseFraming(StringOr(p, "framing", "fit"), framing)) {
        *error = "framing is fit (letterbox or pillarbox) or crop";
        return false;
    }
    return true;
}

// The resolution parameter's choices, for schemas and forms.
std::string ResolutionIds() {
    std::string ids;
    for (const ResolutionPreset &r : ResolutionPresets()) ids += "\"" + r.id + "\", ";
    return ids + "\"viewport\", \"custom\"";
}

// The movie form's resolution choices: the presets' labels, standing for
// their ids.
std::vector<std::string> ResolutionLabels() {
    std::vector<std::string> labels;
    for (const ResolutionPreset &r : ResolutionPresets()) labels.push_back(r.label);
    labels.push_back("The viewport's size");
    labels.push_back("Custom (the width and height below)");
    return labels;
}
std::vector<std::string> ResolutionValues() {
    std::vector<std::string> ids;
    for (const ResolutionPreset &r : ResolutionPresets()) ids.push_back(r.id);
    ids.push_back("viewport");
    ids.push_back("custom");
    return ids;
}

std::string SizeParameters() {
    return R"json(
    "resolution": {"type": "string", "enum": [)json" + ResolutionIds() + R"json(], "description": "A standard size, e.g. 1080p (1920 x 1080), 2160p, dci4k, scope (2.39:1), square, vertical (9:16); or viewport, or custom for width and height"},
    "width": {"type": "integer", "description": "Pixels, with custom or no resolution; 0 or omitted for the viewport's (with only height given, the camera's aspect ratio)"},
    "height": {"type": "integer", "description": "Pixels; 0 or omitted for the viewport's"},
    "framing": {"type": "string", "enum": ["fit", "crop"], "description": "When the size's aspect ratio differs from the camera's: fit (the default) letterboxes or pillarboxes with black bars to keep the camera's whole frame; crop fills the frame"},)json";
}

void RegisterExportActions(ActionRegistry &r) {
    r.Register({
        .name = "export_image", .title = "Export Image…",
        .description = "Write what the viewport shows (without the HUD) to an "
                       "image file, rendered offscreen at the size asked for, so "
                       "covering or minimizing the window doesn't matter. The "
                       "extension picks the format: png, jpg, tif, exr, bmp or "
                       "tga. EXR holds the renderer's linear color, without color "
                       "correction; the others hold what the viewport displays. "
                       "Returns the path, width and height.",
        .parameters = ParameterSchema(std::string(R"json({"properties": {
            "path": {"type": "string", "description": "File to write"},)json") + SizeParameters() + R"json(
            "showSelection": {"type": "boolean", "description": "Draw selection highlights and bounding boxes as the viewport does (default true)"}
        }, "required": ["path"]})json"),
        .menu = "File", .separatorBefore = true,
        .savePathParam = "path",
        .saveTypes = ImageExportExtensions(),
        .run = [](Document &d, const JsObject &p) {
            ImageExport request;
            request.path = p.at("path").GetString();
            const auto &formats = ImageExportExtensions();
            if (std::find(formats.begin(), formats.end(), Extension(request.path)) == formats.end()) {
                return ActionResult::Error("Not an image format Vela writes: " + request.path +
                    " (use " + TfStringJoin(formats, ", ") + ")");
            }
            std::string error;
            if (!ExportSize(p, &request.width, &request.height, &request.framing, &error)) {
                return ActionResult::Error(error);
            }
            request.showSelection = BoolOr(p, "showSelection", true);
            if (!d.GetViewportServices().exportImage) {
                return ActionResult::Error("No viewport to render: open the stage in a window");
            }
            if (!d.GetViewportServices().exportImage(request, &error)) {
                return ActionResult::Error("Could not export " + request.path +
                                           (error.empty() ? "" : ": " + error));
            }
            return ActionResult{true, JsValue(JsObject{
                {"path", JsValue(request.path)},
                {"width", JsValue(request.width)}, {"height", JsValue(request.height)}})};
        },
    });
    r.Register({
        .name = "export_movie", .title = "Export Movie…",
        .description = "Render frames start to end through the viewport's camera "
                       "(the scene camera it looks through, or the free camera) "
                       "into a QuickTime (.mov) or MPEG-4 (.mp4) movie. Frames "
                       "render offscreen, so covering or minimizing the window "
                       "doesn't matter. Codecs: h264 (default), hevc, prores422 "
                       "and prores4444 (.mov only). Defaults: the playback "
                       "range, the stage's frame rate and the viewport's size. "
                       "Returns the path, size and frame count.",
        .parameters = ParameterSchema(std::string(R"json({"properties": {
            "path": {"type": "string", "description": "File to write: .mov or .mp4"},
            "start": {"type": "number", "description": "First time code (default: the playback range's)"},
            "end": {"type": "number", "description": "Last time code, inclusive"},
            "framesPerSecond": {"type": "number", "description": "Default: the stage's"},)json") + SizeParameters() + R"json(
            "codec": {"type": "string", "enum": ["h264", "hevc", "prores422", "prores4444"]},
            "showSelection": {"type": "boolean", "description": "Draw selection highlights and bounding boxes (default false)"}
        }, "required": ["path"]})json"),
        .menu = "File",
        .savePathParam = "path",
        .saveTypes = {"mov", "mp4"},
        .form = [](const Document &d) {
            return std::vector<FormField>{
                {.key = "start", .label = "Start frame", .type = FormField::Type::Float,
                 .value = JsValue(d.GetStartTime())},
                {.key = "end", .label = "End frame", .type = FormField::Type::Float,
                 .value = JsValue(d.GetEndTime())},
                {.key = "framesPerSecond", .label = "Frames per second", .type = FormField::Type::Float,
                 .value = JsValue(d.GetFramesPerSecond())},
                {.key = "resolution", .label = "Resolution", .type = FormField::Type::Choice,
                 .value = JsValue("1080p"), .choices = ResolutionLabels(),
                 .choiceValues = ResolutionValues()},
                {.key = "width", .label = "Custom width (0: the viewport's)", .type = FormField::Type::Int,
                 .value = JsValue(1920)},
                {.key = "height", .label = "Custom height (0: the camera's aspect)", .type = FormField::Type::Int,
                 .value = JsValue(0)},
                {.key = "framing", .label = "When the camera's shape differs", .type = FormField::Type::Choice,
                 .value = JsValue("fit"),
                 .choices = {"Letterbox or pillarbox (keep the whole frame)", "Crop to fill"},
                 .choiceValues = {"fit", "crop"}},
                {.key = "codec", .label = "Codec", .type = FormField::Type::Choice,
                 .value = JsValue("h264"),
                 .choices = {"H.264", "HEVC (H.265)", "ProRes 422 (.mov)", "ProRes 4444 (.mov)"},
                 .choiceValues = {"h264", "hevc", "prores422", "prores4444"}},
                {.key = "showSelection", .label = "Show the selection", .type = FormField::Type::Bool,
                 .value = JsValue(false)},
            };
        },
        .run = [](Document &d, const JsObject &p) {
            MovieExport request;
            request.path = p.at("path").GetString();
            const std::string extension = Extension(request.path);
            if (extension != "mov" && extension != "mp4") {
                return ActionResult::Error("Movies are .mov or .mp4: " + request.path);
            }
            std::string error;
            if (!ExportSize(p, &request.width, &request.height, &request.framing, &error)) {
                return ActionResult::Error(error);
            }
            request.start = NumberOr(p, "start", d.GetStartTime());
            request.end = NumberOr(p, "end", d.GetEndTime());
            request.framesPerSecond = NumberOr(p, "framesPerSecond", d.GetFramesPerSecond());
            request.codec = StringOr(p, "codec", "h264");
            request.showSelection = BoolOr(p, "showSelection", false);
            if (!(request.end >= request.start)) {
                return ActionResult::Error("The end frame comes before the start");
            }
            if (!(request.framesPerSecond > 0 && request.framesPerSecond <= 240)) {
                return ActionResult::Error("framesPerSecond must be between 0 and 240");
            }
            if (request.end - request.start > 100000) {
                return ActionResult::Error("At most 100,000 frames");
            }
            if (extension == "mp4" && request.codec.rfind("prores", 0) == 0) {
                return ActionResult::Error("ProRes movies must be .mov");
            }
            if (!d.GetViewportServices().exportMovie) {
                return ActionResult::Error("No viewport to render: open the stage in a window");
            }
            if (!d.GetViewportServices().exportMovie(request, &error)) {
                return ActionResult::Error("Could not export " + request.path +
                                           (error.empty() ? "" : ": " + error));
            }
            const int frames = int(std::floor(request.end - request.start)) + 1;
            return ActionResult{true, JsValue(JsObject{
                {"path", JsValue(request.path)},
                {"width", JsValue(request.width)}, {"height", JsValue(request.height)},
                {"frames", JsValue(frames)}})};
        },
    });
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
        .name = "replace_session_layer", .title = "Replace Session Layer…",
        .description = "Replace the session layer's contents with a USD file's "
                       "(usdview's --sessionLayer, on the open stage). The file "
                       "is not changed; Export Session Layer… writes the result.",
        .parameters = ParameterSchema(R"({
            "properties": {"path": {"type": "string"}},
            "required": ["path"]
        })"),
        .menu = "File",
        .openPathParam = "path",
        .run = [](Document &d, const JsObject &p) {
            const std::string &path = p.at("path").GetString();
            TfErrorMark mark;
            SdfLayerRefPtr source = SdfLayer::FindOrOpen(path);
            if (!source) {
                std::string error = "Could not open " + path;
                for (const TfError &e : mark) error += "\n" + e.GetCommentary();
                mark.Clear();
                return ActionResult::Error(error);
            }
            d.GetStage()->GetSessionLayer()->TransferContent(source);
            return Done();
        },
    });
    r.Register({
        .name = "validate", .title = "Validate Stage",
        .description = "Check the stage with USD's validators, as usdchecker does "
                       "(all of them, or those with any of the given keywords, "
                       "e.g. [\"UsdGeomValidators\"]). Returns the counts and "
                       "the issues (severity, validator, message, sites); the "
                       "Validation panel (Window → Validation) shows them.",
        .parameters = ParameterSchema(R"({"properties": {
            "keywords": {"type": "array", "items": {"type": "string"}}}})"),
        .run = [](Document &d, const JsObject &p) {
            std::vector<std::string> keywords;
            if (auto found = p.find("keywords"); found != p.end()) {
                for (const JsValue &keyword : found->second.GetJsArray()) {
                    keywords.push_back(keyword.GetString());
                }
            }
            d.SetValidationIssues(RunValidation(d.GetStage(), keywords));
            int counts[3] = {0, 0, 0};
            for (const ValidationIssue &issue : d.GetValidationIssues()) {
                ++counts[static_cast<int>(issue.severity)];
            }
            return ActionResult{true, JsValue(JsObject{
                {"errors", JsValue(counts[0])}, {"warnings", JsValue(counts[1])},
                {"info", JsValue(counts[2])},
                {"issues", ToJson(d.GetValidationIssues())}}), {}};
        },
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
    RegisterExportActions(r);
}

} // namespace vela
