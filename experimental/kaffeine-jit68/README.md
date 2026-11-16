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
| [`host/kj68_host_tests.c`](host/kj68_host_tests.c) | The host checks: 25 groups, through the interface only |
| [`host/include/exec/types.h`](host/include/exec/types.h) | The AROS exec types, for the host build only |
| [`tools/make-translate.py`](tools/make-translate.py) | The generator of `engine/kj68_translate.c` |
| [`Makefile`](Makefile) | A host build of the checks |

## The host checks that have run

On 16 November 2026 the host checks ran once, on an AArch64 Mac with
macOS. They used the clang of the Command Line Tools with the macOS 27.2
SDK; the compiler's version was not queried. There were two configurations,
both with `-std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -g` on the
three compiles and with the test seams compiled in (`-DKJ68_TEST_SEAMS`):

- `-O1 -fno-omit-frame-pointer` with the address and undefined-behavior
  sanitizers (`-fsanitize=address,undefined -fno-sanitize-recover=undefined`);
- `-O2` without sanitizers.

Each configuration compiled the three source files, linked them and ran the
test program: ten programs in all. All ten ended by themselves with status
0 and wrote nothing on their error output. In each configuration all 25
groups passed, 38519 checks with 0 failures, and the two runs printed the
same output.

That shows that this source compiles without warnings with that compiler,
and that it passes the cases of those 25 groups in those two configurations
on that Mac. It does not show that the engine follows the interface in
every case, nor anything about a divisor the host reads itself (the
[engine page](../../docs/kaffeine/jit68-engine.md) gives that limit), a
build without the test seams, Linux or another compiler, anything on AROS
or with an AROS compiler, the host side in `m68kemu.library`, an
application, a multiprocessor system or a speed.

The files here are the ones that ran, byte for byte, with three exceptions,
none of which changes code that was compiled:

- `include/kaffeine-jit68-abi-v1.h`: four comment lines that described the
  interface as a proposal not yet implemented, and named the specification
  by a working file name, now give its state and the name of the
  specification beside it;
- `engine/kj68_engine.c`: one comment names the specification by that name;
- `tools/make-translate.py`, which is not part of the checks: the
  identification it writes into the reuse map beside the translator.

The engine reports itself as `kaffeine-jit68 0.1 (J1 source, AArch64
translator)`. That text is unchanged from the run; "J1" names the first
source milestone of the engine.

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

The generator in this form, with only the identification in the reuse map
changed, has not been run. The translator in `engine/` is the one it made
before that change, and the one the host checks compiled.

## License

These files are AROS code of this project, under the
[AROS Public License](../../LICENSE), like the legacy library they derive
from. The translator derives from the legacy translator, which was written
for this project and contains no Emu68 code; see
[licenses and component provenance](../../docs/kaffeine/LICENSING.md).
