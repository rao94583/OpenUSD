#include "hud.h"
#include "test_support.h"

#include "pxr/usd/usdGeom/basisCurves.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/xform.h"

using namespace vela;

namespace {

std::string Find(const HudLines &lines, const std::string &key) {
    for (const auto &[k, v] : lines) if (k == key) return v;
    return "<missing>";
}

void Test() {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomXform::Define(stage, SdfPath("/World"));
    // A quad and a triangle sharing two vertices: 4 distinct vertices.
    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/World/Mesh"));
    mesh.CreateFaceVertexCountsAttr(VtValue(VtIntArray{4, 3}));
    mesh.CreateFaceVertexIndicesAttr(VtValue(VtIntArray{0, 1, 2, 3, 0, 2, 3}));
    UsdGeomBasisCurves curves = UsdGeomBasisCurves::Define(stage, SdfPath("/World/Curves"));
    curves.CreateCurveVertexCountsAttr(VtValue(VtIntArray{4, 5}));
    stage->DefinePrim(SdfPath("/Loose"));
    UsdGeomCamera::Define(stage, SdfPath("/Cam"));

    HudStats all = ComputeHudStats(stage, {}, UsdTimeCode::Default());
    CHECK(all.prims == 5);
    CHECK(all.types["Mesh"] == 1 && all.types["BasisCurves"] == 1 && all.types["typeless"] == 1);
    CHECK(all.faces == 2 && all.verts == 4 && all.cvs == 9);
    HudStats one = ComputeHudStats(stage, {SdfPath("/World/Mesh")}, UsdTimeCode::Default());
    CHECK(one.prims == 1 && one.faces == 2 && one.cvs == 0);

    Document d(stage);
    HudRenderInfo render;
    render.renderMilliseconds = 4.0;
    render.renderStats["gpuMemoryUsed"] = VtValue(size_t(3 * 1024 * 1024));
    // usdview's defaults: renderer, performance, complexity; no info.
    Hud hud = ComposeHud(d, all, render);
    CHECK(hud.topLeft.empty());
    CHECK(Find(hud.topRight, "Hydra") == "Storm" && Find(hud.topRight, "  Hgi") == "Metal");
    CHECK(Find(hud.topRight, "  AOV") == "<missing>");
    CHECK(Find(hud.bottomLeft, "Render") == "4.00 ms (250.00 FPS)");
    CHECK(Find(hud.bottomLeft, "Playback") == "<missing>");
    CHECK(Find(hud.bottomRight, "Complexity") == "Low");
    CHECK(Find(hud.bottomRight, "Camera") == "Free");

    ViewSettings s = d.GetViewSettings();
    s.hudInfo = s.hudGPUStats = true;
    s.complexity = 1.3;
    d.SetViewSettings(s);
    d.SetViewCamera(SdfPath("/Cam"));
    render.aov = "depth";
    render.playbackMilliseconds = 40.0;
    hud = ComposeHud(d, all, render);
    CHECK(Find(hud.topLeft, "Prims") == "5" && Find(hud.topLeft, "Faces") == "2");
    CHECK(Find(hud.topLeft, "Mesh") == "1");
    CHECK(Find(hud.topRight, "  AOV") == "depth");
    CHECK(Find(hud.bottomLeft, "GPU mem") == "3.0 MB");
    CHECK(Find(hud.bottomLeft, "Playback") == "40.00 ms (25.00 FPS)");
    CHECK(Find(hud.bottomRight, "Complexity") == "Very High");
    CHECK(Find(hud.bottomRight, "Camera") == "Cam");

    s.showHUD = false;
    d.SetViewSettings(s);
    hud = ComposeHud(d, all, render);
    CHECK(hud.topLeft.empty() && hud.topRight.empty() && hud.bottomLeft.empty() &&
          hud.bottomRight.empty());
}

} // namespace

int main() {
    test::RegisterPlugins();
    Test();
    return test::Finish("HUD");
}
