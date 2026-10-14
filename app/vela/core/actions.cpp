#include "actions.h"
#include "document.h"

#include "pxr/base/js/json.h"
#include "pxr/base/tf/diagnostic.h"

#include <algorithm>

namespace vela {

namespace {

bool IsNumber(const JsValue &value) {
    return value.IsReal() || value.IsInt() || value.IsUInt64();
}

// Checks a value against a property schema: "type", "enum" and "items".
std::string ValidateValue(const std::string &name, const JsObject &schema,
                          const JsValue &value) {
    auto type = schema.find("type");
    if (type != schema.end() && type->second.IsString()) {
        const std::string &t = type->second.GetString();
        const bool ok =
            (t == "string" && value.IsString()) ||
            (t == "number" && IsNumber(value)) ||
            (t == "integer" && (value.IsInt() || value.IsUInt64())) ||
            (t == "boolean" && value.IsBool()) ||
            (t == "array" && value.IsArray()) ||
            (t == "object" && value.IsObject());
        if (!ok) {
            return "Parameter '" + name + "' must be of type " + t;
        }
    }
    auto choices = schema.find("enum");
    if (choices != schema.end() && choices->second.IsArray()) {
        const JsArray &allowed = choices->second.GetJsArray();
        if (std::find(allowed.begin(), allowed.end(), value) == allowed.end()) {
            return "Parameter '" + name + "' must be one of " +
                JsWriteToString(choices->second);
        }
    }
    auto items = schema.find("items");
    if (items != schema.end() && items->second.IsObject() && value.IsArray()) {
        const JsArray &array = value.GetJsArray();
        for (size_t i = 0; i < array.size(); ++i) {
            std::string error = ValidateValue(
                name + "[" + std::to_string(i) + "]",
                items->second.GetJsObject(), array[i]);
            if (!error.empty()) return error;
        }
    }
    if (value.IsArray()) {
        const size_t size = value.GetJsArray().size();
        auto least = schema.find("minItems");
        auto most = schema.find("maxItems");
        if ((least != schema.end() && least->second.IsInt() &&
             size < size_t(least->second.GetInt())) ||
            (most != schema.end() && most->second.IsInt() &&
             size > size_t(most->second.GetInt()))) {
            return "Parameter '" + name + "' has the wrong number of items";
        }
    }
    return std::string();
}

} // namespace

JsObject ParameterSchema(const std::string &json) {
    JsParseError error;
    JsValue value = JsParseString(json, &error);
    if (!value.IsObject()) {
        TF_CODING_ERROR("Invalid parameter schema at line %u: %s",
                        error.line, json.c_str());
        return JsObject();
    }
    JsObject schema = value.GetJsObject();
    schema.emplace("type", JsValue("object"));
    return schema;
}

std::string ActionRegistry::Validate(const JsObject &schema, const JsObject &params) {
    JsObject properties;
    if (auto p = schema.find("properties"); p != schema.end() && p->second.IsObject()) {
        properties = p->second.GetJsObject();
    }
    if (auto r = schema.find("required"); r != schema.end() && r->second.IsArray()) {
        for (const JsValue &name : r->second.GetJsArray()) {
            if (name.IsString() && !params.count(name.GetString())) {
                return "Missing parameter '" + name.GetString() + "'";
            }
        }
    }
    for (const auto &[name, value] : params) {
        auto property = properties.find(name);
        if (property == properties.end()) {
            return "Unknown parameter '" + name + "'";
        }
        if (property->second.IsObject()) {
            std::string error = ValidateValue(name, property->second.GetJsObject(), value);
            if (!error.empty()) return error;
        }
    }
    return std::string();
}

ActionRegistry &ActionRegistry::Get() {
    static ActionRegistry *registry = [] {
        auto *r = new ActionRegistry;
        RegisterStandardActions(*r);
        return r;
    }();
    return *registry;
}

void ActionRegistry::Register(Action action) {
    if (!action.run) {
        TF_CODING_ERROR("Action '%s' has no handler", action.name.c_str());
        return;
    }
    if (action.parameters.empty()) {
        action.parameters = ParameterSchema("{}");
    }
    for (Action &existing : _actions) {
        if (existing.name == action.name) {
            existing = std::move(action);
            return;
        }
    }
    _actions.push_back(std::move(action));
}

const Action *ActionRegistry::Find(const std::string &name) const {
    for (const Action &action : _actions) {
        if (action.name == name) return &action;
    }
    return nullptr;
}

const Action *ActionRegistry::FindShortcut(const std::string &key,
                                           unsigned modifiers) const {
    for (const Action &action : _actions) {
        if (!action.key.empty() && action.key == key && action.modifiers == modifiers) {
            return &action;
        }
    }
    return nullptr;
}

bool ActionRegistry::IsEnabled(const Action &action, const Document &document) const {
    return !action.enabled || action.enabled(document);
}

std::string ActionRegistry::GetTitle(const Action &action,
                                     const Document &document) const {
    return action.dynamicTitle ? action.dynamicTitle(document) : action.title;
}

ActionResult ActionRegistry::Run(Document &document, const std::string &name,
                                 const JsObject &params) const {
    const Action *action = Find(name);
    if (!action) {
        return ActionResult::Error("Unknown action '" + name + "'");
    }
    std::string error = Validate(action->parameters, params);
    if (!error.empty()) {
        return ActionResult::Error(name + ": " + error);
    }
    if (!IsEnabled(*action, document)) {
        return ActionResult::Error(name + " is not available now");
    }
    if (action->undoable) {
        UndoBlock block(document.GetUndoStack(), action->title);
        return action->run(document, params);
    }
    return action->run(document, params);
}

JsValue ActionRegistry::Describe() const {
    JsArray result;
    for (const Action &action : _actions) {
        JsObject entry{
            {"name", JsValue(action.name)},
            {"title", JsValue(action.title)},
            {"description", JsValue(action.description)},
            {"parameters", JsValue(action.parameters)},
            {"undoable", JsValue(action.undoable)},
        };
        if (!action.menu.empty()) entry["menu"] = JsValue(action.menu);
        if (!action.choices.empty()) {
            JsArray choices;
            for (const MenuChoice &choice : action.choices) {
                choices.push_back(JsValue(JsObject{
                    {"title", JsValue(choice.title)},
                    {"params", JsValue(choice.params)}}));
            }
            entry["choices"] = JsValue(choices);
        }
        if (!action.key.empty()) {
            std::string shortcut;
            if (action.modifiers & ModifierControl) shortcut += "Control+";
            if (action.modifiers & ModifierOption) shortcut += "Option+";
            if (action.modifiers & ModifierShift) shortcut += "Shift+";
            if (action.modifiers & ModifierCommand) shortcut += "Command+";
            if (action.key == Keys::LeftArrow) shortcut += "Left";
            else if (action.key == Keys::RightArrow) shortcut += "Right";
            else if (action.key == " ") shortcut += "Space";
            else {
                std::string key = action.key;
                std::transform(key.begin(), key.end(), key.begin(), ::toupper);
                shortcut += key;
            }
            entry["shortcut"] = JsValue(shortcut);
        }
        result.push_back(JsValue(entry));
    }
    return JsValue(result);
}

} // namespace vela
