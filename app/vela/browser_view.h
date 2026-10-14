#import <AppKit/AppKit.h>
#include "app/vela/core/document.h"

// Lazy prim outline and read-only inspector, sharing the document's selection.
@interface VelaBrowserController : NSObject
- (instancetype)initWithDocument:(std::shared_ptr<vela::Document>)document;
@property (nonatomic, readonly) NSView *outlineView;
@property (nonatomic, readonly) NSView *inspectorView;
// Display-independent AppKit smoke check for outline/selection/inspector wiring.
- (BOOL)checkBrowsing;
@end
