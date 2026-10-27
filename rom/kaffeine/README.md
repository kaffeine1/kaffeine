# kaffeine.library: Motorola 680x0 Emulation for AROS

**Author of `m68kemu.library`, from which this library comes:** Fabian
Schmieder ([metaneutrons](https://github.com/metaneutrons)). The changes I
made for the kaffeine project are described in
[README.KAFFEINE.md](../../README.KAFFEINE.md).

A transparent emulation layer that allows classic AmigaOS m68k binaries to run
on any AROS port (x86_64, aarch64, etc.) by emulating the m68k CPU and routing
AmigaOS API calls to native AROS implementations.

## How It Works

### Overview

When AROS encounters an m68k hunk executable, `dos.library` detects the hunk
magic (`0x000003F3`) and routes execution through `kaffeine.library` instead of
attempting native execution. The emulator loads the binary into a contained
memory space, emulates the m68k CPU instruction by instruction, and intercepts
all AmigaOS library calls to forward them to the native AROS implementations.

```plain
┌───────────────────────────────────────────────────┐
│  m68k binary (e.g. LZX, SysInfo, DPaint)          │
│  runs inside Moira CPU emulator                   │
│                                                   │
│  JSR -offset(A6) ──► A-line trap ──► thunk        │
│                                        │          │
│  ┌────────────────────────────────┐    │          │
│  │  32 MB containment memory      │    │          │
│  │  (big-endian, m68k layout)     │    │          │
│  └────────────────────────────────┘    │          │
└────────────────────────────────────────┼──────────┘
                                         │
                  ┌──────────────────────▼────────────┐
                  │  Native AROS (x86_64/aarch64)     │
                  │                                   │
                  │  exec.library    dos.library      │
                  │  intuition.library  graphics.lib  │
                  │  ... 17 libraries, ~910 thunks    │
                  └───────────────────────────────────┘
```

### CPU Emulation

The m68k CPU is emulated by [Moira](https://github.com/dirkwhoffmann/Moira),
a cycle-accurate Motorola 680x0 emulator written by Dirk W. Hoffmann. Moira is
integrated as a git submodule. The adapter selects M68EC040, which runs on
Moira's 68020 core without FPU or MMU handlers. FPU instructions and the
68040 `PFLUSH` and `PTEST` deliver Line-F and end the program with return
code 20 without throwing a C++ exception.
`Supervisor()` executes user functions in supervisor mode, enabling CPU
detection code used by programs like SysInfo. See [INTERNALS.md](INTERNALS.md)
for model compatibility and the limits of AROS library exception handling.

**Not emulated**: Interrupts, DMA.

### Guest tasks

`AddTask()` creates up to 16 classic guest children inside the same containment
arena. Each child has a complete Moira CPU and a separate stack. Children run
cooperatively between CPU slices, by priority with round-robin turns among
equals. Guest `Forbid()` and `Disable()` prevent switches; a blocking `Wait()`
or `WaitPort()` permits another guest task to run and restores the waiter's
nesting when it resumes. Native interrupts remain enabled.

`FindTask()`, `SetTaskPri()`, signals, and ordinary guest message queues use
guest identities. IDCMP port events wake waiting guest tasks. Child return or
`RemTask()` removes that child; faults stop the whole program with the failing
task's result. Native Exec never executes a guest Task or its code. Task and
stack ranges, hooks, queue geometry and signal ownership are checked before
they are used. Child signal allocations are released on removal; signal records
of abandoned guest ports are retired before a bit can be reused.

This is a bounded compatibility layer. Signal allocations share the native
owner's namespace. Task exception/switch/launch hooks and foreign-task operations
are refused. DOS calls, `DoIO()`, `Delay()` and Workbench port waits remain
synchronous across guest tasks. Task memory lists are freed only when they are
valid guest-heap `AllocEntry()` records. The backend must provide a complete CPU
factory; this implementation currently uses Moira.
Task memory still referenced by active bridge objects stays allocated until
context cleanup. Applications must delete their guest ports when finished.

### Containment Memory Model

All m68k code and data lives inside a 32 MB memory block ("containment memory").
This block uses big-endian byte order, matching the m68k architecture. A
first-fit heap allocator with coalescing handles `AllocMem`/`FreeMem` requests
from emulated code. The m68k stack, hunk segments, and all dynamically allocated
memory reside within this block.

```plain
Address Map (32 MB):
0x000000 - 0x0003FF  Exception vectors (1 KB)
0x000400 - 0x0103FF  Library base region (64 KB) — fake jump tables
0x010400 - 0x01FEFFFF  Heap (grows upward, first-fit with coalescing)
  0xDFF000 - 0xDFFFFF  Custom chip registers, reserved inside the heap (4 KB)
0x01FF0000 - 0x01FFFFFF  Stack (64 KB, grows downward)
```

### Custom Chip Register Emulation

Programs that poll Amiga custom chip registers (e.g., busy-waiting on the video
beam position via VHPOSR at `$DFF006`) are supported through a minimal custom
chip emulation layer. The emulator intercepts reads/writes to the `$DFF000-$DFFFFF`
address range and provides:

- **VPOSR/VHPOSR** (`$DFF004`/`$DFF006`): PAL beam position counter (313 lines ×
  227 color clocks per frame) with LOF bit and PAL Agnus chip ID
- **VPOSW/VHPOSW** (`$DFF02A`/`$DFF02C`): Writable beam position
- **POTGOR** (`$DFF016`): Returns `$FF00` (no buttons pressed)
- **DMACONR, INTREQR, INTENA, ADKCONR**: Return 0

The emulated 68EC040 decodes all 32 address bits, so the registers appear
once: `$1DFF000` is ordinary memory. The heap reserves the 4 KB at `$DFF000`,
so no allocation lies under the registers, and the largest block a program
can allocate is the 17.9 MB above them.

The beam counter advances by 2 color clocks per custom chip register read,
providing changing values for polling loops and RNG seeding. There is no
per-instruction cycle counting — the beam only advances on register access.

### A-Line Trap Mechanism

AmigaOS programs call library functions via `JSR -offset(A6)`, where A6 holds
the library base address. In the emulator, each library's jump table contains
`A-line` opcodes (`0xA000`). When the m68k CPU executes one of these, Moira
raises a Line-A exception. The exception handler:

1. Determines which library and function (LVO) was called based on the PC
2. Pops the JSR return address from the m68k stack
3. Looks up the thunk: manual thunks first, then auto-generated (manual override)
4. Executes the corresponding native thunk function
5. Returns control to the m68k code via an RTE instruction

Unimplemented LVOs return 0 with a debug log message.

### Thunk System

Thunks bridge the gap between the m68k calling convention (registers D0-D7/A0-A6)
and native AROS C functions. There are two kinds:

- **Auto-generated thunks** (810 across 17 libraries): Created by
  `kaffeine_thunkgen.py` from Amiga FD files and NDK clib prototype headers.
  The generator parses C prototypes for full type information, automatically
  emitting correct pointer translation code: `m68k_to_host_or_shadow` for
  struct pointers (256 uses), `m68k_to_host` for string pointers (133 uses),
  `m68k_to_native_taglist` for TagItem arrays (73 uses), `resolve_rp` for
  RastPort resolution (63 uses), and `shadow_create_by_name` for return values
  that need shadow wrapping (34 uses).

- **Manual thunks** (~100) for functions needing special handling:
  - **exec**: `OpenLibrary` (fake base creation + m68k library fallback),
    `AllocMem`/`FreeMem`/`AllocVec`/`FreeVec` (containment heap),
    `CreatePool`/`DeletePool`/`AllocPooled`/`FreePooled` (guest pool objects and ownership),
    `FindTask` (stable ThisTask and cached named Task shadows), `RawDoFmt` (custom %s/%d),
    `WaitPort`/`GetMsg`/`ReplyMsg` (68k IntuiMessages of IDCMP ports),
    `Supervisor` (68020 exception frame trampoline),
    `Forbid`/`Permit` (no-ops), semaphores (guest memory, one task per context)
  - **dos**: `Open`/`Close`/`Read`/`Write`/`Seek` (BPTR pass-through),
    `ReadArgs`/`FreeArgs` (results copied into guest memory),
    `Lock`/`UnLock`/`CurrentDir`, `AllocDosObject`/`FreeDosObject` (a 68k
    `RDArgs` in guest memory),
    `Execute`/`SystemTagList`/`RunCommand`, `VPrintf`/`VFPrintf`,
    `GetProgramName` (from context), `MatchFirst`/`MatchNext`/`MatchEnd`
  - **intuition**: `OpenScreen`/`OpenScreenTagList`/`CloseScreen` (bounded inputs and owned screen snapshots),
    `OpenWindow`/`OpenWindowTagList`/`CloseWindow` (NewWindow translation, a
    68k Window with its fields, a 68k RastPort and a 68k IDCMP port, the
    screen of `WA_PubScreen`, `WA_CustomScreen` or the NewWindow as the
    native one of a screen of the program, also one from `LockPubScreen`),
    `LockPubScreen`/`UnlockPubScreen` (one 68k Screen for each native screen,
    with the fields that programs read, its font as a 68k TextAttr, kept while
    the program locks it or has a window there), `LockPubScreenList`/
    `UnlockPubScreenList` (a 68k copy of the list), `NextPubScreen`/
    `GetDefaultPubScreen` (names into guest memory),
    `ModifyIDCMP` (the IDCMP port that comes and goes), `AutoRequest`
    (IntuiText chain translation), `EasyRequestArgs`/
    `BuildEasyRequestArgs`/`SysReqHandler`/`FreeSysRequest` (text and gadgets
    formatted by the bridge, the requester as a 68k Window),
    `SetMenuStrip`/`ResetMenuStrip`/`ClearMenuStrip`/`ItemAddress`/
    `OffMenu`/`OnMenu` (on the 68k mirror of a GadTools menu strip),
    `AddGList`/`AddGadget`/`RemoveGList`/`RemoveGadget`/`RefreshGList`/
    `RefreshGadgets` (a GadTools list of the program on its window, whole
    batches), `ActivateGadget` (a GadTools gadget on its window, with the
    BOOL of Intuition), `DisposeObject`/`SetAttrsA`/`GetAttr`/
    `SetGadgetAttrsA`/`DoGadgetMethodA` (not for a GadTools gadget),
    `OnGadget`/`OffGadget` (a GadTools gadget changes as in AROS, drawn on
    the window that it is on, and its 68k mirror follows)
  - **gadtools**: `GT_GetIMsg`/`GT_ReplyIMsg`/`GT_FilterIMsg`/
    `GT_PostFilterIMsg` (68k IntuiMessages of the GadTools copies),
    `CreateMenusA`/`LayoutMenusA`/`LayoutMenuItemsA`/`FreeMenus` (a 68k
    mirror of the native strip), `GetVisualInfoA`/`FreeVisualInfo` (68k
    handles), `DrawBevelBoxA` (with such a handle, and only the tags that
    GadTools reads), `CreateContext`/`CreateGadgetA`/`FreeGadgets` (a 68k mirror
    of every gadget of a list, and a native copy of the List of a
    listview), `GT_GetGadgetAttrsA`/`GT_SetGadgetAttrsA`
    (the attributes that their autodocs give for each kind, as 68k LONGs,
    converted and checked for the classes)
  - **graphics**: `OpenFont`/`CloseFont`/`SetFont` (68k TextFonts),
    `LoadRGB4` (big-endian byte-swap), and the calls on the RastPort of a
    window or a screen: `Move`/`Draw`/`RectFill`/`ReadPixel`/`WritePixel`,
    the pens and the drawing mode, `ScrollRaster`, and
    `Text`/`TextLength`/`TextExtent`/`TextFit` with checked text and 68k
    TextExtents. `ReadPixel` also reads bounded guest planar bitmaps without a
    layer, using guest bytes directly

### Device I/O

Ports, I/O requests and signals of 68k code stay 68k structures in the
guest memory (`kaffeine_exec_io.c`). `timer.device` works through `DoIO()`:

1. `CreateMsgPort` → a 68k port with a signal of the guest, no native port
2. `CreateIORequest` → a cleared 68k request of the requested size
3. `OpenDevice("timer.device", ...)` → a native timerequest of the native size
   with its own reply port stands behind the 68k request, which gets a guest
   device base and the unit number
4. `DoIO` with `TR_ADDREQUEST` or `TR_GETSYSTIME` → the command and both time
   fields go to the native request and the results come back
5. `AddTime`, `SubTime`, `CmpTime`, `ReadEClock` and `GetSysTime` through the
   guest `TimerBase` (`kaffeine_thunks_timer.c`)
6. `CloseDevice`/`DeleteIORequest`/`DeleteMsgPort` → native objects and guest
   memory freed; what a program leaves open is closed when it ends

Other devices fail to open with `IOERR_OPENFAIL`, `TR_SETSYSTIME` and EClock
delays end with `IOERR_NOCMD`, and `SendIO()` stops the program with rc 20
until asynchronous I/O exists. See [INTERNALS.md](INTERNALS.md#device-io-path).

### Icon metadata

`icon.library` acquisitions return owned 68k DiskObject metadata snapshots.
The native object remains private and is released by `FreeDiskObject` only
for its exact live guest identity, or at program teardown. DefaultTool,
ToolWindow and ToolTypes are copied into guest memory. FindToolType decodes
the guest pointer array and returns a guest string address; MatchToolValue
uses bounded guest strings. Images, DrawerData and other native pointers
are zero in the snapshot. Drawing, writing icons and other unsupported
icon operations stop with rc 20 before reaching native code.

### RastPorts and Fonts

The RastPort of a window or a screen is a 68k RastPort in the guest memory
(`kaffeine_rastport.c`). Before a graphics call, the fields that a program
may write, such as the pens, the drawing mode and the pen position, go to
the native RastPort; right after it, the dispatcher copies the native fields
back, also after a generated thunk. The font is a 68k TextFont with the
metrics and the name of the native one. `ReadPixel` also accepts a guest-built
RastPort with no layer and a classic planar BitMap: depth 1..8, Flags 0 or
BMF_STANDARD, and real planes entirely inside guest memory. Coordinates must
lie within BytesPerRow * 8 by Rows. Unsupported inputs stop with rc 20.
Other drawing calls require the registered window or screen RastPort. See
[INTERNALS.md](INTERNALS.md#rastports-and-fonts).

### IDCMP Messages

The IDCMP port of a window is a 68k MsgPort with the signal bit of the native
port, so a program can `Wait()` for it. `GetMsg()`, `WaitPort()` and
`GT_GetIMsg()` give 68k IntuiMessages with every field, the window as its 68k
Window and the fields of that window brought up to date; replying frees them
(`kaffeine_window.c`). `CloseWindow()` gives Intuition and GadTools back the
messages that the program has not replied. See
[INTERNALS.md](INTERNALS.md#idcmp-message-flow).

### GadTools Menus

`CreateMenusA()` reads the 68k NewMenu array, lets native GadTools build the
strip, and gives the program a 68k mirror of it: Menus and MenuItems with
their UserData, IntuiTexts, TextAttrs and separators, which follow the native
strip after each call and before a menu message reaches the program, so
`ItemAddress()`, CHECKED and NextSelect work as on AmigaOS. A VisualInfo is a
68k handle (`kaffeine_menus.c`). A strip that the program builds itself stays
its own: Intuition gets a native copy of it, checked all over first, and
CHECKED, ITEMENABLED, MENUENABLED and NextSelect pass between the two. See
[INTERNALS.md](INTERNALS.md#gadtools-menus) and
[INTERNALS.md](INTERNALS.md#menus-that-the-program-builds).

### GadTools Gadgets

`CreateContext()` and `CreateGadgetA()` let native GadTools make the gadgets
from a native NewGadget and the tags that GadTools reads for each kind, and
give the program a 68k mirror of every gadget of the list, the arrows of a
scroller and the level display of a slider included: geometry, flags, ID,
UserData, the label, a StringInfo with its buffer in guest memory for
strings and integers and a PropInfo for sliders and scrollers. Each 68k
gadget is a shadow, so the generated thunks find the native one
(`kaffeine_gadgets.c`). `GT_GetGadgetAttrsA()` fills in the attributes that
its autodoc gives for each kind as 68k LONGs, with the program's own label
array, colour table and string buffer, after it brings the mirrors up to
date, and `GT_SetGadgetAttrsA()` changes them, converted and checked for
the classes, after which the mirrors follow. See
[INTERNALS.md](INTERNALS.md#gadtools-gadgets).

### Gadgets That the Program Builds

Boolean gadgets that a program builds itself go on its windows as native
copies that the bridge makes after checking the whole run, with their
Border or Image renders and their labels (`kaffeine_classic.c`,
`kaffeine_copy.c`). `GFLG_SELECTED` and `GFLG_DISABLED` pass between a copy
and the program's gadget by bit, so that the program sees what input and
`OffGadget()` change, and Intuition what the program sets before
`RefreshGList()`; a message names the program's gadget. See
[INTERNALS.md](INTERNALS.md#gadgets-that-the-program-builds).

### C Startup Library Auto-Open

Classic Amiga C compilers (SAS/C, Lattice, Aztec, DICE) embed a table of
library base pointer slots followed by library name strings in the binary.
`Kaffeine_AutoOpenLibs()` scans containment memory after hunk loading for this
pattern and pre-fills the base pointer slots with fake library bases, enabling
multi-hunk C programs to find their libraries without executing the startup
code's OpenLibrary loop.

### Shadow Struct System

The central challenge: m68k structs have different field offsets and sizes than
native AROS structs (32-bit vs 64-bit pointers, different alignment). When a
native function returns a struct pointer (e.g., `OpenWindow` returns a
`Window *`), the m68k code expects to read fields at m68k offsets.

The shadow system solves this by maintaining parallel m68k-layout copies of
native structs in containment memory. When a native function returns a struct
pointer, a shadow is created: a block of containment memory with fields copied
from the native struct at the correct m68k offsets. Pointer fields are
recursively resolved.

**297 struct layouts** with **2,918 field mappings** are auto-generated from the
NDK and AROS headers by the shadow layout toolchain. This covers virtually all
public AmigaOS structs across exec, dos, intuition, graphics, utility, workbench,
devices, libraries, resources, and datatypes. The offset extractor recurses into
embedded sub-structs (Node, Message, IORequest, Task, etc.) to map every scalar
leaf field at its actual offset. The most thoroughly mapped structs include
GfxBase (65 fields), Screen (62), Process (56), Window (26), ColorMap (19),
and IOStdReq (16). See [INTERNALS.md](INTERNALS.md) for the complete struct list.

The `OpenScreen`, `OpenScreenTagList`, `LockPubScreen` and `OpenWindow`
thunks construct cleared guest structures with registered embedded objects.
Owned and borrowed Screen snapshots include a ViewPort (offset44), RastPort
(offset84), sizes, borders and font metadata. The Window snapshot includes
its size, limits, borders and pens, a 68k RastPort,
the UserPort (for IDCMP WaitPort/GetMsg), and the WScreen back-pointer for a
screen of the program: one that it opened, or one from `LockPubScreen()`.

The shadow registry is a flat array (max 1024 entries) with linear scan lookup.
Type tags distinguish shadow kinds: 1=Window, 3=RastPort, 4=MsgPort, 5=Screen,
6=ViewPort, 8=IntuiMessage, 9=GadTools copy of an IntuiMessage,
10=IntuiMessage that `WaitPort()` showed, 11=GadTools VisualInfo handle,
12=68k gadget of a GadTools list, 13=68k Screen of a locked public screen,
15=requester window requiring `FreeSysRequest`,
17=List, 20+=auto-generated layouts; 68k TextFonts, GadTools menu strips and
gadget lists have tables of their own.

### m68k Library Loading

Libraries without native AROS equivalents (e.g., `reqtools.library`) are
loaded as m68k hunk files, as Exec loads a disk library. A name with a path,
such as `PROGDIR:libs/x.library`, is that file alone, as ramlib takes it;
otherwise the loader searches in this order:

1. `LIBS68K:<name>`, a dedicated assign for m68k libraries
2. `LIBS:m68k/<name>`, a subdirectory that needs no assign
3. `LIBS:<name>`, for compatibility

This keeps m68k libraries separate from native AROS libraries, avoiding
name conflicts. The search runs with the requesters of the process switched
off, so a missing `LIBS68K:` assign does not stop a program, started through
`Run` for example, on a "Please insert volume" requester. Users can set up
`LIBS68K:` in their Startup-Sequence:

```
Assign LIBS68K: SYS:Libs/m68k
```

The loader:

1. Loads the file with `LoadSeg()`, whose record of relocations lets the
   hunks be copied into guest memory with the `[size][next]` headers of a
   seglist; a native library has no such record and is no 68k library
2. Looks for the resident tag (`RTC_MATCHWORD`, `0x4AFC`, whose
   `rt_MatchTag` points at it) in those hunks
3. Makes an `RTF_AUTOINIT` library as `MakeLibrary()` does: a jump table of
   `JMP` absolute long instructions from the function table, of LONG
   pointers or, after a first WORD of -1, of WORD offsets from the table;
   the data area cleared and set up by the `InitStruct()` table; the node
   type, name, version and IdString of the resident tag, as
   `InitResident()` sets them; and the init function called with the base
   in D0, the seglist in A0 and SysBase in A6, which gives the library
4. Calls the `Open()` of the library for each `OpenLibrary()`, after
   checking `lib_Version` against the version asked for, and its `Close()`
   for each `CloseLibrary()`, also for a base that `Open()` made for its
   opener; the library is found again by its name without path, and stays
   to the end of the program unless `Close()` gives its seglist back after
   an expunge
5. Calls the `Expunge()` of an open library when a newer version is asked
   for, as AmigaOS 3.2.3 does, so that it goes at its last close; a closed
   one stays as it is

Init, `Open()`, `Close()` and `Expunge()` are 68k code that can open and
close other libraries: each library keeps its slot of the 32 while they run,
and the name is a copy that the thunk makes first. The seglist of a 68k
library is guest memory, which native `UnLoadSeg()` and `RunCommand()` never
get.

m68k-to-m68k calls go through the jump table with no traps. Libraries that
are not `RTF_AUTOINIT`, whose `rt_Init` makes the library itself, are not
supported yet.

### Threading

GUI programs that block on `WaitPort` would freeze AROS if run in the main
process. `RunFile` uses `CreateNewProc(NP_Synchronous=TRUE)` to run the emulator
in a child process, allowing the AROS event loop to continue.

### dos.library Integration

To run m68k binaries transparently, two functions in `dos.library` need patching:

- **RunCommand**: Checks `GetSegListInfo(GSLI_68KHUNK)` — if the seglist is an
  m68k hunk binary, opens `kaffeine.library` and calls `RunHunk` instead of
  native execution. Like a native command, the program gets its arguments in
  `pr_Arguments` and at the start of `Input()`, where `ReadArgs()` reads them.
- **CreateNewProc**: Same detection — m68k processes created via `CreateNewProc`
  are also routed through the emulator.

This makes m68k binary execution fully transparent: no special commands needed.

### Execution Lifecycle

1. `dos.library` detects hunk binary, calls `RunHunk(segList, stackSize, argPtr, argSize)`
2. `RunHunk` creates a 32 MB containment buffer and heap
3. Hunk segments are copied into containment memory and rebased with the
   relocation record that `LoadSeg` keeps for the seglist (`GSLI_68KRelocs`)
4. exec.library and dos.library fake bases are set up with A-line trap jump tables
5. ExecBase shadow is initialized at address 4 (AttnFlags report a 68040 without FPU)
6. `AutoOpenLibs` scans for C startup library tables and pre-fills them
7. Sentinel address (`0x000160`, below the heap) is pushed as return address
8. Moira CPU starts executing at the entry point in M68EC040 mode (its 68020 core, no FPU or MMU execution)
9. Library calls trigger A-line traps → thunk dispatch → native AROS calls
10. RTS to the sentinel returns D0; an unsupported Line-F instruction stops with return code 20
11. Context is destroyed, exit code returned to AROS

## Supported Libraries

The bridge provides generated or manual thunks for the following libraries:

| Library | Auto | Manual | Total | Key Coverage |
|---------|------|--------|-------|-------------|
| exec.library | 81 | 33 | 114 | Memory, tasks, signals, semaphores, messages, devices, libraries |
| dos.library | 112 | 47 | 159 | File I/O, locks, CLI, pattern matching, ReadArgs, packets |
| intuition.library | 130 | 8 | 138 | Windows, screens, gadgets, menus, IDCMP, requesters |
| graphics.library | 172 | 26 | 198 | Rendering, text, blitting, layers, fonts, colors, display metadata |
| layers.library | 40 | 40 | 40 | Live 68k Layer snapshots; owned screen-bar drawing contexts, window layer locks/hooks and owned NewLayerInfo/DisposeLayerInfo; remaining slots stop with 20 |
| utility.library | 42 | — | 42 | TagItem processing, hooks, date functions |
| gadtools.library | 18 | — | 18 | GadTools gadget toolkit |
| asl.library | 8 | — | 8 | File/font/screenmode requesters |
| commodities.library | 29 | — | 29 | Input event handling |
| cybergraphics.library | 24 | — | 24 | RTG (retargetable graphics) |
| datatypes.library | 30 | — | 30 | Multimedia data type system |
| diskfont.library | 3 | — | 3 | AvailFonts, NewFontContents and DisposeFontContents |
| expansion.library | 22 | — | 22 | Expansion board management |
| icon.library | 21 | — | 21 | Workbench icon handling |
| iffparse.library | 40 | — | 40 | IFF file parsing |
| keymap.library | 4 | — | 4 | Keyboard mapping |
| locale.library | 34 | — | 34 | Localization |
| workbench.library | 0 | 7 | 7 | AppWindows, AppIcons and AppMenuItems with their AppMessages, WBInfo |
| mathffp.library | 0 | 12 | 12 | FFP arithmetic and conversions with guest condition codes; see the precision limit in INTERNALS.md |

Native lookup uses the filename after the final `:` or `/`, keeping its
exact case. Paths such as `LIBS:diskfont.library` share the canonical base
and its cache entry. When native lookup misses, the original complete
path is passed to the 68k loader.

Libraries not in this list are loaded as m68k code and executed
within the emulator.

## Tested Programs

| Program | Size | Type | Status |
|---------|------|------|--------|
| LZX | 83 KB | CLI archiver (commercial) | ✅ Full functionality |
| olha | 291 KB | SFX archiver (6 hunks, relocations) | ✅ LH5 decompression, CreateDir |
| ROLEX | 13 KB | Clock (custom screen, GUI loop) | ✅ Opens screen, event loop works |
| SysInfo 4.4 | 62 KB | System info (7 hunks, custom screen) | ✅ Renders full UI (title, menus, buttons, text) |
| Directory Opus 4.12 | 328 KB | File manager (GUI, m68k libraries) | ✅ Loads dopus.library, shows AutoRequest dialog, responds to button click |
| AmigaTerm 1.0 | 82 KB | Terminal emulator (GUI) | ✅ Opens window, clean exit |
| Textcraft Plus | 208 KB | Word processor (GUI, custom screen) | ✅ Opens screen/window, sets colors, clean exit |
| reqtools.library | 56 KB | m68k shared library (29 functions) | ✅ |

## Known Limitations

### CPU

- No interrupt delivery to m68k code (INTREQR/INTENA return 0)
- Single-threaded: one m68k instruction at a time, no cycle counting

### Memory

- Fixed 32 MB containment, fixed 64 KB stack (RunHunk ignores stackSize parameter)
- One arena for MEMF_ANY/MEMF_PUBLIC/MEMF_CHIP/MEMF_FAST; MEMF_CLEAR is honored
- No memory protection within containment

### Thunks

- `RawDoFmt` only handles %s, %d, and %ld; other specifiers get basic fallback
- Pools allocate each block directly from the guest heap; puddle and threshold
  sizes are recorded as hints, with no data-puddle reservation. Ownership and
  MEMF_CLEAR are preserved, and DeletePool frees all remaining owned blocks.
- `Forbid`/`Permit` are no-ops. Semaphores operate only within one guest
  context; a call that would wait stops it with rc 20. Procure/Vacate message
  protocols are unsupported and also stop with rc 20. See
  [guest semaphore rules](INTERNALS.md#guest-semaphore-rules).

### Device I/O

- `timer.device` through `DoIO()` and the no-window console library unit;
  asynchronous device I/O and other device adapters remain unsupported
- Ordinary guest FIFO messages and a bounded ACTION_READ packet adapter;
  cross-task messaging and empty ordinary WaitPort remain unsupported
- Personal Paint completes repeated packet reads and replies, then reaches
  a contained CPU callback memory fault; see the target README
- `Signal()` reaches only the current task

### Graphics

- Only the RastPort of a window or a screen reaches graphics.library; a
  RastPort that the program sets up itself, with its own BitMap, does not
- `Layer`, `BitMap` and the area fill fields of a 68k RastPort stay 0, and
  supported monochrome TextFonts have owned guest bitmap and character data
- At most 32 68k TextFonts at a time; the fonts of windows and screens stay
  for the whole program
- The fields of a 68k Window are those at `OpenWindow()`; `WScreen` is 0 for a
  screen that the program neither opened nor locked with `LockPubScreen()`
- `LockPubScreen()` gives one 68k Screen for each native screen, filled in
  again at each lock and kept while the program holds a lock, has a window
  there or holds the list of public screens; its native pointers, such as
  `Title`, `FirstWindow` and the `ColorMap` of the ViewPort, are 0, and at
  most 16 screens have one at a time. A lock gives 0 for a screen whose font
  the bridge cannot hold open, as when `OpenFont()` finds another font of the
  same name and size first. `WScreen` is the 68k Screen of that
  screen, the one that the program opened first. A program that goes on using
  the pointer after its last unlock, without a window there, reads freed guest
  memory, and `WA_PubScreen` of it stops the program with rc 20
- In the 68k copy of `LockPubScreenList()`, `psn_Screen` is 0 for a screen
  that the program has not locked, and `NextPubScreen()` gives 0 after the
  last public screen, as AROS does
- `OpenWindowTagList()` leaves its NewWindow out, and `OpenWindow()` the tags
  of an `ExtNewWindow`
- On a hi- or truecolor screen `ReadPixel()` gives the low byte of the pixel
  rather than its pen, as on native AROS

### IDCMP

- The fields of a 68k Window come up to date with its IDCMP messages, not
  after every Intuition call
- `IAddress` of a message is 0 for gadgets and other native objects, until
  gadgets have a bridge
- The 68k IDCMP port shows an empty list: programs take messages with
  `GetMsg()`, not by walking `mp_MsgList`
- A UserPort shared among windows, and messages between 68k ports, stop the
  program with rc 20

### Menus

- Image menu items of GadTools (`IM_ITEM`, `IM_SUB`) stop the program
  with rc 20
- A strip that the program builds itself goes to Intuition as a native
  copy, strings included, which keeps only CHECKED, ITEMENABLED and
  MENUENABLED in step with the program's strip, and gives it NextSelect.
  What the program changes in its strip while a window shows it, or while
  its copy waits for `ResetMenuStrip()`, reaches Intuition only at the
  next `SetMenuStrip()` or `ResetMenuStrip()`, as the NDK asks, and of
  that only those flags while a copy exists. Menus and items that two
  strips share get a copy in each, which takes what the other one changed
  in them at its next `SetMenuStrip()` or `ResetMenuStrip()`. Images of
  BOOPSI classes, chains in a cycle, a menu or item in two places of one
  strip, more menus, items or subitems than a menu number names, strings
  longer than 255 characters, and structures outside guest memory stop
  the program with rc 20
- The images of `GTMN_Checkmark` and `GTMN_AmigaKey` are left out of the
  layout, so GadTools draws its own
- Of what the program changes in the 68k mirror, only the CHECKED,
  ITEMENABLED and MENUENABLED flags reach the native strip, and the
  UserData stays the program's; new links between menus are not taken

### Gadgets

- GadTools gadgets can be created and freed, their attributes read and
  changed, and their lists put on windows, whole batches at a time; their
  setters draw on the window that a gadget is on, `ActivateGadget()`
  activates one there, and `SetAttrsA()`, `GetAttr()`,
  `SetGadgetAttrsA()`, `DoGadgetMethodA()` or `DisposeObject()` of
  Intuition on one stop the program with rc 20
- A gadget list that a window cannot take as a whole (a gadget that the
  program built itself linked in, a batch cut apart or already on a
  window, a requester, a position between the gadgets of one batch there,
  a palette too small for the layout of its class), `FreeGadgets()` of
  gadgets on a window,
  `CreateGadgetA()` after a gadget on a window, and a listview and the
  string gadget of its `GTLV_ShowSelected` on different windows stop the
  program with rc 20; the 68k Window's `FirstGadget` does not show the
  window's list
- Hooks and functions of 68k code (`GTST_EditHook`, `GTIN_EditHook`,
  `GTSL_DispFunc`, `GTLV_CallBack`), a `GTLV_ShowSelected` that is no
  string gadget of GadTools, `FreeGadgets()` of that string gadget while
  its listview stays, a previous gadget that
  `CreateGadgetA()` did not return, such as an arrow of a scroller, and
  `FreeGadgets()` of a gadget that GadTools made after another in the same
  call, or of a list that leads into one or back to a batch already on
  it, stop the program with rc 20
- A format of `GTNM_Format` or `GTSL_LevelFormat` must read no pointer and
  no more than one ULONG of numbers, and fit the buffer of the text class;
  `GTST_String` is cut to MaxChars, which must be from 0 to 32766;
  labels and font names must be shorter than 16000 characters, and a
  colour table at most 32767 colours long, with guest memory after it for
  256 colours past its offset, or for all of them when there are more
- `DisposeObject()` of a GadTools gadget stops the program with rc 20:
  `FreeGadgets()` frees them
- `FreeGadgets()` follows the program's list as AmigaOS 3.2.3 does, past
  gadgets of its own and where `RemoveGList()` left a link, and cuts the
  NextGadget of every GadTools gadget that led to the freed ones; the
  gadgets that a program leaves go at its end
- Of the gadgets that the program builds itself Boolean, string, integer
  and proportional gadgets of windows go on them: a list with a custom
  gadget, a string gadget with a keymap of its own
  (`GACT_ALTKEYMAP`), a gadget of a requester or a screen, a system
  gadget, a masked Boolean gadget or a gadget object stays out of the
  window, as before, and one that goes on with GadTools gadgets, or with
  a string gadget whose StringExtend has an EditHook or a font that is
  not one of the program's, or whose buffers hold less than MaxChars bytes
  in guest memory, or with a proportional gadget whose PropInfo or knob
  lies outside guest memory, stops the program with rc 20
- What the program changes in the renders, the label or the geometry of
  its own gadget while it is on a window reaches Intuition only after
  `RemoveGList()` and `AddGList()`; `GFLG_SELECTED` and `GFLG_DISABLED`
  follow both ways, and so do BufferPos, DispPos and LongInt of a string
  gadget, whose NumChars, DispCount and UndoPos follow Intuition; the text
  stays in the program's buffers, where Intuition edits it and counts it
  as far as its NUL, beyond MaxChars too as AmigaOS does, at most into a
  guard of zero bytes after guest memory; what the program sets in the
  StringInfo of a gadget while Intuition edits it reaches Intuition once
  it no longer does, unless Intuition changed it meanwhile; the last
  gadget that `RemoveGList()` takes off keeps its NextGadget, and a string
  gadget loses its selection there, as on AmigaOS 3.2.3; the Flags, pots
  and bodies of a PropInfo follow both ways, to Intuition only while the
  button does not hold the gadget, and its `AUTOKNOB`, which chooses the
  render of the knob, only as the renders do; what Intuition works out of
  a PropInfo and where it puts the knob go to the program, which during a
  drag without messages sees the pots at the next pass; `NewModifyProp()`
  and `ModifyProp()` of a gadget on no window change its PropInfo alone,
  where AmigaOS 3.2.3 also works out the rest and draws the gadget into
  the window given; gadget help (`GMORE_GADGETHELP`) of its own gadgets
  stays off
- `SetEditHook()` stops the program with rc 20: the hook would become the
  edit hook of every string gadget in the system

### Shadow System

- Max 1024 simultaneous shadows (linear scan lookup)
- Max 32 libraries, max 16 library name mappings
- 1 of 298 structs lacks a shadow layout (RegionRectangle — no scalar fields)

### Custom Chips

- Only beam position (VPOSR/VHPOSR) and POTGOR are functional
- No blitter, copper, audio, sprites, or DMA emulation
- Beam advances only on register reads, not per-instruction

### Hunk Loader

- Max 100 hunks per binary
- No overlay hunks (HUNK_OVERLAY, HUNK_BREAK)
- No HUNK_DEBUG or HUNK_SYMBOL support
- Non-AUTOINIT m68k libraries not supported

## File Structure

### Core

| File | Description |
|------|-------------|
| `kaffeine.conf` | AROS library definition (LVO 5: RunHunk, LVO 6: RunFile) |
| `kaffeine_intern.h` | Context struct, fake lib bases, inline helpers, memory accessors |
| `kaffeine_moira.cpp` | Moira subclass, A-line exception handler, sentinel detection |
| `kaffeine_run.c` | RunHunk/RunFile entry points, process creation, ExecBase shadow init |
| `kaffeine_memory.c` | Containment memory allocation, heap allocator, hunk loaders |
| `kaffeine_init.c` | Library init/open/close |
| `kaffeine_stubs.c` | AROS library stubs |
| `mmakefile.src` | Build rules (C++20 for Moira, links stdc++/stdcio/stdc/posixc) |

### Thunks

| File | Description |
|------|-------------|
| `kaffeine_thunks.h` | THUNK_D/A/PTR/DPTR/SET_D/SET_A macros, thunk entry/table structs |
| `kaffeine_thunks.c` | ~100 manual thunks, shadow registration, OpenLibrary with auto-registration |
| `kaffeine_exec_io.c` | Guest ports, signals and requests; native timer I/O and console CONU_LIBRARY open/close adapters with trusted device kinds |
| `kaffeine_thunks_console.c` | console.device CONU_LIBRARY and default-keymap RawKeyConvert for one marshalled guest InputEvent; explicit refusal slots for unsupported console operations |
| `kaffeine_thunks_gfx.c` | Intuition compatibility slots, fonts and the graphics calls on a 68k RastPort |
| `kaffeine_thunks_diskfont.c` | Checked OpenDiskFont using the shared graphics font identity and ownership |
| `kaffeine_fontattr.c` | TextAttr/TTextAttr validation and native copies of scalar DPI tags for both font-open APIs |
| `kaffeine_rastport.c` | 68k RastPorts, read-only monochrome TextFont snapshots with glyph data, and shared font lifetimes |
| `kaffeine_window.c` | Fields and IDCMP ports of 68k windows, 68k IntuiMessages, ModifyIDCMP and the GadTools message calls |
| `kaffeine_menus.c` | GadTools menu strips with their 68k mirror, strips that the program built with their native copy, VisualInfo handles, `DrawBevelBoxA()` and the Intuition menu calls |
| `kaffeine_gadgets.c` | GadTools gadget lists with a 68k mirror of every gadget |
| `kaffeine_classic.c` | Gadgets that the program built itself on its windows, as native copies: runs, the Flags passed by bit, links and messages |
| `kaffeine_mirror.c` | 68k mirrors of native objects of GadTools: the tables of a strip or a list, strings, TextAttrs, IntuiTexts and images |
| `kaffeine_copy.c` | Native copies of 68k structures that Intuition reads, checked against guest memory: IntuiText chains with their strings and TextAttrs, Image and Border chains, and gadgets |
| `kaffeine_requester.c` | EasyRequests: 68k EasyStructs formatted by the bridge, requester windows as 68k Windows |
| `kaffeine_thunks_timer.c` | timer.device functions behind the guest TimerBase |
| `kaffeine_thunks_mathffp.c` | FFP arithmetic and conversions on 32-bit words, with guest N/Z/V and no hardware FPU |
| `kaffeine_thunks_gen.c` | 810 auto-generated type-aware thunks across 17 libraries |
| `kaffeine_thunkgen.py` | Generator: reads FD files + NDK clib protos → emits C thunks with correct pointer translation |

### Shadow Struct System

| File | Description |
|------|-------------|
| `kaffeine_shadow.h` | Field type enums (SF_BYTE/WORD/LONG/PTR), M68KFieldMap, M68KStructLayout |
| `kaffeine_shadow.c` | Generic shadow engine: create/sync/destroy with recursive sub-layouts |
| `kaffeine_shadow_layouts.h` | **Generated**: 297 struct layouts, 2,918 field mappings |

### Shadow Layout Toolchain

| File | Description |
|------|-------------|
| `gen_offset_extractor.py` | Scans NDK headers → emits C file with `offsetof()` + `sizeof()` for every struct field |
| `gen_shadow_layouts.py` | Reads m68k + AROS offset files → emits `kaffeine_shadow_layouts.h` |
| `m68k_offsets.txt` | Static m68k struct offsets (3,017 fields, 298 structs) — checked in, never changes |

#### Regenerating Shadow Layouts

The m68k offsets are fixed (AmigaOS binary compatibility) and checked into the
repository. Only the AROS offsets need to be regenerated when targeting a
different architecture:

```bash
# Generate AROS offsets for the current target (done at build time)
python3 gen_offset_extractor.py > offsets_src.c
sed 's/"n"/"i"/g; s/%0/%c0/g; s/%1/%c1/g' offsets_src.c > offsets_aros.c
gcc -S -o aros_offsets.s offsets_aros.c -isystem $AROS_SDK/include ...
grep -oP '(OFF|SIZ)\|[^"]*' aros_offsets.s > aros_offsets.txt

# Merge into header
python3 gen_shadow_layouts.py m68k_offsets.txt aros_offsets.txt > kaffeine_shadow_layouts.h
```

To add new structs to the m68k side (requires `m68k-amigaos-gcc`):

```bash
python3 gen_offset_extractor.py > offsets_src.c
m68k-amigaos-gcc -S -o m68k.s offsets_src.c -I $NDK_INCLUDE
grep -oP '(OFF|SIZ)\|[^"]*' m68k.s | sed 's/#//g' > m68k_offsets.txt
```

### m68k Library Loader

| File | Description |
|------|-------------|
| `kaffeine_m68klib.c` | 68k libraries from disk as Exec makes them: hunks as a guest seglist, resident tag, `MakeLibrary()` with LONG or WORD function tables and `InitStruct()`, init, `Open()`, `Close()` and `Expunge()` |

### Moira Submodule

| Path | Description |
|------|-------------|
| `Moira/` | Git submodule: [github.com/dirkwhoffmann/Moira](https://github.com/dirkwhoffmann/Moira) |

### Tests

| File | Description |
|------|-------------|
| `tests/kaffeine_host/test_kaffeine.cpp` | Host-side unit tests: context, heap, endian, pointer translation, lib bases |
| `tests/kaffeine_host/test_moira_exec.cpp` | Host-side Moira execution tests: MOVEQ/RTS, ADD, loops, memory access |
| `tests/kaffeine_host/test_hello_thunks.cpp` | Host-side end-to-end: A-line trap → thunk dispatch → return |
| `developer/debug/test/kaffeine/test_kaffeine.c` | AROS-side test: opens kaffeine.library (execution test incomplete) |

## Building

### Prerequisites

- AROS source tree with the `emu68-hosted` branch
- GCC 14 (GCC 15 crashes the AROS linux-x86_64 hosted build)
- C++20 support (for Moira)
- Python 3 (for code generators)

### Build Commands

```bash
cd $AROS_BUILD_DIR
rm -rf bin/linux-x86_64/gen/rom/kaffeine
make kernel-kaffeine
```

The built library appears at `bin/linux-x86_64/AROS/Libs/kaffeine.library`.

### Installation

Copy `kaffeine.library` to `AROS:Libs/`. With the `emu68-hosted` branch,
`dos.library` automatically detects m68k hunk binaries and routes them through
the emulator — no configuration needed.

In this fork the library is called `kaffeine.library` since #87, and the
`dos.library` of this tree opens it by that name: an `m68kemu.library` left
in `AROS:Libs/`, from upstream AROS or from before the rename, is not
opened.

Without the dos.library patch, use the `RunM68K` CLI tool:

```
RunM68K <m68k-binary> [arguments]
```

### Running Tests

```bash
# Host-side unit tests (no AROS required)
cd tests/kaffeine_host
make
./test_kaffeine

# Moira execution tests (manual build)
g++ -std=c++20 -I../../rom/kaffeine/Moira -o test_moira_exec \
    test_moira_exec.cpp -DNO_INLINE_STDARG

# AROS-side test (built with AROS)
make test-kaffeine
```

## Dependencies

| Dependency | Version | License | Purpose |
|------------|---------|---------|---------|
| [Moira](https://github.com/dirkwhoffmann/Moira) | `1efd6946` (pinned submodule) | MIT (core) | M68000 CPU emulation core |
| AROS SDK | varies by component | APL 1.1 and component licenses; see [LEGAL](../../LEGAL) | Target platform headers and libraries |
| GCC | 14.x | GPL | C/C++ compiler (C++20 for Moira) |
| Python | 3.x | PSF | Code generators (thunks, shadow layouts) |
| m68k-amigaos-gcc | 15.x | GPL | m68k cross-compiler (only for regenerating m68k_offsets.txt) |
| AmigaOS NDK | 3.2 | See the terms supplied with the NDK | m68k struct definitions and clib prototypes used by the upstream generators |

The M0 host suite combines AROS sources with Moira's MIT-licensed core. It does
not build Moira's upstream Runner or its bundled third-party test tools. The NDK is
referenced by the upstream generators; M0 does not bundle the NDK or regenerate
those tables. The exact NDK terms and the provenance of generated offsets,
layouts and thunks still need review before a release that includes them.
See [Licensing and provenance](../../docs/kaffeine/LICENSING.md) for the scope of
this review and the distinction between current dependencies and Emu68, the
reference for the kaffeine JIT, the translator that will be the second
backend.

## Further Reading

- [INTERNALS.md](INTERNALS.md) — Deep technical reference: memory model, shadow
  engine internals, complete struct layout list, thunk generator details, device
  I/O analysis, and known bugs.

## Author

- **Fabian Schmieder** ([@metaneutrons](https://github.com/metaneutrons)) — Architecture, design, implementation, and AROS system integration.

## Acknowledgments

This project would not be possible without:

- **Dirk W. Hoffmann** — Author of [Moira](https://github.com/dirkwhoffmann/Moira),
  the cycle-accurate Motorola 68000 emulator at the heart of this project. Moira
  is also the CPU core of [vAmiga](https://dirkwhoffmann.github.io/vAmiga/),
  Dirk's outstanding Amiga 500/1000/2000 emulator. His meticulous work on
  accurate m68k emulation made this entire effort possible.

- **The AROS Development Team** — For building and maintaining AROS, the open-source
  AmigaOS-compatible operating system that serves as the host platform.

- **Hyperion Entertainment** — For the AmigaOS NDK 3.2, which provides the
  authoritative struct definitions used to generate the shadow layout tables.

- **The Amiga community** — For keeping the platform alive for over 40 years and
  producing the software that this emulator aims to preserve.

## License

`kaffeine.library` is part of AROS and licensed under the
[AROS Public License 1.1 (APL)](../../LICENSE). Other AROS components and
third-party sources retain their own licenses; see [LEGAL](../../LEGAL).

The Moira CPU core is licensed under the
[MIT License](https://opensource.org/licenses/MIT).
See the [core license at the pinned Moira revision](https://github.com/dirkwhoffmann/Moira/blob/1efd69467ca13b27b2fb40febd5cb31dbecdea5f/Moira/LICENSE)
and the [project licensing notes](../../docs/kaffeine/LICENSING.md) for details.

### Display metadata queries

`GetDisplayInfoData` accepts a NULL handle and a mode ID for `DTAG_DISP`,
`DTAG_DIMS`, `DTAG_NAME` and `DTAG_MNTR`. It queries a complete native temporary
and writes classic big-endian fields into the validated guest destination.
Native padding and reserved pointer fields are not exported. Classic transfer
counts are at most 48, 66, 48 and 88 bytes. Partial dimension queries copy
complete rectangles after their 26-byte prefix; all other bytes stay intact.

Monitor queries publish a stable, context-owned 160-byte MonitorSpec snapshot
and a bounded copied name. Public scalars and geometry are included. Function
pointers, library pointers, HIDD objects and native synchronization state are
excluded; the guest list and semaphore headers are empty. These borrowed
snapshots cannot be released by guest FreeMem and are not generic shadows.
The cache supports 128 monitor identities. Allocation failure, an oversized
name or a full cache returns zero without publishing a new record.

`OpenMonitor`, `CloseMonitor`, `FindDisplayInfo`, non-NULL query handles,
vector chunks and unknown tags stop with return code 20. Owned monitor opens,
native monitor mutation and display handles remain unsupported.
`NextDisplayInfo` remains a scalar mode-ID query.


### Owned screens

`OpenScreen` and `OpenScreenTagList` share the protected classic Screen,
ViewPort, RastPort and font snapshot code used by public screens. An owned
screen keeps one guest identity through public locks and windows. Bounded
NewScreen/ExtNewScreen and tag inputs include geometry, display IDs, fonts,
titles, pen lists, display clip rectangles and error outputs; pointer data
is translated explicitly. Unsupported tags, custom bitmaps, gadgets and
malformed inputs stop with return code20 before native Intuition is called.

Native title/font copies live until a successful close. A refused close keeps
the native screen and guest snapshot valid. Before releasing screen locks
and screens at guest exit, the running task closes its remaining windows,
replies to messages and runs menu/gadget close hooks. Known requester windows
use `FreeSysRequest` so their native gadgets and request data are freed too.
External visitors can still cause native Intuition to refuse a screen close.


Classic AddTask and RemTask currently stop with return code20. Guest task
records, stack pointers and 68k entry points must never reach native Exec's
scheduler. Concurrent guest task execution requires a separate adapter.

A VisualInfo owns its native DrawInfo/Screen reference until freed. At most
32 handles are tracked per context. For a borrowed public screen the bridge
takes its own native visitor lock; for an owned screen CloseScreen returns
FALSE while a VisualInfo remains live. Teardown closes windows, frees menus
and gadgets, then VisualInfos, then screen locks/screens and retained fonts.
Requester-only operations accept only requester handles, so an ordinary
window cannot reach native FreeSysRequest or SysReqHandler.
