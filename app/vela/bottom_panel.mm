#import "bottom_panel.h"
#import "log_view.h"
#import "python_console.h"

#include "app/vela/core/log.h"

namespace {

NSViewController *ClaudePlaceholder() {
    NSTextField *title = [NSTextField labelWithString:@"The Claude console is coming."];
    title.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    NSTextField *text = [NSTextField wrappingLabelWithString:
        @"Meanwhile, Claude Code in Terminal can work with this scene over MCP: "
        @"Vela → Settings → Claude and MCP shows the command that connects it."];
    text.textColor = NSColor.secondaryLabelColor;
    text.alignment = NSTextAlignmentCenter;
    text.preferredMaxLayoutWidth = 440;
    NSStackView *stack = [NSStackView stackViewWithViews:@[title, text]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.spacing = 6;
    NSView *root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 800, 220)];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:root.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:root.centerYAnchor]]];
    NSViewController *controller = [NSViewController new];
    controller.view = root;
    return controller;
}

} // namespace

@implementation VelaBottomPanel {
    NSSegmentedControl *_tabs;
    NSTabView *_pages;
    VelaPythonConsoleController *_python;
    VelaLogViewController *_log;
    // Errors and warnings logged since the Log tab was last shown.
    uint64_t _seenLogId;
    NSInteger _unseen;
}

- (instancetype)init {
    if (!(self = [super initWithNibName:nil bundle:nil])) return nil;
    _python = [VelaPythonConsoleController new];
    _log = [VelaLogViewController new];
    return self;
}

- (VelaPythonConsoleController *)python { return _python; }
- (VelaPanelTab)tab { return VelaPanelTab(_tabs.selectedSegment); }

- (void)loadView {
    _tabs = [NSSegmentedControl segmentedControlWithLabels:@[@"Python", @"Log", @"Claude"]
        trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(tabChosen:)];
    _tabs.controlSize = NSControlSizeSmall;
    _tabs.segmentStyle = NSSegmentStyleTexturedRounded;
    _tabs.selectedSegment = 0;
    _pages = [NSTabView new];
    _pages.tabViewType = NSNoTabsNoBorder;
    for (NSViewController *page in @[_python, _log, ClaudePlaceholder()]) {
        NSTabViewItem *item = [NSTabViewItem tabViewItemWithViewController:page];
        [_pages addTabViewItem:item];
        [self addChildViewController:page];
    }
    NSBox *line = [NSBox new];
    line.boxType = NSBoxSeparator;
    NSView *root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 800, 220)];
    for (NSView *view in @[_tabs, line, _pages]) {
        view.translatesAutoresizingMaskIntoConstraints = NO;
        [root addSubview:view];
    }
    [NSLayoutConstraint activateConstraints:@[
        [_tabs.topAnchor constraintEqualToAnchor:root.topAnchor constant:5],
        [_tabs.centerXAnchor constraintEqualToAnchor:root.centerXAnchor],
        [line.topAnchor constraintEqualToAnchor:_tabs.bottomAnchor constant:5],
        [line.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [line.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [_pages.topAnchor constraintEqualToAnchor:line.bottomAnchor],
        [_pages.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [_pages.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [_pages.bottomAnchor constraintEqualToAnchor:root.bottomAnchor]]];
    self.view = root;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(logChanged:)
                                               name:VelaLogChanged object:nil];
    [self markLogSeen];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)tabChosen:(id)sender {
    [self showTab:VelaPanelTab(_tabs.selectedSegment)];
}

- (void)showTab:(VelaPanelTab)tab {
    (void)self.view;
    _tabs.selectedSegment = tab;
    [_pages selectTabViewItemAtIndex:tab];
    if (tab == VelaPanelTabLog) [self markLogSeen];
}

- (void)focusTab {
    if (self.tab == VelaPanelTabPython) [_python focusInput];
    else [self.view.window makeFirstResponder:_pages.selectedTabViewItem.view];
}

// The Log tab counts errors and warnings it hasn't shown yet.
- (void)logChanged:(NSNotification *)notification {
    if (self.tab == VelaPanelTabLog && self.view.window && !self.view.hiddenOrHasHiddenAncestor) {
        [self markLogSeen];
        return;
    }
    for (const vela::LogEntry &entry : vela::Log::Get().GetEntries(_seenLogId)) {
        if (entry.level != vela::LogEntry::Level::Info) ++_unseen;
        _seenLogId = entry.id;
    }
    [self showLogBadge];
}

- (void)markLogSeen {
    const std::vector<vela::LogEntry> entries = vela::Log::Get().GetEntries(_seenLogId);
    if (!entries.empty()) _seenLogId = entries.back().id;
    _unseen = 0;
    [self showLogBadge];
}

- (void)showLogBadge {
    [_tabs setLabel:_unseen ? [NSString stringWithFormat:@"Log (%ld)", (long)_unseen] : @"Log"
         forSegment:VelaPanelTabLog];
}

@end
