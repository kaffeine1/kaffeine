# Typed default-keymap conversion

The [keymap adapter](../../rom/kaffeine/kaffeine_keymap.c) supports
`keymap.library` vector -42, `MapRawKey()`, with `A2 == NULL`: mapping uses the
native system default. Custom guest `KeyMap` tables are unsupported.

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

The [manual table](../../rom/kaffeine/kaffeine_keymap.h) also intercepts:

| Vector | Operation | Boundary |
| --- | --- | --- |
| -30 | `SetKeyMapDefault` | Refused; a guest table must not become a retained native system default. |
| -36 | `AskKeyMapDefault` | Refused; the native table pointer is not exposed. |
| -48 | `MapANSI` | Refused; its table/buffer conversion is not implemented. |

These operations stop before their generated native fallbacks. No guest
keymap is installed, and the native default remains native-owned.

[test_console.cpp](../../developer/debug/test/kaffeine_host/test_console.cpp)
exercises the real dispatcher, typed adapter and generated fallback table over
recording native collaborators. It checks field/history conversion, ignored
links, signed lengths/results, native output effects, exact bounds, protected
storage, aliases and refusal before native table operations. The mapper's
recorded bytes/results test the boundary, not a country-specific keyboard
layout or physical typing.

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
