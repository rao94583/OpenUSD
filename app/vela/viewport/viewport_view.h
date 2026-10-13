// The Metal viewport: draws the stage through Storm and translates mouse,
// trackpad and keyboard input into free camera moves and picks.
#import <AppKit/AppKit.h>

#include "pxr/usd/usd/stage.h"

@interface VelaViewportView : NSView

- (instancetype)initWithStage:(PXR_NS::UsdStageRefPtr)stage;

@property (nonatomic) PXR_NS::UsdTimeCode time;
@property (nonatomic, readonly) PXR_NS::SdfPathVector selection;
// Called with a one-line description of the selection and frame time.
@property (nonatomic, copy) void (^statusChanged)(NSString *status);

- (void)frameAll;
- (void)frameSelection;
// Tumbles once around the center of interest over the given number of
// presented frames, then calls completion with the elapsed time.
- (void)benchmarkFrames:(int)frames completion:(void (^)(double seconds))completion;

@end
