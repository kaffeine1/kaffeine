# kaffeine host regression tests

The tests build selected production sources with small host-side AROS API
stubs. They are suitable for checking the CPU/backend boundary and HUNK
loading without first building a complete AROS system.

## Prerequisites

- A C++ compiler supporting C++20, a GNU C compiler (or clang), GNU make, Python 3
  and standard C/C++ development libraries.
- The Moira submodule at the revision recorded by the AROS checkout:

```sh
git submodule update --init rom/kaffeine/Moira
```

The checkout used for this change records Moira commit
`1efd69467ca13b27b2fb40febd5cb31dbecdea5f`. The AROS base revision is
`ae45f4644f80de542fb6af320c5d5fb5a1bc9925`.

## Native host

From the repository root:

```sh
make -C developer/debug/test/kaffeine_host test
```

Build files default to `developer/debug/test/kaffeine_host/build`. To place
them elsewhere, pass an absolute `BUILD_DIR`. Source include paths are
resolved from the Makefile location; changing the working directory does
not change them.

## Continuous integration

The `kaffeine host suite` workflow runs this Makefile, the native
`dos_process_host` fixture, and these separate compatibility suites:

- `kaffeine_domain_host`: shared guest modules and relocation ownership.
- `kaffeine_dos_broker_host`: native DOS workers, synchronous RPC and cleanup.
- `kaffeine_domain_run_host`: driver ownership and root/child lifetimes.
- `kaffeine_domain_process_host`: process startup and refused-cleanup recovery.
- `kaffeine_remember_host`: Remember allocation ownership and retirement.
- `kaffeine_videocontrol_host`: read-only ViewPortExtra queries and Screen lifetimes.
- `kaffeine_gfxbase_host`: typed graphics.library initialization and DefaultFont ownership.
- `kaffeine_process_integration_host`: shared driver startup, Signal and owner cleanup.

The standalone suites are not invoked by this directory's `make test`.
CI calls each `run_tests.py` normally and with `--sanitize`, using separate
temporary build directories, GCC/G++, AddressSanitizer and
UndefinedBehaviorSanitizer. Linux sanitizer runs enable leak detection.
Changes to any sibling `kaffeine_*_host` directory also trigger the workflow;
add new standalone runners to its explicit suite list.

These are host checks. Native AROS and application acceptance still require
their separate target tests.

## AArch64 Linux under QEMU user mode

With an AArch64 GNU C++ toolchain and QEMU user mode installed:

```sh
make -C developer/debug/test/kaffeine_host \
    BUILD_DIR="$PWD/build-kaffeine-aarch64" \
    CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc \
    RUNNER="qemu-aarch64 -L /usr/aarch64-linux-gnu" test
```

Adjust the compiler command and QEMU sysroot for the local installation.
Use a distinct build directory for each compiler or architecture: dependency
files track source changes, not changes to compiler options or target flags.

This command runs AArch64 Linux test executables. It does not boot AROS or
validate native AROS libraries, drivers, scheduling, GUI applications or
Darwin-specific JIT constraints.

## Coverage

The Makefile is the authoritative list of executables. The current suite
has 49 test executables and a thunk-registration audit.

| Area | Coverage |
| --- | --- |
| CPU and loaders | Real Moira instructions, A-line dispatch, registers and condition codes, HUNK loading and relocations, callback faults and guest bounds |
| Exec | Heap and pools, lists, signals, task snapshots, embedded ExecBase list headers and bounded Moira callback traversal, semaphores, public ports, guest message queues, I/O requests and timer boundaries |
| DOS | Handle identities and lifetimes, buffers, packet thunks and FileHandle actions, handler ports (`DeviceProc`/`GetDeviceProc`), formatting, CLI state, argument parsing, data structures and directory traversal (`ExAll`/`ExAllEnd`) |
| Graphics, Layers and Intuition | RastPorts, fonts, display and monitor metadata, owned and public screens, `DrawInfo`, live and offscreen planar layers and backfill hooks, clip regions, bitmaps (`InitBitMap`/`BltBitMap`/`GetBitMapAttr`), windows, IDCMP, requests, menus, gadgets and Workbench objects |
| Libraries and devices | Library-path lookup, mathffp and IEEE conversion, locale hooks, `icon.library` metadata snapshots, no-window console calls and diskfont snapshots |
| CPU backend extension | Production AArch64 translator, hosted differential tests and library mode contracts |
| Guest scheduling | Separate Moira and JIT CPUs, task identity/priority, deferred Wait/WaitPort, Forbid/Disable, message exchange, native signal routing, removal, bounds, allocation rollback and cleanup |

Tests compile production adapters as C and use recording native API
collaborators. Literal LVO dispatch, big-endian layouts, bounds, aliases,
register preservation and resource lifetimes are checked. The message and
packet tests use the actual queue and FileHandle adapters; guest addresses
are never treated as native pointers.

The current Task snapshot publishes the guest stack bounds and initial saved
stack pointer. For a Process, `pr_StackSize` reports the actual guest
reservation and `pr_StackBase` is a BPTR to its high end. `test_exec_tasks`
checks those fields independently of the host stack, invalid geometry and
non-Process current tasks. The guest reservation remains 64 KiB; the native
Shell stack setting does not resize it.

### Keyboard and input boundaries

`test_exec_io` checks real unit-zero opens, private native request storage,
KBD_READMATRIX data/length/actual conversion, 32-byte clamping, malformed and
protected output spans, native errors, rollback and cleanup. Classic
CreateExtIO NT_MESSAGE markers work before the first DoIO. Input handler
registration/event commands return IOERR_NOCMD without reaching a native
handler; direct device vectors stop with rc 20.

### Classic service processes

`test_guest_tasks` checks CreateProc with separate CPU instances, bounded
Process/stack/MemList allocations, message handshakes, virtual SIGB_DOS,
priorities, Forbid/Wait, child Exit, quarantine and failure rollback. DOS
calls from these legacy CreateProc children stop before their native thunks.
`test_exec_messages` checks that gate before file and handler packet adapters.
The typed CreateNewProc route instead imports an owned 68k HUNK with trusted
relocation metadata into the shared guest arena, creates a separate guest CPU
and Process, and gives it a private native DOS broker. Exec ports and waits
remain cooperative inside that domain.

`test_process_tags` checks the bounded supported tag subset and typed
module/home-lock transfers. Raw segment values, guest entry/exit callbacks,
stream transfers, and unsupported tags are refused before native forwarding.
The standalone domain, broker, driver and recovery suites check their separate
ownership and lifetime contracts; these host checks do not establish
application startup or native UI behavior.

`test_dos_thunks --process-trace-only` checks numeric startup diagnostics and
refusal cases separately from the supported typed route. The tag walker is
limited to 64 item/control steps and the trace to 16 active tags. Tests
cover control-only cycles, invalid and truncated lists, wrapping skips,
independent traversal/output limits and unchanged guest memory. Names,
entry points and seglists are reported as raw values without following
their pointers. Rejected inputs do not create a native process.

`test_dos_thunks --loadseg-trace-only` checks numeric LoadSeg/NewLoadSeg
provenance traces. The native BPTR is recorded at full width alongside
the delivered opaque 32-bit guest token, without reading segment contents or printing
the filename. Success, failure, rejected strings and converted-tag
cleanup preserve the loader's IoErr even when logging or freeing tags
changes it. `test_dos_thunks --segments-only` checks the implemented registry's
classification, context ownership, stale handles, bounded reservations,
RunCommand loans, transfers and cleanup. Numeric traces alone do not prove
executable kind or process readiness.

### Classic RastPort initialization and font queries

`test_gfx_thunks` exercises literal InitRastPort/AskFont dispatch, exact classic
structure boundaries, cached guest font-name identity, copied caller-owned
RastPorts, SetFont metrics and TextLength signed spacing. Guard bytes, malformed
spans, overlapping font storage, closed fonts, native pointer rejection and
repeated-call heap stability are checked before native graphics calls.

### Caller-owned drawing and GELS initialization

The graphics suite dispatches Text, Draw, RectFill, WritePixel, ReadPixel and
BltTemplate through per-call native adapters for checked planar bitmaps and
copied window RastPorts. It checks changed scalar copyback, unchanged caller
pointers and original window state, source/target aliases, clipping and bridge
ownership. Layer suites check target resolution against actual owned records,
closed targets, lock ownership, altered descriptors and borrowed layers.
InitGels tests compare the entire guest arena against the exact classic
sentinel protocol, with atomic refusal of malformed or overlapping objects.
All 14 other GELS vectors stop before native calls or callbacks.

### Classic requesters and clipping

`test_window` checks the exact 112-byte classic Requester boundary, retained
native copies, converted decorations, stack links, failed-call rollback,
unsupported flags, table limits and cleanup after caller storage is gone.
`test_layers` checks requester Layer/RastPort identity, snapshot rebuilding,
busy-snapshot rollback and retirement. `test_regions` checks leased clips on
owned window, requester and offscreen layers, including replacement,
borrowed/native-hook refusals and removal before native layer deletion.

### Window refresh scopes

`test_layers` also covers the typed Intuition and GadTools refresh adapters,
using the unchanged public native Begin/End bodies over recording layer/update
primitives. Four refresh groups check normal/GZZ lock counts, update OOM,
manual aliases, owner/nesting/contention refusals, preserved guest fields,
implicit Info safety gates and ordered abort cleanup through the real CPU
loop. `test_layers_private` retains independent offscreen-layer coverage.
See [scope and limits](../../../../docs/kaffeine/window-refresh.md) for the
contract and focused normal/sanitizer commands. Host checks do not establish
native target OOM behavior or application rendering acceptance.

### ExAll bridge

`test_dos_thunks --exall-only` runs nine ExAll groups: every NDK entry type
and comment layout, small-buffer scans, overflow and zero sizes, restarts,
invalid pointers and tokens, native errors, patterns and hooks, hook stops
and nested DOS calls, and control ownership and cleanup. The directory fake
advances each native FIB's disk key. The guest heap returns distinct addresses
and records frees, and native FIB allocations must balance after cleanup.
Hooks use the production callback runner with a controlled CPU execute step.

```sh
make -C developer/debug/test/kaffeine_host BUILD_DIR=/tmp/exall-host test
/tmp/exall-host/test_dos_thunks --exall-only
make -C developer/debug/test/kaffeine_host BUILD_DIR=/tmp/exall-asan \
    CFLAGS="-g -fsanitize=address,undefined -fno-sanitize-recover=all" \
    CXXFLAGS="-g -fsanitize=address,undefined -fno-sanitize-recover=all" \
    test_dos_thunks
ASAN_OPTIONS=detect_leaks=0 /tmp/exall-asan/test_dos_thunks
```

Use fresh build directories.

### DOS packet bridge

`test_dos_packet_thunks` exercises the production AllocDosObject and
FreeDosObject paths for DOS_STDPKT and all five packet LVOs. The existing
extractor takes their bodies and private helpers verbatim from production.
The queue, routing, heap, FileHandle and shadow modules are real objects;
native DOS calls are recording fakes. Generated LVO fallbacks and unrelated
object allocators are poisoned.

The groups cover the 32-packet ownership table and layout, SendPkt/WaitPkt,
ReplyPkt exchanges, AbortPkt, DoPkt's private reply storage and D0/D1/IoErr,
NIL packet ports, message length normalization and rc20 boundaries.
`test_dos_packets` also covers reads, writes, seek modes and errors, scalar
handler actions, classic InfoData conversion and buffer checks, unknown
actions and FileHandles with no native handler. The task and scheduler
suites check the Process port layout and reserved DOS signal ownership.

Three typed-Process packet groups also link the real scheduler into
`test_dos_packets`. They exercise direct `PutMsg()` and nested `SendPkt()`/`DoPkt()`
through the existing file/handler adapters on a recording child broker, with
separate native Process, directory and IoErr state. Root and child reply signals,
original message/FIFO identity, file-read errors, malformed/mixed packets,
callback/LayerInfo/owner refusals, transport retry and unchanged classic-task
limits are checked. A real `Kaffeine_RunCPU()` case preserves a marked return
code 20 after a refused worker callback and a subsequent void result of zero.
The broker and CPUs in this lane are collaborators; it does not claim native
thread scheduling, target execution or application GUI acceptance.

The Process packet port lives inside the registered Task shadow: guest queues
take exactly that port, and the SIGB_DOS exception holds for it alone, with
and without the scheduler.

`test_dos_handlers` covers the guest ports of native handlers with the same
objects and the handler module: one port for each native port and the limit
of 32, `pr_ConsoleTask` and `pr_FileSystemTask`, `SetConsoleTask()` with the
`fh_Type` of a file, `DeviceProc()` and its IoErr(), the DevProcs of
`GetDeviceProc()` through a multi-assign and their cleanup, and every
converted filesystem action: names as C strings (the message objects are
built with `AROS_FAST_BSTR`, as for AROS aarch64), locks, FileInfoBlocks,
InfoData and DateStamps, actions that never reach the handler, and the
packets that stop the program.

```sh
make -C developer/debug/test/kaffeine_host BUILD_DIR=/tmp/pkt-host -j4 test
make -C developer/debug/test/kaffeine_host BUILD_DIR=/tmp/pkt-host-asan -j4 \
    CFLAGS="-g -fsanitize=address,undefined -fno-sanitize-recover=all" \
    CXXFLAGS="-g -fsanitize=address,undefined -fno-sanitize-recover=all" \
    test_dos_thunks test_dos_packets test_dos_packet_thunks test_dos_handlers test_exec_messages \
    test_exec_tasks test_guest_tasks test_icon test_display
for test in test_dos_thunks test_dos_packets test_dos_packet_thunks test_dos_handlers \
    test_exec_messages test_exec_tasks test_guest_tasks test_icon test_display; do
    ASAN_OPTIONS=detect_leaks=0 /tmp/pkt-host-asan/"$test" || exit 1
done
```

For disposable mutation sources, pass `KAFFEINE_DIR=/tmp/.../rom/kaffeine`
on the make command line and use another fresh BUILD_DIR. Keep mutation
tooling outside this directory and restore the disposable source after
each mutation. The original rom/kaffeine stays unchanged.

## Registration audit

`make test` also runs `test_thunk_registrations.py`. Its default inputs are
the AROS configuration files in this tree. Optional NDK and AROS FD inputs
can extend the audit; use the script's `--help` for the available options.
The audit covers 339 explicit native-vector cases and 584 manual thunk
registrations across 20 tables. External SDK files are not included in this
repository.

## Sanitizers and build variants

Use a separate absolute `BUILD_DIR` for every compiler and flag combination.
For example, to build the two message-boundary executables with ASan/UBSan:

```sh
make -C developer/debug/test/kaffeine_host \
    BUILD_DIR=/tmp/kaffeine-host-sanitize \
    CXXFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
    CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
    test_exec_messages test_dos_packets
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
    /tmp/kaffeine-host-sanitize/test_exec_messages
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
    /tmp/kaffeine-host-sanitize/test_dos_packets
```

Leak detection is disabled in this example because some legacy API fixtures
retain allocations. The message/handle tests have explicit allocation-balance
checks. The normal host suite, a complete unsigned-char build and eight
focused ASan/UBSan executables have passed. These results do not establish
native AROS scheduling, complete application startup or hardware operation.

## Target validation

Use the [target suite](../kaffeine_target/README.md) for public API behavior
on AROS and for comparison with native AmigaOS. QEMU completion, guest
program completion, expected exit status and output comparisons are separate
parts of the target verdict.
