# Classic application checks on AROS ARM64

The unmodified 68k programs run through `kaffeine.library` on AROS ARM64.
Moira and the Kaffeine JIT are tested separately. Application binaries and
private test disk images are not distributed here.

| Program | Verified behavior | Remaining check |
| --- | --- | --- |
| Redit 2.0 | Edit, save, quit, reopen, edit again and save with both Moira and JIT; extracted text files match the entered content | Repeat the application cycle after the latest FileLock changes |
| Directory Opus 4.12 JR | Opens on the native public Workbench screen using ordinary preferences; lists separate source/destination drawers; copied a 73-byte text file whose extracted bytes match the source with Moira | A zero-length StandardPacket sent directly with PutMsg is refused during the post-copy metadata step; full copy/rename, orderly exit and JIT application cycle remain open |

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
