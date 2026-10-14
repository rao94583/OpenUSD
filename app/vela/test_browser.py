"""Construct browsing controls and exercise selection without showing a window,
on a hand-written scene and on real test scenes (passed as arguments)."""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='Vela browsing ') as temp:
    subprocess.run(['/usr/bin/ditto', '-x', '-k', sys.argv[1], temp], check=True)
    vela, = Path(temp).glob('*.app/Contents/MacOS/Vela')
    scene = Path(temp) / 'browser.usda'
    scene.write_text('''#usda 1.0
    def Xform "World" {
        def Cube "Cube" {
            custom int[] values = [1, 2, 3]
            double size.timeSamples = {1: 2, 3: 8}
        }
    }
    def Xform "Instance" (
        instanceable = true
        prepend references = </World>
    ) {}
    ''')
    result = subprocess.check_output([vela, '--check-browser', scene], text=True)
    assert 'Browser selection and inspector checks passed' in result, result
    empty = Path(temp) / 'empty.usda'
    empty.write_text('#usda 1.0\n')
    subprocess.run([vela, '--check-browser', empty], check=True)
    for scene in sys.argv[2:]:
        result = subprocess.run([vela, '--check-browser', Path(scene).resolve()],
                                text=True, capture_output=True)
        assert result.returncode == 0, f'{scene}: {result.stdout}{result.stderr}'
        print(f'{Path(scene).name}: passed')
