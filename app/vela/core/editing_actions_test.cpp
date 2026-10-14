#include "actions.h"
#include <limits>
#include "document.h"
#include "test_support.h"
#include "pxr/base/js/json.h"
#include "pxr/usd/usdGeom/cube.h"
#include "pxr/usd/usdGeom/xform.h"
#include "pxr/usd/usdGeom/imageable.h"
#include "pxr/usd/usd/variantSets.h"
#include "pxr/usd/usd/payloads.h"

using namespace vela;
namespace {
ActionResult Run(Document &d, const char *name, const char *params) {
    return ActionRegistry::Get().Run(d, name, JsParseString(params).GetJsObject());
}
std::string Text(const SdfLayerHandle &layer) {
    std::string text; layer->ExportToString(&text); return text;
}
void TestAttributes() {
    auto stage = UsdStage::CreateInMemory();
    auto cube = UsdGeomCube::Define(stage, SdfPath("/Cube"));
    cube.CreateSizeAttr(VtValue(2.0));
    auto colors = cube.GetPrim().CreateAttribute(TfToken("colors"), SdfValueTypeNames->Color3fArray);
    auto integer = cube.GetPrim().CreateAttribute(TfToken("count"), SdfValueTypeNames->Int);
    auto signed64 = cube.GetPrim().CreateAttribute(TfToken("signed64"), SdfValueTypeNames->Int64);
    auto unsigned64 = cube.GetPrim().CreateAttribute(TfToken("unsigned64"), SdfValueTypeNames->UInt64);
    auto uniform = cube.GetPrim().CreateAttribute(TfToken("label"), SdfValueTypeNames->String, false, SdfVariabilityUniform);
    Document d(stage);
    CHECK(d.SetEditTarget(stage->GetSessionLayer()));
    const std::string before = Text(stage->GetSessionLayer());
    CHECK(Run(d, "set_attribute", R"({"path":"/Cube","attribute":"size","value":4})").ok);
    double size = 0; cube.GetSizeAttr().Get(&size); CHECK(size == 4);
    CHECK(Text(stage->GetRootLayer()).find("double size = 2") != std::string::npos);
    const std::string after = Text(stage->GetSessionLayer());
    CHECK(d.GetUndoStack().Undo()); CHECK(Text(stage->GetSessionLayer()) == before);
    CHECK(d.GetUndoStack().Redo()); CHECK(Text(stage->GetSessionLayer()) == after);
    CHECK(Run(d, "clear_attribute", R"({"path":"/Cube","attribute":"size"})").ok);
    cube.GetSizeAttr().Get(&size); CHECK(size == 2);
    CHECK(d.GetUndoStack().Undo()); cube.GetSizeAttr().Get(&size); CHECK(size == 4);
    CHECK(Run(d, "set_attribute", R"({"path":"/Cube","attribute":"size","value":8,"time":3})").ok);
    cube.GetSizeAttr().Get(&size, UsdTimeCode(3)); CHECK(size == 8);
    CHECK(Run(d, "clear_attribute", R"({"path":"/Cube","attribute":"size","time":3})").ok);
    CHECK(cube.GetSizeAttr().GetNumTimeSamples() == 0);
    CHECK(Run(d, "set_attribute", R"({"path":"/Cube","attribute":"colors","value":[[1,0,0],[0,0.5,1]]})").ok);
    VtArray<GfVec3f> value; CHECK(colors.Get(&value)); CHECK(value.size() == 2);
    CHECK(value[1] == GfVec3f(0, 0.5, 1));
    CHECK(Run(d, "set_attribute", R"({"path":"/Cube","attribute":"signed64","value":9223372036854775807})").ok);
    int64_t maxSigned = 0; CHECK(signed64.Get(&maxSigned)); CHECK(maxSigned == std::numeric_limits<int64_t>::max());
    CHECK(Run(d, "set_attribute", R"({"path":"/Cube","attribute":"unsigned64","value":18446744073709551615})").ok);
    uint64_t maxUnsigned = 0; CHECK(unsigned64.Get(&maxUnsigned)); CHECK(maxUnsigned == std::numeric_limits<uint64_t>::max());
    const auto unchanged = Text(stage->GetSessionLayer());
    CHECK(!Run(d, "set_attribute", R"({"path":"/Cube","attribute":"colors","value":[[1,0]]})").ok);
    CHECK(!Run(d, "set_attribute", R"({"path":"/Cube","attribute":"size","value":"large"})").ok);
    CHECK(!Run(d, "set_attribute", R"({"path":"/Cube","attribute":"missing","value":1})").ok);
    CHECK(!Run(d, "set_attribute", R"({"path":"/Cube","attribute":"count","value":1.2})").ok);
    CHECK(!Run(d, "set_attribute", R"({"path":"/Cube","attribute":"count","value":2147483648})").ok);
    CHECK(!Run(d, "set_attribute", R"({"path":"/Cube","attribute":"label","value":"x","time":1})").ok);
    CHECK(!Run(d, "set_attribute", R"({"path":"/Cube","attribute":"signed64","value":9223372036854775808})").ok);
    CHECK(!Run(d, "set_attribute", R"({"path":"/Cube","attribute":"signed64","value":9223372036854775808.0})").ok);
    CHECK(!Run(d, "set_attribute", R"({"path":"/Cube","attribute":"unsigned64","value":18446744073709551616.0})").ok);
    CHECK(!Run(d, "set_attribute", R"({"path":"/Cube","attribute":"unsigned64","value":-1})").ok);
    CHECK(Text(stage->GetSessionLayer()) == unchanged);
    stage->GetSessionLayer()->SetPermissionToEdit(false);
    CHECK(!Run(d, "set_attribute", R"({"path":"/Cube","attribute":"size","value":5})").ok);
    CHECK(Text(stage->GetSessionLayer()) == unchanged);
    stage->GetSessionLayer()->SetPermissionToEdit(true);
}
void TestComposition() {
    auto stage = UsdStage::CreateInMemory();
    auto parent = UsdGeomXform::Define(stage, SdfPath("/World"));
    auto child = UsdGeomCube::Define(stage, SdfPath("/World/Cube"));
    auto variants = parent.GetPrim().GetVariantSets().AddVariantSet("shape");
    variants.AddVariant("a"); variants.AddVariant("b"); variants.SetVariantSelection("a");
    Document d(stage);
    const auto before = Text(stage->GetRootLayer());
    CHECK(Run(d, "set_variant", R"({"path":"/World","set":"shape","variant":"b"})").ok);
    CHECK(variants.GetVariantSelection() == "b");
    CHECK(d.GetUndoStack().Undo()); CHECK(Text(stage->GetRootLayer()) == before);
    CHECK(d.GetUndoStack().Redo()); CHECK(variants.GetVariantSelection() == "b");
    CHECK(!Run(d, "set_variant", R"({"path":"/World","set":"shape","variant":"missing"})").ok);
    CHECK(!Run(d, "set_variant", R"({"path":"/World","set":"missing","variant":"a"})").ok);
    CHECK(Run(d, "set_variant", R"({"path":"/World","set":"shape","variant":""})").ok);
    CHECK(variants.GetVariantSelection().empty());
    // Port of testUsdviewDeactivate's parent/child recomposition scenario.
    d.SetSelection({SdfPath("/World/Cube")});
    CHECK(Run(d, "set_active", R"({"path":"/World","active":false})").ok);
    CHECK(!stage->GetPrimAtPath(SdfPath("/World/Cube"))); CHECK(d.GetSelection().empty());
    CHECK(d.GetUndoStack().Undo()); CHECK(stage->GetPrimAtPath(SdfPath("/World/Cube")));
    CHECK(d.GetUndoStack().Redo()); CHECK(!stage->GetPrimAtPath(SdfPath("/World/Cube")));
    CHECK(Run(d, "set_active", R"({"path":"/World","active":true})").ok);
    // Port of testUsdviewInstancingEdits' authored visibility behavior.
    CHECK(Run(d, "set_visibility", R"({"path":"/World","visibility":"invisible"})").ok);
    CHECK(UsdGeomImageable(stage->GetPrimAtPath(SdfPath("/World/Cube"))).ComputeVisibility() == UsdGeomTokens->invisible);
    CHECK(Run(d, "set_visibility", R"({"path":"/World/Cube","visibility":"inherited"})").ok);
    CHECK(UsdGeomImageable(stage->GetPrimAtPath(SdfPath("/World/Cube"))).ComputeVisibility() == UsdGeomTokens->invisible);
    CHECK(d.GetUndoStack().Undo()); CHECK(d.GetUndoStack().Undo());
    CHECK(UsdGeomImageable(stage->GetPrimAtPath(SdfPath("/World/Cube"))).ComputeVisibility() == UsdGeomTokens->inherited);
}
void TestSelectionMenus() {
    auto stage = UsdStage::CreateInMemory();
    UsdGeomXform::Define(stage, SdfPath("/World"));
    UsdGeomCube::Define(stage, SdfPath("/World/Cube"));
    auto plain = stage->DefinePrim(SdfPath("/Plain"));
    Document d(stage);
    auto &registry = ActionRegistry::Get();
    CHECK(!registry.Run(d, "hide_selection").ok);
    d.SetSelection({SdfPath("/World"), SdfPath("/Plain")});
    auto before = Text(stage->GetRootLayer());
    CHECK(!registry.Run(d, "hide_selection").ok);
    CHECK(Text(stage->GetRootLayer()) == before);
    CHECK(!d.GetUndoStack().CanUndo());
    d.SetSelection({SdfPath("/World"), SdfPath("/World/Cube")});
    CHECK(registry.Run(d, "hide_selection").ok);
    CHECK(d.GetUndoStack().Undo()); CHECK(Text(stage->GetRootLayer()) == before);
    CHECK(!d.GetUndoStack().CanUndo());
    CHECK(registry.Run(d, "deactivate_selection").ok);
    CHECK(!stage->GetPrimAtPath(SdfPath("/World/Cube")));
    CHECK(d.GetUndoStack().Undo()); CHECK(Text(stage->GetRootLayer()) == before);
    CHECK(d.GetUndoStack().Redo()); CHECK(!stage->GetPrimAtPath(SdfPath("/World/Cube")));
    CHECK(d.GetUndoStack().Undo());
    CHECK(Run(d, "set_purpose", R"({"path":"/World/Cube","purpose":"render"})").ok);
    CHECK(UsdGeomImageable(stage->GetPrimAtPath(SdfPath("/World/Cube"))).ComputePurpose() == UsdGeomTokens->render);
    CHECK(d.GetUndoStack().Undo()); CHECK(Text(stage->GetRootLayer()) == before);
    CHECK(Run(d, "set_model_draw_mode", R"({"path":"/World","mode":"bounds"})").ok);
    TfToken draw; CHECK(stage->GetPrimAtPath(SdfPath("/World")).GetAttribute(TfToken("model:drawMode")).Get(&draw));
    CHECK(draw == TfToken("bounds"));
    CHECK(d.GetUndoStack().Undo()); CHECK(Text(stage->GetRootLayer()) == before);
}

void TestPayload() {
    auto payload = UsdStage::CreateInMemory();
    UsdGeomXform::Define(payload, SdfPath("/Asset"));
    UsdGeomCube::Define(payload, SdfPath("/Asset/Cube"));
    auto stage = UsdStage::CreateInMemory();
    auto prim = stage->DefinePrim(SdfPath("/World"));
    prim.GetPayloads().AddPayload(payload->GetRootLayer()->GetIdentifier(), SdfPath("/Asset"));
    Document d(stage);
    auto before = Text(stage->GetRootLayer());
    CHECK(Run(d, "set_payload_loaded", R"({"path":"/World","loaded":false})").ok);
    CHECK(!prim.IsLoaded()); CHECK(!stage->GetPrimAtPath(SdfPath("/World/Cube")));
    CHECK(Run(d, "set_payload_loaded", R"({"path":"/World","loaded":true})").ok);
    CHECK(prim.IsLoaded()); CHECK(stage->GetPrimAtPath(SdfPath("/World/Cube")));
    CHECK(Text(stage->GetRootLayer()) == before); CHECK(!d.GetUndoStack().CanUndo());
}
}
int main() {
    test::RegisterPlugins(); TestAttributes(); TestComposition(); TestSelectionMenus(); TestPayload();
    return test::Finish("editing actions");
}
