# MarkupSafe (vendored)

| | |
|---|---|
| Version | 3.0.3 (Jinja2 3.1.6 requires MarkupSafe >= 2.0) |
| Source | https://files.pythonhosted.org/packages/7e/99/7690b6d4034fffd95959cbe0c02de8deb3098cc577c67bb6a24fe5d7caa7/markupsafe-3.0.3.tar.gz |
| SHA-256 | `722695808f4b6457b320fdc131280796bdceb04ab50fe1795cd540799ebe1698` |
| License | BSD-3-Clause (`LICENSE.txt`) |

## What is included

Copied unmodified from the PyPI source distribution:
`src/markupsafe/__init__.py`, `_native.py`, `py.typed`, and `LICENSE.txt`.

The optional C extension (`_speedups.c`, `_speedups.pyi`) is omitted;
`markupsafe/__init__.py` falls back to the pure-Python `_native.py` when it
is missing, which is fast enough for code generation. Tests, docs and
packaging files are omitted.

## Local changes

`BUILD.bazel` is ours. No source files are modified.

## Upgrading

Replace the files listed above from the new sdist and update this file.
