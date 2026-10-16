// Attribute values as editable text, and back to the JSON values the
// set_attribute action takes. No UI dependency.
#pragma once

#include "pxr/pxr.h"
#include "pxr/base/js/value.h"
#include "pxr/base/vt/value.h"
#include "pxr/usd/sdf/valueTypeName.h"

#include <string>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

// Whether values of this type can be edited as text: bool, integers,
// float, double, half, timecode, string, token, asset, vectors,
// quaternions, matrices, and arrays of those. Not dictionaries or other
// types.
bool IsTextEditable(const SdfValueTypeName &type);
// Also, for arrays, whether this value is small enough to edit as text.
bool IsTextEditable(const SdfValueTypeName &type, const VtValue &value);
constexpr size_t MaxEditableArrayElements = 1000;

// A value as text for editing: true/false, numbers in full precision,
// strings and tokens as is, asset paths as their path, vectors and
// quaternions (real first) as "1, 2, 3", matrices as rows "(1, 0), (0, 1)",
// arrays as "[1, 2]", "[(1, 2, 3), (4, 5, 6)]" or "[\"a\", \"b\"]".
std::string FormatForEditing(const VtValue &value);

// Parses edited text for an attribute of the given type into the JSON value
// set_attribute expects. Tuples accept "1, 2, 3", "(1, 2, 3)" or "1 2 3";
// arrays the bracketed forms FormatForEditing writes. Returns false with a
// message in error.
bool ParseEditedText(const std::string &text, const SdfValueTypeName &type,
                     JsValue *value, std::string *error);

} // namespace vela
