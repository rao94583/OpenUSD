// Plugins without Python: shortcuts, reports and the reload action. The
// loading itself is checked in a launched app (plugins_ui_test).
#include "plugins.h"
#include "actions.h"
#include "test_support.h"

#include "pxr/base/js/json.h"

using namespace vela;

namespace {

void TestShortcuts() {
    std::string key;
    unsigned modifiers = 0;
    CHECK(ParsePluginShortcut("Ctrl+Shift+P", &key, &modifiers));
    CHECK(key == "p" && modifiers == (ModifierCommand | ModifierShift));
    CHECK(ParsePluginShortcut("Meta+Alt+k", &key, &modifiers));
    CHECK(key == "k" && modifiers == (ModifierControl | ModifierOption));
    CHECK(ParsePluginShortcut("Ctrl++", &key, &modifiers) && key == "+" &&
          modifiers == ModifierCommand);
    CHECK(ParsePluginShortcut("X", &key, &modifiers) && key == "x" && modifiers == 0);
    CHECK(!ParsePluginShortcut("", &key, &modifiers));
    CHECK(!ParsePluginShortcut("Hyper+X", &key, &modifiers));
    CHECK(!ParsePluginShortcut("Ctrl+F12", &key, &modifiers));
}

void TestReports() {
    const JsValue json = JsParseString(R"j({
        "plugins": ["tools"],
        "commands": [{"name": "Tools.count", "title": "Count Prims", "description": "Counts"},
                     {"title": "nameless"}],
        "menus": [{"path": ["Tools"], "command": "Tools.count", "shortcut": "Ctrl+K"},
                  {"path": ["Tools", "More"], "separator": true},
                  {"path": [], "command": "Tools.count"}],
        "errors": ["Traceback ..."]})j");
    const PluginReport report = ParsePluginReport(json.GetJsObject());
    CHECK(report.plugins == std::vector<std::string>{"tools"});
    CHECK(report.commands.size() == 1 && report.commands[0].title == "Count Prims");
    CHECK(report.menus.size() == 2);
    CHECK(report.menus[0].shortcut == "Ctrl+K" && !report.menus[0].separator);
    CHECK(report.menus[1].separator && report.menus[1].path.size() == 2);
    CHECK(report.errors.size() == 1);
    // Round trip.
    const PluginReport again = ParsePluginReport(report.ToJson());
    CHECK(again.commands.size() == 1 && again.menus.size() == 2 && again.errors == report.errors);
}

void TestActions() {
    const Action *reload = ActionRegistry::Get().Find("reload_plugins");
    CHECK(reload && reload->menu == "Plugins" && !reload->query);
    // Removing actions, as reloading does.
    ActionRegistry registry;
    Action action;
    action.name = "temporary";
    action.run = [](Document &, const JsObject &) { return ActionResult{}; };
    registry.Register(action);
    CHECK(registry.Find("temporary"));
    CHECK(registry.Unregister("temporary") && !registry.Find("temporary"));
    CHECK(!registry.Unregister("temporary"));
}

} // namespace

int main() {
    TestShortcuts();
    TestReports();
    TestActions();
    return test::Finish("plugins");
}
