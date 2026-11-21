<!--
Copyright (C) 2026, The AROS Development Team. All rights reserved.
Author: Michele Dipace (@kaffeine1)
-->

# The engine under m68kemu.library: the adapter

This directory holds the host side of the
[kaffeine-jit68 interface](../../../experimental/kaffeine-jit68/include/kaffeine-jit68-abi-v1.md):
a patch for `rom/m68kemu` of AROS mainline that lets `m68kemu.library` run
68k code on the optional `kaffeine-jit68.library`, and the record of its
first runs on AROS AArch64 under QEMU.

State on 21 November 2026: the patch is written and has run on AROS AArch64
under QEMU. Twenty-three cases with eight small 68k programs, with the
interpreter alone and with the engine under it, gave the expected results;
one of the programs has a process of its own, a software interrupt and a
vertical blank server, one has divisions the engine declines, one is a
BOOPSI class whose dispatcher intuition calls back, and two use a CIA
timer. It has not run an application and has not run on
hardware. It is proposed to AROS mainline, together with the engine
library, as [pull request 2357](https://github.com/aros-development-team/AROS/pull/2357).

## What it does

`m68kemu.library` runs classic 68k programs on a non-68k AROS with an
interpreter, the Moira core. The adapter keeps that interpreter as the
reference for everything and lets the engine run under it:

- Every CPU of a run is still an interpreter CPU. It keeps the registers
  the library thunks read and write, takes every exception and interrupt
  and makes every library call exactly as before.
- With `M68KEMU_CPU=jit` or `auto`, each of those CPUs also has an engine
  CPU. Each of the three execution loops of the library (the program, a
  process of the program, a call from native code into 68k code) tries the
  engine before it interprets an instruction: the registers go to the
  engine, it runs a slice of translated code, and the registers come back.
- Whatever instruction the engine stops at without executing it is one
  step of the interpreter: an A-line opcode (a library call), an
  instruction the engine declines, one that raises an exception, one it
  cannot fetch.

So the engine is an accelerator for the plain 68k code between library
calls, and no host behaviour is written twice. `Supervisor()`, RTE, guest
exception handlers and the vertical blank and CIA interrupts work under
the engine because the interpreter still does them.

Without `M68KEMU_CPU` nothing of the adapter runs: every engine line is
behind one field of the context that stays 0.

## Selection

`M68KEMU_CPU` is read once per `RunHunk`/`RunFile` call, before the program
is loaded (local variable first, then global):

| value | run |
| --- | --- |
| unset, empty | the interpreter, as without the adapter, and no message |
| `interpreter` | the interpreter; one line says so |
| `jit` | the engine under the interpreter, or no run at all: without the library, without a usable engine table, or when the engine cannot take the program's memory or make its CPU, the call returns -1 with a message and the error code `ERROR_INVALID_RESIDENT_LIBRARY` |
| `auto` | the engine when all of that works, otherwise the interpreter, with the reason in the line |
| anything else, or a value of 31 characters or more | -1 with a message |

With the variable set, one line in the system log and the debug output
says what runs the program. After a run with the engine a second line
gives the engine's counters and the number of instructions the interpreter
ran, so a run is never reported as more of the engine's than it was.

## When the engine takes a slice

All of these, at the top of the loop:

- nothing is pending in the interpreter core (not stopped, no trace, no
  interrupt to check);
- no interrupt level is asserted, and none is raised and not yet taken;
- the status register is inside the model of interface 1.0 (supervisor, no
  trace, interrupt stack);
- the PC is even, at or above 0x1000 and inside the program's memory. Below
  0x1000 are the vectors and the library's own stubs, which the interpreter
  runs;
- the instruction at PC is not one the engine just returned.

A slice has a budget of 1024 instructions. The polls of the loops keep
their rhythm from the number of instructions a slice retired: vertical
blank and CIA timers every 1024, the program lock every 65536.

## Memory

The engine sees the program's memory with four ranges whose accesses go
through the library, which answers them with the interpreter's own access
functions:

| range | through the host | why |
| --- | --- | --- |
| `[0, 0x1000)` | writes | the guard on the vectors, and the library's stubs |
| `[0xBFD000, 0xBFF000)` | reads and writes | CIA registers |
| `[0xDFF000, 0xE00000)` | reads and writes | custom chip registers |
| the bitplanes of a mirrored screen | writes | the planar mirror, while a program draws into a screen's planes |

The last range comes and goes with a thunk; the map follows it between
engine runs.

## What it does not do

- It does not replace the interpreter. Moira stays linked and needed.
- The instruction ring of `M68KEMU_TRACE` has one entry per engine slice,
  not per instruction, for the code the engine ran.
- An interrupt can be late by one slice.
- A program that stores into code it already ran and calls no cache
  function runs stale code under the engine and fresh code under the
  interpreter. The engine's translations are dropped at the loaders, when a
  library base or a stub is written, at `CacheClearU()`, `CacheClearE()`
  and `CacheControl()`, and at CINV and CPUSH.
- An engine failure stops the program with result 20 and a message. There
  is no switch back to the interpreter in the middle of a run.

## The patch

[`patches/0001-m68kemu-an-optional-engine-under-the-interpreter.patch`](patches/0001-m68kemu-an-optional-engine-under-the-interpreter.patch)
applies to AROS master `b6dcaeeaa2724bbe4f6d9510c1ada4b8f69789fb`.

| file | change |
| --- | --- |
| `m68kemu_jit.c` (new) | `M68KEMU_CPU`, opening the engine, the memory map, invalidation, the two message lines, the end |
| `kaffeine-jit68-abi-v1.h` (new) | the interface header, as in `experimental/kaffeine-jit68/include` |
| `m68kemu_moira.cpp` | an engine CPU per interpreter CPU, the slice, the slice tried in the three loops, the two access callbacks |
| `m68kemu_run.c` | the selection, before the loader, in `RunHunk` and `RunFile`; the error code of a refused run |
| `m68kemu_memory.c`, `m68kemu_m68klib.c`, `m68kemu_thunks.c` | invalidation where the library writes 68k code; the engine's end in `M68KEmu_DestroyContext` |
| `m68kemu_intern.h` | the adapter's fields, at the end of the context |
| `m68kemu.conf`, `mmakefile.src` | version 1.2; the new file |

No host test can run this code: it needs exec, dos and the library base.
It was cross compiled and run natively instead.

The patched library was built outside an AROS tree from a build of AROS
master `b6dcaeea` for raspi-aarch64. Every object whose source the patch
touches was compiled with the commands that build uses for it, the new file
with the command of `m68kemu_run.c`, and the module was linked with its own
link command plus the new object. Applied to the unchanged sources, the same
procedure gives the build's own `m68kemu.library` byte for byte, and the
objects of the files the patch does not touch came out identical to the
build's objects. The compiler gave no new warning.

Result: `m68kemu.library 1.2 (20.11.2026)`, 14088984 bytes, SHA-256
`45ead9b6cf15511434da5f5ddf17f3d6208247ac20e1030d0123fa5cec4a1719`. The
patch file is the source of exactly this library.

## Native runs

On 21 November 2026, under QEMU 11.0.0, machine `raspi3b`, 1 GiB, TCG, on
a boot image of the same AROS build with `Libs/m68kemu.library` replaced by
the patched one and
[`kaffeine-jit68.library`](../../../experimental/kaffeine-jit68/README.md)
1.0 added to `Libs`. Each case is one boot. A startup script sets
`M68KEMU_CPU` as a shell variable, starts the program from the Shell with
its output in a file, and writes the Shell's result code to another file.
Two cases rename the engine library away before the program. The lines come
from the debug serial port.

The programs are 68k hunk files written by the scripts in [`tests`](tests),
which assemble them from hand-encoded instructions:

- `kjsum` opens `dos.library`, adds the squares of 1..20000 in a loop of
  five instructions, writes a line to `Output()`, closes the library and
  returns 12 when the sum is right.
- `kjmix` checks four things itself and returns 12 when all are right: a
  1024-byte buffer on the stack filled and summed with DBRA loops; a
  subroutine with MOVEM; `TRAP #0` with its own handler in vector 32, which
  returns with RTE; and exec `Supervisor()` with its own function, which
  returns with RTE.
- `kjtime` takes `DateStamp()`, runs the loop of `kjsum` four million
  times, takes `DateStamp()` again and writes both stamps and the sum to
  `Output()`; it returns 12 when the sum is right.
- `kjtask` makes the library run 68k code on more than one CPU of the
  program. It calls exec `Cause()` with a software interrupt of its own;
  starts a process with dos `CreateNewProc()` and `NP_Entry`, which sums in
  a loop while the program waits in `Delay()`; and adds a vertical blank
  server with `AddIntServer()` while it runs a loop of twenty million
  instructions. It writes its three counters and its sum to `Output()` and
  returns 12 when all is right.
- `kjdiv` makes three divisions and checks each result: one whose divisor
  is the custom chip register POTGOR, which the library answers itself, one
  whose divisor is in the program's memory, and a signed one on the same
  register through an absolute address. The engine declines a division
  whose divisor it would have to read through the library.
- `kjhook` is a BOOPSI class written in 68k code. It makes a class under
  rootclass with `MakeClass()`, writes its own dispatcher into it, and calls
  `NewObjectA()`, `GetAttr()`, `SetAttrsA()` and `DisposeObject()`: each
  makes intuition call the dispatcher back once, on a CPU of its own. The
  dispatcher counts the method, runs a loop of 5000 instructions, answers
  two methods itself and passes two to its superclass.
- `ciawait` and `ciabusy` are the two CIA timer programs that AROS's own
  native test, `developer/debug/test/m68kemu/test_m68kemu.c`, holds as byte
  arrays; `tests/extract_cia.py` writes them as files, byte for byte. A
  CIA-B timer A vector at 100 Hz adds to a counter while the program waits
  in `Delay()` for half a second (that test expects 35 to 65), and while it
  spins until the counter reaches 20 (20 to 30). The result is the count.
- `hunk7` is `moveq #7,d0; rts`.

| program | `M68KEMU_CPU` | engine library | result | what the library said |
| --- | --- | --- | --- | --- |
| `kjsum` | unset | present | 12 | nothing |
| `kjsum` | `interpreter` | present | 12 | runs on the interpreter |
| `kjsum` | `jit` | present | 12 | runs on the engine; the engine ran 100018 instructions in 102 runs, the interpreter ran 13 |
| `kjsum` | `auto` | present | 12 | as `jit` |
| `kjsum` | `jit` | absent | -1 | "`M68KEMU_CPU` is jit and kaffeine-jit68.library is not there; the program is not run" |
| `kjsum` | `auto` | absent | 12 | runs on the interpreter, "kaffeine-jit68.library is not there" |
| `kjsum` | `turbo` | present | -1 | "it can be interpreter, jit or auto; the program is not run" |
| `kjsum` | `jit`, thirty spaces and `x` | present | -1 | "is too long: it can be interpreter, jit or auto; the program is not run" |
| `hunk7` | `jit` | present | 7 | the engine ran 1 instruction, the interpreter ran 1 |
| `kjmix` | unset | present | 12 | nothing |
| `kjmix` | `jit` | present | 12 | the engine ran 6179 instructions in 11 runs, the interpreter ran 9 |
| `kjtime` | unset | present | 12 | nothing; the loop took 165 ticks of 1/50 s |
| `kjtime` | `jit` | present | 12 | the engine ran 20000024 instructions in 19519 runs, the interpreter ran 19; the loop took 29 ticks |
| `kjtask` | unset | present | 12 | nothing; the software interrupt ran once, the process stored its sum, the server saw 191 vertical blanks |
| `kjtask` | `jit` | present | 12 | the engine ran 20500121 instructions in 20043 runs, the interpreter ran 350; the software interrupt ran once, the process stored the same sum, the server saw 32 vertical blanks |

| `kjdiv` | unset | present | 12 | nothing |
| `kjdiv` | `jit` | present | 12 | the engine ran 13 instructions in 3 runs, the interpreter ran 3 |

| `kjhook` | unset | present | 12 | nothing; the dispatcher was called four times, once for each method |
| `kjhook` | `jit` | present | 12 | the engine ran 20144 instructions in 33 runs, the interpreter ran 45; four calls, the same values |

| `ciawait` | unset | present | 50 | nothing |
| `ciawait` | `jit` | present | 50 | the engine ran 81 instructions in 57 runs, the interpreter ran 69 |
| `ciabusy` | unset | present | 20 | nothing |
| `ciabusy` | `jit` | present | 20 | the engine ran 2047058 instructions in 2023 runs, the interpreter ran 210 |

The first seventeen cases finished in one run, each with
`m68kemu.library 1.2` reported by `Version`. The `kjhook` and the CIA cases
are two later runs, with the two libraries built from the branch proposed
to AROS.

That shows:

- Not selected, the adapter changes nothing that these programs can see.
- With `jit` or `auto` the code between library calls runs on the engine,
  and the sums and results are the interpreter's.
- Under the engine, a trap with the program's own handler, RTE and
  `Supervisor()` work, because the interpreter completes them. The few
  instructions the interpreter ran in each case are the library calls with
  their return, and the instructions the engine handed back.
- Under the engine, a software interrupt (a nested call on a CPU of its
  own), a process of the program (another CPU of the same engine domain,
  half a million instructions on the engine) and the vertical blank
  interrupt (raised between engine slices, taken by the interpreter, its
  server run 32 times in the shorter loop) work.
- A division the engine declines is executed by the interpreter and the
  run goes on: of the 16 instructions of `kjdiv`, the interpreter ran the
  first one and the two divisions on the custom register, and the engine
  the other 13, the third division among them.
- A hook that a native library calls runs on the engine: intuition called
  the dispatcher of `kjhook` four times, and the 20000 instructions of its
  loops ran on the engine. The counters had been worked out from the
  program before the run, 20144 and 45, and the run gave exactly those.
- CIA timer interrupts work under the engine, in both ways the library
  delivers them: on a nested CPU while the program waits, and as the level
  6 interrupt between engine slices while it runs. The counts are the same
  with and without the engine and inside the ranges of AROS's test.
- `jit` without the library and an invalid value refuse the run before any
  68k instruction; `auto` without the library falls back and says why.

The two times of `kjtime` are one measurement each, of one loop, on an
emulated machine under TCG on a busy host. They say that this loop was
about five times faster with the engine there, and nothing about
applications.

The table is the sixth run. The five before it, on 20 and 21 November
with earlier builds of the patch, gave the same results for the cases
they had: twelve cases, then fourteen, then the two of `kjdiv`, then
sixteen. The engine counters were the same each time, except those of
`kjtask`, which follow the number of vertical blanks. The timed loop took
between 147 and 165 ticks on the interpreter and 29 or 30 with the engine. The first run, of eight
cases, used a `kjsum` that returned 42; the startup script stops at a
result of 21 or more, so the cases of that program could not write their
result. Their output and their lines agree with the table.

Between the fourth and the fifth run the design was read against the
AROS and Moira sources by a second reader, who had the design and the
sources but not this code. Checked against the code, the review found no
defect in what had run. Four of its points are in the patch since then:
no slice while an interrupt level is asserted, the status register
written back before the other registers, a second invalidation after the
two pieces of 68k code the library loader writes, and the instruction
count that the library's watchdog reads.

Between the fifth and the sixth run the same reader read the code as
published, again without running anything. Six of its points held when
checked against the code, with several smaller ones, and all are in the
patch: an engine `run()` that fails stops the program without one more
interpreter instruction; an engine table without a name or without one of
its functions is refused; a refusal found only when the program's engine
CPU is made sets the error code too; without the variable the caller's
`IoErr()` is left as it was; a value of the variable too long for the
buffer is refused instead of cut, which the new case shows; and the
loaders invalidate after they have written code, not before. None of them
changed the result of a case that had run.

## Not done

- Hooks other than a BOOPSI dispatcher (a layers hook, for one). No case
  ran one.
- AROS's own `test_m68kemu` to its end. On the test guest it stops at a
  sub-test that needs a font, with and without the engine, at the same
  line; its two CIA programs were run on their own instead.
- An application.
- Hardware. Every run was under QEMU.
- A refused run returns -1 with `ERROR_INVALID_RESIDENT_LIBRARY`, but in
  these runs the Shell still printed an unrelated reason: after a command,
  dos flushes the command's input, and that flush left its own error code.
  That is in dos, outside this patch; a fix for it is proposed to AROS as
  [pull request 2353](https://github.com/aros-development-team/AROS/pull/2353).
  The two have not been run together.
- Acceptance by AROS. The adapter and the engine library are proposed as
  [pull request 2357](https://github.com/aros-development-team/AROS/pull/2357).
  In that branch the engine is in `workbench/libs/kaffeine-jit68` with a
  build rule for AArch64 targets, the interface header is there with its
  specification beside it, and `rom/m68kemu` includes it from there instead
  of holding a copy; otherwise the adapter is the patch of this page. The
  two libraries were built with the commands of those build rules and ran
  the seventeen cases again with the same results (the timed loop at 200
  and 33 ticks), and the probe of the engine library passed. The full AROS
  build has not built the branch.
- A review by a maintainer of AROS, or by anyone who ran the code. The
  two reviews above were readings of the design and of the source.
