# Typed default-keymap conversion

The [keymap adapter](../../rom/kaffeine/kaffeine_keymap.c) supports
`keymap.library` vectors -42, `MapRawKey()`, and -48, `MapANSI()`, with
`A2 == NULL`: mapping uses the native system default. Custom guest `KeyMap`
tables are unsupported.

`A0` names one word-aligned, 22-byte classic `InputEvent`; `A1` names the output
buffer; `D1` is its full signed 32-bit `LONG` capacity. The adapter constructs a
zeroed native event (32 bytes on AROS ARM64) and assigns class, subclass, code,
qualifier, timestamp and the four dead-key history bytes from their classic
big-endian/scalar offsets. No classic event or KeyMap pointer reaches native
code. The history is scalar data; unused native union bytes remain zero.

As [native MapRawKey](../../rom/keymap/maprawkey.c) specifies, `ie_NextEvent`
is ignored. The guest link is neither followed nor rewritten, and the native
link stays NULL. This is single-event conversion, not an event-chain adapter.

Both event and output must occupy complete, bounded ordinary caller storage.
Bridge mirrors, heap bookkeeping, queue-owned storage, retained fonts,
memory-info snapshots and library bases are protected. A nonempty output span
must be disjoint from the complete event. Byte-aligned output is allowed;
zero capacity is delegated with a valid non-NULL in-arena address and writes
nothing. Negative lengths and invalid/custom-map inputs stop with return code
20 before native conversion. Positive capacities are checked against the
remaining arena without truncating to 16 bits or imposing a 65536-byte limit.

The native signed `WORD` result is sign-extended through `LONG` into guest
`D0`; for example, overflow -1 becomes `0xFFFFFFFF`. Counts, zero, negative
results and any actual output bytes are preserved. The adapter adds no string
terminator and does not turn a native overflow into success.

## MapANSI

`A0` names the ANSI string and `D0` its signed `LONG` count of bytes; `A1`
names the buffer of code/qualifier byte pairs and `D1` its signed capacity in
pairs. [Native MapANSI](../../rom/keymap/mapansi.c) reads exactly `count`
bytes and writes at most three pairs for each character it consumes: a second
dead key, a dead key and the key. It returns the number of pairs it wrote, 0
for a character the keymap lacks and -1 when the buffer is full, and in both
failures it may already have written earlier pairs.

The adapter copies the string and the first `min(length, 3 * count)` pairs of
the buffer into one private native allocation, calls native MapANSI on them
with that capacity, copies every one of those pairs back and frees the
allocation. A byte the native call did not write therefore keeps its guest
value, as if the call had written guest memory directly, and the bound never
changes a result. The string and that part of the buffer must be ordinary
caller storage, as for `MapRawKey`, and must not overlap. Byte-aligned spans
are allowed. A zero or negative `length` gives no pairs: the buffer is then
neither checked, read nor written, and native MapANSI reports -1 or 0 as it
would. A zero `count` returns 0 without a native call. A negative `count`
would read without end, so it stops with 20, as do a custom map, bad storage,
an overlap or a failed native allocation, all before the native call.

The [manual table](../../rom/kaffeine/kaffeine_keymap.h) also intercepts:

| Vector | Operation | Boundary |
| --- | --- | --- |
| -30 | `SetKeyMapDefault` | Refused; a guest table must not become a retained native system default. |
| -36 | `AskKeyMapDefault` | Refused; the native table pointer is not exposed. |

These operations stop before their generated native fallbacks. No guest
keymap is installed, and the native default remains native-owned.

[test_console.cpp](../../developer/debug/test/kaffeine_host/test_console.cpp)
exercises the real dispatcher, typed adapter and generated fallback table over
recording native collaborators. It checks field/history conversion, ignored
links, signed lengths/results, native output effects, exact bounds, protected
storage, aliases and refusal before native table operations, and for MapANSI
the private copies, the pair bound, partial writes before 0 or -1, zero and
negative counts and lengths, and allocation failure. The mapper's recorded
bytes/results test the boundary, not a country-specific keyboard layout or
physical typing. The target fixture
[keymap_mapansi.c](../../developer/debug/test/kaffeine_target/keymap_mapansi.c)
checks real native conversions by mapping their pairs back with MapRawKey.

```sh
make -C developer/debug/test/kaffeine_host BUILD_DIR=/tmp/kaffeine-keymap -j1 test_console
/tmp/kaffeine-keymap/test_console
make -C developer/debug/test/kaffeine_host BUILD_DIR=/tmp/kaffeine-keymap-san -j1 \
    CFLAGS="-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer" \
    CXXFLAGS="-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer" \
    LDFLAGS="-fsanitize=address,undefined" test_console
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    /tmp/kaffeine-keymap-san/test_console
```

Host checks, native keymap behavior and application typing acceptance are
separate evidence lanes; this document claims no target typing result.
