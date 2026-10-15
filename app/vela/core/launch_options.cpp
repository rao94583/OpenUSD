#include "launch_options.h"

#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/work/threadLimits.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/stagePopulationMask.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usd/usdUtils/pipeline.h"

#include <algorithm>
#include <cstdlib>

namespace vela {

namespace {

bool ParseNumber(const std::string &text, double *value) {
    char *end = nullptr;
    *value = std::strtod(text.c_str(), &end);
    return !text.empty() && end && *end == '\0';
}

// usdview's --camera: an absolute path, or a camera prim's name.
SdfPath FindCamera(const UsdStageRefPtr &stage, const std::string &camera) {
    if (camera.empty() || !SdfPath::IsValidPathString(camera)) return SdfPath();
    const SdfPath path(camera);
    if (path.IsAbsolutePath()) {
        return UsdGeomCamera(stage->GetPrimAtPath(path)) ? path : SdfPath();
    }
    if (path.GetPathElementCount() > 1) {
        const SdfPath absolute = path.MakeAbsolutePath(SdfPath::AbsoluteRootPath());
        return UsdGeomCamera(stage->GetPrimAtPath(absolute)) ? absolute : SdfPath();
    }
    for (const UsdPrim &prim : UsdPrimRange::Stage(stage, UsdTraverseInstanceProxies())) {
        if (prim.GetName() == path.GetNameToken() && prim.IsA<UsdGeomCamera>()) {
            return prim.GetPath();
        }
    }
    return SdfPath();
}

} // namespace

const char *LaunchUsage() {
    return
        "Usage: Vela [FILE] [usdview's options]\n"
        "  --select PRIM            select and frame a prim\n"
        "  --camera CAMERA          camera path or name (default: main_cam)\n"
        "  --mask PRIM[,PRIM...]    populate only these prims\n"
        "  --unloaded               do not load payloads\n"
        "  --sessionLayer FILE      use FILE as the session layer\n"
        "  --mute LAYER[,LAYER...]  mute layers\n"
        "  --complexity LEVEL       low, medium, high or veryhigh\n"
        "  --renderer NAME          Hydra renderer, e.g. GL (Storm) or Embree\n"
        "  --clearsettings          reset the saved settings\n"
        "  --defaultsettings        use default settings, without saving changes\n"
        "  --ff/--lf FRAME          first and last frame of the playback range\n"
        "  --cf FRAME               the current frame\n"
        "  --numThreads N           thread limit (0: all, negative: all but N)\n"
        "  --viewportSize WxH       initial viewport size\n"
        "  --norender               do not draw the viewport\n"
        "  --bboxStandin            draw unloaded prims as bounding boxes\n"
        "  --timing                 print load and first-draw times\n"
        "  --quitAfterStartup       quit after drawing the first frame\n";
}

bool ParseLaunchOptions(const std::vector<std::string> &args,
                        LaunchOptions *options, std::string *error) {
    // usdview flags with no meaning in Vela yet (they configure usdview's
    // settings files or Python plugins), accepted so scripts keep working.
    // Accepted so scripts keep working: usdview's plugins and settings files
    // (Vela's settings persist on their own).
    static const std::vector<std::string> ignored{"--noplugins"};
    static const std::vector<std::string> ignoredWithValue{"--config"};
    static const std::vector<std::string> unsupported{
        "--allow-async", "--traceToFile", "--traceFormat", "--tracePython",
        "--memstats", "--dumpFirstImage", "--detachLayers",
        "--detachLayersInclude", "--detachLayersExclude"};
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string &arg = args[i];
        auto value = [&](std::string *out) {
            if (i + 1 >= args.size()) {
                *error = arg + " needs a value";
                return false;
            }
            *out = args[++i];
            return true;
        };
        auto number = [&](std::optional<double> *out) {
            std::string text;
            double n = 0;
            if (!value(&text)) return false;
            if (!ParseNumber(text, &n)) {
                *error = arg + " needs a number, not " + text;
                return false;
            }
            *out = n;
            return true;
        };
        std::string text;
        if (arg == "--select") {
            if (!value(&text)) return false;
            if (!SdfPath::IsValidPathString(text) || !SdfPath(text).IsAbsolutePath()) {
                *error = "--select needs an absolute prim path, not " + text;
                return false;
            }
            options->select = SdfPath(text);
        } else if (arg == "--camera" || arg == "-cam") {
            if (!value(&options->camera)) return false;
        } else if (arg == "--mask") {
            if (!value(&text)) return false;
            for (const std::string &path : TfStringSplit(text, ",")) {
                if (!SdfPath::IsValidPathString(path) || !SdfPath(path).IsAbsolutePath()) {
                    *error = "--mask needs absolute prim paths, not " + path;
                    return false;
                }
                options->mask.emplace_back(path);
            }
        } else if (arg == "--unloaded") {
            options->unloaded = true;
        } else if (arg == "--sessionLayer") {
            if (!value(&options->sessionLayer)) return false;
        } else if (arg == "--mute") {
            if (!value(&text)) return false;
            for (const std::string &layer : TfStringSplit(text, ",")) {
                options->mute.push_back(layer);
            }
        } else if (arg == "--complexity" || arg == "-c") {
            double complexity = 1.0;
            if (!value(&text)) return false;
            if (!ParseComplexity(text, &complexity)) {
                *error = "--complexity is low, medium, high or veryhigh, not " + text;
                return false;
            }
            options->complexity = complexity;
        } else if (arg == "--clearsettings") {
            options->clearSettings = true;
        } else if (arg == "--defaultsettings") {
            options->defaultSettings = true;
        } else if (arg == "--renderer" || arg == "-r") {
            if (!value(&options->renderer)) return false;
        } else if (arg == "--ff") {
            if (!number(&options->firstFrame)) return false;
        } else if (arg == "--lf") {
            if (!number(&options->lastFrame)) return false;
        } else if (arg == "--cf") {
            if (!number(&options->currentFrame)) return false;
        } else if (arg == "--numThreads") {
            std::optional<double> n;
            if (!number(&n)) return false;
            options->numThreads = int(*n);
        } else if (arg == "--viewportSize") {
            if (!value(&text)) return false;
            const auto size = TfStringSplit(text, "x");
            double w = 0, h = 0;
            if (size.size() != 2 || !ParseNumber(size[0], &w) || !ParseNumber(size[1], &h) ||
                w < 1 || h < 1) {
                *error = "--viewportSize is WIDTHxHEIGHT, not " + text;
                return false;
            }
            options->viewportWidth = int(w);
            options->viewportHeight = int(h);
        } else if (arg == "--norender") {
            options->norender = true;
        } else if (arg == "--bboxStandin") {
            options->bboxStandin = true;
        } else if (arg == "--timing") {
            options->timing = true;
        } else if (arg == "--quitAfterStartup") {
            options->quitAfterStartup = true;
        } else if (std::find(ignored.begin(), ignored.end(), arg) != ignored.end()) {
        } else if (std::find(ignoredWithValue.begin(), ignoredWithValue.end(), arg) !=
                   ignoredWithValue.end()) {
            if (!value(&text)) return false;
        } else if (std::find(unsupported.begin(), unsupported.end(), arg) != unsupported.end()) {
            *error = arg + " is not supported by Vela";
            return false;
        } else if (arg.rfind("-", 0) == 0) {
            *error = "Unknown option " + arg;
            return false;
        } else if (options->file.empty()) {
            options->file = arg;
        } else {
            *error = "Only one file can be opened: " + arg;
            return false;
        }
    }
    if (options->firstFrame && options->lastFrame &&
        *options->lastFrame < *options->firstFrame) {
        *error = "--lf is before --ff";
        return false;
    }
    return true;
}

LaunchOptions DefaultLaunchOptions(const AppSettings &settings) {
    LaunchOptions options;
    options.view = AsDefaults(settings.view);
    options.usePrimaryCamera = settings.usePrimaryCamera;
    options.unloaded = !settings.loadPayloads;
    return options;
}

std::shared_ptr<Document> OpenDocument(const LaunchOptions &options,
                                       std::string *error) {
    if (options.numThreads != 0) {
        WorkSetConcurrencyLimitArgument(options.numThreads);
    }
    TfErrorMark mark;
    SdfLayerRefPtr root = SdfLayer::FindOrOpen(options.file);
    SdfLayerRefPtr session;
    if (root && !options.sessionLayer.empty()) {
        session = SdfLayer::FindOrOpen(options.sessionLayer);
    }
    const UsdStage::InitialLoadSet load =
        options.unloaded ? UsdStage::LoadNone : UsdStage::LoadAll;
    UsdStageRefPtr stage;
    if (root && (options.sessionLayer.empty() || session)) {
        if (!options.mask.empty()) {
            UsdStagePopulationMask mask;
            for (const SdfPath &path : options.mask) mask.Add(path);
            stage = session ? UsdStage::OpenMasked(root, session, mask, load)
                            : UsdStage::OpenMasked(root, mask, load);
        } else {
            stage = session ? UsdStage::Open(root, session, load)
                            : UsdStage::Open(root, load);
        }
    }
    if (!stage) {
        if (error) {
            *error = "Could not open USD stage: " +
                (root ? options.sessionLayer : options.file);
            for (const TfError &e : mark) *error += "\n" + e.GetCommentary();
        }
        mark.Clear();
        return nullptr;
    }
    if (!options.mute.empty()) {
        stage->MuteAndUnmuteLayers(options.mute, {});
    }
    return std::make_shared<Document>(stage);
}

std::vector<std::string> ApplyLaunchOptions(const LaunchOptions &options,
                                            Document &document) {
    std::vector<std::string> warnings;
    const UsdStageRefPtr &stage = document.GetStage();
    if (options.firstFrame || options.lastFrame) {
        document.SetPlaybackRange(options.firstFrame.value_or(document.GetStartTime()),
                                  options.lastFrame.value_or(document.GetEndTime()));
        document.SetTime(UsdTimeCode(document.GetStartTime()));
    }
    if (options.currentFrame) {
        document.SetTime(UsdTimeCode(*options.currentFrame));
    }
    ViewSettings settings = options.view;
    if (options.complexity) settings.complexity = *options.complexity;
    if (options.bboxStandin) settings.unloadedAsBounds = true;
    if (options.norender) settings.pauseDrawing = true;
    // Resolved (and warned about if unknown) by the viewport's renderer.
    if (!options.renderer.empty()) settings.renderer = options.renderer;
    document.SetViewSettings(settings);
    std::string camera = options.camera;
    if (camera.empty() && options.usePrimaryCamera) {
        camera = UsdUtilsGetPrimaryCameraName().GetString();
    }
    const SdfPath cameraPath = camera.empty() ? SdfPath() : FindCamera(stage, camera);
    if (!cameraPath.IsEmpty()) {
        document.SetViewCamera(cameraPath);
    } else if (!options.camera.empty()) {
        warnings.push_back("No camera " + options.camera + "; using the free camera");
    }
    if (!options.select.IsEmpty()) {
        if (stage->GetPrimAtPath(options.select)) {
            document.SetSelection({options.select});
            // Framing detaches from a scene camera, as in usdview.
            document.FrameSelection();
        } else {
            warnings.push_back("No prim at " + options.select.GetString() + " to select");
        }
    }
    return warnings;
}

} // namespace vela
