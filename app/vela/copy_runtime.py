"""Materialize symlinks so the app has no dependency on Bazel's runfiles."""
import json
from pathlib import Path
import shutil
import sys

for source, relative in json.loads(Path(sys.argv[1]).read_text()):
    dest = Path(sys.argv[2]) / relative
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, dest)
