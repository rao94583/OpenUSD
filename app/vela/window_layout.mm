#import "window_layout.h"

namespace {
NSView *Pane(NSView *content, NSRect frame) {
    NSView *pane = [[NSView alloc] initWithFrame:frame];
    pane.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [pane addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [content.leadingAnchor constraintEqualToAnchor:pane.leadingAnchor],
        [content.trailingAnchor constraintEqualToAnchor:pane.trailingAnchor],
        [content.topAnchor constraintEqualToAnchor:pane.topAnchor],
        [content.bottomAnchor constraintEqualToAnchor:pane.bottomAnchor]]];
    return pane;
}
}
NSView *VelaDocumentLayout(NSView *outline, NSView *viewport,
                           NSView *inspector, NSView *timeline) {
    NSView *root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1200, 800)];
    NSSplitView *split = [[NSSplitView alloc] initWithFrame:NSMakeRect(0, 0, 1200, 760)];
    split.vertical = YES; split.dividerStyle = NSSplitViewDividerStyleThin;
    [split addSubview:Pane(outline, NSMakeRect(0, 0, 280, 760))];
    [split addSubview:Pane(viewport, NSMakeRect(281, 0, 580, 760))];
    [split addSubview:Pane(inspector, NSMakeRect(862, 0, 338, 760))];
    split.translatesAutoresizingMaskIntoConstraints = NO;
    timeline.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:split]; [root addSubview:timeline];
    [NSLayoutConstraint activateConstraints:@[
        [split.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [split.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [split.topAnchor constraintEqualToAnchor:root.topAnchor],
        [split.bottomAnchor constraintEqualToAnchor:timeline.topAnchor],
        [timeline.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [timeline.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [timeline.bottomAnchor constraintEqualToAnchor:root.bottomAnchor]]];
    [split adjustSubviews];
    return root;
}
BOOL VelaCheckDocumentLayout(NSView *outline, NSView *inspector) {
    NSView *timeline = [NSView new];
    [timeline.heightAnchor constraintEqualToConstant:32].active = YES;
    NSView *root = VelaDocumentLayout(outline, [NSView new], inspector, timeline);
    [root layoutSubtreeIfNeeded];
    const CGFloat oldWidth = outline.frame.size.width;
    root.frame = NSMakeRect(0, 0, 2000, 1100);
    [root layoutSubtreeIfNeeded];
    NSSplitView *split = (NSSplitView *)root.subviews.firstObject;
    [split adjustSubviews]; [root layoutSubtreeIfNeeded];
    for (NSView *content in @[outline, inspector]) {
        const NSRect a = content.frame, b = content.superview.bounds;
        if (fabs(a.origin.x - b.origin.x) > 0.5 || fabs(a.origin.y - b.origin.y) > 0.5 ||
            fabs(a.size.width - b.size.width) > 0.5 || fabs(a.size.height - b.size.height) > 0.5)
            return NO;
    }
    if (outline.frame.size.width <= oldWidth) return NO;
    if (fabs(split.frame.size.width - root.bounds.size.width) > 0.5) return NO;
    // Narrow again: panes must continue to fill their split-view regions.
    root.frame = NSMakeRect(0, 0, 1000, 650);
    [root layoutSubtreeIfNeeded]; [split adjustSubviews]; [root layoutSubtreeIfNeeded];
    return fabs(outline.frame.size.width - outline.superview.bounds.size.width) < 0.5 &&
           fabs(inspector.frame.size.width - inspector.superview.bounds.size.width) < 0.5;
}
