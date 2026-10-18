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

__all__ = ['ActionError', 'CommandPlugin', 'Target', 'Document', 'PluginContainer',
           'PluginMenu', 'PluginRegistry', 'PluginUIBuilder', 'actions', 'current',
           'documents', 'open', 'run']


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


# Plugins, as usdview has them (pxr/usdImaging/usdviewq/plugin.py), so
# usdview plugins port by changing their imports. A plugin is a Python
# package with a plugInfo.json that declares a container type derived from
# vela.PluginContainer; Vela finds it through USD's plugin registry, in
# ~/Library/Application Support/Vela/Plugins or on PXR_PLUGINPATH_NAME:
#
#   {"Plugins": [{"Type": "python", "Name": "myPlugin",
#                 "Info": {"Types": {"myPlugin.MyContainer": {
#                     "bases": ["vela.PluginContainer"]}}}}]}
#
# Each command plugin becomes a Vela action (menus, the palette, vela.run,
# Claude); its callback gets the vela.Document it runs on, and its edits are
# one undo step. PyObjC (AppKit, Foundation) is available for UI.


class DuplicateCommandPlugin(Exception):
    """Two command plugins were registered with the same name."""

    def __init__(self, name):
        super().__init__(
            "A command plugin with the name '{}' has already been registered.".format(name))
        self.name = name


class DeferredImport:
    """Imports a module the first time one of its functions is called, as
    usdview's does: container.deferredImport('.commands').doThing."""

    def __init__(self, moduleName, packageName=None):
        self._moduleName = moduleName
        self._packageName = packageName
        self._module = None

    def __getattr__(self, attr):
        def f(*args, **kwargs):
            if self._module is None:
                import importlib
                self._module = importlib.import_module(self._moduleName, package=self._packageName)
            return getattr(self._module, attr)(*args, **kwargs)
        return f


class PluginContainer:
    """Holds some plugins: subclass it, define registerPlugins and
    configureView, and declare the subclass with Tf.Type.Define."""

    def deferredImport(self, moduleName):
        return DeferredImport(moduleName, self.__module__)

    def registerPlugins(self, plugRegistry, plugCtx):
        """Called once the container is found: call
        plugRegistry.registerCommandPlugin for each command. plugCtx is the
        vela module."""
        raise NotImplementedError

    def configureView(self, plugRegistry, plugUIBuilder):
        """Called after registerPlugins: add the commands to menus with
        plugUIBuilder.findOrCreateMenu(name).addItem(command)."""
        raise NotImplementedError


def _define_container_type():
    from pxr import Tf
    return Tf.Type.Define(PluginContainer)


# Defined at once, so plugins' containers can derive from it as they import.
_PluginContainerTfType = _define_container_type()


class CommandPlugin:
    """A command: callback(document) runs it on a vela.Document."""

    def __init__(self, name, displayName, callback, description):
        self._name = name
        self._displayName = displayName
        self._callback = callback
        self._description = description

    @property
    def name(self):
        return self._name

    @property
    def displayName(self):
        return self._displayName

    @property
    def description(self):
        return self._description

    def run(self, document=None):
        """Runs the callback on document (the current one by default)."""
        return self._callback(document or _require_current())


class PluginRegistry:
    """The commands plugins register."""

    def __init__(self):
        self._commandPlugins = {}

    def registerCommandPlugin(self, name, displayName, callback, description=''):
        """Registers and returns a command. Prefix name with the container's
        name (e.g. 'MyContainer.doThing'); it becomes the action's name."""
        if name in self._commandPlugins:
            raise DuplicateCommandPlugin(name)
        plugin = CommandPlugin(name, displayName, callback, description)
        self._commandPlugins[name] = plugin
        return plugin

    def getCommandPlugin(self, name):
        return self._commandPlugins.get(name)


class PluginMenu:
    """A menu (or submenu) that commands are added to."""

    def __init__(self, builder, path):
        self._builder = builder
        self._path = list(path)

    def addItem(self, commandPlugin, shortcut=None):
        """Adds a command. shortcut is like usdview's: 'Ctrl+Shift+P' (Ctrl
        is Command, Meta is Control, Alt is Option)."""
        self._builder._entries.append({'path': self._path, 'command': commandPlugin.name,
                                       'shortcut': shortcut or ''})

    def findOrCreateSubmenu(self, menuName):
        return PluginMenu(self._builder, self._path + [menuName])

    def addSeparator(self):
        self._builder._entries.append({'path': self._path, 'separator': True})


class PluginUIBuilder:
    """Builds plugins' menus: the menu bar's own menus (by title) or new
    ones."""

    def __init__(self):
        self._entries = []

    def findOrCreateMenu(self, menuName):
        return PluginMenu(self, [menuName])


_plugin_registry = PluginRegistry()

_target_class = None
_targets = []


def Target(callback):
    """An AppKit target that calls callback(sender), for buttons, menu
    items and other controls in plugin UI:

        control.setTarget_(target); control.setAction_(target.action)

    Objective-C classes can't be defined again, so plugins (which reload)
    should use this rather than their own NSObject subclasses. Targets live
    as long as Vela does."""
    global _target_class
    import objc
    from Foundation import NSObject
    if _target_class is None:
        class VelaPythonTarget(NSObject):
            def call_(self, sender):
                try:
                    self.callback(sender)
                except BaseException:
                    traceback.print_exc()
        _target_class = VelaPythonTarget
    target = _target_class.alloc().init()
    target.callback = callback
    target.action = 'call:'
    _targets.append(target)
    return target


def _plugin_module_names(name):
    return sorted((m for m in sys.modules if m == name or m.startswith(name + '.')),
                  key=lambda m: m.count('.'), reverse=True)


def _load_plugins(folders, reload_modules):
    """Finds and loads plugins; returns for the app: plugins, commands, menu
    entries and errors (tracebacks)."""
    global _plugin_registry
    import importlib
    import os
    from pxr import Plug, Tf
    errors = []
    registry = Plug.Registry()
    for folder in folders:
        if os.path.isdir(folder):
            # Packages directly in the folder import by name.
            if folder not in sys.path:
                sys.path.insert(0, folder)
            registry.RegisterPlugins(os.path.join(folder, '*', ''))
    importlib.invalidate_caches()

    by_plugin = {}
    for containerType in Plug.Registry.GetAllDerivedTypes(_PluginContainerTfType):
        plugin = registry.GetPluginForType(containerType)
        if plugin is not None:
            by_plugin.setdefault(plugin, []).append(containerType)

    containers, names = [], []
    original_define = Tf.Type.Define
    for plugin in sorted(by_plugin, key=lambda p: p.name):
        # A plugin whose files are gone (deleted since launch).
        if plugin.path and not os.path.exists(plugin.path):
            continue
        # A Python plugin's package imports from the folder it is in (usdview
        # leaves that to PYTHONPATH, which Vela's Python doesn't read).
        if plugin.isPythonModule and plugin.path:
            parent = os.path.dirname(os.path.normpath(plugin.path))
            if parent not in sys.path:
                sys.path.insert(0, parent)
        try:
            if reload_modules and plugin.isLoaded and plugin.isPythonModule:
                # Defining the same Tf types again is fine when reloading.
                def define(cls, _define=original_define):
                    try:
                        return _define(cls)
                    except Exception:
                        return Tf.Type.FindByName(cls.__module__ + '.' + cls.__name__)
                Tf.Type.Define = define
                try:
                    for name in _plugin_module_names(plugin.name):
                        importlib.reload(sys.modules[name])
                finally:
                    Tf.Type.Define = original_define
            # Importing first gives Python's own traceback on failure (Plug's
            # Load only says that loading failed).
            if plugin.isPythonModule and plugin.name not in sys.modules:
                importlib.import_module(plugin.name)
            plugin.Load()
        except BaseException:
            errors.append('Plugin %s failed to load:\n%s' % (plugin.name, traceback.format_exc()))
            continue
        names.append(plugin.name)
        for containerType in sorted(by_plugin[plugin], key=lambda t: t.typeName):
            moduleName, _, className = containerType.typeName.rpartition('.')
            cls = getattr(sys.modules.get(moduleName), className, None) or containerType.pythonClass
            if cls is None:
                errors.append("Plugin %s: no container class '%s'. Is it defined with "
                              "Tf.Type.Define, and does its module path match plugInfo.json?"
                              % (plugin.name, containerType.typeName))
                continue
            try:
                containers.append(cls())
            except BaseException:
                errors.append('Plugin %s: %s() failed:\n%s'
                              % (plugin.name, className, traceback.format_exc()))

    commands = PluginRegistry()
    builder = PluginUIBuilder()
    this = sys.modules[__name__]
    for step in ('registerPlugins', 'configureView'):
        for container in list(containers):
            try:
                if step == 'registerPlugins':
                    container.registerPlugins(commands, this)
                else:
                    container.configureView(commands, builder)
            except BaseException:
                errors.append('%s.%s failed:\n%s' % (type(container).__name__, step,
                                                       traceback.format_exc()))
                if step == 'registerPlugins':
                    containers.remove(container)
    _plugin_registry = commands
    return {
        'plugins': names,
        'commands': [{'name': c.name, 'title': c.displayName, 'description': c.description}
                     for c in commands._commandPlugins.values()],
        'menus': builder._entries,
        'errors': errors,
    }


def _run_plugin_command(name, document_id):
    """Runs a command on a document; returns (ok, output, error)."""
    output = io.StringIO()
    command = _plugin_registry.getCommandPlugin(name)
    if command is None:
        return False, '', 'No plugin command named %s' % name
    try:
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            command.run(Document(document_id))
        return True, output.getvalue(), None
    except BaseException as error:
        return False, output.getvalue(), ''.join(traceback.format_exception(
            type(error), error, error.__traceback__))


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


def _call(name, args_json):
    """The app calls module functions with JSON arguments and result."""
    return json.dumps(globals()[name](*json.loads(args_json)))


def _is_complete(code):
    """Whether the console should run code on Return: it is a complete
    statement, or invalid (running shows the error)."""
    import codeop
    try:
        return codeop.compile_command(code, '<input>', 'single') is not None
    except (SyntaxError, ValueError, OverflowError):
        return True
