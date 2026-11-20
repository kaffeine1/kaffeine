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

State on 20 November 2026: the patch is written and has run on AROS AArch64
under QEMU. Sixteen cases with five small 68k programs, with the
interpreter alone and with the engine under it, gave the expected results;
one of the programs has a process of its own, a software interrupt and a
vertical blank server, and one has divisions the engine declines. It has not run an application and has not run on
hardware. It is not submitted to AROS mainline.

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
| anything else | -1 with a message |

With the variable set, one line in the system log and the debug output
says what runs the program. After a run with the engine a second line
gives the engine's counters and the number of instructions the interpreter
ran, so a run is never reported as more of the engine's than it was.

## When the engine takes a slice

All of these, at the top of the loop:

- nothing is pending in the interpreter core (not stopped, no trace, no
  interrupt to check);
- no interrupt is raised and not yet taken;
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

Result: `m68kemu.library 1.2 (20.11.2026)`, 14087952 bytes, SHA-256
`c26e176f8425ccf4e3414e6ab274dc8f6eb33e00d7631d5121bc66adca8cad62`. The
patch file is the source of exactly this library.

## Native runs

On 20 November 2026, under QEMU 11.0.0, machine `raspi3b`, 1 GiB, TCG, on
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
| `hunk7` | `jit` | present | 7 | the engine ran 1 instruction, the interpreter ran 1 |
| `kjmix` | unset | present | 12 | nothing |
| `kjmix` | `jit` | present | 12 | the engine ran 6179 instructions in 11 runs, the interpreter ran 9 |
| `kjtime` | unset | present | 12 | nothing; the loop took 155 ticks of 1/50 s |
| `kjtime` | `jit` | present | 12 | the engine ran 20000024 instructions in 19519 runs, the interpreter ran 19; the loop took 30 ticks |
| `kjtask` | unset | present | 12 | nothing; the software interrupt ran once, the process stored its sum, the server saw 158 vertical blanks |
| `kjtask` | `jit` | present | 12 | the engine ran 20500117 instructions in 20041 runs, the interpreter ran 330; the software interrupt ran once, the process stored the same sum, the server saw 30 vertical blanks |

| `kjdiv` | unset | present | 12 | nothing |
| `kjdiv` | `jit` | present | 12 | the engine ran 13 instructions in 3 runs, the interpreter ran 3 |

All sixteen cases finished, each with `m68kemu.library 1.2` reported by
`Version`. The two `kjdiv` cases are a fourth run, on 21 November, with the same library.

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
  server run 30 times in the shorter loop) work.
- A division the engine declines is executed by the interpreter and the
  run goes on: of the 16 instructions of `kjdiv`, the interpreter ran the
  first one and the two divisions on the custom register, and the engine
  the other 13, the third division among them.
- `jit` without the library and an invalid value refuse the run before any
  68k instruction; `auto` without the library falls back and says why.

The two times of `kjtime` are one measurement each, of one loop, on an
emulated machine under TCG on a busy host. They say that this loop was
about five times faster with the engine there, and nothing about
applications.

The table is the third run of that day. The second had the first twelve
rows with the build before this one, which did not yet set the error code
of a refused run: the same results, with the timed loop at 147 and 30
ticks. The first, of eight cases, used a `kjsum` that returned 42; the
startup script stops at a result of 21 or more, so the cases of that
program could not write their result. Their output and their lines agree
with the table.

## Not done

- A hook that a native library calls (a BOOPSI dispatcher, a layers
  hook) and a CIA timer interrupt, under the engine. The code paths exist;
  no case ran them.
- An application.
- Hardware. Every run was under QEMU.
- A refused run returns -1 with `ERROR_INVALID_RESIDENT_LIBRARY`, but in
  these runs the Shell still printed an unrelated reason: after a command,
  dos flushes the command's input, and that flush left its own error code.
  That is in dos, outside this patch; a fix for it is proposed to AROS as
  [pull request 2353](https://github.com/aros-development-team/AROS/pull/2353).
  The two have not been run together.
- A build rule in the AROS tree for the engine library, and a submission of
  the adapter to AROS mainline.
- An independent review of this patch.
