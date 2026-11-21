<!--
Copyright (C) 2026, The AROS Development Team. All rights reserved.
Author: Michele Dipace (@kaffeine1)
-->

# kaffeine-jit68: experimental engine source

This directory holds the source of the optional 68k JIT engine for AROS's
`m68kemu.library`, version 1 of the interface between the two, and the host
checks of the engine. It is experimental. The directory [`aros`](aros) holds
what makes the engine an AROS library, `kaffeine-jit68.library`, and a
native probe of that library; a script builds both outside an AROS tree.
In the branch proposed to AROS as
[pull request 2360](https://github.com/aros-development-team/AROS/pull/2360)
the engine, the probe and these checks have their places in the AROS tree
and the library a build rule; the interface is in `rom/m68kemu` there
([pull request 2359](https://github.com/aros-development-team/AROS/pull/2359)).
The sources here are the files of those branches with an author line added:
[`tools/compare-with-aros.py`](tools/compare-with-aros.py) compares them
with an AROS tree. The side of `m68kemu.library`
that loads the engine is a patch for AROS mainline, on the
[adapter page](../../docs/kaffeine/jit68-adapter/README.md), where the two
libraries have run together under QEMU. What the engine is for, and
the state of each step, are on the
[engine page](../../docs/kaffeine/jit68-engine.md).

## Contents

| Path | What it is |
| --- | --- |
| [`include/m68kemu_engine.h`](include/m68kemu_engine.h) | The interface between `m68kemu.library` and an engine, version 1.0: the file of that name in `rom/m68kemu` of the AROS branch |
| [`include/ENGINE.md`](include/ENGINE.md) | Its specification, from the same place; "this directory" in it is `rom/m68kemu` |
| [`engine/kj68_engine.c`](engine/kj68_engine.c) | The nine operations of the interface, the memory map and the counters |
| [`engine/kj68_translate.c`](engine/kj68_translate.c) | The AArch64 translator, generated from the translator of the legacy library (see below) |
| [`engine/kj68_intern.h`](engine/kj68_intern.h) | The private interface between the two engine files, and the test seams |
| [`engine/kj68_platform.h`](engine/kj68_platform.h) | What the engine asks of the system it runs on: memory, memory for code, a dead end |
| [`host/kj68_host_tests.c`](host/kj68_host_tests.c) | The host checks: 26 groups, through the interface, with the table of operations, the test seams and the self test of the encoder from `kj68_intern.h` |
| [`host/kj68_platform_host.c`](host/kj68_platform_host.c) | The answers of a macOS or Linux host to `kj68_platform.h` |
| [`host/include/exec/types.h`](host/include/exec/types.h) | The AROS exec types, for the host build only |
| [`tools/make-translate.py`](tools/make-translate.py) | The generator of `engine/kj68_translate.c` |
| [`tools/compare-with-aros.py`](tools/compare-with-aros.py) | Compares these sources with an AROS tree that has the branches proposed to AROS |
| [`Makefile`](Makefile) | A host build of the checks |
| [`aros/kaffeine-jit68.conf`](aros/kaffeine-jit68.conf) | The library's configuration: its one function, `KJ68_GetEngine`, at LVO -30 |
| [`aros/kj68_library.c`](aros/kj68_library.c) | That function |
| [`aros/kj68_platform.c`](aros/kj68_platform.c) | The answers of AROS to `kj68_platform.h` |
| [`aros/kj68probe.c`](aros/kj68probe.c) | A native probe of the library through the interface alone |
| [`aros/build-outside-tree.sh`](aros/build-outside-tree.sh) | Builds the library and the probe with the tools and the SDK of an AROS build |

## The host checks that have run

On 17 November 2026 the host checks of this source ran on an AArch64 Mac
with macOS. They used the clang of the Command Line Tools with the macOS
27.2 SDK; the compiler's version was not queried. There are two
configurations, both with `-std=c99 -Wall -Wextra -Wno-unused-parameter
-Werror -g` on the three compiles and with the test seams compiled in
(`-DKJ68_TEST_SEAMS`):

- `-O1 -fno-omit-frame-pointer` with the address and undefined-behavior
  sanitizers (`-fsanitize=address,undefined -fno-sanitize-recover=undefined`);
- `-O2` without sanitizers.

The two configurations ran separately, one after the other, on the same six
source files. In each, the three compiles, the link and the test program
ended by themselves with status 0 and wrote nothing on their error output,
and all 26 groups passed: 40861 checks with 0 failures. The output of the
`-O2` run is byte for byte the output of the sanitizer run.

The count was first expected as 40860: counting the checks of the new group
by hand, I had left out the leak check that ends every group. The sanitizer
run was therefore recorded as not meeting its expectation, although none of
its checks failed, and the `-O2` run was made afterwards against the
corrected count. The six files those runs compiled were the ones of this
directory then. The sources have changed since.

On 21 November 2026 the sources took the form proposed to AROS: the names
of `m68kemu_engine.h`, the code that depends on the system behind
`kj68_platform.h`, the self test of the encoder compiled for the checks
only, and no clock, which fed a counter nothing read. `make check` then ran
again on the same machine: 40861 checks, 0 failed, at `-O1` with the
sanitizers and at `-O2`.

That shows that this source compiles without warnings with that compiler,
and that it passes the cases of those 26 groups in those two configurations
on that Mac, among them `div-host-read`: a division whose divisor would be
read through the host is declined before any host call (the
[engine page](../../docs/kaffeine/jit68-engine.md) says how). It does not
show that the engine follows the interface in every case, a build without
the test seams, Linux or another compiler, anything on AROS or with an AROS
compiler, the host side in `m68kemu.library`, an application, a
multiprocessor system or a speed.

An earlier version of this source, with 25 groups and without the change to
divisions, ran on 16 November 2026 in the same two configurations: 38519
checks with 0 failures in each.

The engine reports itself as `kaffeine-jit68 0.1 (J1 source, AArch64
translator)`; "J1" names the first source milestone of the engine.

## Building and running the checks

The [`Makefile`](Makefile) was written for this publication. It gives the
compiler the options of the run above,
without the sandbox and the absolute paths of the machine it ran on. It
needs an AArch64 host, macOS or Linux, and a C compiler with the address
and undefined-behavior sanitizers; on another host the test program reports
that translated code does not run there and exits with 1.

```sh
make check
```

This builds `build/asan/kj68_host_tests` and `build/o2/kj68_host_tests`
and runs both. Each ends with a line giving the number of checks and of
failures. `make clean` removes `build/`.

On 20 November 2026 the Makefile was run as it is, on a copy of this
directory, on an AArch64 Mac with Apple clang 21.0.0: `make check` ended
with status 0, and each of the two programs reported the 26 groups passed,
40861 checks with 0 failures, with the same output.

## Regenerating the translator

`engine/kj68_translate.c` is generated. The generator copies line ranges of
the legacy translator, checks each range, and refuses an input that is not
the one it was written for: `rom/kaffeine/kaffeine_jit.c` and
`rom/kaffeine/kaffeine_jit.h` as of commit `033602cd` of this repository.
The current `kaffeine_jit.c` is newer (a later change adds hooks to the
legacy blitter emulation, which the engine leaves to the host) and is
refused. From this directory:

```sh
mkdir -p build
git show 033602cd:rom/kaffeine/kaffeine_jit.c > build/kaffeine_jit.c
git show 033602cd:rom/kaffeine/kaffeine_jit.h > build/kaffeine_jit.h
python3 -I -B tools/make-translate.py build/kaffeine_jit.c build/kaffeine_jit.h \
    build/kj68_translate.c build/reuse-map.json
cmp build/kj68_translate.c engine/kj68_translate.c
```

The generator carries the same edits as the translator for the change to
divisions (the probe of the divisor and the two division emitters that call
it), written as checked line ranges like the others. On 20 November 2026
the steps above were run as they are, with Python 3.14: the generator ended
with status 0, and `cmp` found its output byte for byte equal to the
translator in `engine/`, the one the host checks compiled. Its counts in
that form: 110 definitions unchanged, 24 changed, 33 removed, 2 moved from
the legacy header and 27 added.

On 21 November 2026 the generator got a second stage for the form proposed
to AROS: the names of the interface as `m68kemu.library` has it, and
thirteen replacements, each checked to apply exactly once. Run as above, it
ended with status 0 and `cmp` found its output equal to the translator in
`engine/`. Counts in this form: 103 definitions unchanged, 30 changed, 34
removed, 2 moved from the legacy header and 27 added.

## The AROS library and its probe

The files in [`aros`](aros) make the engine a library for AROS AArch64 and
test it there without `m68kemu.library`.

- **The library.** `kaffeine-jit68.library` 1.0 has one function,
  `KJ68_GetEngine`, at LVO -30, which returns the engine's table of
  operations for an interface major. The engine files are compiled
  unchanged. `kj68_library.c` has the function, and `kj68_platform.c` what
  the engine asks of AROS: memory, memory for code with the cache call that
  publishes it, and a dead end alert.
- **The build.** `build-outside-tree.sh` runs `genmodule`, compiles with
  the flags of a plain C disk library of the AROS build, links with the
  static C library and strips. It is not an AROS build rule; the branch
  proposed to AROS as pull request 2360 has one, and the library is not part
  of any AROS build yet.
- **The probe.** `kj68probe` opens the library, checks that interface
  major 2 is refused, takes the engine of major 1, makes a domain over
  64 KiB of its own memory with one address range that goes to its
  callbacks, and runs small 68k programs whose results it knows: arithmetic
  ended by an A-line opcode; a DBRA loop of 1000 turns in one run and in
  runs of 100 instructions; RTS to an address outside memory; a word read
  and a word written in the range of the callbacks; changed code before
  and after `invalidate`; a division by zero and ILLEGAL, which the engine
  reports and does not execute. Then it reads the engine's counters.

On 20 November 2026 both were built with the cross compiler (gcc 10.5),
`genmodule` and SDK of a build of AROS master `b6dcaeea` for
raspi-aarch64. Both engine files compile there with `-Wall -Wextra` and no
warning. The library is a 64-bit AArch64 ELF object of 107960 bytes without
undefined symbols.

On 21 November 2026, with the sources in the form proposed to AROS, the
script built the library again with the same tools: 78808 bytes, and the
same file, byte for byte, as the library the AROS branch builds with its own
rule, which is the one of the native cases of pull request 2360. The probe
the script builds is not that file: the script links and strips it another
way.

The probe then ran once on that AROS system under QEMU 11.0.0 (machine
`raspi3b`, one processor in use), booted from an SD image that holds the
libraries of the same build, with the engine library added to `LIBS:`. The
system is AROS master `b6dcaeea` with the two files of AROS pull request
#2331, which that target needs to boot. The result was 20 checks with 0
failures and return code 0, and the engine reported 10 blocks and 23
instructions translated, 4013 instructions retired and 16 MiB of executable
memory, which it takes with `AllocMem(MEMF_EXECUTABLE)` and publishes with
`CacheClearE`. The same source, compiled on the Mac against the same engine
files with stand-in headers, prints the same lines.

That shows that the engine translates and runs 68k code on AROS AArch64
under QEMU, through its library and the interface. It does not show
anything with `m68kemu.library`, a 68k program, physical hardware, more
than one CPU object or domain at a time, a multiprocessor system, an AROS
build rule, or a speed.

## License

These files are AROS code of this project, under the
[AROS Public License](../../LICENSE), like the legacy library they derive
from. The translator derives from the legacy translator, which was written
for this project and contains no Emu68 code; see
[licenses and component provenance](../../docs/kaffeine/LICENSING.md).
