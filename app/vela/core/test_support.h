// Shared by the core's tests: plugin registration and a CHECK macro.
#pragma once

#include "pxr/base/plug/registry.h"

#include <cstdio>
#include <filesystem>

namespace vela::test {

inline int failures = 0;

// The install tree's resources, from the test's runfiles (see BUILD.bazel).
// Tests run in their runfiles directory.
inline void RegisterPlugins() {
    const std::filesystem::path install =
        std::filesystem::current_path() / "bazel/install";
    PXR_NS::PlugRegistry::GetInstance().RegisterPlugins(
        (install / "lib/usd/plugInfo.json").string());
    PXR_NS::PlugRegistry::GetInstance().RegisterPlugins(
        (install / "plugin/usd/plugInfo.json").string());
}

inline int Finish(const char *name) {
    if (failures) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("All %s checks passed\n", name);
    return 0;
}

} // namespace vela::test

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
                         __FILE__, __LINE__, #condition); \
            ++vela::test::failures; \
        } \
    } while (0)
