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
import zlib



def read_png(path):
    """(width, height, rows of RGB(A) tuples) for an 8-bit RGB or RGBA PNG."""
    data = Path(path).read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, chunks, header = 8, b'', None
    while pos < len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            chunks += body
        pos += 12 + length
    width, height, depth, color = header[:4]
    assert depth == 8 and color in (2, 6), header
    channels = 4 if color == 6 else 3
    raw, stride, rows, previous = zlib.decompress(chunks), width * channels, [], bytearray(width * channels)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = previous[i]
            c = previous[i - channels] if i >= channels else 0
            if kind == 1: line[i] = (line[i] + a) & 255
            elif kind == 2: line[i] = (line[i] + b) & 255
            elif kind == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * channels:(x + 1) * channels]) for x in range(width)])
        previous = line
    return width, height, rows


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
    assert {'run_python', 'query_python', 'ui_action', 'list_actions', 'get_app_state',
            'viewport_screenshot', 'open_document'} <= tools.keys(), tools.keys()
    assert tools['get_app_state']['annotations']['readOnlyHint']
    assert tools['query_python']['annotations']['readOnlyHint']
    assert 'annotations' not in tools['run_python']

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

    # Queries read, and their edits are reverted without an undo step.
    query = call('query_python', code="cube.GetAttribute('size').Get()")
    assert not query['isError'] and query['content'][0]['text'] == '5.0', query
    query = call('query_python', code="cube.GetAttribute('size').Set(9.0)\n"
                                      "vela.stage.DefinePrim('/World/Q')")
    assert query['isError'] and 'run_python' in query['content'][0]['text'], query
    assert python("cube.GetAttribute('size').Get()") == '5.0'
    assert python("bool(vela.stage.GetPrimAtPath('/World/Q'))") == 'False'
    state = call('get_app_state')['structuredContent']['document']
    assert state['undo'] == 'Python' and not state['redo'], state

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

    # Exports render offscreen, so they work with the window minimized.
    subprocess.run(['/usr/bin/osascript', '-e',
                    'tell application "System Events" to tell (first process whose unix id is '
                    f'{app.pid}) to set value of attribute "AXMinimized" of window 1 to true'],
                   capture_output=True)
    for name, size in (('view.png', 320), ('view.jpg', 200), ('view.tif', 160), ('view.exr', 120)):
        result = call('ui_action', action='export_image',
                      params={'path': str(temp / name), 'width': size})
        assert not result['isError'], result
        assert result['structuredContent']['result']['width'] == size, result
        assert (temp / name).stat().st_size > 1000, name
    assert (temp / 'view.png').read_bytes()[:8] == b'\x89PNG\r\n\x1a\n'
    assert struct.unpack('>II', (temp / 'view.png').read_bytes()[16:24])[0] == 320
    assert (temp / 'view.exr').read_bytes()[:4] == b'\x76\x2f\x31\x01'  # OpenEXR
    bad = call('ui_action', action='export_image', params={'path': str(temp / 'view.gif')})
    assert bad['isError'], bad
    movie = call('ui_action', action='export_movie',
                 params={'path': str(temp / 'clip.mp4'), 'width': 161, 'start': 1, 'end': 6})
    assert not movie['isError'], movie
    info = movie['structuredContent']['result']
    assert info['frames'] == 6 and info['width'] == 162, info  # even, for H.264
    data = (temp / 'clip.mp4').read_bytes()
    assert data[4:8] == b'ftyp' and b'avc1' in data, data[:32]
    prores = call('ui_action', action='export_movie',
                  params={'path': str(temp / 'clip.mov'), 'width': 96, 'start': 1, 'end': 3,
                          'codec': 'prores422'})
    assert not prores['isError'] and b'apcn' in (temp / 'clip.mov').read_bytes(), prores
    bad = call('ui_action', action='export_movie',
               params={'path': str(temp / 'clip2.mp4'), 'codec': 'prores422'})
    assert bad['isError'], bad
    # Presets and framing. The free camera has the viewport's aspect ratio,
    # which isn't square: fit boxes a square export (black bars on two
    # opposite edges), crop fills it.
    def edges_black(path):
        width, height, rows = read_png(path)
        top, bottom = rows[0], rows[-1]
        left = [row[0] for row in rows]
        right = [row[-1] for row in rows]
        black = lambda pixels: all(p[:3] == (0, 0, 0) for p in pixels)
        return (black(top) and black(bottom)) or (black(left) and black(right))
    for framing, boxed in (('fit', True), ('crop', False)):
        path = temp / f'square_{framing}.png'
        result = call('ui_action', action='export_image',
                      params={'path': str(path), 'resolution': 'square', 'framing': framing,
                              'showSelection': False})
        assert not result['isError'], result
        info = result['structuredContent']['result']
        assert (info['width'], info['height']) == (1080, 1080), info
        assert edges_black(path) == boxed, framing
    bad = call('ui_action', action='export_image',
               params={'path': str(temp / 'x.png'), 'resolution': '8k'})
    assert bad['isError'], bad
    bad = call('ui_action', action='export_image',
               params={'path': str(temp / 'x.png'), 'framing': 'stretch'})
    assert bad['isError'], bad
    hd = call('ui_action', action='export_movie',
              params={'path': str(temp / 'hd.mp4'), 'resolution': '720p', 'start': 1, 'end': 2})
    assert hd['structuredContent']['result']['width'] == 1280, hd

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
