#import "settings_window.h"
#import "document.h"

#include "app/vela/core/actions.h"
#include "app/vela/core/app_settings.h"

#include "pxr/base/tf/stringUtils.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

NSTextField *Heading(NSString *text) {
    NSTextField *label = [NSTextField labelWithString:text];
    label.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return label;
}

NSTextField *Note(NSString *text) {
    NSTextField *label = [NSTextField wrappingLabelWithString:text];
    label.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    label.textColor = NSColor.secondaryLabelColor;
    label.preferredMaxLayoutWidth = 380;
    return label;
}

// The frontmost document window's document. Not currentDocument: while
// this window is main, that is nil.
vela::Document *FrontDocument() {
    for (NSWindow *window in NSApp.orderedWindows) {
        id document = window.windowController.document;
        if ([document isKindOfClass:[VelaDocument class]]) {
            return ((VelaDocument *)document).core.get();
        }
    }
    return nullptr;
}

} // namespace

@implementation VelaSettingsWindowController {
    NSButton *_loadPayloads;
    NSButton *_primaryCamera;
    NSButton *_unloadedAsBounds;
    NSTextField *_viewSummary;
    NSButton *_useCurrentView;
    NSTextField *_threadLimit;
    int _observer;
}

+ (instancetype)shared {
    static VelaSettingsWindowController *controller = [VelaSettingsWindowController new];
    return controller;
}

- (instancetype)init {
    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 440, 300)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
          backing:NSBackingStoreBuffered defer:YES];
    window.title = @"Settings";
    if (!(self = [super initWithWindow:window])) return nil;

    auto checkbox = [&](NSString *title) {
        return [NSButton checkboxWithTitle:title target:self action:@selector(optionChanged:)];
    };
    _loadPayloads = checkbox(@"Load payloads");
    _primaryCamera = checkbox(@"Look through the stage's primary camera (main_cam)");
    _unloadedAsBounds = checkbox(@"Draw unloaded prims as bounding boxes");
    _viewSummary = Note(@"");
    _useCurrentView = [NSButton buttonWithTitle:@"Use Current Window"
                                         target:self action:@selector(useCurrentView:)];
    NSButton *restoreView = [NSButton buttonWithTitle:@"Restore Defaults"
                                               target:self action:@selector(restoreView:)];
    _threadLimit = [NSTextField textFieldWithString:@"0"];
    _threadLimit.target = self;
    _threadLimit.action = @selector(threadLimitChanged:);
    [_threadLimit.widthAnchor constraintEqualToConstant:60].active = YES;
    NSButton *resetAll = [NSButton buttonWithTitle:@"Reset All Settings"
                                            target:self action:@selector(resetAll:)];

    NSStackView *viewButtons = [NSStackView stackViewWithViews:@[_useCurrentView, restoreView]];
    NSStackView *threads = [NSStackView stackViewWithViews:@[
        [NSTextField labelWithString:@"Thread limit:"], _threadLimit]];
    NSTextField *opening = Heading(@"Opening Files"), *windows = Heading(@"New Windows"),
                *performance = Heading(@"Performance");
    NSStackView *stack = [NSStackView stackViewWithViews:@[
        opening,
        _loadPayloads, _primaryCamera, _unloadedAsBounds,
        Note(@"Apply to files opened from now on."),
        windows,
        _viewSummary, viewButtons,
        Note(@"Use Current Window saves the frontmost window's view settings, as "
             @"View → Use as Default View Settings does. Restore Defaults returns "
             @"to usdview's."),
        performance,
        threads,
        Note(@"0 uses every core; a negative number leaves that many free. "
             @"Takes effect the next time Vela starts."),
        resetAll,
    ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 8;
    // Space before each section and before the reset button.
    for (NSView *view in @[windows, performance, resetAll]) {
        [stack setCustomSpacing:18 afterView:stack.views[[stack.views indexOfObject:view] - 1]];
    }
    stack.edgeInsets = NSEdgeInsetsMake(20, 20, 20, 20);
    window.contentView = stack;
    // The window fits its content.
    [window setContentSize:stack.fittingSize];

    __weak VelaSettingsWindowController *weakSelf = self;
    _observer = vela::AppSettingsStore::Get().AddObserver([weakSelf] { [weakSelf refresh]; });
    [self refresh];
    return self;
}

- (void)showWindow:(id)sender {
    [self refresh];
    if (!self.window.isVisible) [self.window center];
    [super showWindow:sender];
}

// Shows the stored settings.
- (void)refresh {
    const vela::AppSettings &s = vela::AppSettingsStore::Get().GetSettings();
    _loadPayloads.state = s.loadPayloads ? NSControlStateValueOn : NSControlStateValueOff;
    _primaryCamera.state = s.usePrimaryCamera ? NSControlStateValueOn : NSControlStateValueOff;
    _unloadedAsBounds.state = s.unloadedAsBounds ? NSControlStateValueOn : NSControlStateValueOff;
    const std::vector<std::string> changes = vela::DescribeChanges(s.view);
    _viewSummary.stringValue = changes.empty()
        ? @"New windows start with usdview's view settings."
        : [NSString stringWithFormat:@"New windows start with usdview's view settings, except %s.",
           TfStringJoin(changes, ", ").c_str()];
    _threadLimit.stringValue = [NSString stringWithFormat:@"%d", s.threadLimit];
    _useCurrentView.enabled = FrontDocument() != nullptr;
}

- (void)update:(void (^)(vela::AppSettings &settings))edit {
    vela::AppSettings settings = vela::AppSettingsStore::Get().GetSettings();
    edit(settings);
    vela::AppSettingsStore::Get().SetSettings(settings);
}

- (void)optionChanged:(id)sender {
    const bool payloads = _loadPayloads.state == NSControlStateValueOn;
    const bool camera = _primaryCamera.state == NSControlStateValueOn;
    const bool bounds = _unloadedAsBounds.state == NSControlStateValueOn;
    [self update:^(vela::AppSettings &s) {
        s.loadPayloads = payloads;
        s.usePrimaryCamera = camera;
        s.unloadedAsBounds = bounds;
    }];
}

- (void)threadLimitChanged:(id)sender {
    NSScanner *scanner = [NSScanner scannerWithString:_threadLimit.stringValue];
    int limit = 0;
    if ([scanner scanInt:&limit] && scanner.isAtEnd) {
        [self update:^(vela::AppSettings &s) { s.threadLimit = limit; }];
    }
    [self refresh];  // shows the stored value if the text was not a number
}

// Through the action, as from View → Use as Default View Settings.
- (void)useCurrentView:(id)sender {
    if (vela::Document *document = FrontDocument()) {
        vela::ActionRegistry::Get().Run(*document, "use_view_as_default");
    }
}

- (void)restoreView:(id)sender {
    [self update:^(vela::AppSettings &s) { s.view = vela::ViewSettings(); }];
}

- (void)resetAll:(id)sender {
    vela::AppSettingsStore::Get().Reset();
}

@end
