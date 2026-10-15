#include "palette.h"
#include "document.h"
#include "test_support.h"

#include "pxr/usd/usdGeom/cube.h"

#include <algorithm>

using namespace vela;

namespace {

const PaletteEntry *Find(const std::vector<PaletteEntry> &entries, const std::string &title) {
    for (const PaletteEntry &entry : entries) {
        if (entry.title == title) return &entry;
    }
    return nullptr;
}

void TestFuzzy() {
    CHECK(FuzzyScore("", "anything") == 0);
    CHECK(FuzzyScore("dm", "Draw Mode") > 0);
    CHECK(FuzzyScore("xyz", "Draw Mode") == -1);
    // Word starts and runs beat scattered letters.
    CHECK(FuzzyScore("wire", "Wireframe") > FuzzyScore("wire", "Show Interiors Repeatedly"));
    CHECK(FuzzyScore("fs", "Frame Selected") > FuzzyScore("fs", "Default Material"));
    CHECK(FuzzyScore("DRAW", "draw mode") > 0);  // case-insensitive
}

void TestEntries() {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomCube::Define(stage, SdfPath("/World/Box"));
    UsdGeomCube::Define(stage, SdfPath("/World/Ball"));
    UsdGeomCube::Define(stage, SdfPath("/Other/Box"));
    Document d(stage);
    const ActionRegistry &registry = ActionRegistry::Get();
    const std::vector<PaletteEntry> entries = PaletteEntries(registry, d);

    // Plain actions with their shortcuts; choices as entries with the
    // current one checked; actions asking for input.
    const PaletteEntry *frame = Find(entries, "Frame Selected");
    CHECK(frame && frame->menu == "View" && frame->shortcut == "F");
    const PaletteEntry *wire = Find(entries, "Draw Mode › Wireframe");
    CHECK(wire && !wire->checked && wire->params.at("mode") == JsValue("wireframe"));
    CHECK(Find(entries, "Draw Mode › Smooth Shaded")->checked);
    CHECK(Find(entries, "Select Path…"));
    CHECK(Find(entries, "Reset Settings"));  // no menu, no parameters
    // Left out: disabled actions, queries, actions needing parameters the
    // palette cannot ask for.
    CHECK(!Find(entries, "Undo") && !Find(entries, "Select None"));
    CHECK(!Find(entries, "Settings") && !Find(entries, "Renderer Info"));
    CHECK(!Find(entries, "Set Time") && !Find(entries, "Select"));
    // Dynamic choices with nothing to offer yet (no renderer info) add none.
    CHECK(std::none_of(entries.begin(), entries.end(), [](const PaletteEntry &e) {
        return e.action == "set_renderer";
    }));

    // Search: by title, by menu and title, by action name.
    std::vector<PaletteEntry> found = SearchPalette(entries, "wirefr", {}, 5);
    CHECK(!found.empty() && found[0].title == "Draw Mode › Wireframe");
    found = SearchPalette(entries, "view frame all", {}, 5);
    CHECK(!found.empty() && found[0].title == "Frame All");
    found = SearchPalette(entries, "set draw mode points", {}, 5);
    CHECK(!found.empty() && found[0].title == "Draw Mode › Points");
    found = SearchPalette(entries, "points draw", {}, 5);  // any word order
    CHECK(!found.empty() && found[0].title == "Draw Mode › Points");
    CHECK(SearchPalette(entries, "qqqzzz", {}, 5).empty());
    // Recents first, most recent first, with or without a query.
    const std::vector<std::string> recents{Find(entries, "Frame All")->Key(), wire->Key()};
    found = SearchPalette(entries, "", recents, 3);
    CHECK(found.size() == 3 && found[0].title == "Frame All" &&
          found[1].title == "Draw Mode › Wireframe");
    found = SearchPalette(entries, "frame", recents, 2);
    CHECK(found[0].title == "Frame All");
    CHECK(SearchPalette(entries, "", {}, 1000).size() == entries.size());

    // Prims by path, best and shortest first, limited.
    found = PrimEntries(d, "/box", 10);
    CHECK(found.size() == 2 && found[0].action == "select_path");
    CHECK(found[0].params.at("path") == JsValue("/World/Box") ||
          found[0].params.at("path") == JsValue("/Other/Box"));
    CHECK(PrimEntries(d, "/World", 1).size() == 1);
    CHECK(PrimEntries(d, "/nothing", 10).empty());
}

void TestShortcuts() {
    Action a;
    CHECK(FormatShortcut(a).empty());
    a.key = "z";
    a.modifiers = ModifierCommand | ModifierShift;
    CHECK(FormatShortcut(a) == "⇧⌘Z");
    a.key = Keys::RightArrow;
    a.modifiers = 0;
    CHECK(FormatShortcut(a) == "→");
}

} // namespace

int main() {
    test::RegisterPlugins();
    TestFuzzy();
    TestEntries();
    TestShortcuts();
    return test::Finish("palette");
}
