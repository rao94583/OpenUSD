"""Exercise an extracted, signed bundle away from the build, as on another
Mac: a clean environment and an empty home folder. The signature must stay
valid after the app has run (Python must not write into the bundle), every
library must load from the bundle or the system, and nothing the app never
uses may ship.

    test_bundle.py ARCHIVE SMOKE_SCRIPT SCENE
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

archive, smoke, scene = (Path(a).resolve() for a in sys.argv[1:4])


def verify(app):
    subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(app)], check=True)


with tempfile.TemporaryDirectory(prefix='Vela relocation ') as temp:
    temp = Path(temp)
    subprocess.run(['/usr/bin/ditto', '-x', '-k', str(archive), str(temp)], check=True)
    app, = temp.glob('*.app')
    contents = app / 'Contents'
    runtime = contents / 'Resources/Runtime'
    verify(app)
    assert not any(p.is_symlink() for p in app.rglob('*')), 'Bundle contains external symlinks'

    # What ships.
    assert not (contents / 'Frameworks').exists(), 'Duplicate libraries in Frameworks'
    assert not list(app.rglob('*.dSYM')), 'Debug symbols in the bundle'
    assert not (runtime / 'site-packages/PyObjCTest').exists(), "PyObjC's tests in the bundle"
    assert not list(app.rglob('__pycache__')), 'Bytecode in the bundle'
    assert (contents / 'Resources/Vela.icns').stat().st_size > 10000, 'No app icon'
    plist = subprocess.run(['/usr/bin/plutil', '-extract', 'CFBundleIconFile', 'raw',
                            str(contents / 'Info.plist')], capture_output=True, text=True)
    assert plist.stdout.strip() == 'Vela', plist
    # Gatekeeper treats only an APPL bundle as an app.
    package = subprocess.run(['/usr/bin/plutil', '-extract', 'CFBundlePackageType', 'raw',
                              str(contents / 'Info.plist')], capture_output=True, text=True)
    assert package.stdout.strip() == 'APPL', package
    assert (contents / 'PkgInfo').read_bytes().startswith(b'APPL'), 'PkgInfo is not APPL'

    # A clean environment, as a new user's: no Python, USD or Homebrew.
    home = temp / 'home'
    home.mkdir()
    env = {'HOME': str(home), 'PATH': '/usr/bin:/bin:/usr/sbin:/sbin', 'TMPDIR': str(temp),
           'LANG': 'en_US.UTF-8'}
    subprocess.run([str(runtime / 'python/bin/python3'), '-I', '-B', str(smoke), str(runtime)],
                   cwd=temp, env=env, check=True)
    loaded = subprocess.run(
        [str(contents / 'MacOS/Vela'), '--check-python', str(scene)], cwd=temp,
        env=dict(env, DYLD_PRINT_LIBRARIES='1'), capture_output=True, text=True)
    assert loaded.returncode == 0, loaded.stdout + loaded.stderr
    assert 'Python checks passed' in loaded.stdout, loaded.stdout
    libraries = re.findall(r'^dyld\[\d+\]: <[^>]*> (.*)$', loaded.stderr, re.M)
    assert len(libraries) > 100, loaded.stderr[:2000]
    outside = [l for l in libraries
               if not l.startswith((str(app), '/usr/lib/', '/System/'))
               and not l.startswith(str(app.resolve()))]
    assert not outside, outside
    for library in ('usd/lib/libusd_ms.dylib', 'python/lib/libpython3.11.dylib'):
        assert sum(l.endswith(library) for l in libraries) == 1, library
    subprocess.run([str(contents / 'MacOS/Vela'), '--check-document', str(scene)], cwd=temp,
                   env=env, check=True, capture_output=True)

    # Running wrote nothing into the bundle: bytecode went to the cache.
    verify(app)
    assert not list(app.rglob('__pycache__')), 'Python wrote bytecode into the bundle'
    assert list((home / 'Library/Caches/Vela/Python').rglob('*.pyc')), 'No bytecode cache'
    print('Bundle checks passed')
