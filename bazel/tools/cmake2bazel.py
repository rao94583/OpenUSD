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
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))

# Directories translated so far. Grows with each phase of the Bazel port.
SCOPE = ["pxr/external", "pxr/base", "pxr/usd", "pxr/usdValidation"]

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
    "M_LIB": "m",  # Packages.cmake
    # Packages.cmake; mapped to a label in EXTERNAL_LABELS.
    "PYTHON_LIBRARIES": "Python3::Python",
    "PYTHON_INCLUDE_DIRS": "",
    "PROJECT_SOURCE_DIR": "<source>",
    "PROJECT_BINARY_DIR": "<build>",
}
PLATFORM_VARS = {
    "macos": dict(_COMMON_VARS, APPLE="1", CMAKE_SYSTEM_NAME="Darwin",
                  CMAKE_DL_LIBS=""),
    "linux": dict(_COMMON_VARS, CMAKE_SYSTEM_NAME="Linux",
                  CMAKE_DL_LIBS="dl"),
}

# CMake targets defined outside pxr, and their Bazel labels.
EXTERNAL_LABELS = {
    "TBB::tbb": "//vendor/onetbb:tbb",
    # Libraries and Python extensions resolve libpython symbols at load time
    # (PXR_PY_UNDEFINED_DYNAMIC_LOOKUP), so only the headers are needed;
    # executables link libpython themselves.
    "Python3::Python": "@rules_python//python/cc:current_py_cc_headers",
}
# Other targets that if(TARGET ...) should consider defined.
OTHER_TARGETS = {"python_modules", "shared_libs"}

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
    # Tests: Phase 3.
    "pxr_build_test",
    "pxr_build_test_plugin",
    "pxr_build_test_shared_lib",
    "pxr_create_test_module",
    "pxr_install_test_dir",
    "pxr_register_test",
    "pxr_test_scripts",
    # Rpaths of boost::python's test libraries.
    "pxr_set_rpaths_for_target",
}

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
    "RESOURCE_FILES",
    "DOXYGEN_FILES",
]
# Keywords that are accepted but have no Bazel equivalent.
IGNORED_KEYWORDS = {"INCLUDE_DIRS", "TYPE", "PRECOMPILED_HEADER_NAME"}
# pxr_library() options (flags without values).
OPTIONS = {"DISABLE_PRECOMPILED_HEADERS", "INCLUDE_SCHEMA_FILES"}

# Bazel attributes in output order; LIST_KEYWORDS map to their lower-case
# names, the rest come from other CMake commands.
EXTRA_ATTRS = ["linkopts", "defines", "local_defines",
               "pymodule_local_defines", "generated_headers"]
ATTRS = [k.lower() for k in LIST_KEYWORDS] + EXTRA_ATTRS
# Attributes pxr_library() accepts under per_os.
PER_OS_ATTRS = {"libraries", "public_headers", "private_headers", "cppfiles",
                "linkopts"}


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
                    j = i
                    while j < n and not text[j].isspace() and \
                            text[j] not in "()":
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


class Directory:
    """Results of evaluating one CMakeLists.txt."""

    def __init__(self, path):
        self.path = path  # relative to REPO
        self.libraries = []
        self.configured = []  # (template, out, {placeholder: value})
        self.definitions = []  # add_definitions()
        self.cpp_bins = []  # (name, [library])
        self.python_bins = []  # (name, python file)


class Return(Exception):
    pass


class Interpreter:
    def __init__(self, os_name, warn):
        self.os, self.warn = os_name, warn
        self.functions = set()
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
                self.functions.add(self.args(node[1], vars)[0].lower())
            elif kind == "while":
                raise CMakeError("line %d: while() unsupported" % node[3])
            else:
                self.command(node[1], node[2], node[3], vars, directory)

    def command(self, name, raw, line, vars, directory):
        if name == "set":
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
            var = self.args(raw, vars)[0]
            if var not in vars:
                vars[var] = var + "-NOTFOUND"
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
        elif name == "add_definitions":
            for arg in self.args(raw, vars):
                if not arg.startswith("-D"):
                    raise CMakeError("line %d: add_definitions(%s) is not a "
                                     "definition" % (line, arg))
                directory.definitions.append(arg[2:])
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
    attrs["defines"] = [(d,) for d in library.defines]
    attrs["local_defines"] = [
        (d,) for d in directory.definitions + library.local_defines]
    attrs["pymodule_local_defines"] = [
        (d,) for d in directory.definitions + library.pymodule_local_defines]
    attrs["generated_headers"] = [
        (":" + configured_name(out),) for _, out, _ in directory.configured]
    return attrs


def configured_name(out):
    return out.replace("/", "_").replace(".", "_")


def merge(per_os_attrs, where):
    """Splits {os: attrs} into common attrs and per-OS additions."""
    common, per_os = {}, {}
    first = per_os_attrs[OSES[0]]
    for attr in ATTRS:
        lists = {os: per_os_attrs[os][attr] for os in OSES}
        shared = [g for g in first[attr]
                  if all(g in lists[os] for os in OSES)]
        common[attr] = shared
        for os in OSES:
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


def starlark_library(name, plugin, common, per_os):
    out = ["pxr_library(", '    name = %s,' % json.dumps(name)]
    if plugin:
        out.append("    plugin = True,")
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


def generate(warn):
    """Returns {path: contents} for every generated file."""
    interps = {os: evaluate(os, warn) for os in OSES}
    first = interps[OSES[0]]
    for os in OSES[1:]:
        if [d.path for d in interps[os].directories] != \
                [d.path for d in first.directories]:
            raise CMakeError("directories differ between OSes")

    files = {}
    all_libraries, python_modules, resources = [], [], []
    for index, directory in enumerate(first.directories):
        dirs = {os: interps[os].directories[index] for os in OSES}
        names = [lib.name for lib in directory.libraries]
        for os in OSES:
            if [lib.name for lib in dirs[os].libraries] != names:
                raise CMakeError("%s: libraries differ between OSes"
                                 % directory.path)
            for attr in ("configured", "python_bins"):
                if getattr(dirs[os], attr, None) != \
                        getattr(directory, attr, None):
                    raise CMakeError("%s: %s differs between OSes"
                                     % (directory.path, attr))
            if [b[0] for b in dirs[os].cpp_bins] != \
                    [b[0] for b in directory.cpp_bins]:
                raise CMakeError("%s: pxr_cpp_bin()s differ between OSes"
                                 % directory.path)
        if not (names or directory.cpp_bins or directory.python_bins):
            continue
        if directory.configured and len(names) != 1:
            raise CMakeError("%s: configure_file() with several libraries"
                             % directory.path)

        blocks = [starlark_configure_file(*c) for c in directory.configured]
        for i, name in enumerate(names):
            attrs = {os: translate(dirs[os], dirs[os].libraries[i],
                                   interps[os]) for os in OSES}
            common, per_os = merge(attrs, "%s: pxr_library(%s)"
                                   % (directory.path, name))
            library = directory.libraries[i]
            if any(lib.plugin != library.plugin
                   for lib in (d.libraries[i] for d in dirs.values())):
                raise CMakeError("%s: %s is a plugin on some OSes only"
                                 % (directory.path, name))
            blocks.append(starlark_library(name, library.plugin, common,
                                           per_os))
            all_libraries.append(label(directory.path, name))
            if common["pymodule_cppfiles"]:
                python_modules.append((name, directory.path))
            if common["resource_files"]:
                resources.append("//%s:%s_resources" % (directory.path, name))

        for i, (name, _) in enumerate(directory.cpp_bins):
            libs = {}
            for os in OSES:
                labels, linkopts = resolve_libraries(
                    dirs[os].cpp_bins[i][1], interps[os],
                    "%s: pxr_cpp_bin(%s)" % (directory.path, name))
                if linkopts:
                    raise CMakeError("%s: pxr_cpp_bin(%s): link flags are "
                                     "unsupported" % (directory.path, name))
                libs[os] = _flatten(labels)
            if any(l != libs[OSES[0]] for l in libs.values()):
                raise CMakeError("%s: pxr_cpp_bin(%s) differs between OSes"
                                 % (directory.path, name))
            blocks.append(starlark_cpp_bin(name, libs[OSES[0]]))
        for name, python_file in directory.python_bins:
            blocks.append(starlark_python_bin(name, python_file))

        symbols = (
            (["pxr_configure_file"] if directory.configured else []) +
            (["pxr_cpp_bin"] if directory.cpp_bins else []) +
            (["pxr_library"] if names else []) +
            (["pxr_python_bin"] if directory.python_bins else [])
        )
        load = "load(%s)" % ", ".join(
            json.dumps(s) for s in ["//bazel:pxr_library.bzl"] + symbols)
        files[directory.path + "/BUILD.bazel"] = (
            HEADER.format(source=directory.path + "/CMakeLists.txt") +
            load + "\n\n" + "\n\n".join(blocks) + "\n")

    lines = [HEADER.format(source="pxr/**/CMakeLists.txt").rstrip("\n"),
             '"""pxr libraries, Python modules and resources in CMake build order."""',
             "", "PXR_LIBRARIES = ["]
    lines += ["    %s," % json.dumps(l) for l in all_libraries]
    lines += ["]", "", "# One entry per _<library> Python extension module.",
              "PXR_PYTHON_MODULES = ["]
    for name, path in python_modules:
        lines.append("    struct(library = %s, module = %s, package = %s),"
                     % (json.dumps(name), json.dumps(python_module_name(name)),
                        json.dumps("//" + path)))
    lines += ["]", "", "# Resources of each library, placed by //bazel/install.",
              "PXR_RESOURCES = ["]
    lines += ["    %s," % json.dumps(r) for r in resources]
    lines.append("]")
    files[LIBRARIES_BZL] = "\n".join(lines) + "\n"
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
