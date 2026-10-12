# yaml-cpp (vendored)

| | |
|---|---|
| Version | 0.7.0 (as recommended by OpenColorIO 2.4.2) |
| Source | https://github.com/jbeder/yaml-cpp/archive/refs/tags/yaml-cpp-0.7.0.tar.gz |
| SHA-256 | `43e6a9fcb146ad871515f0d0873947e5d497a1c9c60c58cb102a97b47208b7c3` |
| License | MIT (`LICENSE`) |

Used by OpenColorIO.

## What is included

Copied unmodified from the release tarball: `include/`, `src/` (without
`src/contrib/` and CMakeLists.txt files), `LICENSE`. Tests, utilities and
docs are omitted.

## Local changes

`BUILD.bazel` is ours, adapted from the one shipped with yaml-cpp. It
builds the library as OpenColorIO's `Installyaml-cpp.cmake` does: static,
without contrib. No source files are modified.

## Upgrading

Replace the files listed above from the new release and update this file.
