# {fmt} (vendored)

| | |
|---|---|
| Version | 10.2.1 (as pinned by OpenImageIO 3.1.8.0, `src/cmake/build_fmt.cmake`) |
| Source | https://github.com/fmtlib/fmt/archive/refs/tags/10.2.1.tar.gz |
| SHA-256 | `1250e4cc58bf06ee631567523f48848dc4596133e163f02615c97f78bab6c811` |
| License | MIT (`LICENSE`) |

Header-only, used by OpenImageIO.

## What is included

Copied unmodified from the release tarball: `include/fmt/` and `LICENSE`.
Sources (only needed for the compiled library), tests and docs are
omitted.

## Local changes

`BUILD.bazel` is ours (header-only, like CMake's `fmt::fmt-header-only`).
No source files are modified.

## Upgrading

Replace the files listed above from the new release and update this file.
