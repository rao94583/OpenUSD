#include "claude_history.h"
#include "mcp.h"

#include "pxr/base/js/json.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vela {
namespace claude {

namespace {

// Session ids are UUIDs; anything else could escape the folder.
bool ValidId(const std::string &id) {
    return !id.empty() && id.size() <= 64 &&
        std::all_of(id.begin(), id.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                   (c >= 'A' && c <= 'Z') || c == '-';
        });
}

std::string PathFor(const std::string &folder, const std::string &id) {
    return folder + "/" + id + ".jsonl";
}

bool MakeFolders(const std::string &path) {
    struct stat info;
    if (stat(path.c_str(), &info) == 0) return S_ISDIR(info.st_mode);
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0 && !MakeFolders(path.substr(0, slash))) {
        return false;
    }
    return mkdir(path.c_str(), 0700) == 0 || errno == EEXIST;
}

std::string Text(const JsObject &object, const char *key) {
    auto it = object.find(key);
    return it != object.end() && it->second.IsString() ? it->second.GetString() : std::string();
}

double Number(const JsObject &object, const char *key) {
    auto it = object.find(key);
    if (it == object.end()) return 0.0;
    if (it->second.IsReal()) return it->second.GetReal();
    if (it->second.IsUInt64()) return double(it->second.GetUInt64());
    if (it->second.IsInt()) return double(it->second.GetInt64());
    return 0.0;
}

} // namespace

std::string DefaultHistoryFolder() {
    if (const char *folder = getenv("VELA_CLAUDE_HISTORY"); folder && *folder) return folder;
    const char *home = getenv("HOME");
    return std::string(home ? home : "") + "/Library/Application Support/Vela/Claude";
}

std::string WorkspaceFolder() { return DefaultHistoryFolder() + "/Workspace"; }

bool CreateConversation(const std::string &folder, const Conversation &conversation,
                        std::string *error) {
    auto fail = [&](const std::string &message) {
        if (error) *error = message;
        return false;
    };
    if (!ValidId(conversation.id)) return fail("Invalid conversation id");
    if (!MakeFolders(folder)) return fail("Could not create " + folder);
    const std::string path = PathFor(folder, conversation.id);
    if (access(path.c_str(), F_OK) == 0) return true;
    std::ofstream out(path);
    if (!out) return fail("Could not write " + path);
    out << mcp::Compact(JsValue(JsObject{
        {"kind", JsValue("conversation")},
        {"title", JsValue(conversation.title)},
        {"document", JsValue(conversation.document)},
        {"directory", JsValue(conversation.directory)},
        {"created", JsValue(conversation.created)},
    })) << "\n";
    return bool(out);
}

bool AppendEntry(const std::string &folder, const std::string &id, const JsObject &entry) {
    if (!ValidId(id)) return false;
    std::ofstream out(PathFor(folder, id), std::ios::app);
    if (!out) return false;
    out << mcp::Compact(JsValue(entry)) << "\n";
    return bool(out);
}

std::vector<JsObject> LoadEntries(const std::string &folder, const std::string &id) {
    std::vector<JsObject> entries;
    if (!ValidId(id)) return entries;
    std::ifstream in(PathFor(folder, id));
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first) {
            first = false;
            continue;
        }
        const JsValue value = JsParseString(line);
        if (value.IsObject()) entries.push_back(value.GetJsObject());
    }
    return entries;
}

std::vector<Conversation> ListConversations(const std::string &folder, const std::string &document) {
    std::vector<Conversation> conversations;
    DIR *dir = opendir(folder.c_str());
    if (!dir) return conversations;
    while (dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() <= 6 || name.compare(name.size() - 6, 6, ".jsonl") != 0) continue;
        const std::string id = name.substr(0, name.size() - 6);
        if (!ValidId(id)) continue;
        const std::string path = PathFor(folder, id);
        std::ifstream in(path);
        std::string line;
        if (!std::getline(in, line)) continue;
        const JsValue value = JsParseString(line);
        if (!value.IsObject()) continue;
        const JsObject &header = value.GetJsObject();
        if (Text(header, "kind") != "conversation") continue;
        Conversation conversation;
        conversation.id = id;
        conversation.title = Text(header, "title");
        conversation.document = Text(header, "document");
        conversation.directory = Text(header, "directory");
        conversation.created = Number(header, "created");
        struct stat info;
        conversation.updated = stat(path.c_str(), &info) == 0
            ? double(info.st_mtimespec.tv_sec) + info.st_mtimespec.tv_nsec * 1e-9
            : conversation.created;
        if (!document.empty() && conversation.document != document) continue;
        conversations.push_back(conversation);
    }
    closedir(dir);
    std::sort(conversations.begin(), conversations.end(),
              [](const Conversation &a, const Conversation &b) { return a.updated > b.updated; });
    return conversations;
}

bool DeleteConversation(const std::string &folder, const std::string &id) {
    return ValidId(id) && unlink(PathFor(folder, id).c_str()) == 0;
}

std::string TitleFor(const std::string &request) {
    std::string title;
    bool space = false;
    for (char c : request) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            space = !title.empty();
            continue;
        }
        if (space) title += ' ';
        space = false;
        title += c;
    }
    constexpr size_t limit = 60;
    if (title.size() > limit) {
        size_t cut = title.rfind(' ', limit);
        if (cut == std::string::npos || cut < limit / 2) cut = limit;
        // Don't split a UTF-8 character.
        while (cut > 0 && (static_cast<unsigned char>(title[cut]) & 0xC0) == 0x80) --cut;
        title = title.substr(0, cut) + "…";
    }
    return title;
}

} // namespace claude
} // namespace vela
