#import "browser_view.h"
#include "app/vela/core/browser.h"
#include "app/vela/core/actions.h"

PXR_NAMESPACE_USING_DIRECTIVE

@interface VelaBrowserController () <NSOutlineViewDataSource, NSOutlineViewDelegate,
    NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate>
@end

@implementation VelaBrowserController {
    std::shared_ptr<vela::Document> _document;
    std::unique_ptr<vela::Browser> _browser;
    int _observer;
    NSView *_outlineView;
    NSView *_inspectorView;
    NSOutlineView *_outline;
    NSSearchField *_search;
    NSMutableDictionary<NSString *, NSArray<NSString *> *> *_children;
    NSMutableDictionary<NSString *, NSString *> *_items;
    NSMutableArray<NSTableView *> *_tables;
    std::vector<vela::InspectorRows> _rows;
    NSTextField *_selectionLabel;
    BOOL _syncing;
    BOOL _reloadScheduled;
    NSWindow *_detailWindow;
    NSTableView *_detailTable;
    vela::InspectorRows _detailRows;
    std::string _detailProperty;
    SdfPath _detailPath;
    size_t _detailOffset;
    NSButton *_nextPage;
    NSButton *_previousPage;
    NSTextField *_pageLabel;
}

- (instancetype)initWithDocument:(std::shared_ptr<vela::Document>)document {
    if (!(self = [super init])) return nil;
    _document = std::move(document);
    _browser = std::make_unique<vela::Browser>(*_document);
    _children = [NSMutableDictionary dictionary];
    _items = [NSMutableDictionary dictionary];
    _tables = [NSMutableArray array];
    _rows.resize(5);
    _search = [NSSearchField new];
    _search.placeholderString = @"Filter by path or type";
    _search.delegate = self;
    _outline = [NSOutlineView new];
    _outline.dataSource = self; _outline.delegate = self;
    _outline.allowsMultipleSelection = YES;
    _outline.columnAutoresizingStyle = NSTableViewNoColumnAutoresizing;
    for (NSString *name in @[@"Prim", @"Type", @"Visibility", @"Draw Mode"]) {
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:name];
        column.title = name; column.width = [name isEqualToString:@"Prim"] ? 210 : 95;
        [_outline addTableColumn:column];
    }
    _outline.outlineTableColumn = _outline.tableColumns.firstObject;
    NSScrollView *tree = [NSScrollView new];
    tree.hasVerticalScroller = YES; tree.hasHorizontalScroller = YES;
    tree.documentView = _outline;
    NSStackView *left = [NSStackView stackViewWithViews:@[_search, tree]];
    left.orientation = NSUserInterfaceLayoutOrientationVertical;
    left.alignment = NSLayoutAttributeWidth; left.spacing = 6;
    left.edgeInsets = NSEdgeInsetsMake(8, 6, 6, 6);
    [_search setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationVertical];
    [tree setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    _outlineView = left;

    _selectionLabel = [NSTextField labelWithString:@"No selection"];
    _selectionLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    NSTabView *tabs = [NSTabView new];
    NSArray<NSString *> *names = @[@"Properties", @"Metadata", @"Composition", @"Layers", @"Variants"];
    for (NSInteger i = 0; i < names.count; ++i) {
        NSTableView *table = [self makeTable];
        table.tag = i;
        if (i == 0) { table.target = self; table.doubleAction = @selector(openProperty:); }
        [_tables addObject:table];
        NSTabViewItem *tab = [[NSTabViewItem alloc] initWithIdentifier:names[i]];
        tab.label = names[i];
        NSScrollView *scroll = [NSScrollView new];
        scroll.hasVerticalScroller = YES; scroll.hasHorizontalScroller = YES;
        scroll.documentView = table;
        tab.view = scroll; [tabs addTabViewItem:tab];
    }
    NSStackView *right = [NSStackView stackViewWithViews:@[_selectionLabel, tabs]];
    right.orientation = NSUserInterfaceLayoutOrientationVertical;
    right.alignment = NSLayoutAttributeWidth; right.spacing = 6;
    right.edgeInsets = NSEdgeInsetsMake(8, 6, 6, 6);
    [_selectionLabel setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationVertical];
    [tabs setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    _inspectorView = right;

    __weak VelaBrowserController *weakSelf = self;
    _observer = _document->AddObserver([weakSelf](unsigned changes) {
        [weakSelf documentChanged:changes];
    });
    [_outline reloadData]; [self refreshInspector]; [self syncSelection];
    return self;
}
- (void)dealloc { _document->RemoveObserver(_observer); [_detailWindow close]; }
- (NSView *)outlineView { return _outlineView; }
- (NSView *)inspectorView { return _inspectorView; }
- (NSTableView *)makeTable {
    NSTableView *table = [NSTableView new];
    table.dataSource = self; table.delegate = self;
    table.columnAutoresizingStyle = NSTableViewNoColumnAutoresizing;
    for (NSString *name in @[@"Name", @"Type", @"Value", @"Source"]) {
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:name];
        column.title = name; column.width = [name isEqualToString:@"Value"] ? 220 : 150;
        [table addTableColumn:column];
    }
    return table;
}
- (NSString *)itemForPath:(const SdfPath &)path {
    NSString *key = @(path.GetText());
    if (!_items[key]) _items[key] = key;
    return _items[key];
}
- (NSArray<NSString *> *)childrenOf:(NSString *)parent {
    NSString *key = parent ?: @"/";
    NSArray *cached = _children[key];
    if (cached) return cached;
    NSMutableArray *paths = [NSMutableArray array];
    for (const auto &path : _browser->Children(SdfPath(key.UTF8String))) [paths addObject:[self itemForPath:path]];
    _children[key] = paths;
    return paths;
}
- (NSInteger)outlineView:(NSOutlineView *)view numberOfChildrenOfItem:(id)item {
    return [self childrenOf:item].count;
}
- (id)outlineView:(NSOutlineView *)view child:(NSInteger)index ofItem:(id)item {
    return [self childrenOf:item][index];
}
- (BOOL)outlineView:(NSOutlineView *)view isItemExpandable:(id)item {
    return _browser->HasChildren(SdfPath([(NSString *)item UTF8String]));
}
- (NSView *)outlineView:(NSOutlineView *)view viewForTableColumn:(NSTableColumn *)column item:(id)item {
    const auto summary = _browser->Summary(SdfPath([(NSString *)item UTF8String]));
    std::string value;
    if ([column.identifier isEqualToString:@"Prim"]) value = summary.name;
    else if ([column.identifier isEqualToString:@"Type"]) value = summary.type;
    else if ([column.identifier isEqualToString:@"Visibility"]) value = summary.visibility;
    else value = summary.drawMode;
    NSTextField *label = [view makeViewWithIdentifier:column.identifier owner:self];
    if (!label) { label = [NSTextField labelWithString:@""]; label.identifier = column.identifier; }
    label.stringValue = @(value.c_str()); label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    label.textColor = summary.active && summary.loaded ? NSColor.labelColor : NSColor.secondaryLabelColor;
    label.toolTip = [NSString stringWithFormat:@"%@%@%@", item,
        summary.active ? @"" : @" (inactive)", summary.instanceProxy ? @" (instance proxy)" : @""];
    return label;
}
- (void)outlineViewSelectionDidChange:(NSNotification *)notification {
    if (_syncing) return;
    JsArray paths;
    NSIndexSet *selected = _outline.selectedRowIndexes;
    for (NSUInteger index = selected.firstIndex; index != NSNotFound; index = [selected indexGreaterThanIndex:index]) {
        NSString *path = [_outline itemAtRow:index];
        if (path) paths.push_back(JsValue(path.UTF8String));
    }
    vela::ActionRegistry::Get().Run(*_document, "select", {{"paths", JsValue(paths)}});
}
- (void)syncSelection {
    _syncing = YES;
    NSMutableIndexSet *selected = [NSMutableIndexSet indexSet];
    for (const auto &path : _document->GetSelection()) {
        std::vector<SdfPath> ancestors;
        for (SdfPath parent = path.GetParentPath(); parent.IsPrimPath(); parent = parent.GetParentPath()) ancestors.push_back(parent);
        for (auto i = ancestors.rbegin(); i != ancestors.rend(); ++i) [_outline expandItem:[self itemForPath:*i]];
        NSInteger row = [_outline rowForItem:[self itemForPath:path]];
        if (row >= 0) [selected addIndex:row];
    }
    [_outline selectRowIndexes:selected byExtendingSelection:NO];
    if (selected.count) [_outline scrollRowToVisible:selected.lastIndex];
    _syncing = NO;
}
- (void)controlTextDidChange:(NSNotification *)notification {
    _browser->SetFilter(_search.stringValue.UTF8String);
    _syncing = YES; [_children removeAllObjects]; [_outline reloadData]; _syncing = NO;
    [self syncSelection];
}
- (void)documentChanged:(unsigned)changes {
    if (changes & vela::Document::StageContent) {
        // Coalesce synchronous USD notices. Reloading a tree inside a selection
        // callback would invalidate AppKit's current row/items.
        if (!_reloadScheduled) {
            _reloadScheduled = YES;
            __weak VelaBrowserController *weakSelf = self;
            dispatch_async(dispatch_get_main_queue(), ^{
                VelaBrowserController *s = weakSelf;
                if (!s) return;
                s->_reloadScheduled = NO; s->_syncing = YES;
                [s->_children removeAllObjects]; [s->_outline reloadData]; s->_syncing = NO;
                [s syncSelection]; [s refreshInspector];
            });
        }
    } else {
        if (changes & vela::Document::Selection) [self syncSelection];
        if (changes & (vela::Document::Selection | vela::Document::Time | vela::Document::EditTarget | vela::Document::UndoState)) [self refreshInspector];
        if (changes & vela::Document::Time) { _syncing = YES; [_outline reloadData]; _syncing = NO; [self syncSelection]; }
    }
}
- (SdfPath)inspectedPath {
    const auto &selection = _document->GetSelection();
    return selection.empty() ? SdfPath() : selection.back();
}
- (void)refreshInspector {
    const auto path = [self inspectedPath];
    _selectionLabel.stringValue = path.IsEmpty() ? @"No selection" : @(path.GetText());
    _selectionLabel.toolTip = _document->GetSelection().size() > 1 ? @"Inspecting the last selected prim" : nil;
    _rows[0] = _browser->Properties(path); _rows[1] = _browser->Metadata(path);
    _rows[2] = _browser->Composition(path); _rows[3] = _browser->Layers();
    _rows[4] = _browser->Variants(path);
    for (NSTableView *table in _tables) [table reloadData];
    if (_detailWindow) [self refreshDetails];
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)table {
    return table == _detailTable ? _detailRows.size() : _rows[table.tag].size();
}
- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)index {
    const auto &rows = table == _detailTable ? _detailRows : _rows[table.tag];
    const auto &row = rows[index];
    const std::string *value = [column.identifier isEqualToString:@"Name"] ? &row.name :
        [column.identifier isEqualToString:@"Type"] ? &row.type :
        [column.identifier isEqualToString:@"Value"] ? &row.value : &row.source;
    NSTextField *label = [table makeViewWithIdentifier:column.identifier owner:self];
    if (!label) { label = [NSTextField labelWithString:@""]; label.identifier = column.identifier; }
    label.stringValue = @(value->c_str()); label.toolTip = label.stringValue;
    label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    return label;
}
- (void)openProperty:(NSTableView *)table {
    NSInteger row = table.clickedRow;
    if (row < 0 || row >= (NSInteger)_rows[0].size()) return;
    _detailPath = [self inspectedPath]; _detailProperty = _rows[0][row].name; _detailOffset = 0;
    if (!_detailWindow) {
        _detailWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 820, 480)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
            backing:NSBackingStoreBuffered defer:NO];
        _detailWindow.releasedWhenClosed = NO;
        _detailTable = [self makeTable];
        NSScrollView *scroll = [NSScrollView new];
        scroll.documentView = _detailTable; scroll.hasVerticalScroller = YES; scroll.hasHorizontalScroller = YES;
        _previousPage = [NSButton buttonWithTitle:@"Previous" target:self action:@selector(previousPage:)];
        _nextPage = [NSButton buttonWithTitle:@"Next" target:self action:@selector(nextPage:)];
        _pageLabel = [NSTextField labelWithString:@""];
        NSStackView *bar = [NSStackView stackViewWithViews:@[_previousPage, _nextPage, _pageLabel]];
        NSStackView *content = [NSStackView stackViewWithViews:@[scroll, bar]];
        content.orientation = NSUserInterfaceLayoutOrientationVertical; content.alignment = NSLayoutAttributeWidth;
        content.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
        [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
        [bar setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationVertical];
        _detailWindow.contentView = content; [_detailWindow center];
    }
    _detailWindow.title = [NSString stringWithFormat:@"%@.%@", @(_detailPath.GetText()), @(_detailProperty.c_str())];
    [self refreshDetails]; [_detailWindow makeKeyAndOrderFront:nil];
}
- (void)refreshDetails {
    _detailRows = _browser->PropertyDetails(_detailPath, _detailProperty, _detailOffset);
    [_detailTable reloadData];
    _previousPage.enabled = _detailOffset > 0;
    _nextPage.enabled = _detailOffset + 128 < _browser->PropertyDetailCount(_detailPath, _detailProperty);
    _pageLabel.stringValue = [NSString stringWithFormat:@"Page %zu", _detailOffset / 128 + 1];
}
- (void)previousPage:(id)sender { if (_detailOffset >= 128) _detailOffset -= 128; [self refreshDetails]; }
- (void)nextPage:(id)sender { _detailOffset += 128; [self refreshDetails]; }

- (BOOL)checkBrowsing {
    if (_outline.tableColumns.count != 4 || _tables.count != 5) return NO;
    auto roots = _browser->Children();
    if (roots.empty()) return YES;
    SdfPath path = roots.front();
    const auto children = _browser->Children(path);
    if (!children.empty()) path = children.front();
    vela::ActionRegistry::Get().Run(*_document, "select", {{"paths", JsValue(JsArray{JsValue(path.GetString())})}});
    if (_outline.selectedRow < 0) return NO;
    NSString *item = [_outline itemAtRow:_outline.selectedRow];
    if (!item || SdfPath(item.UTF8String) != path) return NO;
    if (_rows[0].size() != _browser->Properties(path).size() || _rows[3].empty()) return NO;
    // Search and clearing search preserve the shared selection, including
    // when the selected row is hidden by the filter.
    _search.stringValue = @"__vela_no_matching_prim__";
    [self controlTextDidChange:nil];
    if (_outline.numberOfRows != 0 || _document->GetSelection().empty()) return NO;
    _search.stringValue = @""; [self controlTextDidChange:nil];
    if (_outline.selectedRow < 0) return NO;
    [_outline deselectAll:nil];
    if (!_document->GetSelection().empty()) return NO;
    // Selecting the outline also selects the document (the picking direction
    // was checked above), and populates the inspector.
    [_outline selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    if (_document->GetSelection().size() != 1 || [self inspectedPath] != roots.front()) return NO;
    [_outline deselectAll:nil];
    return _document->GetSelection().empty();
}
@end
