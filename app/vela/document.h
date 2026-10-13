#import <AppKit/AppKit.h>

@class VelaViewportView;

#include "pxr/usd/usd/stage.h"

@interface VelaDocument : NSDocument
@property (nonatomic, readonly) PXR_NS::UsdStageRefPtr stage;
@property (nonatomic, readonly) VelaViewportView *viewport;
- (size_t)primCount;
@end
