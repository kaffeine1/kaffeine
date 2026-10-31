/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Keys typed into a GadTools string gadget. A window opened active, with a
    context and an empty string gadget in WA_Gadgets, is waited for until
    IDCMP_ACTIVEWINDOW comes; ActivateGadget() of the string returns TRUE.
    Then the program writes SYS:EMU68/NAME-keys-N.txt, NAME being the name
    that it runs under and N 1 from the Shell or, once the runner's
    S:User-Startup wrote NAME-shell-rc.txt, 2 through Run, so that
    run-raspi-qemu.py --keys types "abc" and Return on the USB keyboard; on
    AmigaOS a script types them. Within 60 seconds the IDCMP_GADGETUP of the string comes,
    with the string gadget as IAddress and Code 0, which Return gives, and
    the string holds "abc" with NumChars 3, which GT_GetGadgetAttrs() gives
    too; its GadgetID and UserData are the program's.
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include <string.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
struct Library *GadToolsBase;

#define ID_STRING 7
#define USERDATA 0x5EED1234UL

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, 0 };

/* SYS:EMU68/NAME followed by tail, NAME the program's name without its path */
static BOOL emu68_file(char *path, LONG size, const char *tail)
{
    char program[64];

    if (!GetProgramName((STRPTR)program, sizeof(program)))
        return FALSE;
    if (strlen("SYS:EMU68/") + strlen((const char *)FilePart((CONST_STRPTR)program)) +
        strlen(tail) >= (ULONG)size)
        return FALSE;
    strcpy(path, "SYS:EMU68/");
    strcat(path, (const char *)FilePart((CONST_STRPTR)program));
    strcat(path, tail);
    return TRUE;
}

/* The marker of this execution: 2 once the Shell execution has ended */
static BOOL write_marker(void)
{
    char path[96];
    BPTR file;
    BOOL second = FALSE;

    if (emu68_file(path, sizeof(path), "-shell-rc.txt") &&
        (file = Open((CONST_STRPTR)path, MODE_OLDFILE)))
    {
        Close(file);
        second = TRUE;
    }
    if (!emu68_file(path, sizeof(path), second ? "-keys-2.txt" : "-keys-1.txt") ||
        !(file = Open((CONST_STRPTR)path, MODE_NEWFILE)))
        return FALSE;
    Write(file, (APTR)"ready\n", 6);
    Close(file);
    return TRUE;
}

/* The messages of win until one of class cls, at most ticks times 1/50 s */
static struct IntuiMessage *wait_for(struct Window *win, ULONG cls, int ticks,
                                     struct IntuiMessage *copy)
{
    struct IntuiMessage *msg;
    BOOL got = FALSE;
    int i;

    for (i = 0; i < ticks && !got; i += 5)
    {
        while ((msg = GT_GetIMsg(win->UserPort)))
        {
            if (msg->Class == cls && !got)
            {
                *copy = *msg;
                got = TRUE;
            }
            GT_ReplyIMsg(msg);
        }
        if (!got)
            Delay(5);
    }
    return got ? copy : NULL;
}

static void typed(struct Gadget *string, struct Window *win)
{
    struct IntuiMessage up, *got;
    struct StringInfo *si = (struct StringInfo *)string->SpecialInfo;
    ULONG val = 0;

    got = wait_for(win, IDCMP_GADGETUP, 60 * 50, &up);
    if (!check(got != NULL, "IDCMP_GADGETUP of the string comes within 60 s"))
        return;
    check(up.IAddress == (APTR)string, "its IAddress is the string gadget");
    check_long("its Code, which Return gives", up.Code, 0);
    if (check(si != NULL, "the string has its StringInfo"))
    {
        check_str("the string holds", (const char *)si->Buffer, "abc");
        check_long("its NumChars", si->NumChars, 3);
    }
    check_long("GT_GetGadgetAttrs(GTST_String) fills it in",
               GT_GetGadgetAttrs(string, win, NULL, GTST_String, (ULONG)&val, TAG_DONE), 1);
    check_str("GT_GetGadgetAttrs(GTST_String) gives", (const char *)val, "abc");
    check(string->GadgetID == ID_STRING && (ULONG)string->UserData == USERDATA,
          "its GadgetID and UserData are the program's");
}

int main(void)
{
    struct Gadget *glist = NULL, *context, *string = NULL;
    struct Screen *screen = NULL;
    struct Window *win = NULL;
    struct IntuiMessage active;
    struct NewGadget ng;
    APTR vi = NULL;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 39);
    if (!check(IntuitionBase && GadToolsBase, "intuition.library and gadtools.library 39"))
        goto done;
    screen = LockPubScreen(NULL);
    vi = screen ? GetVisualInfo(screen, TAG_DONE) : NULL;
    if (!check(screen && vi, "LockPubScreen() and GetVisualInfo()"))
        goto done;

    context = CreateContext(&glist);
    memset(&ng, 0, sizeof(ng));
    ng.ng_LeftEdge = 40;
    ng.ng_TopEdge = 40;
    ng.ng_Width = 200;
    ng.ng_Height = 14;
    ng.ng_TextAttr = &topaz8;
    ng.ng_GadgetID = ID_STRING;
    ng.ng_VisualInfo = vi;
    ng.ng_UserData = (APTR)USERDATA;
    string = CreateGadget(STRING_KIND, context, &ng, GTST_MaxChars, 20, TAG_DONE);
    if (!check(context && string, "CreateContext() and an empty STRING_KIND gadget"))
        goto done;

    win = OpenWindowTags(NULL,
                         WA_Left, 30, WA_Top, 30, WA_Width, 300, WA_Height, 100,
                         WA_Title, (ULONG)"gfx_gadget_keys",
                         WA_Gadgets, (ULONG)glist,
                         WA_Flags, WFLG_SMART_REFRESH | WFLG_DRAGBAR | WFLG_DEPTHGADGET,
                         WA_Activate, TRUE,
                         WA_IDCMP, IDCMP_ACTIVEWINDOW | STRINGIDCMP,
                         TAG_DONE);
    if (!check(win != NULL, "OpenWindowTags() with WA_Gadgets and WA_Activate"))
        goto done;
    if (!check(wait_for(win, IDCMP_ACTIVEWINDOW, 10 * 50, &active) != NULL,
               "IDCMP_ACTIVEWINDOW comes"))
        goto done;
    if (!check(ActivateGadget(string, win, NULL) != FALSE,
               "ActivateGadget() of the string gives TRUE"))
        goto done;
    write_marker();
    typed(string, win);

done:
    if (win)
        CloseWindow(win);
    if (GadToolsBase)
        FreeGadgets(glist);
    if (vi)
        FreeVisualInfo(vi);
    if (screen)
        UnlockPubScreen(NULL, screen);
    if (GadToolsBase)
        CloseLibrary(GadToolsBase);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_gadget_keys");
}
