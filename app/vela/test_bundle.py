"""Exercise an extracted, signed bundle away from the build and runfiles."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='Vela relocation ') as temp:
    subprocess.run(['/usr/bin/ditto', '-x', '-k', str(Path(sys.argv[1]).resolve()), temp], check=True)
    app, = Path(temp).glob('*.app')
    subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(app)], check=True)
    assert not any(p.is_symlink() for p in app.rglob('*')), 'Bundle contains external symlinks'
    runtime = app / 'Contents/Resources/Runtime'
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('PYTHON', 'PXR_', 'USD_', 'DYLD_', 'RUNFILES', 'TEST_'))}
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    subprocess.run([str(runtime / 'python/bin/python3'), '-I', '-B',
                    str(runtime / 'bundle_smoke.py')], cwd=temp, env=env, check=True)
    subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(app)], check=True)
