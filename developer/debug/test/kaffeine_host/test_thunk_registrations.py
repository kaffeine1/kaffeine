#!/usr/bin/env python3
"""Check manual registrations and register conventions in the production thunk tables.

Copyright (C) 2026, The AROS Development Team. All rights reserved.

The C preprocessor expands the production header macros for all sixteen manual
thunk tables (exec, dos, intuition, graphics, utility, locale, mathieeedoubbas,
mathieeedoubtrans, mathieeesingbas, mathffp, timer, console, diskfont, gadtools, icon and workbench). Expected
vector numbers come independently from the native AROS entry points and from
the NDK 3.2 fd files (or AROS fd / .conf files for
AROS-only AVL functions). Each manual handler and the helpers it delegates to
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
    ("gadtools", "kaffeine_thunks.c", "kaffeine_thunks_gadtools_manual"),
    ("workbench", "kaffeine_workbench.c", "kaffeine_thunks_workbench"),
)

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
DISKFONT_NDK32 = {30: ("OpenDiskFont", ("A0",))}

# Tables of devices, whose bases are named .device.
DEVICES = ("timer", "console")

# These device slots deliberately stop the guest; they claim no native API.
CONSOLE_REFUSED = frozenset((6, 12, 18, 24, 30, 36, 42, 54, 60, 66, 72))
ICON_REFUSED = frozenset((54, 72, 84, 108, 126, 138, 150, 156, 162, 168, 180, 186, 192, 198))


def module_name(library):
    return library + (".device" if library in DEVICES else ".library")


CASES = (
    ("graphics", "openmonitor.c", "OpenMonitor", "thunk_gfx_OpenMonitor"),
    ("graphics", "closemonitor.c", "CloseMonitor", "thunk_gfx_CloseMonitor"),
    ("graphics", "getdisplayinfodata.c", "GetDisplayInfoData", "thunk_gfx_GetDisplayInfoData"),
    ("graphics", "finddisplayinfo.c", "FindDisplayInfo", "thunk_gfx_FindDisplayInfo"),
    ("icon", "getdiskobject.c", "GetDiskObject", "kaffeine_icon_GetDiskObject"),
    ("icon", "freediskobject.c", "FreeDiskObject", "kaffeine_icon_FreeDiskObject"),
    ("icon", "findtooltype.c", "FindToolType", "kaffeine_icon_FindToolType"),
    ("icon", "matchtoolvalue.c", "MatchToolValue", "kaffeine_icon_MatchToolValue"),
    ("icon", "getdefdiskobject.c", "GetDefDiskObject", "kaffeine_icon_GetDefDiskObject"),
    ("icon", "getdiskobjectnew.c", "GetDiskObjectNew", "kaffeine_icon_GetDiskObjectNew"),
    ("icon", "newdiskobject.c", "NewDiskObject", "kaffeine_icon_NewDiskObject"),

    ("diskfont", "opendiskfont.c", "OpenDiskFont", "thunk_diskfont_OpenDiskFont"),
    ("console", "rawkeyconvert.c", "RawKeyConvert", "kaffeine_console_RawKeyConvert"),
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
    ("graphics", "readpixel.c", "ReadPixel", "thunk_gfx_ReadPixel"),
    ("graphics", "getbpen.c", "GetBPen", "thunk_gfx_GetBPen"),
    ("graphics", "textextent.c", "TextExtent", "thunk_gfx_TextExtent"),
    ("graphics", "textfit.c", "TextFit", "thunk_gfx_TextFit"),
    ("exec", "getmsg.c", "GetMsg", "Kaffeine_GetMsg"),
    ("exec", "waitport.c", "WaitPort", "Kaffeine_WaitPort"),
    ("intuition", "modifyidcmp.c", "ModifyIDCMP", "Kaffeine_ModifyIDCMP"),
    ("gadtools", "gt_getimsg.c", "GT_GetIMsg", "Kaffeine_GT_GetIMsg"),
    ("gadtools", "gt_postfilterimsg.c", "GT_PostFilterIMsg", "Kaffeine_GT_PostFilterIMsg"),
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
    ("exec", "createmsgport.c", "CreateMsgPort", "Kaffeine_CreateMsgPort"),
    ("timer", "beginio.c", "BeginIO", "kaffeine_timer_BeginIO"),
    ("timer", "getsystime.c", "GetSysTime", "kaffeine_timer_GetSysTime"),
)

# Explicit table of manual handlers that intentionally ignore one or more
# fd arguments, with the exact ignored register set and the reason.
ALLOWED_IGNORED_REGS = {
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
    "kaffeine_dos_CreateProc": (
        frozenset({"D1", "D2", "D3", "D4"}),
        "CreateProc() refuses with ERROR_NOT_IMPLEMENTED",
    ),
    "kaffeine_dos_CliInitNewcli": (
        frozenset({"A0"}),
        "CliInitNewcli() refuses a 68k DosPacket with ERROR_NOT_IMPLEMENTED",
    ),
    "kaffeine_dos_CliInitRun": (
        frozenset({"A0"}),
        "CliInitRun() refuses a 68k DosPacket with ERROR_NOT_IMPLEMENTED",
    ),
    "kaffeine_dos_CreateNewProc": (
        frozenset({"D1"}),
        "CreateNewProc() refuses with ERROR_NOT_IMPLEMENTED",
    ),
    "kaffeine_dos_ExAllEnd": (
        frozenset({"D1", "D2", "D3", "D4"}),
        "ExAllEnd() only needs the control: the emulated scan keeps its state there",
    ),
    "kaffeine_dos_DoPkt": (
        frozenset({"D1", "D2", "D3", "D4", "D5", "D6", "D7"}),
        "DoPkt() refuses with ERROR_NOT_IMPLEMENTED outside the owned PutMsg ACTION_READ adapter",
    ),
    "kaffeine_dos_SendPkt": (
        frozenset({"D1", "D2", "D3"}),
        "SendPkt() refuses with ERROR_NOT_IMPLEMENTED outside the owned PutMsg ACTION_READ adapter",
    ),
    "kaffeine_dos_ReplyPkt": (
        frozenset({"D1", "D2", "D3"}),
        "ReplyPkt() refuses with ERROR_NOT_IMPLEMENTED outside the owned PutMsg ACTION_READ adapter",
    ),
    "kaffeine_dos_AbortPkt": (
        frozenset({"D1", "D2"}),
        "AbortPkt() refuses with ERROR_NOT_IMPLEMENTED outside the owned PutMsg ACTION_READ adapter",
    ),
    "Kaffeine_OpenWindowTagList": (
        frozenset({"A0"}),
        "OpenWindowTagList() passes NULL for the optional NewWindow in A0 and uses A1 tags",
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
}

HANDLER_PREFIX_RE = re.compile(
    r"^(?:wb_|thunk_(?:exec|dos|intuition|gfx|graphics|diskfont|utility|locale)_|"
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
    rom_dir = root / "rom" / library
    if rom_dir.is_dir():
        return rom_dir
    device_dir = root / "rom/devs" / library
    if library in DEVICES and device_dir.is_dir():
        return device_dir
    return root / "workbench/libs" / library


def load_library_specs(root, library, ndk_fd_dir, aros_fd_dir):
    conf_entries = parse_conf_file(
        library_source_dir(root, library) / (library + ".conf"))
    ndk_fd = ndk_fd_dir / (library + "_lib.fd") if ndk_fd_dir else None
    aros_fd = aros_fd_dir / (library + "_lib.fd") if aros_fd_dir else None

    ndk_entries = parse_fd_file(ndk_fd) if ndk_fd and ndk_fd.is_file() else {}
    aros_entries = (parse_fd_file(aros_fd)
                    if aros_fd and aros_fd.is_file() else conf_entries)
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


def handler_register_reads(name, functions, seen=None, task_ops=None):
    if seen is None:
        seen = set()
    if name in seen or name not in functions:
        return set()
    seen.add(name)
    body = functions[name]
    regs = direct_register_reads(body)
    for callee in re.findall(r"\b([A-Za-z_]\w*)\s*\(", body):
        # These CPU operations save/restore architectural state or propagate
        # a stopped process result. Their registers are not FD arguments.
        if callee in {"Kaffeine_CallGuest", "Kaffeine_StopGuest"}:
            continue
        if callee in functions and callee != name:
            regs |= handler_register_reads(callee, functions, seen, task_ops)
    for member in re.findall(r"\bctx->task_ops->(\w+)\s*\(", body):
        if not task_ops or member not in task_ops:
            raise ValueError("unresolved guest task operation: " + member)
        regs |= handler_register_reads(task_ops[member], functions, seen, task_ops)
    return regs


# Handlers whose name cannot be the one of the function they implement
HANDLER_FUNCTIONS = {
    # Kaffeine_CreateContext() makes the context of the emulator itself
    "Kaffeine_CreateGadgetContext": "CreateContext",
}


def expected_function_name(handler):
    return HANDLER_FUNCTIONS.get(handler) or HANDLER_PREFIX_RE.sub("", handler)


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
            r"Device|TimerBase|WorkbenchBase|IconBase)"
            r"\s*\*,\s*\w+\s*,\s*(\d+)\s*,\s*"
            r"(?:Dos|Exec|Intuition|Graphics|Utility|Locale|GadTools|"
            r"MathIeeeDoubBas|MathIeeeDoubTrans|Mathieeesingbas|Mathffp|Timer|Console|Diskfont|Workbench|Icon)\s*\)"
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
    total_manual = 0
    seen_ignored = set()
    seen_console_refusals = set()
    seen_icon_refusals = set()
    icon_refused = re.findall(r"IPTR kaffeine_icon_Unsupported\([^\n]*\n\{(.*?)\n\}", sources["kaffeine_icon.c"], re.DOTALL)
    if len(icon_refused) != 1 or not re.fullmatch(r'\s*\(void\)cpu;\s*return stop\(ctx,\s*"unsupported icon.library call"\);\s*', icon_refused[0]):
        raise ValueError("icon refusal slots must only stop the guest")
    refused_body = re.findall(r"static IPTR unsupported\([^\n]*\n\{(.*?)\n\}",
                              sources["kaffeine_thunks_console.c"], re.DOTALL)
    if len(refused_body) != 1 or not re.fullmatch(
            r'\s*\(void\)cpu;\s*return Kaffeine_IORefuse\(ctx,\s*"[^"\n]*"\);\s*',
            refused_body[0]):
        raise ValueError("console unsupported slots must only call Kaffeine_IORefuse")
    for library, _, _ in TABLES:
        ndk_spec, aros_spec = load_library_specs(
            root, library, args.ndk_fd_dir, args.aros_fd_dir)
        for vector, handler in registrations[library]:
            total_manual += 1
            if library == "console" and handler == "unsupported":
                if vector not in CONSOLE_REFUSED:
                    failures += 1
                    print("FAIL console.device", -vector, "unexpected refusal slot")
                seen_console_refusals.add(vector)
                continue
            if library == "icon" and handler == "kaffeine_icon_Unsupported":
                if vector not in ICON_REFUSED:
                    failures += 1
                    print("FAIL icon.library", -vector, "unexpected refusal slot")
                seen_icon_refusals.add(vector)
                continue
            want_func = expected_function_name(handler)
            entry = ndk_spec.get(vector)
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

            read_regs = handler_register_reads(handler, functions, task_ops=task_ops)
            # Reading a register only to discard its value does not implement
            # that argument. Explicit refusal bodies must account for it as
            # ignored, just like handlers that do not read it at all.
            discarded = "\n".join(re.findall(
                r"\(void\)\s*THUNK_[AD]\s*\(\s*\d+\s*\)\s*;",
                functions[handler]))
            read_regs -= direct_register_reads(discarded)
            if handler in PRESERVED_D0_HANDLERS:
                if not re.search(r"return\s+THUNK_D\(0\)\s*;", functions[handler]):
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
