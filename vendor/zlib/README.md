# zlib (vendored)

| | |
|---|---|
| Version | 1.3.2 (as pinned by `build_scripts/build_usd.py`) |
| Source | https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz |
| SHA-256 | `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16` |
| License | Zlib (`LICENSE`) |

Used by the OpenImageIO stack (libpng, libtiff, OpenEXR, minizip-ng,
OpenImageIO), which provides `idiff` for image-comparison tests.

## What is included

Copied unmodified from the release tarball: the top-level `*.c` and `*.h`
files and `LICENSE`. Contributions, examples, tests and platform ports are
omitted.

## Local changes

`BUILD.bazel` is ours, adapted from the one shipped with zlib. No source
files are modified.

## Upgrading

Replace the files listed above from the new release and update this file.
