// The main menu, built from the action registry, and keyboard shortcuts.
#import <AppKit/AppKit.h>

#include "app/vela/core/document.h"

// The app, File and Window menus, with a menu for each menu named by an
// action, in registration order. Items run actions on the current
// document.
NSMenu *VelaBuildMainMenu();

// Runs the action bound to a key event, if one is bound and enabled.
bool VelaRunShortcut(NSEvent *event, vela::Document &document);

// The menu tree as text, one item per line, for tests.
NSString *VelaDescribeMenu(NSMenu *menu);
