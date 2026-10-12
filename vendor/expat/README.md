# expat (vendored)

| | |
|---|---|
| Version | 2.8.5 (OpenColorIO 2.4.2 requires >= 2.4.1 and recommends 2.5.0 for CVE fixes; 2.8.5 has later fixes with the same API) |
| Source | https://github.com/libexpat/libexpat/releases/download/R_2_8_5/expat-2.8.5.tar.gz |
| SHA-256 | `920dde485e15eda0cce8d2310b41d492c534e5e3d89ad407a0b4176dd2ff88fe` |
| License | MIT (`COPYING`) |

Used by OpenColorIO.

## What is included

Copied unmodified from the release tarball: `lib/*.c` and `lib/*.h`
(without the Windows-only `random_rand_s.*` and `winconfig.h`),
`expat_config.h.cmake`, `COPYING`. Tools, examples, tests and docs are
omitted.

## Local changes

`BUILD.bazel` is ours. `expat_config.h` is generated from the upstream
template with CMake's default options and the entropy sources its checks
find on each OS. No source files are modified.

## Upgrading

Replace the files listed above from the new release, check
`CMakeLists.txt` and `ConfigureChecks.cmake` for new sources and checks,
and update this file.
