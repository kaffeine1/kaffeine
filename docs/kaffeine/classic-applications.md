# Classic application checks on AROS ARM64

The unmodified 68k programs run through `kaffeine.library` on AROS ARM64.
Moira and the Kaffeine JIT are tested separately. Application binaries and
private test disk images are not distributed here.

| Program | Verified behavior | Scope and next check |
| --- | --- | --- |
| Redit 2.0 | Edit, save, quit, reopen, edit again and save with both Moira and JIT; extracted text files match the entered content | Both launches return 0; the JIT cycle was repeated after the full-index change. Further editor features remain unverified |
| Directory Opus 4.12 JR | Both Moira and JIT: opens on Workbench using ordinary preferences, copies a 73-byte text file and a 1024-byte binary file, renames the text, quits normally, reopens and lists both persisted files; extracted bytes match the sources. Shell and Run return 0 | The clock/date fields still display invalid values. Broader file operations and application features remain unverified |

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
