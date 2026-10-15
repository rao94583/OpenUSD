// Draws the heads-up display's text over the viewport. Clicks pass through.
#import <AppKit/AppKit.h>

#include "app/vela/core/hud.h"

@interface VelaHudView : NSView
- (void)setHud:(const vela::Hud &)hud;
@end
