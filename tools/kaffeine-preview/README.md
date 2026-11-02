# Kaffeine community preview tools

This is preparation for an experimental **AROS ARM64 / QEMU raspi3b**
tester package. **There is no downloadable runtime preview in this change.**
The commands need a separately built, audited and qualified payload with a
`manifest.json`. The source tree and existing demonstration disks are not
such a payload. The native installer for arbitrary AROS installations is
deferred: Kaffeine also depends on changes in native DOS and boot components.

## Tester workflow (once a qualified payload is released)

Install Python 3.9 or newer and QEMU with `qemu-system-aarch64`, then extract
the preview ZIP into a new folder. QEMU is installed separately and is not
bundled. On Windows use `py -3` instead of `python3` in these examples, and
give `--qemu` the installed executable's path if it is not on PATH.

With Python and QEMU on PATH, double-click **Start.command** on macOS or
**Start.cmd** on Windows. On Linux use `sh Start.command`. This checks the
package and starts a new session under `sessions/` in the extracted folder.
The launcher window stays open to show errors. Terminal commands provide
the same workflow with a session location of your choice:

```sh
python3 preview.py verify .
python3 preview.py run --session ../kaffeine-test-01
```

Each session starts with a verified copy of the supplied raw SD disk. The
original stays unchanged. The session folder must not already exist; keep
it to retain drawings or documents from the test. There is no automatic
host folder sharing, application download or guest networking. To remove
the preview, close its VM and remove its extracted folder and your session
folders. No native AROS system files or startup scripts are installed.

The launcher uses one TCG translation thread. The board exposes four
virtual CPUs because that is the raspi3b hardware configuration; this is
separate from Kaffeine's guest CPU backend. The default session limit is
15 minutes (`--minutes 1` through `--minutes 60` are available). Close the
application and the QEMU window when finished. Ctrl-C in the launcher also
stops its own QEMU process. A timeout or a log exceeding 32 MiB triggers a
stop; the latter threshold is checked every 0.2 seconds, not a hard file
quota. Keep a copy of any test work you want to retain before the limit.

### Import an application you want to test

Use a separate directory containing only the application files you want to
copy. Optional import needs **mtools** on the host; QEMU alone is enough for
the included smoke test. Install mtools separately and specify its `mcopy`
executable if it is not on PATH:

```sh
python3 preview.py run --session ../test-my-app --import-app ../MyApplication --mcopy /path/to/mcopy
```

The selected directory is copied into
`SYS:KaffeinePreview/Applications/Imported` on the **new session disk before
QEMU starts**. Its contents are staged privately first, and host folders are
not shared with the running guest. The package's original disk is unchanged.
Import accepts regular files and directories, up to 256 MiB, 4096 files and
8192 total entries, with at most 16 directory levels. Names must use ASCII
letters, digits, spaces or `_.+()-`, without trailing spaces/dots or Windows
device aliases. Links, case-insensitive filename collisions and non-preview
disk layouts are refused. A failed import stops before launching QEMU.
The local `import.log` is excluded from automatic reports.

Inside AROS, open the imported drawer or launch the program from a Shell.
Import copies files; follow the application's own instructions for assigns,
extra libraries and launch options. There is no automatic application
installation or download. Use a fresh session to test a different app.

### Select the actual 68k backend inside AROS

In the preview desktop, open the Kaffeine disk and its **KaffeinePreview**
drawer. **Test-JIT** and **Test-Moira** run the original included 68000 test:
save a text file, close it, reopen it and compare every byte. The scripts
show the requested backend; the library supplies actual backend diagnostics
in the session UART logs. **Results** retains the test's output and file.

Open an AROS Shell. For example, with your own compatible 68k application:

```text
Stack 1048576
Set KAFFEINE_CPU jit
Work:MyApplication
```

Repeat in the same Shell with `Set KAFFEINE_CPU moira` for the interpreter
comparison. `Unset KAFFEINE_CPU` restores the default selection unless a
global `ENV:` setting exists. Do not use `SetEnv SAVE` for a test.
GUI icon launches can inherit a different environment; record that launch
method separately. The launcher does not pretend that a requested mode
proves JIT use: preserve the actual backend message and, when available,
JIT counters from the same invocation.

Use a new test document, save it, close the application, reopen it and
check the content. Record the application version, 68k CPU/FPU variant,
screen resolution and color depth, steps, expected/actual result and
whether the same case works with Moira. A failure of one application does
not by itself demonstrate a JIT defect.

### Create a report you can inspect before sharing

```sh
python3 preview.py report ../kaffeine-test-01 ../report-01.zip --result success
python3 preview.py report ../kaffeine-test-01 ../report-01-logs.zip --result failure --include-logs
```

The default ZIP contains build identifiers, host OS/CPU type, launcher
termination information and your chosen result. It contains no logs.
`--include-logs` adds only the last 64 KiB of each of `serial.log`,
`serial-aux.log` and `qemu.log` if present. Both UART logs matter: bootstrap
and AROS diagnostics can use different UARTs. These tails can omit earlier
backend lines; quote the observed line separately if necessary.

**Read the ZIP before attaching it to an issue.** Logs are not automatically
redacted and may contain application text or paths. You may edit/remove
private text before sharing and state what was removed. No command uploads
anything. Disks, screenshots, application executables, environment variables
and arbitrary files in the session are never automatically collected.

Use the repository's **Kaffeine community test** issue form. The result
choices are `success`, `failure`, `unsupported`, `timeout`, `not-tested`.
QEMU returning zero is only launcher evidence, not application acceptance.
Include the QEMU version from `qemu-system-aarch64 --version` in the form.

## Maintainer staging and assembly

The initial release lane is a clean, matched ARM64 preview image. Native
installation can follow for specifically identified compatible AROS builds;
library version `1.0` alone is not a sufficient compatibility identifier.

In a new staging directory place only the intended kernel, BSP, DTB, raw
disk, matching source archive and applicable license notices. Use portable
relative names. Create `manifest.json` with these exact top-level keys:

```json
{
  "schema": 1,
  "build_id": "kaffeine-arm64-preview-001",
  "source_commit": "FULL_40_CHARACTER_SOURCE_COMMIT",
  "target": "aros-aarch64-raspi3b",
  "kaffeine_library_sha256": "SHA256_OF_THE_LIBRARY_INSTALLED_INSIDE_THE_DISK",
  "files": [
    {"path": "payload/kernel.img", "role": "kernel", "bytes": 1, "sha256": "ACTUAL_SHA256"},
    {"path": "payload/bsp.rom", "role": "bsp", "bytes": 1, "sha256": "ACTUAL_SHA256"},
    {"path": "payload/raspi3b.dtb", "role": "dtb", "bytes": 1, "sha256": "ACTUAL_SHA256"},
    {"path": "payload/clean.img", "role": "disk", "bytes": 2147483648, "sha256": "ACTUAL_SHA256"},
    {"path": "sources/matching-sources.zip", "role": "sources", "bytes": 1, "sha256": "ACTUAL_SHA256"},
    {"path": "licenses/NOTICES.txt", "role": "notice", "bytes": 1, "sha256": "ACTUAL_SHA256"}
  ]
}
```

This is a template, not an accepted manifest. Replace all placeholders and
sizes with measured values. Each boot/source role occurs exactly once;
multiple `notice` files are allowed. The raw SD image size must be a power
of two for QEMU's SD emulation. Links, duplicate names (including case
variants), Windows device names and parent paths are refused.

```sh
python3 tools/kaffeine-preview/preview.py verify /path/to/staging
python3 tools/kaffeine-preview/preview.py assemble /path/to/staging /path/to/preview.zip
```

Assembly includes only the manifest-listed payload, `manifest.json`, this
README, the Python launcher, both start scripts and `SHA256SUMS`. Unlisted
staging files are omitted.
The command prints an outer ZIP SHA256 to publish alongside the release.
Checksums detect changed contents; they do not certify license compliance,
source correspondence, the library inside the disk or application behavior.
Verify the outer checksum through the project's release page before use.

## Release acceptance, separate from tool tests

Before uploading a **prerelease** for a small first group of testers:

1. Build from a recorded public commit and exact dependency revisions. Keep
   the full matching sources, dependencies, build instructions and notices
   available alongside the binaries. Review the actual image inventory,
   including the open generated-table/NDK provenance point in
   [Licenses and provenance](https://github.com/kaffeine1/kaffeine/blob/master/docs/kaffeine/LICENSING.md).
2. Create a fresh image from reviewed redistributable inputs. Include the
   matched DOS/loader changes, kernel, BSP, DTB and `kaffeine.library`.
   Extract the library back out and verify its recorded hash. Do not reuse
   a demonstration/test disk. Exclude private files, old logs, saved personal
   documents and applications without redistribution permission.
3. Add clearly named launch instructions and a small redistributable 68k
   smoke test, including its source and license. Make application import
   optional; do not bundle Personal Paint or TurboText by assumption.
4. Extract the assembled ZIP into a new directory. Test its own launcher,
   a 24-bit desktop boot, both UARTs, actual Moira/JIT selection, smoke test exit,
   document save/close/reopen, shutdown, timeout and report contents. Bind
   the result to this exact manifest hash and QEMU version. Qualify each
   advertised host OS separately. Tool CI uses stand-ins and proves none
   of these VM/application results.
5. Record known limits and publish the ZIP, checksum and matching sources
   as a prerelease. Collect the first small pilot's reports before widening
   the audience. Turn confirmed minimal failures into regressions and
   focused AROS mainline contributions; upstream integration remains the
   goal. Generic native AROS installation and hardware support are later,
   separately qualified delivery lanes.

QEMU references: [raspi boards and UARTs](https://www.qemu.org/docs/master/system/arm/raspi.html),
[invocation and single-thread TCG](https://www.qemu.org/docs/master/system/invocation.html).

## Offline tool validation

```sh
python3 -I -B tools/kaffeine-preview/test_preview.py
```

The tests use synthetic payloads and a short stand-in process. They do not
start QEMU, compile a runtime or establish application compatibility.
