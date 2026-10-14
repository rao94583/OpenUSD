#import <AppKit/AppKit.h>
#include "app/vela/core/browser.h"

// Read-only sampled spline preview; AppKit drawing, with no GPU dependency.
@interface VelaSplineView : NSView
- (void)setPlot:(const vela::SplinePlot &)plot;
@end
