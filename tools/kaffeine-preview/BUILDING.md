<!--
Copyright (C) 2026, The AROS Development Team. All rights reserved.
Author: Michele Dipace (@kaffeine1)
-->

# Building the QEMU preview components

The experimental framebuffer profile builds a separate
`aros-aarch64-qemu-bsp.rom`. It keeps the common Raspberry Pi system modules
and `fbgfx`, and excludes `vcgfx` and its Gallium build dependency. The normal
hardware BSP target retains VideoCore and its existing module list.
This profile is for the first QEMU preview; physical Raspberry Pi support
and 3D graphics need their own qualification.

These instructions prepare components, not a finished distributable image.
Follow the clean-image, source inventory and extracted-ZIP acceptance steps
in the [preview guide](https://github.com/kaffeine1/kaffeine/blob/master/tools/kaffeine-preview/README.md)
before publishing a binary prerelease.

## Build prerequisites

Use a prebuilt AROS AArch64 GNU toolchain, GNU make 4 or newer, Python with
the modules in `requirements-build.txt`, and the host utilities required by
AROS configure. GNU make must also be named `make` on the build's PATH:
MetaMake invokes it recursively. Apple's system make 3.81 cannot generate
the archive response files used by the current build rules. Use an isolated
host-tools directory instead of changing system programs. Bison must have its
matching data directory too: a copied Bison executable alone may look for a
nonexistent relative `share/bison` directory. Pass `BISON=/absolute/path/to/bison`
to make when selecting a complete host installation.

Initialize the repository's pinned submodules before building. In addition
to the Moira core, catalog submodules provide English message definitions
used to generate C headers, even for an English-only image. Missing DOS
catalogs can leave the build using the system `strings.h` instead of the
generated message header.

The initial build was prepared on macOS ARM64 using GCC 10.5.0 targeting
AROS and explicit Command Line Tools SDK paths for host Clang. Configure's
target features must select the new build's Developer sysroot; do not silently
compile against an older installed AROS SDK. Keep the compiler, bootstrap,
BSP, native libraries and `kaffeine.library` provenance together.

## Component targets

From a fresh out-of-source build directory, with your private toolchain
prefix substituted:

```sh
../kaffeine/configure --target=raspi-aarch64 \
  --with-aros-toolchain=yes --with-aros-toolchain-install=/path/to/toolchain \
  --disable-crosstools --disable-ccache
make -j1 tools compiler tools-crosstools-gcc-libatomic
make -j1 kernel-raspi-aarch64 kernel-package-raspi-aarch64-qemu \
  distfiles-raspi-aarch64le-bootimg compiler-stdc compiler-stdcio \
  compiler-posixc kernel-kaffeine-library
```

GNU configure links C++ components with `libatomic`. A prebuilt compiler
does not necessarily include it: the explicit target above builds it from
the matching GCC sources and installs it into the new SDK. Use the
`--with-gcc-version` option matching your compiler (10.5.0 for the initial
build). The three `compiler-*c*` runtime targets are separate from the
`compiler` target's headers and link libraries.

Use the QEMU BSP explicitly as `-initrd`; the normal `config.txt` continues
to select the normal hardware BSP. Keep separate build directories for
hardware distribution and the preview. Build the native desktop and its
runtime libraries as well before assembling a new SD image. Do not copy
the `Developer` tree, build scratch files or an old demonstration disk
into the runtime image.

## Preview startup assets

`guest/Startup-Sequence`, `Test-JIT`, `Test-Moira` and `ReadMe` are the minimal
preview's original startup/help assets. Create their destination drawers
and install them in `S:` and `SYS:KaffeinePreview` respectively. Include the
native desktop libraries, tools, keymaps and fonts required by those files.
The startup selects a requested JIT default in RAM, prepares screen
preferences before IPrefs and opens Wanderer. The two test icons explicitly
select their own backend. They report their requested choice; actual backend
messages and JIT counters come from the library in the session UART logs.

For deliverable builds, map the source prefix in compiler diagnostics using
GCC/Clang's `-ffile-prefix-map=/absolute/source/root=kaffeine`. Preserve each
target's normal compiler flags. Audit the actual runtime inventory for host
paths after linking; stripping debug sections alone does not remove paths
embedded in assertion strings. Retain the useful relative file names.

## Original 68k file smoke test

`guest/PreviewSmoke.s` is a small original 68000 HUNK program with no SDK
header or C-runtime dependency. Assemble it with vasm:

```sh
vasmm68k_mot -quiet -m68000 -Fhunkexe -o PreviewSmoke guest/PreviewSmoke.s
```

The image must create `SYS:KaffeinePreview/Results` first. The program saves
`smoke.txt`, closes it, reopens it and compares its length and every byte.
It returns 0 only after all checks pass, or 20 on failure. It does not claim
which backend executed it; record the actual library backend message and
JIT counters from the same invocation. Validate Shell and `Run` separately.

`guest/PreviewSetup.c` prepares preview-only 1024x768x32 screen preferences
and launch icons. Compile it with the new SDK, and run it after creating
`ENV:SYS` but before starting IPrefs. It writes only RAM preferences and
the preview drawer's icons; it does not change saved system preferences.
After Wanderer opens, `PreviewSetup status` observes the actual public
screen and fails if it is smaller than 1024x768 or below 24-bit color.
The requested mode alone is not proof of the screen that actually opened.

Assembly and cross-compilation are build evidence. A QEMU boot, successful
file comparison and the finished preview's launch/report behavior remain
separate acceptance steps.
