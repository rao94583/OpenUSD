"""Exercise live material replacement with an already populated Storm renderer.

All scene edits use Vela's Python/MCP path. Captures force material-network
consumption between edits, so cached USD shader handles cannot hide until exit.
"""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix='vela-material-', dir='/tmp') as folder:
    root = Path(folder)
    subprocess.run(['ditto', '-x', '-k', sys.argv[1], folder], check=True)
    vela, = root.glob('*.app/Contents/MacOS/Vela')
    scene = root / 'scene.usda'
    scene.write_text('#usda 1.0\ndef Cube "Cube" {}\n')
    endpoint = root / 'mcp.sock'
    env = {**os.environ, 'VELA_MCP_SOCKET': str(endpoint)}
    with (root / 'app.log').open('w') as log:
        app = subprocess.Popen([vela, scene, '--defaultsettings'], env=env, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 90
            while not endpoint.exists():
                assert app.poll() is None, (root / 'app.log').read_text()
                assert time.monotonic() < deadline
                time.sleep(.1)
            client = socket.socket(socket.AF_UNIX)
            client.settimeout(30)
            client.connect(str(endpoint))
            stream = client.makefile('rw')
            sequence = 0

            def call(name, **arguments):
                global sequence
                sequence += 1
                stream.write(json.dumps({'jsonrpc': '2.0', 'id': sequence,
                    'method': 'tools/call', 'params': {'name': name, 'arguments': arguments}}) + '\n')
                stream.flush()
                line = stream.readline()
                assert line, 'Vela exited during material sync:\n' + (root / 'app.log').read_text()
                result = json.loads(line)['result']
                assert not result['isError'], result
                return result

            def script(code):
                return call('run_python', code=code)

            def capture():
                image = call('viewport_screenshot', width=96, height=96)
                assert image['content'][0]['type'] == 'image'
                assert app.poll() is None

            # Populate material caches before invalidating any shader prim.
            create = """from pxr import UsdShade, Sdf
mat = UsdShade.Material.Define(vela.stage, '/Looks/Material')
surface = UsdShade.Shader.Define(vela.stage, '/Looks/Material/Surface')
surface.CreateIdAttr('UsdPreviewSurface')
surface.CreateOutput('surface', Sdf.ValueTypeNames.Token)
texture = UsdShade.Shader.Define(vela.stage, '/Looks/Material/Texture')
texture.CreateIdAttr('UsdUVTexture')
texture.CreateInput('fallback', Sdf.ValueTypeNames.Float4).Set(Gf.Vec4f(0.2, 0.5, 0.8, 1))
texture.CreateOutput('rgb', Sdf.ValueTypeNames.Float3)
surface.CreateInput('diffuseColor', Sdf.ValueTypeNames.Color3f).ConnectToSource(texture.ConnectableAPI(), 'rgb')
mat.CreateSurfaceOutput().ConnectToSource(surface.ConnectableAPI(), 'surface')
UsdShade.MaterialBindingAPI.Apply(vela.stage.GetPrimAtPath('/Cube')).Bind(mat)
"""
            script(create)
            capture()
            for iteration in range(3):
                # Shader handles in the old network expire; a new prim at the
                # same path has a distinct identity even though its path matches.
                script("vela.stage.RemovePrim('/Looks/Material/Texture')")
                capture()
                script(create)
                capture()
                # Undo/redo replays namespace edits as one Sdf change block.
                call('ui_action', action='undo')
                capture()
                call('ui_action', action='redo')
                capture()
                # Replace the whole graph while preserving its bound path.
                script("vela.stage.RemovePrim('/Looks/Material')\n" + create)
                capture()
                # A value-only edit must still update normally.
                script("surface.CreateInput('roughness', Sdf.ValueTypeNames.Float).Set(0.8)")
                capture()
            # A nested MaterialX graph keeps a surviving normalmap shader while
            # replacing its former upstream chain. Cached input connections
            # must not lead Hydra back to the expired heighttonormal prim.
            mtlx = """G = '/Looks/Fabric/MaterialX/'
mat = UsdShade.Material.Define(vela.stage, '/Looks/Fabric')
UsdShade.NodeGraph.Define(vela.stage, G[:-1])
def node(name, identifier, out_type):
    shader = UsdShade.Shader.Define(vela.stage, G + name)
    shader.CreateIdAttr(identifier)
    shader.CreateOutput('out', out_type)
    return shader
F = Sdf.ValueTypeNames
height = node('height', 'ND_image_float', F.Float)
height.CreateInput('file', F.Asset).Set(Sdf.AssetPath('missing-height.png'))
bump = node('heighttonormal', 'ND_heighttonormal_vector3', F.Vector3f)
bump.CreateInput('in', F.Float).ConnectToSource(height.ConnectableAPI(), 'out')
normal = node('normalmap', 'ND_normalmap_float', F.Vector3f)
normal.CreateInput('in', F.Vector3f).ConnectToSource(bump.ConnectableAPI(), 'out')
surface = node('Surface', 'ND_open_pbr_surface_surfaceshader', F.Token)
surface.CreateInput('geometry_normal', F.Vector3f).ConnectToSource(normal.ConnectableAPI(), 'out')
surface.CreateInput('specular_roughness', F.Float).Set(0.5)
mat.CreateSurfaceOutput('mtlx').ConnectToSource(surface.ConnectableAPI(), 'out')
UsdShade.MaterialBindingAPI.Apply(vela.stage.GetPrimAtPath('/Cube')).Bind(mat)
"""
            script(mtlx)
            capture()
            script("""vela.stage.RemovePrim(G + 'height')
vela.stage.RemovePrim(G + 'heighttonormal')
procedural = node('procedural', 'ND_constant_vector3', F.Vector3f)
procedural.CreateInput('value', F.Vector3f).Set(Gf.Vec3f(0.5, 0.5, 1))
normal.GetInput('in').ConnectToSource(procedural.ConnectableAPI(), 'out')
""")
            capture()
            for action in ('undo', 'redo'):
                call('ui_action', action=action)
                capture()
            stream.close()
            client.close()
            print('Material removal, replacement and undo/redo captures passed')
        finally:
            app.terminate()
            try:
                app.wait(timeout=15)
            except subprocess.TimeoutExpired:
                app.kill()
                app.wait()
