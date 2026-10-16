"""Scripting Vela: its open documents, their stages and its actions.

vela.stage, vela.selection and vela.time belong to the current document:
the Python console's own, or the front window's for scripts that arrive
over MCP. The stage is the one the window shows, not a copy, so edits made
with the USD API appear at once; a script's edits are one undo step.

Everything the menus do is an action, run by name with JSON-style
parameters: vela.run('frame_selection'), vela.run('set_draw_mode',
mode='wireframe'). vela.actions() describes them all.
"""

import ast
import contextlib
import io
import json
import linecache
import sys
import traceback
import types

import _vela
from pxr import Sdf, Usd, UsdUtils

__all__ = ['ActionError', 'Document', 'actions', 'current', 'documents',
           'open', 'run']


class ActionError(RuntimeError):
    """An action failed or does not exist; the message says why."""


def _to_json(value):
    if isinstance(value, Sdf.Path):
        return str(value)
    try:
        return list(value)  # Gf vectors, Vt arrays, tuples
    except TypeError:
        return str(value)


class Document:
    """An open document. Documents compare equal by id."""

    def __init__(self, id):
        self.id = id

    def __repr__(self):
        return 'vela.Document(%d, %r)' % (self.id, self.name)

    def __eq__(self, other):
        return isinstance(other, Document) and other.id == self.id

    def __hash__(self):
        return hash(self.id)

    def _header(self):
        for entry in json.loads(_vela.documents()):
            if entry['id'] == self.id:
                return entry
        raise LookupError('Document %d is closed' % self.id)

    @property
    def name(self):
        """The window's title."""
        return self._header()['name']

    @property
    def path(self):
        """The root layer's file."""
        return self._header()['path']

    @property
    def stage(self):
        """The document's Usd.Stage, shared with the window."""
        cache_id = Usd.StageCache.Id.FromLongInt(_vela.stage_cache_id(self.id))
        return UsdUtils.StageCache.Get().Find(cache_id)

    def state(self):
        """Selection, time, edit target, layers, camera and undo state."""
        return json.loads(_vela.state(self.id))

    @property
    def selection(self):
        """Selected prim paths, as Sdf.Path; assign paths or prims."""
        return [Sdf.Path(p) for p in self.state()['selection']]

    @selection.setter
    def selection(self, paths):
        if isinstance(paths, (str, Sdf.Path, Usd.Prim)):
            paths = [paths]
        self.run('select', paths=[str(p.GetPath() if isinstance(p, Usd.Prim) else p)
                                  for p in paths])

    @property
    def time(self):
        """The current time code, as a Usd.TimeCode; assign a number."""
        time = self.state()['time']
        if time == 'default':
            return Usd.TimeCode.Default()
        if time == 'earliest':
            return Usd.TimeCode.EarliestTime()
        return Usd.TimeCode(time)

    @time.setter
    def time(self, time):
        if isinstance(time, Usd.TimeCode):
            time = time.GetValue()
        self.run('set_time', time=float(time))

    @property
    def edit_target(self):
        """The layer edits go to, as an Sdf.Layer."""
        return Sdf.Layer.Find(self.state()['editTarget'])

    def run(self, action, **params):
        """Runs an action on this document and returns its result.

        Raises ActionError when it fails."""
        text = _vela.run(self.id, action, json.dumps(params, default=_to_json))
        result = json.loads(text)
        if not result['ok']:
            raise ActionError(result['error'])
        return result.get('value')


def documents():
    """Every open document."""
    return [Document(entry['id']) for entry in json.loads(_vela.documents())]


def current():
    """The current document, or None when none is open."""
    id = _vela.current()
    return Document(id) if id else None


def _require_current():
    document = current()
    if document is None:
        raise LookupError('No document is open')
    return document


def run(action, **params):
    """Runs an action on the current document; see Document.run."""
    return _require_current().run(action, **params)


def actions():
    """Every action: name, title, description, parameter schema, menu."""
    return json.loads(_vela.describe())


def open(path):
    """Opens a USD file in a new window and returns its document."""
    return Document(_vela.open(str(path)))


class _Module(types.ModuleType):
    """Module properties for the current document's state."""

    @property
    def document(self):
        """The current document, or None."""
        return current()

    @property
    def stage(self):
        """The current document's stage."""
        return _require_current().stage

    @property
    def selection(self):
        return _require_current().selection

    @selection.setter
    def selection(self, paths):
        _require_current().selection = paths

    @property
    def time(self):
        return _require_current().time

    @time.setter
    def time(self, time):
        _require_current().time = time


sys.modules[__name__].__class__ = _Module


# The app's entry points: namespaces for the console and for MCP, and
# running code in them as an interactive interpreter would.

_namespaces = {}


def _namespace(key):
    namespace = _namespaces.get(key)
    if namespace is None:
        from pxr import Gf, Kind, Tf, UsdGeom, UsdLux, UsdShade, Vt
        namespace = {
            '__name__': '__main__',
            '__builtins__': __builtins__,
            'vela': sys.modules[__name__],
            'Gf': Gf, 'Kind': Kind, 'Sdf': Sdf, 'Tf': Tf, 'Usd': Usd,
            'UsdGeom': UsdGeom, 'UsdLux': UsdLux, 'UsdShade': UsdShade,
            'UsdUtils': UsdUtils, 'Vt': Vt,
        }
        _namespaces[key] = namespace
    return namespace


_inputs = 0


def _execute(code, key):
    """Runs code in the namespace for key. Returns (ok, output, value):
    the printed output, and the last expression's repr (or None), or on
    failure the traceback."""
    global _inputs
    _inputs += 1
    filename = '<input-%d>' % _inputs
    linecache.cache[filename] = (len(code), None, code.splitlines(True), filename)
    namespace = _namespace(key)
    output = io.StringIO()
    try:
        tree = ast.parse(code, filename, 'exec')
        last = None
        if tree.body and isinstance(tree.body[-1], ast.Expr):
            last = ast.Expression(tree.body.pop().value)
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            exec(compile(tree, filename, 'exec'), namespace)
            value = None
            if last is not None:
                result = eval(compile(last, filename, 'eval'), namespace)
                if result is not None:
                    namespace['_'] = result
                    value = repr(result)
        return True, output.getvalue(), value
    except BaseException as error:
        tb = error.__traceback__
        # Leave out this function's frame.
        while tb is not None and tb.tb_frame.f_code.co_filename == '<vela>':
            tb = tb.tb_next
        text = ''.join(traceback.format_exception(type(error), error, tb))
        return False, output.getvalue(), text


def _is_complete(code):
    """Whether the console should run code on Return: it is a complete
    statement, or invalid (running shows the error)."""
    import codeop
    try:
        return codeop.compile_command(code, '<input>', 'single') is not None
    except (SyntaxError, ValueError, OverflowError):
        return True
