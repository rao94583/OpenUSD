// The bottom panel's Python tab: an interactive interpreter for one
// document, replacing usdview's. Input runs through vela::RunPython, as
// MCP's run_python does, with the window's document current; each input's
// edits are one undo step.
#import <AppKit/AppKit.h>

@interface VelaPythonConsoleController : NSViewController
// The document's id in vela::Session; set once its window is registered.
@property (nonatomic) int sessionId;
// Runs code as if typed and entered, for tests.
- (void)runCode:(NSString *)code;
// Puts the insertion point in the input.
- (void)focusInput;
// The transcript so far.
@property (nonatomic, readonly) NSString *transcript;
@end
