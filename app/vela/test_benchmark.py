"""Benchmark the viewport on a generated large scene (see make_large_scene.py).

Manual, since frame rates depend on the machine: fails below 24 fps.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

archive = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='Vela benchmark ') as temp:
    temp = Path(temp)
    subprocess.run(['/usr/bin/ditto', '-x', '-k', str(archive), str(temp)], check=True)
    app, = temp.glob('*.app')
    runtime = app / 'Contents/Resources/Runtime'
    scene = temp / 'large.usdc'
    subprocess.run([str(runtime / 'python/bin/python3'), '-I', '-B',
                    str(Path(__file__).with_name('make_large_scene.py')),
                    str(runtime), str(scene)], check=True)
    result = subprocess.run([str(app / 'Contents/MacOS/Vela'), '--benchmark',
                             str(scene), '--width', '1920', '--height', '1080'],
                            check=True, capture_output=True, text=True)
    print(result.stdout)
    fps = float(re.search(r'([\d.]+) fps', result.stdout).group(1))
    assert fps >= 24, f'{fps} fps is not interactive'
