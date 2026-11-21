<!--
Copyright (C) 2026, The AROS Development Team. All rights reserved.
Author: Michele Dipace (@kaffeine1)
-->

# `kaffeine-jit68.library`: the 68k JIT as a separate engine

[Transition status](TRANSITION.md) | [Manifesto](MANIFESTO.md) | [Current implementation](../../README.KAFFEINE.md)

Last updated: 20 November 2026.

This page records the 68k JIT in the form it is meant to have: an optional
engine library beside AROS's `m68kemu.library`. It says what is written,
what has been reviewed, what has run and what has not. A review of source
is not a run. The source is in
[`experimental/kaffeine-jit68`](../../experimental/kaffeine-jit68/README.md).
As of the date above, its host checks pass in both of their
configurations, each run on its own; the engine has been built as a
library for AROS AArch64, and a probe of that library has run on AROS under
QEMU. Nothing of it has run with `m68kemu.library`.

## Two libraries

- `m68kemu.library`, in `rom/m68kemu` of AROS mainline, is the 68k
  compatibility library. Its interpreter, Moira, stays the default.
  `RunHunk` and `RunFile` keep their entry points, arguments and results,
  and without the engine the library behaves as it does today.
- `kaffeine-jit68.library` is to hold the optional engine and nothing else.
  It translates 68k code into AArch64 code and runs it on guest memory that
  `m68kemu.library` owns. Library calls, nested 68k calls, child tasks,
  interrupts, the custom chip and CIA pages and the program lock stay in
  `m68kemu.library`, where they are today.
- The JIT compiled into the legacy `kaffeine.library` is a different thing.
  Its results are evidence for that library only, even though the new
  engine's translator derives from it.

## The interface, version 1

The interface is a
[C header](../../experimental/kaffeine-jit68/include/kaffeine-jit68-abi-v1.h)
and a
[written specification](../../experimental/kaffeine-jit68/include/kaffeine-jit68-abi-v1.md).
What matters for its behavior:

- **Discovery.** `m68kemu.library` opens `kaffeine-jit68.library` and asks
  its one function for an engine of interface version 1. Each versioned
  record of the interface (the engine's table of operations, the host's
  services, the memory map, and the buffers for exit reasons and counters)
  begins with its own size, and both sides check versions, sizes and
  capabilities before anything runs. Plain records inside them, such as an
  address range of the map, carry no size of their own. An absent library
  or a mismatch is found before the first 68k instruction.
- **Memory.** Guest memory belongs to the host and is described by a map:
  where it starts, how large it is, and up to 16 address ranges whose reads
  or writes the host wants to see, such as the custom chip and CIA pages.
  Every other data access goes straight to memory, big-endian, at any
  alignment. Instruction fetch always goes straight to memory and never
  calls the host.
- **Map changes.** The host changes the map only between engine calls and
  increments a generation number when it does. The engine reads that number
  at the start of every run. The case that needs it is the bitplane range
  that appears when a program draws into the planes of a screen.
- **Execution.** A run executes a bounded number of instructions and
  returns with a reason: the budget is used up; an A-line opcode, which is
  how a 68k program calls a library and which the host completes; a PC the
  engine cannot fetch, which is how the host's return addresses arrive; a
  68040 integer-unit exception, such as division by zero or TRAP; an
  instruction the engine does not execute; a host access that failed; an
  engine fault. The engine never delivers an exception or calls a library
  itself, and an instruction it does not execute is reported, never skipped.
- **One CPU mode.** Version 1 runs in supervisor mode, the mode the
  canonical library gives every CPU. An instruction that would leave it
  (RTE, STOP, a write of the whole status register) stops the run before it
  does anything.
- **Changed code.** After the host loads code, or a program clears the
  instruction cache, the host calls `invalidate`; translated code whose 68k
  bytes have changed does not run again.
- **Selection.** On the host side the variable `M68KEMU_CPU` chooses for a
  whole run: unset or `interpreter` is the interpreter as today; `jit` is
  the engine under the interpreter, and a run that cannot have it fails
  before its first instruction; `auto` uses the engine when it is there and
  works, and the interpreter otherwise. A line names what runs the program.
- **Scope.** Version 1 is for AROS AArch64 with one processor. It claims no
  multiprocessor support, no MMU, no FPU, no complete 68040 and no switch of
  backend during a run.

## The engine source

The translator is not new code. A script generates it from the translator
of the legacy library, `rom/kaffeine/kaffeine_jit.c` as of commit
`033602cd` of this repository. It copies checked line ranges of that file
and refuses to generate when a range differs from what was recorded. Of the
definitions it handles, 111 are kept as they are, 23 are adapted to the
interface, 2 come from the legacy header, 26 are new, and 33 are left out:
the CPU interface of the legacy library, its custom chip emulation and its
own dispatch of A-line opcodes, all of which belong to `m68kemu.library` in
the new design. The one later change to that file in this repository adds
hooks to the legacy blitter emulation and does not apply, for the same
reason.

These counts are those of the generator before the change to divisions
described below. That change adapts the two division emitters and the list
of slow-path kinds, and adds the probe of the divisor and one field of the
CPU state. The generator carries the same edits. Run in that form on 20
November 2026, it wrote the published translator byte for byte, with 110
definitions kept as they are, 24 adapted, 2 from the legacy header, 27 new
and 33 left out.

## Two corrections from the review

The review of the first revision of the source found two faults. Both are
corrected in the current source, each with its own group of host checks.

1. **Translations kept across a map change.** The first revision kept its
   translated code when the host had changed the map, so a block translated
   under the old map could still read or write directly an address that the
   new map sends to the host. Now a new generation empties the engine's code
   memory for that program, and every CPU translates again under the new
   map. That costs a full retranslation at every map change, which the
   engine counts; a finer check is possible later. Checks:
   `map-change-fetch`.
2. **Division by zero with a stepping operand.** For DIVU and DIVS, word and
   long, with the divisor at `(An)+` or `-(An)`, the first revision reported
   the division-by-zero exception after it had already stepped the address
   register. The exception must leave the state as it was before the
   instruction. Now the zero-divisor path undoes the step before it
   returns; the nonzero path is unchanged. Checks: `zero-divisor-state`, ten
   operand forms, each with a zero and a nonzero divisor.

What the second correction restores is the register state of the guest:
the address register gets back the value it had before the instruction, and
the other registers and the flags are as they were. It could not undo a
read of the divisor that the host had already made, which mattered when the
divisor lay in a range the host reads itself. The next section removes that
case.

## A divisor the host would read

A division now looks at where its divisor is before it reads it. When the
read would go to the host (the divisor lies, in whole or in part, in a
range whose reads the host wants to see, or outside guest memory), the
engine does not read it and does not call the host. It ends the run at the
division with `STOP_UNSUPPORTED` and the text "divisor read through the
host", with the PC at the division and nothing of it done, whatever the
divisor's value: the registers, the flags and memory are as they were
before the instruction. The test is made each time the translated code
runs, so the same translation divides when its address register points to
ordinary memory and declines when it points into such a range. A divisor
read directly from memory is read and divided as before, a zero one with
the division-by-zero exception and the address register step undone.

This covers DIVU and DIVS, word and long, the long ones with a 32-bit and a
64-bit dividend, with the divisor at `(An)`, `(An)+`, `-(An)`, `d16(An)`,
`d8(An,Xn)`, an absolute long address and `d16(PC)`. It includes a word
that only ends in such a range, a long divisor whose second word is in it,
a divisor past the end of guest memory and an address that wraps at the
top. The memory indirect forms are outside the instructions the engine
translates and are declined without reading a pointer. Checks:
`div-host-read`.

A declined division has to be executed by the host itself, once, and then
the run can go on after it. The adapter in `m68kemu.library` gives it to
the interpreter, as every instruction the engine does not execute. A
native case has run it: a program with two divisions on a custom chip
register gave the interpreter's results with the engine, which ran the
other instructions.

## Host checks

A test program drives the engine only through the interface, on an AArch64
host (macOS or Linux). It has 26 groups of checks in 13 categories:
discovery; sizes and versions; allocation failures; lifecycle and leaks;
registers; A-line
opcodes; fetch and the host's return addresses, with the map change; the
routing of memory accesses, byte order and the custom chip page; the budget
and block limits; output buffers and counters; invalidation, changed code
and resets of code memory; exceptions, unsupported instructions, the zero
divisor, a divisor the host would read, and faults; and the encoder, with
programs and cases reused from the legacy tests.

On 17 November 2026 the checks ran on an AArch64 Mac with macOS, with the
clang of the Command Line Tools and the macOS 27.2 SDK, in two
configurations: `-O1` with the address and undefined-behavior sanitizers,
and `-O2` without them. Both compiled with `-Werror` and with the test seams
of the engine. The two configurations ran separately, on the same six
source files. In each, three compiles, a link and the test program ended by
themselves with status 0 and wrote nothing on their error output, and all
26 groups passed: 40861 checks with 0 failures. The output of the `-O2` run
is byte for byte the output of the sanitizer run. An earlier version of the
source, with 25 groups and without the change to divisions, had passed on
16 November 2026 with 38519 checks in each configuration. On 20 November
2026 the published Makefile was run as it is, with Apple clang 21.0.0, and
gave the same result: 26 groups and 40861 checks with 0 failures in each
configuration, with the same output.

That shows that the engine compiles without warnings with that compiler,
and that it passes the cases of those 26 groups in those two configurations
on that Mac. It does not show that the engine follows the interface in every
case, a host that executes a declined division, a build without the test
seams, Linux or another compiler, anything on AROS or with an AROS
compiler, the host side in `m68kemu.library`, a native run, an application
or a speed. The
[README of the source](../../experimental/kaffeine-jit68/README.md) gives
the compiler options and how the count of checks was settled.

## On AROS: the library and a probe

The engine is now also an AROS library. A configuration file gives
`kaffeine-jit68.library` its one function at LVO -30, and a small source
file holds that function and the two C library functions the engine calls
that a library module does not get. The four engine files are compiled
unchanged. A script builds the library outside an AROS tree with the tools
and the SDK of an AROS build. In the branch proposed to AROS as
[pull request 2357](https://github.com/aros-development-team/AROS/pull/2357)
it has a build rule, for AArch64 targets only.

On 20 November 2026 the library was built for AROS AArch64 with the cross
compiler of a build of AROS master `b6dcaeea` for raspi-aarch64, and a
native probe ran once on that system under QEMU 11.0.0, machine `raspi3b`.
The probe uses the library and the interface alone, without
`m68kemu.library`: it makes a domain over 64 KiB of its own memory and
runs small 68k programs whose results it knows, among them a loop resumed
across runs, a return to an address outside memory, accesses that go to
its callbacks, changed code after `invalidate`, and a division by zero
and ILLEGAL, which the engine reports and does not execute. All 20 checks
passed. The engine's counters reported translated blocks and executable
memory, which on AROS comes from `AllocMem(MEMF_EXECUTABLE)` and is
published with `CacheClearE`.

That shows that the engine translates and runs 68k code on AROS AArch64
under QEMU through its library. It does not show the engine with
`m68kemu.library`, a 68k program, physical hardware, more than one CPU
object or domain at a time, or a speed. The
[README of the source](../../experimental/kaffeine-jit68/README.md) gives
the build and the numbers.

## Under m68kemu.library: the adapter

The host side is written as a patch for `rom/m68kemu` of AROS master. It
does not replace the interpreter with the engine: it runs the engine under
the interpreter. Every CPU of a run is still an interpreter CPU, which
keeps the registers the library thunks use and takes every exception,
interrupt and library call. With `M68KEMU_CPU=jit` or `auto` each execution
loop of the library tries an engine slice first, and whatever instruction
the engine stops at without executing it is one step of the interpreter.
Library calls, `Supervisor()`, RTE, guest exception handlers and interrupts
are therefore the existing code, not a second copy of it.

On 20 and 21 November 2026 the patched library ran on AROS AArch64 under QEMU with
the engine library beside it: twenty-three cases with eight small 68k programs
started from the Shell, with the interpreter alone and with the engine. All
gave the expected results, among them, under the engine, a program with
its own trap handler, RTE and `Supervisor()`, a program with a process of
its own, a software interrupt and a vertical blank server, a program
with divisions the engine declines, a BOOPSI class whose dispatcher
intuition calls back, the two CIA timer programs of AROS's own native
test, the refusal of `jit` without the library, and the fallback of `auto`. In one timed loop of twenty million
instructions the engine ran all but 19 of them, and the loop took 30 ticks
of 1/50 s against about 160 on the interpreter; that is a measurement on an
emulated machine. The [adapter page](jit68-adapter/README.md) has the
design, the patch, the test programs and every result, and says what has
not been tried: applications and hardware.

## State

| Step | State |
| --- | --- |
| Interface version 1 | Written; reviewed as source |
| Engine and translator | Written; reviewed as source; published in `experimental/kaffeine-jit68`; compiled for the host checks and as an AROS AArch64 library |
| Host checks of the engine | Pass on an AArch64 Mac, 17 November 2026: 26 groups, 40861 checks, 0 failures, at `-O1` with sanitizers and at `-O2`, each configuration in a run of its own; again on 20 November 2026 with the published Makefile |
| Engine library for AROS AArch64 | Built outside an AROS tree, 20 November 2026; a native probe through the interface passes under QEMU: 20 checks, 0 failures |
| Review of the behavior outside the JIT | In progress; its results are published once they are checked |
| Host side in `m68kemu.library` (the adapter): selection, the engine under the interpreter, fallback | Written as a patch for AROS master; its design and its source read by a second reader; see the [adapter page](jit68-adapter/README.md) |
| `m68kemu.library` with the adapter, built for AROS AArch64 | Built outside an AROS tree, 20 November 2026; twenty-three cases from the Shell pass under QEMU, with the interpreter and with the engine, one of them with a process, a software interrupt and a vertical blank server. `Run`, hooks called by native libraries and CIA timers not tried |
| Applications | None run with this engine |
| Multiprocessor systems | Not qualified; version 1 claims one processor |
| Performance | One loop timed under QEMU: 147 to 165 ticks on the interpreter, 29 or 30 with the engine. Nothing else measured |

## Next steps

1. Finish the review of what the legacy library does outside the JIT,
   compared with `m68kemu.library`. What it finds decides what has to move
   before the engine is connected.
2. Get AROS's own `test_m68kemu` to run to its end on the test guest, with
   and without the engine; it stops at a sub-test that needs a font there.
3. Repeat with applications, starting from IBrowse and Amigram, the two
   programs of the checks of 14 November 2026.
4. Follow the review of AROS pull request 2357, which proposes the engine
   library and the adapter to mainline.

## Where the source is

The interface header, its specification, the engine, the generator of its
translator and the host checks are in
[`experimental/kaffeine-jit68`](../../experimental/kaffeine-jit68/README.md),
with a Makefile for the host checks, the steps to regenerate the
translator, and the files that make the engine an AROS library with its
probe. The README says which files the host checks compiled, and what the
Makefile, the generator and the probe gave when they were run.
