# Classic Intuition Remember ownership

This fixture compiles the complete production `kaffeine_remember.c`, allocator,
message/guest-memory guards, and CPU register accessors. Only native allocation,
IoErr, and native-lock protection collaborators are substituted. No extraction,
replacement heap, SDK, application binary, or proprietary fixture is required.

```sh
python3 developer/debug/test/kaffeine_remember_host/run_tests.py --build-dir /tmp/kaffeine-remember
python3 developer/debug/test/kaffeine_remember_host/run_tests.py --sanitize --build-dir /tmp/kaffeine-remember-sanitized
```

The source directory defaults to this repository's `rom/kaffeine`; override it
with `--source-dir`. Headers shared with `kaffeine_host/{memory_stubs,stubs}` and
the repository's public `compiler/include/dos/doshunks.h` are required. Compiler
commands, complete output and results are retained in the explicit build directory.

Both ordinary and ASan/UBSan runs passed **37270 checks, 0 failures**. These cover:

- real manual vector macro at 396/408 and classic Next/Size/Memory fields at 0/4/8;
- 12-byte guest nodes, prepending, MEMF_CLEAR, exact data/padding bounds and DOS-buffer use;
- full CPU register/PC/CCR-X and complete-width native IoErr preservation;
- real RunCPU returning rc20 for each refused zero-result thunk without a next instruction;
- ordinary OOM executing the next instruction and preserving its normal process result;
- ordinary FreeMem/FreeVec/AllocAbs unable to steal remembered data or nodes;
- FreeRemember(TRUE) releasing all data and clearing the supplied key;
- FreeRemember(FALSE) retaining bytes and transferring data to normal guest ownership;
- copied live keys and retired keys attempting to free reused data addresses;
- corrupt/cyclic node fields, forged/foreign nodes, and invalid/straddling/aliasing keys;
- record/node/data OOM, zero/overflow sizes, complete rollback and capacity restoration;
- unpublished rollback despite an unrelated queue becoming malformed;
- a retained I/O buffer refusing the whole chain before any release commit;
- 4096 quarantined identities, refusal before allocation at the cap, and final native cleanup;
- final cleanup with guest memory absent, proving that expired guest keys are never touched.

Native allocations and exact disposal sizes are independently counted. ASan leak
detection is disabled for Darwin portability; all native blocks must balance.
These host checks do not establish ARM/backend/application acceptance.

The native source oracles are `rom/intuition/allocremember.c` (slot 66),
`freeremember.c` (slot 68), and the public `struct Remember` in
`compiler/include/intuition/intuition.h`. `kaffeine_host/test_thunk_registrations.py`
has two corresponding native-source cases. Run that complete checker separately
after integration; the focused fixture validates the production registration macro.
