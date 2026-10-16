#include "browser.h"
#include <algorithm>
#include "test_support.h"
#include "pxr/usd/usdGeom/cube.h"
#include "pxr/usd/usdGeom/xform.h"
#include "pxr/usd/usd/variantSets.h"
#include "pxr/usd/usd/references.h"
#include "pxr/usd/usdGeom/imageable.h"
#include "pxr/usd/usd/payloads.h"
#include "actions.h"
#include "pxr/base/vt/typeHeaders.h"
#include "pxr/base/vt/visitValue.h"
#include "pxr/usd/sdf/schema.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/sdf/pathExpression.h"
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
void TestFiltersAndPrototypes() {
    auto stage = UsdStage::CreateInMemory();
    auto world = UsdGeomXform::Define(stage, SdfPath("/World"));
    world.GetPrim().SetMetadata(TfToken("kind"), TfToken("component"));
    auto cube = UsdGeomCube::Define(stage, SdfPath("/World/Cube"));
    auto inactive = stage->DefinePrim(SdfPath("/Inactive")); inactive.SetActive(false);
    auto instance = stage->DefinePrim(SdfPath("/Instance"));
    instance.GetReferences().AddInternalReference(SdfPath("/World")); instance.SetInstanceable(true);
    const auto prototype = instance.GetPrototype();
    Document d(stage); Browser b(d);
    CHECK(prototype); CHECK(b.Children().size() == 3);
    CHECK(b.PrototypePath(instance.GetPath()) == prototype.GetPath());
    const auto proxy = stage->GetPrimAtPath(SdfPath("/Instance/Cube"));
    CHECK(b.PrototypePath(proxy.GetPath()) == prototype.GetPath().AppendChild(TfToken("Cube")));
    CHECK(b.PrototypePath(SdfPath("/World/Cube")).IsEmpty());
    CHECK(!b.Composition(proxy.GetPath()).empty());
    b.SetShowPrototypes(true); CHECK(b.Children().size() == 4);
    CHECK(b.Summary(prototype.GetPath()).inPrototype);
    CHECK(b.Children(prototype.GetPath()).size() == 1);
    b.SetFilter("cube"); CHECK(b.Children().size() == 3);
    b.SetShowPrototypes(false); CHECK(b.Children().size() == 2);
    b.SetFilter("inactive"); b.SetPrimFilter(Browser::PrimFilter::Active);
    CHECK(b.Children().empty());
    b.SetFilter(""); b.SetPrimFilter(Browser::PrimFilter::Models);
    CHECK(b.Children().size() == 2); // component and its instance
    b.SetPrimFilter(Browser::PrimFilter::Visible);
    cube.CreateVisibilityAttr().Set(UsdGeomTokens->invisible, UsdTimeCode(1));
    cube.GetVisibilityAttr().Set(UsdGeomTokens->inherited, UsdTimeCode(2));
    d.SetTime(UsdTimeCode(1)); CHECK(b.Children(world.GetPath()).empty());
    d.SetTime(UsdTimeCode(2)); CHECK(b.Children(world.GetPath()).size() == 1);
    // An ancestor's animated visibility hides matching descendants, even when
    // the ancestor itself does not match the search.
    cube.GetVisibilityAttr().Clear();
    world.CreateVisibilityAttr().Set(UsdGeomTokens->invisible, UsdTimeCode(1));
    world.GetVisibilityAttr().Set(UsdGeomTokens->inherited, UsdTimeCode(2));
    b.SetFilter("cube");
    d.SetTime(UsdTimeCode(1)); CHECK(b.Children(world.GetPath()).empty());
    d.SetTime(UsdTimeCode(2)); CHECK(b.Children(world.GetPath()).size() == 1);
    // Unanimated visibility: authored edits (not resyncs) still refilter.
    world.GetVisibilityAttr().Clear();
    world.GetVisibilityAttr().Set(UsdGeomTokens->invisible);
    CHECK(b.Children(world.GetPath()).empty());
    world.GetVisibilityAttr().Set(UsdGeomTokens->inherited);
    CHECK(b.Children(world.GetPath()).size() == 1);
    b.SetFilter("");
    // Generated prototypes can be selected for inspection but cannot be edited.
    auto &actions = ActionRegistry::Get();
    const auto protoChild = prototype.GetPath().AppendChild(TfToken("Cube"));
    CHECK(actions.Run(d, "select", {{"paths", JsValue(JsArray{JsValue(protoChild.GetString())})}}).ok);
    CHECK(!actions.Run(d, "set_active", {{"path", JsValue(protoChild.GetString())}, {"active", JsValue(false)}}).ok);
    CHECK(!actions.Run(d, "hide_selection").ok);
    CHECK(d.GetUndoStack().CanUndo()); // visibility edits above were recorded
    CHECK(stage->GetPrimAtPath(protoChild));
    // Unloaded payload roots remain available under All but are excluded by
    // Loaded. Switching filters does not change the stage's load rules.
    auto payload = UsdStage::CreateInMemory();
    UsdGeomXform::Define(payload, SdfPath("/Asset"));
    auto asset = stage->DefinePrim(SdfPath("/Payload"));
    asset.GetPayloads().AddPayload(payload->GetRootLayer()->GetIdentifier(), SdfPath("/Asset"));
    stage->Unload(asset.GetPath());
    b.SetPrimFilter(Browser::PrimFilter::All);
    auto all = b.Children();
    CHECK(std::find(all.begin(), all.end(), asset.GetPath()) != all.end());
    b.SetPrimFilter(Browser::PrimFilter::Loaded); auto loaded = b.Children();
    CHECK(std::find(loaded.begin(), loaded.end(), asset.GetPath()) == loaded.end());
    CHECK(!asset.IsLoaded());
}

struct PopulateArray {
    VtValue scalar;
    template<class T> VtValue operator()(const VtArray<T> &) const {
        return scalar.IsHolding<T>() ? VtValue(VtArray<T>{scalar.UncheckedGet<T>()}) : VtValue();
    }
    template<class T> VtValue operator()(const T &) const { return {}; }
};
void TestArrayTypes() {
    auto stage = UsdStage::CreateInMemory();
    auto prim = stage->DefinePrim(SdfPath("/Arrays"));
    Document d(stage); Browser b(d);
    size_t tested = 0;
    for (const auto &type : SdfSchema::GetInstance().GetAllTypes()) {
        if (!type.IsArray() || type.GetDefaultValue().IsEmpty()) continue;
        const auto value = VtVisitValueWithExtTypes<TfMetaList<VtArray<SdfAssetPath>, VtArray<SdfPathExpression>>>::VisitValue(
            type.GetDefaultValue(), PopulateArray{type.GetScalarType().GetDefaultValue()});
        if (value.IsEmpty()) std::fprintf(stderr, "Missing array type: %s (%s)\n", type.GetAsToken().GetText(), type.GetDefaultValue().GetTypeName().c_str());
        CHECK(!value.IsEmpty());
        if (value.IsEmpty()) continue;
        const std::string name = "array_" + std::to_string(tested++);
        CHECK(prim.CreateAttribute(TfToken(name), type).Set(value));
        const auto rows = b.PropertyDetails(prim.GetPath(), name);
        CHECK(rows.size() == 2);
        CHECK(rows.back().name == "[0]");
        CHECK(rows.back().value.find("unavailable") == std::string::npos);
    }
    CHECK(tested >= 30);
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
    CHECK(b.Properties(path, "SiZe").size() == 1);
    CHECK(!b.Properties(path, "int[]").empty());
    CHECK(b.Properties(path, "does not exist").empty());
    CHECK(Find(rows, "targets") && Find(rows, "targets")->type == "relationship");
    CHECK(b.PropertyDetailCount(path, "values") == 10000);
    CHECK(b.PropertyDetailCount(path, "size") == 2);
    CHECK(b.PropertyDetailCount(path, "targets") == 2);
    auto details = b.PropertyDetails(path, "values", 128, 16);
    CHECK(details.size() == 17); CHECK(details[1].name == "[128]"); CHECK(details[1].value == "128");
    CHECK(b.PropertyDetails(path, "values", 20000).size() == 1);
    CHECK(b.PropertyDetails(path, "values", 0, 10000).size() == 257);
    // A summary row, then one row per target.
    auto targetRows = b.PropertyDetails(path, "targets");
    CHECK(targetRows.size() == 3 && targetRows[0].value == "2 targets");
    CHECK(targetRows[1].value == "/Cube");
    cube.GetPrim().CreateRelationship(TfToken("empty"));
    targetRows = b.PropertyDetails(path, "empty");
    CHECK(targetRows.size() == 1 && targetRows[0].value == "No targets");
    CHECK(b.PropertyDetails(path, "size").size() == 4);
    auto metadata = b.Metadata(path);
    CHECK(Find(metadata, "customData.nested.answer"));
    CHECK(Find(metadata, "customData.nested.answer")->value == "42");
    CHECK(!b.Composition(path).empty()); CHECK(b.Layers().size() == 2);
    auto layerRows = b.Layers(); CHECK(layerRows.back().type == "edit target");
    d.SetEditTarget(stage->GetSessionLayer()); CHECK(b.Layers().front().type == "edit target");
    auto variantRows = b.Variants(path); CHECK(variantRows.size() == 1);
    CHECK(variantRows[0].value == "red"); CHECK(variantRows[0].source.find("blue") != std::string::npos);
    auto sizeMetadata = b.PropertyMetadata(path, "size");
    CHECK(Find(sizeMetadata, "typeName"));
    CHECK(b.PropertyMetadata(path, "missing").empty());
    // Opinion drill-down must show both stronger session and weaker root
    // authoring, without flattening or changing either layer.
    cube.GetSizeAttr().Set(12.0);
    std::string rootText; stage->GetRootLayer()->ExportToString(&rootText);
    std::string sessionText; stage->GetSessionLayer()->ExportToString(&sessionText);
    const auto opinions = b.PropertyOpinions(path, "size");
    CHECK(Find(opinions, "default") && Find(opinions, "default")->value == "12");
    size_t layerOpinions = 0;
    for (const auto &row : opinions) if (row.name == "Opinion") ++layerOpinions;
    CHECK(layerOpinions == 2);
    CHECK(b.PropertyOpinions(path, "missing").empty());
    const auto layerDetails = b.LayerDetails(stage->GetRootLayer()->GetIdentifier());
    CHECK(Find(layerDetails, "Identifier")); CHECK(Find(layerDetails, "Editable"));
    CHECK(b.LayerDetails("not a stage layer").empty());
    std::string after; stage->GetRootLayer()->ExportToString(&after); CHECK(after == rootText);
    stage->GetSessionLayer()->ExportToString(&after); CHECK(after == sessionText);
    CHECK(b.Properties(SdfPath("/Missing")).empty());
    CHECK(b.PropertyDetails(path, "missing").empty());
    auto splineAttr = cube.GetPrim().CreateAttribute(TfToken("curve"), SdfValueTypeNames->Double);
    TsSpline spline;
    TsKnot first; first.SetTime(1); first.SetValue(2.0);
    TsKnot last; last.SetTime(3); last.SetValue(6.0);
    spline.SetKnot(first); spline.SetKnot(last);
    CHECK(splineAttr.SetSpline(spline));
    const auto preview = b.SplinePreview(path, "curve", 10);
    CHECK(preview.samples.size() == 10); CHECK(preview.knots.size() == 2);
    CHECK(preview.samples.front() == GfVec2d(1, 2)); CHECK(preview.samples.back() == GfVec2d(3, 6));
    CHECK(b.SplinePreview(path, "curve", 100000).samples.size() == 1024);
    CHECK(b.SplinePreview(path, "size").samples.empty());
    CHECK(b.PropertyDetailCount(path, "curve") == 2);
    CHECK(b.PropertyDetails(path, "curve").size() == 3);
    CHECK(b.PropertyDetails(path, "curve", 1, 1).size() == 2);
}

// usdview's color coding: prim styles and fonts, value sources, italics
// and relationship targets.
void TestColorCoding() {
    auto stage = UsdStage::CreateInMemory();
    UsdGeomXform::Define(stage, SdfPath("/Plain"));
    stage->OverridePrim(SdfPath("/Over"));
    stage->CreateClassPrim(SdfPath("/Class"));
    auto ref = UsdGeomXform::Define(stage, SdfPath("/Ref")).GetPrim();
    ref.GetReferences().AddInternalReference(SdfPath("/Plain"));
    auto instance = UsdGeomXform::Define(stage, SdfPath("/Instance")).GetPrim();
    UsdGeomCube::Define(stage, SdfPath("/Proto/Child"));
    instance.GetReferences().AddInternalReference(SdfPath("/Proto"));
    instance.SetInstanceable(true);
    auto cube = UsdGeomCube::Define(stage, SdfPath("/Cube"));
    cube.CreateSizeAttr().Set(2.0);
    auto animated = cube.GetPrim().CreateAttribute(TfToken("animated"), SdfValueTypeNames->Double);
    animated.Set(1.0, UsdTimeCode(1)); animated.Set(3.0, UsdTimeCode(3));
    cube.GetPrim().CreateAttribute(TfToken("empty"), SdfValueTypeNames->Double);
    auto source = cube.GetPrim().CreateAttribute(TfToken("source"), SdfValueTypeNames->Double);
    auto connected = cube.GetPrim().CreateAttribute(TfToken("connected"), SdfValueTypeNames->Double);
    connected.AddConnection(source.GetPath());
    cube.GetPrim().CreateRelationship(TfToken("look")).SetTargets({SdfPath("/Plain")});

    Document d(stage); Browser b(d);
    CHECK(b.Summary(SdfPath("/Plain")).style == PrimStyle::Normal);
    CHECK(b.Summary(SdfPath("/Plain")).font == PrimFont::Defined);
    CHECK(b.Summary(SdfPath("/Over")).font == PrimFont::Over);
    CHECK(b.Summary(SdfPath("/Class")).font == PrimFont::Abstract);
    CHECK(b.Summary(SdfPath("/Ref")).style == PrimStyle::HasArcs);
    CHECK(b.Summary(SdfPath("/Instance")).style == PrimStyle::Instance);
    CHECK(b.Summary(SdfPath("/Instance/Child")).style == PrimStyle::Prototype);

    d.SetTime(UsdTimeCode(2));
    auto rows = b.Properties(SdfPath("/Cube"));
    CHECK(Find(rows, "size")->valueSource == ValueSource::Default && !Find(rows, "size")->interpolated);
    CHECK(Find(rows, "purpose")->valueSource == ValueSource::Fallback);
    CHECK(Find(rows, "animated")->valueSource == ValueSource::TimeSamples);
    CHECK(Find(rows, "animated")->interpolated);
    CHECK(Find(rows, "empty")->valueSource == ValueSource::None);
    CHECK(Find(rows, "look")->valueSource == ValueSource::Relationship);
    CHECK(Find(rows, "look")->targets == std::vector<std::string>{"/Plain"});
    CHECK(Find(rows, "connected")->targets == std::vector<std::string>{"/Cube.source"});
    d.SetTime(UsdTimeCode(3));
    CHECK(!Find(b.Properties(SdfPath("/Cube")), "animated")->interpolated);
    CHECK(ColorOf(ValueSource::Relationship) == (Rgb{230, 132, 131}));
    CHECK(PrimLegend().size() == 8 && PropertyLegend().size() == 8);
}

}

int main() { test::RegisterPlugins(); TestOutline(); TestFiltersAndPrototypes(); TestArrayTypes(); TestInspector(); TestColorCoding(); return test::Finish("browser"); }
