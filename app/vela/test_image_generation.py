"""Image jobs through a real Vela/MCP session; fixture providers never use HTTP.

Uses --defaultsettings, a private socket/library, and generated test pixels.
Exercises both provider routes, references, previews, isolation, cancellation,
policy protection, native actions and persistence without keys or paid calls.
"""
import base64
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib


def png():
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + bytes((x * 4, y * 4, 160)) * 64 for y in range(64) for x in [16])
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 64, 64, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


with tempfile.TemporaryDirectory(prefix='vela-images-', dir='/tmp') as folder:
    root = Path(folder)
    subprocess.run(['/usr/bin/ditto', '-x', '-k', sys.argv[1], folder], check=True)
    vela, = root.glob('*.app/Contents/MacOS/Vela')
    scene = root / 'scene.usda'
    scene.write_text('#usda 1.0\ndef Cube "Cube" {}\n')
    fixture = root / 'fixture.png'
    fixture.write_bytes(png())
    endpoint = root / 'mcp.sock'
    library = root / 'library'
    env = {**os.environ, 'VELA_MCP_SOCKET': str(endpoint), 'VELA_IMAGE_LIBRARY': str(library),
           'VELA_IMAGE_FIXTURE': str(fixture), 'VELA_IMAGE_TEST_AUTO': '1'}
    with (root / 'app.log').open('w') as log:
        app = subprocess.Popen([vela, scene, '--defaultsettings'], env=env, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 90
            while not endpoint.exists():
                assert app.poll() is None, (root / 'app.log').read_text()
                assert time.monotonic() < deadline, 'No MCP socket'
                time.sleep(.1)
            client = socket.socket(socket.AF_UNIX)
            client.settimeout(30)
            client.connect(str(endpoint))
            stream = client.makefile('rw')
            sequence = 0

            def rpc(method, **params):
                global sequence
                sequence += 1
                stream.write(json.dumps({'jsonrpc': '2.0', 'id': sequence, 'method': method, 'params': params}) + '\n')
                stream.flush()
                reply = json.loads(stream.readline())
                assert 'error' not in reply, reply
                return reply['result']

            def raw(name, **arguments):
                return rpc('tools/call', name=name, arguments=arguments)

            def call(name, **arguments):
                result = raw(name, **arguments)
                assert not result['isError'], result
                return result.get('structuredContent', {})

            def action(name, **params):
                return call('ui_action', action=name, params=params)

            def wait(job):
                deadline = time.monotonic() + 20
                while True:
                    result = call('get_image_job', job_id=job)
                    if result['state'] == 'succeeded':
                        return result
                    assert result['state'] in ('queued', 'running'), result
                    assert time.monotonic() < deadline
                    # Keep the UI/main-thread tool path active while generation waits.
                    call('get_app_state')
                    time.sleep(.1)

            rpc('initialize', protocolVersion='2025-06-18', capabilities={}, clientInfo={'name': 'images-test', 'version': '1'})
            state = call('get_app_state')['document']
            first_document = state['id']
            models = call('list_image_models')['models']
            assert {m['connection_id'] for m in models} == {'openai', 'gemini'}
            assert all(m['ready'] for m in models)
            assert raw('ui_action', action='set_settings', params={'settings': {'imageAsk': False}})['isError']
            reference = call('capture_image_reference', width=128, height=128)['artifact_id']
            job = call('generate_image', request_id='concept', prompt='A sunset concept', reference_ids=[reference])['job_id']
            assert call('generate_image', request_id='concept', prompt='A sunset concept', reference_ids=[reference])['job_id'] == job
            assert raw('generate_image', request_id='concept', prompt='Different')['isError']
            done = wait(job)
            image = raw('get_generated_image', artifact_id=done['artifact_id'], include_preview=True)
            assert not image['isError'] and len(image['content']) == 2
            assert base64.b64decode(image['content'][1]['data']).startswith(b'\x89PNG')
            assert Path(image['structuredContent']['path']).is_file()
            # Export into a project, bind through the existing USD API, and
            # verify undo/redo plus relative resolution after moving the project.
            assets = root / 'assets'
            assets.mkdir()
            shutil.copyfile(image['structuredContent']['path'], assets / 'fabric.png')
            action('set_edit_target', layer=str(scene))
            code = """from pxr import UsdShade, Sdf
material = UsdShade.Material.Define(vela.stage, '/Looks/Fabric')
shader = UsdShade.Shader.Define(vela.stage, '/Looks/Fabric/Surface')
shader.CreateIdAttr('UsdPreviewSurface')
shader.CreateOutput('surface', Sdf.ValueTypeNames.Token)
texture = UsdShade.Shader.Define(vela.stage, '/Looks/Fabric/Texture')
texture.CreateIdAttr('UsdUVTexture')
texture.CreateInput('file', Sdf.ValueTypeNames.Asset).Set(Sdf.AssetPath('assets/fabric.png'))
texture.CreateInput('sourceColorSpace', Sdf.ValueTypeNames.Token).Set('sRGB')
texture.CreateOutput('rgb', Sdf.ValueTypeNames.Float3)
shader.CreateInput('diffuseColor', Sdf.ValueTypeNames.Color3f).ConnectToSource(texture.ConnectableAPI(), 'rgb')
material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), 'surface')
UsdShade.MaterialBindingAPI.Apply(vela.stage.GetPrimAtPath('/Cube')).Bind(material)
"""
            call('run_python', code=code)
            action('undo')
            assert 'False' in raw('query_python', code="bool(vela.stage.GetPrimAtPath('/Looks/Fabric'))")['content'][0]['text']
            action('redo')
            action('save')
            moved = root / 'moved'
            moved.mkdir()
            shutil.copyfile(scene, moved / scene.name)
            shutil.copytree(assets, moved / 'assets')
            relocated = call('open_document', path=str(moved / scene.name))['document']
            resolution = raw('query_python', document=relocated, code="str(vela.stage.GetPrimAtPath('/Looks/Fabric/Texture').GetAttribute('inputs:file').Get().resolvedPath)")
            assert str(moved / 'assets/fabric.png') in resolution['content'][0]['text'], resolution
            # Restore the source window as the explicit target for later work.
            def original(name, **arguments):
                return call(name, document=first_document, **arguments)
            second = original('generate_image', connection_id='gemini', request_id='snow', prompt='Snowy background', reference_ids=[done['artifact_id']])['job_id']
            # Queries below follow the source document even after opening the copy.
            def wait_original(job):
                deadline = time.monotonic() + 20
                while True:
                    r = original('get_image_job', job_id=job)
                    if r['state'] == 'succeeded': return r
                    assert r['state'] in ('queued', 'running'), r
                    assert time.monotonic() < deadline
                    time.sleep(.1)
            wait_original(second)
            cancelled = original('generate_image', request_id='cancel', prompt='Cancel this')['job_id']
            original('cancel_image_job', job_id=cancelled)
            time.sleep(.4)
            assert original('get_image_job', job_id=cancelled)['state'] == 'cancelled'
            # Native entry point uses the same service; its result opens the library.
            native = original('ui_action', action='generate_image', params={'prompt': 'Native form image'})['result']['job_id']
            wait_original(native)
            action('generated_images')
            action('image_generation_settings')
            other_scene = root / 'other.usda'
            other_scene.write_text('#usda 1.0\ndef Sphere "Ball" {}\n')
            call('open_document', path=str(other_scene))
            assert raw('get_image_job', job_id=job)['isError']
            assert raw('get_generated_image', artifact_id=done['artifact_id'])['isError']
            assert call('list_generated_images')['artifacts'] == []
            assert len(call('list_generated_images', document=first_document)['artifacts']) >= 4
            manifest = json.loads((library / 'manifest.json').read_text())
            assert manifest['version'] == 1 and manifest['artifacts']
            assert 'test-key' not in json.dumps(manifest)
            stream.close()
            client.close()
            print('Image generation UI/MCP checks passed (no network requests)')
        finally:
            app.terminate()
            try:
                app.wait(timeout=15)
            except subprocess.TimeoutExpired:
                app.kill()
                app.wait()
