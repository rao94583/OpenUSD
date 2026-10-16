#import "python_console.h"

#include "app/vela/core/python.h"
#include "app/vela/core/session.h"

#include <algorithm>
#include <string>

namespace {

NSFont *ConsoleFont() {
    return [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
}

NSScrollView *Scrolling(NSTextView *text) {
    NSScrollView *scroll = [NSScrollView new];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSNoBorder;
    scroll.documentView = text;
    text.minSize = NSMakeSize(0, 0);
    text.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    text.verticallyResizable = YES;
    text.horizontallyResizable = NO;
    text.autoresizingMask = NSViewWidthSizable;
    text.textContainer.widthTracksTextView = YES;
    text.font = ConsoleFont();
    text.textContainerInset = NSMakeSize(6, 6);
    text.automaticQuoteSubstitutionEnabled = NO;
    text.automaticDashSubstitutionEnabled = NO;
    text.automaticTextReplacementEnabled = NO;
    text.automaticSpellingCorrectionEnabled = NO;
    text.continuousSpellCheckingEnabled = NO;
    text.smartInsertDeleteEnabled = NO;
    return scroll;
}

const char *const Banner =
    "Python 3.11 in Vela. vela.stage is this document's stage, shared with its window; "
    "vela.selection and vela.time can be read and assigned, and vela.run(action, **params) "
    "runs any menu command (vela.actions() lists them). Usd, Sdf, Gf, Vt, Tf, UsdGeom, "
    "UsdShade, UsdLux, UsdUtils and Kind are imported. Each input's edits are one undo step.\n"
    "Return runs a complete statement; Shift-Return starts a new line; ⌘Return runs "
    "everything; ↑ and ↓ go through history.\n\n";

} // namespace

@interface VelaPythonConsoleController () <NSTextViewDelegate>
@end

@implementation VelaPythonConsoleController {
    int _sessionId;
    NSTextView *_output;
    NSTextView *_input;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyIndex;  // == count while editing new input
    NSString *_draft;         // the new input, while browsing history
}

- (instancetype)initWithSessionId:(int)sessionId {
    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 760, 520)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                  NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    if (!(self = [super initWithWindow:window])) return nil;
    _sessionId = sessionId;
    _history = [NSMutableArray array];
    window.minSize = NSMakeSize(360, 220);
    window.tabbingMode = NSWindowTabbingModeDisallowed;
    window.frameAutosaveName = @"VelaPythonConsole";

    _output = [NSTextView new];
    _output.editable = NO;
    _output.selectable = YES;
    _output.richText = NO;
    _input = [NSTextView new];
    _input.richText = NO;
    _input.allowsUndo = YES;
    _input.delegate = self;
    NSScrollView *outputScroll = Scrolling(_output);
    NSScrollView *inputScroll = Scrolling(_input);
    _input.typingAttributes = @{NSFontAttributeName: ConsoleFont(),
                                NSForegroundColorAttributeName: NSColor.textColor};

    NSSplitView *split = [NSSplitView new];
    split.vertical = NO;
    split.dividerStyle = NSSplitViewDividerStyleThin;
    split.autosaveName = @"VelaPythonConsoleSplit";
    [split addArrangedSubview:outputScroll];
    [split addArrangedSubview:inputScroll];
    [split setHoldingPriority:NSLayoutPriorityDefaultLow - 1 forSubviewAtIndex:0];
    [split setHoldingPriority:NSLayoutPriorityDefaultLow + 1 forSubviewAtIndex:1];
    [inputScroll.heightAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
    [outputScroll.heightAnchor constraintGreaterThanOrEqualToConstant:80].active = YES;
    window.contentView = split;
    [split layoutSubtreeIfNeeded];
    [split setPosition:NSHeight(split.bounds) - 110 ofDividerAtIndex:0];
    window.initialFirstResponder = _input;
    [window center];

    [self append:@(Banner) color:NSColor.secondaryLabelColor];
    return self;
}

- (NSString *)windowTitleForDocumentDisplayName:(NSString *)displayName {
    return [NSString stringWithFormat:@"%@ — Python", displayName];
}

- (NSString *)transcript { return _output.string; }

- (void)append:(NSString *)text color:(NSColor *)color {
    if (!text.length) return;
    NSAttributedString *piece = [[NSAttributedString alloc] initWithString:text attributes:@{
        NSFontAttributeName: ConsoleFont(), NSForegroundColorAttributeName: color}];
    [_output.textStorage appendAttributedString:piece];
    [_output scrollRangeToVisible:NSMakeRange(_output.string.length, 0)];
}

// The input as the transcript shows it: a prompt before each line.
- (void)echo:(NSString *)code {
    NSMutableString *echo = [NSMutableString string];
    __block BOOL first = YES;
    [code enumerateLinesUsingBlock:^(NSString *line, BOOL *) {
        [echo appendFormat:@"%@%@\n", first ? @">>> " : @"... ", line];
        first = NO;
    }];
    [self append:echo color:NSColor.secondaryLabelColor];
}

- (void)runCode:(NSString *)code {
    if (!code.length) return;
    [self echo:code];
    if (![_history.lastObject isEqualToString:code]) [_history addObject:code];
    _historyIndex = NSInteger(_history.count);
    _draft = nil;
    const std::string key = "console:" + std::to_string(_sessionId);
    const vela::PythonResult result = vela::RunPython(code.UTF8String, key, _sessionId);
    NSString *output = @(result.output.c_str());
    [self append:output color:NSColor.textColor];
    if (output.length && ![output hasSuffix:@"\n"]) [self append:@"\n" color:NSColor.textColor];
    if (result.ok) {
        if (!result.value.empty()) {
            [self append:[@(result.value.c_str()) stringByAppendingString:@"\n"]
                   color:NSColor.textColor];
        }
    } else {
        [self append:@(result.error.c_str()) color:NSColor.systemRedColor];
    }
}

- (void)setInput:(NSString *)text {
    _input.string = text;
    _input.selectedRange = NSMakeRange(text.length, 0);
}

- (void)runInput {
    NSString *code = _input.string;
    // Trailing blank lines end a block; they are not part of the code.
    code = [code stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    [self setInput:@""];
    [self runCode:code];
}

// Whether the insertion point is on the input's first (or last) line.
- (BOOL)caretOnEdgeLine:(BOOL)first {
    NSString *text = _input.string;
    const NSRange selection = _input.selectedRange;
    if (selection.length) return NO;
    const NSRange newline = first
        ? [text rangeOfString:@"\n" options:0 range:NSMakeRange(0, selection.location)]
        : [text rangeOfString:@"\n" options:0
                        range:NSMakeRange(selection.location, text.length - selection.location)];
    return newline.location == NSNotFound;
}

- (void)browseHistory:(NSInteger)step {
    const NSInteger count = NSInteger(_history.count);
    const NSInteger next = std::clamp(_historyIndex + step, NSInteger(0), count);
    if (next == _historyIndex) return;
    if (_historyIndex == count) _draft = _input.string;
    _historyIndex = next;
    [self setInput:next == count ? (_draft ?: @"") : _history[NSUInteger(next)]];
}

- (BOOL)textView:(NSTextView *)textView doCommandBySelector:(SEL)selector {
    const NSEventModifierFlags flags = NSApp.currentEvent.modifierFlags;
    if (selector == @selector(insertNewline:)) {
        if (flags & NSEventModifierFlagShift) return NO;  // a new line
        NSString *code = textView.string;
        if ((flags & NSEventModifierFlagCommand) || [self shouldRun:code]) {
            [self runInput];
            return YES;
        }
        // Continue the block, keeping the indentation.
        [textView insertText:[@"\n" stringByAppendingString:[self indentAfter:code]]
            replacementRange:textView.selectedRange];
        return YES;
    }
    if (selector == @selector(moveUp:) && [self caretOnEdgeLine:YES]) {
        [self browseHistory:-1];
        return YES;
    }
    if (selector == @selector(moveDown:) && [self caretOnEdgeLine:NO]) {
        [self browseHistory:1];
        return YES;
    }
    if (selector == @selector(insertTab:)) {
        [textView insertText:@"    " replacementRange:textView.selectedRange];
        return YES;
    }
    return NO;
}

// Return runs a single complete statement at once. A block runs, as in
// Python's interpreter, on a blank line, or once its last line is back at
// the left margin and the whole is complete (pasted statements).
- (BOOL)shouldRun:(NSString *)code {
    if (![code containsString:@"\n"]) return vela::IsCompletePython(code.UTF8String);
    NSString *last = [code componentsSeparatedByString:@"\n"].lastObject;
    if (![last stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet].length) {
        return YES;
    }
    return ![last hasPrefix:@" "] && ![last hasPrefix:@"\t"] && ![last hasSuffix:@":"] &&
        vela::IsCompletePython(code.UTF8String);
}

// The next line's indentation: the current line's, one level deeper after
// a colon.
- (NSString *)indentAfter:(NSString *)code {
    NSString *last = [code componentsSeparatedByString:@"\n"].lastObject;
    NSUInteger spaces = 0;
    while (spaces < last.length && [last characterAtIndex:spaces] == ' ') ++spaces;
    NSString *trimmed = [last stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if ([trimmed hasSuffix:@":"]) spaces += 4;
    return [@"" stringByPaddingToLength:spaces withString:@" " startingAtIndex:0];
}

@end
