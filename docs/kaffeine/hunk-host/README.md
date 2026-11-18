# m68kemu: HUNK and 68k library loading, and the lifetime of a child's seglist copy

A local patch series for `rom/m68kemu` and its host regressions, against AROS
`10ff84d828ff9a074b7d74fb753e3bd731a421a5` (module tree `d91f7a57`). It has not
been submitted.

## What it changes

Module (`rom/m68kemu`):

- `m68kemu_memory.c`, `m68kemu_intern.h`: the hunk file loader
  (`M68KEmu_LoadHunksFromMemory`) refuses malformed files instead of reading or
  writing outside containment. The copy of a `LoadSeg()` seglist into
  containment (`M68KEmu_LoadHunks`) gets the helpers to walk a placed image
  (`M68KEmu_ImageHunk`) and to give a copy back (`M68KEmu_FreeImage`,
  `M68KEmu_FreeHunks`).
- `m68kemu_m68klib.c`: the 68k disk library loader checks the Resident, the
  init table and the function table against the hunk, the base and
  containment, and when it refuses a library it gives back every block that
  attempt took and nothing else (`m68k_library_abandon`).
- `m68kemu_tasks.c`: a 68k child started from a `LoadSeg()` seglist runs on a
  copy, and the copy now has an owner.
  - When the child cannot be started, the copy is given back at once.
  - When it is cancelled because no m68k Process can be made for it, the copy
    is given back inside the call.
  - When the first active `NP_FreeSeglist` is TRUE (the one dos goes by, and
    the `CreateNewProc` default), the child gives it back after its entry and
    exit code, holding the program lock, before its stack.
  - With `NP_FreeSeglist` FALSE, and always for `CreateProc`, the copy stays
    until the program ends, since the program may have published addresses
    inside it.
  - Every `NP_FreeSeglist` tag is still passed on to dos in its order.
- `m68kemu_shadow.c`: the native offsets of the ExecBase lists come from
  `offsetof()` instead of fixed x86_64 numbers. This part does not depend on
  the rest.
- `README.md`, `INTERNALS.md`: the copy's lifetime, the new regressions and
  the limits that remain.

Host regressions (`developer/debug/test/m68kemu_host`):

- New: `run_library_loading_bounds.py` with `test_library_loading_bounds.cpp`,
  `run_hunk_file_loader.py` with `test_hunk_file_loader.cpp`, and
  `run_seglist_copy.py` with `test_seglist_copy.cpp`. Each has a target and a
  `-sanitize` target in the `Makefile`, and `make test` runs them.
- `run_child_projection.py` and `test_child_projection.c`: the child projection
  covers the cancelled copy, the owned copy, the kept copy and duplicate
  `NP_FreeSeglist` tags. It has 151 checks in seven groups.
- `run_library_loading_versions.py` and `test_library_loading_versions.cpp`:
  adapted to the loader's new helpers.
- `regressions/README.md` describes all of them.

## Patches

`patches/0001-m68kemu-hunk-loaders-and-child-seglist-copy-all.patch` is the
whole series: 19 paths, 13 changed and 6 added, with `a/` and `b/` paths
relative to the repository root.

The same change is also split into three units, to be applied in this order:

1. `patches/units/01-m68kemu-hunk-loaders-and-child-seglist-copy.patch`: the
   module code.
2. `patches/units/02-m68kemu-host-regressions.patch`: the host regressions. It
   needs unit 1, because the fixtures compile the functions unit 1 adds.
3. `patches/units/03-m68kemu-docs.patch`: the two module documents. It applies
   on its own as text, but it describes the code and the tests of units 1
   and 2.

Applying the three units in order gives the same files as the whole series.

## Validation

This is host validation only, on macOS arm64 with the clang 21 of Apple's
command line tools and the MacOSX 27.2 SDK. Each runner extracts the
production functions from the patched sources and compiles them with its
fixture, using `-Wall -Wextra -Werror`.

| Regression | Result |
|------------|--------|
| library loading bounds | 27 cases, 0 failed, 301 checks |
| hunk file loader | 33 cases, 0 failed, 215 checks |
| seglist copy | 22 cases, 0 failed, 303 checks |
| child projection | 151 checks, 0 failed |
| child projection with `--sanitize` (ASan and UBSan) | 151 checks, 0 failed |

Four negative controls were each applied to a separate copy of the tree and
are not part of the series. Each made the child projection fail exactly the
checks it should, and no other (151 checks each):

| Control | Expected failures |
|---------|-------------------|
| the cancellation release removed | 5 |
| the copy given to no child | 12 |
| the `NP_FreeSeglist` condition dropped | 8 |
| the last `NP_FreeSeglist` deciding | 8 |

Where a control removed the only read of the flag, it keeps one inert
`(void)freeseg;` so that the tree still compiles with `-Werror`.

Notes on these results:

- They come from separate runs on the same sources.
- The sanitized run executed the binary that an earlier run had compiled with
  the runner's own flags.
- Leak detection is off, as the runners set it (`detect_leaks=0`). No
  additional sanitizer suppression was added; the compiler's defaults were
  kept.
- Nothing was compiled or run to make this package.

## Not covered

- `run_library_loading_versions.py` and the `Makefile` programs that need the
  Moira checkout were not run.
- This series has had no AROS build, no native run, no retarget, and no test
  under QEMU or with an application.
- Known limits: a FALSE or `CreateProc` copy is kept until the program ends,
  and the native seglist is not unloaded on that path. `create_proc` reads at
  most 12 tags. The regressions model the lock and the scheduling; they do not
  exercise SMP or a real lock.
