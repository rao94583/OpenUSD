"""Phase 7: the Claude panel, end to end, with a stand-in for the claude CLI.

The stand-in speaks the CLI's stream-JSON protocol as Claude Code 2.1.296
does: it answers initialize, streams a reply, asks for approval with
can_use_tool, calls Vela's tools through the vela-mcp server it was given
(--mcp-config), and ends each turn with a result. The test drives Vela
through its public MCP socket (ask_claude), and checks the scene, the
single undo step per request, approvals, history and resuming. The app uses
--defaultsettings and private sockets and folders, so the user's Vela,
settings and conversations are left alone."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

FAKE = r'''#!PYTHON
import json, os, select, subprocess, sys, time
LOG = os.environ['FAKE_CLAUDE_LOG']
args = sys.argv[1:]
if args == ['--version']:
    print('2.1.296 (Claude Code)')
    sys.exit(0)

def record(kind, value):
    with open(LOG, 'a') as f:
        f.write(json.dumps({'kind': kind, 'value': value}) + '\n')

def flag(name):
    return args[args.index(name) + 1] if name in args else None

record('argv', args)
session = flag('--resume') or flag('--session-id')
config = json.loads(flag('--mcp-config'))['mcpServers']['vela']
server = None
ids = iter(range(1, 10000))

def emit(message):
    sys.stdout.write(json.dumps(message) + '\n')
    sys.stdout.flush()

def read():
    line = sys.stdin.readline()
    if not line:
        sys.exit(0)
    return json.loads(line)

def mcp(method, params):
    global server
    if server is None:
        server = subprocess.Popen([config['command']] + config['args'], text=True,
                                  env={**os.environ, **config['env']},
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        mcp('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {},
                           'clientInfo': {'name': 'fake', 'version': '1'}})
    message = {'jsonrpc': '2.0', 'id': next(ids), 'method': method, 'params': params}
    server.stdin.write(json.dumps(message) + '\n')
    server.stdin.flush()
    return json.loads(server.stdout.readline())['result']

def tool(name, arguments, ask=True):
    """A tool call as the CLI makes it: tool_use, approval, result."""
    tool_id = 'toolu_%d' % next(ids)
    full = 'mcp__vela__' + name
    emit({'type': 'assistant', 'message': {'role': 'assistant', 'content': [
        {'type': 'tool_use', 'id': tool_id, 'name': full, 'input': arguments}]},
        'parent_tool_use_id': None, 'session_id': session})
    if ask:
        request = 'perm_%d' % next(ids)
        emit({'type': 'control_request', 'request_id': request, 'request': {
            'subtype': 'can_use_tool', 'tool_name': full, 'input': arguments,
            'description': 'Use ' + name}})
        answer = read()
        assert answer['type'] == 'control_response', answer
        decision = answer['response']['response']
        record('permission', {'tool': name, 'behavior': decision['behavior']})
        if decision['behavior'] != 'allow':
            content, error = 'Denied', True
            emit({'type': 'user', 'message': {'role': 'user', 'content': [
                {'type': 'tool_result', 'tool_use_id': tool_id, 'content': content,
                 'is_error': error}]}, 'parent_tool_use_id': None, 'session_id': session})
            return None
        arguments = decision['updatedInput']
    result = mcp('tools/call', {'name': name, 'arguments': arguments})
    content = []
    for block in result['content']:
        if block['type'] == 'image':
            content.append({'type': 'image', 'source': {'type': 'base64',
                            'media_type': block['mimeType'], 'data': block['data']}})
        else:
            content.append(block)
    emit({'type': 'user', 'message': {'role': 'user', 'content': [
        {'type': 'tool_result', 'tool_use_id': tool_id, 'content': content,
         'is_error': result['isError']}]}, 'parent_tool_use_id': None, 'session_id': session})
    return result

def say(text):
    for i in range(0, len(text), 7):
        emit({'type': 'stream_event', 'event': {'type': 'content_block_delta', 'index': 0,
              'delta': {'type': 'text_delta', 'text': text[i:i + 7]}},
              'parent_tool_use_id': None, 'session_id': session})
    emit({'type': 'assistant', 'message': {'role': 'assistant', 'content': [
        {'type': 'text', 'text': text}]}, 'parent_tool_use_id': None, 'session_id': session})

def result(text, error=False):
    emit({'type': 'result', 'subtype': 'error_during_execution' if error else 'success',
          'is_error': error, 'result': text, 'session_id': session, 'total_cost_usd': 0.0123})

initialized = False
while True:
    message = read()
    if message['type'] == 'control_request':
        record('control', message['request']['subtype'])
        emit({'type': 'control_response', 'response': {'subtype': 'success',
              'request_id': message['request_id'], 'response': {}}})
        if message['request']['subtype'] == 'initialize':
            initialized = True
        continue
    assert initialized, 'a user message before initialize'
    content = message['message']['content']
    text = ''.join(b.get('text', '') for b in content if b['type'] == 'text')
    record('user', {'text': text, 'images': sum(b['type'] == 'image' for b in content)})
    emit({'type': 'system', 'subtype': 'init', 'session_id': session, 'model': 'fake-model',
          'cwd': os.getcwd()})
    record('cwd', os.getcwd())
    request = text.split('</vela-context>')[-1].strip()
    if request.startswith('make every light'):
        say('I\'ll warm each light.')
        tool('get_app_state', {}, ask=True)
        tool('run_python', {'code':
            'lights = [p for p in vela.stage.Traverse() if p.HasAPI(UsdLux.LightAPI)]\n'
            'for p in lights[:1]:\n'
            '    a = UsdLux.LightAPI(p).GetColorAttr()\n'
            '    c = a.Get() or Gf.Vec3f(1)\n'
            '    a.Set(Gf.Vec3f(min(1, c[0] * 1.2), c[1], c[2] / 1.2))\n'
            'len(lights)'})
        tool('run_python', {'code':
            'for p in lights[1:]:\n'
            '    a = UsdLux.LightAPI(p).GetColorAttr()\n'
            '    c = a.Get() or Gf.Vec3f(1)\n'
            '    a.Set(Gf.Vec3f(min(1, c[0] * 1.2), c[1], c[2] / 1.2))'})
        tool('viewport_screenshot', {'width': 200})
        say('Done: **2 lights** are warmer.\n\n- Key\n- Fill')
        result('Done')
    elif request.startswith('which'):
        answer = tool('query_python', {'code': '[str(p.GetPath()) for p in vela.stage.Traverse()]'})
        say('These prims: `%s`' % answer['content'][0]['text'])
        result('ok')
    elif request.startswith('deny'):
        tool('run_python', {'code': 'vela.stage.DefinePrim("/Denied")'})
        say('You said no.')
        result('ok')
    elif request.startswith('generate an image'):
        models = tool('list_image_models', {})
        reference = tool('capture_image_reference', {'width': 128, 'height': 128})['structuredContent']['artifact_id']
        job = tool('generate_image', {'request_id': 'claude-image', 'prompt': 'Sunset concept', 'reference_ids': [reference]})['structuredContent']['job_id']
        record('image_job', job)
        say('The image is awaiting approval in Generated Images.')
        result('ok')
    elif request.startswith('slow'):
        # Streams for a while, ending early on an interrupt.
        stopped = False
        for i in range(100):
            ready, _, _ = select.select([sys.stdin], [], [], 0.1)
            if ready:
                message = read()
                if message['type'] == 'control_request':
                    record('control', message['request']['subtype'])
                    emit({'type': 'control_response', 'response': {'subtype': 'success',
                          'request_id': message['request_id'], 'response': {}}})
                    if message['request']['subtype'] == 'interrupt':
                        stopped = True
                        break
            emit({'type': 'stream_event', 'event': {'type': 'content_block_delta', 'index': 0,
                  'delta': {'type': 'text_delta', 'text': 'word '}},
                  'parent_tool_use_id': None, 'session_id': session})
        say('word ' * 10)
        result('Interrupted' if stopped else 'ok', error=stopped)
    elif request.startswith('crash'):
        sys.stderr.write('Not logged in. Please run /login\n')
        sys.exit(1)
    else:
        say('Hello.')
        result('ok')
'''


def main():
    archive = sys.argv[1]
    temp = Path(tempfile.mkdtemp(prefix='Vela claude '))
    socket_dir = Path(tempfile.mkdtemp(prefix='vela-', dir='/tmp'))
    app = helper = None
    try:
        subprocess.run(['/usr/bin/ditto', '-x', '-k', archive, str(temp)], check=True)
        vela, = temp.glob('*.app/Contents/MacOS/Vela')
        fake = temp / 'claude'
        fake.write_text(FAKE.replace('#!PYTHON', '#!' + sys.executable))
        fake.chmod(0o755)
        log = temp / 'fake.jsonl'
        history = temp / 'history'
        scene_dir = temp / 'scene folder'
        scene_dir.mkdir()
        scene = scene_dir / 'room.usda'
        scene.write_text('#usda 1.0\n(upAxis = "Y")\n'
                         'def Xform "Room" {\n'
                         '    def SphereLight "Key" {\n        color3f inputs:color = (1, 1, 1)\n    }\n'
                         '    def RectLight "Fill" {\n        color3f inputs:color = (0.5, 0.5, 0.5)\n    }\n'
                         '    def Cube "Box" {\n    }\n'
                         '}\n')
        socket = socket_dir / 'mcp.sock'
        fixture = temp / 'fixture.png'
        import base64
        fixture.write_bytes(base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aI1sAAAAASUVORK5CYII='))
        env = {**os.environ, 'VELA_IMAGE_FIXTURE': str(fixture), 'VELA_IMAGE_LIBRARY': str(temp / 'images'), 'VELA_MCP_SOCKET' : str(socket), 'VELA_CLAUDE': str(fake),
               'VELA_CLAUDE_HISTORY': str(history),
               'VELA_PLUGINS_FOLDER': str(temp / 'Plugins'), 'FAKE_CLAUDE_LOG': str(log)}
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

        def send(method, params=None):
            message = {'jsonrpc': '2.0', 'method': method, 'id': next(ids)}
            if params is not None:
                message['params'] = params
            helper.stdin.write(json.dumps(message) + '\n')
            helper.stdin.flush()
            reply = json.loads(helper.stdout.readline())
            assert 'error' not in reply, reply
            return reply['result']

        def call(tool, **arguments):
            return send('tools/call', {'name': tool, 'arguments': arguments})

        def python(code):
            result = call('run_python', code=code)
            assert not result['isError'], result
            return result['content'][0]['text']

        def state():
            return call('get_app_state')['structuredContent']['document']

        def records(kind):
            if not log.exists():
                return []
            return [r['value'] for r in map(json.loads, log.read_text().splitlines())
                    if r['kind'] == kind]

        def ask(prompt):
            result = call('ui_action', action='ask_claude', params={'prompt': prompt})
            assert not result['isError'], result

        def wait(condition, what, seconds=60):
            deadline = time.time() + seconds
            while not condition():
                assert app.poll() is None, app.stdout.read()
                assert time.time() < deadline, 'timed out: ' + what
                time.sleep(0.2)

        send('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {},
                            'clientInfo': {'name': 'test', 'version': '1'}})
        assert any(a['name'] == 'ask_claude' for a in
                   call('list_actions')['structuredContent']['actions'])
        color = ("tuple(round(c, 4) for c in UsdLux.LightAPI("
                 "vela.stage.GetPrimAtPath('/Room/%s')).GetColorAttr().Get())")
        before = (python(color % 'Key'), python(color % 'Fill'))
        call('ui_action', action='select', params={'paths': ['/Room/Box']})

        # An edit: two run_python calls and a screenshot, one undo step.
        prompt = 'make every light in this room 20% warmer'
        ask(prompt)
        wait(lambda: state()['undo'] == 'Claude: ' + prompt, 'the request\'s undo step')
        after = (python(color % 'Key'), python(color % 'Fill'))
        assert after == ('(1.0, 1.0, 0.8333)', '(0.6, 0.5, 0.4167)'), after
        argv = records('argv')[0]
        for flag, value in [('--input-format', 'stream-json'), ('--output-format', 'stream-json'),
                            ('--permission-prompt-tool', 'stdio'), ('--tools', 'Read,Glob,Grep,Write,Edit')]:
            assert argv[argv.index(flag) + 1] == value, (flag, argv)
        assert '--strict-mcp-config' in argv and '--session-id' in argv, argv
        session = argv[argv.index('--session-id') + 1]
        assert records('control')[0] == 'initialize'
        # The CLI runs in Vela's workspace and may read the scene's folder.
        assert os.path.samefile(records('cwd')[0], history / 'Workspace'), records('cwd')
        added = [argv[i + 1] for i, a in enumerate(argv) if a == '--add-dir']
        assert any(os.path.samefile(d, scene_dir) for d in added), added
        # And the plugins folder, the one place Claude writes files.
        assert any(os.path.samefile(d, temp / 'Plugins') for d in added), added
        user = records('user')[0]['text']
        assert user.startswith('<vela-context>') and 'Selection: /Room/Box' in user, user
        assert 'room.usda' in user and user.endswith(prompt), user
        # Edits run without asking by default; reads always do.
        assert records('permission') == [
            {'tool': 'get_app_state', 'behavior': 'allow'},
            {'tool': 'run_python', 'behavior': 'allow'},
            {'tool': 'run_python', 'behavior': 'allow'},
            {'tool': 'viewport_screenshot', 'behavior': 'allow'}], records('permission')
        # One step: undo restores both lights; redo brings both back.
        assert not call('ui_action', action='undo')['isError']
        assert (python(color % 'Key'), python(color % 'Fill')) == before
        assert not call('ui_action', action='redo')['isError']
        assert (python(color % 'Key'), python(color % 'Fill')) == after

        # A query in the same conversation (the same CLI process) adds no
        # undo step.
        ask('which prims are there?')
        wait(lambda: len(records('permission')) == 5, 'the query')
        logged = history / (session + '.jsonl')
        wait(lambda: logged.exists() and 'These prims' in logged.read_text(), 'the reply')
        wait(lambda: logged.read_text().count('"kind":"user"') == 2, 'the second request logged')
        assert len(records('argv')) == 1, 'the CLI was restarted'
        assert state()['undo'] == 'Claude: ' + prompt, state()

        # The history has the conversation, for the panel's menu.
        lines = [json.loads(l) for l in logged.read_text().splitlines()]
        assert lines[0]['kind'] == 'conversation' and lines[0]['title'] == prompt, lines[0]
        assert os.path.samefile(lines[0]['directory'], history / 'Workspace'), lines[0]
        assert os.path.samefile(lines[0]['document'], scene), lines[0]
        kinds = [l['kind'] for l in lines[1:]]
        assert kinds[:2] == ['user', 'assistant'], kinds
        assert kinds.count('tool') == 5 and kinds.count('tool_result') >= 4, kinds
        shot = [l for l in lines if l['kind'] == 'tool_result' and l['images']]
        assert len(shot) == 1, 'the screenshot was not kept'
        assert any(l['kind'] == 'note' and 'Changed the scene' in l['text'] for l in lines), kinds

        # Stop, as the panel's Stop button does: the CLI is interrupted and
        # the request ends.
        ask('slow story')
        wait(lambda: len(records('user')) == 3, 'the slow request')
        time.sleep(0.5)
        result = call('ui_action', action='stop_claude')
        assert not result['isError'], result
        wait(lambda: 'interrupt' in records('control'), 'the interrupt')
        wait(lambda: '"text":"Stopped."' in logged.read_text(), 'Stopped in the history')

        # The CLI stopping is reported, and the next request resumes the
        # conversation in a new CLI.
        ask('crash now')
        wait(lambda: 'Not logged in' in logged.read_text(), 'the error in the history')
        ask('hello again')
        wait(lambda: len(records('argv')) == 2, 'a new CLI')
        argv = records('argv')[1]
        assert argv[argv.index('--resume') + 1] == session, argv
        wait(lambda: len(records('user')) == 5, 'the resumed request')
        assert os.path.samefile(records('cwd')[-1], history / 'Workspace'), records('cwd')

        # The embedded agent discovers providers and submits a reference edit.
        # This creates a native pending approval, without HTTP or a key.
        ask('generate an image of this scene at sunset')
        wait(lambda: len(records('image_job')) == 1, 'the image job')
        job = records('image_job')[0]
        image_state = call('get_image_job', job_id=job)['structuredContent']
        assert image_state['state'] == 'awaiting_approval', image_state
        assert image_state['reference_ids'], image_state
        assert state()['undo'] == 'Claude: ' + prompt, state()
        assert 'Image generation in Vela' in records('argv')[0][records('argv')[0].index('--append-system-prompt') + 1]
        assert [p['tool'] for p in records('permission')[-3:]] == ['list_image_models', 'capture_image_reference', 'generate_image']
        assert all(p['behavior'] == 'allow' for p in records('permission')[-3:])
        call('cancel_image_job', job_id=job)
        print('Claude panel checks passed')
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
