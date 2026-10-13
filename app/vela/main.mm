#import <AppKit/AppKit.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#import "document.h"
#import "menus.h"
#import "viewport/viewport_view.h"
#include "app/vela/core/actions.h"
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
@implementation VelaDelegate
- (BOOL)applicationShouldOpenUntitledFile:(NSApplication *)sender { return NO; }
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    [NSApp activateIgnoringOtherApps:YES];
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
};

static bool ParseRenderOptions(int argc, const char *argv[], int first, RenderOptions *options) {
    for (int i = first; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--camera" && hasValue) options->camera = SdfPath(argv[++i]);
        else if (arg == "--frame" && hasValue) options->time = UsdTimeCode(std::atof(argv[++i]));
        else if (arg == "--width" && hasValue) options->width = std::atoi(argv[++i]);
        else if (arg == "--height" && hasValue) options->height = std::atoi(argv[++i]);
        else if (arg == "--frames" && hasValue) options->frames = std::atoi(argv[++i]);
        else if (arg == "--window") options->window = true;
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
    view->domeLightsVisible = false;
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
        view->size = GfVec2i(options.width, std::max(1, int(float(options.width) / aspect)));
        return true;
    }
    const int height = options.height > 0 ? options.height : options.width * 9 / 16;
    view->size = GfVec2i(options.width, height);
    *freeCamera = vela::FreeCamera(UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->z);
    freeCamera->Frame(bounds);
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
    view.clearColor = GfVec4f(0.3f, 0.3f, 0.3f, 1.0f);
    view.domeLightsVisible = true;

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
        "Usage: Vela [FILE...]\n"
        "       Vela --check-document FILE\n"
        "       Vela --describe-actions     # the action registry, as JSON\n"
        "       Vela --describe-menus       # the main menu, as text\n"
        "       Vela --snapshot FILE OUTPUT.png [--camera PATH] [--frame TIME]\n"
        "                       [--width W] [--height H]\n"
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
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        VelaDelegate *delegate = [VelaDelegate new];
        NSApp.delegate = delegate;
        NSMenu *bar = VelaBuildMainMenu();
        NSApp.mainMenu = bar;
        [NSDocumentController sharedDocumentController];
        [NSApp run];
    }
    return 0;
}
