// The embedded interpreter: the bundle's Python 3.11 in this process, so
// scripts share libusd_ms (and the open stages) with the app. The vela
// module (vela_module.py) exposes the session's documents and the action
// registry. Scripts run on the calling thread, which for now is always
// the main thread, holding the GIL.
#pragma once

#include <string>
#include <vector>

namespace vela {

// Starts Python with home as its prefix (stdlib in home/lib/python3.11),
// isolated from the user's environment, with paths ahead of the rest of
// sys.path (the pxr package's directory). Returns false with a message.
// userSite, the per-user packages folder (see packages.h), goes last, with
// site.addsitedir so its .pth files apply.
bool InitializePython(const std::string &home, const std::vector<std::string> &paths,
                      std::string *error, const std::string &userSite = std::string());
// The interpreter's version, e.g. "3.11".
std::string PythonVersion();
bool IsPythonInitialized();

struct PythonResult {
    bool ok = false;
    std::string output;  // what the code printed
    std::string value;   // the last expression's repr, if it had a value
    std::string error;   // the traceback, when !ok
};

// Runs code as an interactive interpreter would, in a namespace kept per
// key (one per console, one for MCP), with the vela module and the common
// pxr modules imported. documentId (0 for the current one) becomes the
// current document while it runs, and its edits become one undo step.
PythonResult RunPython(const std::string &code, const std::string &key, int documentId = 0);

// Calls vela.<function>(*args) with args a JSON array, returning its result
// as JSON; false with the traceback in error if it raised. For the app's
// own calls into Python, such as loading plugins.
bool CallPython(const std::string &function, const std::string &argsJson,
                std::string *result, std::string *error);

// Whether a console should run its input on Return rather than start a
// new line: the input is a complete statement, or not valid Python.
bool IsCompletePython(const std::string &code);

} // namespace vela
