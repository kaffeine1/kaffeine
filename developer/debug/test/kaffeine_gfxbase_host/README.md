# Typed graphics.library DefaultFont regression

This fixture builds the complete production graphics-base setup, font bridge,
heap, bounded guest-buffer checks and CPU interface. It extracts the complete
`Kaffeine_OpenLibrary()` function from `kaffeine_thunks.c` unchanged; generated
library and unrelated manual thunk tables are inert collaborators. Native Exec
allocation, graphics-library opens and font opens are recording collaborators.
There is no application code or application input.

Run from the repository root, choosing separate output directories:

```sh
python3 developer/debug/test/kaffeine_gfxbase_host/run_tests.py --build-dir /tmp/kaffeine-gfxbase
python3 developer/debug/test/kaffeine_gfxbase_host/run_tests.py --sanitize --build-dir /tmp/kaffeine-gfxbase-san
```

The default source directory is calculated from this file's repository location.
`--source-dir /path/to/rom/kaffeine` selects another public source snapshot.
`--reservation-only` runs the bounded public-API regression for positive-area
size and typed DefaultFont provenance. All outputs, commands and logs go in the
explicit build directory. Compilation and execution are sequential and each
subprocess has a 30-second limit. The runner uses the sibling `kaffeine_host`
portable graphics, heap and Exec header collaborators. New test-specific
headers describe only the native Library/GfxBase prefix this bridge reads;
they assert the 64-bit native DefaultFont offset (320). Target native headers
and runtime remain separate validation lanes.

The independent classic layout expectations are GfxBase size 552 and
DefaultFont offset 154, as recorded by the classic layout probe in
`rom/kaffeine/m68k_offsets.txt`. Tests use literal expected offsets/sizes, not
constants exported by the new implementation. They check:

- Dirty base storage is cleared only inside the reserved 552-byte positive
  area. All other positive fields remain zero. Preceding bases and negative
  vectors remain intact, and the following jump table starts after the full
  reservation. Other fake library sizes retain their existing behavior.
- DefaultFont is the typed view of the exact real native default. Names,
  monochrome bitmap and signed character tables are copied with classic byte
  order; native node, reply-port and extension pointers remain zero. Font data
  remains a legitimate readable guest buffer.
- The context owns a real native font open. Repeated library opens keep both
  identities and acquire no extra hold. Existing guest opens, borrowed views,
  pins and retained references are reused without lost or duplicate holds.
  Subsequent native preference selection changes do not replace a published
  identity or invalidate the context's held font. This is an initialization
  snapshot, not a general preference synchronization API.
- Native library/open failures, a different returned font, an overflowing
  native accessor count, unsupported/partial fonts, native metadata OOM and
  library/name/font capacity limits publish nothing and balance acquired
  references. Failed setup also preserves pre-existing borrowed or pinned
  font views and guest heap capacity.
- Context font cleanup closes only acquired references. Two contexts hold
  independent references to the same native font; cleaning up one leaves the
  other valid. Native allocation records detect invalid, duplicate, missing
  and wrong-size releases.

ASan/UBSan checks the complete exercised production paths. LeakSanitizer is
not enabled by this runner; the fixture independently checks that every native
allocation and acquired font/library reference is released. This is host
source evidence. It does not establish an ARM target or application result,
nor explain any application's choice of a requested font name.
