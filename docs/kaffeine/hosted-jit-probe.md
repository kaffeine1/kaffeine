# A hosted AArch64 translation probe

This note records the original hosted experiment. Its translator now lives
in `rom/kaffeine/kaffeine_jit.c` and also powers the opt-in
[experimental library backend](jit-backend.md).

This note records point 4 of the plan of 20 October 2026: a minimal
experiment of a "hosted" Emu68, first on the Mac (macOS on AArch64) and
only later inside AROS. I wanted to know whether the mechanics that a
translating backend of `kaffeine.library` needs work in an ordinary host
process, behind the backend interface that Moira uses today, before
deciding how to bring Emu68 itself in.

The probe is a small 68k to AArch64 block translator that I wrote for this
experiment: `rom/kaffeine/kaffeine_jit.c`, with its
driver `test_hosted_jit.cpp`. **It contains no Emu68 code**. The original probe covers a subset of the
68k instruction set and is built by the host suite and a focused native
AROS probe; library mode adds bounded slices and a guest-task factory. It implements `struct KaffeineCPUOps`,
`flush_code()` included, and runs through the production
`Kaffeine_RunCPU()`, `Kaffeine_DispatchLibraryCall()` and
`Kaffeine_CallGuest()`, with the production `CallHookPkt()`, `RawDoFmt()`,
`CacheClearU()` and `CacheClearE()` thunks. The production Moira adapter
runs the same programs as the reference.

## Results

| Point | Result | Evidence in `test_hosted_jit` |
| --- | --- | --- |
| 1. Executable memory | Works: `MAP_JIT`, `pthread_jit_write_protect_np()` per thread, `sys_icache_invalidate()` after each block; on Linux `mprotect()` W^X and `__builtin___clear_cache()` | `exec-memory`; every other group runs translated code |
| 2. Big-endian guest memory | Works: 32-bit guest addresses as offsets from a 64-bit host base above 4 GB, `REV`/`REV16` on every word and long, misaligned accesses, the end of the arena and accesses outside it as Moira does them | `endian`, and memory compared with Moira in every random program |
| 3. A translated 68k program | Works: a checksum loop, a bubble sort with a subroutine and `MOVEM`, 20000 random programs equal to Moira | `program`, `random-programs` |
| 4. Library call through the bridge dispatcher | Works: the A-line leaves translated code, the production dispatcher pops the return address and runs the thunk with registers the accessors see, and execution resumes in translated code at the return address, with the condition codes the thunk left | `library-calls` |
| 5. Reentrant callbacks | Works: dispatcher, 68k hook, dispatcher again, 16 levels, the 17th stops with 20; `RawDoFmt()` calling a 68k `PutChProc` that calls `CallHookPkt()` | `callback-depth`, `callback-registers`, `callback-format`, `callback-faults` |

Measured with `make bench-hosted-jit` on an Apple M4 Pro (macOS 27.0.1),
with Moira and the probe in the same binary built with `-O2`. The machine
was shared (load average 11 to 13, another session running QEMU), so the
figures are medians of three runs, with the range of the three:

| Program | Moira, clang 21 | Probe, clang 21 | Moira, GCC 15 | Probe, GCC 15 |
| --- | --- | --- | --- | --- |
| Checksum loop, ns per 68k instruction | 12.14 (11.82-12.37) | 0.29 (0.28-0.30) | 9.94 (9.05-10.02) | 0.30 (0.27-0.31) |
| JSR to a thunk and back, ns | 107.4 (102.0-110.9) | 17.2 (15.2-19.0) | 96.2 (94.9-97.3) | 15.4 (14.5-16.7) |
| `CallHookPkt()` of a 68k hook, ns | 264.9 (261.2-276.3) | 82.3 (80.5-88.2) | 251.2 (245.4-271.3) | 90.0 (84.4-90.7) |
| 16 nested callbacks, us per chain | 4.10 (3.76-9.10) | 1.69 (1.53-5.81) | 3.83 (3.72-4.22) | 1.74 (1.62-1.80) |

The loop is the best case: five instructions in one block that branches
back to itself, with the registers in host registers throughout. It runs
33 to 42 times faster than Moira; a library call, which leaves translated
code and enters it again, about 6 times, a callback, which also goes
through `Kaffeine_CallGuest()`, about 3 times, and a chain of 16 nested
callbacks about 2.3 times. Without the inline memory checks
(`-DHJ_MEASURE_UNCHECKED`, for this measurement only) the loop takes 0.22
ns per instruction (0.18-0.23): the checks cost about a quarter there. In
the timing programs a block takes about 0.8 us to translate, publication
included, and the probe emits 6.0 to 6.8 AArch64 words per 68k
instruction; in the random programs about 9, slow paths and exits
included. None of this predicts a Raspberry Pi, or AROS.

The table predates the check of the second word of a long against the
custom chip page, two more instructions on every long access and half a
word more per 68k instruction. Against the old check, with clang and five
alternate runs of each on the same machine (medians), the checksum loop
stays within the noise (0.22 against 0.23 ns per instruction), a library
call takes 13.6 ns instead of 12.8, a callback 67.3 instead of 63.8, and a
chain of 16 nested callbacks 1.31 us both ways.

The probe stops where the subset stops: any other instruction ends the
program with return code 20 and a message, as the Moira adapter does with
Line-F. The limits are listed below.

## What Emu68 assumes, and why I did not start from it

I studied Emu68 out of tree, at
[`af63580`](https://github.com/michalsc/Emu68/tree/af63580f66ac79d9fefb790997f3012df01a5d59)
(23 October 2026, four commits after the revision reviewed in
[LICENSING.md](LICENSING.md)). Its translator is about 30000 lines in
`src/M68k_*.c`, and it is written for bare metal:

- It runs the AArch64 core big-endian (`-mbig-endian` in `CMakeLists.txt`)
  and maps 68k addresses to host addresses one to one. The emitters load
  and store through the 68k address registers directly, with pre- and
  post-indexed addressing on them (`src/M68k_EA.c`); the 68k translator has
  about 550 load and store emission sites. Hosted in AROS, every access
  needs a base register, a byte swap and a bounds check instead.
- It keeps the 68k state in fixed host registers for the whole run, as
  global register variables of the C code: the 68k PC in `w18`, D0-D7 in
  `x19`-`x26`, A0-A4 in `x13`-`x17`, A5-A7 in `x27`-`x29`, SR, FPCR and the
  stack pointers in `v19` and `v21`, the context in `v20`
  (`include/M68k.h`, `src/ExecutionLoop.c`,
  `docs/internals/RegisterMapping.md`). `x18` is the platform register of
  AAPCS64, which Darwin reserves; elsewhere it is a temporary that every
  call may change, `x29` must stay a valid frame pointer on Darwin, and
  none of this survives a call into a native library compiled for
  AAPCS64.
- It takes exceptions, Line-A included, in 68k terms: the generated code
  calls `M68K_Exception`, which builds the frame on the 68k stack and loads
  the PC from the vector table (`src/M68k_Exception.c`,
  `src/M68k_ExceptionEntry.c`). The bridge needs Line-A to leave translated
  code for the native dispatcher.
- On the 68040 cache instructions (`CINV`, `CPUSH`), which the
  `CacheClearU()` of a 68040 Kickstart executes, it increments an epoch and
  later keeps the units whose 68k code still has the same CRC32 (`src/M68k_LINEF.c`,
  `M68K_VerifyUnit()` in `src/M68k_Translator.c`).

Bringing Emu68 in would therefore be a port, not a build option: memory
access, the register convention, the exits and the exception paths change
in most of those files. Emu68 is MPL 2.0, file by file; LICENSING.md
proposes to keep any imported Emu68 files separate from the APL files of
AROS. I copied nothing, and whether to import and port Emu68 files is a
decision for later. The probe answers the question first: what the hosted
side needs, with code that is APL like the rest of `kaffeine`.

## Design of the probe

### Registers and blocks

Inside translated code D0-D7 live in `w0`-`w7` and A0-A7 in `w8`-`w15`.
`x19` holds the state, `x20` the host address of guest memory, `w21` the
flags, `w22` a budget, `w24` the access limit, and `x16`, `x17`, `x23`,
`x25`-`x28` are scratch. `x18` is never used, so the same code suits
Darwin. An enter trampoline, called from C as an AAPCS64 function, saves
`x19`-`x30`, loads the 68k registers from the state and branches to a
block; an exit trampoline stores them back and returns the exit reason. So the registers are in the state, where the
accessors of `struct KaffeineCPUOps` read and write them, whenever C code
runs: at every exit and around every slow access.

A block is the straight run of 68k instructions up to the first branch,
`JSR`, `RTS`, A-line or instruction outside the subset, at most 64. A
branch to another block goes through a lookup: one probe of a 4096-entry
table indexed by the guest PC, and a direct `BR` on a hit. A branch back to
the start of the same block stays in it. Both decrement the budget (65535),
so that translated code returns to the C loop now and then; nothing in
the bridge stops a guest asynchronously today, but a future break or timer
would need that return. A miss returns to C, which looks the block up in a
hash table, translates it if needed and enters again.

The flags live in `w21` as X N Z C V in bits 4 to 0, NZCV in the order of
A64 so that `MRS NZCV` and one shift give them; C is stored as the 68k
carry, inverted after `SUBS`. Conditions are one `TBZ`/`TBNZ` or two or
three instructions. A backward pass over each block computes only the
flags that a later instruction of the block reads before another sets
them; all flags are live at the end of a block.

### Guest memory

Every access checks its address inline: a compare and a branch against
`mem_size - 4`, and three instructions for the custom chip page at
`$DFF000`, five for a long, whose second word can be in the page when its
first is not; then `LDR`/`STR` with the base register and the 32-bit guest
address zero-extended (`[x20, wN, UXTW]`) and `REV` or `REV16`. A long load
is 9 instructions where Emu68 needs one. Anything else goes out of line to
a C slow path that does what the Moira adapter does: the custom chip
registers, the end of the arena, a longword as two independent word
accesses, a read of zero and a dropped write outside the arena, and a stop
with return code 20 during a callback, which leaves the block at once.

### Library calls and callbacks

An A-line ends the block with the address of the opcode. The C side calls
`Kaffeine_DispatchLibraryCall()`, which pops the JSR return address and runs
the thunk; on return the probe continues at that address. There is no
exception frame and no RTE: the condition codes a thunk leaves with
`Kaffeine_SetSR()` are simply the live flags, which is what the Moira
adapter reaches by copying the CCR into its stacked frame.

A thunk that calls back into 68k code uses the production
`Kaffeine_CallGuest()`, which calls `execute()` of the probe again. That is
safe because no translated code is on the host stack while C code runs:
the block has already returned through the exit trampoline. Translating,
resetting the code memory or flushing for `CacheClearU()` is therefore
allowed at any depth. A callback ends when its `RTS` reaches the return
token, which no lookup ever finds, so the block returns to the loop of
`Kaffeine_CallGuest()` and its checks.

### Executable memory and guest code that changes

The code memory is 16 MB mapped with `MAP_JIT`. Each block is emitted into
a scratch buffer, then copied with the thread's write protection off
(`pthread_jit_write_protect_np(0)`), and `sys_icache_invalidate()` publishes
it after the protection is back on. On Linux the same window is
`mprotect()` from read and execute to read and write and back, and
`__builtin___clear_cache()`.

A translation of guest code stays in use until the guest flushes, as on a
68040 with its caches enabled: changed code runs the old translation until
`CacheClearU()` or `CacheClearE()`. Both call the backend's `flush_code()`
(#80), as the bridge's loaders of 68k code do while the program runs; in
the probe it drops the lookup table and starts a new epoch. A block of an
older epoch whose guest bytes are unchanged is kept, compared with the
copy taken at translation; the others are translated again.

## Verification

- 146 encodings compared with the words that Apple clang 21 assembles from
  the same instructions, as `HJ_EncoderSelfTest()`, which also runs where
  translated code cannot (the x86_64 job of the CI).
- The groups in the table, each on both backends where both apply, a group
  of deterministic checks for the extended instructions (`extended-insns`),
  and four more: a block cut at 64 instructions whose X flag the next block
  reads, a loop of 200000 back edges that uses up the budget three times,
  40000 chained blocks inside a callback, with the code memory emptied
  and a `CacheClearU()` while the caller waits in the dispatcher, and a
  16 MB arena with longs across both ends of the custom chip page.
- `bitfield-boundaries` compares all eight memory bitfield operations at
  the last one to four arena bytes with Moira, including its longword
  access convention and untouched host guards. Five-byte callback cases
  also check that a completed leading longword store survives a fault on
  the fifth byte and that execution does not resume the caller.
- 20000 random programs compared with Moira: registers, SR, the scratch
  memory, the stack and the beam counter. The generator keeps to the
  subset and to what both backends define the same way: pointers that stay
  in a scratch area, accesses at the end of the arena and outside it, reads
  of `VPOSR`/`VHPOSR`, forward `Bcc.S`, `Bcc.W`, `Bcc.L`, `BRA` and `JMP` on
  every condition, `DBcc` loops, subroutine calls with `BSR.W`, `BSR.L`,
  `JSR (d16,PC)`, `JSR` absolute, `JSR (A4)` and `JSR 8(A5)` returning with
  `RTS`, `RTR` or `RTD`, `LINK.W`, `LINK.L`, `UNLK`, `PEA`, `MOVEM.W` and
  `MOVEM.L`, `BTST`/`BCHG`/`BCLR`/`BSET`, `ADDX`/`SUBX`/`NEGX`, `CMPM`,
  `EXG`, `EXTB.L`, `MULU`/`MULS` `.W` and `.L`, `DIVU.L`/`DIVS.L`/`DIVUL.L`/
  `DIVSL.L`, all eight 68020 bitfield instructions (`BFTST`, `BFEXTU`,
  `BFCHG`, `BFEXTS`, `BFCLR`, `BFFFO`, `BFSET`, `BFINS`), `TAS`, `MOVEP.W`
  and `MOVEP.L`, `CAS.B`/`CAS.W`/`CAS.L`, all eight shifts and
  rotates across `.B`, `.W`, `.L` and `<ea>.W`, `Scc`, and the SR and CCR
  moves and immediate logic operations.
- 32 mutants of the translator, each one caught: the borrow of `SUB` and
  `CMP`, X from the wrong bit, `GE` reading Z, `HI` without C, byte inserts
  too wide, the byte step of A7, words not swapped on load, the second word
  of a long at the end of the arena, `DBcc` on a byte, the carries of `LSR`
  and `ROL`, `MOVEA.W` and `CMPA.W` without sign extension, the flags of
  byte and word logic, X dead at the end of a block, the beam step, the
  order of `MOVEM`, `Scc` inverted, `ADDQ #8` as 0, the index scale, the
  CCR swap, a back edge off by one or without the budget, `NEG` as `ADD`,
  the flags of `EXT.W`, the custom chip page and the end of the arena on
  the inline path, a long with only its second or only its first word in
  that page, the hash table kept after a reset, the lookup table
  kept after a flush, and blocks always kept after a flush.
- Apple clang 21 with ASan and UBSan on macOS; GCC 15.2 on macOS, plain
  and with UBSan in trap mode (Homebrew's GCC has no sanitizer runtimes
  there); GCC 14.4 with ASan and UBSan on Linux AArch64 in Docker, where
  the code memory goes through `mprotect()`. Neither compiler warns
  (`kaffeine_jit.c` builds with `-Wall -Wextra -Werror`). On x86_64 Linux, as
  in the CI, the encoder and translation checks pass and the rest is
  skipped.

## Limits

- The subset: `MOVEQ`, `MOVE`, `MOVEA`, `LEA`, `PEA`, `LINK.W`, `LINK.L`,
  `UNLK`, `ADD`, `SUB`, `AND`, `OR`, `EOR`, `CMP` and their address,
  immediate (including PC-relative `CMPI`) and quick forms, `CMPM`, `ADDX`,
  `SUBX`, `NEGX`, `MULU.W`, `MULS.W`, `MULU.L`, `MULS.L` (32-bit and 64-bit
  `Dh:Dl`), `DIVU.L`, `DIVS.L`, `DIVUL.L`, `DIVSL.L` (32-bit `Dq` and 64-bit
  `Dr:Dq`, stopping with 20 on divide by zero), `BFTST`, `BFEXTU`, `BFCHG`,
  `BFEXTS`, `BFCLR`, `BFFFO`, `BFSET`, `BFINS`, `CAS.B`, `CAS.W`, `CAS.L`,
  `MOVEP.W`, `MOVEP.L`, `TAS`, `BTST`, `BCHG`, `BCLR`, `BSET`, `CLR`, `TST`
  (including 68020 address register, immediate and PC-relative modes),
  `NEG`, `NOT`, `SWAP`, `EXT.W`, `EXT.L`, `EXTB.L`, `EXG`,
  `ASL`/`ASR`/`LSL`/`LSR`/`ROL`/`ROR`/`ROXL`/`ROXR` (`.B`, `.W`, `.L` with
  immediate or register counts, and 1-bit `<ea>.W` in memory), `Scc`, `Bcc`,
  `BSR`, `DBcc`, `JSR`, `JMP`, `RTS`, `RTR`, `RTD`, `MOVEM.W`, `MOVEM.L`,
  `MOVE` from and to `SR` and `CCR`, `ORI`/`ANDI`/`EORI` to `CCR` and `SR`,
  `NOP` and A-line, with all addressing modes but the full extension word.
  Everything else, `ILLEGAL` and Line-F stop with 20 and a message.
- No exception frames at all: no `TRAP`, `CHK`, address error, privilege
  violation or trace, and no `Supervisor()`, which stops with 20.
- Probe mode has one A7: changing S or M does not switch stacks. Library
  mode refuses full SR writes and native mode/trace changes explicitly.
- The sentinel stops the program when the PC reaches it, not when data is
  read there, and the stack pointer of a callback is checked between
  blocks rather than between instructions.
- One thread, one code memory per backend, emptied when it fills up; no
  patching of block exits; one probe of the lookup table.
- No FPU and no MMU instructions.
- The standalone probe and the library backend have separate target tests.
  See the [library mode contract and results](jit-backend.md).

## What a translating backend needs from the bridge

- The custom chip emulation as C code shared by the backends; the probe
  repeats the beam counter of `kaffeine_moira.cpp`.
- Inside AROS: AROS AArch64 runs tasks at EL1t and leaves ordinary RAM
  executable (only device memory is PXN/UXN, in
  `arch/aarch64-native/kernel/mmu.c`), so the code memory can be an
  allocation (`MEMF_EXECUTABLE` exists for the future) without `MAP_JIT`.
  `CacheClearE(start, length, CACRF_ClearD | CACRF_ClearI)` reaches
  `cache_clear_e()` in `arch/aarch64-native/kernel/syscall.c` through an
  SVC; at EL1 the DC and IC instructions could also be issued directly.
  The AROS GCC target does not reserve `x18`, the kernel saves it with the
  other registers and the TLS pointer is in `TPIDR_EL1`, so on AROS alone
  `x18` would be free; the probe leaves it alone for Darwin.
- Cheaper memory checks: a reserved window with guard pages would remove
  the inline ones, but it needs a fault handler that resumes the guest,
  which I have not looked for in AROS.

## Possible next steps

- Extend the instruction subset and measure library execution. Any future
  interpreter fallback needs an explicit complete CPU-state contract,
  including stack banks, exceptions, prefetch, control registers and the
  relevant FPU state. `KaffeineCPUOps` alone does not provide that transfer.
- Porting Emu68's translator instead, for its coverage and its FPU: the
  memory accesses, the register convention, the exits and the exceptions
  of the hosted side would follow this probe, in MPL files kept apart.
- Broader native AROS execution, physical Raspberry Pi cache validation,
  and integration with the library and its guest-task factory. The focused
  AROS VM probe below publishes code through `CacheClearE()`.

## Native AROS AArch64 probe

`developer/debug/test/kaffeine_native/jit_probe.c` links this translator with
the production CPU loop, dispatcher and callback primitive. On standalone
AROS AArch64 code memory comes from `AllocMem` with `MEMF_EXECUTABLE`;
`CacheClearE(..., CACRF_ClearD | CACRF_ClearI)` publishes each emitted block.
The standalone-flavour check keeps this allocation path out of hosted AROS,
whose executable-memory protection differs.

The initial AROS ARM/QEMU run passed 38 checks in both direct Shell and
`Run` modes: encoder words, executable allocation, translated arithmetic,
big-endian memory, a native thunk, a nested translated callback, invalidated
and retranslated guest code, and explicit unsupported-instruction failure.
The callback used six translated blocks and nested execution; the code
mutation case used seven blocks and one invalidation.

After the DIV.L, bitfield, CAS, MOVEP, TAS and RTD extension, native AROS
AArch64 in QEMU passed the 38 regression checks and a separate 490-check
instruction probe in both Shell and `Run` modes. The latter covers 432
programs plus eight callback bitfield boundary cases, checking completed
prefix stores when the trailing byte faults. Moira generated the expected
program results on the host; the native executable does not link Moira.
On macOS arm64 the full hosted JIT suite passed 3,778 checks, followed by
20,000 additional randomized differential programs. Focused ASan/UBSan runs
passed 304 boundary checks, 164 extended-instruction checks and 256
randomized programs.

These are separate results from the host timings above. No native speedup
or physical-target cache result is claimed. Moira remains the default library backend. The opt-in JIT library mode
adds guest-task integration; full instruction coverage still needs work. See the [native probe instructions](../../developer/debug/test/kaffeine_native/README.md).

## Reproducing

From the repository root:

```sh
git submodule update --init rom/kaffeine/Moira
make -C developer/debug/test/kaffeine_host test_hosted_jit
developer/debug/test/kaffeine_host/build/test_hosted_jit
developer/debug/test/kaffeine_host/build/test_hosted_jit random-programs --programs=20000
make -C developer/debug/test/kaffeine_host BUILD_DIR="$PWD/build-jit-o2" \
    CFLAGS=-O2 CXXFLAGS=-O2 bench-hosted-jit
```

`test_hosted_jit` is part of `make test`. On a host other than AArch64
Darwin or Linux only the encoder and translation checks run, and the other
groups are reported as skipped.
