# Classic application checks on AROS ARM64

The unmodified 68k programs run through `kaffeine.library` on AROS ARM64.
Moira and the Kaffeine JIT are tested separately. Application binaries and
private test disk images are not distributed here.

| Program | Observed behavior | Scope and next check |
| --- | --- | --- |
| Redit 2.0 | Edit, save, quit, reopen, edit again and save with both Moira and JIT; extracted text files match the entered content | Both launches return 0; the JIT cycle was repeated after the full-index change. Further editor features remain unverified |
| Directory Opus 4.12 JR | Both Moira and JIT: opens on Workbench using ordinary preferences, copies a 73-byte text file and a 1024-byte binary file, renames the text, quits normally, reopens and lists both persisted files; extracted bytes match the sources. Shell and Run return 0 | The clock/date fields still display invalid values. Broader file operations and application features remain unverified |
| TurboText 2.0 | Original Aminet launcher and support libraries tested with both Moira and JIT; Shell and Run return 20 with `Could not start TurboText - function not implemented` after `CreateNewProc()`. A bounded trace identifies the requested CLI process and its loaded-program handle | Startup is blocked; no editor window, editing or save/reopen acceptance. Needs typed process startup, module ownership and Process identity support |
| AmiPixelShop 0.6.1-beta | Original release tested as `AmiPixelShop 64 64` with both backends; Shell and Run return 20 before opening the UI. Startup calls Exec `Alert()` with D7 `$00068881` | Startup is blocked. Inline FPU support is absent; P96 and screen-mode requester requirements are additional source-level gaps |

## Additional startup checks (2026-10-29)

[TurboText 2.0](https://aminet.net/package/text/edit/TurboText20) is the
original freeware distribution by Martin Taillefer, written using M2Sprint
Modula-2. Its readme permits redistribution only as an unchanged archive;
the program and libraries are not included here. The readme recommends
manual installation because its old Installer script has compatibility
problems. Copy `Disk/Data` to an application drawer, then on the test system:

```text
Assign TurboText: Work:TurboText
Copy TurboText:Libs/turbotext.library LIBS:
Copy TurboText:Libs/ttxreqs.library LIBS:
Copy TurboText:Libs/ttxsupport.library LIBS:
CD TurboText:
TTX RAM:document.txt
```

The bundled `asl.library` and `amigaguide.library` were left in the original
drawer; the test system's native system libraries were retained. The
bridge successfully opens `TurboText:Libs/ttxsupport.library` as 68k code.
Its startup then calls DOS `LoadSeg()` and `CreateNewProc()` (LVO -498),
which currently returns `ERROR_NOT_IMPLEMENTED`. Both backends produce
the same result in each invocation mode.

`CreateNewProc()` requires more than translating its tags. Guest entry
points cannot execute as ARM code. Existing `CreateProc()` children have
their own CPU and Process, but DOS calls other than `Exit()` are refused:
streams, directories, locks, CLI and packet state currently belong to the
root DOS owner. Root termination also destroys the children and their shared
arena. A DOS-using helper needs ownership, rollback and group-lifetime
rules before it can safely run.

The bounded startup trace now identifies ten active tags in the original
launcher's Shell and Run calls:

| Tag | Observed value or relationship |
| --- | --- |
| `NP_StackSize` | 5000 bytes |
| `NP_Seglist` | The exact D0 returned by the preceding native `LoadSeg()` |
| `NP_FreeSeglist` | `TRUE` |
| `NP_Name`, `NP_CommandName` | The same nonzero pointer; strings are not printed or followed by the trace |
| `NP_ConsoleTask`, `NP_WindowPtr` | `NULL` |
| `NP_CopyVars` | `FALSE` |
| `NP_Cli` | `TRUE` |
| `NP_HomeDir` | A nonzero BPTR; the trace does not dereference it |

No `NP_Entry` or `NP_Arguments` tag is present. Each list terminates after
ten items, within the diagnostic's 64-step and 16-output-item limits.
The loader trace records its native BPTR at full width and the delivered
32-bit D0 without reading segment contents or printing filenames. Logging
and converted-tag cleanup preserve the loader's IoErr.

This establishes a request to start the program loaded by native DOS.
It does not make the native
BPTR a safe guest process handle. The next implementation needs a trusted
loaded-module registry, explicit transfer/unload ownership, typed directory
and CLI setup, and a guest representation of the resulting Process. A
native 68k-module route must also guarantee emulator entry without a native
code fallback. These are separate from the existing shared-arena service
processes. The startup trace does not enable either process route, and
TurboText still returns 20 without opening an editor window.

[AmiPixelShop 0.6.1-beta](https://github.com/radi7777/AmiPixelShop/releases/tag/v0.6.1-beta)
is the untouched 68040/FPU release, using source tag
`84f224e4ce1ad83e5d3c74addd12268ddc8511df`. The `64 64` arguments choose a
small canvas, not the screen resolution. Tests used a 1024x768 AROS desktop,
the current 32-MB guest arena and a VM with networking disabled.

The first observed blocker is an Exec `Alert()` call (LVO -108), before
any P96 call or inline-FPU instruction fault. Its input D7 is `$00068881`
with both backends, consistent with the release's FPU requirement and the
bridge's FPU-free `AttnFlags`. The application returns 20 and writes no
application output; the improved unsupported-call diagnostic exposes this
input without changing the existing zero return or calling native Alert.

The [official sources](https://github.com/radi7777/AmiPixelShop/tree/84f224e4ce1ad83e5d3c74addd12268ddc8511df)
also establish later requirements that these failed launches did not test:

- `Picasso96API.library` v2, mode selection and `p96WritePixelArray()` with
  a classic `RenderInfo`. A CyberGraphics library-name alias cannot translate
  that interface.
- Inline FPU math, including transcendental operations, while initializing
  pixels and doing normal editing. Native math-library thunks do not provide
  an instruction-level FPU.
- A screen-mode requester with a guest filter hook on first start. A later
  minimal probe can use a valid saved RTG mode in `ENV:AmiPixelShop.prefs`.
- A requested 256-KB stack. The documented memory recommendation is 128 MB;
  there is no source-level 128-MB startup gate, so the current failure does
  not establish a memory-shortage diagnosis.

For a later offline GUI probe, preferences `mode=0x<valid RTG mode ID>` and
`updcheck=0`, on separate lines, avoid the mode requester and updater child.
They do not resolve the observed FPU/startup blocker. The small-canvas test
does not validate a complete document workflow or the documented memory floor.

The downloaded executable SHA-256 values identify the tested originals:

| File | SHA-256 |
| --- | --- |
| TurboText `TTX` | `262399d51c98c3c8498c12e2face18f31c79eb99e68a7a7d302b6b746b6d76c3` |
| AmiPixelShop | `71db861244ad496aa7cfa4f0c99cabe42f4cf3579ab77ab2a167affa64573a71` |

## Existing bridge regressions

The FileLock regression `dos_locks` passes 22 checks in each Shell and Run
invocation on native ARM64, Moira and JIT. It covers readable FileLock fields,
object/volume identity, directory and program-directory swaps, a bound assign,
DevProc, ParentDir, file-handle conversion and repeated acquisition/release.
The earlier bridge failed 7 of the original 18 checks; its opaque native lock
values made Directory Opus mistake two different drawers for the same object.

The production lock/heap host test passes 179 checks, including ASan/UBSan.
The complete host suite passes 48 executables. Handler and packet suites also
pass with ASan/UBSan. These are separate from the application checks above.

Broader APIs such as DosList manipulation, hard links, ReadLink and native
FontContents remain explicitly refused until their representations and
ownership have typed adapters. Passing these tests does not imply general
compatibility with every classic application or hardware-dependent software.

The post-copy port failure was traced to physical signal exhaustion: the
old bridge used four private native I/O signal bits in addition to the
application's ports/tasks. All native I/O requests now share one private
reply port. The 8-check signal-budget probe passes on native ARM64 and both
backends, while the previous bridge failed after three original opens.

The JIT startup stop was a 68020 full-format indexed `CLR.B` operand in
the original support library. The translator now implements the
non-indirect form, with separate host and AROS target regressions. The
application binary and support library were not patched for these tests.
