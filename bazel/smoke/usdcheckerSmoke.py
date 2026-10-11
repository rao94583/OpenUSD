"""Runs usdchecker, whose validators are registered through plugInfo.json."""

import os
import subprocess
import sys
import tempfile
import unittest

USDCHECKER = None

GOOD = """#usda 1.0
(
    defaultPrim = "hello"
    metersPerUnit = 0.01
    upAxis = "Y"
)

def Xform "hello"
{
}
"""

# No defaultPrim, metersPerUnit or upAxis.
BAD = """#usda 1.0

def Xform "hello"
{
}
"""


class TestUsdchecker(unittest.TestCase):
    def check(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "stage.usda")
            with open(path, "w") as f:
                f.write(text)
            return subprocess.run([USDCHECKER, path],
                                  capture_output=True, text=True)

    def test_good(self):
        result = self.check(GOOD)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_bad(self):
        result = self.check(BAD)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("usdGeomValidators:StageMetadataChecker", result.stdout)
        self.assertIn("MissingDefaultPrim", result.stdout)


if __name__ == "__main__":
    USDCHECKER = os.path.abspath(sys.argv.pop(1))
    unittest.main()
