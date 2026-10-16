#import "color_coding.h"

#import <objc/runtime.h>

NSColor *VelaColor(vela::Rgb rgb) {
    NSColor *dark = [NSColor colorWithSRGBRed:rgb.r / 255.0 green:rgb.g / 255.0
                                         blue:rgb.b / 255.0 alpha:1];
    // usdview's colors are for a dark background: on a light one, the
    // normal grey is the label color and the others darken.
    const bool normal = rgb == vela::ColorOf(vela::PrimStyle::Normal);
    NSColor *light = normal ? nil : [dark blendedColorWithFraction:0.45 ofColor:NSColor.blackColor];
    return [NSColor colorWithName:nil dynamicProvider:^NSColor *(NSAppearance *appearance) {
        NSAppearanceName match = [appearance bestMatchFromAppearancesWithNames:
            @[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
        if (![match isEqualToString:NSAppearanceNameAqua]) return dark;
        return light ?: NSColor.labelColor;
    }];
}

NSColor *VelaDimmed(NSColor *color) {
    return [color colorWithAlphaComponent:0.45];
}

NSFont *VelaPrimFont(vela::PrimFont font) {
    const CGFloat size = NSFont.systemFontSize;
    switch (font) {
    case vela::PrimFont::Defined: return [NSFont boldSystemFontOfSize:size];
    case vela::PrimFont::Over:
        return [NSFontManager.sharedFontManager convertFont:[NSFont systemFontOfSize:size]
                                                toHaveTrait:NSItalicFontMask];
    case vela::PrimFont::Abstract: break;
    }
    return [NSFont systemFontOfSize:size];
}

namespace {

NSFont *Italic(NSFont *font) {
    return [NSFontManager.sharedFontManager convertFont:font toHaveTrait:NSItalicFontMask];
}

NSView *Swatch(const vela::LegendEntry &entry) {
    using S = vela::LegendEntry::Swatch;
    NSColor *color = VelaColor(entry.color);
    if (entry.swatch == S::Color || entry.swatch == S::Dimmed) {
        NSView *box = [NSView new];
        box.wantsLayer = YES;
        box.layer.cornerRadius = 3;
        box.layer.backgroundColor = (entry.swatch == S::Dimmed ? VelaDimmed(color) : color).CGColor;
        [box.widthAnchor constraintEqualToConstant:28].active = YES;
        [box.heightAnchor constraintEqualToConstant:14].active = YES;
        return box;
    }
    NSTextField *sample = [NSTextField labelWithString:@"Abc"];
    NSFont *base = [NSFont systemFontOfSize:NSFont.systemFontSize];
    sample.font = entry.swatch == S::Bold ? [NSFont boldSystemFontOfSize:NSFont.systemFontSize]
                : entry.swatch == S::Italic ? Italic(base) : base;
    sample.textColor = color;
    [sample.widthAnchor constraintEqualToConstant:28].active = YES;
    return sample;
}

} // namespace

NSView *VelaLegendView(BOOL prims) {
    NSMutableArray<NSArray<NSView *> *> *rows = [NSMutableArray array];
    for (const vela::LegendEntry &entry : prims ? vela::PrimLegend() : vela::PropertyLegend()) {
        NSTextField *label = [NSTextField labelWithString:@(entry.label.c_str())];
        NSTextField *description = [NSTextField labelWithString:@(entry.description.c_str())];
        description.textColor = NSColor.secondaryLabelColor;
        [rows addObject:@[Swatch(entry), label, description]];
    }
    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 6;
    grid.columnSpacing = 10;
    for (NSInteger i = 0; i < grid.numberOfRows; ++i) [grid rowAtIndex:i].yPlacement = NSGridCellPlacementCenter;
    NSTextField *title = [NSTextField labelWithString:prims ? @"Prims" : @"Properties"];
    title.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    NSStackView *stack = [NSStackView stackViewWithViews:@[title, grid]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 8;
    stack.edgeInsets = NSEdgeInsetsMake(12, 14, 12, 14);
    return stack;
}

@interface VelaLegendTarget : NSObject
@property (nonatomic) BOOL prims;
@end
@implementation VelaLegendTarget
- (void)show:(NSButton *)sender {
    NSViewController *controller = [NSViewController new];
    controller.view = VelaLegendView(self.prims);
    const NSSize fitting = controller.view.fittingSize;
    controller.preferredContentSize = NSMakeSize(fitting.width + 12, fitting.height);
    NSPopover *popover = [NSPopover new];
    popover.behavior = NSPopoverBehaviorTransient;
    popover.contentViewController = controller;
    [popover showRelativeToRect:sender.bounds ofView:sender preferredEdge:NSRectEdgeMaxY];
}
@end

NSButton *VelaLegendButton(BOOL prims) {
    VelaLegendTarget *target = [VelaLegendTarget new];
    target.prims = prims;
    NSButton *button = [NSButton buttonWithImage:
        [NSImage imageWithSystemSymbolName:@"paintpalette" accessibilityDescription:@"Color legend"]
        target:target action:@selector(show:)];
    button.bordered = NO;
    button.toolTip = prims ? @"What the prim colors and fonts mean"
                           : @"What the property colors mean";
    // The button holds its target (buttons don't retain targets).
    static char key;
    objc_setAssociatedObject(button, &key, target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return button;
}

@implementation VelaLegendWindowController

+ (instancetype)shared {
    static VelaLegendWindowController *controller = [VelaLegendWindowController new];
    return controller;
}

- (instancetype)init {
    NSPanel *panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 400, 400)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow
        backing:NSBackingStoreBuffered defer:YES];
    panel.title = @"Color Legend";
    panel.hidesOnDeactivate = YES;
    if (!(self = [super initWithWindow:panel])) return nil;
    NSStackView *stack = [NSStackView stackViewWithViews:@[VelaLegendView(YES), VelaLegendView(NO)]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 0;
    panel.contentView = stack;
    // The grids' widest descriptions would otherwise touch the edge.
    const NSSize fitting = stack.fittingSize;
    [panel setContentSize:NSMakeSize(fitting.width + 12, fitting.height)];
    [panel center];
    return self;
}

@end
