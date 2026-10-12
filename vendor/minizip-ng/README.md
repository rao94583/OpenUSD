# minizip-ng (vendored)

| | |
|---|---|
| Version | 3.0.7 (as recommended by OpenColorIO 2.4.2) |
| Source | https://github.com/zlib-ng/minizip-ng/archive/refs/tags/3.0.7.tar.gz |
| SHA-256 | `39981a0db1bb6da504909bce63d7493286c5e50825c056564544c990d15c55cf` |
| License | Zlib (`LICENSE`) |

Used by OpenColorIO (for .ocioz archives). Requires `//vendor/zlib`.

## What is included

Copied unmodified from the release tarball: all `mz*.h` headers, the
sources OpenColorIO's configuration compiles (core, zlib stream, POSIX
I/O), and `LICENSE`. Other compression backends, crypto, the compat layer, tools and
tests are omitted.

## Local changes

`BUILD.bazel` is ours; its definitions are those `CMakeLists.txt` sets for
the options in OpenColorIO's `Installminizip-ng.cmake`. No source files are
modified.

## Upgrading

Replace the files listed above from the new release, re-check
`CMakeLists.txt` for those options, and update this file.
