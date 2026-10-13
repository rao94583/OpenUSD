"""Compare Vela's snapshots with usdrecord's images of the same views.

Both render with Storm on Metal from the same libraries, so most images match
exactly. Storm is not deterministic at the edges of some scenes (usdrecord
differs from itself on up to 0.07% of niSpheres' pixels), so idiff allows
0.2% of pixels to differ by more than one 8-bit step.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

archive, usdrecord, idiff = (Path(p).resolve() for p in sys.argv[1:4])
scenes = [Path(p).resolve() for p in sys.argv[4:]]

def scene(name):
    found, = (s for s in scenes if s.name == name)
    return found

# (scene, scene camera or None to add one, time codes)
CASES = [
    ('AnimCube.usda', '/Cameras/main_cam', [1, 5, 10]),
    ('niSpheres.usda', '/main_cam', [0]),
    ('testPreviewSurfaceTexture.usda', None, [0]),
    ('testPreviewSurfaceNormalMap.usda', None, [0]),
    ('testPreviewSurfaceDomeLight.usda', None, [0]),
]

def run(*args, **kwargs):
    result = subprocess.run([str(a) for a in args], text=True,
                            capture_output=True, **kwargs)
    if result.returncode:
        sys.exit(f'{args[0]} failed ({result.returncode}):\n'
                 f'{result.stdout}\n{result.stderr}')
    return result

# idiff exits with 0 for a match, 1 for a warning (any difference) and 2 or
# more for a failure or an error.
def compare(image, baseline):
    result = subprocess.run([str(idiff), '-fail', '0.004', '-failpercent', '0.2',
                             str(image), str(baseline)],
                            capture_output=True, text=True)
    return result.returncode <= 1, result.stdout + result.stderr

with tempfile.TemporaryDirectory(prefix='Vela snapshots ') as temp:
    temp = Path(temp)
    subprocess.run(['/usr/bin/ditto', '-x', '-k', str(archive), str(temp)], check=True)
    app, = temp.glob('*.app')
    vela = app / 'Contents/MacOS/Vela'
    runtime = app / 'Contents/Resources/Runtime'
    failures = []
    for name, camera, frames in CASES:
        path = scene(name)
        if camera is None:
            camera = '/SnapshotCamera'
            wrapper = temp / (path.stem + '.camera.usda')
            run(runtime / 'python/bin/python3', '-I', '-B',
                Path(__file__).with_name('snapshot_camera.py'),
                runtime, path, wrapper)
            path = wrapper
        for frame in frames:
            expected = temp / f'{path.stem}.usdrecord.{frame}.png'
            actual = temp / f'{path.stem}.vela.{frame}.png'
            run(usdrecord, path, temp / f'{path.stem}.usdrecord.#.png',
                '--camera', camera, '--frames', frame)
            run(vela, '--snapshot', path, actual, '--camera', camera,
                '--frame', frame)
            matched, report = compare(actual, expected)
            print(f'{name} @ {frame}: {report.splitlines()[-1]}')
            if not matched:
                failures.append(f'{name} @ {frame}:\n{report}')
    # The comparison can fail: the cube changes between frames 1 and 10.
    assert not compare(temp / 'AnimCube.vela.1.png',
                       temp / 'AnimCube.usdrecord.10.png')[0], \
        'idiff did not detect a changed image'
    outputs = os.environ.get('TEST_UNDECLARED_OUTPUTS_DIR')
    if outputs:
        for image in temp.glob('*.png'):
            (Path(outputs) / image.name).write_bytes(image.read_bytes())
    if failures:
        sys.exit('\n'.join(failures))
