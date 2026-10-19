#include "claude_skills.h"

#include "claude_history.h"

#include "pxr/base/js/json.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unistd.h>

namespace vela::claude {

namespace {

namespace fs = std::filesystem;

// A parameter's type in a few words: "string", "one of a, b", "number[3]".
std::string TypeText(const JsObject &schema) {
    if (auto values = schema.find("enum"); values != schema.end() && values->second.IsArray()) {
        std::string text = "one of ";
        bool first = true;
        for (const JsValue &value : values->second.GetJsArray()) {
            if (!first) text += ", ";
            first = false;
            text += value.IsString() ? value.GetString() : JsWriteToString(value);
        }
        return text;
    }
    auto type = schema.find("type");
    if (type == schema.end() || !type->second.IsString()) return "any";
    std::string text = type->second.GetString();
    if (text == "array") {
        auto items = schema.find("items");
        text = items != schema.end() && items->second.IsObject()
            ? TypeText(items->second.GetJsObject()) : std::string("any");
        auto count = schema.find("minItems");
        auto most = schema.find("maxItems");
        if (count != schema.end() && most != schema.end() &&
            JsWriteToString(count->second) == JsWriteToString(most->second)) {
            text += "[" + JsWriteToString(count->second) + "]";
        } else {
            text += "[]";
        }
    }
    return text;
}

std::string Parameters(const JsObject &schema) {
    auto properties = schema.find("properties");
    if (properties == schema.end() || !properties->second.IsObject() ||
        properties->second.GetJsObject().empty()) {
        return "";
    }
    std::set<std::string> required;
    if (auto list = schema.find("required"); list != schema.end() && list->second.IsArray()) {
        for (const JsValue &name : list->second.GetJsArray()) {
            if (name.IsString()) required.insert(name.GetString());
        }
    }
    std::string text = "\n";
    for (const auto &[name, value] : properties->second.GetJsObject()) {
        const JsObject property = value.IsObject() ? value.GetJsObject() : JsObject();
        text += "- `" + name + "` (" + TypeText(property) +
            (required.count(name) ? ", required" : "") + ")";
        if (auto about = property.find("description");
            about != property.end() && about->second.IsString()) {
            text += ": " + about->second.GetString();
        }
        text += "\n";
    }
    return text;
}

std::string Shortcut(const JsObject &entry) {
    auto shortcut = entry.find("shortcut");
    return shortcut != entry.end() && shortcut->second.IsString()
        ? " (" + shortcut->second.GetString() + ")" : "";
}

bool WriteFile(const fs::path &path, const std::string &text, std::string *error) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    if (!out) {
        *error = "Couldn't write " + path.string();
        return false;
    }
    return true;
}

} // namespace

std::string ActionsReference(const ActionRegistry &registry) {
    // In registration order within each menu, menus as they first appear.
    std::vector<std::string> menus;
    std::map<std::string, std::vector<std::string>> sections;
    const JsValue described = registry.Describe();
    for (const JsValue &value : described.GetJsArray()) {
        const JsObject &entry = value.GetJsObject();
        auto field = [&](const char *key) {
            auto it = entry.find(key);
            return it != entry.end() && it->second.IsString() ? it->second.GetString() : std::string();
        };
        auto flag = [&](const char *key) {
            auto it = entry.find(key);
            return it != entry.end() && it->second.IsBool() && it->second.GetBool();
        };
        const std::string name = field("name");
        const Action *action = registry.Find(name);
        std::string menu = field("menu");
        if (menu.empty()) menu = "Scripting only (no menu item)";
        if (!sections.count(menu)) menus.push_back(menu);

        std::vector<std::string> marks;
        if (flag("query")) marks.push_back("query");
        if (flag("undoable")) marks.push_back("edits the scene, undoable");
        if (action && action->asksUser) marks.push_back("asks the user");
        // A heading per action, as the schema reference has per schema, so
        // `^### name` finds one.
        std::string text = "\n### " + name + "\n\n" + field("title") + Shortcut(entry);
        if (!marks.empty()) {
            text += " *[";
            for (size_t i = 0; i < marks.size(); ++i) text += (i ? "; " : "") + marks[i];
            text += "]*";
        }
        text += "\n";
        if (const std::string about = field("description"); !about.empty()) text += "\n" + about + "\n";
        auto parameters = entry.find("parameters");
        if (parameters != entry.end() && parameters->second.IsObject()) {
            text += Parameters(parameters->second.GetJsObject());
        }
        sections[menu].push_back(text);
    }

    std::string text =
        "# Vela's actions\n\n"
        "Generated by this copy of Vela when the panel started, so it includes "
        "the user's plugins' commands. Run one with `ui_action` (`action`, "
        "`params`) or `vela.run(name, **params)` in Python. *query* actions "
        "only return information; *undoable* ones edit the scene as one undo "
        "step; the rest change the app or the view.\n";
    for (const std::string &menu : menus) {
        text += "\n## " + menu + "\n";
        for (const std::string &entry : sections[menu]) text += entry;
    }
    return text;
}

bool InstallSkills(const std::string &source, const std::string &destination,
                   const ActionRegistry &registry, const SkillOptions &options,
                   std::string *error) {
    std::error_code failure;
    if (!fs::is_directory(fs::path(source) / ".claude-plugin", failure)) {
        *error = "Vela's Claude skills are missing from " + source;
        return false;
    }
    // Built beside the destination, then swapped in, so a CLI starting from
    // another window never sees a half-written folder.
    const fs::path target(destination);
    const fs::path staging = target.string() + ".new-" + std::to_string(getpid());
    const fs::path previous = target.string() + ".old-" + std::to_string(getpid());
    fs::remove_all(staging, failure);
    fs::create_directories(staging.parent_path(), failure);
    fs::copy(source, staging, fs::copy_options::recursive, failure);
    if (failure) {
        *error = "Couldn't copy Vela's Claude skills: " + failure.message();
        return false;
    }
    // The bundle's files are read-only; the copy is the app's to replace.
    for (const auto &entry : fs::recursive_directory_iterator(staging, failure)) {
        fs::permissions(entry.path(), fs::perms::owner_write, fs::perm_options::add, failure);
    }
    if (!options.imageGeneration) fs::remove_all(staging / "skills/image-generation", failure);

    const fs::path plugins = staging / "skills/plugins/SKILL.md";
    if (fs::exists(plugins, failure)) {
        std::ifstream in(plugins, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        std::string text = buffer.str();
        static const std::string marker = "PLUGINS_FOLDER";
        for (size_t at; (at = text.find(marker)) != std::string::npos;) {
            text.replace(at, marker.size(), options.pluginsFolder);
        }
        if (!WriteFile(plugins, text, error)) return false;
    }

    const fs::path references = staging / "skills/ui/references";
    fs::create_directories(references, failure);
    if (!WriteFile(references / "actions.md", ActionsReference(registry), error)) return false;

    fs::remove_all(previous, failure);
    if (fs::exists(target, failure)) fs::rename(target, previous, failure);
    fs::rename(staging, target, failure);
    if (failure) {
        *error = "Couldn't install Vela's Claude skills in " + destination + ": " + failure.message();
        return false;
    }
    fs::remove_all(previous, failure);
    return true;
}

std::string SkillsFolder() { return DefaultHistoryFolder() + "/Skills"; }

} // namespace vela::claude
