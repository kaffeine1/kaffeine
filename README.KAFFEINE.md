# Kaffeine: current implementation and technical direction

The [manifesto](docs/kaffeine/MANIFESTO.md)
([Italian original](docs/kaffeine/MANIFESTO.it.md)) describes an open source
home for software from the Amiga ecosystem. AROS is the native operating
system. The compatibility bridge should preserve verified API behavior,
objects and formats as CPU engines and host hardware evolve.

The current implementation starts with 68k software on AROS AArch64.
It builds on AROS's original `m68kemu.library`, now `kaffeine.library` in
`rom/kaffeine`; its upstream base is AROS
`ae45f4644f80de542fb6af320c5d5fb5a1bc9925`.
The long-term roadmap is a direction, separate from the verified behavior below.


## How it works

- A 68k HUNK program loads through `LoadSeg()` as a native one does. On a
  host that is no 68k, DOS records the relocations that it applies, and
  `RunCommand()` hands the program to `RunHunk()` of `kaffeine.library`,
  which copies its hunks into a 68k address space of 32 MB of its own,
  moves the relocations there and runs it.
- Each library that the program opens gets a 68k library base whose jump
  table is made of A-line traps. A call leaves the CPU, and the dispatcher
  runs a thunk that calls the native AROS function with the 68k registers.
  Most thunks are generated from the library descriptions; the manual ones,
  each described in [INTERNALS.md](rom/kaffeine/INTERNALS.md), convert the
  structures that differ between the two sides and keep 68k copies of the
  native windows, screens, messages, gadgets and menus in step.
- Native code calls back into 68k code where AmigaOS does, as with hooks
  and the output routines of `RawDoFmt()`, nested up to 16 deep.
- A 68k library that AROS does not have, such as the `bullet.library` of
  AmigaOS, loads from disk and runs as 68k code, made as Exec makes it.

## What runs today

- 68k HUNK programs run from the Shell and through `Run` on AROS AArch64
  in QEMU. The six bzip2 1.0.8 self-test cases match their references byte for byte.
- Targeted graphics tests cover windows, public screens, RastPorts, fonts,
  IDCMP, GadTools and classic gadgets and menus, keyboard/mouse input,
  and Workbench application objects.
- The bridge loads 68k disk libraries and adapts native library paths,
  mathffp calls, the no-window console unit, disk fonts, guest queues and
  bounded DOS read packets.
- Personal Paint loads fonts and image I/O libraries and completes packet
  reads and WaitPort/GetMsg replies. Startup then reaches a contained
  callback memory-access fault; the editor and full application lifecycle
  have not been validated.

The production AROS path still uses the Moira interpreter. Complete
application operation, an FPU, new guest processes, general asynchronous
device I/O, cross-task messages and physical hardware remain unverified or
unsupported within the current contracts.

## The CPU: Moira now, the kaffeine JIT next

`kaffeine.library` uses `struct KaffeineCPUOps`
([BACKENDS.md](rom/kaffeine/BACKENDS.md)). Moira is the operational 68k
interpreter and the reference for extending the execution engine.

A [hosted AArch64 translation probe](docs/kaffeine/hosted-jit-probe.md)
executes a subset through the same interface and compares results with
Moira. Its host-side microbenchmarks do not establish application speed
inside AROS. A production JIT or hybrid backend needs explicit state and
transition contracts covering the relevant exception, stack, prefetch,
control-register and FPU state, beyond D0-D7, A0-A7, PC and SR alone.

The compatibility environment, loading, guest CPU execution and host
services are distinct architectural responsibilities. The current 68k
interface is not a generic PowerPC state representation. C and C++ do not
make AArch64 code generation portable to a different instruction set.
Introduce further abstractions where a concrete implementation and tests
demonstrate the need.

Emu68 remains a reference for translation work; no Emu68 code is included.
Performance must be evaluated with real applications as well as
microbenchmarks, including cold startup, compilation, memory use and
guest/native transitions.

## How it is tested

- Host tests compile production adapters with simulated native APIs.
- The hosted AArch64 probe executes generated host code and compares
  declared behavior with Moira and independent expected results.
- Target programs run inside AROS AArch64 in QEMU, including keyboard and
  mouse interaction, completion markers and file-output comparisons.
- Native AmigaOS reference runs establish behavior that cannot be inferred
  safely from prototypes alone.
- Original application binaries test useful operations and complete
  lifecycles; dedicated fixtures and recompiled programs complement them.

These are separate evidence scopes. Green CI or a passing host build is
not proof of real hardware behavior or of AArch64 JIT execution when that
test is skipped. Thunk and assertion counts are not compatibility percentages.
Personal Paint and AmigaOS resources used for reference tests are licensed
and are not supplied in this repository.

## Licenses

AROS and the changes of this project are under the AROS Public License 1.1,
and Moira under MIT. Emu68 (MPL 2.0) is not included.
[LICENSING.md](docs/kaffeine/LICENSING.md) has the details, with the open
point of the provenance of the NDK data.

## Verified scope

| Environment | Result |
| --- | --- |
| macOS arm64 and Linux GCC 14 | All 38 host executables pass |
| Unsigned-char host build | All 38 executables pass |
| Focused ASan/UBSan build | Eight executables pass |
| AROS AArch64 in QEMU | 63 boots and 126 Shell/Run executions give the expected results |
| Native AmigaOS reference | 26 message checks, 28 packet checks and 20 raw observations match AROS |

The target results include sixteen deliberate nonzero refusal/exit probes.
The six bzip2 compression and decompression cases pass in both launch
modes, with output compared byte for byte. Host, QEMU and native-reference
results describe their respective environments; physical hardware has
not been validated.

Personal Paint progresses through font loading, DOS packet reads and
WaitPort/GetMsg replies. Its startup is still incomplete because it reaches
a contained memory-access fault during a 68k callback. Sampled mathffp
SPDiv results include eight one-ULP differences from AmigaOS.

## Building and testing

Initialize the pinned CPU submodule before building:

```sh
git submodule update --init rom/kaffeine/Moira
make -C developer/debug/test/kaffeine_host test
```

The native AROS library is built through the normal AROS build system.
See the [library build instructions](rom/kaffeine/README.md#building),
the [host suite](developer/debug/test/kaffeine_host/README.md) and the
[target suite](developer/debug/test/kaffeine_target/README.md).

## Remaining work

The full [roadmap](docs/kaffeine/MANIFESTO.md#7-a-roadmap-based-on-observable-results)
uses observable application outcomes:

1. Complete useful 68k application operations on AROS ARM64, prioritizing
   Personal Paint's editor, tools, load/save, cleanup and desktop use.
   The current next blocker is its callback memory-access fault.
2. Accelerate the same binaries with the Kaffeine JIT or a verified hybrid
   mode, preserving behavior and measuring end-to-end benefits.
3. Validate interaction, stability, memory and performance on real hardware.
4. Validate a second host, with RISC-V as a candidate, starting with the
   interpreter before any dedicated JIT.
5. Explore a PPC engine and a bounded guest environment such as WarpOS;
   assess PowerUP separately. MorphOS and AmigaOS 4 application compatibility
   would require further ABI and service work.

These future directions do not start parallel implementation work by
themselves. Immediate changes follow observed application blockers and
the agreed scope.

## Documentation

- [Project manifesto](docs/kaffeine/MANIFESTO.md) and [Italian original](docs/kaffeine/MANIFESTO.it.md).
- [Licenses and component provenance](docs/kaffeine/LICENSING.md).
- [CPU backend contract](rom/kaffeine/BACKENDS.md).
- [Hosted AArch64 translation probe](docs/kaffeine/hosted-jit-probe.md).
- [Library implementation and supported boundaries](rom/kaffeine/INTERNALS.md).
- [Host tests](developer/debug/test/kaffeine_host/README.md).
- [Target programs and QEMU runner](developer/debug/test/kaffeine_target/README.md).
