// The panel under a document window's viewport: tabs for the Python
// console, the Log and the Claude panel.
#import <AppKit/AppKit.h>

@class VelaClaudePanelController;
@class VelaPythonConsoleController;

typedef NS_ENUM(NSInteger, VelaPanelTab) {
    VelaPanelTabPython,
    VelaPanelTabLog,
    VelaPanelTabClaude,
};

@interface VelaBottomPanel : NSViewController
@property (nonatomic, readonly) VelaPythonConsoleController *python;
@property (nonatomic, readonly) VelaClaudePanelController *claude;
@property (nonatomic, readonly) VelaPanelTab tab;
- (void)showTab:(VelaPanelTab)tab;
// Moves keyboard focus into the tab (the console's or Claude's input,
// the log's table).
- (void)focusTab;
@end
