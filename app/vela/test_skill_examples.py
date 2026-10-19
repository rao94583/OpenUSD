"""Runs every ```python example in the Claude skills against the living room
scene, each on a fresh copy, in a namespace like run_python's.

    test_skill_examples.py SCENE SKILL_FILE...
"""

import re
import sys
import types
import unittest

from pxr import Gf, Kind, Sdf, Tf, Usd, UsdGeom, UsdLux, UsdShade, UsdUtils, Vt

SCENE = sys.argv[1]
FILES = sys.argv[2:]
BLOCK = re.compile(r"^```python\n(.*?)^```", re.M | re.S)


def namespace():
    stage = Usd.Stage.Open(Sdf.Layer.OpenAsAnonymous(SCENE))
    vela = types.SimpleNamespace(stage=stage, selection=[], time=Usd.TimeCode.Default())
    return {"__name__": "__main__", "vela": vela, "Gf": Gf, "Kind": Kind, "Sdf": Sdf,
            "Tf": Tf, "Usd": Usd, "UsdGeom": UsdGeom, "UsdLux": UsdLux,
            "UsdShade": UsdShade, "UsdUtils": UsdUtils, "Vt": Vt}


class SkillExamples(unittest.TestCase):
    def test_examples(self):
        count = 0
        for path in FILES:
            with open(path) as f:
                text = f.read()
            for match in BLOCK.finditer(text):
                line = text[: match.start()].count("\n") + 2
                with self.subTest(file=path.rsplit("/", 3)[-3:], line=line):
                    diagnostics = []
                    delegate = UsdUtils.CoalescingDiagnosticDelegate()
                    exec(compile(match.group(1), "%s:%d" % (path, line), "exec"), namespace())
                    errors = [d.commentary for d in delegate.TakeUncoalescedDiagnostics()
                              if d.diagnosticCode in (Tf.TF_DIAGNOSTIC_CODING_ERROR_TYPE,
                                                      Tf.TF_DIAGNOSTIC_RUNTIME_ERROR_TYPE)]
                    self.assertEqual(errors, [], "USD reported errors")
                count += 1
        self.assertGreater(count, 10)


if __name__ == "__main__":
    unittest.main(argv=sys.argv[:1])
