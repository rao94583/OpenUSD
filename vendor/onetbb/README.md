# oneTBB (vendored)

| | |
|---|---|
| Version | 2021.12.0 (matches `VERSIONS.md`) |
| Source | https://github.com/uxlfoundation/oneTBB/archive/refs/tags/v2021.12.0.tar.gz |
| SHA-256 | `c7bb7aa69c254d91b8f0041a71c5bcc3936acb64408a1719aec0b2b7639dd84f` |
| License | Apache-2.0 (`LICENSE.txt`) |

## What is included

Copied unmodified from the release tarball: `include/`, `src/tbb/`,
`src/tbbmalloc/`, `src/tbbmalloc_proxy/`, `LICENSE.txt`,
`third-party-programs.txt`. Tests, docs, examples, and Python bindings are
omitted.

## Local changes

`BUILD.bazel` is ours, adapted from upstream's community-maintained one. No
source files are modified.

## Upgrading

Replace the directories listed above from the new release, update this file,
and diff upstream's `BUILD.bazel` for new sources or defines.
