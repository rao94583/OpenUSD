#import "document.h"
#import "browser_view.h"
#import "window_layout.h"
#import "menus.h"
#import "viewport/viewport_view.h"

#include "app/vela/core/actions.h"

#include "pxr/usd/usd/primRange.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>

PXR_NAMESPACE_USING_DIRECTIVE

// The viewport above a timeline: play, a frame slider and the status line.
// Everything goes through the action registry; the controller observes the
// document to stay in sync.
@interface VelaWindowController : NSWindowController <NSWindowDelegate>
- (instancetype)initWithDocument:(std::shared_ptr<vela::Document>)document
                       primCount:(size_t)count;
@property (nonatomic, readonly) VelaViewportView *viewport;
@end

@implementation VelaWindowController {
    std::shared_ptr<vela::Document> _document;
    int _observer;
    size_t _primCount;
    VelaViewportView *_viewport;
    VelaBrowserController *_browser;
    NSSlider *_slider;
    NSTextField *_frameLabel;
    NSTextField *_statusLabel;
    NSString *_viewportStatus;
    NSButton *_playButton;
    NSTimer *_timer;
}

- (instancetype)initWithDocument:(std::shared_ptr<vela::Document>)document
                       primCount:(size_t)count {
    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1440, 880)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                  NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    if (!(self = [super initWithWindow:window])) {
        return nil;
    }
    _document = std::move(document);
    _primCount = count;
    const bool animated = _document->IsAnimated();

    _viewport = [[VelaViewportView alloc] initWithDocument:_document];
    _playButton = [NSButton buttonWithImage:
        [NSImage imageWithSystemSymbolName:@"play.fill" accessibilityDescription:@"Play"]
        target:self action:@selector(togglePlayback:)];
    _playButton.bezelStyle = NSBezelStyleTexturedRounded;
    _slider = [NSSlider sliderWithValue:_document->GetStartTime()
                               minValue:_document->GetStartTime()
                               maxValue:_document->GetEndTime()
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
    _browser = [[VelaBrowserController alloc] initWithDocument:_document];
    [bar setContentHuggingPriority:NSLayoutPriorityRequired
                    forOrientation:NSLayoutConstraintOrientationVertical];
    window.contentView = VelaDocumentLayout(_browser.outlineView, _viewport,
                                            _browser.inspectorView, bar);
    window.initialFirstResponder = _viewport;
    window.delegate = self;
    [window center];

    __weak VelaWindowController *weakSelf = self;
    _viewport.statusChanged = ^(NSString *status) {
        VelaWindowController *strongSelf = weakSelf;
        if (strongSelf) {
            strongSelf->_viewportStatus = status;
            [strongSelf showStatus];
        }
    };
    _observer = _document->AddObserver([weakSelf](unsigned changes) {
        [weakSelf documentChanged:changes];
    });
    [self showTime];
    return self;
}

- (void)dealloc {
    _document->RemoveObserver(_observer);
    [_timer invalidate];
}

- (VelaViewportView *)viewport { return _viewport; }

- (void)run:(const char *)action params:(const JsObject &)params {
    vela::ActionResult result =
        vela::ActionRegistry::Get().Run(*_document, action, params);
    if (!result.ok) {
        NSLog(@"%s", result.error.c_str());
    }
}

- (void)showStatus {
    _statusLabel.stringValue = [NSString stringWithFormat:@"%zu prims    %@",
                                _primCount, _viewportStatus ?: @""];
}

- (void)documentChanged:(unsigned)changes {
    // Only a resync adds or removes prims.
    if (changes & vela::Document::Resync) {
        _primCount = 0;
        for (const auto &prim : _document->GetStage()->Traverse()) { (void)prim; ++_primCount; }
        [self showStatus];
    }
    if (changes & vela::Document::Time) {
        [self showTime];
    }
    if (changes & vela::Document::Playback) {
        [self showPlayback];
    }
}

- (void)showTime {
    const UsdTimeCode time = _document->GetTime();
    if (time.IsNumeric()) {
        _slider.doubleValue = time.GetValue();
        _frameLabel.stringValue = [NSString stringWithFormat:@"%g / %g",
                                   time.GetValue(), _document->GetEndTime()];
    }
}

- (void)showPlayback {
    const bool playing = _document->IsPlaying();
    [_timer invalidate];
    _timer = nil;
    if (playing) {
        __weak VelaWindowController *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / _document->GetFramesPerSecond()
                                                 repeats:YES
                                                   block:^(NSTimer *) {
            [weakSelf run:"next_frame" params:{}];
        }];
    }
    _playButton.image = [NSImage imageWithSystemSymbolName:playing ? @"pause.fill" : @"play.fill"
                                  accessibilityDescription:playing ? @"Pause" : @"Play"];
}

- (void)sliderMoved:(NSSlider *)slider {
    [self run:"set_time" params:{{"time", JsValue(std::round(slider.doubleValue))}}];
}

- (void)togglePlayback:(id)sender {
    [self run:"toggle_playback" params:{}];
}

// Shortcuts the menus did not take, e.g. while a menu is disabled.
- (void)keyDown:(NSEvent *)event {
    if (!VelaRunShortcut(event, *_document)) {
        [super keyDown:event];
    }
}

- (void)windowWillClose:(NSNotification *)notification {
    _document->SetPlaying(false);
}

@end

namespace {
std::optional<vela::LaunchOptions> &PendingLaunchOptions() {
    static std::optional<vela::LaunchOptions> options;
    return options;
}
double &LaunchTime() {
    static double time = 0.0;
    return time;
}
double Now() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

void VelaSetLaunchOptions(const vela::LaunchOptions &options) {
    PendingLaunchOptions() = options;
}
void VelaSetLaunchTime(double seconds) { LaunchTime() = seconds; }

@implementation VelaDocument {
    std::shared_ptr<vela::Document> _core;
    int _observer;
    vela::LaunchOptions _launch;
}

+ (BOOL)autosavesInPlace { return NO; }

- (instancetype)init {
    if ((self = [super init])) {
        // Undo is the core's, through the Edit menu's actions.
        self.hasUndoManager = NO;
    }
    return self;
}

- (std::shared_ptr<vela::Document>)core { return _core; }

- (VelaViewportView *)viewport {
    return ((VelaWindowController *)self.windowControllers.firstObject).viewport;
}

- (BOOL)readFromURL:(NSURL *)url ofType:(NSString *)type error:(NSError **)error {
    std::string message;
    // The command line's options apply to the file they name.
    std::error_code ignored;
    auto &pending = PendingLaunchOptions();
    if (pending && std::filesystem::equivalent(pending->file, url.fileSystemRepresentation, ignored)) {
        _launch = *pending;
        pending.reset();
    }
    _launch.file = url.fileSystemRepresentation;
    const double start = Now();
    _core = vela::OpenDocument(_launch, &message);
    if (_core && _launch.timing) {
        printf("Opened %s in %.3f s\n", url.lastPathComponent.UTF8String, Now() - start);
    }
    if (_core) {
        for (const std::string &warning : vela::ApplyLaunchOptions(_launch, *_core)) {
            fprintf(stderr, "Vela: %s\n", warning.c_str());
        }
    }
    if (_core) {
        // The window shows unsaved edits, and closing asks to save them.
        __weak VelaDocument *weakSelf = self;
        _observer = _core->AddObserver([weakSelf](unsigned changes) {
            using D = vela::Document;
            if (changes & (D::StageContent | D::UndoState | D::Saved)) [weakSelf showEditedState];
            if (changes & D::Saved) weakSelf.fileModificationDate = [NSDate date];
        });
        return YES;
    }
    if (error) *error = [NSError errorWithDomain:@"org.openusd.vela" code:1
        userInfo:@{NSLocalizedDescriptionKey:
            [NSString stringWithFormat:@"Could not open USD stage: %@", url.lastPathComponent],
                   NSLocalizedFailureReasonErrorKey: @(message.c_str())}];
    return NO;
}

- (void)dealloc {
    if (_core) _core->RemoveObserver(_observer);
}

- (void)showEditedState {
    const bool edited = _core->HasUnsavedEdits();
    if (edited != self.documentEdited) {
        [self updateChangeCount:edited ? NSChangeDone : NSChangeCleared];
    }
}

// Saving writes the edited layers in place, through the "save" action (the
// close prompt's Save comes here). Other save operations would rewrite the
// root layer as a new document; Export Flattened covers that.
- (void)saveToURL:(NSURL *)url ofType:(NSString *)typeName
    forSaveOperation:(NSSaveOperationType)operation
    completionHandler:(void (^)(NSError *))completionHandler {
    if (operation != NSSaveOperation) {
        completionHandler([NSError errorWithDomain:@"org.openusd.vela" code:2 userInfo:@{
            NSLocalizedDescriptionKey: @"Use File → Export Flattened… to write a new file."}]);
        return;
    }
    vela::ActionResult result;
    if (!_core->GetLayersToSave().empty()) {
        result = vela::ActionRegistry::Get().Run(*_core, "save");
    }
    if (result.ok && _core->HasUnsavedEdits()) {
        // In-memory layers have no file to save to.
        result = vela::ActionResult::Error("Some edited layers are in memory only; "
                                           "export them to keep their edits.");
    }
    if (!result.ok) {
        completionHandler([NSError errorWithDomain:@"org.openusd.vela" code:3 userInfo:@{
            NSLocalizedDescriptionKey: @(result.error.c_str())}]);
        return;
    }
    [self showEditedState];
    completionHandler(nil);
}

- (size_t)primCount {
    size_t count = 0;
    if (_core) for (const auto &prim : _core->GetStage()->Traverse()) { (void)prim; ++count; }
    return count;
}

- (void)makeWindowControllers {
    VelaWindowController *controller =
        [[VelaWindowController alloc] initWithDocument:_core primCount:[self primCount]];
    [self addWindowController:controller];
    // Window-level launch options.
    VelaViewportView *viewport = controller.viewport;
    viewport.renderingDisabled = _launch.norender;
    if (_launch.bboxStandin) [viewport setDrawsUnloadedAsBounds:YES];
    if (_launch.viewportWidth > 0) {
        // Grow or shrink the window by the difference.
        [controller.window.contentView layoutSubtreeIfNeeded];
        const NSSize current = viewport.bounds.size;
        NSRect frame = controller.window.frame;
        frame.size.width += _launch.viewportWidth - current.width;
        frame.size.height += _launch.viewportHeight - current.height;
        [controller.window setFrame:frame display:NO];
    }
    const bool timing = _launch.timing, quit = _launch.quitAfterStartup,
               norender = _launch.norender;
    if (timing || quit) {
        auto report = ^{
            if (timing) printf("%s %.3f s after launch\n",
                               norender ? "Started" : "First frame", Now() - LaunchTime());
            fflush(stdout);
            if (quit) [NSApp terminate:nil];
        };
        if (_launch.norender) {
            dispatch_async(dispatch_get_main_queue(), report);
        } else {
            viewport.firstFrameDrawn = report;
        }
    }
}

@end
