#include "export_framing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vela {

bool ParseFraming(const std::string &text, Framing *framing) {
    if (text == "fit") *framing = Framing::Fit;
    else if (text == "crop") *framing = Framing::Crop;
    else return false;
    return true;
}

const std::vector<ResolutionPreset> &ResolutionPresets() {
    static const std::vector<ResolutionPreset> presets = {
        {"2160p", "4K UHD, 3840 × 2160 (16:9)", 3840, 2160},
        {"1440p", "1440p, 2560 × 1440 (16:9)", 2560, 1440},
        {"1080p", "1080p HD, 1920 × 1080 (16:9)", 1920, 1080},
        {"720p", "720p HD, 1280 × 720 (16:9)", 1280, 720},
        {"dci4k", "DCI 4K, 4096 × 2160 (1.90:1)", 4096, 2160},
        {"dci2k", "DCI 2K, 2048 × 1080 (1.90:1)", 2048, 1080},
        {"flat", "Flat, 1998 × 1080 (1.85:1)", 1998, 1080},
        {"scope", "Scope, 2048 × 858 (2.39:1)", 2048, 858},
        {"4:3", "1440 × 1080 (4:3)", 1440, 1080},
        {"square", "Square, 1080 × 1080 (1:1)", 1080, 1080},
        {"portrait", "Portrait, 1080 × 1350 (4:5)", 1080, 1350},
        {"vertical", "Vertical, 1080 × 1920 (9:16)", 1080, 1920},
    };
    return presets;
}

const ResolutionPreset *FindResolutionPreset(const std::string &name) {
    for (const ResolutionPreset &preset : ResolutionPresets()) {
        if (preset.id == name || preset.label == name) return &preset;
    }
    return nullptr;
}

std::string AspectRatioName(double aspect) {
    // Common ratios of small whole numbers, as written for video.
    const int ratios[][2] = {{16, 9}, {9, 16}, {4, 3}, {3, 4}, {1, 1}, {4, 5}, {5, 4}, {3, 2}, {2, 3}};
    for (const auto &r : ratios) {
        if (std::fabs(aspect - double(r[0]) / r[1]) < 0.005) {
            return std::to_string(r[0]) + ":" + std::to_string(r[1]);
        }
    }
    char text[32];
    snprintf(text, sizeof(text), "%.2f:1", aspect);
    return text;
}

GfVec2i ResolveExportSize(int width, int height, double aspect, GfVec2i fallback, bool even) {
    if (!(aspect > 0)) aspect = 1.0;
    GfVec2i size = fallback;
    if (width > 0 && height > 0) size = GfVec2i(width, height);
    else if (width > 0) size = GfVec2i(width, std::max(1, int(std::lround(width / aspect))));
    else if (height > 0) size = GfVec2i(std::max(1, int(std::lround(height * aspect))), height);
    size = GfVec2i(std::max(1, size[0]), std::max(1, size[1]));
    if (even) size = GfVec2i(size[0] + size[0] % 2, size[1] + size[1] % 2);
    return size;
}

FittedFrame FitFrame(GfVec2i output, double aspect) {
    FittedFrame frame{output, GfVec2i(0, 0), false};
    if (!(aspect > 0)) return frame;
    const double outputAspect = double(output[0]) / output[1];
    if (aspect > outputAspect) {
        // Wider than the output: bars above and below.
        const int height = std::max(1, int(std::lround(output[0] / aspect)));
        if (output[1] - height < 1) return frame;
        frame.size = GfVec2i(output[0], height);
        frame.offset = GfVec2i(0, (output[1] - height) / 2);
    } else {
        // Narrower: bars at the sides.
        const int width = std::max(1, int(std::lround(output[1] * aspect)));
        if (output[0] - width < 1) return frame;
        frame.size = GfVec2i(width, output[1]);
        frame.offset = GfVec2i((output[0] - width) / 2, 0);
    }
    frame.boxed = true;
    return frame;
}

} // namespace vela
