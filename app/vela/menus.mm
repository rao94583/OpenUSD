#import "menus.h"
#import "document.h"
#import "form_sheet.h"
#import "palette.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "app/vela/core/actions.h"
#include "app/vela/core/log.h"

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
    vela::Log::Get().Add(vela::LogEntry::Level::Error, "Action", result.error);
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

namespace {

// Display names for the formats a save panel offers.
NSString *FormatName(const std::string &extension) {
    NSDictionary<NSString *, NSString *> *names = @{
        @"png": @"PNG", @"jpg": @"JPEG", @"jpeg": @"JPEG", @"tif": @"TIFF", @"tiff": @"TIFF",
        @"exr": @"OpenEXR (linear)", @"bmp": @"BMP", @"tga": @"TGA",
        @"mov": @"QuickTime Movie", @"mp4": @"MPEG-4 Movie",
    };
    return names[@(extension.c_str())] ?: @(extension.c_str()).uppercaseString;
}

}  // namespace

// The save panel's Format pop-up: choosing one sets the panel's type, and
// with it the file name's extension.
@interface VelaSaveFormats : NSObject
@property (nonatomic, weak) NSSavePanel *panel;
@property (nonatomic, copy) NSArray<UTType *> *types;
@end
@implementation VelaSaveFormats
- (void)formatChosen:(NSPopUpButton *)popUp {
    self.panel.allowedContentTypes = @[self.types[NSUInteger(popUp.indexOfSelectedItem)]];
}
@end

// Asks for the file an action writes (savePathParam), then runs it.
static void RunWithSavePath(const vela::Action &action, const JsObject &params, NSWindow *window) {
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.title = [@(action.title.c_str()) stringByReplacingOccurrencesOfString:@"…" withString:@""];
    panel.allowsOtherFileTypes = NO;
    // Named after the document, e.g. playroom.png.
    if (NSDocument *document = window.windowController.document) {
        panel.nameFieldStringValue = document.displayName.stringByDeletingPathExtension;
    }
    __block VelaSaveFormats *formats = nil;
    if (action.saveTypes.empty()) {
        panel.allowedContentTypes = UsdTypes();
    } else {
        // One type per format (jpg and jpeg are one), the first chosen.
        NSMutableArray<UTType *> *types = [NSMutableArray array];
        NSPopUpButton *popUp = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        for (const std::string &extension : action.saveTypes) {
            UTType *type = [UTType typeWithFilenameExtension:@(extension.c_str())];
            if (!type || [types containsObject:type]) continue;
            [types addObject:type];
            [popUp addItemWithTitle:FormatName(extension)];
        }
        panel.allowedContentTypes = @[types.firstObject];
        if (types.count > 1) {
            formats = [VelaSaveFormats new];
            formats.panel = panel;
            formats.types = types;
            popUp.target = formats;
            popUp.action = @selector(formatChosen:);
            NSTextField *label = [NSTextField labelWithString:@"Format:"];
            NSStackView *accessory = [NSStackView stackViewWithViews:@[label, popUp]];
            accessory.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
            panel.accessoryView = accessory;
        }
    }
    // Blocks capture C++ references as references: copy what the
    // completion needs.
    const std::string name = action.name, param = action.savePathParam;
    const JsObject given = params;
    [panel beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        (void)formats;  // kept alive while the panel is up
        vela::Document *current = CurrentDocument();
        if (response != NSModalResponseOK || !current) return;
        JsObject withPath = given;
        withPath[param] = JsValue(std::string(panel.URL.fileSystemRepresentation));
        ShowResult(vela::ActionRegistry::Get().Run(*current, name, withPath), window);
    }];
}

void VelaRunAction(const std::string &actionName, const JsObject &params, NSWindow *window) {
    vela::Document *document = CurrentDocument();
    const vela::Action *action = vela::ActionRegistry::Get().Find(actionName);
    if (!document || !action) return;
    if (action->form) {
        // Ask for the fields, then run with them under formParam (after
        // asking for a file, if the action writes one).
        const std::string name = action->name, param = action->formParam;
        const JsObject given = params;  // not the reference: see RunWithSavePath
        // The sheet's title is the item's, without its ellipsis.
        NSString *title = [@(action->title.c_str())
            stringByReplacingOccurrencesOfString:@"…" withString:@""];
        VelaRunForm(window, title, action->form(*document),
                    ^(const JsObject &values) {
            vela::Document *current = CurrentDocument();
            const vela::Action *again = vela::ActionRegistry::Get().Find(name);
            if (!current || !again) return;
            JsObject withValues = given;
            if (param.empty()) {
                for (const auto &[key, value] : values) withValues[key] = value;
            } else {
                withValues[param] = JsValue(values);
            }
            if (!again->savePathParam.empty()) {
                // After the form's sheet has gone.
                const JsObject ready = withValues;
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (const vela::Action *a = vela::ActionRegistry::Get().Find(name)) {
                        RunWithSavePath(*a, ready, window);
                    }
                });
                return;
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
        const JsObject given = params;  // not the reference: see RunWithSavePath
        [panel beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
            vela::Document *current = CurrentDocument();
            if (response != NSModalResponseOK || !current) return;
            JsObject withPath = given;
            withPath[param] = JsValue(std::string(panel.URL.fileSystemRepresentation));
            ShowResult(vela::ActionRegistry::Get().Run(*current, name, withPath), window);
        }];
        return;
    }
    if (action->savePathParam.empty()) {
        ShowResult(vela::ActionRegistry::Get().Run(*document, action->name, params), window);
        return;
    }
    RunWithSavePath(*action, params, window);
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

    // The window's panes, for the front document window (its controller
    // handles them and titles them Show or Hide).
    for (NSMenuItem *top in bar.itemArray) {
        if (![top.submenu.title isEqualToString:@"View"]) continue;
        NSMenu *view = top.submenu;
        [view addItem:[NSMenuItem separatorItem]];
        struct { NSString *title; SEL action; NSString *key; NSEventModifierFlags modifiers; } const panes[] = {
            {@"Hide Prim Browser", @selector(togglePrimBrowser:), @"s",
             NSEventModifierFlagControl | NSEventModifierFlagCommand},
            {@"Hide Inspector", @selector(toggleInspector:), @"i",
             NSEventModifierFlagOption | NSEventModifierFlagCommand},
            {@"Hide Bottom Panel", @selector(toggleBottomPanel:), @"y",
             NSEventModifierFlagShift | NSEventModifierFlagCommand},
        };
        for (const auto &pane : panes) {
            NSMenuItem *item = [view addItemWithTitle:pane.title action:pane.action keyEquivalent:pane.key];
            item.keyEquivalentModifierMask = pane.modifiers;
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
    // The bottom panel's tabs: an interpreter for the front document
    // (usdview's Interpreter), the log, and the Claude console.
    NSMenuItem *console = [window addItemWithTitle:@"Python Console"
        action:@selector(showPythonConsole:) keyEquivalent:@"p"];
    console.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    NSMenuItem *log = [window addItemWithTitle:@"Log" action:@selector(showLog:) keyEquivalent:@"l"];
    log.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    NSMenuItem *claude = [window addItemWithTitle:@"Claude" action:@selector(showClaudeConsole:)
                                    keyEquivalent:@"k"];
    claude.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [window addItem:[NSMenuItem separatorItem]];
    [window addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:)
               keyEquivalent:@"m"];
    NSApp.windowsMenu = window;

    // What the browser's colors and fonts mean (the app delegate shows it).
    NSMenu *help = addMenu(@"Help");
    [help addItemWithTitle:@"Color Legend" action:@selector(showColorLegend:) keyEquivalent:@""];
    NSApp.helpMenu = help;
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
        vela::Log::Get().Add(vela::LogEntry::Level::Error, "Action", result.error);
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

namespace {

// Marks what plugins added to the menu bar, to remove on reload.
const NSInteger PluginTag = 0x506C7567;  // 'Plug'

NSMenu *PluginSubmenu(NSMenu *parent, NSString *title, BOOL beforeWindow) {
    for (NSMenuItem *item in parent.itemArray) {
        if (item.submenu && [item.submenu.title compare:title
                options:NSCaseInsensitiveSearch] == NSOrderedSame) {
            return item.submenu;
        }
    }
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    item.submenu = [[NSMenu alloc] initWithTitle:title];
    item.tag = PluginTag;
    NSInteger index = parent.numberOfItems;
    if (beforeWindow) {
        const NSInteger window = [parent indexOfItemWithSubmenu:NSApp.windowsMenu];
        if (window >= 0) index = window;
    }
    [parent insertItem:item atIndex:index];
    return item.submenu;
}

void RemovePluginItems(NSMenu *menu) {
    for (NSMenuItem *item in [menu.itemArray copy]) {
        if (item.tag == PluginTag) [menu removeItem:item];
        else if (item.submenu) RemovePluginItems(item.submenu);
    }
}

} // namespace

void VelaInstallPluginMenus(const vela::PluginReport &report) {
    NSMenu *bar = NSApp.mainMenu;
    if (!bar) return;
    RemovePluginItems(bar);
    VelaActionTarget *target = [VelaActionTarget shared];
    for (const vela::PluginMenuEntry &entry : report.menus) {
        NSMenu *menu = bar;
        for (size_t i = 0; i < entry.path.size(); ++i) {
            menu = PluginSubmenu(menu, @(entry.path[i].c_str()), i == 0);
        }
        // Plugin items follow the menu's own, after a separator.
        if (menu.numberOfItems && menu.itemArray.lastObject.tag != PluginTag) {
            NSMenuItem *separator = [NSMenuItem separatorItem];
            separator.tag = PluginTag;
            [menu addItem:separator];
        }
        if (entry.separator) {
            NSMenuItem *separator = [NSMenuItem separatorItem];
            separator.tag = PluginTag;
            [menu addItem:separator];
            continue;
        }
        const vela::Action *action = vela::ActionRegistry::Get().Find(entry.command);
        if (!action) continue;
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@(action->title.c_str())
                                                      action:@selector(runAction:)
                                               keyEquivalent:@""];
        std::string key;
        unsigned modifiers = 0;
        if (vela::ParsePluginShortcut(entry.shortcut, &key, &modifiers)) {
            item.keyEquivalent = @(key.c_str());
            item.keyEquivalentModifierMask = ModifierFlags(modifiers);
        }
        item.target = target;
        item.representedObject = @(action->name.c_str());
        item.toolTip = @(action->description.c_str());
        item.tag = PluginTag;
        [menu addItem:item];
    }
}
