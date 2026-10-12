"""Run with the bundled interpreter, in isolated mode, after relocation."""
import json
import os
from pathlib import Path
import sys
import subprocess
import tempfile

runtime = Path(__file__).resolve().parents[0]
sys.path.insert(0, str(runtime / 'usd/lib/python'))
from pxr import Plug, Usd, UsdGeom, UsdUtils

for name, module in list(sys.modules.items()):
    if name == 'pxr' or name.startswith('pxr.'):
        if getattr(module, '__file__', None):
            assert Path(module.__file__).resolve().is_relative_to(runtime), module.__file__

registry = Plug.Registry()
for prefix in ('lib/usd', 'plugin/usd'):
    registry.RegisterPlugins(str(runtime / 'usd' / prefix / 'plugInfo.json'))
expected = set()
manifests = list((runtime / 'usd/lib/usd').glob('*/resources/plugInfo.json'))
manifests += list((runtime / 'usd/plugin/usd').glob('*/resources/plugInfo.json'))
for manifest in manifests:
    content = '\n'.join(line for line in manifest.read_text().splitlines()
                        if not line.lstrip().startswith('#'))
    for plugin in json.loads(content).get('Plugins', []):
        expected.add(plugin['Name'])
plugins = {p.name: p for p in registry.GetAllPlugins()}
assert expected and expected <= plugins.keys(), expected - plugins.keys()
for name in expected:
    plugin = plugins[name]
    assert Path(plugin.resourcePath).resolve().is_relative_to(runtime), (name, plugin.resourcePath)
    assert plugin.Load(), name
with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    for ext in ('usd', 'usda', 'usdc'):
        path = root / ('scene.' + ext)
        stage = Usd.Stage.CreateNew(str(path))
        UsdGeom.Xform.Define(stage, '/World')
        UsdGeom.Cube.Define(stage, '/World/Cube')
        stage.GetRootLayer().Save()
        reopened = Usd.Stage.Open(str(path))
        assert len(list(reopened.Traverse())) == 2
        result = subprocess.run([str(runtime.parents[1] / 'MacOS/Vela'),
                                 '--check-document', str(path)], text=True,
                                capture_output=True, check=True,
                                env={**os.environ, 'DYLD_PRINT_LIBRARIES': '1'})
        assert result.stdout.strip() == '2 prims', result.stdout
        assert str(runtime / 'usd/lib/libusd_ms.dylib') in result.stderr, result.stderr
    package = root / 'scene.usdz'
    assert UsdUtils.CreateNewUsdzPackage(str(root / 'scene.usdc'), str(package))
    reopened = Usd.Stage.Open(str(package))
    assert len(list(reopened.Traverse())) == 2
    result = subprocess.check_output([str(runtime.parents[1] / 'MacOS/Vela'),
                                      '--check-document', str(package)], text=True)
    assert result.strip() == '2 prims', result
    invalid = root / 'invalid.usda'
    invalid.write_text('this is not a USD file')
    failed = subprocess.run([str(runtime.parents[1] / 'MacOS/Vela'),
                             '--check-document', str(invalid)], capture_output=True, text=True)
    assert failed.returncode == 1 and 'Could not open USD stage' in failed.stderr
print(f'Loaded {len(expected)} bundled plugins; opened USD, USDA, USDC and USDZ.')
