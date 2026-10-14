#include "browser.h"
#include "test_support.h"
#include "pxr/usd/usdGeom/cube.h"
#include "pxr/usd/usdGeom/xform.h"
#include "pxr/usd/usd/variantSets.h"
#include "pxr/usd/usd/references.h"
#include "pxr/base/ts/spline.h"
#include "pxr/base/ts/knot.h"
using namespace vela;

namespace {
const InspectorRow *Find(const InspectorRows &rows, const std::string &name) {
    for (const auto &row : rows) if (row.name == name) return &row;
    return nullptr;
}
void TestOutline() {
    auto stage = UsdStage::CreateInMemory();
    UsdGeomXform::Define(stage, SdfPath("/World"));
    UsdGeomCube::Define(stage, SdfPath("/World/Needle"));
    UsdGeomXform::Define(stage, SdfPath("/Other"));
    UsdGeomCube::Define(stage, SdfPath("/Other/Inactive"));
    stage->GetPrimAtPath(SdfPath("/Other/Inactive")).SetActive(false);
    UsdGeomXform::Define(stage, SdfPath("/Prototype"));
    UsdGeomCube::Define(stage, SdfPath("/Prototype/Part"));
    auto instance = stage->DefinePrim(SdfPath("/Instance"));
    instance.GetReferences().AddInternalReference(SdfPath("/Prototype"));
    instance.SetInstanceable(true);
    Document d(stage); Browser b(d);
    CHECK(b.Children().size() == 4);
    CHECK(b.HasChildren(SdfPath("/World")));
    CHECK(!b.HasChildren(SdfPath("/World/Needle")));
    CHECK(b.Children(SdfPath("/World")) == SdfPathVector{SdfPath("/World/Needle")});
    CHECK(b.Children(SdfPath("/Other")).size() == 1);
    CHECK(!b.Summary(SdfPath("/Other/Inactive")).active);
    CHECK(b.Children(SdfPath("/Instance")) == SdfPathVector{SdfPath("/Instance/Part")});
    CHECK(b.Summary(SdfPath("/Instance/Part")).instanceProxy);
    b.SetFilter("nEeDlE");
    CHECK(b.Children() == SdfPathVector{SdfPath("/World")});
    CHECK(b.Children(SdfPath("/World")).size() == 1);
    // Search must refresh after recomposition, even with the same query.
    UsdGeomCube::Define(stage, SdfPath("/Other/Needle2"));
    CHECK(b.Children().size() == 2);
    b.SetFilter("cube"); CHECK(b.Children().size() == 4);
    b.SetFilter("no match"); CHECK(b.Children().empty());
    b.SetFilter(""); CHECK(b.Children().size() == 4);
    CHECK(b.Children(SdfPath("/Missing")).empty());
}
void TestInspector() {
    auto stage = UsdStage::CreateInMemory();
    auto cube = UsdGeomCube::Define(stage, SdfPath("/Cube"));
    const auto path = cube.GetPath();
    cube.CreateSizeAttr().Set(2.0, UsdTimeCode(1));
    cube.GetSizeAttr().Set(8.0, UsdTimeCode(3));
    auto array = cube.GetPrim().CreateAttribute(TfToken("values"), SdfValueTypeNames->IntArray);
    VtIntArray data(10000); for (size_t i = 0; i < data.size(); ++i) data[i] = int(i);
    array.Set(data);
    cube.GetPrim().SetCustomDataByKey(TfToken("nested:answer"), VtValue(42));
    cube.GetPrim().CreateRelationship(TfToken("targets")).SetTargets({SdfPath("/Cube"), SdfPath("/Cube.size")});
    auto variants = cube.GetPrim().GetVariantSets().AddVariantSet("look");
    variants.AddVariant("red"); variants.AddVariant("blue"); variants.SetVariantSelection("red");
    Document d(stage); Browser b(d); d.SetTime(UsdTimeCode(1));
    auto rows = b.Properties(path);
    CHECK(Find(rows, "size") && Find(rows, "size")->value == "2");
    CHECK(Find(rows, "values") && Find(rows, "values")->value == "10000 elements");
    d.SetTime(UsdTimeCode(3)); rows = b.Properties(path);
    CHECK(Find(rows, "size") && Find(rows, "size")->value == "8");
    CHECK(Find(rows, "targets") && Find(rows, "targets")->type == "relationship");
    CHECK(b.PropertyDetailCount(path, "values") == 10000);
    CHECK(b.PropertyDetailCount(path, "size") == 2);
    CHECK(b.PropertyDetailCount(path, "targets") == 2);
    auto details = b.PropertyDetails(path, "values", 128, 16);
    CHECK(details.size() == 17); CHECK(details[1].name == "[128]"); CHECK(details[1].value == "128");
    CHECK(b.PropertyDetails(path, "values", 20000).size() == 1);
    CHECK(b.PropertyDetails(path, "values", 0, 10000).size() == 257);
    CHECK(b.PropertyDetails(path, "targets").size() == 2);
    CHECK(b.PropertyDetails(path, "size").size() == 4);
    auto metadata = b.Metadata(path);
    CHECK(Find(metadata, "customData.nested.answer"));
    CHECK(Find(metadata, "customData.nested.answer")->value == "42");
    CHECK(!b.Composition(path).empty()); CHECK(b.Layers().size() == 2);
    auto layerRows = b.Layers(); CHECK(layerRows.back().type == "edit target");
    d.SetEditTarget(stage->GetSessionLayer()); CHECK(b.Layers().front().type == "edit target");
    auto variantRows = b.Variants(path); CHECK(variantRows.size() == 1);
    CHECK(variantRows[0].value == "red"); CHECK(variantRows[0].source.find("blue") != std::string::npos);
    CHECK(b.Properties(SdfPath("/Missing")).empty());
    CHECK(b.PropertyDetails(path, "missing").empty());
    auto splineAttr = cube.GetPrim().CreateAttribute(TfToken("curve"), SdfValueTypeNames->Double);
    TsSpline spline;
    TsKnot first; first.SetTime(1); first.SetValue(2.0);
    TsKnot last; last.SetTime(3); last.SetValue(6.0);
    spline.SetKnot(first); spline.SetKnot(last);
    CHECK(splineAttr.SetSpline(spline));
    CHECK(b.PropertyDetailCount(path, "curve") == 2);
    CHECK(b.PropertyDetails(path, "curve").size() == 3);
    CHECK(b.PropertyDetails(path, "curve", 1, 1).size() == 2);
}
}
int main() { test::RegisterPlugins(); TestOutline(); TestInspector(); return test::Finish("browser"); }
