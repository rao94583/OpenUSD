// The bottom panel's Log tab: vela::Log's entries (USD's errors, warnings
// and status messages, failed actions, MCP calls), filtered by level and
// text. Every window shows the app's one log.
#import <AppKit/AppKit.h>

// Posted on the main thread after the log changes.
extern NSNotificationName const VelaLogChanged;
// Starts capturing USD's diagnostics and posting VelaLogChanged.
void VelaStartLog(void);

@interface VelaLogViewController : NSViewController
@end
