"""Checks the wheel works outside Bazel's runfiles.

Unpacks the wheel into a temporary directory and runs Pixar's install test
for the usd-core wheel (build_scripts/pypi/test.py) plus a MaterialX check
in a separate, isolated interpreter that sees only the unpacked wheel. This
catches broken rpaths and resource layouts.
"""

import os
import subprocess
import sys
import tempfile
import zipfile

CHECK = """
from pxr import Sdr
# Discovered from the MaterialX data library in usdMtlx's resources.
assert 'ND_standard_surface_surfaceshader' in \\
    Sdr.Registry().GetShaderNodeIdentifiers()
"""

def main():
    wheel, install_test = sys.argv[1:3]
    root = tempfile.mkdtemp(dir=os.environ.get("TEST_TMPDIR"))
    site = os.path.join(root, "site-packages")
    with zipfile.ZipFile(wheel) as z:
        z.extractall(site)
    test_dir = os.path.join(root, "test")
    os.mkdir(test_dir)
    with open(install_test) as src, \
            open(os.path.join(test_dir, "test_usdinstall.py"), "w") as dst:
        dst.write(src.read())

    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("PYTHON", "RUNFILES"))}
    env["PYTHONPATH"] = site
    for args in (["-m", "unittest", "-v", "test_usdinstall"],
                 ["-c", CHECK]):
        # -s: ignore user site-packages; -P is not in 3.11, so cwd is a
        # directory with only the test.
        subprocess.check_call([sys.executable, "-s"] + args,
                              cwd=test_dir, env=env)


if __name__ == "__main__":
    main()
