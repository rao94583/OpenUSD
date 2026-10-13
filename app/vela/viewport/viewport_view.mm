#import "viewport_view.h"
#import <QuartzCore/QuartzCore.h>

#include "app/vela/core/actions.h"
#include "app/vela/viewport/renderer.h"

#include "pxr/base/gf/frustum.h"

#include <algorithm>
#include <chrono>
#include <memory>

PXR_NAMESPACE_USING_DIRECTIVE

// usdview's default dark grey background.
static const GfVec4f BackgroundColor(0.3f, 0.3f, 0.3f, 1.0f);

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

@implementation VelaViewportView {
    std::shared_ptr<vela::Document> _document;
    int _observer;
    std::unique_ptr<vela::Renderer> _renderer;
    vela::CameraDrag _drag;
    BOOL _dragged;
    double _frameMilliseconds;
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
}

- (instancetype)initWithDocument:(std::shared_ptr<vela::Document>)document {
    if (!(self = [super initWithFrame:NSMakeRect(0, 0, 800, 600)])) {
        return nil;
    }
    _document = std::move(document);
    _renderer = std::make_unique<vela::Renderer>();
    _renderer->SetStage(_document->GetStage());
    __weak VelaViewportView *weakSelf = self;
    _observer = _document->AddObserver([weakSelf](unsigned changes) {
        [weakSelf documentChanged:changes];
    });
    self.wantsLayer = YES;
    return self;
}

- (void)dealloc {
    _document->RemoveObserver(_observer);
}

- (void)documentChanged:(unsigned)changes {
    using D = vela::Document;
    if (changes & D::Selection) {
        _renderer->SetSelection(_document->GetSelection());
    }
    if (changes & (D::StageContent | D::Selection | D::Time | D::Camera)) {
        [self requestRender];
    }
    if (changes & D::Selection) {
        [self reportStatus];
    }
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
    return _document->GetCamera().ComputeGfCamera(double(size[0]) / size[1],
                                                  _document->GetStageBounds());
}

#pragma mark Drawing

- (void)render {
    CAMetalLayer *layer = (CAMetalLayer *)self.layer;
    const GfVec2i size = [self pixelSize];
    layer.drawableSize = CGSizeMake(size[0], size[1]);
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    if (_benchmarkRemaining > 0) {
        _document->GetCamera().Tumble(360.0 / _benchmarkFrames, 0.0);
    }
    vela::View view;
    view.camera = [self gfCamera];
    view.time = _document->GetTime();
    view.size = size;
    view.clearColor = BackgroundColor;
    id<MTLCommandBuffer> commandBuffer = [_renderer->GetQueue() commandBuffer];
    if (!_renderer->Draw(view, drawable.texture, commandBuffer)) {
        return;
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
    self.statusChanged([NSString stringWithFormat:@"%@    %.1f ms",
                        selection, _frameMilliseconds]);
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
    if (!_renderer->Pick(frustum, _document->GetTime(), &hit, nullptr) && extend) {
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
        _document->GetCamera().Drag(vela::CameraDrag::Track, event.scrollingDeltaX * scale,
                                    event.scrollingDeltaY * scale, [self pixelSize][1]);
    } else {
        // A wheel notch dollies by about 12%, as in usdview.
        const double step = event.hasPreciseScrollingDeltas
            ? event.scrollingDeltaY * 0.01 : event.scrollingDeltaY * 0.12;
        _document->GetCamera().Dolly(1.0 - std::clamp(step, -0.5, 0.5));
    }
    _document->CameraChanged();
}

- (void)magnifyWithEvent:(NSEvent *)event {
    _document->GetCamera().Dolly(1.0 / std::max(0.1, 1.0 + event.magnification));
    _document->CameraChanged();
}

@end
