"""The main menu is built from the action registry: every action with a menu
appears in it, under that menu, with its shortcut."""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

KEYS = {'Left': '', 'Right': '', 'Space': ' '}
FLAGS = {'Shift': 1 << 17, 'Control': 1 << 18, 'Option': 1 << 19, 'Command': 1 << 20}

with tempfile.TemporaryDirectory() as temp:
    subprocess.run(['/usr/bin/ditto', '-x', '-k', sys.argv[1], temp], check=True)
    vela, = Path(temp).glob('*.app/Contents/MacOS/Vela')
    actions = json.loads(subprocess.check_output([vela, '--describe-actions']))
    menus = subprocess.check_output([vela, '--describe-menus'], text=True)

items = {}
menu = None
for line in menus.splitlines():
    if not line.startswith('  '):
        menu = line
        continue
    match = re.match(r'  .* \[(\w+)\](?: key=(.) mods=(\d+))?$', line)
    if match:
        items[match[1]] = (menu, match[2], int(match[3] or 0))

for action in actions:
    name = action['name']
    assert action['description'], name
    assert action['parameters']['type'] == 'object', name
    if 'menu' not in action:
        assert name not in items, f'{name} is in a menu but names none'
        continue
    assert name in items, f'{name} is missing from the {action["menu"]} menu'
    menu, key, mods = items[name]
    assert menu == action['menu'], (name, menu)
    if 'shortcut' in action:
        *modifiers, base = action['shortcut'].split('+')
        assert key == KEYS.get(base, base.lower()), (name, key)
        assert mods == sum(FLAGS[m] for m in modifiers), (name, mods)
assert len(items) == sum('menu' in a for a in actions), 'unknown items in menus'
print(f'{len(items)} menu items match {len(actions)} actions')
