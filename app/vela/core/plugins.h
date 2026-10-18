// Plugins, found and loaded as usdview finds and loads its own: Python
// packages with a plugInfo.json declaring a container type derived from
// vela.PluginContainer (see vela_module.py), registered with USD's plugin
// registry from Vela's plugins folder and PXR_PLUGINPATH_NAME. Each
// command a plugin registers becomes an action, so menus, the palette,
// Python, MCP and Claude run it alike; its edits are one undo step.
#pragma once

#include "pxr/pxr.h"
#include "pxr/base/js/value.h"

#include <functional>
#include <string>
#include <vector>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

class ActionRegistry;

struct PluginCommand {
    std::string name;   // the action's name, e.g. "MyContainer.doThing"
    std::string title;  // its display name
    std::string description;
};

// A plugin's menu item: a command in a menu (path: menu, submenus...), or
// a separator.
struct PluginMenuEntry {
    std::vector<std::string> path;
    std::string command;
    std::string shortcut;  // usdview's form, e.g. "Ctrl+Shift+P"
    bool separator = false;
};

struct PluginReport {
    std::vector<std::string> plugins;  // loaded, by name
    std::vector<PluginCommand> commands;
    std::vector<PluginMenuEntry> menus;
    std::vector<std::string> errors;   // tracebacks, for the Log and Claude
    JsObject ToJson() const;
};
// Reads what vela._load_plugins returns.
PluginReport ParsePluginReport(const JsObject &json);

// ~/Library/Application Support/Vela/Plugins, or VELA_PLUGINS_FOLDER when
// set (tests). Plugins on PXR_PLUGINPATH_NAME are found as well.
std::string PluginsFolder();

// Whether any plugin declares a container, without starting Python (so
// launches without plugins don't import pxr).
bool HasPlugins();

// Loads plugins (again, reloading their modules, when reload is true):
// registers their commands as actions, replacing the previous ones, and
// tells the observer, which builds their menus.
PluginReport LoadPlugins(bool reload);
const PluginReport &CurrentPlugins();
void SetPluginsChangedCallback(std::function<void(const PluginReport &)> callback);

// A usdview shortcut as an action's key and Modifier bits: "Ctrl" is
// Command (as Qt has it on macOS), "Meta" Control, "Alt" Option. False if
// it can't be read.
bool ParsePluginShortcut(const std::string &shortcut, std::string *key, unsigned *modifiers);

// reload_plugins (Plugins menu).
void RegisterPluginActions(ActionRegistry &registry);

} // namespace vela
