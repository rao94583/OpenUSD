"""usdview's command-line options, through a real launch. --norender keeps
the test independent of the display."""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='Vela launch ') as temp:
    temp = Path(temp)
    subprocess.run(['/usr/bin/ditto', '-x', '-k', sys.argv[1], str(temp)], check=True)
    vela, = temp.glob('*.app/Contents/MacOS/Vela')
    scene = temp / 'scene.usda'
    scene.write_text('#usda 1.0\n(startTimeCode = 1\nendTimeCode = 10)\n'
                     'def Xform "World" {\n    def Cube "Cube" {}\n}\n'
                     'def Camera "main_cam" {}\n')

    # --defaultsettings: never read or write the user's saved settings.
    def launch(*args):
        return subprocess.run([vela, scene, '--norender', '--quitAfterStartup',
                               '--defaultsettings', *args],
                              capture_output=True, text=True, timeout=120)

    result = launch('--timing', '--select', '/World/Cube', '--ff', '2', '--lf', '5',
                    '--cf', '3', '--complexity', 'high', '--mask', '/World',
                    '--unloaded', '--numThreads', '2', '--viewportSize', '320x240')
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'Opened scene.usda in' in result.stdout, result.stdout
    assert 'Started' in result.stdout, result.stdout
    assert 'Vela:' not in result.stderr, result.stderr  # no warnings

    # Missing prims and cameras are warnings, as in usdview.
    result = launch('--select', '/Nope', '--camera', 'nothere')
    assert result.returncode == 0, result.stderr
    assert 'No prim at /Nope' in result.stderr and 'No camera nothere' in result.stderr

    # Unsupported and unknown options fail with usage.
    for bad in (['--memstats'], ['--bogus'], ['--complexity', 'max']):
        result = launch(*bad)
        assert result.returncode == 2 and 'Usage: Vela' in result.stderr, (bad, result.stderr)

    # A missing file fails.
    result = subprocess.run([vela, temp / 'missing.usda', '--norender', '--quitAfterStartup',
                             '--defaultsettings'],
                            capture_output=True, text=True, timeout=120)
    assert result.returncode != 0 or 'missing.usda' in result.stderr, result
    print('Launch options checks passed')
