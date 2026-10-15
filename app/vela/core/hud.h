// The viewport's heads-up display, as usdview's: text in the four corners.
// Computes the text; the viewport draws it.
#pragma once

#include "document.h"

#include "pxr/base/vt/dictionary.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

// Prim and geometry counts below some prims, as usdview's HUD Info.
struct HudStats {
    size_t prims = 0;
    std::map<std::string, size_t> types;  // prim type name (or "typeless")
    size_t cvs = 0;                       // curve vertices
    size_t verts = 0;                     // distinct mesh vertices
    size_t faces = 0;                     // mesh faces
};

// Counts the prims below and including paths (the stage when empty), at
// time, and their curve CVs and mesh vertices and faces.
HudStats ComputeHudStats(const UsdStageRefPtr &stage, const SdfPathVector &paths,
                         UsdTimeCode time);

// What the renderer reports, for the HUD.
struct HudRenderInfo {
    std::string renderer = "Storm";
    std::string hgi = "Metal";
    std::string aov = "color";
    double renderMilliseconds = 0.0;
    double playbackMilliseconds = 0.0;  // 0 when not playing
    VtDictionary renderStats;           // UsdImagingGLEngine::GetRenderStats
};

using HudLines = std::vector<std::pair<std::string, std::string>>;
struct Hud {
    HudLines topLeft, topRight, bottomLeft, bottomRight;
};

// The HUD's text for the document's view settings. stats is used only when
// the Info section is shown.
Hud ComposeHud(const Document &document, const HudStats &stats,
               const HudRenderInfo &render);

} // namespace vela
