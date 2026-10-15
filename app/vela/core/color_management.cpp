#include "color_management.h"

#include <OpenColorIO/OpenColorIO.h>

namespace OCIO = OCIO_NAMESPACE;

namespace vela {

namespace {

OCIO::ConstConfigRcPtr Config() {
    try {
        return OCIO::GetCurrentConfig();
    } catch (const OCIO::Exception &) {
        return nullptr;
    }
}

} // namespace

std::vector<std::pair<std::string, std::string>> OcioDisplayViews() {
    std::vector<std::pair<std::string, std::string>> result;
    if (OCIO::ConstConfigRcPtr config = Config()) {
        for (int d = 0; d < config->getNumDisplays(); ++d) {
            const char *display = config->getDisplay(d);
            for (int v = 0; v < config->getNumViews(display); ++v) {
                result.emplace_back(display, config->getView(display, v));
            }
        }
    }
    return result;
}

std::vector<std::string> OcioColorSpaces() {
    std::vector<std::string> result;
    if (OCIO::ConstConfigRcPtr config = Config()) {
        for (int i = 0; i < config->getNumColorSpaces(); ++i) {
            result.emplace_back(config->getColorSpaceNameByIndex(i));
        }
    }
    return result;
}

std::vector<std::string> OcioLooks() {
    std::vector<std::string> result;
    if (OCIO::ConstConfigRcPtr config = Config()) {
        for (int i = 0; i < config->getNumLooks(); ++i) {
            result.emplace_back(config->getLookNameByIndex(i));
        }
    }
    return result;
}

} // namespace vela
