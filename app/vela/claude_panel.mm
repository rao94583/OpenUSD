#include "app/vela/core/image_jobs.h"
#import "claude_panel.h"
#import "claude_markdown.h"
#import "document.h"

#import <objc/runtime.h>

#include "app/vela/core/actions.h"
#include "app/vela/core/app_settings.h"
#include "app/vela/core/claude_cli.h"
#include "app/vela/core/claude_history.h"
#include "app/vela/core/claude_process.h"
#include "app/vela/core/document.h"
#include "app/vela/core/log.h"
#include "app/vela/core/mcp.h"
#include "app/vela/core/mcp_path.h"
#include "app/vela/core/mcp_socket.h"
#include "app/vela/core/session.h"

#include "pxr/base/js/json.h"
#include "pxr/usd/sdf/layer.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

std::string Text(NSString *string) { return string.UTF8String ?: ""; }
NSString *String(const std::string &text) { return @(text.c_str()) ?: @""; }

const CGFloat FontSize = 12.5;

const char *const SystemPrompt =
    "You are working inside Vela, a native macOS viewer and editor for USD "
    "scenes, in its Claude panel next to the user's open scene. The vela MCP "
    "tools act on that scene, live: query_python answers questions with the "
    "full pxr API and never edits; run_python edits (Usd, Sdf, UsdGeom, "
    "UsdShade, UsdLux and the rest of pxr, plus vela.stage, vela.selection, "
    "vela.time and vela.run(action, **params)); list_actions and ui_action "
    "run the app's commands (select, frame, cameras, draw modes, export "
    "...); get_app_state reports the document; viewport_screenshot shows "
    "you the render. Each of the user's requests becomes one undo step, so "
    "edit directly rather than asking for confirmation, author to the "
    "current edit target, and say briefly what you changed. Each message "
    "starts with a <vela-context> block describing the document's state "
    "when it was sent. The panel is small: keep answers short and use "
    "Markdown sparingly.";

// The shared PATH for the CLI and the tools it runs: the app's is minimal
// when launched from Finder.
std::string ChildPath(const std::string &executable) {
    std::string path = executable.substr(0, executable.rfind('/'));
    for (const char *dir : {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin",
                            "/usr/sbin", "/sbin"}) {
        path += std::string(":") + dir;
    }
    return path;
}

// Finds the claude CLI and checks its version; returns its path, or "" with
// a message.
std::string LocateClaude(std::string *error) {
    static std::map<std::string, vela::claude::Version> checked;
    std::vector<std::string> candidates;
    if (const char *forced = getenv("VELA_CLAUDE"); forced && *forced) {
        candidates.push_back(forced);
    } else if (const std::string &setting = vela::AppSettingsStore::Get().GetSettings().claudePath;
               !setting.empty()) {
        candidates.push_back(setting);
    } else {
        candidates = vela::claude::CandidatePaths(Text(NSHomeDirectory()));
        // Where a login shell finds it, for other installs.
        std::string found;
        const char *shell = getenv("SHELL");
        if (vela::RunAndCapture({shell && *shell ? shell : "/bin/zsh", "-lc", "command -v claude"},
                                &found, 5)) {
            while (!found.empty() && (found.back() == '\n' || found.back() == ' ')) found.pop_back();
            if (!found.empty() && found[0] == '/') candidates.push_back(found);
        }
    }
    for (const std::string &path : candidates) {
        if (access(path.c_str(), X_OK) != 0) continue;
        auto known = checked.find(path);
        if (known == checked.end()) {
            std::string output;
            vela::claude::Version version;
            if (!vela::RunAndCapture({path, "--version"}, &output) ||
                !vela::claude::ParseVersion(output, &version)) {
                *error = path + " didn't report a version.";
                continue;
            }
            known = checked.emplace(path, version).first;
        }
        if (known->second < vela::claude::MinimumVersion) {
            *error = "Claude Code " + known->second.ToString() + " at " + path +
                " is too old: Vela needs " + vela::claude::MinimumVersion.ToString() +
                " or later. Update it with `claude update`.";
            return std::string();
        }
        return path;
    }
    if (error->empty()) {
        *error = "Vela couldn't find Claude Code. Install it from claude.com/code, or "
                 "choose it in Settings → Claude and MCP.";
    }
    return std::string();
}

NSString *NewUuid() { return NSUUID.UUID.UUIDString.lowercaseString; }

// The document's file, or "" while it is unsaved.
std::string DocumentPath(const vela::Document &document) {
    const SdfLayerHandle root = document.GetStage()->GetRootLayer();
    return root->IsAnonymous() ? std::string() : root->GetRealPath();
}

// What each request tells Claude about the document, as Claude Code
// includes an editor's selection.
std::string Context(const vela::SessionDocument &entry) {
    const vela::Document &d = *entry.document;
    std::string context = "<vela-context>\nDocument: " + entry.name;
    const std::string path = DocumentPath(d);
    if (!path.empty()) context += " (" + path + ")";
    context += "\nSelection: ";
    const SdfPathVector &selection = d.GetSelection();
    if (selection.empty()) context += "nothing";
    for (size_t i = 0; i < selection.size() && i < 20; ++i) {
        context += (i ? ", " : "") + selection[i].GetString();
    }
    if (selection.size() > 20) context += ", and " + std::to_string(selection.size() - 20) + " more";
    if (const SdfLayerHandle target = d.GetEditTarget()) {
        context += "\nEdit target: " + target->GetIdentifier();
    }
    const UsdTimeCode time = d.GetTime();
    if (d.IsAnimated() && time.IsNumeric()) {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "%g (of %g to %g)", time.GetValue(), d.GetStartTime(),
                 d.GetEndTime());
        context += std::string("\nTime: ") + buffer;
    }
    return context + "\n</vela-context>\n\n";
}

#pragma mark Views

NSAttributedString *Plain(NSString *text, NSFont *font, NSColor *color) {
    return [[NSAttributedString alloc] initWithString:text ?: @"" attributes:@{
        NSFontAttributeName: font, NSForegroundColorAttributeName: color}];
}

NSFont *Small() { return [NSFont systemFontOfSize:NSFont.smallSystemFontSize]; }
NSFont *Code() {
    return [NSFont monospacedSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular];
}

NSImage *Symbol(NSString *name, NSString *description) {
    return [NSImage imageWithSystemSymbolName:name accessibilityDescription:description];
}

NSStackView *Row(NSArray<NSView *> *views, CGFloat spacing = 6) {
    NSStackView *row = [NSStackView stackViewWithViews:views];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.alignment = NSLayoutAttributeCenterY;
    row.spacing = spacing;
    return row;
}

NSStackView *Column(NSArray<NSView *> *views, CGFloat spacing = 4) {
    NSStackView *column = [NSStackView stackViewWithViews:views];
    column.orientation = NSUserInterfaceLayoutOrientationVertical;
    column.alignment = NSLayoutAttributeLeading;
    column.spacing = spacing;
    return column;
}

NSTextField *Label(NSString *text, NSFont *font, NSColor *color) {
    NSTextField *label = [NSTextField labelWithString:text ?: @""];
    label.font = font;
    label.textColor = color;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    return label;
}

NSImage *ImageFromBase64(const std::string &data) {
    NSData *bytes = [[NSData alloc] initWithBase64EncodedString:String(data)
                                                        options:NSDataBase64DecodingIgnoreUnknownCharacters];
    return bytes ? [[NSImage alloc] initWithData:bytes] : nil;
}

} // namespace

// A view with a flipped coordinate system, so the transcript grows down.
@interface VelaFlippedView : NSView
@end
@implementation VelaFlippedView
- (BOOL)isFlipped { return YES; }
@end

// A text block whose height follows its text at the width it is given.
@interface VelaTextBlockView : NSTextView
@end
@implementation VelaTextBlockView
- (NSSize)intrinsicContentSize {
    [self.layoutManager ensureLayoutForTextContainer:self.textContainer];
    const NSRect used = [self.layoutManager usedRectForTextContainer:self.textContainer];
    return NSMakeSize(NSViewNoIntrinsicMetric, ceil(used.size.height) + 1);
}
- (void)setFrameSize:(NSSize)size {
    const CGFloat width = self.frame.size.width;
    [super setFrameSize:size];
    if (size.width != width) [self invalidateIntrinsicContentSize];
}
- (void)didChangeText {
    [super didChangeText];
    [self invalidateIntrinsicContentSize];
}
@end

// Text that sizes itself to its width, for the transcript's stack.
static NSTextView *TextBlock(NSAttributedString *text) {
    NSTextView *view = [[VelaTextBlockView alloc] initWithFrame:NSMakeRect(0, 0, 400, 20)];
    view.editable = NO;
    view.selectable = YES;
    view.drawsBackground = NO;
    view.textContainerInset = NSZeroSize;
    view.textContainer.lineFragmentPadding = 0;
    view.textContainer.widthTracksTextView = YES;
    view.verticallyResizable = NO;
    view.linkTextAttributes = @{NSForegroundColorAttributeName: NSColor.linkColor,
                                NSCursorAttributeName: NSCursor.pointingHandCursor};
    [view.textStorage setAttributedString:text];
    return view;
}


// A thumbnail that opens its image at full size when clicked.
@interface VelaThumbnailView : NSImageView
@end
@implementation VelaThumbnailView
- (void)mouseDown:(NSEvent *)event {
    NSImage *image = self.image;
    if (!image) return;
    NSSize size = image.size;
    const NSRect screen = self.window.screen.visibleFrame;
    const CGFloat scale = MIN(1.0, MIN(screen.size.width * 0.8 / size.width,
                                       screen.size.height * 0.8 / size.height));
    size = NSMakeSize(size.width * scale, size.height * scale);
    NSPanel *panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable |
                  NSWindowStyleMaskUtilityWindow
          backing:NSBackingStoreBuffered defer:NO];
    panel.title = self.toolTip ?: @"Screenshot";
    panel.releasedWhenClosed = NO;
    NSImageView *view = [NSImageView imageViewWithImage:image];
    view.imageScaling = NSImageScaleProportionallyUpOrDown;
    panel.contentView = view;
    [panel center];
    [panel makeKeyAndOrderFront:nil];
    static NSMutableArray *open = [NSMutableArray array];
    [open addObject:panel];
    __weak NSPanel *weak = panel;
    [NSNotificationCenter.defaultCenter addObserverForName:NSWindowWillCloseNotification
        object:panel queue:nil usingBlock:^(NSNotification *) {
        if (weak) [open removeObject:weak];
    }];
}
- (void)resetCursorRects { [self addCursorRect:self.bounds cursor:NSCursor.pointingHandCursor]; }
@end

// The input: Return sends, Shift-Return or Option-Return adds a line.
@interface VelaClaudeInputView : NSTextView
@property (nonatomic, copy) void (^send)(void);
@property (nonatomic, copy) void (^stop)(void);
@property (nonatomic, copy) NSString *placeholder;
@end
@implementation VelaClaudeInputView
- (void)keyDown:(NSEvent *)event {
    const NSEventModifierFlags flags = event.modifierFlags;
    const BOOL returnKey = event.keyCode == 36 || event.keyCode == 76;
    if (event.keyCode == 53 && self.stop) {  // Escape
        self.stop();
        return;
    }
    if (returnKey && !(flags & (NSEventModifierFlagShift | NSEventModifierFlagOption)) &&
        !self.hasMarkedText) {
        if (self.send) self.send();
        return;
    }
    [super keyDown:event];
}
- (void)drawRect:(NSRect)rect {
    [super drawRect:rect];
    if (self.string.length || !self.placeholder) return;
    const NSPoint origin = NSMakePoint(self.textContainerInset.width + self.textContainer.lineFragmentPadding,
                                       self.textContainerInset.height);
    [self.placeholder drawAtPoint:origin withAttributes:@{
        NSFontAttributeName: self.font ?: [NSFont systemFontOfSize:FontSize],
        NSForegroundColorAttributeName: NSColor.placeholderTextColor}];
}
- (BOOL)becomeFirstResponder {
    [self setNeedsDisplay:YES];
    return [super becomeFirstResponder];
}
- (void)didChangeText {
    [super didChangeText];
    [self setNeedsDisplay:YES];
}
@end

// A tool call: its title, a summary of its input, its status, and on
// request the full input and result.
@interface VelaToolCallView : NSStackView
@property (nonatomic, readonly) NSString *toolName;
@property (nonatomic, readonly) BOOL running;
- (instancetype)initWithTool:(NSString *)name input:(const JsObject &)input;
- (void)finishWithText:(NSString *)text images:(NSArray<NSImage *> *)images error:(BOOL)error;
- (void)setNote:(NSString *)note;
@end

@implementation VelaToolCallView {
    NSProgressIndicator *_spinner;
    NSImageView *_status;
    NSButton *_disclosure;
    NSTextField *_note;
    VelaTextBlockView *_details;
    NSMutableAttributedString *_detailText;
    NSString *_toolName;
}

- (instancetype)initWithTool:(NSString *)name input:(const JsObject &)input {
    if (!(self = [super initWithFrame:NSZeroRect])) return nil;
    _toolName = name;
    self.orientation = NSUserInterfaceLayoutOrientationVertical;
    self.alignment = NSLayoutAttributeLeading;
    self.spacing = 4;
    _spinner = [NSProgressIndicator new];
    _spinner.style = NSProgressIndicatorStyleSpinning;
    _spinner.controlSize = NSControlSizeSmall;
    [_spinner startAnimation:nil];
    _status = [NSImageView imageViewWithImage:Symbol(@"checkmark.circle", @"Done")];
    _status.hidden = YES;
    _disclosure = [NSButton new];
    _disclosure.bezelStyle = NSBezelStyleDisclosure;
    _disclosure.buttonType = NSButtonTypeOnOff;
    _disclosure.title = @"";
    _disclosure.target = self;
    _disclosure.action = @selector(toggleDetails:);
    NSTextField *title = Label(String(vela::claude::ToolTitle(Text(name))),
                               [NSFont boldSystemFontOfSize:NSFont.smallSystemFontSize],
                               NSColor.secondaryLabelColor);
    [title setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSTextField *summary = Label(String(vela::claude::ToolSummary(Text(name), input)), Code(),
                                 NSColor.secondaryLabelColor);
    _note = Label(@"", Small(), NSColor.tertiaryLabelColor);
    _note.hidden = YES;
    NSStackView *header = Row(@[_disclosure, _spinner, _status, title, summary, _note]);
    header.detachesHiddenViews = YES;
    [self addArrangedSubview:header];

    // Details: the input in full (code as code), then the result.
    _detailText = [NSMutableAttributedString new];
    auto code = input.find("code");
    NSString *inputText = code != input.end() && code->second.IsString()
        ? String(code->second.GetString())
        : String(vela::mcp::Compact(JsValue(input)));
    [_detailText appendAttributedString:Plain(inputText, Code(), NSColor.textColor)];
    _details = (VelaTextBlockView *)TextBlock(_detailText);
    _details.hidden = YES;
    [self addArrangedSubview:_details];
    [_details.widthAnchor constraintEqualToAnchor:self.widthAnchor constant:-20].active = YES;
    [self setCustomSpacing:2 afterView:header];
    self.edgeInsets = NSEdgeInsetsMake(0, 0, 0, 0);
    return self;
}

- (NSString *)toolName { return _toolName; }
- (BOOL)running { return !_spinner.hidden; }

- (void)toggleDetails:(id)sender {
    _details.hidden = _disclosure.state != NSControlStateValueOn;
    [_details invalidateIntrinsicContentSize];
}

- (void)setNote:(NSString *)note {
    _note.stringValue = note ?: @"";
    _note.hidden = !note.length;
}

- (void)finishWithText:(NSString *)text images:(NSArray<NSImage *> *)images error:(BOOL)error {
    [_spinner stopAnimation:nil];
    _spinner.hidden = YES;
    _status.image = error ? Symbol(@"xmark.circle", @"Failed") : Symbol(@"checkmark.circle", @"Done");
    _status.contentTintColor = error ? NSColor.systemRedColor : NSColor.systemGreenColor;
    _status.hidden = NO;
    if (text.length) {
        [_detailText appendAttributedString:Plain(@"\n\n", Code(), NSColor.textColor)];
        [_detailText appendAttributedString:Plain(text, Code(),
                                                  error ? NSColor.systemRedColor : NSColor.secondaryLabelColor)];
        [_details.textStorage setAttributedString:_detailText];
        [_details invalidateIntrinsicContentSize];
    }
    // Errors show at once; results on request.
    if (error) {
        _disclosure.state = NSControlStateValueOn;
        [self toggleDetails:nil];
    }
    for (NSImage *image in images) {
        VelaThumbnailView *thumbnail = [VelaThumbnailView imageViewWithImage:image];
        thumbnail.imageScaling = NSImageScaleProportionallyDown;
        thumbnail.toolTip = @"Viewport screenshot (click to enlarge)";
        const CGFloat height = MIN(160.0, image.size.height);
        const CGFloat width = image.size.height > 0 ? height * image.size.width / image.size.height : height;
        [thumbnail.heightAnchor constraintEqualToConstant:height].active = YES;
        [thumbnail.widthAnchor constraintEqualToConstant:width].active = YES;
        thumbnail.wantsLayer = YES;
        thumbnail.layer.cornerRadius = 4;
        thumbnail.layer.masksToBounds = YES;
        [self addArrangedSubview:thumbnail];
    }
}

@end

#pragma mark - Conversation list

// The History popover: this document's conversations, newest first, with
// a search field. Clicking one opens it; Delete removes it.
@interface VelaConversationTableView : NSTableView
@property (nonatomic, copy) void (^deleteRow)(NSInteger row);
@end
@implementation VelaConversationTableView
- (void)keyDown:(NSEvent *)event {
    if ((event.keyCode == 51 || event.keyCode == 117) && self.selectedRow >= 0 && self.deleteRow) {
        self.deleteRow(self.selectedRow);
        return;
    }
    if ((event.keyCode == 36 || event.keyCode == 76) && self.selectedRow >= 0) {
        [self sendAction:self.doubleAction to:self.target];
        return;
    }
    [super keyDown:event];
}
@end

@interface VelaConversationListController : NSViewController <NSTableViewDataSource,
                                                                NSTableViewDelegate,
                                                                NSSearchFieldDelegate>
@property (nonatomic, copy) NSString *currentId;
@property (nonatomic, copy) void (^open)(NSString *identifier);
@property (nonatomic, copy) void (^remove)(NSString *identifier);
- (void)setConversations:(const std::vector<vela::claude::Conversation> &)conversations;
@end

@implementation VelaConversationListController {
    std::vector<vela::claude::Conversation> _all;
    std::vector<vela::claude::Conversation> _shown;
    NSSearchField *_search;
    VelaConversationTableView *_table;
    NSTextField *_empty;
}

- (void)loadView {
    _search = [NSSearchField new];
    _search.placeholderString = @"Search conversations";
    _search.delegate = self;
    _search.sendsSearchStringImmediately = YES;
    _table = [VelaConversationTableView new];
    NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"conversation"];
    [_table addTableColumn:column];
    _table.headerView = nil;
    _table.rowHeight = 38;
    _table.style = NSTableViewStyleSourceList;
    _table.dataSource = self;
    _table.delegate = self;
    _table.target = self;
    _table.action = @selector(rowClicked:);
    _table.doubleAction = @selector(rowClicked:);
    __weak VelaConversationListController *weakSelf = self;
    _table.deleteRow = ^(NSInteger row) { [weakSelf deleteRow:row]; };
    NSMenu *menu = [NSMenu new];
    [menu addItemWithTitle:@"Open" action:@selector(openClicked:) keyEquivalent:@""].target = self;
    [menu addItemWithTitle:@"Delete Conversation" action:@selector(deleteClicked:)
             keyEquivalent:@""].target = self;
    _table.menu = menu;
    NSScrollView *scroll = [NSScrollView new];
    scroll.documentView = _table;
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    _empty = [NSTextField labelWithString:@"No earlier conversations about this document."];
    _empty.textColor = NSColor.secondaryLabelColor;
    _empty.font = Small();
    NSView *root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 380, 340)];
    for (NSView *view in @[_search, scroll, _empty]) {
        view.translatesAutoresizingMaskIntoConstraints = NO;
        [root addSubview:view];
    }
    [NSLayoutConstraint activateConstraints:@[
        [_search.topAnchor constraintEqualToAnchor:root.topAnchor constant:10],
        [_search.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:10],
        [_search.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-10],
        [scroll.topAnchor constraintEqualToAnchor:_search.bottomAnchor constant:8],
        [scroll.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],
        [_empty.centerXAnchor constraintEqualToAnchor:root.centerXAnchor],
        [_empty.topAnchor constraintEqualToAnchor:_search.bottomAnchor constant:30],
        [root.widthAnchor constraintEqualToConstant:380],
        [root.heightAnchor constraintEqualToConstant:340],
    ]];
    self.view = root;
    [self filter];
}

- (void)viewDidAppear {
    [super viewDidAppear];
    [self.view.window makeFirstResponder:_search];
}

- (void)setConversations:(const std::vector<vela::claude::Conversation> &)conversations {
    _all = conversations;
    _search.stringValue = @"";
    [self filter];
}

- (void)filter {
    NSString *query = [_search.stringValue stringByTrimmingCharactersInSet:
        NSCharacterSet.whitespaceCharacterSet] ?: @"";
    _shown.clear();
    for (const auto &c : _all) {
        if (!query.length || [String(c.title) rangeOfString:query
                options:NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch].location != NSNotFound) {
            _shown.push_back(c);
        }
    }
    [_table reloadData];
    _empty.hidden = !_shown.empty();
    _empty.stringValue = _all.empty() ? @"No earlier conversations about this document."
                                      : @"No conversations match.";
    for (size_t i = 0; i < _shown.size(); ++i) {
        if ([String(_shown[i].id) isEqualToString:_currentId]) {
            [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];
        }
    }
}

- (void)controlTextDidChange:(NSNotification *)notification { [self filter]; }

// Return and arrows in the search field go to the table.
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)command {
    if (command == @selector(moveDown:) || command == @selector(moveUp:)) {
        const NSInteger count = NSInteger(_shown.size());
        if (!count) return YES;
        NSInteger row = _table.selectedRow + (command == @selector(moveDown:) ? 1 : -1);
        row = MAX(0, MIN(count - 1, row));
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:NSUInteger(row)] byExtendingSelection:NO];
        [_table scrollRowToVisible:row];
        return YES;
    }
    if (command == @selector(insertNewline:)) {
        [self openRow:_table.selectedRow >= 0 ? _table.selectedRow : 0];
        return YES;
    }
    return NO;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView { return NSInteger(_shown.size()); }

- (NSView *)tableView:(NSTableView *)tableView viewForTableColumn:(NSTableColumn *)column
                  row:(NSInteger)row {
    NSTableCellView *cell = [tableView makeViewWithIdentifier:@"row" owner:self];
    NSTextField *title, *detail;
    if (!cell) {
        cell = [NSTableCellView new];
        cell.identifier = @"row";
        title = Label(@"", [NSFont systemFontOfSize:NSFont.systemFontSize], NSColor.labelColor);
        detail = Label(@"", Small(), NSColor.secondaryLabelColor);
        title.tag = 1;
        detail.tag = 2;
        NSStackView *stack = Column(@[title, detail], 1);
        stack.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:stack];
        [NSLayoutConstraint activateConstraints:@[
            [stack.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:4],
            [stack.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-4],
            [stack.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        ]];
        cell.textField = title;
    }
    title = [cell viewWithTag:1];
    detail = [cell viewWithTag:2];
    const auto &c = _shown[size_t(row)];
    title.stringValue = c.title.empty() ? @"Untitled conversation" : String(c.title);
    NSRelativeDateTimeFormatter *relative = [NSRelativeDateTimeFormatter new];
    relative.unitsStyle = NSRelativeDateTimeFormatterUnitsStyleFull;
    NSString *when = [relative localizedStringForDate:[NSDate dateWithTimeIntervalSince1970:c.updated]
                                       relativeToDate:NSDate.date];
    detail.stringValue = [String(c.id) isEqualToString:_currentId]
        ? [when stringByAppendingString:@" · open now"] : when;
    title.toolTip = String(c.title);
    return cell;
}

- (void)openRow:(NSInteger)row {
    if (row < 0 || size_t(row) >= _shown.size()) return;
    if (self.open) self.open(String(_shown[size_t(row)].id));
}

- (void)rowClicked:(id)sender { [self openRow:_table.clickedRow >= 0 ? _table.clickedRow : _table.selectedRow]; }
- (void)openClicked:(id)sender { [self openRow:_table.clickedRow]; }
- (void)deleteClicked:(id)sender { [self deleteRow:_table.clickedRow]; }

- (void)deleteRow:(NSInteger)row {
    if (row < 0 || size_t(row) >= _shown.size()) return;
    const std::string identifier = _shown[size_t(row)].id;
    if (self.remove) self.remove(String(identifier));
    _all.erase(std::remove_if(_all.begin(), _all.end(),
        [&](const vela::claude::Conversation &c) { return c.id == identifier; }), _all.end());
    [self filter];
}

@end

#pragma mark - Panel

// Panels by document, for ask_claude.
static NSMapTable<NSValue *, VelaClaudePanelController *> *Panels() {
    static NSMapTable *panels = [NSMapTable strongToWeakObjectsMapTable];
    return panels;
}

@interface VelaClaudePanelController () <NSTextViewDelegate>
- (void)handleLine:(const std::string &)line;
- (void)processExited:(int)status errors:(const std::string &)errors;
@end

@implementation VelaClaudePanelController {
    // Views.
    NSScrollView *_scroll;
    NSStackView *_transcript;
    VelaClaudeInputView *_input;
    NSButton *_send;
    NSButton *_attach;
    NSButton *_historyButton;
    NSButton *_newButton;
    NSButton *_stopButton;
    NSTextField *_title;
    NSProgressIndicator *_spinner;
    NSTextField *_status;
    NSPopUpButton *_modelPopUp;
    NSPopUpButton *_effortPopUp;
    NSPopUpButton *_modePopUp;
    NSPopover *_history;
    VelaConversationListController *_historyList;
    int _settingsObserver;
    NSTextView *_live;            // Claude's reply as it streams
    NSMutableString *_liveText;
    NSMutableDictionary<NSString *, VelaToolCallView *> *_tools;
    NSMutableDictionary<NSString *, NSView *> *_approvals;

    // The CLI and the tools' socket.
    std::unique_ptr<vela::LineProcess> _process;
    std::unique_ptr<vela::McpSocketServer> _server;
    NSString *_conversationId;   // the CLI's session id
    BOOL _conversationStarted;   // the CLI knows it (so it can be resumed)
    BOOL _logged;                // its history file exists
    std::vector<JsObject> _pendingLog;
    std::set<std::string> _allowedTools;  // "Allow for This Conversation"
    double _cost;
    int _nextRequest;

    // The request running.
    BOOL _busy;
    NSString *_request;
    uint64_t _undoMark;
    NSTimer *_stopTimer;
    BOOL _stopping;
    // The transcript follows new content until the user scrolls up.
    BOOL _pinned;
    // A message typed while Claude works, sent when the request ends.
    NSString *_queued;
    NSView *_queuedView;
    // What the running CLI was started with; a change restarts it (the
    // conversation resumes) before the next message.
    std::string _launchedModel, _launchedEffort;
    BOOL _restartAfterRequest;
}

- (void)loadView {
    _tools = [NSMutableDictionary dictionary];
    _approvals = [NSMutableDictionary dictionary];
    _conversationId = NewUuid();

    // A header, as in Claude Code's editor panels: history, the
    // conversation's title, progress with Stop, and New.
    _historyButton = [self toolButton:@"clock.arrow.circlepath" tip:@"Earlier conversations"
                               action:@selector(showHistory:)];
    _title = Label(@"New conversation", [NSFont boldSystemFontOfSize:NSFont.smallSystemFontSize],
                   NSColor.labelColor);
    _spinner = [NSProgressIndicator new];
    _spinner.style = NSProgressIndicatorStyleSpinning;
    _spinner.controlSize = NSControlSizeSmall;
    _spinner.displayedWhenStopped = NO;
    _status = Label(@"", Small(), NSColor.secondaryLabelColor);
    _status.alignment = NSTextAlignmentRight;
    _stopButton = [NSButton buttonWithTitle:@"Stop" target:self action:@selector(stopClicked:)];
    _stopButton.controlSize = NSControlSizeSmall;
    _stopButton.font = Small();
    _stopButton.image = Symbol(@"stop.fill", @"Stop");
    _stopButton.imagePosition = NSImageLeading;
    _stopButton.toolTip = @"Stop Claude (Escape)";
    _stopButton.hidden = YES;
    _newButton = [self toolButton:@"plus" tip:@"New conversation" action:@selector(newConversation:)];
    NSView *spacer = [NSView new];
    [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *header = Row(@[_historyButton, _title, spacer, _spinner, _status, _stopButton, _newButton]);
    header.detachesHiddenViews = YES;
    header.edgeInsets = NSEdgeInsetsMake(3, 8, 3, 8);
    [header setHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];

    // The transcript.
    _transcript = Column(@[], 10);
    _transcript.edgeInsets = NSEdgeInsetsMake(10, 14, 10, 14);
    _transcript.translatesAutoresizingMaskIntoConstraints = NO;
    VelaFlippedView *document = [VelaFlippedView new];
    document.translatesAutoresizingMaskIntoConstraints = NO;
    [document addSubview:_transcript];
    _scroll = [NSScrollView new];
    _scroll.hasVerticalScroller = YES;
    _scroll.drawsBackground = NO;
    _scroll.documentView = document;
    _pinned = YES;
    _scroll.contentView.postsBoundsChangedNotifications = YES;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(transcriptScrolled:)
        name:NSViewBoundsDidChangeNotification object:_scroll.contentView];
    [NSLayoutConstraint activateConstraints:@[
        [_transcript.topAnchor constraintEqualToAnchor:document.topAnchor],
        [_transcript.leadingAnchor constraintEqualToAnchor:document.leadingAnchor],
        [_transcript.trailingAnchor constraintEqualToAnchor:document.trailingAnchor],
        [_transcript.bottomAnchor constraintEqualToAnchor:document.bottomAnchor],
        [document.leadingAnchor constraintEqualToAnchor:_scroll.contentView.leadingAnchor],
        [document.trailingAnchor constraintEqualToAnchor:_scroll.contentView.trailingAnchor],
        [document.topAnchor constraintEqualToAnchor:_scroll.contentView.topAnchor],
    ]];

    // The input, with attach and send.
    _input = [VelaClaudeInputView new];
    _input.richText = NO;
    _input.allowsUndo = YES;
    _input.font = [NSFont systemFontOfSize:FontSize];
    _input.textContainerInset = NSMakeSize(4, 6);
    _input.placeholder = @"Ask Claude about this scene, or to change it";
    _input.automaticQuoteSubstitutionEnabled = NO;
    _input.automaticDashSubstitutionEnabled = NO;
    __weak VelaClaudePanelController *weakSelf = self;
    _input.send = ^{ [weakSelf sendInput]; };
    _input.delegate = self;
    _input.stop = ^{ [weakSelf cancelOperation:nil]; };
    NSScrollView *inputScroll = [NSScrollView new];
    inputScroll.hasVerticalScroller = YES;
    inputScroll.autohidesScrollers = YES;
    inputScroll.borderType = NSNoBorder;
    inputScroll.drawsBackground = NO;
    inputScroll.documentView = _input;
    _input.minSize = NSMakeSize(0, 0);
    _input.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    _input.verticallyResizable = YES;
    _input.horizontallyResizable = NO;
    _input.autoresizingMask = NSViewWidthSizable;
    _input.textContainer.widthTracksTextView = YES;
    _input.drawsBackground = NO;

    _attach = [NSButton buttonWithImage:Symbol(@"camera.viewfinder", @"Attach the viewport")
                                 target:nil action:nil];
    _attach.buttonType = NSButtonTypePushOnPushOff;
    _attach.bordered = NO;
    _attach.toolTip = @"Attach an image of the viewport to the next message";
    _send = [NSButton buttonWithImage:Symbol(@"arrow.up.circle.fill", @"Send")
                               target:self action:@selector(sendOrStop:)];
    _send.bordered = NO;
    _send.contentTintColor = NSColor.controlAccentColor;
    _send.toolTip = @"Send (Return)";
    [_send.widthAnchor constraintEqualToConstant:22].active = YES;
    [_send.heightAnchor constraintEqualToConstant:22].active = YES;
    _send.imageScaling = NSImageScaleProportionallyUpOrDown;

    // Below the text: model, effort and edit mode, then attach and send.
    _modelPopUp = [self choicePopUp:vela::claude::ModelChoices() tip:@"The model Claude uses"];
    _effortPopUp = [self choicePopUp:vela::claude::EffortChoices()
                                 tip:@"How hard Claude thinks: more effort is slower and costs more"];
    _modePopUp = [self choicePopUp:{{"edit", "Edit automatically"}, {"ask", "Ask before edits"}}
                               tip:@"Whether Claude asks before it changes the scene"];
    NSView *footerSpacer = [NSView new];
    [footerSpacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *footer = Row(@[_modelPopUp, _effortPopUp, _modePopUp, footerSpacer, _attach, _send], 4);
    [footer setHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSView *inputBox = [NSView new];
    inputBox.wantsLayer = YES;
    inputBox.layer.cornerRadius = 7;
    inputBox.layer.borderWidth = 1;
    inputBox.layer.borderColor = NSColor.separatorColor.CGColor;
    for (NSView *view in @[inputScroll, footer]) {
        view.translatesAutoresizingMaskIntoConstraints = NO;
        [inputBox addSubview:view];
    }
    [NSLayoutConstraint activateConstraints:@[
        [inputScroll.leadingAnchor constraintEqualToAnchor:inputBox.leadingAnchor constant:4],
        [inputScroll.trailingAnchor constraintEqualToAnchor:inputBox.trailingAnchor constant:-4],
        [inputScroll.topAnchor constraintEqualToAnchor:inputBox.topAnchor constant:2],
        [inputScroll.heightAnchor constraintEqualToConstant:40],
        [footer.topAnchor constraintEqualToAnchor:inputScroll.bottomAnchor],
        [footer.leadingAnchor constraintEqualToAnchor:inputBox.leadingAnchor constant:4],
        [footer.trailingAnchor constraintEqualToAnchor:inputBox.trailingAnchor constant:-6],
        [footer.bottomAnchor constraintEqualToAnchor:inputBox.bottomAnchor constant:-4],
    ]];

    NSView *root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 800, 220)];
    NSBox *line = [NSBox new];
    line.boxType = NSBoxSeparator;
    for (NSView *view in @[header, line, _scroll, inputBox]) {
        view.translatesAutoresizingMaskIntoConstraints = NO;
        [root addSubview:view];
    }
    [NSLayoutConstraint activateConstraints:@[
        [header.topAnchor constraintEqualToAnchor:root.topAnchor],
        [header.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [header.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [line.topAnchor constraintEqualToAnchor:header.bottomAnchor],
        [line.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [line.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [_scroll.topAnchor constraintEqualToAnchor:line.bottomAnchor],
        [_scroll.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [_scroll.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [_scroll.heightAnchor constraintGreaterThanOrEqualToConstant:30],
        [inputBox.topAnchor constraintEqualToAnchor:_scroll.bottomAnchor constant:6],
        [inputBox.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:10],
        [inputBox.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-10],
        [inputBox.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-8],
    ]];
    self.view = root;
    [self showWelcome];
    [self settingsChanged];
    __weak VelaClaudePanelController *observer = self;
    _settingsObserver = vela::AppSettingsStore::Get().AddObserver([observer] {
        [observer settingsChanged];
    });
    [self updateControls];
}

- (NSButton *)toolButton:(NSString *)symbol tip:(NSString *)tip action:(SEL)action {
    NSButton *button = [NSButton buttonWithImage:Symbol(symbol, tip) target:self action:action];
    button.bordered = NO;
    button.toolTip = tip;
    button.contentTintColor = NSColor.secondaryLabelColor;
    [button.widthAnchor constraintEqualToConstant:22].active = YES;
    return button;
}

- (NSPopUpButton *)choicePopUp:(const std::vector<vela::claude::Choice> &)choices tip:(NSString *)tip {
    NSPopUpButton *popUp = [NSPopUpButton new];
    popUp.controlSize = NSControlSizeSmall;
    popUp.font = Small();
    popUp.bordered = NO;
    popUp.toolTip = tip;
    for (const auto &choice : choices) {
        [popUp addItemWithTitle:String(choice.title)];
        popUp.lastItem.representedObject = String(choice.value);
    }
    popUp.target = self;
    popUp.action = @selector(choiceChanged:);
    return popUp;
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    if (_settingsObserver) vela::AppSettingsStore::Get().RemoveObserver(_settingsObserver);
    [self shutDown];
}

- (void)setSessionId:(int)sessionId {
    _sessionId = sessionId;
    if (const vela::SessionDocument *entry = vela::Session::Get().Find(sessionId)) {
        [Panels() setObject:self forKey:[NSValue valueWithPointer:entry->document.get()]];
    }
}

- (BOOL)isBusy { return _busy; }

- (void)focusInput {
    [self.view.window makeFirstResponder:_input];
}

- (std::shared_ptr<vela::Document>)document {
    const vela::SessionDocument *entry = vela::Session::Get().Find(_sessionId);
    return entry ? entry->document : nullptr;
}

#pragma mark Transcript

- (BOOL)scrolledToBottom { return _pinned; }

- (void)transcriptScrolled:(NSNotification *)notification {
    // Only scrolling moves the clip view's bounds; content growing doesn't.
    NSView *document = _scroll.documentView;
    const NSRect visible = _scroll.contentView.documentVisibleRect;
    _pinned = NSMaxY(visible) >= NSHeight(document.frame) - 30;
}

- (void)scrollToBottom {
    [_scroll.documentView layoutSubtreeIfNeeded];
    NSView *document = _scroll.documentView;
    const CGFloat y = MAX(0, NSHeight(document.frame) - NSHeight(_scroll.contentView.bounds));
    [_scroll.contentView scrollToPoint:NSMakePoint(0, y)];
    [_scroll reflectScrolledClipView:_scroll.contentView];
    _pinned = YES;
}

- (void)addItem:(NSView *)view {
    const BOOL follow = [self scrolledToBottom];
    [_transcript addArrangedSubview:view];
    [view.widthAnchor constraintLessThanOrEqualToAnchor:_transcript.widthAnchor constant:-28].active = YES;
    if ([view isKindOfClass:[NSTextView class]] || [view isKindOfClass:[VelaToolCallView class]]) {
        [view.widthAnchor constraintEqualToAnchor:_transcript.widthAnchor constant:-28].active = YES;
    }
    if (follow) dispatch_async(dispatch_get_main_queue(), ^{ [self scrollToBottom]; });
}

- (NSTextView *)addText:(NSAttributedString *)text {
    NSTextView *view = TextBlock(text);
    [self addItem:view];
    return view;
}

- (void)addNote:(NSString *)text color:(NSColor *)color {
    [self addText:Plain(text, Small(), color)];
}

- (void)addUser:(NSString *)text {
    NSTextField *label = [NSTextField wrappingLabelWithString:text];
    label.font = [NSFont systemFontOfSize:FontSize];
    label.selectable = YES;
    NSView *bubble = [NSView new];
    bubble.wantsLayer = YES;
    bubble.layer.cornerRadius = 8;
    bubble.layer.backgroundColor =
        [NSColor.controlAccentColor colorWithAlphaComponent:0.14].CGColor;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [bubble addSubview:label];
    [NSLayoutConstraint activateConstraints:@[
        [label.leadingAnchor constraintEqualToAnchor:bubble.leadingAnchor constant:9],
        [label.trailingAnchor constraintEqualToAnchor:bubble.trailingAnchor constant:-9],
        [label.topAnchor constraintEqualToAnchor:bubble.topAnchor constant:5],
        [label.bottomAnchor constraintEqualToAnchor:bubble.bottomAnchor constant:-5],
    ]];
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    [self addItem:bubble];
}

- (void)showWelcome {
    [self addNote:@"Ask about this scene (\"which prims have no material bound?\") or ask "
                  @"for changes (\"make every light 20% warmer\"). Claude can use the whole "
                  @"USD Python API and every Vela command. Each request is one undo step."
            color:NSColor.secondaryLabelColor];
}

- (void)clearTranscript {
    for (NSView *view in [_transcript.arrangedSubviews copy]) {
        [_transcript removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    [_tools removeAllObjects];
    [_approvals removeAllObjects];
    _live = nil;
    _liveText = nil;
}

- (void)showReply:(NSString *)text final:(BOOL)final {
    if (!_live) {
        _liveText = [NSMutableString string];
        _live = [self addText:[NSAttributedString new]];
    }
    const BOOL follow = [self scrolledToBottom];
    [_live.textStorage setAttributedString:VelaRenderMarkdown(text, FontSize)];
    [_live invalidateIntrinsicContentSize];
    if (follow) dispatch_async(dispatch_get_main_queue(), ^{ [self scrollToBottom]; });
    if (final) {
        _live = nil;
        _liveText = nil;
    }
}

#pragma mark Controls

- (void)updateControls {
    _send.image = _busy ? Symbol(@"stop.circle.fill", @"Stop") : Symbol(@"arrow.up.circle.fill", @"Send");
    _send.contentTintColor = _busy && !_input.string.length ? NSColor.systemRedColor
                                                           : NSColor.controlAccentColor;
    _send.toolTip = _busy ? @"Stop (Escape); type to queue a message" : @"Send (Return)";
    if (_busy && _input.string.length) {
        _send.image = Symbol(@"arrow.up.circle.fill", @"Queue");
        _send.toolTip = @"Queue this message: it sends when Claude finishes (Return)";
    }
    _stopButton.hidden = !_busy;
    if (_busy) [_spinner startAnimation:nil];
    else [_spinner stopAnimation:nil];
    _title.stringValue = _logged || _conversationStarted ? [self conversationTitle] : @"New conversation";
    _title.toolTip = _title.stringValue;
    [self statusChanged];
}

- (NSString *)conversationTitle {
    for (const auto &c : vela::claude::ListConversations(vela::claude::DefaultHistoryFolder())) {
        if ([String(c.id) isEqualToString:_conversationId]) return String(c.title);
    }
    return _request ? String(vela::claude::TitleFor(Text(_request))) : @"Conversation";
}

- (void)statusChanged {
    NSString *status = _stopping ? @"Stopping…" : _busy ? @"Working…" : @"";
    if (_cost > 0) {
        NSString *cost = [NSString stringWithFormat:@"$%.2f", _cost];
        status = status.length ? [NSString stringWithFormat:@"%@  %@", status, cost] : cost;
    }
    _status.stringValue = status;
}

- (void)setBusy:(BOOL)busy {
    _busy = busy;
    [self updateControls];
}

- (void)cancelOperation:(id)sender {
    if (_busy) [self stop];
}

- (void)stopClicked:(id)sender {
    [self stop];
}

- (void)stopRequest {
    [self stop];
}

- (void)sendOrStop:(id)sender {
    if (_busy && !_input.string.length) [self stop];
    else [self sendInput];
}

- (void)sendInput {
    NSString *text = [_input.string stringByTrimmingCharactersInSet:
        NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (!text.length) return;
    if (_busy) {
        [self queue:text];
        _input.string = @"";
        [_input setNeedsDisplay:YES];
        [self updateControls];
        return;
    }
    NSString *error = nil;
    if ([self ask:text error:&error]) {
        _input.string = @"";
        [_input setNeedsDisplay:YES];
    } else if (error) {
        [self addNote:error color:NSColor.systemRedColor];
    }
}

// A message typed while Claude works waits, shown dimmed, until the request
// ends; more typing joins it.
- (void)queue:(NSString *)text {
    _queued = _queued ? [NSString stringWithFormat:@"%@\n\n%@", _queued, text] : text;
    [_queuedView removeFromSuperview];
    NSTextField *label = [NSTextField wrappingLabelWithString:_queued];
    label.font = [NSFont systemFontOfSize:FontSize];
    label.textColor = NSColor.secondaryLabelColor;
    NSTextField *note = Label(@"Queued: sends when Claude finishes", Small(), NSColor.tertiaryLabelColor);
    _queuedView = Column(@[label, note], 2);
    [self addItem:_queuedView];
}

// Takes the queued message back, e.g. to the input after Stop.
- (NSString *)takeQueued {
    NSString *queued = _queued;
    _queued = nil;
    [_queuedView removeFromSuperview];
    _queuedView = nil;
    return queued;
}

- (void)textDidChange:(NSNotification *)notification {
    if (_busy) [self updateControls];
}

#pragma mark Model, effort and mode

// Shows the settings' choices; a different model or effort restarts the
// CLI before the next message (the conversation resumes).
- (void)settingsChanged {
    const vela::AppSettings &settings = vela::AppSettingsStore::Get().GetSettings();
    auto select = [](NSPopUpButton *popUp, const std::string &value) {
        [popUp selectItemAtIndex:0];
        for (NSMenuItem *item in popUp.itemArray) {
            if ([item.representedObject isEqualToString:String(value)]) [popUp selectItem:item];
        }
    };
    select(_modelPopUp, settings.claudeModel);
    select(_effortPopUp, settings.claudeEffort);
    select(_modePopUp, settings.claudeAsksBeforeEdits ? "ask" : "edit");
    if (_process && (settings.claudeModel != _launchedModel || settings.claudeEffort != _launchedEffort)) {
        if (_busy) _restartAfterRequest = YES;
        else [self endProcess];
    }
}

- (void)choiceChanged:(NSPopUpButton *)sender {
    const std::string value = Text(sender.selectedItem.representedObject);
    vela::AppSettings settings = vela::AppSettingsStore::Get().GetSettings();
    if (sender == _modelPopUp) settings.claudeModel = value;
    else if (sender == _effortPopUp) settings.claudeEffort = value;
    else settings.claudeAsksBeforeEdits = value == "ask";
    vela::AppSettingsStore::Get().SetSettings(settings);
    [self focusInput];
}

#pragma mark Conversations

- (void)showHistory:(id)sender {
    if (_history.shown) {
        [_history close];
        return;
    }
    if (!_history) {
        _historyList = [VelaConversationListController new];
        __weak VelaClaudePanelController *weakSelf = self;
        _historyList.open = ^(NSString *identifier) { [weakSelf switchToConversation:identifier]; };
        _historyList.remove = ^(NSString *identifier) { [weakSelf deleteConversation:identifier]; };
        _history = [NSPopover new];
        _history.behavior = NSPopoverBehaviorTransient;
        _history.contentViewController = _historyList;
    }
    const std::shared_ptr<vela::Document> document = [self document];
    const std::string path = document ? DocumentPath(*document) : std::string();
    std::vector<vela::claude::Conversation> conversations;
    for (const auto &c : vela::claude::ListConversations(vela::claude::DefaultHistoryFolder(), path)) {
        // An unsaved document's conversations have no file to match.
        if (c.document == path) conversations.push_back(c);
    }
    _historyList.currentId = _conversationStarted || _logged ? _conversationId : nil;
    (void)_historyList.view;
    [_historyList setConversations:conversations];
    [_history showRelativeToRect:_historyButton.bounds ofView:_historyButton
                   preferredEdge:_historyButton.isFlipped ? NSRectEdgeMinY : NSRectEdgeMaxY];
}

- (void)switchToConversation:(NSString *)identifier {
    [_history close];
    if ([identifier isEqualToString:_conversationId]) return;
    [self abandonRequest];
    [self openConversationId:identifier];
}

- (void)deleteConversation:(NSString *)identifier {
    vela::claude::DeleteConversation(vela::claude::DefaultHistoryFolder(), Text(identifier));
    // The open conversation keeps going, as a new one.
    if ([identifier isEqualToString:_conversationId]) {
        _logged = NO;
        _conversationStarted = NO;
        [self endProcess];
        _conversationId = NewUuid();
        [self updateControls];
    }
}

// Ends a running request at once, for switching conversations: the CLI
// is ended (the conversation can be resumed later).
- (void)abandonRequest {
    if (!_busy) return;
    _stopping = YES;
    [self endProcess];
    [self finishRequest:@"Stopped." error:NO];
}

- (void)endProcess {
    [_stopTimer invalidate];
    _stopTimer = nil;
    if (_process) _process->Terminate();
    _process.reset();
    _server.reset();
}

- (void)newConversation:(id)sender {
    [self abandonRequest];
    [self takeQueued];
    [self endProcess];
    [self clearTranscript];
    _conversationId = NewUuid();
    _conversationStarted = NO;
    _logged = NO;
    _pendingLog.clear();
    _allowedTools.clear();
    _cost = 0;
    [self showWelcome];
    [self updateControls];
    [self focusInput];
}

- (void)openConversationId:(NSString *)identifier {
    if (_busy || !identifier) return;
    [self takeQueued];
    [self endProcess];
    [self clearTranscript];
    _conversationId = identifier;
    _conversationStarted = YES;  // the CLI resumes it
    _logged = YES;
    _pendingLog.clear();
    _allowedTools.clear();
    _cost = 0;
    [self replay:vela::claude::LoadEntries(vela::claude::DefaultHistoryFolder(), Text(identifier))];
    [self updateControls];
    [self scrollToBottom];
    [self focusInput];
}

// Shows a logged conversation as it went.
- (void)replay:(const std::vector<JsObject> &)entries {
    auto text = [](const JsObject &entry, const char *key) {
        auto it = entry.find(key);
        return it != entry.end() && it->second.IsString() ? String(it->second.GetString()) : @"";
    };
    for (const JsObject &entry : entries) {
        NSString *kind = text(entry, "kind");
        if ([kind isEqualToString:@"user"]) {
            [self addUser:text(entry, "text")];
        } else if ([kind isEqualToString:@"assistant"]) {
            [self showReply:text(entry, "text") final:YES];
        } else if ([kind isEqualToString:@"tool"]) {
            auto input = entry.find("input");
            VelaToolCallView *view = [[VelaToolCallView alloc]
                initWithTool:text(entry, "name")
                       input:input != entry.end() && input->second.IsObject()
                                 ? input->second.GetJsObject() : JsObject()];
            _tools[text(entry, "id")] = view;
            [self addItem:view];
        } else if ([kind isEqualToString:@"tool_result"]) {
            VelaToolCallView *view = _tools[text(entry, "id")];
            NSMutableArray<NSImage *> *images = [NSMutableArray array];
            if (auto it = entry.find("images"); it != entry.end() && it->second.IsArray()) {
                for (const JsValue &image : it->second.GetJsArray()) {
                    if (NSImage *decoded = image.IsString() ? ImageFromBase64(image.GetString()) : nil) {
                        [images addObject:decoded];
                    }
                }
            }
            auto error = entry.find("isError");
            [view finishWithText:text(entry, "text") images:images
                           error:error != entry.end() && error->second.IsBool() && error->second.GetBool()];
        } else if ([kind isEqualToString:@"note"]) {
            [self addNote:text(entry, "text") color:NSColor.secondaryLabelColor];
        } else if ([kind isEqualToString:@"error"]) {
            [self addNote:text(entry, "text") color:NSColor.systemRedColor];
        }
    }
    // Tool calls that never finished (the app quit).
    for (VelaToolCallView *view in _tools.allValues) {
        if (view.running) [view finishWithText:@"" images:@[] error:NO];
    }
}

- (void)log:(JsObject)entry {
    if (!_logged) {
        _pendingLog.push_back(std::move(entry));
        return;
    }
    vela::claude::AppendEntry(vela::claude::DefaultHistoryFolder(), Text(_conversationId), entry);
}

// The history file starts once the CLI has started the conversation.
- (void)startLog {
    if (_logged) return;
    const std::shared_ptr<vela::Document> document = [self document];
    vela::claude::Conversation conversation;
    conversation.id = Text(_conversationId);
    conversation.title = vela::claude::TitleFor(Text(_request ?: @""));
    conversation.document = document ? DocumentPath(*document) : std::string();
    conversation.created = NSDate.date.timeIntervalSince1970;
    std::string error;
    if (!vela::claude::CreateConversation(vela::claude::DefaultHistoryFolder(), conversation, &error)) {
        vela::Log::Get().Add(vela::LogEntry::Level::Warning, "Claude", error);
        return;
    }
    _logged = YES;
    for (JsObject &entry : _pendingLog) {
        vela::claude::AppendEntry(vela::claude::DefaultHistoryFolder(), conversation.id, entry);
    }
    _pendingLog.clear();
    [self updateControls];
}

#pragma mark The CLI

- (BOOL)startProcess:(NSString **)message {
    std::string error;
    const std::string executable = LocateClaude(&error);
    if (executable.empty()) {
        *message = String(error);
        return NO;
    }
    const std::shared_ptr<vela::Document> document = [self document];
    if (!document) {
        *message = @"The document has closed.";
        return NO;
    }
    // A private socket for this panel's tools, with this document current
    // for every message.
    std::string socketPath = vela::DefaultMcpSocketPath();
    socketPath = socketPath.substr(0, socketPath.rfind('/') + 1) + "claude-" +
        std::to_string(getpid()) + "-" + std::to_string(_sessionId) + ".sock";
    const int sessionId = _sessionId;
    _server = std::make_unique<vela::McpSocketServer>(socketPath,
        [sessionId](const std::function<void()> &function) {
            auto run = [&] {
                vela::Session::ScopedCurrent scope(sessionId);
                function();
            };
            if (NSThread.isMainThread) run();
            else dispatch_sync(dispatch_get_main_queue(), ^{ run(); });
        });
    if (!_server->Start(&error)) {
        _server.reset();
        *message = String("Vela couldn't start Claude's tools: " + error);
        return NO;
    }

    vela::claude::LaunchOptions options;
    options.executable = executable;
    if (_conversationStarted) options.resume = Text(_conversationId);
    else options.sessionId = Text(_conversationId);
    options.model = vela::AppSettingsStore::Get().GetSettings().claudeModel;
    options.effort = vela::AppSettingsStore::Get().GetSettings().claudeEffort;
    _launchedModel = options.model;
    _launchedEffort = options.effort;
    options.mcpHelper = Text([NSBundle.mainBundle.executablePath.stringByDeletingLastPathComponent
                              stringByAppendingPathComponent:@"vela-mcp"]);
    options.mcpSocket = socketPath;
    options.systemPrompt = SystemPrompt;
    if (vela::AppSettingsStore::Get().GetSettings().imageGeneration) {
        static const char *instructions =
#include "app/vela/resources/image_generation.inc"
        ;
        options.systemPrompt += instructions;
    }

    // The CLI works in the scene's folder, so it can read its layers.
    std::string directory = DocumentPath(*document);
    directory = directory.empty() ? vela::claude::DefaultHistoryFolder()
                                  : directory.substr(0, directory.rfind('/'));
    [NSFileManager.defaultManager createDirectoryAtPath:String(directory)
                            withIntermediateDirectories:YES attributes:nil error:nil];

    __weak VelaClaudePanelController *weakSelf = self;
    vela::LineProcess::Callbacks callbacks;
    callbacks.line = [weakSelf](const std::string &line) {
        std::string copy = line;
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf handleLine:copy]; });
    };
    callbacks.exited = [weakSelf](int status, const std::string &errors) {
        std::string copy = errors;
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf processExited:status errors:copy]; });
    };
    _process = std::make_unique<vela::LineProcess>();
    if (!_process->Start(vela::claude::Arguments(options), directory,
                         {"PATH=" + ChildPath(executable), "VELA_MCP_SOCKET=" + socketPath},
                         callbacks, &error)) {
        _process.reset();
        _server.reset();
        *message = String(error);
        return NO;
    }
    vela::Log::Get().Add(vela::LogEntry::Level::Info, "Claude",
                         "Started " + executable + (options.resume.empty() ? "" : " (resuming)"));
    _process->Write(vela::claude::InitializeRequest("init-" + std::to_string(++_nextRequest)));
    return YES;
}

- (BOOL)ask:(NSString *)request error:(NSString **)error {
    NSString *message = nil;
    const BOOL sent = [self send:request message:&message];
    if (!sent && error) *error = message;
    return sent;
}

- (BOOL)send:(NSString *)request message:(NSString **)message {
    (void)self.view;
    if (_busy) {
        *message = @"Claude is still working on the last request.";
        return NO;
    }
    const vela::SessionDocument *entry = vela::Session::Get().Find(_sessionId);
    if (!entry) {
        *message = @"The document has closed.";
        return NO;
    }
    if (!_process || !_process->IsRunning()) {
        [self endProcess];
        if (![self startProcess:message]) return NO;
    }
    // An image of the viewport, when asked for.
    std::vector<std::string> images;
    if (_attach.state == NSControlStateValueOn && entry->screenshot) {
        std::vector<uint8_t> png;
        std::string failure;
        if (entry->screenshot(1024, 0, &png, &failure)) {
            images.push_back(vela::mcp::Base64(png.data(), png.size()));
        } else {
            [self addNote:String("The viewport image failed: " + failure) color:NSColor.systemRedColor];
        }
        _attach.state = NSControlStateValueOff;
    }
    const std::string text = Context(*entry) + Text(request);
    if (!_process->Write(vela::claude::UserMessage(text, images))) {
        *message = @"Claude Code stopped unexpectedly.";
        return NO;
    }
    _request = request;
    vela::images::Service::Get().SetOwner(_sessionId, Text(_conversationId) + ":" + Text(NSUUID.UUID.UUIDString));
    _undoMark = entry->document->GetUndoStack().Mark();
    [self addUser:request];
    if (!images.empty()) {
        if (NSImage *image = ImageFromBase64(images[0])) {
            VelaThumbnailView *thumbnail = [VelaThumbnailView imageViewWithImage:image];
            thumbnail.toolTip = @"Attached viewport image";
            [thumbnail.heightAnchor constraintEqualToConstant:80].active = YES;
            [thumbnail.widthAnchor constraintEqualToConstant:80 * image.size.width / MAX(1.0, image.size.height)].active = YES;
            [self addItem:thumbnail];
        }
    }
    JsObject logEntry{{"kind", JsValue("user")}, {"text", JsValue(Text(request))}};
    [self log:logEntry];
    if (_conversationStarted) [self startLog];
    [self setBusy:YES];
    [self scrollToBottom];
    return YES;
}

- (void)stop {
    if (!_busy || !_process) return;
    _stopping = YES;
    [self statusChanged];
    if (NSString *queued = [self takeQueued]) {
        _input.string = _input.string.length ? [NSString stringWithFormat:@"%@\n\n%@", queued, _input.string]
                                             : queued;
        [_input setNeedsDisplay:YES];
    }
    vela::images::Service::Get().CancelOwner(_sessionId, vela::images::Service::Get().Owner(_sessionId));
    _process->Write(vela::claude::InterruptRequest("stop-" + std::to_string(++_nextRequest)));
    for (NSString *requestId in _approvals.allKeys) [self answer:requestId allow:NO always:NO];
    // If the CLI doesn't end the turn, end the CLI; it resumes next time.
    [_stopTimer invalidate];
    __weak VelaClaudePanelController *weakSelf = self;
    _stopTimer = [NSTimer scheduledTimerWithTimeInterval:5 repeats:NO block:^(NSTimer *) {
        VelaClaudePanelController *strongSelf = weakSelf;
        if (strongSelf && strongSelf->_busy) {
            [strongSelf endProcess];
            [strongSelf finishRequest:@"Stopped." error:NO];
        }
    }];
}

- (void)shutDown {
    [self endProcess];
    for (NSValue *key in [Panels().keyEnumerator.allObjects copy]) {
        if ([Panels() objectForKey:key] == self) [Panels() removeObjectForKey:key];
    }
}

- (void)handleLine:(const std::string &)line {
    for (const vela::claude::Event &event : vela::claude::Parse(line)) [self handleEvent:event];
}

- (void)handleEvent:(const vela::claude::Event &)event {
    using Kind = vela::claude::Event::Kind;
    switch (event.kind) {
        case Kind::Init:
            if (!event.sessionId.empty()) _conversationId = String(event.sessionId);
            _conversationStarted = YES;
            [self startLog];
            if (!event.model.empty()) {
                _modelPopUp.toolTip = String("The model Claude uses (now " + event.model + ")");
            }
            break;
        case Kind::TextDelta:
            if (!_liveText) _liveText = [NSMutableString string];
            [_liveText appendString:String(event.text)];
            [self showReply:[_liveText copy] final:NO];
            break;
        case Kind::Thinking:
            _status.stringValue = @"Thinking…";
            break;
        case Kind::Text:
            [self showReply:String(event.text) final:YES];
            [self log:JsObject{{"kind", JsValue("assistant")}, {"text", JsValue(event.text)}}];
            break;
        case Kind::ToolUse: {
            _live = nil;
            _liveText = nil;
            VelaToolCallView *view = [[VelaToolCallView alloc] initWithTool:String(event.toolName)
                                                                      input:event.input];
            _tools[String(event.toolId)] = view;
            [self addItem:view];
            [self log:JsObject{{"kind", JsValue("tool")}, {"id", JsValue(event.toolId)},
                               {"name", JsValue(event.toolName)}, {"input", JsValue(event.input)}}];
            break;
        }
        case Kind::ToolResult: {
            NSMutableArray<NSImage *> *images = [NSMutableArray array];
            JsArray logged;
            for (const std::string &data : event.images) {
                if (NSImage *image = ImageFromBase64(data)) [images addObject:image];
                logged.push_back(JsValue(data));
            }
            [_tools[String(event.toolId)] finishWithText:String(event.text) images:images
                                                   error:event.isError];
            [self log:JsObject{{"kind", JsValue("tool_result")}, {"id", JsValue(event.toolId)},
                               {"text", JsValue(event.text)}, {"images", JsValue(logged)},
                               {"isError", JsValue(event.isError)}}];
            break;
        }
        case Kind::Permission:
            [self permissionRequested:event];
            break;
        case Kind::Result:
            if (event.costUsd > 0) _cost = event.costUsd;
            if (_stopping) [self finishRequest:@"Stopped." error:NO];
            else [self finishRequest:event.isError ? String(event.text) : nil error:event.isError];
            break;
        case Kind::Other:
            break;
    }
}

// Ends the running request: its edits become one undo step.
- (void)finishRequest:(NSString *)message error:(BOOL)error {
    [_stopTimer invalidate];
    _stopTimer = nil;
    for (NSString *requestId in _approvals.allKeys) [_approvals[requestId] removeFromSuperview];
    [_approvals removeAllObjects];
    _live = nil;
    _liveText = nil;
    if (message.length) {
        [self addNote:message color:error ? NSColor.systemRedColor : NSColor.secondaryLabelColor];
        [self log:JsObject{{"kind", JsValue(error ? "error" : "note")}, {"text", JsValue(Text(message))}}];
    }
    if (const std::shared_ptr<vela::Document> document = [self document]) {
        const std::string name = "Claude: " + vela::claude::TitleFor(Text(_request ?: @""));
        if (document->GetUndoStack().Coalesce(_undoMark, name)) [self addUndoNote:String(name)];
    }
    [self log:JsObject{{"kind", JsValue("result")}, {"isError", JsValue(bool(error))},
                       {"costUsd", JsValue(_cost)}}];
    _request = nil;
    vela::images::Service::Get().SetOwner(_sessionId, "");
    _stopping = NO;
    // Tool calls the CLI never answered.
    for (VelaToolCallView *view in _tools.allValues) {
        if (view.running) [view finishWithText:@"" images:@[] error:NO];
    }
    [self setBusy:NO];
    if (_restartAfterRequest) {
        _restartAfterRequest = NO;
        [self endProcess];
    }
    // The message typed meanwhile goes next.
    if (NSString *queued = [self takeQueued]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString *error = nil;
            if (![self ask:queued error:&error] && error) [self addNote:error color:NSColor.systemRedColor];
        });
    }
}

// "Edited the scene. Undo" after a request that changed it.
- (void)addUndoNote:(NSString *)name {
    NSTextField *label = Label(@"Changed the scene, as one undo step.", Small(),
                               NSColor.secondaryLabelColor);
    NSButton *undo = [NSButton buttonWithTitle:@"Undo" target:self action:@selector(undoRequest:)];
    undo.controlSize = NSControlSizeSmall;
    undo.font = Small();
    undo.bezelStyle = NSBezelStyleInline;
    undo.identifier = name;
    NSStackView *row = Row(@[Label(@"↺", Small(), NSColor.secondaryLabelColor), label, undo]);
    [self addItem:row];
    [self log:JsObject{{"kind", JsValue("note")},
                       {"text", JsValue(std::string("Changed the scene: ") + Text(name))}}];
}

- (void)undoRequest:(NSButton *)sender {
    const std::shared_ptr<vela::Document> document = [self document];
    if (!document) return;
    // Only while the request's step is the one Undo would revert.
    if (document->GetUndoStack().GetUndoName() != Text(sender.identifier)) {
        NSBeep();
        return;
    }
    vela::ActionRegistry::Get().Run(*document, "undo");
    sender.enabled = NO;
    sender.title = @"Undone";
}

#pragma mark Approvals

- (void)permissionRequested:(const vela::claude::Event &)event {
    const bool asks = vela::AppSettingsStore::Get().GetSettings().claudeAsksBeforeEdits;
    auto isQuery = [](const std::string &action) {
        const vela::Action *found = vela::ActionRegistry::Get().Find(action);
        return found && found->query;
    };
    const auto decision = vela::claude::Decide(event.toolName, event.input, asks, isQuery);
    if (decision == vela::claude::Decision::Allow || _allowedTools.count(event.toolName)) {
        _process->Write(vela::claude::AllowTool(event.requestId, event.input));
        return;
    }
    // A card in the transcript.
    NSString *requestId = String(event.requestId);
    NSString *title = String(vela::claude::ToolTitle(event.toolName));
    NSTextField *question = Label([NSString stringWithFormat:@"Allow Claude to use %@?", title],
                                  [NSFont boldSystemFontOfSize:FontSize], NSColor.labelColor);
    NSMutableArray<NSView *> *parts = [NSMutableArray arrayWithObject:question];
    if (!event.text.empty()) {
        NSTextField *description = [NSTextField wrappingLabelWithString:String(event.text)];
        description.font = Small();
        description.textColor = NSColor.secondaryLabelColor;
        [parts addObject:description];
    }
    auto code = event.input.find("code");
    NSString *details = code != event.input.end() && code->second.IsString()
        ? String(code->second.GetString()) : String(vela::mcp::Compact(JsValue(event.input)));
    NSTextView *detailView = TextBlock(Plain(details, Code(), NSColor.textColor));
    [parts addObject:detailView];
    NSButton *allow = [NSButton buttonWithTitle:@"Allow" target:self action:@selector(approvalChosen:)];
    allow.keyEquivalent = @"\r";
    NSButton *always = [NSButton buttonWithTitle:@"Allow for This Conversation" target:self
                                          action:@selector(approvalChosen:)];
    NSButton *deny = [NSButton buttonWithTitle:@"Deny" target:self action:@selector(approvalChosen:)];
    allow.tag = 1;
    always.tag = 2;
    deny.tag = 0;
    for (NSButton *button in @[allow, always, deny]) {
        button.controlSize = NSControlSizeSmall;
        button.identifier = requestId;
    }
    [parts addObject:Row(@[allow, always, deny])];
    NSStackView *card = Column(parts, 6);
    card.edgeInsets = NSEdgeInsetsMake(8, 10, 8, 10);
    card.wantsLayer = YES;
    card.layer.cornerRadius = 8;
    card.layer.borderWidth = 1;
    card.layer.borderColor = NSColor.systemOrangeColor.CGColor;
    [detailView.widthAnchor constraintEqualToAnchor:card.widthAnchor constant:-20].active = YES;
    // The card remembers what it asks about.
    objc_setAssociatedObject(card, "tool", String(event.toolName), OBJC_ASSOCIATION_COPY);
    objc_setAssociatedObject(card, "input",
        String(vela::mcp::Compact(JsValue(event.input))), OBJC_ASSOCIATION_COPY);
    _approvals[requestId] = card;
    [self addItem:card];
    [card.widthAnchor constraintEqualToAnchor:_transcript.widthAnchor constant:-28].active = YES;
    _status.stringValue = @"Waiting for your approval";
    [NSApp requestUserAttention:NSInformationalRequest];
}

- (void)approvalChosen:(NSButton *)sender {
    [self answer:sender.identifier allow:sender.tag != 0 always:sender.tag == 2];
}

- (void)answer:(NSString *)requestId allow:(BOOL)allow always:(BOOL)always {
    NSView *card = _approvals[requestId];
    if (!card) return;
    [_approvals removeObjectForKey:requestId];
    NSString *tool = objc_getAssociatedObject(card, "tool");
    NSString *inputText = objc_getAssociatedObject(card, "input");
    const JsValue input = JsParseString(Text(inputText));
    if (_process) {
        _process->Write(allow
            ? vela::claude::AllowTool(Text(requestId), input.IsObject() ? input.GetJsObject() : JsObject())
            : vela::claude::DenyTool(Text(requestId), "The user denied this."));
    }
    if (always) _allowedTools.insert(Text(tool));
    // The card becomes a line saying what was decided.
    NSString *title = String(vela::claude::ToolTitle(Text(tool)));
    NSString *outcome = allow ? (always ? [NSString stringWithFormat:@"Allowed %@ for this conversation.", title]
                                        : [NSString stringWithFormat:@"Allowed %@.", title])
                              : [NSString stringWithFormat:@"Denied %@.", title];
    NSTextView *note = TextBlock(Plain(outcome, Small(), NSColor.secondaryLabelColor));
    const NSUInteger index = [_transcript.arrangedSubviews indexOfObject:card];
    [card removeFromSuperview];
    if (index != NSNotFound) {
        [_transcript insertArrangedSubview:note atIndex:index];
        [note.widthAnchor constraintEqualToAnchor:_transcript.widthAnchor constant:-28].active = YES;
    }
    [self statusChanged];
}

- (void)processExited:(int)status errors:(const std::string &)errors {
    _process.reset();
    _server.reset();
    if (!_busy) return;
    std::string message = "Claude Code stopped";
    if (WIFEXITED(status)) message += " (exit status " + std::to_string(WEXITSTATUS(status)) + ")";
    message += ".";
    std::string tail = errors;
    while (!tail.empty() && (tail.back() == '\n' || tail.back() == ' ')) tail.pop_back();
    if (!tail.empty()) message += "\n" + tail;
    if (tail.find("login") != std::string::npos || tail.find("logged in") != std::string::npos) {
        message += "\nRun `claude` in Terminal once to log in.";
    }
    vela::Log::Get().Add(vela::LogEntry::Level::Warning, "Claude", message);
    [self finishRequest:String(message) error:YES];
}

@end

void VelaRegisterClaudeActions(void) {
    vela::Action ask;
    ask.name = "ask_claude";
    ask.title = "Ask Claude…";
    ask.description = "Send a request to the document's Claude panel, as if typed there, "
                      "and show the panel.";
    ask.parameters = vela::ParameterSchema(R"({
        "properties": {"prompt": {"type": "string", "description": "The request"}},
        "required": ["prompt"]})");
    ask.form = [](const vela::Document &) {
        vela::FormField field;
        field.key = "prompt";
        field.label = "Request";
        field.type = vela::FormField::Type::String;
        field.value = JsValue(std::string());
        return std::vector<vela::FormField>{field};
    };
    ask.run = [](vela::Document &document, const JsObject &params) {
        VelaClaudePanelController *panel =
            [Panels() objectForKey:[NSValue valueWithPointer:&document]];
        if (!panel) return vela::ActionResult::Error("This document has no Claude panel.");
        const std::string prompt = params.at("prompt").GetString();
        if (prompt.find_first_not_of(" \t\r\n") == std::string::npos) {
            return vela::ActionResult::Error("The request is empty.");
        }
        // Show the panel (its view has no window while it is collapsed),
        // then send.
        const SEL show = NSSelectorFromString(@"showClaudeConsole:");
        for (NSDocument *open in NSDocumentController.sharedDocumentController.documents) {
            if (![open isKindOfClass:[VelaDocument class]] ||
                ((VelaDocument *)open).core.get() != &document) {
                continue;
            }
            for (NSWindowController *controller in open.windowControllers) {
                if ([controller respondsToSelector:show]) {
                    [NSApp sendAction:show to:controller from:nil];
                }
            }
        }
        NSString *error = nil;
        if (![panel ask:String(prompt) error:&error]) {
            return vela::ActionResult::Error(Text(error ?: @"Claude couldn't start."));
        }
        return vela::ActionResult{};
    };
    vela::ActionRegistry::Get().Register(std::move(ask));

    vela::Action stop;
    stop.name = "stop_claude";
    stop.title = "Stop Claude";
    stop.description = "Stop the request the document's Claude panel is working on.";
    stop.enabled = [](const vela::Document &document) {
        VelaClaudePanelController *panel = [Panels() objectForKey:[NSValue valueWithPointer:&document]];
        return panel && panel.busy;
    };
    stop.run = [](vela::Document &document, const JsObject &) {
        VelaClaudePanelController *panel = [Panels() objectForKey:[NSValue valueWithPointer:&document]];
        if (!panel || !panel.busy) return vela::ActionResult::Error("Claude isn't working on anything.");
        [panel stopRequest];
        return vela::ActionResult{};
    };
    vela::ActionRegistry::Get().Register(std::move(stop));
}
