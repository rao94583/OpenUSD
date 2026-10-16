// Phase 5 authoring operations, shared by the inspector and scripted clients.
#include "actions.h"
#include "document.h"
#include "pxr/base/gf/half.h"
#include "pxr/base/gf/matrix2d.h"
#include "pxr/base/gf/matrix3d.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/quatd.h"
#include "pxr/base/gf/quatf.h"
#include "pxr/base/gf/quath.h"
#include "pxr/base/gf/traits.h"
#include "pxr/base/gf/vec2d.h"
#include "pxr/base/gf/vec2h.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3h.h"
#include "pxr/base/gf/vec4d.h"
#include "pxr/base/gf/vec4h.h"
#include "pxr/usd/sdf/timeCode.h"
#include "pxr/base/vt/array.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/variantSets.h"
#include "pxr/usd/usdGeom/imageable.h"
#include "pxr/usd/usdGeom/modelAPI.h"
#include "pxr/usd/sdf/assetPath.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace vela {
namespace {

double Number(const JsValue &v) {
    return v.IsReal() ? v.GetReal() : v.IsUInt64() ? double(v.GetUInt64()) : double(v.GetInt64());
}

// Convert against the attribute's actual USD type. Never infer its type
// from JSON (an integer JSON literal may be a floating-point USD value).
template<class T> bool Read(const JsValue &v, T *out) {
    if constexpr (std::is_same_v<T, GfHalf>) {
        // Half's largest finite value.
        if (!(v.IsReal() || v.IsInt() || v.IsUInt64())) return false;
        const double n = Number(v);
        if (!std::isfinite(n) || std::abs(n) > 65504.0) return false;
        *out = GfHalf(float(n));
    } else if constexpr (std::is_same_v<T, SdfTimeCode>) {
        if (!(v.IsReal() || v.IsInt() || v.IsUInt64()) || !std::isfinite(Number(v))) return false;
        *out = SdfTimeCode(Number(v));
    } else if constexpr (GfIsGfQuat<T>::value) {
        // [real, i, j, k], as USD writes quaternions.
        if (!v.IsArray() || v.GetJsArray().size() != 4) return false;
        typename T::ScalarType c[4];
        for (int i = 0; i < 4; ++i) if (!Read(v.GetJsArray()[i], &c[i])) return false;
        *out = T(c[0], c[1], c[2], c[3]);
    } else if constexpr (GfIsGfMatrix<T>::value) {
        // Rows ([[1, 0], [0, 1]]) or all elements in row order.
        if (!v.IsArray()) return false;
        std::vector<JsValue> elements;
        for (const JsValue &row : v.GetJsArray()) {
            if (row.IsArray()) {
                if (row.GetJsArray().size() != size_t(T::numColumns)) return false;
                for (const JsValue &e : row.GetJsArray()) elements.push_back(e);
            } else {
                elements.push_back(row);
            }
        }
        if (elements.size() != size_t(T::numRows * T::numColumns)) return false;
        for (size_t i = 0; i < elements.size(); ++i) {
            if (!Read(elements[i], &(*out)[i / T::numColumns][i % T::numColumns])) return false;
        }
    } else if constexpr (std::is_same_v<T, bool>) {
        if (!v.IsBool()) return false;
        *out = v.GetBool();
    } else if constexpr (std::is_same_v<T, std::string> ||
                         std::is_same_v<T, TfToken> || std::is_same_v<T, SdfAssetPath>) {
        if (!v.IsString()) return false;
        *out = T(v.GetString());
    } else if constexpr (std::is_integral_v<T>) {
        // Preserve all 64 bits of integer JSON values, including uint64 max.
        if (v.IsUInt64()) {
            if (!std::in_range<T>(v.GetUInt64())) return false;
            *out = static_cast<T>(v.GetUInt64());
        } else if (v.IsInt()) {
            if (!std::in_range<T>(v.GetInt64())) return false;
            *out = static_cast<T>(v.GetInt64());
        } else if (v.IsReal()) {
            const double n = v.GetReal();
            const double limit = std::ldexp(1.0, std::numeric_limits<T>::digits);
            const double low = std::is_signed_v<T> ? -limit : 0.0;
            if (!std::isfinite(n) || std::floor(n) != n || n < low || n >= limit)
                return false;
            *out = static_cast<T>(n);
        } else return false;
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!(v.IsReal() || v.IsInt() || v.IsUInt64())) return false;
        const double n = Number(v);
        if (!std::isfinite(n) || n < std::numeric_limits<T>::lowest() ||
            n > std::numeric_limits<T>::max()) return false;
        *out = static_cast<T>(n);
    } else {
        if (!v.IsArray() || v.GetJsArray().size() != T::dimension) return false;
        for (int i = 0; i < T::dimension; ++i)
            if (!Read(v.GetJsArray()[i], &(*out)[i])) return false;
    }
    return true;
}

template<class T> bool Convert(const JsValue &v, const VtValue &model, VtValue *out) {
    if (model.IsHolding<T>()) {
        T value;
        if (!Read(v, &value)) return false;
        *out = VtValue(value);
        return true;
    }
    if (model.IsHolding<VtArray<T>>() && v.IsArray()) {
        VtArray<T> value(v.GetJsArray().size());
        for (size_t i = 0; i < value.size(); ++i) {
            T element;
            if (!Read(v.GetJsArray()[i], &element)) return false;
            value[i] = element;
        }
        *out = VtValue(value);
        return true;
    }
    return false;
}

bool ConvertValue(const JsValue &v, const SdfValueTypeName &type, VtValue *out) {
    const VtValue &model = type.GetDefaultValue();
    return Convert<bool>(v, model, out) || Convert<int>(v, model, out) ||
        Convert<unsigned int>(v, model, out) || Convert<int64_t>(v, model, out) ||
        Convert<uint64_t>(v, model, out) || Convert<float>(v, model, out) ||
        Convert<double>(v, model, out) || Convert<std::string>(v, model, out) ||
        Convert<TfToken>(v, model, out) || Convert<SdfAssetPath>(v, model, out) ||
        Convert<GfVec2f>(v, model, out) || Convert<GfVec3f>(v, model, out) ||
        Convert<GfVec4f>(v, model, out) || Convert<GfVec2d>(v, model, out) ||
        Convert<GfVec3d>(v, model, out) || Convert<GfVec4d>(v, model, out) ||
        Convert<GfVec2i>(v, model, out) || Convert<GfVec3i>(v, model, out) ||
        Convert<GfVec4i>(v, model, out) || Convert<unsigned char>(v, model, out) ||
        Convert<GfHalf>(v, model, out) || Convert<SdfTimeCode>(v, model, out) ||
        Convert<GfVec2h>(v, model, out) || Convert<GfVec3h>(v, model, out) ||
        Convert<GfVec4h>(v, model, out) || Convert<GfQuatf>(v, model, out) ||
        Convert<GfQuatd>(v, model, out) || Convert<GfQuath>(v, model, out) ||
        Convert<GfMatrix2d>(v, model, out) || Convert<GfMatrix3d>(v, model, out) ||
        Convert<GfMatrix4d>(v, model, out);
}

UsdPrim Prim(Document &d, const JsObject &p) {
    const SdfPath path(p.at("path").GetString());
    if (!path.IsAbsolutePath() || !path.IsPrimPath()) return {};
    auto prim = d.GetStage()->GetPrimAtPath(path);
    return prim && !prim.IsInPrototype() ? prim : UsdPrim();
}
bool Editable(const Document &d) { return d.GetEditTarget()->PermissionToEdit(); }
UsdTimeCode Time(const JsObject &p) {
    auto time = p.find("time");
    return time == p.end() ? UsdTimeCode::Default() : UsdTimeCode(Number(time->second));
}
ActionResult Status(bool ok) {
    return ok ? ActionResult() : ActionResult::Error("USD could not author the edit in the current edit target");
}

void AttributeActions(ActionRegistry &r) {
    r.Register({
        .name = "set_attribute", .title = "Set Attribute",
        .description = "Author an existing attribute in the edit target. Omit time to edit the default, or supply a numeric time sample. JSON values must match the USD type: scalars, vectors ([x, y, z]), quaternions ([real, i, j, k]), matrices (rows, [[1, 0], [0, 1]], or elements in row order), or arrays of those.",
        .parameters = ParameterSchema(R"({"properties": {
            "path": {"type": "string"}, "attribute": {"type": "string"},
            "value": {}, "time": {"type": "number"}},
            "required": ["path", "attribute", "value"]})"),
        .undoable = true,
        .run = [](Document &d, const JsObject &p) {
            auto prim = Prim(d, p);
            if (!prim || prim.IsInstanceProxy()) return ActionResult::Error("Expected an editable prim");
            auto attr = prim.GetAttribute(TfToken(p.at("attribute").GetString()));
            if (!attr) return ActionResult::Error("No such attribute on the prim");
            VtValue value;
            if (!ConvertValue(p.at("value"), attr.GetTypeName(), &value))
                return ActionResult::Error("Unsupported or invalid value for USD type " + attr.GetTypeName().GetAsToken().GetString());
            if (!Time(p).IsDefault() && attr.GetVariability() == SdfVariabilityUniform)
                return ActionResult::Error("Uniform attributes cannot have time samples");
            return Status(attr.Set(value, Time(p)));
        }, .enabled = Editable,
    });
    r.Register({
        .name = "clear_attribute", .title = "Clear Attribute",
        .description = "Remove the attribute's authored default or one numeric time sample from the edit target, exposing weaker opinions or the schema fallback.",
        .parameters = ParameterSchema(R"({"properties": {
            "path": {"type": "string"}, "attribute": {"type": "string"},
            "time": {"type": "number"}}, "required": ["path", "attribute"]})"),
        .undoable = true,
        .run = [](Document &d, const JsObject &p) {
            auto prim = Prim(d, p);
            if (!prim || prim.IsInstanceProxy()) return ActionResult::Error("Expected an editable prim");
            auto attr = prim.GetAttribute(TfToken(p.at("attribute").GetString()));
            return attr ? Status(attr.ClearAtTime(Time(p))) : ActionResult::Error("No such attribute on the prim");
        }, .enabled = Editable,
    });
}
void SelectionActions(ActionRegistry &r) {
    // Resolve and validate the entire selection before authoring anything.
    // Children precede parents so deactivation never invalidates a pending path.
    for (const std::string name : {"hide_selection", "show_selection",
                                   "activate_selection", "deactivate_selection",
                                   "load_selection", "unload_selection"}) {
        const bool visibility = name == "hide_selection" || name == "show_selection";
        const bool activation = name == "activate_selection" || name == "deactivate_selection";
        const bool authored = visibility || activation;
        const std::string title = name == "hide_selection" ? "Hide Selected" :
            name == "show_selection" ? "Show Selected" :
            name == "activate_selection" ? "Activate Selected" :
            name == "deactivate_selection" ? "Deactivate Selected" :
            name == "load_selection" ? "Load Selected" : "Unload Selected";
        r.Register({
            .name = name, .title = title,
            .description = title + (authored ? " in the current edit target, as one undo step." : " and descendants, without authoring scene data."),
            .menu = "Prim", .separatorBefore = name == "activate_selection" || name == "load_selection",
            .undoable = authored,
            .run = [name, visibility, activation](Document &d, const JsObject &) {
                SdfPathVector paths = d.GetSelection();
                for (const auto &path : paths) {
                    const auto prim = d.GetStage()->GetPrimAtPath(path);
                    if (!prim || prim.IsInstanceProxy() || prim.IsInPrototype() || (visibility && !UsdGeomImageable(prim)))
                        return ActionResult::Error("Selection contains a prim that cannot perform this operation: " + path.GetString());
                }
                std::sort(paths.begin(), paths.end(), [](const SdfPath &a, const SdfPath &b) {
                    return a.GetPathElementCount() > b.GetPathElementCount();
                });
                for (const auto &path : paths) {
                    const auto prim = d.GetStage()->GetPrimAtPath(path);
                    if (visibility) {
                        if (!UsdGeomImageable(prim).CreateVisibilityAttr().Set(
                            name == "hide_selection" ? UsdGeomTokens->invisible : UsdGeomTokens->inherited))
                            return Status(false);
                    } else if (activation) {
                        if (!prim.SetActive(name == "activate_selection")) return Status(false);
                    } else if (name == "load_selection") d.GetStage()->Load(path);
                    else d.GetStage()->Unload(path);
                }
                return ActionResult();
            },
            .enabled = [authored](const Document &d) {
                return !d.GetSelection().empty() && (!authored || Editable(d));
            },
        });
    }
}

} // namespace

void RegisterEditingActions(ActionRegistry &r) {
    AttributeActions(r);
    SelectionActions(r);
    r.Register({
        .name = "set_variant", .title = "Set Variant",
        .description = "Choose an existing variant in the edit target. An empty variant clears this layer's selection opinion.",
        .parameters = ParameterSchema(R"({"properties": {"path": {"type": "string"},
            "set": {"type": "string"}, "variant": {"type": "string"}},
            "required": ["path", "set", "variant"]})"),
        .undoable = true,
        .run = [](Document &d, const JsObject &p) {
            auto prim = Prim(d, p);
            if (!prim || prim.IsInstanceProxy()) return ActionResult::Error("Expected an editable prim");
            auto sets = prim.GetVariantSets();
            const auto &name = p.at("set").GetString();
            if (!sets.HasVariantSet(name)) return ActionResult::Error("No such variant set");
            auto set = sets.GetVariantSet(name);
            const auto &variant = p.at("variant").GetString();
            if (variant.empty()) return Status(set.ClearVariantSelection());
            const auto names = set.GetVariantNames();
            if (std::find(names.begin(), names.end(), variant) == names.end())
                return ActionResult::Error("No such variant in the set");
            return Status(set.SetVariantSelection(variant));
        }, .enabled = Editable,
    });
    r.Register({
        .name = "set_active", .title = "Set Active",
        .description = "Author prim activation in the current edit target; deactivation removes descendants from composition.",
        .parameters = ParameterSchema(R"({"properties": {"path": {"type": "string"},
            "active": {"type": "boolean"}}, "required": ["path", "active"]})"),
        .undoable = true,
        .run = [](Document &d, const JsObject &p) {
            auto prim = Prim(d, p);
            if (!prim || prim.IsInstanceProxy()) return ActionResult::Error("Expected an editable prim");
            return Status(prim.SetActive(p.at("active").GetBool()));
        }, .enabled = Editable,
    });
    r.Register({
        .name = "set_visibility", .title = "Set Visibility",
        .description = "Author inherited or invisible visibility on an imageable prim, at the default or a numeric time sample. Inherited visibility still respects invisible ancestors.",
        .parameters = ParameterSchema(R"({"properties": {"path": {"type": "string"},
            "visibility": {"type": "string", "enum": ["inherited", "invisible"]},
            "time": {"type": "number"}}, "required": ["path", "visibility"]})"),
        .undoable = true,
        .run = [](Document &d, const JsObject &p) {
            auto prim = Prim(d, p);
            UsdGeomImageable imageable(prim);
            if (!imageable || prim.IsInstanceProxy()) return ActionResult::Error("Expected an editable imageable prim");
            return Status(imageable.CreateVisibilityAttr().Set(TfToken(p.at("visibility").GetString()), Time(p)));
        }, .enabled = Editable,
    });
    r.Register({
        .name = "set_purpose", .title = "Set Purpose",
        .description = "Author default, proxy, render, or guide purpose on an imageable prim in the current edit target.",
        .parameters = ParameterSchema(R"({"properties": {"path": {"type": "string"},
            "purpose": {"type": "string", "enum": ["default", "proxy", "render", "guide"]}},
            "required": ["path", "purpose"]})"),
        .undoable = true,
        .run = [](Document &d, const JsObject &p) {
            auto prim = Prim(d, p);
            UsdGeomImageable imageable(prim);
            if (!imageable || prim.IsInstanceProxy()) return ActionResult::Error("Expected an editable imageable prim");
            return Status(imageable.CreatePurposeAttr().Set(TfToken(p.at("purpose").GetString())));
        }, .enabled = Editable,
    });
    r.Register({
        .name = "set_model_draw_mode", .title = "Set Model Draw Mode",
        .description = "Author model:drawMode in the edit target. USD model kind and model:applyDrawMode determine whether the renderer uses it.",
        .parameters = ParameterSchema(R"({"properties": {"path": {"type": "string"},
            "mode": {"type": "string", "enum": ["default", "inherited", "origin", "bounds", "cards"]}},
            "required": ["path", "mode"]})"),
        .undoable = true,
        .run = [](Document &d, const JsObject &p) {
            auto prim = Prim(d, p);
            if (!prim || prim.IsInstanceProxy()) return ActionResult::Error("Expected an editable prim");
            return Status(UsdGeomModelAPI(prim).CreateModelDrawModeAttr().Set(TfToken(p.at("mode").GetString())));
        }, .enabled = Editable,
    });
    r.Register({
        .name = "set_payload_loaded", .title = "Load / Unload Payload",
        .description = "Load or unload a prim and its descendants. Load rules are document state, so this operation does not author layer opinions or create an undo step.",
        .parameters = ParameterSchema(R"({"properties": {"path": {"type": "string"},
            "loaded": {"type": "boolean"}}, "required": ["path", "loaded"]})"),
        .run = [](Document &d, const JsObject &p) {
            auto prim = Prim(d, p);
            if (!prim || prim.IsInstanceProxy()) return ActionResult::Error("Expected a prim outside an instance proxy");
            const SdfPath path = prim.GetPath();
            if (p.at("loaded").GetBool()) d.GetStage()->Load(path);
            else d.GetStage()->Unload(path);
            return ActionResult();
        },
    });
}
} // namespace vela
