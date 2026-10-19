"""Finish the app bundle before rules_apple signs it: remove what the app
never uses, then sign the nested Mach-O files.

Removed: rules_apple's copies of linked libraries in Contents/Frameworks
(the app loads the ones in Runtime, found first on its rpath; two copies
of libusd_ms would mean two USD type registries), debug symbol bundles,
and PyObjC's test suite.
"""
from pathlib import Path
import shutil
import subprocess
import sys

app, = Path(sys.argv[1]).glob('*.app')
contents = app / 'Contents'
runtime = contents / 'Resources/Runtime'

frameworks = contents / 'Frameworks'
if frameworks.is_dir():
    for library in frameworks.iterdir():
        if not list(runtime.glob('*/lib/' + library.name)):
            raise RuntimeError(f'{library.name} is in Frameworks but not in the runtime')
        library.unlink()
    frameworks.rmdir()
for path in sorted(runtime.rglob('*.dSYM'), reverse=True):
    shutil.rmtree(path)
shutil.rmtree(runtime / 'site-packages/PyObjCTest', ignore_errors=True)

helpers = [contents / 'MacOS/vela-mcp']
for path in sorted(runtime.rglob('*')) + helpers:
    if path.is_file() and not path.is_symlink():
        with path.open('rb') as stream:
            magic = stream.read(4)
        if magic in (b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbe'):
            result = subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(path)],
                                    capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stderr)
