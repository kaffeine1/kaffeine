# Domain process startup recovery

This fixture compiles the complete production `kaffeine_domain_process.c` as C.
Only native DOS, broker, scheduler, allocator, and lock/file collaborators are
substituted. The staged scheduler collaborator checks the required child view,
restores the caller view before CPU retirement, and reports ACK ordering.

Run from the repository root:

```sh
python3 developer/debug/test/kaffeine_domain_process_host/run_tests.py --build-dir /tmp/kaffeine-domain-process
python3 developer/debug/test/kaffeine_domain_process_host/run_tests.py --sanitize --build-dir /tmp/kaffeine-domain-process-sanitized
```

The source directory defaults to `rom/kaffeine` in this repository. An explicit
`--source-dir` selects another public source snapshot. Test-only native headers
are shared with the sibling `kaffeine_domain_run_host/stubs` fixture; no SDK,
application, ROM, target binary, network access, or private artifact is required.

The ordinary and ASan/UBSan runs cover:

- recovery storage reserved before the importer or broker acquires resources;
- allocation, loader metadata, import, home duplication, broker, and scheduler failures;
- original full-width native segment/home references consumed only on publication;
- native broker ownership of a distinct home reference;
- immediate rollback, nested parent-broker rollback, and staged startup rollback;
- a busy broker and an injected refused Finish retaining module/CPU/TCB/record;
- no guest execution, new startup, or view switching while recovery is pending;
- driver/guest/callback capability checks before a drain can enter a DOS scope;
- one DOS tick for every pending pass, preserving complete-width IoErr;
- native cleanup ACK before staged CPU retirement and module release;
- context-owned deferred module cleanup, repeated drains, and alias-drop helpers.

Stream capture now precedes the first import and retires with the broker's
cleanup acknowledgement. Rollback without initialization retires no
uncaptured streams. The focused helper case checks capture retirement and
private-hook reset; file collaborators here only record imports/drops. The
[main DOS packet fixture](../kaffeine_host/process_stream_fixture.h) separately
links the same production helpers to real FileRecords, `Close()`, heap and
snapshot sweeps to verify replacement ownership, saved original identities,
live native close flags and explicit-close/BPTR-reuse safety.

`--baseline` runs only the refused-cleanup ownership check on an old production
source directory that lacks recovery support. It deliberately expects a test
failure: ignoring Finish(FALSE) unloads the still-owned module and resumes the
guest. It does not add a replacement recovery implementation to the old source.

This is a production adapter regression, not a scheduler or native broker
implementation oracle. The real broker's threaded tests and real scheduler's
CPU/backend tests remain separate requirements. ASan leak detection is disabled
for portability on Darwin; startup allocations and native home ownership are
independently counted. These host checks provide no ARM/application proof.

Verified isolated results: ordinary **666 checks, 0 failures** and ASan/UBSan
**666 checks, 0 failures**. The frozen old adapter fails the focused baseline
with **17 checks, 3 failures** (early module release, missing retention, and
continued guest execution after a refused Finish). The normal and sanitizer
builds use the same production source and fixture.
