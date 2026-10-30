# Read-only classic VideoControl ViewPortExtra

The fixture compiles complete production memory.c, messages.c, cpu.c,
rastport.c, videocontrol.c, and screens.c. A generated translation unit includes
screens.c unchanged and exposes only its static screen_forget/screen_release
functions for lifecycle checks. No replacement heap, pointer resolver, tag
walker, or Screen retirement implementation is used. Native font/Intuition and
allocation collaborators record ownership; they do not implement VideoControl.

```sh
python3 developer/debug/test/kaffeine_videocontrol_host/run_tests.py --build-dir /tmp/kaffeine-videocontrol
python3 developer/debug/test/kaffeine_videocontrol_host/run_tests.py --sanitize --build-dir /tmp/kaffeine-videocontrol-sanitized
```

Sources default to this repository's rom/kaffeine. --source-dir selects a public
snapshot. Builds are sequential and --build-dir is mandatory. Native test headers
come from sibling kaffeine_host/{gfx_stubs,memory_stubs,stubs}; public
compiler/include/dos/doshunks.h is also required. The fixture targets native
64-bit layout (120-byte native VPE), independently checking classic 66-byte data.
No SDK/application/ROM/private input is needed. Exact commands and output are
saved in the requested build directory. No native/backend/application acceptance
is claimed by these host tests.

Verified ordinary and ASan/UBSan: **24798 checks / 0 failures each**.

Coverage includes exact classic layout and native DisplayClip provenance (with
viewport dimensions deliberately different), pointer sanitization, flags/origins,
stable query identity, native refresh and replacement identity, edited mirrors,
real AllocateTagItems-owned output slots, IGNORE/MORE/SKIP and duplicate GETs,
unknown/SET/cyclic/overlap/out-of-range/overlong whole-list atomic rejection,
record and mirror OOM with full capacity/heap/output rollback, genuine native
NULL VPE, corrupt ColorMap association/header, foreign/unregistered identities,
real CloseScreen refusal/success and stale pointer rejection, public nested and
last-visitor retirement, retention by a Window holder, 4096 quarantined identities,
cleanup without guest memory, full register/CCR-X/IoErr preservation, and the four
shared manual graphics-node refusal routes with zero native graphics calls.

The independent public ABI sources are compiler/include/graphics/view.h,
gfxnodes.h and videocontrol.h; rom/kaffeine/m68k_offsets.txt records the classic
ViewPortExtra size66, ViewPort24, DisplayClip28, VecTable36, DriverData40,
cop1ptr58 and cop2ptr62. Native VideoControl converts GET to SET and replaces
ti_Data. Native GfxNew creates the NT_GRAPHICS/SS_GRAPHICS/VIEWPORT_EXTRA_TYPE
header; native OpenScreen supplies its DisplayClip. No display size is invented.

ASan leak detection is disabled for Darwin portability; native allocations and
exact disposal sizes are independently balanced. The full host Screen suite and
native-source registration checker remain separate integration gates.
