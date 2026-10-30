# Native backfill recipe host tests

This suite compiles the complete production backfill compiler and native bridge,
heap allocator, Exec message boundary, signal/port adapters and disk-library
close route. Its hook instructions are original synthetic 68000 programs. No
application contents, guest interpreter or native graphics driver is used.

From the repository root:

```sh
python3 developer/debug/test/kaffeine_paint_hook_host/run_tests.py --build-dir /tmp/kaffeine-paint-hook
python3 developer/debug/test/kaffeine_paint_hook_host/run_tests.py --build-dir /tmp/kaffeine-paint-hook-asan --sanitize
```

The native collaborators track exact allocation sizes, signal ownership and
native Layer semaphore ownership. A separate host thread invokes the installed
native Hook while holding that semaphore. The suite checks recipe replay and
RastPort preservation, changed dependencies and invalid owner capabilities,
unsupported native bitmap/pattern inputs, unlocked entry and immediate driver
retirement, recursive entry,
replacement with pending events, task retirement and numeric identity reuse,
callback quiescence, allocation failure and missing/non-UP kernel proof.

Release tests use the real heap, message, signal, port and library functions.
They verify that allocations and all members of a pool or MemList stay unchanged
when retained, that a port's storage/signal stays owned on refusal, and that a
paint-dependent library's Close/Expunge code is not entered. Detachment restores
the native baseline Hook before leases become releasable. Unrelated allocations
and signals remain usable.

The initial native profile requires one registered CPU reported by
`kernel.resource`, a managed HIDD bitmap, no area pattern, and a native Layer
semaphore held by the calling native task. It supports byte-valued dynamic pen
inputs and validated zero-or-owner task capabilities. Other mutable multibyte
controls, unknown native baseline Hooks and nonlocal guest effects are refused.
Unowned native entries publish neither active work nor a deferred event. Valid
locked entries retain the Layer lifetime until return; their validation errors
wake the driver. The snapshot section performs bounded reads and local copies; graphics and
notification happen after `Permit()`. It makes no SMP or CPU-hotplug guarantee.

These are host lifetime, boundary and replay tests. They do not prove native
AROS scheduling, real driver pixels, a visible application window, editing or
saving. Those require separate target tests. Linux sanitizer runs retain leak
detection; macOS AddressSanitizer does not support LeakSanitizer, so exact native
allocation bookkeeping remains active there.
