// OpenColorIO's choices for the current config ($OCIO, or OCIO's default),
// for menus. Rendering uses Storm's OCIO color correction.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace vela {

// The config's display and view pairs, its color spaces and looks. Empty
// when the config cannot be loaded.
std::vector<std::pair<std::string, std::string>> OcioDisplayViews();
std::vector<std::string> OcioColorSpaces();
std::vector<std::string> OcioLooks();

} // namespace vela
