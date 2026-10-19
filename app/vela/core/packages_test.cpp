// Per-user Python packages without running pip: folders, the manifest,
// constraints from a site-packages folder, requirements and pip's command.
// Installing is checked in a launched app (packages_ui_test).
#include "packages.h"
#include "test_support.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace vela;
using namespace vela::packages;

namespace {

void TestFolders() {
    unsetenv("VELA_PYTHON_PACKAGES");
    setenv("HOME", "/Users/x", 1);
    CHECK(BaseFolder("3.11") == "/Users/x/Library/Application Support/Vela/Python/3.11");
    CHECK(SitePackages("/b", "3.11") == "/b/lib/python3.11/site-packages");
    setenv("VELA_PYTHON_PACKAGES", "/tmp/p", 1);
    CHECK(BaseFolder("3.11") == "/tmp/p");
    unsetenv("VELA_PYTHON_PACKAGES");
}

void TestRequirements() {
    CHECK(RequirementName("NumPy>=2") == "numpy");
    CHECK(RequirementName("  Foo_Bar.baz [extra] ; python_version>'3'") == "foo-bar-baz");
    CHECK(RequirementName(">=2").empty());
    CHECK(CheckRequirement("numpy>=2").empty());
    CHECK(CheckRequirement("requests[socks]==2.32").empty());
    CHECK(!CheckRequirement("").empty());
    CHECK(!CheckRequirement("--index-url http://evil").empty());
    CHECK(!CheckRequirement("-e .").empty());
    CHECK(!CheckRequirement("https://example.com/x.whl").empty());
    CHECK(!CheckRequirement("pkg @ file:///tmp/x").empty());
    CHECK(!CheckRequirement("/tmp/x.whl").empty());
    CHECK(CheckRequirement("usd-core").find("second USD") != std::string::npos);
    CHECK(!CheckRequirement("USD_Core>=24").empty());
}

void TestManifest() {
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() / ("vela-packages-" + std::to_string(getpid()));
    std::filesystem::remove_all(base);
    CHECK(ReadManifest(base.string()).All().empty());
    Manifest manifest;
    manifest.plugins["b"] = {"numpy>=2", "pillow"};
    manifest.plugins["a"] = {"numpy>=2"};
    manifest.requested = {"humanize", "pillow"};
    CHECK((manifest.All() == std::vector<std::string>{"numpy>=2", "pillow", "humanize"}));
    CHECK(WriteManifest(base.string(), manifest));
    const Manifest read = ReadManifest(base.string());
    CHECK(read.ToJson() == manifest.ToJson());

    // Constraints from dist-info folders.
    const std::filesystem::path site = base / "site";
    std::filesystem::create_directories(site / "pyobjc_core-12.2.2.dist-info");
    std::filesystem::create_directories(site / "pyobjc_framework_Cocoa-12.2.2.dist-info");
    std::filesystem::create_directories(site / "objc");
    CHECK((Constraints(site.string()) ==
           std::vector<std::string>{"pyobjc-core==12.2.2", "pyobjc-framework-cocoa==12.2.2"}));
    CHECK(Constraints((base / "missing").string()).empty());
    std::filesystem::remove_all(base);
}

void TestCommand() {
    setenv("HOME", "/Users/x", 1);
    const PipCommand command = InstallCommand("/App/python3", "/base", "/App/site", "/base/c.txt",
                                              {"numpy>=2"}, {"--no-index", "--find-links", "/w"});
    const auto &a = command.args;
    CHECK(a[0] == "/App/python3" && a[1] == "-m" && a[2] == "pip" && a[3] == "install");
    CHECK(std::find(a.begin(), a.end(), "--user") != a.end());
    auto after = [&](const std::string &flag) {
        auto it = std::find(a.begin(), a.end(), flag);
        return it != a.end() && it + 1 != a.end() ? *(it + 1) : std::string();
    };
    CHECK(after("--constraint") == "/base/c.txt");
    CHECK(after("--find-links") == "/w" && a.back() == "numpy>=2");
    const auto &env = command.environment;
    CHECK(std::find(env.begin(), env.end(), "PYTHONUSERBASE=/base") != env.end());
    CHECK(std::find(env.begin(), env.end(), "PYTHONPATH=/App/site") != env.end());
    CHECK(std::find(env.begin(), env.end(), "HOME=/Users/x") != env.end());
    CHECK((SplitArguments(" --no-index  --find-links /w ") ==
           std::vector<std::string>{"--no-index", "--find-links", "/w"}));
    CHECK((SplitArguments(R"(--find-links "/a b/c" '' x'y z')") ==
           std::vector<std::string>{"--find-links", "/a b/c", "", "xy z"}));
}

} // namespace

int main() {
    TestFolders();
    TestRequirements();
    TestManifest();
    TestCommand();
    return test::Finish("packages");
}
