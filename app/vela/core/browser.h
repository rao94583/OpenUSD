// Read-only browsing of the composed stage. No AppKit or renderer dependency.
#pragma once
#include "document.h"
#include "pxr/base/gf/vec2d.h"
#include <set>

namespace vela {
struct PrimSummary {
    std::string name, type, visibility, drawMode;
    bool active = false, loaded = false, instanceProxy = false, inPrototype = false;
};
struct InspectorRow {
    std::string name, type, value, source;
};
struct SplinePlot {
    std::vector<GfVec2d> samples, knots;
};
using InspectorRows = std::vector<InspectorRow>;

class Browser {
public:
    explicit Browser(Document &document);
    ~Browser();
    Browser(const Browser &) = delete;
    Browser &operator=(const Browser &) = delete;
    // Without a filter, only enumerates the requested prim's direct children.
    // Search scans once per query or stage change and retains matching ancestors.
    enum class PrimFilter { All, Active, Loaded, Models, Visible };
    void SetPrimFilter(PrimFilter filter);
    void SetShowPrototypes(bool show);
    bool GetShowPrototypes() const { return _showPrototypes; }
    // Destination for an instance or instance proxy's prototype, if any.
    SdfPath PrototypePath(const SdfPath &path) const;
    void SetFilter(std::string query);
    const std::string &GetFilter() const { return _query; }
    SdfPathVector Children(const SdfPath &parent = SdfPath::AbsoluteRootPath());
    bool HasChildren(const SdfPath &parent);
    size_t PropertyDetailCount(const SdfPath &path, const std::string &property) const;
    PrimSummary Summary(const SdfPath &path) const;
    InspectorRows Properties(const SdfPath &path, const std::string &query = "") const;
    SplinePlot SplinePreview(const SdfPath &path, const std::string &property,
                             size_t samples = 256) const;
    InspectorRows PropertyMetadata(const SdfPath &path, const std::string &property) const;
    InspectorRows PropertyOpinions(const SdfPath &path, const std::string &property) const;
    InspectorRows LayerDetails(const std::string &identifier) const;
    InspectorRows Metadata(const SdfPath &path) const;
    InspectorRows Composition(const SdfPath &path) const;
    InspectorRows Layers() const;
    InspectorRows Variants(const SdfPath &path) const;
    // Array elements, time samples or spline knots, in bounded pages. limit
    // is capped at 256; summaries never stringify entire arrays or dictionaries.
    InspectorRows PropertyDetails(const SdfPath &path, const std::string &property,
                                  size_t offset = 0, size_t limit = 128) const;
private:
    void _BuildMatches();
    bool _NeedsMatches() const;
    Document &_document;
    int _observer;
    std::string _query;
    PrimFilter _primFilter = PrimFilter::All;
    bool _showPrototypes = false;
    bool _matchesValid = false;
    // Whether the Visible filter's matches can change with time: some
    // matched prim's visibility (or an ancestor's) is time-varying.
    bool _visibilityAnimated = false;
    std::set<SdfPath> _matches;
};
} // namespace vela
