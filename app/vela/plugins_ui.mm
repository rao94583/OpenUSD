#import "plugins_ui.h"
#import "menus.h"

#import <AppKit/AppKit.h>

#include "app/vela/core/actions.h"
#include "app/vela/core/plugins.h"
#include "app/vela/core/python.h"

PXR_NAMESPACE_USING_DIRECTIVE

void VelaRegisterPluginAppActions(void) {
    vela::Action show;
    show.name = "show_plugins_folder";
    show.title = "Show Plugins Folder";
    show.description = "Show the folder Vela loads plugins from in Finder, creating it if "
                       "needed. Plugins on PXR_PLUGINPATH_NAME load too.";
    show.menu = "Plugins";
    show.run = [](vela::Document &, const JsObject &) {
        NSString *folder = @(vela::PluginsFolder().c_str());
        NSError *error = nil;
        if (![NSFileManager.defaultManager createDirectoryAtPath:folder
                                     withIntermediateDirectories:YES attributes:nil error:&error]) {
            return vela::ActionResult::Error(error.localizedDescription.UTF8String);
        }
        [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:folder isDirectory:YES]];
        return vela::ActionResult{true, JsValue(std::string(folder.UTF8String)), ""};
    };
    vela::ActionRegistry::Get().Register(std::move(show));
}

void VelaStartPlugins(void) {
    vela::SetPluginsChangedCallback([](const vela::PluginReport &report) {
        VelaInstallPluginMenus(report);
    });
    if (!vela::IsPythonInitialized()) return;
    // After the first window opens: loading imports pxr into Python.
    dispatch_async(dispatch_get_main_queue(), ^{
        if (vela::HasPlugins()) vela::LoadPlugins(false);
    });
}
