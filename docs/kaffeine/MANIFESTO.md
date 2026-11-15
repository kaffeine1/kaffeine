# Kaffeine: vision and long-term goal

[Original Italian version](MANIFESTO.it.md) | [Current implementation](../../README.KAFFEINE.md)

## 1. The vision

Build a shared open source home for software from the Amiga ecosystem,
able to evolve with hardware while preserving the heritage of its different
platforms.

AROS is the native operating system. For 68k software its compatibility
layer is `m68kemu.library`, a library that AROS already has. Kaffeine is the
name of a JIT engine for that library: an optional CPU backend beside its
interpreter, not a library of its own and not a replacement for one.

The wider aim, applications from different architectures and environments
on the same desktop, belongs to AROS as a whole. It would need engines and
contracts of their own. It is not a promise about `m68kemu.library`, which is
the 68k compatibility library, nor about Kaffeine, which is a 68k JIT engine.

The goal is not to create another Amiga faction or demand that existing
systems be abandoned. It is to reduce fragmentation for users: choosing a
program for what it enables them to do, rather than for the CPU or branch
of the ecosystem for which it was written.

ARM64 is the first host, not the boundary of the project. The 68k is the
first compatible environment, not necessarily the last.

This is a long-term direction, not a claim of compatibility already achieved.
Before making a change, check the current repository and test evidence;
do not assume that the goals described here are already implemented.

## 2. The experience we want to offer

Users should be able to launch a supported application from the desktop
or the Shell and use it as part of AROS, without manually managing a
separate emulator or virtual machine.

Compatibility should progressively cover the complete usage cycle: startup,
interaction, file access, dependency loading, saving, closing and resource
release. Where possible, it should extend to integration such as the
clipboard, drag and drop, icons, requesters and communication between
applications.

The desired result is to run supported original binaries without requiring
recompilation or changes specific to the compatibility layer. Recompiled programs and
dedicated fixtures remain valuable development tools, but do not replace
verification with existing software.

Personal Paint is the first substantial graphical milestone: start it,
reach the editor, use its tools, load and save images, close it correctly,
then verify desktop startup and responsiveness.

The necessary fixes should implement general contracts. Avoid behavior
selected by the name of Personal Paint or another application, except for
justified, isolated and documented exceptions.

## 3. What must be preserved: compatibility independent of the host CPU

The most important asset of the project is verified knowledge of the
behavior of Amiga APIs, objects and formats.

A fix to a list, message, font or library should remain reusable when moving
from ARM64 to RISC-V or another host. The same compatibility problems
should not have to be solved again for every processor.

The architecture should keep these layers separate:

| Layer | Responsibility |
| --- | --- |
| Guest compatibility environment | Calling conventions, API behavior, objects, structures, handles and callbacks of the supported environment. |
| Loading | Executables, relocations, libraries, initialization and startup conventions specific to the guest format and environment. |
| Guest CPU execution | Interpreting or translating instructions, registers, flags, exceptions and the declared CPU model. |
| Host platform services | Executable memory, code publication, caches, synchronization and other services depending on the CPU and operating system. |

This separation does not require a preemptive rewrite. Introduce abstractions
where they remove real dependencies and are supported by implementations
and tests.

Do not turn the system into a universal framework today merely to anticipate
hypothetical needs.

## 4. Portability: C and C++ are tools; explicit contracts are required

Common code should remain as independent of the host architecture as
possible. C and C++ help, but do not automatically make assumptions about
pointers, alignment, byte order and calling conventions portable.

Guest addresses must remain distinct from host pointers. Guest fields
must have explicit widths and representations. Native structure offsets
must be derived or verified for the target; a layout obtained on x86_64
must not be assumed valid everywhere.

Data conversion must follow meaning. A numeric value must not become a
pointer merely because it falls within the guest address range.

An AArch64 JIT generator remains specific to AArch64 even when written in C.
A future RISC-V host will need its own code-generation backend and execution
services. The compatibility bridge, loaders and reusable tests should be
preserved.

The aim of a future port is therefore:

> Change what depends on the host, preserving what we have learned and
> verified about Amiga software.

## 5. CPU strategy: correctness first, then measured acceleration

Moira should remain an operational reference and a comparison baseline
for the Kaffeine JIT. Comparisons with the interpreter should be accompanied
by tests with independent expected results and, when needed, measurements
on the reference system.

The JIT should accelerate execution without silently reducing compatibility.
Registers, flags, memory, callbacks and observable results must follow the
declared CPU model and contracts.

Any fallback between JIT and interpreter requires an explicit design for
shared state and transitions. Do not assume that copying D0-D7, A0-A7, PC
and SR is sufficient in every situation. Separate stacks, exception state,
prefetch, control registers and any FPU state must be considered within the
relevant scope.

Handling modified code must distinguish invalidating guest translations
from publishing new host instructions to the CPU cache.

Measure performance on real applications as well as microbenchmarks,
including cold startup, warm execution, compilation time, memory use and
the cost of transitions between guest code and native libraries.

The goal is to make applications faster while preserving their behavior,
not to maximize an isolated benchmark.

## 6. Future expansion: PPC and other Amiga environments

In the long term, AROS could host further CPU engines and compatibility
environments beside the 68k one. That would be new work with interfaces of
its own: not an extension promised for `m68kemu.library`, and not a wider
role for Kaffeine.

One possible evolution is PowerPC code support, starting with a bounded
scope such as WarpOS and evaluating PowerUP separately. Compatibility
with MorphOS or AmigaOS 4 applications would be a further project with
its own dependencies, ABIs and services.

Emulating a CPU does not implement the operating system a program expects.
Do not claim compatibility with an application family solely because its
instructions execute.

Shareable components, such as memory management, JIT services, conversions
and resource management, should remain reusable. CPU state and specific
calling conventions must remain distinct: PowerPC must not be forced into
an interface designed exclusively around 68k registers.

Multicore is a future opportunity, not an automatic property. Concurrent
engines will require explicit rules for shared memory, atomics, messages,
callbacks and object lifetimes. Core affinity should be a justified choice,
not a requirement imposed in advance.

This vision does not authorize starting all these workstreams now.
Immediate work must remain focused on the agreed scope.

## 7. A roadmap based on observable results

Phases may overlap where independent work exists, but each advance needs
verifiable criteria.

| Phase | Expected result |
| --- | --- |
| 68k compatibility on AROS ARM64 | Real CLI and graphical applications complete useful operations and release resources correctly. Personal Paint is the priority graphical reference. |
| Kaffeine JIT acceleration | The same binaries work with the JIT or a verified hybrid mode, with measured benefits and no regressions against the interpreted baseline. |
| Consolidation on real hardware | Interaction, stability, memory use and performance are verified on supported machines, beyond QEMU. |
| Portability to other hosts | The common base builds and passes tests on a second host, with RISC-V as a candidate, starting with the interpreter before any dedicated JIT. |
| Further guest environments | A PPC engine and an initial bounded compatibility environment run real software while reusing the common infrastructure. |

The order of individual pull requests should follow actual application
blockers and architectural risks still requiring verification, rather than
only the number of functions that can be implemented.

## 8. Development and verification principles

Each change should state the contract it implements, the defect it fixes,
the tests performed and the remaining limits. Prefer focused pull requests
and regressions that fail on the previous implementation.

Always distinguish host tests with simulated APIs, actual JIT code
execution, execution inside AROS in QEMU, real hardware tests and
comparisons with the reference Amiga system.

Where relevant, verification should include error paths and cleanup,
pointer identity, two-way object synchronization, nested callbacks and
resource lifetimes. A program opening a window is not yet a fully working
application.

Record expected negative results without confusing them with regressions.
Retain source revisions, binary hashes, system configuration and reproducible
results. Green CI that skips AArch64 tests does not certify execution of
the AArch64 JIT.

Thunk, assertion or commit counts are not a compatibility percentage.
The primary measure is what applications can do correctly.

## 9. An open platform built to last

The compatibility system must remain open source, documented and open to
contributions. Preserve the attribution, provenance and licenses of its
components. Applications retain their own licenses; the project must not
depend on unauthorized redistribution of proprietary software.

The ambition to welcome the whole ecosystem must not become a promise
that every program will work without limits. For software requiring
specific hardware or unavailable services, evaluate separately whether
additional compatibility components or a more isolated environment are
needed.

The purpose is not to erase differences between Amiga platforms, but to
prevent those differences from making their software unusable in the future.

## 10. Working inside AROS

The 68k compatibility library is AROS's own `m68kemu.library`, in
`rom/m68kemu`. This work corrects and extends that library where it is. It
does not replace it with another, rename it, or put a second public
interface beside the one AROS has.

Corrections that every user of the library needs go to AROS as small,
separate contributions: native structure layouts made for each target, the
loading of HUNK files, safe behavior on multiprocessor systems. They do not
depend on the JIT. Whether and how they are accepted is for the AROS
maintainers to decide.

The Kaffeine JIT is a separate series of changes: an optional backend behind
the library's CPU interface. The interpreter and the behavior AROS has today
remain the default and stay available.

The long-term CPU reference is the Motorola 68040, including the
floating-point behavior Amiga software relies on. A capability is declared
to software only when it is implemented and verified for the selected
backend. An MMU, if it comes, is a virtual component of the guest: it must
never control the memory of the native system.

These are not goals: a fork that replaces AROS components; a second emulator
library under another name; an installer that silently swaps the library of
a system; a claim of support for a system or a machine on which the work has
not been run.

## 11. Where the work stands

This document describes a direction. What is implemented, with which
library, backend and sources each result was obtained, and what is still
being moved onto `m68kemu.library`, is recorded in the
[current implementation](../../README.KAFFEINE.md) and in the
[transition status](TRANSITION.md).

## The guiding principle for every decision

Build what is needed today to make applications work, without tying
tomorrow's compatibility to one CPU, one engine or one person.

**AROS: an open source home for every part of the Amiga ecosystem. Kaffeine: a JIT engine in its service.**
