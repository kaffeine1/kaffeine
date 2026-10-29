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
has 47 test executables and a thunk-registration audit.

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
calls from child Processes stop before their native thunks. `test_exec_messages`
checks the same gate before file and handler packet adapters, while root
packet dispatch remains available. CreateNewProc remains unsupported.

### Classic RastPort initialization and font queries

`test_gfx_thunks` exercises literal InitRastPort/AskFont dispatch, exact classic
structure boundaries, cached guest font-name identity, copied caller-owned
RastPorts, SetFont metrics and TextLength signed spacing. Guard bytes, malformed
spans, overlapping font storage, closed fonts, native pointer rejection and
repeated-call heap stability are checked before native graphics calls.

### Classic requesters and clipping

`test_window` checks the exact 112-byte classic Requester boundary, retained
native copies, converted decorations, stack links, failed-call rollback,
unsupported flags, table limits and cleanup after caller storage is gone.
`test_layers` checks requester Layer/RastPort identity, snapshot rebuilding,
busy-snapshot rollback and retirement. `test_regions` checks leased clips on
owned window, requester and offscreen layers, including replacement,
borrowed/native-hook refusals and removal before native layer deletion.

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
The audit covers 333 explicit native-vector cases and 564 manual thunk
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
