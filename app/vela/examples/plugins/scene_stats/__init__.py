from collections import Counter

from pxr import Tf, Usd
import vela

from AppKit import (NSPanel, NSScrollView, NSTextView, NSButton, NSFont,
                    NSBackingStoreBuffered, NSBezelStyleRounded,
                    NSWindowStyleMaskTitled, NSWindowStyleMaskClosable,
                    NSWindowStyleMaskResizable, NSWindowStyleMaskUtilityWindow,
                    NSViewWidthSizable, NSViewHeightSizable, NSViewMinXMargin,
                    NSViewMaxYMargin)
from Foundation import NSMakeRect

# Module-level references keep the window and its target alive.
_state = {'panel': None, 'text': None, 'target': None, 'document': None}


def _stats_text(stage):
    counts = Counter(p.GetTypeName() or '(untyped)'
                     for p in stage.Traverse(Usd.TraverseInstanceProxies()))
    total = sum(counts.values())
    width = max([len(t) for t in counts] + [5])
    lines = ['%-*s  %6d' % (width, t, n)
             for t, n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))]
    lines += ['-' * (width + 8), '%-*s  %6d' % (width, 'Total', total)]
    return '\n'.join(lines)


def _refresh(sender=None):
    doc, text = _state['document'], _state['text']
    if doc is None or text is None:
        return
    stage = doc.stage
    text.setString_(_stats_text(stage) if stage else 'No stage')
    _state['panel'].setTitle_('Scene Stats — %s' % (
        stage.GetRootLayer().GetDisplayName() if stage else ''))


def _build_panel():
    w, h = 340, 420
    style = (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
             NSWindowStyleMaskResizable | NSWindowStyleMaskUtilityWindow)
    panel = NSPanel.alloc().initWithContentRect_styleMask_backing_defer_(
        NSMakeRect(200, 300, w, h), style, NSBackingStoreBuffered, False)
    panel.setReleasedWhenClosed_(False)
    panel.setHidesOnDeactivate_(False)
    content = panel.contentView()

    scroll = NSScrollView.alloc().initWithFrame_(NSMakeRect(0, 44, w, h - 44))
    scroll.setHasVerticalScroller_(True)
    scroll.setAutoresizingMask_(NSViewWidthSizable | NSViewHeightSizable)
    text = NSTextView.alloc().initWithFrame_(NSMakeRect(0, 0, w, h - 44))
    text.setEditable_(False)
    text.setFont_(NSFont.monospacedSystemFontOfSize_weight_(12, 0))
    text.setTextContainerInset_((8, 8))
    text.setAutoresizingMask_(NSViewWidthSizable)
    scroll.setDocumentView_(text)
    content.addSubview_(scroll)

    target = vela.Target(_refresh)
    button = NSButton.alloc().initWithFrame_(NSMakeRect(w - 110, 8, 100, 28))
    button.setTitle_('Refresh')
    button.setBezelStyle_(NSBezelStyleRounded)
    button.setTarget_(target)
    button.setAction_(target.action)
    button.setAutoresizingMask_(NSViewMinXMargin | NSViewMaxYMargin)
    content.addSubview_(button)

    _state.update(panel=panel, text=text, target=target)


def show_scene_stats(document):
    if _state['panel'] is None:
        _build_panel()
    _state['document'] = document
    _refresh()
    _state['panel'].makeKeyAndOrderFront_(None)


class Container(vela.PluginContainer):
    def registerPlugins(self, registry, plugCtx):
        self._stats = registry.registerCommandPlugin(
            'scene_stats.show', 'Scene Stats', show_scene_stats,
            'Shows how many prims of each type the stage has')

    def configureView(self, registry, builder):
        builder.findOrCreateMenu('Tools').addItem(self._stats)


Tf.Type.Define(Container)
