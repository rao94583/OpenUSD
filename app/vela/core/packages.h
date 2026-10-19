// Python packages installed on the user's machine, for plugins and
// scripts: the bundle can't hold every package a plugin might need.
//
// Python's search path has three layers, in this order: USD's pxr, the
// bundle's pinned packages (Runtime/site-packages: PyObjC), and a per-user
// folder that the bundled pip installs into with its --user scheme. The
// bundle's packages always win; pip gets them as constraints so it never
// installs other versions of them.
//
// Plugins declare what they need in plugInfo.json ("PythonRequires":
// ["numpy>=2"]); a manifest records who asked for what, and every install
// resolves all of it together, since one Python process can only import one
// version of a module. Installing always needs the user's consent (the app
// asks); this file only describes the work.
#pragma once

#include "pxr/pxr.h"
#include "pxr/base/js/value.h"

#include <map>
#include <string>
#include <vector>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

namespace packages {

// ~/Library/Application Support/Vela/Python/<version> (PYTHONUSERBASE for
// pip), or VELA_PYTHON_PACKAGES when set (tests). Keyed by Python version:
// compiled packages only load in the Python they were built for.
std::string BaseFolder(const std::string &pythonVersion);
// Where pip's --user scheme puts packages under the base folder.
std::string SitePackages(const std::string &base, const std::string &pythonVersion);

// Who asked for which requirements (PEP 508 strings).
struct Manifest {
    std::map<std::string, std::vector<std::string>> plugins;  // by plugin name
    std::vector<std::string> requested;  // by scripts, the console and Claude
    // Every requirement, each once, in a stable order.
    std::vector<std::string> All() const;
    JsObject ToJson() const;
};
Manifest ReadManifest(const std::string &base);
bool WriteManifest(const std::string &base, const Manifest &manifest);

// "name==version" for each distribution in a site-packages folder (from its
// *.dist-info), as pip constraints.
std::vector<std::string> Constraints(const std::string &sitePackages);

// The distribution a requirement names, normalized ("Foo_Bar>=1" ->
// "foo-bar"), or "" if it isn't one.
std::string RequirementName(const std::string &requirement);
// Why a requirement can't be installed, or "": empty, options ("-e",
// "--index-url"), paths and URLs, and packages that would bring a second
// USD (usd-core).
std::string CheckRequirement(const std::string &requirement);

struct PipCommand {
    std::vector<std::string> args;         // the bundled Python, -m pip install ...
    std::vector<std::string> environment;  // NAME=value
};
// The install: python is the bundled interpreter, bundled its
// site-packages (constraints, and visible to pip so it isn't reinstalled).
// extra adds pip options (VELA_PIP_ARGS, for tests: a local index).
PipCommand InstallCommand(const std::string &python, const std::string &base,
                          const std::string &bundled, const std::string &constraintsFile,
                          const std::vector<std::string> &requirements,
                          const std::vector<std::string> &extra);

// Splits VELA_PIP_ARGS-style text at spaces, as a shell would: quotes
// (single or double) keep spaces in an argument.
std::vector<std::string> SplitArguments(const std::string &text);

} // namespace packages
} // namespace vela
