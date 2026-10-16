#include "value_text.h"

#include "pxr/base/gf/half.h"
#include "pxr/base/gf/matrix2d.h"
#include "pxr/base/gf/matrix3d.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/quatd.h"
#include "pxr/base/gf/quatf.h"
#include "pxr/base/gf/quath.h"
#include "pxr/base/gf/traits.h"
#include "pxr/base/gf/vec2d.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec2h.h"
#include "pxr/base/gf/vec2i.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec3h.h"
#include "pxr/base/gf/vec3i.h"
#include "pxr/base/gf/vec4d.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/gf/vec4h.h"
#include "pxr/base/gf/vec4i.h"
#include "pxr/base/js/json.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/array.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/sdf/timeCode.h"
#include "pxr/usd/sdf/types.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace vela {

namespace {

enum class Kind { None, Bool, Integer, Unsigned, Real, Text };

// A type's values as text: an element kind, a number of components (1 for
// scalars; vectors, quaternions and matrices have more), matrix rows, and
// whether it is an array of those.
struct TypeInfo {
    Kind kind = Kind::None;
    int count = 1;
    int rows = 0;
    bool array = false;
};

TypeInfo Info(const SdfValueTypeName &type) {
    const VtValue model = type.GetScalarType().GetDefaultValue();
    TypeInfo info;
    info.array = type.IsArray();
    auto is = [&](auto... types) { return (model.IsHolding<decltype(types)>() || ...); };
    if (is(bool())) info.kind = Kind::Bool;
    else if (is(int(), int64_t())) info.kind = Kind::Integer;
    else if (is((unsigned int)0, uint64_t(), (unsigned char)0)) info.kind = Kind::Unsigned;
    else if (is(float(), double(), GfHalf(), SdfTimeCode())) info.kind = Kind::Real;
    else if (is(std::string(), TfToken(), SdfAssetPath())) info.kind = Kind::Text;
    else if (is(GfVec2f(), GfVec2d(), GfVec2h())) info = {Kind::Real, 2, 0, info.array};
    else if (is(GfVec3f(), GfVec3d(), GfVec3h())) info = {Kind::Real, 3, 0, info.array};
    else if (is(GfVec4f(), GfVec4d(), GfVec4h())) info = {Kind::Real, 4, 0, info.array};
    else if (is(GfVec2i())) info = {Kind::Integer, 2, 0, info.array};
    else if (is(GfVec3i())) info = {Kind::Integer, 3, 0, info.array};
    else if (is(GfVec4i())) info = {Kind::Integer, 4, 0, info.array};
    else if (is(GfQuatf(), GfQuatd(), GfQuath())) info = {Kind::Real, 4, 0, info.array};
    else if (is(GfMatrix2d())) info = {Kind::Real, 4, 2, info.array};
    else if (is(GfMatrix3d())) info = {Kind::Real, 9, 3, info.array};
    else if (is(GfMatrix4d())) info = {Kind::Real, 16, 4, info.array};
    return info;
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
std::string Number(GfHalf value) { return Number(float(value)); }
template <class Int, std::enable_if_t<std::is_integral_v<Int>, int> = 0>
std::string Number(Int value) { return std::to_string(value); }

// One value as text. In an array, text is quoted and tuples are in
// parentheses; on its own a vector is "1, 2, 3".
std::string Format(bool v, bool) { return v ? "true" : "false"; }
template <class N, std::enable_if_t<std::is_arithmetic_v<N> || std::is_same_v<N, GfHalf>, int> = 0>
std::string Format(N v, bool) { return Number(v); }
std::string Format(const SdfTimeCode &v, bool) { return Number(v.GetValue()); }
std::string Quoted(const std::string &text, bool inArray) {
    return inArray ? JsWriteToString(JsValue(text)) : text;
}
std::string Format(const std::string &v, bool inArray) { return Quoted(v, inArray); }
std::string Format(const TfToken &v, bool inArray) { return Quoted(v.GetString(), inArray); }
std::string Format(const SdfAssetPath &v, bool inArray) { return Quoted(v.GetAssetPath(), inArray); }

std::string Tuple(const std::vector<std::string> &parts, bool parenthesized) {
    const std::string text = TfStringJoin(parts, ", ");
    return parenthesized ? "(" + text + ")" : text;
}
template <class V, std::enable_if_t<GfIsGfVec<V>::value, int> = 0>
std::string Format(const V &v, bool inArray) {
    std::vector<std::string> parts;
    for (size_t i = 0; i < V::dimension; ++i) parts.push_back(Number(v[i]));
    return Tuple(parts, inArray);
}
template <class Q, std::enable_if_t<GfIsGfQuat<Q>::value, int> = 0>
std::string Format(const Q &q, bool inArray) {
    const auto &i = q.GetImaginary();
    return Tuple({Number(q.GetReal()), Number(i[0]), Number(i[1]), Number(i[2])}, inArray);
}
template <class M, std::enable_if_t<GfIsGfMatrix<M>::value, int> = 0>
std::string Format(const M &m, bool inArray) {
    std::vector<std::string> rows;
    for (size_t r = 0; r < M::numRows; ++r) {
        std::vector<std::string> row;
        for (size_t c = 0; c < M::numColumns; ++c) row.push_back(Number(m[r][c]));
        rows.push_back(Tuple(row, true));
    }
    return Tuple(rows, inArray);
}

template <class T> bool TryFormat(const VtValue &value, std::string *text) {
    if (value.IsHolding<T>()) {
        *text = Format(value.UncheckedGet<T>(), false);
        return true;
    }
    if (value.IsHolding<VtArray<T>>()) {
        std::vector<std::string> elements;
        for (const T &element : value.UncheckedGet<VtArray<T>>()) {
            elements.push_back(Format(element, true));
        }
        *text = "[" + TfStringJoin(elements, ", ") + "]";
        return true;
    }
    return false;
}

template <class... T> bool FormatAny(const VtValue &value, std::string *text) {
    return (TryFormat<T>(value, text) || ...);
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

bool ParseScalar(const std::string &text, Kind kind, JsValue *value, std::string *error) {
    if (kind == Kind::Text) {
        *value = JsValue(text);
        return true;
    }
    if (kind == Kind::Bool) {
        const std::string t = TfStringToLower(TfStringTrim(text));
        if (t == "true" || t == "1" || t == "yes") *value = JsValue(true);
        else if (t == "false" || t == "0" || t == "no") *value = JsValue(false);
        else {
            *error = "Not true or false: " + text;
            return false;
        }
        return true;
    }
    return ParseNumber(text, kind, value, error);
}

// A tuple's components, in any of "1, 2, 3", "(1, 2, 3)", "1 2 3", and for
// matrices rows in parentheses or brackets.
bool ParseTuple(const std::string &text, const TypeInfo &info, JsValue *value,
                std::string *error) {
    std::string t = text;
    for (char &c : t) if (c == ',' || c == '(' || c == ')' || c == '[' || c == ']') c = ' ';
    const std::vector<std::string> parts = TfStringTokenize(t, " \t\n");
    if (int(parts.size()) != info.count) {
        *error = "Expected " + std::to_string(info.count) + " numbers: " + text;
        return false;
    }
    JsArray array;
    for (const std::string &part : parts) {
        JsValue element;
        if (!ParseNumber(part, info.kind, &element, error)) return false;
        array.push_back(element);
    }
    *value = JsValue(array);
    return true;
}

// A JSON element of an array of numbers or tuples (flattening matrix rows).
bool Flatten(const JsValue &element, std::vector<JsValue> *numbers) {
    if (element.IsArray()) {
        for (const JsValue &e : element.GetJsArray()) {
            if (!Flatten(e, numbers)) return false;
        }
        return true;
    }
    if (!(element.IsReal() || element.IsInt() || element.IsUInt64() || element.IsBool())) {
        return false;
    }
    numbers->push_back(element);
    return true;
}

bool Fits(Kind kind, const JsValue &v) {
    switch (kind) {
    case Kind::Bool: return v.IsBool();
    case Kind::Integer: return v.IsInt() || v.IsUInt64();
    case Kind::Unsigned: return v.IsUInt64() || (v.IsInt() && v.GetInt64() >= 0);
    case Kind::Real: return v.IsReal() || v.IsInt() || v.IsUInt64();
    default: return false;
    }
}

// Arrays as written by FormatForEditing: [1, 2], [(1, 2), (3, 4)], ["a"].
bool ParseArray(const std::string &text, const TypeInfo &info, JsValue *value,
                std::string *error) {
    std::string json = TfStringTrim(text);
    if (info.kind != Kind::Text) {
        // USD's tuples in parentheses, as JSON arrays.
        for (char &c : json) {
            if (c == '(') c = '[';
            else if (c == ')') c = ']';
        }
    }
    JsParseError jsError;
    const JsValue parsed = JsParseString(json, &jsError);
    if (!parsed.IsArray()) {
        *error = info.kind == Kind::Text
            ? "Expected a list of quoted strings, like [\"a\", \"b\"]"
            : "Expected a list in brackets, like " +
              std::string(info.count == 1 ? "[1, 2, 3]" : "[(1, 2, 3), (4, 5, 6)]");
        return false;
    }
    JsArray elements;
    size_t index = 0;
    for (const JsValue &element : parsed.GetJsArray()) {
        const std::string where = "Element " + std::to_string(index++) + ": ";
        if (info.kind == Kind::Text) {
            if (!element.IsString()) {
                *error = where + "expected a quoted string";
                return false;
            }
            elements.push_back(element);
            continue;
        }
        std::vector<JsValue> numbers;
        if ((info.count == 1 && element.IsArray()) || !Flatten(element, &numbers) ||
            int(numbers.size()) != info.count) {
            *error = where + (info.count == 1 ? "expected a single value"
                              : "expected " + std::to_string(info.count) + " numbers");
            return false;
        }
        for (const JsValue &n : numbers) {
            if (!Fits(info.kind, n)) {
                *error = where + "wrong kind of value";
                return false;
            }
        }
        elements.push_back(info.count == 1 ? numbers[0] : JsValue(JsArray(numbers.begin(), numbers.end())));
    }
    *value = JsValue(elements);
    return true;
}

} // namespace

bool IsTextEditable(const SdfValueTypeName &type) {
    return Info(type).kind != Kind::None;
}

bool IsTextEditable(const SdfValueTypeName &type, const VtValue &value) {
    return IsTextEditable(type) &&
        (!value.IsArrayValued() || value.GetArraySize() <= MaxEditableArrayElements);
}

std::string FormatForEditing(const VtValue &value) {
    std::string text;
    if (FormatAny<bool, int, unsigned int, int64_t, uint64_t, unsigned char, float, double,
                  GfHalf, SdfTimeCode, std::string, TfToken, SdfAssetPath,
                  GfVec2f, GfVec3f, GfVec4f, GfVec2d, GfVec3d, GfVec4d,
                  GfVec2h, GfVec3h, GfVec4h, GfVec2i, GfVec3i, GfVec4i,
                  GfQuatf, GfQuatd, GfQuath, GfMatrix2d, GfMatrix3d, GfMatrix4d>(value, &text)) {
        return text;
    }
    return value.IsEmpty() ? std::string() : TfStringify(value);
}

bool ParseEditedText(const std::string &text, const SdfValueTypeName &type,
                     JsValue *value, std::string *error) {
    const TypeInfo info = Info(type);
    if (info.kind == Kind::None) {
        *error = "Values of type " + type.GetAsToken().GetString() +
            " cannot be edited as text";
        return false;
    }
    if (info.array) return ParseArray(text, info, value, error);
    if (info.count > 1) return ParseTuple(text, info, value, error);
    return ParseScalar(text, info.kind, value, error);
}

} // namespace vela
