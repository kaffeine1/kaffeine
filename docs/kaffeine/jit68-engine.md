<!--
Copyright (C) 2026, The AROS Development Team. All rights reserved.
Author: Michele Dipace (@kaffeine1)
-->

# `kaffeine-jit68.library`: the 68k JIT as a separate engine

[Transition status](TRANSITION.md) | [Manifesto](MANIFESTO.md) | [Current implementation](../../README.KAFFEINE.md)

Last updated: 16 November 2026.

This page records the 68k JIT in the form it is meant to have: an optional
engine library beside AROS's `m68kemu.library`. It says what is written,
what has been reviewed, what has run and what has not. A review of source
is not a run. The source is in
[`experimental/kaffeine-jit68`](../../experimental/kaffeine-jit68/README.md).
As of the date above, its host checks have run once and passed; nothing of
it has run on AROS.

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
- **Selection.** On the host side, which is not written yet, the variable
  `M68KEMU_CPU` chooses the backend for a whole run: unset or `interpreter`
  is the interpreter as today; `jit` is the engine, and a run that cannot
  use it fails before its first instruction; `auto` uses the engine when
  every check passes and the interpreter otherwise. A diagnostic line names
  the backend actually used.
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
the other registers and the flags are as they were. It does not undo the
read of the divisor. When the divisor lies in a range the host reads itself,
the host has already been asked for it when the engine reports the
exception, once for a word divisor and twice for a long one, and whatever
those reads did on the host side stays done. A host that then executes the
division itself reads the same address again. For a page where a read has a
side effect, those are reads too many. This limit is not qualified, and
nothing here qualifies the engine for such pages.

## Host checks: one run

A test program drives the engine only through the interface, on an AArch64
host (macOS or Linux). It has 25 groups of checks in 13 categories:
discovery; sizes and versions; allocation failures; lifecycle and leaks;
registers; A-line
opcodes; fetch and the host's return addresses, with the map change; the
routing of memory accesses, byte order and the custom chip page; the budget
and block limits; output buffers and counters; invalidation, changed code
and resets of code memory; exceptions, unsupported instructions, the zero
divisor and faults; and the encoder, with programs and cases reused from the
legacy tests.

On 16 November 2026 the checks ran once, on an AArch64 Mac with macOS, with
the clang of the Command Line Tools and the macOS 27.2 SDK, in two
configurations: `-O1` with the address and undefined-behavior sanitizers,
and `-O2` without them. Both compiled with `-Werror` and with the test seams
of the engine. Ten programs ran in all: in each configuration three
compiles, a link and the test program. All ten ended by themselves with
status 0 and wrote nothing on their error output. In each configuration all
25 groups passed, 38519 checks with 0 failures, and the two runs printed the
same output.

That shows that the engine compiles without warnings with that compiler,
and that it passes the cases of those 25 groups in those two configurations
on that Mac. It does not show that the engine follows the interface in every
case, nor anything about a divisor the host reads itself, a build without
the test seams, Linux or another compiler, anything on AROS or with an AROS
compiler, the host side in `m68kemu.library`, a native run, an application
or a speed. The
[README of the source](../../experimental/kaffeine-jit68/README.md) gives
the compiler options of the run.

## State

| Step | State |
| --- | --- |
| Interface version 1 | Written; reviewed as source |
| Engine and translator | Written; reviewed as source; published in `experimental/kaffeine-jit68`; compiled for the host checks only |
| Host checks of the engine | Ran once on 16 November 2026 on an AArch64 Mac: 25 groups, 38519 checks, 0 failures, at `-O1` with sanitizers and at `-O2` |
| Review of the behavior outside the JIT | In progress; its results are published once they are checked |
| Host side in `m68kemu.library`: selection, A-line completion, fallback | Not written |
| Build for AROS AArch64; Shell and `Run` in QEMU | Not done |
| Applications | None run with this engine |
| Multiprocessor systems | Not qualified; version 1 claims one processor |
| Performance | Not measured |

## Next steps

1. Finish the review of what the legacy library does outside the JIT,
   compared with `m68kemu.library`. What it finds decides what has to move
   before the engine is connected.
2. Write the host side in `m68kemu.library`, with the interpreter as the
   default.
3. Build both libraries for AROS AArch64 and run programs from the Shell
   and with `Run` in QEMU, with the interpreter and with the engine.
4. Repeat with applications, starting from IBrowse and Amigram, the two
   programs of the checks of 14 November 2026.

## Where the source is

The interface header, its specification, the engine, the generator of its
translator and the host checks are in
[`experimental/kaffeine-jit68`](../../experimental/kaffeine-jit68/README.md),
with a Makefile for the host checks and the steps to regenerate the
translator. The Makefile and the regeneration steps are written for this
publication and have not been run in that form; the README says which
files differ from the ones that ran, and how.
