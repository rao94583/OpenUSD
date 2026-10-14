#include "value_text.h"

#include "pxr/base/gf/vec2d.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec2i.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec3i.h"
#include "pxr/base/gf/vec4d.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/gf/vec4i.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/tf/token.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/sdf/types.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace vela {

namespace {

enum class Kind { None, Bool, Integer, Unsigned, Real, Text, Vector };

struct TypeInfo {
    Kind kind = Kind::None;
    int dimension = 0;     // vectors
    Kind element = Kind::None;
};

TypeInfo Info(const SdfValueTypeName &type) {
    if (type.IsArray()) return {};
    const VtValue model = type.GetDefaultValue();
    if (model.IsHolding<bool>()) return {Kind::Bool};
    if (model.IsHolding<int>() || model.IsHolding<int64_t>()) return {Kind::Integer};
    if (model.IsHolding<unsigned int>() || model.IsHolding<uint64_t>() ||
        model.IsHolding<unsigned char>()) return {Kind::Unsigned};
    if (model.IsHolding<float>() || model.IsHolding<double>()) return {Kind::Real};
    if (model.IsHolding<std::string>() || model.IsHolding<TfToken>() ||
        model.IsHolding<SdfAssetPath>()) return {Kind::Text};
    if (model.IsHolding<GfVec2f>() || model.IsHolding<GfVec2d>()) return {Kind::Vector, 2, Kind::Real};
    if (model.IsHolding<GfVec3f>() || model.IsHolding<GfVec3d>()) return {Kind::Vector, 3, Kind::Real};
    if (model.IsHolding<GfVec4f>() || model.IsHolding<GfVec4d>()) return {Kind::Vector, 4, Kind::Real};
    if (model.IsHolding<GfVec2i>()) return {Kind::Vector, 2, Kind::Integer};
    if (model.IsHolding<GfVec3i>()) return {Kind::Vector, 3, Kind::Integer};
    if (model.IsHolding<GfVec4i>()) return {Kind::Vector, 4, Kind::Integer};
    return {};
}

// The shortest text that reads back as the same value.
std::string Number(double value) {
    char buffer[64];
    for (int precision = 6; precision <= 17; ++precision) {
        std::snprintf(buffer, sizeof buffer, "%.*g", precision, value);
        if (std::strtod(buffer, nullptr) == value) break;
    }
    return buffer;
}
std::string Number(float value) {
    char buffer[64];
    for (int precision = 6; precision <= 9; ++precision) {
        std::snprintf(buffer, sizeof buffer, "%.*g", precision, double(value));
        if (std::strtof(buffer, nullptr) == value) break;
    }
    return buffer;
}
std::string Number(int value) { return std::to_string(value); }

template <class V>
std::string Vector(const V &v) {
    std::string text;
    for (size_t i = 0; i < V::dimension; ++i) {
        text += (i ? ", " : "") + Number(v[i]);
    }
    return text;
}

bool ParseNumber(const std::string &raw, Kind kind, JsValue *value, std::string *error) {
    const std::string text = TfStringTrim(raw);
    if (text.empty()) {
        *error = "A number is required";
        return false;
    }
    errno = 0;
    char *end = nullptr;
    if (kind == Kind::Integer) {
        const long long n = std::strtoll(text.c_str(), &end, 10);
        if (*end || errno == ERANGE) {
            *error = "Not an integer: " + text;
            return false;
        }
        *value = JsValue(int64_t(n));
    } else if (kind == Kind::Unsigned) {
        if (text[0] == '-') {
            *error = "Not a non-negative integer: " + text;
            return false;
        }
        const unsigned long long n = std::strtoull(text.c_str(), &end, 10);
        if (*end || errno == ERANGE) {
            *error = "Not a non-negative integer: " + text;
            return false;
        }
        *value = JsValue(uint64_t(n));
    } else {
        const double n = std::strtod(text.c_str(), &end);
        if (*end || !std::isfinite(n)) {
            *error = "Not a number: " + text;
            return false;
        }
        *value = JsValue(n);
    }
    return true;
}

} // namespace

bool IsTextEditable(const SdfValueTypeName &type) {
    return Info(type).kind != Kind::None;
}

std::string FormatForEditing(const VtValue &value) {
    if (value.IsHolding<bool>()) return value.UncheckedGet<bool>() ? "true" : "false";
    if (value.IsHolding<float>()) return Number(value.UncheckedGet<float>());
    if (value.IsHolding<double>()) return Number(value.UncheckedGet<double>());
    if (value.IsHolding<std::string>()) return value.UncheckedGet<std::string>();
    if (value.IsHolding<TfToken>()) return value.UncheckedGet<TfToken>().GetString();
    if (value.IsHolding<SdfAssetPath>()) return value.UncheckedGet<SdfAssetPath>().GetAssetPath();
    if (value.IsHolding<GfVec2f>()) return Vector(value.UncheckedGet<GfVec2f>());
    if (value.IsHolding<GfVec3f>()) return Vector(value.UncheckedGet<GfVec3f>());
    if (value.IsHolding<GfVec4f>()) return Vector(value.UncheckedGet<GfVec4f>());
    if (value.IsHolding<GfVec2d>()) return Vector(value.UncheckedGet<GfVec2d>());
    if (value.IsHolding<GfVec3d>()) return Vector(value.UncheckedGet<GfVec3d>());
    if (value.IsHolding<GfVec4d>()) return Vector(value.UncheckedGet<GfVec4d>());
    if (value.IsHolding<GfVec2i>()) return Vector(value.UncheckedGet<GfVec2i>());
    if (value.IsHolding<GfVec3i>()) return Vector(value.UncheckedGet<GfVec3i>());
    if (value.IsHolding<GfVec4i>()) return Vector(value.UncheckedGet<GfVec4i>());
    return value.IsEmpty() ? std::string() : TfStringify(value);
}

bool ParseEditedText(const std::string &text, const SdfValueTypeName &type,
                     JsValue *value, std::string *error) {
    const TypeInfo info = Info(type);
    switch (info.kind) {
    case Kind::None:
        *error = "Values of type " + type.GetAsToken().GetString() +
            " cannot be edited as text";
        return false;
    case Kind::Bool: {
        const std::string t = TfStringToLower(TfStringTrim(text));
        if (t == "true" || t == "1" || t == "yes") *value = JsValue(true);
        else if (t == "false" || t == "0" || t == "no") *value = JsValue(false);
        else {
            *error = "Not true or false: " + text;
            return false;
        }
        return true;
    }
    case Kind::Integer:
    case Kind::Unsigned:
    case Kind::Real:
        return ParseNumber(text, info.kind, value, error);
    case Kind::Text:
        *value = JsValue(text);
        return true;
    case Kind::Vector: {
        std::string t = TfStringTrim(text);
        if (t.size() >= 2 && t.front() == '(' && t.back() == ')') t = t.substr(1, t.size() - 2);
        for (char &c : t) if (c == ',') c = ' ';
        std::vector<std::string> parts = TfStringTokenize(t, " \t");
        if (int(parts.size()) != info.dimension) {
            *error = "Expected " + std::to_string(info.dimension) + " numbers: " + text;
            return false;
        }
        JsArray array;
        for (const std::string &part : parts) {
            JsValue element;
            if (!ParseNumber(part, info.element, &element, error)) return false;
            array.push_back(element);
        }
        *value = JsValue(array);
        return true;
    }
    }
    return false;
}

} // namespace vela
