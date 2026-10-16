// The command palette (Window → Command Palette, ⌥Space): a floating panel
// that searches everything Vela can do. Entries come from the action
// registry (core/palette.h) and the main menu's other commands; a query
// starting with "/" finds prims by path.
#pragma once

#import <Cocoa/Cocoa.h>

@interface VelaPaletteController : NSWindowController
+ (instancetype)shared;
// Shows the palette over the front window, or hides it if shown.
- (void)toggle:(id)sender;
// Makes ⌥Space toggle the palette everywhere in the app, text fields
// included (they would take it as a non-breaking space before the menu's
// key equivalent is tried).
+ (void)installShortcut;
@end
