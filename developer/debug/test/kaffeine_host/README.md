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
has 38 test executables and a thunk-registration audit.

| Area | Coverage |
| --- | --- |
| CPU and loaders | Real Moira instructions, A-line dispatch, registers and condition codes, HUNK loading and relocations, callback faults and guest bounds |
| Exec | Heap and pools, lists, signals, task snapshots, embedded ExecBase list headers and bounded Moira callback traversal, semaphores, public ports, guest message queues, I/O requests and timer boundaries |
| DOS | Handle identities and lifetimes, buffers, read packets, formatting, CLI state, argument parsing, data structures and directory traversal |
| Graphics and Intuition | RastPorts, fonts, screens, windows, IDCMP, requests, menus, gadgets and Workbench objects |
| Libraries and devices | Library-path lookup, mathffp and IEEE conversion, locale hooks, no-window console calls and diskfont snapshots |
| CPU backend extension | Hosted AArch64 translation probe behind the same CPU interface |

Tests compile production adapters as C and use recording native API
collaborators. Literal LVO dispatch, big-endian layouts, bounds, aliases,
register preservation and resource lifetimes are checked. The message and
packet tests use the actual queue and FileHandle adapters; guest addresses
are never treated as native pointers.

## Registration audit

`make test` also runs `test_thunk_registrations.py`. Its default inputs are
the AROS configuration files in this tree. Optional NDK and AROS FD inputs
can extend the audit; use the script's `--help` for the available options.
The extended audit covers 278 explicit native-vector cases and 399 manual
thunk registrations. External SDK files are not included in this repository.

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
