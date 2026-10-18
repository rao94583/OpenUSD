"""Plugins in a launched Vela, as usdview has them: found through USD's
plugin registry in Vela's plugins folder (VELA_PLUGINS_FOLDER here) and on
PXR_PLUGINPATH_NAME. Checks commands as actions with one undo step each,
menus, PyObjC UI with vela.Target, reloading changed and new plugins, and
errors. The app uses --defaultsettings and a private socket."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import time

TOOLS = '''
from pxr import Tf, UsdGeom
import vela

_windows = {}
clicked = []

def add_marker(document):
    UsdGeom.Xform.Define(document.stage, '/Marker')
    print('added', document.name)

def show_panel(document):
    from AppKit import NSButton, NSPanel, NSMakeRect
    panel = NSPanel.alloc().initWithContentRect_styleMask_backing_defer_(
        NSMakeRect(200, 200, 240, 80), 1 | 2, 2, False)
    panel.setTitle_('Marker Panel')
    panel.setReleasedWhenClosed_(False)
    button = NSButton.alloc().initWithFrame_(NSMakeRect(20, 20, 200, 30))
    button.setTitle_('Add Marker')
    target = vela.Target(lambda sender: (clicked.append(1), add_marker(vela.current())))
    button.setTarget_(target)
    button.setAction_(target.action)
    panel.contentView().addSubview_(button)
    panel.orderFront_(None)
    _windows['panel'] = panel
    _windows['button'] = button

class Container(vela.PluginContainer):
    def registerPlugins(self, registry, plugCtx):
        self._add = registry.registerCommandPlugin(
            'tools.addMarker', 'TITLE', add_marker, 'Adds /Marker')
        self._panel = registry.registerCommandPlugin(
            'tools.showPanel', 'Show Marker Panel', show_panel, 'A PyObjC panel')
    def configureView(self, registry, builder):
        menu = builder.findOrCreateMenu('Tools')
        menu.addItem(self._add, shortcut='Ctrl+Shift+J')
        menu.addSeparator()
        menu.findOrCreateSubmenu('Windows').addItem(self._panel)

Tf.Type.Define(Container)
'''

# Found the usdview way, on PXR_PLUGINPATH_NAME, adding to a Vela menu.
EXTRA = '''
from pxr import Tf
import vela

def count(document):
    return print(len(list(document.stage.Traverse())))

class ExtraContainer(vela.PluginContainer):
    def registerPlugins(self, registry, plugCtx):
        self._count = registry.registerCommandPlugin('extra.count', 'Count Prims', count)
    def configureView(self, registry, builder):
        builder.findOrCreateMenu('Edit').addItem(self._count)

Tf.Type.Define(ExtraContainer)
'''


def plug_info(name, container):
    return json.dumps({'Plugins': [{'Type': 'python', 'Name': name, 'Info': {'Types': {
        '%s.%s' % (name, container): {'bases': ['vela.PluginContainer']}}}}]})


def write_plugin(folder, name, container, code):
    package = folder / name
    package.mkdir(parents=True, exist_ok=True)
    (package / 'plugInfo.json').write_text(plug_info(name, container))
    (package / '__init__.py').write_text(textwrap.dedent(code))


def main():
    temp = Path(tempfile.mkdtemp(prefix='Vela plugins '))
    socket_dir = Path(tempfile.mkdtemp(prefix='vela-', dir='/tmp'))
    app = helper = None
    try:
        subprocess.run(['/usr/bin/ditto', '-x', '-k', sys.argv[1], str(temp)], check=True)
        vela, = temp.glob('*.app/Contents/MacOS/Vela')
        plugins = temp / 'Plugins'
        write_plugin(plugins, 'tools', 'Container', TOOLS.replace('TITLE', 'Add Marker'))
        extra = temp / 'elsewhere'
        write_plugin(extra, 'extra', 'ExtraContainer', EXTRA)
        scene = temp / 'scene.usda'
        scene.write_text('#usda 1.0\ndef Xform "World" {\n    def Cube "Box" {\n    }\n}\n')
        socket = socket_dir / 'mcp.sock'
        env = {**os.environ, 'VELA_MCP_SOCKET': str(socket), 'VELA_PLUGINS_FOLDER': str(plugins),
               'PXR_PLUGINPATH_NAME': str(extra / 'extra') + '/'}
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

        def python(code):
            result = call('run_python', code=code)
            assert not result['isError'], result
            return result['content'][0]['text']

        def names():
            return {a['name'] for a in call('list_actions')['structuredContent']['actions']}

        send('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {},
                            'clientInfo': {'name': 'test', 'version': '1'}})
        deadline = time.time() + 60
        while 'tools.addMarker' not in names():
            assert time.time() < deadline, 'the plugins never loaded'
            time.sleep(0.3)
        assert {'tools.showPanel', 'extra.count', 'reload_plugins',
                'show_plugins_folder'} <= names(), names()

        # A command is an action: its edits are one undo step named after it.
        assert action('tools.addMarker') == 'added scene.usda\n'
        assert python("bool(vela.stage.GetPrimAtPath('/Marker'))") == 'True'
        state = call('get_app_state')['structuredContent']['document']
        assert state['undo'] == 'Add Marker', state
        action('undo')
        assert python("bool(vela.stage.GetPrimAtPath('/Marker'))") == 'False'

        # Menus: a new Tools menu (with a submenu) before Window, and an item
        # in Vela's own Edit menu.
        menus = json.loads(python(textwrap.dedent('''
            import json
            from AppKit import NSApp
            def items(menu):
                return [(i.title(), i.keyEquivalent(), int(i.keyEquivalentModifierMask()),
                         items(i.submenu()) if i.submenu() else None) for i in menu.itemArray()]
            bar = NSApp.mainMenu()
            menu = {i.submenu().title(): i.submenu() for i in bar.itemArray() if i.submenu()}
            print(json.dumps({'bar': [i.submenu().title() for i in bar.itemArray() if i.submenu()],
                              'tools': items(menu['Tools']),
                              'edit': [i.title() for i in menu['Edit'].itemArray()],
                              'plugins': [i.title() for i in menu['Plugins'].itemArray()]}))
            ''')))
        bar = menus['bar']
        assert bar.index('Tools') < bar.index('Window'), bar
        tools = menus['tools']
        assert tools[0][:2] == ['Add Marker', 'j'], tools
        assert tools[0][2] == (1 << 20) | (1 << 17), tools  # Command, Shift
        assert tools[2][0] == 'Windows' and tools[2][3][0][0] == 'Show Marker Panel', tools
        assert menus['edit'][-1] == 'Count Prims', menus['edit']
        assert menus['plugins'][:2] == ['Reload Plugins', 'Show Plugins Folder'], menus

        # PyObjC UI, with a vela.Target button.
        action('tools.showPanel')
        assert 'Marker Panel' in python(
            "from AppKit import NSApp\n[w.title() for w in NSApp.windows() if w.isVisible()]")
        python("import tools\ntools._windows['button'].performClick_(None)")
        assert python('import tools\nlen(tools.clicked)') == '1'
        assert python("bool(vela.stage.GetPrimAtPath('/Marker'))") == 'True'

        # Reloading picks up changed code, new plugins and errors.
        write_plugin(plugins, 'tools', 'Container', TOOLS.replace('TITLE', 'Add a Marker'))
        write_plugin(plugins, 'fresh', 'FreshContainer', '''
            from pxr import Tf
            import vela
            class FreshContainer(vela.PluginContainer):
                def registerPlugins(self, registry, plugCtx):
                    self._c = registry.registerCommandPlugin('fresh.hello', 'Hello', lambda d: print('hi'))
                def configureView(self, registry, builder):
                    builder.findOrCreateMenu('Tools').addItem(self._c)
            Tf.Type.Define(FreshContainer)
            ''')
        write_plugin(plugins, 'broken', 'BrokenContainer', 'def broken(:\n')
        report = action('reload_plugins')
        assert report['plugins'] == ['extra', 'fresh', 'tools'], report
        assert any('broken' in e and 'SyntaxError' in e for e in report['errors']), report['errors']
        titles = {c['name']: c['title'] for c in report['commands']}
        assert titles['tools.addMarker'] == 'Add a Marker', titles
        assert action('fresh.hello') == 'hi\n'
        tools = json.loads(python(
            "import json\nfrom AppKit import NSApp\n"
            "menu = [i.submenu() for i in NSApp.mainMenu().itemArray() "
            "if i.submenu() and i.submenu().title() == 'Tools'][0]\n"
            "print(json.dumps([i.title() for i in menu.itemArray()]))"))
        assert tools == ['Hello', 'Add a Marker', '', 'Windows'], tools  # plugins in name order

        # A plugin taken away is gone after reloading, with its actions.
        shutil.rmtree(plugins / 'fresh')
        report = action('reload_plugins')
        assert 'fresh' not in report['plugins'] and 'fresh.hello' not in names(), report
        print('Plugin checks passed')
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
