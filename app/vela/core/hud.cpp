#include "hud.h"

#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usdGeom/curves.h"
#include "pxr/usd/usdGeom/mesh.h"

#include <cstdio>
#include <unordered_set>

namespace vela {

namespace {

std::string Count(size_t n) { return std::to_string(n); }

std::string Milliseconds(double ms) {
    char text[64];
    std::snprintf(text, sizeof text, "%.2f ms (%.2f FPS)", ms, ms > 0.0 ? 1000.0 / ms : 0.0);
    return text;
}

// Bytes as usdview's ReportMetricSize: B, KB, MB or GB.
std::string Seconds(double seconds) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f s", seconds);
    return text;
}

std::string Size(double bytes) {
    const char *units[] = {"B", "KB", "MB", "GB"};
    int unit = 0;
    while (bytes >= 1024.0 && unit < 3) {
        bytes /= 1024.0;
        ++unit;
    }
    char text[64];
    std::snprintf(text, sizeof text, unit ? "%.1f %s" : "%.0f %s", bytes, units[unit]);
    return text;
}

std::string ComplexityName(double complexity) {
    const auto &names = ComplexityNames();
    const char *titles[] = {"Low", "Medium", "High", "Very High"};
    for (size_t i = 0; i < names.size(); ++i) {
        if (std::abs(complexity - (1.0 + 0.1 * i)) < 1e-6) return titles[i];
    }
    char text[32];
    std::snprintf(text, sizeof text, "%.2f", complexity);
    return text;
}

} // namespace

HudStats ComputeHudStats(const UsdStageRefPtr &stage, const SdfPathVector &paths,
                         UsdTimeCode time) {
    HudStats stats;
    const SdfPathVector roots = paths.empty() ? SdfPathVector{SdfPath::AbsoluteRootPath()} : paths;
    for (const SdfPath &root : roots) {
        const UsdPrim prim = stage->GetPrimAtPath(root);
        if (!prim || !prim.IsActive()) continue;
        for (const UsdPrim &p : UsdPrimRange(prim)) {
            if (p.IsPseudoRoot()) continue;
            ++stats.prims;
            const std::string type = p.GetTypeName().GetString();
            ++stats.types[type.empty() ? "typeless" : type];
            if (UsdGeomCurves curves{p}) {
                VtIntArray counts;
                curves.GetCurveVertexCountsAttr().Get(&counts, time);
                for (int count : counts) stats.cvs += size_t(std::max(count, 0));
            } else if (UsdGeomMesh mesh{p}) {
                VtIntArray counts, indices;
                mesh.GetFaceVertexCountsAttr().Get(&counts, time);
                mesh.GetFaceVertexIndicesAttr().Get(&indices, time);
                if (!counts.empty() && !indices.empty()) {
                    stats.faces += counts.size();
                    stats.verts += std::unordered_set<int>(indices.begin(), indices.end()).size();
                }
            }
        }
    }
    return stats;
}

Hud ComposeHud(const Document &document, const HudStats &stats,
               const HudRenderInfo &render) {
    Hud hud;
    const ViewSettings &s = document.GetViewSettings();
    if (!s.showHUD) return hud;
    if (s.hudInfo) {
        hud.topLeft.emplace_back("Prims", Count(stats.prims));
        for (const auto &[type, count] : stats.types) {
            hud.topLeft.emplace_back(type, Count(count));
        }
        hud.topLeft.emplace_back("CVs", Count(stats.cvs));
        hud.topLeft.emplace_back("Verts", Count(stats.verts));
        hud.topLeft.emplace_back("Faces", Count(stats.faces));
        // usdview's --timing, as the window shows it.
        const Document::LoadStatistics &load = document.GetLoadStatistics();
        if (load.open > 0.0) hud.topLeft.emplace_back("Open", Seconds(load.open));
        if (load.firstFrame > 0.0) hud.topLeft.emplace_back("First frame", Seconds(load.firstFrame));
    }
    hud.topRight.emplace_back("Hydra", render.renderer);
    if (!render.hgi.empty()) hud.topRight.emplace_back("  Hgi", render.hgi);
    if (render.aov != "color") hud.topRight.emplace_back("  AOV", render.aov);
    if (s.hudGPUStats) {
        auto size = [&](const char *label, const char *key) {
            auto found = render.renderStats.find(key);
            if (found == render.renderStats.end()) return;
            const VtValue number = VtValue::Cast<double>(found->second);
            if (number.IsHolding<double>()) {
                hud.bottomLeft.emplace_back(label, Size(number.UncheckedGet<double>()));
            }
        };
        size("GPU mem", "gpuMemoryUsed");
        size(" primvar", "primvar");
        size(" topology", "topology");
        size(" shader", "drawingShader");
        size(" texture", "textureMemory");
    }
    if (s.hudPerformance) {
        hud.bottomLeft.emplace_back("Render", Milliseconds(render.renderMilliseconds));
        if (render.playbackMilliseconds > 0.0) {
            hud.bottomLeft.emplace_back("Playback", Milliseconds(render.playbackMilliseconds));
        }
    }
    if (s.hudComplexity) {
        hud.bottomRight.emplace_back("Complexity", ComplexityName(s.complexity));
        const SdfPath &camera = document.GetViewCamera();
        hud.bottomRight.emplace_back("Camera", camera.IsEmpty() ? "Free" : camera.GetName());
    }
    return hud;
}

} // namespace vela
