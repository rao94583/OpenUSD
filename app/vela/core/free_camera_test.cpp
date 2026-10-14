#include "free_camera.h"

#include "pxr/base/gf/frustum.h"
#include "pxr/base/gf/math.h"

#include <cmath>
#include <cstdio>

using namespace vela;

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
                         __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

static bool Near(const GfVec3d &a, const GfVec3d &b, double eps = 1e-6) {
    return (a - b).GetLength() < eps;
}

static bool Near(const GfMatrix4d &a, const GfMatrix4d &b, double eps = 1e-6) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (std::abs(a[i][j] - b[i][j]) > eps) return false;
    return true;
}

static const GfBBox3d NoBounds;

// Pixel position of a world point in a width x height view, y down.
static GfVec2d Project(const GfCamera &camera, const GfVec3d &point,
                       double width, double height) {
    const GfFrustum frustum = camera.GetFrustum();
    const GfVec3d ndc = (frustum.ComputeViewMatrix() *
                         frustum.ComputeProjectionMatrix()).Transform(point);
    return GfVec2d((ndc[0] + 1.0) * 0.5 * width,
                   (1.0 - ndc[1]) * 0.5 * height);
}

static void TestDefaultOrientation() {
    FreeCamera yUp(false);
    GfFrustum frustum = yUp.ComputeGfCamera(1.0, NoBounds).GetFrustum();
    CHECK(Near(frustum.GetPosition(), GfVec3d(0, 0, 100)));
    CHECK(Near(frustum.ComputeViewDirection(), GfVec3d(0, 0, -1)));
    CHECK(Near(frustum.ComputeUpVector(), GfVec3d(0, 1, 0)));

    FreeCamera zUp(true);
    frustum = zUp.ComputeGfCamera(1.0, NoBounds).GetFrustum();
    CHECK(Near(frustum.GetPosition(), GfVec3d(0, -100, 0)));
    CHECK(Near(frustum.ComputeViewDirection(), GfVec3d(0, 1, 0)));
    CHECK(Near(frustum.ComputeUpVector(), GfVec3d(0, 0, 1)));
}

static void TestTumbleStaysUprightAndOnSphere() {
    for (bool zUp : {false, true}) {
        const GfVec3d worldUp = zUp ? GfVec3d::ZAxis() : GfVec3d::YAxis();
        FreeCamera camera(zUp);
        camera.Frame(GfBBox3d(GfRange3d(GfVec3d(1, 2, 3), GfVec3d(4, 6, 5))));
        const GfVec3d center = camera.GetCenter();
        const double dist = camera.GetDistance();
        // 4 pixels per degree, as in usdview and Maya.
        camera.Drag(CameraDrag::Tumble, 148, -80, 600);
        const GfFrustum frustum =
            camera.ComputeGfCamera(1.5, NoBounds).GetFrustum();
        CHECK(Near(camera.GetCenter(), center));
        CHECK(std::abs((frustum.GetPosition() - center).GetLength() - dist)
              < 1e-6);
        CHECK(Near(frustum.GetPosition() +
                   dist * frustum.ComputeViewDirection(), center));
        // No roll: the right vector is horizontal.
        const GfVec3d right = GfCross(frustum.ComputeViewDirection(),
                                      frustum.ComputeUpVector());
        CHECK(std::abs(GfDot(right, worldUp)) < 1e-9);
        CHECK(GfDot(frustum.ComputeUpVector(), worldUp) > 0.0);
    }
    // A quarter turn around the up axis.
    FreeCamera camera(false);
    camera.Tumble(90, 0);
    const GfVec3d position =
        camera.ComputeGfCamera(1.0, NoBounds).GetFrustum().GetPosition();
    CHECK(std::abs(position[1]) < 1e-9);
    CHECK(std::abs(std::abs(position[0]) - 100.0) < 1e-9);
}

static void TestTrackKeepsPointUnderCursor() {
    for (bool zUp : {false, true}) {
        const double width = 800, height = 500;
        FreeCamera camera(zUp, 45.0);
        camera.Tumble(30, -20);
        const GfVec3d point = camera.GetCenter();
        const GfVec2d before = Project(
            camera.ComputeGfCamera(width / height, NoBounds), point,
            width, height);
        camera.Drag(CameraDrag::Track, 60, -25, height);
        const GfVec2d after = Project(
            camera.ComputeGfCamera(width / height, NoBounds), point,
            width, height);
        CHECK(std::abs(after[0] - before[0] - 60) < 1e-3);
        CHECK(std::abs(after[1] - before[1] + 25) < 1e-3);
    }
}

static void TestDolly() {
    FreeCamera camera(false);
    camera.Track(3, 4);
    const GfVec3d center = camera.GetCenter();
    const GfVec3d direction =
        camera.ComputeGfCamera(1.0, NoBounds).GetFrustum()
            .ComputeViewDirection();
    camera.Dolly(0.5);
    const GfFrustum frustum = camera.ComputeGfCamera(1.0, NoBounds).GetFrustum();
    CHECK(std::abs(camera.GetDistance() - 50.0) < 1e-9);
    CHECK(Near(camera.GetCenter(), center));
    CHECK(Near(frustum.ComputeViewDirection(), direction));
    CHECK(Near(frustum.GetPosition(), center - 50.0 * direction));
    // Dragging right or down dollies in; left or up dollies out.
    camera.Drag(CameraDrag::Dolly, 50, 0, 500);
    CHECK(camera.GetDistance() < 50.0);
    const double in = camera.GetDistance();
    camera.Drag(CameraDrag::Dolly, 0, -100, 500);
    CHECK(camera.GetDistance() > in);
    // Never reaches or passes the center of interest.
    for (int i = 0; i < 10000; ++i) camera.Drag(CameraDrag::Dolly, 1e6, 0, 1);
    CHECK(camera.GetDistance() > 0.0);
}

static void TestFrameFitsBounds() {
    const GfBBox3d bounds(GfRange3d(GfVec3d(-10, 0, 5), GfVec3d(30, 2, 9)),
                          GfMatrix4d(1.0).SetRotate(
                              GfRotation(GfVec3d(1, 1, 0), 30)));
    // Tall views (narrower horizontally) as well as square and wide ones.
    for (double aspect : {1.0, 0.5, 2.0})
    for (bool zUp : {false, true}) {
        for (double theta : {0.0, 45.0, 135.0}) {
            FreeCamera camera(zUp, 50.0);
            camera.Tumble(theta, -30);
            CHECK(camera.Frame(bounds, 1.1, aspect));
            CHECK(Near(camera.GetCenter(), bounds.ComputeCentroid()));
            const GfCamera gf = camera.ComputeGfCamera(aspect, bounds);
            const GfFrustum frustum = gf.GetFrustum();
            const GfMatrix4d viewProj = frustum.ComputeViewMatrix() *
                frustum.ComputeProjectionMatrix();
            double extent = 0.0;
            for (int i = 0; i < 8; ++i) {
                const GfVec3d corner =
                    bounds.GetMatrix().Transform(bounds.GetRange().GetCorner(i));
                const GfVec3d ndc = viewProj.Transform(corner);
                // In view, and between the clipping planes.
                CHECK(std::abs(ndc[0]) <= 1.0 && std::abs(ndc[1]) <= 1.0);
                CHECK(ndc[2] >= -1.0 && ndc[2] <= 1.0);
                extent = std::max({extent, std::abs(ndc[0]), std::abs(ndc[1])});
            }
            // Exact: the outermost corner touches the margin frameFit
            // leaves (1 / 1.1 of the view).
            CHECK(std::abs(extent - 1.0 / 1.1) < 1e-6);
        }
    }
    FreeCamera camera(false);
    CHECK(!camera.Frame(GfBBox3d()));
    CHECK(Near(camera.GetCenter(), GfVec3d(0.0)));
}

static void TestClippingRange() {
    const GfBBox3d bounds(GfRange3d(GfVec3d(-1), GfVec3d(1)));
    FreeCamera camera(false);
    camera.Frame(bounds);
    GfRange1f range = camera.ComputeGfCamera(1.0, bounds).GetClippingRange();
    const double dist = camera.GetDistance();
    CHECK(range.GetMin() > 0.0 && range.GetMin() < dist - 1.0);
    CHECK(range.GetMax() > dist + 1.0 && range.GetMax() < dist + 2.0);
    // From inside the bounds, near is small but keeps depth precision.
    camera.Dolly(1e-3);
    range = camera.ComputeGfCamera(1.0, bounds).GetClippingRange();
    CHECK(range.GetMin() > 0.0 && range.GetMin() < 1e-3);
    CHECK(range.GetMax() / range.GetMin() <= 1e6 * 1.0001);
    // No bounds: the defaults.
    range = camera.ComputeGfCamera(1.0, NoBounds).GetClippingRange();
    CHECK(range.GetMin() == float(FreeCamera::DefaultNear));
    CHECK(range.GetMax() == float(FreeCamera::DefaultFar));
}

static void TestFromGfCameraRoundTrip() {
    for (bool zUp : {false, true}) {
        FreeCamera camera(zUp, 35.0);
        camera.Track(5, -2);
        camera.Tumble(-70, 25);
        camera.Dolly(0.3);
        const GfCamera gf = camera.ComputeGfCamera(1.0, NoBounds);
        const FreeCamera copy = FreeCamera::FromGfCamera(gf, zUp, 1.0);
        CHECK(Near(copy.GetCenter(), camera.GetCenter()));
        CHECK(std::abs(copy.GetDistance() - camera.GetDistance()) < 1e-9);
        CHECK(std::abs(copy.GetFieldOfView() - 35.0) < 1e-4);
        CHECK(Near(copy.ComputeTransform(), camera.ComputeTransform()));
    }
    // A scene camera with no focus distance uses the fallback.
    GfCamera scene;
    scene.SetTransform(GfMatrix4d(1.0).SetTranslate(GfVec3d(0, 0, 10)));
    scene.SetFocusDistance(0.0);
    const FreeCamera copy = FreeCamera::FromGfCamera(scene, false, 4.0);
    CHECK(Near(copy.GetCenter(), GfVec3d(0, 0, 6)));
    CHECK(Near(copy.ComputeTransform(), scene.GetTransform()));
}

static void TestDragModes() {
    CHECK(CameraDragFor(MouseLeft, false, false) == CameraDrag::None);
    CHECK(CameraDragFor(MouseRight, false, false) == CameraDrag::None);
    CHECK(CameraDragFor(MouseLeft, true, false) == CameraDrag::Tumble);
    CHECK(CameraDragFor(MouseMiddle, true, false) == CameraDrag::Track);
    CHECK(CameraDragFor(MouseRight, true, false) == CameraDrag::Dolly);
    CHECK(CameraDragFor(MouseLeft | MouseMiddle, true, false) ==
          CameraDrag::Dolly);
    CHECK(CameraDragFor(MouseLeft, true, true) == CameraDrag::Track);
    CHECK(CameraDragFor(0, true, false) == CameraDrag::None);
}

int main() {
    TestDefaultOrientation();
    TestTumbleStaysUprightAndOnSphere();
    TestTrackKeepsPointUnderCursor();
    TestDolly();
    TestFrameFitsBounds();
    TestClippingRange();
    TestFromGfCameraRoundTrip();
    TestDragModes();
    if (failures) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("All free camera checks passed\n");
    return 0;
}
