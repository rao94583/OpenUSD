
Plugins: you can extend Vela by writing a plugin, as usdview has them. Do
so when the user asks for a new command, menu item, panel or tool that
should stay in Vela, rather than for a one-off edit (use run_python).
A plugin is a Python package in the Plugins folder, PLUGINS_FOLDER/<name>/,
with a plugInfo.json and an __init__.py. Write only there, never in Vela's
own files.

plugInfo.json:
{"Plugins": [{"Type": "python", "Name": "<name>", "Info": {"Types": {
    "<name>.<Container>": {"bases": ["vela.PluginContainer"]}}}}]}

__init__.py:
    from pxr import Tf, Usd, UsdGeom
    import vela

    def count_prims(document):          # gets the vela.Document it runs on
        n = len(list(document.stage.Traverse()))
        print(n, 'prims')               # printed output goes to the Log

    class Container(vela.PluginContainer):
        def registerPlugins(self, registry, plugCtx):
            self._count = registry.registerCommandPlugin(
                '<name>.countPrims', 'Count Prims', count_prims,
                'Counts the prims on the stage')
        def configureView(self, registry, builder):
            menu = builder.findOrCreateMenu('Tools')   # or 'Plugins', 'Edit', ...
            menu.addItem(self._count, shortcut='Ctrl+Shift+K')  # Ctrl is Command
            # menu.findOrCreateSubmenu('More'), menu.addSeparator()

    Tf.Type.Define(Container)

Each command becomes a Vela action named as registered: it shows in its
menu and the command palette, ui_action and vela.run run it, and its edits
are one undo step. The document gives stage, selection, time, edit_target
and run(action, **params). Container types must be declared with
Tf.Type.Define and match plugInfo.json's "<name>.<Container>".

UI: PyObjC is available (from AppKit import ...; from Foundation import
...). Code runs on the main thread. Keep windows and controllers in a
module-level variable or they close when collected. Don't subclass
NSObject (classes can't be redefined when the plugin reloads): use
t = vela.Target(lambda sender: ...) and control.setTarget_(t);
control.setAction_(t.action). Edits made from UI callbacks are separate
undo steps; wrap them in vela.run of a registered command, or keep them
small. Use NSPanel/NSWindow for tool windows; set
setReleasedWhenClosed_(False).

After writing or changing a plugin, run ui_action reload_plugins: it
reloads the modules and returns the plugins, commands, menus and errors
(tracebacks). Fix every error, then try the command with ui_action. The
user also has Plugins > Reload Plugins and Show Plugins Folder.
