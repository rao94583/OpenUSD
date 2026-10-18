// The bottom panel's Claude tab: a conversation with Claude Code about the
// window's document. The claude CLI runs as a subprocess (see
// core/claude_cli.h) with the app's MCP tools bound to this document, so
// Claude can use the whole pxr API and every action. Replies stream in;
// tool calls show with their results and screenshots; approvals appear in
// the panel; each request is one undo step; conversations are kept and can
// be resumed.
#import <AppKit/AppKit.h>

@interface VelaClaudePanelController : NSViewController
// The document's id in vela::Session; set once its window is registered.
@property (nonatomic) int sessionId;
// Whether a request is running.
@property (nonatomic, readonly, getter=isBusy) BOOL busy;
// Sends a request as if typed; NO (with a reason) while one is running or
// the CLI can't start.
- (BOOL)ask:(NSString *)request error:(NSString **)error;
// Stops the running request, as the Stop button does.
- (void)stopRequest;
// Puts the insertion point in the input.
- (void)focusInput;
// Ends the conversation's CLI, e.g. as the window closes.
- (void)shutDown;
@end

// Registers ask_claude, which sends a request to a document's panel (the
// palette's "Ask Claude", and scripts), and stop_claude.
void VelaRegisterClaudeActions(void);
