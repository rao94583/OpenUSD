"""Sign nested runtime Mach-O files before rules_apple signs the app."""
from pathlib import Path
import subprocess
import sys

app, = Path(sys.argv[1]).glob('*.app')
for path in sorted((app / 'Contents/Resources/Runtime').rglob('*')):
    if path.is_file() and not path.is_symlink():
        with path.open('rb') as stream:
            magic = stream.read(4)
        if magic in (b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbe'):
            result = subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(path)],
                                    capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stderr)
