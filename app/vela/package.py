"""Package Vela for other Macs: sign it with the hardened runtime, put it in
a disk image and, with an Apple Developer ID, notarize both.

    bazel run --config=release //app/vela:package -- [options]

Options:
    --identity NAME     A "Developer ID Application: ..." identity from the
                        keychain. Without one, the app is signed ad hoc: it
                        runs, but Gatekeeper blocks it until the recipient
                        chooses Open Anyway in System Settings.
    --notarize PROFILE  Notarize with the credentials saved by `xcrun
                        notarytool store-credentials PROFILE`, then staple the
                        tickets to the app and the disk image.
    --output DIR        Where to write Vela-<version>.dmg (default: dist/ in
                        the workspace).
"""
import argparse
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

MACHO = (b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbe')


def run(*args, **kwargs):
    print('+', ' '.join(str(a) for a in args), flush=True)
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def is_macho(path):
    if not path.is_file() or path.is_symlink():
        return False
    with path.open('rb') as stream:
        return stream.read(4) in MACHO


def is_executable(path):
    """A program (rather than a library or Python extension)."""
    kind = subprocess.run(['/usr/bin/file', '-b', str(path)], capture_output=True, text=True)
    return 'executable' in kind.stdout


def sign(app, identity, entitlements):
    """Signs inside-out: nested code first, then the bundle, every one with the
    hardened runtime and executables with the entitlements."""
    base = ['/usr/bin/codesign', '--force', '--options', 'runtime']
    base += ['--timestamp'] if identity != '-' else ['--timestamp=none']
    base += ['--sign', identity]
    contents = app / 'Contents'
    nested = [p for p in (contents / 'Resources').rglob('*') if is_macho(p)]
    nested += [p for p in (contents / 'MacOS').iterdir() if is_macho(p) and p.name != 'Vela']
    print(f'Signing {len(nested)} nested files', flush=True)
    for path in nested:
        extra = ['--entitlements', str(entitlements)] if is_executable(path) else []
        subprocess.run(base + extra + [str(path)], check=True, capture_output=True)
    run(*base, '--entitlements', entitlements, app)
    run('/usr/bin/codesign', '--verify', '--deep', '--strict', '--verbose=2', app)


def notarize(path, profile, staple=None):
    """Submits path and staples the ticket to staple (default: path). A zip
    can't hold a ticket, so an app is submitted zipped and stapled itself."""
    run('/usr/bin/xcrun', 'notarytool', 'submit', path, '--keychain-profile', profile, '--wait')
    run('/usr/bin/xcrun', 'stapler', 'staple', staple or path)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('archive', help=argparse.SUPPRESS)
    parser.add_argument('entitlements', help=argparse.SUPPRESS)
    parser.add_argument('--identity', default='-')
    parser.add_argument('--notarize', metavar='PROFILE')
    parser.add_argument('--output')
    args = parser.parse_args()
    if args.notarize and args.identity == '-':
        parser.error('notarizing needs a Developer ID identity (--identity)')
    workspace = Path(os.environ.get('BUILD_WORKSPACE_DIRECTORY', '.'))
    output = Path(args.output) if args.output else workspace / 'dist'
    if not output.is_absolute():
        output = workspace / output
    archive = Path(args.archive).resolve()
    entitlements = Path(args.entitlements).resolve()

    with tempfile.TemporaryDirectory(prefix='vela-package-') as temp:
        temp = Path(temp)
        staging = temp / 'Vela'
        staging.mkdir()
        run('/usr/bin/ditto', '-x', '-k', archive, staging)
        app = staging / 'Vela.app'
        info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
        version = info['CFBundleShortVersionString']
        sign(app, args.identity, entitlements)
        if args.notarize:
            # The app's own ticket, so it opens offline after being copied out.
            zipped = temp / 'Vela.zip'
            run('/usr/bin/ditto', '-c', '-k', '--keepParent', app, zipped)
            notarize(zipped, args.notarize, staple=app)
            run('/usr/bin/codesign', '--verify', '--deep', '--strict', app)

        # The disk image: the app and a shortcut to Applications to drag it to.
        (staging / 'Applications').symlink_to('/Applications')
        output.mkdir(parents=True, exist_ok=True)
        dmg = output / f'Vela-{version}.dmg'
        dmg.unlink(missing_ok=True)
        run('/usr/bin/hdiutil', 'create', '-volname', 'Vela', '-srcfolder', staging,
            '-fs', 'APFS', '-format', 'ULFO', '-ov', dmg)
        if args.identity != '-':
            run('/usr/bin/codesign', '--force', '--timestamp', '--sign', args.identity, dmg)
        if args.notarize:
            notarize(dmg, args.notarize)
            run('/usr/sbin/spctl', '--assess', '--type', 'open',
                '--context', 'context:primary-signature', '--verbose=2', dmg)
    size = dmg.stat().st_size / 1e6
    print(f'\n{dmg} ({size:.0f} MB)')
    if args.identity == '-':
        print('Signed ad hoc: recipients must choose Open Anyway in System Settings → '
              'Privacy & Security the first time. Use --identity and --notarize to avoid that.')


if __name__ == '__main__':
    sys.exit(main())
