<!--
Copyright (C) 2026, The AROS Development Team. All rights reserved.
Author: Michele Dipace (@kaffeine1)
-->

# kaffeine-jit68: experimental engine source

This directory holds the source of the optional 68k JIT engine for AROS's
`m68kemu.library`, version 1 of the interface between the two, and the host
checks of the engine. It is experimental. It builds no AROS library: no
`kaffeine-jit68.library` is produced here, and the side of
`m68kemu.library` that would load the engine is not written yet. What the
engine is for, and the state of each step, are on the
[engine page](../../docs/kaffeine/jit68-engine.md).

## Contents

| Path | What it is |
| --- | --- |
| [`include/kaffeine-jit68-abi-v1.h`](include/kaffeine-jit68-abi-v1.h) | The interface, version 1.0: every name the host and the engine share |
| [`include/kaffeine-jit68-abi-v1.md`](include/kaffeine-jit68-abi-v1.md) | Its specification; where a comment in the header is shorter, the specification governs |
| [`engine/kj68_engine.c`](engine/kj68_engine.c) | The nine operations of the interface, the memory map and the counters |
| [`engine/kj68_translate.c`](engine/kj68_translate.c) | The AArch64 translator, generated from the translator of the legacy library (see below) |
| [`engine/kj68_intern.h`](engine/kj68_intern.h) | The private interface between the two engine files, and the test seams |
| [`host/kj68_host_tests.c`](host/kj68_host_tests.c) | The host checks: 26 groups, through the interface only |
| [`host/include/exec/types.h`](host/include/exec/types.h) | The AROS exec types, for the host build only |
| [`tools/make-translate.py`](tools/make-translate.py) | The generator of `engine/kj68_translate.c` |
| [`Makefile`](Makefile) | A host build of the checks |

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
corrected count. The six files the runs compiled are the ones in this
directory, byte for byte: `include/kaffeine-jit68-abi-v1.h`,
`engine/kj68_engine.c`, `engine/kj68_intern.h`, `engine/kj68_translate.c`,
`host/kj68_host_tests.c` and `host/include/exec/types.h`.

That shows that this source compiles without warnings with that compiler,
and that it passes the cases of those 26 groups in those two configurations
on that Mac, among them `div-host-read`: a division whose divisor would be
read through the host is declined before any host call (the
[engine page](../../docs/kaffeine/jit68-engine.md) says how). It does not
show that the engine follows the interface in every case, a build without
the test seams, Linux or another compiler, anything on AROS or with an AROS
compiler, the host side in `m68kemu.library`, which would have to execute a
declined division itself and is not written, an application, a
multiprocessor system or a speed.

An earlier version of this source, with 25 groups and without the change to
divisions, ran on 16 November 2026 in the same two configurations: 38519
checks with 0 failures in each.

The engine reports itself as `kaffeine-jit68 0.1 (J1 source, AArch64
translator)`; "J1" names the first source milestone of the engine.

## Building and running the checks

The [`Makefile`](Makefile) is written for this publication and has not
been run as it is. It gives the compiler the options of the run above,
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

The generator in this form has not been run. It carries the same edits as
the translator for the change to divisions (the probe of the divisor and
the two division emitters that call it), written as checked line ranges
like the others and compared with the translator by reading. The translator
in `engine/` was edited with those same changes, and it is the one the host
checks compiled. That the generator, run on the input above, writes it byte
for byte is expected, not yet shown.

## License

These files are AROS code of this project, under the
[AROS Public License](../../LICENSE), like the legacy library they derive
from. The translator derives from the legacy translator, which was written
for this project and contains no Emu68 code; see
[licenses and component provenance](../../docs/kaffeine/LICENSING.md).
