#!/usr/bin/env python3
"""CMake's configure_file() for vendored dependencies' config headers.

Usage: cmake_configure_file.py <template> <output> <values.json> [--at-only]

Implements configure_file's substitutions: "#cmakedefine VAR ..." becomes
"#define VAR ..." when VAR is true and "/* #undef VAR */" otherwise;
"#cmakedefine01 VAR" becomes "#define VAR 1" or "0"; @VAR@ and (unless
--at-only, i.e. @ONLY) ${VAR} are replaced by VAR's value, or "" when
undefined. Truthiness follows CMake's if(<variable>).
"""

import json
import re
import sys

_FALSE = {"", "0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND"}


def _true(values, name):
    value = values.get(name)
    if value is None:
        return False
    upper = value.upper()
    return upper not in _FALSE and not upper.endswith("-NOTFOUND")


def configure(text, values, at_only):
    def substitute(line):
        line = re.sub(r"@([A-Za-z0-9_.+-]+)@",
                      lambda m: values.get(m.group(1), ""), line)
        if not at_only:
            line = re.sub(r"\$\{([A-Za-z0-9_.+-]+)\}",
                          lambda m: values.get(m.group(1), ""), line)
        return line

    out = []
    for line in text.splitlines(True):
        m = re.match(r"^([ \t]*)#([ \t]*)cmakedefine(01)?[ \t]+"
                     r"([A-Za-z0-9_]+)(.*?)(\r?\n)?$", line)
        if not m:
            out.append(substitute(line))
            continue
        indent, space, zero_one, name, rest, newline = m.groups()
        newline = newline or ""
        if zero_one:
            out.append("%s#%sdefine %s %d%s" % (
                indent, space, name, 1 if _true(values, name) else 0,
                newline))
        elif _true(values, name):
            out.append(substitute("%s#%sdefine %s%s%s" % (
                indent, space, name, rest, newline)))
        else:
            out.append("%s/* #undef %s */%s" % (indent, name, newline))
    return "".join(out)


def main():
    template, output, values_file = sys.argv[1:4]
    at_only = "--at-only" in sys.argv[4:]
    with open(values_file) as f:
        values = json.load(f)
    with open(template) as f:
        text = f.read()
    with open(output, "w") as f:
        f.write(configure(text, values, at_only))


if __name__ == "__main__":
    main()
