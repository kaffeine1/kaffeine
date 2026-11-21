<!--
Copyright (C) 2026, The AROS Development Team. All rights reserved.
Author: Michele Dipace (@kaffeine1)
-->

# From `kaffeine.library` to `m68kemu.library`: status of the transition

[Manifesto](MANIFESTO.md) | [Current implementation](../../README.KAFFEINE.md)

Last updated: 20 November 2026.

This page records where the work stands while it moves from a renamed copy
of the 68k compatibility library onto the library of AROS mainline. It is
updated when something actually moves. Nothing on it is a plan presented as
a result.

## The decision

- The 68k compatibility library is AROS's `m68kemu.library`, in
  `rom/m68kemu`. Its name, its module, its two public functions `RunHunk` and
  `RunFile`, its interpreter and its default behavior stay as AROS has them.
- Kaffeine is the name of the JIT engine and of nothing else. The 68k JIT is
  to be a separate optional library, `kaffeine-jit68.library`, which
  `m68kemu.library` can use as its CPU backend. Version 1 of the interface
  between the two and the source of the engine are written, reviewed as
  source and published in `experimental/kaffeine-jit68`; the engine's host
  checks pass on an AArch64 Mac, and since 20 November 2026 the engine is
  also built as a library for AROS AArch64, where a probe of that library
  has run under QEMU. The side of `m68kemu.library` that loads it is
  written as a patch, and the two libraries have run small 68k programs
  together under QEMU ([adapter page](jit68-adapter/README.md)). The
  [engine page](jit68-engine.md) describes them. Apart from those
  checks and that probe, the JIT that has run is compiled into the legacy
  `kaffeine.library` and into standalone probes.
- What every user of the library needs, with or without a JIT, goes to AROS
  as small, separate contributions. Whether and how they are accepted is for
  the AROS maintainers to decide.
  [Pull request #2273](https://github.com/aros-development-team/AROS/pull/2273),
  `m68kemu.library` 1.1 (the root stack, the process streams, the arguments
  and the result of `AllocBitMap`), was merged into AROS mainline on
  16 November 2026. The corrections of points 1 and 2 below have not been
  proposed yet.
- No component of an AROS system is replaced. There is no second
  compatibility library: `kaffeine-jit68.library` is to hold the optional
  engine only, and `m68kemu.library` works without it.
- A JIT for PowerPC code, `kaffeine-ppc.library`, is a separate future goal
  that depends on whether it proves feasible. It is no part of the 68k
  compatibility of `m68kemu.library`, nothing of it exists, and nothing here
  claims support for it.

## What exists today, and under which name

| What | Identity | State |
| --- | --- | --- |
| Legacy library | `kaffeine.library` 1.1, built from `rom/kaffeine` of this repository; derives from AROS's `m68kemu.library` at AROS `ae45f4644f80de542fb6af320c5d5fb5a1bc9925` | The community preview and most published results, videos and captures of programs running inside AROS were made with it; each page names the build it used. The IBrowse check of 14 November 2026 used a local development build that is not published. The library stays in the repository as it is, so that the published results can be reproduced. It is not developed further as a library of its own. |
| Locally patched `m68kemu.library` | AROS master `83449051` plus five local changes of mine, as they stood on 14 November 2026; the build reports version 1.5; Moira | One recorded check: Amigram 0.1 from the Shell and from `Run` in QEMU, 14 November 2026. It did not use `kaffeine.library`. It is not the series now being prepared, and it qualifies nothing of that series. |
| Tests without an emulator library | Host tests compile sources of this repository with simulated native APIs; the standalone probes run the JIT translator alone on AROS AArch64; the reference runs are on native AmigaOS | Their results describe those environments. They load neither `kaffeine.library` nor `m68kemu.library`. |
| Community preview 001 | Release `preview-arm64-001`: a matched AROS ARM64 system for QEMU that carries the legacy library | Reproducible as published, with the commands of its own guide. It is not an installer, and it says nothing about `m68kemu.library` of AROS mainline. |
| Corrections to `m68kemu.library` | Changes to `rom/m68kemu` on AROS mainline `8fdbf649ea4d2bc87d6b73c2da45148b1ed61dbe` | Being prepared, locally. Nothing of this series has been proposed to AROS. Four checks have been made: one compile-only check of the layout change, described under point 1 below, and three host regressions of the HUNK loading corrections, described under point 2. No module of this series has been built, linked or run. AROS mainline has since moved on: it now holds `m68kemu.library` 1.1 (pull request #2273, merge `2e3de55a`), which changes eleven files of `rom/m68kemu`. The series has not been moved onto it yet, and that move is work of its own, not a formality. |
| Module build with the layout correction | `m68kemu.library` 1.0, built with Make from AROS mainline `656d2825` with a version of the layout correction prepared for that tree; Moira | Built and qualified as a build on 16 November 2026, after a first record that was not a pass; described under point 1. Not loaded or run. It is neither the series above nor current mainline 1.1. |
| `m68kemu.library` of AROS mainline on AROS AArch64 | `m68kemu.library` 1.1, built with the AROS build system from AROS master `b6dcaeea`, for raspi-aarch64, with the two files of AROS pull request #2331 that the target needs to boot; the library itself is not patched; Moira | On 20 November 2026 it opened on that AROS system under QEMU 11.0.0, once nine libraries it asks for at start had been built and installed, and ran a 40-byte 68k hunk to its end: return code 7 when started from the Shell, and 42 as the result of a process made from the hunk's seglist. That is the interpreter, on one trivial program. It says nothing about a real program, and the structure layouts of point 1 below are not corrected in that build. |
| `kaffeine-jit68.library`, the 68k JIT as a separate optional library | Interface version 1 and engine source in `experimental/kaffeine-jit68` | Written and reviewed as source: the interface, an engine whose translator is generated from the legacy one, and host checks in 26 groups. On 17 November 2026 they passed on an AArch64 Mac: 40861 checks with 0 failures at `-O1` with sanitizers and, in a separate run of the same source, at `-O2`, with the same output. A division whose divisor the host would read is declined before any host call. On 20 November 2026 the engine was built as a library for AROS AArch64, and a native probe that uses that library alone passed on AROS under QEMU: 20 checks with 0 failures. The side of `m68kemu.library` that selects and loads the engine is written as a patch for AROS master: the engine runs under the interpreter, which completes every instruction the engine does not execute. On the same day the patched library and the engine ran twenty-three cases with eight small 68k programs from the Shell under QEMU, with the expected results, one of the programs with a process of its own, a software interrupt and a vertical blank server; no application has run this way, and nothing on hardware. Both are proposed to AROS: the side of `m68kemu.library` as [pull request 2359](https://github.com/aros-development-team/AROS/pull/2359) and the engine library as [pull request 2360](https://github.com/aros-development-team/AROS/pull/2360). See the [engine page](jit68-engine.md) and the [adapter page](jit68-adapter/README.md). Apart from those host checks, that probe and those cases, the JIT that has run is the one compiled into the legacy library and into the standalone probes, which load no emulator library. |

## What moves, in this order

1. **Native structure layouts for each target.** The emulator copies fields
   between 68k structures and native ones by a table of native offsets.
   The table checked into `rom/m68kemu` was generated for x86_64, as its
   makefile says. For AROS AArch64 on the Raspberry Pi the compiler gives
   other numbers: `struct Library` is 80 bytes there against 64 in the
   table, and `struct MsgPort` 96 against 256. I measured this by compiling
   a probe to assembly with the AArch64 AROS cross compiler; nothing was run.
   The correction lets the build make the table for its own target and
   refuse a table that does not fit. It is now written for `rom/m68kemu`, as
   local commits on AROS `8fdbf649` that are not published (head `75ebce04`).
   One check has been made on it: three compiler calls that compile to
   assembly only, with the AArch64 AROS cross compiler and the headers of an
   existing uniprocessor Raspberry Pi build of AROS from 13 November 2026,
   not of a build of this series. All 3171 mapped fields of the 297
   structures resolve; every native
   offset and size of the resulting table then holds in a second,
   independent compile; and a table with one offset deliberately made wrong
   is refused by the compiler, which names the field. On that target 386 of
   the 3171 native offsets and the size of 33 structures differ from the
   x86_64 table. Not measured by that check: the rules under the AROS build
   system, a build or a link of the module, any run, a multiprocessor
   configuration, x86_64 and riscv64.

   One more local commit (head `c42dc0af`) changes only the rules that make
   the table: which compiler flags the probe gets, which files it depends
   on, and what happens when its output is missing or empty. It has been
   reviewed as source. No make has run on these rules as they are committed
   there, and the direct compiler check above did not use them.

   A separate build has been made and qualified since, on 16 November 2026:
   `m68kemu.library` 1.0, built with Make from AROS mainline `656d2825` with
   a version of this layout correction prepared for that tree, its build
   rules included. The single module build ended with exit status 0 and
   produced the module, with 47 compiler warnings, none of them from the
   layout probe or its check. The first record of that build was not a pass:
   it did not show how the probe had been invoked, and three planned
   inspections had not run. A supplementary check then ran them once: the
   module is a 64-bit little-endian AArch64 ELF object, the layout check
   defines no symbols and has empty code and data sections, so all of its
   checks happen at compile time, and the probe, run again with the known
   flags, gives assembly byte for byte identical to the one kept from the
   build. That build and its layout are qualified as a build. Not shown:
   loading the module on AROS, its relocations as a whole, QEMU,
   applications such as IBrowse or Amigram, a multiprocessor system,
   physical hardware, and anything about `m68kemu.library` 1.1 of current
   mainline, which has not been built with these corrections.
2. **Loading of HUNK files.** `m68kemu.library` already reads the record of
   relocations that the DOS of AROS mainline keeps. I read the current
   source for a few cases: a seglist for which no record is answered,
   relocations that do not fit their hunk, sizes and bounds in the reader of
   a file image and in the loader of 68k disk libraries. Three corrections
   follow from that reading, each with its own test.

   The first, for the loader of 68k disk libraries, is written as three
   local commits on AROS `8fdbf649` that are not published (head
   `23a8ef23`): what a library file says is checked before it is used, and
   a refused library gives back what it took. One host regression has been
   run on it, once, on 15 November 2026. The loader's functions are taken
   verbatim from the source and compiled as C++20 with Apple clang 21 on
   macOS, against a fixture that stands in for the emulated memory and its
   heap. With the corrected source all 27 cases pass, with and without the
   address and undefined-behavior sanitizers. With the source of `8fdbf649`
   the same fixture passes 6 cases and fails 21, the split I had written
   down from the reading before the run. The existing regression of
   loaded-library versions, which runs the Moira CPU core at `1efd6946`,
   still passes. This is a host check of functions cut out of the module.
   It does not show the module compiled as C by the AROS build, linked,
   loaded or run, a 68k library loaded on AROS, or a multiprocessor system.

   The second, for the reader of a hunk file image, is written as two
   further local commits (head `0695b548`) with a regression of 33 cases.
   One host regression has been run on it, once, on 15 November 2026, in
   the same way and with the same compiler. With the corrected source all
   33 cases pass, with and without the sanitizers. With the source of
   `8fdbf649` the same fixture passes 15 cases and fails 18, the split I
   had written down from the reading before the run. The limits are those
   of the first: a host check of functions cut out of the module, not the
   module compiled as C by the AROS build, linked, loaded or run, not a
   hunk file loaded on AROS, not a multiprocessor system.

   The third, for the copy of a seglist, is written as two further local
   commits (head `9df4263b`) with a regression of 22 cases. One host
   regression has been run on it, once, on 15 November 2026, in the same
   way and with the same compiler. With the corrected source all 22 cases
   pass, with and without the sanitizers, and both runs print the same
   bytes. With the source of `8fdbf649` the same fixture passes 7 cases and
   fails 15, the split I had written down from the reading before the run.
   This check has limits of its own beside those of the first two. The
   relocation list is answered by a stand-in written for the test, not by
   the DOS of AROS. The test puts the link between two segments on an
   aligned address, which a 64-bit AROS does not, and relocates only
   longwords at offsets that are multiples of four, which 68k code does
   not keep to; neither case is exercised. The change to the creation of a
   child process is read, not run. And as before: not the module compiled
   as C by the AROS build, linked, loaded or run, not a seglist of the real
   DOS copied on AROS, not a multiprocessor system.
3. **Multiprocessor systems.** Not qualified, and not started for
   `m68kemu.library`. The legacy library refuses native backfill hooks on a
   multiprocessor kernel; I have verified no such guard in `m68kemu.library`
   of AROS mainline. Before use on a multiprocessor system is claimed, the
   ownership, the locking and the lifetime of what callbacks touch have to
   be established and checked.
4. **The JIT as a separate optional library, `kaffeine-jit68.library`.**
   Version 1 of the interface is designed: how `m68kemu.library` finds the
   engine and checks its version and sizes, which memory accesses the engine
   makes itself and which it leaves to the host, why and where it returns,
   and what happens when it is absent or does not fit, with the interpreter
   as the default. The engine source is written, reviewed as source and
   published, and its host checks pass, most recently with 26 groups on 17
   November 2026. Since 20 November 2026 it is also built as a library for
   AROS AArch64 and probed on AROS under QEMU.
   Connecting it to `m68kemu.library` comes after the common corrections and
   after the review of the behavior outside the JIT, which is in progress.
   The [engine page](jit68-engine.md) gives the state of each step.
5. **Installation on an existing AROS system.** Not qualified on any system,
   and no installer exists. One has to show which `m68kemu.library` is
   actually resident and whether an optional `kaffeine-jit68.library` is
   present, with their versions and hashes; which backend is selected;
   whether the two were built for the same interface; who loads and unloads
   the engine; and how the system falls back, restarts or rolls back when
   something does not fit. It must not replace the library of a system
   silently. A private directory, or the separate name the legacy library
   had, does not by itself keep an installation apart from the system.

## How to read the older material

- Documents, captions, videos, logs and commands that say `kaffeine.library`
  describe the legacy library. They are left as they were made. Where a page
  gives instructions, it says that they are for the legacy tree. A record
  made with something else, as the Amigram check was, says with what.
- A result obtained with the legacy library is evidence for that library,
  that backend and those sources. It becomes evidence for `m68kemu.library`
  when it has been repeated there, and not before. A result obtained with
  the JIT compiled into the legacy library is no evidence for
  `kaffeine-jit68.library`, even though the engine's translator derives from
  the legacy one.
- The names `rom/kaffeine`, `kaffeine.library`, `KAFFEINE_CPU` and the
  `kaffeine_*` test directories are the names of the legacy tree. They stay
  until the code they name has moved. The repository keeps its name.
