"""Saving through the AppKit document: edited state, Save and Save As."""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='Vela saving ') as temp:
    temp = Path(temp)
    subprocess.run(['/usr/bin/ditto', '-x', '-k', sys.argv[1], str(temp)], check=True)
    vela, = temp.glob('*.app/Contents/MacOS/Vela')
    scene = temp / 'scene.usda'
    scene.write_text('#usda 1.0\ndef Xform "World" {\n    def Cube "Cube" {}\n}\n')
    result = subprocess.run([vela, '--check-saving', scene, '/World/Cube'],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    # The saved file has the edit.
    text = scene.read_text()
    assert 'token visibility = "invisible"' in text, text
    print(result.stdout.strip())
