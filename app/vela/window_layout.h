#import <AppKit/AppKit.h>

// Edge-pinned panes inside a frame-managed split view. Kept independent of
// the renderer so window resizing can be verified without GPU/display work.
NSView *VelaDocumentLayout(NSView *outline, NSView *viewport,
                           NSView *inspector, NSView *timeline);
BOOL VelaCheckDocumentLayout(NSView *outline, NSView *inspector);
