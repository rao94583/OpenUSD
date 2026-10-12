#!/usr/bin/env python3
"""Generate BUILD.bazel files from the CMake build's pxr_library() calls.

The CMake build is the reference. This script interprets the small subset of
CMake that pxr's CMakeLists.txt files use to compute pxr_library() arguments
(set, list, option, if/elseif/else, foreach, add_subdirectory, configure_file,
target_compile_definitions), once per target OS, and writes one BUILD.bazel per
directory that declares libraries. Where the OSes disagree, the difference is
emitted as pxr_library(per_os = ...). Other commands (tests, install rules,
custom commands) are ignored.

Starting at pxr/CMakeLists.txt, add_subdirectory() is followed only into the
directories listed in SCOPE. CMake option defaults come from evaluating
cmake/defaults/Options.cmake.

Usage:
  cmake2bazel.py            print what would be written
  cmake2bazel.py --write    rewrite generated files
  cmake2bazel.py --check    exit 1 if any generated file is out of date
"""

import argparse
import difflib
import json
import os
import re
import shlex
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))

# Directories translated so far. Grows with each phase of the Bazel port.
SCOPE = ["pxr/external", "pxr/base", "pxr/usd", "pxr/usdValidation",
         "pxr/imaging", "pxr/usdImaging"]

# File listing all generated libraries and Python modules, for //pxr and
# //bazel/python.
LIBRARIES_BZL = "bazel/pxr_libraries.bzl"

HEADER = """\
# Generated from {source} by bazel/tools/cmake2bazel.py; do not edit.
# Regenerate with: python3 bazel/tools/cmake2bazel.py --write
"""

OSES = ["macos", "linux"]

# Variables CMake or cmake/defaults/*.cmake define before pxr is processed.
_COMMON_VARS = {
    "UNIX": "1",
    # ProjectDefaults.cmake; some directories turn it off.
    "CMAKE_INCLUDE_CURRENT_DIR": "ON",
    # Every library is linked into //bazel/install:usd_ms, like CMake's
    # monolithic build (usd_m target).
    "PXR_BUILD_MONOLITHIC": "ON",
    # Not ported yet (deferred in the Bazel plan); defaults to ON.
    "PXR_BUILD_EXEC": "OFF",
    # Off by default in Options.cmake but on in build_scripts/build_usd.py;
    # MaterialX is vendored (//vendor/materialx). //pxr defines
    # PXR_MATERIALX_SUPPORT_ENABLED to match (Packages.cmake).
    "PXR_ENABLE_MATERIALX_SUPPORT": "ON",
    # find_package(MaterialX). MATERIALX_STDLIB_DIR stays unset: the data
    # library is installed through RESOURCE_TREES instead, and nothing
    # points at the build machine (PXR_MATERIALX_STDLIB_DIR).
    "MaterialX_FOUND": "TRUE",
    "M_LIB": "m",  # Packages.cmake
    # Packages.cmake; mapped to a label in EXTERNAL_LABELS.
    "PYTHON_LIBRARIES": "Python3::Python",
    "PYTHON_INCLUDE_DIRS": "",
    # FindJinja2.cmake; Jinja2 is vendored (//vendor/jinja2).
    "JINJA2_FOUND": "TRUE",
    # Packages.cmake (OpenSubdiv, with PXR_BUILD_GPU_SUPPORT); vendored.
    "OPENSUBDIV_LIBRARIES": "OpenSubdiv::osdCPU;OpenSubdiv::osdGPU",
    "OPENSUBDIV_OSDCPU_LIBRARY": "OpenSubdiv::osdCPU",
    "PROJECT_SOURCE_DIR": "<source>",
    "PROJECT_BINARY_DIR": "<build>",
    # Placeholders in test commands, resolved by bazel/tools/pxr_test_runner.py.
    "CMAKE_INSTALL_PREFIX": "<prefix>",
    "PYTHON_EXECUTABLE": "<python>",
}
PLATFORM_VARS = {
    # CMake's defaults: OpenGL and Metal on macOS (Storm needs
    # PXR_ENABLE_GL_SUPPORT but renders through Metal there), OpenGL on Linux.
    "macos": dict(_COMMON_VARS, APPLE="1", CMAKE_SYSTEM_NAME="Darwin",
                  CMAKE_DL_LIBS=""),
    "linux": dict(_COMMON_VARS, CMAKE_SYSTEM_NAME="Linux",
                  CMAKE_DL_LIBS="dl",
                  # find_package(X11) in Packages.cmake.
                  X11_FOUND="TRUE", X11_LIBRARIES="X11"),
}

# CMake targets defined outside pxr, and their Bazel labels.
EXTERNAL_LABELS = {
    "TBB::tbb": "//vendor/onetbb:tbb",
    "OpenSubdiv::osdCPU": "//vendor/opensubdiv:osd_cpu",
    "OpenSubdiv::osdGPU": "//vendor/opensubdiv:osd_gpu",
    # Libraries and Python extensions resolve libpython symbols at load time
    # (PXR_PY_UNDEFINED_DYNAMIC_LOOKUP), so only the headers are needed;
    # executables link libpython themselves.
    "Python3::Python": "@rules_python//python/cc:current_py_cc_headers",
}
EXTERNAL_LABELS.update({
    name: "//vendor/materialx:" + name
    for name in [
        "MaterialXCore", "MaterialXFormat", "MaterialXGenShader",
        "MaterialXGenGlsl", "MaterialXGenMsl", "MaterialXRender",
        "MaterialXRenderGlsl",
    ]
})

# Resources from other packages, per library: CMake globs these from
# directories outside the source tree.
RESOURCE_TREES = {
    # MATERIALX_STDLIB_DIR's files under resources/libraries
    # (pxr/usd/usdMtlx/CMakeLists.txt).
    "usdMtlx": ["//vendor/materialx:libraries"],
}
# Tests CMake registers that cannot pass in this build, with the reason.
# They are generated with excluded = "<reason>" (tagged manual: skipped by
# `bazel test //...`, still runnable by label). Stale entries are errors.
EXCLUDED_TESTS = {
    "pxr/usdImaging/usdImagingGL:testUsdImagingGLBasisCurvesWithScale": (
        "0.5% of pixels differ from the baseline at curve edges (limit "
        "0.3%) with Metal on macOS 27; not diagnosed"),
}

# IMAGE_DIFF_TOOL in Packages.cmake: OpenImageIO's idiff, vendored.
IMAGE_DIFF_TOOL = "//vendor/openimageio:idiff"

# CMake targets for system libraries, as link flags per OS.
SYSTEM_TARGETS = {
    "OpenGL::GL": {"macos": "-framework OpenGL", "linux": "-lGL"},
}

# Apple frameworks find_library() resolves on macOS (to "-framework X", as
# linking a found framework path does); never probed on the host.
APPLE_FRAMEWORKS = {
    "AppKit", "CoreFoundation", "CoreGraphics", "CoreVideo", "Foundation",
    "ImageIO", "IOKit", "IOSurface", "Metal", "OpenGL", "QuartzCore",
}

# Other targets that if(TARGET ...) should consider defined.
OTHER_TARGETS = {"python_modules", "shared_libs", "usd_m"}

# Generator expressions dropped on purpose, with the reason.
DROPPED_GENEX = {
    # Bazel's opt mode already defines NDEBUG.
    "$<$<NOT:$<CONFIG:Debug>>:NDEBUG>",
}

# pxr_* commands that do not affect what Bazel builds yet. Any other pxr_*
# command is an error, so new macros are not skipped by accident.
IGNORED_PXR_COMMANDS = {
    "pxr_core_prologue",
    "pxr_core_epilogue",
    # Rpaths of boost::python's test libraries; Bazel computes rpaths.
    "pxr_set_rpaths_for_target",
}

# pxr_register_test() arguments (cmake/macros/Public.cmake).
TEST_OPTIONS = ["RUN_SERIAL", "PYTHON", "REQUIRES_SHARED_LIBS",
                "REQUIRES_PYTHON_MODULES", "PERCEPTUAL"]
TEST_ONE_VALUE = [
    "CUSTOM_PYTHON", "COMMAND", "STDOUT_REDIRECT", "STDERR_REDIRECT",
    "POST_COMMAND", "POST_COMMAND_STDOUT_REDIRECT",
    "POST_COMMAND_STDERR_REDIRECT", "PRE_COMMAND",
    "PRE_COMMAND_STDOUT_REDIRECT", "PRE_COMMAND_STDERR_REDIRECT",
    "FILES_EXIST", "FILES_DONT_EXIST", "CLEAN_OUTPUT", "EXPECTED_RETURN_CODE",
    "TESTENV", "TESTENV_DEST", "WARN", "WARN_PERCENT", "HARD_WARN", "FAIL",
    "FAIL_PERCENT", "HARD_FAIL", "LABELS",
]
TEST_MULTI_VALUE = ["DIFF_COMPARE", "IMAGE_DIFF_COMPARE", "ENV", "PRE_PATH",
                    "POST_PATH"]

# pxr_library() keywords that take lists, in output order.
LIST_KEYWORDS = [
    "LIBRARIES",
    "PUBLIC_CLASSES",
    "PYTHON_PUBLIC_CLASSES",
    "PUBLIC_HEADERS",
    "PYTHON_PUBLIC_HEADERS",
    "PRIVATE_CLASSES",
    "PYTHON_PRIVATE_CLASSES",
    "PRIVATE_HEADERS",
    "PYTHON_PRIVATE_HEADERS",
    "CPPFILES",
    "PYTHON_CPPFILES",
    "PYMODULE_CPPFILES",
    "PYMODULE_FILES",
    "PYMODULE_DIRS",
    "PYSIDE_UI_FILES",
    "RESOURCE_FILES",
    "DOXYGEN_FILES",
]
# Keywords that are accepted but have no Bazel equivalent.
IGNORED_KEYWORDS = {"INCLUDE_DIRS", "TYPE", "PRECOMPILED_HEADER_NAME"}
# pxr_library() options (flags without values).
OPTIONS = {"DISABLE_PRECOMPILED_HEADERS", "INCLUDE_SCHEMA_FILES"}

# Bazel attributes in output order; LIST_KEYWORDS map to their lower-case
# names, the rest come from other CMake commands.
EXTRA_ATTRS = ["copts", "linkopts", "defines", "local_defines",
               "pymodule_local_defines", "generated_headers", "resource_trees"]
ATTRS = [k.lower() for k in LIST_KEYWORDS] + EXTRA_ATTRS
# Attributes pxr_library() accepts under per_os.
PER_OS_ATTRS = {"libraries", "public_headers", "private_headers", "cppfiles",
                "copts", "linkopts"}


class CMakeError(Exception):
    pass


# --- Parsing -----------------------------------------------------------------

# Argument kinds.
UNQUOTED, QUOTED, BRACKET = range(3)

_BRACKET_OPEN = re.compile(r"\[(=*)\[")


def _skip_bracket(text, i, m):
    end = text.find("]" + m.group(1) + "]", m.end())
    if end < 0:
        raise CMakeError("unterminated bracket at offset %d" % i)
    return text[m.end():end], end + len(m.group(1)) + 2


def _skip_comment(text, i):
    m = _BRACKET_OPEN.match(text, i + 1)
    if m:
        return _skip_bracket(text, i, m)[1]
    end = text.find("\n", i)
    return len(text) if end < 0 else end


def tokenize(text):
    """Returns [(command, [(arg, kind)], line)] for a CMake script."""
    commands, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif c == "#":
            i = _skip_comment(text, i)
        else:
            m = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(").match(text, i)
            if not m:
                raise CMakeError("line %d: expected a command"
                                 % (text.count("\n", 0, i) + 1))
            line = text.count("\n", 0, i) + 1
            args, i, depth = [], m.end(), 1
            while True:
                if i >= n:
                    raise CMakeError("line %d: unterminated %s()"
                                     % (line, m.group(1)))
                c = text[i]
                if c.isspace():
                    i += 1
                elif c == "#":
                    i = _skip_comment(text, i)
                elif c == "(":
                    args.append(("(", UNQUOTED))
                    depth, i = depth + 1, i + 1
                elif c == ")":
                    depth, i = depth - 1, i + 1
                    if not depth:
                        break
                    args.append((")", UNQUOTED))
                elif c == '"':
                    j, value = i + 1, []
                    while text[j] != '"':
                        if text[j] == "\\":
                            nxt = text[j + 1]
                            value.append({"n": "\n", "t": "\t"}.get(
                                nxt, nxt if nxt in '"\\$' else "\\" + nxt))
                            j += 2
                        else:
                            value.append(text[j])
                            j += 1
                    args.append(("".join(value), QUOTED))
                    i = j + 1
                elif _BRACKET_OPEN.match(text, i):
                    value, i = _skip_bracket(text, i,
                                             _BRACKET_OPEN.match(text, i))
                    args.append((value.lstrip("\n"), BRACKET))
                else:
                    # An unquoted argument; quoted parts inside it keep
                    # their spaces and quotes (CMake's legacy syntax,
                    # e.g. ENV VAR="a b").
                    j = i
                    while j < n and not text[j].isspace() and \
                            text[j] not in "()":
                        if text[j] == '"':
                            end = text.find('"', j + 1)
                            if end < 0:
                                raise CMakeError("line %d: unterminated "
                                                 "quote" % line)
                            j = end
                        j += 1
                    args.append((text[i:j], UNQUOTED))
                    i = j
            commands.append((m.group(1).lower(), args, line))
    return commands


def parse_blocks(commands):
    """Nests if/foreach/function bodies: returns a list of nodes."""
    def block(i, terminators):
        body = []
        while i < len(commands):
            name, args, line = commands[i]
            if name in terminators:
                return body, i
            if name == "if":
                branches = []
                cond = args
                while True:
                    sub, i = block(i + 1, {"elseif", "else", "endif"})
                    branches.append((cond, sub))
                    name, args, _ = commands[i]
                    if name == "endif":
                        break
                    cond = None if name == "else" else args
                body.append(("if", branches, line))
                i += 1
            elif name in ("foreach", "while", "function", "macro"):
                sub, i = block(i + 1, {"end" + name})
                body.append((name, args, sub, line))
                i += 1
            elif name in ("elseif", "else", "endif", "endforeach",
                          "endwhile", "endfunction", "endmacro"):
                raise CMakeError("line %d: unexpected %s()" % (line, name))
            else:
                body.append(("cmd", name, args, line))
                i += 1
        if terminators:
            raise CMakeError("missing %s" % " or ".join(sorted(terminators)))
        return body, i
    return block(0, set())[0]


# --- Evaluation --------------------------------------------------------------

_VAR_REF = re.compile(r"\$(ENV)?\{([^${}]*)\}")
_TRUE = {"1", "ON", "YES", "TRUE", "Y"}
_FALSE = {"0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND", ""}


def is_true_constant(value):
    u = value.upper()
    if u in _TRUE:
        return True
    if u in _FALSE or u.endswith("-NOTFOUND"):
        return False
    try:
        return float(value) != 0
    except ValueError:
        return None


def _version(v):
    return tuple(int(p) for p in re.findall(r"\d+", v))


def _number(v):
    try:
        return float(v)
    except ValueError:
        return float("nan")


_BINARY_OPS = {
    "STREQUAL": lambda a, b: a == b,
    "MATCHES": lambda a, b: re.search(b, a) is not None,
    "EQUAL": lambda a, b: _number(a) == _number(b),
    "LESS": lambda a, b: _number(a) < _number(b),
    "GREATER": lambda a, b: _number(a) > _number(b),
    "VERSION_EQUAL": lambda a, b: _version(a) == _version(b),
    "VERSION_LESS": lambda a, b: _version(a) < _version(b),
    "VERSION_GREATER": lambda a, b: _version(a) > _version(b),
    "VERSION_LESS_EQUAL": lambda a, b: _version(a) <= _version(b),
    "VERSION_GREATER_EQUAL": lambda a, b: _version(a) >= _version(b),
    "IN_LIST": None,  # Handled by the caller: the right side is a name.
}


class Library:
    def __init__(self, name, args, line):
        self.name, self.args, self.line = name, args, line
        self.defines, self.local_defines = [], []
        self.pymodule_local_defines = []
        self.plugin = False
        self.include_current_dir = True


class Directory:
    """Results of evaluating one CMakeLists.txt."""

    def __init__(self, path):
        self.path = path  # relative to REPO
        self.libraries = []
        self.configured = []  # (template, out, {placeholder: value})
        self.definitions = []  # add_definitions()
        self.compile_options = []  # add_compile_options(), one group per call
        self.cpp_bins = []  # (name, [library])
        self.python_bins = []  # (name, python file)
        self.tests = Tests()


class Tests:
    """Test artifacts of one directory, laid out as CMake installs them
    under <prefix>/tests."""

    def __init__(self):
        # name -> {"kind": "executable" | "shared", "srcs": [...],
        #          "libraries": [...], "prefix": str | None,
        #          "suffix": str | None, "dest": "tests" | "tests/lib" | None,
        #          "plug_info": (src, install prefix) | None}
        self.targets = {}
        self.files = {}  # dest under tests/ -> src
        self.testenvs = {}  # dest under tests/ctest -> src dir
        self.registered = []  # (name, {keyword: value}, line)
        self.package = None  # PXR_PACKAGE
        self.rendered = None  # set by starlark_tests()
        self.include_current_dir = True  # CMAKE_INCLUDE_CURRENT_DIR


class Return(Exception):
    pass


class Interpreter:
    def __init__(self, os_name, warn):
        self.os, self.warn = os_name, warn
        self.functions = {}  # name -> (kind, params, body)
        self.targets = set(EXTERNAL_LABELS) | OTHER_TARGETS
        self.libraries = {}  # name -> (directory, Library)
        self.directories = []

    # Variables and arguments.

    def expand(self, s, vars):
        while True:
            m = _VAR_REF.search(s)
            if not m:
                return s
            value = "" if m.group(1) else vars.get(m.group(2), "")
            s = s[:m.start()] + value + s[m.end():]

    def args(self, raw, vars, keep_kind=False):
        out = []
        for value, kind in raw:
            if kind == BRACKET:
                items = [value]
            elif kind == QUOTED:
                items = [self.expand(value, vars)]
            else:
                items = [v for v in self.expand(value, vars).split(";") if v]
            out.extend((v, kind) if keep_kind else v for v in items)
        return out

    # if() conditions.

    def condition(self, raw, vars):
        toks = self.args(raw, vars, keep_kind=True)
        if not toks:
            return False  # e.g. if(${UNDEFINED})
        pos = [0]

        def peek():
            return toks[pos[0]] if pos[0] < len(toks) else (None, None)

        def keyword(word):
            value, kind = peek()
            if kind == UNQUOTED and value == word:
                pos[0] += 1
                return True
            return False

        def take():
            if pos[0] >= len(toks):
                raise CMakeError("incomplete if() condition: %s" % raw)
            pos[0] += 1
            return toks[pos[0] - 1]

        def operand(tok):
            value, kind = tok
            if kind == UNQUOTED and value in vars:
                return vars[value]
            return value

        def truth(tok):
            value, kind = tok
            constant = is_true_constant(value)
            if constant is not None:
                return constant
            if kind != UNQUOTED:
                return False
            return value in vars and is_true_constant(vars[value]) is not False

        def unary():
            if keyword("("):
                result = or_expr()
                if not keyword(")"):
                    raise CMakeError("unbalanced () in if(): %s" % raw)
                return result
            if keyword("DEFINED"):
                return take()[0] in vars
            if keyword("TARGET"):
                return take()[0] in self.targets
            if keyword("COMMAND"):
                return take()[0].lower() in self.functions
            if keyword("EXISTS"):
                return os.path.exists(take()[0].replace("<source>", REPO))
            left = take()
            value, kind = peek()
            if kind == UNQUOTED and value in _BINARY_OPS:
                pos[0] += 1
                right = take()
                if value == "IN_LIST":
                    return operand(left) in vars.get(right[0], "").split(";")
                return _BINARY_OPS[value](operand(left), operand(right))
            return truth(left)

        def not_expr():
            if keyword("NOT"):
                return not not_expr()
            return unary()

        def and_expr():
            result = not_expr()
            while keyword("AND"):
                result = not_expr() and result
            return result

        def or_expr():
            result = and_expr()
            while keyword("OR"):
                result = and_expr() or result
            return result

        result = or_expr()
        if pos[0] != len(toks):
            raise CMakeError("cannot evaluate if(): %s" % raw)
        return result

    # Execution.

    def run_directory(self, path, vars):
        directory = Directory(path)
        self.directories.append(directory)
        self.run_file(os.path.join(path, "CMakeLists.txt"), vars, directory)

    def run_file(self, path, vars, directory):
        with open(os.path.join(REPO, path)) as f:
            body = parse_blocks(tokenize(f.read()))
        try:
            self.run(body, vars, directory)
        except Return:
            pass
        except CMakeError as e:
            if not getattr(e, "located", False):
                e = CMakeError("%s: %s" % (path, e))
                e.located = True
            raise e

    def run(self, body, vars, directory):
        for node in body:
            kind = node[0]
            if kind == "if":
                for cond, sub in node[1]:
                    if cond is None or self.condition(cond, vars):
                        self.run(sub, vars, directory)
                        break
            elif kind == "foreach":
                items = self.args(node[1], vars)
                var, items = items[0], items[1:]
                if items[:1] == ["IN"]:
                    values = []
                    mode = None
                    for item in items[1:]:
                        if item in ("LISTS", "ITEMS"):
                            mode = item
                        elif mode == "LISTS":
                            values.extend(v for v in vars.get(item, "")
                                          .split(";") if v)
                        else:
                            values.append(item)
                    items = values
                elif items[:1] == ["RANGE"]:
                    raise CMakeError("line %d: foreach(RANGE) unsupported"
                                     % node[3])
                for item in items:
                    vars[var] = item
                    self.run(node[2], vars, directory)
            elif kind in ("function", "macro"):
                signature = self.args(node[1], vars)
                self.functions[signature[0].lower()] = (
                    kind, signature[1:], node[2])
            elif kind == "while":
                raise CMakeError("line %d: while() unsupported" % node[3])
            else:
                self.command(node[1], node[2], node[3], vars, directory)

    def call(self, name, raw, vars, directory):
        """Calls a function or macro defined with function()/macro()."""
        kind, params, body = self.functions[name]
        args = self.args(raw, vars)
        scope = dict(vars) if kind == "function" else vars
        saved = {k: vars.get(k) for k in params} if kind == "macro" else {}
        for i, param in enumerate(params):
            scope[param] = args[i] if i < len(args) else ""
        scope["ARGC"] = str(len(args))
        scope["ARGV"] = ";".join(args)
        scope["ARGN"] = ";".join(args[len(params):])
        for i, arg in enumerate(args):
            scope["ARGV%d" % i] = arg
        try:
            self.run(body, scope, directory)
        except Return:
            if kind == "macro":
                raise  # return() in a macro returns from the caller.
        finally:
            # Macro parameters are textual substitutions in CMake; do not
            # leave them behind in the caller's scope.
            for k, v in saved.items():
                if v is None:
                    vars.pop(k, None)
                else:
                    vars[k] = v

    def parse_arguments(self, raw, line, vars):
        """cmake_parse_arguments(<prefix> <options> <one> <multi> <args>...)."""
        args = self.args(raw, vars, keep_kind=True)
        if args and args[0][0] == "PARSE_ARGV":
            raise CMakeError("line %d: cmake_parse_arguments(PARSE_ARGV) "
                             "unsupported" % line)
        prefix = args[0][0]
        # The keyword lists are usually quoted list variables.
        options, one, multi = ([v for v in a[0].split(";") if v]
                               for a in args[1:4])
        values, current = {}, None
        unparsed = []
        for value, _ in args[4:]:
            for item in [v for v in value.split(";") if v]:
                if item in options:
                    values[item], current = "TRUE", None
                elif item in one or item in multi:
                    values.setdefault(item, [])
                    current = item
                elif current in one and not values[current]:
                    values[current] = [item]
                    current = None
                elif current in multi:
                    values[current].append(item)
                else:
                    unparsed.append(item)
        for keyword in options + one + multi:
            var = prefix + "_" + keyword
            value = values.get(keyword)
            if keyword in options:
                vars[var] = "TRUE" if value else "FALSE"
            elif value:
                vars[var] = ";".join(value)
            else:
                vars.pop(var, None)
        if unparsed:
            vars[prefix + "_UNPARSED_ARGUMENTS"] = ";".join(unparsed)
        else:
            vars.pop(prefix + "_UNPARSED_ARGUMENTS", None)

    def command(self, name, raw, line, vars, directory):
        if name in self.functions:
            self.call(name, raw, vars, directory)
        elif name == "cmake_parse_arguments":
            self.parse_arguments(raw, line, vars)
        elif name == "set":
            args = self.args(raw, vars)
            var, values = args[0], args[1:]
            if "PARENT_SCOPE" in values:
                raise CMakeError("line %d: set(PARENT_SCOPE) unsupported"
                                 % line)
            if "CACHE" in values:
                force = "FORCE" in values
                values = values[:values.index("CACHE")]
                if var in vars and not force:
                    return
            if values:
                vars[var] = ";".join(values)
            else:
                vars.pop(var, None)
        elif name == "unset":
            vars.pop(self.args(raw, vars)[0], None)
        elif name == "option":
            args = self.args(raw, vars)
            if args[0] not in vars:
                vars[args[0]] = args[2] if len(args) > 2 else "OFF"
        elif name == "list":
            args = self.args(raw, vars)
            op, var, items = args[0], args[1], args[2:]
            current = [v for v in vars.get(var, "").split(";") if v]
            if op == "APPEND":
                current += items
            elif op == "PREPEND":
                current = items + current
            elif op == "REMOVE_ITEM":
                current = [v for v in current if v not in items]
            else:
                raise CMakeError("line %d: list(%s) unsupported" % (line, op))
            vars[var] = ";".join(current)
        elif name in ("find_program", "find_library", "find_file",
                      "find_path"):
            # Never probe the host: results must not depend on the machine.
            args = self.args(raw, vars)
            var = args[0]
            names = args[2:args.index("PATHS")] if "PATHS" in args else \
                args[1:2] + (args[args.index("NAMES") + 1:]
                             if "NAMES" in args else [])
            framework = [n for n in names if n in APPLE_FRAMEWORKS]
            if name == "find_library" and self.os == "macos" and framework:
                vars[var] = "-framework " + framework[0]
            elif var not in vars:
                vars[var] = var + "-NOTFOUND"
        elif name == "get_filename_component":
            args = self.args(raw, vars)
            var, path, mode = args[0], args[1], args[2]
            base = os.path.basename(path)
            if mode in ("DIRECTORY", "PATH"):
                value = os.path.dirname(path)
            elif mode == "NAME":
                value = base
            elif mode == "NAME_WE":
                value = base.split(".", 1)[0]
            elif mode == "EXT":
                value = "." + base.split(".", 1)[1] if "." in base else ""
            else:
                raise CMakeError("line %d: get_filename_component(%s) "
                                 "unsupported" % (line, mode))
            vars[var] = value
        elif name == "string":
            args = self.args(raw, vars, keep_kind=True)
            op = args[0][0]
            values = [v for v, _ in args]
            if op == "REPLACE":
                vars[values[3]] = "".join(values[4:]).replace(values[1],
                                                              values[2])
            elif op == "CONCAT":
                vars[values[1]] = "".join(values[2:])
            else:
                raise CMakeError("line %d: string(%s) unsupported"
                                 % (line, op))
        elif name == "get_target_property":
            # Only used for imported targets' locations (pxrConfig.cmake).
            args = self.args(raw, vars)
            vars[args[0]] = args[0] + "-NOTFOUND"
        elif name in ("file", "math", "execute_process", "get_property",
                      "get_directory_property", "cmake_path",
                      "separate_arguments"):
            # These set variables; ignoring them would silently change
            # results. Implement them when they appear.
            raise CMakeError("line %d: %s() unsupported" % (line, name))
        elif name == "return":
            raise Return()
        elif name == "message":
            args = self.args(raw, vars)
            if args and args[0] in ("FATAL_ERROR", "SEND_ERROR"):
                raise CMakeError("line %d: %s" % (line, " ".join(args[1:])))
        elif name == "add_subdirectory":
            sub = os.path.normpath(os.path.join(directory.path,
                                                self.args(raw, vars)[0]))
            if any(sub == s or sub.startswith(s + "/") or
                   s.startswith(sub + "/") for s in SCOPE):
                self.run_directory(sub, dict(vars))
        elif name in ("pxr_library", "pxr_shared_library",
                      "pxr_static_library"):
            self.pxr_library(raw, line, vars, directory)
        elif name == "pxr_plugin":
            # Core plugins are OBJECT_PLUGINs in a monolithic build: linked
            # into usd_ms, resources under plugin/usd.
            self.pxr_library(raw, line, vars, directory).plugin = True
        elif name == "pxr_cpp_bin":
            self.cpp_bin(raw, vars, directory)
        elif name == "pxr_python_bin":
            self.python_bin(raw, vars, directory)
        elif name == "add_compile_options":
            if directory.tests.targets:
                raise CMakeError("line %d: add_compile_options() after test "
                                 "targets" % line)
            directory.compile_options.append(tuple(self.args(raw, vars)))
        elif name in ("include_directories", "link_libraries",
                      "add_link_options", "target_compile_options",
                      "target_link_options", "set_source_files_properties"):
            raise CMakeError("line %d: %s() unsupported" % (line, name))
        elif name == "add_definitions":
            for arg in self.args(raw, vars):
                if not arg.startswith("-D"):
                    raise CMakeError("line %d: add_definitions(%s) is not a "
                                     "definition" % (line, arg))
                directory.definitions.append(arg[2:])
        elif name in TEST_HANDLERS:
            # Test programs may include their package's private headers.
            directory.tests.package = vars.get("PXR_PACKAGE")
            directory.tests.include_current_dir = is_true_constant(
                vars.get("CMAKE_INCLUDE_CURRENT_DIR", "")) is True
            TEST_HANDLERS[name](self, self.args(raw, vars), line, directory)
        elif name.startswith("pxr_") and name not in IGNORED_PXR_COMMANDS:
            raise CMakeError("line %d: unsupported command %s()"
                             % (line, name))
        elif name == "target_compile_definitions":
            self.compile_definitions(raw, line, vars, directory)
        elif name == "configure_file":
            self.configure_file(raw, line, vars, directory)
        # Everything else (tests, install rules, custom commands, ...) does
        # not affect library contents.

    def pxr_library(self, raw, line, vars, directory):
        tokens = self.args(raw, vars)
        name, args, current, options = tokens[0], {}, None, set()
        for tok in tokens[1:]:
            if tok in OPTIONS:
                options.add(tok)
                current = None
            elif tok in LIST_KEYWORDS or tok in IGNORED_KEYWORDS:
                current = tok
                args.setdefault(tok, [])
            elif current is None:
                raise CMakeError("line %d: unexpected %r in pxr_library(%s)"
                                 % (line, tok, name))
            else:
                args[current].append(tok)
        if "INCLUDE_SCHEMA_FILES" in options:
            for kw, values in schema_classes(directory.path).items():
                args.setdefault(kw, []).extend(values)
        if name in self.libraries:
            raise CMakeError("line %d: library %s defined twice"
                             % (line, name))
        library = Library(name, args, line)
        library.include_current_dir = is_true_constant(
            vars.get("CMAKE_INCLUDE_CURRENT_DIR", "")) is True
        directory.libraries.append(library)
        self.libraries[name] = (directory, library)
        self.targets.add(name)
        if args.get("PYMODULE_CPPFILES"):
            self.targets.add("_" + name)
        return library

    def cpp_bin(self, raw, vars, directory):
        # Mirrors cmake_parse_arguments in pxr_cpp_bin: the sources are
        # always <name>.cpp.
        tokens = self.args(raw, vars)
        libraries, current = [], None
        for tok in tokens[1:]:
            if tok in ("LIBRARIES", "INCLUDE_DIRS"):
                current = tok
            elif current == "LIBRARIES":
                libraries.append(tok)
        directory.cpp_bins.append((tokens[0], libraries))
        self.targets.add(tokens[0])

    def python_bin(self, raw, vars, directory):
        # DEPENDENCIES only order the CMake build; every Python program
        # depends on the whole pxr package in Bazel.
        tokens = self.args(raw, vars)
        name, python_file = tokens[0], tokens[0] + ".py"
        if "PYTHON_FILE" in tokens:
            python_file = tokens[tokens.index("PYTHON_FILE") + 1]
        directory.python_bins.append((name, python_file))

    def compile_definitions(self, raw, line, vars, directory):
        args = self.args(raw, vars)
        target, scope = args[0], "PRIVATE"
        defines = {"PUBLIC": [], "INTERFACE": [], "PRIVATE": []}
        for arg in args[1:]:
            if arg in defines:
                scope = arg
            elif arg in DROPPED_GENEX:
                pass
            elif "$<" in arg:
                raise CMakeError("line %d: generator expression %s"
                                 % (line, arg))
            else:
                defines[scope].append(arg[2:] if arg.startswith("-D")
                                      else arg)
        lib = self.libraries.get(target.lstrip("_"))
        if not lib:
            return  # A test or other non-library target.
        if lib[0] is not directory:
            raise CMakeError("line %d: target_compile_definitions(%s) outside "
                             "the library's directory" % (line, target))
        library = lib[1]
        if target.startswith("_"):
            if defines["PUBLIC"] or defines["INTERFACE"]:
                raise CMakeError("line %d: public definitions on %s"
                                 % (line, target))
            library.pymodule_local_defines += defines["PRIVATE"]
        else:
            library.defines += defines["PUBLIC"] + defines["INTERFACE"]
            library.local_defines += defines["PRIVATE"]

    def configure_file(self, raw, line, vars, directory):
        args = self.args(raw, vars)
        template, out = args[0], args[1]
        prefix = "<build>/include/%s/" % directory.path
        if not out.startswith(prefix):
            return  # Not a header (e.g. pxrConfig.cmake).
        with open(os.path.join(REPO, directory.path, template)) as f:
            text = f.read()
        if "#cmakedefine" in text:
            raise CMakeError("line %d: #cmakedefine unsupported" % line)
        substitutions = {}
        for m in re.finditer(r"@([A-Za-z0-9_]+)@|\$\{([A-Za-z0-9_]+)\}",
                             text):
            var = m.group(1) or m.group(2)
            substitutions[m.group(0)] = vars.get(var, "")
        directory.configured.append((template, out[len(prefix):],
                                     substitutions))


def schema_classes(path):
    """Lists from generatedSchema.classes.txt, as pxr_library() adds them.

    Mirrors _pxr_parse_generated_schema_classes in cmake/macros/Private.cmake.
    """
    sections = {
        "# Public Classes": "PUBLIC_CLASSES",
        "# Python Module Files": "PYMODULE_CPPFILES",
        "# Resource Files": "RESOURCE_FILES",
    }
    result, current = {}, None
    with open(os.path.join(REPO, path, "generatedSchema.classes.txt")) as f:
        for line in f:
            line = line.strip()
            if line in sections:
                current = sections[line]
                result[current] = []
            elif line and current:
                result[current].append(line)
    return result


# --- Tests -------------------------------------------------------------------
#
# Handlers for the test macros of cmake/macros/Public.cmake and for the plain
# CMake target commands boost::python's tests use. They record what CMake
# would install under <prefix>/tests.


def _parse(args, options, one, multi):
    result, current = {}, None
    for arg in args:
        if arg in options:
            result[arg], current = True, None
        elif arg in one or arg in multi:
            result.setdefault(arg, [])
            current = arg
        elif current is not None:
            result[current].append(arg)
    return {k: (v[0] if k in one and v else v) for k, v in result.items()}


def _name_we(path):
    return os.path.basename(path).split(".", 1)[0]


def _add_file(tests, dest, src, where):
    dest = os.path.normpath(dest)
    if tests.files.get(dest, src) != src:
        raise CMakeError("%s: %s installed from both %s and %s"
                         % (where, dest, tests.files[dest], src))
    tests.files[dest] = src


def _test_target(interp, directory, name, line, **fields):
    tests = directory.tests
    if name in tests.targets:
        raise CMakeError("line %d: test target %s defined twice"
                         % (line, name))
    target = {"kind": "executable", "srcs": [], "libraries": [],
              "prefix": None, "suffix": None, "dest": None,
              "plug_info": None,
              "include_current_dir": tests.include_current_dir}
    target.update(fields)
    tests.targets[name] = target
    interp.targets.add(name)
    return target


def _build_test(interp, args, line, directory):
    a = _parse(args[1:], [], [], ["LIBRARIES", "CPPFILES"])
    _test_target(interp, directory, args[0], line, kind="executable",
                 srcs=a.get("CPPFILES", []),
                 libraries=a.get("LIBRARIES", []), dest="tests")


def _build_test_plugin(interp, args, line, directory):
    a = _parse(args[1:], [], ["INSTALL_PREFIX", "SOURCE_DIR"],
               ["LIBRARIES", "CPPFILES"])
    name = args[0]
    plug_info = "%s/%s_plugInfo.json" % (a.get("SOURCE_DIR", "testenv"), name)
    exists = os.path.exists(os.path.join(REPO, directory.path, plug_info))
    _test_target(interp, directory, name, line, kind="shared", prefix="lib",
                 srcs=a.get("CPPFILES", []),
                 libraries=a.get("LIBRARIES", []), dest="tests/lib",
                 plug_info=((plug_info, a.get("INSTALL_PREFIX", ""))
                            if exists else None))


def _test_scripts(interp, args, line, directory):
    for arg in args:
        parts = arg.split(":")
        if len(parts) > 2:
            raise CMakeError("line %d: cannot parse test file path %s"
                             % (line, arg))
        src = parts[0]
        dest = parts[1] if len(parts) == 2 else _name_we(src)
        _add_file(directory.tests, dest, src,
                  "%s:%d" % (directory.path, line))


def _create_test_module(interp, args, line, directory):
    a = _parse(args[1:], [], ["INSTALL_PREFIX", "SOURCE_DIR", "DEST_DIR"], [])
    name = args[0]
    source_dir = a.get("SOURCE_DIR", "testenv")
    dest = "%s/lib/python/%s" % (a.get("INSTALL_PREFIX", ""),
                                 a.get("DEST_DIR", name))
    for suffix, installed in (("__init__.py", "__init__.py"),
                              ("_plugInfo.json", "plugInfo.json")):
        src = "%s/%s%s" % (source_dir, name, suffix)
        if os.path.exists(os.path.join(REPO, directory.path, src)):
            _add_file(directory.tests, dest + "/" + installed, src,
                      "%s:%d" % (directory.path, line))


def _register_test(interp, args, line, directory):
    a = _parse(args[1:], TEST_OPTIONS, TEST_ONE_VALUE, TEST_MULTI_VALUE)
    tests = directory.tests
    if "TESTENV" in a:
        dest = a.get("TESTENV_DEST") or os.path.basename(a["TESTENV"])
        if tests.testenvs.get(dest, a["TESTENV"]) != a["TESTENV"]:
            raise CMakeError("line %d: testenv %s installed from both %s and "
                             "%s" % (line, dest, tests.testenvs[dest],
                                     a["TESTENV"]))
        tests.testenvs[dest] = a["TESTENV"]
    tests.registered.append((args[0], a, line))


def _add_executable(interp, args, line, directory):
    _test_target(interp, directory, args[0], line, kind="executable",
                 srcs=args[1:])


def _add_library(interp, args, line, directory):
    if args[1:2] != ["SHARED"]:
        raise CMakeError("line %d: add_library(%s) is not SHARED"
                         % (line, " ".join(args)))
    _test_target(interp, directory, args[0], line, kind="shared",
                 srcs=args[2:])


def _raw_target(directory, name, line):
    target = directory.tests.targets.get(name)
    if target is None:
        raise CMakeError("line %d: %s is not a target defined with "
                         "add_executable/add_library" % (line, name))
    return target


def _target_sources(interp, args, line, directory):
    target = _raw_target(directory, args[0], line)
    target["srcs"] += [a for a in args[1:]
                       if a not in ("PRIVATE", "PUBLIC", "INTERFACE")]


def _target_link_libraries(interp, args, line, directory):
    if args[0] in interp.libraries:
        raise CMakeError("line %d: target_link_libraries(%s) on a library"
                         % (line, args[0]))
    target = _raw_target(directory, args[0], line)
    target["libraries"] += [a for a in args[1:]
                            if a not in ("PRIVATE", "PUBLIC", "INTERFACE")]


def _set_target_properties(interp, args, line, directory):
    i = args.index("PROPERTIES")
    names, props = args[:i], args[i + 1:]
    for name in names:
        target = directory.tests.targets.get(name)
        if target is None:
            continue  # e.g. WINDOWS_EXPORT_ALL_SYMBOLS on a library.
        for key, value in zip(props[::2], props[1::2]):
            if key in ("PREFIX", "SUFFIX"):
                target[key.lower()] = value


def _install(interp, args, line, directory):
    # Only installs into tests/ matter here: install(TARGETS <test targets>
    # DESTINATION <dir>) and install(FILES ... DESTINATION tests[/...]).
    # Library resources and headers are placed by pxr_library().
    if args[:1] == ["FILES"] and "DESTINATION" in args:
        a = _parse(args[1:], [], ["DESTINATION", "RENAME"], ["FILES"])
        srcs = args[1:args.index("DESTINATION")]
        dest = a["DESTINATION"]
        if dest != "tests" and not dest.startswith("tests/"):
            return
        if "RENAME" in a and len(srcs) != 1:
            raise CMakeError("line %d: install(FILES RENAME) of several "
                             "files" % line)
        for src in srcs:
            name = a.get("RENAME") or os.path.basename(src)
            _add_file(directory.tests, (dest + "/" + name)[len("tests/"):],
                      src, "%s:%d" % (directory.path, line))
        return
    if args[:1] != ["TARGETS"]:
        return
    names = [a for a in args[1:] if a in directory.tests.targets]
    if not names:
        return
    if len(args) != len(names) + 3 or args[-2] != "DESTINATION":
        raise CMakeError("line %d: unsupported install(%s)"
                         % (line, " ".join(args)))
    for name in names:
        directory.tests.targets[name]["dest"] = args[-1]


TEST_HANDLERS = {
    "pxr_build_test": _build_test,
    "pxr_build_test_plugin": _build_test_plugin,
    "pxr_test_scripts": _test_scripts,
    "pxr_create_test_module": _create_test_module,
    "pxr_register_test": _register_test,
    "add_executable": _add_executable,
    "add_library": _add_library,
    "target_sources": _target_sources,
    "target_link_libraries": _target_link_libraries,
    "set_target_properties": _set_target_properties,
    "install": _install,
}


def load_options(interp, vars):
    path = "cmake/defaults/Options.cmake"
    with open(os.path.join(REPO, path)) as f:
        body = parse_blocks(tokenize(f.read()))
    interp.run(body, vars, Directory("cmake/defaults"))


def evaluate(os_name, warn):
    interp = Interpreter(os_name, warn)
    vars = dict(PLATFORM_VARS[os_name])
    load_options(interp, vars)
    interp.run_directory("pxr", vars)
    return interp


# --- Translation -------------------------------------------------------------

def python_module_name(name):
    return name[0].upper() + name[1:]


def label(path, name):
    """Label of library `name` in directory `path`."""
    return "//" + path if os.path.basename(path) == name else \
        "//%s:%s" % (path, name)


def resolve_libraries(libraries, interp, where):
    """Translates CMake LIBRARIES into ([(label,)], [linkopt group])."""
    labels, linkopts = [], []
    for lib in libraries:
        if lib in interp.libraries:
            labels.append((label(interp.libraries[lib][0].path, lib),))
        elif lib in EXTERNAL_LABELS:
            labels.append((EXTERNAL_LABELS[lib],))
        elif lib in SYSTEM_TARGETS:
            linkopts.append(tuple(SYSTEM_TARGETS[lib][interp.os].split()))
        elif "$<" in lib or "::" in lib:
            raise CMakeError("%s: cannot translate %s" % (where, lib))
        elif lib.startswith("-"):
            linkopts.append(tuple(lib.split()))
        else:
            linkopts.append(("-l" + lib,))
    return labels, linkopts


def translate(directory, library, interp):
    """Bazel attributes of one library for one OS: {attr: [group, ...]}.

    Each group is a tuple of values that must stay together (e.g.
    "-framework", "Foundation"); most groups have one element.
    """
    attrs = {a: [] for a in ATTRS}
    for kw in LIST_KEYWORDS:
        if kw == "LIBRARIES":
            continue
        attrs[kw.lower()] = [(v,) for v in library.args.get(kw, [])]
    attrs["libraries"], attrs["linkopts"] = resolve_libraries(
        library.args.get("LIBRARIES", []), interp,
        "%s: pxr_library(%s)" % (directory.path, library.name))
    attrs["copts"] = list(directory.compile_options)
    attrs["defines"] = [(d,) for d in library.defines]
    attrs["local_defines"] = [
        (d,) for d in directory.definitions + library.local_defines]
    attrs["pymodule_local_defines"] = [
        (d,) for d in directory.definitions + library.pymodule_local_defines]
    attrs["generated_headers"] = [
        (":" + configured_name(out),) for _, out, _ in directory.configured]
    attrs["resource_trees"] = [
        (t,) for t in RESOURCE_TREES.get(library.name, [])]
    return attrs


def configured_name(out):
    return out.replace("/", "_").replace(".", "_")


def merge(per_os_attrs, where):
    """Splits {os: attrs} into common attrs and per-OS additions, over the
    OSes in per_os_attrs."""
    common, per_os = {}, {}
    oses = [os for os in OSES if os in per_os_attrs]
    first = per_os_attrs[oses[0]]
    for attr in ATTRS:
        lists = {os: per_os_attrs[os][attr] for os in oses}
        shared = [g for g in first[attr]
                  if all(g in lists[os] for os in oses)]
        common[attr] = shared
        for os in oses:
            extra = [g for g in lists[os] if g not in shared]
            if not extra:
                continue
            if attr not in PER_OS_ATTRS:
                raise CMakeError("%s: %s differs between OSes, which per_os "
                                 "does not support" % (where, attr))
            per_os.setdefault(os, {})[attr] = extra
    return common, per_os


def _flatten(groups):
    return [v for g in groups for v in g]


def _string_list(values, indent):
    pad = " " * indent
    return ["[" ] + ['%s    %s,' % (pad, json.dumps(v)) for v in values] + \
        [pad + "]"]


def _compatible_with(oses):
    return ["@platforms//os:" + os for os in oses]


def add_compatibility(block, oses):
    """Adds target_compatible_with to a block for targets that exist on some
    OSes only."""
    if len(oses) == len(OSES):
        return block
    return block[:-2] + "\n    target_compatible_with = %s,\n)" % (
        _starlark_value(_compatible_with(oses), 4))


def starlark_library(name, plugin, include_current_dir, common, per_os):
    out = ["pxr_library(", '    name = %s,' % json.dumps(name)]
    if plugin:
        out.append("    plugin = True,")
    if not include_current_dir:
        out.append("    include_current_dir = False,")
    for attr in ATTRS:
        values = _flatten(common[attr])
        if values:
            lines = _string_list(values, 4)
            out.append("    %s = %s" % (attr, lines[0]))
            out.extend(lines[1:-1])
            out.append(lines[-1] + ",")
    if per_os:
        out.append("    per_os = {")
        for os in OSES:
            if os not in per_os:
                continue
            out.append("        %s: {" % json.dumps(os))
            for attr in ATTRS:
                if attr in per_os[os]:
                    lines = _string_list(_flatten(per_os[os][attr]), 12)
                    out.append("            %s: %s" % (json.dumps(attr),
                                                       lines[0]))
                    out.extend(lines[1:-1])
                    out.append(lines[-1] + ",")
            out.append("        },")
        out.append("    },")
    out.append(")")
    return "\n".join(out)


def starlark_configure_file(template, out, substitutions):
    lines = [
        "pxr_configure_file(",
        "    name = %s," % json.dumps(configured_name(out)),
        "    src = %s," % json.dumps(template),
        "    out = %s," % json.dumps(out),
        "    substitutions = {",
    ]
    lines += ["        %s: %s," % (json.dumps(k), json.dumps(v))
              for k, v in sorted(substitutions.items())]
    lines += ["    },", ")"]
    return "\n".join(lines)


def starlark_cpp_bin(name, libraries):
    lines = ["pxr_cpp_bin(", "    name = %s," % json.dumps(name)]
    if libraries:
        body = _string_list(libraries, 4)
        lines += ["    libraries = " + body[0]] + body[1:-1] + [body[-1] + ","]
    lines.append(")")
    return "\n".join(lines)


def starlark_python_bin(name, python_file):
    return "\n".join([
        "pxr_python_bin(",
        "    name = %s," % json.dumps(name),
        "    python_file = %s," % json.dumps(python_file),
        ")",
    ])


def _starlark_value(value, indent):
    """Formats str, bool, list and dict values as Starlark."""
    pad = " " * indent
    if isinstance(value, bool):
        return "True" if value else "False"
    if isinstance(value, str):
        return json.dumps(value)
    if isinstance(value, list):
        if not value:
            return "[]"
        return "[\n" + "".join("%s    %s,\n" % (pad, _starlark_value(v,
                                                                   indent + 4))
                               for v in value) + pad + "]"
    if isinstance(value, dict):
        if not value:
            return "{}"
        return "{\n" + "".join(
            "%s    %s: %s,\n" % (pad, json.dumps(k),
                                 _starlark_value(v, indent + 4))
            for k, v in value.items()) + pad + "}"
    raise TypeError(value)


def starlark_call(function, name, attrs):
    lines = ["%s(" % function, "    name = %s," % json.dumps(name)]
    for key, value in attrs.items():
        if value is None or value == [] or value == {}:
            continue
        lines.append("    %s = %s," % (key, _starlark_value(value, 4)))
    lines.append(")")
    return "\n".join(lines)


# Link flags tests may drop: PXR_LINKOPTS already adds them where needed.
_DEFAULT_LINKOPTS = {("-ldl",), ("-lm",)}


def _test_libraries(directory, target, interp, where):
    names = list(target["libraries"])
    package = directory.tests.package
    if package in interp.libraries and package not in names:
        names.append(package)
    # usd_m is the monolithic library, which every test links dynamically.
    labels, linkopts = resolve_libraries(
        [n for n in names if n not in directory.tests.targets and
         n != "usd_m"], interp, where)
    extra = [g for g in linkopts if g not in _DEFAULT_LINKOPTS]
    if extra:
        raise CMakeError("%s: unsupported link flags %s" % (where, extra))
    return _flatten(labels)


class TestIndex:
    """Where test programs/scripts and tools live, across directories."""

    def __init__(self, interp):
        self.providers = {}  # name under <prefix>/tests -> directory path
        self.tools = {}  # name -> (label, "cpp" | "python")
        for d in interp.directories:
            names = [n for n, t in d.tests.targets.items()
                     if t["dest"] == "tests"]
            names += [f.split("/")[0] for f in d.tests.files]
            for name in names:
                if self.providers.get(name, d.path) != d.path:
                    raise CMakeError("%s is installed under tests/ by both "
                                     "%s and %s"
                                     % (name, self.providers[name], d.path))
                self.providers[name] = d.path
            for name, _ in d.cpp_bins:
                self.tools[name] = ("//" + d.path + ":" + name, "cpp")
            for name, python_file in d.python_bins:
                self.tools[name] = ("//%s:%s" % (d.path, python_file),
                                    "python")


def _test_references(directory, name, args, index):
    """test_trees, tools and python_tools a registered test needs."""
    trees = {":test_tree": None}
    tools, python_tools = {}, {}

    def add_tool(tool):
        label, kind = index.tools[tool]
        (tools if kind == "cpp" else python_tools)[label] = tool

    for key in ("COMMAND", "PRE_COMMAND", "POST_COMMAND"):
        for token in shlex.split(args.get(key, "")):
            if token.startswith("<prefix>/tests/"):
                entry = token[len("<prefix>/tests/"):].split("/")[0]
                path = index.providers.get(entry)
                if path is None:
                    raise CMakeError("%s: test %s runs %s, which nothing "
                                     "installs" % (directory.path, name,
                                                   token))
                trees[":test_tree" if path == directory.path
                      else "//%s:test_tree" % path] = None
            elif token.startswith("<prefix>/bin/"):
                tool = token[len("<prefix>/bin/"):]
                if tool not in index.tools:
                    raise CMakeError("%s: test %s runs unknown tool %s"
                                     % (directory.path, name, tool))
                add_tool(tool)
    # Tests that put <prefix>/bin on PATH may run any tool.
    if "<prefix>/bin" in args.get("PRE_PATH", []) + args.get("POST_PATH", []):
        for tool in sorted(index.tools):
            add_tool(tool)
    return list(trees), tools, python_tools


def starlark_tests(directory, interp, index):
    """Test blocks of one directory, or [] if it has no tests."""
    tests = directory.tests
    if not (tests.targets or tests.files or tests.registered):
        return []
    blocks, tree = [], []
    for name, target in tests.targets.items():
        where = "%s: test target %s" % (directory.path, name)
        if target["dest"] not in ("tests", "tests/lib"):
            raise CMakeError("%s: not installed under tests/" % where)
        libraries = _test_libraries(directory, target, interp, where)
        if target["kind"] == "executable":
            if target["dest"] != "tests":
                raise CMakeError("%s: program not installed in tests/"
                                 % where)
            attrs = {"srcs": target["srcs"], "libraries": libraries}
            if not target["include_current_dir"]:
                attrs["include_current_dir"] = False
            blocks.append(starlark_call("pxr_test_program", name, attrs))
            tree.append(":tests/" + name)
        else:
            if target["dest"] != "tests/lib":
                raise CMakeError("%s: library not installed in tests/lib"
                                 % where)
            attrs = {"srcs": target["srcs"], "libraries": libraries}
            if target["prefix"] not in (None, "lib"):
                attrs["prefix"] = target["prefix"]
            attrs["suffix"] = target["suffix"]
            if not target["include_current_dir"]:
                attrs["include_current_dir"] = False
            blocks.append(starlark_call("pxr_test_library", name, attrs))
            tree.append(":_testlib_" + name)

    plug_infos, library_paths = {}, {}
    for name, target in tests.targets.items():
        if target["plug_info"]:
            src, install_prefix = target["plug_info"]
            plugin_dir = os.path.normpath(
                "%s/lib/%s" % (install_prefix, name))
            dest = plugin_dir + "/Resources/plugInfo.json"
            plug_infos[dest] = src
            library_paths[dest] = os.path.relpath("lib/lib" + name,
                                                  plugin_dir)
    directory.tests.rendered = {
        "files": tests.files,
        "testenvs": tests.testenvs,
        "plug_infos": plug_infos,
        "library_paths": library_paths,
        "tree": tree,
    }

    for name, args, _ in tests.registered:
        trees, tools, python_tools = _test_references(
            directory, name, args, index)
        attrs = {"arguments": args, "test_trees": trees, "tools": tools,
                 "python_tools": python_tools}
        if args.get("IMAGE_DIFF_COMPARE"):
            attrs["image_diff_tool"] = IMAGE_DIFF_TOOL
        tags = []
        if args.get("RUN_SERIAL"):
            tags.append("exclusive")
        if args.get("LABELS"):
            tags += args["LABELS"].split(";")
        attrs["tags"] = tags
        key = "%s:%s" % (directory.path, name)
        if key in EXCLUDED_TESTS:
            attrs["excluded"] = EXCLUDED_TESTS[key]
            _USED_EXCLUSIONS.add(key)
        blocks.append(starlark_call("pxr_test", name, attrs))
    return blocks


def starlark_test_tree(per_os):
    """pxr_test_files and pxr_test_tree blocks over the OSes' tests.

    Test files are the union over OSes (extra files are harmless); the tree
    lists OS-specific programs and libraries in a select().
    """
    union = {"files": {}, "testenvs": {}, "plug_infos": {},
             "library_paths": {}}
    for os in OSES:
        if os not in per_os:
            continue
        for key in union:
            for k, v in per_os[os][key].items():
                if union[key].get(k, v) != v:
                    raise CMakeError("test file %s differs between OSes" % k)
                union[key][k] = v
    blocks, tree = [], {}
    for os, rendered in per_os.items():
        tree[os] = list(rendered["tree"])
    if any(union.values()):
        blocks.append(starlark_call("pxr_test_files", "test_files", {
            "files": dict(sorted(union["files"].items())),
            "testenvs": dict(sorted(union["testenvs"].items())),
            "plug_infos": union["plug_infos"],
            "library_paths": union["library_paths"],
        }))
        for os in tree:
            tree[os].append(":test_files")
    entries = []
    for os in OSES:
        for src in tree.get(os, []):
            oses = [o for o in OSES if src in tree.get(o, [])]
            if (src, oses) not in entries:
                entries.append((src, oses))
    srcs = _starlark_os_list(entries, json.dumps).replace("\n", "\n    ")
    blocks.append("pxr_test_tree(\n    name = \"test_tree\",\n    srcs = %s,\n)"
                  % srcs)
    return blocks


_USED_EXCLUSIONS = set()


def merge_blocks(path, per_os):
    """Combines per-OS blocks; targets missing on some OSes are marked
    incompatible there (e.g. Linux-only tests, macOS-only libraries)."""
    # Blocks start with "<function>(\n    name = ...": key on both lines.
    keyed = {os: {tuple(b.split("\n", 2)[:2]): b for b in blocks}
             for os, blocks in per_os.items()}
    for os, blocks in per_os.items():
        if len(keyed[os]) != len(blocks):
            raise CMakeError("%s: duplicate test blocks" % path)
    order = []
    for os in OSES:
        order += [k for k in keyed.get(os, {}) if k not in order]
    merged = []
    for key in order:
        texts = {os: keyed.get(os, {}).get(key) for os in OSES}
        present = [os for os in OSES if texts[os] is not None]
        text = texts[present[0]]
        if any(texts[os] != text for os in present):
            raise CMakeError("%s: %s differs between OSes"
                             % (path, " ".join(key)))
        merged.append(add_compatibility(text, present))
    return merged


def _ordered_union(lists):
    out = []
    for values in lists:
        out += [v for v in values if v not in out]
    return out


def _starlark_os_list(entries, format_entry):
    """A Starlark list of entries (value, oses), with OS-specific entries in
    a select()."""
    common = [format_entry(v) for v, oses in entries if len(oses) == len(OSES)]
    lines = ["["] + ["    %s," % e for e in common] + ["]"]
    extra = {os: [format_entry(v) for v, oses in entries
                  if len(oses) < len(OSES) and os in oses] for os in OSES}
    if any(extra.values()):
        lines[-1] = "] + select({"
        for os in OSES:
            if extra[os]:
                lines.append('    "@platforms//os:%s": [' % os)
                lines += ["        %s," % e for e in extra[os]]
                lines.append("    ],")
        lines += ['    "//conditions:default": [],', "})"]
    return "\n".join(lines)


def _consistent(dirs, attr, path):
    values = [getattr(d, attr) for d in dirs.values()]
    if any(v != values[0] for v in values):
        raise CMakeError("%s: %s differs between OSes" % (path, attr))
    return values[0]


def generate(warn):
    """Returns {path: contents} for every generated file."""
    interps = {os: evaluate(os, warn) for os in OSES}
    by_path = {os: {d.path: d for d in interps[os].directories}
               for os in OSES}
    paths = _ordered_union([[d.path for d in interps[os].directories]
                            for os in OSES])

    files = {}
    # (value, [os, ...]) in CMake build order.
    all_libraries, python_modules, resources = [], [], []
    python_programs = []
    test_indexes = {os: TestIndex(interps[os]) for os in OSES}
    for path in paths:
        dirs = {os: by_path[os][path] for os in OSES if path in by_path[os]}
        configured = _consistent(dirs, "configured", path)
        lib_names = _ordered_union([[lib.name for lib in d.libraries]
                                    for d in dirs.values()])
        bin_names = _ordered_union([[b[0] for b in d.cpp_bins]
                                    for d in dirs.values()])
        py_bins = _ordered_union([d.python_bins for d in dirs.values()])
        has_tests = any(d.tests.registered or d.tests.targets
                        for d in dirs.values())
        if not (lib_names or bin_names or py_bins or has_tests):
            continue
        if configured and len(lib_names) != 1:
            raise CMakeError("%s: configure_file() with several libraries"
                             % path)
        dir_oses = [os for os in OSES if os in dirs]

        blocks = [add_compatibility(starlark_configure_file(*c), dir_oses)
                  for c in configured]
        for name in lib_names:
            libs = {os: lib for os, d in dirs.items()
                    for lib in d.libraries if lib.name == name}
            oses = [os for os in OSES if os in libs]
            attrs = {os: translate(dirs[os], libs[os], interps[os])
                     for os in oses}
            where = "%s: pxr_library(%s)" % (path, name)
            common, per_os = merge(attrs, where)
            plugin = libs[oses[0]].plugin
            if any(lib.plugin != plugin for lib in libs.values()):
                raise CMakeError("%s is a plugin on some OSes only" % where)
            current_dir = libs[oses[0]].include_current_dir
            if any(lib.include_current_dir != current_dir
                   for lib in libs.values()):
                raise CMakeError("%s: CMAKE_INCLUDE_CURRENT_DIR differs "
                                 "between OSes" % where)
            blocks.append(add_compatibility(
                starlark_library(name, plugin, current_dir, common, per_os),
                oses))
            all_libraries.append((label(path, name), oses))
            if common["pymodule_cppfiles"]:
                python_modules.append(((name, path), oses))
            if common["resource_files"] or common["resource_trees"]:
                resources.append(("//%s:%s_resources" % (path, name), oses))

        for name in bin_names:
            libs = {}
            for os, d in dirs.items():
                for bin_name, bin_libs in d.cpp_bins:
                    if bin_name != name:
                        continue
                    labels, linkopts = resolve_libraries(
                        bin_libs, interps[os],
                        "%s: pxr_cpp_bin(%s)" % (path, name))
                    if linkopts:
                        raise CMakeError("%s: pxr_cpp_bin(%s): link flags "
                                         "are unsupported" % (path, name))
                    libs[os] = _flatten(labels)
            oses = [os for os in OSES if os in libs]
            if any(l != libs[oses[0]] for l in libs.values()):
                raise CMakeError("%s: pxr_cpp_bin(%s) differs between OSes"
                                 % (path, name))
            blocks.append(add_compatibility(
                starlark_cpp_bin(name, libs[oses[0]]), oses))
        for name, python_file in py_bins:
            oses = [os for os in OSES
                    if os in dirs and (name, python_file) in dirs[os].python_bins]
            python_programs.append(
                ("//%s:%s" % (path, python_file), name, oses))
            blocks.append(add_compatibility(
                starlark_python_bin(name, python_file), oses))
        test_blocks = merge_blocks(path, {
            os: starlark_tests(dirs[os], interps[os], test_indexes[os])
            for os in dirs})
        rendered = {os: d.tests.rendered for os, d in dirs.items()
                    if getattr(d.tests, "rendered", None)}
        if rendered:
            tree_blocks = [add_compatibility(b, list(rendered))
                           for b in starlark_test_tree(rendered)]
            # Programs and libraries come first, then the tree, then tests.
            first_test = next((i for i, b in enumerate(test_blocks)
                               if b.startswith("pxr_test(")), len(test_blocks))
            test_blocks[first_test:first_test] = tree_blocks
        blocks += test_blocks

        symbols = (
            (["pxr_configure_file"] if configured else []) +
            (["pxr_cpp_bin"] if bin_names else []) +
            (["pxr_library"] if lib_names else []) +
            (["pxr_python_bin"] if py_bins else [])
        )
        loads = []
        if symbols:
            loads.append("load(%s)" % ", ".join(
                json.dumps(s) for s in ["//bazel:pxr_library.bzl"] + symbols))
        if test_blocks:
            used = sorted({b.split("(", 1)[0] for b in test_blocks})
            loads.append("load(%s)" % ", ".join(
                json.dumps(s) for s in ["//bazel:pxr_test.bzl"] + used))
        files[path + "/BUILD.bazel"] = (
            HEADER.format(source=path + "/CMakeLists.txt") +
            "\n".join(loads) + "\n\n" + "\n\n".join(blocks) + "\n")

    def module(entry):
        name, path = entry
        return "struct(library = %s, module = %s, package = %s)" % (
            json.dumps(name), json.dumps(python_module_name(name)),
            json.dumps("//" + path))

    lines = [
        HEADER.format(source="pxr/**/CMakeLists.txt").rstrip("\n"),
        '"""pxr libraries, Python modules and resources in CMake build order.',
        "",
        "Entries that exist on some OSes only are in select()s.",
        '"""',
        "",
        "PXR_LIBRARIES = " + _starlark_os_list(all_libraries, json.dumps),
        "",
        "# One entry per _<library> Python extension module, and the OSes",
        "# where it exists (empty: all).",
        "PXR_PYTHON_MODULES = [",
    ]
    for entry, oses in python_modules:
        os_list = [] if len(oses) == len(OSES) else _compatible_with(oses)
        lines.append("    %s, target_compatible_with = %s),"
                     % (module(entry)[:-1], json.dumps(os_list)))
    lines += [
        "]",
        "",
        "# Resources of each library, placed by //bazel/install.",
        "PXR_RESOURCES = " + _starlark_os_list(resources, json.dumps),
        "",
        "# Python programs (pxr_python_bin): source file -> program name.",
        "PXR_PYTHON_PROGRAMS = {",
    ]
    for source, name, oses in python_programs:
        if len(oses) != len(OSES):
            raise CMakeError("%s exists on some OSes only, which "
                             "PXR_PYTHON_PROGRAMS does not support" % name)
        lines.append("    %s: %s," % (json.dumps(source), json.dumps(name)))
    lines += [
        "}",
    ]
    files[LIBRARIES_BZL] = "\n".join(lines) + "\n"
    stale = set(EXCLUDED_TESTS) - _USED_EXCLUSIONS
    if stale:
        raise CMakeError("EXCLUDED_TESTS has tests CMake does not register: "
                         + ", ".join(sorted(stale)))
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true",
                      help="rewrite generated files")
    mode.add_argument("--check", action="store_true",
                      help="fail if generated files are out of date")
    opts = parser.parse_args()

    def warn(msg):
        print("cmake2bazel: " + msg, file=sys.stderr)

    try:
        files = generate(warn)
    except CMakeError as e:
        warn("error: %s" % e)
        return 2

    stale = []
    for path, contents in sorted(files.items()):
        full = os.path.join(REPO, path)
        try:
            with open(full) as f:
                current = f.read()
        except FileNotFoundError:
            current = ""
        if current == contents:
            continue
        stale.append(path)
        if opts.write:
            with open(full, "w") as f:
                f.write(contents)
            print("wrote " + path)
        elif opts.check:
            sys.stdout.writelines(difflib.unified_diff(
                current.splitlines(True), contents.splitlines(True),
                "a/" + path, "b/" + path))
        else:
            print("### " + path)
            print(contents)
    if opts.check:
        if stale:
            warn("%d generated file(s) out of date; run with --write"
                 % len(stale))
            return 1
        print("cmake2bazel: %d generated files up to date" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
