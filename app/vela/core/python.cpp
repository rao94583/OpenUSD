#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "python.h"
#include "actions.h"
#include "document.h"
#include "session.h"

#include "pxr/base/js/json.h"

#include <optional>

namespace vela {

namespace {

const char *const ModuleSource =
#include "app/vela/core/vela_module.inc"
    ;

// Installs the vela module, which is imported on first use (importing pxr
// takes a moment, so not at launch).
const char *const Bootstrap = R"(
import importlib.abc, importlib.util, sys, _vela

class _VelaImporter(importlib.abc.MetaPathFinder, importlib.abc.Loader):
    def find_spec(self, name, path, target=None):
        if name == 'vela':
            return importlib.util.spec_from_loader(name, self)
        return None
    def create_module(self, spec):
        return None
    def exec_module(self, module):
        exec(compile(_vela.source(), '<vela>', 'exec'), module.__dict__)

sys.meta_path.append(_VelaImporter())
)";

bool initialized = false;

PyObject *String(const std::string &text) {
    return PyUnicode_FromStringAndSize(text.data(), Py_ssize_t(text.size()));
}

const SessionDocument *FindDocument(long id) {
    const SessionDocument *entry = Session::Get().Find(int(id));
    if (!entry) {
        PyErr_Format(PyExc_LookupError, "Document %ld is closed", id);
    }
    return entry;
}

// The _vela module: what vela_module.py builds on. JSON travels as text.

PyObject *Source(PyObject *, PyObject *) { return PyUnicode_FromString(ModuleSource); }

PyObject *Documents(PyObject *, PyObject *) {
    return String(JsWriteToString(JsValue(DocumentList())));
}

PyObject *Current(PyObject *, PyObject *) {
    const SessionDocument *current = Session::Get().GetCurrent();
    return PyLong_FromLong(current ? current->id : 0);
}

PyObject *StageCacheId(PyObject *, PyObject *args) {
    long id;
    if (!PyArg_ParseTuple(args, "l", &id)) return nullptr;
    const SessionDocument *entry = FindDocument(id);
    return entry ? PyLong_FromLongLong(entry->stageCacheId) : nullptr;
}

PyObject *State(PyObject *, PyObject *args) {
    long id;
    if (!PyArg_ParseTuple(args, "l", &id)) return nullptr;
    const SessionDocument *entry = FindDocument(id);
    return entry ? String(JsWriteToString(JsValue(DocumentState(*entry)))) : nullptr;
}

PyObject *Run(PyObject *, PyObject *args) {
    long id;
    const char *name, *paramsJson;
    if (!PyArg_ParseTuple(args, "lss", &id, &name, &paramsJson)) return nullptr;
    const SessionDocument *entry = FindDocument(id);
    if (!entry) return nullptr;
    const JsValue params = JsParseString(paramsJson);
    if (!params.IsObject()) {
        PyErr_SetString(PyExc_ValueError, "Action parameters must be a JSON object");
        return nullptr;
    }
    // Keep the document alive if the action closes its window.
    const std::shared_ptr<Document> document = entry->document;
    const ActionResult result =
        ActionRegistry::Get().Run(*document, name, params.GetJsObject());
    JsObject reply{{"ok", JsValue(result.ok)}, {"value", result.value}};
    if (!result.ok) reply["error"] = JsValue(result.error);
    return String(JsWriteToString(JsValue(reply)));
}

PyObject *Describe(PyObject *, PyObject *) {
    return String(JsWriteToString(ActionRegistry::Get().Describe()));
}

PyObject *Open(PyObject *, PyObject *args) {
    const char *path;
    if (!PyArg_ParseTuple(args, "s", &path)) return nullptr;
    std::string error = "Vela cannot open files here";
    const int id = Session::Get().open ? Session::Get().open(path, &error) : 0;
    if (!id) {
        PyErr_SetString(PyExc_OSError, error.c_str());
        return nullptr;
    }
    return PyLong_FromLong(id);
}

PyMethodDef Methods[] = {
    {"source", Source, METH_NOARGS, nullptr},
    {"documents", Documents, METH_NOARGS, nullptr},
    {"current", Current, METH_NOARGS, nullptr},
    {"stage_cache_id", StageCacheId, METH_VARARGS, nullptr},
    {"state", State, METH_VARARGS, nullptr},
    {"run", Run, METH_VARARGS, nullptr},
    {"describe", Describe, METH_NOARGS, nullptr},
    {"open", Open, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef ModuleDef = {PyModuleDef_HEAD_INIT, "_vela", nullptr, -1, Methods};

PyObject *InitModule() { return PyModule_Create(&ModuleDef); }

// The current exception's traceback, as text, clearing it.
std::string TakeError() {
    PyObject *type, *value, *traceback;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);
    std::string text = "Python error";
    if (PyObject *module = PyImport_ImportModule("traceback")) {
        if (PyObject *lines = PyObject_CallMethod(module, "format_exception", "OOO",
                                                  type ? type : Py_None,
                                                  value ? value : Py_None,
                                                  traceback ? traceback : Py_None)) {
            PyObject *empty = PyUnicode_FromString("");
            if (PyObject *joined = PyUnicode_Join(empty, lines)) {
                text = PyUnicode_AsUTF8(joined);
                Py_DECREF(joined);
            }
            Py_DECREF(empty);
            Py_DECREF(lines);
        }
        Py_DECREF(module);
    }
    PyErr_Clear();
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(traceback);
    return text;
}

// Calls vela.<function>(args...), importing vela first.
PyObject *CallVela(const char *function, PyObject *args) {
    PyObject *module = PyImport_ImportModule("vela");
    if (!module) return nullptr;
    PyObject *callable = PyObject_GetAttrString(module, function);
    Py_DECREF(module);
    if (!callable) return nullptr;
    PyObject *result = PyObject_CallObject(callable, args);
    Py_DECREF(callable);
    return result;
}

std::string Text(PyObject *object) {
    if (!object || object == Py_None) return std::string();
    const char *text = PyUnicode_AsUTF8(object);
    return text ? text : std::string();
}

} // namespace

bool InitializePython(const std::string &home, const std::vector<std::string> &paths,
                      std::string *error) {
    if (initialized) return true;
    PyImport_AppendInittab("_vela", &InitModule);
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    // The app keeps its own signal handling (no KeyboardInterrupt on ^C).
    config.install_signal_handlers = 0;
    auto check = [&](PyStatus status) {
        if (!PyStatus_Exception(status)) return true;
        if (error) *error = status.err_msg ? status.err_msg : "Python failed to start";
        PyConfig_Clear(&config);
        return false;
    };
    if (!check(PyConfig_SetBytesString(&config, &config.home, home.c_str())) ||
        !check(PyConfig_SetBytesString(&config, &config.executable,
                                       (home + "/bin/python3").c_str())) ||
        !check(Py_InitializeFromConfig(&config))) {
        return false;
    }
    PyConfig_Clear(&config);

    PyObject *sysPath = PySys_GetObject("path");  // borrowed
    for (size_t i = 0; i < paths.size(); ++i) {
        PyObject *path = String(paths[i]);
        PyList_Insert(sysPath, Py_ssize_t(i), path);
        Py_DECREF(path);
    }
    if (PyRun_SimpleString(Bootstrap) != 0) {
        if (error) *error = "Could not install the vela module";
        return false;
    }
    initialized = true;
    // Other threads may take the GIL from now on.
    PyEval_SaveThread();
    return true;
}

bool IsPythonInitialized() { return initialized; }

PythonResult RunPython(const std::string &code, const std::string &key, int documentId) {
    PythonResult result;
    if (!initialized) {
        result.error = "Python is not available";
        return result;
    }
    const SessionDocument *entry =
        documentId ? Session::Get().Find(documentId) : Session::Get().GetCurrent();
    if (documentId && !entry) {
        result.error = "Document " + std::to_string(documentId) + " is closed";
        return result;
    }
    // The document stays current, and its edits are grouped, until the
    // script ends, even if it closes the window.
    const std::shared_ptr<Document> document = entry ? entry->document : nullptr;
    std::optional<Session::ScopedCurrent> scoped;
    std::optional<UndoBlock> undo;
    if (entry) {
        scoped.emplace(entry->id);
        undo.emplace(document->GetUndoStack(), "Python");
    }

    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject *args = Py_BuildValue("(s#s#)", code.data(), Py_ssize_t(code.size()),
                                   key.data(), Py_ssize_t(key.size()));
    PyObject *reply = args ? CallVela("_execute", args) : nullptr;
    Py_XDECREF(args);
    if (reply && PyTuple_Check(reply) && PyTuple_Size(reply) == 3) {
        result.ok = PyObject_IsTrue(PyTuple_GetItem(reply, 0));
        result.output = Text(PyTuple_GetItem(reply, 1));
        (result.ok ? result.value : result.error) = Text(PyTuple_GetItem(reply, 2));
    } else {
        result.error = TakeError();
    }
    Py_XDECREF(reply);
    PyGILState_Release(gil);
    return result;
}

bool CallPython(const std::string &function, const std::string &argsJson,
                std::string *result, std::string *error) {
    if (!initialized) {
        if (error) *error = "Python is not available";
        return false;
    }
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject *args = Py_BuildValue("(s#s#)", function.data(), Py_ssize_t(function.size()),
                                   argsJson.data(), Py_ssize_t(argsJson.size()));
    PyObject *reply = args ? CallVela("_call", args) : nullptr;
    Py_XDECREF(args);
    const bool ok = reply && PyUnicode_Check(reply);
    if (ok) {
        if (result) *result = Text(reply);
    } else if (error) {
        *error = reply ? "vela._call returned no text" : TakeError();
    } else {
        PyErr_Clear();
    }
    Py_XDECREF(reply);
    PyGILState_Release(gil);
    return ok;
}

bool IsCompletePython(const std::string &code) {
    if (!initialized) return true;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject *args = Py_BuildValue("(s#)", code.data(), Py_ssize_t(code.size()));
    PyObject *reply = args ? CallVela("_is_complete", args) : nullptr;
    Py_XDECREF(args);
    const bool complete = !reply || PyObject_IsTrue(reply);
    if (!reply) PyErr_Clear();
    Py_XDECREF(reply);
    PyGILState_Release(gil);
    return complete;
}

} // namespace vela
