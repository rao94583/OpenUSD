// Window → Python Console: an interactive interpreter for one document,
// replacing usdview's. Input runs through vela::RunPython, as MCP's
// run_python does, with the window's document current; each input's edits
// are one undo step.
#import <AppKit/AppKit.h>

@interface VelaPythonConsoleController : NSWindowController
- (instancetype)initWithSessionId:(int)sessionId;
// Runs code as if typed and entered, for tests.
- (void)runCode:(NSString *)code;
// The transcript so far.
@property (nonatomic, readonly) NSString *transcript;
@end
