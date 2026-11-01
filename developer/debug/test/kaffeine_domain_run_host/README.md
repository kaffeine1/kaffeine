# Asynchronous domain driver regression

The fixture compiles the complete production `kaffeine_domain_run.c` as C99
against threaded native collaborators. It models the native DOS caller and
the independent ARM driver, not a guest CPU or the scheduler.

From the repository root:

```sh
python3 developer/debug/test/kaffeine_domain_run_host/run_tests.py --build-dir /tmp/kaffeine-domain-run-host
python3 developer/debug/test/kaffeine_domain_run_host/run_tests.py --sanitize --build-dir /tmp/kaffeine-domain-run-asan
```

`--source-dir` may select another production `rom/kaffeine` directory.
`CC`/`CXX` override Clang/Clang++. Both units use warnings as errors.
The sanitizer run enables AddressSanitizer/UndefinedBehaviorSanitizer with
immediate failure. Leak detection is disabled for macOS; separate context,
record, semaphore, library, directory and stream accounting stays active.
The runner writes exact commands and output to the explicit build directory.

The actual driver is tested for:

- A deliberately unreadable native `CreateNewProc` success sentinel `1`.
  Native worker state is published through the private gate, without reading
  the returned Process pointer. Early worker startup failure is also tested
  before that success value returns to the caller.
- Ownership transfer only at COMMIT, exactly one `run` and native-owner
  `dispose`, and no caller context inspection after a TRUE result. A fast
  run may dispose the context before the caller observes completion.
- Root completion before an independent child/domain finishes, with the
  private gate and library pin retained until final driver disposal. The
  first root result wins over repeat notifications and the final child code.
- All three full-width caller streams explicitly passed with close flags
  FALSE. Borrowed guest identities are retired and detached before the gate
  permits caller return/close. Repeated completion is idempotent; native DOS
  never closes the caller-owned handles.
- No `NP_Arguments` injection into borrowed input: arguments are already owned
  by the context. CLI and non-CLI drivers retain the corresponding native
  behavior, and the driver has a separate native current-directory reference.
- Allocation, private-library-open, directory-duplication, native-process and
  worker-command failures, preserving complete native errors and leaving
  context/result ownership with the caller on FALSE.
- Bounded startup timeout with a delayed native worker. The caller frees its
  untransferred context before that worker enters; ASan verifies that late
  abort neither runs nor disposes nor accesses that freed context.
- Invalid input and pure Exec Task refusal before any native `IoErr` query.
  AROS `IoErr` requires a Process; `SetIoErr` safely ignores other Tasks.
- Break forwarding. CTRL-C to CTRL-F that the caller holds at the commit, or
  gets while the root runs (as `Break` of its CLI does), go with `Signal()`
  under the gate semaphore to the task that the driver published at READY;
  the caller keeps every other signal. Before the commit, on failure or
  timeout and after root completion the caller keeps its breaks, and no
  `Signal()` reaches a driver whose root has completed or whose entry has
  returned. The commit edge is deterministic: the caller's first poll after
  the commit waits until run has begun, so only breaks given with the commit
  can be there. The late case parks the caller in its poll across root
  completion and a break that follows it.

These are host collaborator tests. They do not establish target execution,
guest scheduler fairness, arbitrary asynchronous IPC or application startup.
The compiled run callback must not terminate the native driver without
returning. Independent guest Process behavior is represented by a controlled
barrier; target tests must verify the real scheduler and application.
