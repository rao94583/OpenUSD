// The action registry: every operation the UI offers, registered once with
// a name, a JSON schema for its parameters and a handler. Menus, keyboard
// shortcuts, the command palette, Python and Claude all run actions by
// name, and the vela-ui skill and MCP tool schemas are generated from
// Describe().
#pragma once

#include "pxr/pxr.h"
#include "pxr/base/js/value.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

class Document;

// Modifier keys for shortcuts.
enum Modifier : unsigned {
    ModifierCommand = 1u << 0,
    ModifierShift = 1u << 1,
    ModifierOption = 1u << 2,
    ModifierControl = 1u << 3,
};

// Special keys, as their AppKit function key characters.
namespace Keys {
inline const std::string LeftArrow = "\xEF\x9C\x82";   // U+F702
inline const std::string RightArrow = "\xEF\x9C\x83";  // U+F703
}

struct ActionResult {
    bool ok = true;
    JsValue value;       // the action's result, null if it has none
    std::string error;   // when !ok

    static ActionResult Error(std::string message) {
        return ActionResult{false, JsValue(), std::move(message)};
    }
};

// One item of an action's submenu: the action run with fixed parameters.
struct MenuChoice {
    std::string title;
    JsObject params;
};

// A field of an action's form, filled in from a sheet; see Action::form.
struct FormField {
    enum class Type { Bool, Int, Float, String, Choice };
    std::string key;
    std::string label;
    Type type = Type::String;
    JsValue value;                     // the current value, shown first
    std::vector<std::string> choices;  // for Choice
    // For Choice, the values the choices stand for (the same count), when
    // they differ from the titles shown. value is one of these.
    std::vector<std::string> choiceValues;
};

struct Action {
    std::string name;         // stable identifier, e.g. "frame_selection"
    std::string title;        // menu title, e.g. "Frame Selected"
    std::string description;  // for the command palette, skills and MCP
    // A JSON schema for the parameters: an object schema with "properties"
    // and "required". The registry validates the supported subset (types,
    // enums, array item types, required properties) before running.
    JsObject parameters;

    // Menu placement and shortcut. Actions without a menu are still
    // available by name.
    std::string menu;
    std::string key;          // a character, or one of Keys
    unsigned modifiers = 0;   // Modifier bits
    bool separatorBefore = false;
    // When set, the menu shows a submenu titled with the action's title, an
    // item per choice, and a checkmark on chosen ones.
    std::vector<MenuChoice> choices;
    // Choices that depend on the document, e.g. its cameras; the menu asks
    // each time it opens.
    std::function<std::vector<MenuChoice>(const Document &)> dynamicChoices;
    std::function<bool(const Document &, const JsObject &params)> chosen;
    // When set, the menu item first asks for a file to write, with a save
    // panel, and passes its path in this parameter (after the form, if the
    // action has one). saveTypes are the file extensions it offers, the
    // first chosen first; empty offers USD files.
    std::string savePathParam;
    std::vector<std::string> saveTypes;
    // Likewise with an open panel, for a USD file to read.
    std::string openPathParam;
    // When set, the menu item opens a form with these fields and runs the
    // action with every field's value: as {formParam: {key: value, ...}},
    // or, without formParam, as the parameters themselves.
    std::function<std::vector<FormField>(const Document &)> form;
    std::string formParam;

    // Returns information without changing anything (e.g. get_settings):
    // the palette leaves it out, and tools can mark it read-only.
    bool query = false;

    // Edits made by the handler become one undo step named after the
    // title. Actions that only change the view leave undo alone.
    bool undoable = false;

    std::function<ActionResult(Document &, const JsObject &params)> run;
    // Whether the action can run now; defaults to always.
    std::function<bool(const Document &)> enabled;
    // A title that depends on state, e.g. "Undo Rename"; defaults to title.
    std::function<std::string(const Document &)> dynamicTitle;
};

class ActionRegistry {
public:
    // The registry with the standard actions; see standard_actions.cpp.
    static ActionRegistry &Get();

    // Adds an action, replacing one with the same name.
    void Register(Action action);
    const Action *Find(const std::string &name) const;
    // In registration order, which menus follow.
    const std::vector<Action> &GetActions() const { return _actions; }
    // The action bound to a key and modifiers, if any.
    const Action *FindShortcut(const std::string &key, unsigned modifiers) const;

    bool IsEnabled(const Action &action, const Document &document) const;
    std::string GetTitle(const Action &action, const Document &document) const;

    // Validates the parameters and runs the action.
    ActionResult Run(Document &document, const std::string &name,
                     const JsObject &params = {}) const;

    // Every action's name, title, description, parameter schema, menu and
    // shortcut, as a JSON array.
    JsValue Describe() const;

    // Checks params against an action's parameter schema; returns an error
    // message, or "" if they are valid.
    static std::string Validate(const JsObject &schema, const JsObject &params);

private:
    std::vector<Action> _actions;
};

// Builds a parameter schema from a JSON string, e.g.
// R"({"properties": {"time": {"type": "number"}}, "required": ["time"]})".
JsObject ParameterSchema(const std::string &json);

void RegisterStandardActions(ActionRegistry &registry);

} // namespace vela
