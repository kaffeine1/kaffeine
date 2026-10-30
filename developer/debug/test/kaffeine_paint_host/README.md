# Bounded classic paint-Hook compiler prototype

This pure-C module compiles a restricted, finite classic 68000 Hook into an
immutable integer-expression plan. A foreign native paint callback evaluates
that plan and replays typed native drawing operations. It never executes a
68k CPU, JIT, Moira instance, guest callback, or native pointer from guest data.
No application input, instruction signature, binary, or asset is used here.

`kaffeine_paint_compile.h` is the complete API. The initial Hook ABI is A0=Hook,
A1=20-byte BackFillMessage, A2=100-byte classic RastPort, and A7=a private abstract
stack. The driver supplies a read-only bounded source and the already typed
Exec/graphics fake bases. Compilation is transactional and uses caller-owned,
preallocated storage. Query the workspace sizes and alignment functions.

The proof supports MOVE/MOVEA, MOVEQ, LEA, ADDA/SUBA, EXT, MOVEM, CLR, ST, TST, constant DBRA,
BRA/BEQ/BNE, static internal JSR/JMP, RTS, and NOP. Other instructions and unproven
control flow fail closed. Known typed calls are SetAPen342, SetDrMd354,
RectFill306 and Signal324. Void bridge calls set D0=0 and preserve other virtual
registers/CCR, matching the current manual bridge. Only Z is modeled; relational
branches are refused. EA modes are a restricted legal 68000 subset, and
predecrement MOVEM with its own base in the register mask is refused.
ADDA/SUBA.W/L require compile-time constant operands, sign-extend WORDs,
leave CCR unchanged, and check abstract stack displacement and numeric address
overflow before changing state. Dynamic operands and wrapping addresses refuse.

A local RastPort clone must prove all 100 original bytes, except Layer must be
zero and Mask may be changed. Ordered setter calls update modeled local state;
SetAPen's linpatcnt/Flags effects and SetDrMd's linpatcnt/minterms effects are
accounted for. Reading recomputed minterms is conservatively refused. Direct
unmodeled field stores invalidate the clone. Only local stack stores are
permitted. Copying opaque pointer/private RastPort bytes is allowed; observing
them numerically is refused. `rp_used` identifies observable public scalar bytes.

RectFill endpoints must be original Min/Max coordinate expressions, after signed
WORD truncation, and stay within the actual invocation Bounds. Reversed endpoints
within Bounds produce no RectFill command, matching public AROS RectFill. Affine
insets, arbitrary coordinates, relational guards, AreaPtrn effects and arbitrary
graphics APIs remain outside this profile. Native replay must clone its actual
native RastPort (never memcpy 100 classic bytes into a native struct), clear Layer,
and preserve command order. Native callback baselines and bitmap provenance must
be validated by the caller.

Code, complete Hook headers, pointer-chain intermediates and signal-mask cells
become exact equality dependencies. Ordinary scalar cells remain dynamic.
`used_mask` and cell roles expose the bits needed by evaluation: byte-valued pen
inputs can be snapshotted as their low byte. An owner capability is a 32-bit enum
`{0,captured live guest owner}`; it cannot influence paint scalars, and zero must
select a no-Signal path. Signal masks must be constant or immutable and their
finite union is `plan.signal_mask`. Signals are tail effects and carry the
captured numeric generation cookie. There is no fallback task.

The native adapter must lease/pin every code/header/pointer/scalar dependency,
retain typed screen/layer/RastPort provenance, lease signal bits, preallocate all
workspaces, and reject release/reuse while installed. It must validate all code
and pointer snapshots and collect all scalar input before evaluating or painting.
Evaluation checks the entire selected command stream before publishing output.
The plan is private, immutable compiler output, not an untrusted serialization
format. Workspaces, input and output buffers must be distinct.

This prototype establishes no SMP or native foreign-callback lifetime proof.
UP Forbid can bound snapshot collection but cannot make an already half-written
emulated multi-byte scalar coherent. General mutable multi-byte scalars require
a stronger publication contract or must be refused by integration. The narrowly
validated owner endpoint enum is separate from such general scalar arithmetic.
There must be no allocation, waiting, graphics, or signaling inside Forbid.

Run the host tests from an installed repository with explicit disposable output:

```sh
python3 developer/debug/test/kaffeine_paint_host/run_tests.py \
  --build-dir /tmp/kaffeine-paint-normal --oracle
python3 developer/debug/test/kaffeine_paint_host/run_tests.py \
  --build-dir /tmp/kaffeine-paint-san --sanitize --oracle
```

`--source-dir` overrides repository discovery. In the isolated review directory,
the core source can remain next to the tests and `--source-dir` supplies only the
pinned Moira repository for the optional oracle. Clang/C99 and Clang++/C++20 are
used by default; CC/CXX may override them.

The ordinary and ASan/UBSan runs each passed 4,988 original core checks plus 557
independent Moira checks. The oracle executes original generated 68000 programs,
intercepts only known typed bridge-call boundaries with public setter
collaborators, and compares all 100 RastPort bytes and signed coordinates at each
fill, deferred signals, and restored registers/SP. It covers word/long copies,
varied registers/counters, ordered multiple fills, and the zero-owner no-Signal
path. Core negative cases cover immutable changes, pointer/private observations,
nonlocal stores, unsupported instruction/control forms, malformed clones,
invalid owner/mask/cookie, bounded loops, capacity and workspace failure. These
are host proof lanes; no target or application acceptance is claimed.

The default host runner also validates the original native paint fixture's
portable emitter in `../kaffeine_target/paint_fixture_native.c`. Its 767 checks
parse and relocate the emitted HUNK, verify deterministic bounded publication
and classic layouts, compile the actual emitted Hook, and evaluate ordered
paint commands, signed bounds, exact owner CTRL_D, zero-owner behavior,
invalid-owner refusal, dynamic low-byte pen and every changed dependency.
`PAINT_FIXTURE_EMIT_ONLY` excludes all native controller code from this host
build. This requires no AROS SDK and runs in both ordinary and sanitizer modes;
the native observer/controller lifetime and actual painted pixels require a
separate AROS target invocation.
