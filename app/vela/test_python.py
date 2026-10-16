"""Phase 6: Vela --check-python runs the embedded interpreter against a
document, from the extracted (signed) bundle."""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='Vela python ') as temp:
    temp = Path(temp)
    subprocess.run(['/usr/bin/ditto', '-x', '-k', sys.argv[1], str(temp)], check=True)
    vela, = temp.glob('*.app/Contents/MacOS/Vela')
    scene = temp / 'scene.usda'
    scene.write_text('#usda 1.0\ndef Xform "World" {\n    def Cube "Cube" {\n    }\n}\n')
    result = subprocess.run([vela, '--check-python', scene], capture_output=True, text=True,
                            timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout, end='')
