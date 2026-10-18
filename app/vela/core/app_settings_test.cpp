#include "app_settings.h"
#include "test_support.h"

#include "pxr/base/js/json.h"

using namespace vela;

namespace {

void TestViewSettingsJson() {
    ViewSettings s;
    s.drawMode = DrawMode::Points;
    s.showGuides = true;
    s.hudInfo = true;
    s.complexity = 1.3;
    s.background = Background::White;
    s.colorCorrection = ColorCorrection::OpenColorIO;
    s.ocioDisplay = "sRGB - Display";
    s.renderer = "Embree";
    s.aov = "depth";
    s.rendererSettings["spp"] = JsValue(int64_t(4));
    s.renderSettingsPrim = SdfPath("/Render/Settings");
    s.defaultMaterialAmbient = 0.5;
    // A round trip, through text as when saved.
    ViewSettings read;
    FromJson(JsParseString(JsWriteToString(JsValue(ToJson(s)))).GetJsObject(), &read);
    CHECK(read == s);

    // Lenient: unknown keys, wrong types and bad names keep current values.
    ViewSettings lenient;
    FromJson(JsParseString(R"({"drawMode": "sketchy", "showGuides": 1, "complexity": "x",
        "renderPassPrim": "not a path", "futureSetting": true, "cameraLight": false})")
        .GetJsObject(), &lenient);
    ViewSettings expected;
    expected.cameraLight = false;
    CHECK(lenient == expected);

    // Defaults for any file drop what belongs to one scene.
    const ViewSettings general = AsDefaults(s);
    CHECK(general.aov == "color" && general.rendererSettings.empty());
    CHECK(general.renderSettingsPrim.IsEmpty());
    CHECK(general.drawMode == DrawMode::Points && general.renderer == "Embree");

    CHECK(DescribeChanges(ViewSettings()).empty());
    ViewSettings changed;
    changed.drawMode = DrawMode::Wireframe;
    changed.cameraLight = false;
    CHECK((DescribeChanges(changed) ==
           std::vector<std::string>{"cameraLight: false", "drawMode: wireframe"}));
}

void TestAppSettings() {
    AppSettings a;
    a.loadPayloads = false;
    a.threadLimit = -2;
    a.mcpServer = false;
    a.claudePath = "/opt/claude";
    a.claudeAsksBeforeEdits = true;
    a.claudeModel = "opus";
    a.view.background = Background::Black;
    AppSettings read;
    FromJson(JsParseString(JsWriteToString(JsValue(ToJson(a)))).GetJsObject(), &read);
    CHECK(read == a);

    AppSettingsStore &store = AppSettingsStore::Get();
    int calls = 0;
    const int id = store.AddObserver([&] { ++calls; });
    store.SetSettings(a);
    store.SetSettings(a);  // unchanged: no notification
    CHECK(calls == 1 && store.GetSettings() == a);
    store.Reset();
    CHECK(calls == 2 && store.GetSettings() == AppSettings());
    store.RemoveObserver(id);
    store.SetSettings(a);
    CHECK(calls == 2);
    store.Reset();
}

} // namespace

int main() {
    TestViewSettingsJson();
    TestAppSettings();
    return test::Finish("settings");
}
