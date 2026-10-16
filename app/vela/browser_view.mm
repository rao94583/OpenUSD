#import "browser_view.h"
#import "window_layout.h"
#import "spline_view.h"
#import "color_coding.h"
#include "app/vela/core/browser.h"
#include "app/vela/core/actions.h"
#include "app/vela/core/log.h"
#include "pxr/usd/usd/primRange.h"

PXR_NAMESPACE_USING_DIRECTIVE

@interface VelaBrowserController () <NSOutlineViewDataSource, NSOutlineViewDelegate,
    NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate, NSMenuDelegate>
@end

@implementation VelaBrowserController {
    std::shared_ptr<vela::Document> _document;
    std::unique_ptr<vela::Browser> _browser;
    int _observer;
    NSView *_outlineView;
    NSView *_inspectorView;
    NSOutlineView *_outline;
    NSSearchField *_search;
    NSSearchField *_propertySearch;
    NSPopUpButton *_primFilter;
    NSButton *_showPrototypes;
    NSButton *_prototypeButton;
    NSMutableDictionary<NSString *, NSArray<NSString *> *> *_children;
    NSMutableDictionary<NSString *, NSString *> *_items;
    NSMutableArray<NSTableView *> *_tables;
    std::vector<vela::InspectorRows> _rows;
    NSTextField *_selectionLabel;
    BOOL _syncing;
    BOOL _reloadScheduled;
    BOOL _pendingResync;
    BOOL _timeRefreshScheduled;
    NSWindow *_detailWindow;
    NSTableView *_detailTable;
    VelaSplineView *_splineView;
    NSTableView *_detailMetadataTable;
    NSTableView *_detailOpinionsTable;
    vela::InspectorRows _detailMetadataRows, _detailOpinionsRows;
    NSWindow *_layerWindow;
    NSTableView *_layerTable;
    vela::InspectorRows _layerRows;
    std::string _layerIdentifier;
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
    _primFilter = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_primFilter addItemsWithTitles:@[@"All prims", @"Active prims", @"Loaded prims", @"Models", @"Visible prims"]];
    _primFilter.target = self; _primFilter.action = @selector(filterOptionsChanged:);
    _showPrototypes = [NSButton checkboxWithTitle:@"Prototypes" target:self action:@selector(filterOptionsChanged:)];
    NSView *spacer = [NSView new];
    [spacer setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1
                       forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *filterBar = [NSStackView stackViewWithViews:
        @[_primFilter, _showPrototypes, spacer, VelaLegendButton(YES)]];
    filterBar.spacing = 6;
    _outline = [NSOutlineView new];
    _outline.dataSource = self; _outline.delegate = self;
    _outline.allowsMultipleSelection = YES;
    _outline.columnAutoresizingStyle = NSTableViewNoColumnAutoresizing;
    // Keep the Prim column's width as rows expand, so the other columns
    // stay in view.
    _outline.autoresizesOutlineColumn = NO;
    _outline.intercellSpacing = NSMakeSize(6, _outline.intercellSpacing.height);
    // Widths that fit the outline pane's default width.
    NSDictionary<NSString *, NSNumber *> *outlineWidths =
        @{@"Prim": @128, @"Type": @60, @"Visibility": @30, @"Draw Mode": @66};
    for (NSString *name in @[@"Prim", @"Type", @"Visibility", @"Draw Mode"]) {
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:name];
        // usdview's compact visibility column: V (visible) or I (invisible).
        column.title = [name isEqualToString:@"Visibility"] ? @"Vis" : name;
        column.headerToolTip = name;
        column.width = outlineWidths[name].doubleValue;
        [_outline addTableColumn:column];
    }
    _outline.outlineTableColumn = _outline.tableColumns.firstObject;
    NSScrollView *tree = [NSScrollView new];
    tree.hasVerticalScroller = YES; tree.hasHorizontalScroller = YES;
    tree.documentView = _outline;
    NSView *left = [NSView new];
    for (NSView *view in @[_search, filterBar, tree]) {
        view.translatesAutoresizingMaskIntoConstraints = NO; [left addSubview:view];
    }
    [NSLayoutConstraint activateConstraints:@[
        [_search.leadingAnchor constraintEqualToAnchor:left.leadingAnchor constant:6],
        [_search.trailingAnchor constraintEqualToAnchor:left.trailingAnchor constant:-6],
        [_search.topAnchor constraintEqualToAnchor:left.topAnchor constant:8],
        [tree.leadingAnchor constraintEqualToAnchor:left.leadingAnchor constant:6],
        [tree.trailingAnchor constraintEqualToAnchor:left.trailingAnchor constant:-6],
        [filterBar.leadingAnchor constraintEqualToAnchor:left.leadingAnchor constant:6],
        [filterBar.trailingAnchor constraintEqualToAnchor:left.trailingAnchor constant:-6],
        [filterBar.topAnchor constraintEqualToAnchor:_search.bottomAnchor constant:6],
        [tree.topAnchor constraintEqualToAnchor:filterBar.bottomAnchor constant:6],
        [tree.bottomAnchor constraintEqualToAnchor:left.bottomAnchor constant:-6]]];
    _outlineView = left;

    _selectionLabel = [NSTextField labelWithString:@"No selection"];
    _selectionLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    _prototypeButton = [NSButton buttonWithTitle:@"Prototype" target:self action:@selector(goToPrototype:)];
    NSView *selectionSpacer = [NSView new];
    [selectionSpacer setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1
                                forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *selectionBar = [NSStackView stackViewWithViews:
        @[_selectionLabel, _prototypeButton, selectionSpacer, VelaLegendButton(NO)]];
    [_selectionLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _propertySearch = [NSSearchField new];
    _propertySearch.placeholderString = @"Filter properties by name or type";
    _propertySearch.delegate = self;
    NSTabView *tabs = [NSTabView new];
    NSArray<NSString *> *names = @[@"Properties", @"Metadata", @"Composition", @"Layers", @"Variants"];
    for (NSInteger i = 0; i < names.count; ++i) {
        NSTableView *table = [self makeTable];
        table.tag = i;
        if (i == 0) {
            table.target = self; table.doubleAction = @selector(openProperty:);
            // Relationship targets and connections to go to.
            table.menu = [NSMenu new];
            table.menu.delegate = self;
        }
        if (i == 2 || i == 3) { table.target = self; table.doubleAction = @selector(openLayer:); }
        [_tables addObject:table];
        NSTabViewItem *tab = [[NSTabViewItem alloc] initWithIdentifier:names[i]];
        tab.label = names[i];
        NSScrollView *scroll = [NSScrollView new];
        scroll.hasVerticalScroller = YES; scroll.hasHorizontalScroller = YES;
        scroll.documentView = table;
        tab.view = scroll; [tabs addTabViewItem:tab];
    }
    NSView *right = [NSView new];
    for (NSView *view in @[selectionBar, _propertySearch, tabs]) {
        view.translatesAutoresizingMaskIntoConstraints = NO; [right addSubview:view];
    }
    [NSLayoutConstraint activateConstraints:@[
        [selectionBar.leadingAnchor constraintEqualToAnchor:right.leadingAnchor constant:6],
        [selectionBar.trailingAnchor constraintEqualToAnchor:right.trailingAnchor constant:-6],
        [selectionBar.topAnchor constraintEqualToAnchor:right.topAnchor constant:8],
        [tabs.leadingAnchor constraintEqualToAnchor:right.leadingAnchor constant:6],
        [tabs.trailingAnchor constraintEqualToAnchor:right.trailingAnchor constant:-6],
        [_propertySearch.leadingAnchor constraintEqualToAnchor:right.leadingAnchor constant:6],
        [_propertySearch.trailingAnchor constraintEqualToAnchor:right.trailingAnchor constant:-6],
        [_propertySearch.topAnchor constraintEqualToAnchor:selectionBar.bottomAnchor constant:6],
        [tabs.topAnchor constraintEqualToAnchor:_propertySearch.bottomAnchor constant:6],
        [tabs.bottomAnchor constraintEqualToAnchor:right.bottomAnchor constant:-6]]];
    _inspectorView = right;

    __weak VelaBrowserController *weakSelf = self;
    _observer = _document->AddObserver([weakSelf](unsigned changes) {
        [weakSelf documentChanged:changes];
    });
    [_outline reloadData]; [self refreshInspector]; [self syncSelection];
    return self;
}
- (void)dealloc { _document->RemoveObserver(_observer); [_detailWindow close]; [_layerWindow close]; }
- (NSView *)outlineView { return _outlineView; }
- (NSView *)inspectorView { return _inspectorView; }
- (NSTableView *)makeTable {
    NSTableView *table = [NSTableView new];
    table.dataSource = self; table.delegate = self;
    table.columnAutoresizingStyle = NSTableViewNoColumnAutoresizing;
    // Name and Value first, so values show at the inspector's default width.
    NSDictionary<NSString *, NSNumber *> *widths =
        @{@"Name": @150, @"Value": @190, @"Type": @110, @"Source": @260};
    for (NSString *name in @[@"Name", @"Value", @"Type", @"Source"]) {
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:name];
        column.title = name; column.width = widths[name].doubleValue;
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
    else if ([column.identifier isEqualToString:@"Visibility"])
        value = summary.visibility.empty() ? "" : summary.visibility == "invisible" ? "I" : "V";
    else value = summary.drawMode;
    NSTextField *label = [view makeViewWithIdentifier:column.identifier owner:self];
    if (!label) { label = [NSTextField labelWithString:@""]; label.identifier = column.identifier; }
    label.stringValue = @(value.c_str()); label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    // usdview's color coding: what the prim is, how it is declared, and
    // whether it is active.
    NSColor *color = VelaColor(vela::ColorOf(summary.style));
    label.textColor = summary.active ? color : VelaDimmed(color);
    label.font = [column.identifier isEqualToString:@"Prim"] ? VelaPrimFont(summary.font)
                                                            : [NSFont systemFontOfSize:NSFont.systemFontSize];
    label.toolTip = [NSString stringWithFormat:@"%@%@%@%@%@", item,
        summary.visibility.empty() ? @"" : [@" (visibility: " stringByAppendingFormat:@"%s)", summary.visibility.c_str()],
        summary.inPrototype ? @" (USD prototype, read-only)" : @"",
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
    if (notification.object == _propertySearch) [self refreshInspector];
    else [self applyFilter];
}
- (void)filterOptionsChanged:(id)sender {
    _browser->SetPrimFilter(static_cast<vela::Browser::PrimFilter>(_primFilter.indexOfSelectedItem));
    _browser->SetShowPrototypes(_showPrototypes.state == NSControlStateValueOn);
    [self applyFilter];
}
- (void)goToPrototype:(id)sender {
    const auto path = _browser->PrototypePath([self inspectedPath]);
    if (path.IsEmpty()) return;
    _showPrototypes.state = NSControlStateValueOn;
    [_primFilter selectItemAtIndex:0]; _search.stringValue = @"";
    [self filterOptionsChanged:nil];
    vela::ActionRegistry::Get().Run(*_document, "select", {{"paths", JsValue(JsArray{JsValue(path.GetString())})}});
}
- (void)applyFilter {
    _browser->SetFilter(_search.stringValue.UTF8String);
    _syncing = YES; [_children removeAllObjects]; [_outline reloadData]; _syncing = NO;
    [self syncSelection];
}
- (void)documentChanged:(unsigned)changes {
    using D = vela::Document;
    if (changes & D::StageContent) {
        // Coalesce synchronous USD notices. Reloading a tree inside a selection
        // callback would invalidate AppKit's current row/items. Only a resync
        // changes the tree; value edits redraw the rows on screen.
        _pendingResync |= (changes & D::Resync) != 0;
        if (!_reloadScheduled) {
            _reloadScheduled = YES;
            __weak VelaBrowserController *weakSelf = self;
            dispatch_async(dispatch_get_main_queue(), ^{
                VelaBrowserController *s = weakSelf;
                if (!s) return;
                s->_reloadScheduled = NO;
                if (s->_pendingResync || [s visibleFilter]) {
                    s->_pendingResync = NO;
                    [s reloadOutline];
                } else {
                    [s reloadVisibleRows];
                }
                [s refreshInspector];
            });
        }
        return;
    }
    if (changes & D::Selection) [self syncSelection];
    if (changes & (D::Selection | D::EditTarget | D::UndoState)) [self refreshInspector];
    if (changes & D::Time) [self scheduleTimeRefresh];
}
- (BOOL)visibleFilter { return _primFilter.indexOfSelectedItem == 4; }
- (void)reloadOutline {
    _syncing = YES; [_children removeAllObjects]; [_outline reloadData]; _syncing = NO;
    [self syncSelection];
}
// Rows on screen show time-dependent visibility and draw modes.
- (void)reloadVisibleRows {
    const NSRange rows = [_outline rowsInRect:_outline.visibleRect];
    if (!rows.length) return;
    _syncing = YES;
    [_outline reloadDataForRowIndexes:[NSIndexSet indexSetWithIndexesInRange:rows]
                        columnIndexes:[NSIndexSet indexSetWithIndexesInRange:
                                          NSMakeRange(0, _outline.numberOfColumns)]];
    _syncing = NO;
}
// Time changes refresh only what depends on time: outline rows, the Visible
// filter and property values. During playback this runs at most four times
// a second; the last refresh shows the frame playback stopped on.
- (void)scheduleTimeRefresh {
    if (_timeRefreshScheduled) return;
    _timeRefreshScheduled = YES;
    const double delay = _document->IsPlaying() ? 0.25 : 0.0;
    __weak VelaBrowserController *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(delay * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        VelaBrowserController *s = weakSelf;
        if (!s) return;
        s->_timeRefreshScheduled = NO;
        if ([s visibleFilter]) [s applyFilter];
        else [s reloadVisibleRows];
        [s refreshProperties];
    });
}
- (SdfPath)inspectedPath {
    const auto &selection = _document->GetSelection();
    return selection.empty() ? SdfPath() : selection.back();
}
- (void)refreshInspector {
    const auto path = [self inspectedPath];
    _prototypeButton.enabled = !_browser->PrototypePath(path).IsEmpty();
    _selectionLabel.stringValue = path.IsEmpty() ? @"No selection" : @(path.GetText());
    _selectionLabel.toolTip = _document->GetSelection().size() > 1 ? @"Inspecting the last selected prim" : nil;
    _rows[0] = _browser->Properties(path, _propertySearch.stringValue.UTF8String); _rows[1] = _browser->Metadata(path);
    _rows[2] = _browser->Composition(path); _rows[3] = _browser->Layers();
    _rows[4] = _browser->Variants(path);
    for (NSTableView *table in _tables) [table reloadData];
    if (_detailWindow) [self refreshDetails];
    if (_layerWindow) { _layerRows = _browser->LayerDetails(_layerIdentifier); [_layerTable reloadData]; }
}
// The time-dependent part of the inspector: property values and an open
// property window.
- (void)refreshProperties {
    _rows[0] = _browser->Properties([self inspectedPath], _propertySearch.stringValue.UTF8String);
    [_tables[0] reloadData];
    if (_detailWindow) [self refreshDetails];
}
- (const vela::InspectorRows &)rowsForTable:(NSTableView *)table {
    if (table == _detailTable) return _detailRows;
    if (table == _detailMetadataTable) return _detailMetadataRows;
    if (table == _detailOpinionsTable) return _detailOpinionsRows;
    if (table == _layerTable) return _layerRows;
    return _rows[table.tag];
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)table {
    return [self rowsForTable:table].size();
}
- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)index {
    const auto &rows = [self rowsForTable:table];
    const auto &row = rows[index];
    const bool valueColumn = [column.identifier isEqualToString:@"Value"];
    // Variant selections are chosen from a pop-up.
    if (valueColumn && table == _tables[4]) {
        return [self variantPopUpForSet:row.name selection:row.value];
    }
    const std::string *value = [column.identifier isEqualToString:@"Name"] ? &row.name :
        [column.identifier isEqualToString:@"Type"] ? &row.type :
        valueColumn ? &row.value : &row.source;
    // Editable attribute values: an editable field holding the full value.
    std::string editText;
    const bool editable = valueColumn && table == _tables[0] &&
        _browser->EditableText([self inspectedPath], row.name, &editText);
    NSString *identifier = editable ? @"EditableValue" : column.identifier;
    NSTextField *label = [table makeViewWithIdentifier:identifier owner:self];
    if (!label) {
        label = [NSTextField labelWithString:@""];
        label.identifier = identifier;
        if (editable) {
            label.editable = YES;
            label.target = self;
            label.action = @selector(valueEdited:);
        }
    }
    label.stringValue = @((editable ? editText : *value).c_str());
    [self styleLabel:label table:table column:column row:row];
    // An unauthored value reads as such, and typing starts from empty.
    label.placeholderString = editable && editText.empty() ? @"<no value>" : nil;
    label.toolTip = editable ? @"Edit the value; Return applies it" : label.stringValue;
    label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    return label;
}
// Properties are colored by where their values come from, as in usdview;
// names are bold, and both are italic when the value is interpolated.
// Other tables use the plain label style.
- (void)styleLabel:(NSTextField *)label table:(NSTableView *)table
            column:(NSTableColumn *)column row:(const vela::InspectorRow &)row {
    const CGFloat size = NSFont.systemFontSize;
    NSFont *regular = [NSFont systemFontOfSize:size];
    if (table != _tables[0]) {
        label.textColor = NSColor.labelColor;
        label.font = regular;
        return;
    }
    const bool name = [column.identifier isEqualToString:@"Name"];
    const bool value = [column.identifier isEqualToString:@"Value"];
    if (!name && !value) {
        label.textColor = NSColor.secondaryLabelColor;
        label.font = regular;
        return;
    }
    label.textColor = VelaColor(vela::ColorOf(row.valueSource));
    NSFont *font = name ? [NSFont boldSystemFontOfSize:size] : regular;
    if (row.interpolated) {
        font = [NSFontManager.sharedFontManager convertFont:regular toHaveTrait:NSItalicFontMask];
    }
    label.font = font;
}

#pragma mark Going to targets

// The properties table's menu: each target of the clicked relationship,
// or source of the clicked attribute's connections.
- (void)menuNeedsUpdate:(NSMenu *)menu {
    [menu removeAllItems];
    NSTableView *table = _tables[0];
    const NSInteger index = table.clickedRow;
    if (index < 0 || index >= (NSInteger)_rows[0].size()) return;
    const vela::InspectorRow &row = _rows[0][index];
    const bool relationship = row.valueSource == vela::ValueSource::Relationship;
    if (row.targets.empty()) {
        NSMenuItem *none = [menu addItemWithTitle:relationship ? @"No Targets" : @"No Connections"
                                           action:nil keyEquivalent:@""];
        none.enabled = NO;
        if (!relationship) [menu removeAllItems];
    }
    const size_t shown = std::min<size_t>(row.targets.size(), 25);
    for (size_t i = 0; i < shown; ++i) {
        NSString *target = @(row.targets[i].c_str());
        NSMenuItem *item = [menu addItemWithTitle:
            [NSString stringWithFormat:relationship ? @"Go to Target %@" : @"Go to Source %@", target]
            action:@selector(goToTarget:) keyEquivalent:@""];
        item.target = self;
        item.representedObject = target;
    }
    if (row.targets.size() > shown) {
        NSMenuItem *more = [menu addItemWithTitle:
            [NSString stringWithFormat:@"%zu more (double-click to list all)", row.targets.size() - shown]
            action:nil keyEquivalent:@""];
        more.enabled = NO;
    }
    if (menu.numberOfItems) [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *copy = [menu addItemWithTitle:@"Copy Property Path" action:@selector(copyPropertyPath:)
                                keyEquivalent:@""];
    copy.target = self;
    copy.representedObject = @([self inspectedPath].AppendProperty(TfToken(row.name)).GetText());
}

// Selects the target's prim; a property target is then shown in the
// inspector.
- (void)goToTarget:(NSMenuItem *)item {
    [self goToPath:SdfPath([item.representedObject UTF8String])];
}

- (void)goToPath:(const SdfPath &)target {
    const SdfPath prim = target.GetPrimPath();
    if (!_document->GetStage()->GetPrimAtPath(prim)) {
        [self showResult:vela::ActionResult::Error("Nothing at " + target.GetString())];
        return;
    }
    // Prototype prims are only in the outline when it shows prototypes.
    if (_document->GetStage()->GetPrimAtPath(prim).IsInPrototype()) {
        if (_showPrototypes.state != NSControlStateValueOn) {
            _showPrototypes.state = NSControlStateValueOn;
            [self filterOptionsChanged:nil];
        }
    }
    [self showResult:vela::ActionRegistry::Get().Run(*_document, "select",
        {{"paths", JsValue(JsArray{JsValue(prim.GetString())})}})];
    if (!target.IsPropertyPath()) return;
    // Show the target property: clear a filter that hides it, then select it.
    const std::string name = target.GetName();
    [self refreshInspector];
    auto find = [&] {
        for (size_t i = 0; i < _rows[0].size(); ++i) if (_rows[0][i].name == name) return NSInteger(i);
        return NSInteger(-1);
    };
    if (find() < 0 && _propertySearch.stringValue.length) {
        _propertySearch.stringValue = @"";
        [self refreshInspector];
    }
    const NSInteger row = find();
    if (row >= 0) {
        [_tables[0] selectRowIndexes:[NSIndexSet indexSetWithIndex:NSUInteger(row)] byExtendingSelection:NO];
        [_tables[0] scrollRowToVisible:row];
    }
}

- (void)copyPropertyPath:(NSMenuItem *)item {
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:item.representedObject forType:NSPasteboardTypeString];
}

- (NSPopUpButton *)variantPopUpForSet:(const std::string &)set selection:(const std::string &)selection {
    NSPopUpButton *popUp = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    popUp.controlSize = NSControlSizeSmall;
    popUp.bordered = NO;
    [popUp addItemWithTitle:@"(none)"];
    for (const std::string &variant : _browser->VariantChoices([self inspectedPath], set)) {
        [popUp addItemWithTitle:@(variant.c_str())];
    }
    [popUp selectItemWithTitle:selection.empty() ? @"(none)" : @(selection.c_str())];
    popUp.identifier = @(set.c_str());
    popUp.target = self;
    popUp.action = @selector(variantChosen:);
    popUp.enabled = _document->GetEditTarget()->PermissionToEdit();
    return popUp;
}
- (void)variantChosen:(NSPopUpButton *)popUp {
    const std::string variant = popUp.indexOfSelectedItem == 0 ? "" : popUp.titleOfSelectedItem.UTF8String;
    [self showResult:vela::ActionRegistry::Get().Run(*_document, "set_variant", {
        {"path", JsValue([self inspectedPath].GetString())},
        {"set", JsValue(std::string(popUp.identifier.UTF8String))},
        {"variant", JsValue(variant)}})];
}
- (void)valueEdited:(NSTextField *)field {
    NSTableView *table = _tables[0];
    const NSInteger row = [table rowForView:field];
    if (row < 0 || row >= (NSInteger)_rows[0].size()) return;
    const std::string name = _rows[0][row].name;
    std::string current;
    // Return without a change does nothing.
    if (_browser->EditableText([self inspectedPath], name, &current) &&
        current == field.stringValue.UTF8String) return;
    [self showResult:_browser->EditAttribute([self inspectedPath], name, field.stringValue.UTF8String)];
    [self refreshProperties];
}
- (void)showResult:(const vela::ActionResult &)result {
    if (result.ok) return;
    vela::Log::Get().Add(vela::LogEntry::Level::Error, "Inspector", result.error);
    NSAlert *alert = [NSAlert new];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = @(result.error.c_str());
    NSWindow *window = _inspectorView.window;
    if (window) [alert beginSheetModalForWindow:window completionHandler:nil];
    else NSLog(@"%s", result.error.c_str());
}
- (void)openProperty:(NSTableView *)table {
    NSInteger row = table.clickedRow;
    if (row < 0 || row >= (NSInteger)_rows[0].size()) return;
    // A double-click on an editable value edits it instead.
    if (table.clickedColumn >= 0 &&
        [table.tableColumns[table.clickedColumn].identifier isEqualToString:@"Value"] &&
        _browser->EditableText([self inspectedPath], _rows[0][row].name, nullptr)) {
        [table editColumn:table.clickedColumn row:row withEvent:nil select:YES];
        return;
    }
    [self inspectProperty:_rows[0][row].name show:YES];
}
- (void)inspectProperty:(const std::string &)property show:(BOOL)show {
    _detailPath = [self inspectedPath]; _detailProperty = property; _detailOffset = 0;
    if (!_detailWindow) {
        _detailWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 820, 480)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
            backing:NSBackingStoreBuffered defer:NO];
        _detailWindow.releasedWhenClosed = NO;
        _detailTable = [self makeTable];
        // A relationship's targets: double-click one to go there.
        _detailTable.target = self;
        _detailTable.doubleAction = @selector(openDetailRow:);
        _detailMetadataTable = [self makeTable]; _detailOpinionsTable = [self makeTable];
        NSTabView *tabs = [NSTabView new];
        NSArray *names = @[@"Values", @"Metadata", @"Opinions"];
        NSArray *tables = @[_detailTable, _detailMetadataTable, _detailOpinionsTable];
        for (NSUInteger i = 0; i < names.count; ++i) {
            NSTabViewItem *tab = [[NSTabViewItem alloc] initWithIdentifier:names[i]];
            tab.label = names[i];
            NSScrollView *scroll = [NSScrollView new];
            scroll.documentView = tables[i]; scroll.hasVerticalScroller = YES; scroll.hasHorizontalScroller = YES;
            tab.view = scroll; [tabs addTabViewItem:tab];
        }
        _splineView = [VelaSplineView new];
        NSTabViewItem *curveTab = [[NSTabViewItem alloc] initWithIdentifier:@"Curve"];
        curveTab.label = @"Curve"; curveTab.view = _splineView; [tabs addTabViewItem:curveTab];
        _previousPage = [NSButton buttonWithTitle:@"Previous" target:self action:@selector(previousPage:)];
        _nextPage = [NSButton buttonWithTitle:@"Next" target:self action:@selector(nextPage:)];
        _pageLabel = [NSTextField labelWithString:@""];
        NSStackView *bar = [NSStackView stackViewWithViews:@[_previousPage, _nextPage, _pageLabel]];
        NSView *content = [NSView new];
        for (NSView *view in @[tabs, bar]) { view.translatesAutoresizingMaskIntoConstraints = NO; [content addSubview:view]; }
        [NSLayoutConstraint activateConstraints:@[
            [tabs.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:8],
            [tabs.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-8],
            [tabs.topAnchor constraintEqualToAnchor:content.topAnchor constant:8],
            [tabs.bottomAnchor constraintEqualToAnchor:bar.topAnchor constant:-6],
            [bar.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:8],
            [bar.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-8],
            [bar.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-8]]];
        _detailWindow.contentView = content; [_detailWindow center];
    }
    _detailWindow.title = [NSString stringWithFormat:@"%@.%@", @(_detailPath.GetText()), @(_detailProperty.c_str())];
    [self refreshDetails]; if (show) [_detailWindow makeKeyAndOrderFront:nil];
}
- (void)refreshDetails {
    _detailRows = _browser->PropertyDetails(_detailPath, _detailProperty, _detailOffset);
    [_splineView setPlot:_browser->SplinePreview(_detailPath, _detailProperty)];
    _detailMetadataRows = _browser->PropertyMetadata(_detailPath, _detailProperty);
    _detailOpinionsRows = _browser->PropertyOpinions(_detailPath, _detailProperty);
    [_detailTable reloadData]; [_detailMetadataTable reloadData]; [_detailOpinionsTable reloadData];
    _previousPage.enabled = _detailOffset > 0;
    _nextPage.enabled = _detailOffset + 128 < _browser->PropertyDetailCount(_detailPath, _detailProperty);
    _pageLabel.stringValue = [NSString stringWithFormat:@"Page %zu", _detailOffset / 128 + 1];
}
- (void)openLayer:(NSTableView *)table {
    const auto &rows = [self rowsForTable:table];
    if (table.clickedRow < 0 || table.clickedRow >= (NSInteger)rows.size()) return;
    _layerIdentifier = rows[table.clickedRow].value;
    _layerRows = _browser->LayerDetails(_layerIdentifier);
    if (_layerRows.empty()) return;
    if (!_layerWindow) {
        _layerWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 820, 480)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
            backing:NSBackingStoreBuffered defer:NO];
        _layerWindow.releasedWhenClosed = NO; _layerTable = [self makeTable];
        NSScrollView *scroll = [NSScrollView new];
        scroll.documentView = _layerTable; scroll.hasVerticalScroller = YES; scroll.hasHorizontalScroller = YES;
        _layerWindow.contentView = scroll; [_layerWindow center];
    }
    _layerWindow.title = @(_layerIdentifier.c_str());
    [_layerTable reloadData]; [_layerWindow makeKeyAndOrderFront:nil];
}
- (void)openDetailRow:(NSTableView *)table {
    const NSInteger row = table.clickedRow;
    if (row < 0 || row >= (NSInteger)_detailRows.size()) return;
    const std::string &text = _detailRows[row].value;
    if (!SdfPath::IsValidPathString(text) || !SdfPath(text).IsAbsolutePath()) return;
    // The detail window belongs to the property it shows; go in the
    // document window.
    [_inspectorView.window makeKeyAndOrderFront:nil];
    [self goToPath:SdfPath(text)];
}
- (void)previousPage:(id)sender { if (_detailOffset >= 128) _detailOffset -= 128; [self refreshDetails]; }
- (void)nextPage:(id)sender { _detailOffset += 128; [self refreshDetails]; }

- (BOOL)checkBrowsing {
    if (!VelaCheckDocumentLayout(_outlineView, _inspectorView)) return NO;
    if (fabs(_search.frame.size.width - (_outlineView.bounds.size.width - 12)) > 0.5) return NO;
    if (fabs(_outline.enclosingScrollView.frame.size.width - (_outlineView.bounds.size.width - 12)) > 0.5) return NO;
    if (fabs(_outline.enclosingScrollView.frame.origin.x - 6) > 0.5) return NO;
    if (_outline.tableColumns.count != 4 || _tables.count != 5) return NO;
    auto roots = _browser->Children();
    if (roots.empty()) return YES;
    const auto prototypes = _document->GetStage()->GetPrototypes();
    if (!prototypes.empty()) {
        for (const auto &prim : _document->GetStage()->Traverse()) {
            if (!prim.IsInstance()) continue;
            vela::ActionRegistry::Get().Run(*_document, "select", {{"paths", JsValue(JsArray{JsValue(prim.GetPath().GetString())})}});
            if (!_prototypeButton.enabled) return NO;
            [self goToPrototype:nil];
            if (!_browser->GetShowPrototypes() || [self inspectedPath] != prim.GetPrototype().GetPath() || _outline.selectedRow < 0) return NO;
            _showPrototypes.state = NSControlStateValueOff; [self filterOptionsChanged:nil];
            _document->SetSelection({});
            break;
        }
    }
    SdfPath path = roots.front();
    const auto children = _browser->Children(path);
    if (!children.empty()) path = children.front();
    vela::ActionRegistry::Get().Run(*_document, "select", {{"paths", JsValue(JsArray{JsValue(path.GetString())})}});
    if (_outline.selectedRow < 0) return NO;
    NSString *item = [_outline itemAtRow:_outline.selectedRow];
    if (!item || SdfPath(item.UTF8String) != path) return NO;
    if (_rows[0].size() != _browser->Properties(path).size() || _rows[3].empty()) return NO;
    if (!_rows[0].empty()) {
        const auto name = _rows[0].front().name;
        _propertySearch.stringValue = @(name.c_str()); [self refreshInspector];
        if (_rows[0].empty()) return NO;
        [self inspectProperty:name show:NO];
        if (_detailRows.empty() || _detailMetadataRows.empty()) return NO;
        _propertySearch.stringValue = @"__no_property__"; [self refreshInspector];
        if (!_rows[0].empty()) return NO;
        _propertySearch.stringValue = @""; [self refreshInspector];
    }
    // Search and clearing search preserve the shared selection, including
    // when the selected row is hidden by the filter.
    _search.stringValue = @"__vela_no_matching_prim__";
    [self applyFilter];
    if (_outline.numberOfRows != 0 || _document->GetSelection().empty()) return NO;
    _search.stringValue = @""; [self applyFilter];
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
