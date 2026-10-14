// How the viewport draws the stage: usdview's view settings, with its
// defaults. No renderer dependency; the renderer maps these to Hydra.
#pragma once

#include "pxr/pxr.h"
#include "pxr/base/gf/vec4f.h"

#include <string>
#include <vector>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

enum class DrawMode {
    Points,
    Wireframe,
    WireframeOnSurface,
    ShadedFlat,
    ShadedSmooth,
    GeomOnly,
    GeomFlat,
    GeomSmooth,
};

enum class Background { Black, DarkGrey, LightGrey, White };

// When selection highlights are drawn.
enum class HighlightMode { Never, WhenPaused, Always };

struct ViewSettings {
    DrawMode drawMode = DrawMode::ShadedSmooth;
    // Purposes drawn besides default.
    bool showProxy = true;
    bool showRender = false;
    bool showGuides = false;
    // Mesh refinement: 1.0 (low) to 1.3 (very high).
    double complexity = 1.0;
    bool cameraLight = true;
    bool domeLight = false;
    bool domeLightTexturesVisible = true;
    bool sceneLights = true;
    bool sceneMaterials = true;
    bool cullBackfaces = false;
    Background background = Background::DarkGrey;
    HighlightMode highlightMode = HighlightMode::WhenPaused;

    bool operator==(const ViewSettings &other) const = default;
};

// Whether the draw mode is lit (the others ignore lights and materials).
bool IsShaded(DrawMode mode);
// Linear RGBA, as usdview's presets.
GfVec4f BackgroundColor(Background background);

// Identifiers for actions and the command line, e.g. "wireframeOnSurface",
// "darkGrey", "whenPaused", and usdview's complexity names "low" ...
// "veryhigh". Parse functions return false for unknown names.
const std::vector<std::string> &DrawModeNames();
const std::vector<std::string> &BackgroundNames();
const std::vector<std::string> &HighlightModeNames();
const std::vector<std::string> &ComplexityNames();
std::string ToString(DrawMode mode);
std::string ToString(Background background);
std::string ToString(HighlightMode mode);
bool Parse(const std::string &name, DrawMode *mode);
bool Parse(const std::string &name, Background *background);
bool Parse(const std::string &name, HighlightMode *mode);
// usdview's named complexity levels.
bool ParseComplexity(const std::string &name, double *complexity);

} // namespace vela
