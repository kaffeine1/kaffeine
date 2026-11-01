#!/usr/bin/env python3
"""Check manual registrations and register conventions in the production thunk tables.

Copyright (C) 2026, The AROS Development Team. All rights reserved.
Author: Michele Dipace (@kaffeine1)

The C preprocessor expands the production header macros for all twenty-two manual
thunk tables (exec, dos, intuition, graphics, utility, locale, mathieeedoubbas,
mathieeedoubtrans, mathieeesingbas, mathffp, timer, console, keyboard, input,
diskfont, gadtools, icon, workbench, cybergraphics, layers, keymap and
Picasso96API). The vectors and registers of Picasso96API come from the inline
header of the Picasso96 SDK in workbench/devs/diskimage, the only fd source of
that library in the tree. Expected
vector numbers come independently from the native AROS entry points and from
the NDK 3.2 fd files (or AROS fd / .conf files for
AROS-only AVL functions and explicitly listed extensions). Each manual handler and the helpers it delegates to
are checked so that every register read matches the fd file and every ignored
argument is explicitly accounted for.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


def env_path(var_name):
    value = os.environ.get(var_name, "").strip()
    return Path(value) if value else None


TABLES = (
    ("dos", "kaffeine_thunks.c", "kaffeine_thunks_dos"),
    ("exec", "kaffeine_thunks.c", "kaffeine_thunks_exec"),
    ("intuition", "kaffeine_thunks.c", "kaffeine_thunks_intuition_manual"),
    ("graphics", "kaffeine_thunks.c", "kaffeine_thunks_graphics_manual"),
    ("cybergraphics", "kaffeine_thunks.c", "kaffeine_thunks_cybergraphics_manual"),
    ("diskfont", "kaffeine_thunks_diskfont.c", "kaffeine_thunks_diskfont_manual"),
    ("icon", "kaffeine_icon.c", "kaffeine_thunks_icon_manual"),
    ("utility", "kaffeine_thunks_utility.c", "kaffeine_thunks_utility_manual"),
    ("locale", "kaffeine_thunks_locale.c", "kaffeine_thunks_locale_manual"),
    ("mathieeedoubbas", "kaffeine_thunks_mathieee.c",
     "kaffeine_thunks_mathieeedoubbas"),
    ("mathieeedoubtrans", "kaffeine_thunks_mathieee.c",
     "kaffeine_thunks_mathieeedoubtrans"),
    ("mathieeesingbas", "kaffeine_thunks_mathieee.c",
     "kaffeine_thunks_mathieeesingbas"),
    ("mathffp", "kaffeine_thunks.c", "kaffeine_thunks_mathffp"),
    ("timer", "kaffeine_thunks_timer.c", "kaffeine_thunks_timer"),
    ("console", "kaffeine_thunks_console.c", "kaffeine_thunks_console"),
    ("keymap", "kaffeine_thunks.c", "kaffeine_thunks_keymap_manual"),
    ("keyboard", "kaffeine_exec_io.c", "kaffeine_thunks_keyboard"),
    ("input", "kaffeine_exec_io.c", "kaffeine_thunks_input"),
    ("gadtools", "kaffeine_thunks.c", "kaffeine_thunks_gadtools_manual"),
    ("workbench", "kaffeine_workbench.c", "kaffeine_thunks_workbench"),
    ("layers", "kaffeine_layers.c", "kaffeine_thunks_layers_manual"),
    ("Picasso96API", "kaffeine_p96.c", "kaffeine_thunks_p96"),
    ("asl", "kaffeine_asl.c", "kaffeine_thunks_asl"),
)

# The Picasso96 SDK in the tree: its inline header names the offset and the
# registers of every function of Picasso96API.library ("jsr -60(a6)").
P96_SDK_INLINE = "workbench/devs/diskimage/include/amigaos/inline/Picasso96API_protos.h"
# Every Picasso96API.library vector of version 2 but p96BestModeIDTagList()
# and p96WritePixelArray() stops the program in the first bridge.
P96_REFUSED = frozenset(range(30, 198, 6)) - {60, 102}
P96_SUPPORTED = {60: "thunk_p96_BestModeIDTagList", 102: "thunk_p96_WritePixelArray"}
# The whole expansion of P96_UNSUPPORTED(name), white space collapsed: the
# two statements and nothing else, so no read can come before the refusal.
P96_UNSUPPORTED_BODY = (
    'static IPTR thunk_p96_##name(struct KaffeineContext *ctx, void *cpu) '
    '{ (void)cpu; return refuse(ctx, "p96" #name "(): an unsupported '
    'Picasso96API.library call"); }')

# NDK 3.2 workbench_lib.fd entries implemented by this bridge. Keep a 68k
# oracle when no external NDK is supplied: AROS's .conf uses A3 for the
# AddAppMenuItemA tag list, whereas the NDK uses A2.
WORKBENCH_NDK32 = {
    48: ("AddAppWindowA", ("D0", "D1", "A0", "A1", "A2")),
    54: ("RemoveAppWindow", ("A0",)),
    60: ("AddAppIconA", ("D0", "D1", "A0", "A1", "A2", "A3", "A4")),
    66: ("RemoveAppIcon", ("A0",)),
    72: ("AddAppMenuItemA", ("D0", "D1", "A0", "A1", "A2")),
    78: ("RemoveAppMenuItem", ("A0",)),
    90: ("WBInfo", ("A0", "A1", "A2")),
}

# Public NDK 3.2 diskfont_lib.fd: ##bias 30, OpenDiskFont(textAttr)(a0).
DISKFONT_NDK32 = {30: ("OpenDiskFont", ("A0",)), 42: ("NewFontContents", ("A0", "A1"))}

# These extensions have no public NDK slot; their register/slot oracle is
# the checked-in native .conf, even when an external AROS fd is older.
AROS_ONLY_FUNCTIONS = {
    ("dos", 1176): "GetSegListInfo",
    ("dos", 1356): "AssignAddToList",
    ("intuition", 936): "GetDrawInfoAttr",
}

# Tables of devices, whose bases are named .device.
DEVICES = ("timer", "console", "keyboard", "input")

# These device slots deliberately stop the guest; they claim no native API.
CONSOLE_REFUSED = frozenset((6, 12, 18, 24, 30, 36, 42, 54, 60, 66, 72))
# The supported keymap profile uses only MapRawKey with the native default.
# No guest pointer may replace or expose the system's native KeyMap tables.
KEYMAP_REFUSED = frozenset((30, 36, 48))
# Every vector of keyboard.device and input.device: Open, Close, Expunge and
# the reserved one, then BeginIO, AbortIO and the .conf function list
# (input's PeekQualifier), which main() checks against the native .conf.
DEVICE_LIBRARY_VECTORS = frozenset((6, 12, 18, 24))
DEVICE_REFUSED = {
    "keyboard": DEVICE_LIBRARY_VECTORS | {30, 36},
    "input": DEVICE_LIBRARY_VECTORS | {30, 36, 42},
}
ICON_REFUSED = frozenset((54, 72, 108, 126, 138, 150, 156, 162, 168, 180, 186, 192, 198))
# Every generated layers.library slot but DeleteLayer, LockLayer, UnlockLayer,
# LockLayerInfo, UnlockLayerInfo, NewLayerInfo, DisposeLayerInfo,
# CreateUpfrontHookLayer and InstallLayerHook,
# and the graphics calls SyncSBitMap and CopySBitMap: they stop the guest
# before any native call (InstallLayerInfoHook, DoHookClipRects,
# ChangeLayerShape, CollectPixelsLayer and the other hook layer constructors
# would otherwise run guest Hooks from native code).
LAYERS_REFUSED = frozenset((30, 36, 42, 48, 54, 60, 66, 72, 78, 84, 108, 114,
                            126, 132, 156, 162, 168, 180, 192,
                            210, 216, 222, 228, 234, 240, 246, 258, 264, 270))
GRAPHICS_GELS_REFUSED = frozenset((96, 102, 108, 114, 126, 132, 138, 144, 150, 156, 162, 168, 174, 600))
GRAPHICS_LAYER_REFUSED = frozenset((444, 450))
CYBERGRAPHICS_BITMAP_REFUSED = frozenset((96, 168, 174, 180, 186))
GRAPHICS_BITMAP_REFUSED = frozenset((606, 636, 678, 918, 924, 942, 1020, 1206))
# GetColorMap, AttachPalExtra, ObtainPen and FindColor: no owned ColorMaps
# or other pen calls yet. ObtainBestPenA and ReleasePen are
# manual handlers checked as native vector cases.
GRAPHICS_PALETTE_REFUSED = frozenset((570, 834, 954, 1008))
GRAPHICS_NODE_REFUSED = frozenset((660, 666, 672, 702))
# Every graphics.conf vector that takes a Region but NewRegion, OrRectRegion
# and DisposeRegion: the generated thunks would read a 68k Region as native.
GRAPHICS_REGION_REFUSED = frozenset((504, 522, 528, 558, 612, 618, 624,
                                     1086, 1092, 1098, 1104, 1110, 1116, 1122,
                                     1128, 1134, 1140, 1146, 1152, 1158))


def module_name(library):
    return library + (".device" if library in DEVICES else ".library")


CASES = (
    ("intuition", "allocremember.c", "AllocRemember", "thunk_intuition_AllocRemember"),
    ("intuition", "freeremember.c", "FreeRemember", "thunk_intuition_FreeRemember"),
    ("graphics", "openmonitor.c", "OpenMonitor", "thunk_gfx_OpenMonitor"),
    ("graphics", "closemonitor.c", "CloseMonitor", "thunk_gfx_CloseMonitor"),
    ("graphics", "getdisplayinfodata.c", "GetDisplayInfoData", "thunk_gfx_GetDisplayInfoData"),
    ("graphics", "finddisplayinfo.c", "FindDisplayInfo", "thunk_gfx_FindDisplayInfo"),
    ("icon", "getdiskobject.c", "GetDiskObject", "kaffeine_icon_GetDiskObject"),
    ("icon", "putdiskobject.c", "PutDiskObject", "kaffeine_icon_PutDiskObject"),
    ("icon", "freediskobject.c", "FreeDiskObject", "kaffeine_icon_FreeDiskObject"),
    ("icon", "findtooltype.c", "FindToolType", "kaffeine_icon_FindToolType"),
    ("icon", "matchtoolvalue.c", "MatchToolValue", "kaffeine_icon_MatchToolValue"),
    ("icon", "getdefdiskobject.c", "GetDefDiskObject", "kaffeine_icon_GetDefDiskObject"),
    ("icon", "getdiskobjectnew.c", "GetDiskObjectNew", "kaffeine_icon_GetDiskObjectNew"),
    ("icon", "newdiskobject.c", "NewDiskObject", "kaffeine_icon_NewDiskObject"),

    ("diskfont", "opendiskfont.c", "OpenDiskFont", "thunk_diskfont_OpenDiskFont"),
    ("console", "rawkeyconvert.c", "RawKeyConvert", "kaffeine_console_RawKeyConvert"),
    ("keymap", "maprawkey.c", "MapRawKey", "Kaffeine_MapRawKey"),
    ("mathffp", "spfix.c", "SPFix", "kaffeine_mathffp_SPFix"),
    ("mathffp", "spflt.c", "SPFlt", "kaffeine_mathffp_SPFlt"),
    ("mathffp", "spcmp.c", "SPCmp", "kaffeine_mathffp_SPCmp"),
    ("mathffp", "sptst.c", "SPTst", "kaffeine_mathffp_SPTst"),
    ("mathffp", "spabs.c", "SPAbs", "kaffeine_mathffp_SPAbs"),
    ("mathffp", "spneg.c", "SPNeg", "kaffeine_mathffp_SPNeg"),
    ("mathffp", "spadd.c", "SPAdd", "kaffeine_mathffp_SPAdd"),
    ("mathffp", "spsub.c", "SPSub", "kaffeine_mathffp_SPSub"),
    ("mathffp", "spmul.c", "SPMul", "kaffeine_mathffp_SPMul"),
    ("mathffp", "spdiv.c", "SPDiv", "kaffeine_mathffp_SPDiv"),
    ("mathffp", "spfloor.c", "SPFloor", "kaffeine_mathffp_SPFloor"),
    ("mathffp", "spceil.c", "SPCeil", "kaffeine_mathffp_SPCeil"),
    ("workbench", "addappwindowa.c", "AddAppWindowA", "wb_AddAppWindowA"),
    ("workbench", "removeappwindow.c", "RemoveAppWindow", "wb_RemoveAppWindow"),
    ("workbench", "addappicona.c", "AddAppIconA", "wb_AddAppIconA"),
    ("workbench", "removeappicon.c", "RemoveAppIcon", "wb_RemoveAppIcon"),
    ("workbench", "addappmenuitema.c", "AddAppMenuItemA", "wb_AddAppMenuItemA"),
    ("workbench", "removeappmenuitem.c", "RemoveAppMenuItem", "wb_RemoveAppMenuItem"),
    ("workbench", "wbinfo.c", "WBInfo", "wb_WBInfo"),
    ("dos", "getprogramname.c", "GetProgramName", "Kaffeine_GetProgramName"),
    ("dos", "info.c", "Info", "kaffeine_dos_Info"),
    ("dos", "createdir.c", "CreateDir", "kaffeine_dos_CreateDir"),
    ("dos", "exit.c", "Exit", "kaffeine_dos_Exit"),
    ("dos", "setvbuf.c", "SetVBuf", "kaffeine_dos_SetVBuf"),
    ("dos", "close.c", "Close", "kaffeine_dos_Close"),
    ("dos", "input.c", "Input", "kaffeine_dos_Input"),
    ("dos", "output.c", "Output", "kaffeine_dos_Output"),
    ("dos", "seek.c", "Seek", "kaffeine_dos_Seek"),
    ("dos", "selectinput.c", "SelectInput", "kaffeine_dos_SelectInput"),
    ("dos", "selectoutput.c", "SelectOutput", "kaffeine_dos_SelectOutput"),
    ("dos", "fgetc.c", "FGetC", "kaffeine_dos_FGetC"),
    ("dos", "fputc.c", "FPutC", "kaffeine_dos_FPutC"),
    ("dos", "ungetc.c", "UnGetC", "kaffeine_dos_UnGetC"),
    ("dos", "flush.c", "Flush", "kaffeine_dos_Flush"),
    ("dos", "waitforchar.c", "WaitForChar", "kaffeine_dos_WaitForChar"),
    ("dos", "isinteractive.c", "IsInteractive", "kaffeine_dos_IsInteractive"),
    ("dos", "duplockfromfh.c", "DupLockFromFH", "kaffeine_dos_DupLockFromFH"),
    ("dos", "openfromlock.c", "OpenFromLock", "kaffeine_dos_OpenFromLock"),
    ("dos", "parentoffh.c", "ParentOfFH", "kaffeine_dos_ParentOfFH"),
    ("dos", "setmode.c", "SetMode", "kaffeine_dos_SetMode"),
    ("dos", "setfilesize.c", "SetFileSize", "kaffeine_dos_SetFileSize"),
    ("dos", "lockrecord.c", "LockRecord", "kaffeine_dos_LockRecord"),
    ("dos", "unlockrecord.c", "UnLockRecord", "kaffeine_dos_UnLockRecord"),
    ("dos", "lockrecords.c", "LockRecords", "kaffeine_dos_LockRecords"),
    ("dos", "unlockrecords.c", "UnLockRecords", "kaffeine_dos_UnLockRecords"),
    ("dos", "changemode.c", "ChangeMode", "kaffeine_dos_ChangeMode"),
    ("dos", "setfiledate.c", "SetFileDate", "kaffeine_dos_SetFileDate"),
    ("dos", "splitname.c", "SplitName", "kaffeine_dos_SplitName"),
    ("dos", "getargstr.c", "GetArgStr", "kaffeine_dos_GetArgStr"),
    ("dos", "setargstr.c", "SetArgStr", "kaffeine_dos_SetArgStr"),
    ("dos", "setcurrentdirname.c", "SetCurrentDirName", "kaffeine_dos_SetCurrentDirName"),
    ("dos", "getcurrentdirname.c", "GetCurrentDirName", "kaffeine_dos_GetCurrentDirName"),
    ("dos", "setprogramname.c", "SetProgramName", "kaffeine_dos_SetProgramName"),
    ("dos", "setprompt.c", "SetPrompt", "kaffeine_dos_SetPrompt"),
    ("dos", "getprompt.c", "GetPrompt", "kaffeine_dos_GetPrompt"),
    ("dos", "assignlock.c", "AssignLock", "kaffeine_dos_AssignLock"),
    ("dos", "assignlate.c", "AssignLate", "kaffeine_dos_AssignLate"),
    ("dos", "assignpath.c", "AssignPath", "kaffeine_dos_AssignPath"),
    ("dos", "assignadd.c", "AssignAdd", "kaffeine_dos_AssignAdd"),
    ("dos", "assignaddtolist.c", "AssignAddToList", "kaffeine_dos_AssignAddToList"),
    ("dos", "remassignlist.c", "RemAssignList", "kaffeine_dos_RemAssignList"),
    ("dos", "isfilesystem.c", "IsFileSystem", "kaffeine_dos_IsFileSystem"),
    ("dos", "addbuffers.c", "AddBuffers", "kaffeine_dos_AddBuffers"),
    ("dos", "comparedates.c", "CompareDates", "kaffeine_dos_CompareDates"),
    ("dos", "datetostr.c", "DateToStr", "kaffeine_dos_DateToStr"),
    ("dos", "strtodate.c", "StrToDate", "kaffeine_dos_StrToDate"),
    ("dos", "findarg.c", "FindArg", "kaffeine_dos_FindArg"),
    ("dos", "readitem.c", "ReadItem", "kaffeine_dos_ReadItem"),
    ("dos", "writechars.c", "WriteChars", "kaffeine_dos_WriteChars"),
    ("dos", "setowner.c", "SetOwner", "kaffeine_dos_SetOwner"),
    ("dos", "strtolong.c", "StrToLong", "kaffeine_dos_StrToLong"),
    ("dos", "datestamp.c", "DateStamp", "kaffeine_dos_DateStamp"),
    ("dos", "filepart.c", "FilePart", "kaffeine_dos_FilePart"),
    ("dos", "pathpart.c", "PathPart", "kaffeine_dos_PathPart"),
    ("dos", "fgets.c", "FGets", "kaffeine_dos_FGets"),
    ("dos", "fread.c", "FRead", "kaffeine_dos_FRead"),
    ("dos", "fwrite.c", "FWrite", "kaffeine_dos_FWrite"),
    ("dos", "namefromlock.c", "NameFromLock", "kaffeine_dos_NameFromLock"),
    ("dos", "namefromfh.c", "NameFromFH", "kaffeine_dos_NameFromFH"),
    ("dos", "addpart.c", "AddPart", "kaffeine_dos_AddPart"),
    ("dos", "setvar.c", "SetVar", "kaffeine_dos_SetVar"),
    ("dos", "getvar.c", "GetVar", "kaffeine_dos_GetVar"),
    ("dos", "deletevar.c", "DeleteVar", "kaffeine_dos_DeleteVar"),
    ("dos", "fault.c", "Fault", "kaffeine_dos_Fault"),
    ("dos", "printfault.c", "PrintFault", "kaffeine_dos_PrintFault"),
    ("dos", "setcomment.c", "SetComment", "kaffeine_dos_SetComment"),
    ("dos", "setprotection.c", "SetProtection", "kaffeine_dos_SetProtection"),
    ("dos", "parsepattern.c", "ParsePattern", "kaffeine_dos_ParsePattern"),
    ("dos", "parsepatternnocase.c", "ParsePatternNoCase", "kaffeine_dos_ParsePatternNoCase"),
    ("dos", "matchpattern.c", "MatchPattern", "kaffeine_dos_MatchPattern"),
    ("dos", "matchpatternnocase.c", "MatchPatternNoCase", "kaffeine_dos_MatchPatternNoCase"),
    ("dos", "examinefh.c", "ExamineFH", "kaffeine_dos_ExamineFH"),
    ("dos", "examine.c", "Examine", "kaffeine_dos_Examine"),
    ("dos", "exnext.c", "ExNext", "kaffeine_dos_ExNext"),
    ("dos", "exall.c", "ExAll", "kaffeine_dos_ExAll"),
    ("dos", "exallend.c", "ExAllEnd", "kaffeine_dos_ExAllEnd"),
    ("dos", "dopkt.c", "DoPkt", "kaffeine_dos_DoPkt"),
    ("dos", "sendpkt.c", "SendPkt", "kaffeine_dos_SendPkt"),
    ("dos", "waitpkt.c", "WaitPkt", "kaffeine_dos_WaitPkt"),
    ("dos", "replypkt.c", "ReplyPkt", "kaffeine_dos_ReplyPkt"),
    ("dos", "abortpkt.c", "AbortPkt", "kaffeine_dos_AbortPkt"),
    ("dos", "deviceproc.c", "DeviceProc", "kaffeine_dos_DeviceProc"),
    ("dos", "getdeviceproc.c", "GetDeviceProc", "kaffeine_dos_GetDeviceProc"),
    ("dos", "freedeviceproc.c", "FreeDeviceProc", "kaffeine_dos_FreeDeviceProc"),
    ("dos", "getconsoletask.c", "GetConsoleTask", "kaffeine_dos_GetConsoleTask"),
    ("dos", "setconsoletask.c", "SetConsoleTask", "kaffeine_dos_SetConsoleTask"),
    ("dos", "getfilesystask.c", "GetFileSysTask", "kaffeine_dos_GetFileSysTask"),
    ("dos", "setfilesystask.c", "SetFileSysTask", "kaffeine_dos_SetFileSysTask"),
    ("dos", "cli.c", "Cli", "kaffeine_dos_Cli"),
    ("dos", "createnewproc.c", "CreateNewProc", "kaffeine_dos_CreateNewProc"),
    ("dos", "createproc.c", "CreateProc", "kaffeine_dos_CreateProc"),
    ("dos", "systemtaglist.c", "SystemTagList", "kaffeine_dos_SystemTagList"),
    ("dos", "open.c", "Open", "kaffeine_dos_Open"),
    ("dos", "read.c", "Read", "kaffeine_dos_Read"),
    ("dos", "write.c", "Write", "kaffeine_dos_Write"),
    ("dos", "deletefile.c", "DeleteFile", "kaffeine_dos_DeleteFile"),
    ("dos", "rename.c", "Rename", "kaffeine_dos_Rename"),
    ("dos", "lock.c", "Lock", "kaffeine_dos_Lock"),
    ("dos", "loadseg.c", "LoadSeg", "kaffeine_dos_LoadSeg"),
    ("dos", "execute.c", "Execute", "kaffeine_dos_Execute"),
    ("dos", "fputs.c", "FPuts", "kaffeine_dos_FPuts"),
    ("dos", "runcommand.c", "RunCommand", "kaffeine_dos_RunCommand"),
    ("dos", "newloadseg.c", "NewLoadSeg", "kaffeine_dos_NewLoadSeg"),
    ("dos", "unloadseg.c", "UnLoadSeg", "kaffeine_dos_UnLoadSeg"),
    ("dos", "addsegment.c", "AddSegment", "kaffeine_dos_SegmentUnsupported"),
    ("dos", "findsegment.c", "FindSegment", "kaffeine_dos_SegmentUnsupported"),
    ("dos", "remsegment.c", "RemSegment", "kaffeine_dos_SegmentUnsupported"),
    ("dos", "getseglistinfo.c", "GetSegListInfo", "kaffeine_dos_SegmentUnsupported"),
    ("dos", "cliinitnewcli.c", "CliInitNewcli", "kaffeine_dos_CliInitNewcli"),
    ("dos", "cliinitrun.c", "CliInitRun", "kaffeine_dos_CliInitRun"),
    ("dos", "putstr.c", "PutStr", "kaffeine_dos_PutStr"),
    ("dos", "vprintf.c", "VPrintf", "kaffeine_dos_VPrintf"),
    ("dos", "matchfirst.c", "MatchFirst", "kaffeine_dos_MatchFirst"),
    ("dos", "matchnext.c", "MatchNext", "kaffeine_dos_MatchNext"),
    ("dos", "matchend.c", "MatchEnd", "kaffeine_dos_MatchEnd"),
    ("exec", "allocmem.c", "AllocMem", "thunk_exec_AllocMem"),
    ("exec", "allocabs.c", "AllocAbs", "thunk_exec_AllocAbs"),
    ("exec", "allocentry.c", "AllocEntry", "thunk_exec_AllocEntry"),
    ("exec", "freeentry.c", "FreeEntry", "thunk_exec_FreeEntry"),
    ("exec", "findresident.c", "FindResident", "Kaffeine_FindResident"),
    ("exec", "addport.c", "AddPort", "Kaffeine_AddPort"),
    ("exec", "remport.c", "RemPort", "Kaffeine_RemPort"),
    ("exec", "putmsg.c", "PutMsg", "Kaffeine_PutMsg"),
    ("exec", "findport.c", "FindPort", "Kaffeine_FindPort"),
    ("exec", "addresource.c", "AddResource", "Kaffeine_AddResource"),
    ("exec", "remresource.c", "RemResource", "Kaffeine_RemResource"),
    ("exec", "openresource.c", "OpenResource", "Kaffeine_OpenResource"),
    ("exec", "cacheclearu.c", "CacheClearU", "Kaffeine_CacheClearU"),
    ("exec", "cachecleare.c", "CacheClearE", "Kaffeine_CacheClearE"),
    ("exec", "freemem.c", "FreeMem", "thunk_exec_FreeMem"),
    ("exec", "availmem.c", "AvailMem", "thunk_exec_AvailMem"),
    ("exec", "allocvec.c", "AllocVec", "thunk_exec_AllocVec"),
    ("exec", "freevec.c", "FreeVec", "thunk_exec_FreeVec"),
    ("exec", "createpool.c", "CreatePool", "thunk_exec_CreatePool"),
    ("exec", "deletepool.c", "DeletePool", "Kaffeine_DeletePool"),
    ("exec", "allocpooled.c", "AllocPooled", "thunk_exec_AllocPooled"),
    ("exec", "freepooled.c", "FreePooled", "thunk_exec_FreePooled"),
    ("exec", "rawdofmt.c", "RawDoFmt", "thunk_exec_RawDoFmt"),
    ("exec", "copymem.c", "CopyMem", "thunk_exec_CopyMem"),
    ("exec", "copymemquick.c", "CopyMemQuick", "thunk_exec_CopyMemQuick"),
    ("exec", "findtask.c", "FindTask", "Kaffeine_FindTask"),
    ("exec", "initsemaphore.c", "InitSemaphore", "Kaffeine_InitSemaphore"),
    ("exec", "obtainsemaphore.c", "ObtainSemaphore", "Kaffeine_ObtainSemaphore"),
    ("exec", "releasesemaphore.c", "ReleaseSemaphore", "Kaffeine_ReleaseSemaphore"),
    ("exec", "attemptsemaphore.c", "AttemptSemaphore", "Kaffeine_AttemptSemaphore"),
    ("exec", "obtainsemaphorelist.c", "ObtainSemaphoreList", "Kaffeine_ObtainSemaphoreList"),
    ("exec", "releasesemaphorelist.c", "ReleaseSemaphoreList", "Kaffeine_ReleaseSemaphoreList"),
    ("exec", "findsemaphore.c", "FindSemaphore", "Kaffeine_FindSemaphore"),
    ("exec", "addsemaphore.c", "AddSemaphore", "Kaffeine_AddSemaphore"),
    ("exec", "remsemaphore.c", "RemSemaphore", "Kaffeine_RemSemaphore"),
    ("exec", "obtainsemaphoreshared.c", "ObtainSemaphoreShared", "Kaffeine_ObtainSemaphoreShared"),
    ("exec", "attemptsemaphoreshared.c", "AttemptSemaphoreShared",
     "Kaffeine_AttemptSemaphoreShared"),
    ("exec", "procure.c", "Procure", "Kaffeine_Procure"),
    ("exec", "vacate.c", "Vacate", "Kaffeine_Vacate"),
    ("exec", "avl.c", "AVL_AddNode", "thunk_exec_AVL_AddNode"),
    ("exec", "avl.c", "AVL_RemNodeByAddress", "thunk_exec_AVL_RemNodeByAddress"),
    ("exec", "avl.c", "AVL_FindNode", "thunk_exec_AVL_FindNode"),
    ("intuition", "addglist.c", "AddGList", "Kaffeine_AddGList"),
    ("intuition", "addgadget.c", "AddGadget", "Kaffeine_AddGadget"),
    ("intuition", "removeglist.c", "RemoveGList", "Kaffeine_RemoveGList"),
    ("intuition", "removegadget.c", "RemoveGadget", "Kaffeine_RemoveGadget"),
    ("intuition", "refreshglist.c", "RefreshGList", "Kaffeine_RefreshGList"),
    ("intuition", "disposeobject.c", "DisposeObject", "Kaffeine_DisposeObject"),
    ("intuition", "activategadget.c", "ActivateGadget", "Kaffeine_ActivateGadget"),
    ("intuition", "setattrsa.c", "SetAttrsA", "Kaffeine_SetAttrsA"),
    ("intuition", "getattr.c", "GetAttr", "Kaffeine_GetAttr"),
    ("intuition", "setgadgetattrsa.c", "SetGadgetAttrsA", "Kaffeine_SetGadgetAttrsA"),
    ("intuition", "dogadgetmethoda.c", "DoGadgetMethodA", "Kaffeine_DoGadgetMethodA"),
    ("intuition", "ongadget.c", "OnGadget", "Kaffeine_OnGadget"),
    ("intuition", "offgadget.c", "OffGadget", "Kaffeine_OffGadget"),
    ("intuition", "refreshgadgets.c", "RefreshGadgets", "Kaffeine_RefreshGadgets"),
    ("intuition", "setwindowpointera.c", "SetWindowPointerA",
     "thunk_intuition_SetWindowPointerA"),
    ("graphics", "openfont.c", "OpenFont", "thunk_gfx_OpenFont"),
    ("graphics", "scrollraster.c", "ScrollRaster", "thunk_gfx_ScrollRaster"),
    ("graphics", "setabpendrmd.c", "SetABPenDrMd", "thunk_gfx_SetABPenDrMd"),
    ("graphics", "closefont.c", "CloseFont", "thunk_gfx_CloseFont"),
    ("graphics", "initgels.c", "InitGels", "thunk_gfx_InitGels"),
    ("graphics", "initrastport.c", "InitRastPort", "thunk_gfx_InitRastPort"),
    # A1 RastPort and A0 TextAttr, both read: no ignored registers.
    ("graphics", "askfont.c", "AskFont", "thunk_gfx_AskFont"),
    ("graphics", "readpixel.c", "ReadPixel", "thunk_gfx_ReadPixel"),
    ("graphics", "getbpen.c", "GetBPen", "thunk_gfx_GetBPen"),
    ("graphics", "textextent.c", "TextExtent", "thunk_gfx_TextExtent"),
    ("graphics", "textfit.c", "TextFit", "thunk_gfx_TextFit"),
    # Guest heap planes: D0/D1 for AllocRaster, A0/D0/D1 for FreeRaster, all read.
    ("graphics", "allocraster.c", "AllocRaster", "thunk_gfx_AllocRaster"),
    ("graphics", "freeraster.c", "FreeRaster", "thunk_gfx_FreeRaster"),
    # A0 template, D0 xSrc, D1 srcMod, A1 destRP, D2/D3 xDest/yDest and
    # D4/D5 xSize/ySize, all read.
    ("graphics", "blttemplate.c", "BltTemplate", "thunk_gfx_BltTemplate"),
    ("exec", "getmsg.c", "GetMsg", "Kaffeine_GetMsg"),
    ("exec", "waitport.c", "WaitPort", "Kaffeine_WaitPort"),
    ("intuition", "modifyidcmp.c", "ModifyIDCMP", "Kaffeine_ModifyIDCMP"),
    ("intuition", "beginrefresh.c", "BeginRefresh", "Kaffeine_BeginRefresh"),
    ("intuition", "endrefresh.c", "EndRefresh", "Kaffeine_EndRefresh"),
    ("gadtools", "gt_beginrefresh.c", "GT_BeginRefresh", "Kaffeine_BeginRefresh"),
    ("gadtools", "gt_endrefresh.c", "GT_EndRefresh", "Kaffeine_EndRefresh"),
    ("gadtools", "gt_getimsg.c", "GT_GetIMsg", "Kaffeine_GT_GetIMsg"),
    ("gadtools", "gt_postfilterimsg.c", "GT_PostFilterIMsg", "Kaffeine_GT_PostFilterIMsg"),
    ("intuition", "cleardmrequest.c", "ClearDMRequest", "Kaffeine_ClearDMRequest"),
    ("intuition", "endrequest.c", "EndRequest", "Kaffeine_EndRequest"),
    ("intuition", "initrequester.c", "InitRequester", "Kaffeine_InitRequester"),
    ("intuition", "request.c", "Request", "Kaffeine_Request"),
    ("intuition", "setdmrequest.c", "SetDMRequest", "Kaffeine_SetDMRequest"),
    ("intuition", "easyrequestargs.c", "EasyRequestArgs", "Kaffeine_EasyRequestArgs"),
    ("intuition", "buildeasyrequestargs.c", "BuildEasyRequestArgs", "Kaffeine_BuildEasyRequestArgs"),
    ("intuition", "sysreqhandler.c", "SysReqHandler", "Kaffeine_SysReqHandler"),
    ("intuition", "freesysrequest.c", "FreeSysRequest", "Kaffeine_FreeSysRequest"),
    ("gadtools", "createmenusa.c", "CreateMenusA", "Kaffeine_CreateMenusA"),
    ("gadtools", "freemenus.c", "FreeMenus", "Kaffeine_FreeMenus"),
    ("gadtools", "layoutmenuitemsa.c", "LayoutMenuItemsA", "Kaffeine_LayoutMenuItemsA"),
    ("gadtools", "layoutmenusa.c", "LayoutMenusA", "Kaffeine_LayoutMenusA"),
    ("gadtools", "getvisualinfoa.c", "GetVisualInfoA", "Kaffeine_GetVisualInfoA"),
    ("gadtools", "freevisualinfo.c", "FreeVisualInfo", "Kaffeine_FreeVisualInfo"),
    ("gadtools", "creategadgeta.c", "CreateGadgetA", "Kaffeine_CreateGadgetA"),
    ("gadtools", "freegadgets.c", "FreeGadgets", "Kaffeine_FreeGadgets"),
    ("gadtools", "gt_setgadgetattrsa.c", "GT_SetGadgetAttrsA", "Kaffeine_GT_SetGadgetAttrsA"),
    ("gadtools", "createcontext.c", "CreateContext", "Kaffeine_CreateGadgetContext"),
    ("gadtools", "gt_getgadgetattrsa.c", "GT_GetGadgetAttrsA", "Kaffeine_GT_GetGadgetAttrsA"),
    ("intuition", "clearmenustrip.c", "ClearMenuStrip", "Kaffeine_ClearMenuStrip"),
    ("intuition", "itemaddress.c", "ItemAddress", "Kaffeine_ItemAddress"),
    ("intuition", "offmenu.c", "OffMenu", "Kaffeine_OffMenu"),
    ("intuition", "onmenu.c", "OnMenu", "Kaffeine_OnMenu"),
    ("intuition", "setmenustrip.c", "SetMenuStrip", "Kaffeine_SetMenuStrip"),
    ("intuition", "resetmenustrip.c", "ResetMenuStrip", "Kaffeine_ResetMenuStrip"),
    ("intuition", "lockpubscreen.c", "LockPubScreen", "Kaffeine_LockPubScreen"),
    ("intuition", "unlockpubscreen.c", "UnlockPubScreen", "Kaffeine_UnlockPubScreen"),
    ("intuition", "lockpubscreenlist.c", "LockPubScreenList", "Kaffeine_LockPubScreenList"),
    ("intuition", "unlockpubscreenlist.c", "UnlockPubScreenList", "Kaffeine_UnlockPubScreenList"),
    ("intuition", "nextpubscreen.c", "NextPubScreen", "Kaffeine_NextPubScreen"),
    ("intuition", "getdefaultpubscreen.c", "GetDefaultPubScreen", "Kaffeine_GetDefaultPubScreen"),
    ("intuition", "openscreen.c", "OpenScreen", "Kaffeine_OpenScreen"),
    ("intuition", "openscreentaglist.c", "OpenScreenTagList", "Kaffeine_OpenScreenTagList"),
    ("intuition", "closescreen.c", "CloseScreen", "Kaffeine_CloseScreen"),
    ("intuition", "getscreendrawinfo.c", "GetScreenDrawInfo", "Kaffeine_GetScreenDrawInfo"),
    ("intuition", "freescreendrawinfo.c", "FreeScreenDrawInfo", "Kaffeine_FreeScreenDrawInfo"),
    ("intuition", "drawimagestate.c", "DrawImageState", "Kaffeine_DrawImageState"),
    ("intuition", "getdrawinfoattr.c", "GetDrawInfoAttr", "Kaffeine_GetDrawInfoAttr"),
    ("exec", "addtask.c", "AddTask", "Kaffeine_AddTask"),
    ("exec", "remtask.c", "RemTask", "Kaffeine_RemTask"),
    ("utility", "callhookpkt.c", "CallHookPkt", "kaffeine_utility_CallHookPkt"),
    ("utility", "vsnprintf.c", "VSNPrintf", "kaffeine_utility_VSNPrintf"),
    ("utility", "findtagitem.c", "FindTagItem", "kaffeine_utility_FindTagItem"),
    ("utility", "gettagdata.c", "GetTagData", "kaffeine_utility_GetTagData"),
    ("utility", "packbooltags.c", "PackBoolTags", "kaffeine_utility_PackBoolTags"),
    ("utility", "nexttagitem.c", "NextTagItem", "kaffeine_utility_NextTagItem"),
    ("utility", "filtertagchanges.c", "FilterTagChanges", "kaffeine_utility_FilterTagChanges"),
    ("utility", "maptags.c", "MapTags", "kaffeine_utility_MapTags"),
    ("utility", "allocatetagitems.c", "AllocateTagItems", "kaffeine_utility_AllocateTagItems"),
    ("utility", "clonetagitems.c", "CloneTagItems", "kaffeine_utility_CloneTagItems"),
    ("utility", "freetagitems.c", "FreeTagItems", "kaffeine_utility_FreeTagItems"),
    ("utility", "refreshtagitemclones.c", "RefreshTagItemClones",
     "kaffeine_utility_RefreshTagItemClones"),
    ("utility", "taginarray.c", "TagInArray", "kaffeine_utility_TagInArray"),
    ("utility", "filtertagitems.c", "FilterTagItems", "kaffeine_utility_FilterTagItems"),
    ("utility", "stricmp.c", "Stricmp", "kaffeine_utility_Stricmp"),
    ("utility", "strnicmp.c", "Strnicmp", "kaffeine_utility_Strnicmp"),
    ("utility", "amiga2date.c", "Amiga2Date", "kaffeine_utility_Amiga2Date"),
    ("utility", "date2amiga.c", "Date2Amiga", "kaffeine_utility_Date2Amiga"),
    ("utility", "checkdate.c", "CheckDate", "kaffeine_utility_CheckDate"),
    ("locale", "closecatalog.c", "CloseCatalog", "kaffeine_locale_CloseCatalog"),
    ("locale", "closelocale.c", "CloseLocale", "kaffeine_locale_CloseLocale"),
    ("locale", "convtolower.c", "ConvToLower", "kaffeine_locale_ConvToLower"),
    ("locale", "convtoupper.c", "ConvToUpper", "kaffeine_locale_ConvToUpper"),
    ("locale", "formatdate.c", "FormatDate", "kaffeine_locale_FormatDate"),
    ("locale", "formatstring.c", "FormatString", "kaffeine_locale_FormatString"),
    ("locale", "parsedate.c", "ParseDate", "kaffeine_locale_ParseDate"),
    ("locale", "getcatalogstr.c", "GetCatalogStr", "kaffeine_locale_GetCatalogStr"),
    ("locale", "getlocalestr.c", "GetLocaleStr", "kaffeine_locale_GetLocaleStr"),
    ("locale", "isxxxx.c", "IsAlNum", "kaffeine_locale_IsAlNum"),
    ("locale", "isxxxx.c", "IsAlpha", "kaffeine_locale_IsAlpha"),
    ("locale", "isxxxx.c", "IsCntrl", "kaffeine_locale_IsCntrl"),
    ("locale", "isxxxx.c", "IsDigit", "kaffeine_locale_IsDigit"),
    ("locale", "isxxxx.c", "IsGraph", "kaffeine_locale_IsGraph"),
    ("locale", "isxxxx.c", "IsLower", "kaffeine_locale_IsLower"),
    ("locale", "isxxxx.c", "IsPrint", "kaffeine_locale_IsPrint"),
    ("locale", "isxxxx.c", "IsPunct", "kaffeine_locale_IsPunct"),
    ("locale", "isxxxx.c", "IsSpace", "kaffeine_locale_IsSpace"),
    ("locale", "isxxxx.c", "IsUpper", "kaffeine_locale_IsUpper"),
    ("locale", "isxxxx.c", "IsXDigit", "kaffeine_locale_IsXDigit"),
    ("locale", "opencataloga.c", "OpenCatalogA", "kaffeine_locale_OpenCatalogA"),
    ("locale", "openlocale.c", "OpenLocale", "kaffeine_locale_OpenLocale"),
    ("locale", "strconvert.c", "StrConvert", "kaffeine_locale_StrConvert"),
    ("locale", "strncmp.c", "StrnCmp", "kaffeine_locale_StrnCmp"),
    ("mathieeedoubbas", "ieeedpfix.c", "IEEEDPFix",
     "kaffeine_mathieeedoubbas_IEEEDPFix"),
    ("mathieeedoubbas", "ieeedpcmp.c", "IEEEDPCmp",
     "kaffeine_mathieeedoubbas_IEEEDPCmp"),
    ("mathieeedoubbas", "ieeedpceil.c", "IEEEDPCeil",
     "kaffeine_mathieeedoubbas_IEEEDPCeil"),
    ("mathieeedoubtrans", "ieeedpsincos.c", "IEEEDPSincos",
     "kaffeine_mathieeedoubtrans_IEEEDPSincos"),
    ("mathieeedoubtrans", "ieeedppow.c", "IEEEDPPow",
     "kaffeine_mathieeedoubtrans_IEEEDPPow"),
    ("mathieeedoubtrans", "ieeedplog10.c", "IEEEDPLog10",
     "kaffeine_mathieeedoubtrans_IEEEDPLog10"),
    ("mathieeesingbas", "ieeespfix.c", "IEEESPFix",
     "kaffeine_mathieeesingbas_IEEESPFix"),
    ("mathieeesingbas", "ieeespcmp.c", "IEEESPCmp",
     "kaffeine_mathieeesingbas_IEEESPCmp"),
    ("mathieeesingbas", "ieeespceil.c", "IEEESPCeil",
     "kaffeine_mathieeesingbas_IEEESPCeil"),
    ("exec", "opendevice.c", "OpenDevice", "Kaffeine_OpenDevice"),
    ("exec", "doio.c", "DoIO", "Kaffeine_DoIO"),
    ("exec", "sendio.c", "SendIO", "Kaffeine_SendIO"),
    ("exec", "checkio.c", "CheckIO", "Kaffeine_CheckIO"),
    ("exec", "waitio.c", "WaitIO", "Kaffeine_WaitIO"),
    ("exec", "abortio.c", "AbortIO", "Kaffeine_AbortIO"),
    ("exec", "createmsgport.c", "CreateMsgPort", "Kaffeine_CreateMsgPort"),
    ("timer", "beginio.c", "BeginIO", "kaffeine_timer_BeginIO"),
    ("timer", "abortio.c", "AbortIO", "kaffeine_timer_AbortIO"),
    ("timer", "getsystime.c", "GetSysTime", "kaffeine_timer_GetSysTime"),
    ("intuition", "drawimage.c", "DrawImage", "thunk_intuition_DrawImage"),
    ("intuition", "drawborder.c", "DrawBorder", "thunk_intuition_DrawBorder"),
    ("intuition", "windowtoback.c", "WindowToBack", "Kaffeine_WindowToBack"),
    ("intuition", "windowtofront.c", "WindowToFront", "Kaffeine_WindowToFront"),
    ("layers", "installlayerhook.c", "InstallLayerHook", "Kaffeine_InstallLayerHook"),
    ("layers", "locklayerinfo.c", "LockLayerInfo", "Kaffeine_LockLayerInfo"),
    ("layers", "unlocklayerinfo.c", "UnlockLayerInfo", "Kaffeine_UnlockLayerInfo"),
    ("layers", "newlayerinfo.c", "NewLayerInfo", "Kaffeine_NewLayerInfo"),
    ("layers", "disposelayerinfo.c", "DisposeLayerInfo", "Kaffeine_DisposeLayerInfo"),
    ("layers", "createupfronthooklayer.c", "CreateUpfrontHookLayer",
     "Kaffeine_CreateUpfrontHookLayer"),
    ("layers", "deletelayer.c", "DeleteLayer", "Kaffeine_DeleteLayer"),
    ("layers", "installclipregion.c", "InstallClipRegion", "Kaffeine_InstallClipRegion"),
    ("graphics", "newregion.c", "NewRegion", "Kaffeine_NewRegion"),
    ("graphics", "orrectregion.c", "OrRectRegion", "Kaffeine_OrRectRegion"),
    ("graphics", "disposeregion.c", "DisposeRegion", "Kaffeine_DisposeRegion"),

    # Reads every fd register, A2 only to refuse a TempA: no ignored ones.
    ("graphics", "bltbitmap.c", "BltBitMap", "Kaffeine_BltBitMap"),
    ("graphics", "getbitmapattr.c", "GetBitMapAttr", "Kaffeine_GetBitMapAttr"),
    # A0, D0, D1 and D2, narrowed as the BYTE/UWORD prototype; Planes untouched.
    ("graphics", "initbitmap.c", "InitBitMap", "Kaffeine_InitBitMap"),
    # Screen-owned pen leases: every fd register read, A1 by the tag walk.
    ("graphics", "obtainbestpena.c", "ObtainBestPenA", "Kaffeine_ObtainBestPenA"),
    ("graphics", "releasepen.c", "ReleasePen", "Kaffeine_ReleasePen"),
    ("graphics", "videocontrol.c", "VideoControl", "Kaffeine_VideoControl"),
)

# Explicit table of manual handlers that intentionally ignore one or more
# fd arguments, with the exact ignored register set and the reason.
ALLOWED_IGNORED_REGS = {
    "thunk_diskfont_NewFontContents": (frozenset({"A0", "A1"}), "native FontContents are unsupported"),
    "thunk_gfx_OpenMonitor": (frozenset({"A1", "D0"}), "owned native monitors are unsupported"),
    "thunk_gfx_CloseMonitor": (frozenset({"A0"}), "borrowed snapshots cannot close native monitors"),
    "thunk_gfx_FindDisplayInfo": (
        frozenset({"D0"}),
        "FindDisplayInfo() stops20 until native display handles have guest identities",
    ),
    "kaffeine_dos_LockRecords": (
        frozenset({"D1", "D2"}),
        "LockRecords() stops with rc20; native-width record arrays are unsupported",
    ),
    "kaffeine_dos_UnLockRecords": (
        frozenset({"D1"}),
        "UnLockRecords() stops with rc20; native-width record arrays are unsupported",
    ),
    "Kaffeine_GetVisualInfoA": (
        frozenset({"A1"}),
        "GetVisualInfoA() takes no tags; GadTools of AROS does not read them either",
    ),
    "Kaffeine_DrawImageState": (
        frozenset({"A0", "A1", "A2", "D0", "D1", "D2"}),
        "DrawImageState() stops with rc 20 until Image/DrawInfo inputs have typed conversions",
    ),
    "Kaffeine_GetDrawInfoAttr": (
        frozenset({"A0", "D0", "A1"}),
        "GetDrawInfoAttr() stops with rc 20 until its attributes and result storage have typed conversions",
    ),
    "thunk_exec_TypeOfMem": (
        frozenset({"A1"}),
        "TypeOfMem() returns MEMF_PUBLIC for all containment memory",
    ),
    "Kaffeine_Procure": (
        frozenset({"A0", "A1"}),
        "Procure() stops with rc 20 until guest semaphore messages have a bridge",
    ),
    "Kaffeine_Vacate": (
        frozenset({"A0", "A1"}),
        "Vacate() stops with rc 20, paired with refused Procure()",
    ),
    "Kaffeine_AddResource": (
        frozenset({"A1"}),
        "AddResource() is a no-op; native resources cannot use guest structures",
    ),
    "Kaffeine_RemResource": (
        frozenset({"A1"}),
        "RemResource() is a no-op, paired with refused guest resource publication",
    ),
    "thunk_exec_AVL_AddNode": (
        frozenset({"A0", "A1", "A2"}),
        "AVL_AddNode() is a no-op stub; the comparison callback is 68k code",
    ),
    "thunk_exec_AVL_RemNodeByAddress": (
        frozenset({"A0", "A1"}),
        "AVL_RemNodeByAddress() is a no-op stub",
    ),
    "thunk_exec_AVL_FindNode": (
        frozenset({"A0", "A1", "A2"}),
        "AVL_FindNode() is a no-op stub; the comparison callback is 68k code",
    ),
    "kaffeine_dos_CliInitNewcli": (
        frozenset({"A0"}),
        "CliInitNewcli() refuses a 68k DosPacket with ERROR_NOT_IMPLEMENTED",
    ),
    "kaffeine_dos_CliInitRun": (
        frozenset({"A0"}),
        "CliInitRun() refuses a 68k DosPacket with ERROR_NOT_IMPLEMENTED",
    ),
    "kaffeine_dos_ExAllEnd": (
        frozenset({"D2", "D3", "D4"}),
        "ExAllEnd validates the lock and resets the bridge-owned control state",
    ),
    "kaffeine_dos_AbortPkt": (
        frozenset({"D1", "D2"}),
        "AbortPkt() does nothing, as on AmigaOS V37 to V47",
    ),
    "Kaffeine_RefreshGList": (
        frozenset({"A2"}),
        "RefreshGList() refreshes GadTools gadgets on their window; requesters hold none",
    ),
    "Kaffeine_RefreshGadgets": (
        frozenset({"A2"}),
        "RefreshGadgets() refreshes GadTools gadgets on their window; requesters hold none",
    ),
    "Kaffeine_ModifyProp": (
        frozenset({"A2"}),
        "ModifyProp() passes no requester: the bridge puts no gadget of the program on one",
    ),
    "Kaffeine_NewModifyProp": (
        frozenset({"A2"}),
        "NewModifyProp() passes no requester: the bridge puts no gadget of the program on one",
    ),
    "Kaffeine_GT_SetGadgetAttrsA": (
        frozenset({"A2"}),
        "GT_SetGadgetAttrsA() does not pass the requester, which the autodoc wants NULL",
    ),
    "Kaffeine_GT_GetGadgetAttrsA": (
        frozenset({"A1", "A2"}),
        "GT_GetGadgetAttrsA() reads neither the window nor the requester, as GadTools",
    ),
    "thunk_intuition_SetWindowPointerA": (
        frozenset({"A0", "A1"}),
        "SetWindowPointerA() is a no-op stub",
    ),
    "kaffeine_locale_CloseCatalog": (
        frozenset({"A0"}),
        "CloseCatalog() is a no-op paired with NULL-returning OpenCatalogA()",
    ),
    "kaffeine_locale_GetCatalogStr": (
        frozenset({"A0", "D0"}),
        "GetCatalogStr() returns defaultString in A1 unchanged until catalogs are bridged",
    ),
    "kaffeine_locale_OpenCatalogA": (
        frozenset({"A0", "A1", "A2"}),
        "OpenCatalogA() returns NULL so 68k programs use their built-in strings",
    ),
    "Kaffeine_LockLayer": (
        frozenset({"A0"}),
        "LockLayer() ignores its dummy argument, as layers.library does",
    ),
    "Kaffeine_DeleteLayer": (
        frozenset({"A0"}),
        "DeleteLayer() ignores its dummy argument, as layers.library does",
    ),
}

HANDLER_PREFIX_RE = re.compile(
    r"^(?:wb_|thunk_(?:exec|dos|intuition|gfx|graphics|diskfont|utility|locale|asl)_|"
    r"kaffeine_(?:exec|dos|intuition|gfx|graphics|utility|locale|icon|"
    r"mathieeedoubbas|mathieeedoubtrans|mathieeesingbas|mathffp|timer|console)_|"
    r"Kaffeine_(?:List|MemHeader)?)"
)


def extract_table(source, symbol):
    pattern = (r"(?:const|static)\s+.*?\bstruct M68KThunkEntry\s+" +
               re.escape(symbol) + r"\[\]\s*=\s*\{(.*?)^\};")
    match = re.search(pattern, source, re.MULTILINE | re.DOTALL)
    if not match:
        raise ValueError("missing production table: " + symbol)
    return match.group(1)


def extract_count_expression(source, symbol):
    match = re.search(r"const\s+ULONG\s+" + re.escape(symbol) +
                      r"_count\s*=\s*(.*?);", source, re.DOTALL)
    if not match:
        raise ValueError("missing production table count: " + symbol)
    return match.group(1)


def parse_fd_file(path):
    entries = {}
    bias = 30
    for raw_line in path.read_text(encoding="latin-1").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("*"):
            continue
        if line.startswith("##bias"):
            bias = int(line.split()[1])
            continue
        if line.startswith("##"):
            continue
        match = re.match(r"^(\w+)\(([^)]*)\)\(([^)]*)\)", line)
        if match:
            func_name = match.group(1)
            reg_str = match.group(3).strip()
            regs = tuple(
                r.strip().upper()
                for r in re.split(r"[/,]", reg_str)
                if r.strip()
            ) if reg_str else ()
            entries[bias] = (func_name, regs)
        bias += 6
    return entries


def parse_p96_inline(path):
    """Offset -> (name, registers) of the VBCC inline header of the P96 SDK;
    the ...Tags forms share the offset of their ...TagList function."""
    entries = {}
    text = path.read_text(encoding="latin-1")
    for match in re.finditer(
            r'__(p96\w+)\((.*?)\)\s*=\s*"\\tjsr\\t-(\d+)\(a6\)"', text):
        name, args, offset = match.group(1), match.group(2), int(match.group(3))
        regs = tuple(reg.upper() for reg in re.findall(r'__reg\("(\w+)"\)', args)
                     if reg.lower() != "a6")
        if name.endswith("Tags") and offset in entries:
            continue
        entries.setdefault(offset, (name, regs))
    return entries


def parse_conf_file(path):
    entries = {}
    in_config = False
    in_funclist = False
    noresident = False
    device = False
    slot = 5
    for raw_line in path.read_text(encoding="latin-1").splitlines():
        line = raw_line.strip()
        if line == "##begin config":
            in_config = True
            continue
        if line == "##end config":
            in_config = False
            continue
        # As tools/genmodule/config.c: only "noresident" starts the function
        # list at LVO 1, where exec and dos list their own open and close.
        # mathieeesingbas has "noautolib" without it and starts at 5.
        if in_config and line.startswith("options") and "noresident" in line:
            noresident = True
            continue
        # A device has BeginIO at -30 and AbortIO at -36, both with the
        # request in A1; its function list starts at -42.
        if in_config and line.split()[:1] == ["beginio_func"]:
            device = True
            entries[30] = (line.split()[1], ("A1",))
            continue
        if in_config and line.split()[:1] == ["abortio_func"]:
            entries[36] = (line.split()[1], ("A1",))
            continue
        if line == "##begin functionlist":
            in_funclist = True
            slot = 7 if device else 1 if noresident else 5
            continue
        if line == "##end functionlist":
            break
        if not in_funclist or line.startswith("#"):
            continue
        # genmodule's readsectionfunctionlist() reserves one LVO for an empty
        # line, including locale.library's private slot before CloseCatalog.
        if not line:
            slot += 1
            continue
        if line.startswith(".skip"):
            parts = line.split()
            slot += int(parts[1]) if len(parts) > 1 else 1
            continue
        if line.startswith("."):
            continue
        match = re.match(r"^.*?\b(\w+)\s*\(([^)]*)\)\s*\(([^)]*)\)", line)
        if match:
            func_name = match.group(1)
            reg_str = match.group(3).strip()
            regs = tuple(
                r.strip().upper()
                for r in re.split(r"[/,]", reg_str)
                if r.strip()
            ) if reg_str else ()
            if slot >= 5:
                entries[slot * 6] = (func_name, regs)
            slot += 1
    return entries


def library_source_dir(root, library):
    if library == "cybergraphics":
        return root / "workbench" / "libs" / "cgfx"
    # layers.library of AROS is built from rom/hyperlayers
    if library == "layers":
        return root / "rom/hyperlayers"
    rom_dir = root / "rom" / library
    if rom_dir.is_dir():
        return rom_dir
    device_dir = root / "rom/devs" / library
    if library in DEVICES and device_dir.is_dir():
        return device_dir
    return root / "workbench/libs" / library


def load_library_specs(root, library, ndk_fd_dir, aros_fd_dir):
    if library == "Picasso96API":
        sdk = parse_p96_inline(root / P96_SDK_INLINE)
        return sdk, sdk
    conf_entries = parse_conf_file(
        library_source_dir(root, library) / (library + ".conf"))
    ndk_fd = ndk_fd_dir / (library + "_lib.fd") if ndk_fd_dir else None
    aros_fd = aros_fd_dir / (library + "_lib.fd") if aros_fd_dir else None

    ndk_entries = parse_fd_file(ndk_fd) if ndk_fd and ndk_fd.is_file() else {}
    aros_entries = (parse_fd_file(aros_fd)
                    if aros_fd and aros_fd.is_file() else conf_entries)
    for (source_library, vector), function in AROS_ONLY_FUNCTIONS.items():
        if source_library == library:
            entry = conf_entries.get(vector)
            if entry and entry[0] == function:
                aros_entries[vector] = entry
    # The fd file of a device lists only the functions after BeginIO and
    # AbortIO, which the .conf file names.
    if ndk_entries:
        for vector in (30, 36):
            if vector in conf_entries:
                ndk_entries.setdefault(vector, conf_entries[vector])
    if library == "workbench" and not ndk_entries:
        print("NOTE: Workbench uses the embedded NDK 3.2 LVO/register oracle; "
              "AROS AddAppMenuItemA(A3 tags) differs from 68k A2")
        return WORKBENCH_NDK32, aros_entries
    if library == "diskfont" and not ndk_entries:
        return DISKFONT_NDK32, aros_entries
    return ndk_entries or conf_entries, aros_entries


def strip_c_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def parse_c_functions(core):
    functions = {}
    sig_re = re.compile(
        r"(?:static\s+)?(?:IPTR|BOOL|ULONG|void|const\s+char\s*\*|"
        r"struct\s+\w+\s*\*)\s+(\w+)\s*\([^;{]*\)\s*\{",
        re.MULTILINE,
    )
    for cfile in sorted(core.glob("kaffeine_*.c")):
        if cfile.name == "kaffeine_thunks_gen.c":
            continue
        text = strip_c_comments(cfile.read_text(encoding="latin-1"))
        for match in sig_re.finditer(text):
            name = match.group(1)
            start = match.end() - 1
            depth = 0
            idx = start
            while idx < len(text):
                if text[idx] == "{":
                    depth += 1
                elif text[idx] == "}":
                    depth -= 1
                    if depth == 0:
                        functions[name] = text[start:idx + 1]
                        break
                idx += 1
    return functions


# These void functions return the caller's D0 to the dispatcher.
# It is a preserved register, not an additional FD argument. Runtime tests
# check that it and every other non-result register survive successful calls.
PRESERVED_D0_HANDLERS = {
    "thunk_exec_FreeEntry", "Kaffeine_AddPort", "Kaffeine_RemPort", "Kaffeine_PutMsg",
    "Kaffeine_AddResource", "Kaffeine_RemResource", "Kaffeine_CacheClearU",
    "Kaffeine_InitSemaphore", "Kaffeine_ObtainSemaphore", "Kaffeine_ReleaseSemaphore",
    "Kaffeine_ObtainSemaphoreList", "Kaffeine_ReleaseSemaphoreList",
    "Kaffeine_AddSemaphore", "Kaffeine_RemSemaphore", "Kaffeine_ObtainSemaphoreShared",
    "kaffeine_utility_FreeTagItems", "kaffeine_utility_RefreshTagItemClones",
    "kaffeine_locale_CloseCatalog", "kaffeine_locale_CloseLocale",
    "kaffeine_locale_FormatDate",
}


def direct_register_reads(body):
    regs = set()
    for match in re.finditer(r"\bTHUNK_(?:D|DPTR)\s*\(\s*(\d+)\s*\)", body):
        regs.add("D" + match.group(1))
    for match in re.finditer(r"\bTHUNK_(?:A|PTR)\s*\(\s*(\d+)\s*\)", body):
        regs.add("A" + match.group(1))
    for match in re.finditer(r"\bKaffeine_GetD\s*\([^,]+,\s*(\d+)\s*\)", body):
        regs.add("D" + match.group(1))
    for match in re.finditer(r"\bKaffeine_GetA\s*\([^,]+,\s*(\d+)\s*\)", body):
        regs.add("A" + match.group(1))
    return regs


def parse_task_delegates(core, functions):
    """Follow the actual production vtable, rather than waive ABI arguments."""
    header = strip_c_comments((core / "kaffeine_sched.h").read_text())
    source = strip_c_comments((core / "kaffeine_sched.c").read_text())
    structure = re.search(r"struct KaffeineTaskOps\s*\{(.*?)\n\};", header, re.S)
    initializer = re.search(r"\bKaffeine_GuestTaskOps\s*=\s*\{(.*?)\n\};", source, re.S)
    if not structure or not initializer:
        raise ValueError("missing guest task operation declarations")
    members = re.findall(r"\(\s*\*\s*(\w+)\s*\)", structure.group(1))
    targets = [x.strip() for x in initializer.group(1).split(",") if x.strip()]
    if len(members) != len(targets) or len(set(members)) != len(members):
        raise ValueError("guest task operation table does not match its structure")
    if any(target not in functions for target in targets):
        raise ValueError("guest task operation has no auditable production body")
    return dict(zip(members, targets))


def matching_c_delimiter(text, start, opening, closing):
    depth, quote, escaped = 0, None, False
    for position in range(start, len(text)):
        char = text[position]
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
            continue
        if char in ("'", '"'):
            quote = char
        elif char == opening:
            depth += 1
        elif char == closing:
            depth -= 1
            if not depth:
                return position
    raise ValueError("unbalanced C expression in register audit")


def split_c_expression(text, separator):
    parts, start, position = [], 0, 0
    while position < len(text):
        char = text[position]
        if char == "(":
            position = matching_c_delimiter(text, position, "(", ")") + 1
        elif char in ("'", '"'):
            quote = char
            position += 1
            while position < len(text):
                if text[position] == "\\":
                    position += 2
                elif text[position] == quote:
                    position += 1
                    break
                else:
                    position += 1
        elif text.startswith(separator, position):
            parts.append(text[start:position].strip())
            position += len(separator)
            start = position
        else:
            position += 1
    return parts + [text[start:].strip()]


def strip_outer_parentheses(text):
    text = text.strip()
    while text.startswith("(") and matching_c_delimiter(text, 0, "(", ")") == len(text) - 1:
        text = text[1:-1].strip()
    return text


def process_lvo_constants(core, task_ops):
    source = strip_c_comments((core / "kaffeine_sched.c").read_text())
    target = task_ops.get("process_dos", "")
    # Specialize only the actual third formal parameter, not a local or a
    # guessed argument of an unrelated callback with the same member name.
    if not re.search(r"\b" + re.escape(target) +
                     r"\s*\([^,()]+,[^,()]+,\s*ULONG\s+lvo\s*,", source):
        return {}
    for enum in re.finditer(r"\benum(?:\s+\w+)?\s*\{([^{}]*)\}", source):
        match = re.search(r"\bDOS_EXIT_LVO\s*=\s*(0[xX][0-9a-fA-F]+|[0-9]+)\s*(?:,|$)", enum.group(1))
        if match:
            return {"DOS_EXIT_LVO": int(match.group(1), 0)}
    return {}


def zero_lvo_body(body, constants):
    # A modified/escaped parameter cannot retain its callsite value.
    for symbol in ("lvo", *constants):
        identifier = re.escape(symbol)
        if re.search(r"\b" + identifier + r"\s*(?:=(?!=)|[+*/%&|^-]=|\+\+|--)|"
                     r"(?:\+\+|--)\s*" + identifier + r"\b|(?<!&)&(?!&)\s*" + identifier + r"\b", body):
            return body
        if re.search(r"\b\w+\s+" + identifier + r"\b|\benum\b[^;{}]*\{[^{}]*\b" + identifier + r"\b", body):
            return body  # conservatively reject shadowed parameters/constants
    cursor = 0
    while (match := re.search(r"\bif\s*\(", body[cursor:])):
        start = cursor + match.start()
        opening = cursor + match.end() - 1
        closing = matching_c_delimiter(body, opening, "(", ")")
        condition = strip_outer_parentheses(body[opening + 1:closing])
        terms = split_c_expression(condition, "&&")
        guard = None
        for i, term in enumerate(terms):
            equality = re.fullmatch(r"lvo\s*==\s*(\w+)|(\w+)\s*==\s*lvo",
                                    strip_outer_parentheses(term))
            if equality and constants.get(equality.group(1) or equality.group(2), 0) != 0:
                guard = i
                break
        block = closing + 1
        while block < len(body) and body[block].isspace():
            block += 1
        cursor = closing + 1
        if guard is None or block >= len(body) or body[block] != "{":
            continue
        end = matching_c_delimiter(body, block, "{", "}") + 1
        # Leave unfamiliar control flow conservative, including else paths.
        if re.match(r"\s*else\b", body[end:]):
            continue
        # In C, preceding && operands may already have read registers. Keep
        # those operands; only the false comparison's suffix/body cannot run.
        prefix = "(" + " && ".join(terms[:guard]) + ");" if guard else ""
        body = body[:start] + prefix + body[end:]
        cursor = start + len(prefix)
    return body


def handler_register_reads(name, functions, seen=None, task_ops=None,
                           lvo_constants=None, zero_lvo=False):
    if seen is None:
        seen = set()
    state = (name, zero_lvo)
    if state in seen or name not in functions:
        return set()
    seen.add(state)
    body = functions[name]
    if zero_lvo:
        body = zero_lvo_body(body, lvo_constants or {})
    regs = direct_register_reads(body)
    for callee in re.findall(r"\b([A-Za-z_]\w*)\s*\(", body):
        # These CPU operations save/restore architectural state or propagate
        # a stopped process result. Their registers are not FD arguments.
        if callee in {"Kaffeine_CallGuest", "Kaffeine_StopGuest"}:
            continue
        if callee in functions and callee != name:
            regs |= handler_register_reads(callee, functions, seen, task_ops, lvo_constants)
    for call in re.finditer(r"\bctx->task_ops->(\w+)\s*\(", body):
        member = call.group(1)
        if not task_ops or member not in task_ops:
            raise ValueError("unresolved guest task operation: " + member)
        opening = call.end() - 1
        closing = matching_c_delimiter(body, opening, "(", ")")
        args = split_c_expression(body[opening + 1:closing], ",")
        # PutMsg's DOS-packet guard explicitly passes LVO zero. It cannot
        # read Exit's D1 argument, but nonconstant/Exit calls still can.
        is_zero = (member == "process_dos" and len(args) == 4 and
                   re.fullmatch(r"0[UuLl]*", args[2]) is not None)
        regs |= handler_register_reads(task_ops[member], functions, seen, task_ops,
                                       lvo_constants, is_zero)
    return regs


def check_zero_lvo_register_audit():
    functions = {
        "put": "{ return route(ctx, THUNK_A(0), THUNK_A(1), THUNK_D(0)); }",
        "route": "{ return ctx->task_ops->process_dos(ctx, cpu, 0, &out); }",
        "process": "{ if (task == root && lvo == DOS_EXIT_LVO && Kaffeine_GetD(cpu, 1)) "
                   "{ return Kaffeine_GetD(cpu, 1); } return FALSE; }",
    }
    delegates, constants = {"process_dos": "process"}, {"DOS_EXIT_LVO": 144}
    def reads(changes=None, symbols=constants):
        return handler_register_reads("put", dict(functions, **(changes or {})),
                                      task_ops=delegates, lvo_constants=symbols)
    controls = [
        (reads() == {"A0", "A1", "D0"}, "constant-zero guard"),
        ("D1" in reads({"put": functions["put"].replace("THUNK_A(0)", "THUNK_D(1)")}),
         "direct wrong PutMsg argument"),
        ("D1" in reads({"route": functions["route"].replace(", 0,", ", selected_lvo,")}),
         "nonconstant DOS delegate"),
        ("D1" in reads({"route": functions["route"].replace(", 0,", ", DOS_EXIT_LVO,")}),
         "Exit DOS delegate"),
        ("D1" in reads({"process": "{ Kaffeine_GetD(cpu, 1);" + functions["process"][1:]}),
         "unconditional delegate read"),
        ("D1" in reads({"process": "{ if (Kaffeine_GetD(cpu, 1) && lvo == DOS_EXIT_LVO) { return TRUE; } }"}),
         "read before short-circuit guard"),
        ("D1" in reads({"process": "{ if (lvo == DOS_EXIT_LVO || Kaffeine_GetD(cpu, 1)) { return TRUE; } }"}),
         "non-dominating OR guard"),
        ("D1" in reads({"route": "{ ctx->task_ops->process_dos(ctx, cpu, 0, &out); "
                                 "return ctx->task_ops->process_dos(ctx, cpu, selected_lvo, &out); }"}),
         "mixed callsite memoization"),
        ("D1" in reads({"process": "{ lvo = DOS_EXIT_LVO;" + functions["process"][1:]}),
         "modified parameter"),
        ("D1" in reads({"process": "{ mutate(&lvo);" + functions["process"][1:]}),
         "escaped parameter"),
        ("D1" in reads({"process": "{ ULONG DOS_EXIT_LVO = 0;" + functions["process"][1:]}),
         "shadowed guard constant"),
        ("D1" in reads({"process": "{ { ULONG lvo;" + functions["process"][1:] + "}"}),
         "shadowed parameter"),
        ("D1" in reads({"process": "{ enum { DOS_EXIT_LVO };" + functions["process"][1:]}),
         "shadowed enum constant"),
        ("D1" in reads(symbols={}), "unproved guard constant"),
    ]
    for passed, label in controls:
        if not passed:
            raise ValueError("constant-LVO register audit failed: " + label)
    return len(controls)


def p96_audit(source, entries, sdk):
    """The failures of the Picasso96API.library table: its vectors must be
    exactly those of the SDK, once each, -60 and -102 their own thunks,
    every other one P96_UNSUPPORTED() of its SDK function, defined once,
    and the macro exactly its two statements."""
    failures = []
    vectors = [vector for vector, _ in entries]
    handlers = dict(entries)
    if len(vectors) != len(set(vectors)) or set(vectors) != set(sdk):
        failures.append("vectors " + str(sorted(vectors)) + " are not the " +
                        str(len(sdk)) + " of the SDK")
    for vector, handler in sorted(P96_SUPPORTED.items()):
        if handlers.get(vector) != handler:
            failures.append(str(-vector) + " is not " + handler)
    for vector in sorted(set(sdk) - set(P96_SUPPORTED)):
        handler = handlers.get(vector, "")
        stem = handler[len("thunk_p96_"):] if handler.startswith("thunk_p96_") else ""
        if (not stem or "p96" + stem != sdk[vector][0] or
                len(re.findall(r"^P96_UNSUPPORTED\(" + re.escape(stem) + r"\)$",
                               source, re.MULTILINE)) != 1):
            failures.append(str(-vector) + " is not P96_UNSUPPORTED(" +
                            sdk[vector][0][len("p96"):] + ")")
    macro = re.findall(r"^#define P96_UNSUPPORTED\(name\)((?:[^\n]*\\\n)*[^\n]*)$",
                       source, re.MULTILINE)
    if (len(macro) != 1 or
            " ".join(macro[0].replace("\\\n", " ").split()) != P96_UNSUPPORTED_BODY):
        failures.append("P96_UNSUPPORTED() is not exactly (void)cpu and the refusal")
    return failures


def check_p96_audit(source, entries, sdk):
    """In-memory controls: the audit passes the production table and source
    and fails each of their mutants."""
    handlers = dict(entries)

    def without(*gone):
        return [(v, h) for v, h in entries if v not in gone]

    swapped = [(v, handlers[36] if v == 30 else handlers[30] if v == 36 else h)
               for v, h in entries]
    statement = "        (void)cpu; \\\n"
    controls = [
        (not p96_audit(source, entries, sdk), "production table"),
        (p96_audit(source, without(60), sdk), "remove-60"),
        (p96_audit(source, without(102), sdk), "remove-102"),
        (p96_audit(source, without(60, 102), sdk), "remove-60-and-102"),
        (p96_audit(source, without(30), sdk), "remove a refusal"),
        (p96_audit(source, entries + [(66, handlers[66])], sdk), "duplicate vector"),
        (p96_audit(source, entries + [(198, handlers[66])], sdk), "vector past the SDK"),
        (p96_audit(source, [(v, handlers[66] if v == 60 else h) for v, h in entries], sdk),
         "-60 refused"),
        (p96_audit(source, swapped, sdk), "two refusals swapped"),
        (p96_audit(source.replace(statement, statement + "        return 0; \\\n"),
                   entries, sdk), "early return before the refusal"),
        (p96_audit(source.replace(statement, statement + "        extra_read(cpu); \\\n"),
                   entries, sdk), "helper read before the refusal"),
        (p96_audit(source.replace(statement, "        (void)THUNK_D(0); \\\n"),
                   entries, sdk), "register read in the macro"),
        (p96_audit(source.replace("return refuse(ctx,", "return 0; refuse(ctx,"),
                   entries, sdk), "refusal not returned"),
        (p96_audit(source.replace("P96_UNSUPPORTED(AllocBitMap)\n",
                                  "P96_UNSUPPORTED(AllocBitMap)\nP96_UNSUPPORTED(AllocBitMap)\n"),
                   entries, sdk), "refusal defined twice"),
        (p96_audit(source.replace("#define P96_UNSUPPORTED(name)", "#define P96_REFUSED(name)"),
                   entries, sdk), "macro missing"),
    ]
    for passed, label in controls:
        if not passed:
            raise ValueError("Picasso96API.library table audit failed: " + label)
    return len(controls)


# Handlers whose name cannot be the one of the function they implement
HANDLER_FUNCTIONS = {
    # Kaffeine_CreateContext() makes the context of the emulator itself
    "Kaffeine_CreateGadgetContext": "CreateContext",
}


def expected_function_name(handler, library):
    # Public GadTools aliases have exactly the same ABI and implementation
    # semantics, while their vector names retain the GT_ prefix.
    aliases = {
        ("gadtools", "Kaffeine_BeginRefresh"): "GT_BeginRefresh",
        ("gadtools", "Kaffeine_EndRefresh"): "GT_EndRefresh",
    }
    if library == "Picasso96API" and handler.startswith("thunk_p96_"):
        return "p96" + handler[len("thunk_p96_"):]
    return aliases.get((library, handler)) or HANDLER_FUNCTIONS.get(handler) or HANDLER_PREFIX_RE.sub("", handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path,
                        default=Path(__file__).resolve().parents[4])
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--ndk-fd-dir", type=Path,
                        default=env_path("NDK_FD_DIR"))
    parser.add_argument("--aros-fd-dir", type=Path,
                        default=env_path("AROS_FD_DIR"))
    args = parser.parse_args()
    root = args.root.resolve()
    core = root / "rom/kaffeine"
    if not (args.ndk_fd_dir and args.ndk_fd_dir.is_dir()):
        print("NOTE: NDK_FD_DIR not set or missing; using repository .conf files as fallback")

    sources = {
        filename: (core / filename).read_text(encoding="latin-1")
        for _, filename, _ in TABLES
    }
    headers = []
    for text in sources.values():
        for name in re.findall(r'^#include "(kaffeine_[^\"]+\.h)"',
                               text, re.MULTILINE):
            if name not in headers:
                headers.append(name)

    snippets = ['#include "' + name + '"' for name in headers]
    for _, filename, symbol in TABLES:
        snippets.append("const struct M68KThunkEntry " + symbol + "[] = {" +
                        extract_table(sources[filename], symbol) + "\n};")

    compiler = shlex.split(args.cc)
    include_flags = [
        "-I" + str(root / "developer/debug/test/kaffeine_host/gfx_stubs"),
        "-I" + str(root / "developer/debug/test/kaffeine_host/stubs"),
        "-I" + str(core),
    ]
    command = compiler + ["-E", "-P", "-x", "c"] + include_flags + ["-"]
    result = subprocess.run(command, input="\n".join(snippets), text=True,
                            capture_output=True)
    if result.returncode:
        sys.stderr.write(result.stderr)
        return result.returncode

    registrations = {}
    for library, _, symbol in TABLES:
        registrations[library] = [
            (int(vector), handler)
            for vector, handler in re.findall(
                r"\{\s*(\d+)\s*,\s*([A-Za-z_]\w*)\s*\}",
                extract_table(result.stdout, symbol))
            if int(vector) != 0
        ]

    # Keep the expanded initializer text intact: rebuilding it from the regex
    # results would silently discard malformed separators such as "},,".
    syntax = ['#include "kaffeine_thunks.h"']
    handlers = {handler for entries in registrations.values()
                for _, handler in entries}
    syntax += ["extern IPTR " + handler +
               "(struct KaffeineContext *, void *);"
               for handler in sorted(handlers)]
    for library, filename, symbol in TABLES:
        syntax.append("const struct M68KThunkEntry " + symbol + "[] = {" +
                      extract_table(result.stdout, symbol) + "\n};")
        # Count active entries: Workbench has no trailing zero sentinel.
        syntax.append(
            "typedef char " + library + "_count_covers_production_table[(" +
            extract_count_expression(sources[filename], symbol) + ") == " +
            str(len(registrations[library])) + " ? 1 : -1];"
        )
    result = subprocess.run(
        compiler + ["-std=c99", "-fsyntax-only", "-Werror", "-x", "c"] +
        include_flags + ["-"],
        input="\n".join(syntax), text=True, capture_output=True)
    if result.returncode:
        sys.stderr.write(result.stderr)
        return result.returncode

    failures = 0
    for library, filename, func_name, expected in CASES:
        native = (library_source_dir(root, library) / filename).read_text(
            encoding="latin-1")
        pattern = (
            r"AROS_LH\w+\s*\([^,]+,\s*" + re.escape(func_name) + r"\s*,.*?"
            r"struct\s+(?:DosLibrary|ExecBase|IntuitionBase|GfxBase|UtilityBase|"
            r"LocaleBase|Library|LibHeader|MathIeeeDoubBasBase|MathIeeeDoubTransBase|"
            r"Device|TimerBase|WorkbenchBase|IconBase|LayersBase)"
            r"\s*\*,\s*\w+\s*,\s*(\d+)\s*,\s*"
            r"(?:Dos|Exec|Intuition|Graphics|Utility|Locale|GadTools|"
            r"MathIeeeDoubBas|MathIeeeDoubTrans|Mathieeesingbas|Mathffp|Timer|Console|Keymap|Diskfont|Workbench|Icon|"
            r"Layers)\s*\)"
        )
        location = re.search(pattern, native, re.DOTALL)
        if not location:
            location = re.search(
                r"\bIS\s*\(\s*" + re.escape(func_name) + r"\s*,\s*\w+\s*,\s*(\d+)\s*\)",
                native,
            )
        if not location:
            raise ValueError("missing native entry-point slot: " +
                             filename + ":" + func_name)
        vector = 6 * int(location.group(1))
        actual = [handler for slot, handler in registrations[library]
                  if slot == vector]
        if actual == [expected]:
            print("PASS", module_name(library), -vector, expected)
        else:
            failures += 1
            print("FAIL", module_name(library), -vector,
                  "expected", expected, "found", actual)

    functions = parse_c_functions(core)
    task_ops = parse_task_delegates(core, functions)
    lvo_constants = process_lvo_constants(core, task_ops)
    print("PASS constant-LVO register audit (" + str(check_zero_lvo_register_audit()) + " controls)")
    total_manual = 0
    seen_ignored = set()
    seen_p96_refusals = set()
    p96_source = strip_c_comments((core / "kaffeine_p96.c").read_text(encoding="latin-1"))
    p96_sdk = parse_p96_inline(root / P96_SDK_INLINE)
    p96_failures = p96_audit(p96_source, registrations["Picasso96API"], p96_sdk)
    for failure in p96_failures:
        print("FAIL Picasso96API.library:", failure)
    if not p96_failures:
        print("PASS Picasso96API.library table audit (" +
              str(check_p96_audit(p96_source, registrations["Picasso96API"], p96_sdk)) +
              " controls)")
    failures_before = len(p96_failures)
    seen_console_refusals = set()
    seen_keymap_refusals = set()
    seen_icon_refusals = set()
    seen_layers_refusals = set()
    seen_graphics_layer_refusals = set()
    seen_graphics_region_refusals = set()
    seen_graphics_bitmap_refusals = set()
    seen_graphics_palette_refusals = set()
    seen_graphics_node_refusals = set()
    seen_cybergraphics_bitmap_refusals = set()
    palette_refused = re.findall(
        r"IPTR Kaffeine_PaletteUnsupported\([^\n]*\n\{(.*?)\n\}",
        (core / "kaffeine_palette.c").read_text(encoding="latin-1"), re.DOTALL)
    if len(palette_refused) != 1 or not re.fullmatch(
            r'\s*\(void\)cpu;\s*return refuse\(ctx\);\s*', palette_refused[0]):
        raise ValueError("Palette refusal slots must only stop the guest")
    if not re.fullmatch(
            r'\s*\{\s*\(void\)cpu;\s*'
            r'Kaffeine_MessageFailure\(ctx,\s*"untyped graphics extended node"\);\s*'
            r'ctx->callback_failed\s*=\s*TRUE;\s*return 20;\s*\}\s*',
            functions.get("Kaffeine_VideoControlUnsupported", "")):
        raise ValueError("Graphics node refusal slots must only stop the guest")
    bitmap_refused = re.findall(
        r"IPTR kaffeine_bitmaps_Unsupported\([^\n]*\n\{(.*?)\n\}",
        sources["kaffeine_layers.c"], re.DOTALL)
    if len(bitmap_refused) != 1 or not re.fullmatch(
            r'\s*\(void\)cpu;\s*return layers_refuse\(ctx,\s*"unsupported graphics BitMap call"\);\s*',
            bitmap_refused[0]):
        raise ValueError("Bitmap refusal slots must only stop the guest")
    regions_refused = re.findall(
        r"IPTR kaffeine_regions_Unsupported\([^\n]*\n\{(.*?)\n\}",
        (core / "kaffeine_regions.c").read_text(encoding="latin-1"), re.DOTALL)
    if len(regions_refused) != 1 or not re.fullmatch(
            r'\s*\(void\)cpu;\s*return regions_refuse\(ctx,\s*"unsupported Region call"\);\s*',
            regions_refused[0]):
        raise ValueError("Region refusal slots must only stop the guest")
    layers_refused = re.findall(r"IPTR kaffeine_layers_Unsupported\([^\n]*\n\{(.*?)\n\}",
                                sources["kaffeine_layers.c"], re.DOTALL)
    if len(layers_refused) != 1 or not re.fullmatch(
            r'\s*\(void\)cpu;\s*return layers_refuse\(ctx,\s*"unsupported layers\.library call"\);\s*',
            layers_refused[0]):
        raise ValueError("layers refusal slots must only stop the guest")
    icon_refused = re.findall(r"IPTR kaffeine_icon_Unsupported\([^\n]*\n\{(.*?)\n\}", sources["kaffeine_icon.c"], re.DOTALL)
    if len(icon_refused) != 1 or not re.fullmatch(r'\s*\(void\)cpu;\s*return stop\(ctx,\s*"unsupported icon.library call"\);\s*', icon_refused[0]):
        raise ValueError("icon refusal slots must only stop the guest")
    refused_body = re.findall(r"static IPTR unsupported\([^\n]*\n\{(.*?)\n\}",
                              sources["kaffeine_thunks_console.c"], re.DOTALL)
    if len(refused_body) != 1 or not re.fullmatch(
            r'\s*\(void\)cpu;\s*return Kaffeine_IORefuse\(ctx,\s*"[^"\n]*"\);\s*',
            refused_body[0]):
        raise ValueError("console unsupported slots must only call Kaffeine_IORefuse")
    keymap_refused = re.findall(r"IPTR Kaffeine_KeymapUnsupported\([^\n]*\n\{(.*?)\n\}",
                               (core / "kaffeine_keymap.c").read_text(), re.DOTALL)
    if len(keymap_refused) != 1 or not re.fullmatch(
            r'\s*\(void\)cpu;\s*return Kaffeine_IORefuse\(ctx,\s*"[^"\n]*"\);\s*',
            keymap_refused[0]):
        raise ValueError("keymap unsupported slots must only call Kaffeine_IORefuse")
    if set(parse_conf_file(library_source_dir(root, "keymap") / "keymap.conf")) != KEYMAP_REFUSED | {42}:
        raise ValueError("keymap manual profile does not cover its exact native vectors")
    device_refused_body = re.findall(r"static IPTR device_unsupported\([^\n]*\n\{(.*?)\n\}",
                                     sources["kaffeine_exec_io.c"], re.DOTALL)
    if len(device_refused_body) != 1 or not re.fullmatch(
            r'\s*\(void\)cpu;\s*return Kaffeine_IORefuse\(ctx,\s*"[^"\n]*"\);\s*',
            device_refused_body[0]):
        raise ValueError("keyboard/input unsupported slots must only call Kaffeine_IORefuse")
    gels_refused = re.findall(r"IPTR thunk_gfx_GelsUnsupported\([^\n]*\n\{(.*?)\n\}",
                              (core / "kaffeine_thunks_gfx.c").read_text(), re.DOTALL)
    if len(gels_refused) != 1 or not re.fullmatch(
            r'\s*\(void\)cpu;\s*return refuse\(ctx,\s*"unsupported GELS drawing or collision call"\);\s*',
            gels_refused[0]):
        raise ValueError("GELS refusal slots must only stop the guest")
    seen_gels_refusals = set()
    seen_device_refusals = {library: set() for library in DEVICE_REFUSED}
    for library, native_vectors in DEVICE_REFUSED.items():
        conf = parse_conf_file(library_source_dir(root, library) / (library + ".conf"))
        if set(conf) | DEVICE_LIBRARY_VECTORS != native_vectors:
            failures += 1
            print("FAIL", module_name(library), "native vectors", sorted(conf),
                  "differ from the refused ones")
    for library, _, _ in TABLES:
        ndk_spec, aros_spec = load_library_specs(
            root, library, args.ndk_fd_dir, args.aros_fd_dir)
        for vector, handler in registrations[library]:
            total_manual += 1
            if handler == "kaffeine_dos_SegmentUnsupported":
                if library != "dos" or vector not in {774, 780, 786, 1176}:
                    failures += 1
                    print("FAIL unexpected native DOS segment refusal", library, vector)
                continue
            if handler in {"kaffeine_dos_UnsupportedLockObject", "kaffeine_dos_ReadLinkUnsupported"}:
                allowed = ({438} if handler.endswith("ReadLinkUnsupported") else
                           {480, 654, 660, 666, 672, 678, 684, 690, 696, 702})
                if library != "dos" or vector not in allowed:
                    failures += 1
                    print("FAIL unexpected native DOS object refusal", library, vector)
                continue
            if handler == "thunk_gfx_GelsUnsupported":
                if library != "graphics" or vector not in GRAPHICS_GELS_REFUSED:
                    failures += 1
                    print("FAIL", module_name(library), -vector, "unexpected GELS refusal slot")
                else:
                    seen_gels_refusals.add(vector)
                continue
            if handler == "Kaffeine_VideoControlUnsupported":
                if library != "graphics" or vector not in GRAPHICS_NODE_REFUSED:
                    failures += 1
                    print("FAIL", module_name(library), -vector, "unexpected graphics node refusal slot")
                else:
                    seen_graphics_node_refusals.add(vector)
                continue
            if handler == "device_unsupported":
                if library not in DEVICE_REFUSED or vector not in DEVICE_REFUSED[library]:
                    failures += 1
                    print("FAIL", module_name(library), -vector, "unexpected device refusal slot")
                else:
                    seen_device_refusals[library].add(vector)
                continue
            if library == "console" and handler == "unsupported":
                if vector not in CONSOLE_REFUSED:
                    failures += 1
                    print("FAIL console.device", -vector, "unexpected refusal slot")
                seen_console_refusals.add(vector)
                continue
            if handler == "Kaffeine_KeymapUnsupported":
                if library != "keymap" or vector not in KEYMAP_REFUSED:
                    failures += 1
                    print("FAIL", module_name(library), -vector, "unexpected keymap refusal slot")
                else:
                    seen_keymap_refusals.add(vector)
                continue
            if library == "icon" and handler == "kaffeine_icon_Unsupported":
                if vector not in ICON_REFUSED:
                    failures += 1
                    print("FAIL icon.library", -vector, "unexpected refusal slot")
                seen_icon_refusals.add(vector)
                continue
            if handler == "kaffeine_layers_Unsupported":
                allowed = {"layers": (LAYERS_REFUSED, seen_layers_refusals),
                           "graphics": (GRAPHICS_LAYER_REFUSED,
                                        seen_graphics_layer_refusals)}.get(library)
                if not allowed or vector not in allowed[0]:
                    failures += 1
                    print("FAIL", module_name(library), -vector, "unexpected layer refusal slot")
                else:
                    allowed[1].add(vector)
                continue
            if handler == "kaffeine_bitmaps_Unsupported":
                allowed = {"graphics": (GRAPHICS_BITMAP_REFUSED, seen_graphics_bitmap_refusals),
                           "cybergraphics": (CYBERGRAPHICS_BITMAP_REFUSED,
                                             seen_cybergraphics_bitmap_refusals)}.get(library)
                if not allowed or vector not in allowed[0]:
                    failures += 1
                    print("FAIL", module_name(library), -vector, "unexpected bitmap refusal slot")
                else:
                    allowed[1].add(vector)
                continue
            if handler == "kaffeine_regions_Unsupported":
                if library != "graphics" or vector not in GRAPHICS_REGION_REFUSED:
                    failures += 1
                    print("FAIL", module_name(library), -vector, "unexpected Region refusal slot")
                else:
                    seen_graphics_region_refusals.add(vector)
                continue
            if handler == "Kaffeine_PaletteUnsupported":
                if library != "graphics" or vector not in GRAPHICS_PALETTE_REFUSED:
                    failures += 1
                    print("FAIL", module_name(library), -vector, "unexpected palette refusal slot")
                else:
                    seen_graphics_palette_refusals.add(vector)
                continue
            if library == "Picasso96API" and vector in P96_REFUSED:
                # Each refusal is P96_UNSUPPORTED(name) of kaffeine_p96.c for
                # the SDK function at its vector, and the macro reads no
                # register and stops the program.
                want_func = expected_function_name(handler, library)
                entry = ndk_spec.get(vector)
                stem = handler[len("thunk_p96_"):]
                if (entry is None or entry[0] != want_func or
                        len(re.findall(r"^P96_UNSUPPORTED\(" + stem + r"\)$", p96_source,
                                       re.MULTILINE)) != 1):
                    failures += 1
                    print("FAIL", module_name(library), -vector, handler,
                          "is not the refusal of", entry[0] if entry else "an SDK function")
                else:
                    seen_p96_refusals.add(vector)
                continue
            want_func = expected_function_name(handler, library)
            entry = ndk_spec.get(vector)
            if AROS_ONLY_FUNCTIONS.get((library, vector)) == want_func:
                entry = aros_spec.get(vector)
            # AROS-only AVL functions occupy private slots in the 68k NDK fd.
            if (entry is None or entry[0].startswith("execPrivate")) and (
                want_func.startswith("AVL_")
            ):
                entry = aros_spec.get(vector)
            if entry is None:
                failures += 1
                print("FAIL", module_name(library), -vector, handler,
                      "is not a valid LVO in", library + "_lib.fd")
                continue

            fd_func, fd_regs = entry
            if fd_func != want_func:
                failures += 1
                print("FAIL", module_name(library), -vector, handler,
                      "is registered at", fd_func + "()",
                      "instead of", want_func + "()")
                continue

            if handler not in functions:
                failures += 1
                print("FAIL", module_name(library), -vector, handler,
                      "definition not found in rom/kaffeine")
                continue

            read_regs = handler_register_reads(handler, functions, task_ops=task_ops,
                                               lvo_constants=lvo_constants)
            # Reading a register only to discard its value does not implement
            # that argument. Explicit refusal bodies must account for it as
            # ignored, just like handlers that do not read it at all.
            discarded = "\n".join(re.findall(
                r"\(void\)\s*THUNK_[AD]\s*\(\s*\d+\s*\)\s*;",
                functions[handler]))
            read_regs -= direct_register_reads(discarded)
            if handler in PRESERVED_D0_HANDLERS:
                preserves_d0 = re.search(r"return\s+THUNK_D\(0\)\s*;", functions[handler])
                if handler == "Kaffeine_PutMsg":
                    preserves_d0 = re.search(
                        r"return Kaffeine_RoutePutMsg\(ctx, THUNK_A\(0\), THUNK_A\(1\), THUNK_D\(0\)\);",
                        functions[handler]) and "return d0;" in functions["Kaffeine_RoutePutMsg"]
                if not preserves_d0:
                    failures += 1
                    print("FAIL", handler, "does not return its preserved D0")
                read_regs.discard("D0")
            expected_regs = set(fd_regs)
            wrong_regs = sorted(read_regs - expected_regs)
            ignored_regs = frozenset(expected_regs - read_regs)

            if wrong_regs:
                failures += 1
                print("FAIL", module_name(library), -vector, handler,
                      "reads wrong registers", ",".join(wrong_regs),
                      "for", fd_func + "(" + "/".join(fd_regs) + ")")

            if ignored_regs:
                seen_ignored.add(handler)
                allowed = ALLOWED_IGNORED_REGS.get(handler)
                if not allowed or allowed[0] != ignored_regs:
                    failures += 1
                    print("FAIL", module_name(library), -vector, handler,
                          "ignores registers", ",".join(sorted(ignored_regs)),
                          "for", fd_func + "(" + "/".join(fd_regs) + ")")

    if seen_icon_refusals != ICON_REFUSED:
        failures += 1
        print("FAIL icon.library explicit refusal slots changed")
    if seen_console_refusals != CONSOLE_REFUSED:
        failures += 1
        print("FAIL console.device explicit refusal slots changed")
    if seen_keymap_refusals != KEYMAP_REFUSED:
        failures += 1
        print("FAIL keymap.library explicit refusal slots changed")
    for library, vectors in DEVICE_REFUSED.items():
        if seen_device_refusals[library] != vectors:
            failures += 1
            print("FAIL", module_name(library), "explicit refusal slots changed")
    if seen_layers_refusals != LAYERS_REFUSED:
        failures += 1
        print("FAIL layers.library explicit refusal slots changed")
    if seen_gels_refusals != GRAPHICS_GELS_REFUSED:
        failures += 1
        print("FAIL graphics.library GELS refusal slots changed")
    if seen_graphics_layer_refusals != GRAPHICS_LAYER_REFUSED:
        failures += 1
        print("FAIL graphics.library layer refusal slots changed")
    if seen_graphics_region_refusals != GRAPHICS_REGION_REFUSED:
        failures += 1
        print("FAIL graphics.library Region refusal slots changed")
    if seen_graphics_bitmap_refusals != GRAPHICS_BITMAP_REFUSED:
        failures += 1
        print("FAIL graphics.library bitmap refusal slots changed")
    if seen_graphics_palette_refusals != GRAPHICS_PALETTE_REFUSED:
        failures += 1
        print("FAIL graphics.library palette refusal slots changed")
    if seen_graphics_node_refusals != GRAPHICS_NODE_REFUSED:
        failures += 1
        print("FAIL graphics.library node refusal slots changed")
    if seen_p96_refusals != P96_REFUSED:
        failures += 1
        print("FAIL Picasso96API.library refusal slots changed")
    failures += failures_before
    if seen_cybergraphics_bitmap_refusals != CYBERGRAPHICS_BITMAP_REFUSED:
        failures += 1
        print("FAIL cybergraphics.library bitmap refusal slots changed")
    unused_exceptions = sorted(set(ALLOWED_IGNORED_REGS) - seen_ignored)
    for handler in unused_exceptions:
        failures += 1
        print("FAIL", handler, "is listed in ALLOWED_IGNORED_REGS",
              "but ignores no registers")

    print(str(len(CASES)) + " explicit native-vector cases and " +
          str(total_manual) + " manual thunks checked (" +
          str(failures) + " failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
