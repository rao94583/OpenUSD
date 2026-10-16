// A document window's panes: the prim browser (left sidebar), the viewport,
// the inspector (right sidebar) and the bottom panel (Python, Log,
// Claude), above the timeline. Each sidebar and the bottom panel can be
// collapsed: from the View menu, the toolbar, or by dragging its divider
// to the edge. The viewport takes the window's extra space.
#import <AppKit/AppKit.h>

typedef NS_ENUM(NSInteger, VelaPane) {
    VelaPanePrimBrowser,
    VelaPaneInspector,
    VelaPaneBottomPanel,
};

@interface VelaDocumentLayout : NSViewController
- (instancetype)initWithOutline:(NSView *)outline viewport:(NSView *)viewport
                      inspector:(NSView *)inspector bottomPanel:(NSView *)bottomPanel
                       timeline:(NSView *)timeline;
- (BOOL)isCollapsed:(VelaPane)pane;
- (void)setPane:(VelaPane)pane collapsed:(BOOL)collapsed animated:(BOOL)animated;
// Sets the default widths and heights, once the view has its size.
- (void)applyDefaultSizes;
@end

// Display-independent checks of resizing and collapsing.
BOOL VelaCheckDocumentLayout(NSView *outline, NSView *inspector);
