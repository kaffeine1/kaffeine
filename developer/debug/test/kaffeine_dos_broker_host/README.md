# Native DOS broker regression

This fixture compiles the actual `rom/kaffeine/kaffeine_dos_broker.c` as C99.
Threaded native collaborators implement `CreateNewProc`, real message queues,
private native Processes, full-width directory references, and independent
native `IoErr` values. The fixture does not emulate a guest CPU or DOS.

From the repository root:

```sh
python3 developer/debug/test/kaffeine_dos_broker_host/run_tests.py --build-dir /tmp/kaffeine-dos-broker-host
python3 developer/debug/test/kaffeine_dos_broker_host/run_tests.py --sanitize --build-dir /tmp/kaffeine-dos-broker-asan
```

The runner also accepts `--source-dir` pointing to another directory containing
the production broker source. Clang/Clang++ are the defaults; `CC` and `CXX`
override them. Both compilation units use warnings as errors. The sanitizer
run enables AddressSanitizer and UndefinedBehaviorSanitizer, with immediate
failure; leak detection is disabled for macOS compatibility. Independent
allocation, lock, stream, port, semaphore, and library-pin accounting remains
active in both runs.

Coverage includes:

- `CreateNewProc` returning the deliberately unreadable success value `1`;
  only the ARM worker entry publishes its own inbox and identity.
- Copied names, command, arguments and backend, separate native CLI,
  current/home directories, NIL streams, local variables and full-width
  native handles. A nested Start from a parent broker retains the original
  domain driver as RPC owner. That parent can coordinate only LVO-zero
  initialization or final cleanup, with its private scoped owner capability;
  ordinary DOS thunks and a foreign domain are refused. Nested replies restore
  the parent's capability. Failed child initialization can be cleaned before
  publication without running guest code.
- Serial full-width callback results, cleanup on the same worker after
  clearing `after_thunk`, publication before driver resumption, preserved
  thunk `IoErr` even when cleanup changes it, and unchanged caller `IoErr`
  even when broker-internal polling changes it.
- Private `broker_owner` capability scoped to the callback and its cleanup;
  `exec_task` remains the domain driver and the capability is clear before
  acknowledgement. Diagnostic LVO zero permits a trusted initialization
  helper before the first guest instruction.
- Interleaved calls for two guest Processes with distinct native owners and
  `IoErr`. One broker can stop while its peer remains usable.
- Foreign caller and incomplete request refusal without callback execution;
  exact request/reply accounting; no driver reply signal allocation and no
  stale native `SignalTask` in the private reply queue.
- Library-open, home-duplication, record-allocation, native-CNP, worker-inbox,
  variable-installation and command-installation failures. Startup errors
  preserve complete native error values and leave the original home with its
  caller. Only successful Start consumes that original reference.
- Bounded startup timeout before the worker has entered, followed by late
  worker abort and balanced private cleanup. The worker's reference and
  library pin survive until that late entry can safely retire its userdata.
- Zero home, optional strings, non-CLI worker, and independent inherited
  native directory cleanup.
- Final `Finish` cleanup with a NULL CPU on the same native owner, before
  native streams/directories/inbox/library are retired. A controlled native
  barrier proves that acknowledgement cannot arrive while cleanup remains
  active. Incomplete cleanup/context pairs are refused before submission.
- Metadata-only `IsIdle` on an owned broker handle, with an actual callback
  held at a native barrier: the busy probe performs no DOS/IoErr operation
  or Context mutation, and becomes ready after the driver receives its reply.
  Recovery callers must stop guest execution and ensure that every nested
  coordinator has returned before using this serial probe followed by a
  scoped Finish. The probe does not reserve a concurrent transaction.
- Native `DosEntry` stream ownership: the collaborator saves startup
  input/output/error handles before worker entry and reads the live
  `PRF_CLOSE*` bits at exit. Interleaved brokers can replace all selected
  streams without native DOS closing those replacements. Explicitly closing
  a displaced original disarms every matching saved slot, including shared
  startup handles; a fresh owned file reusing that native address survives
  worker exit. These recording callbacks exercise native exit semantics;
  the [main DOS packet fixture](../kaffeine_host/process_stream_fixture.h)
  separately checks production alias retirement with real FileRecords.

Normal and ASan/UBSan runs each pass **3095 checks**, with **150 compiled
callbacks** and **157 RPC/reply pairs**. The startup-stream recording callbacks
do not replace the independent real-registry tests above.

This is host evidence, not an AROS execution or application acceptance test.
The broker is a private synchronous transport. Its trusted scheduler caller
must serialize domain execution, reject guest CPU reentry from a broker,
intercept process Exit, bound the number of live guest Processes, and keep
the domain/native driver alive until all brokers have stopped. A native DOS
operation that blocks also pauses the driver; peer-unblocking DOS and general
asynchronous IPC are outside this transport's contract.
