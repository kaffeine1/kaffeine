# kaffeine Internals

**Author:** Fabian Schmieder ([@metaneutrons](https://github.com/metaneutrons))

Deep technical reference for the kaffeine.library implementation. For an overview,
see [README.md](README.md).

## Table of Contents

- [Memory Model](#memory-model)
- [CPU Integration](#cpu-integration)
- [Thunk System Details](#thunk-system-details)
- [Shadow Struct Engine](#shadow-struct-engine)
- [Shadow Struct Layouts](#shadow-struct-layouts)
- [Device I/O Path](#device-io-path)
- [Custom Chip Emulation](#custom-chip-emulation)
- [Thunk Generator Toolchain](#thunk-generator-toolchain)
- [Test Infrastructure](#test-infrastructure)
- [Known Bugs](#known-bugs)

## Memory Model

### Containment Buffer

`Kaffeine_CreateContext()` allocates a single 32 MB buffer with
`AllocMem(MEMF_CLEAR | MEMF_31BIT)`, with 32 KB more of zero bytes after it
(`KAFFEINE_MEM_GUARD`) that no m68k address reaches: native code that counts a
text of the program as far as its NUL, as Intuition counts the Buffer of a
string gadget up to 32766 bytes and writes at its end, stops in them however
the program left its text, also while the input handler edits it. All m68k
addresses are byte offsets into this buffer. Translation is trivial:

```c
host_ptr = ctx->mem + m68k_addr;    // m68k → host
m68k_addr = host_ptr - ctx->mem;    // host → m68k
```

The `m68k_to_host_or_shadow()` variant checks the shadow registry first (for
native AROS structs that appear at m68k addresses), then falls back to
containment. The word and longword helpers check the complete range with
`addr >= mem_size || mem_size - addr < width` before forming a host pointer.
An access outside the arena, including one whose end would wrap at 2^32,
returns zero on read and is silently dropped on write outside callbacks.
During a guest callback, an invalid CPU access stops the program with rc 20
and suppresses subsequent accesses from the pending instruction. Moira's word, reset
and disassembly callbacks use those same helpers, after the existing
sentinel and custom-chip cases where applicable.

Moira performs longword bus accesses as two independent word callbacks. At
0xFFFFFFFE the second callback wraps to address 0, and at the end of the
arena a valid first word can still be accessed. These CPU bus operations
are not atomic range-checked longword helpers. The adapter cannot recover
the original longword address or width from an isolated callback; changing
that requires a Moira interface change. The pinned submodule is unchanged.

### Address Map

```plain
0x000000 - 0x0003FF      Exception vectors (1 KB, 256 × 4 bytes)
0x000100                 RTE instruction (planted at init)
0x000108                 Supervisor trampoline: ADDQ.L #8,SP; JMP (A5)
0x000120 - 0x00015F      Callback return tokens (16 levels, four bytes each)
0x000160                 Sentinel: the return address of the program
0x000400 - 0x0103FF      Library base region (64 KB)
                         Each library: num_vectors × 6 bytes (A-line + 2×NOP)
                         Base address follows jump table (AmigaOS convention)
0x010400 - ~0x01FEFFFF   Heap (first-fit, 8-byte aligned, 8-byte headers)
  0x00DFF000 - 0x00DFFFFF  Custom chip registers, reserved inside the heap
~0x01FF0000 - 0x01FFFFFF Stack (64 KB, grows downward)
```

### Heap Allocator

- **Strategy**: First-fit with splitting and forward/backward coalescing
- **Alignment**: 8-byte (`(size + 7) & ~7`), with overflow checked before rounding
- **Header**: Ordinary allocations reserve 8 bytes in containment memory
  (big-endian ULONG size + ULONG next for free chunks); absolute allocations
  use only a native ownership record, as described below
- **Splitting**: If remainder ≥ 16 bytes after allocation; allocates from the END of the free block
- **Custom chips**: On an Amiga the custom chips answer at `$DFF000-$DFFFFF`,
  and the CPU callbacks keep that window for them, although it lies inside
  the heap bounds. `Kaffeine_HeapInit()` makes the part inside the bounds a
  `HEAP_RESERVED` interval, so an empty heap is two free intervals, 13.9 MB
  below the window and 17.9 MB above it. First fit serves the lower one
  first. Neither `AllocMem()` nor `AllocAbs()` reaches the window, `FreeMem()`
  cannot release it, and a resize keeps it out whenever the bounds include it
- **Free**: Address-ordered insertion into free list with coalescing
- **Clear**: `MEMF_CLEAR` clears the entire returned payload, including rounding
  and unsplittable slack, for both new and reused `AllocMem()`/`AllocVec()` blocks
- **Memory class**: One arena serves `MEMF_ANY`, `MEMF_PUBLIC`, `MEMF_CHIP`
  and `MEMF_FAST`, including combinations. Other flags keep their previous
  ignored behavior. In particular, `MEMF_REVERSE` does not change first fit
  or allocation from the end of a chunk, and `MEMF_NO_EXPUNGE` has no effect
  because there is no guest expunge/retry path. Physical DMA, locality,
  page-alignment and semaphore-protection requirements are not implemented.
- **Ownership**: Native `M68KHeapAllocation` intervals partition the heap,
  including free intervals. An AVL tree keyed by block address gives exact
  ownership lookups, insertion and removal in O(log n), without scanning live
  blocks. Allocated intervals retain the actual span, requested size and pool
  owner. The subtree's `largest_free` value skips allocated subtrees when
  locating a free predecessor or successor.
- **Free list**: Free intervals also form an address-ordered native doubly
  linked list. Allocation retains the first-fit walk and end splitting.
  Release locates the insertion point through the AVL tree, then merges with
  adjacent free intervals. Splitting reserves a new native record before any
  guest write; whole-block allocation reuses the free record, and release
  needs no allocation. Native allocation failure returns zero with guest
  memory unchanged.
- **Validation**: The native intervals and links are authoritative. Guest free
  headers are checked mirrors of the expected size and next address, never
  pointers to follow or boundaries to trust. Every operation checks the free
  head. Allocation checks each chunk visited by first fit; release checks the
  free predecessor and successor before writing either header. Accounting
  checks every free interval. An inconsistent mirror fails the affected call
  without writes. An untouched corrupt mirror may remain until visited; it
  cannot steer an allocation, overlap a live block or cause a guest-link loop.
  Allocated guest headers are ignored for ownership and release, so forging
  one or copying a pool header cannot establish ownership.
- **Release**: `FreeMem()`/`FreeVec()` accept only the exact start of a live
  ordinary or absolute allocation. They retain the existing size-independent
  release behavior. NULL, interior pointers, double frees and blocks owned
  by pools, tag arrays, locales or MemList groups are ignored by these
  calls. Zero-size `AllocMem()`/`AllocVec()` calls return NULL, as Exec
  guarantees; internal heap callers retain their zero-payload allocation.

`AvailMem()` at -216 reads D1 and reports guest payload capacity. With no
query flag it sums `chunk_size - 8` for each free chunk; `MEMF_LARGEST`
returns the maximum of those capacities. The reported largest size is a
request that `AllocMem()` can satisfy, including the eight-byte private
header and allocation rounding. A fragmented free total is a sum, not a
promise of one contiguous allocation. `MEMF_TOTAL` reports what the empty
heap offers, regardless of live blocks: each free interval of the empty heap
less its header, `heap_end - heap_start - 8` when the custom chip window is
outside the bounds.
`MEMF_LARGEST` takes precedence over `MEMF_TOTAL`, following
`nommu_AvailMem()` in `rom/exec/memory_nommu.c`. The same single-arena flag
policy applies to all queries. Native ownership records are outside this
guest accounting; native bookkeeping exhaustion can still make an
allocation fail. On a corrupt free list queries return zero.

For n live blocks and f free intervals, allocation costs O(log n) plus its
first-fit walk, release costs O(log n), and `AvailMem()` costs O(f). Free
intervals are coalesced, so f <= n + 1. On 64-bit hosts a native record is
64 bytes. Bookkeeping uses
64(n + f) bytes, at most 128n + 64, plus native allocator overhead. The
context adds one pointer for `heap_first_free`. Pool objects count as live
blocks; their membership links reuse fields in the same record. There is no
separate native allocation per pool member.

`Kaffeine_HeapInit()` initializes the native tree and guest mirror. It can
resize an empty heap, as the small host fixtures do, but refuses a reset
while any allocation is live. The arena and context are freed if the initial
native record cannot be allocated. The other production heap callers in
`kaffeine_cli.c`, `kaffeine_m68klib.c`, `kaffeine_readargs.c`, `kaffeine_run.c`,
`kaffeine_shadow.c` and `kaffeine_thunks.c` use only returned payload addresses;
they neither read nor maintain these private headers. The guest header layout
is retained for host fixtures and diagnostics.

### Exec pools

The pool API uses guest addresses, with no native Exec pool calls.
`CreatePool()` at -696 allocates a 16-byte opaque object in the guest heap:

| Offset | BE32 field |
| --- | --- |
| 0 | Diagnostic magic (`GUEST_POOL_MAGIC`) |
| 4 | Allocation requirements |
| 8 | Original puddle-size hint |
| 12 | Original threshold-size hint |

The native allocation record marks this object as a pool. Its magic is
diagnostic, not proof that an arbitrary address is a pool. `AllocPooled()`
at -708 validates that handle before reading its requirements and allocates
directly from the guest heap, recording the owner privately. It applies
the pool's `MEMF_CLEAR` on every allocation, including reused blocks and
requests larger than the puddle or threshold. The hints reserve no memory
and do not control allocation granularity: there are no data puddles.
Zero and inverted hints are accepted, following the actual AROS
`CreatePool()` implementation, which does not enforce the historical
`threshold <= puddle` restriction and ignores the threshold.

`FreePooled()` at -714 requires the exact live block, its owning pool and
the original requested size. NULL memory, zero sizes, unknown or foreign
blocks and mismatched sizes do nothing. `DeletePool()` at -702 releases
all live blocks owned by that pool, then its object. With no other heap
changes the free total returns to its value before `CreatePool()`.
Each pool has a native doubly linked member chain. Deletion visits only
that chain, preflights the free neighbours of every affected block before
writing, and releases each block through the AVL tree. Its cost is
O((members + 1) log n), independently of unrelated pools and free intervals.
NULL or unknown pool handles fail allocations with zero and make release
and deletion no-ops. AROS explicitly allows `DeletePool(NULL)`, while the
bridge additionally guards NULL for allocation and release.

Context destruction releases every remaining native ownership record and
the guest arena, including abandoned pools. These APIs use C status values,
not exceptions. They do not add concurrent guest execution or synchronization.

### Absolute allocations and MemLists

The manual `AllocAbs()` vector at -204 reads D0 (byteSize) and A1
(location), as both NDK 3.2 `exec_lib.fd` and `rom/exec/exec.conf` specify.
It follows the alignment in `nommu_AllocAbs()` in
`rom/exec/memory_nommu.c`: round location down to eight bytes, add that
adjustment to byteSize and round the extent up to eight bytes. The returned
guest address is the rounded start, which can differ from A1. The entire
rounded interval must lie inside one free heap interval. Allocated payloads
and their private headers are both occupied memory.

`Kaffeine_HeapAllocAbs()` splits that interval into an absolute allocation
and up to two free intervals. It reserves all needed native records before
changing ownership or guest bytes. Eight-byte free fragments are retained.
The absolute allocation has a native ownership kind and no private guest
header, so its contents are preserved, including an allocation at
`heap_start`. Free mirror headers may change outside the claimed range,
as in Exec. `FreeMem()` accepts the exact returned address and releases the
recorded rounded interval, keeping its existing size-independent policy.
An interior or repeated free has no effect. `AvailMem()` still reports the
payload capacity available to ordinary `AllocMem()` calls, subtracting
eight bytes per free interval; it does not promise absolute allocations
at arbitrary locations.

`AllocEntry()` at -222 and `FreeEntry()` at -228 read A0. The 68k layout is
independent of native pointer size and byte order:

| Offset | Guest field |
| --- | --- |
| 0 | 14-byte Node |
| 14 | BE16 ml_NumEntries |
| 16 + 8 * i | BE32 me_Reqs on input, me_Addr on output |
| 20 + 8 * i | BE32 me_Length |

`Kaffeine_HeapAllocEntry()` checks the complete specification, including
all UWORD-counted entries, and snapshots requirements and lengths in native
scratch memory before any heap writes. It also checks every existing free
mirror before starting, so pre-existing corruption cannot interrupt
rollback. A successful call allocates a new guest MemList and its nonempty
entries through the same heap. The output Node is zeroed, the count and
lengths are retained, and the requirements union becomes a guest address.
Each entry uses the ordinary heap flag policy, including `MEMF_CLEAR`.
Like `NewAllocEntry()` in `rom/exec/newallocentry.c`, a zero-length entry
has a NULL address and does not fail. A zero-entry list is a valid 16-byte
allocation. No native pointer becomes a guest result.

The native records distinguish a MemList and its members from ordinary
allocations and retain their membership independently of guest links.
`FreeEntry()` preflights the count, every address and length, and the free
neighbours before releasing any member or the list. Node links, type,
priority and name can be changed by the caller. The count and entries
must still describe the allocated group. Raw `FreeMem()`/`FreeVec()` do
not release these group-owned allocations; use `FreeEntry()`. This is a
deliberate restriction relative to native Exec's separate `FreeMem()`
calls, preventing a modified list or an early member free from releasing
unrelated allocations. Context destruction also reclaims abandoned groups.

| Failure case | Result and reason |
| --- | --- |
| AllocAbs: zero size, wrapping or outside-arena request, rounded interval outside the heap, or any occupied byte | NULL, without changing the heap. The exact rounded span must be wholly available. |
| AllocAbs: corrupt affected free mirrors or native split-record exhaustion | NULL, without guest writes or partial ownership changes. Split records are reserved first. |
| AllocEntry: NULL, truncated or outside-arena specification, corrupt free mirrors, scratch failure, or MemList allocation failure | `0x80000000 | MEMF_PUBLIC`, with no guest allocation left. No trustworthy failing entry requirements exist yet. |
| AllocEntry: a nonempty member cannot be allocated, including size overflow or native record exhaustion | That member's original requirements OR bit 31, after freeing earlier members and the new list. Scratch memory is also freed. Freed payload bytes need not retain their contents. |
| FreeEntry: NULL, foreign, interior, repeated, forged or altered group, or corrupt affected free mirrors | No-op, preserving D0 and releasing nothing. Native ownership and the complete preflight prevent partial or unrelated frees. Restoring valid metadata permits a later retry. |

The arena stays below bit 31, so successful addresses cannot be confused
with the documented failure result. These paths use C status values only.

### Limits

| Resource | Limit | Constant |
|----------|-------|----------|
| Containment memory | 32 MB | `KAFFEINE_MEM_SIZE` |
| Guard after it | 32 KB of zero bytes | `KAFFEINE_MEM_GUARD` |
| Stack | 64 KB | `KAFFEINE_STACK_SIZE` |
| Libraries | 32 | `KAFFEINE_MAX_LIBS` |
| Library name map | 16 | `KAFFEINE_MAX_LIBMAP` |
| LVOs per library | 512 | `KAFFEINE_MAX_LVO` |
| Shadow entries | 1024 | `KAFFEINE_MAX_SHADOWS` |
| Hunks per binary | 100 | hardcoded in `LoadHunksFromMemory` |

## CPU Integration

### Moira Subclass

`AROSMoira` (in `kaffeine_moira.cpp`) extends `moira::Moira` with:

- **Memory callbacks**: `read8/read16/write8/write16` — intercept custom chip range (`$DFF000`), sentinel address, and out-of-bounds access
- **Exception dispatch**: `willExecute(LINEA)` — the core thunk dispatch mechanism
- **Unsupported FPU/Line-F**: `willExecute(LINEF)` logs a fault and stops the run loop without throwing
- **Halt handler**: `cpuDidHalt()` records a CPU fault and stops execution

The `AROSMoira` constructor selects `Model::M68EC040`, a 040 without an
FPU and without an MMU. Moira lists the 040 models as disassembler only and
executes M68EC040, M68LC040 and M68040 with its C68020 core and a 32-bit
address mask. In this revision `hasFPU()` is true only for M68040. Only that
model installs the FPU handlers, which throw `std::runtime_error`. M68EC040
leaves the entire coprocessor-ID-1 opcode area (0xF200 to 0xF3FF) mapped to
`execLineF()`, which delivers exception vector 11 without a C++ throw.

M68LC040 and M68040 also install the 040 PFLUSH and PTEST handlers, which
throw, so a PFLUSH or PTEST aborted AROS while the adapter selected
M68LC040. M68EC040 does not register those 48 opcodes, so they stay
Line-F too. Apart from them and the FPU, M68EC040 keeps the M68040 decode
table, `hasCPI()` (false) and the CACR mask; outside the disassembler,
Moira uses `hasMMU()` (false for M68EC040) only for the availability shown
in instruction descriptions. CINV/CPUSH and MOVE16 are stubs that deliver
Line-F and terminate with 20 under the adapter's Line-F policy.
`test_guest_bounds` compares all 65536 instruction descriptions with
M68040, runs every PFLUSH and PTEST opcode without the adapter's catch, and
exercises the other Line-F stubs separately.

### FPU

The ExecBase shadow therefore reports `M68K_ATTNFLAGS_68040` (`AFF_68010`
to `AFF_68040`) without `AFF_68881`, `AFF_68882` or `AFF_FPU40`. Programs
built for an FPU with libnix stop in its startup code: `___fpucheck`
requires `AFF_68020` and `AFF_68881`, calls `Alert(0x00068881)` and exits
with return code 20 before `main()`. Programs for the 68000 compute with
doubles through the IEEE math libraries, which the manual thunks provide.
`test_moira_exec` checks that AttnFlags report an FPU only if
`FMOVE.L FPCR,D0` runs.

Running FPU code needs, in order:

1. An FPU in the backend: the 6888x and 68040 data formats, FMOVE,
   FMOVEM and FMOVECR, the arithmetic and transcendental operations, FPCR,
   FPSR and FPIAR, rounding modes and FPU exceptions, and FSAVE/FRESTORE.
   Moira has the decoder for all of it; a hosted Emu68 backend would bring
   its own FPU translation.
2. A fault path that does not require C++ unwinding (implemented for the
   absent FPU): M68EC040 delivers Line-F, and `willExecute()` stops the
   program with `RETURN_FAIL` (20). The C++ catches remain a host safety
   net; AROS library exceptions cannot rely on them today (see below).
3. The FPU flags in `M68K_ATTNFLAGS_68040` (`AFF_68881`, `AFF_68882`, and
   `AFF_FPU40` for the 68040 model) once the target battery runs
   `c_compute_fpu` past `main()`.

### Run Loop

```plain
Kaffeine_Execute(ctx):
  1. Construct AROSMoira on stack, selecting M68EC040 in its constructor
  2. Write initial SP → vector 0, entry_point → vector 4
  3. cpu.reset()
  4. Restore AbsExecBase at address 4
  5. Plant RTE (0x4E73) at 0x000100
  6. Plant supervisor trampoline at 0x000108: ADDQ.L #8,SP (0x508F) + JMP (A5) (0x4ED5)
  7. Set LINEA vector (address 0x28) → 0x000100
  8. Push SENTINEL_ADDR (0x000160) as return address on stack
  9. Set registers: A0=argptr, D0=argsize, A6=m68k_sysbase
  10. Kaffeine_RunCPU() calls moira_execute() until ctx->running is FALSE
  11. Return RETURN_FAIL (20) after a CPU fault, otherwise D0
```

### Termination

- **DOS Exit**: The manual -144 thunk clears `running`, stores returnCode
  and returns it for D0. The next run-loop iteration is skipped and normal
  native entry-point cleanup runs. No native `Exit()` or exception is used.

- **Sentinel**: When m68k code does RTS and fetches the instruction at
  `0x000160`, `read16` returns STOP opcode (`0x4E72`) and sets
  `ctx->running = FALSE`. Any read of that word stops the CPU, so the
  sentinel lies outside the heap, in the user interrupt vectors that no
  exception uses. At `0x00DEAD00` it was inside the heap, where the code of a
  100 KB program lands since the heap serves the memory below the custom
  chips first.
- **HALT**: `cpuDidHalt()` logs the halt and PC and records a fault
- **Line-F**: `willExecute(LINEF)` logs the kind, vector 11 and the faulting
  instruction PC, records a fault and clears `ctx->running`. Moira finishes
  the exception frame and vector fetch, but the run loop executes no further
  instruction. FPU opcodes take this path without a throw.
- **Backend exception (best effort)**: `moira_execute()` catches address errors, bus
  errors, double faults (objects and the pointer thrown by Moira's
  `processException()`), standard exceptions and unknown exceptions. It
  logs the kind and the instruction PC captured before execution, records
  a fault and clears `ctx->running`. Moira slices unrecognized standard
  exceptions when rethrowing, so the saved coprocessor-ID-1 opcode
  identifies an unsupported FPU instruction even when its message is lost.

With a working unwinder, the catches keep backend exceptions from entering
the C `Kaffeine_RunCPU()` frames. The outer C++ entry also catches
construction, model-selection and reset exceptions. They are safety nets,
not a supported AROS fault mechanism. A recorded fault overrides D0:
`Kaffeine_Execute()` returns `RETURN_FAIL` and sets `ctx->exit_code` to 20.
The sentinel still returns the program's D0 unchanged.

`setup_and_execute()` returns this value directly, or reads `ctx->exit_code`
after its synchronous emulator process finishes. `RunHunk()` and
`RunFile()` retain their cleanup paths. DOS `RunCommand()` returns it to
the Shell, which updates its `RC`. Through `Run`, the background CLI sees
that result; `Run` itself reports whether the launch succeeded, not the
later command result. Scripts that must continue after this deliberate
failure need `FailAt 21`, as the target runner's wrappers already use.

Classic AmigaOS routes an unhandled F-line instruction to task trap 11,
normally resulting in a Software Error requester or alert, rather than
successful instruction execution. Returning 20 is this bridge's contained
failure policy, not an emulation of that requester or of `tc_TrapCode`.
It allows native DOS cleanup and the caller to continue. See
[Exec task traps](https://wiki.amigaos.net/wiki/Exec_Tasks#Task_Traps) and
[AmigaDOS condition flags](https://wiki.amigaos.net/wiki/AmigaOS_Manual:_AmigaDOS_Using_Scripts#Condition_Flags).

### AROS module exceptions

The AROS target run reported an abort requester after FMOVE, even with a
catch in the same C++ file. Host catch tests cannot establish that AROS
library unwinding works. FPU handling therefore uses Line-F and does not
throw at all. Initialization and other throwing backend paths, including
the unsupported MMU handlers above, cannot rely on the retained catches.

The source contains an explanation consistent with that abort:

- `rom/kaffeine/mmakefile.src` builds a library module, adds C++20 and
  narrowing flags, links statically with stdc++ and does not request
  `-fno-exceptions` for its C++ files. `config/make.tmpl` links modules with
  `NOSTARTUP_LDFLAGS`, configured as `-nostartfiles`.
- `tools/collect-aros/ldscript.h` retains `.eh_frame` and defines its
  bounds. `rom/dos/internalloadseg_elf.c` loads allocated ELF sections and
  relocates them, but contains no unwind-frame registration call.
- `compiler/autoinit/initexitsets.c` registers `__eh_frame_start` through
  `__register_frame()` in a `PROGRAM_ENTRIES` hook. Its comment explicitly
  identifies the missing registration for shared libraries. It also
  provides weak no-op frame-registration functions for C executables.
- `tools/genmodule/writestart.c` marks `PROGRAM_ENTRIES` as handled but
  ignores it in modules. Generated library initialization calls
  `compiler/autoinit/functions.c`'s `_set_libinit()`, which invokes library,
  init and constructor sets but does not register `.eh_frame`.

Keeping frame bytes in the module is not registration with its unwinder.
The loaded frame registry and the exact abort call stack have not been
inspected, so this is source evidence for the failure, not a runtime proof
of every contributing cause. No loader, startup or compiler flags are
changed by the Line-F fix.

### Bounds audit

The audit excludes the Moira submodule and generated
`kaffeine_thunks_gen.c`. In addition to the accessors and callbacks,
`Kaffeine_LoadHunksFromMemory()` now checks remaining file bytes by
subtraction before pointer movement, checks resident-name and CODE/DATA
longword counts before multiplication, bounds payloads by their allocated
hunk and checks relocation widths by subtraction. Invalid relocation
offsets are still skipped. File contents are reachable through the 68k
library loader as well as `RunFile()`.

The following checks were inspected and retained:

| Location | Reason |
| --- | --- |
| `m68k_to_host()`, `m68k_to_host_or_shadow()` | Compare the starting address directly, without addition. These translate a pointer, not an arbitrary buffer length. |
| `kaffeine_lists.c` and `kaffeine_memheader.c`, `guest_range()` | Check the size before subtracting it from the arena limit. MemHeader sums and overlap checks follow complete range validation. |
| `kaffeine_readargs.c` and `kaffeine_thunks_dos.c`, `guest_range()` and `guest_string()` | Starting-address checks precede remaining-byte subtraction and bounded string search. The ReadArgs item count is at most 256. |
| `walk_system_tags()` and `tag_step()` | Validate eight bytes first; division bounds TAG_SKIP before `(data + 1) * 8`. |
| `kaffeine_thunks_mathieee.c`, `IEEEDPSincos()` | Checks the eight-byte cosine destination with subtraction. |
| `thunk_gfx_OpenFont()` | Checks the full TextAttr with subtraction and bounds the name's terminator. |
| `kaffeine_dos_PathPart()` | Compares a native result with the end of an already bounded, terminated string. |
| `Kaffeine_HeapAlloc()` | Rejects size/header/alignment overflow before additions. |
| `Kaffeine_HeapFree()` | Coalescing compares sums for equality against address-ordered neighbors. A wrapped sum is below its starting address and cannot equal the later neighbor. This is not validation of arbitrary heap metadata. |
| `Kaffeine_LoadHunks()` | DOS-owned native seglist/record; checks `size >= 4` and `offset <= size - 4`. Relocation values intentionally wrap modulo 2^32. |
| `Kaffeine_SetupLibBase()` | Uses host-owned library metadata and a UWORD vector count in the fixed 64 KiB library region. |
| `Kaffeine_AutoOpenLibs()` | Offsets and name scans remain within the context's fixed 32 MiB arena; the count is at most 16. |
| `Kaffeine_LoadHunksFromMemory()`, hunk-size table | Masks the count with 0x3FFFFFFF before multiplying by four; the hunk count is at most 100. Heap allocation rejects sizes outside the arena. |
| `AROSMoira::read8()`, `write8()` and sentinel check | Byte accesses compare the address directly; the sentinel's upper bound is a constant. |

The remaining non-generated sources (`kaffeine_m68klib.c`,
`kaffeine_shadow.c`, `kaffeine_thunks.c`, `kaffeine_cli.c`, dispatch, CPU,
initialization, stubs and compatibility thunks) contain no further
addition-based range check admitting a wrapped guest value. This is a
wrap-check audit, not a general safety audit: existing unbounded native
buffer handoffs, direct guest-structure byte accesses and corrupt heap
metadata need separate validation work.

### Changed guest code

`struct KaffeineCPUOps` has one optional operation, `flush_code(state, addr,
size)`. A backend that keeps what it derived from guest code, such as
translated blocks, forgets what came from the guest bytes [addr, addr +
size), or from all guest memory for address 0 and `KAFFEINE_FLUSH_ALL`
(0xFFFFFFFF), before that code runs again. Moira fetches every instruction
from guest memory and leaves it NULL. `Kaffeine_FlushCode(cpu, addr, size)`
calls it and accepts a NULL handle or operation; it is inline in
`kaffeine_cpu.h`, so that the loaders in `kaffeine_memory.c` need no CPU loop.

`Kaffeine_RunCPU()` keeps its CPU in `ctx->cpu` while it runs and restores the
previous value afterwards, for callers that have the context alone:

| Caller | Range |
| --- | --- |
| `CacheClearU()` (-636) | All guest memory |
| `CacheClearE()` (-642) with `CACRF_ClearI` | A0/D0, or all guest memory for a length of ~0 or a range outside it |
| `Kaffeine_LoadHunksFromMemory()`, `Kaffeine_LoadHunks()`, `Kaffeine_LoadLibraryHunks()` | Each hunk loaded |
| `Kaffeine_SetupLibBase()`, `Kaffeine_SetupDeviceBase()` | The jump table of the new base |
| `Kaffeine_OpenM68KLibrary()` | The jump table that `MakeLibrary()` builds for a library from disk; its hunks through `Kaffeine_LoadLibraryHunks()`, which copies them as `Kaffeine_LoadHunks()` does |

While the bridge sets up a program, outside `Kaffeine_RunCPU()`, there is no
backend to tell. A call from a thunk, the loader of 68k libraries included,
arrives while the backend's `execute()` waits in the dispatcher, possibly
inside a callback, so the backend must not free code that is running then.
Guest writes to code are not detected: as on a 68040 with its caches
enabled, a program that changes code calls `CacheClearU()` or
`CacheClearE()`.

### A-Line Dispatch (willExecute)

1. Restore LINEA vector to `RTE_ADDR` (0x000100)
2. Get PC0 (faulting instruction address)
3. Scan `ctx->libs[]` to find which library's jump table contains PC
4. Compute LVO offset = `lib->m68k_addr - pc`
5. Search manual thunks first (`lib->thunks[]`), then generated (`lib->gen_thunks[]`)
6. If found: pop JSR return address from m68k stack, call thunk, set D0, redirect PC
7. If `ctx->sv_redirect` is set: dispatch Supervisor() trampoline instead
8. Unimplemented LVOs: pop return, set D0=0, log warning

Moira takes the SR for the exception frame before it calls `willExecute`,
and the RTE at `RTE_ADDR` restores that frame. A thunk that sets condition
codes with `Kaffeine_SetSR()`, as the IEEE math thunks do, would lose them on
the way back. After a library call, `didExecute` therefore copies the CCR
of the live SR into the stacked SR, so the caller sees the flags the thunk
left; a thunk that does not touch the CCR returns the caller's own.

### Supervisor() Support

`thunk_exec_Supervisor` stores the user function address in `ctx->sv_redirect`. After the thunk returns, `willExecute` detects this and calls `dispatchSupervisor()`:

1. Pushes a 68020-format exception frame (8 bytes: vector offset + PC + SR) onto SSP
2. Redirects LINEA vector to `SV_TRAMP_ADDR` (0x000108)
3. Sets PC to the user function address
4. User function runs in supervisor mode, typically does `MOVE.W SR,D0; RTE`
5. RTE pops the manually-pushed frame, returns to caller

### Synchronous guest callbacks

`Kaffeine_CallGuest()` in `kaffeine_cpu.c`, declared in
`kaffeine_callback.h`, runs a 68k function synchronously from a native thunk.
`KaffeineGuestCall.d_mask` and `a_mask` select its D/A inputs; A7 is always
owned by the primitive. It saves D0-D7, A0-A7, SR and PC, pushes a return
token on the checked guest stack, sets the entry PC and calls the backend's
`execute()` operation until that token is reached. On success it copies
all D/A results to the call structure, then restores the saved caller state.
The thunk selects its own results (D0 for a hook, A3 for a character writer).
Guest memory changes are retained, including scratch words below the stack.

The return area is reserved within the vector page, after the existing
RTE and Supervisor trampolines. Level zero uses 0x120, level one 0x124,
through level fifteen at 0x15C. Moira reads the area as NOPs for RTS
prefetch; these instructions are never executed. The loop accepts only the
current level's exact token with A7 equal to its value before the push.
A jump there without popping the return word, RTD with excess popping,
an odd target, or a token from a different level stops with rc 20. An
attempt to execute a consumed token outside a callback also stops. A jump
with the exact token and correct A7 is equivalent to a return; this checks
stack balance, not the provenance of the control-flow instruction. A second
RTS within guest code can still consume an ordinary guest return address,
as on a real CPU. It cannot resurrect a completed native callback frame.

Library traps inside a callback use the same dispatcher and can call back
again. Sixteen simultaneous callbacks are allowed; the seventeenth stops
with rc 20 before any stack write. There is no instruction budget or timeout:
a 68k callback loop remains a running program. Stack pointers are checked
on entry and between slices; they may use any aligned area of the arena,
not just the initial stack allocation. An instruction that leaves A7 outside
the arena is rejected before the next instruction. A normal return may
restore a one-past-arena A7 if that was the checked pre-push value.

The backend contract in `kaffeine_cpu.h` now requires reentrant execution,
`set_pc()` that prepares instruction fetching, and a slice boundary at a
return token. Moira saves its suspended LINEA prefetch, PC/PC0, banked
stacks, flags and exception/bus metadata around native dispatch. Callback
entry refills IRD/IRC. This avoids resuming the outer exception with a
nested instruction's decode or stack-bank state. It also reinstalls the
bridge-owned LINEA vector after a successful nested call, before the outer
exception resumes. A future JIT must
materialize registers, leave translated blocks at tokens and check stack
and arena accesses during callback execution.

Illegal instructions, unhandled Line-A, Line-F, other CPU exceptions in a callback, invalid
PCs, stack escape, invalid callback memory accesses and excess depth stop
without throwing. The custom-chip register window retains its existing
emulation. The first invalid bus word stops a split longword access, and
later accesses in that instruction are suppressed. An earlier valid word
may already have been written. The ordinary, non-callback bounds behavior
above is unchanged. `callback_failed` also suppresses the outer LINEA's
pending frame writes after nested dispatch stops.

`Kaffeine_StopGuest()` sets `running = FALSE`, `exit_code` and D0 together.
`stopFault()` supplies 20; a thunk that stops intentionally supplies its
own result (for example 7 in the host fixture or 20 for semaphore failure).
Failure restores no guest registers or stack and supplies no callback
results. Callers return `ctx->exit_code` immediately and perform no further
guest accesses. The outer dispatcher and Moira slice preserve that result;
`Kaffeine_RunCPU()` and the existing process cleanup then finish normally.
The retained host exception catches are not needed by these paths. The
host fixture also repeats faults with those catches bypassed.

The cost is a register snapshot, one guest stack word, a prefetch refill,
and the ordinary execute loop per callback, plus suspended backend metadata
at dispatch. Formatting calls this once per character for arbitrary guest
writers. The recognized two-instruction copy writer avoids interpretation.
No callback throughput or target timing has been measured.

### Native Hooks during a thunk

`Kaffeine_InitNativeHook()` in `kaffeine_hook.c`, declared in
`kaffeine_hook.h`, initializes a stack-owned `KaffeineNativeHook`. It checks
the whole aligned 20-byte guest Hook and captures its checked, nonzero,
even h_Entry, excluding callback return tokens. Invalid inputs return
FALSE without stopping or calling the guest. h_SubEntry and h_Data stay
in guest memory, for the guest entry stub to interpret.

The native Hook's h_Entry is an `AROS_UFH3S` trampoline with hook in A0,
object in A2 and a pointer-sized message in A1. Its native h_Data points
to the adapter, which holds the context, CPU handle, guest Hook address,
entry, translator and caller-owned translation data. The translator must
map or validate the native object/message and supply their guest values;
the trampoline never truncates a native object pointer into a guest
address. It calls `Kaffeine_CallGuest()` with A0 = guest Hook, A2 = mapped
object, A1 = mapped message, and returns the guest's complete 32-bit D0.
Successful calls retain the primitive's register and SR preservation.
Each adapter has its own state, so nested native calls do not replace the
suspended call's translations or failure status.

A translator rejection stops with rc 20. A fault or intentional stop
keeps the callback's process code. Every failure latches `failed`; this
and the context's stopped/failed state make later native trampoline calls
return zero immediately, without translation, guest access or execution.
The native API may finish its own loop and cleanup, but the thunk must
check the adapter/context before writing results and return `exit_code`
on a stop. A parser's apparent success after seeing that zero is ignored.
Neither exceptions nor native-stack unwinding are involved.

The native call must finish using the Hook before the thunk returns and
must call it on the same task. Retained Hooks, asynchronous calls and
other tasks are not covered; they need lifetime management, scheduling
and serialization. An ASL filter still needs API-specific checked object
and message shadows, result translation and stop propagation. Passing a
guest Hook directly to native code remains unsafe.

### Hook and formatting thunks

The NDK 3.2 `.fd` files and the AROS Exec/Utility `.conf` files agree:

| Vector | Inputs | D0 result |
| --- | --- | --- |
| utility -102, CallHookPkt | A0 hook, A2 object, A1 packet | Hook's D0 |
| exec -522, RawDoFmt | A0 format, A1 data stream, A2 PutChProc, A3 PutChData | Advanced guest data-stream pointer |
| utility -312, VSNPrintf | A0 buffer, D0 capacity, A1 format, A2 data stream | Required bytes including terminating zero |

The manual hook thunk checks the whole 20-byte classic Hook and the even,
nonzero h_Entry at offset 8. Invalid hooks or entries return zero without
invoking guest or native code. Object and packet are opaque guest values,
passed unchanged to A2 and A1. Accesses made by the hook are checked by the
CPU adapter. h_SubEntry is interpreted only by the guest's own entry stub,
as described in NDK `utility/hooks.h` and AROS `rom/utility/callhookpkt.c`.

`kaffeine_format.c` implements the executable rules in
`rom/exec/rawdofmt.c` with guest widths and endianness. Numeric arguments
occupy a WORD, or a LONG with `l` (`i` also means a guest 32-bit integer).
`%s` consumes a guest C pointer and `%b` a classic shifted BPTR with a byte
length prefix. NULL strings are empty. `%c`, `%d/%D`, `%u/%U`, `%x/%X`,
`%%`, unknown conversion characters and trailing `%` follow that source.
Both hexadecimal cases produce uppercase digits. `%p/%P` consume a guest
pointer and pad to eight hexadecimal digits. The supported flags are `-`
and `0`, in that order; width pads and precision truncates the body,
including numeric bodies, rather than implementing ISO printf precision.
There are no host varargs, host pointer conversions or fixed output buffer.
The source parser supports only `l` and `i`, despite its autodoc also
mentioning `ll` and `L`; this bridge follows the parser.

Every format and C string must terminate within the arena, even if precision
would print fewer bytes. BSTRs must fit their entire declared length.
Consumed data-stream words must be aligned and fit in full. Width parsing,
the argument cursor and the required count cannot wrap; output sizes above
LONG_MAX (including zero) are rejected. A count-only pass validates input
before emission. Guest callbacks can change inputs, so emission rechecks
each read. Such a mutation can fail after earlier output has occurred.

RawDoFmt emits the terminating zero to every writer. D0 is the character
and A3 the current data value at guest writer entry; the returned A3 is
carried to the next call. The RawDoFmt caller's A3 is preserved. A writer
beginning with exactly 0x16C0 0x4E75 (MOVE.B D0,(A3)+; RTS) copies directly.
Guest values 0, 1 and 2 select STRING, SERIAL (native RawPutChar) and COUNT
(a checked BE32 counter, retaining its initial value), as defined by AROS
`exec/rawfmt.h`. The installed NDK 3.2 includes have no RAWFMTFUNC defines;
its `clib/exec_protos.h` and `exec_lib.sfd` do confirm the pointer result.
Copy destinations are checked for the complete preflight size, and again
on each byte. COUNT checks four bytes on every increment. Invalid RawDoFmt
input, writer or destination stops with rc 20 because its pointer return
does not provide an error channel. Callback stops retain their own code.

VSNPrintf validates the entire advertised non-NULL destination before any
write, requires positive capacity at most LONG_MAX, writes at most that
capacity and always terminates it. NULL destinations are count-only,
including capacity zero. Invalid input, invalid non-NULL buffers and count
overflow return -1 without stopping the process. The preflight pass leaves
the destination unchanged on invalid input. Overlapping destination/input
ranges can modify later input during emission and are not transactional.
The required size includes zero even after truncation, matching AROS
`rom/utility/vsnprintf.c`. Copying stops at the first embedded zero, matching
that implementation's final Strlcpy, while counting continues. Existing
DOS format argument converters in `kaffeine_thunks.c` are unchanged.

## Thunk System Details

### Calling Convention

Thunks receive `(struct KaffeineContext *ctx, void *cpu)` and access m68k
registers via macros:

| Macro | Purpose |
|-------|---------|
| `THUNK_D(n)` | Read data register Dn as ULONG |
| `THUNK_A(n)` | Read address register An as ULONG (raw m68k address) |
| `THUNK_PTR(n)` | Read An and translate via `m68k_to_host_or_shadow` |
| `THUNK_DPTR(n)` | Read Dn and translate via `m68k_to_host_or_shadow` |
| `SET_D(n, val)` | Write data register Dn |
| `SET_A(n, val)` | Write address register An |

Return value is placed in D0 by the dispatch code. Condition codes set with
`Kaffeine_SetSR()` reach the caller through `didExecute` (see A-Line Dispatch).

### Manual Thunk Priority

When both manual and generated thunks exist for the same LVO, the manual thunk
wins. The dispatch code in `willExecute` searches `lib->thunks[]` (manual)
before `lib->gen_thunks[]` (generated).

### Complete Manual Thunk List

**exec.library:**

| LVO | Function | Handling |
|-----|----------|----------|
| -30 | Supervisor | Stores user func addr, triggers 68020 exception frame trampoline |
| -96 | FindResident | Checks the guest name and returns NULL; native residents contain native pointers/code |
| -132 | Forbid | No-op (single-threaded) |
| -138 | Permit | No-op (single-threaded) |
| -186 | Allocate | Guest MemHeader allocation (`kaffeine_memheader.c`) |
| -192 | Deallocate | Guest MemHeader deallocation (`kaffeine_memheader.c`) |
| -198 | AllocMem | Guest heap, honors MEMF_CLEAR on new and reused blocks (`kaffeine_thunks_memory.c`) |
| -204 | AllocAbs | Claims a wholly free rounded guest heap range, returning the aligned start |
| -210 | FreeMem | Containment heap free |
| -216 | AvailMem | Free payload total, largest free payload or total heap capacity |
| -222 | AllocEntry | Allocates a guest MemList and owned entries; failure requirements have bit 31 set |
| -228 | FreeEntry | Validates and frees the owned guest MemList group |
| -234 | Insert | Guest list links (`kaffeine_lists.c`) |
| -240 | AddHead | Guest list links |
| -246 | AddTail | Guest list links |
| -252 | Remove | Guest list links |
| -258 | RemHead | Returns the guest node |
| -264 | RemTail | Returns the guest node |
| -270 | Enqueue | Guest signed-priority insertion |
| -276 | FindName | Guest name lookup; native lookup only for registered list shadows |
| -294 | FindTask | NULL returns the guest ExecBase ThisTask field; named native tasks reuse a registered Task shadow (`kaffeine_shadow.c`) |
| -306 | SetSignal | Pass-through to native |
| -324 | Signal | The current guest task signals the native task that runs the guest; any other task stops the guest with rc 20 (`kaffeine_exec_io.c`) |
| -330 | AllocSignal | Native bit of the task that runs the guest, from the signed low byte of D0, recorded as the guest's |
| -336 | FreeSignal | Frees only a bit the guest allocated; the App objects of a port with that bit go first, and its AppMessages with them (`kaffeine_workbench.c`) |
| -354 | AddPort | Initializes and publishes the guest port in the context's ExecBase PortList |
| -360 | RemPort | Checks membership and removes the guest port |
| -366 | PutMsg | Retains the exact-shadow native path; ordinary guest queues use checked big-endian FIFO links, and recognized guest file-handler identities accept bounded ACTION_READ packets. NULL, mixed or unsupported protocols stop with rc 20 (`kaffeine_exec_compat.c`, `kaffeine_messages.c`, `kaffeine_filehandle.c`) |
| -372 | GetMsg | IDCMP port of a window: native GetMsg, a 68k IntuiMessage with every field and the 68k Window brought up to date; a 68k port: its first node (`kaffeine_window.c`), where the next AppMessage of a port of App objects comes as a 68k copy when the list is empty |
| -378 | ReplyMsg | Keeps the IntuiMessage and AppMessage ownership paths. Ordinary guest replies queue type 7, or set type 6 for a NULL reply port; malformed or unsupported guest objects stop with rc 20. Other native-shadow protocols retain their ownership rules |
| -384 | WaitPort | IDCMP port of a window: native WaitPort, returning a 68k message for the first message that becomes the one GetMsg gives for it; a port of App objects with an empty list: native WaitPort, then the 68k copy of the AppMessage at the tail of the list; another 68k port: its first node, or rc 20 when empty, as no message can reach it yet |
| -390 | FindPort | Searches only the checked guest public port list |
| -408 | OldOpenLibrary | As OpenLibrary, with version 0 |
| -414 | CloseLibrary | Calls the `Close()` of a 68k library from disk, also for a base that its `Open()` made for the opener (one whose `Close()` lies in the hunks of the library); the library goes when `Close()` gives its seglist back after an expunge; nothing for the base of a native library (`kaffeine_m68klib.c`) |
| -444 | OpenDevice | timer.device on a native timerequest, and console.device unit -1 on a native IOStdReq, each with a private reply port. Console accepts the 32-byte guest IORequest prefix, including zero mn_Length; its guest unit is -1. Adapter selection uses trusted native records |
| -450 | CloseDevice | Closes and deletes native request and port. Console clears guest io_Device and io_Unit to zero; timer keeps its -1 closed-device marker |
| -456 | DoIO | timer.device TR_ADDREQUEST and TR_GETSYSTIME with both time fields copied each way. Console commands, TR_SETSYSTIME and EClock delays return IOERR_NOCMD without native I/O; guest device/unit edits cannot select another adapter |
| -462 | SendIO | Stops the guest with rc 20 until asynchronous I/O exists |
| -468 | CheckIO | As Exec on the guest fields: NULL only for a request in flight |
| -474 | WaitIO | io_Error of a complete request; a request in flight stops the guest with rc 20 |
| -480 | AbortIO | -1: nothing is in flight |
| -486 | AddResource | No-op; guest resource publication is unsupported |
| -492 | RemResource | No-op, paired with AddResource |
| -498 | OpenResource | Checks the guest name and returns NULL; native resource ABIs are not bridged |
| -522 | RawDoFmt | Checked guest formatter and 68k PutChProc callbacks (`kaffeine_format.c`) |
| -534 | TypeOfMem | Returns MEMF_PUBLIC always |
| -540 | Procure | Stops the guest with rc 20; asynchronous guest semaphore messages are unsupported |
| -546 | Vacate | Stops the guest with rc 20, paired with refused Procure |
| -552 | OpenLibrary | A name of at most 255 bytes in guest memory, copied before any 68k code runs, and the low word of D0 as the version, as AmigaOS 3.2.3 takes it. A native library: a fake base and its thunks, whose version is not checked. Else a 68k library from disk, made as Exec makes it (`kaffeine_m68klib.c`): `LoadSeg()` of the file, its hunks copied into guest memory as a seglist, the resident tag in them, for `RTF_AUTOINIT` the library of `MakeLibrary()` (a jump table from LONG pointers or WORD offsets, the `InitStruct()` table, then the node type, name, version and IdString of the resident tag as `InitResident()` sets them, and the init function with D0 base, A0 seglist and A6 SysBase), then `lib_Version` checked and the `Open()` of the library called with the version; a version newer than that of an open library calls its `Expunge()`, as AmigaOS 3.2.3 does, which sets `LIBF_DELEXP` for the last close, and leaves a closed one as it is. Each library keeps its slot of the 32 while its init, `Open()` and `Close()` open and close others, and its own init does not find it yet. The trace names every library with that version and the answer (`native`, `68k at` its base, or `not found`), so that the trace of an application lists the libraries that it needs |
| -558 | InitSemaphore | Initializes the 46-byte guest SignalSemaphore, preserving node name/priority and reserved fields |
| -564 | ObtainSemaphore | Guest exclusive acquisition/nesting; a call that would wait stops with rc 20 |
| -570 | ReleaseSemaphore | Decrements guest nesting/queue counts and clears owner on release to zero |
| -576 | AttemptSemaphore | Guest exclusive attempt, returns AROS TRUE (1) or FALSE (0) |
| -582 | ObtainSemaphoreList | Validates a guest list and obtains every member atomically, up to 256 members |
| -588 | ReleaseSemaphoreList | Validates every member before releasing any |
| -594 | FindSemaphore | Searches only the context's guest public list, returns the original guest address |
| -600 | AddSemaphore | Initializes and publishes a guest semaphore in signed-priority order |
| -606 | RemSemaphore | Removes a semaphore from the guest public list |
| -624 | CopyMem | Copies guest bytes where they lie (`kaffeine_thunks_memory.c`); copies nothing when either range leaves guest memory |
| -630 | CopyMemQuick | Same as CopyMem |
| -636 | CacheClearU | `flush_code()` of the backend for all guest memory (`kaffeine_exec_compat.c`); preserves D0 and every register; no native cache operation |
| -642 | CacheClearE | With `CACRF_ClearI`, `flush_code()` of the backend for the A0/D0 range, or for all guest memory for a length of ~0 or a range outside it, which is logged; preserves registers; no native cache operation |
| -654 | CreateIORequest | Cleared guest request of 20 to 65535 bytes with its reply port and length |
| -660 | DeleteIORequest | Frees a request from CreateIORequest(), closing it first if still open |
| -666 | CreateMsgPort | Cleared signalling 68k port of the current task with a guest signal; no native port |
| -672 | DeleteMsgPort | Frees the port and the signal that CreateMsgPort() gave, after the App objects on it go and its AppMessages with them |
| -678 | ObtainSemaphoreShared | Guest shared acquisition/nesting; a call that would wait stops with rc 20 |
| -684 | AllocVec | Guest heap with requirements, as AllocMem |
| -690 | FreeVec | Containment heap free |
| -696 | CreatePool | Guest object storing requirements and sizing hints |
| -702 | DeletePool | Releases all owned blocks and the pool object (`kaffeine_exec_compat.c`) |
| -708 | AllocPooled | Guest heap allocation with the pool's requirements and owner |
| -714 | FreePooled | Releases an exact live block owned by the specified pool |
| -720 | AttemptSemaphoreShared | Guest shared attempt, returns 1 or 0 (`kaffeine_exec_compat.c`) |
| -828 | NewMinList | Initializes the guest list sentinels |
| -852 | AVL_AddNode | No-op stub (`kaffeine_exec_compat.c`) |
| -858 | AVL_RemNodeByAddress | No-op stub (`kaffeine_exec_compat.c`) |
| -870 | AVL_FindNode | No-op stub (`kaffeine_exec_compat.c`) |

Native OpenLibrary lookup and its cache use the filename following the
last `:` or `/`, preserving its exact case. All aliases of a registered
library share the canonical name and one fake base. The original bounded
guest path stays intact for tracing and for the 68k disk loader when the
native lookup misses. OldOpenLibrary follows the same routing with version
zero. This matches the basename identity in `rom/lddemon/lddemon.c` and the
classic-system observations recorded by `exec_library_paths`.

**dos.library (97 manual thunks):**

| LVO | Function | Handling |
|-----|----------|----------|
| -30 | Open | Bounded guest name; native handle with the existing opaque BPTR convention (`kaffeine_thunks_dos.c`) |
| -36 | Close | BPTR pass-through |
| -42 | Read | Checks the whole D3-byte guest buffer and refuses a negative length; a zero length still goes to DOS, with the start of the arena for an unusable pointer; native DOS reports a NULL handle |
| -48 | Write | As Read, for the source bytes |
| -54 | Input | Pass-through |
| -60 | Output | Pass-through |
| -66 | Seek | BPTR pass-through |
| -72 | DeleteFile | Bounded guest name |
| -78 | Rename | Two bounded guest names |
| -84 | Lock | Bounded guest name; native lock with the existing opaque BPTR convention |
| -90 | UnLock | BPTR pass-through |
| -102 | Examine | Checks the whole guest FIB, examines into a native FIB and converts its public fields (`kaffeine_thunks_dos.c`) |
| -108 | ExNext | As Examine, starting from the key and type in the guest FIB |
| -114 | Info | Checks the 36-byte guest InfoData; native temporary, BE32 fields, id_VolumeNode is zero until a DosList bridge exists |
| -120 | CreateDir | Bounded guest name; returns the native lock using the existing opaque BPTR convention |
| -126 | CurrentDir | BPTR pass-through |
| -132 | IoErr | Pass-through |
| -138 | CreateProc | Fails with `ERROR_NOT_IMPLEMENTED`: the new process would run 68k code natively |
| -144 | Exit | Stops via running = FALSE and returns signed D1 through D0; never calls native Exit |
| -150 | LoadSeg | Bounded guest name; native seglist |
| -156 | UnLoadSeg | BPTR pass-through, except the guest seglist of a 68k library from disk, which stays until its expunge |
| -180 | SetComment | Two bounded, NUL-terminated guest strings; native result and `IoErr()` |
| -186 | SetProtection | Bounded guest name and unchanged scalar protection mask |
| -192 | DateStamp | Native `DateStamp`, three big-endian longwords, returns the guest pointer |
| -198 | Delay | Pass-through |
| -222 | Execute | Bounded command string; the input and output handles pass through |
| -228 | AllocDosObject | TagList translation; FIB returns shadow; RDArgs is a 68k structure in guest memory |
| -234 | FreeDosObject | Object from D2 (shadow lookup); a 68k RDArgs goes back to the guest heap |
| -240 | DoPkt | Fails with `ERROR_NOT_IMPLEMENTED`: no packet bridge yet (`kaffeine_thunks_dos.c`) |
| -246 | SendPkt | Fails with `ERROR_NOT_IMPLEMENTED`: no packet bridge yet (`kaffeine_thunks_dos.c`) |
| -252 | WaitPkt | Fails with `ERROR_NOT_IMPLEMENTED`: no packet bridge yet (`kaffeine_thunks_dos.c`) |
| -258 | ReplyPkt | Fails with `ERROR_NOT_IMPLEMENTED`: no packet bridge yet (`kaffeine_thunks_dos.c`) |
| -264 | AbortPkt | Fails with `ERROR_NOT_IMPLEMENTED`: no packet bridge yet (`kaffeine_thunks_dos.c`) |
| -324 | FRead | Checks unsigned block-count multiplication and the entire guest buffer before buffered I/O; native block count or -1 |
| -330 | FWrite | As FRead for the source bytes; zero block length or count is a no-op |
| -336 | FGets | Native `FGets` into the guest buffer after checking the bytes it can write (two for `buflen` 1); `buflen` 0 or a range outside guest memory fails with `ERROR_BAD_NUMBER` before reading; returns the guest buffer (`kaffeine_thunks_dos.c`) |
| -342 | FPuts | Bounded guest string; the handle passes through |
| -348 | VFWritef | Bounded format string, then `m68k_to_native_fmt_stream` |
| -354 | VFPrintf | The checked RawDoFmt formatter (`kaffeine_format.c`) checks the whole template and data stream, then hands each character to `FPutC()` as in AROS, without a native buffer: zero bytes are skipped, the result is the count or -1 from the first failed write |
| -366 | SetVBuf | Validates mode, signed size and any used guest buffer; passes NULL so DOS owns the persistent buffer |
| -390 | ExamineFH | Checks the whole guest FIB, examines into a native FIB and converts its public fields |
| -396 | SetFileDate | Bounded name and converted 12-byte guest DateStamp |
| -402 | NameFromLock | Checks positive capacity and the whole guest output; zero lock keeps native boot-volume semantics |
| -408 | NameFromFH | Checks positive capacity, nonzero handle and the whole guest output |
| -414 | SplitName | Bounded name, oldpos within its string, positive output capacity and no source/output overlap |
| -432 | ExAll | Fails with `ERROR_NOT_IMPLEMENTED`: `ExAllData` and `ExAllControl` are not converted yet (`kaffeine_thunks_dos.c`) |
| -462 | SetIoErr | Pass-through |
| -468 | Fault | Nullable bounded header, nonoverlapping output, positive capacity and native write footprint (including unconditional header punctuation); returns the native spare-capacity value |
| -474 | PrintFault | Nullable bounded header; native result and error code in `IoErr()` |
| -492 | Cli | The context's guest CLI snapshot, also in `pr_CLI` of the guest Process (`kaffeine_cli.c`) |
| -498 | CreateNewProc | Fails with `ERROR_NOT_IMPLEMENTED`: the new process would run 68k code natively |
| -504 | RunCommand | A non-NULL argument string needs its NUL and all D4 bytes in guest memory; NULL with size 0 passes NULL; the guest seglist of a 68k library from disk gives -1 with `ERROR_OBJECT_WRONG_TYPE` |
| -534 | GetArgStr | Validated guest ThisTask.pr_Arguments address, never native process arguments |
| -540 | SetArgStr | Changes guest ThisTask.pr_Arguments, returns its previous guest address; NULL is legal |
| -558 | SetCurrentDirName | Changes the directory-name BSTR in the shared guest CLI only |
| -564 | GetCurrentDirName | Reads the guest CLI directory-name BSTR into a checked buffer; does not query the native directory lock |
| -570 | SetProgramName | Changes the guest CLI command-name BSTR and the cached startup-name fallback |
| -576 | GetProgramName | Reads the guest CLI command BSTR when present, otherwise the startup name; retains its existing truncation policy (`kaffeine_readargs.c`) |
| -582 | SetPrompt | Changes the prompt BSTR in the shared guest CLI only |
| -588 | GetPrompt | Reads the guest CLI prompt BSTR into a checked buffer |
| -606 | SystemTagList | Command string validation and tag-aware `TagList` conversion (`kaffeine_thunks_dos.c`) |
| -612 | AssignLock | Bounded name and opaque lock; native ownership transfer on success, zero removes |
| -618 | AssignLate | Two bounded strings; padded temporary path for the native strlen(path)+2 read, native DOS copies it |
| -624 | AssignPath | As AssignLate, preserving native non-binding assign semantics |
| -630 | AssignAdd | Bounded name and nonzero opaque lock; native ownership transfer on success |
| -636 | RemAssignList | Bounded name and opaque comparison lock; native DOS releases the matching assign lock |
| -708 | IsFileSystem | Bounded name; native result and IoErr |
| -732 | AddBuffers | Bounded name and signed scalar buffer count; native result and IoErr |
| -738 | CompareDates | Two converted DateStamps; preserves native reverse ordering without overflowing signed subtraction |
| -744 | DateToStr | Converts the 26-byte guest DateTime and its three nullable LEN_DATSTRING outputs |
| -750 | StrToDate | Bounded guest date/time strings, safe month/calendar arithmetic, native DateStamp copied back even on partial failure |
| -768 | NewLoadSeg | Bounded guest name; the tags still go through the generic TagList conversion |
| -798 | ReadArgs | Native `ReadArgs()` on a host array and a native RDArgs filled from the program's one; strings, numbers and arrays copied into one guest block (`kaffeine_readargs.c`) |
| -804 | FindArg | D1 is the template, D2 is the keyword; bounded strings and native parsing, despite misleading NDK argument names |
| -810 | ReadItem | Native CSource with checked counted source, native cursor copied back as BE32; NULL source means Input |
| -816 | StrToLong | Native `StrToLong` into a native LONG, written big-endian (zero when there are no digits) |
| -822 | MatchFirst | Bounded pattern and the whole guest AnchorPath with its `ap_Strlen` buffer; a native AnchorPath for each guest one, with guest copies of its first and current AChain in `ap_Base` and `ap_Last` (`kaffeine_thunks_dos.c`) |
| -828 | MatchNext | The native AnchorPath of an open match; flags, break bits and buffer go in, the results come back |
| -834 | MatchEnd | Ends and frees the native match and the copies; clears `ap_Base` and `ap_Last` as AROS does |
| -840 | ParsePattern | Bounded source and output, at least `2 * strlen(source) + 2` bytes for the native token stack, safe source syntax |
| -846 | MatchPattern | Bounded strings and validated token nesting/classes before the native matcher |
| -858 | FreeArgs | Frees the native `RDArgs` and the guest block of the handle |
| -870 | FilePart | Native `FilePart` on the guest string, returned as an address inside it |
| -876 | PathPart | Native `PathPart` on the guest string, returned as an address inside it (`kaffeine_thunks_dos.c`) |
| -882 | AddPart | Bounded, nonoverlapping strings and output capacity; native no-fit failure leaves dirname unchanged |
| -900 | SetVar | Bounded name, byte range for nonnegative size or terminated value for -1; type/scope flags unchanged |
| -906 | GetVar | Positive bounded output that does not overlap the name, plus the native text lookahead byte; native truncation result and full length in `IoErr()` |
| -912 | DeleteVar | Bounded name and unchanged type/scope flags |
| -930 | CliInitNewcli | Fails with `ERROR_NOT_IMPLEMENTED`: AROS reads a native DosPacket, even for NULL, and replaces the CLI of the process that runs the emulator |
| -936 | CliInitRun | As CliInitNewcli |
| -942 | WriteChars | Checked unsigned byte range; zero length is a no-op and native EOF is preserved |
| -948 | PutStr | Bounded guest string |
| -954 | VPrintf | As VFPrintf, to `Output()` |
| -966 | ParsePatternNoCase | As ParsePattern, with native case folding |
| -972 | MatchPatternNoCase | As MatchPattern, with native case folding |
| -990 | ExAllEnd | No-op paired with refused `ExAll` (`kaffeine_thunks_dos.c`) |
| -996 | SetOwner | Bounded name and unchanged scalar UID/GID packed in D2 |

The sixteen byte-buffer/string overrides from FRead through the pattern
functions live in `kaffeine_thunks_dos.c`. Their ranges use starting-address
checks and remaining-byte subtraction; block I/O checks multiplication
before forming a byte count. Invalid addresses, unterminated strings,
negative lengths and unsafe overlaps fail before native DOS, with
`ERROR_BAD_NUMBER`. The buffer policy and native quirks are documented in
the [host tests](../../developer/debug/test/kaffeine_host/README.md#dos-guest-buffers-and-strings).

The second DOS batch adds 27 manual vectors, from `Info()` through
`SetOwner()`. Its [host policy and regression table](../../developer/debug/test/kaffeine_host/README.md#dos-second-string-and-structure-batch)
describes every refusal and the native edge cases retained by the bridge.

Fifteen base calls, from `Open()` to `VPrintf()`, used to pass unchecked
guest pointers from `kaffeine_thunks.c`. They live in
`kaffeine_thunks_dos.c` now, with the same policy: invalid addresses,
unterminated strings, negative lengths and buffers that leave guest memory
fail before native DOS, with `ERROR_BAD_NUMBER`. `VFPrintf()` and
`VPrintf()` render their text with the checked RawDoFmt formatter instead
of the generic format-stream converter. Handles, locks and seglists keep
the existing opaque BPTR convention. The
[host policy](../../developer/debug/test/kaffeine_host/README.md#dos-base-calls)
lists every refusal.

`MatchFirst()`, `MatchNext()` and `MatchEnd()` keep a native AnchorPath
for each guest one, at most 16 per program, and copy the public fields
both ways around every call. `ap_Base` and `ap_Last` point to
bridge-owned guest copies of the first and the current AChain, whose
`an_Lock` works with `CurrentDir()` and `NameFromLock()`. The context
cleanup ends the matches a program left open. The
[host policy](../../developer/debug/test/kaffeine_host/README.md#dos-matchfirst-and-anchorpath)
has the details and the refusals.
`InfoData` is 36 bytes, `DateStamp` 12, `DateTime` 26 (string APTRs at
14/18/22) and `CSource` 12. `dos_strings.c` checks all those sizes and field
offsets against the actual 68k headers at compile time. Native structures
are temporaries; guest addresses and pointers are always 32-bit BE values.

The guest CLI now owns three 260-byte, BPTR-aligned BSTR slots after its
64-byte structure. The command starts with `program_name`; the prompt and
directory name start empty. Setters use these owned slots even if a guest
has repointed a BSTR field; getters honor validated guest BSTR fields,
including direct guest writes. `SetProgramName()` and `GetProgramName()`
therefore agree with `cli_CommandName`. None of these APIs changes native
Shell strings or the native current-directory lock. A context prepared by
the Shell keeps its CLI when executed in the emulator child without one.
`GetCurrentDirName()` without a guest CLI fails with an empty result instead
of importing the native `NameFromLock()` fallback.

`GetArgStr()` and `SetArgStr()` follow `ExecBase.ThisTask.pr_Arguments` in
guest memory, including direct guest changes. NULL is legal. The immutable
`m68k_argptr`/`m68k_argsize` startup registers and the native process are not
changed. A replaced string must stay live until the guest restores it.

`FindArg()` uses template D1 and keyword D2. The installed NDK 3.2
`dos_lib.fd`, `clib/dos_protos.h` and `inline/dos.h` label the first argument
`keyword` and the second `arg_template`. The inline actually sends the
first C argument to D1 and the second to D2; parameter names do not change
that mapping. `rom/dos/findarg.c` searches its first argument for its
second, and `rom/dos/readargs.c` calls `FindArg(template, s1)`. The bridge
keeps that positional ABI; it neither swaps arguments nor guesses which
string looks like a template. No NDK autodoc is installed in the supplied
NDK directories.

`SetVBuf()` always passes NULL to native DOS after validation. A native
file handle can outlive the guest arena or retain its buffer until Close;
a guest buffer would become a dangling native pointer after context
release. DOS owns and frees its replacement. This does not add a handle
registry, and the guest is still responsible for closing handles.

`Exit()` clears `running`, stores the signed D1 in `exit_code` and returns
it for dispatcher D0, just like the semaphore stop path. It works through
the stop flag without C++ exceptions or native `Exit()`. The real run loop
returns D0. `setup_and_execute()` returns that code directly, or obtains it
from the synchronous emulator child. `RunHunk()`/`RunFile()` then run
`Kaffeine_ReadArgsCleanup()` and `Kaffeine_DestroyContext()`: native RDArgs
results, native heap records, the guest arena (including CLI/arguments,
fake bases and guest pool allocations), and the context are released.
There is no new automatic close/unlock/remove pass for native handles,
locks or assigns, and no guest C-runtime unwinding. The Shell receives the
code through `RunCommand()`; a command under `Run` reports it inside its
background CLI, while the launching `Run` reports launch success. Future
guest callbacks must preserve this stop state and exit code when returning
to their caller. Callback integration and target execution remain separate
validation work.

**intuition.library (38 manual thunks):**

| LVO | Function | Handling |
|-----|----------|----------|
| -66 | CloseScreen | Shadow-resolves screen; when native Intuition closes it, forgets the embedded ViewPort and 68k RastPort and frees the 68k Screen |
| -72 | CloseWindow | Takes the window's menu strip off it, takes the gadgets of the program off it, GadTools ones and those that it built itself, whose copies go, gives Intuition and GadTools back the messages of the window that the program has not replied, frees its 68k Window, RastPort, IDCMP port and messages, then closes the native window (`kaffeine_window.c`) |
| -198 | OpenScreen | Reads m68k NewScreen fields, creates a cleared Screen shadow with a ViewPort sub-shadow and its RastPort as a 68k RastPort (`kaffeine_rastport.c`) |
| -204 | OpenWindow | Reads m68k NewWindow fields after checking that the NewWindow lies in guest memory (else rc 20) and passes them as tags; Type picks the screen as Intuition does: WBENCHSCREEN the Workbench without reading Screen, PUBLICSCREEN `WA_PubScreen` with the native screen in Screen or NULL for the default public screen, any other type `WA_CustomScreen` with the native screen in Screen, or the Workbench for NULL, as before. A Screen that is not a screen of the program (one that it opened, or one from `LockPubScreen()`) stops it with rc 20 before Intuition. Creates a cleared 68k Window with size, limits, flags, borders, IDCMP flags, pens and title, its RastPort as a 68k RastPort, its IDCMP port as a 68k MsgPort, and `WScreen` for a screen of the program: the 68k screen that it named when the window lies there, else its 68k screen of that native screen (one that it opened first), else 0; without guest memory for any of them it closes the native window again and returns 0. A FirstGadget list goes on the window as for AddGList with -1: a GadTools list of the program as the native list, a list of Boolean gadgets that the program built itself as their native copies; a list of the program's own with other gadgets stays out of the window, as before (`kaffeine_window.c`) |
| -348 | AutoRequest | Translates IntuiText chains (up to 8 deep), calls native |
| -432 | RefreshGList | Native for gadgets of the program on that window, GadTools ones or its own, with the native gadget and numGad; what the program changed of `GFLG_SELECTED` and `GFLG_DISABLED` in its own gadgets there, and of BufferPos, DispPos and LongInt of its own string gadgets that Intuition does not edit, reaches their copies first, and what rendering set goes back after; nothing for other gadgets (`kaffeine_gadgets.c`) |
| -222 | RefreshGadgets | As RefreshGList with -1 (`kaffeine_gadgets.c`) |
| -42 | AddGadget | 0 without a window or a gadget, as Intuition; a GadTools batch of one gadget on a window of the program, natively, and the UWORD position; a larger batch, a requester or another window stops the program with rc 20; a Boolean, string or integer gadget that the program built itself goes on as its native copy, with the UWORD position, and one of another kind gives `(UWORD)~0` without Intuition (`kaffeine_gadgets.c`, `kaffeine_classic.c`) |
| -438 | AddGList | `(UWORD)~0` without a window, a gadget or a number of gadgets, as Intuition; a GadTools list of the program, whole batches from a context or the first gadget of a batch, with its 68k links within a batch the native ones and from one batch to the next as the program's 68k list goes, as AmigaOS 3.2.3 takes it, natively on a window of the program, and the UWORD position; anything else of the bridge stops the program with rc 20; a run of Boolean, string and integer gadgets that the program built itself goes on as their native copies, numGad of them or to the end of its 68k list, with the requester ignored, as Intuition ignores it for gadgets without `GTYP_REQGADGET`, and a run with other gadgets of the program's own gives `(UWORD)~0` without Intuition; a run that goes on with GadTools gadgets, holds a gadget twice or one on a window already, lies outside guest memory or would go between the gadgets of one CreateGadgetA() on the window stops the program with rc 20 (`kaffeine_gadgets.c`, `kaffeine_classic.c`) |
| -444 | RemoveGList | `(UWORD)~0` without a window, a gadget or a number of gadgets, and for gadgets not on that window, as Intuition; whole GadTools batches of the program off the window natively, with the 68k NextGadget of the last one left on the gadget that followed it, as AmigaOS 3.2.3 leaves it where the NDK says that V36 cuts it, and its native one NULL, and the UWORD position; likewise a run of the program's own gadgets, which must all be its own, whose copies stay while a message names them, with the NextGadget of the last one left as it is (`kaffeine_gadgets.c`, `kaffeine_classic.c`) |
| -228 | RemoveGadget | As RemoveGList of one gadget (`kaffeine_gadgets.c`) |
| -606 | OpenWindowTagList | TagList translation through `TAG_MORE`, `TAG_SKIP` and `TAG_IGNORE`: WA_Title, WA_ScreenTitle and WA_PubScreenName as strings in guest memory; WA_CustomScreen and WA_PubScreen as the native screen of a screen of the program (one that it opened, or one from `LockPubScreen()`), NULL as NULL (for WA_PubScreen the default public screen), anything else stops the program with rc 20 before Intuition; WA_PubScreenFallBack and the other tags as they are; the last WA_Gadgets as FirstGadget of OpenWindow; a list outside guest memory stops the program with rc 20; the 68k Window as for OpenWindow, its `WScreen` the 68k screen of the last WA_CustomScreen or WA_PubScreen when the window lies there. The NewWindow of A0 is left out, as before (`kaffeine_window.c`) |
| -510 | LockPubScreen | Native LockPubScreen with the name in guest memory, a name that does not end there stops the program with rc 20; one 68k Screen for each native screen, the same at every lock, its fields filled in again: place, size, mouse, flags, bar and border sizes, Font as a 68k TextAttr named as the 68k TextFont of its RastPort, the display fields of the ViewPort, the sizes and depth of the BitMap, the pens; native pointers 0; the embedded ViewPort registered for the native one and the embedded RastPort as a 68k RastPort, whose native font the bridge holds open for the program. A screen that the program opened keeps its 68k Screen. 0, with the native lock given back, without memory or room, also for the 68k TextFont, when the font cannot be held open, and at 32767 locks of one screen (`kaffeine_screens.c`) |
| -516 | UnlockPubScreen | Native UnlockPubScreen only for a lock that the program took: of the public screen of that name, found as Intuition finds it, else of its 68k Screen in A1; nothing for anything else. The 68k Screen goes with the last lock, unless a window of the program is still there or the program holds the list of public screens; a name that does not end in guest memory stops the program with rc 20 (`kaffeine_screens.c`) |
| -522 | LockPubScreenList | Native lock and a 68k copy of the list, the same for nested locks and filled in again at each: names of at most 139 characters, types, priorities, flags, sizes, visitor counts and signal bits, psn_Screen the program's 68k Screen of a screen or 0, psn_SigTask 0; 0 without memory while the lock is held all the same; 32767 nested locks stop the program with rc 20 (`kaffeine_screens.c`) |
| -528 | UnlockPubScreenList | Native unlock only for a lock that the program holds; with the last one the copy goes, and so do the 68k Screens that only it kept (`kaffeine_screens.c`) |
| -534 | NextPubScreen | The name after that screen of the program, or the first one for NULL, from the native list under its lock; 0 after the last screen and after one that is not public, where AROS's NextPubScreen reads through NULL, and for a NULL buffer, as AROS; a screen that is not a screen of the program, and a name that does not fit into guest memory with its NUL, stop the program with rc 20 (`kaffeine_screens.c`) |
| -582 | GetDefaultPubScreen | Native GetDefaultPubScreen into a native buffer, the name copied into guest memory, nothing for NULL; D0 0, since the function is VOID in the NDK; a name that does not fit into guest memory with its NUL stops the program with rc 20 (`kaffeine_screens.c`) |
| -642 | DisposeObject | A GadTools gadget of the program stops it with rc 20, since FreeGadgets frees those; other objects as the generated thunk passes them (`kaffeine_gadgets.c`) |
| -186 | OnGadget | A GadTools gadget of the program: nothing without a window, as AROS; with a window of the program, enabled as AROS enables it, through GT_SetGadgetAttrsA with GA_Disabled (GFLG_DISABLED for a GENERIC gadget and the context), drawn when its batch is on that window, and the mirrors of its batch follow; any other window stops the program with rc 20. A gadget that the program built itself: on that window its copy natively, drawn with the gadgets after it, the state of every gadget of the program's own there passed to the copies first and back after; with another window or none only the flag of its copy; on no window `GFLG_DISABLED` of its own Flags; then the program's gadget follows. A native object behind another shadow as the generated thunk passes it (`kaffeine_gadgets.c`, `kaffeine_classic.c`) |
| -174 | OffGadget | As OnGadget, disabling |
| -468 | NewModifyProp | A window of the program, else the program stops with rc 20, and no requester, as no gadget of the program is on one; without a window nothing, as AROS's Intuition does nothing then (AmigaOS 3.2.3 hangs). A GadTools gadget goes to Intuition as the generated thunk passed it, which AROS changes only while the class of a slider or scroller makes it a proportional gadget of Intuition, as it handles input or draws it; a proportional gadget that the program built itself on a window: the state of the program's own gadgets there passed to the copies first, its copy changed by Intuition with numGad as given and drawn into the window given, `AUTOKNOB` as the copy has it (item 8), and the program's gadgets following, its PropInfo as the copy's with the `AUTOKNOB` given; on no window the five values into its PropInfo; any other gadget is ignored (`kaffeine_gadgets.c`, `kaffeine_classic.c`) |
| -156 | ModifyProp | As NewModifyProp, through Intuition's ModifyProp(), which draws the gadget and those after it |
| -492 | SetEditHook | Stops the program with rc 20: the hook would become the edit hook of every string gadget in the system, which the input handler would call as native code, and the old one would reach the program as a native pointer (`kaffeine_gadgets.c`) |
| -462 | ActivateGadget | A GadTools gadget of the program on the window that its batch is on goes to Intuition, whose BOOL it returns, and its mirrors follow; so does the copy of a gadget that the program built itself on that window, which gets what the program changed first, and whose 68k gadget follows (for a Boolean or proportional gadget Intuition gives TRUE while that window is active and no gadget is in use, enabled or not, and activates nothing, as AmigaOS 3.2.3); any other gadget, another window, none, or a requester gives FALSE without Intuition, which activates whatever gadget it gets (`kaffeine_gadgets.c`) |
| -648 | SetAttrsA | A GadTools gadget of the program stops it with rc 20, since its class would get 68k tags; other objects with the tag list as the generated thunk converts it (`kaffeine_gadgets.c`) |
| -654 | GetAttr | A GadTools gadget of the program stops it with rc 20, since its class would write a native IPTR into the 68k LONG; GT_GetGadgetAttrsA gives its attributes; other objects as the generated thunk passes them (`kaffeine_gadgets.c`) |
| -660 | SetGadgetAttrsA | As SetAttrsA, with the window and the requester as the generated thunk passes them (`kaffeine_gadgets.c`) |
| -810 | DoGadgetMethodA | A GadTools gadget of the program stops it with rc 20, since its class would get a 68k message; other gadgets as the generated thunk passes them (`kaffeine_gadgets.c`) |
| -150 | ModifyIDCMP | Native ModifyIDCMP; the IDCMP port comes and goes as a 68k MsgPort with the flags, and a 68k port that cannot be made undoes the change and gives FALSE; a UserPort that the program set itself stops it with rc 20 (`kaffeine_window.c`) |
| -372 | FreeSysRequest | Takes a menu strip off the requester's window, frees its 68k Window, then the native requester; 0 and 1 pass as Intuition takes them (`kaffeine_requester.c`) |
| -588 | EasyRequestArgs | As AROS: the requester built as BuildEasyRequestArgs, handled until it gives a result, then freed; the 68k IDCMP flags both ways |
| -594 | BuildEasyRequestArgs | Formats the text and the gadgets of the 68k EasyStruct with its 68k arguments through the checked RawDoFmt, cuts the labels as AROS does, and gives Intuition the text and the labels as native `%s` arguments; the requester as a 68k Window, or 0 without memory for the text |
| -600 | SysReqHandler | The native requester of a 68k Window, with the 68k IDCMP flags both ways |
| -54 | ClearMenuStrip | Native ClearMenuStrip; MenuStrip of the 68k Window back to 0; what Intuition set goes to the program's side, and the native copy of a strip that the program built, once no window shows it, waits for a ResetMenuStrip, as one of the eight copies that waited last (`kaffeine_menus.c`) |
| -144 | ItemAddress | The 68k item or subitem of a strip of CreateMenusA, through native ItemAddress on the native strip; in a strip that the program built, its own item or subitem, found where it lies as AROS's ItemAddress finds it, after what Intuition set reached it; 0 for MENUNULL, a missing number or a NULL strip; a strip that leaves guest memory on the way stops the program with rc 20 |
| -180 | OffMenu | Native OffMenu, then the flags back to the 68k mirror of the window's strip, or by bit to the program's own strip |
| -192 | OnMenu | Native OnMenu, then the flags back to the 68k mirror of the window's strip, or by bit to the program's own strip |
| -264 | SetMenuStrip | The native strip of a 68k one from CreateMenusA, after the CHECKED, ITEMENABLED and MENUENABLED flags that the program set; the mirror gets what Intuition set. A strip that the program built gets a new native copy, checked all over, unless a window shows it: then it takes the copy that the windows share and the flags that the program changed; a strip that the bridge refuses stops the program with rc 20, and no memory for a new copy gives FALSE. MenuStrip of the 68k Window the 68k strip, NULL for none; the BOOL in D0 |
| -702 | ResetMenuStrip | As SetMenuStrip, through native ResetMenuStrip, but a strip that the program built takes the copy that windows show or that waits, with the flags that the program changed, without memory; a new copy, for a strip without one, goes through native SetMenuStrip, which works out its page sizes |
| -816 | SetWindowPointerA | No-op (avoids X11 blocking) (`kaffeine_thunks_gfx.c`) |

**graphics.library (22 manual thunks, all but LoadRGB4 in `kaffeine_thunks_gfx.c`):**

Every thunk with a RastPort takes only the 68k RastPort of a window or a
screen (see RastPorts and Fonts below); any other RastPort stops the
program with rc 20 before native graphics.library. A thunk that stops the
program returns 20, so that the code reaches D0, from which the CPU loop
takes the exit code.

| LVO | Function | Handling |
|-----|----------|----------|
| -54 | TextLength | Checks UWORD(count) characters of guest text |
| -60 | Text | Checks the text as TextLength; the pen position comes back |
| -66 | SetFont | Takes a 68k TextFont of graphics.library, or NULL for the default font; any other font, also one closed and gone, stops the program with rc 20 |
| -72 | OpenFont | Converts the checked TextAttr/TTextAttr at A0, calls native OpenFont and returns the shared 68k TextFont with monochrome glyph data, counting the open |
| -78 | CloseFont | Balances the native open; the final native close and guest snapshot release wait while a RastPort or pin retains the font. Foreign and repeated closes acquire no native close |
| -192 | LoadRGB4 | Big-endian byte-swap of UWORD color array |
| -240 | Move | WORD coordinates |
| -246 | Draw | WORD coordinates, from the pen position the program may have written |
| -306 | RectFill | WORD coordinates |
| -318 | ReadPixel | WORD coordinates; the native LONG, -1 outside the RastPort |
| -324 | WritePixel | WORD coordinates; the native LONG |
| -342 | SetAPen | Pen to the native RastPort |
| -348 | SetBPen | Pen to the native RastPort |
| -354 | SetDrMd | Mode to the native RastPort |
| -396 | ScrollRaster | Signed WORD offsets and bounds |
| -690 | TextExtent | Checks the text and the 12-byte 68k TextExtent, writes the native result big-endian |
| -696 | TextFit | Checks every character from string on, a WORD strDirection apart, and both 68k TextExtents; converts the constraint and the result |
| -858 | GetAPen | Native getter after the fields of the 68k RastPort |
| -864 | GetBPen | Native getter after the fields of the 68k RastPort |
| -870 | GetDrMd | Native getter after the fields of the 68k RastPort |
| -876 | GetOutlinePen | Native getter after the fields of the 68k RastPort |
| -894 | SetABPenDrMd | Both pens and the mode to the native RastPort |

**diskfont.library (one manual thunk, `kaffeine_thunks_diskfont.c`):**

LVO -30, OpenDiskFont, reads A0 and returns a guest TextFont or zero.
It shares checked input conversion and font ownership with graphics
OpenFont. An ordinary TextAttr needs 8 bytes; FSF_TAGGED requires the
12-byte TTextAttr. The name must terminate inside the guest arena.
Native attributes are local copies because OpenDiskFont can edit ta_Name.

A NULL tagged list is valid. The supported scalar tag is TA_DeviceDPI,
with up to 64 effective items in a native array. The guest tag walker
handles DONE, IGNORE, SKIP and MORE, including NULL MORE and cycle/range
checks. Unknown tags, invalid ranges and over-limit lists return zero
before native calls or guest writes. Guest attribute and tag bytes remain
unchanged. This conversion is in `kaffeine_fontattr.c`.

Successful native opens enter the same font table as OpenFont. Output
conversion failure, unsupported color fonts and table exhaustion balance
the native open with CloseFont. The returned monochrome snapshot and its
RastPort lifetime are described below. Diagnostics record the request,
selected native font and final guest identity; a native name is read before
adoption can close a rejected font.

**gadtools.library (15 manual thunks, table in `kaffeine_thunks.c`):**

| LVO | Function | Handling |
|-----|----------|----------|
| -72 | GT_GetIMsg | As AROS's, on the IDCMP port of a window: GetMsg and GT_FilterIMsg of each message, replying those of GadTools only; a 68k IntuiMessage for the GadTools copy |
| -78 | GT_ReplyIMsg | As AROS's: GT_PostFilterIMsg of a copy from GT_GetIMsg, then ReplyMsg of the original; the 68k messages of both are freed |
| -102 | GT_FilterIMsg | A 68k IntuiMessage from GetMsg; a 68k message for the GadTools copy, or 0 |
| -108 | GT_PostFilterIMsg | Frees the 68k copy and returns the 68k message of the original |
| -48 | CreateMenusA | Reads the 68k NewMenu array and tags, refuses image items, builds the native strip from native entries whose strings stay in guest memory, and returns a 68k mirror of it; the 68k error ULONG of GTMN_SecondaryError gets the error (`kaffeine_menus.c`) |
| -54 | FreeMenus | Takes the strip off any window of the program that still shows it, then frees the 68k mirror and the native strip; a strip that the program built is no strip of CreateMenusA and is left alone |
| -60 | LayoutMenuItemsA | Native layout of the items of a strip with a 68k VisualInfo handle, GTMN_TextAttr as a native copy and GTMN_Menu as the native menu; the mirror gets the result; the BOOL in D0 |
| -66 | LayoutMenusA | As LayoutMenuItemsA, for a whole strip |
| -120 | DrawBevelBoxA | Native DrawBevelBoxA into the 68k RastPort of a window or a screen, with the first GT_VisualInfo as the native VisualInfo behind its 68k handle and the first GTBB_Recessed and GTBB_FrameType, the only tags GadTools reads; nothing without a VisualInfo, as GadTools; any other VisualInfo or RastPort, or a tag list outside guest memory or in a cycle, stops the program with rc 20 (`kaffeine_menus.c`) |
| -126 | GetVisualInfoA | Native GetVisualInfoA of a screen of the program or of LockPubScreen; a 68k handle for it |
| -132 | FreeVisualInfo | Frees the native VisualInfo and its handle |
| -114 | CreateContext | Native CreateContext; the 68k context of a new gadget list, also in the 68k list pointer (`kaffeine_gadgets.c`) |
| -30 | CreateGadgetA | After a gadget that CreateContext or CreateGadgetA returned: a native NewGadget and the tags that GadTools uses for the kind, converted; the 68k mirror of every gadget that GadTools made, and the last of them in D0; a listview's List as a native copy. Hooks and functions of 68k code, and a `GTLV_ShowSelected` that is no string gadget of GadTools, stop the program with rc 20 |
| -36 | FreeGadgets | From a context or the first gadget of a batch: GadTools frees the GadTools gadgets of the program's 68k list from there, as AmigaOS 3.2.3 does, past gadgets of the program's own and as far as the program cut it, and the bridge their batches, each by itself; the native and 68k NextGadget of each gadget of the bridge that stays and led into them go; a list that leads into the middle of a batch, back to a batch already on it or to a batch on a window, a gadget that GadTools keeps active without the memory to make it forget it, and another gadget of the bridge, stop the program with rc 20, before anything goes; a ring of the program's own gadgets ends the list after 65536 of them, and one outside guest memory ends it; anything else is ignored |
| -42 | GT_SetGadgetAttrsA | Nothing for a NULL gadget or tag list, as GadTools. For a gadget of the bridge, the attributes that the autodoc gives for its kind, converted and checked, in one native call, with the native window when its batch is on that window, where GadTools then draws the change, else without one; then the mirrors of its batch follow. Any other gadget or window, a list outside guest memory or in a cycle, more than 512 attributes, or an attribute that the bridge refuses stop the program with rc 20 before native GadTools sees any (`kaffeine_gadgets.c`) |
| -174 | GT_GetGadgetAttrsA | 0 for a NULL gadget or tag list, as GadTools. For a gadget of the bridge, after the mirrors of its batch are brought up to date, the attributes that the autodoc gives for its kind, each as a 68k LONG, and their number; any other address, a list outside guest memory or in a cycle, more than 512 attributes or a storage outside guest memory stop the program with rc 20 before anything is written (`kaffeine_gadgets.c`) |

**utility.library (23 manual thunks, table in `kaffeine_thunks_utility.c`):**

| LVO | Function | Handling |
|-----|----------|----------|
| -30 | FindTagItem | Walks the guest list in place with `kaffeine_taglist.c` (`TAG_IGNORE`, `TAG_SKIP`, `TAG_MORE`, bounds and cycle checks) and returns the guest item; nothing is converted, copied or freed |
| -36 | GetTagData | Looks up the first matching user item in the guest list with `kaffeine_utility_FindTagItem()` and returns its 32-bit `ti_Data` unchanged, or `defaultVal` in D1 when no item matches |
| -42 | PackBoolTags | D0 flags, A0 list, A1 boolean map; first matching mask, last boolean duplicate wins |
| -48 | NextTagItem | Validates the 4-byte guest pointer variable at A0, steps from `*A0` to the next user item with `kaffeine_taglist.c`, writes the position after it (or 0 at `TAG_DONE`, a NULL `TAG_MORE` or a bad list) back to `*A0`, and returns the item's guest address |
| -54 | FilterTagChanges | A0 changes, A1 original, D0 apply; equal changes become TAG_IGNORE, nonzero apply updates only matching original data |
| -60 | MapTags | A0 list, A1 map, D0 map type; replace tag words, mapping TAG_DONE to TAG_IGNORE |
| -66 | AllocateTagItems | D0 count, including the caller's terminator; returns cleared, owned guest memory |
| -72 | CloneTagItems | A0 source; returns a flat guest copy of the user pairs plus a cleared TAG_DONE |
| -78 | FreeTagItems | A0 allocation; frees only an exact base owned by the utility allocator |
| -84 | RefreshTagItemClones | A0 destination, A1 original; checks ownership and capacity, then restores user pairs only |
| -90 | TagInArray | D0 value, A0 BE32 tag array ending in TAG_DONE; returns TRUE or FALSE |
| -96 | FilterTagItems | A0 list, A1 tag array, D0 logic; writes TAG_IGNORE to excluded tag words, returns the count left |
| -102 | CallHookPkt | Checked classic Hook, synchronous call of guest h_Entry with A0/A2/A1 |
| -120 | Amiga2Date | Checks that the 14-byte 68k `ClockData` at A0 lies in guest memory, calls native `Amiga2Date()` and writes the seven fields as big-endian UWORDs; writes nothing for an invalid address |
| -126 | Date2Amiga | Reads the 68k `ClockData` at A0 into a native one and returns native `Date2Amiga()`; 0 for an invalid address or a month outside 1 to 12, which AROS would use as an index without checking |
| -132 | CheckDate | Reads the 68k `ClockData` at A0 and returns native `CheckDate()`, 0 for an invalid address; like AROS it does not check 29 February outside leap years |
| -150 | SDivMod32 | Quotient in D0, remainder in D1 |
| -156 | UDivMod32 | Quotient in D0, remainder in D1 |
| -162 | Stricmp | A0/A1 complete guest strings; returns the signed difference of the first differing folded bytes |
| -168 | Strnicmp | A0/A1 strings, D0 length; same comparison with a character limit, reading only the compared bytes |
| -198 | SMult64 | High 32 bits of the product in D0, low 32 bits in D1 |
| -204 | UMult64 | High 32 bits of the product in D0, low 32 bits in D1 |
| -312 | VSNPrintf | Bounded guest formatter, required size including zero (`kaffeine_format.c`) |

These registers and LVOs agree with NDK 3.2 `utility_lib.fd` and
`rom/utility/utility.conf`. The tag operations use the shared guest walker;
an item must fit all 8 bytes, including at TAG_DONE. No tag data is treated
as a host pointer. Hook and formatter failure rules are described above.

The layout follows the current `rom/utility/allocatetagitems.c` and
`clonetagitems.c`: one contiguous cleared block of exactly `numTags * 8`
guest bytes, with no extra count, TAG_MORE prefix or implicit terminator
for AllocateTagItems. A clone adds one cleared terminator after the user
items returned by the walker, flattening TAG_MORE and discarding controls.
CloneTagItems(NULL) returns an allocated empty list. The native heap
interval records use `HEAP_TAGITEMS` to keep utility ownership and the
requested capacity outside guest memory. Ordinary heap and pool blocks,
interior addresses, forged headers and already freed blocks cannot be
freed or refreshed through utility. Context destruction also releases the
native records for outstanding tag blocks.

`rom/utility/refreshtagitemclones.c` copies both longwords of each user
item into consecutive slots; it does not rewrite the terminator or the
remaining tail for a non-NULL original. A NULL original writes only the
first TAG_DONE word, leaving ti_Data alone. The bridge accepts either a
CloneTagItems or AllocateTagItems block as a destination, since both have
the same layout, but requires space for the source user items plus a
terminator. It leaves the whole destination unchanged for a foreign or
undersized block, a malformed original, a ring, or scratch allocation
failure. This extends safety beyond the native function's precondition
that the original clone size and original list are unchanged.

Mutation walks first snapshot the addresses returned by the walker, so a
mapping to a control tag, or overlapping items whose writes change a
TAG_MORE link, cannot invalidate cycle detection. The subsequent lookups
and updates are live: FilterTagChanges with apply sees earlier changes
when a tag occurs again, and only the first original match is updated.
Cloning and refresh also snapshot the pair values before allocating or
writing their destination. There are no C++ exceptions on these paths.

Failure and edge rules:

| Input or failure | Bridge result and reason |
| --- | --- |
| Bad item, bad TAG_MORE, overflowing TAG_SKIP or ring | PackBoolTags, MapTags and the filters process the finite prefix returned by the walker, once per returned item. Searches of secondary lists use the same rule. No invalid item is read or written. |
| NULL tag list or NULL TAG_MORE | Ends a walk normally. PackBoolTags returns initialFlags, filters have no items, and cloning returns an empty allocation. FilterTagChanges does nothing if either list is NULL. |
| Bad or cyclic clone source | CloneTagItems returns NULL with no guest allocation; refresh does nothing. A truncated copy would not satisfy the clone contract. |
| Zero, overflowing or unavailable allocation | AllocateTagItems returns NULL; a clone also returns NULL on native scratch or heap-record failure. No native address is returned. |
| Mutation scratch allocation failure | MapTags and FilterTagChanges change nothing and preserve D0; FilterTagItems changes nothing and returns 0. Refresh also changes nothing. Temporary native storage is always freed. |
| Foreign or invalid FreeTagItems pointer | Ignored, including NULL, ordinary AllocMem/AllocVec blocks, pools and interior pointers. Only a live utility owner at the exact base can be freed. |
| Bad or unterminated tag array | TagInArray returns FALSE; FilterTagItems returns 0 without changing the list. The complete array, including its 4-byte TAG_DONE, must fit even if a match occurs earlier. TAG_MORE and TAG_SKIP in an array are ordinary values. |
| Unknown filter logic | Return 0 and change nothing, as in rom/utility/filtertagitems.c. AND keeps hits, NOT keeps misses; both preserve data and control tags. |
| NULL or invalid map, unmatched tag | Remove only for MAP_REMOVE_NOT_FOUND (0). MAP_KEEP_NOT_FOUND (1) and other map types keep misses, following rom/utility/maptags.c. A mapping to TAG_DONE becomes TAG_IGNORE; other values are copied to ti_Tag. |
| Invalid or unterminated string | Stricmp returns 0 unless both complete strings end inside guest memory, checked before it compares. Strnicmp reads only the bytes it compares, as rom/utility/strnicmp.c does, and returns 0 when one of them lies outside guest memory. These APIs have no error channel; 0 is a neutral failure result and does not certify equality for invalid input. |
| Strnicmp length 0 | Return 0 without accessing either string pointer. |
| Negative Strnicmp LONG length | Use the 32-bit unsigned countdown, avoiding signed overflow. In the 32 MiB arena the end of guest memory, a NUL or a difference comes first. |

String folding matches the unpatched `rom/utility/tolower.c`: ASCII A-Z
and bytes 0xC0-0xDE except 0xD7 gain 0x20; other bytes stay unchanged.
Bytes are unsigned, independently of host char signedness and locale.
Locale.library patches are not mirrored. Stricmp needs both full strings
to end inside guest memory even when a mismatch occurs first; Strnicmp
needs no NUL beyond the bytes it compares, so fixed-width fields work.
All new thunks preserve D1-D7, A0-A6 and SR. Void tag functions also
preserve D0; dispatch consumes the usual single return address from A7.

**mathieeedoubbas.library (12 manual thunks, `kaffeine_thunks_mathieee.c`; all update `Z`, `N` and `V` in the guest `SR` while preserving `X`, `C` and the system byte):**

| LVO | Function | Handling |
|-----|----------|----------|
| -30 | IEEEDPFix | Unpacks D0/D1 double, truncates toward zero in D0 with 32-bit signed saturation |
| -36 | IEEEDPFlt | Converts signed 32-bit LONG in D0 to IEEE 754 double in D0/D1 |
| -42 | IEEEDPCmp | Compares D0/D1 and D2/D3 doubles, returns -1/0/+1 in D0 and sets `Z`/`N`/`V` |
| -48 | IEEEDPTst | Compares D0/D1 double against 0.0, returns -1/0/+1 in D0 and sets `Z`/`N`/`V` |
| -54 | IEEEDPAbs | `fabs(D0/D1)` returned in D0/D1 |
| -60 | IEEEDPNeg | `-D0/D1` returned in D0/D1 |
| -66 | IEEEDPAdd | `D0/D1 + D2/D3` returned in D0/D1 |
| -72 | IEEEDPSub | `D0/D1 - D2/D3` returned in D0/D1 |
| -78 | IEEEDPMul | `D0/D1 * D2/D3` returned in D0/D1 |
| -84 | IEEEDPDiv | `D0/D1 / D2/D3` returned in D0/D1 |
| -90 | IEEEDPFloor | `floor(D0/D1)` returned in D0/D1 |
| -96 | IEEEDPCeil | `ceil(D0/D1)` returned in D0/D1 |

**mathieeedoubtrans.library (17 manual thunks, `kaffeine_thunks_mathieee.c`; all update `Z`, `N` and `V` in the guest `SR` while preserving `X`, `C` and the system byte):**

| LVO | Function | Handling |
|-----|----------|----------|
| -30 | IEEEDPAtan | `atan(D0/D1)` returned in D0/D1 |
| -36 | IEEEDPSin | `sin(D0/D1)` returned in D0/D1 |
| -42 | IEEEDPCos | `cos(D0/D1)` returned in D0/D1 |
| -48 | IEEEDPTan | `tan(D0/D1)` returned in D0/D1 |
| -54 | IEEEDPSincos | `sin(D0/D1)` in D0/D1, big-endian `cos(D0/D1)` written to validated guest pointer in A0 |
| -60 | IEEEDPSinh | `sinh(D0/D1)` returned in D0/D1 |
| -66 | IEEEDPCosh | `cosh(D0/D1)` returned in D0/D1 |
| -72 | IEEEDPTanh | `tanh(D0/D1)` returned in D0/D1 |
| -78 | IEEEDPExp | `exp(D0/D1)` returned in D0/D1 |
| -84 | IEEEDPLog | `log(D0/D1)` returned in D0/D1 |
| -90 | IEEEDPPow | `pow(D0/D1, D2/D3)` (`arg` in D0/D1 raised to `exp` in D2/D3) returned in D0/D1 |
| -96 | IEEEDPSqrt | `sqrt(D0/D1)` returned in D0/D1 |
| -102 | IEEEDPTieee | Converts D0/D1 double to IEEE 754 single-precision bit pattern in D0 |
| -108 | IEEEDPFieee | Converts IEEE 754 single-precision bit pattern in D0 to double in D0/D1 |
| -114 | IEEEDPAsin | `asin(D0/D1)` returned in D0/D1 |
| -120 | IEEEDPAcos | `acos(D0/D1)` returned in D0/D1 |
| -126 | IEEEDPLog10 | `log10(D0/D1)` returned in D0/D1 |

**mathieeesingbas.library (12 manual thunks, `kaffeine_thunks_mathieee.c`; all update `Z`, `N` and `V` in the guest `SR` while preserving `X`, `C` and the system byte):**

A single travels in D0, a second one in D1, and the result comes back in
D0 only. AROS implements this library with integer code that truncates
instead of rounding; as for the double precision libraries, the thunks
compute in host IEEE 754 single precision and set the flags of the Flags
sections in `workbench/libs/mathieeesingbas`. Those of `IEEESPMul()` and
`IEEESPDiv()` call a result out of range, and the AROS code sets `V` for an
exponent that is too small as well, so these two also set `V` on IEEE 754
underflow: a zero or denormal result that is not exact. libnix calls the
library for float division, so a libnix program that divides floats
anywhere opens it at startup.

| LVO | Function | Handling |
|-----|----------|----------|
| -30 | IEEESPFix | Truncates the single in D0 toward zero, with 32-bit signed saturation and `V` |
| -36 | IEEESPFlt | Converts the LONG in D0 to a single; `V` when the single is not exactly the integer |
| -42 | IEEESPCmp | Compares D0 and D1, returns -1/0/+1 in D0 |
| -48 | IEEESPTst | Compares D0 against 0.0, returns -1/0/+1 in D0 |
| -54 | IEEESPAbs | `fabsf(D0)` returned in D0 |
| -60 | IEEESPNeg | `-D0` returned in D0 |
| -66 | IEEESPAdd | `D0 + D1` returned in D0; `V` for an infinite result |
| -72 | IEEESPSub | `D0 - D1` returned in D0; `V` when finite operands overflow |
| -78 | IEEESPMul | `D0 * D1` returned in D0; `V` when finite operands overflow or the result underflows |
| -84 | IEEESPDiv | `D0 / D1` returned in D0; `V` when finite operands overflow or divide by zero, or the result underflows |
| -90 | IEEESPFloor | `floorf(D0)` returned in D0 |
| -96 | IEEESPCeil | `ceilf(D0)` returned in D0 |

**mathffp.library (12 manual thunks, `kaffeine_thunks_mathffp.c`):**

FFP words are passed as 32-bit integers, with the 24-bit explicit mantissa
in bits 31..8, the sign in bit 7 and the excess-64 exponent in bits 6..0.
They are not host IEEE floats. Zero is the all-zero word; the smallest
positive normal has exponent 1, while a negative result can retain
exponent 0. The bridge computes with explicitly sized unsigned integers
and updates the guest `N`, `Z` and `V` through `Kaffeine_SetSR()`. It keeps
`X`, `C` and the upper SR bits; native FFP treats X/C as unspecified.
No FPU or native math library is needed. At the negative exponent-zero
boundary, native sign and integral-rounding operations retain the mantissa
while clearing the sign and setting Z; division treats that dividend as
zero. The bridge keeps these measured legacy results instead of imposing
an idealized floor/ceil result.

| LVO | Function | Guest registers and result |
|-----|----------|----------------------------|
| -30 | SPFix | FFP in D0 to signed LONG in D0, truncating and saturating |
| -36 | SPFlt | Signed LONG in D0 to FFP in D0 |
| -42 | SPCmp | Returns sign(D1 - D0); the relational condition codes compare D0 with D1 |
| -48 | SPTst | FFP in D1 to -1, 0 or 1 in D0 |
| -54 | SPAbs | Clears the sign bit of D0 |
| -60 | SPNeg | Reverses the sign of D0, keeping zero canonical |
| -66 | SPAdd | D0 + D1 to D0 |
| -72 | SPSub | D0 - D1 to D0 |
| -78 | SPMul | D0 * D1 to D0 |
| -84 | SPDiv | D0 / D1 to D0; zero divisor saturates with V, including 0/0 |
| -90 | SPFloor | Floor of D0, returned as FFP |
| -96 | SPCeil | Ceiling of D0, returned as FFP |

The arithmetic and boundary cases were compared with mathffp.library 46.1
on AmigaOS 3.2.3. Eight sampled division inputs near rounding midpoints
still differ by one mantissa ULP, with matching sign, exponent and N/Z/V;
the bridge does not claim bit-identical native division. Strict replay of
the reference logs remains available in `test_mathffp_thunks --reference`.
The [target test instructions](../../developer/debug/test/kaffeine_target/README.md)
record the reference test and the AROS results. No proprietary library code
or ROM is included.

**console.device (`kaffeine_thunks_console.c`, behind its guest Device base):**

OpenDevice supports CONU_LIBRARY (-1), with separate native IOStdReq and
reply-port resources for each guest request. Opens share one guest Device
identity outside the OpenLibrary namespace; close/delete/cleanup balance
all native resources. Known CreateIORequest capacities bound every request
access, including pointers into a short allocation. Native device kind is
stored in the I/O record rather than inferred from guest-editable fields.

LVO -48, RawKeyConvert, accepts a single 22-byte guest InputEvent, a bounded
output buffer, nonnegative length and a NULL keymap. Class/subclass, code,
qualifier, four dead-key bytes and timestamp are copied into a native
InputEvent; native MapRawKey supplies the system default map. Guest pointers
are never passed as native event or keymap structures. A2 must be zero,
ie_NextEvent must be zero, and the output must not overlap the event.
Violations stop with rc 20 before native calls or output writes.

The other eleven registered device slots explicitly refuse unsupported
operations, including CDInputHandler and window-console I/O. Their fixed
LVO set and pure Kaffeine_IORefuse handler are checked by the registration
audit; they are not claims of implemented native functions. Classic linked
event and negative-length behavior is recorded separately in the target
README. The bridge deliberately supports the measured single-event subset.

**timer.device (7 manual thunks, `kaffeine_thunks_timer.c`, behind the guest base that `OpenDevice()` gives out):**

| LVO | Function | Handling |
|-----|----------|----------|
| -30 | BeginIO | Stops the guest with rc 20: asynchronous I/O |
| -36 | AbortIO | -1: nothing is in flight |
| -42 | AddTime | AROS's ADDTIME on the 68k timevals: add, then normalize |
| -48 | SubTime | AROS's SUBTIME: normalizes src in place, then dest, and subtracts |
| -54 | CmpTime | -1 when dest is the later time, 1 when src is, 0 when equal |
| -60 | ReadEClock | Native ReadEClock(); both halves to the 68k EClockVal, the rate in D0 |
| -66 | GetSysTime | Native GetSysTime() to the 68k timeval |

**locale.library (24 manual thunks, `kaffeine_thunks_locale.c`):**

| LVO | Function | Handling |
|-----|----------|----------|
| -36 | CloseCatalog | No-op paired with NULL-returning `OpenCatalogA`; preserves D0 |
| -42 | CloseLocale | A0 open guest locale: frees `GetLocaleStr` blocks and the guest `struct Locale` block (`HEAP_LOCALE`), closes the native locale, and preserves D0; NULL or foreign addresses are ignored |
| -48 | ConvToLower | A0 open guest locale, D0 character: calls native `ConvToLower` on the native locale; returns D0 unchanged for NULL or foreign locale |
| -54 | ConvToUpper | A0 open guest locale, D0 character: calls native `ConvToUpper` on the native locale; returns D0 unchanged for NULL or foreign locale |
| -60 | FormatDate | A0 open guest locale or NULL for the default locale, A1 checked/copied template, A2 checked 12-byte BE DateStamp, A3 checked 68k Hook: converts the date, passes a native Hook, delivers A2 = guest locale and A1 = unsigned character including zero, and preserves D0 |
| -66 | FormatString | A0 open guest locale or NULL for the default locale, A1 checked/copied template (NULL returns A2 unchanged), A2 checked positional BE stream, A3 checked 68k Hook: renders with AROS rules into a native snapshot, delivers A2 = guest locale and A1 = unsigned character including zero, and returns the guest address past the highest argument used |
| -72 | GetCatalogStr | Returns the caller's `defaultString` address in A1 unchanged without reading guest memory or calling native code |
| -78 | GetLocaleStr | A0 open guest locale, D0 `stringNum` (`< 71`): copies the native string into a `HEAP_LOCALE` block once per `(locale, stringNum)` and returns its guest address; returns NULL (0) for NULL/foreign locale, `stringNum >= 71`, or native NULL |
| -84 | IsAlNum | A0 open guest locale, D0 character: calls native `IsAlNum` (0 for NULL/foreign locale) |
| -90 | IsAlpha | A0 open guest locale, D0 character: calls native `IsAlpha` (0 for NULL/foreign locale) |
| -96 | IsCntrl | A0 open guest locale, D0 character: calls native `IsCntrl` (0 for NULL/foreign locale) |
| -102 | IsDigit | A0 open guest locale, D0 character: calls native `IsDigit` (0 for NULL/foreign locale) |
| -108 | IsGraph | A0 open guest locale, D0 character: calls native `IsGraph` (0 for NULL/foreign locale) |
| -114 | IsLower | A0 open guest locale, D0 character: calls native `IsLower` (0 for NULL/foreign locale) |
| -120 | IsPrint | A0 open guest locale, D0 character: calls native `IsPrint` (0 for NULL/foreign locale) |
| -126 | IsPunct | A0 open guest locale, D0 character: calls native `IsPunct` (0 for NULL/foreign locale) |
| -132 | IsSpace | A0 open guest locale, D0 character: calls native `IsSpace` (0 for NULL/foreign locale) |
| -138 | IsUpper | A0 open guest locale, D0 character: calls native `IsUpper` (0 for NULL/foreign locale) |
| -144 | IsXDigit | A0 open guest locale, D0 character: calls native `IsXDigit` (0 for NULL/foreign locale) |
| -150 | OpenCatalogA | Returns NULL (0) without calling native code so 68k programs use their built-in fallback strings |
| -156 | OpenLocale | A0 NULL (default locale) or bounded NUL-terminated guest string: opens the native locale and builds a 168-byte 68k `struct Locale` plus all pointed-to strings and grouping byte arrays in one `HEAP_LOCALE` block (up to `KAFFEINE_MAX_LOCALES` = 8 per context) |
| -162 | ParseDate | A0 open guest locale or NULL for the default locale, A1 checked 12-byte DateStamp output, A2 checked/copied template, A3 checked 68k Hook: delivers A2 = guest locale and A1 = zero, returns the hook's full D0 character to native code, writes three BE32 fields only on success, and returns native BOOL unchanged |
| -174 | StrConvert | A0 open guest locale, A1 bounded NUL-terminated guest string, A2 guest buffer of D0 `bufferSize` bytes (0 writes nothing), D1 `type`: calls native `StrConvert` |
| -180 | StrnCmp | A0 open guest locale, A1/A2 bounded NUL-terminated guest strings, D0 signed `length`, D1 `type`: calls native `StrnCmp` |

The date thunks take an open guest locale or NULL. As in AROS, NULL
selects the default locale: the bridge opens the native default locale
for this one call and closes it afterwards. Native code receives the
matching native locale while the guest Hook always receives the original
guest address, so NULL in A2 for the default locale, which has no guest
copy. They copy DateStamp fields as signed LONGs,
without normalizing calendar values. Templates must terminate inside the
arena and cannot end in an unpaired percent sign: native FormatDate would
otherwise advance past the NUL. A native allocation holds an immutable
copy, so a callback cannot remove the terminator while native code reads.

| Failure | FormatDate result | ParseDate result | Reason and effects |
| --- | --- | --- | --- |
| Foreign, closed or closing locale | Preserved D0 | FALSE | No native call, hook call or date write |
| NULL locale and no native default locale opens | Preserved D0 | FALSE | No native call, hook call or date write, as in AROS |
| NULL, odd, wrapping or short DateStamp range | Preserved D0 | FALSE | Require all 12 guest bytes before any native call; ParseDate's native verification-only NULL date mode is not exposed |
| NULL, out-of-range, unterminated or dangling-percent template | Preserved D0 | FALSE | Refuse before native parsing or output; other conversion semantics remain native |
| Malformed Hook/entry, including a return token | Preserved D0 | FALSE | Refuse before native code can invoke the hook |
| Native template allocation fails | Preserved D0 | FALSE | No output or date write; no guest heap allocation is needed |
| Native ParseDate returns FALSE | N/A | Native FALSE | Discard the native temporary even if native code changed it |
| Wrong native object or nonzero ParseDate message | rc 20 | rc 20 | Translator rejection; later hook calls return zero |
| Guest hook faults, exceeds callback depth or stops intentionally | Callback stop code | Callback stop code | Suppress later callbacks and date writeback; already completed hook memory changes/output cannot be rolled back |

`locale_map.active_calls` pins each native locale during these native
calls. `CloseLocale()` from a callback marks `close_pending`, making the
guest handle unavailable immediately; its strings, guest copy and native
locale are released after the last active native call. On a stop they
remain for ordinary context cleanup, avoiding further guest accesses in
the failed thunk. `Kaffeine_LocaleCleanup(ctx)` runs before
`Kaffeine_DestroyContext(ctx)` in `kaffeine_run.c` and releases pending closes
as well as other open locales.

`FormatString()` uses a separate renderer in `kaffeine_thunks_locale.c`.
It follows `InternalFormatString()` in
`workbench/libs/locale/formatstring.c`, with `USE_GLOBALLIMIT=1`, rather
than the sequential RawDoFmt parser in `kaffeine_format.c`. The private
vectors (-30, -168, -186, -192, -198) remain generated.

The bridge renderer avoids native packed WORD/LONG/IPTR/QUAD loads,
unaligned native pointers, architecture-dependent BSTR encodings, and
native positional indexing of untrusted templates. The native formatter
and its index generator are never given guest data. A copied template is
parsed first for the maximum argument position, then for each position's
size and kind. Arguments are packed in position order in the guest stream,
independently of their output order. Repeated references must agree on size
and kind; numeric signed/unsigned/hex/character views of the same size may
share a position. Every position through the highest one must have a known
type. Explicit positions are one-based. Implicit positions count consuming
conversions, including explicit ones, as the AROS renderer does.

Supported conversions are `b`, `s`, `c`, `d`, `D`, `u`, `U`, `x`, `X`,
`p`, `P` and `%%`. Numeric and character arguments occupy two guest bytes
by default, four with `l` or `i`, and eight with `ll` or `L`. Strings and
BSTRs always occupy four. Pointers use at least four bytes and print eight
hex digits unless an explicit eight-byte value needs more. `x`/`P` use
uppercase hex, `X`/`p` lowercase, as in AROS locale. Flags, zero padding,
width and `.limit` follow AROS, including truncating numeric bodies and
their sign and padding before a minus sign. A bare dot leaves the limit
unspecified. Unknown conversion codes print that code without consuming
an argument; a bare trailing percent or missing final code is refused.

`D` and `U` use the open native locale's grouping and separator arrays.
Each group selects its corresponding separator byte; a zero next group
repeats the previous group. This matches the AROS implementation, including
its byte-oriented separators. Guest edits to the copied Locale fields do
not change the open native locale. All guest string and BSTR ranges are
checked before output, even if `.limit` is zero. A BSTR argument is a
four-byte BCPL pointer to a length byte plus that many bytes, which may
include zero. NULL C strings and BSTRs are empty.

The bridge sizes the complete result, allocates it, and renders it before
the first callback. It owns at most three scratch allocations (template,
argument descriptors and output), using space proportional to those
objects. Only this output snapshot reaches the Hook emission loop, so
changes to the guest template, stream or strings during a callback cannot
change later output or remove a terminator. The native Hook comes from
`Kaffeine_InitNativeHook()` and is invoked with the same A0/A2/A1 order as
AROS. Its entry maps the native locale to the original guest locale and
passes each unsigned byte, including embedded zeros and the final zero.
The locale uses the same active-call pin and deferred close as the date
thunks. Successful calls preserve D1-D7, A0-A7 and SR, returning only D0.

| FormatString case | Result and effects | Reason |
| --- | --- | --- |
| NULL template | Original stream unchanged, no callback, locale and Hook unused | As in AROS `InternalFormatString()` |
| NULL locale | The native default locale renders `D` and `U` for this call; the hook gets NULL in A2 | As in AROS; zero and no callback if no default locale opens |
| Foreign, closed or closing locale | Zero, no callback | Require an open mapped locale, like the date thunks |
| Outside-arena or unterminated template | Zero, no callback | No native parser may walk unchecked guest text |
| Dangling percent, missing code, zero position, decimal width/limit/position above `0x7FFFFFFF` | Zero, no callback | Parsing and arithmetic must finish before emission |
| Missing positional type or conflicting repeated size/kind | Zero, no callback | Unknown preceding widths cannot safely locate later arguments |
| Consuming stream is NULL, odd, wrapping or too short | Zero, no callback | Check every argument's whole range by subtraction before BE loads |
| Non-NULL C string is outside the arena or has no NUL; BSTR pointer shift would overflow or its complete payload leaves the arena | Zero, no callback | Precision never excuses an invalid string range |
| No conversion consumes data | Original stream unchanged, including NULL; normal literal bytes and final zero delivered | The unused stream is never dereferenced |
| Invalid Hook or entry, including callback return tokens | Zero, no callback | Reuse the checked Hook adapter |
| Missing native grouping/separator arrays or a required grouping/separator index beyond its terminator | Zero, no callback | Refuse an incomplete native grouping description rather than read past it |
| Scratch allocation fails or total output including its terminator exceeds `0x7FFFFFFF` bytes | Zero, no callback | No partial output or guest heap allocation |
| Guest hook faults, exceeds callback depth or stops intentionally | Callback stop code; no later callbacks | Preserve the primitive's first stop code, release native scratch, and perform no further guest accesses or unwinding; earlier callback effects remain |
| Success with consumed arguments | Guest address immediately after the highest argument position, even if the final output uses an earlier/repeated position | Guest offsets, not native pointer widths or output order, determine advancement |

The host fixture executes the unchanged AROS renderer as an independent
output oracle with aligned native arguments and controlled native grouping.
Pointer-width expectations are checked separately against the 68k ABI.
The target probe uses an assembler stub to call a 68k C hook and checks
mixed WORD/LONG/string/BSTR positions, repetition and the returned pointer.

68k `struct Locale` layout (`M68K_SIZEOF_LOCALE` = 168 bytes in `kaffeine_offsets.h`):

| Define | Offset | Size | Field |
|--------|--------|------|-------|
| `M68K_LOC_LOCALENAME` | 0 | 4 | `STRPTR loc_LocaleName` |
| `M68K_LOC_LANGUAGENAME` | 4 | 4 | `STRPTR loc_LanguageName` |
| `M68K_LOC_PREFLANGUAGES` | 8 | 40 | `STRPTR loc_PrefLanguages[10]` (`M68K_LOC_PREFLANGUAGES_COUNT` = 10) |
| `M68K_LOC_FLAGS` | 48 | 4 | `ULONG loc_Flags` |
| `M68K_LOC_CODESET` | 52 | 4 | `ULONG loc_CodeSet` |
| `M68K_LOC_COUNTRYCODE` | 56 | 4 | `ULONG loc_CountryCode` |
| `M68K_LOC_TELEPHONECODE` | 60 | 4 | `ULONG loc_TelephoneCode` |
| `M68K_LOC_GMTOFFSET` | 64 | 4 | `LONG loc_GMTOffset` |
| `M68K_LOC_MEASURINGSYSTEM` | 68 | 1 | `UBYTE loc_MeasuringSystem` |
| `M68K_LOC_CALENDARTYPE` | 69 | 1 | `UBYTE loc_CalendarType` |
| `M68K_LOC_RESERVED0` | 70 | 2 | `UBYTE loc_Reserved0[2]` |
| `M68K_LOC_DATETIMEFORMAT` | 72 | 4 | `STRPTR loc_DateTimeFormat` |
| `M68K_LOC_DATEFORMAT` | 76 | 4 | `STRPTR loc_DateFormat` |
| `M68K_LOC_TIMEFORMAT` | 80 | 4 | `STRPTR loc_TimeFormat` |
| `M68K_LOC_SHORTDATETIMEFORMAT` | 84 | 4 | `STRPTR loc_ShortDateTimeFormat` |
| `M68K_LOC_SHORTDATEFORMAT` | 88 | 4 | `STRPTR loc_ShortDateFormat` |
| `M68K_LOC_SHORTTIMEFORMAT` | 92 | 4 | `STRPTR loc_ShortTimeFormat` |
| `M68K_LOC_DECIMALPOINT` | 96 | 4 | `STRPTR loc_DecimalPoint` |
| `M68K_LOC_GROUPSEPARATOR` | 100 | 4 | `STRPTR loc_GroupSeparator` |
| `M68K_LOC_FRACGROUPSEPARATOR` | 104 | 4 | `STRPTR loc_FracGroupSeparator` |
| `M68K_LOC_GROUPING` | 108 | 4 | `UBYTE *loc_Grouping` |
| `M68K_LOC_FRACGROUPING` | 112 | 4 | `UBYTE *loc_FracGrouping` |
| `M68K_LOC_MONDECIMALPOINT` | 116 | 4 | `STRPTR loc_MonDecimalPoint` |
| `M68K_LOC_MONGROUPSEPARATOR` | 120 | 4 | `STRPTR loc_MonGroupSeparator` |
| `M68K_LOC_MONFRACGROUPSEPARATOR` | 124 | 4 | `STRPTR loc_MonFracGroupSeparator` |
| `M68K_LOC_MONGROUPING` | 128 | 4 | `UBYTE *loc_MonGrouping` |
| `M68K_LOC_MONFRACGROUPING` | 132 | 4 | `UBYTE *loc_MonFracGrouping` |
| `M68K_LOC_MONFRACDIGITS` | 136 | 1 | `UBYTE loc_MonFracDigits` |
| `M68K_LOC_MONINTFRACDIGITS` | 137 | 1 | `UBYTE loc_MonIntFracDigits` |
| `M68K_LOC_RESERVED1` | 138 | 2 | `UBYTE loc_Reserved1[2]` |
| `M68K_LOC_MONCS` | 140 | 4 | `STRPTR loc_MonCS` |
| `M68K_LOC_MONSMALLCS` | 144 | 4 | `STRPTR loc_MonSmallCS` |
| `M68K_LOC_MONINTCS` | 148 | 4 | `STRPTR loc_MonIntCS` |
| `M68K_LOC_MONPOSITIVESIGN` | 152 | 4 | `STRPTR loc_MonPositiveSign` |
| `M68K_LOC_MONPOSITIVESPACESEP` | 156 | 1 | `UBYTE loc_MonPositiveSpaceSep` |
| `M68K_LOC_MONPOSITIVESIGNPOS` | 157 | 1 | `UBYTE loc_MonPositiveSignPos` |
| `M68K_LOC_MONPOSITIVECSPOS` | 158 | 1 | `UBYTE loc_MonPositiveCSPos` |
| `M68K_LOC_RESERVED2` | 159 | 1 | `UBYTE loc_Reserved2` |
| `M68K_LOC_MONNEGATIVESIGN` | 160 | 4 | `STRPTR loc_MonNegativeSign` |
| `M68K_LOC_MONNEGATIVESPACESEP` | 164 | 1 | `UBYTE loc_MonNegativeSpaceSep` |
| `M68K_LOC_MONNEGATIVESIGNPOS` | 165 | 1 | `UBYTE loc_MonNegativeSignPos` |
| `M68K_LOC_MONNEGATIVECSPOS` | 166 | 1 | `UBYTE loc_MonNegativeCSPos` |
| `M68K_LOC_RESERVED3` | 167 | 1 | `UBYTE loc_Reserved3` |

**workbench.library (7 manual thunks, `kaffeine_workbench.c`):**

The native workbench.library sends AppMessages to a native port that the
bridge makes for each 68k port that the program gives it, which replies
each at once and keeps a carrier of it for the program in a second port
with the signal of the 68k port, which must be one that the program
allocated, and the task that runs the guest (see
[AppMessage Flow](#appmessage-flow)). The
program gets a guest token for each App object. A tag list with any tag
stops the program: the tags are not bridged yet. Every other LVO of the
library returns 0.

| LVO | Function | Handling |
|-----|----------|----------|
| -48 | AddAppWindowA | A window of the program; the native AppWindow behind a guest token, or 0 |
| -54 | RemoveAppWindow | Native RemoveAppWindow() and its BOOL; 0 gives FALSE, a token of no AppWindow rc 20 |
| -60 | AddAppIconA | A DiskObject that the program built in its memory, as the autodoc has it (no magic, no version, no `GFLG_GADGIMAGE`) or as an icon file has it, with any magic, copied into native memory with its gadget, its render Image, the select Image of the `GFLG_GADGHIMAGE` bit (which `GADGHNONE` has too), its default tool and tool types; the copy has the magic of an icon and `GFLG_GADGIMAGE`, without which icon.library of AROS draws no image. Each Image has its Depth planes in the copy, as `DupDiskObject()` copies them; from guest memory come the planes that drawing takes, one for each bit of PlanePick and at most Depth, as `intuition.h` says and `DrawImage()` takes them, and the others stay zero. As on AmigaOS 3.2.3, measured, 0 without stopping the program for `GFLG_GADGHIMAGE` without a select Image and for an Image without a width, a height, planes or data, or with more than 8 planes, and without a render Image, as AROS gives; an Image wider or higher than 1024, or a DiskObject, Image or data outside guest memory, stops the program. The text copied; the lock as the bridge passes every lock. The copies stay until RemoveAppIcon(), as AROS keeps both pointers. A DiskObject of the bridge's icon.library is a native one, which the bridge cannot read yet |
| -66 | RemoveAppIcon | Native RemoveAppIcon(), then the copies go |
| -72 | AddAppMenuItemA | The text, of at most 1024 bytes, copied until RemoveAppMenuItem() |
| -78 | RemoveAppMenuItem | Native RemoveAppMenuItem() and its BOOL |
| -90 | WBInfo | The lock as the bridge passes every lock, a name of at most 1024 bytes in guest memory, a screen of the program or NULL |

### Guest semaphore rules

All thirteen semaphore vectors in the NDK 3.2 `exec_lib.fd` are manual
overrides in `kaffeine_exec_compat.c`; no guest semaphore or message reaches
native Exec. Before this change, -558/-564/-570 were manual no-ops with
incorrect comments naming -372/-378/-384, -720 was a manual FALSE stub,
and the other nine vectors used generated native passthroughs. No generated
file was edited or regenerated.

The guest SignalSemaphore is 46 bytes: Node at 0, signed WORD nesting at
14, MinList wait queue at 16, SemaphoreRequest multiple link at 28, owner
at 40 and signed WORD queue count at 44. `InitSemaphore()` follows
`rom/exec/initsemaphore.c`, leaving node links, name, priority and the
multiple link untouched. The free state is nesting 0, owner NULL and queue
count -1, with an empty wait queue containing guest sentinel addresses.

For one guest task, the rules in `rom/exec/semaphores.c` reduce to:

- An exclusive acquisition of a free semaphore sets owner to the guest
  ExecBase ThisTask value. Further exclusive or shared calls by that owner
  increment nesting and queue counts without changing the owner.
- A shared acquisition of a free or already shared semaphore leaves owner
  NULL and increments the shared nesting/queue counts. An exclusive attempt
  of a shared lock fails, even though this context has only one guest task.
- Each release decrements both counts. The final release restores owner
  NULL, nesting 0 and queue count -1. Attempts return exactly 1 or 0, as
  AROS does, and a failed attempt leaves the guest structure unchanged.

The bridge accepts only initialized states with an empty wait queue and
queue count equal to nesting minus one. It rejects count overflow, corrupt
links/counts, native shadow objects and any whole structure or string
outside the guest arena. Subtraction-based bounds checks precede every
guest access, including list links and strings; no unchecked guest address
is passed to the generic inline memory accessors.

Attempts fail with FALSE on invalid state, a missing task identity or a
foreign owner. A void call that would wait, mutate corrupt state, release a
foreign owner or underflow a free semaphore stops the context with
`ctx->running = FALSE`, `ctx->exit_code = 20` and D0 = 20. It leaves the
guest structures unchanged. A shared-to-exclusive blocking obtain takes
that same path, since the guest cannot release its shared lock while
waiting. `Procure()` and `Vacate()` always take this explicit stop path:
their message queue/reply protocol has no bridge yet, and returning as if
the void call succeeded could leave the caller waiting for a reply forever.
No exception, native wait, message or signal is used. `Kaffeine_RunCPU()`
returns D0 after the stop, so setting only `exit_code` would be insufficient.
Successful void semaphore operations preserve D0 and every other register.

List calls snapshot and check the complete list, its reciprocal links,
non-overlapping member structures and all member states before any writes.
They accept at most 256 members, using a fixed 1 KiB native address array.
A longer list, ring, overlap or invalid member stops with rc 20 without
partially acquiring or releasing members. `FindSemaphore()` returns NULL
on an invalid name/list. This bounded policy also prevents a corrupt list
from trapping the emulator in an unbounded walk.

`shadow_init_execbase()` initializes the guest ExecBase SemaphoreList as an
empty normal guest List and does not register it as a native list shadow.
`AddSemaphore()` initializes its argument and enqueues it there, higher
signed priorities first and existing equal priorities first. Duplicate or
overlapping insertion fails before initialization. `RemSemaphore()` checks
membership and preserves the removed node's own links, as native Exec does.
The list is private to the guest context: native public semaphores and other
guest contexts are not visible. This is not synchronization between contexts.

`test_exec_semaphores` exercises every vector through the real dispatcher
with literal LVOs and poisoned fallbacks. The target `exec_memory` covers
the normal lifecycle and provides separate `procure`/`vacate` negative
probes. The target README records both on AROS AArch64.

### Guest public ports and native-only Exec objects

`AddPort()` (-354), `RemPort()` (-360) and `FindPort()` (-390) use A1
and only the public PortList embedded at offset 392 in the guest ExecBase.
`shadow_init_execbase()` initializes it as an empty guest List of type
NT_MSGPORT (4) and never registers it as a native list shadow. Port names
in native Exec, including names associated with native shadows, are not
imported. Each guest context has its own list.

A classic MsgPort is 34 bytes: Node at 0, flags at 14, signal bit at 15,
signal task at 16 and the 14-byte message List at 20. `AddPort()` follows
`rom/exec/addport.c`: set ln_Type to NT_MSGPORT, initialize the three
message-list links, then enqueue by descending signed priority with
existing equal-priority members first. It preserves the flags, signal
fields, name, priority and reserved List bytes. Reinitializing the message
links discards any previously active queue, as native AddPort does; callers
must publish an inactive port.

The shared `exec_list()` checker validates the whole list before any
mutation: header bounds and NULL tail, each full object range, reciprocal
predecessors, the final tail predecessor, no native shadow, no overlapping
objects or header, and at most 256 members. `FindPort()` checks every
published name before returning the first match. `AddPort()` also checks
the new name and all existing names, and refuses duplicate or overlapping
insertion before initializing anything. A string must terminate within
guest memory; a NUL in the last byte is valid. `RemPort()` requires list
membership and preserves the removed node's own stale links. It can remove
a member whose name has become invalid, provided the links remain valid.

A published guest port can carry ordinary messages within its context.
PutMsg, GetMsg, ReplyMsg and nonempty WaitPort use checked big-endian FIFO
links in `kaffeine_messages.c`. Enqueue operations support PA_IGNORE or
PA_SIGNAL for the current task and an owned signal. Native IntuiMessage,
GadTools and Workbench protocols retain their separate ownership paths.
A native message reaches native Exec only through its exact native shadow;
a guest message is never cast to a native Message or MsgPort.

Queue inspection is separate from arrival permission checks. Nonempty
WaitPort peeks without signaling or waiting; GetMsg removes a message, or
returns zero from an empty queue, without consulting arrival metadata.
Enqueue and packet reply preflight still validate the action, current task
and allocated signal before writes, native I/O or Signal. A port may change
its arrival flags and use reserved bit4 between enqueue and inspection,
as measured on AmigaOS, without granting permission to signal that bit.

The native PutMsg path additionally requires a known UserPort shadow and
a taken IntuiMessage. An exact Window, Task or unknown shadow is not a
MsgPort or Message and is refused before native Exec. Taken messages become
WaitPort-owned shadows after forwarding; GadTools copies and already queued
IntuiMessages remain refused.

| Failure case | Result and reason |
| --- | --- |
| FindPort: missing name, native-only name or another context's name | NULL, like a program that is not running. There is no cross-context port bridge. |
| FindPort: invalid or unterminated search/published name, corrupt list or more than 256 members | NULL, with no writes. The full checked search must succeed before returning an object. |
| AddPort: invalid name/object, native shadow, overlapping or duplicate node, full list or corrupt links/names | Stop the guest with running FALSE, exit_code 20 and D0 20, before any guest writes. A void mutation cannot report successful publication. |
| RemPort: corrupt/oversized list or NULL, foreign, absent or repeatedly removed node | The same atomic rc 20 stop. Foreign or stale node links are never followed. |
| FindResident (-96) or OpenResource (-498): valid, missing, NULL, outside or unterminated name in A1 | Check the name, log invalid ranges and return NULL. No native Resident or resource pointer is exposed. |
| AddResource (-486) or RemResource (-492): any A1 value | No-op, without dereferencing A1 or touching native Exec. Guest resource registration is unsupported. |
| PutMsg (-366): NULL, mixed native/guest protocols, malformed or overlapping guest queues, duplicate messages or unsupported arrival actions | Stop with running FALSE, exit_code 20 and D0 20 before native Exec or native file I/O sees an invalid object. Valid ordinary guest queues and recognized ACTION_READ handlers use explicit adapters. |

Successful void operations preserve D0, all other registers and guest
memory except the specified list fields. FindResident and OpenResource
only change their D0 result. All twelve vectors in this batch match the
register declarations in NDK 3.2 `exec_lib.fd` and `rom/exec/exec.conf`.

Programs that probe ARexx or another public server see it as absent.
Single-instance checks cannot detect other native programs or other guest
contexts, so they may allow another instance. Publishing a guest port does
not enable communication across those contexts.
Resident scanners, ROM/module-presence probes and patchers see no resident;
software requiring resources such as `dos.resource` or hardware resources
must handle an unavailable resource or cannot proceed. Their native
structures, function vectors and code addresses have no safe 68k ABI in
this bridge. AddResource and RemResource therefore leave native state alone.

`CacheClearE()` at -642 reads A0 (address), D0 (length) and D1 (caches).
It checks `addr >= mem_size || mem_size - addr < length` before any range
could be used and logs invalid ranges, except the length ~0 that the
autodoc gives for all addresses. With `CACRF_ClearI` in D1 it calls the
backend's `flush_code()` for the range, or for all guest memory for that
length and for a range outside guest memory; without it nothing changes.
It preserves D0 and every other register, does not read or write the range
and never passes an arena offset to native CacheClearE. `CacheClearU()` at
-636 calls `flush_code()` for all guest memory and preserves D0. Moira
fetches instructions directly, has no translated instruction cache and no
`flush_code()`; see [Changed guest code](#changed-guest-code).

### Auto-Generated Thunk Type Translations

The generator (`kaffeine_thunkgen.py`) classifies each parameter:

| Classification | Translation | Used For |
|---------------|-------------|----------|
| shadow | `m68k_to_host_or_shadow()` | RastPort, Window, Screen, ViewPort, BitMap, ColorMap, Layer, Region, TextFont, Menu, Gadget, Task, Process, SignalSemaphore, IORequest, Library, Device, MsgPort, etc. |
| string | `m68k_to_host()` | STRPTR, CONST_STRPTR, char*, UBYTE* |
| taglist | `m68k_to_native_taglist()` + cleanup | struct TagItem* |
| input_struct | Skipped (needs manual thunk) | NewWindow, NewScreen, TextAttr, EasyStruct, NewGadget, Border, IntuiText |
| scalar | `THUNK_D(n)` as ULONG | ULONG, LONG, WORD, BOOL, BPTR, Tag, APTR |
| pointer | `THUNK_PTR(n)` or `THUNK_DPTR(n)` | Remaining pointer types |

Return values: void functions return 0; struct pointer returns get wrapped with `shadow_create_by_name()`; scalars are cast to IPTR.

## Shadow Struct Engine

### API

| Function | Purpose |
|----------|---------|
| `shadow_create(ctx, layout, native)` | Allocate m68k heap, register, sync all fields native→m68k. Returns m68k address. |
| `shadow_sync(ctx, layout, m68k_addr, native)` | Re-sync all fields from native→m68k for existing shadow. |
| `shadow_destroy(ctx, layout, m68k_addr)` | Remove from registry, free m68k heap. |
| `shadow_find_layout(name)` | Linear scan of 297-entry table (with static cache for last hit). |
| `shadow_create_by_name(ctx, name, native)` | Convenience: find_layout + create. |
| `shadow_init_execbase(ctx, m68k_base, native)` | Syncs fields and AttnFlags, uses `shadow_task()` for ThisTask, initializes guest public semaphore and port lists and registers the other 7 embedded lists. |
| `shadow_task(ctx, native, current)` | Reuses a Task shadow or allocates one cleared 228-byte Process-sized snapshot; only the current process gets this context's CLI, arguments and streams: `Input()`, `Output()` and the native `pr_CES`, the error stream that the Shell sets for `*>` and libnix uses as stderr. |
| `shadow_register(ctx, m68k_addr, native, type)` | Append to shadow_map[] (max 1024). Defined with lookup/removal in `kaffeine_shadow.c`. |
| `shadow_lookup(ctx, m68k_addr)` | Linear scan by m68k_addr, returns native pointer or NULL. |
| `shadow_remove(ctx, m68k_addr)` | Linear scan + swap-with-last removal. |
| `m68k_to_native_taglist(ctx, m68k_addr)` | Convert m68k TagItem list (8B/entry) to native (16B on 64-bit). Values in [0x100, mem_size) treated as pointers. |
| `Kaffeine_TagWalkInit()`, `Kaffeine_TagWalkNext()` | Walk a guest TagItem list in place (`kaffeine_taglist.c`) with the semantics of AROS `NextTagItem()` and bounds checks. A list that loops ends where the walk would return an item a second time: Floyd's cycle detection finds the loop, then where it starts and its length. `ti_Data` is never converted. Used by the manual utility tag thunks; mutation walks snapshot their returned addresses before writing. |
| `m68k_to_native_struct(ctx, name, m68k_addr)` | Reverse-sync m68k→native (allocates with 3× overestimate). |

### Field Types

```c
enum { SF_END=0, SF_BYTE, SF_WORD, SF_LONG, SF_PTR };
```

| Type | Sync Behavior |
|------|--------------|
| SF_BYTE | Direct byte copy |
| SF_WORD | 16-bit big-endian via m68k_write16/read16 |
| SF_LONG | 32-bit big-endian via m68k_write32/read32 |
| SF_PTR | Complex: NULL→0, scan for existing shadow of same native_ptr+type, if sub_layout→recursive create, else→0 |
| SF_END | Terminator sentinel |

### Type Detection Heuristic (in generator)

| m68k size | AROS size | Classification |
|-----------|-----------|---------------|
| 1 | 1 | SF_BYTE |
| 2 | 2 | SF_WORD |
| 4 | 8 | SF_PTR (pointer widening 32→64 bit) |
| 4 | 4 | SF_LONG |
| other | other | Skipped |

### Registry

Flat array in `KaffeineContext`:

```c
struct { ULONG m68k_addr; void *native_ptr; UWORD type; } shadow_map[1024];
UWORD num_shadows;
```

- **Register**: O(1) append
- **Lookup**: O(n) linear scan by m68k_addr
- **Remove**: O(n) scan + O(1) swap-with-last
- **Dedup**: sync_fields scans for matching native_ptr + shadow_type to avoid duplicates

`FindTask(NULL)` at -294 reads A1 and returns the value at offset 276 of
`ctx->m68k_sysbase`. It neither calls native FindTask nor allocates or
refreshes a shadow. Preparing a context in one native process and executing
it in another therefore retains the same guest task identity. Startup
arguments are published in `setup_and_execute()` before creating that
initial snapshot.

A named FindTask first validates the complete guest string, asks native
Exec for its task and passes the result to `shadow_task()`. The cache key
is the native pointer plus the generated Task shadow type, so repeated
names or aliases share the same guest address, including a named lookup of
the original current task. Cached guest edits are retained. With a full
1024-entry registry, an existing task still resolves and a new task returns
NULL before allocation. Allocation failure also returns NULL and publishes
no shadow. Snapshots last for the context; native task removal/address reuse
and live field synchronization are not tracked. `test_exec_tasks` uses the
real initialization, layouts and dispatcher to check these identities,
capacity/failure behavior and guest-pointer bounds.

Type tags: 0=untyped, 1=Window, 3=RastPort, 4=MsgPort (IDCMP port), 5=Screen, 6=ViewPort, 8=IntuiMessage that the program took, 9=GadTools copy of an IntuiMessage, 10=IntuiMessage that WaitPort() showed, still in its port, 11=handle of a GadTools VisualInfo, 12=68k mirror of a gadget of a GadTools list, 13=68k Screen of a public screen that the program locked, 17=List, 20+=auto-generated layout types. The manual Intuition and graphics types are named in `kaffeine_rastport.h`; 68k TextFonts have a table of their own.

### Recursive Sub-Shadows

`M68KFieldMap` has `sub_layout` (const M68KStructLayout*) for pointer fields. When `sync_fields` encounters SF_PTR with non-NULL `sub_layout`, it recursively allocates and syncs the pointee. **However**: all auto-generated layouts set `sub_layout=NULL`. The recursive capability is only exercised by manually constructed layouts (OpenScreen, OpenWindow, ExecBase init).

## Shadow Struct Layouts

297 struct layouts with 2,918 field mappings (1,237 pointers correctly detected as SF_PTR for 32→64 bit conversion). Format: Name (m68k size → AROS size, field count).

### Most Thoroughly Mapped

| Struct | m68k | AROS | Fields | Domain |
|--------|------|------|--------|--------|
| GfxBase | 552B | 1040B | 65 | graphics |
| Screen | 346B | 1408B | 62 | intuition |
| Process | 228B | 768B | 56 | exec |
| Window | 136B | 272B | 26 | intuition |
| ColorMap | 52B | 96B | 19 | graphics |
| ConUnit | 296B | 640B | 19 | devices |
| IOStdReq | 48B | 96B | 16 | exec |
| CommandLineInterface | 64B | 128B | 16 | dos |
| MonitorSpec | 160B | 896B | 14 | graphics |
| timerequest | 40B | 80B | 14 | devices |
| MsgPort | 34B | 256B | 13 | exec |

### Complete List

The full list of 297 struct layouts is in `kaffeine_shadow_layouts.h` (auto-generated).

### Missing Structs

1 of 298 structs in `m68k_offsets.txt` lacks a shadow layout:

**RegionRectangle** — its only field is `bounds` (an embedded `struct Rectangle`,
8 bytes), which has no scalar leaf fields to map.

## Device I/O Path

Ports, I/O requests and signals of 68k code are 68k structures in the guest
arena (`kaffeine_exec_io.c`). Behind each open request stand a native request
of the native size and a private native reply port, which only that file
passes to native Exec. Nothing native reaches the guest, and no guest
structure reaches native Exec. This is the first step, synchronous
`timer.device`. Asynchronous I/O with guest reply queues, waits on mixed
signals, messages between guest ports, IDCMP on the same model and the
other devices, each with its own adapter, follow it.

### Call Chain

```plain
m68k: CreateMsgPort()
  → cleared 34-byte 68k port in the guest heap: NT_MSGPORT, PA_SIGNAL,
    a native signal of the task that runs the guest, mp_SigTask = ThisTask

m68k: CreateIORequest(port, sizeof(struct timerequest))
  → cleared 68k request of that size (20 to 65535) with mn_ReplyPort, mn_Length

m68k: OpenDevice("timer.device", UNIT_MICROHZ, ioReq, 0)
  → native CreateMsgPort() and CreateIORequest(sizeof(struct timerequest)),
    native OpenDevice() on that request
  → guest io_Device = guest timer.device base, io_Unit = unit, io_Error = 0

m68k: DoIO(ioReq)
  → io_Command and both time fields to the native request, native DoIO()
  → both time fields, io_Error, IOF_QUICK and ln_Type back to the guest

m68k: TimerBase = ioReq->io_Device; GetSysTime(&tv)
  → native GetSysTime() of the native TimerBase, both fields to the guest

m68k: CloseDevice(ioReq)
  → native CloseDevice(), DeleteIORequest() and DeleteMsgPort(); io_Device = -1

m68k: DeleteIORequest(ioReq), DeleteMsgPort(port)
  → guest memory and the guest signal freed
```

### Rules

- Signals: `AllocSignal()`, `CreateMsgPort()` and the private reply ports take
  native bits of the task that runs the guest. With a stack size, `RunHunk()`
  runs the guest in a child process, not in the task behind the guest's
  ThisTask snapshot: `Signal()` of the current guest task goes to the running
  task, and `Signal()` of any other task stops the guest with rc 20.
  `FreeSignal()` frees only bits that the guest allocated.
- Devices: only `timer.device` has an adapter so far. Another name fails with
  `IOERR_OPENFAIL`, as an unknown device does, without a native call. The
  guest request needs room for a 40-byte timerequest. Units 0 to 4 open;
  `TR_ADDREQUEST` on `UNIT_ECLOCK` and `UNIT_WAITECLOCK` ends with
  `IOERR_NOCMD`, because the AROS driver never replies to it.
- Commands: `TR_ADDREQUEST` and `TR_GETSYSTIME`. `TR_SETSYSTIME` would set the
  clock of the whole system and ends with `IOERR_NOCMD`, as every other
  command does. The time fields pass unchanged, so the native driver treats
  oversized microseconds as it does for native callers.
- Asynchronous I/O: `SendIO()` and `BeginIO()` of the device stop the guest
  with rc 20; guest reply queues and a completion pump are the next phase.
  `CheckIO()`, `WaitIO()` and `AbortIO()` answer for complete requests.
- Copies: `DoIO()` on a request that the bridge did not open, such as a copy
  of an open timerequest, stops the guest with rc 20; with no device at all
  it returns -1, as Exec does.
- Ownership: a context keeps at most 16 ports and 16 requests. The guest
  `timer.device` base is a fake device base with the name, type and version of
  the native one; `OpenLibrary()` does not find it. `DeleteIORequest()` of an
  open request closes it first. At the end of the program `Kaffeine_IOCleanup()`
  closes what the guest left open and frees its signals, in the task that ran
  it, before that task ends: `RunHunk()` and `RunFile()` call it there.

### IDCMP Message Flow

`kaffeine_window.c` keeps the IDCMP side of 68k windows:

1. `OpenWindow()` gives the window a cleared 68k Window (offset 86 is
   UserPort) and its IDCMP port as a 68k MsgPort registered as type 4:
   signalling, with the signal bit of the native port and the current guest
   task, and an empty list. `Wait(1L << win->UserPort->mp_SigBit)` waits for
   the native port, whose signal belongs to the task that runs the guest.
   Without guest memory for the Window, its RastPort or its port, the
   native window closes again and the program gets 0.
2. `GetMsg()` on that port takes the native message and gives a 68k
   IntuiMessage of 52 bytes, registered as type 8: Class, Code, Qualifier,
   MouseX, MouseY, Seconds, Micros and IDCMPWindow as the 68k Window, whose
   fields come up to date with it. IAddress is the 68k Window when it names
   the window, the 68k mirror of a GadTools gadget of the program when it
   names one (GadTools Gadgets, item 12), and 0 for any other native
   object and for the tag list of `IDCMP_IDCMPUPDATE`.
3. `WaitPort()` gives a 68k message for the first native message without
   taking it, registered as type 10. That message is still Intuition's, and
   something native, such as `SysReqHandler()` or Intuition's
   `CloseWindow()`, may take it and free it, so the bridge never reads the
   native message of a type 10 shadow: it only compares the pointer. When
   `GetMsg()` takes that message, the shadow becomes type 8 and `GetMsg()`
   returns the same 68k message; what is left of messages that have left the
   port goes at the next `WaitPort()` or `GetMsg()` of the window, at
   `ModifyIDCMP()` when the port goes, and at `CloseWindow()`.
4. `ReplyMsg()` returns the native message of a type 8 shadow, with the Code
   that the program may set for IDCMP_MENUVERIFY, and frees the 68k one, so a
   program can take any number of messages.
5. `GT_GetIMsg()` and `GT_ReplyIMsg()` work as AROS's do: `GetMsg()` and
   `GT_FilterIMsg()`, which replies a message of GadTools only at once, then
   `GT_PostFilterIMsg()` and `ReplyMsg()` of the original. The copy that
   GadTools makes of each message gets a 68k message registered as type 9,
   which only `GT_ReplyIMsg()` and `GT_PostFilterIMsg()` take, so the bridge
   sees each original go back too. `GT_FilterIMsg()` and
   `GT_PostFilterIMsg()` do the same for programs that take messages with
   `GetMsg()`.
6. `ModifyIDCMP()` makes or deletes the 68k MsgPort when Intuition makes or
   deletes the native port. When the 68k port cannot be made, the bridge
   calls `ModifyIDCMP()` again with the old flags and returns FALSE, as
   Intuition does when it cannot make its port. A UserPort that the program
   set itself, to share one port among windows, would need messages between
   68k ports, and stops the program with rc 20.
7. `CloseWindow()`, before the native window goes, gives GadTools back each
   copy that the program holds, replies its original and every message that
   the program took and has not replied, and frees the 68k messages of the
   window with its 68k Window, RastPort and port: a program cannot reply a
   message once its 68k message is gone. Intuition's own `CloseWindow()`
   replies what is still in the port.
8. EasyRequests (`kaffeine_requester.c`): the bridge formats the text and the
   gadget labels of a 68k EasyStruct with the checked RawDoFmt of
   `kaffeine_format.c`, the gadgets taking the arguments after the text, and
   cuts the labels as AROS does: one gadget more than the `|` of the gadget
   template, each label up to the next `|` of the formatted labels, also past
   a NUL that an argument put there, so a `|` that an argument prints does
   not make a gadget of its own. Native Intuition gets the text and the
   labels as native `%s` arguments of `"%s"` and `"%s|...|%s"`, so nothing of
   the 68k argument stream reaches it and it makes the same gadgets. Without
   memory for the text, the requester fails as Intuition fails then, and
   the program goes on. A requester window from `BuildEasyRequestArgs()` is
   a 68k Window;
   `SysReqHandler()` handles it natively and takes the messages of its port,
   so what `WaitPort()` showed of them goes as in point 3, and
   `EasyRequestArgs()` never hands the window to the program.

### AppMessage Flow

`kaffeine_workbench.c` keeps the AppMessages of the App objects:

1. The first App object on a 68k port, a signalling port whose signal the
   program allocated (from `CreateMsgPort()` or `AllocSignal()`), gets two
   native ports. Workbench gets the first, a `PA_CALL` port of AROS:
   `PutMsg()` to it calls a function of the bridge in the task of the
   sender, which copies each AppMessage into a native carrier, with its
   WBArgs, their names and `DupLock()` duplicates of their locks, puts the
   carrier to the second port and replies the AppMessage at once. Workbench
   of AmigaOS does not wait for the program, and the `SendAppWindowMessage()`
   of AROS waits for the reply holding the lock of Workbench, which
   `RemoveAppWindow()` and the other App calls take, so a reply that waited
   for the program could stop both, at a removal of the program's or of
   the bridge's own. The second port has the signal bit of the 68k port and
   the current task, and all App objects on the 68k port share both, so
   that `Wait(1L << port->mp_SigBit)` wakes up for an AppMessage of any of
   them, as on AmigaOS.
2. `GetMsg()` of the 68k port takes the program's own messages first. With
   an empty list it takes the next carrier and gives a 68k copy of 86
   bytes: every field, its WBArgs and their names in the same guest block,
   and the duplicated locks as the native BPTRs that the bridge passes for
   every lock. `WaitPort()` with an empty list waits natively, then puts
   the copy at the tail of the list, where `GetMsg()` takes it. Up to 32
   copies can be out at once; a carrier that finds no entry or no guest
   memory goes, and the program finds its port empty, so that `WaitPort()`
   stops it with rc 20 rather than wait for ever.
3. `ReplyMsg()` of a copy frees the copy and its carrier, with the
   duplicated locks, taking the copy out of the port's list first when the
   program replies it without `GetMsg()`.
4. `DeleteMsgPort()` of the port, `FreeSignal()` of its signal and the end
   of the program remove its App objects from Workbench, then free its
   copies, the carriers still in the second port and the native ports;
   after the App objects, nothing puts to them.

### GadTools Menus

`kaffeine_menus.c` gives 68k programs the menus of GadTools:

1. `GetVisualInfoA()` takes a screen of the program, one that it opened or
   the 68k Screen of `LockPubScreen()`, and returns a 4-byte 68k handle registered as type
   11; the layout calls take it, and `FreeVisualInfo()` frees both.
2. `CreateMenusA()` checks the whole 68k NewMenu array up to NM_END first:
   entries with NM_IGNORE and types that GadTools does not know are left
   out, image items (`IM_ITEM`, `IM_SUB`) and a separator as a title stop
   the program with rc 20, and every label and command key must be a string
   in guest memory. Native GadTools gets native entries whose strings point
   into guest memory and whose UserData is the 68k value, and the tags
   `GTMN_FrontPen`, `GTMN_FullMenu` and `GTMN_SecondaryError` through a
   checked walk of the 68k tag list. AROS writes the secondary error into
   its tag, as an earlier GadTools did, except when it runs out of memory at
   once; the NDK writes it through the pointer. The bridge gives the 68k
   ULONG the error either way.
3. The strip is mirrored in guest memory: a 68k Menu with the UserData of
   GadTools after it for each Menu, a 68k MenuItem with its UserData for
   each item and subitem, 68k IntuiTexts for labels, command strings and
   the mark of a submenu, a 68k TextAttr for each font (the program's own
   for `GTMN_TextAttr`), a 68k Image without data for each separator, and
   guest copies of the strings that GadTools adds itself. Each strip keeps
   its own table of native objects and their mirrors, so a menu of any size
   takes no entry of the shadow table. Without NM_TITLE, as
   `GTMN_FullMenu` FALSE allows, the strip is a chain of 68k MenuItems,
   which Intuition does not take as a strip.
4. The mirror follows the native strip after every call that may change
   it, and before an IDCMP_MENUPICK, IDCMP_MENUVERIFY or IDCMP_MENUHELP
   message reaches the program (`ctx->menu_sync`, called by
   `kaffeine_window.c`), so that CHECKED, the mutual exclusions and the
   NextSelect chain are right when the program reads them. `ClearMenuStrip()`
   mirrors the strip once more after Intuition lets it go, for a menu
   message still on its way. The CHECKED and ITEMENABLED flags of items and
   MENUENABLED of menus that the program sets in the mirror go to the
   native strip before `SetMenuStrip()`, `ResetMenuStrip()` and the layout
   calls, as `ResetMenuStrip()` allows, but not before `OffMenu()` and
   `OnMenu()`, which work on a strip in use, where Intuition may have
   checked an item for a message still on its way. The UserData of menus
   and items is written once and stays the program's. Of the tags, the
   first of each counts, as for `GetTagData()`.
5. `SetMenuStrip()`, `ResetMenuStrip()` and `ClearMenuStrip()` set the
   MenuStrip field of the 68k Window; `ItemAddress()` returns 68k items.
   Other changes to the mirror, such as new links between menus, are not
   taken. The images of `GTMN_Checkmark` and `GTMN_AmigaKey` are left out
   of the layout, so GadTools draws its own.
6. `FreeMenus()` frees the native strip and the mirror, after taking the
   strip off any window of the program that still shows it, which would
   show a freed strip otherwise. `CloseWindow()` takes a strip off its
   window first, and at the end of the program every strip comes off the
   windows that it left open and goes, also one that it never freed.

### Menus That the Program Builds

`kaffeine_menus.c` also gives Intuition menu strips that a program builds
itself in its own memory, of NDK Menus (30 bytes) and MenuItems (34
bytes), with the native copies that `kaffeine_copy.c` makes:

1. `SetMenuStrip()` checks all of the strip before native code sees any of
   it, in the first of two runs over it that count the room of the copies,
   and makes the copy in the second run in one block: Menus and MenuItems
   with one level of subitems (the SubItem of a subitem is not followed:
   Intuition has no submenus of submenus), ItemFill and SelectFill as
   IntuiText chains for text items and as Image chains for others. Strings
   (menu names, texts and font names) are copied too, so that what the
   program writes in its own later, even over their end, does not reach
   Intuition; image data stays where it lies in guest memory, planar and
   big-endian as graphics takes it. Texts that use a TextAttr one after
   another share its copy, which goes without the tags of a TTextAttr. A
   table of the copies of Menus and MenuItems, sorted by their native
   addresses, finds the program's one for each. JazzX, JazzY, BeatX and
   BeatY are Intuition's, which native
   `SetMenuStrip()` works out in the copy; the flags that Intuition keeps
   for itself (HIGHITEM, ISDRAWN, MENUTOGGLED, MIDRAWN, and 0x8000, with
   which AROS marks an item that has subitems) are left out of it.
2. A strip with a Menu, MenuItem, IntuiText, TextAttr or Image outside
   guest memory, more menus (31), items (63) or subitems (31) than a menu
   number names, which also ends a chain in a cycle, a Menu or MenuItem in
   two places of the strip (two copies of it would change apart), an
   IntuiText or Image chain longer than 1024, a string that does not end in
   guest memory or is longer than 255 characters (`TextExtent()` and
   `TextLength()` give its width in a WORD, which the layouts of Intuition
   add up in WORDs), a menu without a name, a text item without an IntuiText (AROS
   follows both in any case), an image of a BOOPSI class, of a negative size or of
   more than 8 planes, planes that `DrawImage()` reads with more than 32767
   words or outside guest memory or at an odd address, or more than 32768
   structures to copy in all, a shared TextAttr counted once for each text,
   stops the program with rc 20, and the window keeps the strip that it
   showed. An IntuiText without a text, a TextAttr without a font name and
   the ImageData of an image whose planes `DrawImage()` does not read are
   taken as AROS takes them. No memory for a new copy makes the call return
   FALSE.
3. Windows that show the same strip share its copy, which `SetMenuStrip()`
   and `ResetMenuStrip()` of a strip that a window shows, the same window
   included, take. Once no window shows it, after `ClearMenuStrip()`,
   another strip or none on the window, `CloseWindow()` or
   `FreeSysRequest()` (both take a strip off the window first), the copy
   waits, as one of the eight that waited last: `ResetMenuStrip()` on any
   window takes it back, as the Clear, change, Reset of the NDK asks,
   without memory and as fast as AROS's, while `SetMenuStrip()` makes a
   new copy, since the program may have changed any of it. A ninth copy
   that comes to wait lets the oldest go, and the end of the program frees
   them all; a copy that waits points at no window. A new copy goes through
   native `SetMenuStrip()` also for `ResetMenuStrip()`, which works out no
   page sizes.
4. Intuition writes in its copy as AROS does in any strip, so the
   program's structures keep what it put there. CHECKED and ITEMENABLED of
   items and MENUENABLED of menus pass between the two by bit, each way
   only what one side changed since both last agreed, which the bridge
   keeps for each Menu and MenuItem: to the program before a menu message
   reaches it, after `OffMenu()` and `OnMenu()`, whenever the strip leaves
   a window (`ClearMenuStrip()`, another strip or none, `CloseWindow()`),
   and for the item that `ItemAddress()` returns; to the copy at
   `SetMenuStrip()` and `ResetMenuStrip()` of a strip that a copy exists
   for. The windows that show the copy let it go meanwhile, with native
   `ClearMenuStrip()`, which waits for a menu of Intuition that uses it,
   and get it back with `ResetMenuStrip()`. A change of the program that
   has not reached Intuition yet stays. NextSelect is Intuition's, which
   follows the chain of a pick in its copy: it goes to the program as a
   whole, the items of a menu pick, from the code of the message along the
   chain, in any case, and never back.
5. `ItemAddress()` walks the program's own strip as AROS's walks a native
   one, also a strip that no window shows, and returns the program's item
   or subitem. `FreeMenus()` and the layout calls of GadTools take only
   strips of `CreateMenusA()`: `FreeMenus()` leaves a strip that the
   program built alone, and `LayoutMenusA()` and `LayoutMenuItemsA()` stop
   the program with rc 20, as for any strip or item that is not one of
   GadTools.

### GadTools Gadgets

`kaffeine_gadgets.c` lets 68k programs create the gadgets of GadTools, read
and change their attributes, and put their lists on windows (item 12),
whose messages name the 68k gadgets. The setters draw on the window that
a gadget is on, and `ActivateGadget()` activates a gadget there.

1. `CreateContext()` lets native GadTools make the context: the 68k context
   goes to the program's list pointer and to D0.
2. `CreateGadgetA()` takes as the previous gadget only one that
   `CreateContext()` or `CreateGadgetA()` returned; NULL as the previous
   gadget, the NewGadget or its VisualInfo gives 0 at once, as GadTools, so
   that a program may create its gadgets without a check after each. The
   68k NewGadget (30 bytes) becomes a native one: the label stays in guest
   memory, which GadTools copies, the TextAttr is a native copy kept with
   the gadgets, since the classes may keep it, the VisualInfo is the native
   one of the 68k handle and UserData is the 68k value. `makeitext()` counts
   the label and the name of its font in WORDs, so each must be shorter
   than 16000 characters. NULL as the VisualInfo gives 0 before the
   previous gadget is looked at, as in GadTools.
3. Only the tags that GadTools reads for the kind reach it, flat, through
   `TAG_MORE`, `TAG_SKIP` and `TAG_IGNORE`, and of a repeated tag only the
   one that GadTools uses: the first for `GT_Underscore` and for buttons,
   checkboxes, cycles and mxes, whose make functions use `GetTagData()`,
   the last for the other kinds, whose make functions loop over the list.
   Only that one is checked, and the tags keep their order, which matters
   for `GTPA_Depth` and `GTPA_NumColors`. Numbers and flags are
   sign-extended as LONGs; a NUL `GT_Underscore`, which `makeitext()` would
   find at the end of every label, becomes the default of GadTools, no key
   letter. `GTCY_Labels` and `GTMX_Labels` become native arrays of the
   program's strings; `GTTX_Text` stays in guest memory; `GTNM_Format` and
   `GTSL_LevelFormat` must be a RawDoFmt() format that fetches no pointer
   and no more numbers than the single ULONG that the text class passes
   (a WORD takes two of its bytes, a LONG four), and whose output for any
   number fits the buffer of 256 characters of the class as AROS's
   RawDoFmt() counts it, with width, precision and sign in ULONG
   arithmetic; they are copied. The palette class keeps as its count of
   colours the UWORD of what GadTools works out from `GTPA_Depth` and
   `GTPA_NumColors`; of a depth of 64 or more the shift of the CPU makes a
   count that the bridge cannot know. `GTPA_ColorTable` must have room in
   guest memory from its start for 256 colours past `GTPA_ColorOffset`, or
   for all of them when there are more, since the class draws them from the
   offset on and reads the table at its choice and at its old choice plus
   the offset, each a UBYTE that a pen which is not in the table makes as
   it is; and for at most 32767 colours, since the class looks for the pen
   of `GTPA_Color` in them with a WORD index. The bridge
   keeps the choice of the palette as the class keeps it, an index of the
   table: where the pen is in the table, else the pen itself, or without a
   table the pen less the offset; creation lays the palette out, which
   takes back a choice beyond its colours. `GTST_String` is cut to the
   MaxChars of the string class, which copied it without a limit before it
   cut texts to MaxChars itself, and the
   MaxChars of a string or an integer, one more as a WORD, must stay above
   0. The text
   class indexes the pens of the DrawInfo with `GTTX_FrontPen` and
   `GTTX_BackPen` (and their `GTNM_` names): a pen beyond `NUMDRIPENS` is
   left out, and the class keeps its own. The cycle class draws
   `labels[active]` of its array: an empty array stops the program, and a
   `GTCY_Active` beyond the labels becomes the first, as the setter of the
   class makes it. A listview's `GTLV_Labels` becomes a native copy of the
   List (item 11), and its `GTLV_ShowSelected` is NULL, `~0` or the
   string gadget of a `CreateGadgetA(STRING_KIND)` (item 11). Hooks and
   functions of 68k code (`GTST_EditHook`, `GTIN_EditHook`,
   `GTSL_DispFunc`, `GTLV_CallBack`), any other `GTLV_ShowSelected` and
   input outside guest memory stop the program with rc 20.
4. Every gadget that GadTools made in the call is mirrored in guest memory
   as a 68k ExtGadget (56 bytes), linked as the native list: the one it
   returns and those it puts before it, the two arrows of a scroller, the
   level display of a slider and the scroller of a listview. A mirror has
   the geometry, the flags,
   the activation, the type, the ID, the bounds of an extended gadget with
   the bits of `MoreFlags` that mean the same in AROS and the NDK
   (`GMORE_BOUNDS`, `GMORE_GADGETHELP`, `GMORE_SCROLLRASTER`; AROS's
   `GMORE_BOOPSIGADGET`, `0x8`, which gadgetclass sets on every gadget, is
   another bit in the NDK, whose own `0x400` a mirror does not get, being
   no 68k BOOPSI object), the label as 68k IntuiTexts with their
   TextAttrs, as a string or as an image, and the UserData, which is the
   program's: written once, and only for a gadget with the UserData of the
   NewGadget. The mirrors, as the VisualInfo handles of the menus, lie in
   guest memory of a kind of their own, which `FreeMem()` of the program
   cannot release while the bridge still writes them. STRING_KIND and
   INTEGER_KIND get a 68k StringInfo with the text in a buffer in guest
   memory and LongInt, SLIDER_KIND and SCROLLER_KIND a 68k PropInfo, and so
   does the prop of a listview's scroller, which GadTools puts just before
   the listview. Native
   pointers that the program has no use for stay out: the render images,
   the dispatcher of the class that gadgetclass keeps in MutualExclude, and
   the extension of a StringInfo with the flag that announces it.
5. Each 68k gadget is also a shadow of type 12, so that the generated
   thunks of Intuition find the native gadget; the shadow table has 1024
   entries for them. Without guest memory or a free shadow, the gadgets of
   the call go again, natively and in the mirror, and `CreateGadgetA()`
   returns 0 with no NextGadget after the previous gadget, as when GadTools
   fails.
6. Each call makes a batch: its gadgets, a table of their native objects
   and mirrors, and the native copies that they point to. Since the
   previous gadget is always the last of a batch, what GadTools cuts off a
   list there is whole batches, which stay until the program frees them.
   `FreeGadgets()` starts at a context or at the first gadget of a batch,
   anything else that the bridge made stops the program with rc 20, and
   anything else is ignored; it frees the batches of the program's list
   from there on, as GadTools frees the gadgets: each batch as GadTools
   linked it, and from its last gadget on where the 68k NextGadget leads,
   past gadgets of the program's own and as far as the program cut the
   list, as AmigaOS 3.2.3 does. The native links between batches need not
   be the program's, so each batch goes by itself. It cuts the native and
   the 68k NextGadget of every gadget of the bridge that stays and led
   into them, which would point into freed memory until the program cuts
   it, as it should. A list that leads into the middle of a batch, back
   to a batch already on it or to a batch on a window stops the program
   before anything goes (item 12); one that goes round through gadgets of
   the program's own ends after 65536 of them, and one of them outside
   guest memory ends it. At the end of the program the bridge takes the
   gadgets that it left off the windows that are still open, then frees
   them.
7. `DisposeObject()` of a gadget stops the program with rc 20, as it
   would free the gadget behind the back of its batch. So do the calls of
   Intuition that would reach the native
   gadget through its shadow past the bridge: `SetAttrsA()`,
   `SetGadgetAttrsA()` and `DoGadgetMethodA()`, which would hand its class
   68k tags or a 68k message, `GetAttr()`, whose class would write a native
   IPTR into the 68k LONG; other objects reach these as their generated
   thunks pass them. `ActivateGadget()` passes Intuition a GadTools gadget
   of the program only with the window that its batch is on, and returns
   Intuition's BOOL, which needs that window active, no other gadget
   active and the gadget enabled; the gadget then stays active until
   input ends it or `RemoveGList()` or `CloseWindow()`, which deactivate
   it, take it off, before `FreeGadgets()` can free it. Anything else
   gives FALSE without Intuition: AROS activates whatever gadget it gets,
   on the window's list or not, and no gadget of the program's own is on a
   window.
8. `GT_GetGadgetAttrsA()` fills in the attributes that its autodoc gives
   for the kind of the gadget that a `CreateGadgetA()` call returned, each
   as a 68k LONG, and returns how many it filled in:

   | Kind | Attributes |
   | --- | --- |
   | BUTTON | `GA_Disabled` |
   | CHECKBOX | `GA_Disabled`, `GTCB_Checked` |
   | CYCLE | `GA_Disabled`, `GTCY_Active`, `GTCY_Labels` |
   | INTEGER | `GA_Disabled`, `GTIN_Number` |
   | LISTVIEW | `GA_Disabled`, `GTLV_Top`, `GTLV_Labels`, `GTLV_Selected`, `GTLV_Total`, `GTLV_Visible` |
   | MX | `GA_Disabled`, `GTMX_Active` |
   | NUMBER | `GTNM_Number` |
   | PALETTE | `GA_Disabled`, `GTPA_Color`, `GTPA_ColorOffset`, `GTPA_ColorTable` |
   | SCROLLER | `GA_Disabled`, `GTSC_Top`, `GTSC_Total`, `GTSC_Visible` |
   | SLIDER | `GA_Disabled`, `GTSL_Min`, `GTSL_Max`, `GTSL_Level` |
   | STRING | `GA_Disabled`, `GTST_String` |
   | TEXT | `GTTX_Text` |

   The bridge reads the whole list first, through `TAG_MORE`, `TAG_SKIP`
   and `TAG_IGNORE`: the attributes of the kind, every occurrence, at most
   512, each with a storage of four bytes in guest memory, else the
   program stops with rc 20 before anything is written; tags of another
   kind are passed over with their storage, as GadTools passes them over.
   It brings the mirrors of the gadget's batch up to date with the native
   gadgets, and then asks native GadTools for each attribute through a
   one-tag list into a native IPTR. Numbers are written as the classes
   give them, the WORDs of a slider sign-extended and the UWORDs of a
   scroller as they are; `GTCY_Labels` is the program's own array, which
   the bridge keeps with the native copy, `GTPA_ColorTable` the program's
   own table, and `GTST_String` the buffer of the 68k StringInfo. AROS's
   cycle and palette classes fill their attributes in without counting
   them, and the bridge counts them as the autodoc does; AROS's text class
   has no getter, so `GTTX_Text` and `GTNM_Number` come from what
   `CreateGadgetA()` converted: the number, the program's text, or, for
   `GTTX_CopyText`, a copy of the text in guest memory, as AmigaOS gives
   its own copy. A GENERIC gadget, the context and the gadgets that
   GadTools puts before another have no attributes and get no method,
   since the first two are no BOOPSI objects. NULL as the gadget or the
   list gives 0, as GadTools; any other address than a gadget of the
   bridge stops the program.
9. Bringing the mirrors of a batch up to date goes by its table, not by
   native links, and leaves `NextGadget` and the program's UserData alone.
   The text of a StringInfo goes into the guest buffer up to the size that
   the buffer was made with, since native code may change MaxChars without
   another buffer, and the 68k MaxChars is the smaller of the two. A label
   or a StringInfo whose new copy finds no guest memory stays as it was,
   whole: an IntuiText chain gets every copy that it needs before any of
   them is written, a TextAttr only with its name, a StringInfo only with
   the copy of its buffer, and the old label keeps its own kind in
   `Flags`.

10. `GT_SetGadgetAttrsA()` changes the attributes that its autodoc gives
    for the kind of the gadget that a `CreateGadgetA()` call returned:

    | Kind | Attributes |
    | --- | --- |
    | BUTTON | `GA_Disabled` |
    | CHECKBOX | `GA_Disabled`, `GTCB_Checked` |
    | CYCLE | `GA_Disabled`, `GTCY_Active`, `GTCY_Labels` |
    | INTEGER | `GA_Disabled`, `GTIN_Number` |
    | LISTVIEW | `GA_Disabled`, `GTLV_Top`, `GTLV_MakeVisible`, `GTLV_Labels`, `GTLV_Selected` |
    | MX | `GA_Disabled`, `GTMX_Active` |
    | NUMBER | `GTNM_Number`, `GTNM_FrontPen`, `GTNM_BackPen`, `GTNM_Justification`, `GTNM_Format` |
    | PALETTE | `GA_Disabled`, `GTPA_Color`, `GTPA_ColorOffset`, `GTPA_ColorTable` |
    | SCROLLER | `GA_Disabled`, `GTSC_Top`, `GTSC_Total`, `GTSC_Visible` |
    | SLIDER | `GA_Disabled`, `GTSL_Min`, `GTSL_Max`, `GTSL_Level`, `GTSL_LevelFormat`, `GTSL_DispFunc`, `GTSL_Justification` |
    | STRING | `GA_Disabled`, `GTST_String` |
    | TEXT | `GTTX_Text`, `GTTX_FrontPen`, `GTTX_BackPen`, `GTTX_Justification` |

    Every occurrence goes to native GadTools in its order, in one call,
    since a class may collapse some attributes before its superclass sees
    them: the button takes the first `GA_Disabled`, the others the last
    of what they keep. Before the call each attribute is converted and
    checked, and anything that the bridge refuses stops the program before
    native GadTools sees any of them: a `GTCY_Labels` array becomes a
    native array kept with the gadget, with the program's own array for
    the getter, and an empty one stops the program; a format must be one
    that the text class can take (as at creation) and is copied, and a
    NULL `GTNM_Format` stops the program, since the class would format
    with it; `GTTX_Text` stays in guest memory, which the class borrows,
    and the getter gives it from then on, as `GTNM_Number`; `GTST_String`
    is cut to the buffer of the string class, the smaller of its MaxChars
    and the size that the class made the buffer with, since strgclass.c
    copied it with `strcpy()` until it cut texts to MaxChars itself, and
    the bridge may run on either, and NULL empties it; `GTIN_Number` needs a
    MaxChars of at least 1 and no larger than that buffer, since the class
    writes the number as far as MaxChars, and the class of older systems
    before the buffer for less;
    a pen beyond `NUMDRIPENS` is left out; a non-NULL `GTSL_DispFunc`
    stops the program. The copies that a class keeps, label arrays and
    formats, join the gadget only when the call goes through.

    A palette must stay in guest memory with the table that it has after
    the call, the one of the call or its own: the class looks for the pen
    of `GTPA_Color` in as many colours of the table as it has, with a WORD
    index, it draws them from its offset on, and `GT_GetGadgetAttrsA()`
    reads the pen of its choice. So a table needs at most 32767 colours,
    and room in guest memory for 256 colours past the offset, or for all of
    them when there are more: the class reads it at its choice and, as it
    draws the change on a window or a click, at its old choice, plus the
    offset, each a UBYTE, and the old choice can be a pen of an earlier
    `GTPA_Color` that is not in the table, or of the same call. The bridge
    works the choice out as the class does: where the pen is in the table,
    else the pen itself, or without a table the pen less the offset.

    `win` must be a window of the program or NULL; `req` is not passed.
    GadTools draws the change when it gets the window of the gadget: the
    bridge passes the native window when the batch of the gadget is on
    that window, where a palette was laid out as it went on; on no window,
    or with another one, the gadget changes without drawing, and the
    program sees the change once `RefreshGList()` draws it on a window. AROS's slider takes no `GTSL_LevelFormat` or
    `GTSL_Justification` after creation; it keeps its levels in WORDs and
    their number in a UWORD of the prop class, so a range of 65536 levels
    makes a total of 0 there, whose knob fills the gadget, as for a native
    program: the bridge passes it, since nothing reads or writes outside
    memory then. After the call the mirrors of the batch follow the native
    gadgets, as they do after `OnGadget()` and `OffGadget()` of Intuition
    on a GadTools gadget. Those leave a gadget alone without a window, as
    AROS does; with a window of the program they change it as AROS does,
    through `GA_Disabled` for a BOOPSI gadget, which disables the arrows of
    a scroller too, and `GFLG_DISABLED` for a GENERIC gadget and the
    context, drawn when its batch is on that window. `ModifyProp()` and
    `NewModifyProp()` of a GadTools gadget go to Intuition as the generated
    thunks passed them, with a window of the program and no requester: AROS
    changes only prop gadgets of Intuition (`GTYP_PROPGADGET`), which the
    class of a GadTools slider or scroller makes it only while it handles
    input or draws it; those of the program's own go through the bridge of
    the next section (item 8).

11. A listview holds a List, which the bridge gives its class as a native
    copy: the 68k List is checked first as listviewclass.c walks it, from
    `lh_Head` along `ln_Succ` to the Node whose `ln_Succ` is 0, which needs
    only those four bytes and is not counted, also when it is not
    `&lh_Tail`; every Node before it needs its 14 bytes in guest memory and
    a name that is a string in guest memory, or none, which becomes an
    empty one; at most 32767 Nodes, the WORD that the class counts in,
    which also ends a ring. Then one native block gets the header and the
    Nodes, linked natively, with the names in guest memory and `ln_Type`
    and `ln_Pri` copied. `0xFFFFFFFF` is the native `~0` that detaches the
    List, and NULL stays NULL. Every attach makes a new copy, also of the
    same List, which the program may have changed while it was detached;
    the batch keeps the copy that the class holds and frees the one before,
    and the copies of earlier `GTLV_Labels` of the same call, right after
    the native call, within which the class drew, if at all, with the List
    that it holds; no input comes in between. The
    getter gives the program's List for the copy, `0xFFFFFFFF` for `~0` and
    0 for NULL; `GTLV_Selected` -1 for none; `GTLV_Total` and
    `GTLV_Visible` under their NDK numbers, `GT_TagBase + 92` and `+ 93`,
    which AROS numbers `+ 150` and `+ 151`. The class divides by the height
    of an item plus the spacing, so a listview whose item height (of
    `GTLV_ItemHeight`, else of the TextAttr) is below 1 or whose
    `LAYOUTA_Spacing` is negative as a WORD stops the program, as does a
    `GTLV_ScrollWidth` below 1: at 0 the scroller gets no arrows, and
    gadgets.c takes its prop from beyond them; a negative width is no
    width either. AROS's builder gives the
    class `GA_Disabled` before the scroller exists, and `GTLV_Top` and
    `GTLV_MakeVisible` before the List, which resets the top; the bridge
    gives them to the new listview again, in that order, as AmigaOS takes
    them at creation. A listview too low for one row keeps AROS's
    arithmetic: it shows no row, and `GTLV_MakeVisible` can put its top
    past the last entry.
    A string gadget of `GTLV_ShowSelected` belongs to the batch of its own
    `CreateGadgetA()`. gadgets.c takes it only when its bounds are as wide
    as the listview and lower than the listview's corrected height, else
    the creation fails, and moves it to the bottom of the listview's box
    with its own height, before it rounds the list area to whole rows; it
    does so even when the creation fails afterwards. The class writes the
    name of the selected entry into it with `GT_SetGadgetAttrsA()` at every
    `GTLV_Labels`, also of the same List, and at every `GTLV_Selected` that
    changes the selection, and an empty string when the List is detached
    or empty or no entry of it is selected; the mirrors of its batch follow
    after the creation and after every `GT_SetGadgetAttrsA()` of the
    listview. strgclass.c cuts that name to the MaxChars of the string
    gadget. AmigaOS 3.1 and 3.2.3 refuse the same widths and heights, but
    move only the edges of the string gadget and keep its bounds, and keep
    the last name while the List is detached; 3.2.3 shows the same names,
    3.1 one character more. A `FreeGadgets()` that would free the string
    gadget while the listview stays stops the program: the class would
    write into freed memory.
12. A gadget list of the program goes on a window of the program as a
    whole, as `FirstGadget` of `OpenWindow()`, as the last `WA_Gadgets` of
    `OpenWindowTagList()` or through `AddGList()` and `AddGadget()`, and
    each of its batches knows the native window it is on. The list starts
    at a context or at the first gadget of a batch and holds whole
    batches, as far as it goes or for `AddGList()` a `numGad` that ends at
    the end of a batch. Within a batch its 68k links must be the native
    ones; from the last gadget of a batch on it goes where the program's
    68k list goes, as AmigaOS 3.2.3 takes it: to its end where the program
    cut it, to the gadget that followed on a window, where `RemoveGList()`
    leaves it, or to another batch that the program linked on, and the
    native links follow, as a window takes the native list to its end. A
    list with a gadget that the program built itself, a batch already on a
    window or a list in a cycle stops the program with rc 20 before
    Intuition sees it, as does a requester. `AddGadget()` takes a batch of
    one gadget, such as a context or a button. Gadgets that the program
    built itself go on as copies (see the next section).
    Intuition lays the gadgets out and splices them in; then their mirrors
    and 68k links follow, and the call returns the UWORD position.
    `RemoveGList()` and `RemoveGadget()` take whole batches off the window,
    `numGad` of them or, for -1, the rest of the window's list, which must
    then hold only gadgets of the bridge. The 68k NextGadget of the last
    one stays on the gadget that followed it, the mirror of a GadTools
    gadget, the program's own gadget behind its copy or 0 for a system
    gadget, as AmigaOS 3.2.3 leaves it where the NDK says that V36 cuts
    it; its native one goes, so that no native link leads from gadgets off
    a window into its list. `CloseWindow()` takes every gadget of the
    program off the window before it closes, and they stay the program's,
    with their 68k links as they were. `FreeGadgets()`
    of a chain with a batch on a window stops the program, as do
    `CreateGadgetA()` after a gadget on a window, which GadTools would link
    into the window's list, and a `GTLV_ShowSelected` string gadget on a
    window, which gadgets.c would move there. Since the class draws the
    string gadget of a listview's `GTLV_ShowSelected` into the listview's
    window, a listview goes on a window only with that string gadget,
    already on it or coming with it, and the string gadget comes off only
    with its listview or after it; the listview may come off alone. A
    palette goes on a window only when the layout of paletteclass.c, which
    the bridge works out as the class does, leaves its colours at least one
    column and one row of a pixel each: the class divides by them there and
    on a click. A list does not go between the gadgets of one batch on the
    window, such as the arrows of a scroller, at the position that
    `AddGList()` or `AddGadget()` would put it. `RefreshGList()` and
    `RefreshGadgets()` refresh gadgets of the bridge on that window
    natively, and the mirrors of the gadgets on it then follow what
    rendering set, such as the NumChars of a string; they do nothing for
    other gadgets. A window whose 68k Window cannot be made after it took a
    list gives the list back before it closes again.
    Before a message of a window reaches the program through `GetMsg()`,
    `WaitPort()`, `GT_GetIMsg()` or `GT_FilterIMsg()`, and when GadTools
    keeps a message for itself, the mirrors of the gadgets on that window
    follow what the input changed, and so do those of the gadget that the
    message names, which GadTools changes as it filters the message also
    when the program took it off the window after the message came.
    IAddress names the 68k mirror of the gadget, after GadTools' filter,
    which gives the arrows of a scroller to the scroller; Code stays as
    Intuition and GadTools set it. The 68k Window's `FirstGadget` does not
    show the window's list, whose own gadgets have no mirror. A
    `GADGETDOWN` or `GADGETUP` still waiting for a gadget that is neither
    on its window, nor in a requester there, nor a gadget of the bridge
    any more, which the program freed after the message came, goes back to
    Intuition, and the program does not see it: GadTools' filter, which
    reads the gadget of a `GADGETDOWN`, does not see that one, and sees
    the `GADGETUP` without its gadget, so that it forgets the gadget that
    it keeps active since the `GADGETDOWN`. Where no `GADGETUP` comes, as
    when the button goes up outside an arrow, GadTools keeps that gadget
    until the next `GADGETDOWN` and reads it at each tick or mouse move: the
    bridge notes the gadget of every `GADGETDOWN` that GadTools filters, and
    `FreeGadgets()` of that gadget, while its context stays, first makes
    GadTools forget it, with a `GADGETUP` without a gadget through its
    filter as from a window that holds only that context. GadTools keeps
    it only when it has no memory for its copy of that message while the
    program holds its copy of another: then `FreeGadgets()` stops the
    program with rc 20 and frees nothing. `FreeGadgets()` frees the
    gadgets of the bridge from its argument on, as far as the program's
    list goes (item 6).

### Gadgets That the Program Builds

`kaffeine_classic.c` puts gadgets that a program builds itself in its own
memory on its windows, Boolean, string, integer and proportional gadgets,
as native copies that `kaffeine_copy.c` makes:

1. A run of the program's gadgets goes on a window whole or stays off it.
   `FirstGadget` of `OpenWindow()`, `WA_Gadgets`, `AddGList()` (numGad
   gadgets, or with -1 as far as the 68k list goes) and `AddGadget()` (one
   gadget) check the run first: a gadget outside guest memory (44 bytes,
   56 for `GFLG_EXTENDED`), a run that goes on with GadTools gadgets, a
   gadget twice in it or one that is on a window already, a run longer
   than 4096 gadgets, and a position between the gadgets of one
   `CreateGadgetA()` on the window, which stay together, stop the program
   with rc 20. A run with a gadget of another kind than `GTYP_BOOLGADGET`,
   `GTYP_STRGADGET` and `GTYP_PROPGADGET`, of a requester or a screen, a
   system gadget (also
   one with a system subtype alone, which the input handler goes by), a
   masked Boolean gadget (`GACT_BOOLEXTEND`, the bit of
   `GACT_STRINGEXTEND` for a string gadget), a string gadget with a keymap
   of its own (`GACT_ALTKEYMAP` and an AltKeyMap: a 68k KeyMap would need
   a native copy of its tables) or a gadget object (the NDK's
   `GMORE_BOOPSIGADGET`, `0x400` in `MoreFlags`) stays off the window, as every gadget of the
   program's own did before: `AddGList()` and `AddGadget()` give
   `(UWORD)~0`, and the window opens without it. So does a run without
   memory or a free shadow for its copies. The requester of `AddGList()`
   is ignored, as Intuition ignores it for gadgets without
   `GTYP_REQGADGET`.
2. Each gadget gets a native copy in one block: a Gadget, or an ExtGadget
   for `GFLG_EXTENDED` with its bounds and of its `MoreFlags`
   `GMORE_BOUNDS` and `GMORE_SCROLLRASTER`, which mean the same in AROS
   and the NDK (`GMORE_GADGETHELP` stays out until a test with the
   pointer checks it: before #71 the input handler kept the gadget under
   the pointer for its help past `RemoveGList()`, when a copy may go); its
   GadgetRender and, for
   `GFLG_GADGHIMAGE`, which alone draws it, its SelectRender, as Image
   chains for `GFLG_GADGIMAGE` and as Border chains otherwise; its
   GadgetText as an IntuiText chain, or as a string or an Image chain for
   `GFLG_LABELSTRING` and `GFLG_LABELIMAGE`, which AROS draws, and nothing
   for both label bits; MutualExclude and UserData as the numbers they
   are, the StringInfo of a string gadget (item 7) and the PropInfo and
   knob of a proportional gadget (item 8), no SpecialInfo for other
   gadgets. A Border gets its Count XY pairs as native
   WORDs (none for a Count below 1, which `DrawBorder()` does not draw),
   image data stays in guest memory as for menus, and strings are copied,
   at most 255 characters. Chains longer than 1024 or in a cycle, and
   Borders, XY pairs, texts, fonts or images outside guest memory or that
   menus refuse too, stop the program with rc 20. A copy starts without
   `GACT_ACTIVEGADGET`, which Intuition sets itself. The copies are linked
   as the 68k run, and each is a shadow of type 14 while it is on the
   window, so that the generated thunks of Intuition find it.
3. `GFLG_SELECTED` and `GFLG_DISABLED` pass between a copy and the
   program's gadget by bit, against what both sides last agreed, each way
   only what one side changed, in the copy bit by bit, as the input
   handler changes the other bits meanwhile: to the program before every
   message of the window reaches it, after `AddGList()`, which AROS draws,
   `OnGadget()`, `OffGadget()` and `RefreshGList()`, and as the gadget
   comes off the window; to the copy
   before `RefreshGList()`, `RefreshGadgets()`, `OnGadget()` and
   `OffGadget()`, so that a program can select a toggle gadget itself and
   refresh it, and so does the state of a string gadget (item 7).
   `GACT_ACTIVEGADGET` only goes to the program. What the program changes
   in the renders, the label, the geometry, the other bits or the rest of
   a StringInfo reaches Intuition as the gadget goes on a window again
   after `RemoveGList()`, the way the NDK has a program change a gadget.
4. The 68k NextGadget of the program's gadgets on a window follows the
   native list as `AddGList()` and `RemoveGList()` splice it, also next to
   GadTools gadgets: the program's next gadget there, a GadTools mirror or
   its own, or 0 before a system gadget and at the end. `RemoveGList()`
   and `RemoveGadget()` take a run of the program's own gadgets off
   natively, numGad of them or the rest of the window's list, which must
   all be its own (anything else stops the program with rc 20), and leave
   the NextGadget of the last one on the gadget that followed it, as
   AmigaOS 3.2.3 and AROS leave it, where the NDK says that V36 cuts it
   (DOpus relies on it); a gadget that is not on that window gives
   `(UWORD)~0`.
5. `OnGadget()` and `OffGadget()` of the program's gadget on that window
   enable or disable its copy natively, which Intuition draws with the
   gadgets after it, as the NDK has it, the state of every gadget of the
   program's own on the window passed to the copies first and back after;
   with another window or none only
   the flag of the copy changes, and the program's gadget follows it, and
   a gadget on no window gets
   `GFLG_DISABLED` set or cleared in its own Flags, with nothing to draw.
   A native object behind another shadow, such as a gadget object,
   reaches Intuition as before. `ActivateGadget()` of the program's gadget
   on that window passes its copy to Intuition and returns its BOOL, after
   which the program's gadget follows: for a Boolean gadget Intuition
   gives TRUE while the window is active and no gadget is in use, enabled
   or not, and leaves the gadget as it is, as AmigaOS 3.2.3 (AROS gave
   FALSE before #71).
6. A message names the program's gadget in IAddress, also once the gadget
   came off the window: its copy stays while a message of the window, in
   the port or held by the program, names it, so that no new copy at its
   address takes the message over, and the next `AddGList()` or
   `RemoveGList()` frees the copies that no message names. `CloseWindow()`
   takes the program's gadgets off the window first and frees the copies
   of its gadgets, and at the end of the program the bridge takes the
   gadgets off the windows left open and frees every copy.
7. A string gadget, with `GACT_LONGINT` or not, gets a native StringInfo
   in its block whose Buffer, UndoBuffer and, with a StringExtend,
   WorkBuffer point at the program's buffers in guest memory, MaxChars
   bytes each, so that Intuition edits the program's text itself; its
   other fields as their values, and AltKeyMap NULL, as a 68k KeyMap is
   not a native one. `GFLG_STRINGEXTEND` or `GACT_STRINGEXTEND`, the same
   since V37, give the copy a native StringExtend, with the native font of
   the program's TextFont, its Pens, ActivePens and InitialModes, and
   `GFLG_STRINGEXTEND`. A StringInfo or a StringExtend outside guest
   memory, a MaxChars below 1, a buffer of less than MaxChars bytes in
   guest memory, an EditHook, which Intuition would call as native code
   from the input handler's task, or a font that is not a 68k TextFont of
   the bridge (one that the program opened, or the font of one of its
   windows or screens) stop the program with rc 20. The text may hold its
   NUL past MaxChars, as AmigaOS 3.2.3 counts a text beyond MaxChars:
   Intuition counts it up to 32766 bytes as far as its NUL and writes at
   its end, which a guard of 32 KB of zero bytes after guest memory
   (`KAFFEINE_MEM_GUARD`), out of the program's reach, keeps inside the
   memory of the bridge, however the program leaves its text and whenever
   the input handler reads it. BufferPos, DispPos and LongInt pass both
   ways as wholes, each way only what one side changed since both agreed,
   to the copy only while Intuition does not edit the gadget
   (`GFLG_SELECTED`), as the input handler reads them between its own
   checks, and the positions within the text as it is then, which
   Intuition counts first at a refresh too, so that the program gets them
   as Intuition would have made them; NumChars, DispCount and UndoPos,
   which Intuition keeps, only go to the program. They pass at the
   moments of item 3, after `OpenWindow()` and `AddGList()`, which draw
   the gadgets and count their text, and around `ActivateGadget()`. A
   copy of a string gadget starts without `GFLG_SELECTED`, which Intuition
   sets while it edits the gadget, and loses it as it comes off the
   window, as on AmigaOS 3.2.3; the program's gadget follows at the next
   pass. `SetEditHook()` stops the program with rc 20.
8. A proportional gadget gets a native PropInfo in its block with the
   program's values, and as its GadgetRender the Image of its knob whatever
   its flags, into which Intuition writes where the knob is, and for
   `AUTOKNOB` how large: for `AUTOKNOB` the Image alone, without data or a
   next Image, which programs may leave unset, else the program's own Image
   chain, its data in guest memory as for other images; and for a gadget
   without one an empty Image that the program does not see, as Intuition
   would write through NULL for a knob of the program's own. No
   SelectRender, which Intuition does not read for it. A PropInfo or a knob
   Image outside guest memory stops the program with rc 20. The PropInfo's
   Flags pass both ways by bit, `KNOBHIT` among them, and HorizPot,
   VertPot, HorizBody and VertBody as wholes, each way only what one side
   changed since both agreed, to the copy only while the button does not
   hold the gadget (`GFLG_SELECTED`), when the input handler moves them
   itself; the bridge checks that and writes them under `Forbid()`, since
   the input handler selects the gadget and sets `KNOBHIT` as the button
   goes down. CWidth, CHeight, HPotRes, VPotRes, LeftBorder and TopBorder,
   which Intuition works out, and the LeftEdge, TopEdge, Width and Height
   that it writes into the copy's knob Image only go to the program, into
   its PropInfo and its knob Image, at the moments of item 3, from one
   sample of both taken under `Forbid()`, so that the input handler changes
   neither while it is taken; an update of its that waits halfway, as on a
   lock, can still show in it. `AUTOKNOB` stays out of this: it chooses the
   render of the knob, so it reaches Intuition as the gadget goes on a
   window again after `RemoveGList()`, as a change of the renders does, and
   the program's PropInfo keeps the one that the program set; Intuition
   would otherwise draw an Image of the program's own at the size that it
   gave an automatic knob, past the data checked for it. A copy starts
   without `GFLG_SELECTED`. `NewModifyProp()` and `ModifyProp()` of the
   program's gadget on a window, with a window of the program, pass the
   state of the program's gadgets on its window to the copies, change the
   copy through Intuition, which draws it into the window given, its own or
   another, as AmigaOS 3.2.3 does, and the program's PropInfo becomes the
   copy's, with the `AUTOKNOB` given, and its knob Image takes the copy's
   place, and size where both have `AUTOKNOB`, also where Intuition worked
   out the same values again. For a gadget on no window the five values go
   into its PropInfo alone, where 3.2.3 also works out the rest of the
   PropInfo and draws the gadget into the window given. Without a window
   AROS's Intuition changes nothing, and neither does the bridge; AmigaOS
   3.2.3 hangs there. A program that reads the pots while the user drags
   the knob without messages (`GACT_FOLLOWMOUSE` sends them) sees them at
   the next of these passes, not as the knob moves.

## RastPorts and Fonts

The RastPort of a window or a screen is a 68k RastPort in the guest arena
(`kaffeine_rastport.c`), registered as type 3 for the native RastPort behind
it. `OpenWindow()` makes one of its own for each window, and `OpenScreen()`
and `LockPubScreen()` fill the one inside the 68k Screen.

```plain
m68k: SetAPen(rp, 2); rp->cp_x = 10; Draw(rp, 50, 10)
  → Kaffeine_RastPortIn(): the fields that a program may write go to the
    native RastPort: Mask, FgPen, BgPen, AOlPen, DrawMode, AreaPtSz,
    linpatcnt, Flags, LinePtrn, cp_x, cp_y, PenWidth, PenHeight,
    AlgoStyle, TxSpacing
  → the native call
  → ctx->after_thunk, run by the dispatcher right after the thunk:
    Kaffeine_RastPortOut() copies those fields back with the minterms,
    TxFlags, TxHeight, TxWidth, TxBaseline and the font as a 68k TextFont
```

- `resolve_rp()` of the generated thunks takes the same path, so a call
  such as `PolyDraw()` leaves the right pen position in the 68k RastPort,
  and the next call does not move the native pen back. For any other
  address it still returns the native object of another shadow or the guest
  bytes themselves.
- The manual thunks take only the 68k RastPort of a window or a screen;
  any other RastPort, such as one that the program set up with
  `InitRastPort()` or copied from a window, stops the program with rc 20.
- `Layer`, `BitMap`, `AreaPtrn`, `TmpRas`, `AreaInfo`, `GelsInfo` and
  `RP_User` are native pointers: they stay as the program left them, 0 in a
  new RastPort, and never reach native code.
- Fonts have a 52-byte guest TextFont, name and read-only monochrome glyph
  snapshot in one owned allocation, with one identity per native font in
  the 32-entry `ctx->fonts` table. CharData contains the complete YSize *
  Modulo bitmap. CharLoc is BE32; optional CharSpace and CharKern are BE16,
  including signed values. HiChar - LoChar + 2 records include the undefined
  glyph. Dimensions, alignment and allocation sizes are checked first.
  Every published pointer stays in that allocation; native list, reply and
  extension pointers stay private. AROS's native FSF_TAGGED output marker
  is cleared because no guest TextFontExtension is published. Intrinsic
  style bits survive, and tagged input attributes remain supported.
  Existing table-less objects keep their metadata-only representation.
  Color font opens and partial bitmap objects are refused.
  OpenFont and OpenDiskFont share identity and counts. CloseFont balances
  the native open; the final native close and snapshot release wait while
  a guest or native RastPort shows it or a pin retains it. The font a
  window or screen came with stays for restoring rp->Font. Native UI
  cleanup precedes terminal font cleanup, which closes unfinished owned
  opens and releases snapshots without closing borrowed native opens.
  Released identities no longer reach SetFont. Guest edits of snapshot
  glyphs do not change the native font used for native drawing.
- `TextExtent()` and `TextFit()` write the native result to the 68k
  TextExtent field by field; `TextFit()` also reads a 68k constraint. The
  text calls check UWORD(count) characters, because AROS reads a WORD count
  that way, and `TextFit()` checks every character from `string` on, a WORD
  `strDirection` apart.
- On a hi- or truecolor screen AROS's `ReadPixel()` gives the low byte of
  the pixel rather than its pen; the bridge passes that value on.
- `CloseWindow()` frees the 68k Window, its RastPort, its IDCMP port and its
  68k messages; `CloseScreen()` forgets the RastPort and the ViewPort inside the
  68k Screen once native Intuition closes it, and the 68k Screen of a public
  screen that the program locked forgets them when it goes.

## Public Screens

`kaffeine_screens.c` gives 68k programs the screens of `LockPubScreen()`:

1. Each native screen that the program locks has one 68k Screen of 346
   bytes in guest memory, followed by its 68k TextAttr and the font name,
   registered as type 13. Every `LockPubScreen()` of that screen returns
   it, so programs can compare the pointers, and fills in again what
   programs read: LeftEdge, TopEdge, Width, Height, MouseY, MouseX, Flags,
   BarHeight, BarVBorder, BarHBorder, MenuVBorder, MenuHBorder, WBorTop,
   WBorLeft, WBorRight, WBorBottom, Font, the display fields of the
   ViewPort (DWidth, DHeight, DxOffset, DyOffset, Modes, SpritePriorities,
   ExtendedModes), BytesPerRow, Rows, Flags and Depth of the BitMap, which
   AROS's `OpenScreen()` fills into its obsolete BitMap, DetailPen,
   BlockPen and SaveColor0. The TextAttr has no tags, and its name is the
   one of the 68k TextFont of the RastPort, as AROS's `AskFont()` points it
   into the TextFont, or else a copy after the TextAttr, written once.
2. The embedded ViewPort is registered as type 6 for the native one, as
   `OpenScreen()` does, so that `GetVPModeID()` and the other generated
   thunks take `&scr->ViewPort`; the embedded RastPort is a 68k RastPort
   (type 3), whose screen font stays for the program as the font of a
   window does; the bridge holds that native font open until the end of the
   program, so that the 68k TextFont, and the name in a copy of the
   TextAttr that the program keeps, never stand for a font that has gone.
   It opens the font with the attributes that `AskFont()` gives for it, as
   AROS's `OpenScreen()` makes the screen's TextAttr, in a TTextAttr
   without tags, since `ExtendFont()` marks the font `FSF_TAGGED`, and
   before the 68k TextFont is made: when `OpenFont()` finds another font or
   none, or the table of held fonts is full, there is no 68k Screen, and a
   68k Screen that fails later gives the font back. That table is as large
   as the one of the 68k TextFonts, and each held font has a 68k TextFont
   that stays, so it fills only with it. Without the 68k TextFont there is
   no 68k Screen either. Native pointers are not shown: NextScreen,
   FirstWindow, Title, DefaultTitle, the ColorMap and RasInfo of the
   ViewPort, the planes of the BitMap, the LayerInfo, FirstGadget,
   BarLayer, ExtData and UserData stay 0.
3. The 68k Screen stays while the program holds a lock on the screen, has
   a window there or holds the list of public screens, and goes with the
   last of them: `Kaffeine_WindowDelete()` calls `ctx->screen_release` once
   the 68k Window has gone. `WScreen` of a window therefore stays valid
   after the program unlocked the screen, as on AmigaOS, and the guest
   memory comes back afterwards. AROS's `CloseScreen()` refuses a public
   screen with visitors or windows and waits for the list lock, so a 68k
   Screen never stands for a screen that has gone. A screen that the
   program opened keeps the 68k Screen of `OpenScreen()`, which a lock
   does not fill in again.
4. `UnlockPubScreen()` gives Intuition only locks that the program took,
   since AROS counts down whatever it gets, and finds a name in the list as
   AROS does, without regard to the case of ASCII letters. The locks and
   the list lock that the program leaves go back to Intuition at its end,
   in the task that ran it (`Kaffeine_ScreensCleanup()`), which also closes
   the fonts that the bridge held.
5. `LockPubScreenList()` gives a 68k copy of the list in one block, made
   under the lock and filled in again in place at each nested lock;
   psn_Screen is the program's 68k Screen of a screen, or 0 for one that it
   has none of, which the NDK says cannot be used safely in a copy anyway.
   `NextPubScreen()` walks the native list itself, since AROS's reads
   through NULL for a screen that is not public, and returns 0 after the
   last screen, as AROS does; `GetDefaultPubScreen()` writes the name as
   Intuition gives it. Both need room in guest memory only for the name
   and its NUL.
6. A table of 16 screens (`ctx->pubscreens`) holds them, and one of 32
   fonts (`ctx->pubscreen_fonts`) their held fonts; a lock that finds no
   room, no memory, no three free shadow entries or a font that cannot be
   held, and the lock after 32767 locks of one screen, return 0 and give the
   native lock back at once.

## Custom Chip Emulation

All accesses to `$DFF000–$DFFFFF` are intercepted in `AROSMoira::read8/read16/write8/write16`.

`CUSTOM_MASK` compares all 32 address bits, as the 68EC040 model decodes
them: the registers appear once, and `$1DFF000` in the 32 MB arena is
ordinary memory. The heap reserves the 4 KB of the window (see Heap
Allocator), so no allocation lies under the registers.

### Read Registers

| Offset | Name | Value |
|--------|------|-------|
| 0x002 | DMACONR | 0 |
| 0x004 | VPOSR | `(beam_lof << 15) \| ((beam_v >> 8) & 1)` |
| 0x006 | VHPOSR | `((beam_v & 0xFF) << 8) \| beam_h` |
| 0x010 | ADKCONR | 0 |
| 0x016 | POTGOR | 0xFF00 (no buttons pressed) |
| 0x01C | INTREQR | 0 |
| 0x01E | INTENA | 0 |
| other | — | 0 |

Every read calls `beamAdvance(2)` — 2 color clocks per register access.

### Write Registers

| Offset | Name | Effect |
|--------|------|--------|
| 0x02A | VPOSW | Sets beam_lof from bit 15, merges V8 into beam_v |
| 0x02C | VHPOSW | Sets beam_v[7:0] and beam_h |
| other | — | Silently ignored |

Byte writes (`write8`) to the custom chip range are silently ignored.

### Beam Counter

PAL timing: 313 lines × 227 color clocks per line. `beamAdvance(ticks)` adds
ticks to beam_h, wraps at 227 (incrementing beam_v), wraps beam_v at 313
(toggling beam_lof for interlace). The 2-tick-per-read value approximates a
real 68000 register read (~4 cycles = 2 color clocks at 7.09 MHz / 3.546 MHz).

## Thunk Generator Toolchain

Two generators exist:

### kaffeine_thunkgen.py (production)

- **Input**: Amiga FD files (`*_lib.fd`) + NDK clib proto headers
- **Output**: Multi-library C source with master registration table
- **Features**: Full type classification (shadow/string/taglist/input_struct/scalar),
  shadow object support, return value wrapping, extensive skip lists (~70 libraries,
  ~160 functions, ~300 void functions)
- **Proto parsing**: Reads NDK `*_protos.h` for accurate C types; falls back to
  heuristics (arg name, register class) when no proto available

### tools/m68kthunkgen/m68kthunkgen.py (simplified)

- **Input**: AROS `.conf` files
- **Output**: Single-library C source
- **Features**: Basic pointer-vs-scalar classification only. No shadow objects,
  no taglist translation, no struct layout awareness.
- **Use case**: Generating thunks from AROS's own `.conf` format (which already
  contains type + register info)

## Test Infrastructure

### Host-Side Tests (no AROS required)

| Test | File | What It Tests |
|------|------|--------------|
| t_ctx | test_kaffeine.cpp | Context create/destroy |
| t_heap | test_kaffeine.cpp | Heap alloc/free/reuse |
| t_stress | test_kaffeine.cpp | 1000 × 64B alloc, free all, 100KB alloc |
| t_custom_window | test_kaffeine.cpp | The reserved custom chip window: the two free intervals, refused `AllocAbs()` ranges, exhaustion, `MEMF_TOTAL` and resizes |
| (Exec memory) | test_exec_memory.cpp | Literal memory LVOs, clear/reuse, free/largest/total accounting, pool ownership/deletion, invalid pointers, corrupt metadata, native allocation failure and register preservation |
| t_endian | test_kaffeine.cpp | Big-endian read16/write16/read32/write32 |
| t_xlat | test_kaffeine.cpp | Pointer translation round-trip |
| t_libbase | test_kaffeine.cpp | Fake lib base setup, A-line opcode verification |
| t_exec_moveq_rts | test_moira_exec.cpp | MOVEQ #42,D0; RTS → result 42 |
| t_exec_add | test_moira_exec.cpp | MOVEQ + ADDI → result 42 |
| t_exec_loop | test_moira_exec.cpp | Loop summing 1..10 → result 55 |
| t_exec_memory_access | test_moira_exec.cpp | MOVE.L store/load round-trip |
| (integration) | test_hello_thunks.cpp | Full A-line trap → thunk dispatch → return |

### AROS-Side Test

`test_kaffeine.c` — Opens `kaffeine.library`, writes an embedded m68k hunk binary
(MOVEQ #42,D0; RTS) to `RAM:`, calls `RunFile`, and verifies the result is 42.

### Test Gaps

- Custom chip tests cover POTGOR, VHPOSW and the single window at `$DFF000`
  (`test_guest_bounds`) and VHPOSR advancing on AROS (`exec_memory`), not the
  wrap of the beam counter
- No tests for Supervisor() dispatch path
- No tests for generated thunk fallback path
- No tests for unimplemented LVO handling
- No tests for hunk loader (LoadHunksFromMemory)
- Heap alignment, exhaustion and guest pool ownership are covered by `test_exec_memory`
- Longword bus accesses crossing the arena or 2^32 are split by Moira;
  callback bounds are tested, but whole-access refusal needs a backend change
- Makefile builds and runs all host tests (`test_kaffeine`, `test_moira_exec`, `test_hello_thunks`) via `make test`

## Known Bugs

No known bugs at this time.


## Draft DOS file handles and read packets on 2026-10-26

Open, Input and Output publish guest FileHandle BPTRs, with a 52-byte
classic view and a private handler identity. Handle-taking manual thunks
resolve them to the full native BPTR; locks keep their separate ABI.
Native function, handler and buffering pointers do not escape. Native
buffer data has a checked guest mirror; mixing guest-managed and native
buffering is explicitly refused. There are 32 active file associations.
Close releases the active slot and mirror; 88-byte guest identities stay
quarantined in heap-bounded slabs until teardown so stale tokens cannot
become later files. Cleanup closes owned files and preserves borrowed streams.

The ACTION_READ adapter validates a contiguous 68-byte StandardPacket,
its key, buffer, reply queue and aliases before native Read. It stages the
native result, copies only the returned bytes and preserves caller IoErr.
The DOS reply has type 5 and dp_Port becomes the guest handler; generic
ReplyMsg instead uses type 7, or type 6 with no reply port. Other packet
actions and DOS packet entry points remain unsupported. The final target candidate
matches the measured 26 message checks, 28 packet checks and 20 raw
observations in Shell and Run. Personal Paint completes repeated packet
reads and replies, then stops at a contained CPU callback memory-access
fault at PC 0x00c98d92 in both modes. The failing address and callback still
need classification; the PC alone does not establish the cause. Complete
application and hardware operation remain unverified.
