#import <AppKit/AppKit.h>

#include "app/vela/core/document.h"

#include <memory>

@class VelaViewportView;

// The AppKit document: opens files and owns the core document and window.
@interface VelaDocument : NSDocument
@property (nonatomic, readonly) std::shared_ptr<vela::Document> core;
@property (nonatomic, readonly) VelaViewportView *viewport;
- (size_t)primCount;
@end
