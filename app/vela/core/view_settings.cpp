#include "view_settings.h"

#include <algorithm>

namespace vela {

namespace {

template <class Enum>
std::string Name(const std::vector<std::string> &names, Enum value) {
    return names[static_cast<size_t>(value)];
}

template <class Enum>
bool Find(const std::vector<std::string> &names, const std::string &name, Enum *value) {
    auto found = std::find(names.begin(), names.end(), name);
    if (found == names.end()) return false;
    *value = static_cast<Enum>(found - names.begin());
    return true;
}

} // namespace

bool IsShaded(DrawMode mode) {
    switch (mode) {
    case DrawMode::WireframeOnSurface:
    case DrawMode::ShadedFlat:
    case DrawMode::ShadedSmooth:
    case DrawMode::GeomFlat:
    case DrawMode::GeomSmooth:
        return true;
    default:
        return false;
    }
}

GfVec4f BackgroundColor(Background background) {
    switch (background) {
    case Background::Black: return GfVec4f(0.0f, 0.0f, 0.0f, 1.0f);
    case Background::DarkGrey: return GfVec4f(0.07074f, 0.07074f, 0.07074f, 1.0f);
    case Background::LightGrey: return GfVec4f(0.45626f, 0.45626f, 0.45626f, 1.0f);
    case Background::White: return GfVec4f(1.0f);
    }
    return GfVec4f(0.0f, 0.0f, 0.0f, 1.0f);
}

GfVec4f BBoxLineColor(Background background) {
    const GfVec4f clear = BackgroundColor(background);
    GfVec4f color(0.0f, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < 3; ++i) {
        color[i] = std::clamp(clear[i] > 0.5f ? clear[i] - 0.6f : clear[i] + 0.6f, 0.0f, 1.0f);
    }
    return color;
}

// In enum order.
const std::vector<std::string> &DrawModeNames() {
    static const std::vector<std::string> names{
        "points", "wireframe", "wireframeOnSurface", "shadedFlat",
        "shadedSmooth", "geomOnly", "geomFlat", "geomSmooth"};
    return names;
}

const std::vector<std::string> &BackgroundNames() {
    static const std::vector<std::string> names{
        "black", "darkGrey", "lightGrey", "white"};
    return names;
}

const std::vector<std::string> &HighlightModeNames() {
    static const std::vector<std::string> names{"never", "whenPaused", "always"};
    return names;
}

const std::vector<std::string> &ColorCorrectionNames() {
    static const std::vector<std::string> names{"disabled", "sRGB", "openColorIO"};
    return names;
}
std::string ToString(ColorCorrection mode) { return Name(ColorCorrectionNames(), mode); }
bool Parse(const std::string &name, ColorCorrection *mode) {
    return Find(ColorCorrectionNames(), name, mode);
}

const std::vector<std::string> &ComplexityNames() {
    static const std::vector<std::string> names{"low", "medium", "high", "veryhigh"};
    return names;
}

std::string ToString(DrawMode mode) { return Name(DrawModeNames(), mode); }
std::string ToString(Background background) { return Name(BackgroundNames(), background); }
std::string ToString(HighlightMode mode) { return Name(HighlightModeNames(), mode); }
bool Parse(const std::string &name, DrawMode *mode) { return Find(DrawModeNames(), name, mode); }
bool Parse(const std::string &name, Background *background) {
    return Find(BackgroundNames(), name, background);
}
bool Parse(const std::string &name, HighlightMode *mode) {
    return Find(HighlightModeNames(), name, mode);
}

bool ParseComplexity(const std::string &name, double *complexity) {
    const auto &names = ComplexityNames();
    auto found = std::find(names.begin(), names.end(), name);
    if (found == names.end()) return false;
    *complexity = 1.0 + 0.1 * double(found - names.begin());
    return true;
}

} // namespace vela
