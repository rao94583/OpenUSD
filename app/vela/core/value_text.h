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
// float/double, string, token, asset, and 2-4 component float, double and
// int vectors (not arrays, matrices, quaternions or half).
bool IsTextEditable(const SdfValueTypeName &type);

// A value as text for editing: true/false, numbers in full precision, strings
// and tokens as is, asset paths as their path, vectors as "1, 2, 3".
std::string FormatForEditing(const VtValue &value);

// Parses edited text for an attribute of the given type into the JSON value
// set_attribute expects. Vectors accept "1, 2, 3", "(1, 2, 3)" or "1 2 3".
// Returns false with a message in error.
bool ParseEditedText(const std::string &text, const SdfValueTypeName &type,
                     JsValue *value, std::string *error);

} // namespace vela
