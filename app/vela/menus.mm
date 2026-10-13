#import "menus.h"
#import "document.h"

#include "app/vela/core/actions.h"

#include <set>
#include <string>

namespace {

NSEventModifierFlags ModifierFlags(unsigned modifiers) {
    NSEventModifierFlags flags = 0;
    if (modifiers & vela::ModifierCommand) flags |= NSEventModifierFlagCommand;
    if (modifiers & vela::ModifierShift) flags |= NSEventModifierFlagShift;
    if (modifiers & vela::ModifierOption) flags |= NSEventModifierFlagOption;
    if (modifiers & vela::ModifierControl) flags |= NSEventModifierFlagControl;
    return flags;
}

unsigned Modifiers(NSEventModifierFlags flags) {
    unsigned modifiers = 0;
    if (flags & NSEventModifierFlagCommand) modifiers |= vela::ModifierCommand;
    if (flags & NSEventModifierFlagShift) modifiers |= vela::ModifierShift;
    if (flags & NSEventModifierFlagOption) modifiers |= vela::ModifierOption;
    if (flags & NSEventModifierFlagControl) modifiers |= vela::ModifierControl;
    return modifiers;
}

// The document of the key window, if it is a Vela document.
vela::Document *CurrentDocument() {
    id document = NSDocumentController.sharedDocumentController.currentDocument;
    if ([document isKindOfClass:[VelaDocument class]]) {
        return ((VelaDocument *)document).core.get();
    }
    return nullptr;
}

// Shortcuts without Command or Control are plain typing in a text field.
bool IsTyping() {
    id responder = NSApp.keyWindow.firstResponder;
    return [responder isKindOfClass:[NSText class]];
}

bool IsPlainKey(const vela::Action &action) {
    return !(action.modifiers & (vela::ModifierCommand | vela::ModifierControl));
}

} // namespace

// Menu items carry the action name; this target runs and validates them.
@interface VelaActionTarget : NSObject <NSMenuItemValidation>
+ (instancetype)shared;
@end

@implementation VelaActionTarget

+ (instancetype)shared {
    static VelaActionTarget *target = [VelaActionTarget new];
    return target;
}

- (void)runAction:(NSMenuItem *)item {
    vela::Document *document = CurrentDocument();
    if (!document) return;
    vela::ActionResult result = vela::ActionRegistry::Get().Run(
        *document, [item.representedObject UTF8String]);
    if (!result.ok) {
        NSLog(@"%s", result.error.c_str());
    }
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    const vela::ActionRegistry &registry = vela::ActionRegistry::Get();
    const vela::Action *action = registry.Find([item.representedObject UTF8String]);
    vela::Document *document = CurrentDocument();
    if (!action || !document) {
        return NO;
    }
    item.title = @(registry.GetTitle(*action, *document).c_str());
    if (IsPlainKey(*action) && IsTyping()) {
        // Disabled items pass their key on, here to the text field.
        return NO;
    }
    return registry.IsEnabled(*action, *document);
}

@end

NSMenu *VelaBuildMainMenu() {
    NSMenu *bar = [NSMenu new];
    auto addMenu = [&](NSString *title) {
        NSMenuItem *item = [NSMenuItem new];
        NSMenu *menu = [[NSMenu alloc] initWithTitle:title];
        item.submenu = menu;
        [bar addItem:item];
        return menu;
    };

    NSMenu *app = addMenu(@"Vela");
    [app addItemWithTitle:@"Hide Vela" action:@selector(hide:) keyEquivalent:@"h"];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItemWithTitle:@"Quit Vela" action:@selector(terminate:) keyEquivalent:@"q"];

    NSMenu *file = addMenu(@"File");
    [file addItemWithTitle:@"Open…" action:@selector(openDocument:) keyEquivalent:@"o"];
    [file addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];

    // Menus named by actions, in the order they first appear.
    VelaActionTarget *target = [VelaActionTarget shared];
    std::set<std::string> built;
    for (const vela::Action &first : vela::ActionRegistry::Get().GetActions()) {
        if (first.menu.empty() || !built.insert(first.menu).second) continue;
        NSMenu *menu = addMenu(@(first.menu.c_str()));
        menu.autoenablesItems = YES;
        for (const vela::Action &action : vela::ActionRegistry::Get().GetActions()) {
            if (action.menu != first.menu) continue;
            if (action.separatorBefore && menu.numberOfItems) {
                [menu addItem:[NSMenuItem separatorItem]];
            }
            NSMenuItem *item = [[NSMenuItem alloc]
                initWithTitle:@(action.title.c_str())
                       action:@selector(runAction:)
                keyEquivalent:@(action.key.c_str())];
            item.keyEquivalentModifierMask = ModifierFlags(action.modifiers);
            item.target = target;
            item.representedObject = @(action.name.c_str());
            [menu addItem:item];
        }
    }

    NSMenu *window = addMenu(@"Window");
    [window addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:)
               keyEquivalent:@"m"];
    NSApp.windowsMenu = window;
    return bar;
}

bool VelaRunShortcut(NSEvent *event, vela::Document &document) {
    NSString *characters = event.charactersIgnoringModifiers.lowercaseString;
    if (!characters.length) return false;
    const vela::ActionRegistry &registry = vela::ActionRegistry::Get();
    const vela::Action *action = registry.FindShortcut(
        characters.UTF8String, Modifiers(event.modifierFlags));
    if (!action || !registry.IsEnabled(*action, document)) {
        return false;
    }
    vela::ActionResult result = registry.Run(document, action->name);
    if (!result.ok) {
        NSLog(@"%s", result.error.c_str());
    }
    return true;
}

NSString *VelaDescribeMenu(NSMenu *menu) {
    NSMutableString *text = [NSMutableString string];
    for (NSMenuItem *top in menu.itemArray) {
        [text appendFormat:@"%@\n", top.submenu.title];
        for (NSMenuItem *item in top.submenu.itemArray) {
            if (item.isSeparatorItem) {
                [text appendString:@"  ---\n"];
                continue;
            }
            [text appendFormat:@"  %@", item.title];
            if (item.representedObject) {
                [text appendFormat:@" [%@]", item.representedObject];
            }
            if (item.keyEquivalent.length) {
                [text appendFormat:@" key=%@ mods=%lu", item.keyEquivalent,
                    (unsigned long)(item.keyEquivalentModifierMask &
                     NSEventModifierFlagDeviceIndependentFlagsMask)];
            }
            [text appendString:@"\n"];
        }
    }
    return text;
}
