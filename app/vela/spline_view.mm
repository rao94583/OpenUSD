#import "spline_view.h"
#include <algorithm>
#include <cmath>
#include <limits>

@implementation VelaSplineView {
    vela::SplinePlot _plot;
}
- (void)setPlot:(const vela::SplinePlot &)plot { _plot = plot; self.needsDisplay = YES; }
- (void)drawRect:(NSRect)dirtyRect {
    [NSColor.textBackgroundColor setFill]; NSRectFill(self.bounds);
    NSDictionary *style = @{NSFontAttributeName: [NSFont systemFontOfSize:11],
                            NSForegroundColorAttributeName: NSColor.secondaryLabelColor};
    if (_plot.samples.empty()) {
        [@"No authored scalar spline on this property" drawAtPoint:NSMakePoint(20, 20) withAttributes:style];
        return;
    }
    NSRect area = NSInsetRect(self.bounds, 50, 35);
    if (area.size.width <= 0 || area.size.height <= 0) return;
    double x0 = _plot.samples.front()[0], x1 = _plot.samples.back()[0];
    double y0 = std::numeric_limits<double>::max(), y1 = std::numeric_limits<double>::lowest();
    for (const auto &p : _plot.samples) { y0 = std::min(y0, p[1]); y1 = std::max(y1, p[1]); }
    if (x0 == x1) { x0 -= 0.5; x1 += 0.5; }
    if (y0 == y1) { y0 -= 0.5; y1 += 0.5; }
    if (!std::isfinite(x1 - x0) || !std::isfinite(y1 - y0) || x1 <= x0 || y1 <= y0) return;
    auto point = [&](const PXR_NS::GfVec2d &p) {
        return NSMakePoint(area.origin.x + (p[0] - x0) / (x1 - x0) * area.size.width,
                           area.origin.y + (p[1] - y0) / (y1 - y0) * area.size.height);
    };
    [NSColor.separatorColor setStroke]; [NSBezierPath strokeRect:area];
    NSBezierPath *curve = [NSBezierPath bezierPath];
    [curve moveToPoint:point(_plot.samples.front())];
    for (size_t i = 1; i < _plot.samples.size(); ++i) [curve lineToPoint:point(_plot.samples[i])];
    curve.lineWidth = 1.5; [NSColor.controlAccentColor setStroke]; [curve stroke];
    [NSColor.labelColor setFill];
    for (const auto &p : _plot.knots) {
        const auto xy = point(p);
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(xy.x - 2.5, xy.y - 2.5, 5, 5)] fill];
    }
    [[NSString stringWithFormat:@"%.4g", y0] drawAtPoint:NSMakePoint(3, area.origin.y) withAttributes:style];
    [[NSString stringWithFormat:@"%.4g", y1] drawAtPoint:NSMakePoint(3, NSMaxY(area) - 12) withAttributes:style];
    [[NSString stringWithFormat:@"Time %.4g — %.4g · sampled preview", x0, x1]
        drawAtPoint:NSMakePoint(area.origin.x, 10) withAttributes:style];
}
@end
