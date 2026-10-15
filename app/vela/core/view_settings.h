// How the viewport draws the stage: usdview's view settings, with its
// defaults. No renderer dependency; the renderer maps these to Hydra.
#pragma once

#include "pxr/pxr.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/js/value.h"
#include "pxr/usd/sdf/path.h"

#include <map>
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

// How the image is converted for display, as usdview's modes.
enum class ColorCorrection { Disabled, SRGB, OpenColorIO };

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
    // Dashed boxes around the selection (or the stage when nothing is
    // selected), as usdview: axis-aligned and oriented, not during playback.
    bool showBBoxes = true;
    bool showAlignedBBox = true;
    bool showOrientedBBox = true;
    bool showBBoxesDuringPlayback = false;
    // Color management. OCIO settings left empty use the config's defaults.
    ColorCorrection colorCorrection = ColorCorrection::SRGB;
    std::string ocioDisplay, ocioView, ocioColorSpace, ocioLook;
    // The heads-up display and its sections, with usdview's defaults.
    bool showHUD = true;
    bool hudInfo = false;
    bool hudComplexity = true;
    bool hudPerformance = true;
    bool hudGPUStats = false;
    // The Hydra renderer: a renderer plugin id or display name (as usdview's
    // --renderer), empty for the default (Storm). The renderer resolves it.
    std::string renderer;
    // The AOV shown: "color", or e.g. "depth", "primId", "primvars:st".
    std::string aov = "color";
    // Renderer settings changed from the renderer's defaults, by key; they
    // are dropped when the renderer changes.
    std::map<std::string, JsValue> rendererSettings;
    // The active render settings and render pass prims. An empty settings
    // prim means the stage's renderSettingsPrimPath metadata, as usdrecord.
    SdfPath renderSettingsPrim;
    SdfPath renderPassPrim;
    // The fallback material for geometry without one, as usdview's View →
    // Adjust Default Material (gray levels).
    double defaultMaterialAmbient = 0.2;
    double defaultMaterialSpecular = 0.1;
    // Draw unloaded prims as their bounding boxes (usdview's --bboxStandin),
    // and pause viewport drawing (--norender).
    bool unloadedAsBounds = false;
    bool pauseDrawing = false;

    bool operator==(const ViewSettings &other) const = default;
};

// As a JSON object with the names used by actions ("drawMode": "wireframe",
// "background": "darkGrey", ...). Reading ignores unknown keys and values
// of the wrong type, keeping the current value, so older saved settings
// still load.
JsObject ToJson(const ViewSettings &settings);
void FromJson(const JsObject &json, ViewSettings *settings);
// The settings as defaults for newly opened files: what describes one scene
// rather than how to view any (AOV, renderer setting overrides, render
// settings and pass prims) and paused drawing are reset.
ViewSettings AsDefaults(const ViewSettings &settings);
// What differs from the defaults (usdview's), as "name: value" in ToJson's
// names and order, e.g. {"background: white", "drawMode: wireframe"}.
std::vector<std::string> DescribeChanges(const ViewSettings &settings);

// Whether the draw mode is lit (the others ignore lights and materials).
bool IsShaded(DrawMode mode);
// Linear RGBA, as usdview's presets.
GfVec4f BackgroundColor(Background background);
// Bounding box lines: the background shifted by 0.6, as usdview.
GfVec4f BBoxLineColor(Background background);

// Identifiers for actions and the command line, e.g. "wireframeOnSurface",
// "darkGrey", "whenPaused", and usdview's complexity names "low" ...
// "veryhigh". Parse functions return false for unknown names.
const std::vector<std::string> &DrawModeNames();
const std::vector<std::string> &BackgroundNames();
const std::vector<std::string> &HighlightModeNames();
const std::vector<std::string> &ComplexityNames();
// "disabled", "sRGB", "openColorIO": usdview's and HdxColorCorrectionTokens.
const std::vector<std::string> &ColorCorrectionNames();
std::string ToString(ColorCorrection mode);
bool Parse(const std::string &name, ColorCorrection *mode);
std::string ToString(DrawMode mode);
std::string ToString(Background background);
std::string ToString(HighlightMode mode);
bool Parse(const std::string &name, DrawMode *mode);
bool Parse(const std::string &name, Background *background);
bool Parse(const std::string &name, HighlightMode *mode);
// usdview's named complexity levels.
bool ParseComplexity(const std::string &name, double *complexity);

} // namespace vela
