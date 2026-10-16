#import "hud_view.h"

@implementation VelaHudView {
    vela::Hud _hud;
}

- (BOOL)isFlipped { return YES; }
- (NSView *)hitTest:(NSPoint)point { return nil; }

- (void)setHud:(const vela::Hud &)hud {
    _hud = hud;
    self.needsDisplay = YES;
}

// One corner's lines, "Key: value", aligned to its corner.
- (void)drawLines:(const vela::HudLines &)lines right:(BOOL)right bottom:(BOOL)bottom
       attributes:(NSDictionary *)attributes {
    const CGFloat lineHeight = 15, margin = 8;
    CGFloat y = bottom ? self.bounds.size.height - margin - lineHeight * lines.size() : margin;
    for (const auto &[key, value] : lines) {
        // %@ with UTF-8 strings: names may not be ASCII.
        NSString *text = [NSString stringWithFormat:@"%@: %@", @(key.c_str()), @(value.c_str())];
        const NSSize size = [text sizeWithAttributes:attributes];
        const CGFloat x = right ? self.bounds.size.width - margin - size.width : margin;
        [text drawAtPoint:NSMakePoint(x, y) withAttributes:attributes];
        y += lineHeight;
    }
}

// The text attributes, built once: looking the font up on every frame
// returned nil now and then while Embree kept every core busy, and a nil
// value in the dictionary literal threw.
static NSDictionary *HudAttributes() {
    static NSDictionary *attributes = [] {
        NSShadow *shadow = [NSShadow new];
        shadow.shadowColor = [NSColor colorWithWhite:0 alpha:0.8];
        shadow.shadowOffset = NSMakeSize(1, -1);
        shadow.shadowBlurRadius = 1;
        NSFont *font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular]
            ?: [NSFont userFixedPitchFontOfSize:11] ?: [NSFont systemFontOfSize:11];
        // usdview's HUD color.
        return @{
            NSFontAttributeName: font,
            NSForegroundColorAttributeName: [NSColor colorWithSRGBRed:0.733 green:0.604
                                                                  blue:0.333 alpha:1],
            NSShadowAttributeName: shadow,
        };
    }();
    return attributes;
}

- (void)drawRect:(NSRect)dirtyRect {
    NSDictionary *attributes = HudAttributes();
    [self drawLines:_hud.topLeft right:NO bottom:NO attributes:attributes];
    [self drawLines:_hud.topRight right:YES bottom:NO attributes:attributes];
    [self drawLines:_hud.bottomLeft right:NO bottom:YES attributes:attributes];
    [self drawLines:_hud.bottomRight right:YES bottom:YES attributes:attributes];
}

@end
