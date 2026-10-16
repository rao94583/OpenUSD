// The panel under a document window's viewport: tabs for the Python
// console, the Log and (in Phase 7) the Claude console.
#import <AppKit/AppKit.h>

@class VelaPythonConsoleController;

typedef NS_ENUM(NSInteger, VelaPanelTab) {
    VelaPanelTabPython,
    VelaPanelTabLog,
    VelaPanelTabClaude,
};

@interface VelaBottomPanel : NSViewController
@property (nonatomic, readonly) VelaPythonConsoleController *python;
@property (nonatomic, readonly) VelaPanelTab tab;
- (void)showTab:(VelaPanelTab)tab;
// Moves keyboard focus into the tab (the console's input, the log's
// table).
- (void)focusTab;
@end
