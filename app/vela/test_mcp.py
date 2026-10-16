"""Phase 6: an MCP client drives a running Vela through vela-mcp, as Claude
Code does: lists prims, reads and changes attributes, undoes, and renders
the viewport. The app listens on a private socket (VELA_MCP_SOCKET) and
uses --defaultsettings, so the user's Vela and settings are left alone."""
import base64
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time

temp = Path(tempfile.mkdtemp(prefix='Vela mcp '))
# Unix socket paths are short (104 bytes).
socket_dir = Path(tempfile.mkdtemp(prefix='vela-', dir='/tmp'))
app = helper = None
try:
    subprocess.run(['/usr/bin/ditto', '-x', '-k', sys.argv[1], str(temp)], check=True)
    vela, = temp.glob('*.app/Contents/MacOS/Vela')
    scene = temp / 'scene.usda'
    scene.write_text('#usda 1.0\n(upAxis = "Y")\n'
                     'def Xform "World" {\n'
                     '    def Cube "Cube" {\n        double size = 2\n    }\n'
                     '    def Sphere "Ball" {\n        double radius = 1\n    }\n'
                     '}\n')
    socket = socket_dir / 'mcp.sock'
    env = {**os.environ, 'VELA_MCP_SOCKET': str(socket)}
    app = subprocess.Popen([vela, scene, '--defaultsettings'], env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    deadline = time.time() + 120
    while not socket.exists():
        assert app.poll() is None, app.stdout.read()
        assert time.time() < deadline, 'Vela never listened'
        time.sleep(0.2)
    assert (socket.stat().st_mode & 0o777) == 0o600

    helper = subprocess.Popen([vela.parent / 'vela-mcp'], env=env, text=True,
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    ids = iter(range(1, 1000))

    def send(method, params=None, notify=False):
        message = {'jsonrpc': '2.0', 'method': method}
        if params is not None:
            message['params'] = params
        if not notify:
            message['id'] = next(ids)
        helper.stdin.write(json.dumps(message) + '\n')
        helper.stdin.flush()
        if notify:
            return None
        reply = json.loads(helper.stdout.readline())
        assert reply['id'] == message['id'], reply
        assert 'error' not in reply, reply
        return reply['result']

    def call(tool, **arguments):
        return send('tools/call', {'name': tool, 'arguments': arguments})

    def python(code):
        result = call('run_python', code=code)
        assert not result['isError'], result
        return result['content'][0]['text']

    # The legacy handshake, as today's Claude Code does.
    init = send('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {},
                               'clientInfo': {'name': 'test', 'version': '1'}})
    assert init['protocolVersion'] == '2025-06-18' and 'tools' in init['capabilities'], init
    send('notifications/initialized', notify=True)
    tools = {t['name']: t for t in send('tools/list')['tools']}
    assert {'run_python', 'ui_action', 'list_actions', 'get_app_state',
            'viewport_screenshot', 'open_document'} <= tools.keys(), tools.keys()
    assert tools['get_app_state']['annotations']['readOnlyHint']

    state = call('get_app_state')['structuredContent']
    assert os.path.samefile(state['document']['path'], scene), state
    assert len(state['documents']) == 1, state['documents']

    # List prims and read an attribute.
    prims = python('[str(p.GetPath()) for p in vela.stage.Traverse()]')
    assert prims == "['/World', '/World/Cube', '/World/Ball']", prims
    assert python("vela.stage.GetPrimAtPath('/World/Cube').GetAttribute('size').Get()") == '2.0'
    # Change it: the window's stage changes, as one undo step.
    assert python("print('setting')\n"
                  "vela.stage.GetPrimAtPath('/World/Cube').GetAttribute('size').Set(5.0)"
                  ) == 'setting\nTrue'
    state = call('get_app_state')['structuredContent']['document']
    assert state['undo'] == 'Python' and state['unsavedEdits'], state
    # The namespace persists between calls.
    python('cube = vela.stage.GetPrimAtPath("/World/Cube")')
    assert python('cube.GetAttribute("size").Get()') == '5.0'
    # Errors come back as tracebacks, marked as errors.
    failed = call('run_python', code='1/0')
    assert failed['isError'] and 'ZeroDivisionError' in failed['content'][0]['text'], failed

    # Menu commands by name: select, then undo the edit.
    actions = call('list_actions')['structuredContent']['actions']
    assert any(a['name'] == 'frame_selection' for a in actions)
    result = call('ui_action', action='select', params={'paths': ['/World/Ball']})
    assert not result['isError'], result
    assert call('get_app_state')['structuredContent']['document']['selection'] == ['/World/Ball']
    bad = call('ui_action', action='select', params={'paths': ['/Nope']})
    assert bad['isError'], bad
    assert not call('ui_action', action='undo')['isError']
    assert python("cube.GetAttribute('size').Get()") == '2.0'

    # The viewport, as an image.
    shot = call('viewport_screenshot', width=320)
    assert not shot['isError'], shot
    image = shot['content'][0]
    assert image['type'] == 'image' and image['mimeType'] == 'image/png'
    png = base64.b64decode(image['data'])
    assert png[:8] == b'\x89PNG\r\n\x1a\n', png[:16]
    width, height = struct.unpack('>II', png[16:24])
    assert width == 320 and height > 0, (width, height)

    # The stateless protocol revision, on the same connection.
    meta = {'io.modelcontextprotocol/protocolVersion': '2026-07-28',
            'io.modelcontextprotocol/clientInfo': {'name': 'test', 'version': '1'},
            'io.modelcontextprotocol/clientCapabilities': {}}
    discover = send('server/discover', {'_meta': meta})
    assert '2026-07-28' in discover['supportedVersions'], discover
    listed = send('tools/list', {'_meta': meta})
    assert listed['ttlMs'] > 0 and listed['cacheScope'] == 'public', listed
    modern = send('tools/call', {'_meta': meta, 'name': 'get_app_state', 'arguments': {}})
    assert modern['resultType'] == 'complete', modern

    # Closing stdin ends the helper cleanly.
    helper.stdin.close()
    assert helper.wait(timeout=30) == 0
    print('MCP checks passed')
finally:
    for process in (helper, app):
        if process and process.poll() is None:
            process.kill()
            process.wait()
    shutil.rmtree(temp, ignore_errors=True)
    shutil.rmtree(socket_dir, ignore_errors=True)
