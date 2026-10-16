// Stage validation with pxr/usdValidation, as usdchecker runs it: every
// registered validator except the root-package-only check, or those with
// given keywords. Results are plain records the document keeps, for the
// Validation panel, the validate action and scripts.
#pragma once

#include "pxr/pxr.h"
#include "pxr/base/js/value.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/stage.h"

#include <string>
#include <vector>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

struct ValidationIssue {
    enum class Severity { Error, Warning, Info };
    Severity severity = Severity::Error;
    std::string validator;  // e.g. "usdGeomValidators:StageMetadataChecker"
    std::string message;
    // Where: prim or property paths on the stage, or layer identifiers
    // (with a spec path) for layer problems.
    struct Site {
        SdfPath path;       // empty for a whole layer
        std::string layer;  // empty for stage objects
        bool operator==(const Site &other) const = default;
    };
    std::vector<Site> sites;

    // The prim to select for this issue: the first site's prim (a
    // property's owner), or empty.
    SdfPath GetPrimPath() const;
    bool operator==(const ValidationIssue &other) const = default;
};

// "error", "warning", "info".
std::string ToString(ValidationIssue::Severity severity);

// Validates the stage; errors first, then warnings, then info, each in the
// validators' order. keywords, when given, picks validators by keyword.
std::vector<ValidationIssue> RunValidation(const UsdStageRefPtr &stage,
                                           const std::vector<std::string> &keywords = {});

// [{"severity": ..., "validator": ..., "message": ..., "sites": [{"path",
// "layer"}]}]
JsValue ToJson(const std::vector<ValidationIssue> &issues);

} // namespace vela
