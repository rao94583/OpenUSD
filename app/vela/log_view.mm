#import "log_view.h"

#include "app/vela/core/log.h"

#include <atomic>
#include <vector>

NSNotificationName const VelaLogChanged = @"VelaLogChanged";

void VelaStartLog(void) {
    // Entries arrive from any thread; one notification per main loop pass.
    static std::atomic<bool> pending{false};
    vela::Log::Get().SetChangedCallback([] {
        if (pending.exchange(true)) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            pending = false;
            [NSNotificationCenter.defaultCenter postNotificationName:VelaLogChanged object:nil];
        });
    });
    vela::Log::Get().CaptureDiagnostics();
}

namespace {

NSColor *LevelColor(vela::LogEntry::Level level) {
    switch (level) {
    case vela::LogEntry::Level::Error: return NSColor.systemRedColor;
    case vela::LogEntry::Level::Warning: return NSColor.systemOrangeColor;
    case vela::LogEntry::Level::Info: break;
    }
    return NSColor.labelColor;
}

NSString *LevelSymbol(vela::LogEntry::Level level) {
    switch (level) {
    case vela::LogEntry::Level::Error: return @"xmark.octagon.fill";
    case vela::LogEntry::Level::Warning: return @"exclamationmark.triangle.fill";
    case vela::LogEntry::Level::Info: break;
    }
    return @"info.circle";
}

NSString *Text(const std::string &text) { return @(text.c_str()); }

} // namespace

@interface VelaLogViewController () <NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate>
@end

@implementation VelaLogViewController {
    std::vector<vela::LogEntry> _entries;  // all, oldest first
    std::vector<size_t> _shown;            // indexes into _entries
    NSTableView *_table;
    NSButton *_errors, *_warnings, *_messages;
    NSSearchField *_search;
    NSDateFormatter *_timeFormat;
}

- (void)loadView {
    _timeFormat = [NSDateFormatter new];
    _timeFormat.dateFormat = @"HH:mm:ss.SSS";
    auto toggle = [&](NSString *title) {
        NSButton *button = [NSButton buttonWithTitle:title target:self action:@selector(filterChanged:)];
        button.buttonType = NSButtonTypePushOnPushOff;
        button.bezelStyle = NSBezelStyleRecessed;
        button.controlSize = NSControlSizeSmall;
        button.state = NSControlStateValueOn;
        return button;
    };
    _errors = toggle(@"Errors");
    _warnings = toggle(@"Warnings");
    _messages = toggle(@"Messages");
    _search = [NSSearchField new];
    _search.placeholderString = @"Filter";
    _search.controlSize = NSControlSizeSmall;
    _search.delegate = self;
    [_search.widthAnchor constraintGreaterThanOrEqualToConstant:160].active = YES;
    NSButton *clear = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearLog:)];
    clear.bezelStyle = NSBezelStyleRecessed;
    clear.controlSize = NSControlSizeSmall;
    NSView *spacer = [NSView new];
    [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *bar = [NSStackView stackViewWithViews:@[_errors, _warnings, _messages, _search, spacer, clear]];
    bar.spacing = 6;
    bar.edgeInsets = NSEdgeInsetsMake(4, 8, 4, 8);
    // Stack views hug their content above the window's size (500): left
    // alone, the bar would shrink the window to its own width.
    [bar setHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];

    _table = [NSTableView new];
    _table.dataSource = self;
    _table.delegate = self;
    _table.allowsMultipleSelection = YES;
    _table.usesAlternatingRowBackgroundColors = YES;
    _table.style = NSTableViewStylePlain;
    _table.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
    NSDictionary<NSString *, NSNumber *> *widths =
        @{@"Time": @96, @"Level": @24, @"Source": @80, @"Message": @700};
    for (NSString *name in @[@"Time", @"Level", @"Source", @"Message"]) {
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:name];
        column.title = [name isEqualToString:@"Level"] ? @"" : name;
        column.width = widths[name].doubleValue;
        [_table addTableColumn:column];
    }
    NSScrollView *scroll = [NSScrollView new];
    scroll.hasVerticalScroller = YES;
    scroll.documentView = _table;

    NSView *root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 800, 220)];
    for (NSView *view in @[bar, scroll]) {
        view.translatesAutoresizingMaskIntoConstraints = NO;
        [root addSubview:view];
    }
    [NSLayoutConstraint activateConstraints:@[
        [bar.topAnchor constraintEqualToAnchor:root.topAnchor],
        [bar.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [bar.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [scroll.topAnchor constraintEqualToAnchor:bar.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:root.bottomAnchor]]];
    self.view = root;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(logChanged:)
                                               name:VelaLogChanged object:nil];
    [self logChanged:nil];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (vela::LogFilter)filter {
    vela::LogFilter filter;
    filter.errors = _errors.state == NSControlStateValueOn;
    filter.warnings = _warnings.state == NSControlStateValueOn;
    filter.info = _messages.state == NSControlStateValueOn;
    filter.query = _search.stringValue.UTF8String;
    return filter;
}

// At most once per main loop pass. The table follows new entries while
// it is scrolled to the end.
- (void)logChanged:(NSNotification *)notification {
    _entries = vela::Log::Get().GetEntries();
    const NSRect visible = _table.enclosingScrollView.contentView.documentVisibleRect;
    const BOOL atEnd = NSMaxY(visible) >= NSHeight(_table.bounds) - 2 * _table.rowHeight;
    [self refilter];
    if (atEnd && !_shown.empty()) [_table scrollRowToVisible:NSInteger(_shown.size()) - 1];
}

- (void)refilter {
    const vela::LogFilter filter = [self filter];
    _shown.clear();
    size_t counts[3] = {0, 0, 0};
    for (size_t i = 0; i < _entries.size(); ++i) {
        ++counts[size_t(_entries[i].level)];
        if (filter.Matches(_entries[i])) _shown.push_back(i);
    }
    _errors.title = [NSString stringWithFormat:@"Errors %zu", counts[0]];
    _warnings.title = [NSString stringWithFormat:@"Warnings %zu", counts[1]];
    _messages.title = [NSString stringWithFormat:@"Messages %zu", counts[2]];
    [_table reloadData];
}

- (void)filterChanged:(id)sender { [self refilter]; }
- (void)controlTextDidChange:(NSNotification *)notification { [self refilter]; }

- (void)clearLog:(id)sender {
    vela::Log::Get().Clear();
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)table { return NSInteger(_shown.size()); }

- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    const vela::LogEntry &entry = _entries[_shown[size_t(row)]];
    if ([column.identifier isEqualToString:@"Level"]) {
        NSImageView *image = [table makeViewWithIdentifier:@"Level" owner:self];
        if (!image) {
            image = [NSImageView new];
            image.identifier = @"Level";
        }
        image.image = [NSImage imageWithSystemSymbolName:LevelSymbol(entry.level)
                                accessibilityDescription:@(vela::LevelName(entry.level))];
        image.contentTintColor = entry.level == vela::LogEntry::Level::Info
            ? NSColor.secondaryLabelColor : LevelColor(entry.level);
        image.toolTip = @(vela::LevelName(entry.level));
        return image;
    }
    NSTextField *label = [table makeViewWithIdentifier:column.identifier owner:self];
    if (!label) {
        label = [NSTextField labelWithString:@""];
        label.identifier = column.identifier;
        label.lineBreakMode = NSLineBreakByTruncatingTail;
    }
    if ([column.identifier isEqualToString:@"Time"]) {
        label.stringValue = [_timeFormat stringFromDate:[NSDate dateWithTimeIntervalSince1970:entry.time]];
        label.textColor = NSColor.secondaryLabelColor;
        label.font = [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize
                                                      weight:NSFontWeightRegular];
    } else if ([column.identifier isEqualToString:@"Source"]) {
        label.stringValue = Text(entry.source);
        label.textColor = NSColor.secondaryLabelColor;
        label.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    } else {
        // One line per entry; the whole message is in the tooltip and copies.
        NSString *message = Text(entry.message);
        label.stringValue = [[message componentsSeparatedByCharactersInSet:
                              NSCharacterSet.newlineCharacterSet] componentsJoinedByString:@"  ⏎  "];
        label.toolTip = message;
        label.textColor = LevelColor(entry.level);
        label.font = [NSFont monospacedSystemFontOfSize:NSFont.smallSystemFontSize
                                                 weight:NSFontWeightRegular];
    }
    return label;
}

// Edit → Copy: the selected entries as text.
- (void)copy:(id)sender {
    NSMutableString *text = [NSMutableString string];
    NSIndexSet *rows = _table.selectedRowIndexes;
    for (NSUInteger row = rows.firstIndex; row != NSNotFound; row = [rows indexGreaterThanIndex:row]) {
        const vela::LogEntry &entry = _entries[_shown[row]];
        [text appendFormat:@"%@  %s  %@  %@\n",
            [_timeFormat stringFromDate:[NSDate dateWithTimeIntervalSince1970:entry.time]],
            vela::LevelName(entry.level), Text(entry.source), Text(entry.message)];
    }
    if (!text.length) return;
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:text forType:NSPasteboardTypeString];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(copy:)) return _table.selectedRowIndexes.count > 0;
    return YES;
}

@end
