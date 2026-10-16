#include "color_codes.h"

namespace vela {

namespace {

// usdviewq/common.py: UIBaseColors.
constexpr Rgb Red{230, 132, 131};
constexpr Rgb LightSkyBlue{135, 206, 250};
constexpr Rgb DarkYellow{222, 158, 46};

} // namespace

Rgb ColorOf(PrimStyle style) {
    switch (style) {
    case PrimStyle::HasArcs: return DarkYellow;
    case PrimStyle::Instance: return LightSkyBlue;
    case PrimStyle::Prototype: return Rgb{118, 136, 217};
    case PrimStyle::Normal: break;
    }
    return Rgb{227, 227, 227};
}

Rgb ColorOf(ValueSource source) {
    switch (source) {
    case ValueSource::Fallback: return DarkYellow;
    case ValueSource::Default: return LightSkyBlue;
    case ValueSource::TimeSamples: return Rgb{177, 207, 153};
    case ValueSource::ValueClips: return Rgb{230, 150, 230};
    case ValueSource::Spline: return Rgb{80, 200, 180};
    case ValueSource::Relationship: return Red;
    case ValueSource::None: break;
    }
    return Rgb{140, 140, 140};
}

std::vector<LegendEntry> PrimLegend() {
    using S = LegendEntry::Swatch;
    return {
        {S::Color, ColorOf(PrimStyle::Normal), "Normal", "A prim with no composition arcs"},
        {S::Color, ColorOf(PrimStyle::HasArcs), "Has arcs",
         "References, payloads, inherits, specializes or variant sets"},
        {S::Color, ColorOf(PrimStyle::Instance), "Instance", "An instanceable prim's instance"},
        {S::Color, ColorOf(PrimStyle::Prototype), "Prototype",
         "Inside a prototype, or an instance proxy"},
        {S::Dimmed, ColorOf(PrimStyle::Normal), "Dimmed", "Inactive"},
        {S::Bold, ColorOf(PrimStyle::Normal), "Bold", "Defined (def)"},
        {S::Italic, ColorOf(PrimStyle::Normal), "Italic", "Undefined (over)"},
        {S::Regular, ColorOf(PrimStyle::Normal), "Regular", "Abstract (class, and its children)"},
    };
}

std::vector<LegendEntry> PropertyLegend() {
    using S = LegendEntry::Swatch;
    return {
        {S::Color, ColorOf(ValueSource::Default), "Default", "The attribute's default value"},
        {S::Color, ColorOf(ValueSource::TimeSamples), "Time samples", "Authored time samples"},
        {S::Color, ColorOf(ValueSource::Spline), "Spline", "An animation spline"},
        {S::Color, ColorOf(ValueSource::ValueClips), "Value clips", "Samples from value clips"},
        {S::Color, ColorOf(ValueSource::Fallback), "Fallback", "The schema's fallback; nothing authored"},
        {S::Color, ColorOf(ValueSource::None), "No value", "Neither authored nor a fallback"},
        {S::Color, ColorOf(ValueSource::Relationship), "Relationship",
         "Targets other prims or properties; right-click to go to one"},
        {S::Italic, ColorOf(ValueSource::TimeSamples), "Italic",
         "Interpolated between time samples or spline knots"},
    };
}

} // namespace vela
