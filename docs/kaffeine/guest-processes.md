# Guest Processes and shared domains

Typed `CreateNewProc()` starts another 68k program in the caller's Context.
Root and child share the same 32-bit guest pointer domain, with separate CPU
state, stacks, Process identities and DOS state. Guest Process addresses are
never native Process pointers.

## Execution and ownership

The [scheduler](../../rom/kaffeine/kaffeine_sched.c) executes one guest CPU at a
time on the native driver, using the Context's backend factory.
`ExecBase.ThisTask` follows the current guest Process. Scheduling occurs between
outer slices, outside thunks/callbacks, respecting guest Forbid/Disable and
supported cooperative waits.

Each typed child has a separate native ARM
[DOS broker](../../rom/kaffeine/kaffeine_dos_broker.h), whose entry is a compiled
library function, never a guest `NP_Entry` or seglist. It owns its optional CLI, copied
arguments, local variables, distinct current/home locks and private streams, initially
opened on `NIL:`. It runs selected compiled DOS thunks, with its own `IoErr()`;
it never runs the guest CPU. Guest files and locks use typed registries.

The [startup adapter](../../rom/kaffeine/kaffeine_domain_process.c) privately
captures the broker's original native streams before importing any guest
aliases. Native `DosEntry` closes those saved input/output/error handles at
exit according to the live `PRF_CLOSE*` flags; `SelectInput()` and
`SelectOutput()` change selections without transferring that exit ownership.
Broker cleanup retires only captured originals still armed for native close.
Guest-owned replacement files remain registered for explicit `Close()` or
Context cleanup. A partial initialization retains its capture until cleanup;
a broker that never imported streams has no aliases to retire.

Explicit guest `Close()` first disarms every matching captured startup slot
across the Context's controlled native Processes, even if native close later
reports failure. It also detaches selected native aliases on the other
brokers and driver. Captured identities are invalidated before the native
handle can be reused, so a fresh file at the same BPTR survives later broker
exit. Duplicate/shared originals retain exactly one native exit-close slot.
These operations use the existing serial driver/broker protocol; they do not
inspect or change unrelated native Processes. The private close hook is
removed with the final startup-stream record.

Guest queues, messages and nested payload pointers remain in the shared arena;
a child can receive the exact address used by its parent. This does not permit
forwarding arbitrary guest payloads/ports to native `PutMsg()`.

Classic `AddTask()`/`CreateProc()` retain their program-end lifetime. A
classic-only `CreateProc()` has no native DOS context and refuses DOS calls
other than `Exit()`.
Typed children enable persistent Process lifetime.

## Typed DOS packets

`PutMsg()` to a registered file or handler port uses the same validated
[FileHandle](../../rom/kaffeine/kaffeine_filehandle.c) and
[handler](../../rom/kaffeine/kaffeine_handlers.c) packet adapters as the root.
The supported actions, StandardPacket layout, buffer bounds, private handle
identities and reply-queue checks remain in those adapters. An ordinary guest
port or unrecognized packet cannot select a native DOS route.

For a typed Process, the scheduler stages only the guest port, message address
and adapter family in private native metadata. A fixed compiled helper executes
the adapter in that Process's broker, with its native directory and `IoErr()`
state. No guest function pointer or CPU instruction runs there. `SendPkt()` and
`DoPkt()` already inside that same selected broker call reuse its owner without
submitting another RPC. The driver stays paused until acknowledgement.

Packets from a guest callback, inside a held LayerInfo scope or with a mismatched
Process/CPU/native owner are refused. Classic `CreateProc()` still has no DOS
packet context; root and ordinary `AddTask()` behavior is unchanged. LVO zero
remains an internal refusal sentinel, not a general DOS capability.

Replies keep the original guest message address and big-endian queue links.
Child notifications update that child's virtual signals. A reply addressed to
the live root signals the fixed native driver, even when the packet adapter is
running on a broker. Staged arguments clear on completion or failure and before
Process teardown; a transport failure never permits execution on the driver.

## Loader and startup transaction

[`LoadSeg()` / `NewLoadSeg()`](../../rom/kaffeine/kaffeine_thunks_dos.c) reserve a
private [segment record](../../rom/kaffeine/kaffeine_segments.h) before native
DOS runs. The guest receives a nonreused opaque token outside arena BPTR space,
resolvable only by its owning Context. `GetSegListInfo()` establishes loader
identity; an unknown format is not assumed to be native code.

Typed startup accepts an owned, idle 68k HUNK with loader relocation metadata.
A transfer lease hides its token. The
[importer](../../rom/kaffeine/kaffeine_domain_modules.h) validates and copies it
to independent heap-owned classic segments, relocates guest addresses and
invalidates code caches without replacing the running root's state. Code/data
remain valid guest buffers; guest `FreeMem()` cannot release imported blocks.

The [startup adapter](../../rom/kaffeine/kaffeine_domain_process.c) stages the
broker, guest CPU, Process, stack, arguments, SegArray and optional CLI. Success
publishes the guest Process, consumes the original module/home-lock references
once and retires their handles. Failure restores those handles; private copies
awaiting cleanup stay Context-owned. Native `CreateNewProc()` return pointers
are not used to construct guest identity.

Current bounds are 64 registered loader modules, 16 child task slots, 16 live
or deferred imported modules, 1000 hunks and 1,048,576 relocation records per
import. Allocation and metadata checks can reject startup before these limits.

## Supported CreateNewProc tags

The [parser](../../rom/kaffeine/kaffeine_process_tags.c) bounds the tag walk to
64 steps, rejects duplicate/unknown tags and copies strings before startup.

| Tag | Accepted value and default |
| --- | --- |
| `NP_Seglist` | Required registered 68k HUNK token with relocations. |
| `NP_FreeSeglist` | Only `TRUE` when supplied; success consumes the source module. |
| `NP_StackSize` | 256 to 65536 bytes; default 65536. This is the guest stack; the broker has a separate native stack of at least 128 KiB. |
| `NP_Name`, `NP_CommandName` | At most 255 characters each. Name defaults to `68k process`; command defaults to name. |
| `NP_Arguments` | At most 4095 characters; default empty. Both native and guest startup storage are copied. |
| `NP_HomeDir` | A nonzero, caller-owned typed lock transferred on success. Omission inherits; explicit zero is refused. |
| `NP_Cli`, `NP_CopyVars` | Boolean values only; defaults `FALSE` and `TRUE`, respectively. |
| `NP_Priority` | Signed byte range; default native caller priority. |
| `NP_ConsoleTask`, `NP_WindowPtr` | Zero only. |

The parent's `KAFFEINE_CPU` value is validated as `moira` or `jit` and copied
into the broker's local environment; absence defaults to `moira`. Guest entry
pointers, exit callbacks, user data, injected stream/current-directory handles
and other unsupported tags are refused. Invalid tags/values return zero and a
DOS error; they are never passed through as native pointers.

## Domain lifetime, serialization and cleanup

With a nonzero `RunHunk()`/`RunFile()` stack parameter,
[`Kaffeine_RunDomain()`](../../rom/kaffeine/kaffeine_domain_run.h) transfers the
Context to a controlled ARM driver. Normal root return can release the original
native caller while typed children continue. Borrowed stream aliases retire
before that caller can close them; the arena, root code, arguments and library
pin survive until domain retirement. A zero stack parameter stays in the
current native owner until execution finishes. Faults/refusals can stop the
whole domain.

Broker RPC is synchronous and serial. While it uses a Context/CPU, the driver
must not execute guest code, switch DOS views or allow guest callback reentry.
`exec_task` remains the driver; `broker_owner` is scoped to the native thunk
and cleanup. A parent broker can coordinate trusted child initialization or
cleanup, not arbitrary nested DOS dispatch. Blocking DOS can pause the whole
domain; this is not a general asynchronous transport.

Broker cleanup acknowledgement precedes child CPU/memory release. File/lock
aliases retire before native DOS closes resources; I/O and shared IDCMP bindings
retire before task signals/stack reuse. Tracked messages or I/O can quarantine
task memory or imported modules until safe Context cleanup.

Startup pre-reserves recovery storage. Busy/refused cleanup retains the broker,
staged CPU/Process and module, stops the guest and queues
[driver recovery](../../rom/kaffeine/kaffeine_domain_process.h). It probes broker
metadata before changing DOS views, retries with a one-tick yield and releases
resources only after cleanup acknowledgement. Driver/Context lifetime extends
through recovery; refusal never permits freeing a worker's resources.

UI resources also follow their existing owners within this shared lifetime.
Shared IDCMP inboxes belong to the driver and their guest signal owner;
Remember allocations retain their private allocation ownership; VPE mirrors
follow their screen holds; the typed graphics default font has a Context-long
native hold. They are not duplicated for each guest CPU. Final native UI/I/O
cleanup precedes font and whole-arena disposal.

## Regression evidence and current boundary

The original [native controller](../../developer/debug/test/kaffeine_target/domain_fixture_native.c)
and [HUNK generator](../../developer/debug/test/kaffeine_target/domain_fixture_hunks.c)
exercise the complete guest loader/startup path. The generator emits original
68000 root and child programs without an application binary or a 68k compiler.
See the [target instructions](../../developer/debug/test/kaffeine_target/README.md)
for its standalone build and external execution deadline.

Run `domain_fixture_native moira` and `domain_fixture_native jit` separately on
a disposable uniprocessor AROS ARM64 target. Each successful invocation is
expected to report **37 checks, 0 failed**. The checks cover the same 128-byte
message and nested pointer, distinct guest Processes/CLIs and DOS state, root
result 55 before child completion, survival after native source/argument and
caller-state changes, child `Exit(7)`, and bounded native-owner retirement.
The controller rejects an SMP build and requires exactly one active CPU;
these checks do not establish SMP support.

Standalone host suites compile production components with controlled native
collaborators:

| Suite | Scope |
| --- | --- |
| [kaffeine_domain_host](../../developer/debug/test/kaffeine_domain_host/README.md) | Real heap/importer, relocation, rollback, ownership, cache notification and deferred unload. |
| [kaffeine_dos_broker_host](../../developer/debug/test/kaffeine_dos_broker_host/README.md) | Threaded broker ownership, serial RPC, native state, cleanup acknowledgement and idle probing. |
| [kaffeine_domain_process_host](../../developer/debug/test/kaffeine_domain_process_host/README.md) | Startup transaction and retained recovery resources under injected failures. |
| [kaffeine_domain_run_host](../../developer/debug/test/kaffeine_domain_run_host/README.md) | Driver publication, root-return gate, caller resources and final Context ownership. |

Each suite documents its ordinary and ASan/UBSan runner with an explicit build
directory. The main host suite also covers the
[tag parser](../../developer/debug/test/kaffeine_host/test_process_tags.cpp) and
[real scheduler/backends](../../developer/debug/test/kaffeine_host/test_guest_tasks.cpp).
Its [DOS packet fixture](../../developer/debug/test/kaffeine_host/process_packet_fixture.h)
links the real scheduler, dispatcher, heap, file/handler adapters and queues to
recording CPUs/native transport. It checks child Process/directory/error
identity, file reads and packet errors, direct and nested routes, root/child
reply ownership, malformed or mixed inputs, callback refusal, staged-request clearing,
classic-task limits and authoritative return code 20 through the real CPU loop.
The [startup-stream groups](../../developer/debug/test/kaffeine_host/process_stream_fixture.h)
also compile the production initialization/retirement helpers verbatim and use
real FileRecords, `Close()`, heap and full-file preparation/snapshot sweeps.
They cover selected owned replacements, native-freed originals, root/peer
aliases, duplicate/shared startup handles, live close flags, explicit close
failure followed by BPTR reuse, partial initialization and untrusted callers.
Normal and ASan/UBSan runs each pass **18548 checks** in `test_dos_packets`.
The threaded broker suite independently models native `DosEntry` saving its
streams before worker entry and consulting the live close flags afterward.

```sh
make -C developer/debug/test/kaffeine_host BUILD_DIR=/tmp/kaffeine-packets -j1 test_dos_packets
/tmp/kaffeine-packets/test_dos_packets
make -C developer/debug/test/kaffeine_host BUILD_DIR=/tmp/kaffeine-packets-san -j1 \
    CFLAGS="-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer" \
    CXXFLAGS="-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer" \
    LDFLAGS="-fsanitize=address,undefined" test_dos_packets
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    /tmp/kaffeine-packets-san/test_dos_packets
```

Host, native target and application GUI acceptance are separate evidence lanes;
this document does not claim a current application probe or editor UI result.
