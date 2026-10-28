# CPU backend boundary

This experimental change prepares `kaffeine.library` for a second CPU backend.
Moira remains the only implemented backend, and the public `RunFile` and
`RunHunk` API does not change. The second backend will be the kaffeine
JIT, a translator of 68k code to AArch64 written for this hosted case, which grows
from the probe of the host suite with Moira running for one step what it
does not translate yet; Emu68 is the reference that the requirements below
come from, and no Emu68 code is imported.

## Responsibilities

| Component | Responsibility |
| --- | --- |
| `kaffeine_cpu.h` and `.c` | Backend handle, register access and execution loop |
| `kaffeine_dispatch.h` and `.c` | Library/LVO lookup and invocation of native thunks |
| `kaffeine_moira.cpp` | Moira instance, guest memory callbacks, reset, exception frames and return sequence |
| Existing thunk and shadow files | Native API adaptation and guest/native data conversion |
| Existing memory and loader files | Guest arena, heap and HUNK loading |

`KaffeineCPU` binds an operation table to a backend-owned state object. The
opaque `cpu` argument passed to every thunk now points to this handle. It
must never be cast directly to a Moira or Emu68 object outside the backend.

The backend owns the handle and its state for the complete execution. The
current adapter creates both for one invocation of `Kaffeine_Execute()`.

## Register and execution contract

The operation table provides access to D0-D7, A0-A7, SR and PC. Register values
use guest widths, independently of the host pointer size. At a native call
boundary, accessors must expose current architectural values; writes by the
thunk must be reflected when guest execution resumes.

`execute()` advances one backend slice. A slice can be one instruction or one
translated block. It is not a cycle count. The backend must return to the
common loop often enough to observe an execution stop through `ctx->running`.
`Kaffeine_RunCPU()` returns D0 as the process result after that stop.

The common loop adds an indirect call per slice. With the current Moira
adapter a slice is one instruction. Its performance impact has not been
measured; these regression checks do not establish throughput or a speedup.

Initialization, CPU model selection, the initial guest stack, exception setup,
entry registers and the final return sentinel remain backend responsibilities.

`flush_code()` is an optional operation for translating backends. A backend
that keeps what it derived from guest code, such as translated blocks, forgets
what came from the guest bytes of the range before that code runs again;
address 0 with `KAFFEINE_FLUSH_ALL` means all guest memory. `CacheClearU()`,
`CacheClearE()` with `CACRF_ClearI` and the loaders of 68k code call it through
`Kaffeine_FlushCode()`, the loaders through `ctx->cpu`, which
`Kaffeine_RunCPU()` sets while it runs. A call from a thunk arrives while
`execute()` waits in the dispatcher, possibly inside a callback. Moira
fetches every instruction from guest memory and leaves the operation NULL.

The cooperative scheduler uses a separate `KaffeineCPUFactory` in
`kaffeine_sched.h`, referenced by `ctx->cpu_factory`; these operations are
outside `KaffeineCPUOps`. Its `create(ctx)` returns a complete reset CPU
sharing the context's 32 MB arena. The scheduler then initializes the child's
registers, user SR, stack pointer and initial PC. `RemTask()`, child return
and context teardown release it through `destroy(ctx, cpu)`, after that CPU
has stopped executing. The Moira adapter installs this factory while
`Kaffeine_Execute()` runs. A backend without a factory cannot run `AddTask()`
children; the standalone JIT probe does not provide one yet.

This interface is deliberately not a complete serialized CPU state. It does
not support switching backends during an execution, transferring FPU state,
host-thread preemption, or arbitrary reentrant callbacks outside
`Kaffeine_CallGuest()`. Those require explicit designs and additional tests
before being advertised.

## Native library dispatch

The backend calls `Kaffeine_DispatchLibraryCall()` for a guest A-line trap,
passing the address of the faulting guest instruction.

For a recognized library jump table the dispatcher:

1. Resolves the LVO, preferring a manual thunk over a generated thunk.
2. Reads and removes the guest JSR return address.
3. Calls the thunk with the backend-independent CPU handle.
4. Stores the return value in D0, unless a Supervisor redirect is pending.
5. Returns the guest return PC and the Supervisor flag to the backend.

The backend retains responsibility for exception framing and resumption.
Moira continues to use its existing PC0/PC adjustment and RTE trampoline.
The existing diagnostic and return-zero behavior for an unimplemented LVO
is preserved. A trap outside the fake library tables is left to the backend.

## What a translating backend needs

Emu68 upstream, the reference for the kaffeine JIT, is a bare-metal,
big-endian runtime with a custom register convention. A hosted backend must
address at least the following before it can be connected here:

- Guest memory access must honor the guest address space. The current AROS
  arena uses offsets relative to `ctx->mem`; guest addresses are not arbitrary
  host pointers. Word and long accesses must retain big-endian semantics on
  a little-endian host.
- Native calls need an ABI boundary that saves the host state, materializes
  guest registers and restores writes made through the accessors. AArch64
  platform-reserved registers must be respected.
- Code generation, executable memory and instruction-cache publication need
  host-specific services. JIT block invalidation must cover guest writes and
  the relevant cache-control APIs, including `CacheClearU`; `flush_code()`
  brings it the requests of those APIs and of the bridge's code loaders.
- Block exits, A-line dispatch, exceptions and the stop sentinel need a
  well-defined return path. FPU behavior must be tested separately.

The Emu68 source reviewed for this work was commit
`2fb0e320f30cf9c47fa250cd84d2bcfff97d40fc` in
[michalsc/Emu68](https://github.com/michalsc/Emu68).

A standalone AArch64 translation probe in the host suite (`hosted_jit.c`) and
its native AROS counterpart ([`developer/debug/test/kaffeine_native`](../../developer/debug/test/kaffeine_native/README.md)),
written for this purpose and not derived from Emu68, meet these requirements
on macOS, Linux and standalone AROS AArch64 (`AllocMem(MEMF_PUBLIC | MEMF_EXECUTABLE)`
and `CacheClearE`) for a subset of the instruction set, behind this interface
and with the production dispatcher and callback primitive; see
[the probe's note](../../docs/kaffeine/hosted-jit-probe.md). It implements
`flush_code()`, which `CacheClearU()`, `CacheClearE()` and the loaders of
68k code call.

## Compatibility remains a separate concern

The existing TagItem heuristics, callback handling, native pointer/handle
translation and Exec synchronization behavior are not repaired by this
extraction. A faster CPU backend does not supply missing API semantics.

In particular, `RunFile` and the transparent `RunHunk` path need separate
validation. `Kaffeine_LoadHunksFromMemory()` applies HUNK relocations relative
to guest addresses. The transparent path receives a seglist that DOS has
already relocated for its native addresses. On non-m68k hosts the DOS HUNK
loader now records the relocations it applies (`GSLI_68KRelocs`,
`struct HunkRelocTable`), and `Kaffeine_LoadHunks()` uses that record to
rebase its copy in the guest arena. `RunHunk` refuses a seglist without a
record. The host tests cover the DOS loader, the record and the rebased
copy, and the complete path from the Shell has run on AROS AArch64 in QEMU
(see `developer/debug/test/kaffeine_target`).

## Validation

See [the host test instructions](../../developer/debug/test/kaffeine_host/README.md).
Host tests use the production CPU adapter and memory manager with stubbed
AROS APIs. Running their AArch64 Linux builds under QEMU validates that
CPU/compiler path; it does not establish that the AROS kernel, GUI or a
complete Amiga application has run on AArch64.
