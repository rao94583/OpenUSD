"""Runs the Jinja2-based schema tools: usdInitSchema and usdGenSchema."""

import os
import subprocess
import sys
import tempfile
import unittest

TOOLS = {}

SCHEMA = """#usda 1.0
(
    subLayers = [
        @usd/schema.usda@
    ]
)

over "GLOBAL" (
    customData = {
        string libraryName = "mySchema"
        string libraryPath = "pxr/usd/mySchema"
    }
)
{
}

class Gadget "Gadget" (
    inherits = </Typed>
    doc = "A test schema."
)
{
    double size = 1.0 (
        doc = "Size of the gadget."
    )
}
"""


def run(tool, *args, cwd):
    result = subprocess.run([TOOLS[tool]] + list(args), cwd=cwd,
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise AssertionError("%s failed:\n%s%s"
                             % (tool, result.stdout, result.stderr))
    return result


class TestSchemaTools(unittest.TestCase):
    def test_init_and_generate(self):
        with tempfile.TemporaryDirectory() as tmp:
            run("usdInitSchema", ".", "pxr/usd/mySchema", "pxr/usd", cwd=tmp)
            for name in ("CMakeLists.txt", "__init__.py", "module.cpp",
                         "schema.usda"):
                self.assertTrue(os.path.exists(os.path.join(tmp, name)), name)

            with open(os.path.join(tmp, "schema.usda"), "w") as f:
                f.write(SCHEMA)
            run("usdGenSchema", "schema.usda", ".", cwd=tmp)
            with open(os.path.join(tmp, "gadget.h")) as f:
                self.assertIn("class MySchemaGadget", f.read())
            with open(os.path.join(tmp, "generatedSchema.usda")) as f:
                self.assertIn("double size = 1", f.read())
            with open(os.path.join(tmp, "generatedSchema.classes.txt")) as f:
                self.assertIn("gadget", f.read().split())

            # Regenerating must reproduce the files just written.
            run("usdGenSchema", "--validate", "schema.usda", ".", cwd=tmp)

    def test_usdgenschemafromsdr_starts(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = run("usdgenschemafromsdr", "--help", cwd=tmp).stdout
            self.assertIn("usage:", out)


if __name__ == "__main__":
    for path in sys.argv[1:4]:
        TOOLS[os.path.basename(path)] = os.path.abspath(path)
    del sys.argv[1:4]
    unittest.main()
