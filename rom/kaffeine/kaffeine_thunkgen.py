#!/usr/bin/env python3
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Fabian Schmieder (@metaneutrons)
"""kaffeine thunk generator for AROS."""
# Usage: python3 kaffeine_thunkgen.py <fd_directory> [<ndk_clib_directory>] > kaffeine_thunks_gen.c
#
# If clib directory is provided, function prototypes are parsed for accurate
# parameter types (shadow lookups, string translation, struct translation, etc.).
# Without it, falls back to generic APTR/ULONG behavior.

import argparse, csv, sys, os, re, glob

# ── Libraries to SKIP entirely ──
SKIP_LIBRARIES = {
    "workbench", "usergroup", "timer", "task", "rexxsyslib", "rexxsupport",
    "reqtools", "realtime", "openurl", "nonvolatile", "muiscreen", "muimaster",
    "misc", "mathtrans", "mathieeesingtrans", "mathieeesingbas",
    "mathieeedoubtrans", "mathieeedoubbas", "mathffp", "lowlevel", "input",
    "identify", "console", "colorwheel", "cardres", "camd", "battclock",
    "asyncio", "amigaguide", "codesets", "utf8proc", "glu", "png", "tiff",
    "expat", "zstd", "lzma", "bz2", "bullet", "freetype2",
    "aros", "hostlib", "kernel", "kms", "gallium", "x11gfx", "efi",
    "openfirmware", "rtas", "vmm", "hpet", "prometheus", "pccard",
    "ata", "scsi", "poseidon", "usbclass", "lddemon", "debug",
    "oop", "log", "processor", "bootloader", "partition",
    "posixc", "stdc", "stdcio", "kaffeine", "fd",
    "pngdt", "dxtn", "jfif", "coolimages", "diskimage", "nvdisk", "ramdrive",
    "z1", "uuid",
}

# ── Functions to SKIP (manual impl, dangerous, or internal) ──
SKIP_FUNCTIONS = {
    "AddPort", "AllocMem", "AllocPooled", "AllocVec", "AllocVecPooled",
    "AvailMem", "CacheClearU", "Close", "CloseLibrary", "CloseScreen",
    "CloseWindow", "CopyMem", "CreatePool", "CurrentDir", "Delay",
    "DeleteFile", "DeletePool", "Draw", "Examine", "FPuts", "FindName",
    "FindTask", "Forbid", "FreeArgs", "FreeMem", "FreePooled", "FreeVec",
    "FreeVecPooled2", "GetMsg", "GetProgramName", "InitSemaphore", "Input",
    "IoErr", "Lock", "Move", "ObtainSemaphore", "OldOpenLibrary", "Open",
    "OpenLibrary", "OpenScreen", "OpenWindowTagList", "Output", "Permit",
    "PutStr", "RawDoFmt", "Read", "ReadArgs", "RectFill",
    "ReleaseSemaphore", "RemPort", "Rename", "ReplyMsg", "ScrollRaster",
    "Seek", "SetABPenDrMd", "SetAPen", "SetBPen", "SetDrMd", "SetFont",
    "SetIoErr", "SetSignal", "Text", "TextLength", "TypeOfMem", "UnLock",
    "VPrintf", "WaitPort", "Write",
    # AROS keeps these LVOs private - no public C prototype exists for them
    "AddDisplayData", "AddDisplayInfoData", "SetDisplayInfoData",
    "LocRawDoFmt", "LocStrnicmp", "LocStricmp", "LocToLower", "LocToUpper",
    "LocDateToStr", "LocStrToDate", "LocDosGetLocalizedString", "LocVNewRawDoFmt",
    # takes a native va_list, which cannot be built from a m68k argument stream
    "VNewRawDoFmt",
    "AVL_AddNode", "AVL_FindNode", "AVL_RemNodeByAddress", "AddDevice",
    "AddIntServer", "AddLibrary", "AddMemHandler", "AddMemList",
    "AddResetCallback", "Alert", "AllocDosObject", "CachePostDMA",
    "CachePreDMA", "Cause", "CliInitNewcli", "CliInitRun", "CloseLib",
    "ColdReboot", "CreateNewProc", "CreateProc", "Debug", "Dispatch",
    "DoPkt", "ExAll", "ExAllEnd", "ExNext", "Exception", "Execute",
    "ExitIntr", "FreeDosObject", "InitCode", "InitResident", "InitStruct",
    "InternalLoadSeg", "InternalUnLoadSeg", "LoadSeg", "MakeFunctions",
    "MakeLibrary", "MatchEnd", "MatchFirst", "MatchNext", "NewLoadSeg",
    "OpenLib", "RemDevice", "RemIntServer", "RemLibrary", "RemMemHandler",
    "RemResetCallback", "ReplyPkt", "Reschedule", "RunCommand", "RunProcess",
    "Schedule", "SendPkt", "SetFunction", "SetIntVector", "SetSR",
    "StackSwap", "SumKickData", "SumLibrary", "SuperState", "Supervisor",
    "Switch", "SystemTagList", "UnLoadSeg", "UserState", "VFPrintf",
    "VFWritef", "WaitPkt", "close", "open", "private",
}

# ── Functions that return void ──
VOID_FUNCTIONS = {
    "Text", "ZipWindow", "WriteExpansionWord", "WriteExpansionByte",
    "WriteChunkyPixels", "WriteBattClock", "WindowAction", "WaitBOVP",
    "URL_OldFreePrefs", "URL_FreePrefsA", "UpdateBitMap", "UnLockTaskList",
    "UnlockRexxBase", "UnlockRealTime", "UnlockIBase", "UnlockCAMD",
    "UnLockBitMapTagList", "UnLockBitMap", "ThinLayerInfo", "TextExtent",
    "SyncSBitMap", "SubTime", "StripFont", "StoreItemInContext",
    "StopTimerInt", "StartTimerInt", "StartClusterNotify", "SortLayerCR",
    "SkipSysEx", "ShowTitle", "setutent", "SetTranslate", "SetRGB4CM",
    "SetRGB32CM", "SetRast", "setpwent", "SetPointer", "SetLocalItemPurge",
    "SetKeyMapDefault", "setgrent", "SetFilterIX", "SetFilter",
    "SetDisplayDriverCallback", "SetDefaultScreenFont", "SetCurrentBinding",
    "SetCollision", "SetABPenDrMd", "SendIntuiMessage",
    "ScrollWindowRasterNoFill", "ScrollRasterBF", "ScrollLayer",
    "ScreenPosition", "ScreenDepth", "rtUnlockWindow", "rtUnlockPrefs",
    "rtSpread", "rtSetWaitPointer", "rtSetReqPosition",
    "rtScreenToFrontSafely", "rtFreeRequest", "rtFreeReqBuffer",
    "rtFreeFileList", "rtCloseWindowSafely", "RouteCxMsg",
    "RestoreTaskStorage", "ResetBattClock", "ReplyAmigaGuideMsg",
    "RemVSprite", "RemVBlankInt", "RemTimerInt", "RemoveMidiLink",
    "RemoveClass", "RemNamedObject", "RemKBInt", "RemIBob", "RemFont",
    "RemConSnipHook", "RemConfigDev", "ReleasePen", "ReleaseGIRPort",
    "ReleaseDTDrawInfo", "ReleaseDataType", "ReleaseConfigBinding",
    "ReleaseCard", "RefreshTagItemClones", "RefreshGList",
    "RefreshDTObjectA", "QueryTaskTagList", "QueryKeys", "QBSBlit",
    "QBlit", "PutSysEx", "PutMidi", "ProcessPixelArray", "PrintIText",
    "PolyDraw", "ParseMidi", "ObtainConfigBinding", "MUIS_RemInfoClient",
    "MUIS_FreePubScreenDesc", "MUIS_ClosePubFile", "MUIS_AddInfoClient",
    "MUI_RequestIDCMP", "MUI_RemoveClipRegion", "MUI_RemoveClipping",
    "MUI_ReleasePen", "MUI_RejectIDCMP", "MUI_Redraw", "MUI_FreeClass",
    "MUI_FreeAslRequest", "MUI_EndRefresh", "MUI_DisposeObject",
    "MoveWindowInFrontOf", "MoveScreen", "MapTags", "LockRexxBase",
    "LockDataType", "LoadView", "LendMenus", "InsertCxObj", "InitVPort",
    "InitView", "InitRequester", "InitMasks", "InitLayers", "InitIFFasDOS",
    "InitIFFasClip", "InitIFF", "InitGMasks", "InitGels", "InitArea",
    "IdHardwareUpdate", "HelpControl", "GT_ReplyIMsg", "GT_EndRefresh",
    "GT_BeginRefresh", "GfxFree", "GfxAssociate", "GetUpTime", "GetSysTime",
    "GetRPAttrsA", "GetRGB32", "GadgetMouse", "FreeTaskStorageSlot",
    "FreeSpriteData", "FreeNVData", "FreeMonitorList", "FreeMiscResource",
    "FreeLocalItem", "FreeIntuiMessage", "FreeICData", "FreeExpansionMem",
    "FreeDTMethods", "FreeDBufInfo", "FreeConfigDev", "FreeCModeList",
    "FreeBrokerList", "FreeBoardMem", "FormatDate", "FontExtent",
    "FlushMidi", "FilterTagChanges", "ExpungeXRef", "EraseImage",
    "endutent", "EndUpdate", "EndRefresh", "endpwent", "endgrent",
    "EndClusterNotify", "DrawImageState", "DrawImage", "DrawIconStateA",
    "DrawEllipse", "DrawBorder", "DrawBevelBoxA", "DoHookClipRects",
    "DoCollision", "DoCDrawMethodTagList", "DivertCxMsg",
    "DisposeLayerInfo", "DisposeFontContents", "DisposeCxMsg",
    "DisplayBeep", "DeleteRexxMsg", "DeletePlayer", "DeleteMidi",
    "DeleteArgstring", "CWait", "CVideoCtrlTagList", "CurrentTime",
    "CopySBitMap", "ConvertRGBToHSB", "ConvertHSBToRGB", "ConfigChain",
    "CollectPixelsLayer", "CMove", "ClosePortRsrc", "CloseMidiDevice",
    "CloseClipboard", "ClearScreen", "ClearRexxMsg", "ClearEOL",
    "ChangeVPBitMap", "ChangeToSelectedIconColor", "ChangeDecoration",
    "CBump", "BltTemplateAlpha", "BltClear", "BitMapScale", "BeginRefresh",
    "AskFont", "ApplyTagChanges", "Animate", "Amiga2Date", "AlohaWorkbench",
    "AllocBoardMem", "AddVSprite", "AddTime", "AddIEvents", "AddFont",
    "AddConSnipHook", "AddConfigDev", "AddClass", "AddBob", "AddAnimOb",
    "ActivateAslRequest", "AbortAslRequest", "UnlockLayerInfo",
    "LockLayerInfo", "UnlockLayers", "LockLayers", "UnlockLayer",
    "LockLayer", "UnlockLayerRom", "LockLayerRom", "MoveSprite",
    "ChangeSprite", "FreeGBuffers", "DrawGList", "SortGList",
    "SwapBitsRastPortClipRect", "DisposeRegion", "ClearRegion",
    "ClearRectRegion", "XorRectRegion", "OrRectRegion", "AndRectRegion",
    "SetWriteMask", "SetMaxPen", "BltMaskBitMapRastPort", "BltPattern",
    "BltTemplate", "ClipBlit", "BltBitMapRastPort", "BltBitMap",
    "WritePixelArray8", "WritePixelLine8", "SetRPAttrsA",
    "ScrollWindowRaster", "EraseRect", "SetPointerA", "ModifyProp",
    "NewModifyProp", "RemoveGadget", "AddGadget", "RemoveGList", "AddGList",
    "OffGadget", "OnGadget", "OffMenu", "OnMenu", "SetMenuStrip",
    "UnlockPubScreenList", "LockPubScreenList", "SetDefaultPubScreen",
    "ChangeWindowBox", "FreeScreenBuffer", "UnlockPubScreen", "SetAttrsA",
    "ReleaseNamedObject", "FreeNamedObject", "SetAmigaGuideAttrsA",
    "CloseAmigaGuide", "UnlockAmigaGuideBase", "Forbid", "Permit",
    "Disable", "Enable", "InitSemaphore", "ObtainSemaphore",
    "ReleaseSemaphore", "AddPort", "RemPort", "AddSemaphore",
    "RemSemaphore", "CopyMem", "CopyMemQuick", "CacheClearU", "CacheClearE",
    "FreeMem", "FreeVec", "FreePooled", "DeletePool", "FreeVecPooled",
    "CloseLibrary", "Close", "UnLock", "Delay", "FreeArgs", "FreeDosObject",
    "AbortPkt", "FreeDeviceProc", "FreeDosEntry", "EndNotify",
    "SortDirList", "SetIoErr", "RawIOInit", "RawPutChar", "ChildFree",
    "ChildOrphan", "RemResetCallback", "Move", "Draw", "SetAPen", "SetBPen",
    "SetDrMd", "SetFont", "RectFill", "ScrollRaster", "SetRGB4", "SetRGB32",
    "WaitBlit", "WaitTOF", "InitRastPort", "InitBitMap", "FreeRaster",
    "FreeBitMap", "FreeColorMap", "DisownBlitter", "OwnBlitter", "CloseFont",
    "FreeSprite", "LoadRGB4", "SetWindowPointerA", "RefreshGList", "LoadRGB32", "ScrollVPort", "MakeVPort",
    "MrgCop", "FreeVPortCopLists", "FreeCopList", "FreeCprList",
    "CloseWindow", "CloseScreen", "ClearMenuStrip", "ClearPointer",
    "ActivateWindow", "ActivateGadget", "RefreshGadgets",
    "RefreshWindowFrame", "MoveWindow", "SizeWindow", "WindowToFront",
    "WindowToBack", "ScreenToFront", "ScreenToBack", "ModifyIDCMP",
    "ReportMouse", "FreeScreenDrawInfo", "FreeRemember", "DisposeObject",
    "SetWindowTitles", "SetWindowPointerA", "RefreshGList", "EndRequest", "FreeSysRequest",
    "FreeGadgets", "FreeMenus", "FreeVisualInfo", "GT_SetGadgetAttrsA",
    "GT_RefreshWindow", "InstallClipRegion", "DeleteLayer", "FreeIFF",
    "CloseIFF", "FreeAslRequest", "FreeFileRequest", "FreeDiskObject",
    "FreeFreeList", "CloseCatalog", "CloseLocale", "CloseWorkbenchObjectA",
    "FreeTagItems", "DisposeDTObject", "DeleteCxObj", "DeleteCxObjAll",
    "ClearCxObjError", "SetCxObjPri", "AttachCxObj", "EnqueueCxObj",
    "RemoveCxObj", "ActivateCxObj", "AddHead", "AddResource", "AddTail",
    "CloseDevice", "Deallocate", "DeleteIORequest", "DeleteMsgPort",
    "Enqueue", "Exit", "FreeEntry", "FreeSignal", "FreeTrap", "Insert",
    "NewMinList", "ObtainSemaphoreList", "ObtainSemaphoreShared", "PutMsg",
    "ReleaseSemaphoreList", "Remove", "RemResource", "RemTask", "SendIO",
    "Signal", "UnLockDosList", "Vacate",
}

# Functions that return struct pointers — wrap with shadow_create_by_name
SHADOW_RETURNS = {
    "FindName": "Node", "FindTask": "Task", "FindPort": "MsgPort",
    "FindSemaphore": "SignalSemaphore", "FindResident": "Resident",
    "CreateIORequest": "IORequest", "CreateMsgPort": "MsgPort",
    "OpenFont": "TextFont", "AllocBitMap": "BitMap",
    "CreateRastPort": "RastPort", "CloneRastPort": "RastPort",
    "GetColorMap": "ColorMap",
    "CreateGadgetA": "Gadget", "CreateMenusA": "Menu",
    "FindTagItem": "TagItem", "NextTagItem": "TagItem",
    "AllocNamedObjectA": "NamedObject",
    "LockPubScreen": "Screen",
    "BuildEasyRequestArgs": "Window",
    "GetDeviceProc": "DevProc",
    "FindTaskByPID": "Task",
}

# ── D-register args that are pointers (fallback when no proto info) ──
DPTR_OVERRIDES = {
    "Open": {"d1"}, "DeleteFile": {"d1"}, "Rename": {"d1", "d2"},
    "Read": {"d2"}, "Write": {"d2"}, "Lock": {"d1"},
    "PutStr": {"d1"}, "FPuts": {"d2"}, "VPrintf": {"d1", "d2"},
    "FWrite": {"d2"}, "FRead": {"d2"}, "FGets": {"d2"},
    "SetFileDate": {"d1"}, "NameFromLock": {"d2"}, "NameFromFH": {"d2"},
    "FilePart": {"d1"}, "PathPart": {"d1"}, "AddPart": {"d1", "d2"},
    "GetProgramName": {"d1"}, "SetComment": {"d1", "d2"},
    "SetProtection": {"d1"}, "MakeDosEntry": {"d1"},
    "SetVar": {"d1", "d2"}, "GetVar": {"d1", "d2"},
    "DeleteVar": {"d1"}, "FindVar": {"d1"},
    "MatchPattern": {"d1", "d2"}, "MatchPatternNoCase": {"d1", "d2"},
    "ParsePattern": {"d1", "d2"}, "ParsePatternNoCase": {"d1", "d2"},
    "CreateDir": {"d1"}, "SetCurrentDirName": {"d1"},
    "GetCurrentDirName": {"d1"}, "SetProgramName": {"d1"},
    "Fault": {"d3"}, "PrintFault": {"d2"},
    "Relabel": {"d1", "d2"}, "SplitName": {"d1", "d3"},
    "AssignLock": {"d1"}, "AssignLate": {"d1", "d2"},
    "AssignPath": {"d1", "d2"}, "AssignAdd": {"d1"},
    "GetDeviceProc": {"d1"},
    "Text": {"d0"},
    "Stricmp": {"d0", "d1"}, "Strnicmp": {"d0", "d1"},
    "GetDiskObject": {"d0"}, "PutDiskObject": {"d0"},
    "GetDiskObjectNew": {"d0"},
    "OpenCatalog": {"d0"},
}

# ── Type classification for proto-aware thunk generation ──

# Opaque struct pointers: use shadow_lookup to find native pointer
SHADOW_LOOKUP_TYPES = {
    "RastPort", "ViewPort", "Screen", "Window", "ColorMap", "BitMap",
    "Layer", "Region", "TextFont", "GadgetInfo", "DrawInfo", "Menu",
    "Gadget", "Requester", "IntuiMessage", "MsgPort", "IORequest",
    "Window", "Screen", "MsgPort", "RastPort", "ViewPort",
    "Gadget", "Menu", "MenuItem", "Requester", "IntuiMessage",
    "IORequest", "DiskObject", "IFFHandle", "FileRequester",
}

# ── Types that are input structs needing manual thunks ──
INPUT_STRUCT_TYPES = {
    "NewWindow", "NewScreen", "EasyStruct", "ExtNewScreen",
}

STRING_TYPES = {"STRPTR", "CONST_STRPTR", "char *", "const char *", "UBYTE *", "CONST UBYTE *"}

SCALAR_TYPES = {"ULONG", "LONG", "WORD", "UWORD", "BOOL", "UBYTE", "BYTE",
                "Tag", "IPTR", "SIPTR", "int", "long"}

# BCPL pointers are passed through untranslated, like the hand written thunks do
BPTR_TYPES = {"BPTR", "BSTR"}

# pointer typedefs that carry no '*' of their own
POINTER_TYPES = {"APTR", "CONST_APTR", "PLANEPTR", "Msg", "VOID_FUNC",
                 "DisplayInfoHandle"}

DPTR_OVERRIDES = {
    "DrawImage": {"d0"},
    "DrawBorder": {"d0"},
    "PrintIText": {"d0"},
    "GetDTMethods": {"d0"},
    "GetDTTriggerMethods": {"d0"},
}


def parse_param(p):
    """Parse individual parameter string."""
    p = p.strip()
    if not p or p == "VOID":
        return None
    if "(" in p:
        return ("APTR", "funcptr")
    m = re.match(r"^(.*?)\s*(\**)(\w+)$", p)
    if m:
        base_type = m.group(1).strip()
        stars = m.group(2)
        name = m.group(3)
        full_type = base_type + " " + stars if (stars and base_type) else (stars or base_type)
        return (full_type.strip(), name)
    return ("ULONG", p)


def parse_proto_chunk(chunk):
    """Parse a single C prototype chunk."""
    m = re.search(
        r"((?:CONST\s+)?(?:struct\s+\w+\s*\*|[\w]+(?:\s*\*)?)\s*)"
        r"__stdargs\s+(\w+)\s*\(\s*(.*?)\s*\)",
        chunk,
    )
    if not m:
        return None
    ret_type, func_name, params_str = m.group(1).strip(), m.group(2).strip(), m.group(3).strip()
    if "..." in params_str:
        return None
    params = []
    if params_str and params_str != "VOID":
        for p in params_str.split(","):
            res = parse_param(p)
            if res:
                params.append(res)
    return func_name, (ret_type, params)


def parse_clib_protos(clib_dir):
    """Parse all *_protos.h files in clib_dir."""
    protos = {}
    if not clib_dir or not os.path.isdir(clib_dir):
        return protos
    for proto_file in sorted(glob.glob(os.path.join(clib_dir, "*_protos.h"))):
        with open(proto_file, encoding="latin-1") as f:
            text = f.read()
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
        text = re.sub(r"#[^\n]*", " ", text)
        text = re.sub(r"\\\n", " ", text)
        text = re.sub(r"\s+", " ", text)
        for chunk in text.split(";"):
            entry = parse_proto_chunk(chunk)
            if entry:
                protos[entry[0]] = entry[1]
    return protos


def classify_type(ptype):
    """Classify a C type string into a category for thunk generation."""
    ptype = ptype.strip()
    clean = re.sub(r"\bCONST\b|\bconst\b", "", ptype).strip()
    if clean.count("*") > 1:
        return ("unsupported",)     # pointer to pointer - needs write back to m68k memory
    if clean in STRING_TYPES or ptype in STRING_TYPES:
        return ("string",)
    base = clean.rstrip(" *")
    if base in BPTR_TYPES:
        return ("bptr",)
    if base == "RAWARG":
        return ("rawarg",)
    if base in POINTER_TYPES:
        return ("pointer",)
    sm = re.match(r"struct\s+(\w+)\s*\*", clean)
    if sm:
        sname = sm.group(1)
        if sname == "TagItem":
            return ("taglist",)
        if sname in SHADOW_LOOKUP_TYPES:
            return ("shadow", sname)
        if sname in INPUT_STRUCT_TYPES:
            return ("input_struct", sname)
        return ("pointer",)
    if base in SCALAR_TYPES or clean in SCALAR_TYPES or "*" not in clean:
        return ("pointer",) if "*" in clean else ("scalar",)
    return ("pointer",)


def classify_return_type(rtype):
    """Classify return type."""
    rtype = rtype.strip()
    if rtype.lower() == "void":
        return ("void",)
    clean = re.sub(r"\bCONST\b|\bconst\b", "", rtype).strip()
    sm = re.match(r"struct\s+(\w+)\s*\*", clean)
    if sm:
        sname = sm.group(1)
        if sname in SHADOW_RETURNS.values():
            return ("shadow_create", sname)
    return ("plain",)


def parse_fd_line(line):
    """Parse a single FD entry line."""
    m = re.match(r"(\w+)\(([^)]*)\)\(([^)]*)\)", line)
    if not m:
        return None
    name = m.group(1)
    args = [a.strip() for a in m.group(2).split(",") if a.strip()]
    regs = [r.strip().lower() for r in m.group(3).split(",") if r.strip()]
    return name, args, regs


def parse_fd_directive(line, base_name, bias, public):
    """Handle ## directive in FD line."""
    if line.startswith("##base"):
        return line.split()[1].lstrip("_"), bias, public
    if line.startswith("##bias"):
        return base_name, int(line.split()[1]), public
    if line == "##public":
        return base_name, bias, True
    if line == "##private":
        return base_name, bias, False
    return base_name, bias, public


def parse_fd(filename):
    """Parse an FD file, returning (base_name, list of (name, bias, args, regs))."""
    funcs = []
    bias = 30
    public = True
    base_name = None

    with open(filename) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("*"):
                continue
            if line.startswith("##"):
                base_name, bias, public = parse_fd_directive(line, base_name, bias, public)
                continue
            if public:
                entry = parse_fd_line(line)
                if entry:
                    funcs.append((entry[0], bias, entry[1], entry[2]))
            bias += 6

    return base_name, funcs


def validate_regs(regs):
    """Validate register names."""
    for reg in regs:
        if len(reg) != 2 or reg[0] not in ("a", "d") or not reg[1].isdigit():
            return False
    return True


def get_func_properties(funcname, proto):
    """Determine void status and shadow return type for a function."""
    is_void = funcname in VOID_FUNCTIONS
    shadow_ret = SHADOW_RETURNS.get(funcname)
    if proto:
        ret_class = classify_return_type(proto[0])
        if not is_void and ret_class[0] == "void":
            is_void = True
        if not shadow_ret and not is_void and ret_class[0] == "shadow_create":
            shadow_ret = ret_class[1]
    return is_void, shadow_ret


def gen_typed_param(ctype, arg, rtype, rnum, sname):
    """Generate C code line for typed parameter."""
    if ctype == "shadow":
        if sname == "RastPort":
            return f"    struct RastPort *arg_{arg} = resolve_rp(ctx, THUNK_A({rnum}));", False
        macro = f"THUNK_A({rnum})" if rtype == "a" else f"THUNK_D({rnum})"
        return f"    struct {sname} *arg_{arg} = (struct {sname} *)m68k_to_host_or_shadow(ctx, {macro});", False
    if ctype in ("input_struct", "unsupported"):
        return None, False
    if ctype == "bptr":
        macro = f"THUNK_A({rnum})" if rtype == "a" else f"THUNK_D({rnum})"
        return f"    BPTR arg_{arg} = (BPTR){macro};", False
    if ctype == "taglist":
        return f"    struct TagItem *arg_{arg} = m68k_to_native_taglist(ctx, THUNK_A({rnum}));", True
    if ctype == "string":
        macro = f"THUNK_A({rnum})" if rtype == "a" else f"THUNK_D({rnum})"
        return f"    CONST_STRPTR arg_{arg} = (CONST_STRPTR)m68k_to_host(ctx, {macro});", False
    if ctype == "pointer":
        macro = "THUNK_PTR" if rtype == "a" else "THUNK_DPTR"
        return f"    APTR arg_{arg} = {macro}({rnum});", False
    macro = f"THUNK_A({rnum})" if rtype == "a" else f"THUNK_D({rnum})"
    return f"    ULONG arg_{arg} = {macro};", False


def gen_param_line(funcname, arg, reg, ptype):
    """Generate C code line for one parameter."""
    rtype = reg[0]
    rnum = int(reg[1])
    cls = classify_type(ptype) if ptype else None

    if cls:
        sname = cls[1] if len(cls) > 1 else None
        return gen_typed_param(cls[0], arg, rtype, rnum, sname)

    dptr = DPTR_OVERRIDES.get(funcname, set())
    if (rtype == "a") or (reg in dptr):
        if arg.lower() in ("taglist", "tags", "tagitems"):
            return f"    struct TagItem *arg_{arg} = m68k_to_native_taglist(ctx, THUNK_A({rnum}));", True
        macro = "THUNK_PTR" if rtype == "a" else "THUNK_DPTR"
        return f"    APTR arg_{arg} = {macro}({rnum});", False

    return f"    ULONG arg_{arg} = THUNK_D({rnum});", False


def gen_return_block(call, is_void, shadow_ret, cleanup):
    """Generate return and cleanup statements for thunk."""
    lines = []
    if is_void:
        lines.append(f"    {call};")
        lines.extend(cleanup)
        lines.append("    return 0;")
    elif shadow_ret:
        lines.append(f"    void *_ret = (void *){call};")
        lines.extend(cleanup)
        lines.append("    if (!_ret) return 0;")
        lines.append(f'    return shadow_create_by_name(ctx, "{shadow_ret}", _ret);')
    elif cleanup:
        lines.append(f"    IPTR _ret = (IPTR){call};")
        lines.extend(cleanup)
        lines.append("    return _ret;")
    else:
        lines.append(f"    return (IPTR){call};")
    return lines


def gen_thunk(libname, funcname, bias, args, regs, protos):
    """Generate a C thunk function. Returns None if skipped."""
    if funcname in SKIP_FUNCTIONS or not validate_regs(regs):
        return None

    proto = protos.get(funcname)
    is_void, shadow_ret = get_func_properties(funcname, proto)
    proto_params = proto[1] if proto and len(proto) > 1 else None

    sig_parts = ", ".join(f"{a}={r}" for a, r in zip(args, regs))
    lines = [
        f"/* -{bias}: {funcname}({sig_parts}) */",
        f"static IPTR thunk_{libname}_{funcname}(struct KaffeineContext *ctx, void *cpu)",
        "{",
    ]

    call_args = []
    cleanup = []
    fmt_var = None

    for i, (arg, reg) in enumerate(zip(args, regs)):
        ptype = proto_params[i][0] if proto_params and i < len(proto_params) else None
        cls = classify_type(ptype) if ptype else None

        if cls and cls[0] == "rawarg":
            # a m68k RawDoFmt datastream is packed by specifier - repack it natively
            if not fmt_var:
                return None
            macro = f"THUNK_A({reg[1]})" if reg[0] == "a" else f"THUNK_D({reg[1]})"
            lines.append(f"    ULONG _{arg}_size;")
            lines.append(f"    APTR arg_{arg} = m68k_to_native_fmt_stream(ctx, {fmt_var}, {macro}, &_{arg}_size);")
            cleanup.append(f"    if (arg_{arg}) FreeMem(arg_{arg}, _{arg}_size);")
            call_args.append(f"(RAWARG)arg_{arg}")
            continue

        param_code, is_taglist = gen_param_line(funcname, arg, reg, ptype)
        if param_code is None:
            return None
        lines.append(param_code)
        if is_taglist:
            cleanup.append(f"    if (arg_{arg}) FreeVec(arg_{arg});")
        if cls and cls[0] == "string":
            fmt_var = f"arg_{arg}"
        call_args.append(f"arg_{arg}")

    call = f"{funcname}({', '.join(call_args)})"
    lines.extend(gen_return_block(call, is_void, shadow_ret, cleanup))
    lines.append("}\n")
    return "\n".join(lines)


def libname_from_fd(fd_path):
    """Extract library name from FD filename."""
    return os.path.basename(fd_path).replace("_lib.fd", "")


def proto_header(libname):
    """Return #include <proto/libname.h> line."""
    return f"#include <proto/{libname}.h>"


def load_libraries(fd_dir):
    """Load valid library FD files."""
    libraries = {}
    for fd_path in sorted(glob.glob(os.path.join(fd_dir, "*_lib.fd"))):
        libname = libname_from_fd(fd_path)
        if libname not in SKIP_LIBRARIES:
            base_name, funcs = parse_fd(fd_path)
            if funcs:
                libraries[libname] = (base_name, funcs)
    return libraries


def emit_c_headers(libraries, protos):
    """Emit top C headers and stubs."""
    proto_doc = " + NDK clib protos" if protos else ""
    print("/* Auto-generated by kaffeine_thunkgen.py — DO NOT EDIT */")
    print(f"/* Generated from AROS FD files{proto_doc} */\n")
    print("#include <exec/types.h>")
    print("#include <exec/memory.h>")
    print("#include <string.h>\n")
    for libname in sorted(libraries.keys()):
        print(proto_header(libname))
    print("\n#include \"kaffeine_intern.h\"")
    print("#include \"kaffeine_thunks.h\"")
    print("#include \"kaffeine_shadow.h\"\n")
    if protos:
        print("/* shadow_lookup is defined in kaffeine_thunks.c */")
        print("extern void *shadow_lookup(struct KaffeineContext *ctx, ULONG m68k_addr);\n")
        print("/* Shadow-aware RastPort lookup (mirrors kaffeine_thunks.c) */")
        print("static struct RastPort *resolve_rp(struct KaffeineContext *ctx, ULONG m68k_rp)")
        print("{\n    struct RastPort *rp = (struct RastPort *)shadow_lookup(ctx, m68k_rp);")
        print("    if (rp) return rp;\n    return (struct RastPort *)m68k_to_host(ctx, m68k_rp);\n}\n")


def generate_all_thunks(libraries, protos):
    """Generate thunk functions for all libraries."""
    all_tables = {}
    total = 0
    for libname in sorted(libraries.keys()):
        _, funcs = libraries[libname]
        thunks = []
        table = []
        for name, bias, args, regs in funcs:
            code = gen_thunk(libname, name, bias, args, regs, protos)
            if code:
                thunks.append(code)
                table.append((bias, f"thunk_{libname}_{name}"))
        if thunks:
            print(f"/* ── {libname}.library ({len(thunks)} thunks) ── */\n")
            for t in thunks:
                print(t)
            all_tables[libname] = table
            total += len(thunks)
    return all_tables, total


def emit_thunk_tables(all_tables, total):
    """Emit table registrations."""
    print("/* ── Thunk tables ── */\n")
    for libname in sorted(all_tables.keys()):
        table = all_tables[libname]
        print(f"const struct M68KThunkEntry kaffeine_thunks_{libname}_gen[] = {{")
        for bias, func in table:
            print(f"    {{ {bias}, {func} }},")
        print("    { 0, NULL }\n};")
        print(f"const ULONG kaffeine_thunks_{libname}_gen_count = {len(table)};\n")

    print("/* ── Library registration table ── */")
    print("const struct M68KLibThunkSet kaffeine_all_gen_libs[] = {")
    for libname in sorted(all_tables.keys()):
        # a const variable is not a constant expression in C - size the table instead
        print(f'    {{ "{libname}.library", kaffeine_thunks_{libname}_gen, '
              f'sizeof(kaffeine_thunks_{libname}_gen) / sizeof(struct M68KThunkEntry) - 1 }},')
    print("    { NULL, NULL, 0 }\n};")
    print(f"const ULONG kaffeine_all_gen_libs_count = {len(all_tables)};\n")
    print(f"/* Total: {total} auto-generated thunks across {len(all_tables)} libraries */")


AUDIT_MULTI_REG_OR_CCR = {
    "SDivMod32": "Returns quotient in D0 and remainder in D1; generated thunk only sets D0",
    "UDivMod32": "Returns quotient in D0 and remainder in D1; generated thunk only sets D0",
    "SMult64": "Returns 64-bit product in D0:D1; generated thunk only sets D0",
    "UMult64": "Returns 64-bit product in D0:D1; generated thunk only sets D0",
    "GetCC": "Should return m68k CCR flags; native AROS GetCC() is unimplemented (~0)",
}

AUDIT_CALLBACK_FUNCS = {
    "AddTask": "initPC/finalPC are m68k code addresses passed to native AddTask",
    "NewAddTask": "initialPC/finalPC are m68k code addresses passed to native NewAddTask",
    "NewCreateTaskA": "Taglist carries m68k task entry points",
    "NewStackSwap": "function (A1) is an m68k code entry point",
    "ObtainQuickVector": "interruptCode (A0) is an m68k interrupt handler address",
    "SetExcept": "Arms task tc_ExceptCode (m68k code) to be called as a native function",
    "Exit": "Unwinds native AROS process instead of stopping the m68k emulator loop",
    "AVL_FindNode": "func (A2) is an m68k AVLKeyComp callback",
    "AVL_FindPrevNodeByKey": "func (A2) is an m68k AVLKeyComp callback",
    "AVL_FindNextNodeByKey": "func (A2) is an m68k AVLKeyComp callback",
    "CustomCx": "custom_func (A0) is an m68k callback function pointer",
    "ScanVars": "hook (D1) is an m68k struct Hook callback",
    "QBlit": "bltnode (A1) contains an m68k blitter cleanup callback",
    "QBSBlit": "bltnode (A1) contains an m68k blitter cleanup callback",
    "SetCollision": "routine (A0) is an m68k GEL collision callback",
    "DoCollision": "Invokes GEL collision callbacks registered in RastPort",
    "AddBob": "Bob/VSprite structures contain m68k drawing callbacks",
    "AddVSprite": "VSprite structure can contain m68k callbacks",
    "AddAnimOb": "AnimOb/AnimComp structures contain m68k AnimCRoutine callbacks",
    "Animate": "Invokes AnimComp.AnimCRoutine m68k callbacks",
    "SetDisplayDriverCallback": "callback (A0) is a function pointer",
    "DoRenderFunc": "render_func (A3) is a callback function pointer",
    "DoPixelFunc": "render_func (A1) is a callback function pointer",
    "EntryHandler": "handler (A1) is an m68k struct Hook callback",
    "ExitHandler": "handler (A1) is an m68k struct Hook callback",
    "SetLocalItemPurge": "purgeHook (A1) is an m68k struct Hook callback",
    "InitIFF": "streamHook (A1) is an m68k struct Hook callback",
    "ChangeWindowShape": "callback (A2) is an m68k struct Hook callback",
    "CreateUpfrontHookLayer": "hook (A3) is an m68k backfill Hook callback",
    "CreateBehindHookLayer": "hook (A3) is an m68k backfill Hook callback",
    "InstallLayerHook": "hook (A1) is an m68k backfill Hook callback",
    "InstallLayerInfoHook": "hook (A1) is an m68k backfill Hook callback",
    "DoHookClipRects": "hook (A0) is an m68k cliprect Hook callback",
    "ChangeLayerShape": "callback (A2) is an m68k struct Hook callback",
    "CollectPixelsLayer": "callback (A2) is an m68k struct Hook callback",
    "FormatDate": "putCharFunc (A2) is an m68k struct Hook callback",
    "FormatString": "putCharFunc (A2) is an m68k struct Hook callback",
    "ParseDate": "getCharFunc (A2) is an m68k struct Hook callback",
    "CallHookPkt": "hook (A0) h_Entry/h_SubEntry m68k code pointers called natively",
}

AUDIT_QUESTION_FUNCS = {
    "CliInit": "Private Kickstart vector (dosPrivate4 in NDK 3.2); 68k callers expect D0 flags and D1 IoErr",
    "DosGetString": "Private DOS vector (dosPrivate6 in NDK 3.2); internal catalog string lookup",
    "MakeLink": "Polymorphic dest (D2): STRPTR path for soft links, BPTR lock for hard links",
    "ErrorReport": "Polymorphic arg1 (D3): BPTR lock/fh or DeviceList pointer depending on type",
    "SetIPrefs": "Private Intuition vector (intuitionPrivate1 in NDK 3.2); payload layout depends on type",
    "FindClass": "Private Intuition vector (intuitionPrivate2 in NDK 3.2); internal Boopsi class lookup",
    "LocalePrefsUpdate": "Private Locale vector (localePrivate2 in NDK 3.2); internal prefs update",
    "GetCyberMapAttr": "Polymorphic attribute: CYBRMATTR_DISPADR returns host framebuffer pointer",
    "GetDrawInfoAttr": "Polymorphic attribute query writing to storagePtr (A1)",
}

AUDIT_OUT_FUNCS = {
    "DateStamp", "ExamineFH", "Info", "DateToStr", "StrToDate", "ReadItem",
    "Amiga2Date", "ParseDate", "TextExtent", "FontExtent", "AskSoftStyle",
    "QueryOverscan", "GetDefaultPubScreen", "GetDefPrefs", "GetPrefs",
    "GetCurrentBinding", "AskKeyMap", "FilterTagChanges", "MapTags",
    "RefreshTagItemClones", "FilterTagItems", "ApplyTagChanges",
    "PackStructureTags", "UnpackStructureTags", "InitRastPort", "InitBitMap",
    "InitView", "InitVPort", "InitGels", "InitArea", "InitTmpRas", "InitMasks",
    "InitGMasks", "InitRequester", "InitLayers", "NewMinList", "Insert",
    "AddHead", "AddTail", "Remove", "RemHead", "RemTail", "Enqueue",
    "Allocate", "Deallocate", "GetRPAttrsA", "GetDTAttrsA", "GetIconRectangleA",
    "FRead", "FGets", "NameFromLock", "NameFromFH", "SplitName", "Fault",
    "GetCurrentDirName", "GetPrompt", "ParsePattern", "ParsePatternNoCase",
    "AddPart", "GetVar", "Strlcpy", "Strlcat", "VSNPrintf", "StrConvert",
    "IDtoStr", "MapRawKey", "MapANSI", "SetMem", "CopyMemQuick", "BltClear",
    "ReadPixelArray", "ReadPixelLine8", "ReadPixelArray8", "GetRGB32",
    "ReadChunkBytes", "ReadChunkRecords", "AvailFonts", "GetDisplayInfoData",
    "GetScreenData", "ReadLink", "NextPubScreen", "NextObject", "BumpRevision",
    "ChangeToSelectedIconColor", "InvertKeyMap", "CopyBrokerList",
    "GT_GetGadgetAttrsA", "BitMapScale", "GetSprite", "GetExtSpriteA",
    "RequestFile", "AslRequest", "OpenDevice", "DoIO", "SendIO", "WaitIO",
    "PutMsg", "Procure",
}

AUDIT_CREATED_SHADOW_TYPES = {
    "Window", "Screen", "RastPort", "ViewPort", "MsgPort", "IntuiMessage",
    "IORequest", "Task", "Menu", "Region", "ColorMap", "BitMap",
    "DrawInfo", "Layer",
}

AUDIT_UNSYNCED_SHADOW_INPUT = {
    "OpenDevice", "DoIO", "SendIO", "InitRastPort", "InitVPort", "InitBitMap",
}

AUDIT_SHADOW_GEOM_MUTATORS = {
    "MoveWindow", "SizeWindow", "ChangeWindowBox", "ZipWindow",
    "MoveScreen", "ScreenPosition", "MoveLayer", "SizeLayer", "MoveSizeLayer",
}

AUDIT_STRUCT_BASES = {
    "Object", "Class", "IClass", "CxObj", "CxMsg", "DataType", "DataTypeHeader",
    "ToolNode", "Catalog", "Locale", "IFFHandle", "FileRequester", "DiskObject",
    "FontContentsHeader", "TFontContents", "DisplayInfoHandle", "Msg",
}

AUDIT_PTR_RET_TYPES = {
    "APTR", "CONST_APTR", "STRPTR", "CONST_STRPTR", "PLANEPTR",
    "DisplayInfoHandle", "Msg",
}

AUDIT_STR_BUF_TYPES = {
    "RAWARG", "STRPTR", "CONST_STRPTR", "char *", "const char *",
    "CONST char *", "UBYTE *", "CONST UBYTE *", "APTR", "CONST_APTR", "PLANEPTR",
}

AUDIT_FUNCPTR_BASES = {"VOID_FUNC", "AVLKeyComp", "AVLNODECOMP", "HOOKFUNC"}


def parse_fd_full_line(line, bias, public):
    """Parse one FD entry line into a dict."""
    m = re.match(r"(\w+)\(([^)]*)\)\(([^)]*)\)", line)
    if not m:
        return None
    args = [a.strip() for a in m.group(2).split(",") if a.strip()]
    regs = [r.strip().lower() for r in re.split(r"[,/]", m.group(3)) if r.strip()]
    return {"name": m.group(1), "bias": bias, "args": args, "regs": regs, "public": public}


def record_fd_entry(info, by_bias, by_name):
    """Store parsed FD entry in lookup dicts."""
    if not info:
        return
    by_bias[info["bias"]] = info
    if info["public"] and not re.match(r".*Private\d+$", info["name"]):
        by_name[info["name"]] = info


def parse_fd_full(filename):
    """Parse an FD file returning (by_bias, public_by_name)."""
    by_bias, by_name = {}, {}
    if not os.path.isfile(filename or ""):
        return by_bias, by_name
    bias, public = 30, True
    with open(filename, encoding="latin-1") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("*"):
                continue
            if line.startswith("##"):
                _, bias, public = parse_fd_directive(line, None, bias, public)
            else:
                record_fd_entry(parse_fd_full_line(line, bias, public), by_bias, by_name)
                bias += 6
    return by_bias, by_name


def parse_one_c_param(param_str):
    """Parse one C parameter declaration into (type, name)."""
    p = param_str.strip()
    if "(" in p:
        return ("VOID_FUNC", "funcptr")
    pm = re.match(r"^(.*?)\s*(\**)(\w+)$", p)
    if pm:
        return ((pm.group(1).strip() + " " + pm.group(2)).strip(), pm.group(3))
    return (p, "")


def parse_c_param_list(cargs):
    """Parse a comma-separated C parameter list into (type, name) pairs."""
    if not cargs or cargs in {"void", "VOID"}:
        return []
    return [parse_one_c_param(p) for p in cargs.split(",")]


def parse_conf_func_line(line, bias):
    """Parse a function declaration line inside a .conf file."""
    clean = re.sub(r"#.*$", "", line).strip()
    m = re.match(r"^(.*?)\b(\w+)\s*\(([^)]*)\)\s*\(([^)]*)\)$", clean)
    if not m:
        return None
    ret, name, cargs = m.group(1).strip(), m.group(2).strip(), m.group(3).strip()
    regs = [r.strip().lower() for r in re.split(r"[,/]", m.group(4)) if r.strip()]
    return {
        "ret": ret, "name": name, "params": parse_c_param_list(cargs),
        "regs": regs, "bias": bias, "proto_str": f"{ret} {name}({cargs or 'VOID'})",
    }


def step_conf_directive(line, bias):
    """Apply a .bias or .skip directive inside a .conf functionlist."""
    if line.startswith(".bias"):
        return int(line.split()[1])
    if line.startswith(".skip"):
        return bias + 6 * int(line.split()[1])
    return bias


def step_conf_line(line, in_func, bias, by_name, by_bias):
    """Process one stripped line of a .conf file."""
    if line in {"##begin functionlist", "##end functionlist"}:
        return (line == "##begin functionlist"), bias
    if not in_func or line[:1] in {"", "#", "*"}:
        return in_func, bias
    if line.startswith("."):
        return in_func, step_conf_directive(line, bias)
    info = parse_conf_func_line(line, bias)
    if info:
        by_name[info["name"]] = by_bias[bias] = info
    return in_func, bias + 6


def parse_single_conf(path):
    """Parse one AROS .conf file into (by_name, by_bias)."""
    by_name, by_bias = {}, {}
    in_func, bias = False, 30
    with open(path, encoding="latin-1") as f:
        for raw in f:
            in_func, bias = step_conf_line(raw.strip(), in_func, bias, by_name, by_bias)
    return by_name, by_bias


def load_all_confs(repo_root):
    """Load all library .conf files from rom/ and workbench/libs/."""
    confs = {}
    for sub in ("rom/*/*.conf", "workbench/libs/*/*.conf"):
        for path in sorted(glob.glob(os.path.join(repo_root, sub))):
            lib = os.path.basename(path).replace(".conf", "")
            confs[lib] = parse_single_conf(path)
    return confs


def parse_audit_clib_chunk(chunk):
    """Parse a single C prototype chunk with full parameter info for audit."""
    m = re.search(
        r"((?:CONST\s+)?(?:struct\s+\w+\s*\*\s*\*?|[\w]+(?:\s*\*\s*\*?)?)\s*)"
        r"__stdargs\s+(\w+)\s*\(\s*(.*?)\s*\)",
        chunk,
    )
    if not m or "..." in m.group(3):
        return None
    ret, name, pstr = m.group(1).strip(), m.group(2).strip(), m.group(3).strip()
    return {
        "ret": ret, "name": name, "params": parse_c_param_list(pstr),
        "proto_str": f"{ret} {name}({pstr or 'VOID'})",
    }


AUDIT_EXPECTED_LIBS = {
    "asl": 8, "commodities": 29, "cybergraphics": 24, "datatypes": 30,
    "diskfont": 3, "dos": 112, "exec": 79, "expansion": 22, "gadtools": 17,
    "graphics": 167, "icon": 21, "iffparse": 40, "intuition": 127,
    "keymap": 4, "layers": 40, "locale": 25, "utility": 41,
}


def load_audit_clib_file(pfile):
    """Parse one *_protos.h file and return a dict of its prototypes."""
    protos = {}
    if not os.path.isfile(pfile or ""):
        return protos
    with open(pfile, encoding="latin-1") as f:
        text = re.sub(r"/\*.*?\*/|#[^\n]*|\\\n", " ", f.read(), flags=re.DOTALL)
    for chunk in re.sub(r"\s+", " ", text).split(";"):
        info = parse_audit_clib_chunk(chunk)
        if info:
            protos[info["name"]] = info
    return protos


def load_audit_clib(clib_dir):
    """Load all *_protos.h files for audit mode."""
    protos = {}
    if not os.path.isdir(clib_dir or ""):
        return protos
    for pfile in sorted(glob.glob(os.path.join(clib_dir, "*_protos.h"))):
        protos.update(load_audit_clib_file(pfile))
    return protos


def load_thunk_macros(kaffeine_dir):
    """Load KAFFEINE_*_THUNKS macro definitions from rom/kaffeine headers."""
    macros = {}
    for path in sorted(glob.glob(os.path.join(kaffeine_dir, "*.h"))):
        with open(path, encoding="latin-1") as f:
            text = f.read()
        for m in re.finditer(r"#define\s+(KAFFEINE_\w+_THUNKS)\s+((?:[^\n]*\\\n)*[^\n]*)", text):
            entries = re.findall(r"\{\s*(\d+)\s*,\s*(\w+)\s*\}", m.group(2))
            macros[m.group(1)] = [(int(b), fn) for b, fn in entries]
    return macros


def parse_manual_table_body(lib, tbody, macros, overrides):
    """Extract manual LVO overrides from one C table body."""
    for line in tbody.splitlines():
        clean = re.sub(r"/\*.*?\*/", "", line).strip()
        for mname, mentries in macros.items():
            if mname in clean:
                for bias, fn in mentries:
                    overrides.setdefault((lib, bias), fn)
        for b_str, fn in re.findall(r"\{\s*(\d+)\s*,\s*(\w+)\s*\}", clean):
            if int(b_str) != 0:
                overrides.setdefault((lib, int(b_str)), fn)


def load_manual_overrides(kaffeine_dir):
    """Load all manual thunk overrides across rom/kaffeine/*.c."""
    macros = load_thunk_macros(kaffeine_dir)
    tmap = {
        "kaffeine_thunks_exec": "exec",
        "kaffeine_thunks_dos": "dos",
        "kaffeine_thunks_intuition_manual": "intuition",
        "kaffeine_thunks_graphics_manual": "graphics",
        "kaffeine_thunks_utility_manual": "utility",
        "kaffeine_thunks_locale_manual": "locale",
        "kaffeine_thunks_gadtools_manual": "gadtools",
    }
    overrides = {}
    for path in sorted(glob.glob(os.path.join(kaffeine_dir, "*.c"))):
        if os.path.basename(path) == "kaffeine_thunks_gen.c":
            continue
        with open(path, encoding="latin-1") as f:
            text = f.read()
        for tname, lib in tmap.items():
            tm = re.search(rf"struct\s+M68KThunkEntry\s+{tname}\s*\[\]\s*=\s*\{{(.*?)\}};", text, re.S)
            if tm:
                parse_manual_table_body(lib, tm.group(1), macros, overrides)
    return overrides


def parse_generated_c_file(gen_file):
    """Parse thunk bodies and table registrations from a generated C file."""
    if not os.path.isfile(gen_file or ""):
        return []
    with open(gen_file, encoding="latin-1") as f:
        text = f.read()
    bodies = {}
    pat = (r"/\*\s*-(\d+):\s*(\w+)\(([^)]*)\)\s*\*/\s*"
           r"static\s+IPTR\s+(thunk_(\w+?)_\2)\s*\([^)]*\)\s*\{(.*?)\n\}")
    for m in re.finditer(pat, text, re.S):
        bias, func, sig, tname, lib, body = int(m.group(1)), m.group(2), m.group(3), m.group(4), m.group(5), m.group(6)
        if (lib, bias) in bodies:
            return []
        bodies[(lib, bias)] = {"func": func, "sig": sig.strip(), "tname": tname, "body": body}
    counts = {
        m.group(1): int(m.group(2))
        for m in re.finditer(r"const\s+ULONG\s+kaffeine_thunks_(\w+)_gen_count\s*=\s*(\d+)\s*;", text)
    }
    rows, lib_counts, seen = [], {}, set()
    tpat = r"const\s+struct\s+M68KThunkEntry\s+kaffeine_thunks_(\w+)_gen\[\]\s*=\s*\{(.*?)\};"
    for tm in re.finditer(tpat, text, re.S):
        lib = tm.group(1)
        entries = [
            (int(b), tn) for b, tn in re.findall(r"\{\s*(\d+)\s*,\s*(thunk_\w+)\s*\}", tm.group(2))
            if int(b) != 0
        ]
        lib_counts[lib] = len(entries)
        for bias, tname in entries:
            info = bodies.get((lib, bias))
            if not info or info.get("tname") != tname or (lib, bias) in seen:
                return []
            seen.add((lib, bias))
            rows.append((lib, bias, tname, info))
    if len(bodies) != text.count("static IPTR thunk_") or seen != set(bodies):
        return []
    if any(lib_counts.get(k, -1) != counts.get(k, -2) for k in set(lib_counts) | set(counts)):
        return []
    return rows


def is_public_ndk_entry(nfd):
    """Return True if NDK FD entry is a public non-private slot."""
    if not nfd or not nfd["public"]:
        return False
    return not re.match(r".*Private\d+$", nfd["name"])


def check_ndk_fd_mismatch(func, bias, afd, ndk_by_bias, ndk_by_name, cats, notes):
    """Compare AROS FD entry against NDK 3.2 FD entry for category W."""
    nfd = ndk_by_bias.get(bias)
    nfd_name = ndk_by_name.get(func)
    if is_public_ndk_entry(nfd):
        if nfd["name"] != func:
            cats.add("W")
            notes.append(f"NDK 3.2 FD has {nfd['name']}({','.join(nfd['regs'])}) at -{bias}")
        elif afd and afd["regs"] != nfd["regs"]:
            cats.add("W")
            notes.append(f"AROS FD regs ({','.join(afd['regs'])}) != NDK 3.2 FD ({','.join(nfd['regs'])})")
    elif nfd_name and nfd_name["bias"] != bias:
        cats.add("W")
        notes.append(f"NDK 3.2 FD places {func} at -{nfd_name['bias']}, not -{bias}")


def check_single_ret_type(rt, cats):
    """Add P or R category for a single return type string."""
    clean = re.sub(r"\b(CONST|const)\b", "", rt or "").strip()
    if clean in {"BPTR", "BSTR"}:
        cats.add("P")
    elif "*" in clean or clean in AUDIT_PTR_RET_TYPES:
        cats.add("R")


def classify_audit_return(func, ret_type, conf_ret, body, cats, notes):
    """Classify return type defects (P, R, O) for a generated thunk."""
    check_single_ret_type(ret_type, cats)
    check_single_ret_type(conf_ret, cats)
    if func in {"ChildWait", "OpenWorkBench"}:
        cats.add("R")
    if func == "Cli":
        cats.add("P")
    actual_ret = (conf_ret or ret_type or "VOID").strip()
    if "return 0;" in body and "if (!_ret)" not in body and actual_ret.lower() not in {"void", "void *"}:
        cats.add("O")
        notes.append(f"VOID_FUNCTIONS bug: thunk discards {actual_ret} return value and returns 0")


def classify_struct_or_ptr_param(clean, base, is_const, cats):
    """Classify a struct or pointer parameter type."""
    if "struct Hook" in clean:
        cats.update(("C", "T"))
    elif clean.startswith("struct ") or base in AUDIT_STRUCT_BASES:
        cats.add("T")
        if "**" in clean:
            cats.add("O")
    elif "*" in clean:
        cats.add("B")
        cats.add("T" if is_const else "O")


def classify_single_param_type(pt, is_const, cats):
    """Classify a single parameter C type into audit categories."""
    clean = re.sub(r"\b(CONST|const)\b", "", pt).strip()
    base = clean.rstrip(" *")
    if base in {"BPTR", "BSTR"}:
        cats.add("P")
    elif base in AUDIT_FUNCPTR_BASES:
        cats.add("C")
    elif clean in AUDIT_STR_BUF_TYPES or base in AUDIT_STR_BUF_TYPES:
        cats.add("B")
    else:
        classify_struct_or_ptr_param(clean, base, is_const, cats)


def apply_body_and_func_param_rules(func, body, cats):
    """Apply function-level and thunk-body parameter classification rules."""
    if func in AUDIT_OUT_FUNCS:
        cats.add("O")
    if func in {"MakeLink", "ErrorReport"} or "(BPTR)" in body:
        cats.add("P")
    if any(k in body for k in ("m68k_to_native_taglist", "m68k_to_host_or_shadow", "resolve_rp")):
        cats.add("T")
    if not any(k in body for k in ("m68k_to_host(", "THUNK_PTR(", "THUNK_DPTR(")):
        cats.discard("B")


def classify_audit_params(func, params, conf_params, body, cats):
    """Classify all parameters of a generated thunk."""
    count = max(len(params), len(conf_params))
    for i in range(count):
        pt1 = params[i][0] if i < len(params) else ""
        pt2 = conf_params[i][0] if i < len(conf_params) else ""
        is_const = bool(re.search(r"\b(CONST|const)\b", pt1 + " " + pt2))
        for pt in (pt1, pt2):
            if pt:
                classify_single_param_type(pt, is_const, cats)
    apply_body_and_func_param_rules(func, body, cats)


def check_arg_line_shadow(line):
    """Return 'shadow' for created shadow lookup, 'str' for string, or 'raw'."""
    m = re.match(
        r"struct\s+(\w+)\s*\*\s*\w+\s*=\s*(?:\(struct\s+\w+\s*\*\))?"
        r"(?:m68k_to_host_or_shadow|resolve_rp)",
        line,
    )
    if m:
        return "shadow" if m.group(1) in AUDIT_CREATED_SHADOW_TYPES else "raw"
    if "m68k_to_native_taglist" in line:
        return "raw"
    if any(k in line for k in ("THUNK_PTR", "THUNK_DPTR", "m68k_to_host(")):
        return "str" if line.startswith("CONST_STRPTR ") else "raw"
    return "scalar"


def is_shadow_resolved_t(func, body):
    """Return True if all struct arguments are resolved via active shadow tables."""
    if func in AUDIT_UNSYNCED_SHADOW_INPUT:
        return False
    arg_lines = [l.strip() for l in body.splitlines() if "=" in l and ("arg_" in l or "native =" in l)]
    kinds = [check_arg_line_shadow(l) for l in arg_lines]
    return ("shadow" in kinds) and ("raw" not in kinds)


def determine_shadow_t_risk(func, cats, body):
    """Determine risk for a thunk whose struct arguments are shadow-resolved."""
    unshadowed_r = ("R" in cats) and ("shadow_create_by_name" not in body and func != "CheckIO")
    if (cats & {"C", "B"}) or unshadowed_r:
        return "crash or memory corruption"
    if (cats & {"O", "W", "?", "R"}) or func in AUDIT_SHADOW_GEOM_MUTATORS:
        return "wrong result"
    return "harmless on the current target only by accident"


def determine_audit_risk(func, cats, body, shadow_t):
    """Determine the risk level string for an audit row."""
    if "X" in cats:
        return "overridden"
    if cats == {"S"}:
        return "none"
    if shadow_t:
        return determine_shadow_t_risk(func, cats, body)
    if cats & {"C", "T", "B"}:
        return "crash or memory corruption"
    if (cats & {"W", "O"}) or ("shadow_create_by_name" in body and cats <= {"R", "X"}):
        return "wrong result"
    if "P" in cats and "R" not in cats:
        return "harmless on the current target only by accident"
    return "crash or memory corruption"


def append_t_note(func, cats, body, shadow_t, notes):
    """Append the structure/taglist translation note for category T."""
    if "m68k_to_native_taglist" in body:
        notes.append("Taglist heuristic corrupts scalar ti_Data in 0x100..mem_size")
    elif shadow_t:
        msg = "Shadow-resolved struct pointer (m68k_to_host_or_shadow/resolve_rp)"
        if func in AUDIT_SHADOW_GEOM_MUTATORS:
            msg += "; native position/size changes not synced back to 68k shadow"
        notes.append(msg)
    elif "T" in cats:
        notes.append("Raw 68k struct layout/endianness not converted for native AROS")


def build_default_note(func, cats, body, overridden_by, existing_notes, shadow_t):
    """Build a concise explanation note for an audit row."""
    if cats == {"S"}:
        return "Scalar arguments and scalar/void return only"
    notes = list(existing_notes)
    append_t_note(func, cats, body, shadow_t, notes)
    fallback = (
        ("B", "Guest buffer/string pointer passed via m68k_to_host without bounds check"),
        ("R", "Returns 64-bit native pointer truncated to 32 bits in guest D0"),
        ("P", "Passes/returns raw 64-bit AROS BPTR without BADDR shift or 32-bit check"),
    )
    for cat, msg in fallback:
        if cat in cats and not notes:
            notes.append(msg)
    if overridden_by and "wrong LVO" not in overridden_by:
        notes.append(f"Overridden at runtime by {overridden_by}")
    return "; ".join(notes)


def apply_special_func_tables(func, cats, notes):
    """Apply table-driven O, C, and ? categories for special functions."""
    for table, cat in (
        (AUDIT_MULTI_REG_OR_CCR, "O"),
        (AUDIT_CALLBACK_FUNCS, "C"),
        (AUDIT_QUESTION_FUNCS, "?"),
    ):
        if func in table:
            cats.add(cat)
            notes.append(table[func])


def classify_one_entry(lib, bias, info, confs, clib_protos, aros_fds, ndk_fds, overrides):
    """Classify one generated thunk entry and return its 9 CSV fields."""
    func, body, sig = info.get("func", ""), info.get("body", ""), info.get("sig", "")
    c_by_name, c_by_bias = confs.get(lib, ({}, {}))
    fconf = c_by_name.get(func) or c_by_bias.get(bias) or {}
    proto = clib_protos.get(func) or fconf

    cats, notes = set(), []
    afd = aros_fds.get(lib, ({}, {}))[0].get(bias)
    ndk_by_bias, ndk_by_name = ndk_fds.get(lib, ({}, {}))
    check_ndk_fd_mismatch(func, bias, afd, ndk_by_bias, ndk_by_name, cats, notes)
    apply_special_func_tables(func, cats, notes)
    classify_audit_return(func, proto.get("ret", ""), fconf.get("ret", ""), body, cats, notes)
    classify_audit_params(func, proto.get("params", []), fconf.get("params", []), body, cats)

    overridden_by = ""
    if (lib, bias) in overrides:
        cats.add("X")
        overridden_by = overrides[(lib, bias)]
    if not cats:
        cats.add("S")

    shadow_t = ("T" in cats) and is_shadow_resolved_t(func, body)
    order = ["S", "B", "T", "R", "O", "C", "P", "W", "X", "?"]
    cat_str = ",".join(c for c in order if c in cats)
    risk = determine_audit_risk(func, cats, body, shadow_t)
    note = build_default_note(func, cats, body, overridden_by, notes, shadow_t)
    return [lib, f"-{bias}", func, sig, proto.get("proto_str", ""), cat_str, risk, overridden_by, note]


def resolve_default_ndk_fd(clib_dir):
    """Infer NDK fd directory from NDK clib directory when adjacent."""
    if not clib_dir:
        return None
    base = os.path.dirname(os.path.dirname(os.path.abspath(clib_dir)))
    candidate = os.path.join(base, "ndk", "lib", "fd")
    return candidate if os.path.isdir(candidate) else None


def parse_audit_args(argv, script_dir):
    """Parse command-line arguments for --audit mode."""
    p = argparse.ArgumentParser(add_help=False)
    p.add_argument("--audit", action="store_true")
    p.add_argument("--ndk-fd", default=None)
    p.add_argument("--gen-file", default=os.path.join(script_dir, "kaffeine_thunks_gen.c"))
    p.add_argument("positional", nargs="*")
    args = p.parse_args(argv)
    dirs = [a for a in args.positional if not a.endswith(".c")]
    cfiles = [a for a in args.positional if a.endswith(".c")]
    gen_file = cfiles[0] if cfiles else args.gen_file
    fd_dir = dirs[0] if len(dirs) > 0 else None
    clib_dir = dirs[1] if len(dirs) > 1 else None
    ndk_fd = args.ndk_fd or (dirs[2] if len(dirs) > 2 else resolve_default_ndk_fd(clib_dir))
    return fd_dir, clib_dir, ndk_fd, gen_file


def load_fd_map(fd_dir, libs):
    """Load FD dictionaries for each library in libs."""
    if not os.path.isdir(fd_dir or ""):
        return {}
    return {lib: parse_fd_full(os.path.join(fd_dir, f"{lib}_lib.fd")) for lib in libs}


def check_required_audit_files(fd_dir, ndk_fd_dir, clib_dir, libs):
    """Return list of missing or empty FD and clib files across input directories."""
    missing = []
    for lib in sorted(libs):
        if not parse_fd_full(os.path.join(fd_dir or "", f"{lib}_lib.fd"))[0]:
            missing.append(f"AROS FD: {lib}_lib.fd")
        if lib != "cybergraphics":
            if not parse_fd_full(os.path.join(ndk_fd_dir or "", f"{lib}_lib.fd"))[0]:
                missing.append(f"NDK FD: {lib}_lib.fd")
            if not load_audit_clib_file(os.path.join(clib_dir or "", f"{lib}_protos.h")):
                missing.append(f"NDK clib: {lib}_protos.h")
    return missing


def validate_audit_inputs(fd_dir, clib_dir, ndk_fd_dir, gen_file, rows):
    """Ensure generated C file, AROS FD, NDK FD, and NDK clib inputs are valid."""
    lib_counts = {}
    for r in rows:
        lib_counts[r[0]] = lib_counts.get(r[0], 0) + 1
    bad_libs = [
        lib for lib, exp in sorted(AUDIT_EXPECTED_LIBS.items())
        if lib_counts.get(lib, 0) < exp
    ]
    if not rows or bad_libs or any(not r[3].get("body") for r in rows):
        print(
            f"Error: invalid or incomplete generated file '{gen_file}' "
            f"(incomplete libraries: {', '.join(bad_libs) or 'unparsed bodies'})",
            file=sys.stderr,
        )
        sys.exit(1)
    if not (os.path.isdir(fd_dir or "") and os.path.isdir(clib_dir or "") and os.path.isdir(ndk_fd_dir or "")):
        print(
            "Usage: kaffeine_thunkgen.py --audit <aros_fd_dir> <ndk_clib_dir> "
            "[--ndk-fd <ndk_fd_dir>] [--gen-file <file.c>]",
            file=sys.stderr,
        )
        sys.exit(1)
    missing = check_required_audit_files(fd_dir, ndk_fd_dir, clib_dir, lib_counts.keys())
    if missing:
        print(f"Error: missing required audit inputs ({', '.join(missing[:5])})", file=sys.stderr)
        sys.exit(1)


def run_audit(argv, script_dir):
    """Run --audit mode and write CSV to stdout."""
    fd_dir, clib_dir, ndk_fd_dir, gen_file = parse_audit_args(argv, script_dir)
    rows = parse_generated_c_file(gen_file)
    validate_audit_inputs(fd_dir, clib_dir, ndk_fd_dir, gen_file, rows)
    repo_root = os.path.abspath(os.path.join(script_dir, "..", ".."))
    confs = load_all_confs(repo_root)
    clib_protos = load_audit_clib(clib_dir)
    aros_fds = load_fd_map(fd_dir, confs.keys())
    ndk_fds = load_fd_map(ndk_fd_dir, confs.keys())
    overrides = load_manual_overrides(script_dir)

    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow([
        "library", "LVO", "function", "fd registers", "clib prototype",
        "categories", "risk", "overridden by", "note",
    ])
    for lib, bias, _, info in rows:
        writer.writerow(
            classify_one_entry(lib, bias, info, confs, clib_protos, aros_fds, ndk_fds, overrides)
        )


def main():
    """Main entry point."""
    if "--audit" in sys.argv:
        run_audit(sys.argv[1:], os.path.dirname(os.path.abspath(__file__)))
        return

    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <fd_directory> [<ndk_clib_directory>]", file=sys.stderr)
        sys.exit(1)

    fd_dir = sys.argv[1]
    clib_dir = sys.argv[2] if len(sys.argv) > 2 else None

    protos = parse_clib_protos(clib_dir)
    if protos:
        print(f"/* Parsed {len(protos)} prototypes from {clib_dir} */", file=sys.stderr)

    libraries = load_libraries(fd_dir)
    emit_c_headers(libraries, protos)
    all_tables, total = generate_all_thunks(libraries, protos)
    emit_thunk_tables(all_tables, total)


if __name__ == "__main__":
    main()

