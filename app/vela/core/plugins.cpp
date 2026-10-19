#include "plugins.h"
#include "actions.h"
#include "log.h"
#include "mcp.h"
#include "packages.h"
#include "python.h"
#include "session.h"

#include "pxr/base/js/json.h"
#include "pxr/base/plug/registry.h"
#include "pxr/base/tf/type.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <sys/stat.h>

namespace vela {

namespace {

PluginReport current;
std::function<void(const PluginReport &)> changed;
// The actions plugins added, removed on reload.
std::set<std::string> pluginActions;

std::string String(const JsObject &object, const char *key) {
    auto it = object.find(key);
    return it != object.end() && it->second.IsString() ? it->second.GetString() : std::string();
}

std::vector<std::string> Strings(const JsValue &value) {
    std::vector<std::string> strings;
    if (!value.IsArray()) return strings;
    for (const JsValue &item : value.GetJsArray()) {
        if (item.IsString()) strings.push_back(item.GetString());
    }
    return strings;
}

const JsValue &Field(const JsObject &object, const char *key) {
    static const JsValue none;
    auto it = object.find(key);
    return it == object.end() ? none : it->second;
}

bool IsDirectory(const std::string &path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// Registers the plugins folder's packages with USD (PXR_PLUGINPATH_NAME's
// are registered at startup).
void RegisterFolder() {
    const std::string folder = PluginsFolder();
    if (IsDirectory(folder)) PlugRegistry::GetInstance().RegisterPlugins(folder + "/*/");
}

// The document's id in the session, for running a command on it.
int SessionIdOf(const Document &document) {
    for (const SessionDocument &entry : Session::Get().GetDocuments()) {
        if (entry.document.get() == &document) return entry.id;
    }
    return 0;
}

void Register(const PluginReport &report) {
    ActionRegistry &registry = ActionRegistry::Get();
    for (const std::string &name : pluginActions) registry.Unregister(name);
    pluginActions.clear();
    for (const PluginCommand &command : report.commands) {
        if (registry.Find(command.name)) {
            Log::Get().Add(LogEntry::Level::Warning, "Plugins",
                           "Plugin command " + command.name + " has the name of a Vela action, "
                           "so it was left out.");
            continue;
        }
        Action action;
        action.name = command.name;
        action.title = command.title.empty() ? command.name : command.title;
        action.description = command.description.empty()
            ? "A plugin command." : command.description;
        action.undoable = true;
        // Its first menu, for the palette, and its shortcut.
        for (const PluginMenuEntry &entry : report.menus) {
            if (entry.command != command.name) continue;
            if (!entry.path.empty()) action.menu = entry.path.front();
            ParsePluginShortcut(entry.shortcut, &action.key, &action.modifiers);
            break;
        }
        const std::string name = command.name;
        action.run = [name](Document &document, const JsObject &) {
            const int id = SessionIdOf(document);
            if (!id) return ActionResult::Error("The document is not open");
            Session::ScopedCurrent scope(id);
            std::string result, error;
            if (!CallPython("_run_plugin_command",
                            mcp::Compact(JsValue(JsArray{JsValue(name), JsValue(id)})),
                            &result, &error)) {
                return ActionResult::Error(error);
            }
            // [ok, output, error]
            const JsValue reply = JsParseString(result);
            if (!reply.IsArray() || reply.GetJsArray().size() != 3) {
                return ActionResult::Error("The plugin command returned nothing readable");
            }
            const JsArray &parts = reply.GetJsArray();
            const std::string output = parts[1].IsString() ? parts[1].GetString() : "";
            if (!output.empty()) Log::Get().Add(LogEntry::Level::Info, name, output);
            if (!parts[0].IsBool() || !parts[0].GetBool()) {
                return ActionResult::Error(parts[2].IsString() ? parts[2].GetString()
                                                               : name + " failed");
            }
            return output.empty() ? ActionResult{} : ActionResult{true, JsValue(output), ""};
        };
        registry.Register(std::move(action));
        pluginActions.insert(command.name);
    }
}

} // namespace

std::vector<std::string> PluginReport::Missing() const {
    std::vector<std::string> missing;
    std::set<std::string> seen;
    for (const PluginWaiting &plugin : waiting) {
        for (const std::string &requirement : plugin.missing) {
            if (seen.insert(requirement).second) missing.push_back(requirement);
        }
    }
    return missing;
}

JsObject PluginReport::ToJson() const {
    JsArray pluginsJson, commandsJson, menusJson, errorsJson;
    for (const std::string &plugin : plugins) pluginsJson.push_back(JsValue(plugin));
    for (const PluginCommand &command : commands) {
        commandsJson.push_back(JsValue(JsObject{{"name", JsValue(command.name)},
                                                {"title", JsValue(command.title)},
                                                {"description", JsValue(command.description)}}));
    }
    for (const PluginMenuEntry &entry : menus) {
        JsArray path;
        for (const std::string &part : entry.path) path.push_back(JsValue(part));
        JsObject item{{"path", JsValue(path)}};
        if (entry.separator) item["separator"] = JsValue(true);
        else {
            item["command"] = JsValue(entry.command);
            if (!entry.shortcut.empty()) item["shortcut"] = JsValue(entry.shortcut);
        }
        menusJson.push_back(JsValue(item));
    }
    for (const std::string &error : errors) errorsJson.push_back(JsValue(error));
    JsObject requirementsJson;
    for (const auto &[plugin, list] : requirements) {
        JsArray items;
        for (const std::string &requirement : list) items.push_back(JsValue(requirement));
        requirementsJson[plugin] = JsValue(items);
    }
    JsArray waitingJson;
    for (const PluginWaiting &plugin : waiting) {
        JsArray missing;
        for (const std::string &requirement : plugin.missing) missing.push_back(JsValue(requirement));
        waitingJson.push_back(JsValue(JsObject{{"plugin", JsValue(plugin.plugin)},
                                               {"missing", JsValue(missing)}}));
    }
    return JsObject{{"plugins", JsValue(pluginsJson)}, {"commands", JsValue(commandsJson)},
                    {"menus", JsValue(menusJson)}, {"errors", JsValue(errorsJson)},
                    {"requirements", JsValue(requirementsJson)},
                    {"waiting", JsValue(waitingJson)}};
}

PluginReport ParsePluginReport(const JsObject &json) {
    PluginReport report;
    report.plugins = Strings(Field(json, "plugins"));
    report.errors = Strings(Field(json, "errors"));
    if (Field(json, "commands").IsArray()) {
        for (const JsValue &item : Field(json, "commands").GetJsArray()) {
            if (!item.IsObject()) continue;
            const JsObject &c = item.GetJsObject();
            if (String(c, "name").empty()) continue;
            report.commands.push_back({String(c, "name"), String(c, "title"),
                                       String(c, "description")});
        }
    }
    if (Field(json, "requirements").IsObject()) {
        for (const auto &[plugin, list] : Field(json, "requirements").GetJsObject()) {
            report.requirements[plugin] = Strings(list);
        }
    }
    if (Field(json, "waiting").IsArray()) {
        for (const JsValue &item : Field(json, "waiting").GetJsArray()) {
            if (!item.IsObject()) continue;
            const JsObject &w = item.GetJsObject();
            if (String(w, "plugin").empty()) continue;
            report.waiting.push_back({String(w, "plugin"), Strings(Field(w, "missing"))});
        }
    }
    if (Field(json, "menus").IsArray()) {
        for (const JsValue &item : Field(json, "menus").GetJsArray()) {
            if (!item.IsObject()) continue;
            const JsObject &m = item.GetJsObject();
            PluginMenuEntry entry;
            entry.path = Strings(Field(m, "path"));
            entry.command = String(m, "command");
            entry.shortcut = String(m, "shortcut");
            entry.separator = Field(m, "separator").IsBool() && Field(m, "separator").GetBool();
            if (entry.path.empty() || (!entry.separator && entry.command.empty())) continue;
            report.menus.push_back(entry);
        }
    }
    return report;
}

std::string PluginsFolder() {
    if (const char *folder = getenv("VELA_PLUGINS_FOLDER"); folder && *folder) return folder;
    const char *home = getenv("HOME");
    return std::string(home ? home : "") + "/Library/Application Support/Vela/Plugins";
}

bool HasPlugins() {
    RegisterFolder();
    const TfType base = TfType::FindByName("vela.PluginContainer");
    if (base.IsUnknown()) return false;
    std::set<TfType> derived;
    PlugRegistry::GetAllDerivedTypes(base, &derived);
    return !derived.empty();
}

PluginReport LoadPlugins(bool reload) {
    RegisterFolder();
    std::string result, error;
    PluginReport report;
    if (CallPython("_load_plugins",
                   mcp::Compact(JsValue(JsArray{JsValue(JsArray{JsValue(PluginsFolder())}),
                                                JsValue(reload)})),
                   &result, &error)) {
        const JsValue json = JsParseString(result);
        if (json.IsObject()) report = ParsePluginReport(json.GetJsObject());
        else report.errors.push_back("Plugins: an unreadable report from Python");
    } else {
        report.errors.push_back("Plugins could not load:\n" + error);
    }
    for (const std::string &message : report.errors) {
        Log::Get().Add(LogEntry::Level::Error, "Plugins", message);
    }
    for (const PluginWaiting &plugin : report.waiting) {
        std::string missing;
        for (const std::string &r : plugin.missing) missing += (missing.empty() ? "" : ", ") + r;
        Log::Get().Add(LogEntry::Level::Warning, "Plugins",
                       plugin.plugin + " waits for Python packages: " + missing);
    }
    // The manifest follows the plugins there are (removed ones drop out).
    if (IsPythonInitialized()) {
        const std::string base = packages::BaseFolder(PythonVersion());
        packages::Manifest manifest = packages::ReadManifest(base);
        manifest.plugins.clear();
        for (const auto &[plugin, list] : report.requirements) {
            if (!list.empty()) manifest.plugins[plugin] = list;
        }
        if (manifest.ToJson() != packages::ReadManifest(base).ToJson()) {
            packages::WriteManifest(base, manifest);
        }
    }
    if (!report.plugins.empty()) {
        std::string names;
        for (const std::string &name : report.plugins) names += (names.empty() ? "" : ", ") + name;
        Log::Get().Add(LogEntry::Level::Info, "Plugins", "Loaded " + names);
    }
    Register(report);
    current = report;
    if (changed) changed(current);
    return report;
}

const PluginReport &CurrentPlugins() { return current; }

void SetPluginsChangedCallback(std::function<void(const PluginReport &)> callback) {
    changed = std::move(callback);
}

bool ParsePluginShortcut(const std::string &shortcut, std::string *key, unsigned *modifiers) {
    if (shortcut.empty()) return false;
    unsigned bits = 0;
    std::string last;
    size_t start = 0;
    while (start <= shortcut.size()) {
        size_t plus = shortcut.find('+', start);
        // "Ctrl++" ends with the plus key.
        if (plus == start && plus + 1 == shortcut.size()) plus = std::string::npos;
        std::string part = shortcut.substr(start, plus == std::string::npos ? std::string::npos
                                                                           : plus - start);
        std::string lower = part;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (plus == std::string::npos) {
            last = part;
            break;
        }
        if (lower == "ctrl" || lower == "cmd" || lower == "command") bits |= ModifierCommand;
        else if (lower == "meta" || lower == "control") bits |= ModifierControl;
        else if (lower == "alt" || lower == "option") bits |= ModifierOption;
        else if (lower == "shift") bits |= ModifierShift;
        else return false;
        start = plus + 1;
    }
    if (last.size() != 1) return false;
    *key = std::string(1, char(std::tolower(static_cast<unsigned char>(last[0]))));
    *modifiers = bits;
    return true;
}

void RegisterPluginActions(ActionRegistry &registry) {
    Action reload;
    reload.name = "reload_plugins";
    reload.title = "Reload Plugins";
    reload.description =
        "Find plugins again (in the Plugins folder and on PXR_PLUGINPATH_NAME) and reload "
        "their Python modules, so new and changed plugins take effect. Returns the plugins "
        "loaded, their commands and menus, and any errors with tracebacks.";
    reload.menu = "Plugins";
    reload.run = [](Document &, const JsObject &) {
        if (!IsPythonInitialized()) return ActionResult::Error("Python is not available");
        return ActionResult{true, JsValue(LoadPlugins(true).ToJson()), ""};
    };
    registry.Register(std::move(reload));
}

} // namespace vela
