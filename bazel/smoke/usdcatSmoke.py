"""Runs usdcat (a C++ program) on a .usda file, converting it to .usdc."""

import os
import subprocess
import sys
import tempfile
import unittest

USDCAT = None

HELLO = """#usda 1.0

def Xform "hello"
{
    def Sphere "world"
    {
        double radius = 2
    }
}
"""


class TestUsdcat(unittest.TestCase):
    def test_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            usda = os.path.join(tmp, "hello.usda")
            usdc = os.path.join(tmp, "hello.usdc")
            with open(usda, "w") as f:
                f.write(HELLO)
            out = subprocess.run([USDCAT, usda], check=True,
                                 capture_output=True, text=True).stdout
            self.assertEqual(out.strip(), HELLO.strip())
            subprocess.run([USDCAT, usda, "-o", usdc], check=True)
            with open(usdc, "rb") as f:
                self.assertEqual(f.read(8), b"PXR-USDC")
            out = subprocess.run([USDCAT, usdc], check=True,
                                 capture_output=True, text=True).stdout
            self.assertIn("double radius = 2", out)


if __name__ == "__main__":
    USDCAT = os.path.abspath(sys.argv.pop(1))
    unittest.main()
