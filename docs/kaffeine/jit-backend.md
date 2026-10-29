# Experimental Kaffeine JIT backend

`kaffeine.library` defaults to Moira. On standalone AROS AArch64 it can
also execute 68k applications with the Kaffeine block translator:

```text
Set KAFFEINE_CPU jit
my-68k-program
Run my-68k-program
Unset KAFFEINE_CPU
```

`KAFFEINE_CPU=moira` explicitly selects the interpreter. A local variable
takes precedence over a global `ENV:` variable, so `SetEnv KAFFEINE_CPU
jit` also selects the JIT for processes that have no local override.
An unknown value, an unavailable JIT host or an allocation failure returns
20. Backend selection happens once for each application run. The debug
log identifies the selected backend and reports translation counters.

## Execution and lifetime

The translator lives in `rom/kaffeine/kaffeine_jit.c`. Host differential
tests and the native standalone probe compile this same source. Its
internal `HJ_` names retain the prefix of the initial hosted experiment.

The root owns its CPU until the application returns. Each cooperative
guest task gets its own CPU, registers and translation cache over the
shared guest arena. A CPU slice executes at most one translated block
(up to 64 guest instructions), returning at branches and native calls so
the existing scheduler and GUI polling can run. Native thunks and nested
68k callbacks use the production dispatcher and guest-call primitive.

`CacheClearU`, `CacheClearE` and loaders invalidate translated code through
the CPU interface. A generation shared by the arena makes every other JIT
CPU invalidate its cache before its next slice. No translated code is
running when a native thunk invalidates or replaces code memory.
Children are destroyed before the root CPU and guest arena are released.

## Supported model

The [instruction subset](hosted-jit-probe.md#limits) is still incomplete.
Unsupported instructions, exceptions, FPU/MMU operations and `Supervisor`
redirection stop with 20 and a diagnostic. The library backend also
refuses full SR writes, stack-bank changes and trace mode before executing
them. CCR instructions and native updates of condition codes remain
available. There is no automatic switch to Moira during execution.

Code memory is allocated with `MEMF_EXECUTABLE` and published with
`CacheClearE(CACRF_ClearD | CACRF_ClearI)` on standalone AROS AArch64.
Hosted AROS is excluded from that allocation path. Other library targets
can continue using Moira.

## Validation

On AROS AArch64 in `raspi3b` QEMU, the actual library running with
`KAFFEINE_CPU=jit` passed the relocated HUNK/DATA/BSS program, all 17
`hooks` checks, 12 `exec_guest_tasks` checks, 120 `gfx_window` checks and
7 `exec_guest_task_window` checks, each from both Shell and `Run`. UART
output identified the JIT and reported translated blocks for every
execution. The native `library_smoke` client also passed its 12 checks in
both modes, including both library APIs, expected refusals and a separate
Moira run. These are application runs through the library, separate from
the 38-check native standalone probe, which passed again in both modes.

Host coverage includes nested callbacks, separate parent/child CPUs,
shared-code invalidation in both directions, scheduler cleanup and
explicit rejection of unsupported SR operations. Moira remains the
reference for the existing instruction regression suite.

`DIVU.W` and `DIVS.W` additionally use an independent widened-integer oracle
for arithmetic and defined flags. Their native standalone probe passed
22 checks in both Shell and `Run` on AROS AArch64. Quotient overflow keeps
the destination unchanged and sets V; division by zero stops with 20
because exception delivery remains unsupported. See the
[word-division test notes](hosted-jit-probe.md#verification) for the known
flag differences in the pinned reference interpreter.

The original 68k Personal Paint 7.2.1.0 also reached its editor with the JIT
on AROS AArch64 in QEMU, from both Shell and `Run`, using the CPU blitting
preferences of the existing Moira demonstration. A line was drawn in the
first run, and both runs exited normally with return code 0. UART identified
the JIT and reported translated blocks for each run.

The JIT save/reload follow-up on 2026-10-29 saved an original rectangle and
line as a 320×200 IFF/ILBM image with eight planes and 256 palette entries.
After adding a visible extra rectangle, loading the saved file restored the
original drawing. An independent FFmpeg decode matched the captured drawing
and the reloaded canvas pixel for pixel in the comparison area, excluding
the toolbar, title and mouse pointer. The saved file is 9,386 bytes, SHA-256
`3a9c33f0e559b84907044e0beb318acb7d8e8307565960f7982f61ed19fba876`.
The run used the original application executable and the same CPU blitting
preferences; AROS screen switching was needed to return to the editor after
the load dialog.

This path required generic classic Requester conversion and owned-layer
clip Regions. On AROS AArch64, the JIT passed 4 `gfx_requester_init`, 23
`gfx_classic_request`, 22 `gfx_window_clip`, 13 `gfx_request` and 46
`gfx_private_regions` checks in each of Shell and `Run`. The 22 clipping
checks also passed as a native ARM program in both modes, independently of
the 68k bridge.

A separate JIT run saved a 1024×768 image with eight palette entries; its
independent decode matched the entire visible canvas. Reopening that larger
file still reports “The image is too large”. Broader editor use, full
instruction coverage, native performance and physical Raspberry Pi cache
behavior remain unverified. The published Personal Paint video continues
to show Moira.

For repeatable target runs, pass `--cpu jit` or `--cpu moira` to
`developer/debug/test/kaffeine_target/run-raspi-qemu.py`; it selects the
backend in both generated launch scripts.
