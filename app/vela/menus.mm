#import "menus.h"
#import "document.h"
#import "form_sheet.h"
#import "palette.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "app/vela/core/actions.h"

#include "pxr/base/js/json.h"

PXR_NAMESPACE_USING_DIRECTIVE

#include <set>
#include <string>

@interface VelaPaletteTrampoline : NSObject
@end
@implementation VelaPaletteTrampoline
- (void)toggle:(id)sender { [[VelaPaletteController shared] toggle:sender]; }
@end

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

// Menu items carry an action name, or for submenu choices the name and
// fixed parameters as JSON: @[name, params].
std::string ActionName(id represented) {
    if ([represented isKindOfClass:[NSArray class]]) represented = [represented firstObject];
    return [represented UTF8String];
}
JsObject ActionParams(id represented) {
    if (![represented isKindOfClass:[NSArray class]]) return {};
    const JsValue value = JsParseString([[represented lastObject] UTF8String]);
    return value.IsObject() ? value.GetJsObject() : JsObject();
}

// Errors from actions run from the menu show as an alert on the window.
void ShowResult(const vela::ActionResult &result, NSWindow *window) {
    if (result.ok) return;
    NSAlert *alert = [NSAlert new];
    alert.alertStyle = NSAlertStyleWarning;
    const std::string &error = result.error;
    const size_t newline = error.find('\n');
    alert.messageText = @(error.substr(0, newline).c_str());
    if (newline != std::string::npos) {
        alert.informativeText = @(error.substr(newline + 1).c_str());
    }
    if (window) [alert beginSheetModalForWindow:window completionHandler:nil];
    else [alert runModal];
}

// Files a panel offers: .usda, .usdc and .usd.
NSArray<UTType *> *UsdTypes() {
    return @[[UTType typeWithFilenameExtension:@"usda"],
             [UTType typeWithFilenameExtension:@"usdc"],
             [UTType typeWithFilenameExtension:@"usd"]];
}

// The palette, made when first shown (not while the menu is built, e.g.
// for --describe-menus).
id PaletteTarget() {
    static id target = [VelaPaletteTrampoline new];
    return target;
}

bool IsPlainKey(const vela::Action &action) {
    return !(action.modifiers & (vela::ModifierCommand | vela::ModifierControl));
}

} // namespace

void VelaRunAction(const std::string &actionName, const JsObject &params, NSWindow *window) {
    vela::Document *document = CurrentDocument();
    const vela::Action *action = vela::ActionRegistry::Get().Find(actionName);
    if (!document || !action) return;
    if (action->form) {
        // Ask for the fields, then run with them under formParam.
        const std::string name = action->name, param = action->formParam;
        // The sheet's title is the item's, without its ellipsis.
        NSString *title = [@(action->title.c_str())
            stringByReplacingOccurrencesOfString:@"…" withString:@""];
        VelaRunForm(window, title, action->form(*document),
                    ^(const JsObject &values) {
            vela::Document *current = CurrentDocument();
            if (!current) return;
            JsObject withValues = params;
            if (param.empty()) {
                for (const auto &[key, value] : values) withValues[key] = value;
            } else {
                withValues[param] = JsValue(values);
            }
            ShowResult(vela::ActionRegistry::Get().Run(*current, name, withValues), window);
        });
        return;
    }
    if (!action->openPathParam.empty()) {
        // Ask for a USD file to read.
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.title = [@(action->title.c_str())
            stringByReplacingOccurrencesOfString:@"…" withString:@""];
        panel.allowedContentTypes = UsdTypes();
        const std::string name = action->name, param = action->openPathParam;
        [panel beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
            vela::Document *current = CurrentDocument();
            if (response != NSModalResponseOK || !current) return;
            JsObject withPath = params;
            withPath[param] = JsValue(std::string(panel.URL.fileSystemRepresentation));
            ShowResult(vela::ActionRegistry::Get().Run(*current, name, withPath), window);
        }];
        return;
    }
    if (action->savePathParam.empty()) {
        ShowResult(vela::ActionRegistry::Get().Run(*document, action->name, params), window);
        return;
    }
    // Ask for the file first.
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.title = @(action->title.c_str());
    panel.allowedContentTypes = UsdTypes();
    panel.allowsOtherFileTypes = NO;
    const std::string name = action->name, param = action->savePathParam;
    [panel beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        vela::Document *current = CurrentDocument();
        if (response != NSModalResponseOK || !current) return;
        JsObject withPath = params;
        withPath[param] = JsValue(std::string(panel.URL.fileSystemRepresentation));
        ShowResult(vela::ActionRegistry::Get().Run(*current, name, withPath), window);
    }];
}

// Menu items carry the action name; this target runs and validates them.
@interface VelaActionTarget : NSObject <NSMenuItemValidation, NSMenuDelegate>
+ (instancetype)shared;
@end

@implementation VelaActionTarget

+ (instancetype)shared {
    static VelaActionTarget *target = [VelaActionTarget new];
    return target;
}

// Items for one choice of an action.
- (NSMenuItem *)itemForChoice:(const vela::MenuChoice &)choice
                       action:(const vela::Action &)action {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@(choice.title.c_str())
        action:@selector(runAction:) keyEquivalent:@""];
    item.target = self;
    item.representedObject = @[@(action.name.c_str()),
        @(JsWriteToString(JsValue(choice.params)).c_str())];
    return item;
}

// Dynamic submenus are rebuilt from the current document as they open. The
// parent item carries the action name.
- (void)menuNeedsUpdate:(NSMenu *)menu {
    [menu removeAllItems];
    NSMenuItem *parent = nil;
    for (NSMenuItem *item in menu.supermenu.itemArray) {
        if (item.submenu == menu) parent = item;
    }
    const vela::Action *action = parent.representedObject
        ? vela::ActionRegistry::Get().Find([parent.representedObject UTF8String]) : nullptr;
    vela::Document *document = CurrentDocument();
    if (!action || !action->dynamicChoices || !document) return;
    for (const vela::MenuChoice &choice : action->dynamicChoices(*document)) {
        [menu addItem:[self itemForChoice:choice action:*action]];
    }
}

- (void)runAction:(NSMenuItem *)item {
    VelaRunAction(ActionName(item.representedObject), ActionParams(item.representedObject),
                  NSApp.keyWindow ?: NSApp.mainWindow);
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    const vela::ActionRegistry &registry = vela::ActionRegistry::Get();
    const vela::Action *action = registry.Find(ActionName(item.representedObject));
    vela::Document *document = CurrentDocument();
    if (!action || !document) {
        return NO;
    }
    if ([item.representedObject isKindOfClass:[NSArray class]]) {
        const bool chosen = action->chosen &&
            action->chosen(*document, ActionParams(item.representedObject));
        item.state = chosen ? NSControlStateValueOn : NSControlStateValueOff;
        return registry.IsEnabled(*action, *document);
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
    // Returns the menu with this title, adding it if needed, so actions can
    // add to the built-in menus.
    auto addMenu = [&](NSString *title) {
        for (NSMenuItem *existing in bar.itemArray) {
            if ([existing.submenu.title isEqualToString:title]) return existing.submenu;
        }
        NSMenuItem *item = [NSMenuItem new];
        NSMenu *menu = [[NSMenu alloc] initWithTitle:title];
        item.submenu = menu;
        [bar addItem:item];
        return menu;
    };

    NSMenu *app = addMenu(@"Vela");
    // The app delegate shows the window.
    [app addItemWithTitle:@"Settings…" action:@selector(showSettings:) keyEquivalent:@","];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItemWithTitle:@"Hide Vela" action:@selector(hide:) keyEquivalent:@"h"];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItemWithTitle:@"Quit Vela" action:@selector(terminate:) keyEquivalent:@"q"];

    NSMenu *file = addMenu(@"File");
    [file addItemWithTitle:@"Open…" action:@selector(openDocument:) keyEquivalent:@"o"];
    // usdview's stage options: the app delegate opens, the front document
    // reopens.
    [file addItemWithTitle:@"Open with Options…" action:@selector(openWithOptions:)
             keyEquivalent:@""];
    [file addItemWithTitle:@"Reopen with Options…" action:@selector(reopenWithOptions:)
             keyEquivalent:@""];
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
            if (!action.choices.empty() || action.dynamicChoices) {
                // A submenu with an item per choice, built now or as it opens.
                NSMenuItem *parent = [[NSMenuItem alloc] initWithTitle:@(action.title.c_str())
                                                                action:nil keyEquivalent:@""];
                NSMenu *submenu = [[NSMenu alloc] initWithTitle:@(action.title.c_str())];
                for (const vela::MenuChoice &choice : action.choices) {
                    [submenu addItem:[target itemForChoice:choice action:action]];
                }
                if (action.dynamicChoices) {
                    parent.representedObject = @(action.name.c_str());
                    submenu.delegate = target;
                }
                parent.submenu = submenu;
                [menu addItem:parent];
                continue;
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

    // Text editing in fields (filters, forms, the palette) needs the
    // standard items: AppKit routes ⌘X, ⌘C, ⌘V and ⌘A through them.
    NSMenu *edit = addMenu(@"Edit");
    NSInteger at = 0;
    for (NSMenuItem *item in edit.itemArray) {
        ++at;
        if ([item.representedObject isEqual:@"redo"]) break;
    }
    [edit insertItem:[NSMenuItem separatorItem] atIndex:at++];
    for (NSArray *standard in @[@[@"Cut", @"cut:", @"x"], @[@"Copy", @"copy:", @"c"],
                                @[@"Paste", @"paste:", @"v"],
                                @[@"Select All", @"selectAll:", @"a"]]) {
        [edit insertItem:[[NSMenuItem alloc] initWithTitle:standard[0]
                                                    action:NSSelectorFromString(standard[1])
                                             keyEquivalent:standard[2]]
                 atIndex:at++];
    }

    NSMenu *window = addMenu(@"Window");
    // Everything Vela can do, searchable. An event monitor handles ⌥Space
    // (see installShortcut); the key equivalent shows it in the menu.
    NSMenuItem *palette = [window addItemWithTitle:@"Command Palette"
        action:@selector(toggle:) keyEquivalent:@" "];
    palette.keyEquivalentModifierMask = NSEventModifierFlagOption;
    palette.target = PaletteTarget();
    // The front document shows its issues.
    [window addItemWithTitle:@"Validation" action:@selector(showValidation:) keyEquivalent:@""];
    // An interpreter for the front document (usdview's Interpreter).
    NSMenuItem *console = [window addItemWithTitle:@"Python Console"
        action:@selector(showPythonConsole:) keyEquivalent:@"p"];
    console.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [window addItem:[NSMenuItem separatorItem]];
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
            for (NSMenuItem *choice in item.submenu.itemArray) {
                // Parameters on one line.
                NSString *params = [[[choice.representedObject lastObject]
                    componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]
                    componentsJoinedByString:@""];
                [text appendFormat:@"\n    %@ {%@ %@}", choice.title,
                    [choice.representedObject firstObject], params];
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
