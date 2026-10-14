#include "document.h"
#include "test_support.h"

#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/usd/editContext.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/base/gf/frustum.h"
#include "pxr/base/gf/range2d.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usd/usdGeom/cube.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/xformCommonAPI.h"

using namespace vela;

namespace {

// Two cubes, ten frames, a sublayer.
UsdStageRefPtr MakeStage(bool animated = true) {
    SdfLayerRefPtr sub = SdfLayer::CreateAnonymous("sub.usda");
    SdfLayerRefPtr root = SdfLayer::CreateAnonymous("root.usda");
    root->SetSubLayerPaths({sub->GetIdentifier()});
    UsdStageRefPtr stage = UsdStage::Open(root);
    UsdGeomSetStageUpAxis(stage, UsdGeomTokens->z);
    if (animated) {
        stage->SetStartTimeCode(1);
        stage->SetEndTimeCode(10);
    }
    UsdGeomCube::Define(stage, SdfPath("/A"));
    UsdGeomCube b = UsdGeomCube::Define(stage, SdfPath("/B"));
    UsdGeomXformCommonAPI(b).SetTranslate(GfVec3d(10, 0, 0));
    return stage;
}

void TestOpen() {
    std::string error;
    CHECK(!Document::Open("/nonexistent/scene.usda", &error));
    CHECK(error.find("Could not open") == 0);
}

void TestDefaults() {
    Document d(MakeStage());
    CHECK(d.GetCamera().IsZUp());
    CHECK(d.IsAnimated());
    CHECK(d.GetTime() == UsdTimeCode(1));
    CHECK(d.GetSelection().empty());
    // Opening frames everything.
    CHECK((d.GetCamera().GetCenter() - GfVec3d(5, 0, 0)).GetLength() < 1e-9);
    // Every layer in use records undo.
    for (const SdfLayerHandle &layer : d.GetStage()->GetUsedLayers()) {
        CHECK(d.GetUndoStack().IsTracking(layer));
    }

    Document still(MakeStage(false));
    CHECK(!still.IsAnimated());
    CHECK(still.GetTime() == UsdTimeCode::EarliestTime());
    still.StepTime(1);
    CHECK(still.GetTime() == UsdTimeCode::EarliestTime());
    still.SetPlaying(true);
    CHECK(!still.IsPlaying());
}

void TestObserversAndSelection() {
    Document d(MakeStage());
    unsigned seen = 0;
    int calls = 0;
    const int id = d.AddObserver([&](unsigned changes) { seen |= changes; ++calls; });
    d.SetSelection({SdfPath("/A"), SdfPath("/B"), SdfPath("/A")});
    CHECK(seen == Document::Selection);
    CHECK((d.GetSelection() == SdfPathVector{SdfPath("/A"), SdfPath("/B")}));
    // No change, no notification.
    calls = 0;
    d.SetSelection({SdfPath("/A"), SdfPath("/B")});
    CHECK(calls == 0);

    // Removing a selected prim drops it from the selection.
    seen = 0;
    d.GetStage()->RemovePrim(SdfPath("/A"));
    CHECK(seen & Document::StageContent);
    CHECK(seen & Document::Selection);
    CHECK(d.GetSelection() == SdfPathVector{SdfPath("/B")});

    d.RemoveObserver(id);
    calls = 0;
    d.SetSelection({});
    CHECK(calls == 0);
}

void TestTime() {
    Document d(MakeStage());
    unsigned seen = 0;
    d.AddObserver([&](unsigned changes) { seen |= changes; });
    d.StepTime(3);
    CHECK(d.GetTime() == UsdTimeCode(4));
    CHECK(seen == Document::Time);
    d.StepTime(7);    // 11 wraps to 1
    CHECK(d.GetTime() == UsdTimeCode(1));
    d.StepTime(-1);   // wraps to 10
    CHECK(d.GetTime() == UsdTimeCode(10));
    d.StepTime(-23);  // 10 - 23 = -13, wraps to 7
    CHECK(d.GetTime() == UsdTimeCode(7));
    d.SetTime(UsdTimeCode(4.5));
    d.StepTime(1);    // steps from the nearest frame
    CHECK(d.GetTime() == UsdTimeCode(6));

    seen = 0;
    d.SetPlaying(true);
    CHECK(d.IsPlaying());
    CHECK(seen == Document::Playback);
}

void TestFramingAndBounds() {
    Document d(MakeStage());
    unsigned seen = 0;
    d.AddObserver([&](unsigned changes) { seen |= changes; });
    d.SetSelection({SdfPath("/B")});
    d.FrameSelection();
    CHECK(seen & Document::Camera);
    CHECK((d.GetCamera().GetCenter() - GfVec3d(10, 0, 0)).GetLength() < 1e-9);
    d.FrameAll();
    CHECK((d.GetCamera().GetCenter() - GfVec3d(5, 0, 0)).GetLength() < 1e-9);
    // Stage edits invalidate the cached bounds.
    const GfRange3d before = d.GetStageBounds().ComputeAlignedRange();
    UsdGeomXformCommonAPI(d.GetStage()->GetPrimAtPath(SdfPath("/B")))
        .SetTranslate(GfVec3d(20, 0, 0));
    const GfRange3d after = d.GetStageBounds().ComputeAlignedRange();
    CHECK(before.GetMax()[0] == 11.0);
    CHECK(after.GetMax()[0] == 21.0);
    // Empty selection frames everything.
    d.SetSelection({});
    d.FrameSelection();
    CHECK((d.GetCamera().GetCenter() - GfVec3d(10, 0, 0)).GetLength() < 1e-9);
}

void TestEditTarget() {
    Document d(MakeStage());
    const SdfLayerHandle sub = d.GetStage()->GetLayerStack()[2];  // session, root, sub
    CHECK(sub->GetIdentifier().find("sub.usda") != std::string::npos);
    unsigned seen = 0;
    d.AddObserver([&](unsigned changes) { seen |= changes; });
    CHECK(d.SetEditTarget(sub));
    CHECK(d.GetEditTarget() == sub);
    CHECK(seen == Document::EditTarget);
    std::string error;
    SdfLayerRefPtr outside = SdfLayer::CreateAnonymous();
    CHECK(!d.SetEditTarget(outside, &error));
    CHECK(error.find("Not a layer") == 0);
    CHECK(d.GetEditTarget() == sub);

    // Edits through the target record undo; undo notifies.
    seen = 0;
    {
        UndoBlock block(d.GetUndoStack(), "Define");
        UsdGeomCube::Define(d.GetStage(), SdfPath("/C"));
    }
    CHECK(sub->GetPrimAtPath(SdfPath("/C")));
    CHECK(seen & Document::UndoState);
    CHECK(d.GetUndoStack().Undo());
    CHECK(!d.GetStage()->GetPrimAtPath(SdfPath("/C")));

    // Layers added later are tracked too.
    SdfLayerRefPtr extra = SdfLayer::CreateAnonymous("extra.usda");
    d.GetStage()->GetRootLayer()->InsertSubLayerPath(extra->GetIdentifier());
    CHECK(d.GetUndoStack().IsTracking(extra));
}

bool Near(const GfMatrix4d &a, const GfMatrix4d &b, double eps = 1e-6) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (std::abs(a[i][j] - b[i][j]) > eps) return false;
    return true;
}

void TestSceneCameras() {
    UsdStageRefPtr stage = MakeStage();
    UsdGeomCamera shot = UsdGeomCamera::Define(stage, SdfPath("/Shot"));
    // Moves from z = 10 to z = 20 over the time range, looking down -Z.
    auto translate = shot.AddTranslateOp();
    translate.Set(GfVec3d(0, 0, 10), UsdTimeCode(1));
    translate.Set(GfVec3d(0, 0, 20), UsdTimeCode(10));
    shot.CreateFocusDistanceAttr(VtValue(0.0f));
    Document d(stage);
    CHECK(d.GetSceneCameras() == SdfPathVector{SdfPath("/Shot")});
    CHECK(d.GetViewCamera().IsEmpty());

    std::string error;
    CHECK(!d.SetViewCamera(SdfPath("/A"), &error));
    CHECK(error == "No camera at /A");
    unsigned seen = 0;
    d.AddObserver([&](unsigned changes) { seen |= changes; });
    CHECK(d.SetViewCamera(SdfPath("/Shot")));
    CHECK(seen & Document::Camera);

    // Through the scene camera, following its animation.
    GfCamera camera = d.ComputeViewGfCamera(2.0);
    CHECK(Near(camera.GetTransform(), shot.GetCamera(UsdTimeCode(1)).GetTransform()));
    d.SetTime(UsdTimeCode(10));
    camera = d.ComputeViewGfCamera(2.0);
    CHECK(Near(camera.GetTransform(), shot.GetCamera(UsdTimeCode(10)).GetTransform()));
    // Fitted to the viewport: the window has its aspect ratio and contains
    // the camera's whole aperture.
    const GfRange2d window = camera.GetFrustum().GetWindow();
    CHECK(std::abs(window.GetSize()[0] / window.GetSize()[1] - 2.0) < 1e-6);
    const GfCamera original = shot.GetCamera(UsdTimeCode(10));
    CHECK(camera.GetHorizontalAperture() >= original.GetHorizontalAperture() - 1e-4);
    CHECK(camera.GetVerticalAperture() >= original.GetVerticalAperture() - 1e-4);

    // Detaching keeps the view and leaves the scene camera alone.
    std::string before;
    stage->GetRootLayer()->ExportToString(&before);
    d.SetViewportAspectRatio(1.5);
    const GfCamera through = d.ComputeViewGfCamera(1.5);
    d.DetachViewCamera();
    CHECK(d.GetViewCamera().IsEmpty());
    const GfCamera detached = d.ComputeViewGfCamera(1.5);
    CHECK(Near(detached.GetTransform(), through.GetTransform()));
    CHECK(std::abs(detached.GetFieldOfView(GfCamera::FOVVertical) -
                   through.GetFieldOfView(GfCamera::FOVVertical)) < 1e-3);
    // With no focus distance, the center of interest is at the stage
    // center's depth: z = 20 to the cubes' center plane at z = 0.
    CHECK(std::abs(d.GetCamera().GetDistance() - 20.0) < 1e-6);
    d.GetCamera().Tumble(30, 0);
    d.CameraChanged();
    std::string after;
    stage->GetRootLayer()->ExportToString(&after);
    CHECK(after == before);

    // Framing detaches too.
    d.SetViewCamera(SdfPath("/Shot"));
    d.FrameAll();
    CHECK(d.GetViewCamera().IsEmpty());

    // Deleting the camera returns to the free camera.
    d.SetViewCamera(SdfPath("/Shot"));
    stage->RemovePrim(SdfPath("/Shot"));
    CHECK(d.GetViewCamera().IsEmpty());
    CHECK(d.GetSceneCameras().empty());
}

} // namespace

int main() {
    test::RegisterPlugins();
    TestOpen();
    TestDefaults();
    TestObserversAndSelection();
    TestTime();
    TestFramingAndBounds();
    TestEditTarget();
    TestSceneCameras();
    return test::Finish("document");
}
