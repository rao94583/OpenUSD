#include "palette.h"
#include "document.h"

#include "pxr/base/js/json.h"
#include "pxr/usd/usd/primRange.h"

#include <algorithm>
#include <cctype>

namespace vela {

namespace {

// Whether the palette can run the action: it changes something, and it
// needs no parameters, or the menu supplies them (choices, a form or a
// file panel).
bool Runnable(const Action &action) {
    if (action.query) return false;
    if (!action.choices.empty() || action.dynamicChoices || action.form ||
        !action.savePathParam.empty() || !action.openPathParam.empty()) {
        return true;
    }
    auto required = action.parameters.find("required");
    return required == action.parameters.end() || !required->second.IsArray() ||
        required->second.GetJsArray().empty();
}

// What a query is matched against: the menu, the title, and the action's
// name in words ("set draw mode").
std::string SearchText(const PaletteEntry &entry) {
    std::string name = entry.action;
    std::replace(name.begin(), name.end(), '_', ' ');
    return entry.menu + " " + entry.title + " " + name;
}

bool IsWordStart(const std::string &text, size_t i) {
    if (i == 0) return true;
    const unsigned char previous = text[i - 1], current = text[i];
    return !std::isalnum(previous) ||
        (std::islower(previous) && std::isupper(current));
}

// Each word of the query matches on its own, in any order ("points draw
// mode" finds Draw Mode › Points); -1 unless all do.
int WordsScore(const std::vector<std::string> &words, const std::string &text) {
    int total = 0;
    for (const std::string &word : words) {
        const int score = FuzzyScore(word, text);
        if (score < 0) return -1;
        total += score;
    }
    return total;
}

std::vector<std::string> Words(const std::string &query) {
    std::vector<std::string> words;
    std::string word;
    for (const char c : query + " ") {
        if (c != ' ') { word += c; continue; }
        if (!word.empty()) words.push_back(word);
        word.clear();
    }
    return words;
}

} // namespace

std::string PaletteEntry::Key() const {
    return action + " " + JsWriteToString(JsValue(params));
}

std::string FormatShortcut(const Action &action) {
    if (action.key.empty()) return std::string();
    std::string text;
    if (action.modifiers & ModifierControl) text += "⌃";
    if (action.modifiers & ModifierOption) text += "⌥";
    if (action.modifiers & ModifierShift) text += "⇧";
    if (action.modifiers & ModifierCommand) text += "⌘";
    if (action.key == Keys::LeftArrow) return text + "←";
    if (action.key == Keys::RightArrow) return text + "→";
    if (action.key == " ") return text + "Space";
    std::string key = action.key;
    for (char &c : key) c = char(std::toupper(static_cast<unsigned char>(c)));
    return text + key;
}

std::vector<PaletteEntry> PaletteEntries(const ActionRegistry &registry,
                                         const Document &document) {
    std::vector<PaletteEntry> entries;
    for (const Action &action : registry.GetActions()) {
        if (!Runnable(action) || !registry.IsEnabled(action, document)) continue;
        const std::string title = registry.GetTitle(action, document);
        std::vector<MenuChoice> choices = action.choices;
        if (action.dynamicChoices) choices = action.dynamicChoices(document);
        if (choices.empty() && (action.dynamicChoices || !action.choices.empty())) continue;
        if (choices.empty()) {
            entries.push_back({title, action.menu, FormatShortcut(action), action.name});
            continue;
        }
        for (const MenuChoice &choice : choices) {
            PaletteEntry entry{title + " › " + choice.title, action.menu, "",
                               action.name, choice.params};
            entry.checked = action.chosen && action.chosen(document, choice.params);
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

int FuzzyScore(const std::string &query, const std::string &text) {
    int score = 0;
    size_t at = 0;
    bool run = false;
    for (const char q : query) {
        if (q == ' ') continue;
        const char lower = char(std::tolower(static_cast<unsigned char>(q)));
        size_t found = std::string::npos;
        // Prefer the next word start with this letter, then any match.
        for (size_t i = at; i < text.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(text[i])) != lower) continue;
            if (run && i == at) { found = i; break; }
            if (IsWordStart(text, i)) { found = i; break; }
            if (found == std::string::npos) found = i;
        }
        if (found == std::string::npos) return -1;
        const bool contiguous = run && found == at;
        score += contiguous ? 5 : IsWordStart(text, found) ? 8 : 1;
        if (!contiguous) score -= int(std::min<size_t>(found - at, 10));
        run = true;
        at = found + 1;
    }
    return score;
}

std::vector<PaletteEntry> SearchPalette(const std::vector<PaletteEntry> &entries,
                                        const std::string &query,
                                        const std::vector<std::string> &recents,
                                        size_t limit) {
    auto recency = [&](const PaletteEntry &entry) {
        auto found = std::find(recents.begin(), recents.end(), entry.Key());
        return found == recents.end() ? -1 : int(recents.end() - found);
    };
    const std::vector<std::string> words = Words(query);
    std::vector<std::pair<int, size_t>> ranked;  // (score, index)
    for (size_t i = 0; i < entries.size(); ++i) {
        const int recent = recency(entries[i]);
        int score = 0;
        if (!words.empty()) {
            score = WordsScore(words, SearchText(entries[i]));
            if (score < 0) continue;
            // Matching the title alone beats matching across menu and title.
            const int title = WordsScore(words, entries[i].title);
            if (title >= 0) score = std::max(score, title + 4);
        }
        if (recent > 0) score += 1000 + recent;
        ranked.emplace_back(score, i);
    }
    // Best first; ties keep menu order.
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<PaletteEntry> result;
    for (const auto &[score, index] : ranked) {
        if (result.size() == limit) break;
        result.push_back(entries[index]);
    }
    return result;
}

std::vector<PaletteEntry> PrimEntries(const Document &document,
                                      const std::string &query, size_t limit) {
    std::vector<std::pair<int, std::string>> ranked;
    for (const UsdPrim &prim : document.GetStage()->Traverse()) {
        const std::string path = prim.GetPath().GetString();
        const int score = FuzzyScore(query, path);
        if (score < 0) continue;
        // Shorter paths first among equals.
        ranked.emplace_back(score * 1000 - int(std::min<size_t>(path.size(), 999)), path);
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<PaletteEntry> entries;
    for (const auto &[score, path] : ranked) {
        if (entries.size() == limit) break;
        entries.push_back({path, "Select and Frame", "", "select_path",
                           JsObject{{"path", JsValue(path)}}});
    }
    return entries;
}

} // namespace vela
