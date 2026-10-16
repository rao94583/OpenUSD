#import <AppKit/AppKit.h>

#include "app/vela/core/document.h"
#include "app/vela/core/launch_options.h"

#include <memory>

@class VelaViewportView;

// usdview's flags from the command line, for the document that opens their
// file. Other documents open with the defaults.
void VelaSetLaunchOptions(const vela::LaunchOptions &options);
// Opens url with options (its file is set from url), unless it is already
// open; then shows an alert and returns NO.
BOOL VelaOpenWithOptions(NSURL *url, vela::LaunchOptions options);
// When the app started, for --timing.
void VelaSetLaunchTime(double seconds);

// The AppKit document: opens files and owns the core document and window.
@interface VelaDocument : NSDocument
@property (nonatomic, readonly) std::shared_ptr<vela::Document> core;
@property (nonatomic, readonly) VelaViewportView *viewport;
// The document's id in vela::Session (Python and MCP), once its window
// exists; 0 before.
@property (nonatomic, readonly) int sessionId;
- (size_t)primCount;
@end
