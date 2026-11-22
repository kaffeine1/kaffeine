<!--
Copyright (C) 2026 Michele Dipace. All rights reserved.
Author: Michele Dipace (@kaffeine1)
-->

# Building an Optional 68k-to-AArch64 JIT for AROS

*Technical case study · status checked 22 November 2026 · proposed for the Kaffeine repository*

## The engineering problem

AROS runs 68k Amiga software through `m68kemu.library`, which combines guest memory and operating-system integration with the Moira interpreter. Translating every guest instruction is useful for compatibility, but compute-heavy loops can spend much of their time in the interpreter. The challenge was to add dynamic translation without duplicating AROS's library calls, task handling, interrupts or exception machinery, and without changing the default execution path.

The work grew out of Kaffeine's earlier, experimental `kaffeine.library`. That legacy library has run real 68k applications under an AROS AArch64 guest. The published Personal Paint video uses Moira; separate editor and drawing checks use the legacy JIT. Those demonstrations establish behavior of that legacy implementation and its matched preview. They do **not** establish application compatibility for the new upstream engine. [Kaffeine's implementation overview](https://github.com/kaffeine1/kaffeine/blob/master/README.KAFFEINE.md) records the distinction.

## Design: an engine beneath the interpreter

The [upstream interface, merged as AROS PR #2359](https://github.com/aros-development-team/AROS/pull/2359), leaves Moira as the default. `m68kemu.library` retains ownership of guest memory, registers at system-call boundaries, library thunks, child tasks, interrupts and locking. It can load a separate engine library when `M68KEMU_CPU` is set to `jit` or `auto`; with no setting, it uses the interpreter. If `jit` is requested and the engine is unavailable, execution fails before a 68k instruction runs. `auto` falls back to Moira.

The proposed [`m68kjit.library` engine, AROS PR #2360](https://github.com/aros-development-team/AROS/pull/2360), translates 68k blocks into AArch64 code. It accepts a map of guest memory and host-observed address ranges, plus a bounded instruction budget. The engine returns when it reaches an A-line library call, exception, unsupported instruction, unavailable PC or slice boundary. For an instruction it declines, `m68kemu.library` executes that instruction with Moira, then can resume the engine. This keeps exceptional and operating-system behavior in the existing runtime. The [interface specification](https://github.com/aros-development-team/AROS/blob/master/rom/m68kemu/ENGINE.md) defines the versioned records, memory rules and exit reasons.

The translator was adapted from Michele Dipace's earlier Kaffeine translator. A checked generator in the [Kaffeine engine source](https://github.com/kaffeine1/kaffeine/tree/master/experimental/kaffeine-jit68) records which definitions are reused, adapted or omitted. The AROS proposal uses the neutral library name `m68kjit.library`; the Kaffeine tree retains the historical project name. This is an adaptation to AROS's engine interface, not a claim that every result from the legacy library carries over.

## Hard boundaries handled explicitly

- **Host-observed memory.** Ordinary guest data accesses use the guest memory map; accesses to ranges reserved for host handling go through callbacks. A map generation change invalidates translated code so a block cannot keep using a stale access route. Divisions that would read a divisor through a host callback are declined before that read and completed by Moira.
- **Exceptions and state.** The engine returns register and PC state at instruction boundaries. A division-by-zero path restores stepped address registers before reporting the exception. The review also found and corrected signed long-division overflow reporting.
- **Code lifetime.** A domain starts with about 1.5 MiB of code and indexing storage and grows only when needed, up to about 22 MiB. Translation is dropped on relevant map or code invalidation. A program that modifies executed code without a cache operation may see the old translation, matching the documented 68040 instruction-cache model.
- **Host-specific AArch64 details.** The native AROS engine may use x18 under its ABI, while the macOS test build substitutes x23 because Darwin does not preserve x18 in the same way. The native probe exercises callbacks that clobber x16–x18. These are separate, stated test environments.

The [engine README in the proposed AROS branch](https://github.com/kaffeine1/AROS/blob/feature/kaffeine-jit68-library/workbench/libs/m68kjit/README.md) describes the memory policy and supported execution mode.

## What has been demonstrated

| Evidence | Result | Scope |
| --- | --- | --- |
| [AROS PR #2359](https://github.com/aros-development-team/AROS/pull/2359) | Merged on 21 November 2026 | Optional engine interface and interpreter integration; **no JIT engine in this PR** |
| [AROS PR #2360](https://github.com/aros-development-team/AROS/pull/2360) host validation | 28 groups, 71,636 checks, no failures in the reported sanitizer and optimized builds on an AArch64 Mac | Engine behavior covered by those tests; not a general compatibility proof |
| [AROS PR #2360](https://github.com/aros-development-team/AROS/pull/2360) differential testing | Two million randomized single-instruction cases in the main run, with no reported differences after the documented division-flag normalization; five additional ten-million-case seeds reported no differences | Many cases were declined or stopped before execution; division flags use the manual where the configured Moira behavior differs |
| [AROS PR #2360](https://github.com/aros-development-team/AROS/pull/2360) AROS build and native tests | Selected `raspi-aarch64` targets built; the native probe passed 23 checks; `test_m68kemu` and small hand-encoded 68k programs passed with the engine under QEMU 11.0.0 | One emulated target, selected build targets and small test programs |
| [AROS PR #2360](https://github.com/aros-development-team/AROS/pull/2360) timed loop | In one 20-million-instruction loop, 165 ticks with Moira and 17 with the engine; other recorded runs ranged from 147–200 and 17–34 ticks respectively | QEMU/TCG timing of this loop only; **not** an application benchmark or hardware speedup |

The native runs used an AROS AArch64 guest built from a specified master revision with the Raspberry Pi bootstrap changes later merged as [PR #2331](https://github.com/aros-development-team/AROS/pull/2331). The PR reports the test programs, library selection and counters per case. It does not report a complete distribution build.

## Current limits and next evidence

As checked on 22 November 2026, the engine PR #2360 is **open**. The engine targets AArch64, one processor and supervisor-mode execution. It has no FPU or MMU implementation of its own; instructions it cannot translate go to Moira. Physical Raspberry Pi execution and real-application tests of the proposed `m68kjit.library` plus upstream `m68kemu.library` remain to be shown. Kaffeine's legacy application runs cannot fill that gap.

The next credible milestones are an accepted upstream engine review, a reproducible build from current AROS master, application-level correctness tests with the upstream pair, and measurements on physical hardware with a clearly defined baseline. Until then, the JIT is a tested experimental engine proposal, not a general performance or compatibility guarantee.
