#include "validation.h"

#include "pxr/base/tf/token.h"
#include "pxr/usdValidation/usdUtilsValidators/validatorTokens.h"
#include "pxr/usdValidation/usdValidation/context.h"
#include "pxr/usdValidation/usdValidation/error.h"
#include "pxr/usdValidation/usdValidation/registry.h"
#include "pxr/usdValidation/usdValidation/validator.h"

#include <algorithm>

namespace vela {

std::string ToString(ValidationIssue::Severity severity) {
    switch (severity) {
    case ValidationIssue::Severity::Error: return "error";
    case ValidationIssue::Severity::Warning: return "warning";
    case ValidationIssue::Severity::Info: return "info";
    }
    return "error";
}

SdfPath ValidationIssue::GetPrimPath() const {
    for (const Site &site : sites) {
        // Stage-level sites are the pseudo-root, which is not selectable.
        if (site.layer.empty() && !site.path.IsEmpty() &&
            site.path != SdfPath::AbsoluteRootPath()) {
            return site.path.GetPrimPath();
        }
    }
    return SdfPath();
}

std::vector<ValidationIssue> RunValidation(const UsdStageRefPtr &stage,
                                           const std::vector<std::string> &keywords) {
    UsdValidationRegistry &registry = UsdValidationRegistry::GetInstance();
    UsdValidationValidatorMetadataVector metadata;
    if (keywords.empty()) {
        // usdchecker's default: not the check for a .usdz's root layer only.
        metadata = registry.GetAllValidatorMetadata();
        metadata.erase(std::remove_if(metadata.begin(), metadata.end(),
            [](const UsdValidationValidatorMetadata &m) {
                return m.name == UsdUtilsValidatorNameTokens->rootPackageValidator;
            }), metadata.end());
    } else {
        TfTokenVector tokens;
        for (const std::string &keyword : keywords) tokens.emplace_back(keyword);
        metadata = registry.GetValidatorMetadataForKeywords(tokens);
    }
    const UsdValidationContext context(metadata);
    std::vector<ValidationIssue> issues;
    for (const UsdValidationError &error : context.Validate(stage)) {
        ValidationIssue issue;
        switch (error.GetType()) {
        case UsdValidationErrorType::Warn: issue.severity = ValidationIssue::Severity::Warning; break;
        case UsdValidationErrorType::Info: issue.severity = ValidationIssue::Severity::Info; break;
        default: issue.severity = ValidationIssue::Severity::Error; break;
        }
        if (const UsdValidationValidator *validator = error.GetValidator()) {
            issue.validator = validator->GetMetadata().name.GetString();
        }
        issue.message = error.GetMessage();
        for (const UsdValidationErrorSite &site : error.GetSites()) {
            if (site.IsProperty()) {
                issue.sites.push_back({site.GetProperty().GetPath(), ""});
            } else if (site.IsPrim()) {
                issue.sites.push_back({site.GetPrim().GetPath(), ""});
            } else if (site.GetLayer()) {
                SdfPath path;
                if (const SdfPrimSpecHandle prim = site.GetPrimSpec()) {
                    path = prim->GetPath();
                } else if (const SdfPropertySpecHandle property = site.GetPropertySpec()) {
                    path = property->GetPath();
                }
                issue.sites.push_back({path, site.GetLayer()->GetIdentifier()});
            }
        }
        issues.push_back(std::move(issue));
    }
    // Validators run in parallel: sort for a stable order.
    std::stable_sort(issues.begin(), issues.end(),
        [](const ValidationIssue &a, const ValidationIssue &b) {
            if (a.severity != b.severity) return a.severity < b.severity;
            if (a.validator != b.validator) return a.validator < b.validator;
            return a.message < b.message;
        });
    return issues;
}

JsValue ToJson(const std::vector<ValidationIssue> &issues) {
    JsArray array;
    for (const ValidationIssue &issue : issues) {
        JsArray sites;
        for (const ValidationIssue::Site &site : issue.sites) {
            sites.push_back(JsValue(JsObject{{"path", JsValue(site.path.GetString())},
                                             {"layer", JsValue(site.layer)}}));
        }
        array.push_back(JsValue(JsObject{
            {"severity", JsValue(ToString(issue.severity))},
            {"validator", JsValue(issue.validator)},
            {"message", JsValue(issue.message)},
            {"sites", JsValue(sites)}}));
    }
    return JsValue(array);
}

} // namespace vela
