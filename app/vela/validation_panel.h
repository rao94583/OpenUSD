// The Validation panel (Window → Validation): the document's validation
// issues, from the validate action, with a button to validate again.
// Double-clicking an issue selects its prim.
#pragma once

#import <Cocoa/Cocoa.h>

#include "app/vela/core/document.h"

#include <memory>

@interface VelaValidationPanelController : NSWindowController
- (instancetype)initWithDocument:(std::shared_ptr<vela::Document>)document
                           title:(NSString *)title;
@end
