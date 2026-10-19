"""Python packages for plugins and scripts, in a launched Vela, offline: pip
installs from a local index of wheels made here (VELA_PIP_ARGS), into a
private packages folder (VELA_PYTHON_PACKAGES), with the confirmation
answered (VELA_PACKAGES_AUTO_APPROVE). Checks a plugin that waits for its
packages at launch and loads once they're installed, scripts installing
packages, refusals, conflicts, the bundle's packages kept, and upgrades."""
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import time
import zipfile


def make_wheel(folder, name, version, code, requires=()):
    """A pure-Python wheel with one module."""
    dist = '%s-%s.dist-info' % (name, version)
    files = {
        '%s/__init__.py' % name: code,
        dist + '/METADATA': 'Metadata-Version: 2.1\nName: %s\nVersion: %s\n%s' % (
            name, version, ''.join('Requires-Dist: %s\n' % r for r in requires)),
        dist + '/WHEEL': 'Wheel-Version: 1.0\nGenerator: vela-test\nRoot-Is-Purelib: true\n'
                         'Tag: py3-none-any\n',
    }
    record = []
    for path, text in files.items():
        digest = base64.urlsafe_b64encode(hashlib.sha256(text.encode()).digest()).rstrip(b'=')
        record.append('%s,sha256=%s,%d' % (path, digest.decode(), len(text.encode())))
    record.append(dist + '/RECORD,,')
    files[dist + '/RECORD'] = '\n'.join(record) + '\n'
    with zipfile.ZipFile(folder / ('%s-%s-py3-none-any.whl' % (name, version)), 'w') as wheel:
        for path, text in files.items():
            wheel.writestr(path, text)


NEEDY = '''
from pxr import Tf
import vela
import velatestpkg

def hello(document):
    print('velatestpkg', velatestpkg.VALUE)

class Needy(vela.PluginContainer):
    def registerPlugins(self, registry, plugCtx):
        self._hello = registry.registerCommandPlugin('needy.hello', 'Hello', hello)
    def configureView(self, registry, builder):
        builder.findOrCreateMenu('Tools').addItem(self._hello)

Tf.Type.Define(Needy)
'''


def write_needy(plugins, requirement):
    package = plugins / 'needy'
    package.mkdir(parents=True, exist_ok=True)
    (package / 'plugInfo.json').write_text(json.dumps({'Plugins': [{
        'Type': 'python', 'Name': 'needy', 'Info': {
            'PythonRequires': [requirement],
            'Types': {'needy.Needy': {'bases': ['vela.PluginContainer']}}}}]}))
    (package / '__init__.py').write_text(textwrap.dedent(NEEDY))


def main():
    temp = Path(tempfile.mkdtemp(prefix='Vela packages '))
    socket_dir = Path(tempfile.mkdtemp(prefix='vela-', dir='/tmp'))
    app = helper = None
    try:
        subprocess.run(['/usr/bin/ditto', '-x', '-k', sys.argv[1], str(temp)], check=True)
        vela, = temp.glob('*.app/Contents/MacOS/Vela')
        index = temp / 'index'
        index.mkdir()
        make_wheel(index, 'velatestpkg', '1.0', "VALUE = '1.0'\n")
        make_wheel(index, 'velatestpkg', '2.0', "VALUE = '2.0'\n")
        make_wheel(index, 'velatestdep', '1.0', 'from velatestpkg import VALUE\n',
                   requires=['velatestpkg>=1'])
        plugins = temp / 'Plugins'
        write_needy(plugins, 'velatestpkg>=1,<2')
        packages = temp / 'Python'
        scene = temp / 'scene.usda'
        scene.write_text('#usda 1.0\ndef Xform "World" {\n}\n')
        socket = socket_dir / 'mcp.sock'
        env = {**os.environ, 'VELA_MCP_SOCKET': str(socket), 'VELA_PLUGINS_FOLDER': str(plugins),
               'VELA_PYTHON_PACKAGES': str(packages), 'VELA_PACKAGES_AUTO_APPROVE': '1',
               'VELA_PIP_ARGS': '--no-index --find-links "%s"' % index}
        app = subprocess.Popen([vela, scene, '--defaultsettings'], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        deadline = time.time() + 120
        while not socket.exists():
            assert app.poll() is None, app.stdout.read()
            assert time.time() < deadline, 'Vela never listened'
            time.sleep(0.2)
        helper = subprocess.Popen([vela.parent / 'vela-mcp'], env=env, text=True,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        ids = iter(range(1, 1000))

        def send(method, params):
            helper.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': next(ids), 'method': method,
                                           'params': params}) + '\n')
            helper.stdin.flush()
            reply = json.loads(helper.stdout.readline())
            assert 'error' not in reply, reply
            return reply['result']

        def call(tool, **arguments):
            return send('tools/call', {'name': tool, 'arguments': arguments})

        def action(name, **params):
            result = call('ui_action', action=name, params=params)
            assert not result['isError'], result
            return result['structuredContent']['result']

        def failing(name, **params):
            result = call('ui_action', action=name, params=params)
            assert result['isError'], result
            return result['content'][0]['text']

        def python(code):
            result = call('run_python', code=code)
            assert not result['isError'], result
            return result['content'][0]['text']

        def names():
            return {a['name'] for a in call('list_actions')['structuredContent']['actions']}

        send('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {},
                            'clientInfo': {'name': 'test', 'version': '1'}})
        site = packages / 'lib/python3.11/site-packages'

        # At launch the plugin waits for velatestpkg; Vela offers to install
        # it (answered yes here) and then loads the plugin.
        deadline = time.time() + 120
        while 'needy.hello' not in names():
            assert app.poll() is None, app.stdout.read()
            if time.time() > deadline:
                print('files:', sorted(str(p.relative_to(temp)) for p in packages.rglob('*'))[:40])
                print('report:', call('ui_action', action='reload_plugins'))
                raise AssertionError('the plugin never loaded')
            time.sleep(0.5)
        assert action('needy.hello') == 'velatestpkg 1.0\n'
        assert (site / 'velatestpkg').is_dir(), list(packages.rglob('*'))[:20]
        manifest = json.loads((packages / 'requirements.json').read_text())
        assert manifest['plugins'] == {'needy': ['velatestpkg>=1,<2']}, manifest
        # The bundle's packages come first on the path; the user's last.
        order = json.loads(python(textwrap.dedent('''
            import json, sys
            print(json.dumps([p for p in sys.path if 'site-packages' in p]))''')))
        assert order[0].endswith('Runtime/site-packages') and Path(order[-1]) == site, order

        # A script installs a package (with what it depends on).
        result = action('install_python_packages', packages=['velatestdep'])
        assert 'velatestdep' in result['installed'], result
        assert python('import velatestdep\nvelatestdep.VALUE') == "'1.0'"
        manifest = json.loads((packages / 'requirements.json').read_text())
        assert manifest['requested'] == ['velatestdep'], manifest

        # Refused before pip runs: options, URLs, and a second USD.
        assert 'pip option' in failing('install_python_packages', packages=['--index-url x'])
        assert 'path or URL' in failing('install_python_packages',
                                        packages=['https://example.com/x.whl'])
        assert 'second USD' in failing('install_python_packages', packages=['usd-core'])
        # The bundle's PyObjC is kept: pip can't install another version.
        error = failing('install_python_packages', packages=['pyobjc-core<12'])
        assert 'pip couldn' in error, error
        # A conflict with what a plugin needs fails; nothing changes.
        error = failing('install_python_packages', packages=['velatestpkg>=2'])
        assert 'pip couldn' in error, error
        assert action('needy.hello') == 'velatestpkg 1.0\n'

        # The plugin asks for a newer version: it waits until installed.
        write_needy(plugins, 'velatestpkg>=2')
        report = action('reload_plugins')
        assert report['waiting'] == [{'plugin': 'needy', 'missing': ['velatestpkg>=2']}], report
        assert 'needy.hello' not in names()
        report = action('install_plugin_packages')
        assert 'needy' in report['plugins'] and not report['waiting'], report
        assert python("from importlib import metadata\nmetadata.version('velatestpkg')") == "'2.0'"
        manifest = json.loads((packages / 'requirements.json').read_text())
        assert manifest['plugins'] == {'needy': ['velatestpkg>=2']}, manifest

        # Resetting deletes the packages and installs what was asked for.
        action('reset_python_packages')
        assert (site / 'velatestpkg').is_dir() and (site / 'velatestdep').is_dir()
        print('Package checks passed')
    finally:
        if helper:
            helper.stdin.close()
            helper.wait(timeout=10)
        if app:
            app.terminate()
            try:
                app.wait(timeout=20)
            except subprocess.TimeoutExpired:
                app.kill()
        shutil.rmtree(temp, ignore_errors=True)
        shutil.rmtree(socket_dir, ignore_errors=True)


if __name__ == '__main__':
    main()
