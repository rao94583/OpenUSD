// usdview's color coding for the prim browser and inspector (usdviewq's
// common.py), shared by the views and the legend that explains them.
//
// Prims: the name's color says what the prim is (it has composition arcs,
// is an instance, or comes from a prototype), its font says how it is
// declared (def, over or class), and inactive prims are dimmed. Property
// values: the color says where the value comes from at the current time;
// italic means it is interpolated between time samples or spline knots.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vela {

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb &other) const = default;
};

// In usdview's order of precedence: an instance with arcs is an instance.
enum class PrimStyle { Normal, HasArcs, Instance, Prototype };
// def (bold), over (italic), class and its children (regular).
enum class PrimFont { Defined, Over, Abstract };
enum class ValueSource { None, Fallback, Default, TimeSamples, ValueClips, Spline, Relationship };

Rgb ColorOf(PrimStyle style);
Rgb ColorOf(ValueSource source);

// One line of a legend.
struct LegendEntry {
    enum class Swatch { Color, Bold, Italic, Regular, Dimmed };
    Swatch swatch = Swatch::Color;
    Rgb color;
    std::string label;
    std::string description;
};
std::vector<LegendEntry> PrimLegend();
std::vector<LegendEntry> PropertyLegend();

} // namespace vela
