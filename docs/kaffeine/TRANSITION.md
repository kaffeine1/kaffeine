<!--
Copyright (C) 2026, The AROS Development Team. All rights reserved.
Author: Michele Dipace (@kaffeine1)
-->

# From `kaffeine.library` to `m68kemu.library`: status of the transition

[Manifesto](MANIFESTO.md) | [Current implementation](../../README.KAFFEINE.md)

Last updated: 15 November 2026.

This page records where the work stands while it moves from a renamed copy
of the 68k compatibility library onto the library of AROS mainline. It is
updated when something actually moves. Nothing on it is a plan presented as
a result.

## The decision

- The 68k compatibility library is AROS's `m68kemu.library`, in
  `rom/m68kemu`. Its name, its module, its two public functions `RunHunk` and
  `RunFile`, its interpreter and its default behavior stay as AROS has them.
- Kaffeine is the name of the JIT engine and of nothing else: an optional CPU
  backend for that library.
- What every user of the library needs, with or without a JIT, goes to AROS
  as small, separate contributions. Whether and how they are accepted is for
  the AROS maintainers to decide. The change sets this page describes have
  not been proposed yet.
- No component of an AROS system is replaced. There is no second emulator
  library.

## What exists today, and under which name

| What | Identity | State |
| --- | --- | --- |
| Legacy library | `kaffeine.library` 1.1, built from `rom/kaffeine` of this repository; derives from AROS's `m68kemu.library` at AROS `ae45f4644f80de542fb6af320c5d5fb5a1bc9925` | The community preview and most published results, videos and captures of programs running inside AROS were made with it; each page names the build it used. The IBrowse check of 14 November 2026 used a local development build that is not published. The library stays in the repository as it is, so that the published results can be reproduced. It is not developed further as a library of its own. |
| Locally patched `m68kemu.library` | AROS master `83449051` plus five changes of mine that are not submitted upstream yet; the build reports version 1.5; Moira | One recorded check: Amigram 0.1 from the Shell and from `Run` in QEMU, 14 November 2026. It did not use `kaffeine.library`. It is not the series now being prepared, and it qualifies nothing of that series. |
| Tests without an emulator library | Host tests compile sources of this repository with simulated native APIs; the standalone probes run the JIT translator alone on AROS AArch64; the reference runs are on native AmigaOS | Their results describe those environments. They load neither `kaffeine.library` nor `m68kemu.library`. |
| Community preview 001 | Release `preview-arm64-001`: a matched AROS ARM64 system for QEMU that carries the legacy library | Reproducible as published, with the commands of its own guide. It is not an installer, and it says nothing about `m68kemu.library` of AROS mainline. |
| Corrections to `m68kemu.library` | Changes to `rom/m68kemu` on AROS mainline `8fdbf649ea4d2bc87d6b73c2da45148b1ed61dbe` | Being prepared, locally. Of this series nothing has been proposed to AROS, built or run yet. |
| Kaffeine JIT as a backend of `m68kemu.library` | None yet | Not started as a change set for AROS mainline. The only library the JIT is integrated in is the legacy library. Apart from that it is tested on the host and in standalone probes, which load no emulator library. |

## What moves, in this order

1. **Native structure layouts for each target.** The emulator copies fields
   between 68k structures and native ones by a table of native offsets.
   The table checked into `rom/m68kemu` was generated for x86_64, as its
   makefile says. For AROS AArch64 on the Raspberry Pi the compiler gives
   other numbers: `struct Library` is 80 bytes there against 64 in the
   table, and `struct MsgPort` 96 against 256. I measured this by compiling
   a probe to assembly with the AArch64 AROS cross compiler; nothing was run.
   The correction lets the build make the table for its own target and
   refuse a table that does not fit. It was written and checked on the legacy
   copy and is being moved to `rom/m68kemu`. It has not been built.
2. **Loading of HUNK files.** `m68kemu.library` already reads the record of
   relocations that the DOS of AROS mainline keeps. A few cases are being
   checked against the current source before anything is proposed: a seglist
   for which no record is answered, relocations that do not fit their hunk,
   bounds in the reader of a file image. No finding is claimed yet.
3. **Multiprocessor systems.** Not qualified, and not started for
   `m68kemu.library`. The legacy library refuses native backfill hooks on a
   multiprocessor kernel; I have verified no such guard in `m68kemu.library`
   of AROS mainline. Before use on a multiprocessor system is claimed, the
   ownership, the locking and the lifetime of what callbacks touch have to
   be established and checked.
4. **The JIT as an optional backend.** After the common corrections.
5. **Installation on an existing AROS system.** Not qualified on any system.
   An installer has to show which build of `m68kemu.library` is loaded and
   which backend is selected, has to account for a library that is already
   resident, and must not replace the library of a system silently. A
   private directory, or the separate name the legacy library had, does not
   by itself keep an installation apart from the system.

## How to read the older material

- Documents, captions, videos, logs and commands that say `kaffeine.library`
  describe the legacy library. They are left as they were made. Where a page
  gives instructions, it says that they are for the legacy tree. A record
  made with something else, as the Amigram check was, says with what.
- A result obtained with the legacy library is evidence for that library,
  that backend and those sources. It becomes evidence for `m68kemu.library`
  when it has been repeated there, and not before.
- The names `rom/kaffeine`, `kaffeine.library`, `KAFFEINE_CPU` and the
  `kaffeine_*` test directories are the names of the legacy tree. They stay
  until the code they name has moved. The repository keeps its name.
