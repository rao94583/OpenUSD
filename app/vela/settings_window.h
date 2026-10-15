// The Settings window (Vela → Settings…, ⌘,): the opening options that
// replace usdview's launch flags, the view settings new windows start
// with, and the thread limit. Changes apply at once and are saved.
#pragma once

#import <Cocoa/Cocoa.h>

@interface VelaSettingsWindowController : NSWindowController
+ (instancetype)shared;
@end
