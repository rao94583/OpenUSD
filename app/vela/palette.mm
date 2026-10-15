#import "palette.h"
#import "document.h"
#import "menus.h"

#include "app/vela/core/actions.h"
#include "app/vela/core/palette.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

NSString *const RecentsKey = @"VelaPaletteRecents";
const size_t MaxRecents = 20, MaxResults = 50;

// App commands (not actions) carry this prefix and their menu item.
const std::string AppPrefix = "app:";

vela::Document *DocumentOf(NSWindow *window) {
    id document = window.windowController.document;
    return [document isKindOfClass:[VelaDocument class]] ? ((VelaDocument *)document).core.get()
                                                         : nullptr;
}

std::string Text(NSString *string) { return string.UTF8String ?: ""; }

NSString *ShortcutOf(NSMenuItem *item) {
    if (!item.keyEquivalent.length) return @"";
    const NSEventModifierFlags flags = item.keyEquivalentModifierMask;
    NSMutableString *text = [NSMutableString string];
    if (flags & NSEventModifierFlagControl) [text appendString:@"⌃"];
    if (flags & NSEventModifierFlagOption) [text appendString:@"⌥"];
    if (flags & NSEventModifierFlagShift) [text appendString:@"⇧"];
    if (flags & NSEventModifierFlagCommand) [text appendString:@"⌘"];
    NSString *key = [item.keyEquivalent isEqualToString:@" "] ? @"Space"
                                                              : item.keyEquivalent.uppercaseString;
    [text appendString:key];
    return text;
}

} // namespace

// A panel that can take keyboard focus without a title bar.
@interface VelaPalettePanel : NSPanel
@end
@implementation VelaPalettePanel
- (BOOL)canBecomeKeyWindow { return YES; }
@end

@interface VelaPaletteController () <NSTextFieldDelegate, NSTableViewDataSource,
                                     NSTableViewDelegate, NSWindowDelegate>
@end

@implementation VelaPaletteController {
    NSTextField *_search;
    NSTableView *_table;
    NSScrollView *_scroll;
    // The window the palette acts on, and what it offers there.
    __weak NSWindow *_target;
    std::vector<vela::PaletteEntry> _entries;
    std::vector<vela::PaletteEntry> _results;
    NSMutableDictionary<NSString *, NSMenuItem *> *_appItems;
}

+ (instancetype)shared {
    static VelaPaletteController *controller = [VelaPaletteController new];
    return controller;
}

- (instancetype)init {
    VelaPalettePanel *panel = [[VelaPalettePanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 560, 360)
                  styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                    backing:NSBackingStoreBuffered defer:YES];
    panel.floatingPanel = YES;
    panel.level = NSFloatingWindowLevel;
    panel.opaque = NO;
    panel.backgroundColor = NSColor.clearColor;
    panel.hasShadow = YES;
    panel.hidesOnDeactivate = YES;
    if (!(self = [super initWithWindow:panel])) return nil;
    panel.delegate = self;

    NSVisualEffectView *background = [[NSVisualEffectView alloc] initWithFrame:panel.contentView.bounds];
    background.material = NSVisualEffectMaterialPopover;
    background.state = NSVisualEffectStateActive;
    background.wantsLayer = YES;
    background.layer.cornerRadius = 12;
    background.layer.masksToBounds = YES;
    panel.contentView = background;

    _search = [NSTextField textFieldWithString:@""];
    _search.placeholderString = @"Search commands, or / for prims";
    _search.font = [NSFont systemFontOfSize:20];
    _search.bezeled = NO;
    _search.drawsBackground = NO;
    _search.focusRingType = NSFocusRingTypeNone;
    _search.delegate = self;
    _search.translatesAutoresizingMaskIntoConstraints = NO;

    _table = [[NSTableView alloc] init];
    NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"entry"];
    [_table addTableColumn:column];
    _table.headerView = nil;
    _table.rowHeight = 30;
    _table.backgroundColor = NSColor.clearColor;
    _table.style = NSTableViewStyleSourceList;
    _table.dataSource = self;
    _table.delegate = self;
    _table.target = self;
    _table.doubleAction = @selector(runSelected:);
    _table.action = @selector(runSelected:);
    _scroll = [[NSScrollView alloc] init];
    _scroll.documentView = _table;
    _scroll.hasVerticalScroller = YES;
    _scroll.drawsBackground = NO;
    _scroll.translatesAutoresizingMaskIntoConstraints = NO;

    NSBox *line = [[NSBox alloc] init];
    line.boxType = NSBoxSeparator;
    line.translatesAutoresizingMaskIntoConstraints = NO;
    [background addSubview:_search];
    [background addSubview:line];
    [background addSubview:_scroll];
    [NSLayoutConstraint activateConstraints:@[
        [_search.topAnchor constraintEqualToAnchor:background.topAnchor constant:14],
        [_search.leadingAnchor constraintEqualToAnchor:background.leadingAnchor constant:16],
        [_search.trailingAnchor constraintEqualToAnchor:background.trailingAnchor constant:-16],
        [line.topAnchor constraintEqualToAnchor:_search.bottomAnchor constant:10],
        [line.leadingAnchor constraintEqualToAnchor:background.leadingAnchor],
        [line.trailingAnchor constraintEqualToAnchor:background.trailingAnchor],
        [_scroll.topAnchor constraintEqualToAnchor:line.bottomAnchor constant:4],
        [_scroll.leadingAnchor constraintEqualToAnchor:background.leadingAnchor constant:6],
        [_scroll.trailingAnchor constraintEqualToAnchor:background.trailingAnchor constant:-6],
        [_scroll.bottomAnchor constraintEqualToAnchor:background.bottomAnchor constant:-6],
    ]];
    return self;
}

#pragma mark Showing

- (void)toggle:(id)sender {
    if (self.window.isVisible) {
        [self close];
        return;
    }
    // The front document window, which the palette acts on.
    _target = nil;
    for (NSWindow *window in NSApp.orderedWindows) {
        if (DocumentOf(window)) { _target = window; break; }
    }
    [self collectEntries];
    _search.stringValue = @"";
    [self search];
    // Centered over the window (or the screen), a quarter of the way down.
    NSRect area = _target ? _target.frame : NSScreen.mainScreen.visibleFrame;
    NSRect frame = self.window.frame;
    frame.origin.x = NSMidX(area) - frame.size.width / 2;
    frame.origin.y = NSMaxY(area) - area.size.height / 4 - frame.size.height / 2;
    [self.window setFrame:frame display:NO];
    [self.window makeKeyAndOrderFront:sender];
    [self.window makeFirstResponder:_search];
}

- (void)windowDidResignKey:(NSNotification *)notification {
    [self close];
}

// The registry's entries for the target document, and the main menu's
// other commands, enabled ones only.
- (void)collectEntries {
    _entries.clear();
    _appItems = [NSMutableDictionary dictionary];
    if (vela::Document *document = DocumentOf(_target)) {
        _entries = vela::PaletteEntries(vela::ActionRegistry::Get(), *document);
    }
    for (NSMenuItem *top in NSApp.mainMenu.itemArray) {
        NSMenu *menu = top.submenu;
        [menu update];  // validates its items
        for (NSMenuItem *item in menu.itemArray) {
            // Actions are already listed; skip separators and submenus.
            if (item.isSeparatorItem || item.submenu || item.representedObject ||
                !item.action || item.action == @selector(toggle:) || !item.enabled) {
                continue;
            }
            const std::string key = AppPrefix + Text(menu.title) + "/" + Text(item.title);
            _appItems[@(key.c_str())] = item;
            _entries.push_back({Text(item.title), Text(menu.title), Text(ShortcutOf(item)), key});
        }
    }
}

- (void)search {
    const std::string query = Text([_search.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]);
    vela::Document *document = DocumentOf(_target);
    if (!query.empty() && query[0] == '/') {
        _results = document ? vela::PrimEntries(*document, query, MaxResults)
                            : std::vector<vela::PaletteEntry>();
    } else {
        std::vector<std::string> recents;
        for (NSString *key in [NSUserDefaults.standardUserDefaults stringArrayForKey:RecentsKey]) {
            recents.push_back(Text(key));
        }
        _results = vela::SearchPalette(_entries, query, recents, MaxResults);
    }
    [_table reloadData];
    if (!_results.empty()) {
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
        [_table scrollRowToVisible:0];
    }
}

#pragma mark Running

- (void)runSelected:(id)sender {
    const NSInteger row = _table.selectedRow;
    if (row < 0 || size_t(row) >= _results.size()) return;
    const vela::PaletteEntry entry = _results[size_t(row)];
    NSWindow *target = _target;
    [self close];
    [self remember:entry];
    if (entry.action.rfind(AppPrefix, 0) == 0) {
        NSMenuItem *item = _appItems[@(entry.action.c_str())];
        if (item) [NSApp sendAction:item.action to:item.target from:item];
        return;
    }
    // Act on the target window's document, as its menus do.
    [target makeKeyAndOrderFront:nil];
    VelaRunAction(entry.action, entry.params, target);
}

- (void)remember:(const vela::PaletteEntry &)entry {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSMutableArray<NSString *> *recents =
        [[defaults stringArrayForKey:RecentsKey] mutableCopy] ?: [NSMutableArray array];
    NSString *key = @(entry.Key().c_str());
    [recents removeObject:key];
    [recents insertObject:key atIndex:0];
    while (recents.count > MaxRecents) [recents removeLastObject];
    [defaults setObject:recents forKey:RecentsKey];
}

#pragma mark Keys and table

- (void)controlTextDidChange:(NSNotification *)notification {
    [self search];
}

- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView
    doCommandBySelector:(SEL)command {
    const NSInteger rows = NSInteger(_results.size());
    NSInteger row = _table.selectedRow;
    if (command == @selector(moveDown:) || command == @selector(moveUp:)) {
        if (rows == 0) return YES;
        row = command == @selector(moveDown:) ? std::min(row + 1, rows - 1) : std::max(row - 1, NSInteger(0));
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:NSUInteger(row)] byExtendingSelection:NO];
        [_table scrollRowToVisible:row];
        return YES;
    }
    if (command == @selector(insertNewline:)) {
        [self runSelected:nil];
        return YES;
    }
    if (command == @selector(cancelOperation:)) {
        [self close];
        return YES;
    }
    return NO;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView {
    return NSInteger(_results.size());
}

- (NSView *)tableView:(NSTableView *)tableView viewForTableColumn:(NSTableColumn *)column
                  row:(NSInteger)row {
    NSTableCellView *cell = [tableView makeViewWithIdentifier:@"entry" owner:self];
    NSTextField *menu, *shortcut;
    if (!cell) {
        cell = [[NSTableCellView alloc] init];
        cell.identifier = @"entry";
        NSTextField *title = [NSTextField labelWithString:@""];
        title.font = [NSFont systemFontOfSize:14];
        title.lineBreakMode = NSLineBreakByTruncatingTail;
        menu = [NSTextField labelWithString:@""];
        menu.textColor = NSColor.secondaryLabelColor;
        menu.tag = 1;
        shortcut = [NSTextField labelWithString:@""];
        shortcut.textColor = NSColor.secondaryLabelColor;
        shortcut.tag = 2;
        for (NSTextField *field in @[title, menu, shortcut]) {
            field.translatesAutoresizingMaskIntoConstraints = NO;
            [cell addSubview:field];
        }
        [title setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
        [NSLayoutConstraint activateConstraints:@[
            [title.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:8],
            [title.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
            [menu.leadingAnchor constraintGreaterThanOrEqualToAnchor:title.trailingAnchor constant:12],
            [menu.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
            [shortcut.leadingAnchor constraintEqualToAnchor:menu.trailingAnchor constant:12],
            [shortcut.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-8],
            [shortcut.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        ]];
        cell.textField = title;
    }
    menu = [cell viewWithTag:1];
    shortcut = [cell viewWithTag:2];
    const vela::PaletteEntry &entry = _results[size_t(row)];
    cell.textField.stringValue = [NSString stringWithFormat:@"%@%s",
        entry.checked ? @"✓ " : @"", entry.title.c_str()];
    menu.stringValue = @(entry.menu.c_str());
    shortcut.stringValue = @(entry.shortcut.c_str());
    return cell;
}

@end
