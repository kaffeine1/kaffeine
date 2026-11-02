/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Author: Michele Dipace (@kaffeine1)
 *
 * Preview-only desktop preferences and launch icons. Run before IPrefs.
 * "status" observes the actual public screen after Wanderer has opened it.
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <graphics/gfx.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <workbench/workbench.h>
#include <utility/tagitem.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/icon.h>
#include <proto/intuition.h>
#include <string.h>

static void be16(UBYTE *out, UWORD value)
{
    out[0] = value >> 8;
    out[1] = value;
}

static void be32(UBYTE *out, ULONG value)
{
    out[0] = value >> 24;
    out[1] = value >> 16;
    out[2] = value >> 8;
    out[3] = value;
}

static BOOL screen_preferences(void)
{
    struct TagItem tags[] = {
        { BIDTAG_NominalWidth, 1024 },
        { BIDTAG_NominalHeight, 768 },
        { BIDTAG_DesiredWidth, 1024 },
        { BIDTAG_DesiredHeight, 768 },
        { BIDTAG_Depth, 32 },
        { TAG_DONE, 0 }
    };
    ULONG mode = BestModeIDA(tags);
    UBYTE prefs[62] = { 0 };
    BPTR file;
    BOOL ok;

    if (mode == (ULONG)INVALID_ID) {
        PutStr("PreviewSetup: no 32-bit desktop mode is available.\n");
        return FALSE;
    }
    /* IFF FORM/PREF: packed six-byte PRHD and 28-byte SCRM, big endian. */
    memcpy(prefs, "FORM", 4);
    be32(prefs + 4, sizeof(prefs) - 8);
    memcpy(prefs + 8, "PREFPRHD", 8);
    be32(prefs + 16, 6);
    memcpy(prefs + 26, "SCRM", 4);
    be32(prefs + 30, 28);
    be32(prefs + 50, mode);
    be16(prefs + 54, 1024);
    be16(prefs + 56, 768);
    be16(prefs + 58, 32);
    file = Open("ENV:SYS/screenmode.prefs", MODE_NEWFILE);
    if (file == BNULL)
        return FALSE;
    ok = Write(file, prefs, sizeof(prefs)) == sizeof(prefs);
    if (!Close(file))
        ok = FALSE;
    if (ok)
        Printf("PreviewSetup: requested 1024x768x32, mode 0x%08lx.\n", mode);
    return ok;
}

static BOOL make_icon(CONST_STRPTR name, LONG type, STRPTR tool)
{
    struct DiskObject *icon = GetDefDiskObject(type);
    STRPTR saved_tool;
    STRPTR *saved_types;
    STRPTR types[] = { (STRPTR)"CLI", NULL };
    BOOL ok;

    if (icon == NULL)
        return FALSE;
    saved_tool = icon->do_DefaultTool;
    saved_types = icon->do_ToolTypes;
    if (tool != NULL) {
        icon->do_DefaultTool = tool;
        icon->do_ToolTypes = types;
        icon->do_StackSize = 1048576;
    }
    icon->do_CurrentX = NO_ICON_POSITION;
    icon->do_CurrentY = NO_ICON_POSITION;
    ok = PutDiskObject(name, icon);
    /* FreeDiskObject owns the original arrays, not our static strings. */
    icon->do_DefaultTool = saved_tool;
    icon->do_ToolTypes = saved_types;
    FreeDiskObject(icon);
    return ok;
}

static int screen_status(void)
{
    struct Screen *screen = LockPubScreen(NULL);
    ULONG depth, width, height, mode;
    if (screen == NULL) {
        PutStr("PreviewSetup: no public screen.\n");
        return RETURN_FAIL;
    }
    width = screen->Width;
    height = screen->Height;
    depth = GetBitMapAttr(screen->RastPort.BitMap, BMA_DEPTH);
    mode = GetVPModeID(&screen->ViewPort);
    UnlockPubScreen(NULL, screen);
    Printf("Preview desktop: %lux%lux%lu, mode 0x%08lx.\n",
           width, height, depth, mode);
    return width >= 1024 && height >= 768 && depth >= 24
        ? RETURN_OK : RETURN_FAIL;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "status") == 0)
        return screen_status();
    if (argc != 1) {
        PutStr("Usage: PreviewSetup [status]\n");
        return RETURN_FAIL;
    }
    if (!screen_preferences()
        || !make_icon("SYS:KaffeinePreview", WBDRAWER, NULL)
        || !make_icon("SYS:KaffeinePreview/Applications", WBDRAWER, NULL)
        || !make_icon("SYS:KaffeinePreview/Results", WBDRAWER, NULL)
        || !make_icon("SYS:KaffeinePreview/Test-JIT", WBPROJECT, (STRPTR)"C:Execute")
        || !make_icon("SYS:KaffeinePreview/Test-Moira", WBPROJECT, (STRPTR)"C:Execute")
        || !make_icon("SYS:KaffeinePreview/ReadMe", WBPROJECT, (STRPTR)"C:Type")) {
        PutStr("PreviewSetup: desktop preparation failed.\n");
        return RETURN_FAIL;
    }
    PutStr("PreviewSetup: desktop preferences and launch icons prepared.\n");
    return RETURN_OK;
}
