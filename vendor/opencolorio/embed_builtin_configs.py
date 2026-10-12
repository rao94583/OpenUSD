#!/usr/bin/env python3
"""Embeds OpenColorIO's built-in configs, as src/OpenColorIO/CMakeLists.txt
does: every builtinconfigs/configs/<name>.ocio becomes the variable <name>
holding its bytes as "0x..,0x..,...,0x00", substituted (@ONLY) into a
builtinconfigs/*.cpp.in template.

Usage: embed_builtin_configs.py <template> <output> <config.ocio>...
"""

import os
import re
import sys


def main():
    template, output, configs = sys.argv[1], sys.argv[2], sys.argv[3:]
    values = {}
    for path in configs:
        name = re.match(r"^(.*)\.ocio$", os.path.basename(path)).group(1)
        with open(path, "rb") as f:
            data = f.read()
        values[name] = ",".join("0x%02x" % b for b in data) + ",0x00"
    with open(template) as f:
        text = f.read()
    text = re.sub(r"@([A-Za-z0-9_.+-]+)@",
                  lambda m: values.get(m.group(1), ""), text)
    with open(output, "w") as f:
        f.write(text)


if __name__ == "__main__":
    main()
