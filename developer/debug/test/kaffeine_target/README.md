# kaffeine target test on AROS AArch64

These files run real 68k HUNK programs through `kaffeine.library` on AROS
AArch64 in QEMU (`raspi3b`), along the transparent path: the Shell loads
the program with `LoadSeg()`, `RunCommand()` hands the seglist to
`RunHunk()`, which copies and rebases it with the relocation record kept by
DOS and runs it with Moira. The result sections are chronological: each
records the build it ran, and the latest results are in the dated sections
at the end.

| File | Purpose |
| --- | --- |
| `hello_seglist.s` | 68k program with CODE, DATA and BSS hunks. Every absolute operand needs a relocation to DATA or BSS, so its message is printed only if the rebased copy is correct. |
| `cpu_faults.c` | Deliberate out-of-arena long accesses at 0xFFFFFFFC, aligned word accesses at 0xFFFFFFFE, inline-assembler odd word accesses and byte accesses at 0xFFFFFFFF, then `FMOVE.L FPCR,D0` without FPU startup. Prints and flushes a STEP line before each step. Expected guest result: bounds checks pass, Line-F ends the command with `RETURN_FAIL` (20), and its Shell/Run caller continues. With the argument `mmu` it runs only `PFLUSHA`, which must also end with rc 20. |
| `hooks.c` | C hooks behind NDK register-to-libnix assembler stubs, nested CallHookPkt calls, RawDoFmt with a custom C writer, interpreted advancing A3 and the standard copy routine, packed WORD/LONG/string/BSTR arguments, the advanced argument result, magic STRING/COUNT, and VSNPrintf output, truncation and required bytes including zero. |
| `dos_fileio.c` | dos.library file I/O (`Open()`, `Write()`, `Read()`, `Seek()`, `Close()`), an FWrite/FRead block round trip, NameFromLock/NameFromFH paths, locks, `Examine()`, `Rename()`, `DeleteFile()`, `CreateDir()`, local text/binary variables and truncation, Fault/PrintFault, AddPart and its no-fit failure, SetComment/SetProtection read back through Examine, ParsePattern/MatchPattern and their NoCase forms, and converted results of `ExamineFH()`, `StrToLong()`, `FilePart()`, `PathPart()`, `FGets()` and `DateStamp()`, including guest pointer identity and aliasing. |
| `dos_strings.c` | Second DOS batch in RAM: only: structure conversions and NDK layout assertions, dates, names/CLI/arguments, parsing, counted output, DOS-owned SetVBuf and assign ownership/removal. Separate `exit` mode flushes a line and must end with RC 7. |
| `dos_readargs.c` | `ReadArgs()` with a `/M`, `/K/N`, `/K` and `/S` template, the way most CLI tools parse their arguments, then on a string of its own through an `RDArgs` from `AllocDosObject()` with `RDA_Source`. With `INPUT` it also reads the first line of its redirected input, which `ReadArgs()` must leave unread. |
| `dos_lvo.c` | Isolated regressions for historically misplaced DOS thunks, selected by `fprintf`, `cli`, `freedosobject`, `launch` or `system`. The CLI case checks the shared guest identity returned by `Cli()`, `ThisTask.pr_CLI` and `FindTask(NULL)->pr_CLI`, and the command line in `pr_Arguments` of both. The launch case checks that `CreateNewProc()` and `CreateProc()` with 68k code fail with `ERROR_NOT_IMPLEMENTED` instead of running it natively. The system case checks `SystemTagList()` without tags and with `NP_StackSize`. |
| `dos_exall.c` | `ExAll()` and `ExAllEnd()` in a drawer of `RAM:` with four files, a comment and a subdrawer: every type from `ED_NAME` to `ED_OWNER`, scans in several calls, a buffer too small for one entry, invalid types, a lock on a file, `eac_MatchString`, `eac_MatchFunc` with its registers (A0 hook, A1 entry, A2 type) and `ExAllEnd()`. It checks only what AmigaOS 3.2.3 and the bridge both do: an absent comment may be NULL or empty, and the error for a small buffer any non-zero code. |
| `exec_memory.c` | `AllocMem()`, `AllocVec()` and pools with and without `MEMF_CLEAR`, reuse and alignment, `AvailMem()` free/largest/total queries and query precedence, `DeletePool(NULL)` and deletion with outstanding allocations restoring the free total, cleared pool requests above puddle and threshold sizes, `CopyMem()`, signals, `FindTask(NULL)` returning `SysBase->ThisTask` every time, and semaphores in 68k memory: `InitSemaphore()`, exclusive and shared obtains and attempts with nesting, release to zero, `ObtainSemaphoreList()` and `ReleaseSemaphoreList()`, and `AddSemaphore()`, `FindSemaphore()` and `RemSemaphore()` on the public list of the guest ExecBase, which other programs do not see. With `procure` or `vacate` it only calls that function, which must end the program with rc 20. |
| `exec_lists.c` | exec list functions (`AddTail()`, `AddHead()`, `RemHead()`, `RemTail()`, `Insert()`, `Remove()`, `Enqueue()`, `FindName()`) on a list in 68k memory. |
| `exec_syslists.c` | Read-only bounded successor walks over the seven embedded Exec lists, checking header, tail and predecessor links. Node counts can differ between native AmigaOS and the guest view, which exposes empty headers for native lists without guest nodes. |
| `exec_public.c` | AddPort/FindPort/RemPort on the guest ExecBase list, native-only name invisibility, refused residents/resources, CacheClearE, AllocAbs inside a freed block and FreeMem, AllocEntry/FreeEntry and partial-failure cleanup. With the argument `putmsg` it only sends a message to its published port, which must end the program with rc 20. Compiling the same source with KAFFEINE_NATIVE_PORT_FIXTURE produces the optional native port companion. |
| `exec_timer.c` | timer.device through Exec, as C programs use it: `CreateMsgPort()`, `CreateIORequest()` of a timerequest, `OpenDevice()`, `DoIO()` with `TR_GETSYSTIME` and `TR_ADDREQUEST`, `AddTime()`, `SubTime()`, `CmpTime()`, `ReadEClock()` and `GetSysTime()` through `TimerBase`, `CloseDevice()` and the deletes, twice, with the guest memory back each time; units VBLANK, WAITUNTIL and ECLOCK; `AllocSignal()`, `Signal()` of the current task and `Wait()`; the errors for `TR_SETSYSTIME`, an EClock delay, another device and a small request. Delays are measured with the timer itself. With `sendio` it only calls `SendIO()`, which must end the program with rc 20. |
| `exec_libraries.c` | 68k libraries from disk (`emu68test_lib.c`, built as `emu68test.library` with a WORD offset function table and with `-DLONG_TABLE` as `emu68test_l.library` with LONG function pointers, whose `Expunge()` removes the node, frees the base and gives the seglist back unless the library is open; the WORD library takes its `lib_Version`, `ln_Name` and `lib_IdString` from its resident tag alone, which `InitResident()` copies before the init), each through the same life, as on AmigaOS 3.2.3: `OpenLibrary()` from `PROGDIR:` with version 2 making the library from its `RTF_AUTOINIT` resident tag, the fields set by its `InitStruct()` table (`ln_Type`, `lib_Version`, `lib_Revision` and a WORD and a LONG in the extension of the base) and by its init function (`ln_Name`, `lib_IdString`, `seglist`, `SysBase` and the count of inits), `lib_NegSize` rounded up to a multiple of four from the ten vectors, `lib_PosSize` and `lib_OpenCnt`; calls to `TestAdd()`, `TestInits()`, `TestOpens()`, `TestSysBase()` and `TestSegList()` with the size of the first hunk before `BADDR()` of the seglist; a second `OpenLibrary()` of the name alone returning the same base with `lib_OpenCnt` 2, `TestInits()` 1 and one more open in `TestOpens()`; a third one with `0x12340002` in D0 returning it too, since Exec takes the low word as the version; version 3 refused, which calls the `Expunge()` of the open library and so sets `LIBF_DELEXP`; the last of the two `CloseLibrary()` calls expunging it, after which the name alone finds none, since the library is not in `LIBS:`. Then both loaded afresh from `PROGDIR:` (`TestInits()` and `TestOpens()` 1, no `LIBF_DELEXP`, bases of their own), closed, which keeps them, version 3 refused again, which leaves a closed library as it is, opened again from the cache with `TestOpens()` 2, and expunged at the last close, which `TestExpunge()` asks for, so that the name alone finds none and a later run starts afresh. Needs `--files exec_libraries BUILD_DIR/exec_libraries-files`. |
| `gfx_window.c` | A window on the default public screen and its RastPort, as C programs use them: the size, borders, title and RastPort of the `Window`; the font, text height, width and baseline of the `RastPort`; pens, drawing modes and the pen position through graphics.library and through the fields; `RectFill()`, `WritePixel()` and `Draw()` read back with `ReadPixel()`, which on a hi- or truecolor screen gives the low byte of the pixel, so pixels of the same pen are compared; `TextLength()`, `TextExtent()`, `TextFit()` and `Text()`; topaz.font 8 through `OpenFont()`, `SetFont()` and `CloseFont()`. Twice, with the guest memory back after the second window. Then windows on the public screen of `LockPubScreen(NULL)`: through `WA_PubScreen`, NULL for the default one, a `WA_PubScreenName` that is not there with and without `WA_PubScreenFallBack`, and `OpenWindow()` with a NewWindow of Type `PUBLICSCREEN` (the screen and NULL), `WBENCHSCREEN` and `CUSTOMSCREEN`, each lying on that screen through `WScreen`, as a visitor window or not, with the guest memory back. With the argument `badscreen` it only opens a window with `WA_PubScreen` of something that is not a screen, which must end the program with rc 20. |
| `gfx_idcmp.c` | The IDCMP messages of a window, with no input needed: a window opened active gets `IDCMP_ACTIVEWINDOW` and `IDCMP_INTUITICKS`, whose classes, window and times are checked; `Wait()` on the signal bit of `win->UserPort`; `WaitPort()` returning the message that `GetMsg()` takes; `SizeWindow()` and `MoveWindow()` bringing `IDCMP_NEWSIZE` and `IDCMP_CHANGEWINDOW` with the new size and place in the Window; twenty messages through `GetMsg()`, twenty through `GT_GetIMsg()` and twenty through `WaitPort()` before `GT_GetIMsg()` without keeping guest memory; the window closed with a message from `GetMsg()`, one from `GT_GetIMsg()` and one that `WaitPort()` showed, none of them replied; `ModifyIDCMP()` giving a window without flags its IDCMP port and taking it away. |
| `gfx_request.c` | EasyRequests with no input needed: the text and gadgets of an `EasyStruct` from printf-style formats with arguments; `BuildEasyRequestArgs()` giving a requester window with its size, RastPort and title, `SysReqHandler()` without waiting finding no answer yet, and `FreeSysRequest()`; the usual loop of `WaitPort()` on the port of the requester and `SysReqHandler()`, which takes the message that `WaitPort()` showed, and `EasyRequestArgs()`, both with `IDCMP_INTUITICKS` among their IDCMP flags, returning -1 at the first tick; the guest memory back after each. |
| `gfx_menus.c` | GadTools menus with no input needed: a VisualInfo of the default public screen; a strip from a NewMenu array with titles, items, a separator, a submenu, command keys, a command string, a disabled menu and item, CHECKIT items that exclude each other, a toggle, an ignored entry and UserData everywhere, read back through the 68k structures; its layout with the program's own TextAttr; `SetMenuStrip()`, `ItemAddress()`, `OffMenu()`, `OnMenu()`, flags changed by the program and `ResetMenuStrip()`, `ClearMenuStrip()` and `FreeMenus()`; a strip of items only; the secondary error of an invalid array; the guest memory back at the end. |
| `gfx_bevel.c` | `DrawBevelBox()` of gadtools.library into a window's `RastPort` on the default public screen, with no input needed: a VisualInfo of the default public screen; four 40x20 boxes cleared to pen 0 with `SetAPen()` and `RectFill()` and read back at their four edge midpoints and centre with `ReadPixel()`: a raised box with `GT_VisualInfo` whose top and left edges share the shine pen, whose bottom and right edges share the shadow pen, with shine and shadow distinct and non-zero and the centre still 0; a recessed box with `GTBB_Recessed, TRUE` swapping the shine and shadow edges while keeping the centre 0; a call without `GT_VisualInfo` (`GTBB_Recessed, FALSE` only) that draws nothing so all five points stay 0; and a ridge box with `GTBB_FrameType, BBFT_RIDGE` whose top edge is non-zero. |
| `gfx_gadgets.c` | GadTools gadgets with no window and no input needed: a VisualInfo of the default public screen, a context and a gadget of every kind but LISTVIEW_KIND with labels, the program's own TextAttr and UserData, read back through the 68k structures: the links of the list with the level display of a slider and the arrows of a scroller before them, IDs, UserData, flags, the label with its underlined key letter, a checked checkbox, the StringInfo of a string cut to MaxChars and of an integer, the PropInfo of a slider and a scroller, a generic gadget as an ExtGadget with its bounds; NULL for no previous gadget or VisualInfo; `FreeGadgets()` with the guest memory back. |
| `gfx_gadget_attrs.c` | The attributes of GadTools gadgets through `GT_GetGadgetAttrsA()`, with no window and no input needed: a gadget of every kind but LISTVIEW_KIND made with values to read again, a disabled button, a checked checkbox, a cycle on its third label, an mx, a palette with a colour table, a text, a text that copies its string while the program reuses its buffer, a number, a slider with a negative range and level, a scroller, a string and an integer; every value and the count that the autodoc gives, also for cycle and palette attributes, repeated tags and `TAG_MORE`, `TAG_SKIP` and `TAG_IGNORE`; nothing for attributes of another kind, a generic gadget, the context and an arrow of a scroller, whose storage stays as it was; NULL for no gadget or list; only the bits of `MoreFlags` that the NDK gives; the guest memory back. |
| `gfx_gadget_setattrs.c` | Changing the attributes of GadTools gadgets with `GT_SetGadgetAttrsA()`, with no input needed: for a gadget of every kind but LISTVIEW_KIND and GENERIC_KIND, each attribute that the autodoc gives for its kind submitted, `GA_Disabled`, `GTCB_Checked`, `GTCY_Labels`, `GTCY_Active`, `GTMX_Active`, `GTPA_Color`, `GTPA_ColorOffset`, `GTPA_ColorTable`, `GTTX_Text`, `GTNM_Number`, `GTSL_Min`, `GTSL_Max`, `GTSL_Level`, `GTSC_Top`, `GTSC_Total`, `GTSC_Visible`, `GTST_String` and `GTIN_Number` read back with `GT_GetGadgetAttrsA()`, while `GTTX_FrontPen`, `GTTX_BackPen`, `GTTX_Justification`, `GTNM_FrontPen`, `GTNM_BackPen`, `GTNM_Justification`, `GTNM_Format`, `GTSL_LevelFormat`, `GTSL_Justification` and a NULL `GTSL_DispFunc` are accepted without a readable effect; right after `GT_SetGadgetAttrsA()`, before any `GT_GetGadgetAttrsA()`, the 68k structures that the setter updates (`Flags & GFLG_DISABLED` of the button, `GFLG_SELECTED` of the checkbox, `HorizPot` and `VertPot` of the slider's and scroller's `PropInfo`, `Buffer` of the string's `StringInfo` and `LongInt` and `Buffer` of the integer's `StringInfo`); `GTCY_Labels` resetting `GTCY_Active` to 0 before a later `GTCY_Active` in the same list applies, an out-of-range `GTMX_Active` ignored, `GTSL_Level` clamped to `GTSL_Min..GTSL_Max`, `GTSC_Top` kept within `GTSC_Total` less `GTSC_Visible`, `GTST_String` cut to `GTST_MaxChars` and NULL emptying the buffer, `GTTX_Text` NULL clearing the text, `GTTX_Text` after `GTTX_CopyText` borrowing the new string, and the button taking the first `GA_Disabled` of a list while the other kinds take the last of a repeated tag; `TAG_MORE`, `TAG_SKIP` and `TAG_IGNORE`, and NULL for no gadget or no tag list; a window part for `GT_SetGadgetAttrsA()` on the palette, `OffGadget()` and `OnGadget()` with and without a window and on the scroller with its two arrows, `ModifyProp()` on the slider and `RefreshGadgets()`; the guest memory back after `FreeGadgets()`. |
| `gfx_gadget_listview.c` | A GadTools `LISTVIEW_KIND` gadget with no input needed: two Exec Lists built with `NewList()` and `AddTail()`; a listview of 10-pixel items on the first List with `GTLV_Top` 4, `GTLV_Selected` 2 and `GTLV_ShowSelected` NULL; the two arrows and the vertical prop with a PropInfo that GadTools puts before it; `GTLV_Labels`, `GTLV_Selected`, `GTLV_Top` (the 4 that AROS's builder loses and the bridge gives again) and `GA_Disabled` through `GT_GetGadgetAttrsA()`, and `GTLV_Total` and `GTLV_Visible` under their NDK numbers; `GTLV_Top` moving the prop's PropInfo right after the call and clamped, `GTLV_Selected` ~0 as -1, `GTLV_MakeVisible` 7, 100 and 0x8000, `GA_Disabled` on the listview and its prop; detaching with ~0, changing the List while detached and attaching it again, another List, NULL; a second listview created disabled with its scroller disabled too; a third with a `GTLV_ShowSelected` string gadget, moved below it, showing the selected name right after each change, cut to its MaxChars; the guest memory back after `FreeGadgets()`. |
| `gfx_gadget_window.c` | A GadTools gadget list on a window, with no input needed: a VisualInfo of the default public screen; a context and four gadgets with a topaz 8 TextAttr (`BUTTON_KIND`, `STRING_KIND` with `"abc"` and `GTST_MaxChars` 20, `CHECKBOX_KIND` with `GTCB_Checked` TRUE, `SLIDER_KIND` with range 0..10 and level 4), each below the previous inside a 300 by 160 window; a first window opened with `WA_Gadgets` and refreshed with `GT_RefreshWindow()`, each gadget keeping its `GadgetID` and place, `GT_GetGadgetAttrs()` reading back `GTST_String`, `GTCB_Checked` and `GTSL_Level`, and `ReadPixel()` on the top edge of the button's frame reading a pen other than 0; `RemoveGList()` taking the list off and leaving the same number of gadgets with a NULL `NextGadget` at the end, `AddGList()` at `~0` and at `0`, `RefreshGList()` and `RemoveGList()` putting the list back and taking it off again, then `AddGList()` at `~0` and `CloseWindow()` with the list on it before `FreeGadgets()`; a second window opened without `WA_Gadgets` taking a new list made the same way through `AddGList()`, `RefreshGList()` and `RemoveGList()`; a third one opened active with `WA_Gadgets`, waited for until `IDCMP_ACTIVEWINDOW`, where `GT_SetGadgetAttrs()` with the window counts a new text in `NumChars` at once, `OffGadget()` and `OnGadget()` change the button, and `ActivateGadget()` of the string gives TRUE and leaves it active, FALSE without the window; the guest memory back at the end. |
| `gfx_gadget_links.c` | The links of a GadTools gadget list on a window and the memory that `FreeGadgets()` gives back, with no input needed: a VisualInfo of the default public screen and a context and three topaz 8 `BUTTON_KIND` gadgets opened on a window with `WA_Gadgets` and refreshed with `GT_RefreshWindow()`; `RemoveGList()` of the second button with 1 giving the position of the context plus 2, leaving its `NextGadget` on the third button rather than NULL and linking the first button to the third, and `AddGList()` with 1 putting it back at that position and restoring the links; `RemoveGList()` of the context and the first button with 2 giving the position of the context and leaving the first button linked to the second, and `AddGList()` with 2 putting them back at that position; removing the context and the three buttons one by one with 1 giving that same position each time and leaving all `NextGadget` links intact with NULL at the end, `AddGList()` of the whole list with -1 at that position putting them back, and `RemoveGList()` with -1 taking them off from that position again; `AvailMem(MEMF_ANY)` before and after showing that `FreeGadgets()` gives all memory back for a whole list never put on a window, for a list whose gadgets were removed from a window one by one with 1 or all at once with -1 before `CloseWindow()`, and for a list with a `GTYP_BOOLGADGET` of the program's own spliced between the two buttons, whose `NextGadget` still names the second button afterwards, while cutting `NextGadget` of the first button to NULL leaves the second button allocated after `FreeGadgets()` of the head until `FreeGadgets()` of the second button frees the rest. |
| `gfx_gadget_keys.c` | Keys typed into a GadTools string gadget: a window opened active with a context and an empty `STRING_KIND` gadget in `WA_Gadgets`, waited for until `IDCMP_ACTIVEWINDOW`; `ActivateGadget()` of the string gives TRUE; the program then writes `SYS:EMU68/NAME-keys-N.txt` for `--keys`, NAME being the name that it runs under and N 1 from the Shell or 2 through Run, which it tells apart by the `NAME-shell-rc.txt` that `S:User-Startup` writes as the Shell execution ends, and within 60 seconds the `IDCMP_GADGETUP` of the string comes with the gadget as `IAddress` and Code 0, which Return gives, and the string holds `"abc"` with `NumChars` 3, as `GT_GetGadgetAttrs()` gives it, its `GadgetID` and `UserData` the program's. Run it with `--keys gfx_gadget_keys "a b c ret"`. |
| `gfx_classic_menus.c` | A classic Intuition menu strip that the program builds in its own memory, without GadTools, with topaz 8 positions and sizes and `JAM1` `IntuiText` labels: two `Menu` headers, `"Project"` with `"Open"` (`ITEMTEXT`, `COMMSEQ`, `ITEMENABLED` and `HIGHCOMP`, `Command` `'O'`), `"Check"` (`ITEMTEXT`, `CHECKIT`, `MENUTOGGLE`, `ITEMENABLED` and `HIGHCOMP`) and `"More"` with a `SubItem` chain of two items, and `"Edit"` with `"A"` and `"B"`, `CHECKIT` items that exclude each other through `MutualExclude`, B with `Command` `'B'`; on a window with `WA_Activate`, `IDCMP_MENUPICK` and `IDCMP_ACTIVEWINDOW`, with no input, `SetMenuStrip()` giving TRUE with the strip in `Window.MenuStrip`, `ItemAddress()` of `FULLMENUNUM(0, 1, NOSUB)` giving `"Check"` and of `FULLMENUNUM(0, 2, 1)` the second subitem, `OffMenu()` and `OnMenu()` clearing and setting `ITEMENABLED` of `"Open"` and `MENUENABLED` of `"Edit"` (item `NOITEM`) in the program's own structures, and after `ClearMenuStrip()` and setting `CHECKED` on `"A"`, `ResetMenuStrip()` giving TRUE with `"A"` still `CHECKED`; then, after `IDCMP_ACTIVEWINDOW` and the marker of `--keys` as `gfx_gadget_keys.c` writes it, the `IDCMP_MENUPICK` of Right Amiga + O within 60 seconds with `Code` `FULLMENUNUM(0, 0, NOSUB)`, whose `ItemAddress()` is `"Open"` with `NextSelect` `MENUNULL`, and the one of Right Amiga + B with `FULLMENUNUM(1, 1, NOSUB)`: `"B"` `CHECKED` and `"A"`, which it excludes, no longer, with `NextSelect` `MENUNULL`; the guest memory back after `ClearMenuStrip()` and `CloseWindow()`. Run it with `--keys gfx_classic_menus "meta_r+o meta_r+b"`. |
| `gfx_pubscreen.c` | The default public screen as GUI programs read it, with no input needed: `LockPubScreen(NULL)` with a size, a screen font as a TextAttr whose name ends in `.font` and whose size the screen's `RastPort` and `OpenFont()` give again, a bar height, window borders, a GadTools title bar height that fits, a BitMap depth, and `TextLength()` on `&scr->RastPort`; two locks of NULL and one of `"Workbench"` giving the same pointer, which still reads the same with one lock left, and `UnlockPubScreen("Workbench", NULL)` for a lock taken with NULL; the guest memory back after a lock and its unlock and after 1100 of them, each giving a screen; a window with `WA_PubScreen` whose `WScreen` still reads the screen after the unlock, with the guest memory back after `CloseWindow()`; `LockPubScreenList()` with a `Workbench` node whose `psn_Screen` is the locked screen and whose `psn_VisitorCount` follows the locks and unlocks of the program, not an extra unlock; `GetDefaultPubScreen()`, `NextPubScreen()` of NULL and of the screen; `GetVPModeID()` of `&scr->ViewPort`. |
| `gfx_classic_bool.c` | Classic Intuition Boolean (`GTYP_BOOLGADGET`) gadgets that the program builds in its own memory, without GadTools, with no input needed: a list of four gadgets, each with its own `GadgetID` and `UserData`, attached to a 320 by 160 window through `WA_Gadgets`: a plain gadget (`GFLG_GADGHCOMP`, `GACT_RELVERIFY`) with a 5-point `Border` (`FrontPen` 1, `JAM1`) and an `IntuiText` label, a toggle gadget (`GFLG_GADGIMAGE | GFLG_GADGHIMAGE`, `GACT_TOGGLESELECT | GACT_IMMEDIATE`) with 16 by 8 1-plane `Image` bitmaps in `MEMF_CHIP` (all bits set in `GadgetRender`, all bits clear in `SelectRender`), a `GFLG_GADGHBOX | GFLG_RELRIGHT` gadget with a negative `LeftEdge`, a `Border` and two `IntuiText` labels linked through `NextText`, and a `struct ExtGadget` with `GFLG_EXTENDED`, `GMORE_BOUNDS` and a `Border`; each gadget keeping its place, `GadgetID` and `UserData`, `ReadPixel()` on the first gadget's `Border` reading as `FrontPen` 1 and inside the toggle gadget as pen 1, each against a pixel that the program draws in that pen (on a screen of more than 256 colours `ReadPixel()` gives the pen found for a colour, which need not be the pen that drew it), setting `GFLG_SELECTED` on the toggle gadget with `RefreshGList()` drawing its `SelectRender` (AmigaOS, and AROS since #71, draw it as it is, pen 0; AROS before drew an `Image` of a selected gadget inverted) while preserving `GFLG_SELECTED`, and clearing it drawing pen 1 again, `OffGadget()` and `OnGadget()` setting and clearing `GFLG_DISABLED` on the first gadget (`ActivateGadget()` of a Boolean gadget is left out: AmigaOS 3.2.3 and AROS since #71 give TRUE, AROS before FALSE), `RemoveGList()` and `AddGList()` detaching and re-inserting the list at the same position with `RefreshGList()` and `ReadPixel()` on the first gadget's `Border`, `ReadPixel()` on the `GFLG_RELRIGHT` gadget's `Border` at its offset from `win->Width`, `RemoveGadget()` and `AddGadget()` on the third gadget returning the same position, a second window opened through `OpenWindow()` with a `NewWindow` whose `FirstGadget` has a `Border` with `FrontPen` 2 checked with `ReadPixel()`, and the guest memory back after `CloseWindow()`. |
| `gfx_classic_string.c` | Classic Intuition string and integer gadgets (`GTYP_STRGADGET`) that the program builds in its own memory, without GadTools, with keys that the runner types: three gadgets attached through `WA_Gadgets` to a 320 by 160 window opened active, a string gadget with `"Hello"` (MaxChars 32, BufferPos 5, `GFLG_TABCYCLE`), an integer gadget (`GACT_LONGINT`) with `"42"` (MaxChars 12, BufferPos 2, LongInt 42, `GFLG_TABCYCLE`) that shares the UndoBuffer of the first one, and a `GFLG_STRINGEXTEND` string gadget with `"Pens"` whose StringExtend has topaz 8 from `OpenFont()`, Pens {1, 3}, ActivePens {2, 3} and a WorkBuffer; NumChars of the three right after `OpenWindowTags()`, which counts their text as it draws them; `ReadPixel()` inside the third gadget, where no text is drawn, reading as pen 3, the background of its StringExtend, against a pixel that the program draws in that pen; `ActivateGadget()` of the first gadget giving TRUE and setting `GACT_ACTIVEGADGET`; then the program writes `SYS:EMU68/NAME-keys-N.txt` for `--keys` as `gfx_gadget_keys` does, and the keys give two `IDCMP_GADGETUP` within 60 seconds: the first names the first gadget, which holds `"Hello12"` with NumChars 7 and BufferPos 7, and which Tab ends, activating the integer gadget with its cursor at its BufferPos; the second names the integer gadget, which holds `"99"` with NumChars 2 and LongInt 99, the shared UndoBuffer holding `"42"`; BufferPos 2 that the program sets staying through `RefreshGList()`; `RemoveGList()` of the integer gadget alone giving its position and leaving its NextGadget on the gadget after it, as AmigaOS 3.2.3 does, and `AddGList()` putting it back there; the guest memory back after `CloseWindow()` and `CloseFont()`. Run it with `--keys gfx_classic_string "1 2 tab backspace backspace 9 9 ret"`. |
| `gfx_classic_prop.c` | Classic Intuition proportional gadgets (`GTYP_PROPGADGET`) that the program builds in its own memory, without GadTools, with no input needed (298 checks): four gadgets with their own `PropInfo` and knob `Image` structures attached through `WA_Gadgets` to a 320 by 160 window opened active, a horizontal `AUTOKNOB` gadget (`HorizBody` `MAXBODY / 4`), a vertical `AUTOKNOB | PROPNEWLOOK` gadget (`VertPot` `MAXPOT / 3`, `VertBody` `MAXBODY / 5`), a horizontal gadget with a 16 by 8 1-plane knob `Image` of its own in `MEMF_CHIP` memory with all bits set (`HorizBody` `MAXBODY / 8`), and a horizontal and vertical `AUTOKNOB | PROPBORDERLESS` gadget (`HorizBody` and `VertBody` `MAXBODY / 2`), plus a fifth horizontal `AUTOKNOB` gadget with the same box as the first that never goes on a window; for the four window gadgets right after `OpenWindowTags()`, the `PropInfo` (`Flags`, pots, bodies, `CWidth`, `CHeight`, `HPotRes`, `VPotRes`, `LeftBorder`, `TopBorder`) and knob `Image` (`LeftEdge`, `TopEdge`, `Width`, `Height`, and for the third gadget `Depth` 1, `ImageData` unchanged and `PlanePick` 1), followed by `IDCMP_ACTIVEWINDOW` within 10 seconds; `NewModifyProp()` of the first gadget to `MAXPOT / 2` (`HorizPot` 32767, knob `LeftEdge` 57); writing `HorizPot` `MAXPOT` and `0` in the program and calling `RefreshGList()` (`HorizPot` 65535 and knob `LeftEdge` 114, then 0 and 0); `ModifyProp()` of the third gadget to `MAXPOT / 4` (`HorizPot` 16383, knob `LeftEdge` 34); `NewModifyProp()` of the first gadget with `KNOBHIT` in the flags argument setting `Flags` to `0x103`, and without it clearing `Flags` back to `0x3`; `RemoveGList()` and `AddGList()` of the first gadget returning the same position with its `PropInfo` and knob `Image` unchanged, followed by `RefreshGList()`; `OffGadget()` setting `GFLG_DISABLED` and `OnGadget()` clearing it with the `PropInfo` unchanged, and `ActivateGadget()` of the first gadget giving TRUE; a second window B opened beside the first, `NewModifyProp()` of the first gadget to `HorizPot` 0 on the first window and then to `MAXPOT / 2` with B updating its `PropInfo` and knob `Image` (`LeftEdge` 57, `Width` 38, `Height` 8), drawing the knob into B (`ReadPixel()` inside the knob differing from B's background pixel read before) while leaving the pixel at that point in the first window as its background, before closing B; `NewModifyProp()` of the fifth gadget (on no window) with the first window to `MAXPOT / 4` and `MAXBODY / 4` setting `Flags`, `HorizPot`, `VertPot`, `HorizBody` and `VertBody` (`CWidth` to `TopBorder`, the knob `Image` and drawing are left out because AmigaOS 3.2.3 computes and draws them while the bridge stores the five values alone); `NewModifyProp()` of the first gadget on the first window with `FREEHORIZ` without `AUTOKNOB` to `MAXPOT / 4` setting `Flags` to `0x2`, `HorizPot` to 16383 and knob `LeftEdge` to 28 with `Width` 38 and `Height` 8 unchanged, and back with `AUTOKNOB | FREEHORIZ` to `MAXPOT / 2` restoring `Flags` to `0x3`; the `PropInfo` still as it was after `CloseWindow()`, and the guest memory back after `CloseWindow()` and `FreeMem()` of the image data. |
| `gfx_mouse.c` | The mouse on a window of the program's own, with mouse input that the runner sends (40 checks): a 300 by 150 window opened inactive at 40,40 of the default public screen, with a GadTools `BUTTON_KIND` gadget in `WA_Gadgets` and, through `AddGList()`, a Boolean gadget (`GACT_IMMEDIATE | GACT_RELVERIFY`) and two horizontal `AUTOKNOB` proportional gadgets (`HorizBody` `MAXBODY / 4`) that the program builds in its own memory, the second with `GACT_FOLLOWMOUSE`; the program writes `SYS:EMU68/NAME-mouse-N.txt` for `--mouse` as `gfx_gadget_keys` does for `--keys`, and waits up to 60 seconds for the first message and 20 or 30 for each later step: a click that activates the window (`IDCMP_ACTIVEWINDOW`, and the `SELECTUP` at the click), a second click (`SELECTDOWN` and `SELECTUP` at it, the first with `IEQUALIFIER_LEFTBUTTON`), the `IDCMP_GADGETUP` of the button with the button as `IAddress`, `IDCMP_GADGETDOWN` and `IDCMP_GADGETUP` of the Boolean gadget with the program's gadget as `IAddress`, a drag of the first knob by 57 pixels, half its travel (selected, with `KNOBHIT`, as it is pressed; `HorizPot` 32767, or 32768 as on AmigaOS 3.2.3, and `KNOBHIT` after the release) during which the program reads the gadget without reading messages, and a drag of the second knob by 76 pixels in steps whose `IDCMP_MOUSEMOVE` messages show `HorizPot` growing and the last pointer position (`HorizPot` 43690 after the release), and no `IDCMP_MOUSEBUTTONS` from the second click to a second after the last step; lines that start with `info` report what it read without messages and the `IAddress` of `IDCMP_MOUSEMOVE`. Run it with `--mouse gfx_mouse` and the script of the results below. |
| `gfx_workbench.c` | `workbench.library` AppWindow, AppMenuItem and AppIcon registration, removal and `AppMessage`s on a `MsgPort` of the program's own, selected by `register` (default, no input needed, 28 checks), `appicon` (28 checks), `appwindow` (22 checks) or `appmenu` (18 checks): `OpenLibrary("workbench.library", 37)` into `WorkbenchBase`, `CreateMsgPort()`, a 300 by 120 window opened inactive at 40,40 of the default public screen, `AddAppWindowA(1, 0x1234, window, port, NULL)`, `AddAppMenuItemA(2, 0x5678, "gfx_workbench item", port, NULL)` and `AddAppIconA(3, 0x9ABC, "gfx_workbench", port, 0, &icon, NULL)` with a `DiskObject` built in the program's own memory as the autodoc of `AddAppIconA()` has it (no magic, no version, a 32 by 16 gadget without flags, so without `GFLG_GADGIMAGE`, whose `GadgetRender` is a 32 by 16 2-plane `Image` with `PlanePick` 3, `PlaneOnOff` 0 and `ImageData` in `MEMF_CHIP`, `do_CurrentX` 500 and `do_CurrentY` 40), in `register` and `appicon` mode four more AppIcons at `do_CurrentY` 110 from `do_CurrentX` 380 on, each `AddAppIconA()` succeeding (a `DiskObject` as an icon file has it, with `WB_DISKMAGIC`, `WB_DISKVERSION`, a `GTYP_BOOLGADGET` with `GFLG_GADGIMAGE | GFLG_GADGHIMAGE` and `GACT_RELVERIFY | GACT_IMMEDIATE`, an inverted select `Image` and `do_StackSize` 4096; one with `GFLG_GADGHNONE` and the select `Image`; a 1-plane `Image` with `PlanePick` 2; and a 2-plane `Image` with `PlanePick` 1, `PlaneOnOff` 2 and only the plane of data that `PlanePick` takes, at the end of the program's chip block), in `register` mode `AddAppIconA()` giving NULL, as on AmigaOS 3.2.3, for seven DiskObjects (`GFLG_GADGHIMAGE` and `GFLG_GADGHNONE` without a select `Image`, and a render `Image` of `Width` 0, `Height` 0, `Depth` 0 or `Depth` 9, or without `ImageData`), `GetMsg()` returning NULL right after registration, and `RemoveAppIcon()` of each AppIcon, `RemoveAppMenuItem()` and `RemoveAppWindow()` each returning TRUE in that order before closing the window, freeing the chip memory, deleting the port and closing the libraries; in the three message modes, after registration the program writes `SYS:EMU68/NAME-mouse-N.txt` for `--mouse` as `gfx_mouse` does, waits up to 60 seconds for an `AppMessage` on the port and replies it with `ReplyMsg()` before removing the App objects, in `appwindow` mode waiting for the signal of the port only and removing the AppWindow with `RemoveAppWindow()` while the `AppMessage` waits in the port, neither taken nor replied, before `GetMsg()` takes it (`appicon`: `am_Type` `AMTYPE_APPICON`, `am_ID` 3, `am_UserData` `0x9ABC`, `am_NumArgs` 0, `am_Version` at least `AM_VERSION`, and an `info` line with `am_MouseX,am_MouseY`; `appwindow`: `am_Type` `AMTYPE_APPWINDOW`, `am_ID` 1, `am_UserData` `0x1234`, `am_NumArgs` at least 1, `am_ArgList[0].wa_Lock` non-zero and `NameFromLock()` succeeding, with the path and `wa_Name` in an `info` line; `appmenu`: `am_Type` `AMTYPE_APPMENUITEM`, `am_ID` 2 and `am_UserData` `0x5678`). Run `register` without arguments, or each interactive mode with `--args NAME MODE` and `--mouse NAME SCRIPT` (for example `gfx_workbench_icon=build/gfx_workbench --args gfx_workbench_icon appicon --mouse gfx_workbench_icon ...`, and likewise with `appwindow` and `appmenu`). |
| `exec_memheader.c` | `Allocate()` and `Deallocate()` on a guest `MemHeader` built in an `AllocMem()` block: alignment, rounding, splitting, exhaustion, coalescing and reuse, without calling stdio or malloc. |
| `utility_tags.c` | Guest tag lookup and iteration with controls, chains and rings; PackBoolTags duplicate/mask rules; FilterTagChanges with and without apply; both MAP types and filter logics; guest allocation, flat clones, NULL and ring clones, refresh of tag/data pairs with the tail left alone, and ignored foreign frees; BE32 tag arrays; Stricmp/Strnicmp ASCII and extended-byte case rules, signed differences, limits and invalid pointers. |
| `utility_dates.c` | `Amiga2Date()`, `Date2Amiga()` and `CheckDate()` from utility.library on `struct ClockData` in 68k memory: known timestamps (`0`, `1`, leap day `2000-02-29`, last second of years, non-leap century year `2100`, `0xFFFFFFFF`), round trips on boundary and pseudo-random values, invalid dates for `CheckDate()`, out-of-range months for `Date2Amiga()`, and `NULL` addresses. |
| `locale.c` | `locale.library` catalog fallback (`OpenCatalogA()` returning NULL, `GetCatalogStr()` returning the default string pointer unchanged, `CloseCatalog()`), `OpenLocale(NULL)` and its 68k `struct Locale` fields and strings, `IsAlpha()`, `IsDigit()`, `ConvToUpper()`, `ConvToLower()`, `GetLocaleStr(DAY_1)` caching, `StrConvert()`, `StrnCmp()`, `FormatDate()` of a known date through an assembler/C hook, a `ParseDate()` round trip and failed-parse date preservation, `FormatString()` with mixed positional WORD/LONG/string/BSTR arguments and repetition, its advanced guest pointer, literals and NULL strings, guest locale/character registers and terminating zero, the same date and format calls with a NULL locale (the default locale, NULL in A2) and a NULL `FormatString()` template, refusal of guest `FreeMem()` on the open locale block, and `CloseLocale()`. |
| `math_ieee.c` | Every vector of mathieeedoubbas.library and mathieeedoubtrans.library called from 68k code built for the 68000, without FPU code: arguments and results in D0/D1 and D2/D3, the operand order of `IEEEDPPow()`, integer and single precision conversions, the cosine `IEEEDPSincos()` writes through A0, and N, Z and V after each call with X, C and the other registers kept. An assembly helper sets the CCR before the call and reads it with Scc and ADDX right after. |
| `math_ieee_cases.py` | Prints the case table of `math_ieee.c`: exact IEEE 754 results as bit patterns, and host libm results with a tolerance for the transcendental functions. |
| `c_stdio.c` | A libnix C program with stdio: `argv`, `printf()` and `sprintf()` formats, including floating point through the `printf()` of libnix's `libm.a`, the `malloc()` family, `strdup()`, `qsort()`, `strtol()`, and `stderr`, which libnix takes from `pr_CES` of the process: the error stream that the runner redirects with `*>` must reach it. Built as `c_stdio`, and as `c_stdio_vec` with its own `malloc()` family on `AllocVec()`. |
| `libgcc_ieee.c` | `__fixdfsi()` and `__floatsidf()` on mathieeedoubbas.library, which the floating point `printf()` of libnix needs and the empty libgcc of the toolchain lacks. Linked into `c_stdio` and `c_stdio_vec`. |
| `c_compute.c` | Compiled C code with few library calls: libnix `argv`, `qsort()`, `strtol()`, string functions, a CRC32, 32 and 64-bit integer arithmetic. Built for the 68000 (`c_compute`, which also checks the FPU flags and the IEEE math libraries) and for the 68020 with 68881 instructions (`c_compute_fpu`, with double precision arithmetic). |
| `bzip2_amiga.c` | What bzip2 1.0.8 needs beyond libnix to run as a target program: `fchmod()` and `fchown()`, which succeed without doing anything. |
| `libgcc_ieee_ports.c` | `__floatsisf()` and `__fixsfsi()` on mathieeesingbas.library and `__floatunsidf()` on mathieeedoubbas.library, the conversions that bzip2 1.0.8 and the IJG JPEG library 9f call and the empty libgcc of the toolchain lacks. Linked into `bzip2`, `cjpeg`, `djpeg` and `jpegtran`. |
| `jpeg_jconfig.h` | The `jconfig.h` of the IJG JPEG library 9f for m68k-amigaos-gcc with libnix, after the `jconfig.sas` of its distribution: Unix command line with standard input and output, everything in memory with `jmemnobs.c`. |
| `m68ktest.h` | Check helpers of the C programs. Every check prints a line that starts with `ok` or `FAIL`, output goes through dos.library only and is flushed after each line, and each program ends with a summary line. |
| `Makefile` | Builds the programs into `build/`, bzip2 1.0.8 when `BZIP2_SRC` names its source, and `cjpeg`, `djpeg` and `jpegtran` of the IJG JPEG library 9f when `JPEG_SRC` names its source. |
| `hunkdump.py` | Prints the hunks and the relocation format of an executable. |
| `run-raspi-qemu.py` | Copies an SD image, installs the library, the programs and their launch scripts, boots the given kernel and BSP package, and reports VM completion separately from program results. |
| `test_run_raspi_qemu.py` | Offline tests of result parsing and per-program completion, of the keys and mouse scripts and of the reports that move the pointer exactly, without QEMU or a cross compiler. |
| `math_ffp.c` | FFP arithmetic, conversion and condition-code behavior. |
| `exec_library_paths.c` | Native library lookup with explicit paths and basename resolution. |
| `exec_console.c` | The no-window console.device adapter and RawKeyConvert boundary. |
| `exec_diskfont.c` | OpenDiskFont and original monochrome font fixtures. |
| `exec_messages.c` | FIFO messages, signals, replies and nonempty WaitPort. |
| `exec_guest_tasks.c` | Parent/child task identity, Forbid/Permit, Wait inside Forbid, signals, self-removal and normal child return. |
| `exec_guest_messages.c` | Child-owned CreateMsgPort, task-to-task PutMsg, blocking WaitPort inside Forbid, FIFO peek/removal, payload processing, ReplyMsg and port cleanup. |
| `exec_guest_task_window.c` | A child-owned IDCMP window, native Intuition ticks waking a guest child inside Forbid, replies, window cleanup and task removal without user input. |
| `exec_message_lifetime.c` | Manual ports drained by RemHead, RemTail, Remove and inline Node links, followed by FreeVec or FreeMem. Reacquires the freed bytes with AllocAbs before reusing them, then checks an unrelated message roundtrip, payload guards and the reused buffer. |
| `exec_packets.c` | Read-only ACTION_READ packets and shared DOS file position. |
| `gfx_planar_read.c` | Original 32 by 3 planar images at depths 1, 2 and 8. Manually initialized public RastPort/BitMap structures, all 288 in-range pixel values, Flags 0/BMF_STANDARD, and byte-for-byte preservation of both structures, planes and guards. |
| `gfx_display_info.c` | NULL-handle display queries by native mode ID: classic DISP/DIMS/NAME/MNTR layouts, headers, every requested length0..104 (monitors0..120), partial rectangles, reserved-tail/guard preservation, unavailable modes and stable monitor names/scalars/identities. |
| `gfx_layer_topology.c` | Stable Window/RastPort layer identities, live depth order while polling without OS calls, backfill sentinels, backdrop flag preservation and removal of a closed sibling. The native ARM build allows an input-handler turn after depth calls; current native priority classes are preserved. |
| `gfx_layer_resources.c` | Drawing through the internal RastPort of an owned screen bar, pen state and indexed pixels, repeated screen retirement, and independent owned NewLayerInfo/DisposeLayerInfo lifetimes. |
| `gfx_private_layers.c` | Original offscreen planar BitMaps, private layers, synchronous backfill register ABI and object/task identities, guest-plane drawing, hook replacement, layer order and repeated deletion/recreation. BltBitMap copy, clear and masks are checked directly in the planes; return checks match the AROS planar-driver result of 8, independently observed in the native ARM build. |
| `gfx_private_regions.c` | Owned Regions and big-endian rectangle lists, exact union geometry and relative bounds, installing/replacing/clearing clip Regions, and clipped drawing checked directly in the planar storage over two lifetimes. |
| `gfx_bitmap_transfer.c` | Standalone planar BitMaps and an owned indexed screen, with two-way native bitmap transfers, exact guest-plane pixels, native return values and bitmap identity after RastPort drawing. Built for ARM as an independent native reference as well as for 68k. |
| `gfx_bitmap_guard.c` | Expected containment: literal cybergraphics GetCyberMapAttr LVO with no converted bitmap support must stop with return code 20 before returning to guest code; the STEP line distinguishes successful setup from a missing library. |
| `gfx_drawinfo.c` | Original DrawInfo scalar/font/pen snapshots, stable nested gets, exact pen values and surviving references, followed by text metrics and screen retirement; ARM native reference and classic68k target use the same source. |
| `gfx_window_bitmap.c` | Owned-window RastPort bitmap identity, readable depth and typed attributes shared with the screen, preserved through drawing and window retirement; the same source runs natively on ARM and through the 68k bridge. |
| `gfx_owned_screen.c` | Owned indexed screens through OpenScreenTagList/OpenScreen, chained tags, classic fonts and clip rectangles, native error output guards, drawing, window/screen identity, refused close and extended NewScreen. Optional `leave` mode leaves a named public screen and a window for context cleanup. |
| `gfx_screen_cleanup_native.c` | Native AArch64 observer after `gfx_owned_screen leave` in the same boot; the named screen must no longer be lockable. |
| `gfx_framebuffer_palette_native.c` | Native AArch64 Raspberry driver probe: enumerates truecolor and LUT8 modes, verifies actual eight-bit bitmap storage, pre-renders an indexed screen behind Workbench, checks the physical RGBX pixels after showing it, and changes a live palette entry without rewriting the source indices. Requires kernel framebuffer attributes. |
| `icon_objects.c` | Owned DiskObject acquisition/free, NULL free, 96 sequential allocation cycles, exact guest FindToolType result pointers, case-insensitive MatchToolValue and preservation of original strings/arrays. The optional `leave` argument is an AROS bridge teardown probe that leaves three objects for context cleanup. Scalar metadata observations may differ between native backends; no icon images are copied or tested. |

The DrawInfo bridge exposes a classic version-1 snapshot with copied pen
values and a retained, typed TextFont. Nested gets share one identity until
the last matching free. It holds the screen for that lifetime, including a
native visitor lock for borrowed public screens. Native menu image objects
are not exposed; `dri_CheckMark` and `dri_AmigaKey` remain NULL.
`DrawImageState` and `GetDrawInfoAttr` stop with return code 20 until their
Image and attribute conversions are implemented.

## Building the test programs

The C programs need `m68k-amigaos-gcc` with libnix (`-noixemul`); the
assembly test needs vasm. GNU make and Python 3 are also required.

```sh
make -C developer/debug/test/kaffeine_target
```

`M68K_PREFIX`, `M68K_CC`, `M68K_CFLAGS`, `VASM` and `BUILD_DIR` select tools
and output locations. The default output directory is this test directory's
`build/`. `hello_short` uses short HUNK relocations; `hello_long` uses
`HUNK_RELOC32` through vasm's `-kick1hunks` option.

Set `BZIP2_SRC` to an unpacked bzip2 1.0.8 source tree to include its test
executable. Its sample input and reference output files are supplied by
that source distribution.

Set `JPEG_SRC` to an unpacked IJG JPEG library 9f source tree, such as the
one that the AROS build fetches for its Ports, to include `cjpeg`, `djpeg`
and `jpegtran`. The Makefile copies the sources to `build/jpeg-src` with
`jpeg_jconfig.h` as their `jconfig.h`, because the `jconfig.h` of the tree
belongs to the AROS host and a quoted `#include` finds it first. The test
images and reference files are supplied by that source distribution.

## Running on AROS AArch64

`run-raspi-qemu.py --help` lists all options. Supply an AROS raspi-aarch64
SD image, kernel, BSP package and device tree, mtools and qemu-system-aarch64.
The base image is copied for a run. The runner installs the selected library,
programs and launch scripts in that copy.

Example from the repository root:

```sh
python3 developer/debug/test/kaffeine_target/run-raspi-qemu.py \
    --image /path/to/base.img \
    --kernel /path/to/aros-aarch64-raspi.img \
    --bsp /path/to/aros-aarch64-bsp.rom \
    --dtb /path/to/bcm2710-rpi-3-b.dtb \
    --library /path/to/kaffeine.library \
    --mtools /path/to/mtools/bin \
    --run-dir /path/to/results/new-run --separate --run-wait 15 \
    --files exec_packets developer/debug/test/kaffeine_target/build/exec_packets-files \
    developer/debug/test/kaffeine_target/build/exec_messages \
    developer/debug/test/kaffeine_target/build/exec_packets
```

Every program runs from the Shell and through `Run`. The runner waits for
per-program completion markers and reports VM completion separately from
program status. `--separate` gives each program its own boot. Supply a new
run directory for each invocation. An idle timeout is optional; long
calculations can remain quiet, so choose it for the workload.

Options precede program names. `NAME=FILE` installs an executable under an
alias, `--args NAME TEXT` supplies arguments, `--input NAME FILE` redirects
input and `--expect NAME FILE` compares output byte for byte. `--keys` and
`--mouse` drive the guest through QMP. `--keep-image` retains the modified
copy for inspection.

## Companion files

| Program | Runner option |
| --- | --- |
| `exec_diskfont` | `--files exec_diskfont build/exec_diskfont-files` |
| `exec_libraries` | `--files exec_libraries build/exec_libraries-files` |
| `exec_packets` | `--files exec_packets build/exec_packets-files` |

The bitmap font is generated from original fixture data by the Makefile.
The packet fixture is an original 12-byte file. Requests of 5, 9 and 4 bytes
return 5, 7 and 0; ordinary Read and Seek share the position. No AmigaOS ROM,
application or proprietary font is supplied with these tests.

## The IJG JPEG self-test

The seven steps of the library's own `make test` run as seven programs,
each a filter from standard input to standard output, and pass only if the
output is byte for byte the reference file of the distribution:

| Program | Executable and arguments | `--input` | `--expect` |
| --- | --- | --- | --- |
| `jpeg_d_ppm` | `djpeg -dct int -ppm` | `testorig.jpg` | `testimg.ppm` |
| `jpeg_d_gif` | `djpeg -dct int -gif` | `testorig.jpg` | `testimg.gif` |
| `jpeg_d_bmp` | `djpeg -dct int -bmp -colors 256` | `testorig.jpg` | `testimg.bmp` |
| `jpeg_c_int` | `cjpeg -dct int` | `testimg.ppm` | `testimg.jpg` |
| `jpeg_d_prog` | `djpeg -dct int -ppm` | `testprog.jpg` | `testimg.ppm` |
| `jpeg_c_prog` | `cjpeg -dct int -progressive -opt` | `testimg.ppm` | `testimgp.jpg` |
| `jpeg_tran` | `jpegtran` | `testprog.jpg` | `testorig.jpg` |

For example, with `J` the jpeg-9f directory, the first step is
`--args jpeg_d_ppm "-dct int -ppm" --input jpeg_d_ppm $J/testorig.jpg
--expect jpeg_d_ppm $J/testimg.ppm jpeg_d_ppm=build/djpeg`. The same seven
filters give the reference files when they are built for the host from the
same sources and `jpeg_jconfig.h`.

## Expected containment and exit probes

| Program or alias | Argument | Expected exit |
| --- | --- | --- |
| `cpu_faults` | none | 20 after its bounds checks |
| `c_compute_fpu` | normal compute arguments | 20 at the unsupported FPU instruction |
| `exec_timer_sendio=exec_timer` | `sendio` | 20 |
| `dos_strings_exit=dos_strings` | `exit` | 7 |
| `exec_public_putmsg=exec_public` | `putmsg` | 20 for a NULL destination |
| `exec_memory_procure=exec_memory` | `procure` | 20 |
| `exec_memory_vacate=exec_memory` | `vacate` | 20 |
| `cpu_faults_mmu=cpu_faults` | `mmu` | 20 |

These sixteen Shell/Run executions are deliberate nonzero cases. They must
be checked against their expected behavior; raw nonzero status alone is
not a regression verdict. Valid ordinary queues are positive tests in
`exec_messages`.

## Verified results and limits

The combined message/packet compatibility baseline completed 57 separate
boots and 114 Shell/Run executions. Six bzip2 boots added twelve executions
with byte-exact compressed and decompressed outputs. All 63 VMs completed
normally without input errors.
The expected containment/exit cases above account for sixteen executions.

Focused native AmigaOS reference results and AROS results agree on 26
message checks, 28 packet checks and all 20 raw observation rows. The
message reference includes a nonempty queue whose unused arrival metadata
changes before WaitPort/GetMsg. The read-side calls preserve signal state.

The embedded-list regression adds 28 checks on native AmigaOS and in each
AROS Shell/Run execution. The old library fails 21 of these checks in each
mode; corrected guest headers pass all 28 without exposing native nodes.

`exec_memory` passes 62 checks. It reads exactly `$DEAD00` to detect the old
return-sentinel trap, and checks AllocAbs/writes in a known freed block;
fixed addresses can belong to live allocations. Custom-chip window and
`$1DFF000` alias checks remain separate.

The sampled mathffp SPDiv cases retain eight one-ULP differences from
AmigaOS. Personal Paint progresses through packet reads and replies and
the blitter library's memory-list scan, then stops at a later DOS read packet
rejected by the bridge's boundary checks. Complete application
startup, an FPU, general asynchronous/cross-task messaging and physical
hardware have not been established.

## Offline runner checks

```sh
python3 developer/debug/test/kaffeine_target/test_run_raspi_qemu.py
```

These checks exercise parsing, completion markers, input scripts and output
comparison without booting QEMU or using a 68k cross compiler.


## Raspberry framebuffer palette validation — 27 October 2026

The framebuffer driver keeps its original truecolor format and adds a second,
real LUT8 format. Each indexed bitmap owns one byte per pixel and a 256-entry
RGB palette. Refresh converts the requested, clipped rectangle to the hardware
RGBX framebuffer; a palette change refreshes a visible bitmap immediately.
Showing a previously rendered bitmap also refreshes it, even if its position
has not changed. This does not alter the depth reported for truecolor modes.

Build the native probe with the configured AArch64 AROS SDK, separately from
the 68k compatibility tests:

```sh
make -C developer/debug/test/kaffeine_target \
    NATIVE_CC='aarch64-aros-gcc --sysroot=/path/to/AROS/Developer' \
    BUILD_DIR=/path/to/native-tests \
    /path/to/native-tests/gfx_framebuffer_palette_native
```

Rebuild the BSP's `fbgfx.hidd` module with `hidd-fbgfx-quick` in a configured
raspi-aarch64 build and package it with the matching kernel and other BSP
modules. Passing the native probe to the QEMU runner uses the same isolated
image and Shell/Run launch paths as other target programs; its code itself
runs natively, rather than through the 68k CPU backend.

In QEMU the native probe passed **14 checks with zero failures**, in both
Shell and Run. It observed the retained 24-bit mode and an additional 8-bit
mode, verified bitmap depth and 256 palette entries, checked red/green/blue
pixels in the actual hardware framebuffer after the first show, and changed
red to yellow through the palette while the source index remained unchanged.
The portable refresh helper passed **11,042 host checks**, including clipped
rectangles, positive/negative offsets, extreme coordinates, row padding,
source preservation, invalid surfaces and RGBX copies. The same checks passed
with AddressSanitizer and UndefinedBehaviorSanitizer.

These results establish indexed framebuffer storage and conversion on QEMU.
Physical Raspberry Pi validation and full application startup remain separate
gates.


## Owned screen validation — 27 October 2026

The original `gfx_owned_screen` HUNK passed **43 checks with zero failures**
through Moira on AROS AArch64, in both Shell and Run. It selected a genuine
indexed display mode, opened and drew to owned screens, checked classic font
and screen fields, exercised TAG_MORE/TAG_SKIP/TAG_IGNORE and SA_DClip,
verified native errors without changing adjacent words, retained a screen
while a VisualInfo was live, preserved a live
screen after a close refused by an open window, and opened an ExtNewScreen
through the legacy OpenScreen vector. The earlier library failed two of the
five checks reached before the adapter existed.

The optional `leave` mode leaves a VisualInfo too and checks six conditions in each launch path. In the
same boot, the native `gfx_screen_cleanup_native` observer then confirmed
that the named public screen was no longer lockable. This checks that the
bridge closed the guest's remaining window and screen, rather than relying
on a fresh boot to discard them. Run the two programs together without
`--separate`, pass `--args gfx_owned_screen leave`, and allow enough idle
time for the launch script's waits.

The screen host suite passed37 groups; window lifecycle tests and the screen
suite also passed with AddressSanitizer and UndefinedBehaviorSanitizer. All41
host executables passed. AmigaOS comparison for these new owned-screen tests
and physical hardware validation are not established by these QEMU results.
