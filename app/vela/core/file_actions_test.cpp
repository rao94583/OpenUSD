#include "actions.h"
#include "document.h"
#include "test_support.h"

#include "pxr/usd/usd/editContext.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usdGeom/cube.h"
#include "pxr/usd/usdGeom/sphere.h"

#include <cstdlib>
#include <filesystem>

using namespace vela;
namespace fs = std::filesystem;

namespace {

JsObject Path(const fs::path &path) {
    return JsObject{{"path", JsValue(path.string())}};
}

// A root layer with a sublayer, both on disk.
fs::path MakeFiles(const fs::path &dir) {
    fs::create_directories(dir);
    SdfLayerRefPtr sub = SdfLayer::CreateNew((dir / "sub.usda").string());
    sub->Save();
    UsdStageRefPtr stage = UsdStage::CreateNew((dir / "root.usda").string());
    stage->GetRootLayer()->SetSubLayerPaths({"sub.usda"});
    UsdGeomCube::Define(stage, SdfPath("/Cube"));
    stage->GetRootLayer()->Save();
    return dir / "root.usda";
}

void TestSave(const fs::path &dir) {
    ActionRegistry &registry = ActionRegistry::Get();
    auto d = Document::Open(MakeFiles(dir).string());
    CHECK(d && !d->HasUnsavedEdits());
    CHECK(!registry.IsEnabled(*registry.Find("save"), *d));

    // Edits in the root layer, the sublayer and the session layer.
    UsdStageRefPtr stage = d->GetStage();
    UsdGeomSphere::Define(stage, SdfPath("/Sphere"));
    SdfLayerHandle sub = stage->GetLayerStack()[2];
    CHECK(d->SetEditTarget(sub));
    stage->GetPrimAtPath(SdfPath("/Cube")).SetCustomDataByKey(TfToken("note"), VtValue("sub"));
    CHECK(d->SetEditTarget(stage->GetSessionLayer()));
    stage->GetPrimAtPath(SdfPath("/Cube")).SetActive(false);
    CHECK(d->HasUnsavedEdits());
    CHECK(d->GetLayersToSave().size() == 2);  // not the session layer

    ActionResult result = registry.Run(*d, "save");
    CHECK(result.ok);
    CHECK(result.value.GetJsObject().at("saved").GetJsArray().size() == 2);
    CHECK(!d->HasUnsavedEdits());
    CHECK(stage->GetSessionLayer()->IsDirty());

    // Reopened from disk: the root and sublayer edits, not the session's.
    std::string rootText, subText;
    SdfLayer::OpenAsAnonymous((dir / "root.usda").string())->ExportToString(&rootText);
    SdfLayer::OpenAsAnonymous((dir / "sub.usda").string())->ExportToString(&subText);
    CHECK(rootText.find("\"Sphere\"") != std::string::npos);
    CHECK(subText.find("note") != std::string::npos);
    CHECK(rootText.find("active") == std::string::npos);

    // The session layer exports separately.
    CHECK(registry.Run(*d, "export_session_layer", Path(dir / "overrides.usda")).ok);
    std::string overrides;
    SdfLayer::OpenAsAnonymous((dir / "overrides.usda").string())->ExportToString(&overrides);
    CHECK(overrides.find("active = false") != std::string::npos);
}

void TestExportAndRevert(const fs::path &dir) {
    ActionRegistry &registry = ActionRegistry::Get();
    auto d = Document::Open(MakeFiles(dir).string());
    UsdStageRefPtr stage = d->GetStage();
    CHECK(d->SetEditTarget(stage->GetLayerStack()[2]));
    UsdGeomSphere::Define(stage, SdfPath("/InSub"));

    // Flattened: one layer with both layers' prims; the stage is unchanged.
    CHECK(registry.Run(*d, "export_flattened", Path(dir / "flat.usdc")).ok);
    UsdStageRefPtr flat = UsdStage::Open((dir / "flat.usdc").string());
    CHECK(flat && flat->GetPrimAtPath(SdfPath("/Cube")) && flat->GetPrimAtPath(SdfPath("/InSub")));
    CHECK(flat->GetRootLayer()->GetSubLayerPaths().empty());
    CHECK(d->HasUnsavedEdits());
    ActionResult bad = registry.Run(*d, "export_flattened", Path(dir / "flat.txt"));
    CHECK(!bad.ok && bad.error.find("Not a USD file extension") == 0);

    // Revert discards the edit and the undo history.
    CHECK(d->GetUndoStack().CanUndo());
    CHECK(registry.Run(*d, "revert").ok);
    CHECK(!stage->GetPrimAtPath(SdfPath("/InSub")));
    CHECK(!d->HasUnsavedEdits());
    CHECK(!d->GetUndoStack().CanUndo());
    CHECK(!registry.Run(*d, "revert").ok);  // nothing to revert
}

void TestInMemory() {
    ActionRegistry &registry = ActionRegistry::Get();
    Document d(UsdStage::CreateInMemory());
    UsdGeomCube::Define(d.GetStage(), SdfPath("/Cube"));
    // Unsaved, but there is no file to save it to.
    CHECK(d.HasUnsavedEdits());
    CHECK(d.GetLayersToSave().empty());
    CHECK(!registry.Run(d, "save").ok);
}

} // namespace

int main() {
    test::RegisterPlugins();
    const char *tmp = std::getenv("TEST_TMPDIR");
    const fs::path dir = fs::path(tmp ? tmp : fs::temp_directory_path().string()) / "vela_files";
    fs::remove_all(dir);
    TestSave(dir / "save");
    TestExportAndRevert(dir / "export");
    TestInMemory();
    return test::Finish("file action");
}
