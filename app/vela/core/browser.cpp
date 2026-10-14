#include "browser.h"
#include "pxr/base/tf/enum.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/ts/spline.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/typeHeaders.h"
#include "pxr/base/vt/visitValue.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/primCompositionQuery.h"
#include "pxr/usd/usd/relationship.h"
#include "pxr/usd/usd/variantSets.h"
#include "pxr/usd/usdGeom/imageable.h"
#include "pxr/usd/usdGeom/modelAPI.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/sdf/pathExpression.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace vela {
namespace {
std::string Lower(std::string text) {
    for (char &c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
std::string Short(std::string text) {
    return text.size() > 512 ? text.substr(0, 512) + "…" : text;
}
std::string Value(const VtValue &value) {
    if (value.IsEmpty()) return "<no value>";
    if (value.IsArrayValued()) return std::to_string(value.GetArraySize()) + " elements";
    if (value.IsHolding<VtDictionary>()) return std::to_string(value.UncheckedGet<VtDictionary>().size()) + " dictionary entries";
    if (value.IsHolding<TsSpline>()) return "spline";
    return Short(TfStringify(value));
}
std::string Source(const UsdProperty &property, UsdTimeCode time) {
    const auto stack = property.GetPropertyStack(time);
    return stack.empty() ? "schema fallback / unauthored" : "strongest spec: " + stack.front()->GetLayer()->GetIdentifier();
}
template<class Map> void MetadataRows(const Map &metadata, const std::string &prefix,
                  InspectorRows *rows, int depth = 0) {
    for (const auto &[name, value] : metadata) {
        if (rows->size() >= 512) { rows->push_back({"…", "", "Metadata preview limited to 512 rows", ""}); return; }
        rows->push_back({prefix + TfStringify(name), value.GetTypeName(), Value(value), "composed"});
        if (value.template IsHolding<VtDictionary>() && depth < 4)
            MetadataRows(value.template UncheckedGet<VtDictionary>(), prefix + TfStringify(name) + ".", rows, depth + 1);
    }
}
struct ArrayPreview {
    size_t offset, limit;
    InspectorRows *rows;
    template<class T> bool operator()(const VtArray<T> &array) const {
        const size_t end = offset + std::min(limit, array.size() - std::min(offset, array.size()));
        for (size_t i = offset; i < end; ++i) {
            std::string preview;
            if constexpr (std::is_same_v<T, char> || std::is_same_v<T, unsigned char>) preview = TfStringify(int(array[i]));
            else preview = Short(TfStringify(array[i]));
            rows->push_back({"[" + std::to_string(i) + "]", "element", preview, ""});
        }
        return true;
    }
    template<class T> bool operator()(const T &) const { return false; }
};
bool Arrays(const VtValue &value, size_t offset, size_t limit, InspectorRows *rows) {
    return VtVisitValueWithExtTypes<TfMetaList<VtArray<SdfAssetPath>, VtArray<SdfPathExpression>>>::VisitValue(
        value, ArrayPreview{offset, limit, rows});
}

} // namespace

Browser::Browser(Document &document) : _document(document) {
    // Paths, types, activation, load state and kind change only with a
    // resync. Visibility is a value: authored edits invalidate the Visible
    // filter, and time does only when some visibility is animated.
    _observer = document.AddObserver([this](unsigned changes) {
        const bool visible = _primFilter == PrimFilter::Visible;
        if ((changes & Document::Resync) ||
            (visible && (changes & Document::StageContent)) ||
            (visible && (changes & Document::Time) && _visibilityAnimated))
            _matchesValid = false;
    });
}
Browser::~Browser() { _document.RemoveObserver(_observer); }
void Browser::SetFilter(std::string query) {
    query = Lower(std::move(query));
    if (_query != query) { _query = std::move(query); _matchesValid = false; }
}
void Browser::SetPrimFilter(PrimFilter filter) {
    if (_primFilter != filter) { _primFilter = filter; _matchesValid = false; }
}
void Browser::SetShowPrototypes(bool show) {
    if (_showPrototypes != show) { _showPrototypes = show; _matchesValid = false; }
}
bool Browser::_NeedsMatches() const { return !_query.empty() || _primFilter != PrimFilter::All; }
SdfPath Browser::PrototypePath(const SdfPath &path) const {
    const auto prim = _document.GetStage()->GetPrimAtPath(path);
    if (!prim) return {};
    const auto prototype = prim.IsInstanceProxy() ? prim.GetPrimInPrototype() : prim.GetPrototype();
    return prototype ? prototype.GetPath() : SdfPath();
}
void Browser::_BuildMatches() {
    _matches.clear();
    _visibilityAnimated = false;
    std::vector<UsdPrim> roots{_document.GetStage()->GetPseudoRoot()};
    if (_showPrototypes) {
        const auto prototypes = _document.GetStage()->GetPrototypes();
        roots.insert(roots.end(), prototypes.begin(), prototypes.end());
    }
    for (const auto &root : roots) {
        for (const auto &prim : UsdPrimRange(root, UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate))) {
            // Every prim's visibility is inherited by its descendants, so
            // check it whether or not the prim itself matches the search.
            if (_primFilter == PrimFilter::Visible && !_visibilityAnimated) {
                const UsdGeomImageable imageable(prim);
                if (imageable && imageable.GetVisibilityAttr().ValueMightBeTimeVarying())
                    _visibilityAnimated = true;
            }
            if (Lower(prim.GetPath().GetString() + " " + prim.GetTypeName().GetString()).find(_query) == std::string::npos)
                continue;
            if (_primFilter == PrimFilter::Active && !prim.IsActive()) continue;
            if (_primFilter == PrimFilter::Loaded && !prim.IsLoaded()) continue;
            if (_primFilter == PrimFilter::Models && !prim.IsModel()) continue;
            if (_primFilter == PrimFilter::Visible) {
                const UsdGeomImageable imageable(prim);
                if (!imageable || imageable.ComputeVisibility(_document.GetTime()) == UsdGeomTokens->invisible) continue;
            }
            for (SdfPath path = prim.GetPath(); !path.IsEmpty(); path = path.GetParentPath())
                _matches.insert(path);
        }
    }
    _matchesValid = true;
}
SdfPathVector Browser::Children(const SdfPath &parent) {
    const auto prim = _document.GetStage()->GetPrimAtPath(parent);
    if (!prim) return {};
    if (_NeedsMatches() && !_matchesValid) _BuildMatches();
    SdfPathVector paths;
    for (const auto &child : prim.GetFilteredChildren(UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate)))
        if (!_NeedsMatches() || _matches.count(child.GetPath())) paths.push_back(child.GetPath());
    if (_showPrototypes && parent == SdfPath::AbsoluteRootPath())
        for (const auto &prototype : _document.GetStage()->GetPrototypes())
            if (!_NeedsMatches() || _matches.count(prototype.GetPath())) paths.push_back(prototype.GetPath());
    return paths;
}
bool Browser::HasChildren(const SdfPath &parent) {
    const auto prim = _document.GetStage()->GetPrimAtPath(parent);
    if (!prim) return false;
    if (_NeedsMatches() && !_matchesValid) _BuildMatches();
    for (const auto &child : prim.GetFilteredChildren(UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate)))
        if (!_NeedsMatches() || _matches.count(child.GetPath())) return true;
    if (_showPrototypes && parent == SdfPath::AbsoluteRootPath())
        for (const auto &prototype : _document.GetStage()->GetPrototypes())
            if (!_NeedsMatches() || _matches.count(prototype.GetPath())) return true;
    return false;
}
size_t Browser::PropertyDetailCount(const SdfPath &path, const std::string &property) const {
    const auto prim = _document.GetStage()->GetPrimAtPath(path);
    if (!prim) return 0;
    const auto attr = prim.GetAttribute(TfToken(property));
    if (!attr) {
        const auto rel = prim.GetRelationship(TfToken(property));
        SdfPathVector targets;
        if (rel) rel.GetTargets(&targets);
        return targets.size();
    }
    VtValue value; attr.Get(&value, _document.GetTime());
    if (value.IsArrayValued()) return value.GetArraySize();
    if (attr.HasSpline()) return attr.GetSpline().GetKnots().size();
    return attr.GetNumTimeSamples();
}
PrimSummary Browser::Summary(const SdfPath &path) const {
    const auto prim = _document.GetStage()->GetPrimAtPath(path);
    if (!prim) return {};
    PrimSummary result{prim.GetName().GetString(), prim.GetTypeName().GetString()};
    result.inPrototype = prim.IsInPrototype();
    result.active = prim.IsActive(); result.loaded = prim.IsLoaded(); result.instanceProxy = prim.IsInstanceProxy();
    if (UsdGeomImageable imageable{prim}) result.visibility = imageable.ComputeVisibility(_document.GetTime()).GetString();
    result.drawMode = UsdGeomModelAPI(prim).ComputeModelDrawMode().GetString();
    return result;
}
InspectorRows Browser::Properties(const SdfPath &path, const std::string &query) const {
    const auto prim = _document.GetStage()->GetPrimAtPath(path);
    if (!prim) return {};
    InspectorRows rows;
    for (const auto &property : prim.GetProperties()) {
        if (const auto attr = property.As<UsdAttribute>()) {
            VtValue value; attr.Get(&value, _document.GetTime());
            rows.push_back({attr.GetName().GetString(), attr.GetTypeName().GetAsToken().GetString(),
                            Value(value) + (attr.HasSpline() ? " (spline)" : ""), Source(attr, _document.GetTime())});
        } else if (const auto rel = property.As<UsdRelationship>()) {
            SdfPathVector targets; rel.GetTargets(&targets);
            std::string value;
            for (size_t i = 0; i < std::min(size_t(8), targets.size()); ++i) value += (i ? ", " : "") + targets[i].GetString();
            rows.push_back({rel.GetName().GetString(), "relationship", Short(value), Source(rel, _document.GetTime())});
        }
    }
    if (!query.empty()) {
        const auto needle = Lower(query);
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const InspectorRow &row) {
            return Lower(row.name + " " + row.type).find(needle) == std::string::npos;
        }), rows.end());
    }
    return rows;
}
SplinePlot Browser::SplinePreview(const SdfPath &path, const std::string &property, size_t samples) const {
    const auto prim = _document.GetStage()->GetPrimAtPath(path);
    const auto attr = prim ? prim.GetAttribute(TfToken(property)) : UsdAttribute();
    SplinePlot plot;
    if (!attr || !attr.HasSpline()) return plot;
    const auto knots = attr.GetSpline().GetKnots();
    if (knots.empty()) return plot;
    const double first = knots.begin()->GetTime(), last = std::prev(knots.end())->GetTime();
    if (!std::isfinite(first) || !std::isfinite(last)) return plot;
    auto evaluate = [&](double time, std::vector<GfVec2d> *points) {
        VtValue value;
        if (!attr.Get(&value, UsdTimeCode(time))) return;
        const auto numeric = VtValue::Cast<double>(value);
        if (numeric.IsHolding<double>() && std::isfinite(numeric.UncheckedGet<double>()))
            points->emplace_back(time, numeric.UncheckedGet<double>());
    };
    samples = std::clamp(samples, size_t(2), size_t(1024));
    for (size_t i = 0; i < samples; ++i) {
        const double f = double(i) / (samples - 1);
        evaluate(first * (1.0 - f) + last * f, &plot.samples);
    }
    // The complete knot table remains paginated; the graph marks at most 256.
    for (const auto &knot : knots) {
        if (plot.knots.size() >= 256) break;
        evaluate(knot.GetTime(), &plot.knots);
    }
    return plot;
}
InspectorRows Browser::PropertyMetadata(const SdfPath &path, const std::string &property) const {
    const auto prim = _document.GetStage()->GetPrimAtPath(path);
    const auto prop = prim ? prim.GetProperty(TfToken(property)) : UsdProperty();
    InspectorRows rows;
    if (prop) MetadataRows(prop.GetAllMetadata(), "", &rows);
    return rows;
}
InspectorRows Browser::PropertyOpinions(const SdfPath &path, const std::string &property) const {
    const auto prim = _document.GetStage()->GetPrimAtPath(path);
    const auto prop = prim ? prim.GetProperty(TfToken(property)) : UsdProperty();
    InspectorRows rows;
    if (!prop) return rows;
    for (const auto &[spec, offset] : prop.GetPropertyStackWithLayerOffsets(_document.GetTime())) {
        const auto layer = spec->GetLayer();
        const std::string source = layer->GetIdentifier() + " @ " + spec->GetPath().GetString();
        rows.push_back({"Opinion", "layer", source, "offset " + TfStringify(offset)});
        for (const auto &field : spec->ListFields()) {
            if (rows.size() >= 1024) { rows.push_back({"…", "", "Opinion preview limited to 1024 rows", ""}); return rows; }
            const auto value = spec->GetField(field);
            const std::string preview = field == SdfFieldKeys->TimeSamples ?
                std::to_string(layer->GetNumTimeSamplesForPath(spec->GetPath())) + " authored time samples" : Value(value);
            rows.push_back({field.GetString(), value.GetTypeName(), preview, source});
        }
    }
    return rows;
}
InspectorRows Browser::LayerDetails(const std::string &identifier) const {
    for (const auto &layer : _document.GetStage()->GetUsedLayers()) {
        if (layer->GetIdentifier() != identifier) continue;
        InspectorRows rows{{"Identifier", "layer", identifier, ""},
            {"Path", "", layer->GetRealPath(), ""},
            {"Dirty", "bool", layer->IsDirty() ? "true" : "false", ""},
            {"Editable", "bool", layer->PermissionToEdit() ? "true" : "false", ""}};
        for (const auto &field : layer->ListFields(SdfPath::AbsoluteRootPath())) {
            const auto value = layer->GetField(SdfPath::AbsoluteRootPath(), field);
            rows.push_back({field.GetString(), value.GetTypeName(), Value(value), "layer metadata"});
        }
        for (const auto &subLayer : layer->GetSubLayerPaths())
            rows.push_back({"Sublayer", "asset path", subLayer, "relative to this layer"});
        return rows;
    }
    return {};
}
InspectorRows Browser::Metadata(const SdfPath &path) const {
    const auto prim = _document.GetStage()->GetPrimAtPath(path);
    if (!prim) return {};
    InspectorRows rows;
    MetadataRows(prim.GetAllMetadata(), "", &rows);
    return rows;
}
InspectorRows Browser::Composition(const SdfPath &path) const {
    const auto prim = _document.GetStage()->GetPrimAtPath(path);
    if (!prim) return {};
    if (prim.IsInstanceProxy()) return {{"Prototype", "instance proxy", PrototypePath(path).GetString(), "generated by USD"}};
    InspectorRows rows;
    auto query = UsdPrimCompositionQuery(prim);
    for (const auto &arc : query.GetCompositionArcs()) {
        const auto layer = arc.GetTargetLayer();
        rows.push_back({arc.GetTargetPrimPath().GetString(), TfEnum::GetName(arc.GetArcType()),
                        layer ? layer->GetIdentifier() : "", arc.GetIntroducingLayer() ? arc.GetIntroducingLayer()->GetIdentifier() : "root"});
    }
    for (const auto &spec : prim.GetPrimStack())
        rows.push_back({spec->GetPath().GetString(), "prim spec", spec->GetLayer()->GetIdentifier(), "strong to weak"});
    return rows;
}
InspectorRows Browser::Layers() const {
    InspectorRows rows;
    for (const auto &layer : _document.GetStage()->GetLayerStack())
        rows.push_back({layer->GetDisplayName(), layer == _document.GetEditTarget() ? "edit target" : "layer",
                        layer->GetIdentifier(), layer->IsDirty() ? "modified" : "saved"});
    return rows;
}
InspectorRows Browser::Variants(const SdfPath &path) const {
    const auto prim = _document.GetStage()->GetPrimAtPath(path);
    if (!prim) return {};
    InspectorRows rows;
    auto sets = prim.GetVariantSets();
    for (const auto &name : sets.GetNames()) {
        auto set = sets.GetVariantSet(name);
        std::string choices;
        for (const auto &variant : set.GetVariantNames()) choices += (choices.empty() ? "" : ", ") + variant;
        rows.push_back({name, "variant set", set.GetVariantSelection(), Short(choices)});
    }
    return rows;
}
InspectorRows Browser::PropertyDetails(const SdfPath &path, const std::string &property,
                                      size_t offset, size_t limit) const {
    limit = std::min(limit, size_t(256));
    const auto prim = _document.GetStage()->GetPrimAtPath(path);
    if (!prim) return {};
    const auto attr = prim.GetAttribute(TfToken(property));
    if (!attr) {
        const auto rel = prim.GetRelationship(TfToken(property));
        if (!rel) return {};
        SdfPathVector targets; rel.GetTargets(&targets);
        InspectorRows rows{{"Targets", "relationship",
                            targets.empty() ? "No targets" : std::to_string(targets.size()) + " targets",
                            Source(rel, _document.GetTime())}};
        const size_t end = offset + std::min(limit, targets.size() - std::min(offset, targets.size()));
        for (size_t i = offset; i < end; ++i) rows.push_back({std::to_string(i), "target", targets[i].GetString(), ""});
        return rows;
    }
    VtValue value; attr.Get(&value, _document.GetTime());
    InspectorRows rows{{"Current value", attr.GetTypeName().GetAsToken().GetString(), Value(value), Source(attr, _document.GetTime())}};
    if (value.IsArrayValued()) {
        if (!Arrays(value, offset, limit, &rows)) rows.push_back({"Array", "", "Element preview unavailable for this type", ""});
    } else if (attr.HasSpline()) {
        const auto knots = attr.GetSpline().GetKnots();
        size_t i = 0;
        for (const auto &knot : knots) {
            if (i++ < offset) continue;
            if (i - offset > limit) break;
            VtValue v, pre, post; knot.GetValue(&v); knot.GetPreTanSlope(&pre); knot.GetPostTanSlope(&post);
            rows.push_back({TfStringify(knot.GetTime()), TfEnum::GetName(knot.GetNextInterpolation()),
                            Value(v), "slopes: " + Value(pre) + " / " + Value(post)});
        }
    } else {
        VtValue defaultValue; attr.Get(&defaultValue, UsdTimeCode::Default());
        rows.push_back({"Default", "", Value(defaultValue), Source(attr, UsdTimeCode::Default())});
        std::vector<double> times; attr.GetTimeSamples(&times);
        const size_t end = offset + std::min(limit, times.size() - std::min(offset, times.size()));
        for (size_t i = offset; i < end; ++i) {
            VtValue sample; attr.Get(&sample, UsdTimeCode(times[i]));
            rows.push_back({TfStringify(times[i]), "time sample", Value(sample), Source(attr, UsdTimeCode(times[i]))});
        }
    }
    return rows;
}
} // namespace vela
