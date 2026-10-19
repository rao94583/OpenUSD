#include "test_support.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdShade/material.h"
#include "pxr/usd/usdShade/shader.h"
#include "pxr/usdImaging/usdImaging/materialAdapter.h"
#include "pxr/imaging/hd/materialSchema.h"

PXR_NAMESPACE_USING_DIRECTIVE

int main() {
    vela::test::RegisterPlugins();
    auto stage = UsdStage::CreateInMemory();
    auto material = UsdShadeMaterial::Define(stage, SdfPath("/Material"));
    auto shader = UsdShadeShader::Define(stage, SdfPath("/Material/Graph/Surface"));
    shader.CreateIdAttr(VtValue(TfToken("UsdPreviewSurface")));
    shader.CreateOutput(TfToken("surface"), SdfValueTypeNames->Token);
    material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), TfToken("surface"));
    UsdImagingMaterialAdapter adapter;
    const auto root = HdMaterialSchema::GetDefaultLocator();
    auto invalidate = [&](const TfTokenVector &properties, UsdImagingPropertyInvalidationType type) {
        return adapter.InvalidateImagingSubprimFromDescendent(
            material.GetPrim(), shader.GetPrim(), TfToken(), properties, type);
    };
    // Property updates can change connections and graph membership. A node-only
    // locator leaves cached connections to deleted upstream shaders reachable.
    CHECK(invalidate({TfToken("inputs:diffuseColor")},
        UsdImagingPropertyInvalidationType::Update).Contains(root));
    CHECK(invalidate({TfToken("info:id")},
        UsdImagingPropertyInvalidationType::Update).Contains(root));
    // A structural resync with no property list is not an empty set of UI edits.
    CHECK(invalidate({}, UsdImagingPropertyInvalidationType::Resync).Contains(root));
    CHECK(invalidate({}, UsdImagingPropertyInvalidationType::Update).Contains(root));
    CHECK(invalidate({TfToken("ui:nodegraph:node:pos")},
        UsdImagingPropertyInvalidationType::Update).IsEmpty());
    CHECK(invalidate({TfToken("ui:nodegraph:node:pos"), TfToken("inputs:roughness")},
        UsdImagingPropertyInvalidationType::Update).Contains(root));
    CHECK(invalidate({TfToken("ui:nodegraph:node:pos")},
        UsdImagingPropertyInvalidationType::Resync).Contains(root));
    return vela::test::Finish("material invalidation");
}
