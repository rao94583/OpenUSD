#include "packages.h"
#include "mcp.h"

#include "pxr/base/js/json.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#include <dirent.h>
#include <sys/stat.h>

namespace vela {
namespace packages {

namespace {

bool MakeFolders(const std::string &path) {
    struct stat info;
    if (stat(path.c_str(), &info) == 0) return S_ISDIR(info.st_mode);
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0 && !MakeFolders(path.substr(0, slash))) {
        return false;
    }
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

std::vector<std::string> Strings(const JsValue &value) {
    std::vector<std::string> strings;
    if (!value.IsArray()) return strings;
    for (const JsValue &item : value.GetJsArray()) {
        if (item.IsString()) strings.push_back(item.GetString());
    }
    return strings;
}

std::string Trim(const std::string &text) {
    const size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return std::string();
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

// PEP 503 normalization: lowercase, runs of -_. become -.
std::string Normalize(const std::string &name) {
    std::string out;
    for (char c : name) {
        if (c == '-' || c == '_' || c == '.') {
            if (!out.empty() && out.back() != '-') out += '-';
        } else {
            out += char(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

} // namespace

std::string BaseFolder(const std::string &pythonVersion) {
    if (const char *folder = getenv("VELA_PYTHON_PACKAGES"); folder && *folder) return folder;
    const char *home = getenv("HOME");
    return std::string(home ? home : "") + "/Library/Application Support/Vela/Python/" +
        pythonVersion;
}

std::string SitePackages(const std::string &base, const std::string &pythonVersion) {
    return base + "/lib/python" + pythonVersion + "/site-packages";
}

std::vector<std::string> Manifest::All() const {
    std::vector<std::string> all;
    std::set<std::string> seen;
    auto add = [&](const std::string &requirement) {
        if (seen.insert(requirement).second) all.push_back(requirement);
    };
    for (const auto &[plugin, requirements] : plugins) {
        for (const std::string &requirement : requirements) add(requirement);
    }
    for (const std::string &requirement : requested) add(requirement);
    return all;
}

JsObject Manifest::ToJson() const {
    JsObject pluginsJson;
    for (const auto &[plugin, requirements] : plugins) {
        JsArray list;
        for (const std::string &requirement : requirements) list.push_back(JsValue(requirement));
        pluginsJson[plugin] = JsValue(list);
    }
    JsArray requestedJson;
    for (const std::string &requirement : requested) requestedJson.push_back(JsValue(requirement));
    return JsObject{{"plugins", JsValue(pluginsJson)}, {"requested", JsValue(requestedJson)}};
}

Manifest ReadManifest(const std::string &base) {
    Manifest manifest;
    std::ifstream in(base + "/requirements.json");
    if (!in) return manifest;
    std::stringstream text;
    text << in.rdbuf();
    const JsValue json = JsParseString(text.str());
    if (!json.IsObject()) return manifest;
    const JsObject &object = json.GetJsObject();
    if (auto it = object.find("plugins"); it != object.end() && it->second.IsObject()) {
        for (const auto &[plugin, requirements] : it->second.GetJsObject()) {
            manifest.plugins[plugin] = Strings(requirements);
        }
    }
    if (auto it = object.find("requested"); it != object.end()) {
        manifest.requested = Strings(it->second);
    }
    return manifest;
}

bool WriteManifest(const std::string &base, const Manifest &manifest) {
    if (!MakeFolders(base)) return false;
    std::ofstream out(base + "/requirements.json");
    out << JsWriteToString(JsValue(manifest.ToJson())) << "\n";
    return bool(out);
}

std::vector<std::string> Constraints(const std::string &sitePackages) {
    std::vector<std::string> constraints;
    DIR *dir = opendir(sitePackages.c_str());
    if (!dir) return constraints;
    while (dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        const std::string suffix = ".dist-info";
        if (name.size() <= suffix.size() ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }
        // name-version.dist-info; names have no '-' (wheel names use '_').
        const std::string stem = name.substr(0, name.size() - suffix.size());
        const size_t dash = stem.find('-');
        if (dash == std::string::npos) continue;
        constraints.push_back(Normalize(stem.substr(0, dash)) + "==" + stem.substr(dash + 1));
    }
    closedir(dir);
    std::sort(constraints.begin(), constraints.end());
    return constraints;
}

std::string RequirementName(const std::string &requirement) {
    const std::string text = Trim(requirement);
    size_t end = 0;
    while (end < text.size() &&
           (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '-' ||
            text[end] == '_' || text[end] == '.')) {
        ++end;
    }
    return end ? Normalize(text.substr(0, end)) : std::string();
}

std::string CheckRequirement(const std::string &requirement) {
    const std::string text = Trim(requirement);
    if (text.empty()) return "An empty requirement";
    if (text[0] == '-') return "'" + text + "' is a pip option, not a package";
    if (text.find("://") != std::string::npos || text[0] == '/' || text[0] == '.' ||
        text.find(" @ ") != std::string::npos) {
        return "'" + text + "' is a path or URL; Vela installs packages from the package index";
    }
    const std::string name = RequirementName(text);
    if (name.empty()) return "'" + text + "' doesn't name a package";
    static const std::set<std::string> usd = {"usd-core", "openusd", "pxr"};
    if (usd.count(name)) {
        return "'" + name + "' would install a second USD; Vela's pxr is already available";
    }
    return std::string();
}

PipCommand InstallCommand(const std::string &python, const std::string &base,
                          const std::string &bundled, const std::string &constraintsFile,
                          const std::vector<std::string> &requirements,
                          const std::vector<std::string> &extra) {
    PipCommand command;
    command.args = {python, "-m", "pip", "install",
                    "--user",
                    "--upgrade-strategy", "only-if-needed",
                    "--prefer-binary",
                    "--no-warn-script-location",
                    "--disable-pip-version-check",
                    "--progress-bar", "off"};
    if (!constraintsFile.empty()) {
        command.args.push_back("--constraint");
        command.args.push_back(constraintsFile);
    }
    command.args.insert(command.args.end(), extra.begin(), extra.end());
    command.args.insert(command.args.end(), requirements.begin(), requirements.end());
    // A known environment: the user base, the bundle's packages visible (so
    // pip counts them as installed), nothing else of the app's Python. The
    // user's pip configuration (index, certificates, proxy) still applies.
    const char *home = getenv("HOME");
    command.environment = {
        "PYTHONUSERBASE=" + base,
        "PYTHONPATH=" + bundled,
        "PYTHONNOUSERSITE=",
        "PYTHONHOME=",
        "PIP_DISABLE_PIP_VERSION_CHECK=1",
        "PIP_NO_INPUT=1",
        "PYTHONUNBUFFERED=1",
        "LANG=en_US.UTF-8",
        std::string("HOME=") + (home ? home : ""),
    };
    return command;
}

std::vector<std::string> SplitArguments(const std::string &text) {
    std::vector<std::string> parts;
    std::string part;
    bool inPart = false;
    char quote = 0;
    for (char c : text) {
        if (quote) {
            if (c == quote) quote = 0;
            else part += c;
        } else if (c == '"' || c == '\'') {
            quote = c;
            inPart = true;
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (inPart) parts.push_back(part);
            part.clear();
            inPart = false;
        } else {
            part += c;
            inPart = true;
        }
    }
    if (inPart) parts.push_back(part);
    return parts;
}

} // namespace packages
} // namespace vela
