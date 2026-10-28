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
  native windows, screens, layers, regions, bitmaps, messages, gadgets,
  menus and icons in step.
- Native code calls back into 68k code where AmigaOS does, as with hooks,
  `ExAll()` match hooks, layer backfill hooks and the output routines of
  `RawDoFmt()`, nested up to 16 deep.
- Cooperative guest child tasks created with `AddTask()` and removed with
  `RemTask()` run inside Moira in the same 32 MB guest arena, each with its
  own Moira CPU and checked stack, scheduled by priority and round-robin
  slices across `Forbid()`/`Permit()`, `Disable()`/`Enable()`, `Wait()`,
  `WaitPort()`, `Signal()` and guest message queues.
- A 68k library that AROS does not have, such as the `bullet.library` of
  AmigaOS or Personal Paint's 68k support libraries, loads from disk and
  runs as 68k code, made as Exec makes it.

## What runs today

- 68k HUNK programs run from the Shell and through `Run` on AROS AArch64
  in QEMU. The six bzip2 1.0.8 self-test cases and the seven IJG JPEG
  library 9f self-test steps (`cjpeg`, `djpeg`, `jpegtran`) match their
  references byte for byte.
- Targeted graphics and system tests cover windows, owned and public
  screens (including genuine 8-bit `LUT8` indexed screens on the Raspberry Pi
  framebuffer driver), `DrawInfo` snapshots, display and monitor metadata
  queries, RastPorts, fonts, live and offscreen planar layers, clip regions,
  planar and screen bitmaps (`InitBitMap`, `BltBitMap`, `GetBitMapAttr`),
  IDCMP, GadTools and classic gadgets and menus, keyboard/mouse input,
  `icon.library` metadata snapshots and Workbench application objects
  (`AppWindow`, `AppIcon`, `AppMenuItem`).
- The bridge loads 68k disk libraries and adapts native library paths,
  mathffp and IEEE math calls, the no-window console unit, disk fonts,
  cooperative `AddTask`/`RemTask` child tasks, guest message and semaphore
  queues, `ExAll`/`ExAllEnd`, FileHandle DOS packets and guest ports for
  native DOS handlers (`DeviceProc`, `GetDeviceProc`, `FreeDeviceProc`,
  `GetConsoleTask`, `SetConsoleTask`, `GetFileSysTask`, `SetFileSysTask`).
- The original 68k Personal Paint 7.2.1.0 opens from a native desktop icon
  in AROS AArch64/QEMU. The [recorded demonstration](docs/kaffeine/personal-paint-demo.md)
  starts with a 24-bit Workbench, selects a 1024×768 canvas with a 256-entry
  palette inside the application, draws with rectangles, circles and text,
  saves an IFF/ILBM image and returns to the desktop. Independently decoded
  image pixels match the visible canvas exactly. Desktop icon launch uses
  the standard `CLI` tooltype and Personal Paint's CPU blitting library.
  The unchanged 68k executable runs under Moira, without an “Attention /
  Memory save mode” requester in this session. Earlier direct Shell tests
  also returned exit code zero. Loading saved 1024×768 IFF and PNG images
  still reports “The image is too large”; full application compatibility
  and Workbench `WBStartup` message handling remain open.

The guest `ExecBase.MemList` contains read-only capacity descriptors for
the chip-accessible containment heap, excluding the custom-register window
and the guest stack. Allocator free lists and native memory headers remain
private; `AvailMem()` reports current free space. The other embedded native
Exec lists retain empty guest headers with valid sentinels and native
identities for intercepted lookups. Public guest ports and semaphores have
their own guest nodes.

The production AROS path still uses the Moira interpreter. Complete
application operation, an FPU, new guest DOS processes (`CreateProc`,
`CreateNewProc`), general asynchronous device I/O, communication between
separate guest contexts and physical hardware remain unverified or
unsupported within the current contracts.

## The CPU: Moira now, the kaffeine JIT next

`kaffeine.library` uses `struct KaffeineCPUOps`
([BACKENDS.md](rom/kaffeine/BACKENDS.md)). Moira is the operational 68k
interpreter and the reference for extending the execution engine.

A [standalone AArch64 translation probe](docs/kaffeine/hosted-jit-probe.md)
executes a growing 68k instruction subset through the same interface and
compares results with Moira on the host. Native AROS AArch64 tests execute
the JIT using `AllocMem(MEMF_PUBLIC | MEMF_EXECUTABLE)` and `CacheClearE`;
the instruction corpus carries expected results computed with Moira on
the host, without linking Moira into the native executable. See
the [native probe](developer/debug/test/kaffeine_native/README.md). Its
microbenchmarks do not establish application speed inside AROS. A production
JIT or hybrid backend needs explicit state and transition contracts covering
the relevant exception, stack, prefetch, control-register, per-task CPU and
FPU state, beyond D0-D7, A0-A7, PC and SR alone.

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

The current test and target baseline includes:

| Environment | Result |
| --- | --- |
| macOS arm64 and Linux CI | All 47 host executables pass; the macOS JIT run passes 3,778 checks. The registration audit checks 319 native-vector cases and 544 manual thunks across 18 tables |
| Focused ASan/UBSan JIT build | 304 bitfield boundary checks, 164 extended-instruction checks and 256 randomized programs pass |
| Standalone AROS AArch64 JIT probe in QEMU | `jit_probe` passes 38 checks in Shell and `Run`; a separate instruction corpus passes 490 checks covering 432 programs and eight callback boundary cases in each mode |
| AROS AArch64 in QEMU | Synthetic CLI/GUI probes, the 6-step `bzip2 1.0.8` self-test and the 7-step IJG JPEG `9f` self-test pass in both Shell and `Run` modes |
| Personal Paint 7.2.1.0 on AROS AArch64 in QEMU | Original 68k executable opens from a desktop icon through the `CLI` route, draws, saves a 1024×768 ILBM image and returns to the desktop; [video, screenshots and output](docs/kaffeine/personal-paint-demo.md) are available |
| Native AmigaOS reference | Message, packet, `ExAll`, `icon.library`, `AppIcon`, layer, region, bitmap, screen and `NewWindow` observations match the bridged AROS contract |

The target suite includes deliberate nonzero refusal/exit probes alongside
positive functional checks. The six bzip2 compression/decompression steps
and the seven IJG JPEG compression, decompression and lossless transform
steps pass in both launch modes, with output compared byte for byte. Host,
standalone native AROS, QEMU and native-reference results describe their
respective environments; physical hardware has not been validated.

The original embedded-list regression passed 28 checks on native AmigaOS
and in each AROS Shell/Run execution. Its capacity extension passes 29
checks on AROS AArch64 in both modes; the former empty MemList fails that
new check. The host task/list fixture passes 109 checks, including bounded
Moira traversal, capacity descriptors, reuse and allocation failure.

Personal Paint's editor, selected drawing tools, IFF/ILBM saving and return
to the native desktop have been recorded in QEMU with the original 68k
binary. The published ILBM has eight bitplanes and a 256-entry palette;
its decoded pixels match the visible canvas. The drawing uses a small
subset of that palette. Loading saved 1024×768 images, remaining painting
operations, full Workbench startup, responsiveness and complete resource
cleanup remain open checks. Sampled mathffp SPDiv results include eight
one-ULP differences from AmigaOS.

## Building and testing

Initialize the pinned CPU submodule before building:

```sh
git submodule update --init rom/kaffeine/Moira
make -C developer/debug/test/kaffeine_host test
```

The native AROS library is built through the normal AROS build system.
See the [library build instructions](rom/kaffeine/README.md#building),
the [host suite](developer/debug/test/kaffeine_host/README.md), the
[standalone AROS JIT probe](developer/debug/test/kaffeine_native/README.md)
and the [target suite](developer/debug/test/kaffeine_target/README.md).

## Remaining work

The full [roadmap](docs/kaffeine/MANIFESTO.md#7-a-roadmap-based-on-observable-results)
uses observable application outcomes:

1. Complete useful 68k application operations on AROS ARM64, prioritizing
   Personal Paint's editor, tools, load/save, cleanup and desktop use.
   The current application blocker is loading saved 1024×768 images, which
   reports “The image is too large”.
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

- [Personal Paint demonstration, screenshots and saved output](docs/kaffeine/personal-paint-demo.md).
- [Project manifesto](docs/kaffeine/MANIFESTO.md) and [Italian original](docs/kaffeine/MANIFESTO.it.md).
- [Licenses and component provenance](docs/kaffeine/LICENSING.md).
- [CPU backend contract](rom/kaffeine/BACKENDS.md).
- [Hosted AArch64 translation probe](docs/kaffeine/hosted-jit-probe.md) and [standalone AROS AArch64 JIT probe](developer/debug/test/kaffeine_native/README.md).
- [Library overview](rom/kaffeine/README.md) and [internals reference](rom/kaffeine/INTERNALS.md).
- [Host tests](developer/debug/test/kaffeine_host/README.md).
- [Target programs and QEMU runner](developer/debug/test/kaffeine_target/README.md).
