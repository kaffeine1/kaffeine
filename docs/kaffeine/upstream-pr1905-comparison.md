# Upstream m68kemu PR #1905 and Kaffeine

Comparison recorded **28 October 2026**. Kaffeine derives from Fabian
Schmieder's `m68kemu.library`; the upstream continuation by `terminills`
addresses much of the same compatibility work. The useful next step is
to converge contracts and share focused fixes and regression tests.

## Upstream follow-up, 29 October 2026

The checked guest-memory span fix was merged upstream in
[AROS #1922](https://github.com/aros-development-team/AROS/pull/1922).
The review follow-up was merged in
[AROS #1943](https://github.com/aros-development-team/AROS/pull/1943): the
remaining thunk/shadow validation TODO stays explicit, and the independent
511-check bounds regression now runs through the host Makefile's normal
`test` target, with a separate sanitizer target. The ordinary and ASan/UBSan
bounds runs and the complete m68kemu host suite passed before submission.
The comparison below remains a record of the revisions inspected on 28 October.

## Revisions and evidence scope

| Input | Revision |
| --- | --- |
| Upstream [PR #1905](https://github.com/aros-development-team/AROS/pull/1905) | [`e016d6fb7999eb3c7962f078e5ff75d50f957079`](https://github.com/terminills/AROS/commit/e016d6fb7999eb3c7962f078e5ff75d50f957079), branch `m68kemu-update`; open when checked |
| Its included DOS dependency, [PR #1855](https://github.com/aros-development-team/AROS/pull/1855) | [`e978e8e3e9244fa1284f77b6af1279f68f09ec48`](https://github.com/terminills/AROS/commit/e978e8e3e9244fa1284f77b6af1279f68f09ec48); open when checked |
| Kaffeine runtime baseline | [`6fc72c263fa64341c517e4b6d3ec419bd59ae926`](https://github.com/kaffeine1/kaffeine/commit/6fc72c263fa64341c517e4b6d3ec419bd59ae926) |

The comparison uses all **30 changed files** in the PR, plus selected
unchanged CPU, generator and test sources. It concentrates on implementation
contracts and selected executable probes; every generated structure offset
has not been independently audited.

The [offline comparison tool](../../developer/debug/test/kaffeine_upstream_compare/README.md)
compiles selected upstream functions verbatim with fixture contexts and
native collaborators, and compares accessor/private-pool behavior with
Kaffeine. Its **28 observations** include positive controls and **13
upstream boundary failures across five areas**. Those are selected cases,
not a compatibility percentage. See the [structured receipt](upstream-pr1905-results.json).

The probes use ASan/UBSan with leak detection disabled. They are not a full
upstream library build, Moira instruction run or AROS VM test. Seven focused
Kaffeine host executables were rebuilt and passed separately. No upstream
AROS image or application was executed in this comparison; application
results reported in the upstream PR remain the author's target evidence.

## What overlaps and what is useful to reuse

| Area | Upstream PR | Kaffeine baseline | Direction |
| --- | --- | --- | --- |
| Loading and relocation | Captures DOS relocations and rebases copied hunks; recognizes additional HUNK records | Already captures/rebases relocations, with rollback and loader fixtures | Align the public relocation ABI first; exchange fixtures instead of maintaining two incompatible formats |
| Native handles and shadows | BPTR handle mapping, object shadows, library bases and returned-pointer conversion | Typed handle/ownership adapters, checked copies and focused lifetime tests | Review conversion rules together; preserve identity and stale-handle tests when adopting code |
| Guest heap | Separate Chip/Fast regions, ROM header and guest-visible free chunks | Native-owned allocator metadata and capacity-only MemList descriptors | Reuse the compatibility requirements; the allocator implementations require an explicit reconciliation |
| Windows, drawing, screens, menus and gadgets | Extensive native twins, planar drawing, palette and GadTools work | Similar API coverage, with ARM framebuffer/indexed-screen and layer tests | Run common fixtures on both; compare actual structures, pixels and cleanup |
| BOOPSI and ReAction | Native class/object bridge and classes implemented in 68k | General BOOPSI/ReAction coverage remains open | Strong reuse candidate after ownership, tag, method and callback tests |
| DOS child processes | `CreateNewProc` starts a separate CPU sharing an arena and a native semaphore | `CreateProc`/`CreateNewProc` return an unsupported error; `AddTask` has a cooperative scheduler | Study the upstream process implementation as a unit; it cannot replace the scheduler independently |
| Devices | Native IORequest twins for several devices and asynchronous paths | Bounded timer/console contracts, with general asynchronous I/O refused | Adapt by device and completion contract, retaining reply-port and cleanup tests |
| ARexx | Dedicated `rexxsyslib` adapters | General ARexx compatibility remains open | Reuse candidate with ownership/length/message tests |
| CPU and callbacks | Moira, per-depth callback CPUs, cache/MOVE16/VBR handling | Backend interface, guarded callbacks, cooperative guest CPUs and a separate AArch64 JIT probe | Keep the JIT work distinct; share instruction/callback fixtures and assess CPU-model differences |
| Diagnostics | Call tracing and retained call history | Focused fault reports, host/native/target fixtures | Adopt useful diagnostics independently of runtime semantics |

The concrete reuse entry points are the upstream
[BOOPSI/ReAction implementation][up-boopsi], [process and message code][up-tasks],
[device adapters][up-devices] and [ARexx bridge][up-rexx]. Their native
handles, callback execution and cleanup depend on other parts of that
branch. Importing isolated functions without those contracts would lose
important context.

## Resolve the relocation ABI before mixing components

Both forks use **`GSLI_Dummy + 7`**, but their payloads differ:

| Contract | Kaffeine | Upstream proposal |
| --- | --- | --- |
| Tag spelling | `GSLI_68KRelocs` | `GSLI_68KRELOCS` |
| Table | `hrt_NumHunks`, `hrt_NumRelocs`, record pointer | `hrt_Count`, record pointer |
| Relocation record | Offset plus 16-bit source/target/type fields; 12 bytes | Four 32-bit fields for source, offset, target and type; 16 bytes |
| Failure representation | The Kaffeine loader's recorded-table contract | Count `0xFFFFFFFF` marks a lost relocation list |

Sources: [Kaffeine tag][our-tag], [Kaffeine records][our-records],
[upstream tag and records][up-tag]. This is a data-layout incompatibility,
not just a spelling difference. DOS and the bridge must use matching
producers and consumers. Agree on the upstream public contract, adapt the
Kaffeine producer/consumer together, and test native word sizes, relocation
kinds, failed recording and lifetime through `UnLoadSeg`.

Upstream's [RunCommand lifecycle fix, PR #1876](https://github.com/aros-development-team/AROS/pull/1876),
was already merged when checked. Kaffeine has an argument injection and
flush correction for the same problem. The pinned #1905 head still contains
the earlier RunCommand path; integration must use current mainline and
avoid proposing that fix again.

## Five focused fix packages to discuss upstream

### C1: whole-access bounds without 32-bit addition overflow

The upstream [16/32-bit accessors][up-access] test `addr + width - 1` before
forming a host pointer. With 32-bit `ULONG`, addresses near `UINT32_MAX`
wrap that test. Four probes, covering reads and writes at `0xFFFFFFFF`
or `0xFFFFFFFE`, reach an invalid native access under ASan. Positive and
last-byte/partial-access controls behave normally. The upstream CPU's
word/reset/disassembly callbacks contain the same style of check.

Kaffeine's [accessors][our-access] check `addr >= mem_size` and the remaining
length before adding. Its four wrapping cases pass. Port the whole-span
check while preserving upstream's low-vector write guard and CPU-specific
special addresses. Share [test_guest_bounds.cpp][test-bounds], including
small arenas, partial accesses and wrapping values.

### C2: distinguish custom registers from valid RAM

Upstream's [custom mask][up-mask] is `0x00FFF000`. Its
[word-read callback][up-read] therefore treats valid RAM at `$1DFF016` as
the POTGOR register at `$DFF016`. With RAM initialized to `0xA5`, the probe
reads `0xFF00` instead of `0xA5A5`; the actual register control is correct.

The [Fast-region free list][up-regions] also spans the original custom
register page. A valid 12 MiB Fast allocation in the probe returns
`$380000` and includes `$DFF000`, whose CPU reads/writes are intercepted.
Reserve the original register page and use a full 32-bit decoder mask.
Kaffeine's guest-bounds/custom-window and allocator tests already cover
these contracts. Keep upstream's separately reserved ROM region intact.

### C3: reject allocation-size overflow before alignment

The upstream [heap allocator][up-heap] rounds `size + 3` and adds its
header without rejecting overflow. In the isolated free-region fixture,
`HeapAlloc(UINT32_MAX)` returns `$600` and consumes eight bytes instead of
failing. A normal 16-byte allocation passes its positive control.

Add checked size/alignment/header arithmetic before changing free lists.
Share the large-request and unchanged-on-failure cases from
[test_exec_memory.cpp][test-memory]. Preserve the distinction between the
internal heap's intentional zero-payload allocations and public Exec
calls that return NULL for zero bytes.

### C4: validate a private MemHeader before changing its chain

The upstream [Allocate/Deallocate adapters][up-memheader] accept the four
tested invalid cases: an overflowing allocation size, a self-referencing
successor, a free overlapping an existing chunk and a free outside the
declared pool. Depending on the case they return a block for an impossible
request or change links/free counts. A normal allocate/free control passes.
Kaffeine refuses all four invalid cases without changing guest bytes.

Port the checks and interval-oracle tests from
[kaffeine_memheader.c][our-memheader] and [test_memheader.cpp][test-memheader],
then run [test_memheader_moira.cpp][test-memheader-moira] through real Exec
LVOs. These corrupted-input checks are bridge hardening, not claims that
classic Exec defines behavior for corrupt lists or overlapping frees.
Upstream also uses MemHeaders for its own Chip/Fast allocator: retain that
integration instead of copying Kaffeine's capacity-header exclusions blindly.

### C5: normalize classic library-version arguments at the ABI boundary

With a cached library at version 2, the upstream
[OpenLibrary thunk][up-open] rejects D0=`0x12340002`. Classic callers may
initialize D0.W alone. Its `OldOpenLibrary` delegates to the same thunk
and also rejects the cached library when D0 contains an unrelated value.
Both observations are reproduced with native/class lookups declining the
name and the already-loaded 68k cache populated; ordinary version 2 passes.

Kaffeine's [entry thunks][our-open] normalize `OpenLibrary` to a `UWORD`
and give `OldOpenLibrary` a zero version. Propose that small ABI correction
and the [version-low-word fixture][test-libraries]. The portable
[exec_libraries.c][target-libraries] also exercises the low-word case.
Keep internal loader APIs and their full-width version parameters separate.

## Additional source findings and tests worth sharing

- **ExAll hooks:** upstream explicitly sets `eac_MatchFunc` to NULL in
  [its ExAll adapter][up-exall]. Kaffeine supports guest match hooks and
  nested DOS calls. Share the nine ExAll groups in
  [test_dos_thunks.cpp][test-dos] and [dos_exall.c][target-exall], then adapt
  callback handling. This comparison did not execute upstream ExAll.
- **Disk library lifecycle:** share the loader fixtures for WORD/LONG
  function tables, InitStruct alignment, init/Open/Close/Expunge, seglist
  ownership, allocation failure and callbacks. All **139 cases / 11,251
  checks** in Kaffeine's disk-library fixture passed in this comparison.
- **LVO/register audit:** adapt [test_thunk_registrations.py][test-registers]
  to upstream table names and its module layout. This audit was not run on
  the PR with an assumed rename; its new tables need an explicit mapping.
- **DOS messages and handlers:** share [dos_packets.c][target-packets] and
  [dos_handlers.c][target-handlers], with native/68k results for port
  identity, packet ownership, D0/D1/IoErr and FileHandle conversions.
- **Graphics on ARM:** share the native-versus-68k fixtures for
  [clipped blits][target-blit], [IntuiText widths][target-itext],
  [screen palettes][target-palette] and [LayerInfo backfill modes][target-layer].
  The [native framebuffer palette probe][target-fb] covers a driver change
  outside #1905's 30 files. Verify target offsets: the [upstream build notes][up-build]
  say that its checked-in shadow table was generated for x86_64.
- **Math:** exchange the FFP/IEEE and utility corpora with register/CCR
  checks and native references. Kaffeine still records eight one-ULP SPDiv
  differences from AmigaOS; that corpus must not be presented as a perfect
  numerical oracle.

The upstream host smoke tests do exist under
[`developer/debug/test/m68kemu_host`][up-tests]; its README still names their older
location. Their Makefile and source includes retain `../../rom/m68kemu`
paths. Include build-path/fixture maintenance in any test contribution.
This comparison did not build that existing upstream suite.

The seven Kaffeine executables rebuilt and passed here are `test_guest_bounds`,
`test_exec_memory`, `test_memheader`, `test_memheader_moira`,
`test_utility_thunks`, `test_hunk_loading` and `test_m68klib`. Their native
API collaborators are fixtures. Earlier [Personal Paint ARM64 evidence](personal-paint-demo.md)
is a separate target result and has not been transferred to the upstream branch.

## Suggested integration order

1. Share C1, C2 and C5 as small fixes with reproducing tests; follow with
   C3/C4 while agreeing on guest allocator ownership and MemHeader semantics.
2. Agree on the relocation record API and migrate both DOS and the bridge
   together. Test Shell/Run argument lifetime against current mainline.
3. Establish common 68k/native target fixtures, then evaluate BOOPSI/ReAction,
   ARexx, device I/O and child-process reuse one contract at a time.
   Revalidate AArch64 offsets, teardown and blocked-child lifetime.
4. Run the same Personal Paint draw/save/exit scenario on the resulting
   AROS ARM64 build. Preserve the known image-loading limitation until a
   new test demonstrates its resolution.
5. Continue the Kaffeine JIT behind the CPU boundary with Moira as reference.
   Its standalone native probe does not yet establish Personal Paint or
   full guest-task execution on the JIT.

These are contribution candidates and integration decisions. This report
does not change the runtime bridge or submit fixes to upstream.

[up-boopsi]: https://github.com/terminills/AROS/blob/e016d6fb7999eb3c7962f078e5ff75d50f957079/rom/m68kemu/m68kemu_boopsi.c
[up-tasks]: https://github.com/terminills/AROS/blob/e016d6fb7999eb3c7962f078e5ff75d50f957079/rom/m68kemu/m68kemu_tasks.c
[up-devices]: https://github.com/terminills/AROS/blob/e016d6fb7999eb3c7962f078e5ff75d50f957079/rom/m68kemu/m68kemu_devices.c
[up-rexx]: https://github.com/terminills/AROS/blob/e016d6fb7999eb3c7962f078e5ff75d50f957079/rom/m68kemu/m68kemu_rexx.c
[up-build]: https://github.com/terminills/AROS/blob/e016d6fb7999eb3c7962f078e5ff75d50f957079/rom/m68kemu/mmakefile.src#L74
[up-tests]: https://github.com/terminills/AROS/blob/e016d6fb7999eb3c7962f078e5ff75d50f957079/developer/debug/test/m68kemu_host/Makefile
[up-tag]: https://github.com/terminills/AROS/blob/e016d6fb7999eb3c7962f078e5ff75d50f957079/compiler/include/dos/dostags.h#L182
[our-tag]: https://github.com/kaffeine1/kaffeine/blob/6fc72c263fa64341c517e4b6d3ec419bd59ae926/compiler/include/dos/dostags.h#L182
[our-records]: https://github.com/kaffeine1/kaffeine/blob/6fc72c263fa64341c517e4b6d3ec419bd59ae926/compiler/include/dos/doshunks.h#L84
[up-access]: https://github.com/terminills/AROS/blob/e016d6fb7999eb3c7962f078e5ff75d50f957079/rom/m68kemu/m68kemu_intern.h#L350
[our-access]: https://github.com/kaffeine1/kaffeine/blob/6fc72c263fa64341c517e4b6d3ec419bd59ae926/rom/kaffeine/kaffeine_intern.h#L443
[up-mask]: https://github.com/terminills/AROS/blob/e016d6fb7999eb3c7962f078e5ff75d50f957079/rom/m68kemu/m68kemu_offsets.h#L235
[up-read]: https://github.com/terminills/AROS/blob/e016d6fb7999eb3c7962f078e5ff75d50f957079/rom/m68kemu/m68kemu_moira.cpp#L308
[up-regions]: https://github.com/terminills/AROS/blob/e016d6fb7999eb3c7962f078e5ff75d50f957079/rom/m68kemu/m68kemu_memory.c#L64
[up-heap]: https://github.com/terminills/AROS/blob/e016d6fb7999eb3c7962f078e5ff75d50f957079/rom/m68kemu/m68kemu_memory.c#L283
[up-memheader]: https://github.com/terminills/AROS/blob/e016d6fb7999eb3c7962f078e5ff75d50f957079/rom/m68kemu/m68kemu_thunks.c#L5388
[our-memheader]: ../../rom/kaffeine/kaffeine_memheader.c
[up-open]: https://github.com/terminills/AROS/blob/e016d6fb7999eb3c7962f078e5ff75d50f957079/rom/m68kemu/m68kemu_thunks.c#L156
[our-open]: https://github.com/kaffeine1/kaffeine/blob/6fc72c263fa64341c517e4b6d3ec419bd59ae926/rom/kaffeine/kaffeine_thunks.c#L208
[up-exall]: https://github.com/terminills/AROS/blob/e016d6fb7999eb3c7962f078e5ff75d50f957079/rom/m68kemu/m68kemu_thunks.c#L2432
[test-bounds]: ../../developer/debug/test/kaffeine_host/test_guest_bounds.cpp
[test-memory]: ../../developer/debug/test/kaffeine_host/test_exec_memory.cpp
[test-memheader]: ../../developer/debug/test/kaffeine_host/test_memheader.cpp
[test-memheader-moira]: ../../developer/debug/test/kaffeine_host/test_memheader_moira.cpp
[test-libraries]: ../../developer/debug/test/kaffeine_host/test_m68klib.cpp
[test-dos]: ../../developer/debug/test/kaffeine_host/test_dos_thunks.cpp
[test-registers]: ../../developer/debug/test/kaffeine_host/test_thunk_registrations.py
[target-libraries]: ../../developer/debug/test/kaffeine_target/exec_libraries.c
[target-exall]: ../../developer/debug/test/kaffeine_target/dos_exall.c
[target-packets]: ../../developer/debug/test/kaffeine_target/dos_packets.c
[target-handlers]: ../../developer/debug/test/kaffeine_target/dos_handlers.c
[target-blit]: ../../developer/debug/test/kaffeine_target/gfx_blit_clip.c
[target-itext]: ../../developer/debug/test/kaffeine_target/gfx_intuitext_width.c
[target-palette]: ../../developer/debug/test/kaffeine_target/gfx_screen_palette.c
[target-layer]: ../../developer/debug/test/kaffeine_target/gfx_layerinfo_mode.c
[target-fb]: ../../developer/debug/test/kaffeine_target/gfx_framebuffer_palette_native.c
