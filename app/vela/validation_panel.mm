#import "validation_panel.h"
#import "menus.h"

#include "app/vela/core/actions.h"

#include "pxr/base/tf/stringUtils.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

NSImage *SeverityImage(vela::ValidationIssue::Severity severity) {
    using S = vela::ValidationIssue::Severity;
    NSString *name = severity == S::Error ? @"xmark.octagon.fill"
                   : severity == S::Warning ? @"exclamationmark.triangle.fill"
                                            : @"info.circle.fill";
    NSColor *color = severity == S::Error ? NSColor.systemRedColor
                   : severity == S::Warning ? NSColor.systemYellowColor
                                            : NSColor.systemBlueColor;
    NSImage *image = [NSImage imageWithSystemSymbolName:name
                               accessibilityDescription:@(vela::ToString(severity).c_str())];
    return [image imageWithSymbolConfiguration:
        [NSImageSymbolConfiguration configurationWithPaletteColors:@[color]]];
}

std::string SiteText(const vela::ValidationIssue &issue) {
    std::vector<std::string> sites;
    for (const auto &site : issue.sites) {
        std::string text = site.path.IsEmpty() ? std::string()
                         : site.path == SdfPath::AbsoluteRootPath() ? "Stage"
                                                                    : site.path.GetString();
        if (!site.layer.empty()) {
            text = TfGetBaseName(site.layer) + (text.empty() ? "" : " " + text);
        }
        if (!text.empty()) sites.push_back(text);
    }
    return TfStringJoin(sites, ", ");
}

} // namespace

@interface VelaValidationPanelController () <NSTableViewDataSource, NSTableViewDelegate>
@end

@implementation VelaValidationPanelController {
    std::shared_ptr<vela::Document> _document;
    int _observer;
    NSTableView *_table;
    NSTextField *_summary;
    NSTextField *_detail;  // the selected issue in full
}

- (instancetype)initWithDocument:(std::shared_ptr<vela::Document>)document
                           title:(NSString *)title {
    NSPanel *panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 760, 320)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                  NSWindowStyleMaskResizable | NSWindowStyleMaskUtilityWindow
          backing:NSBackingStoreBuffered defer:YES];
    panel.title = [NSString stringWithFormat:@"Validation — %@", title];
    panel.hidesOnDeactivate = YES;
    panel.minSize = NSMakeSize(420, 160);
    if (!(self = [super initWithWindow:panel])) return nil;
    _document = std::move(document);

    NSButton *validate = [NSButton buttonWithTitle:@"Validate" target:self
                                            action:@selector(validate:)];
    _summary = [NSTextField labelWithString:@""];
    _summary.textColor = NSColor.secondaryLabelColor;

    _table = [[NSTableView alloc] init];
    for (NSArray *column in @[@[@"severity", @"", @24], @[@"message", @"Message", @380],
                              @[@"validator", @"Validator", @180], @[@"site", @"Where", @160]]) {
        NSTableColumn *c = [[NSTableColumn alloc] initWithIdentifier:column[0]];
        c.title = column[1];
        c.width = [column[2] doubleValue];
        if ([column[0] isEqual:@"severity"]) c.minWidth = c.maxWidth = 24;
        [_table addTableColumn:c];
    }
    _table.usesAlternatingRowBackgroundColors = YES;
    _table.rowHeight = 22;
    _table.dataSource = self;
    _table.delegate = self;
    _table.target = self;
    _table.doubleAction = @selector(selectIssue:);
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.documentView = _table;
    scroll.hasVerticalScroller = YES;

    _detail = [NSTextField wrappingLabelWithString:@""];
    _detail.selectable = YES;
    _detail.textColor = NSColor.secondaryLabelColor;

    NSStackView *bar = [NSStackView stackViewWithViews:@[validate, _summary]];
    NSStackView *stack = [NSStackView stackViewWithViews:@[bar, scroll, _detail]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.edgeInsets = NSEdgeInsetsMake(10, 10, 10, 10);
    [scroll.widthAnchor constraintEqualToAnchor:stack.widthAnchor constant:-20].active = YES;
    [_detail.widthAnchor constraintEqualToAnchor:stack.widthAnchor constant:-20].active = YES;
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow
                       forOrientation:NSLayoutConstraintOrientationVertical];
    panel.contentView = stack;

    __weak VelaValidationPanelController *weakSelf = self;
    _observer = _document->AddObserver([weakSelf](unsigned changes) {
        if (changes & vela::Document::Validation) [weakSelf reload];
    });
    [self reload];
    return self;
}

- (void)dealloc {
    _document->RemoveObserver(_observer);
}

- (void)showWindow:(id)sender {
    if (!self.window.isVisible) [self.window center];
    [super showWindow:sender];
    if (!_document->HasValidated()) [self validate:nil];
}

// Through the action, as scripts and the palette run it.
- (void)validate:(id)sender {
    _summary.stringValue = @"Validating…";
    [_summary displayIfNeeded];
    const vela::ActionResult result = vela::ActionRegistry::Get().Run(*_document, "validate");
    if (!result.ok) _summary.stringValue = @(result.error.c_str());
}

- (void)reload {
    const auto &issues = _document->GetValidationIssues();
    int counts[3] = {0, 0, 0};
    for (const auto &issue : issues) ++counts[static_cast<int>(issue.severity)];
    auto plural = [](int n, NSString *word) {
        return [NSString stringWithFormat:@"%d %@%@", n, word, n == 1 ? @"" : @"s"];
    };
    if (!_document->HasValidated()) {
        _summary.stringValue = @"Not validated yet";
    } else if (issues.empty()) {
        _summary.stringValue = @"No issues";
    } else {
        _summary.stringValue = [NSString stringWithFormat:@"%@, %@, %d info",
            plural(counts[0], @"error"), plural(counts[1], @"warning"), counts[2]];
    }
    [_table reloadData];
    [self showDetail];
}

- (void)tableViewSelectionDidChange:(NSNotification *)notification {
    [self showDetail];
}

// The selected issue's message and sites, which the rows truncate.
- (void)showDetail {
    const auto &issues = _document->GetValidationIssues();
    const NSInteger row = _table.selectedRow;
    if (row < 0 || size_t(row) >= issues.size()) {
        _detail.stringValue = issues.empty() ? @""
            : @"Select an issue to read it in full; double-click to select its prim.";
        return;
    }
    const vela::ValidationIssue &issue = issues[size_t(row)];
    // UTF-8, as messages may not be ASCII (%s would read the system encoding).
    const std::string text = issue.message + "\n" + issue.validator + " — " + SiteText(issue);
    _detail.stringValue = @(text.c_str());
}

- (void)selectIssue:(id)sender {
    const NSInteger row = _table.clickedRow;
    const auto &issues = _document->GetValidationIssues();
    if (row < 0 || size_t(row) >= issues.size()) return;
    const SdfPath prim = issues[size_t(row)].GetPrimPath();
    if (prim.IsEmpty()) return;  // the stage or a layer: nothing to select
    vela::ActionRegistry::Get().Run(*_document, "select_path",
                                    JsObject{{"path", JsValue(prim.GetString())}});
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView {
    return NSInteger(_document->GetValidationIssues().size());
}

- (NSView *)tableView:(NSTableView *)tableView viewForTableColumn:(NSTableColumn *)column
                  row:(NSInteger)row {
    const vela::ValidationIssue &issue = _document->GetValidationIssues()[size_t(row)];
    NSString *identifier = column.identifier;
    if ([identifier isEqual:@"severity"]) {
        NSImageView *image = [NSImageView imageViewWithImage:SeverityImage(issue.severity)];
        return image;
    }
    NSTextField *text = [tableView makeViewWithIdentifier:identifier owner:self];
    if (!text) {
        text = [NSTextField labelWithString:@""];
        text.identifier = identifier;
        text.lineBreakMode = NSLineBreakByTruncatingTail;
    }
    std::string value = [identifier isEqual:@"message"] ? issue.message
                      : [identifier isEqual:@"validator"] ? issue.validator
                                                          : SiteText(issue);
    text.stringValue = @(value.c_str());
    text.toolTip = text.stringValue;
    return text;
}

@end
