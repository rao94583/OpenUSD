#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#import "document.h"
#import "browser_view.h"
#import "form_sheet.h"
#import "menus.h"
#import "claude_panel.h"
#import "image_generation.h"
#import "palette.h"
#import "scripting.h"
#import "settings_window.h"
#import "color_coding.h"
#import "log_view.h"
#import "viewport/viewport_view.h"
#include "app/vela/core/actions.h"
#include "app/vela/core/app_settings.h"
#include "app/vela/core/launch_options.h"
#include "app/vela/core/document.h"
#include "app/vela/core/free_camera.h"
#include "app/vela/core/mcp.h"
#include "app/vela/core/python.h"
#include "app/vela/core/session.h"
#include "pxr/base/js/json.h"
#include "app/vela/viewport/renderer.h"
#include "pxr/base/plug/registry.h"
#include "pxr/base/work/threadLimits.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usdGeom/bboxCache.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/tokens.h"
#include "pxr/usd/usdRender/pass.h"
#include "pxr/usd/usdRender/settings.h"
#include "pxr/usd/usdRender/tokens.h"

PXR_NAMESPACE_USING_DIRECTIVE

@interface VelaDelegate : NSObject <NSApplicationDelegate>
@end
@implementation VelaDelegate {
    NSURL *_launchFile;
}
- (instancetype)initWithFile:(NSURL *)file {
    if ((self = [super init])) _launchFile = file;
    return self;
}
// File → Open with Options…: a file, then usdview's stage options.
- (void)openWithOptions:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"usda"],
                                  [UTType typeWithFilenameExtension:@"usdc"],
                                  [UTType typeWithFilenameExtension:@"usd"],
                                  [UTType typeWithFilenameExtension:@"usdz"]];
    if ([panel runModal] != NSModalResponseOK) return;
    NSURL *url = panel.URL;
    const vela::LaunchOptions defaults =
        vela::DefaultLaunchOptions(vela::AppSettingsStore::Get().GetSettings());
    VelaRunForm(nil, [NSString stringWithFormat:@"Open %@", url.lastPathComponent],
                vela::OpeningFields(defaults), ^(const JsObject &values) {
        vela::LaunchOptions options = defaults;
        std::string error;
        if (!vela::ApplyOpeningFields(values, &options, &error)) {
            NSAlert *alert = [NSAlert new];
            alert.messageText = @(error.c_str());
            [alert runModal];
            return;
        }
        VelaOpenWithOptions(url, options);
    });
}

// Help → Color Legend.
- (void)showColorLegend:(id)sender {
    [[VelaLegendWindowController shared] showWindow:sender];
}

// Vela → Settings… (the window is made on first use).
- (void)showSettings:(id)sender {
    [[VelaSettingsWindowController shared] showWindow:sender];
}
- (BOOL)applicationShouldOpenUntitledFile:(NSApplication *)sender { return NO; }
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    [NSApp activateIgnoringOtherApps:YES];
    [VelaPaletteController installShortcut];
    VelaStartScripting();
    if (_launchFile) {
        [NSDocumentController.sharedDocumentController openDocumentWithContentsOfURL:_launchFile
            display:YES completionHandler:^(NSDocument *document, BOOL, NSError *error) {
            if (!document) {
                fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
                [NSApp terminate:nil];
            }
        }];
    }
}
@end


// Options shared by the command-line render modes.
struct RenderOptions {
    std::string scene;
    std::string output;
    SdfPath camera;
    UsdTimeCode time = UsdTimeCode::EarliestTime();
    int width = 960;
    int height = 0;
    int frames = 240;
    bool window = false;
    // Render through the camera as the viewport does (fitted, by matrices)
    // rather than by Hydra camera path as usdrecord does.
    bool viewCamera = false;
    // usdrecord's defaults: no dome light textures in the background.
    vela::ViewSettings settings = [] {
        vela::ViewSettings s;
        s.domeLightTexturesVisible = false;
        return s;
    }();
};

// usdrecord's --purposes: a comma-separated list of proxy, render, guide.
static bool ParsePurposes(const std::string &list, vela::ViewSettings *settings) {
    settings->showProxy = settings->showRender = settings->showGuides = false;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t end = std::min(list.find(',', start), list.size());
        const std::string purpose = list.substr(start, end - start);
        if (purpose == "proxy") settings->showProxy = true;
        else if (purpose == "render") settings->showRender = true;
        else if (purpose == "guide") settings->showGuides = true;
        else if (!purpose.empty()) return false;
        start = end + 1;
    }
    return true;
}

static bool ParseRenderOptions(int argc, const char *argv[], int first, RenderOptions *options) {
    for (int i = first; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--camera" && hasValue) options->camera = SdfPath(argv[++i]);
        else if (arg == "--viewCamera" && hasValue) {
            options->camera = SdfPath(argv[++i]);
            options->viewCamera = true;
        }
        else if (arg == "--frame" && hasValue) options->time = UsdTimeCode(std::atof(argv[++i]));
        else if (arg == "--width" && hasValue) options->width = std::atoi(argv[++i]);
        else if (arg == "--height" && hasValue) options->height = std::atoi(argv[++i]);
        else if (arg == "--frames" && hasValue) options->frames = std::atoi(argv[++i]);
        else if (arg == "--window") options->window = true;
        // View settings, with usdrecord's flag names where it has them.
        else if (arg == "--purposes" && hasValue) {
            if (!ParsePurposes(argv[++i], &options->settings)) return false;
        } else if (arg == "--complexity" && hasValue) {
            if (!vela::ParseComplexity(argv[++i], &options->settings.complexity)) return false;
        } else if (arg == "--drawMode" && hasValue) {
            if (!vela::Parse(argv[++i], &options->settings.drawMode)) return false;
        } else if (arg == "--colorCorrectionMode" && hasValue) {
            if (!vela::Parse(argv[++i], &options->settings.colorCorrection)) return false;
        } else if (arg == "--disableCameraLight") options->settings.cameraLight = false;
        else if (arg == "--enableDomeLightVisibility") options->settings.domeLightTexturesVisible = true;
        else if ((arg == "--renderer" || arg == "-r") && hasValue) {
            options->settings.renderer = argv[++i];
            if (vela::Renderer::ResolveRenderer(options->settings.renderer).empty()) {
                fprintf(stderr, "Unknown renderer %s\n", argv[i]);
                return false;
            }
        } else if ((arg == "--renderSettingsPrimPath" || arg == "-rs") && hasValue) {
            options->settings.renderSettingsPrim = SdfPath(argv[++i]);
        } else if ((arg == "--renderPassPrimPath" || arg == "-rp") && hasValue) {
            options->settings.renderPassPrim = SdfPath(argv[++i]);
        } else if (arg == "--aov" && hasValue) options->settings.aov = argv[++i];
        else if (arg.rfind("--", 0) == 0) return false;
        else if (options->scene.empty()) options->scene = arg;
        else if (options->output.empty()) options->output = arg;
        else return false;
    }
    return !options->scene.empty() && options->width > 0;
}

// The view a command-line render uses: the scene camera, sized from its
// aspect ratio as usdrecord does, or the free camera framing the stage.
static bool MakeView(const UsdStageRefPtr &stage, const RenderOptions &options,
                     vela::FreeCamera *freeCamera, vela::View *view) {
    view->time = options.time;
    // Snapshots match usdrecord's defaults.
    view->settings = options.settings;
    UsdGeomBBoxCache cache(options.time, {UsdGeomTokens->default_, UsdGeomTokens->proxy}, true);
    const GfBBox3d bounds = cache.ComputeWorldBound(stage->GetPseudoRoot());
    if (!options.camera.IsEmpty()) {
        UsdGeomCamera camera(stage->GetPrimAtPath(options.camera));
        if (!camera) {
            fprintf(stderr, "No camera at %s\n", options.camera.GetText());
            return false;
        }
        view->camera = camera.GetCamera(options.time);
        view->cameraPath = options.camera;
        float aspect = view->camera.GetAspectRatio();
        if (GfIsClose(aspect, 0.0f, 1e-4)) aspect = 1.0f;
        const int height = options.height > 0 ? options.height
                                              : std::max(1, int(float(options.width) / aspect));
        view->size = GfVec2i(options.width, height);
        if (options.viewCamera) {
            vela::Document document(stage);
            document.SetTime(options.time);
            document.SetViewCamera(options.camera);
            view->camera = document.ComputeViewGfCamera(double(options.width) / height);
            view->cameraPath = SdfPath();
        }
        return true;
    }
    const int height = options.height > 0 ? options.height : options.width * 9 / 16;
    view->size = GfVec2i(options.width, height);
    *freeCamera = vela::FreeCamera(UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->z);
    freeCamera->Frame(bounds, 1.1, double(options.width) / height);
    view->camera = freeCamera->ComputeGfCamera(double(options.width) / height, bounds);
    return true;
}

// Writes the image the viewport would show, for comparison with usdrecord.
static int Snapshot(const RenderOptions &givenOptions) {
    RenderOptions options = givenOptions;
    UsdStageRefPtr stage = UsdStage::Open(options.scene);
    if (!stage || options.output.empty()) {
        fprintf(stderr, "Could not open %s\n", options.scene.c_str());
        return 1;
    }
    // usdrecord's render settings: from a render pass's render source, the
    // flag, or (in the renderer) the stage's metadata; the camera from the
    // settings when none is given.
    vela::ViewSettings &settings = options.settings;
    if (!settings.renderPassPrim.IsEmpty()) {
        if (!settings.renderSettingsPrim.IsEmpty()) {
            fprintf(stderr, "Cannot specify both --renderSettingsPrimPath and "
                            "--renderPassPrimPath\n");
            return 1;
        }
        if (!UsdRenderPass(stage->GetPrimAtPath(settings.renderPassPrim))) {
            fprintf(stderr, "Unknown render pass <%s>\n", settings.renderPassPrim.GetText());
            return 1;
        }
        settings.renderSettingsPrim = vela::RenderPassSource(stage, settings.renderPassPrim);
        if (settings.renderSettingsPrim.IsEmpty()) {
            fprintf(stderr, "Render source not authored on %s\n",
                    settings.renderPassPrim.GetText());
            return 1;
        }
    }
    SdfPath settingsPrim = settings.renderSettingsPrim;
    if (settingsPrim.IsEmpty()) {
        std::string path;
        if (stage->GetMetadata(UsdRenderTokens->renderSettingsPrimPath, &path) &&
            SdfPath::IsValidPathString(path)) {
            settingsPrim = SdfPath(path);
        }
    }
    if (options.camera.IsEmpty() && !settingsPrim.IsEmpty()) {
        SdfPathVector cameras;
        if (UsdRenderSettings rs{stage->GetPrimAtPath(settingsPrim)}) {
            rs.GetCameraRel().GetForwardedTargets(&cameras);
        }
        if (cameras.size() == 1) options.camera = cameras.front();
    }
    vela::FreeCamera freeCamera;
    vela::View view;
    if (!MakeView(stage, options, &freeCamera, &view)) {
        return 1;
    }
    vela::Renderer renderer;
    renderer.SetStage(stage);
    if (!renderer.WriteImage(view, options.output)) {
        fprintf(stderr, "Could not write %s\n", options.output.c_str());
        return 1;
    }
    return 0;
}

// Tumbles once around the stage, drawing each frame as the viewport does,
// and reports the time to the first image and the interactive frame rate.
static int Benchmark(const RenderOptions &options) {
    using Clock = std::chrono::steady_clock;
    const auto openStart = Clock::now();
    UsdStageRefPtr stage = UsdStage::Open(options.scene);
    if (!stage) {
        fprintf(stderr, "Could not open %s\n", options.scene.c_str());
        return 1;
    }
    RenderOptions freeOptions = options;
    freeOptions.camera = SdfPath();
    vela::FreeCamera camera;
    vela::View view;
    MakeView(stage, freeOptions, &camera, &view);
    UsdGeomBBoxCache cache(options.time, {UsdGeomTokens->default_, UsdGeomTokens->proxy}, true);
    const GfBBox3d bounds = cache.ComputeWorldBound(stage->GetPseudoRoot());
    view.settings = vela::ViewSettings();
    view.clearColor = vela::BackgroundColor(view.settings.background);

    vela::Renderer renderer;
    renderer.SetStage(stage);
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:view.size[0] height:view.size[1] mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget;
    descriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> target = [renderer.GetDevice() newTextureWithDescriptor:descriptor];
    auto drawFrame = [&]() {
        @autoreleasepool {
            id<MTLCommandBuffer> commandBuffer = [renderer.GetQueue() commandBuffer];
            renderer.Draw(view, target, commandBuffer);
            [commandBuffer commit];
            [commandBuffer waitUntilCompleted];
        }
    };
    drawFrame();
    const double firstSeconds =
        std::chrono::duration<double>(Clock::now() - openStart).count();
    const auto start = Clock::now();
    for (int i = 0; i < options.frames; ++i) {
        camera.Tumble(360.0 / options.frames, 0.0);
        view.camera = camera.ComputeGfCamera(double(view.size[0]) / view.size[1], bounds);
        drawFrame();
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    printf("Opened and drew the first frame in %.2f s\n", firstSeconds);
    printf("%d frames at %dx%d: %.1f fps (%.2f ms per frame)\n", options.frames,
           view.size[0], view.size[1], options.frames / seconds,
           1000.0 * seconds / options.frames);
    return 0;
}

// The same tumble through a document window: the CAMetalLayer, display
// link and presentation included. Frames are capped at the display rate.
// Stops [NSApp run] from outside an event: stop: only takes effect after
// the next event, so post one.
static void StopApplication() {
    [NSApp stop:nil];
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
        location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0
        context:nil subtype:0 data1:0 data2:0] atStart:YES];
}

static int WindowBenchmark(const RenderOptions &options) {
    // AppKit opens command-line arguments as documents: here they are
    // options, and the window shouldn't restore earlier documents either.
    [NSUserDefaults.standardUserDefaults setVolatileDomain:@{
        @"NSTreatUnknownArgumentsAsOpen": @"NO",
        @"ApplePersistenceIgnoreState": @"YES",
    } forName:NSArgumentDomain];
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSURL *url = [NSURL fileURLWithPath:@(options.scene.c_str())];
    __block int status = 1;
    [NSDocumentController.sharedDocumentController openDocumentWithContentsOfURL:url
        display:YES completionHandler:^(NSDocument *document, BOOL, NSError *error) {
        if (!document) {
            fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
            StopApplication();
            return;
        }
        VelaViewportView *viewport = ((VelaDocument *)document).viewport;
        // Let the first frame (shader compilation) finish before timing.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            [viewport benchmarkFrames:options.frames completion:^(double seconds) {
                const NSSize size = [viewport convertSizeToBacking:viewport.bounds.size];
                printf("%d presented frames at %.0fx%.0f: %.1f fps (%.2f ms per frame)\n",
                       options.frames, size.width, size.height, options.frames / seconds,
                       1000.0 * seconds / options.frames);
                fflush(stdout);
                status = 0;
                StopApplication();
            }];
        });
    }];
    [NSApp run];
    return status;
}

// The embedded interpreter against a document, as the console and MCP use
// it: the vela module sees the document's own stage, edits are one undo
// step, and errors come back as tracebacks.
static int CheckPython(const char *file) {
    std::string error;
    auto core = vela::Document::Open(file, &error);
    if (!core) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
    vela::SessionDocument entry;
    entry.name = "check";
    entry.document = core;
    vela::Session::Get().Add(entry);
    if (!VelaStartPython()) return 1;
    int failures = 0;
    auto check = [&](bool ok, const std::string &what) {
        if (!ok) { fprintf(stderr, "FAILED: %s\n", what.c_str()); ++failures; }
    };
    auto run = [&](const std::string &code) {
        vela::PythonResult result = vela::RunPython(code, "check");
        if (!result.ok) fprintf(stderr, "%s", result.error.c_str());
        return result;
    };
    const std::string root = core->GetStage()->GetRootLayer()->GetIdentifier();
    vela::PythonResult r = run("from pxr import Usd\nvela.stage.GetRootLayer().identifier");
    check(r.ok && r.value == "'" + root + "'", "vela.stage is the document's stage: " + r.value);
    r = run("print('hello')\nlen(vela.documents())");
    check(r.ok && r.output == "hello\n" && r.value == "1", "output and value: " + r.output + r.value);
    // Edits through the USD API are one undo step.
    r = run("UsdGeom.Xform.Define(vela.stage, '/FromPython')\n"
            "vela.stage.GetPrimAtPath('/FromPython').CreateAttribute('n', Sdf.ValueTypeNames.Int).Set(3)");
    check(r.ok && core->GetStage()->GetPrimAtPath(SdfPath("/FromPython")), "define a prim");
    check(core->GetUndoStack().GetUndoName() == "Python", "undo step name: " +
          core->GetUndoStack().GetUndoName());
    r = run("vela.selection = ['/FromPython']\n[str(p) for p in vela.selection]");
    check(r.ok && core->GetSelection() == SdfPathVector{SdfPath("/FromPython")} &&
          r.value == "['/FromPython']", "selection: " + r.value);
    r = run("vela.run('get_settings') is not None");
    check(r.ok && r.value == "True", "vela.run: " + r.value);
    vela::ActionRegistry::Get().Run(*core, "undo");
    check(!core->GetStage()->GetPrimAtPath(SdfPath("/FromPython")), "undo the script's edits");
    r = vela::RunPython("1/0", "check");
    check(!r.ok && r.error.find("ZeroDivisionError") != std::string::npos &&
          r.error.find("<vela>") == std::string::npos, "traceback: " + r.error);
    r = vela::RunPython("vela.run('no_such_action')", "check");
    check(!r.ok && r.error.find("ActionError") != std::string::npos, "action error: " + r.error);
    check(!vela::IsCompletePython("for i in range(3):") && vela::IsCompletePython("x = 1"),
          "completeness");
    // The same through MCP's run_python.
    const std::string reply = vela::mcp::Handle(
        R"j({"jsonrpc": "2.0", "id": 7, "method": "tools/call", "params": {"name": "run_python",)j"
        R"j( "arguments": {"code": "len(list(vela.stage.Traverse()))"}}})j");
    const JsValue parsed = JsParseString(reply);
    std::string text;
    if (parsed.IsObject() && parsed.GetJsObject().count("result")) {
        const JsObject &result = parsed.GetJsObject().at("result").GetJsObject();
        text = result.at("content").GetJsArray()[0].GetJsObject().at("text").GetString();
    }
    size_t prims = 0;
    for (const auto &prim : core->GetStage()->Traverse()) { (void)prim; ++prims; }
    check(text == std::to_string(prims), "MCP run_python: " + reply);
    if (failures) return 1;
    printf("Python checks passed\n");
    return 0;
}

static void Usage() {
    fprintf(stderr,
        "Usage: Vela [FILE] [usdview's options; see above]\n"
        "       Vela --check-document FILE\n"
        "       Vela --check-saving FILE PRIM   # edit and save through the document\n"
        "       Vela --check-browser FILE    # read-only browsing smoke check\n"
        "       Vela --check-python FILE    # the embedded interpreter and MCP tools\n"
        "       Vela --describe-actions     # the action registry, as JSON\n"
        "       Vela --describe-menus       # the main menu, as text\n"
        "       Vela --snapshot FILE OUTPUT.png [--camera PATH | --viewCamera PATH] [--frame TIME]\n"
        "                       [--width W] [--height H] [--purposes P[,P...]]\n"
        "                       [--complexity low|medium|high|veryhigh] [--drawMode MODE]\n"
        "                       [--disableCameraLight] [--enableDomeLightVisibility]\n"
        "                       [--colorCorrectionMode disabled|sRGB|openColorIO]\n"
        "                       [--renderer NAME] [--aov NAME]\n"
        "                       [--renderSettingsPrimPath PATH | --renderPassPrimPath PATH]\n"
        "       Vela --benchmark FILE [--frames N] [--frame TIME] [--width W] [--height H]\n"
        "       Vela --benchmark FILE --window [--frames N]\n");
}

// The Settings window's settings, saved as JSON in the user defaults.
// clear forgets the saved ones; useDefaults ignores them for this run and
// saves nothing.
static void LoadSettings(bool clear, bool useDefaults) {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSString *const key = @"VelaSettings";
    if (clear) [defaults removeObjectForKey:key];
    vela::AppSettingsStore &store = vela::AppSettingsStore::Get();
    if (NSString *saved = [defaults stringForKey:key]; saved && !useDefaults) {
        const JsValue json = JsParseString(saved.UTF8String);
        vela::AppSettings settings;
        if (json.IsObject()) vela::FromJson(json.GetJsObject(), &settings);
        store.SetSettings(settings);
    }
    if (!useDefaults) {
        store.AddObserver([key] {
            const std::string json =
                JsWriteToString(JsValue(vela::ToJson(vela::AppSettingsStore::Get().GetSettings())));
            [NSUserDefaults.standardUserDefaults setObject:@(json.c_str()) forKey:key];
        });
    }
    // At launch only, as worker threads start once.
    if (const int limit = store.GetSettings().threadLimit) {
        WorkSetConcurrencyLimitArgument(limit);
    }
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        // App actions the core can't provide, before menus and --describe-actions.
        VelaRegisterClaudeActions();
        VelaRegisterImageActions();
        NSString *runtime = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"Runtime/usd"];
        PlugRegistry::GetInstance().RegisterPlugins(
            [[runtime stringByAppendingPathComponent:@"lib/usd/plugInfo.json"] UTF8String]);
        PlugRegistry::GetInstance().RegisterPlugins(
            [[runtime stringByAppendingPathComponent:@"plugin/usd/plugInfo.json"] UTF8String]);
        if (argc == 2 && strcmp(argv[1], "--describe-actions") == 0) {
            printf("%s\n", JsWriteToString(vela::ActionRegistry::Get().Describe()).c_str());
            return 0;
        }
        if (argc == 2 && strcmp(argv[1], "--describe-menus") == 0) {
            printf("%s", VelaDescribeMenu(VelaBuildMainMenu()).UTF8String);
            return 0;
        }
        if (argc == 3 && strcmp(argv[1], "--check-browser") == 0) {
            std::string error;
            auto core = vela::Document::Open(argv[2], &error);
            if (!core) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
            [NSApplication sharedApplication];
            VelaBrowserController *browser = [[VelaBrowserController alloc] initWithDocument:core];
            if (![browser checkBrowsing]) return 1;
            printf("Browser selection and inspector checks passed\n");
            return 0;
        }
        if (argc == 4 && strcmp(argv[1], "--check-saving") == 0) {
            // Edits FILE through a VelaDocument: hides PRIM and saves as the
            // close prompt's Save does.
            NSError *error = nil;
            NSURL *url = [NSURL fileURLWithPath:@(argv[2])];
            VelaDocument *document = [[VelaDocument alloc] initWithContentsOfURL:url
                ofType:@"USD Scene" error:&error];
            auto fail = [](const char *what) { fprintf(stderr, "%s\n", what); return 1; };
            if (!document) return fail(error.localizedDescription.UTF8String);
            if (document.documentEdited) return fail("edited before any edit");
            auto &registry = vela::ActionRegistry::Get();
            registry.Run(*document.core, "select", {{"paths", JsValue(JsArray{JsValue(argv[3])})}});
            if (!registry.Run(*document.core, "hide_selection").ok) return fail("hide failed");
            if (!document.documentEdited) return fail("not edited after an edit");
            __block NSError *saveError = [NSError errorWithDomain:@"pending" code:0 userInfo:nil];
            [document saveToURL:url ofType:@"USD Scene" forSaveOperation:NSSaveOperation
              completionHandler:^(NSError *e) { saveError = e; }];
            if (saveError) return fail(saveError.localizedDescription.UTF8String);
            if (document.documentEdited) return fail("still edited after saving");
            __block NSError *saveAsError = nil;
            [document saveToURL:url ofType:@"USD Scene" forSaveOperation:NSSaveAsOperation
              completionHandler:^(NSError *e) { saveAsError = e; }];
            if (!saveAsError) return fail("Save As should be refused");
            // Undo after saving makes the document edited again.
            registry.Run(*document.core, "undo");
            if (!document.documentEdited) return fail("not edited after undoing a saved edit");
            printf("Saving checks passed\n");
            return 0;
        }
        if (argc == 3 && strcmp(argv[1], "--check-python") == 0) {
            return CheckPython(argv[2]);
        }
        if (argc == 3 && strcmp(argv[1], "--check-document") == 0) {
            VelaDocument *document = [VelaDocument new];
            NSError *error = nil;
            NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]]];
            if (![document readFromURL:url ofType:@"USD Scene" error:&error]) {
                fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
                return 1;
            }
            printf("%zu prims\n", [document primCount]);
            return 0;
        }
        if (argc > 1 && (strcmp(argv[1], "--snapshot") == 0 ||
                         strcmp(argv[1], "--benchmark") == 0)) {
            RenderOptions options;
            if (!ParseRenderOptions(argc, argv, 2, &options)) {
                Usage();
                return 2;
            }
            if (strcmp(argv[1], "--snapshot") == 0) return Snapshot(options);
            return options.window ? WindowBenchmark(options) : Benchmark(options);
        }
        // A normal launch: an optional file and usdview's options. Vela opens
        // the file itself, so AppKit must not treat arguments as files.
        VelaSetLaunchTime(std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) {
            // Arguments AppKit adds itself, e.g. -NSDocumentRevisionsDebugMode YES.
            if (argv[i][0] == '-' && argv[i][1] == 'N' && argv[i][2] == 'S') { ++i; continue; }
            args.push_back(argv[i]);
        }
        // Settings first: the options start from them. --clearsettings and
        // --defaultsettings decide whether the saved ones are used.
        auto has = [&](const char *flag) {
            return std::find(args.begin(), args.end(), flag) != args.end();
        };
        LoadSettings(has("--clearsettings"), has("--defaultsettings"));
        VelaInitializeImages(has("--defaultsettings"));
        vela::LaunchOptions launch =
            vela::DefaultLaunchOptions(vela::AppSettingsStore::Get().GetSettings());
        std::string error;
        if (!vela::ParseLaunchOptions(args, &launch, &error)) {
            fprintf(stderr, "%s\n%s", error.c_str(), vela::LaunchUsage());
            Usage();
            return 2;
        }
        // Tests (--defaultsettings) neither restore nor save open windows.
        if (has("--defaultsettings")) {
            [NSUserDefaults.standardUserDefaults setVolatileDomain:@{
                @"ApplePersistenceIgnoreState": @"YES"} forName:NSArgumentDomain];
        }
        NSURL *launchFile = nil;
        if (!launch.file.empty()) {
            launchFile = [NSURL fileURLWithPath:@(launch.file.c_str())];
            launch.file = launchFile.fileSystemRepresentation;
            VelaSetLaunchOptions(launch);
            NSMutableDictionary *arguments = [[NSUserDefaults.standardUserDefaults
                volatileDomainForName:NSArgumentDomain] mutableCopy] ?: [NSMutableDictionary dictionary];
            arguments[@"NSTreatUnknownArgumentsAsOpen"] = @"NO";
            [NSUserDefaults.standardUserDefaults setVolatileDomain:arguments
                                                           forName:NSArgumentDomain];
        }
        // The Log tab shows USD's diagnostics from here on.
        VelaStartLog();
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        VelaDelegate *delegate = [[VelaDelegate alloc] initWithFile:launchFile];
        NSApp.delegate = delegate;
        NSMenu *bar = VelaBuildMainMenu();
        NSApp.mainMenu = bar;
        [NSDocumentController sharedDocumentController];
        [NSApp run];
    }
    return 0;
}
