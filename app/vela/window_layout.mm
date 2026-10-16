#import "window_layout.h"
#import "bottom_panel.h"

#include <initializer_list>
#include <string>
#include <algorithm>

namespace {

// The default sizes, in points.
constexpr CGFloat OutlineWidth = 340, InspectorWidth = 380, PanelHeight = 220;

NSViewController *Holding(NSView *view) {
    NSViewController *controller = [NSViewController new];
    controller.view = view;
    return controller;
}

} // namespace

@interface VelaDocumentLayout () <NSSplitViewDelegate>
@end

@implementation VelaDocumentLayout {
    NSSplitViewController *_columns;  // prim browser | viewport | inspector
    // The columns above the bottom panel. A plain split view: a split view
    // controller resizes the window to fit an item it expands.
    NSSplitView *_rows;
    NSView *_panel;
    CGFloat _panelHeight;  // to restore when expanding
    NSSplitViewItem *_outlineItem, *_viewportItem, *_inspectorItem;
    NSView *_timeline;
}

- (instancetype)initWithOutline:(NSView *)outline viewport:(NSView *)viewport
                      inspector:(NSView *)inspector bottomPanel:(NSView *)bottomPanel
                       timeline:(NSView *)timeline {
    if (!(self = [super initWithNibName:nil bundle:nil])) return nil;
    _timeline = timeline;
    _columns = [NSSplitViewController new];
    _columns.splitView.vertical = YES;
    _columns.splitView.dividerStyle = NSSplitViewDividerStyleThin;
    _outlineItem = [NSSplitViewItem sidebarWithViewController:Holding(outline)];
    _viewportItem = [NSSplitViewItem splitViewItemWithViewController:Holding(viewport)];
    if (@available(macOS 14.0, *)) {
        _inspectorItem = [NSSplitViewItem inspectorWithViewController:Holding(inspector)];
    } else {
        _inspectorItem = [NSSplitViewItem splitViewItemWithViewController:Holding(inspector)];
    }
    // Resizing the window resizes the viewport; the sidebars keep their
    // widths (the lowest holding priority gives way first).
    for (NSSplitViewItem *side in @[_outlineItem, _inspectorItem]) {
        side.canCollapse = YES;
        side.holdingPriority = NSLayoutPriorityDefaultLow + 10;
        // Sidebar and inspector items cap their widths by default.
        side.maximumThickness = NSSplitViewItemUnspecifiedDimension;
        if (@available(macOS 14.0, *)) side.canCollapseFromWindowResize = NO;
    }
    _outlineItem.minimumThickness = 200;
    _inspectorItem.minimumThickness = 260;
    _viewportItem.minimumThickness = 240;
    _viewportItem.holdingPriority = NSLayoutPriorityDefaultLow - 10;
    for (NSSplitViewItem *item in @[_outlineItem, _viewportItem, _inspectorItem]) {
        [_columns addSplitViewItem:item];
    }

    _panel = bottomPanel;
    _panelHeight = PanelHeight;
    _rows = [[NSSplitView alloc] initWithFrame:NSMakeRect(0, 0, 1440, 848)];
    _rows.vertical = NO;
    _rows.dividerStyle = NSSplitViewDividerStyleThin;
    _rows.delegate = self;
    NSView *columns = _columns.view;
    columns.frame = NSMakeRect(0, 0, 1440, 848);
    _panel.frame = NSMakeRect(0, 0, 1440, PanelHeight);
    [_rows addSubview:columns];
    [_rows addSubview:_panel];
    // Window resizing goes to the columns; the panel keeps its height.
    [_rows setHoldingPriority:NSLayoutPriorityDefaultLow - 10 forSubviewAtIndex:0];
    [_rows setHoldingPriority:NSLayoutPriorityDefaultLow + 10 forSubviewAtIndex:1];
    [self addChildViewController:_columns];
    return self;
}

- (void)loadView {
    NSView *root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1440, 880)];
    NSView *split = _rows;
    split.translatesAutoresizingMaskIntoConstraints = NO;
    _timeline.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:split];
    [root addSubview:_timeline];
    // The timeline keeps its height; the panes take the rest. (A stack
    // view hugs through its own priority, not content hugging.)
    if ([_timeline isKindOfClass:[NSStackView class]]) {
        NSStackView *stack = (NSStackView *)_timeline;
        [stack setHuggingPriority:NSLayoutPriorityRequired
                   forOrientation:NSLayoutConstraintOrientationVertical];
        // Horizontally it must not hug above the window's size (500).
        [stack setHuggingPriority:NSLayoutPriorityDefaultLow
                   forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
    [_timeline setContentHuggingPriority:NSLayoutPriorityRequired
                          forOrientation:NSLayoutConstraintOrientationVertical];
    [split setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];
    [NSLayoutConstraint activateConstraints:@[
        [split.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [split.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [split.topAnchor constraintEqualToAnchor:root.topAnchor],
        [split.bottomAnchor constraintEqualToAnchor:_timeline.topAnchor],
        [_timeline.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [_timeline.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [_timeline.bottomAnchor constraintEqualToAnchor:root.bottomAnchor]]];
    self.view = root;
}

- (NSSplitViewItem *)itemFor:(VelaPane)pane {
    return pane == VelaPanePrimBrowser ? _outlineItem : _inspectorItem;
}

- (BOOL)isCollapsed:(VelaPane)pane {
    if (pane == VelaPaneBottomPanel) {
        return [_rows isSubviewCollapsed:_panel] || NSHeight(_panel.frame) < 1;
    }
    return [self itemFor:pane].isCollapsed;
}

- (void)setPane:(VelaPane)pane collapsed:(BOOL)collapsed animated:(BOOL)animated {
    if ([self isCollapsed:pane] == collapsed) return;
    if (pane == VelaPaneBottomPanel) {
        [_rows layoutSubtreeIfNeeded];
        const CGFloat height = NSHeight(_rows.bounds);
        if (collapsed) _panelHeight = NSHeight(_panel.frame);
        const CGFloat position = collapsed ? height
            : height - std::min(_panelHeight, height - 200) - _rows.dividerThickness;
        [_rows setPosition:position ofDividerAtIndex:0];
        [_rows adjustSubviews];
        return;
    }
    NSSplitViewItem *item = [self itemFor:pane];
    if (animated) item.animator.collapsed = collapsed;
    else item.collapsed = collapsed;
}

- (void)applyDefaultSizes {
    [self.view layoutSubtreeIfNeeded];
    // Closed until asked for.
    [_rows setPosition:NSHeight(_rows.bounds) ofDividerAtIndex:0];
    [_rows adjustSubviews];
    NSSplitView *columns = _columns.splitView;
    [columns setPosition:OutlineWidth ofDividerAtIndex:0];
    [columns setPosition:NSWidth(columns.bounds) - InspectorWidth - columns.dividerThickness
        ofDividerAtIndex:1];
}

// The bottom panel collapses when dragged to the bottom, and only it.
- (BOOL)splitView:(NSSplitView *)splitView canCollapseSubview:(NSView *)subview {
    return subview == _panel;
}
- (CGFloat)splitView:(NSSplitView *)splitView constrainMinCoordinate:(CGFloat)proposed
         ofSubviewAt:(NSInteger)index {
    return std::max(proposed, CGFloat(200));  // the columns' least height
}
- (CGFloat)splitView:(NSSplitView *)splitView constrainMaxCoordinate:(CGFloat)proposed
         ofSubviewAt:(NSInteger)index {
    return std::min(proposed, NSHeight(splitView.bounds) - 100);  // the panel's
}
- (BOOL)splitView:(NSSplitView *)splitView shouldAdjustSizeOfSubview:(NSView *)view {
    return view != _panel || [splitView isSubviewCollapsed:_panel];
}

@end

BOOL VelaCheckDocumentLayout(NSView *outline, NSView *inspector) {
    NSView *timeline = [NSView new];
    [timeline.heightAnchor constraintEqualToConstant:32].active = YES;
    NSView *viewport = [NSView new];
    VelaBottomPanel *panel = [VelaBottomPanel new];
    VelaDocumentLayout *layout = [[VelaDocumentLayout alloc] initWithOutline:outline
        viewport:viewport inspector:inspector bottomPanel:panel.view timeline:timeline];
    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1440, 880)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:YES];
    window.releasedWhenClosed = NO;
    window.contentView = layout.view;  // as document windows do
    [window setContentSize:NSMakeSize(1440, 880)];
    [layout applyDefaultSizes];
    auto close = [](CGFloat a, CGFloat b) { return fabs(a - b) <= 1.0; };
    auto fail = [&](const char *what) {
        fprintf(stderr, "Layout check failed: %s (prim browser %.1f, viewport %.1f x %.1f, inspector %.1f)\n",
                what, NSWidth(outline.frame), NSWidth(viewport.frame), NSHeight(viewport.frame),
                NSWidth(inspector.frame));
        [window close];
        return NO;
    };
    const CGFloat outlineWidth = NSWidth(outline.frame), inspectorWidth = NSWidth(inspector.frame);
    const CGFloat viewportWidth = NSWidth(viewport.frame);
    if (!close(outlineWidth, OutlineWidth) || !close(inspectorWidth, InspectorWidth)) return fail("default widths");
    // The panes fill the window above the timeline.
    if (!close(NSHeight(viewport.frame), 880 - 32)) return fail("viewport height");
    // Widening gives the viewport the extra width; the sidebars keep theirs.
    [window setContentSize:NSMakeSize(2000, 1100)];
    [layout.view layoutSubtreeIfNeeded];
    if (!close(NSWidth(outline.frame), outlineWidth) ||
        !close(NSWidth(inspector.frame), inspectorWidth) ||
        NSWidth(viewport.frame) <= viewportWidth) return fail("widening");
    // Collapsing a sidebar gives its width to the viewport; expanding brings
    // it back.
    const CGFloat wide = NSWidth(viewport.frame);
    [layout setPane:VelaPanePrimBrowser collapsed:YES animated:NO];
    [layout.view layoutSubtreeIfNeeded];
    if (![layout isCollapsed:VelaPanePrimBrowser] || NSWidth(viewport.frame) <= wide) return fail("collapsing the prim browser");
    [layout setPane:VelaPanePrimBrowser collapsed:NO animated:NO];
    [layout setPane:VelaPaneInspector collapsed:YES animated:NO];
    [layout.view layoutSubtreeIfNeeded];
    if (![layout isCollapsed:VelaPaneInspector] || [layout isCollapsed:VelaPanePrimBrowser] ||
        !close(NSWidth(outline.frame), outlineWidth)) return fail("collapsing the inspector");
    [layout setPane:VelaPaneInspector collapsed:NO animated:NO];
    [layout.view layoutSubtreeIfNeeded];
    if (!close(NSWidth(inspector.frame), inspectorWidth)) return fail("expanding the inspector");
    // The bottom panel starts closed and opens at its default height.
    if (![layout isCollapsed:VelaPaneBottomPanel]) return fail("bottom panel closed");
    const CGFloat tall = NSHeight(viewport.frame);
    [layout setPane:VelaPaneBottomPanel collapsed:NO animated:NO];
    [layout.view layoutSubtreeIfNeeded];
    if (!close(tall - NSHeight(viewport.frame), PanelHeight + 1) &&
        !close(tall - NSHeight(viewport.frame), PanelHeight)) return fail("opening the bottom panel");
    // No tab's content resizes the window.
    for (VelaPanelTab tab : {VelaPanelTabLog, VelaPanelTabClaude, VelaPanelTabPython}) {
        [panel showTab:tab];
        [layout.view layoutSubtreeIfNeeded];
        if (!close(NSWidth(window.contentView.frame), 2000)) {
            const char *names[] = {"Python", "Log", "Claude"};
            return fail((std::string("the ") + names[tab] + " tab's width").c_str());
        }
    }
    // Narrowing keeps the panes filling their regions.
    [window setContentSize:NSMakeSize(1000, 650)];
    [layout.view layoutSubtreeIfNeeded];
    const BOOL filled = close(NSWidth(outline.frame), NSWidth(outline.superview.bounds)) &&
                        close(NSWidth(inspector.frame), NSWidth(inspector.superview.bounds));
    if (!filled) return fail("narrowing");
    [window close];
    return YES;
}
