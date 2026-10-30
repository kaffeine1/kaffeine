# Bounded native backfill hooks

`InstallLayerHook()` can install a supported classic Hook on an owned Window's
WLayer. The driver compiles the Hook into a finite paint plan before publishing
a native Hook. A callback from another native task evaluates that plan and
replays typed drawing operations; it never enters a guest CPU, Moira, JIT or
guest library dispatcher. Private-layer synchronous guest Hooks retain their
separate existing path.

The [compiler API](../../rom/kaffeine/kaffeine_paint_compile.h),
[native adapter](../../rom/kaffeine/kaffeine_paint_hook.c) and
[layer integration](../../rom/kaffeine/kaffeine_layers.c) define the contract.

## Accepted paint programs

The classic entry ABI is A0=20-byte Hook, A1=20-byte BackFillMessage,
A2=100-byte RastPort and A7=private abstract stack. Compilation follows bounded
68000 register/stack operations, static local calls, constant-count DBRA loops
and BRA/BEQ/BNE branches. ADDA/SUBA.W/L require constant operands, preserve CCR,
sign-extend WORDs and reject address wrapping or stack overflow. The
[compiler tests](../../developer/debug/test/kaffeine_paint_host/README.md)
describe the instruction and addressing subset.

| Typed operation | Accepted effect |
| --- | --- |
| graphics `SetAPen`, LVO -342 | Byte-valued foreground pen on a proven local clone. |
| graphics `SetDrMd`, LVO -354 | Byte-valued drawing mode on that clone. |
| Local Mask store | Byte-valued mask on that clone. |
| graphics `RectFill`, LVO -306 | Original signed-WORD Min/Max expressions within the actual invocation Bounds; reversed endpoints produce no fill. |
| exec `Signal`, LVO -324 | Deferred tail notification to the captured guest owner, using a constant or immutable finite signal mask. |

A clone must prove the original 100 RastPort bytes, with Layer cleared and
supported Mask/setter changes. Pointer/private bytes may be copied but cannot
drive arithmetic or control flow. Replay copies the actual native RastPort and
clears its Layer; classic struct bytes never become a native RastPort.
Unproven clones, nonlocal stores, other APIs and unbounded paths are refused.

External mutable inputs are restricted to their required low byte, such as a
pen. Immutable multibyte cells are equality-checked. A separate 32-bit owner
capability may contain zero or the captured live owner; zero must select a path
without Signal, and the capability cannot control paint arithmetic. General
mutable multibyte controls are refused: even on a uniprocessor (UP) system,
`Forbid()` cannot repair a guest write already preempted halfway through.

| Bound | Current limit |
| --- | --- |
| Installed bindings per Context | 16 |
| Compile proof | 4096 steps, 8 nested local calls, 1024-byte abstract stack |
| Plan | 2048 expression nodes, 512 dependencies, 16 scalar cells, 16 paths |
| Selected replay path | 2 local RastPort clones, 32 commands |

The native profile requires UP Exec and exactly one registered CPU reported by
`kernel.resource`; SMP builds are rejected. It makes no CPU-hotplug guarantee.
The invocation must supply the bound Layer, a managed HIDD BitMap, no AreaPtrn,
and ordered Bounds. Installation on a bridge-clipped window layer or over an
unknown native baseline Hook is refused. Arbitrary coordinates, affine insets,
patterns, raw planar bitmap replay and other drawing APIs remain outside this
profile.

## Callback, owner delivery and lifetime

The calling native task must retain the Layer semaphore through Hook return.
An unlocked invocation is ignored without publishing active work or an event;
a recursive invocation fails before sharing evaluation buffers. The callback
uses preallocated private workspaces. Its bounded UP snapshot copies dependency
values, permitted scalar inputs and native RastPort state under `Forbid()`.
The adapter makes no explicit allocation or wait call in the callback. Drawing
and the native wake happen after `Permit()`.
All selected commands and capabilities are validated before native drawing.

Painting preserves command order. Signals are coalesced and queued only after
paint replay. A failed validation skips replay for that invocation and queues a
fault. The foreign callback signals only the captured driver's private wake bit.
[Owner polling](../../rom/kaffeine/kaffeine_async.h) delivers guest signals
through `TaskOps.send_signal`, or to the exact captured root in a Context with
no scheduler. A retired child never falls back to root. Private owner cookies
prevent numeric Task-address reuse from reviving an old notification.

`TaskOps.can_signal` admits the owner's allocated native bits and supported
standard break capabilities, including CTRL_D; reserved DOS/private bits and
arbitrary unallocated signals are excluded. `owns_signal` retains its stricter
native-port ownership meaning. A child's break signal updates only that
child's virtual received signals.

`LayerOps.signal_mask` and `poll` join the I/O wake provider without changing
`io_signals`. The private layer bit stays hidden from guest signal allocation,
Signal, SetSignal, Wait and port validation. Driver slices and native/cooperative
Wait, WaitPort and supported WaitIO paths poll pending work. DOS brokers never
pump driver-owned events. A deferred fault stops the domain with authoritative
return code 20, including when an enclosing void thunk returns zero.

Code, complete Hook headers, pointer chains, scalar storage and emitted native
signal bits remain leased while installed. Query-only heap, pool, MemList,
port/signal and library Close/Expunge preflights retain dependencies before any
transaction member changes. Unrelated storage and signals remain usable.
Changes to equality dependencies fail validation before replay.

Replacement drains valid old notifications to their original owner before
freeing the old binding. Detachment restores the native baseline under the
Layer semaphore and quiesces callbacks before releasing storage. Task retirement
revokes bindings and pending notifications before broker, CPU, signal or TCB
reuse. Root retirement also runs without a scheduler. Final cleanup retains
the driver, wake bit and arena until native callback storage is releasable.

## Tests

From the repository root, with disposable host output directories:

```sh
python3 developer/debug/test/kaffeine_paint_host/run_tests.py \
  --source-dir "$PWD" --oracle --build-dir /tmp/kaffeine-paint-normal
python3 developer/debug/test/kaffeine_paint_host/run_tests.py \
  --source-dir "$PWD" --oracle --sanitize --build-dir /tmp/kaffeine-paint-san
python3 developer/debug/test/kaffeine_paint_hook_host/run_tests.py \
  --source-dir "$PWD/rom/kaffeine" --build-dir /tmp/kaffeine-paint-hook-normal
python3 developer/debug/test/kaffeine_paint_hook_host/run_tests.py \
  --source-dir "$PWD/rom/kaffeine" --sanitize --build-dir /tmp/kaffeine-paint-hook-san
```

The compiler runner takes a repository root; the native-adapter runner takes
`rom/kaffeine`. `CC`/`CXX` can select host compilers. The compiler suite includes
the original target emitter regression; `--oracle` compares original 68000
programs with pinned Moira. The adapter suite uses a separate host thread and
recording native collaborators for replay, ownership, replacement, retirement,
quiescence, dependency changes and release refusal. The
[main host suite](../../developer/debug/test/kaffeine_host/README.md) also tests
the real scheduler and asynchronous wait/message integration. CI runs both
standalone suites normally and with ASan/UBSan.

Build the original native observer/controller with a matching AROS ARM64 SDK:

```sh
make -C developer/debug/test/kaffeine_target \
  BUILD_DIR="$PWD/build-paint-fixture" NATIVE_CC=aarch64-aros-gcc \
  "$PWD/build-paint-fixture/paint_fixture_native"
```

Copy the executable to a disposable UP AROS ARM64 target and run each backend
separately, one invocation at a time, under an externally enforced deadline:

```text
paint_fixture_native moira
paint_fixture_native jit
```

The [target instructions](../../developer/debug/test/kaffeine_target/README.md#foreign-native-paint-probe)
describe the original HUNK, native foreign invocation, pixel comparison, exact
CTRL_D wake and observer Process retirement checks. Host checks, native target
execution and application GUI acceptance are separate evidence lanes.
