// Undo and redo round trips: after undoing, every layer's text matches what
// it was before the edits; after redoing, what it was after them.
#include "undo.h"
#include "test_support.h"

#include "pxr/usd/sdf/attributeSpec.h"
#include "pxr/usd/sdf/namespaceEdit.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/editContext.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/variantSets.h"
#include "pxr/usd/usdGeom/sphere.h"
#include "pxr/usd/usdGeom/xform.h"

#include <map>
#include <string>
#include <vector>

using namespace vela;

namespace {

// A stage with a root layer, a sublayer and the session layer, all tracked.
struct Fixture {
    SdfLayerRefPtr root = SdfLayer::CreateAnonymous("root.usda");
    SdfLayerRefPtr sub = SdfLayer::CreateAnonymous("sub.usda");
    UsdStageRefPtr stage;
    UndoStack undo;

    Fixture() {
        root->SetSubLayerPaths({sub->GetIdentifier()});
        stage = UsdStage::Open(root, SdfLayer::CreateAnonymous("session.usda"));
        // Content that edits can change and delete.
        {
            UsdEditContext context(stage, sub);
            UsdGeomXform world = UsdGeomXform::Define(stage, SdfPath("/World"));
            UsdGeomSphere ball = UsdGeomSphere::Define(stage, SdfPath("/World/Ball"));
            ball.CreateRadiusAttr().Set(2.0);
            ball.GetRadiusAttr().Set(1.0, UsdTimeCode(1));
            ball.GetRadiusAttr().Set(3.0, UsdTimeCode(10));
            ball.GetPrim().SetCustomDataByKey(TfToken("note"), VtValue("hello"));
            ball.GetPrim().CreateRelationship(TfToken("target"))
                .AddTarget(SdfPath("/World"));
            UsdVariantSet shape = world.GetPrim().GetVariantSets().AddVariantSet("shape");
            shape.AddVariant("round");
            shape.SetVariantSelection("round");
            {
                UsdEditContext variant(shape.GetVariantEditContext());
                UsdGeomSphere::Define(stage, SdfPath("/World/Extra"));
            }
        }
        for (const SdfLayerHandle &layer : stage->GetLayerStack()) {
            undo.Track(layer);
        }
    }

    std::map<std::string, std::string> Snapshot() const {
        std::map<std::string, std::string> text;
        for (const SdfLayerHandle &layer : stage->GetLayerStack()) {
            layer->ExportToString(&text[layer->GetIdentifier()]);
        }
        return text;
    }

    // Undo restores before, redo restores after, twice over.
    void CheckRoundTrip(const std::map<std::string, std::string> &before,
                        const char *what) {
        const auto after = Snapshot();
        if (after == before) {
            std::fprintf(stderr, "%s: the edit changed nothing\n", what);
            ++test::failures;
        }
        for (int i = 0; i < 2; ++i) {
            CHECK(undo.Undo());
            if (Snapshot() != before) {
                std::fprintf(stderr, "%s: undo did not restore the layers\n", what);
                ++test::failures;
            }
            CHECK(undo.Redo());
            if (Snapshot() != after) {
                std::fprintf(stderr, "%s: redo did not restore the edit\n", what);
                ++test::failures;
            }
        }
    }
};

UsdAttribute Radius(const UsdStageRefPtr &stage) {
    return UsdGeomSphere::Get(stage, SdfPath("/World/Ball")).GetRadiusAttr();
}

void TestAttributeValue() {
    Fixture f;
    const auto before = f.Snapshot();
    {
        UndoBlock block(f.undo, "Set Radius");
        UsdEditContext context(f.stage, f.sub);
        Radius(f.stage).Set(5.0);
    }
    CHECK(f.undo.GetUndoName() == "Set Radius");
    f.CheckRoundTrip(before, "attribute value");
    double radius = 0;
    Radius(f.stage).Get(&radius);
    CHECK(radius == 5.0);
}

void TestTimeSamples() {
    Fixture f;
    const auto before = f.Snapshot();
    {
        UndoBlock block(f.undo, "Animate");
        UsdEditContext context(f.stage, f.sub);
        Radius(f.stage).Set(7.0, UsdTimeCode(10));   // replaces a sample
        Radius(f.stage).Set(4.0, UsdTimeCode(5));    // adds one
        f.sub->EraseTimeSample(Radius(f.stage).GetPath(), 1.0);
    }
    f.CheckRoundTrip(before, "time samples");
}

void TestCreatePrims() {
    Fixture f;
    const auto before = f.Snapshot();
    {
        UndoBlock block(f.undo, "Create");
        UsdEditContext context(f.stage, f.sub);
        UsdGeomXform group = UsdGeomXform::Define(f.stage, SdfPath("/World/Group"));
        UsdGeomSphere::Define(f.stage, SdfPath("/World/Group/A")).CreateRadiusAttr(VtValue(0.5));
        UsdGeomSphere::Define(f.stage, SdfPath("/World/Group/B"));
        group.AddTranslateOp().Set(GfVec3d(1, 2, 3));
    }
    f.CheckRoundTrip(before, "create prims");
    CHECK(f.stage->GetPrimAtPath(SdfPath("/World/Group/B")));
}

void TestDeletePrims() {
    Fixture f;
    const auto before = f.Snapshot();
    {
        UndoBlock block(f.undo, "Delete");
        UsdEditContext context(f.stage, f.sub);
        // A subtree with attributes, samples, metadata, relationships and a
        // variant set.
        f.stage->RemovePrim(SdfPath("/World"));
    }
    CHECK(!f.stage->GetPrimAtPath(SdfPath("/World/Ball")));
    f.CheckRoundTrip(before, "delete prims");
    CHECK(!f.stage->GetPrimAtPath(SdfPath("/World")));
    CHECK(f.undo.Undo());
    CHECK(f.stage->GetPrimAtPath(SdfPath("/World/Extra")));
}

void TestRenameAndReparent() {
    Fixture f;
    const auto before = f.Snapshot();
    {
        UndoBlock block(f.undo, "Rename");
        SdfPrimSpecHandle ball = f.sub->GetPrimAtPath(SdfPath("/World/Ball"));
        CHECK(ball->SetName("Sphere"));
        SdfBatchNamespaceEdit edits;
        edits.Add(SdfPath("/World/Sphere"), SdfPath("/Sphere"));
        CHECK(f.sub->Apply(edits));
    }
    CHECK(f.stage->GetPrimAtPath(SdfPath("/Sphere")));
    f.CheckRoundTrip(before, "rename and reparent");
}

void TestMetadataAndDictionaries() {
    Fixture f;
    const auto before = f.Snapshot();
    {
        UndoBlock block(f.undo, "Metadata");
        UsdEditContext context(f.stage, f.sub);
        UsdPrim ball = f.stage->GetPrimAtPath(SdfPath("/World/Ball"));
        ball.SetCustomDataByKey(TfToken("note"), VtValue("changed"));
        ball.SetCustomDataByKey(TfToken("nested:key"), VtValue(3));
        ball.ClearCustomDataByKey(TfToken("note"));
        ball.SetMetadata(SdfFieldKeys->Documentation, std::string("A ball"));
        UsdEditContext rootContext(f.stage, f.stage->GetRootLayer());
        f.stage->SetStartTimeCode(1);
        f.stage->SetEndTimeCode(48);
        f.stage->GetPrimAtPath(SdfPath("/World")).GetVariantSet("shape")
            .AddVariant("square");
    }
    f.CheckRoundTrip(before, "metadata");
}

// One step spans the root layer, a sublayer and the session layer.
void TestAcrossLayers() {
    Fixture f;
    const auto before = f.Snapshot();
    {
        UndoBlock block(f.undo, "Across Layers");
        {
            UsdEditContext context(f.stage, f.sub);
            Radius(f.stage).Set(8.0);
        }
        {
            UsdEditContext context(f.stage, f.stage->GetRootLayer());
            Radius(f.stage).Set(9.0);
            UsdGeomSphere::Define(f.stage, SdfPath("/World/RootOnly"));
        }
        {
            UsdEditContext context(f.stage, f.stage->GetSessionLayer());
            f.stage->GetPrimAtPath(SdfPath("/World/Ball")).SetActive(false);
            f.stage->RemovePrim(SdfPath("/World/RootOnly"));
        }
        f.stage->GetRootLayer()->SetDocumentation("edited");
    }
    for (const auto &[layer, text] : f.Snapshot()) {
        if (text == before.at(layer)) {
            std::fprintf(stderr, "across layers: %s was not edited\n", layer.c_str());
            ++test::failures;
        }
    }
    f.CheckRoundTrip(before, "across layers");
    CHECK(f.undo.Undo());
    CHECK(!f.undo.CanUndo());
    CHECK(f.stage->GetPrimAtPath(SdfPath("/World/Ball")).IsActive());
}

// Clearing and replacing a whole (non-streaming, text) layer's content.
void TestWholeLayer() {
    Fixture f;
    const auto before = f.Snapshot();
    {
        UndoBlock block(f.undo, "Clear");
        f.sub->Clear();
    }
    CHECK(!f.stage->GetPrimAtPath(SdfPath("/World/Ball")));
    f.CheckRoundTrip(before, "clear layer");
    CHECK(f.undo.Undo());
    const auto restored = f.Snapshot();
    {
        UndoBlock block(f.undo, "Replace");
        SdfLayerRefPtr other = SdfLayer::CreateAnonymous("other.usda");
        SdfCreatePrimInLayer(other, SdfPath("/Other/Thing"));
        f.sub->TransferContent(other);
    }
    CHECK(f.stage->GetPrimAtPath(SdfPath("/Other/Thing")));
    f.CheckRoundTrip(restored, "transfer content");
}

void TestSequenceAndGrouping() {
    Fixture f;
    const auto start = f.Snapshot();
    UsdEditContext context(f.stage, f.sub);
    for (int i = 0; i < 5; ++i) {
        UndoBlock outer(f.undo, "Step " + std::to_string(i));
        Radius(f.stage).Set(10.0 + i);
        {
            // Nested groups fold into the outer step.
            UndoBlock inner(f.undo, "Inner");
            UsdGeomSphere::Define(f.stage, SdfPath("/World/S" + std::to_string(i)));
        }
        CHECK(!f.undo.CanUndo());  // inside a group
    }
    CHECK(f.undo.GetUndoName() == "Step 4");
    for (int i = 4; i >= 0; --i) {
        CHECK(f.undo.GetUndoName() == "Step " + std::to_string(i));
        CHECK(f.undo.Undo());
    }
    CHECK(!f.undo.CanUndo());
    CHECK(f.Snapshot() == start);
    for (int i = 0; i < 5; ++i) CHECK(f.undo.Redo());
    CHECK(!f.undo.CanRedo());
    CHECK(f.stage->GetPrimAtPath(SdfPath("/World/S4")));

    // Edits outside a group form one implicit step, ended by the next group.
    CHECK(f.undo.Undo());
    CHECK(f.undo.CanRedo());
    Radius(f.stage).Set(20.0);
    Radius(f.stage).Set(21.0);
    CHECK(!f.undo.CanRedo());  // new edits discard the redo step
    CHECK(f.undo.GetUndoName() == "Edit");
    { UndoBlock block(f.undo, "After"); Radius(f.stage).Set(22.0); }
    CHECK(f.undo.GetUndoName() == "After");
    CHECK(f.undo.Undo());
    CHECK(f.undo.GetUndoName() == "Edit");
    CHECK(f.undo.Undo());
    double radius = 0;
    Radius(f.stage).Get(&radius);
    CHECK(radius == 13.0);

    // An empty group adds no step.
    const std::string name = f.undo.GetUndoName();
    { UndoBlock block(f.undo, "Nothing"); }
    CHECK(f.undo.GetUndoName() == name);
}

// Claude's requests: the steps a request adds become one, and a query's
// edits are reverted without a redo step.
void TestCoalesceAndDiscard() {
    Fixture f;
    UsdEditContext context(f.stage, f.sub);
    { UndoBlock block(f.undo, "Before"); Radius(f.stage).Set(7.0); }
    const auto before = f.Snapshot();

    uint64_t mark = f.undo.Mark();
    CHECK(!f.undo.Coalesce(mark, "Nothing"));
    for (int i = 0; i < 3; ++i) {
        UndoBlock block(f.undo, "Python");
        UsdGeomSphere::Define(f.stage, SdfPath("/World/C" + std::to_string(i)));
    }
    Radius(f.stage).Set(8.0);  // an implicit step, ended by Coalesce
    CHECK(f.undo.Coalesce(mark, "Claude: add spheres"));
    CHECK(f.undo.GetUndoName() == "Claude: add spheres");
    f.CheckRoundTrip(before, "coalesced request");
    CHECK(f.undo.Undo());
    CHECK(f.Snapshot() == before);
    CHECK(f.undo.GetUndoName() == "Before");
    CHECK(f.undo.Redo());

    // Undoing below the mark during a request leaves those steps alone.
    mark = f.undo.Mark();
    { UndoBlock block(f.undo, "One"); Radius(f.stage).Set(9.0); }
    CHECK(f.undo.Undo());
    CHECK(f.undo.Undo());  // the earlier request
    { UndoBlock block(f.undo, "Two"); Radius(f.stage).Set(10.0); }
    CHECK(f.undo.Coalesce(mark, "Claude: radius"));
    CHECK(f.undo.GetUndoName() == "Claude: radius");
    CHECK(f.undo.Undo());
    CHECK(f.undo.GetUndoName() == "Before");
    CHECK(f.Snapshot() == before);

    // Discard reverts and forgets.
    mark = f.undo.Mark();
    CHECK(!f.undo.Discard(mark));
    { UndoBlock block(f.undo, "Query"); UsdGeomSphere::Define(f.stage, SdfPath("/World/Q")); }
    CHECK(f.undo.Discard(mark));
    CHECK(!f.stage->GetPrimAtPath(SdfPath("/World/Q")));
    CHECK(f.Snapshot() == before);
    CHECK(f.undo.GetUndoName() == "Before");
    CHECK(!f.undo.CanRedo());
}

void TestUntrackedAndCleanup() {
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("loose.usda");
    {
        UndoStack undo;
        undo.Track(layer);
        undo.Track(layer);  // tracking twice is harmless
        CHECK(undo.IsTracking(layer));
        CHECK(!layer->IsDirty());
        SdfCreatePrimInLayer(layer, SdfPath("/A"));
        CHECK(layer->IsDirty());
        CHECK(undo.Undo());
        CHECK(!layer->GetPrimAtPath(SdfPath("/A")));
    }
    // After the stack is gone the layer still works and stays dirty.
    CHECK(layer->IsDirty());
    SdfCreatePrimInLayer(layer, SdfPath("/B"));
    CHECK(layer->GetPrimAtPath(SdfPath("/B")));

    // Changes flow to the callback.
    UndoStack undo;
    int changes = 0;
    undo.SetChangedCallback([&] { ++changes; });
    undo.Track(layer);
    { UndoBlock block(undo, "C"); SdfCreatePrimInLayer(layer, SdfPath("/C")); }
    undo.Undo();
    undo.Redo();
    undo.Clear();
    CHECK(changes == 4);
    CHECK(!undo.CanUndo() && !undo.CanRedo());
}

} // namespace

int main() {
    test::RegisterPlugins();
    TestAttributeValue();
    TestTimeSamples();
    TestCreatePrims();
    TestDeletePrims();
    TestRenameAndReparent();
    TestMetadataAndDictionaries();
    TestAcrossLayers();
    TestWholeLayer();
    TestSequenceAndGrouping();
    TestCoalesceAndDiscard();
    TestUntrackedAndCleanup();
    return test::Finish("undo");
}
