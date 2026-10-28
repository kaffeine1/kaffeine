# Native AROS AArch64 JIT probe

`jit_probe` runs the existing Kaffeine block translator inside standalone
AROS AArch64. It links the production CPU loop, dispatcher and guest-call
primitive. It is a native executable with its own small guest arena; the
`kaffeine.library` application backend remains Moira.

The translator requests code memory with
`AllocMem(MEMF_PUBLIC | MEMF_EXECUTABLE)` and publishes emitted instructions
with `CacheClearE(..., CACRF_ClearD | CACRF_ClearI)`. This path is enabled
only for standalone AROS AArch64. Hosted AROS has a different memory
protection contract and is excluded.

Build with the AROS AArch64 compiler and its matching SDK:

```sh
make -C developer/debug/test/kaffeine_native \
  AROS_CC=aarch64-aros-gcc \
  AROS_SYSROOT=/path/to/AROS/Developer \
  BUILD_DIR=/tmp/kaffeine-native
```

Copy the resulting `jit_probe` ELF executable to AROS and run it from a
Shell, or through `Run`. A successful run returns 0 and prints:

```text
jit_probe: 38 checks, 0 failed
```

The checks cover encoder reference words, executable memory and publication
counters, a translated return, big-endian guest memory, a native thunk
through the production dispatcher, a nested translated callback, changed
guest code followed by invalidation and retranslation, and the explicit
failure boundary for an unsupported instruction. Each case releases its
backend and guest arena.

The initial AROS ARM/QEMU run passed all 38 checks in both Shell and `Run`
modes, and the extended instruction-coverage candidate additionally passed
431 instruction-corpus checks across 429 programs in both Shell and `Run`
modes. This establishes execution of the translated subset on that VM. It
does not establish physical Raspberry Pi cache behavior, full instruction
coverage, guest-task support, application performance or Personal Paint on
the JIT. Those require subsequent backend integration and target validation.
