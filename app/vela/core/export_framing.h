// Sizes and framing for exported images and movies: standard resolution
// presets, and where the camera's frame goes when its aspect ratio differs
// from the output's (letterboxed or pillarboxed, or cropped to fill).
#pragma once

#include "pxr/pxr.h"
#include "pxr/base/gf/vec2i.h"

#include <string>
#include <vector>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

// Fit keeps the camera's whole frame, with black bars above and below
// (letterbox) or at the sides (pillarbox); Crop fills the output and cuts
// off what doesn't fit.
enum class Framing { Fit, Crop };
bool ParseFraming(const std::string &text, Framing *framing);  // "fit", "crop"

struct ResolutionPreset {
    std::string id;     // e.g. "1080p", for scripts
    std::string label;  // e.g. "1080p HD, 1920 × 1080 (16:9)", for menus
    int width = 0, height = 0;
};
const std::vector<ResolutionPreset> &ResolutionPresets();
// By id or label; null if neither.
const ResolutionPreset *FindResolutionPreset(const std::string &name);

// e.g. "16:9", "1.85:1", "2.39:1".
std::string AspectRatioName(double aspect);

// The output size: width and height as given; with one of them 0, the
// other follows aspect; with both 0, fallback. even rounds up to even
// numbers, as H.264 and HEVC need.
GfVec2i ResolveExportSize(int width, int height, double aspect, GfVec2i fallback, bool even);

// Where a camera frame of the given aspect ratio goes in the output when
// fitted: its size and its offset from the top left. boxed is false when
// it fills the output (the aspect ratios match to within a pixel).
struct FittedFrame {
    GfVec2i size;
    GfVec2i offset;
    bool boxed = false;
};
FittedFrame FitFrame(GfVec2i output, double aspect);

} // namespace vela
