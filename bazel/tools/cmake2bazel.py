#!/usr/bin/env python3
"""Translate pxr_library() calls in a CMakeLists.txt into pxr_library() Starlark.

Early version: prints the translated call for one CMakeLists.txt. Variable
references such as ${ARCH_PLATFORM_CPPFILES} are not evaluated; they are
reported on stderr and must be handled by hand (usually with select()).

Usage: cmake2bazel.py path/to/CMakeLists.txt [--labels name=label ...]
"""

import argparse
import re
import sys

# CMake keywords of pxr_library() that take lists, in output order.
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
IGNORED_KEYWORDS = {"INCLUDE_DIRS", "DISABLE_PRECOMPILED_HEADERS", "TYPE",
                    "PRECOMPILED_HEADER_NAME"}

# CMake target name -> Bazel label, for libraries outside pxr.
EXTERNAL_LABELS = {
    "TBB::tbb": "//vendor/onetbb:tbb",
    # Python extensions resolve libpython symbols at load time, so only the
    # headers are needed; executables link libpython separately.
    "${PYTHON_LIBRARIES}": "@rules_python//python/cc:current_py_cc_headers",
}


def strip_comments(text):
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def find_calls(text, function):
    """Yields the argument token lists of each `function(...)` call."""
    for m in re.finditer(r"^\s*%s\s*\(" % re.escape(function), text, re.M):
        depth, i = 1, m.end()
        while depth:
            depth += {"(": 1, ")": -1}.get(text[i], 0)
            i += 1
        yield text[m.end():i - 1].split()


def parse_library(tokens):
    name, args, current = tokens[0], {}, None
    for tok in tokens[1:]:
        if tok in LIST_KEYWORDS or tok in IGNORED_KEYWORDS:
            current = tok
            args.setdefault(tok, [])
        elif current is None:
            raise ValueError("unexpected token %r in pxr_library(%s)"
                             % (tok, name))
        else:
            args[current].append(tok)
    return name, args


def label_for(lib, package_labels):
    if lib in EXTERNAL_LABELS:
        return EXTERNAL_LABELS[lib]
    return package_labels.get(lib, ":" + lib)


def to_starlark(name, args, package_labels, warn):
    out = ["pxr_library(", '    name = "%s",' % name]
    for kw in LIST_KEYWORDS:
        values = []
        for v in args.get(kw, []):
            if v.startswith("${") and v not in EXTERNAL_LABELS:
                warn("pxr_library(%s) %s: unevaluated %s" % (name, kw, v))
                continue
            values.append(label_for(v, package_labels)
                          if kw == "LIBRARIES" else v)
        if not values:
            continue
        out.append("    %s = [" % kw.lower())
        out.extend('        "%s",' % v for v in values)
        out.append("    ],")
    out.append(")")
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("cmakelists")
    parser.add_argument("--labels", nargs="*", default=[],
                        help="name=label mappings for pxr libraries")
    opts = parser.parse_args()

    package_labels = dict(m.split("=", 1) for m in opts.labels)
    with open(opts.cmakelists) as f:
        text = strip_comments(f.read())

    def warn(msg):
        print("cmake2bazel: " + msg, file=sys.stderr)

    calls = [to_starlark(*parse_library(t), package_labels, warn)
             for t in find_calls(text, "pxr_library")]
    print("\n\n".join(calls))


if __name__ == "__main__":
    main()
