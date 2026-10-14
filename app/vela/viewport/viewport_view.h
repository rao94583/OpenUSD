// The Metal viewport: draws a document's stage through Storm and translates
// mouse, trackpad and keyboard input into free camera moves and picks.
#import <AppKit/AppKit.h>

#include "app/vela/core/document.h"

#include <memory>

@interface VelaViewportView : NSView

- (instancetype)initWithDocument:(std::shared_ptr<vela::Document>)document;

// Called with a one-line description of the selection and frame time.
@property (nonatomic, copy) void (^statusChanged)(NSString *status);
// Called once, when the first frame has been drawn.
@property (nonatomic, copy) void (^firstFrameDrawn)(void);
// usdview's --norender: draw nothing.
@property (nonatomic) BOOL renderingDisabled;
// usdview's --bboxStandin: draw unloaded prims as bounding boxes.
- (void)setDrawsUnloadedAsBounds:(BOOL)bounds;

// Tumbles once around the center of interest over the given number of
// presented frames, then calls completion with the elapsed time.
- (void)benchmarkFrames:(int)frames completion:(void (^)(double seconds))completion;

@end
