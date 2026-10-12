# libdeflate (vendored)

| | |
|---|---|
| Version | 1.18 (as pinned by OpenEXR 3.3.6, `OPENEXR_DEFLATE_TAG`) |
| Source | https://github.com/ebiggers/libdeflate/archive/refs/tags/v1.18.tar.gz |
| SHA-256 | `225d982bcaf553221c76726358d2ea139bb34913180b20823c782cede060affd` |
| License | MIT (`COPYING`) |

Used by OpenEXR (`//vendor/openexr`).

## What is included

Copied unmodified from the release tarball: `lib/`, `common_defs.h`,
`libdeflate.h`, `COPYING`. Programs and scripts are omitted.

## Local changes

`BUILD.bazel` is ours. No source files are modified.

## Upgrading

Replace the files listed above from the new release, check
`CMakeLists.txt` for new sources, and update this file.
