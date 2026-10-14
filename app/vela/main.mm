#import <AppKit/AppKit.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#import "document.h"
#import "browser_view.h"
#import "menus.h"
#import "viewport/viewport_view.h"
#include "app/vela/core/actions.h"
#include "app/vela/core/document.h"
#include "app/vela/core/free_camera.h"
#include "pxr/base/js/json.h"
#include "app/vela/viewport/renderer.h"
#include "pxr/base/plug/registry.h"
#include "pxr/usd/usdGeom/bboxCache.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/tokens.h"

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
- (BOOL)applicationShouldOpenUntitledFile:(NSApplication *)sender { return NO; }
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    [NSApp activateIgnoringOtherApps:YES];
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
        } else if (arg == "--disableCameraLight") options->settings.cameraLight = false;
        else if (arg == "--enableDomeLightVisibility") options->settings.domeLightTexturesVisible = true;
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
static int Snapshot(const RenderOptions &options) {
    UsdStageRefPtr stage = UsdStage::Open(options.scene);
    if (!stage || options.output.empty()) {
        fprintf(stderr, "Could not open %s\n", options.scene.c_str());
        return 1;
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

static void Usage() {
    fprintf(stderr,
        "Usage: Vela [FILE] [usdview's options; see above]\n"
        "       Vela --check-document FILE\n"
        "       Vela --check-saving FILE PRIM   # edit and save through the document\n"
        "       Vela --check-browser FILE    # read-only browsing smoke check\n"
        "       Vela --describe-actions     # the action registry, as JSON\n"
        "       Vela --describe-menus       # the main menu, as text\n"
        "       Vela --snapshot FILE OUTPUT.png [--camera PATH | --viewCamera PATH] [--frame TIME]\n"
        "                       [--width W] [--height H] [--purposes P[,P...]]\n"
        "                       [--complexity low|medium|high|veryhigh] [--drawMode MODE]\n"
        "                       [--disableCameraLight] [--enableDomeLightVisibility]\n"
        "       Vela --benchmark FILE [--frames N] [--frame TIME] [--width W] [--height H]\n"
        "       Vela --benchmark FILE --window [--frames N]\n");
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
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
        vela::LaunchOptions launch;
        std::string error;
        if (!vela::ParseLaunchOptions(args, &launch, &error)) {
            fprintf(stderr, "%s\n%s", error.c_str(), vela::LaunchUsage());
            Usage();
            return 2;
        }
        NSURL *launchFile = nil;
        if (!launch.file.empty()) {
            launchFile = [NSURL fileURLWithPath:@(launch.file.c_str())];
            launch.file = launchFile.fileSystemRepresentation;
            VelaSetLaunchOptions(launch);
            [NSUserDefaults.standardUserDefaults setVolatileDomain:@{
                @"NSTreatUnknownArgumentsAsOpen": @"NO"} forName:NSArgumentDomain];
        }
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
