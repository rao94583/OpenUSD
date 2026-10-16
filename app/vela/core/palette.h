// The command palette's entries and search (⌥Space; see PLAN.md, "Command
// palette"). Entries come from the action registry: every action the menus
// offer, each choice of an action with choices as its own entry, and
// actions that ask for input with a form or a file panel. A query starting
// with "/" searches prims by path instead. The app adds its own commands
// (Settings…, Open…) and shows the panel.
#pragma once

#include "actions.h"

#include "pxr/pxr.h"
#include "pxr/base/js/value.h"

#include <string>
#include <vector>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

class Document;

struct PaletteEntry {
    std::string title;     // e.g. "Draw Mode › Wireframe"
    std::string menu;      // e.g. "View", or "" for actions not in a menu
    std::string shortcut;  // e.g. "⇧⌘Z"
    std::string action;    // the action to run
    JsObject params;       // its fixed parameters (a choice's)
    bool checked = false;  // the choice is the current one
    // Identifies the entry across runs, for recents: action and params.
    std::string Key() const;
};

// The registry's entries for the document, enabled ones only.
std::vector<PaletteEntry> PaletteEntries(const ActionRegistry &registry,
                                         const Document &document);

// Prims whose paths match query (which starts with "/"), best first, as
// entries that select and frame them (select_path).
std::vector<PaletteEntry> PrimEntries(const Document &document,
                                      const std::string &query, size_t limit);

// How well query matches text, case-insensitively: its characters in
// order, preferring word starts and runs. -1 when they are not all there.
// Anchored, the first letter must begin a word. Searches match each word of
// a query separately, anchored, in any order.
int FuzzyScore(const std::string &query, const std::string &text, bool anchored = false);

// The entries matching query, best first, at most limit. Recent entries
// (keys, most recent first) rank higher; with an empty query they come
// first and the rest follow in menu order.
std::vector<PaletteEntry> SearchPalette(const std::vector<PaletteEntry> &entries,
                                        const std::string &query,
                                        const std::vector<std::string> &recents,
                                        size_t limit);

// An action's shortcut as the menus show it, e.g. "⇧⌘Z", or "".
std::string FormatShortcut(const Action &action);

} // namespace vela
