#import "document.h"
#import "viewport/viewport_view.h"

#include "pxr/usd/usd/primRange.h"

#include <cmath>

PXR_NAMESPACE_USING_DIRECTIVE

// The viewport above a timeline: play, a frame slider and the status line.
// Left and right arrows step frames and space plays, when no control has
// claimed the keys first.
@interface VelaWindowController : NSWindowController <NSWindowDelegate>
- (instancetype)initWithStage:(UsdStageRefPtr)stage primCount:(size_t)count;
@property (nonatomic, readonly) VelaViewportView *viewport;
@end

@implementation VelaWindowController {
    UsdStageRefPtr _stage;
    size_t _primCount;
    VelaViewportView *_viewport;
    NSSlider *_slider;
    NSTextField *_frameLabel;
    NSTextField *_statusLabel;
    NSButton *_playButton;
    NSTimer *_timer;
    double _start, _end;
}

- (instancetype)initWithStage:(UsdStageRefPtr)stage primCount:(size_t)count {
    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1200, 800)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                  NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    if (!(self = [super initWithWindow:window])) {
        return nil;
    }
    _stage = stage;
    _primCount = count;
    _start = stage->GetStartTimeCode();
    _end = stage->GetEndTimeCode();
    const bool animated = stage->HasAuthoredTimeCodeRange() && _end > _start;

    _viewport = [[VelaViewportView alloc] initWithStage:stage];
    _playButton = [NSButton buttonWithImage:
        [NSImage imageWithSystemSymbolName:@"play.fill" accessibilityDescription:@"Play"]
        target:self action:@selector(togglePlayback:)];
    _playButton.bezelStyle = NSBezelStyleTexturedRounded;
    _slider = [NSSlider sliderWithValue:_start minValue:_start maxValue:_end
                                 target:self action:@selector(sliderMoved:)];
    _slider.continuous = YES;
    _frameLabel = [NSTextField labelWithString:@""];
    _frameLabel.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
    [_frameLabel setContentHuggingPriority:NSLayoutPriorityRequired
                            forOrientation:NSLayoutConstraintOrientationHorizontal];
    _statusLabel = [NSTextField labelWithString:@""];
    _statusLabel.textColor = NSColor.secondaryLabelColor;
    _statusLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [_statusLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                           forOrientation:NSLayoutConstraintOrientationHorizontal];
    for (NSView *view in @[_playButton, _slider, _frameLabel]) {
        view.hidden = !animated;
    }
    [_slider.widthAnchor constraintGreaterThanOrEqualToConstant:200].active = YES;

    NSStackView *bar = [NSStackView stackViewWithViews:
        @[_playButton, _slider, _frameLabel, _statusLabel]];
    bar.edgeInsets = NSEdgeInsetsMake(6, 10, 6, 10);
    bar.spacing = 10;
    bar.detachesHiddenViews = YES;
    NSStackView *content = [NSStackView stackViewWithViews:@[_viewport, bar]];
    content.orientation = NSUserInterfaceLayoutOrientationVertical;
    content.spacing = 0;
    content.alignment = NSLayoutAttributeWidth;
    [_viewport setContentHuggingPriority:NSLayoutPriorityDefaultLow
                          forOrientation:NSLayoutConstraintOrientationVertical];
    [bar setContentHuggingPriority:NSLayoutPriorityRequired
                    forOrientation:NSLayoutConstraintOrientationVertical];
    window.contentView = content;
    window.initialFirstResponder = _viewport;
    [window center];

    __weak VelaWindowController *weakSelf = self;
    _viewport.statusChanged = ^(NSString *status) {
        VelaWindowController *strongSelf = weakSelf;
        if (strongSelf) {
            strongSelf->_statusLabel.stringValue = [NSString stringWithFormat:
                @"%zu prims    %@", strongSelf->_primCount, status];
        }
    };
    [self showTime];
    return self;
}

- (VelaViewportView *)viewport { return _viewport; }

- (void)setTime:(double)time {
    _viewport.time = UsdTimeCode(time);
    [self showTime];
}

- (void)showTime {
    const UsdTimeCode time = _viewport.time;
    if (time.IsNumeric()) {
        _slider.doubleValue = time.GetValue();
        _frameLabel.stringValue = [NSString stringWithFormat:@"%g / %g",
                                   time.GetValue(), _end];
    }
}

- (void)sliderMoved:(NSSlider *)slider {
    [self setTime:std::round(slider.doubleValue)];
}

- (void)stepBy:(double)frames {
    if (_end <= _start || !_viewport.time.IsNumeric()) {
        return;
    }
    double time = _viewport.time.GetValue() + frames;
    if (time > _end) time = _start;
    if (time < _start) time = _end;
    [self setTime:time];
}

- (void)togglePlayback:(id)sender {
    if (_timer) {
        [_timer invalidate];
        _timer = nil;
    } else if (_end > _start) {
        const double fps = _stage->GetFramesPerSecond() > 0 ? _stage->GetFramesPerSecond() : 24;
        __weak VelaWindowController *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / fps repeats:YES
                                                   block:^(NSTimer *) { [weakSelf stepBy:1]; }];
    }
    _playButton.image = [NSImage imageWithSystemSymbolName:_timer ? @"pause.fill" : @"play.fill"
                                  accessibilityDescription:_timer ? @"Pause" : @"Play"];
}

- (void)keyDown:(NSEvent *)event {
    NSString *key = event.charactersIgnoringModifiers;
    const unichar c = key.length ? [key characterAtIndex:0] : 0;
    if (c == NSRightArrowFunctionKey) {
        [self stepBy:1];
    } else if (c == NSLeftArrowFunctionKey) {
        [self stepBy:-1];
    } else if (c == ' ') {
        [self togglePlayback:nil];
    } else {
        [super keyDown:event];
    }
}

- (void)windowWillClose:(NSNotification *)notification {
    [_timer invalidate];
    _timer = nil;
}

@end

@implementation VelaDocument {
    UsdStageRefPtr _stage;
}

+ (BOOL)autosavesInPlace { return NO; }

- (UsdStageRefPtr)stage { return _stage; }

- (VelaViewportView *)viewport {
    return ((VelaWindowController *)self.windowControllers.firstObject).viewport;
}

- (BOOL)readFromURL:(NSURL *)url ofType:(NSString *)type error:(NSError **)error {
    _stage = UsdStage::Open(url.fileSystemRepresentation);
    if (_stage) return YES;
    if (error) *error = [NSError errorWithDomain:@"org.openusd.vela" code:1
        userInfo:@{NSLocalizedDescriptionKey:
            [NSString stringWithFormat:@"Could not open USD stage: %@", url.lastPathComponent]}];
    return NO;
}

- (size_t)primCount {
    size_t count = 0;
    if (_stage) for (const auto &prim : _stage->Traverse()) { (void)prim; ++count; }
    return count;
}

- (void)makeWindowControllers {
    VelaWindowController *controller =
        [[VelaWindowController alloc] initWithStage:_stage primCount:[self primCount]];
    controller.window.delegate = controller;
    [self addWindowController:controller];
}

@end
