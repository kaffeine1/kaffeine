# Typed window refresh scopes

The layer bridge implements Intuition `BeginRefresh()` / `EndRefresh()` and
their GadTools aliases `GT_BeginRefresh()` / `GT_EndRefresh()` with the same
typed adapters. Only these selected manual vectors may enter Intuition or
GadTools while a LayerInfo scope is held. Other synchronous GUI operations,
DOS, guest waits and guest CPU callbacks retain their existing refusals.

One refresh can be active in a Context. It captures a live typed window, its
native screen, main layer and distinct GimmeZeroZero border layer, the native
driver, private guest task identity and CPU. Guest window/layer pointers and
semaphore bytes cannot redirect that ownership. Begin refuses callbacks,
brokers, nested refresh, preheld explicit layer locks, foreign owners and
untracked native refresh/update state.

The bridge attempts the native Info, border and main-layer semaphores without
blocking, in that order. Provisional references stay held until native
`BeginRefresh()` acquires its recursive references. The implicit Info guard is
already active inside that call; the driver cannot switch guest CPUs or enter
a guest callback. Existing explicit Info scopes must have the same private
guest and native owner.

Native `BeginUpdate()` can fail while `BeginRefresh()` retains its outer Info
and layer references. The bridge tracks that begun scope independently of
`WFLG_WINDOWREFRESH`, including when the flag stays clear. A matching End runs
once on the captured owner. Native `EndRefresh()` decides whether `EndUpdate()`
is needed and always releases the outer references. `complete == FALSE` keeps
the native damage state according to that API.

Within refresh, explicit layer locks are limited to the already-held main
layer and must end before normal `EndRefresh()`. New explicit Info scopes and
Info unlocks are refused. `InstallClipRegion()` is refused because its native
implementation can end and restart update without reporting allocation
failure. A validated `InstallLayerHook()` swap on that same captured layer
does not restart update; cross-window hook changes are refused.

Periodic topology snapshots pause while refresh is active. The adapter mirrors
only native refresh/update flag bits; it does not import guest geometry into
the native window or overwrite guest geometry, pending backdrop flags or
RastPort state. After End, ordinary snapshot polling resumes.

Abort cleanup releases explicit inner layer references, calls
`EndRefresh(FALSE)`, then releases outer explicit Info references. This occurs
before the captured window, screen, task or Context can retire. A foreign
native owner cannot unlock the scope or discard its private metadata.

The [host refresh cases](../../developer/debug/test/kaffeine_host/refresh_fixture_tests.h)
use the production dispatcher, layer/window bridges and heap. The fixture
extracts the unchanged checked-in native Intuition Begin/End bodies; recording
layer/update primitives inject contention and allocation failure. It checks
normal and GZZ reference counts, successful/failed update, alias registrations,
owner/nesting guards, guest-field preservation and abort ordering through the
real CPU run loop. The registration audit independently checks the four
vectors and register conventions against the public native entries.

```sh
make -C developer/debug/test/kaffeine_host BUILD_DIR=/tmp/kaffeine-refresh -j1 \
    test_layers test_layers_private
/tmp/kaffeine-refresh/test_layers
/tmp/kaffeine-refresh/test_layers_private
make -C developer/debug/test/kaffeine_host BUILD_DIR=/tmp/kaffeine-refresh-san -j1 \
    CFLAGS="-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer" \
    CXXFLAGS="-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer" \
    LDFLAGS="-fsanitize=address,undefined" test_layers test_layers_private
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    /tmp/kaffeine-refresh-san/test_layers
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    /tmp/kaffeine-refresh-san/test_layers_private
make -C developer/debug/test/kaffeine_host check-thunk-registrations
```

Host/source-body checks, native target execution and application rendering
acceptance are separate evidence lanes. These tests do not establish target
allocation-failure behavior or a visible application result.
