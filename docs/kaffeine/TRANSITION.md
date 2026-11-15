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
  the AROS maintainers to decide. None has been proposed yet.
- No component of an AROS system is replaced. There is no second emulator
  library.

## What exists today, and under which name

| What | Identity | State |
| --- | --- | --- |
| Legacy library | `kaffeine.library` 1.1, built from `rom/kaffeine` of this repository; derives from AROS's `m68kemu.library` at AROS `ae45f4644f80de542fb6af320c5d5fb5a1bc9925` | Every published result, video, capture and the community preview were made with it. It stays in the repository as it is, so that they can be reproduced. It is not developed further as a library of its own. |
| Community preview 001 | Release `preview-arm64-001`: a matched AROS ARM64 system for QEMU that carries the legacy library | Reproducible as published, with the commands of its own guide. It is not an installer, and it says nothing about `m68kemu.library` of AROS mainline. |
| Corrections to `m68kemu.library` | Changes to `rom/m68kemu` on AROS mainline `8fdbf649ea4d2bc87d6b73c2da45148b1ed61dbe` | Being prepared, locally. Nothing has been proposed to AROS, nothing of it has been built, nothing of it has run. |
| Kaffeine JIT as a backend of `m68kemu.library` | None yet | Not started as a change set for AROS mainline. The JIT exists, and is tested, inside the legacy library only. |

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
3. **Multiprocessor systems.** Not started. Callbacks that touch guest state
   stay closed on a multiprocessor kernel until ownership and lifetime are
   established.
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
  gives instructions, it says that they are for the legacy tree.
- A result obtained with the legacy library is evidence for that library,
  that backend and those sources. It becomes evidence for `m68kemu.library`
  when it has been repeated there, and not before.
- The names `rom/kaffeine`, `kaffeine.library`, `KAFFEINE_CPU` and the
  `kaffeine_*` test directories are the names of the legacy tree. They stay
  until the code they name has moved. The repository keeps its name.
