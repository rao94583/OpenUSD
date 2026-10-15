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

def label(path, flags, frame=None):
    tag = ''.join(f.strip('-').replace(',', '+') for f in flags)
    return f'{path.stem}{"." + tag if tag else ""}'

# (scene, scene camera or None to add one, time codes, view setting flags
# that Vela and usdrecord share)
CASES = [
    ('AnimCube.usda', '/Cameras/main_cam', [1, 5, 10], []),
    ('AnimCube.usda', '/Cameras/main_cam', [5], ['--disableCameraLight']),
    # hdEmbree, a CPU path tracer, through the same AOV presentation.
    ('AnimCube.usda', '/Cameras/main_cam', [5], ['--renderer', 'Embree']),
    ('niSpheres.usda', '/main_cam', [0], []),
    ('testPreviewSurfaceTexture.usda', None, [0], []),
    ('testPreviewSurfaceNormalMap.usda', None, [0], []),
    ('testPreviewSurfaceDomeLight.usda', None, [0], []),
    ('testPreviewSurfaceDomeLight.usda', None, [0], ['--enableDomeLightVisibility']),
    ('subdivMesh.usda', None, [0], ['--complexity', 'veryhigh']),
    ('purposes.usda', '/Camera', [0], []),
    ('purposes.usda', '/Camera', [0], ['--purposes', 'render,guide']),
    ('purposes.usda', '/Camera', [0], ['--purposes', 'proxy,render,guide']),
    ('testPreviewSurfaceTexture.usda', None, [0], ['--colorCorrectionMode', 'disabled']),
    ('testPreviewSurfaceTexture.usda', None, [0], ['--colorCorrectionMode', 'openColorIO']),
]

# OpenColorIO's built-in default config (ACES), so results do not depend on
# the machine's $OCIO.
os.environ['OCIO'] = 'ocio://default'

# One prim of each purpose, side by side.
PURPOSES = """#usda 1.0
(
    upAxis = "Y"
)
def Xform "World" {
    def Cube "Default" {
        double3 xformOp:translate = (-4.5, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Sphere "Proxy" {
        uniform token purpose = "proxy"
        double3 xformOp:translate = (-1.5, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Cone "Render" {
        uniform token purpose = "render"
        double3 xformOp:translate = (1.5, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Cylinder "Guide" {
        uniform token purpose = "guide"
        double3 xformOp:translate = (4.5, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
def Camera "Camera" {
    float2 clippingRange = (1, 100)
    double3 xformOp:translate = (0, 0, 30)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
"""

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
    purposes = temp / 'purposes.usda'
    purposes.write_text(PURPOSES)
    scenes.append(purposes)
    for name, camera, frames, flags in CASES:
        path = scene(name)
        if camera is None:
            camera = '/SnapshotCamera'
            wrapper = temp / (path.stem + '.camera.usda')
            if wrapper.exists():
                path = wrapper
            else:
                run(runtime / 'python/bin/python3', '-I', '-B',
                    Path(__file__).with_name('snapshot_camera.py'),
                    runtime, path, wrapper)
                path = wrapper
        for frame in frames:
            stem = label(path, flags, frame)
            expected = temp / f'{stem}.usdrecord.{frame}.png'
            actual = temp / f'{stem}.vela.{frame}.png'
            run(usdrecord, path, temp / f'{stem}.usdrecord.#.png',
                '--camera', camera, '--frames', frame, *flags)
            run(vela, '--snapshot', path, actual, '--camera', camera,
                '--frame', frame, *flags)
            matched, report = compare(actual, expected)
            print(f'{name} {" ".join(flags)} @ {frame}: {report.splitlines()[-1]}')
            if not matched:
                failures.append(f'{name} {" ".join(flags)} @ {frame}:\n{report}')
    # The viewport's route through a scene camera (fitted, then by
    # matrices) matches usdrecord's (by camera path), at every frame of an
    # animated camera.
    for frame in [1, 5, 10]:
        actual = temp / f'AnimCube.viewCamera.vela.{frame}.png'
        run(vela, '--snapshot', scene('AnimCube.usda'), actual,
            '--viewCamera', '/Cameras/main_cam', '--frame', frame)
        matched, report = compare(actual, temp / f'AnimCube.usdrecord.{frame}.png')
        print(f'AnimCube.usda --viewCamera @ {frame}: {report.splitlines()[-1]}')
        if not matched:
            failures.append(f'AnimCube.usda --viewCamera @ {frame}:\n{report}')
    # A wider image than the camera's aspect shows the whole aperture with
    # more around it, so it is not the same image.
    run(vela, '--snapshot', scene('AnimCube.usda'), temp / 'AnimCube.wide.png',
        '--viewCamera', '/Cameras/main_cam', '--frame', 5, '--height', 400)
    assert not compare(temp / 'AnimCube.wide.png', temp / 'AnimCube.vela.5.png')[0]

    # The comparisons can fail: the cube changes between frames 1 and 10,
    # and each purpose and lighting setting changes the image.
    for a, b in [('AnimCube.vela.1', 'AnimCube.usdrecord.10'),
                 ('purposes.vela.0', 'purposes.purposesrender+guide.vela.0'),
                 ('purposes.purposesrender+guide.vela.0',
                  'purposes.purposesproxy+render+guide.vela.0'),
                 ('AnimCube.vela.5', 'AnimCube.disableCameraLight.vela.5'),
                 ('AnimCube.vela.5', 'AnimCube.rendererEmbree.vela.5'),
                 ('testPreviewSurfaceTexture.camera.vela.0',
                  'testPreviewSurfaceTexture.camera.colorCorrectionModeopenColorIO.vela.0'),
                 ('testPreviewSurfaceTexture.camera.vela.0',
                  'testPreviewSurfaceTexture.camera.colorCorrectionModedisabled.vela.0')]:
        assert not compare(temp / f'{a}.png', temp / f'{b}.png')[0], \
            f'idiff did not detect a change between {a} and {b}'
    # Draw modes have no usdrecord equivalent: each must differ from the
    # default (shaded smooth) and from the others.
    modes = ['points', 'wireframe', 'wireframeOnSurface', 'shadedFlat', 'geomOnly']
    cube = scene('AnimCube.usda')
    for mode in modes:
        run(vela, '--snapshot', cube, temp / f'mode.{mode}.png',
            '--camera', '/Cameras/main_cam', '--frame', 5, '--drawMode', mode)
    for i, mode in enumerate(modes):
        assert not compare(temp / f'mode.{mode}.png', temp / 'AnimCube.vela.5.png')[0], mode
        for other in modes[i + 1:]:
            assert not compare(temp / f'mode.{mode}.png', temp / f'mode.{other}.png')[0], \
                (mode, other)
    print(f'{len(modes)} draw modes differ from shaded and from each other')
    # AOVs have no usdrecord equivalent either: Storm's depth and primId,
    # visualized, differ from color and from each other.
    aovs = ['depth', 'primId']
    for aov in aovs:
        run(vela, '--snapshot', cube, temp / f'aov.{aov}.png',
            '--camera', '/Cameras/main_cam', '--frame', 5, '--aov', aov)
        assert not compare(temp / f'aov.{aov}.png', temp / 'AnimCube.vela.5.png')[0], aov
    assert not compare(temp / 'aov.depth.png', temp / 'aov.primId.png')[0]
    print(f'{len(aovs)} AOVs differ from color and from each other')
    outputs = os.environ.get('TEST_UNDECLARED_OUTPUTS_DIR')
    if outputs:
        for image in temp.glob('*.png'):
            (Path(outputs) / image.name).write_bytes(image.read_bytes())
    if failures:
        sys.exit('\n'.join(failures))
