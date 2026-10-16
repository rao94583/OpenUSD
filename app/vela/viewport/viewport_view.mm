#import "viewport_view.h"
#import "hud_view.h"
#import "movie_writer.h"
#import <QuartzCore/QuartzCore.h>

#include "app/vela/core/actions.h"
#include "app/vela/viewport/renderer.h"

#include "pxr/base/gf/frustum.h"
#include "pxr/base/gf/half.h"
#include "pxr/imaging/cameraUtil/conformWindow.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>

PXR_NAMESPACE_USING_DIRECTIVE

// Forwards display link ticks without retaining the view.
@interface VelaDisplayLinkTarget : NSObject
@property (nonatomic, weak) VelaViewportView *view;
@end

@interface VelaViewportView ()
- (void)displayLinkFired;
@end

@implementation VelaDisplayLinkTarget
- (void)tick:(id)link { [self.view displayLinkFired]; }
@end

// A movie export's progress, with Cancel, in a modal session: the main
// thread renders the frames, and the session keeps the UI responding
// between them.
@interface VelaExportProgress : NSObject
- (instancetype)initWithTitle:(NSString *)title frames:(int64_t)frames window:(NSWindow *)window;
- (BOOL)updateFrame:(int64_t)frame;  // NO when cancelled
- (void)end;
@end

@implementation VelaExportProgress {
    NSPanel *_panel;
    NSProgressIndicator *_bar;
    NSTextField *_label;
    NSModalSession _session;
    int64_t _frames;
    BOOL _cancelled;
}

- (instancetype)initWithTitle:(NSString *)title frames:(int64_t)frames window:(NSWindow *)window {
    if (!(self = [super init])) return nil;
    _frames = frames;
    _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 360, 110)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    _panel.title = title;
    _bar = [NSProgressIndicator new];
    _bar.indeterminate = NO;
    _bar.minValue = 0;
    _bar.maxValue = double(frames);
    _label = [NSTextField labelWithString:@""];
    _label.font = [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular];
    NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancel:)];
    cancel.keyEquivalent = @"\e";
    NSStackView *stack = [NSStackView stackViewWithViews:@[_bar, _label, cancel]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.edgeInsets = NSEdgeInsetsMake(16, 20, 16, 20);
    [_bar.widthAnchor constraintEqualToConstant:320].active = YES;
    _panel.contentView = stack;
    if (window) {
        // Over the document's window, wherever it is (even minimized).
        NSRect frame = _panel.frame;
        frame.origin = NSMakePoint(NSMidX(window.frame) - frame.size.width / 2,
                                   NSMidY(window.frame) - frame.size.height / 2);
        [_panel setFrame:frame display:NO];
    } else {
        [_panel center];
    }
    _session = [NSApp beginModalSessionForWindow:_panel];
    [self updateFrame:0];
    return self;
}

- (void)cancel:(id)sender { _cancelled = YES; }

- (BOOL)updateFrame:(int64_t)frame {
    _bar.doubleValue = double(frame);
    _label.stringValue = [NSString stringWithFormat:@"Frame %lld of %lld", frame, _frames];
    [NSApp runModalSession:_session];
    return !_cancelled;
}

- (void)end {
    [NSApp endModalSession:_session];
    [_panel orderOut:nil];
}

@end

@implementation VelaViewportView {
    std::shared_ptr<vela::Document> _document;
    int _observer;
    std::unique_ptr<vela::Renderer> _renderer;
    // The renderer info version last published to the document.
    unsigned _publishedRendererInfo;
    // The renderer's stage draws unloaded prims as boxes.
    BOOL _unloadedAsBounds;
    vela::CameraDrag _drag;
    BOOL _dragged;
    double _frameMilliseconds;
    VelaHudView *_hudView;
    // HUD Info counts, recomputed when they may have changed.
    vela::HudStats _hudStats;
    BOOL _hudStatsValid;
    // Time between frames during playback, for the HUD.
    double _playbackMilliseconds;
    std::chrono::steady_clock::time_point _lastPlaybackFrame;
    // The document frames the stage before the view has a size; frame
    // again once the viewport's shape is known.
    BOOL _framedForSize;
    // Renders are requested by input and changes, and run at most once per
    // display refresh (macOS 14+) or main loop pass (macOS 13).
    BOOL _needsRender;
    BOOL _renderScheduled;
    id _displayLink;
    VelaDisplayLinkTarget *_displayLinkTarget;
    // An on-screen benchmark in progress: frames left to tumble through.
    int _benchmarkFrames;
    int _benchmarkRemaining;
    std::chrono::steady_clock::time_point _benchmarkStart;
    void (^_benchmarkCompletion)(double seconds);
    // While exporting, the viewport doesn't draw (the renderer is busy).
    BOOL _exporting;
}

- (instancetype)initWithDocument:(std::shared_ptr<vela::Document>)document {
    if (!(self = [super initWithFrame:NSMakeRect(0, 0, 800, 600)])) {
        return nil;
    }
    _document = std::move(document);
    _renderer = std::make_unique<vela::Renderer>();
    _unloadedAsBounds = _document->GetViewSettings().unloadedAsBounds;
    _renderer->SetStage(_document->GetStage(), _unloadedAsBounds);
    // The document may already have a selection, e.g. from --select.
    _renderer->SetSelection(_document->GetSelection());
    __weak VelaViewportView *weakSelf = self;
    _observer = _document->AddObserver([weakSelf](unsigned changes) {
        [weakSelf documentChanged:changes];
    });
    self.wantsLayer = YES;
    _hudView = [[VelaHudView alloc] initWithFrame:self.bounds];
    _hudView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [self addSubview:_hudView];
    // File → Export Image… and Export Movie…, through the document's actions.
    vela::Document::ViewportServices services;
    services.exportImage = [weakSelf](vela::ImageExport &request, std::string *error) {
        VelaViewportView *strongSelf = weakSelf;
        if (!strongSelf) { *error = "The window has closed"; return false; }
        return bool([strongSelf exportImage:request error:error]);
    };
    services.exportMovie = [weakSelf](vela::MovieExport &request, std::string *error) {
        VelaViewportView *strongSelf = weakSelf;
        if (!strongSelf) { *error = "The window has closed"; return false; }
        return bool([strongSelf exportMovie:request error:error]);
    };
    _document->SetViewportServices(std::move(services));
    return self;
}

- (void)dealloc {
    _document->RemoveObserver(_observer);
    _document->SetViewportServices({});
}

- (void)documentChanged:(unsigned)changes {
    using D = vela::Document;
    if (changes & D::Selection) {
        _renderer->SetSelection(_document->GetSelection());
    }
    // Unloaded prims as boxes is an engine parameter: a new engine.
    if ((changes & D::Display) &&
        _document->GetViewSettings().unloadedAsBounds != _unloadedAsBounds) {
        _unloadedAsBounds = _document->GetViewSettings().unloadedAsBounds;
        _renderer->SetStage(_document->GetStage(), _unloadedAsBounds);
        _renderer->SetSelection(_document->GetSelection());
    }
    // Playback hides selection highlights while playing, as in usdview.
    if (changes & (D::StageContent | D::Selection | D::Time | D::Camera | D::Playback | D::Display)) {
        [self requestRender];
    }
    if (changes & D::Selection) {
        [self reportStatus];
    }
    // Info counts follow the selection, the stage and (when paused) time.
    if ((changes & (D::Selection | D::StageContent | D::Display)) ||
        ((changes & D::Time) && !_document->IsPlaying())) {
        _hudStatsValid = NO;
    }
    if ((changes & D::Time) && _document->IsPlaying()) {
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - _lastPlaybackFrame).count();
        if (ms < 2000) _playbackMilliseconds = ms;
        _lastPlaybackFrame = now;
    }
    if (changes & D::Playback) {
        _playbackMilliseconds = 0;
        _lastPlaybackFrame = std::chrono::steady_clock::now();
        if (!_document->IsPlaying()) _hudStatsValid = NO;
    }
    if (changes & (D::Display | D::Camera | D::Playback | D::Renderer)) [self updateHud];
}

- (void)updateHud {
    const vela::ViewSettings &settings = _document->GetViewSettings();
    if (settings.showHUD && settings.hudInfo && !_hudStatsValid) {
        _hudStats = vela::ComputeHudStats(_document->GetStage(), _document->GetSelection(),
                                          _document->GetTime());
        _hudStatsValid = YES;
    }
    vela::HudRenderInfo render;
    render.renderer = _renderer->GetRendererName();
    render.aov = settings.aov;
    render.renderMilliseconds = _frameMilliseconds;
    render.playbackMilliseconds = _document->IsPlaying() ? _playbackMilliseconds : 0.0;
    if (settings.hudGPUStats) render.renderStats = _renderer->GetRenderStats();
    [_hudView setHud:vela::ComposeHud(*_document, _hudStats, render)];
}

- (CALayer *)makeBackingLayer {
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.device = _renderer->GetDevice();
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    // Storm's sRGB color correction has already encoded the image.
    CGColorSpaceRef sRGB = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    layer.colorspace = sRGB;
    CGColorSpaceRelease(sRGB);
    layer.opaque = YES;
    return layer;
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }
- (BOOL)isOpaque { return YES; }

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    self.layer.contentsScale = self.window.backingScaleFactor;
    [self requestRender];
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (@available(macOS 14.0, *)) {
        [(CADisplayLink *)_displayLink invalidate];
        _displayLink = nil;
        if (self.window) {
            _displayLinkTarget = [VelaDisplayLinkTarget new];
            _displayLinkTarget.view = self;
            CADisplayLink *link = [self displayLinkWithTarget:_displayLinkTarget
                                                     selector:@selector(tick:)];
            link.paused = YES;
            [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
            _displayLink = link;
        }
    }
    [self requestRender];
}

- (void)requestRender {
    _needsRender = YES;
    if (@available(macOS 14.0, *)) {
        ((CADisplayLink *)_displayLink).paused = NO;
    } else if (!_renderScheduled) {
        _renderScheduled = YES;
        __weak VelaViewportView *weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            VelaViewportView *strongSelf = weakSelf;
            if (strongSelf) {
                strongSelf->_renderScheduled = NO;
                [strongSelf displayLinkFired];
            }
        });
    }
}

- (void)displayLinkFired {
    if (!_needsRender || !self.window) {
        if (@available(macOS 14.0, *)) {
            ((CADisplayLink *)_displayLink).paused = YES;
        }
        return;
    }
    _needsRender = NO;
    [self render];
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self requestRender];
}

#pragma mark Camera

- (double)backingScale {
    return self.window ? self.window.backingScaleFactor : 1.0;
}

- (GfVec2i)pixelSize {
    const NSSize size = [self convertSizeToBacking:self.bounds.size];
    return GfVec2i(std::max(1, int(size.width)), std::max(1, int(size.height)));
}

- (GfCamera)gfCamera {
    const GfVec2i size = [self pixelSize];
    return _document->ComputeViewGfCamera(double(size[0]) / size[1]);
}

#pragma mark Drawing

// What the viewport draws, at a size in pixels.
- (vela::View)viewOfSize:(GfVec2i)size {
    return [self viewOfSize:size time:_document->GetTime() showSelection:YES];
}

// The same at another time (the camera too, when it is a scene camera),
// with or without the selection's highlights and bounding boxes.
- (vela::View)viewOfSize:(GfVec2i)size time:(UsdTimeCode)time showSelection:(BOOL)showSelection {
    vela::View view;
    view.camera = _document->ComputeViewGfCamera(double(size[0]) / size[1], time);
    view.time = time;
    view.size = size;
    view.settings = _document->GetViewSettings();
    view.clearColor = vela::BackgroundColor(view.settings.background);
    const vela::ViewSettings &s = view.settings;
    if (s.showBBoxes && (s.showBBoxesDuringPlayback || !_document->IsPlaying())) {
        const GfBBox3d &bounds = _document->GetSelectionBounds();
        if (!bounds.GetRange().IsEmpty()) {
            if (s.showAlignedBBox) view.bboxes.push_back(GfBBox3d(bounds.ComputeAlignedRange()));
            if (s.showOrientedBBox) view.bboxes.push_back(bounds);
        }
        view.bboxColor = vela::BBoxLineColor(s.background);
    }
    switch (view.settings.highlightMode) {
    case vela::HighlightMode::Never: view.highlight = false; break;
    case vela::HighlightMode::WhenPaused: view.highlight = !_document->IsPlaying(); break;
    case vela::HighlightMode::Always: view.highlight = true; break;
    }
    if (!showSelection) {
        view.highlight = false;
        view.bboxes.clear();
    }
    return view;
}

// An export's size: as asked, or the viewport's; with one of them, the
// other follows the camera's aspect ratio. even rounds up to even
// numbers, as video codecs need.
- (GfVec2i)exportSizeWithWidth:(int)width height:(int)height time:(UsdTimeCode)time even:(BOOL)even {
    return vela::ResolveExportSize(width, height, _document->GetViewCameraAspectRatio(time),
                                   [self pixelSize], even);
}

namespace {

// pixels placed in a black frame of the output's size (as a fitted frame
// says), for letterboxing and pillarboxing.
vela::Renderer::Pixels Boxed(const vela::Renderer::Pixels &inner, const vela::FittedFrame &frame,
                             GfVec2i output) {
    vela::Renderer::Pixels out;
    out.width = output[0];
    out.height = output[1];
    out.format = inner.format;
    out.flipped = inner.flipped;
    const size_t pixelSize = inner.data.size() / (size_t(inner.width) * inner.height);
    // Opaque black in the pixels' format.
    std::vector<uint8_t> black(pixelSize, 0);
    if (inner.format == HioFormatUNorm8Vec4) black[3] = 255;
    else if (inner.format == HioFormatFloat16Vec4) {
        const GfHalf one(1.0f);
        std::memcpy(&black[6], &one, sizeof(one));
    } else if (inner.format == HioFormatFloat32Vec4) {
        const float one = 1.0f;
        std::memcpy(&black[12], &one, sizeof(one));
    }
    out.data.resize(size_t(output[0]) * output[1] * pixelSize);
    for (size_t i = 0; i < out.data.size(); i += pixelSize) {
        std::memcpy(&out.data[i], black.data(), pixelSize);
    }
    // Rows by their place in the image, top first, whichever way the
    // buffers store them.
    const size_t innerRow = size_t(inner.width) * pixelSize, outRow = size_t(output[0]) * pixelSize;
    for (int y = 0; y < inner.height; ++y) {
        const int from = inner.flipped ? inner.height - 1 - y : y;
        const int imageRow = frame.offset[1] + y;
        const int to = out.flipped ? output[1] - 1 - imageRow : imageRow;
        std::memcpy(&out.data[size_t(to) * outRow + size_t(frame.offset[0]) * pixelSize],
                    &inner.data[size_t(from) * innerRow], innerRow);
    }
    return out;
}

} // namespace

// A frame for export, rendered offscreen at the output's size. When the
// camera's aspect ratio differs from the output's, Fit renders the
// camera's whole frame and adds bars; Crop fills the output with the
// middle of it.
- (BOOL)renderExportFrame:(GfVec2i)output time:(UsdTimeCode)time framing:(vela::Framing)framing
            showSelection:(BOOL)showSelection linear:(BOOL)linear
                   pixels:(vela::Renderer::Pixels *)pixels {
    const double aspect = _document->GetViewCameraAspectRatio(time);
    const double outputAspect = double(output[0]) / output[1];
    if (framing == vela::Framing::Fit) {
        const vela::FittedFrame frame = vela::FitFrame(output, aspect);
        if (frame.boxed) {
            vela::Renderer::Pixels inner;
            if (!_renderer->ReadPixels([self viewOfSize:frame.size time:time showSelection:showSelection],
                                       linear, &inner)) {
                return NO;
            }
            *pixels = Boxed(inner, frame, output);
            return YES;
        }
        return _renderer->ReadPixels([self viewOfSize:output time:time showSelection:showSelection],
                                     linear, pixels);
    }
    vela::View view = [self viewOfSize:output time:time showSelection:showSelection];
    GfCamera camera = _document->ComputeViewGfCamera(aspect, time);
    CameraUtilConformWindow(&camera, CameraUtilCrop, outputAspect);
    view.camera = camera;
    return _renderer->ReadPixels(view, linear, pixels);
}

// Rendered offscreen, into a texture of its own: what the window shows on
// screen (covered, minimized, or smaller) doesn't matter.
- (BOOL)exportImage:(vela::ImageExport &)request error:(std::string *)error {
    const UsdTimeCode time = _document->GetTime();
    const GfVec2i size = [self exportSizeWithWidth:request.width height:request.height
                                              time:time even:NO];
    request.width = size[0];
    request.height = size[1];
    _exporting = YES;
    vela::Renderer::Pixels pixels;
    const bool rendered = [self renderExportFrame:size time:time framing:request.framing
                                    showSelection:request.showSelection
                                           linear:vela::Renderer::WantsLinear(request.path)
                                           pixels:&pixels];
    const bool ok = rendered && vela::Renderer::WritePixels(pixels, request.path);
    _exporting = NO;
    [self requestRender];
    if (!ok) *error = rendered ? "the image could not be written (is the folder writable?)"
                               : "the renderer could not render it";
    return ok;
}

- (BOOL)exportMovie:(vela::MovieExport &)request error:(std::string *)error {
    const GfVec2i size = [self exportSizeWithWidth:request.width height:request.height
        time:UsdTimeCode(request.start) even:request.codec == "h264" || request.codec == "hevc"];
    request.width = size[0];
    request.height = size[1];
    const int64_t frames = int64_t(std::floor(request.end - request.start)) + 1;
    NSString *message = nil;
    VelaMovieWriter *writer = [[VelaMovieWriter alloc] initWithPath:@(request.path.c_str())
        width:size[0] height:size[1] framesPerSecond:request.framesPerSecond
        codec:@(request.codec.c_str()) error:&message];
    if (!writer) {
        *error = message ? message.UTF8String : "could not start the movie";
        return false;
    }
    VelaExportProgress *progress = [[VelaExportProgress alloc]
        initWithTitle:[NSString stringWithFormat:@"Exporting %@",
                       @(request.path.c_str()).lastPathComponent]
               frames:frames window:self.window];
    _exporting = YES;
    bool ok = true;
    vela::Renderer::Pixels pixels;
    for (int64_t i = 0; i < frames && ok; ++i) {
        if (![progress updateFrame:i]) {
            *error = "cancelled";
            ok = false;
            break;
        }
        if (![self renderExportFrame:size time:UsdTimeCode(request.start + double(i))
                             framing:request.framing showSelection:request.showSelection
                              linear:NO pixels:&pixels]) {
            *error = "frame " + std::to_string(i) + " did not render";
            ok = false;
        } else if (![writer appendFrame:pixels.data.data() error:&message]) {
            *error = message ? message.UTF8String : "could not add a frame";
            ok = false;
        }
    }
    if (ok) {
        [progress updateFrame:frames];
        ok = [writer finish:&message];
        if (!ok) *error = message ? message.UTF8String : "could not finish the movie";
    } else {
        [writer cancel];
    }
    [progress end];
    _exporting = NO;
    [self requestRender];
    return ok;
}

- (NSData *)pngWithWidth:(int)width height:(int)height error:(NSString **)error {
    GfVec2i size = [self pixelSize];
    const double aspect = double(size[0]) / size[1];
    if (width > 0 && height > 0) size = GfVec2i(width, height);
    else if (width > 0) size = GfVec2i(width, std::max(1, int(std::lround(width / aspect))));
    else if (height > 0) size = GfVec2i(std::max(1, int(std::lround(height * aspect))), height);
    NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:
        [NSString stringWithFormat:@"vela-screenshot-%@.png", NSUUID.UUID.UUIDString]];
    if (!_renderer->WriteImage([self viewOfSize:size], path.UTF8String)) {
        if (error) *error = @"The viewport could not render an image";
        return nil;
    }
    NSData *data = [NSData dataWithContentsOfFile:path];
    [NSFileManager.defaultManager removeItemAtPath:path error:nil];
    // The viewport draws again with its interactive settings.
    [self requestRender];
    if (!data && error) *error = @"The rendered image could not be read";
    return data;
}

- (void)render {
    // View → Display → Pause Viewport Drawing (usdview's --norender), and
    // exports, which use the renderer.
    if (_document->GetViewSettings().pauseDrawing || _exporting) {
        return;
    }
    CAMetalLayer *layer = (CAMetalLayer *)self.layer;
    const GfVec2i size = [self pixelSize];
    layer.drawableSize = CGSizeMake(size[0], size[1]);
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) {
        return;
    }
    _document->SetViewportAspectRatio(double(size[0]) / size[1]);
    if (!_framedForSize) {
        // Redo the opening framing for the viewport's shape: the selection
        // if there is one, otherwise everything. A scene camera chosen at
        // launch is kept.
        _framedForSize = YES;
        if (_document->GetViewCamera().IsEmpty()) _document->FrameSelection();
    }
    const auto start = std::chrono::steady_clock::now();
    if (_benchmarkRemaining > 0) {
        _document->DetachViewCamera();
        _document->GetCamera().Tumble(360.0 / _benchmarkFrames, 0.0);
    }
    const vela::View view = [self viewOfSize:size];
    id<MTLCommandBuffer> commandBuffer = [_renderer->GetQueue() commandBuffer];
    if (!_renderer->Draw(view, drawable.texture, commandBuffer)) {
        return;
    }
    // Menus and scripts learn what the renderer offers (after Draw, which
    // applies renderer changes).
    if (_renderer->GetInfoVersion() != _publishedRendererInfo) {
        _publishedRendererInfo = _renderer->GetInfoVersion();
        _document->SetRendererInfo(_renderer->GetInfo());
    }
    __weak VelaViewportView *weakSelf = self;
    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) {
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        dispatch_async(dispatch_get_main_queue(), ^{
            VelaViewportView *strongSelf = weakSelf;
            if (strongSelf) {
                strongSelf->_frameMilliseconds = ms;
                [strongSelf reportStatus];
                [strongSelf updateHud];
            }
        });
    }];
    if (_benchmarkRemaining > 0 && --_benchmarkRemaining == 0) {
        void (^completion)(double) = _benchmarkCompletion;
        const auto benchmarkStart = _benchmarkStart;
        _benchmarkCompletion = nil;
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) {
            const double seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - benchmarkStart).count();
            dispatch_async(dispatch_get_main_queue(), ^{ completion(seconds); });
        }];
    }
    if (self.firstFrameDrawn) {
        void (^firstFrame)(void) = self.firstFrameDrawn;
        self.firstFrameDrawn = nil;
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) {
            dispatch_async(dispatch_get_main_queue(), firstFrame);
        }];
    }
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];
    if (!_renderer->IsConverged() || _benchmarkRemaining > 0) {
        [self requestRender];
    }
}

- (void)benchmarkFrames:(int)frames completion:(void (^)(double seconds))completion {
    _benchmarkFrames = _benchmarkRemaining = std::max(1, frames);
    _benchmarkCompletion = [completion copy];
    _benchmarkStart = std::chrono::steady_clock::now();
    [self requestRender];
}

- (void)reportStatus {
    if (!self.statusChanged) {
        return;
    }
    const SdfPathVector &paths = _document->GetSelection();
    NSString *selection = @"No selection";
    if (paths.size() == 1) {
        selection = @(paths.front().GetText());
    } else if (!paths.empty()) {
        selection = [NSString stringWithFormat:@"%zu prims selected", paths.size()];
    }
    self.statusChanged(selection, _frameMilliseconds);
}

#pragma mark Picking

- (void)pickAt:(NSPoint)location extend:(BOOL)extend {
    const NSPoint pixel = [self convertPointToBacking:
        [self convertPoint:location fromView:nil]];
    const GfVec2i size = [self pixelSize];
    // Backing coordinates have y up, like normalized device coordinates.
    const GfVec2d point(2.0 * pixel.x / size[0] - 1.0, 2.0 * pixel.y / size[1] - 1.0);
    const GfVec2d pickSize(1.0 / size[0], 1.0 / size[1]);
    const GfFrustum frustum =
        [self gfCamera].GetFrustum().ComputeNarrowedFrustum(point, pickSize);
    SdfPath hit;
    if (!_renderer->Pick(frustum, _document->GetTime(), _document->GetViewSettings(), &hit, nullptr) && extend) {
        return;
    }
    JsArray paths;
    if (!hit.IsEmpty()) paths.push_back(JsValue(hit.GetString()));
    vela::ActionRegistry::Get().Run(*_document, "select", {
        {"paths", JsValue(paths)},
        {"mode", JsValue(extend ? "toggle" : "replace")},
    });
}

#pragma mark Mouse and trackpad

- (void)beginDrag:(NSEvent *)event {
    const NSEventModifierFlags flags = event.modifierFlags;
    _drag = vela::CameraDragFor(unsigned(NSEvent.pressedMouseButtons),
                                flags & NSEventModifierFlagOption,
                                flags & NSEventModifierFlagCommand);
    _dragged = NO;
}

- (void)continueDrag:(NSEvent *)event {
    _dragged = YES;
    if (_drag == vela::CameraDrag::None) {
        return;
    }
    // Drag in backing pixels, y down, as the camera math expects.
    const double scale = [self backingScale];
    // Navigating a scene camera moves a free copy of it.
    _document->DetachViewCamera();
    _document->GetCamera().Drag(_drag, event.deltaX * scale, event.deltaY * scale,
                                [self pixelSize][1]);
    _document->CameraChanged();
}

- (void)mouseDown:(NSEvent *)event { [self beginDrag:event]; }
- (void)rightMouseDown:(NSEvent *)event {
    [self beginDrag:event];
    if (_drag == vela::CameraDrag::None) {
        [super rightMouseDown:event];
    }
}
- (void)otherMouseDown:(NSEvent *)event { [self beginDrag:event]; }
- (void)mouseDragged:(NSEvent *)event { [self continueDrag:event]; }
- (void)rightMouseDragged:(NSEvent *)event { [self continueDrag:event]; }
- (void)otherMouseDragged:(NSEvent *)event { [self continueDrag:event]; }

- (void)mouseUp:(NSEvent *)event {
    if (_drag == vela::CameraDrag::None && !_dragged) {
        [self pickAt:event.locationInWindow
              extend:event.modifierFlags & NSEventModifierFlagShift];
    }
    [self endDragFor:event];
}
- (void)rightMouseUp:(NSEvent *)event { [self endDragFor:event]; }
- (void)otherMouseUp:(NSEvent *)event { [self endDragFor:event]; }

// Releasing one of several buttons changes the mode, e.g. from dolly with
// left + middle back to tumble with left.
- (void)endDragFor:(NSEvent *)event {
    const NSEventModifierFlags flags = event.modifierFlags;
    _drag = vela::CameraDragFor(unsigned(NSEvent.pressedMouseButtons),
                                flags & NSEventModifierFlagOption,
                                flags & NSEventModifierFlagCommand);
}

- (void)scrollWheel:(NSEvent *)event {
    if (event.hasPreciseScrollingDeltas &&
        (event.modifierFlags & NSEventModifierFlagOption)) {
        // Option + two-finger drag tracks; the content follows the fingers.
        const double scale = [self backingScale];
        // Navigating a scene camera moves a free copy of it.
        _document->DetachViewCamera();
        _document->GetCamera().Drag(vela::CameraDrag::Track, event.scrollingDeltaX * scale,
                                    event.scrollingDeltaY * scale, [self pixelSize][1]);
    } else {
        // A wheel notch dollies by about 12%, as in usdview.
        const double step = event.hasPreciseScrollingDeltas
            ? event.scrollingDeltaY * 0.01 : event.scrollingDeltaY * 0.12;
        // Navigating a scene camera moves a free copy of it.
        _document->DetachViewCamera();
        _document->GetCamera().Dolly(1.0 - std::clamp(step, -0.5, 0.5));
    }
    _document->CameraChanged();
}

- (void)magnifyWithEvent:(NSEvent *)event {
    // Navigating a scene camera moves a free copy of it.
    _document->DetachViewCamera();
    _document->GetCamera().Dolly(1.0 / std::max(0.1, 1.0 + event.magnification));
    _document->CameraChanged();
}

@end
