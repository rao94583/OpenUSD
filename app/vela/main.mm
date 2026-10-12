#import <AppKit/AppKit.h>
#include <cstdio>
#include <cstring>
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/base/plug/registry.h"

PXR_NAMESPACE_USING_DIRECTIVE

@interface VelaDocument : NSDocument
- (size_t)primCount;
@end

@implementation VelaDocument {
    UsdStageRefPtr _stage;
}
+ (BOOL)autosavesInPlace { return NO; }
- (BOOL)readFromURL:(NSURL *)url ofType:(NSString *)type error:(NSError **)error {
    _stage = UsdStage::Open(url.fileSystemRepresentation);
    if (_stage) return YES;
    if (error) *error = [NSError errorWithDomain:@"org.openusd.vela" code:1
        userInfo:@{NSLocalizedDescriptionKey:
            [NSString stringWithFormat:@"Could not open USD stage: %@", url.lastPathComponent]}];
    return NO;
}
- (size_t)primCount {
    size_t count = 0;
    if (_stage) for (const auto &prim : _stage->Traverse()) { (void)prim; ++count; }
    return count;
}
- (void)makeWindowControllers {
    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 500)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                  NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    size_t count = [self primCount];
    NSTextField *label = [NSTextField labelWithString:
        [NSString stringWithFormat:@"%zu prims", count]];
    label.font = [NSFont systemFontOfSize:28];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [window.contentView addSubview:label];
    [NSLayoutConstraint activateConstraints:@[
        [label.centerXAnchor constraintEqualToAnchor:window.contentView.centerXAnchor],
        [label.centerYAnchor constraintEqualToAnchor:window.contentView.centerYAnchor]]];
    [window center];
    [self addWindowController:[[NSWindowController alloc] initWithWindow:window]];
}
@end

@interface VelaDelegate : NSObject <NSApplicationDelegate>
@end
@implementation VelaDelegate
- (BOOL)applicationShouldOpenUntitledFile:(NSApplication *)sender { return NO; }
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    [NSApp activateIgnoringOtherApps:YES];
}
@end

static void AddMenuItem(NSMenu *menu, NSString *title, SEL action, NSString *key) {
    [menu addItemWithTitle:title action:action keyEquivalent:key];
}
int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSString *runtime = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"Runtime/usd"];
        PlugRegistry::GetInstance().RegisterPlugins(
            [[runtime stringByAppendingPathComponent:@"lib/usd/plugInfo.json"] UTF8String]);
        PlugRegistry::GetInstance().RegisterPlugins(
            [[runtime stringByAppendingPathComponent:@"plugin/usd/plugInfo.json"] UTF8String]);
        if (argc == 3 && strcmp(argv[1], "--check-document") == 0) {
            VelaDocument *document = [VelaDocument new];
            NSError *error = nil;
            NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]]];
            if (![document readFromURL:url ofType:@"USD Scene" error:&error]) {
                fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
                return 1;
            }
            printf("%zu prims\n", [document primCount]);
            return 0;
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        VelaDelegate *delegate = [VelaDelegate new];
        NSApp.delegate = delegate;
        NSMenu *bar = [NSMenu new];
        NSMenuItem *appItem = [NSMenuItem new];
        NSMenu *app = [[NSMenu alloc] initWithTitle:@"Vela"];
        AddMenuItem(app, @"Quit Vela", @selector(terminate:), @"q");
        appItem.submenu = app; [bar addItem:appItem];
        NSMenuItem *fileItem = [NSMenuItem new];
        NSMenu *file = [[NSMenu alloc] initWithTitle:@"File"];
        AddMenuItem(file, @"Open…", @selector(openDocument:), @"o");
        AddMenuItem(file, @"Close", @selector(performClose:), @"w");
        fileItem.submenu = file; [bar addItem:fileItem];
        NSApp.mainMenu = bar;
        [NSDocumentController sharedDocumentController];
        [NSApp run];
    }
    return 0;
}
