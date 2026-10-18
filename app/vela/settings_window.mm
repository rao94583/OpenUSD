#import "settings_window.h"
#import "document.h"
#import "scripting.h"
#import "image_generation.h"

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

// The Claude panel's model choices: the CLI's default, or an alias for the
// latest of a family.
NSArray<NSString *> *ModelTitles() {
    return @[@"Claude Code's default", @"Opus", @"Sonnet", @"Haiku", @"Fable"];
}
NSArray<NSString *> *ModelValues() {
    return @[@"", @"opus", @"sonnet", @"haiku", @"fable"];
}

} // namespace

@implementation VelaSettingsWindowController {
    NSButton *_loadPayloads;
    NSButton *_primaryCamera;
    NSButton *_unloadedAsBounds;
    NSTextField *_viewSummary;
    NSButton *_useCurrentView;
    NSTextField *_threadLimit;
    NSButton *_mcpServer;
    NSTextField *_mcpStatus;
    NSTextField *_mcpCommand;
    NSPopUpButton *_claudeModel;
    NSButton *_claudeAsks;
    NSTextField *_claudePath;
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
    _mcpServer = checkbox(@"Let Claude Code and other MCP clients use Vela");
    _mcpStatus = Note(@"");
    // The command that adds the helper to Claude Code, to copy.
    _mcpCommand = [NSTextField textFieldWithString:VelaMcpAddCommand()];
    _mcpCommand.editable = NO;
    _mcpCommand.selectable = YES;
    _mcpCommand.font = [NSFont monospacedSystemFontOfSize:NSFont.smallSystemFontSize
                                                   weight:NSFontWeightRegular];
    _mcpCommand.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [_mcpCommand.widthAnchor constraintEqualToConstant:300].active = YES;
    NSButton *copyCommand = [NSButton buttonWithTitle:@"Copy"
                                               target:self action:@selector(copyMcpCommand:)];
    NSStackView *command = [NSStackView stackViewWithViews:@[_mcpCommand, copyCommand]];
    // The Claude panel: model, approvals and where Claude Code is.
    _claudeModel = [NSPopUpButton new];
    for (NSString *title in ModelTitles()) [_claudeModel addItemWithTitle:title];
    _claudeModel.target = self;
    _claudeModel.action = @selector(optionChanged:);
    NSStackView *model = [NSStackView stackViewWithViews:@[
        [NSTextField labelWithString:@"Model:"], _claudeModel]];
    _claudeAsks = checkbox(@"Ask before Claude edits the scene");
    _claudePath = Note(@"");
    NSButton *choosePath = [NSButton buttonWithTitle:@"Choose…" target:self
                                              action:@selector(chooseClaude:)];
    NSButton *automaticPath = [NSButton buttonWithTitle:@"Find Automatically" target:self
                                                 action:@selector(findClaude:)];
    NSStackView *pathButtons = [NSStackView stackViewWithViews:@[choosePath, automaticPath]];
    NSButton *resetAll = [NSButton buttonWithTitle:@"Reset All Settings"
                                            target:self action:@selector(resetAll:)];

    NSStackView *viewButtons = [NSStackView stackViewWithViews:@[_useCurrentView, restoreView]];
    NSStackView *threads = [NSStackView stackViewWithViews:@[
        [NSTextField labelWithString:@"Thread limit:"], _threadLimit]];
    NSTextField *opening = Heading(@"Opening Files"), *windows = Heading(@"New Windows"),
                *performance = Heading(@"Performance"), *mcp = Heading(@"Claude and MCP");
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
        mcp,
        model,
        _claudeAsks,
        Note(@"Otherwise Claude edits at once and the panel says what changed. Either "
             @"way, each request is one step that Undo reverts."),
        _claudePath, pathButtons,
        [NSButton buttonWithTitle:@"Image Generation Settings…" target:self action:@selector(imageSettings:)],
        _mcpServer, _mcpStatus,
        Note(@"To use Vela from Claude Code in Terminal, add it as an MCP server:"),
        command,
        resetAll,
    ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 8;
    // Space before each section and before the reset button.
    for (NSView *view in @[windows, performance, mcp, resetAll]) {
        [stack setCustomSpacing:18 afterView:stack.views[[stack.views indexOfObject:view] - 1]];
    }
    stack.edgeInsets = NSEdgeInsetsMake(20, 20, 20, 20);
    window.contentView = stack;
    // The window fits its content.
    [window setContentSize:stack.fittingSize];

    __weak VelaSettingsWindowController *weakSelf = self;
    _observer = vela::AppSettingsStore::Get().AddObserver([weakSelf] { [weakSelf refresh]; });
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(refresh)
                                               name:VelaMcpStatusChanged object:nil];
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
    _unloadedAsBounds.state = s.view.unloadedAsBounds ? NSControlStateValueOn : NSControlStateValueOff;
    const std::vector<std::string> changes = vela::DescribeChanges(s.view);
    _viewSummary.stringValue = changes.empty()
        ? @"New windows start with usdview's view settings."
        : [NSString stringWithFormat:@"New windows start with usdview's view settings, except %@.",
           @(TfStringJoin(changes, ", ").c_str())];
    _threadLimit.stringValue = [NSString stringWithFormat:@"%d", s.threadLimit];
    _mcpServer.state = s.mcpServer ? NSControlStateValueOn : NSControlStateValueOff;
    _mcpStatus.stringValue = VelaMcpStatus();
    [_claudeModel selectItemAtIndex:0];
    for (NSUInteger i = 0; i < ModelValues().count; ++i) {
        if ([ModelValues()[i] isEqualToString:@(s.claudeModel.c_str())]) {
            [_claudeModel selectItemAtIndex:NSInteger(i)];
        }
    }
    _claudeAsks.state = s.claudeAsksBeforeEdits ? NSControlStateValueOn : NSControlStateValueOff;
    _claudePath.stringValue = s.claudePath.empty()
        ? @"Vela finds Claude Code by itself."
        : [NSString stringWithFormat:@"Claude Code: %@", @(s.claudePath.c_str())];
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
    const bool mcp = _mcpServer.state == NSControlStateValueOn;
    const bool asks = _claudeAsks.state == NSControlStateValueOn;
    const NSInteger modelIndex = MAX(0, _claudeModel.indexOfSelectedItem);
    const std::string model = ModelValues()[NSUInteger(modelIndex)].UTF8String;
    [self update:^(vela::AppSettings &s) {
        s.claudeAsksBeforeEdits = asks;
        s.claudeModel = model;
        s.mcpServer = mcp;
        s.loadPayloads = payloads;
        s.usePrimaryCamera = camera;
        s.view.unloadedAsBounds = bounds;
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

- (void)copyMcpCommand:(id)sender {
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:_mcpCommand.stringValue
                                      forType:NSPasteboardTypeString];
}

// The claude executable, for installs Vela doesn't find.
- (void)chooseClaude:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.message = @"Choose the claude program (Claude Code).";
    panel.showsHiddenFiles = YES;
    panel.treatsFilePackagesAsDirectories = YES;
    panel.directoryURL = [NSURL fileURLWithPath:[NSHomeDirectory()
        stringByAppendingPathComponent:@".local/bin"]];
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
        if (response != NSModalResponseOK) return;
        const std::string path = panel.URL.fileSystemRepresentation;
        [self update:^(vela::AppSettings &s) { s.claudePath = path; }];
    }];
}

- (void)imageSettings:(id)sender { VelaShowImageSettings(); }

- (void)findClaude:(id)sender {
    [self update:^(vela::AppSettings &s) { s.claudePath.clear(); }];
}

- (void)resetAll:(id)sender {
    vela::AppSettingsStore::Get().Reset();
}

@end
