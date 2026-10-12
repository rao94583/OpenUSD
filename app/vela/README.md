# Vela

Phase 1 native macOS document viewer. Requires macOS 13+, Xcode and the
repository's Bazel toolchain. It opens `.usd`, `.usda`, `.usdc` and `.usdz`
and displays the number of active, loaded, defined prims (USD's default
traversal). File → Open and Finder document opening use `NSDocument`.

```sh
bazel build //app/vela:archive
bazel test //app/vela:bundle_smoke_test
```

Extract `bazel-bin/app/vela/archive.zip` with Archive Utility or `ditto -x -k`,
then open `Vela.app`. The app can be moved independently of the checkout.
The local bundle identifier is `org.openusd.vela`; release branding,
notarization and distribution signing remain open decisions.

The bundle contains the existing monolithic USD install layout and the
hermetic Python 3.11 runtime under `Contents/Resources/Runtime`. The native
app and Python extensions link the same bundled `libusd_ms.dylib`. A
compilation-only header interface prevents statically linking another copy
of USD into the app. Runtime copies dereference Bazel symlinks; nested
Mach-O files are ad-hoc signed before the outer app is signed.

The smoke test extracts the signed archive into a temporary directory with
spaces, removes Python/USD/dyld environment overrides, verifies signatures,
and runs the bundled interpreter in isolated mode. It checks each plugin
manifest against the registry, loads every plugin, and creates and opens all
four supported formats through Python and the native document reader. It
verifies the bundle signature again after execution. The native reader's
`--check-document PATH` mode uses the same document code as the window and
prints the prim count without starting the AppKit event loop.

Python is bundled and tested as an interpreter subprocess in this phase.
The in-process interpreter, live `vela.stage` API and console are Phase 6.
The smoke test does not automate Finder or visually inspect the window.
Offline builds require refreshing `bazel vendor` for the new Apple rules.
