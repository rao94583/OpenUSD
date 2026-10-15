#include "launch_options.h"
#include "test_support.h"

#include "pxr/base/work/threadLimits.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/payloads.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usd/usdGeom/cube.h"

#include <cstdlib>
#include <filesystem>

using namespace vela;
namespace fs = std::filesystem;

namespace {

LaunchOptions Parse(std::vector<std::string> args, std::string *error = nullptr) {
    LaunchOptions options;
    std::string message;
    const bool ok = ParseLaunchOptions(args, &options, &message);
    if (error) *error = ok ? "" : message;
    return options;
}

void TestParse() {
    std::string error;
    LaunchOptions o = Parse({"scene.usda", "--select", "/World/A", "--camera", "shot",
                             "--mask", "/World/A,/World/B", "--unloaded",
                             "--sessionLayer", "s.usda", "--mute", "a.usda,b.usda",
                             "--complexity", "high", "--ff", "2", "--lf", "8", "--cf", "4.5",
                             "--numThreads", "-2", "--viewportSize", "640x480",
                             "--norender", "--bboxStandin", "--timing", "--quitAfterStartup",
                             "--clearsettings", "--config", "name"}, &error);
    CHECK(error.empty());
    CHECK(o.file == "scene.usda" && o.select == SdfPath("/World/A") && o.camera == "shot");
    CHECK((o.mask == std::vector<SdfPath>{SdfPath("/World/A"), SdfPath("/World/B")}));
    CHECK(o.unloaded && o.sessionLayer == "s.usda" && o.mute.size() == 2);
    CHECK(std::abs(*o.complexity - 1.2) < 1e-9);
    CHECK(*o.firstFrame == 2 && *o.lastFrame == 8 && *o.currentFrame == 4.5);
    CHECK(o.numThreads == -2 && o.viewportWidth == 640 && o.viewportHeight == 480);
    CHECK(o.norender && o.bboxStandin && o.timing && o.quitAfterStartup);
    CHECK(o.clearSettings && !o.defaultSettings);
    CHECK(Parse({"--defaultsettings"}, &error).defaultSettings);

    Parse({"a.usda", "--select", "World"}, &error);
    CHECK(error.find("absolute") != std::string::npos);
    Parse({"a.usda", "--complexity", "max"}, &error);
    CHECK(error.find("--complexity") == 0);
    Parse({"a.usda", "--ff", "x"}, &error);
    CHECK(error.find("needs a number") != std::string::npos);
    Parse({"a.usda", "--ff", "5", "--lf", "2"}, &error);
    CHECK(error == "--lf is before --ff");
    Parse({"a.usda", "--viewportSize", "640"}, &error);
    CHECK(error.find("WIDTHxHEIGHT") != std::string::npos);
    Parse({"a.usda", "--memstats"}, &error);
    CHECK(error == "--memstats is not supported by Vela");
    Parse({"a.usda", "--bogus"}, &error);
    CHECK(error == "Unknown option --bogus");
    Parse({"a.usda", "b.usda"}, &error);
    CHECK(error.find("Only one file") == 0);
    Parse({"a.usda", "--select"}, &error);
    CHECK(error == "--select needs a value");
}

// /World with A, B (a payload) and cameras main_cam and shot.
fs::path MakeScene(const fs::path &dir) {
    fs::create_directories(dir);
    UsdStageRefPtr asset = UsdStage::CreateNew((dir / "asset.usda").string());
    UsdGeomCube::Define(asset, SdfPath("/Asset/Box"));
    asset->GetRootLayer()->Save();
    UsdStageRefPtr stage = UsdStage::CreateNew((dir / "scene.usda").string());
    stage->SetStartTimeCode(1);
    stage->SetEndTimeCode(10);
    UsdGeomCube::Define(stage, SdfPath("/World/A"));
    stage->DefinePrim(SdfPath("/World/B")).GetPayloads().AddPayload(
        "asset.usda", SdfPath("/Asset"));
    UsdGeomCamera::Define(stage, SdfPath("/Cameras/main_cam"));
    UsdGeomCamera::Define(stage, SdfPath("/Cameras/Shots/shot"));
    stage->GetRootLayer()->Save();
    SdfLayerRefPtr session = SdfLayer::CreateNew((dir / "session.usda").string());
    session->SetComment("session");
    session->Save();
    return dir / "scene.usda";
}

void TestOpenAndApply(const fs::path &dir) {
    const fs::path scene = MakeScene(dir);
    std::string error;

    // Defaults: everything loaded, through main_cam, as usdview.
    LaunchOptions o = Parse({scene.string()});
    auto d = OpenDocument(o, &error);
    CHECK(d && error.empty());
    CHECK(d->GetStage()->GetPrimAtPath(SdfPath("/World/B/Box")));
    CHECK(ApplyLaunchOptions(o, *d).empty());
    CHECK(d->GetViewCamera() == SdfPath("/Cameras/main_cam"));

    // Stage options.
    o = Parse({scene.string(), "--mask", "/World/B", "--unloaded",
               "--sessionLayer", (dir / "session.usda").string()});
    d = OpenDocument(o, &error);
    CHECK(d);
    CHECK(!d->GetStage()->GetPrimAtPath(SdfPath("/World/A")));  // masked out
    CHECK(d->GetStage()->GetPrimAtPath(SdfPath("/World/B")));
    CHECK(!d->GetStage()->GetPrimAtPath(SdfPath("/World/B/Box")));  // unloaded
    CHECK(d->GetStage()->GetSessionLayer()->GetComment() == "session");

    // The view: camera by name, frame range, current frame, complexity,
    // selection (framing leaves the scene camera).
    o = Parse({scene.string(), "--camera", "shot", "--ff", "3", "--lf", "6",
               "--cf", "5", "--complexity", "veryhigh"});
    d = OpenDocument(o, &error);
    CHECK(ApplyLaunchOptions(o, *d).empty());
    CHECK(d->GetViewCamera() == SdfPath("/Cameras/Shots/shot"));
    CHECK(d->GetStartTime() == 3 && d->GetEndTime() == 6);
    CHECK(d->GetTime() == UsdTimeCode(5));
    d->StepTime(2);  // wraps within the playback range
    CHECK(d->GetTime() == UsdTimeCode(3));
    CHECK(std::abs(d->GetViewSettings().complexity - 1.3) < 1e-9);
    o = Parse({scene.string(), "--select", "/World/A"});
    d = OpenDocument(o, &error);
    CHECK(ApplyLaunchOptions(o, *d).empty());
    CHECK(d->GetSelection() == SdfPathVector{SdfPath("/World/A")});
    CHECK(d->GetViewCamera().IsEmpty());

    // Missing camera or prim: warnings, and the free camera.
    o = Parse({scene.string(), "--camera", "/Nope", "--select", "/Missing"});
    d = OpenDocument(o, &error);
    const auto warnings = ApplyLaunchOptions(o, *d);
    CHECK(warnings.size() == 2);
    CHECK(d->GetViewCamera().IsEmpty() && d->GetSelection().empty());

    // Muting a sublayer-free layer is harmless; a missing file fails.
    o = Parse({(dir / "missing.usda").string()});
    CHECK(!OpenDocument(o, &error));
    CHECK(error.find("Could not open USD stage") == 0);
    o = Parse({scene.string(), "--sessionLayer", (dir / "nope.usda").string()});
    CHECK(!OpenDocument(o, &error));
    CHECK(error.find("nope.usda") != std::string::npos);

    // Settings seed the options; flags apply on top.
    AppSettings settings;
    settings.loadPayloads = false;
    settings.usePrimaryCamera = false;
    settings.view.unloadedAsBounds = true;
    settings.view.pauseDrawing = true;  // never a default
    settings.view.drawMode = DrawMode::Wireframe;
    settings.view.complexity = 1.1;
    settings.view.aov = "depth";  // scene-specific: not a default
    o = DefaultLaunchOptions(settings);
    CHECK(o.unloaded && !o.usePrimaryCamera);
    CHECK(o.view.unloadedAsBounds && !o.view.pauseDrawing);
    std::string parseError;
    CHECK(ParseLaunchOptions({scene.string(), "--complexity", "high"}, &o, &parseError));
    d = OpenDocument(o, &error);
    CHECK(!d->GetStage()->GetPrimAtPath(SdfPath("/World/B/Box")));  // unloaded
    CHECK(ApplyLaunchOptions(o, *d).empty());
    CHECK(d->GetViewCamera().IsEmpty());  // not main_cam
    CHECK(d->GetViewSettings().drawMode == DrawMode::Wireframe);
    CHECK(std::abs(d->GetViewSettings().complexity - 1.2) < 1e-9);  // the flag wins
    CHECK(d->GetViewSettings().aov == "color");
    // --camera still works without the primary camera.
    o = DefaultLaunchOptions(settings);
    CHECK(ParseLaunchOptions({scene.string(), "--camera", "shot"}, &o, &parseError));
    d = OpenDocument(o, &error);
    ApplyLaunchOptions(o, *d);
    CHECK(d->GetViewCamera() == SdfPath("/Cameras/Shots/shot"));

    // The opening options form: shows options, sets them back.
    o = Parse({scene.string(), "--mask", "/World/A,/World/B", "--unloaded"});
    std::vector<FormField> fields = OpeningFields(o);
    CHECK(fields.size() == 4 && fields[0].value == JsValue(false));
    CHECK(fields[1].value == JsValue(std::string("/World/A, /World/B")));
    o = LaunchOptions();
    CHECK(ApplyOpeningFields(JsObject{
        {"loadPayloads", JsValue(false)}, {"mask", JsValue(" /World/B , ")},
        {"sessionLayer", JsValue((dir / "session.usda").string())},
        {"mute", JsValue("a.usda, b.usda")}}, &o, &error));
    CHECK(o.unloaded && o.mask == std::vector<SdfPath>{SdfPath("/World/B")});
    CHECK(o.mute == (std::vector<std::string>{"a.usda", "b.usda"}));
    o.file = scene.string();
    d = OpenDocument(o, &error);
    CHECK(d && d->GetStage()->GetSessionLayer()->GetComment() == "session");
    CHECK(!d->GetStage()->GetPrimAtPath(SdfPath("/World/A")));
    CHECK(!ApplyOpeningFields(JsObject{{"mask", JsValue("World")}}, &o, &error));
    CHECK(error.find("absolute") != std::string::npos);

    // Threads.
    o = Parse({scene.string(), "--numThreads", "1"});
    d = OpenDocument(o, &error);
    CHECK(WorkGetConcurrencyLimit() == 1);
}

} // namespace

int main() {
    test::RegisterPlugins();
    const char *tmp = std::getenv("TEST_TMPDIR");
    const fs::path dir = fs::path(tmp ? tmp : fs::temp_directory_path().string()) / "vela_launch";
    fs::remove_all(dir);
    TestParse();
    TestOpenAndApply(dir);
    return test::Finish("launch option");
}
