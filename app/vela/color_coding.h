// usdview's color coding in AppKit (see core/color_codes.h): colors that
// keep usdview's values in dark mode and darken for light mode, the fonts,
// and the legends that explain them.
#import <AppKit/AppKit.h>

#include "app/vela/core/color_codes.h"

NSColor *VelaColor(vela::Rgb rgb);
// Inactive prims (usdview darkens them by half).
NSColor *VelaDimmed(NSColor *color);
NSFont *VelaPrimFont(vela::PrimFont font);

// A legend for prims or properties, for a popover or the legend window.
NSView *VelaLegendView(BOOL prims);
// A small button that shows a legend in a popover.
NSButton *VelaLegendButton(BOOL prims);

// Help → Color Legend: both legends in a panel.
@interface VelaLegendWindowController : NSWindowController
+ (instancetype)shared;
@end
