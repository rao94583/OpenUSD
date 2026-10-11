# Jinja2 (vendored)

| | |
|---|---|
| Version | 3.1.6 (`VERSIONS.md` lists 3.1.2; 3.1.6 is the same API with later security fixes) |
| Source | https://files.pythonhosted.org/packages/df/bf/f7da0350254c0ed7c72f3e33cef02e048281fec7ecec5f032d4aac52226b/jinja2-3.1.6.tar.gz |
| SHA-256 | `0137fb05990d35f1275a587e9aee6d56da821fc83491a0fb838183be43f66d6d` |
| License | BSD-3-Clause (`LICENSE.txt`) |

Used only by the schema code generators (`usdGenSchema`, `usdInitSchema`,
`usdgenschemafromsdr`); CMake builds them only when Jinja2 is importable
(`cmake/modules/FindJinja2.cmake`). Requires `//vendor/markupsafe`.

## What is included

Copied unmodified from the PyPI source distribution: `src/jinja2/`,
`LICENSE.txt`. Tests, docs and packaging files are omitted.

## Local changes

`BUILD.bazel` is ours. No source files are modified.

## Upgrading

Replace `src/jinja2/` from the new sdist, check its `MarkupSafe`
requirement in `pyproject.toml`, and update this file.
