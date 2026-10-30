# Same-domain HUNK import checks

These host checks compile the complete production allocator/copier,
classic-message memory guard and domain-module adapter as C. The original
native segment fixtures use the public HunkRelocTable ABI. No replacement
guest allocator, relocation copier, emulator or application binary is used.
The native allocation and CPU cache callbacks are recording collaborators.

From the repository root:

```sh
python3 developer/debug/test/kaffeine_domain_host/run_tests.py \
  --build-dir "${TMPDIR:-/tmp}/kaffeine-domain-host"
python3 developer/debug/test/kaffeine_domain_host/run_tests.py \
  --build-dir "${TMPDIR:-/tmp}/kaffeine-domain-host-sanitized" --sanitize
```

The source directory defaults to the repository's `rom/kaffeine` and can be
overridden with `--source-dir`. The build directory is mandatory. `CC` and `CXX`
select C99/C++20 compilers. Existing `kaffeine_host/memory_stubs` and `stubs`
provide shared native ABI scaffolding; the small local overlay supplies DOS
error declarations and unused Exec signal declarations. Each subprocess has
a 30-second limit. Sanitizers use ASan/UBSan; Darwin disables unsupported leak
detection, while other hosts enable it. Native allocations are also counted
and checked independently for exact size and single release.

The checks cover absolute, relative, duplicate and modulo-32 relocations;
distinct imports into a live arena; unchanged root entry/stack/arguments/live
bytes and all CPU registers including PC/CCR/X; classic size/next/BPTR layouts;
source disposal immediately after import; stale/foreign private owners; guest
FreeMem/FreeVec refusal while code/data remain graphics/DOS/message buffers;
rollback at each allocation with conservative reference guards; malformed
metadata, cycles and limits; 1000-hunk import; edited guest headers; busy unload
and deferred cleanup; native-record cleanup at final quiescent destruction;
closing during native allocation; and code-cache notifications at publication
and retirement, including a destroyed current CPU.

This is host ownership, allocator and relocation evidence. The cache callback
records invalidation without executing a JIT. It does not establish scheduler
behavior, native process semantics or application UI acceptance.

The expected result is 3599 checks with no failures. CI or the main host-suite
target should call this runner with its own explicit build directory.
