#include "actions.h"
#include "document.h"
#include "test_support.h"
#include "validation.h"

#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdShade/material.h"

#include <algorithm>

using namespace vela;

namespace {

bool Has(const std::vector<ValidationIssue> &issues, const std::string &validator) {
    return std::any_of(issues.begin(), issues.end(),
                       [&](const ValidationIssue &i) { return i.validator == validator; });
}

void Test() {
    // No upAxis, metersPerUnit or defaultPrim: stage-level errors, at the
    // pseudo-root, which has no prim to select.
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/World/Mesh"));
    std::vector<ValidationIssue> issues = RunValidation(stage);
    CHECK(Has(issues, "usdGeomValidators:StageMetadataChecker"));
    CHECK(Has(issues, "usdValidation:StageMetadataChecker"));
    for (const ValidationIssue &issue : issues) {
        if (issue.validator.find("StageMetadataChecker") == std::string::npos) continue;
        CHECK(issue.severity == ValidationIssue::Severity::Error);
        CHECK(issue.sites.size() == 1 && issue.sites[0].path == SdfPath::AbsoluteRootPath());
        CHECK(issue.GetPrimPath().IsEmpty());
    }

    // Fixing the metadata clears those; a material bound without
    // MaterialBindingAPI is reported on the prim.
    UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y);
    UsdGeomSetStageMetersPerUnit(stage, 1.0);
    stage->SetDefaultPrim(stage->GetPrimAtPath(SdfPath("/World")));
    UsdShadeMaterial::Define(stage, SdfPath("/World/Material"));
    mesh.GetPrim().CreateRelationship(TfToken("material:binding"))
        .AddTarget(SdfPath("/World/Material"));
    issues = RunValidation(stage);
    CHECK(!Has(issues, "usdGeomValidators:StageMetadataChecker"));
    CHECK(!Has(issues, "usdValidation:StageMetadataChecker"));
    auto binding = std::find_if(issues.begin(), issues.end(), [](const ValidationIssue &i) {
        return i.GetPrimPath() == SdfPath("/World/Mesh");
    });
    CHECK(binding != issues.end());

    // Keywords pick validators.
    for (const ValidationIssue &issue : RunValidation(stage, {"UsdGeomValidators"})) {
        CHECK(issue.validator.rfind("usdGeomValidators:", 0) == 0);
    }

    // Through the action: counts, issues as JSON, kept on the document.
    Document d(stage);
    unsigned seen = 0;
    d.AddObserver([&](unsigned changes) { seen |= changes; });
    CHECK(!d.HasValidated());
    ActionResult result = ActionRegistry::Get().Run(d, "validate");
    CHECK(result.ok && d.HasValidated() && (seen & Document::Validation));
    const JsObject &json = result.value.GetJsObject();
    CHECK(json.at("issues").GetJsArray().size() == d.GetValidationIssues().size());
    CHECK(json.at("errors").GetInt() + json.at("warnings").GetInt() + json.at("info").GetInt() ==
          int(d.GetValidationIssues().size()));
    if (!d.GetValidationIssues().empty()) {
        const JsObject &first = json.at("issues").GetJsArray()[0].GetJsObject();
        CHECK(first.count("severity") && first.count("validator") && first.count("message"));
        CHECK(first.at("sites").IsArray());
    }
}

} // namespace

int main() {
    test::RegisterPlugins();
    Test();
    return test::Finish("validation");
}
