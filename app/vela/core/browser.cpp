#include "browser.h"
#include "pxr/base/tf/enum.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/ts/spline.h"
#include "pxr/base/vt/array.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/primCompositionQuery.h"
#include "pxr/usd/usd/relationship.h"
#include "pxr/usd/usd/variantSets.h"
#include "pxr/usd/usdGeom/imageable.h"
#include "pxr/usd/usdGeom/modelAPI.h"
#include "pxr/usd/sdf/assetPath.h"
#include <algorithm>
#include <cctype>

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
template<class T> bool ArrayRows(const VtValue &value, size_t offset,
                                size_t limit, InspectorRows *rows) {
    if (!value.IsHolding<VtArray<T>>()) return false;
    const auto &array = value.UncheckedGet<VtArray<T>>();
    const size_t end = offset + std::min(limit, array.size() - std::min(offset, array.size()));
    for (size_t i = offset; i < end; ++i)
        rows->push_back({"[" + std::to_string(i) + "]", "element", Short(TfStringify(array[i])), ""});
    return true;
}
bool Arrays(const VtValue &v, size_t offset, size_t limit, InspectorRows *rows) {
    return ArrayRows<bool>(v, offset, limit, rows) || ArrayRows<int>(v, offset, limit, rows) ||
        ArrayRows<unsigned int>(v, offset, limit, rows) || ArrayRows<int64_t>(v, offset, limit, rows) ||
        ArrayRows<uint64_t>(v, offset, limit, rows) || ArrayRows<float>(v, offset, limit, rows) ||
        ArrayRows<double>(v, offset, limit, rows) || ArrayRows<std::string>(v, offset, limit, rows) ||
        ArrayRows<TfToken>(v, offset, limit, rows) || ArrayRows<SdfAssetPath>(v, offset, limit, rows) ||
        ArrayRows<GfVec2f>(v, offset, limit, rows) || ArrayRows<GfVec3f>(v, offset, limit, rows) ||
        ArrayRows<GfVec4f>(v, offset, limit, rows) || ArrayRows<GfVec2d>(v, offset, limit, rows) ||
        ArrayRows<GfVec3d>(v, offset, limit, rows) || ArrayRows<GfVec4d>(v, offset, limit, rows) ||
        ArrayRows<GfVec2i>(v, offset, limit, rows) || ArrayRows<GfVec3i>(v, offset, limit, rows) ||
        ArrayRows<GfVec4i>(v, offset, limit, rows) || ArrayRows<GfMatrix4d>(v, offset, limit, rows);
}
} // namespace

Browser::Browser(Document &document) : _document(document) {
    _observer = document.AddObserver([this](unsigned changes) {
        if (changes & Document::StageContent) _matchesValid = false;
    });
}
Browser::~Browser() { _document.RemoveObserver(_observer); }
void Browser::SetFilter(std::string query) {
    query = Lower(std::move(query));
    if (_query != query) { _query = std::move(query); _matchesValid = false; }
}
void Browser::_BuildMatches() {
    _matches.clear();
    for (const auto &prim : UsdPrimRange(_document.GetStage()->GetPseudoRoot(),
                                       UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate))) {
        if (Lower(prim.GetPath().GetString() + " " + prim.GetTypeName().GetString()).find(_query) == std::string::npos)
            continue;
        for (SdfPath path = prim.GetPath(); !path.IsEmpty(); path = path.GetParentPath())
            _matches.insert(path);
    }
    _matchesValid = true;
}
SdfPathVector Browser::Children(const SdfPath &parent) {
    const auto prim = _document.GetStage()->GetPrimAtPath(parent);
    if (!prim) return {};
    if (!_query.empty() && !_matchesValid) _BuildMatches();
    SdfPathVector paths;
    for (const auto &child : prim.GetFilteredChildren(UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate)))
        if (_query.empty() || _matches.count(child.GetPath())) paths.push_back(child.GetPath());
    return paths;
}
bool Browser::HasChildren(const SdfPath &parent) {
    const auto prim = _document.GetStage()->GetPrimAtPath(parent);
    if (!prim) return false;
    if (!_query.empty() && !_matchesValid) _BuildMatches();
    for (const auto &child : prim.GetFilteredChildren(UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate)))
        if (_query.empty() || _matches.count(child.GetPath())) return true;
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
    result.active = prim.IsActive(); result.loaded = prim.IsLoaded(); result.instanceProxy = prim.IsInstanceProxy();
    if (UsdGeomImageable imageable{prim}) result.visibility = imageable.ComputeVisibility(_document.GetTime()).GetString();
    result.drawMode = UsdGeomModelAPI(prim).ComputeModelDrawMode().GetString();
    return result;
}
InspectorRows Browser::Properties(const SdfPath &path) const {
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
    return rows;
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
    if (!prim || prim.IsInstanceProxy()) return {};
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
        InspectorRows rows;
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
