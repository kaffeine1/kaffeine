# Process startup and Signal integration regression

The runner extracts the complete production `Kaffeine_Signal`, `cleanup_owned`, `execute_owned`, `start_owned`, `dispose_context`, and `setup_and_execute` functions into a native host collaborator fixture. Run it from the repository root:

```sh
python3 developer/debug/test/kaffeine_process_integration_host/run_tests.py --build-dir /tmp/kaffeine-process-integration
python3 developer/debug/test/kaffeine_process_integration_host/run_tests.py --sanitize --build-dir /tmp/kaffeine-process-integration-asan
```

The source directory can be overridden with `--source-dir`. Warnings are errors. This fixture tests the actual extracted function bodies; it does not reproduce their implementation.

It checks self Signal masking, refusal of unowned native Signal targets, scheduler routing for all shared guest tasks, and owner cleanup after missing Process shadows. Startup binds the native owner before shadow creation and captures all three typed borrowed stream identities before AutoOpen/CPU execution. Both successful and failed startup exercise direct execution without context ownership transfer and RunDomain handoff with cleanup/disposal in the driver owner. Every setup call supplies the production `BOOL *transferred` output.

The RunDomain collaborator deliberately tests only startup/cleanup routing. It returns the selected driver result and invokes the supplied compiled disposer; fixture Context storage remains owned by the harness. The separate `kaffeine_domain_run_host` fixture compiles the complete actual driver with native threads and tests early parent return, surviving child lifetime, real context disposal, borrowed stream retirement, CNP result lifetime, startup failures and late abort. Passing this focused integration fixture alone is not asynchronous-driver, target, or original-application acceptance.

Recording startup-protocol collaborators check that publication follows the
Process shadow, task operations and captured streams, then precedes AutoOpen
and CPU execution. Publication failure exercises rollback in both the direct
and domain owners; retirement precedes native resource cleanup and final
disposal. The real message adapter and replies are tested separately.

Screen cleanup uses a recording screen with a native bitmap lease: the first
pass must keep it alive, layer cleanup releases the lease in the same owner,
and the final pass must close the screen exactly once. This checks resource
retirement rather than assuming the cleanup function can only be called once.

A recording blitter collaborator checks that the production owner cleanup
invokes its release callback before startup retirement and GUI/native teardown.
Normal returns and protected stops cover direct and domain owners, with and
without an outstanding hold; later context disposal cannot release it twice.
The actual blitter ownership implementation is tested by the separate blitter
host and target suites.
