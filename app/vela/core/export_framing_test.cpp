#include "export_framing.h"
#include "test_support.h"

using namespace vela;

namespace {

void TestPresets() {
    CHECK(ResolutionPresets().size() >= 10);
    const ResolutionPreset *hd = FindResolutionPreset("1080p");
    CHECK(hd && hd->width == 1920 && hd->height == 1080);
    CHECK(FindResolutionPreset(hd->label) == hd);
    CHECK(!FindResolutionPreset("8k"));
    for (const ResolutionPreset &preset : ResolutionPresets()) {
        // Every label names its aspect ratio, and video sizes are even.
        const std::string ratio = AspectRatioName(double(preset.width) / preset.height);
        CHECK(preset.label.find("(" + ratio + ")") != std::string::npos);
        CHECK(preset.width % 2 == 0 && preset.height % 2 == 0);
    }
    CHECK(AspectRatioName(16.0 / 9) == "16:9");
    CHECK(AspectRatioName(2048.0 / 858) == "2.39:1");
    Framing framing = Framing::Fit;
    CHECK(ParseFraming("crop", &framing) && framing == Framing::Crop);
    CHECK(!ParseFraming("stretch", &framing));
}

void TestSizes() {
    const GfVec2i viewport(1200, 800);
    CHECK(ResolveExportSize(0, 0, 1.5, viewport, false) == viewport);
    CHECK(ResolveExportSize(640, 480, 1.5, viewport, false) == GfVec2i(640, 480));
    CHECK(ResolveExportSize(900, 0, 1.5, viewport, false) == GfVec2i(900, 600));
    CHECK(ResolveExportSize(0, 300, 2.0, viewport, false) == GfVec2i(600, 300));
    CHECK(ResolveExportSize(161, 0, 1.0, viewport, true) == GfVec2i(162, 162));
}

void TestFitting() {
    // A 2.39:1 camera in 16:9 HD: letterboxed.
    FittedFrame f = FitFrame(GfVec2i(1920, 1080), 2.39);
    CHECK(f.boxed && f.size == GfVec2i(1920, 803) && f.offset == GfVec2i(0, 138));
    // A 4:3 camera in 16:9: pillarboxed.
    f = FitFrame(GfVec2i(1920, 1080), 4.0 / 3);
    CHECK(f.boxed && f.size == GfVec2i(1440, 1080) && f.offset == GfVec2i(240, 0));
    // A 16:9 camera in 9:16 vertical video: letterboxed.
    f = FitFrame(GfVec2i(1080, 1920), 16.0 / 9);
    CHECK(f.boxed && f.size == GfVec2i(1080, 608) && f.offset == GfVec2i(0, 656));
    // Matching (or within a pixel): no bars.
    f = FitFrame(GfVec2i(1920, 1080), 16.0 / 9);
    CHECK(!f.boxed && f.size == GfVec2i(1920, 1080) && f.offset == GfVec2i(0, 0));
    f = FitFrame(GfVec2i(1998, 1080), 1.85);
    CHECK(!f.boxed);
}

} // namespace

int main() {
    TestPresets();
    TestSizes();
    TestFitting();
    return test::Finish("export framing");
}
