# Offline PR #1905 comparison probes

This tool reproduces the source-function observations in the
[upstream comparison report](../../../../docs/kaffeine/upstream-pr1905-comparison.md).
It compares the original upstream PR revision with Kaffeine's checked
accessors and private MemHeader adapter. Additional probes cover allocation
overflow, custom-register address decoding and classic library-version
arguments.

## Inputs and execution

Use Python **3.9+** on a POSIX host with a C++ compiler providing ASan/UBSan.
The recorded run used macOS arm64. The tool needs a local source snapshot
of `terminills/AROS` at
`e016d6fb7999eb3c7962f078e5ff75d50f957079`, plus the Kaffeine source tree.
It performs no network requests and builds no AROS binary.

Only these five upstream source files are required, under their normal
repository paths: `m68kemu_intern.h`, `m68kemu_thunks.c`,
`m68kemu_moira.cpp`, `m68kemu_offsets.h` and `m68kemu_memory.c` in
`rom/m68kemu/`. Their SHA-256 hashes are checked before execution. No NDK,
ROM, application binary, AROS SDK or Moira submodule is needed for these
isolated probes.

From the Kaffeine repository root:

```sh
python3 developer/debug/test/kaffeine_upstream_compare/compare_pr1905.py \
    --upstream-root /path/to/aros-pr1905 \
    --build-dir /tmp/pr1905-probes
```

The output directory must be new or empty. `--kaffeine-root` selects a
different Kaffeine checkout; its default is this repository. `--cxx`
selects the compiler command. Generated source, executables, build/runtime
logs and `probe-results.json` stay in the output directory. Keep that
directory outside the checkout.

The baseline Kaffeine revision is
`6fc72c263fa64341c517e4b6d3ec419bd59ae926`. Later revisions can change the
comparison and need a new report. A different upstream source hash is
rejected rather than silently presenting results for the original PR.

## What successful completion means

**Exit status zero means that all 28 documented observations were
reproduced. It does not mean that the upstream branch passed all contracts.**
The reference observations contain 13 upstream failures, five upstream
positive controls and ten Kaffeine controls/boundary passes. Four upstream
wrapping-access cases terminate under the sanitizer in separate child
processes; the supervising script records those expected failures.
Leak detection is disabled for these runs.

Functions are extracted verbatim, with a digest recorded for each extract.
Kaffeine's private MemHeader implementation is included from its source
file behind fixture declarations. Contexts, CPU register access and native
lookups are controlled collaborators. The custom-register callback probe
supplies POTGOR's documented value through a fixture and does not link the
full Moira engine. The library probes exercise an already-populated 68k
cache after native/class lookup declines the name; they do not load a disk
library.

These probes establish the reported host function behavior. They do not
establish a full upstream build, AROS runtime behavior, application
compatibility, JIT behavior or hardware acceptance. The source test
algorithms are Kaffeine/AROS development work; extracted functions retain
the provenance and licensing of their respective source files.
