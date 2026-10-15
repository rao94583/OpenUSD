// usdview's command-line flags, for opening a file from a terminal:
//   Vela FILE [--select PATH] [--camera CAMERA] [--mask PATH[,PATH...]] ...
// Parsing and applying them is UI-independent; the app handles the flags
// that concern windows (see AppOptions).
#pragma once

#include "actions.h"
#include "app_settings.h"
#include "document.h"

#include "pxr/pxr.h"
#include "pxr/usd/sdf/path.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

struct LaunchOptions {
    std::string file;
    // The view settings the document starts with (the Settings window's
    // defaults), before --complexity and --renderer.
    ViewSettings view;
    // Look through the stage's primary camera (main_cam) when --camera is
    // not given.
    bool usePrimaryCamera = true;
    // Stage opening.
    std::vector<SdfPath> mask;          // --mask: population mask
    bool unloaded = false;              // --unloaded: load no payloads
    std::string sessionLayer;           // --sessionLayer FILE
    std::vector<std::string> mute;      // --mute LAYER[,LAYER...]
    // The initial view.
    SdfPath select;                     // --select PRIM: select and frame
    std::string camera;                 // --camera PATH or camera name
    std::optional<double> complexity;   // --complexity low|medium|high|veryhigh
    std::string renderer;               // --renderer NAME (e.g. GL, Embree)
    std::optional<double> firstFrame;   // --ff
    std::optional<double> lastFrame;    // --lf
    std::optional<double> currentFrame; // --cf
    // The app.
    int numThreads = 0;                 // --numThreads; 0 = default
    bool timing = false;                // --timing: print load times
    bool quitAfterStartup = false;      // --quitAfterStartup
    bool norender = false;              // --norender: no viewport drawing
    bool bboxStandin = false;           // --bboxStandin: unloaded prims as boxes
    int viewportWidth = 0;              // --viewportSize WxH
    int viewportHeight = 0;
    // Settings: --clearsettings resets the saved settings, and
    // --defaultsettings uses the defaults without saving changes (usdview's
    // meanings). main applies them before the other options are parsed.
    bool clearSettings = false;
    bool defaultSettings = false;
};

// The options a file opens with when no flags are given: the settings'
// view defaults and opening options. ParseLaunchOptions adds flags on top.
LaunchOptions DefaultLaunchOptions(const AppSettings &settings);

// Parses usdview's flags (arguments after the program name) into options,
// which start as DefaultLaunchOptions. Rejects flags Vela does not support. Returns false with a
// message in error.
bool ParseLaunchOptions(const std::vector<std::string> &args,
                        LaunchOptions *options, std::string *error);

// Opens the file with the stage options (mask, load, session layer,
// muting), or returns null with a message in error.
std::shared_ptr<Document> OpenDocument(const LaunchOptions &options,
                                       std::string *error);

// Applies the initial view: time range and frame, complexity, camera and
// the selection (framed). As in usdview, the camera defaults to the stage's
// primary camera ("main_cam") when there is one, a camera given by name
// matches the first camera prim with that name, and a missing camera or
// prim is a warning (returned), not an error.
std::vector<std::string> ApplyLaunchOptions(const LaunchOptions &options,
                                            Document &document);

// The opening options as form fields (File → Open with Options… and
// Reopen with Options…): load payloads, population mask, session layer and
// muted layers, showing options' values. Lists are comma-separated.
std::vector<FormField> OpeningFields(const LaunchOptions &options);
// Sets options from the form's values; false with a message in error for a
// prim path that is not absolute.
bool ApplyOpeningFields(const JsObject &values, LaunchOptions *options,
                        std::string *error);

// The flags, for usage messages.
const char *LaunchUsage();

} // namespace vela
