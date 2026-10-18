#import "bottom_panel.h"
#import "claude_panel.h"
#import "log_view.h"
#import "python_console.h"

#include "app/vela/core/log.h"

@implementation VelaBottomPanel {
    NSSegmentedControl *_tabs;
    NSTabView *_pages;
    VelaPythonConsoleController *_python;
    VelaLogViewController *_log;
    VelaClaudePanelController *_claude;
    // Errors and warnings logged since the Log tab was last shown.
    uint64_t _seenLogId;
    NSInteger _unseen;
}

- (instancetype)init {
    if (!(self = [super initWithNibName:nil bundle:nil])) return nil;
    _python = [VelaPythonConsoleController new];
    _log = [VelaLogViewController new];
    _claude = [VelaClaudePanelController new];
    return self;
}

- (VelaPythonConsoleController *)python { return _python; }
- (VelaClaudePanelController *)claude { return _claude; }
- (VelaPanelTab)tab { return VelaPanelTab(_tabs.selectedSegment); }

- (void)loadView {
    _tabs = [NSSegmentedControl segmentedControlWithLabels:@[@"Python", @"Log", @"Claude"]
        trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(tabChosen:)];
    _tabs.controlSize = NSControlSizeSmall;
    _tabs.segmentStyle = NSSegmentStyleTexturedRounded;
    _tabs.selectedSegment = 0;
    _pages = [NSTabView new];
    _pages.tabViewType = NSNoTabsNoBorder;
    for (NSViewController *page in @[_python, _log, _claude]) {
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
    else if (self.tab == VelaPanelTabClaude) [_claude focusInput];
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
