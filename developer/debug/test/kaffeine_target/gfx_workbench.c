/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    workbench.library AppWindow, AppMenuItem and AppIcon registration,
    removal and AppMessages on a MsgPort of the program's own.

    The AppIcon has a DiskObject as the autodoc of AddAppIconA() builds it.

    Modes from argv[1] (no argument means "register"):
      register   register all three App objects on one port, and four more
                 AppIcons whose DiskObjects and images differ, verify that
                 AddAppIconA() gives NULL for seven DiskObjects that
                 AmigaOS 3.2.3 does not take and that no message is queued
                 right after registration, and remove them in order (the
                 AppIcons, AppMenuItem, AppWindow);
      appicon    register as "register" does, write the SYS:EMU68 mouse
                 marker and wait up to 60 s for AMTYPE_APPICON from
                 double-clicking the icon at 500,40;
      appwindow  after registration, write the SYS:EMU68 mouse marker and
                 wait up to 60 s for the signal of an AppMessage from
                 dropping an icon onto the window at 40,40, remove the
                 AppWindow while the message waits in the port, then take
                 it, AMTYPE_APPWINDOW, and check am_ArgList[0];
      appmenu    after registration, write the SYS:EMU68 mouse marker and
                 wait up to 60 s for AMTYPE_APPMENUITEM from selecting the
                 Tools menu item.
*/

#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <workbench/workbench.h>
#include <workbench/startup.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/wb.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
struct Library *WorkbenchBase;

/* The 68k layouts that programs read and pass to workbench.library. */
typedef char check_wbarg[sizeof(struct WBArg) == 8 ? 1 : -1];
typedef char check_appmsg[sizeof(struct AppMessage) == 86 ? 1 : -1];
typedef char check_diskobj[sizeof(struct DiskObject) == 78 ? 1 : -1];

#define WIN_LEFT     40
#define WIN_TOP      40
#define WIN_WIDTH    300
#define WIN_HEIGHT   120

#define ICON_X       500
#define ICON_Y       40
#define ICON_WIDTH   32
#define ICON_HEIGHT  16
#define ICON_DEPTH   2
#define ICON_ROW_W   (ICON_WIDTH / 16)
#define ICON_PLANE_W (ICON_ROW_W * ICON_HEIGHT)
#define ICON_WORDS   (ICON_PLANE_W * ICON_DEPTH)
/* the image data of all the AppIcons in one block: the two planes of the
   render image, the two of the select image, and at the end the one plane
   of the image whose PlanePick takes one plane of its two */
#define SELECT_DATA  ICON_WORDS
#define ONE_PLANE    (2 * ICON_WORDS)
#define DATA_BYTES   ((2 * ICON_WORDS + ICON_PLANE_W) * sizeof(UWORD))

#define EXTRA_ICONS  4
#define EXTRA_X      380
#define EXTRA_STEP   70
#define EXTRA_Y      110

#define APPWIN_ID    1UL
#define APPWIN_USER  0x1234UL
#define APPMENU_ID   2UL
#define APPMENU_USER 0x5678UL
#define APPICON_ID   3UL
#define APPICON_USER 0x9ABCUL

enum Mode
{
    MODE_REGISTER,
    MODE_APPICON,
    MODE_APPWINDOW,
    MODE_APPMENU
};

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
    if (!emu68_file(path, sizeof(path), second ? "-mouse-2.txt" : "-mouse-1.txt") ||
        !(file = Open((CONST_STRPTR)path, MODE_NEWFILE)))
        return FALSE;
    Write(file, (APTR)"ready\n", 6);
    Close(file);
    return TRUE;
}

/* An Image of the AppIcons' size */
static void init_image(struct Image *img, WORD depth, UWORD *data, UBYTE pick, UBYTE onoff)
{
    memset(img, 0, sizeof(*img));
    img->Width = ICON_WIDTH;
    img->Height = ICON_HEIGHT;
    img->Depth = depth;
    img->ImageData = data;
    img->PlanePick = pick;
    img->PlaneOnOff = onoff;
}

/* Fill the two 32x16 bitplanes in chip memory with a framed pattern and
   initialise the Image and the DiskObject as the autodoc of AddAppIconA()
   has them: no magic, no version, no GFLG_GADGIMAGE, and GadgetRender an
   Image all the same. */
static void init_diskobject(struct DiskObject *dobj, struct Image *img, UWORD *data)
{
    int row;

    for (row = 0; row < ICON_HEIGHT; row++)
    {
        BOOL border = (row == 0 || row == ICON_HEIGHT - 1);

        /* Plane 0: solid frame with an alternating interior */
        data[row * 2] = border ? 0xFFFF : 0xAAAA;
        data[row * 2 + 1] = border ? 0xFFFF : 0x5555;
        /* Plane 1: filled inner box */
        data[ICON_PLANE_W + row * 2] = border ? 0x0000 : 0x7FFE;
        data[ICON_PLANE_W + row * 2 + 1] = border ? 0x0000 : 0x7FFE;
    }
    init_image(img, ICON_DEPTH, data, 3, 0);

    memset(dobj, 0, sizeof(*dobj));
    dobj->do_Gadget.Width = ICON_WIDTH;
    dobj->do_Gadget.Height = ICON_HEIGHT;
    dobj->do_Gadget.GadgetRender = img;
    dobj->do_CurrentX = ICON_X;
    dobj->do_CurrentY = ICON_Y;
}

/* Four more AppIcons, from EXTRA_X on at EXTRA_Y: a DiskObject as an icon
   file has it (the magic, GFLG_GADGIMAGE and a select image), one with
   GFLG_GADGHNONE and a select image, an image of Depth 1 whose plane goes
   to plane 1 (PlanePick 2), and an image of Depth 2 with the one plane of
   data that PlanePick 1 takes, at the end of the data block */
struct Extra
{
    struct DiskObject dobj[EXTRA_ICONS];
    struct Image select, plane1, pick1;
    struct AppIcon *icon[EXTRA_ICONS];
};

static const char *const extra_name[EXTRA_ICONS] =
{
    "wb file", "wb none", "wb plane1", "wb pick1"
};

static const char *const extra_add[EXTRA_ICONS] =
{
    "AddAppIconA() of a DiskObject as an icon file has it, with a select image",
    "AddAppIconA() with GFLG_GADGHNONE and a select image",
    "AddAppIconA() of an image of Depth 1 with PlanePick 2",
    "AddAppIconA() of an image of Depth 2 with PlanePick 1 and one plane of data"
};

static const char *const extra_remove[EXTRA_ICONS] =
{
    "RemoveAppIcon() of the DiskObject as an icon file has it",
    "RemoveAppIcon() of the one with GFLG_GADGHNONE",
    "RemoveAppIcon() of the image with PlanePick 2",
    "RemoveAppIcon() of the image with one plane of data"
};

static void add_extra(struct Extra *extra, struct Image *img, UWORD *data, struct MsgPort *port)
{
    int i;

    for (i = 0; i < ICON_WORDS; i++)
        data[SELECT_DATA + i] = (UWORD)~data[i];
    for (i = 0; i < ICON_PLANE_W; i++)
        data[ONE_PLANE + i] = data[i];
    init_image(&extra->select, ICON_DEPTH, data + SELECT_DATA, 3, 0);
    init_image(&extra->plane1, 1, data, 2, 0);
    init_image(&extra->pick1, ICON_DEPTH, data + ONE_PLANE, 1, 2);

    memset(extra->dobj, 0, sizeof(extra->dobj));
    for (i = 0; i < EXTRA_ICONS; i++)
    {
        extra->dobj[i].do_Gadget.Width = ICON_WIDTH;
        extra->dobj[i].do_Gadget.Height = ICON_HEIGHT;
        extra->dobj[i].do_Gadget.GadgetRender = img;
        extra->dobj[i].do_CurrentX = EXTRA_X + i * EXTRA_STEP;
        extra->dobj[i].do_CurrentY = EXTRA_Y;
    }
    extra->dobj[0].do_Magic = WB_DISKMAGIC;
    extra->dobj[0].do_Version = WB_DISKVERSION;
    extra->dobj[0].do_Gadget.Flags = GFLG_GADGIMAGE | GFLG_GADGHIMAGE;
    extra->dobj[0].do_Gadget.Activation = GACT_RELVERIFY | GACT_IMMEDIATE;
    extra->dobj[0].do_Gadget.GadgetType = GTYP_BOOLGADGET;
    extra->dobj[0].do_Gadget.SelectRender = &extra->select;
    extra->dobj[0].do_StackSize = 4096;
    extra->dobj[1].do_Gadget.Flags = GFLG_GADGHNONE;
    extra->dobj[1].do_Gadget.SelectRender = &extra->select;
    extra->dobj[2].do_Gadget.GadgetRender = &extra->plane1;
    extra->dobj[3].do_Gadget.GadgetRender = &extra->pick1;

    for (i = 0; i < EXTRA_ICONS; i++)
    {
        extra->icon[i] = AddAppIconA(APPICON_ID + 1 + i, APPICON_USER + 1 + i,
                                     (CONST_STRPTR)extra_name[i], port, 0, &extra->dobj[i], NULL);
        check(extra->icon[i] != NULL, extra_add[i]);
    }
}

/* DiskObjects that AddAppIconA() of AmigaOS 3.2.3 does not take, giving
   NULL: GFLG_GADGHIMAGE, and GFLG_GADGHNONE, which has its bit, without a
   select image, and render images without a width, a height, planes or
   data, or with 9 planes */
static void try_failing(struct MsgPort *port, struct Image *img)
{
    static const char *const what[7] =
    {
        "AddAppIconA() with GFLG_GADGHIMAGE and no select image gives NULL",
        "AddAppIconA() with GFLG_GADGHNONE and no select image gives NULL",
        "AddAppIconA() of an image of Width 0 gives NULL",
        "AddAppIconA() of an image of Height 0 gives NULL",
        "AddAppIconA() of an image of Depth 0 gives NULL",
        "AddAppIconA() of an image of Depth 9 gives NULL",
        "AddAppIconA() of an image without ImageData gives NULL"
    };
    struct DiskObject dobj;
    struct Image bad;
    struct AppIcon *icon;
    int i;

    for (i = 0; i < 7; i++)
    {
        memset(&dobj, 0, sizeof(dobj));
        dobj.do_Gadget.Width = ICON_WIDTH;
        dobj.do_Gadget.Height = ICON_HEIGHT;
        dobj.do_Gadget.GadgetRender = i < 2 ? img : &bad;
        dobj.do_CurrentX = NO_ICON_POSITION;
        dobj.do_CurrentY = NO_ICON_POSITION;
        bad = *img;
        if (i == 0)
            dobj.do_Gadget.Flags = GFLG_GADGHIMAGE;
        else if (i == 1)
            dobj.do_Gadget.Flags = GFLG_GADGHNONE;
        else if (i == 2)
            bad.Width = 0;
        else if (i == 3)
            bad.Height = 0;
        else if (i == 4)
            bad.Depth = 0;
        else if (i == 5)
            bad.Depth = 9;
        else
            bad.ImageData = NULL;
        icon = AddAppIconA(10 + i, 0, (CONST_STRPTR)"wb fails", port, 0, &dobj, NULL);
        if (icon)
            RemoveAppIcon(icon);
        check(icon == NULL, what[i]);
    }
}

/* Wait up to ticks (in 1/50 s) for an AppMessage on port, polling every
   10 ticks with Delay(10) and consuming the port's signal with Wait()
   when set. */
static struct AppMessage *wait_appmsg(struct MsgPort *port, int ticks)
{
    ULONG mask = 1UL << port->mp_SigBit;
    struct AppMessage *msg;
    int i;

    for (i = 0; i < ticks; i += 10)
    {
        if (SetSignal(0, 0) & mask)
            Wait(mask);
        if ((msg = (struct AppMessage *)GetMsg(port)) != NULL)
            return msg;
        Delay(10);
    }
    if (SetSignal(0, 0) & mask)
        Wait(mask);
    return (struct AppMessage *)GetMsg(port);
}

static void step_appicon(struct MsgPort *port)
{
    struct AppMessage *msg;

    if (!check(write_marker(), "SYS:EMU68 marker written for the mouse"))
        return;
    msg = wait_appmsg(port, 60 * 50);
    if (!check(msg != NULL, "AppIcon AppMessage arrives within 60 s"))
        return;
    check_long("AppIcon am_Type", msg->am_Type, AMTYPE_APPICON);
    check_long("AppIcon am_ID", (LONG)msg->am_ID, APPICON_ID);
    check_long("AppIcon am_UserData", (LONG)msg->am_UserData, APPICON_USER);
    check_long("AppIcon am_NumArgs", msg->am_NumArgs, 0);
    check(msg->am_Version >= AM_VERSION, "AppIcon am_Version is at least AM_VERSION");
    Printf("info appicon: am_MouseX,am_MouseY %ld,%ld\n",
           (LONG)msg->am_MouseX, (LONG)msg->am_MouseY);
    test_flush();
    ReplyMsg((struct Message *)msg);
}

/* The AppWindow goes while its AppMessage waits in the port, neither taken
   nor replied, as when a program quits right after a drop: Workbench must
   not wait for the reply then, and the message stays the program's, with
   its lock, until the program replies it. */
static void step_appwindow(struct MsgPort *port, struct AppWindow **appwin)
{
    ULONG mask = 1UL << port->mp_SigBit;
    struct AppMessage *msg;
    char path[256];
    int i;

    if (!check(write_marker(), "SYS:EMU68 marker written for the mouse"))
        return;
    for (i = 0; i < 60 * 50 && !(SetSignal(0, 0) & mask); i += 10)
        Delay(10);
    if (!check((SetSignal(0, mask) & mask) != 0, "AppWindow AppMessage signals within 60 s"))
        return;
    check(RemoveAppWindow(*appwin) != FALSE, "RemoveAppWindow() returns TRUE while the AppMessage waits");
    *appwin = NULL;
    msg = (struct AppMessage *)GetMsg(port);
    if (!check(msg != NULL, "GetMsg() gives the AppMessage after RemoveAppWindow()"))
        return;
    check_long("AppWindow am_Type", msg->am_Type, AMTYPE_APPWINDOW);
    check_long("AppWindow am_ID", (LONG)msg->am_ID, APPWIN_ID);
    check_long("AppWindow am_UserData", (LONG)msg->am_UserData, APPWIN_USER);
    if (check(msg->am_NumArgs >= 1 && msg->am_ArgList != NULL,
              "AppWindow am_NumArgs is at least 1"))
    {
        BPTR lock = msg->am_ArgList[0].wa_Lock;
        STRPTR name = msg->am_ArgList[0].wa_Name;

        path[0] = '\0';
        check(lock != 0, "AppWindow am_ArgList[0].wa_Lock is non-zero");
        if (check(lock != 0 && NameFromLock(lock, (STRPTR)path, sizeof(path)) != 0,
                  "NameFromLock() of am_ArgList[0].wa_Lock"))
        {
            Printf("info appwindow: wa_Lock \"%s\", wa_Name \"%s\"\n",
                   (LONG)path, (LONG)(name ? (const char *)name : "(null)"));
            test_flush();
        }
    }
    ReplyMsg((struct Message *)msg);
}

static void step_appmenu(struct MsgPort *port)
{
    struct AppMessage *msg;

    if (!check(write_marker(), "SYS:EMU68 marker written for the mouse"))
        return;
    msg = wait_appmsg(port, 60 * 50);
    if (!check(msg != NULL, "AppMenuItem AppMessage arrives within 60 s"))
        return;
    check_long("AppMenuItem am_Type", msg->am_Type, AMTYPE_APPMENUITEM);
    check_long("AppMenuItem am_ID", (LONG)msg->am_ID, APPMENU_ID);
    check_long("AppMenuItem am_UserData", (LONG)msg->am_UserData, APPMENU_USER);
    ReplyMsg((struct Message *)msg);
}

int main(int argc, char **argv)
{
    const char *arg = argc > 1 ? argv[1] : "register";
    enum Mode mode = MODE_REGISTER;
    struct MsgPort *port = NULL;
    struct Window *win = NULL;
    struct AppWindow *appwin = NULL;
    struct AppMenuItem *appitem = NULL;
    struct AppIcon *appicon = NULL;
    struct DiskObject icon;
    struct Image icon_image;
    struct Extra extra;
    UWORD *img_data = NULL;
    struct AppMessage *msg;
    int i;

    memset(extra.icon, 0, sizeof(extra.icon));

    if (strcmp(arg, "register") == 0)
        mode = MODE_REGISTER;
    else if (strcmp(arg, "appicon") == 0)
        mode = MODE_APPICON;
    else if (strcmp(arg, "appwindow") == 0)
        mode = MODE_APPWINDOW;
    else if (strcmp(arg, "appmenu") == 0)
        mode = MODE_APPMENU;
    else
    {
        check(FALSE, "mode is register, appicon, appwindow or appmenu");
        return test_done("gfx_workbench");
    }

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
    if (!check(IntuitionBase != NULL, "OpenLibrary(\"intuition.library\", 37)"))
        goto done;
    WorkbenchBase = OpenLibrary((CONST_STRPTR)"workbench.library", 37);
    if (!check(WorkbenchBase != NULL, "OpenLibrary(\"workbench.library\", 37)"))
        goto done;

    port = CreateMsgPort();
    if (!check(port != NULL, "CreateMsgPort()"))
        goto done;

    win = OpenWindowTags(NULL,
                         WA_Left, WIN_LEFT, WA_Top, WIN_TOP,
                         WA_Width, WIN_WIDTH, WA_Height, WIN_HEIGHT,
                         WA_Title, (ULONG)"gfx_workbench",
                         WA_Flags, WFLG_SMART_REFRESH | WFLG_DRAGBAR | WFLG_DEPTHGADGET,
                         TAG_DONE);
    if (!check(win != NULL, "OpenWindowTags() of an inactive window at 40,40"))
        goto done;
    check(!(win->Flags & WFLG_WINDOWACTIVE), "the window opens inactive");

    appwin = AddAppWindowA(APPWIN_ID, APPWIN_USER, win, port, NULL);
    if (!check(appwin != NULL, "AddAppWindowA(1, 0x1234, window, port, NULL)"))
        goto done;

    appitem = AddAppMenuItemA(APPMENU_ID, APPMENU_USER,
                              (CONST_STRPTR)"gfx_workbench item", port, NULL);
    if (!check(appitem != NULL, "AddAppMenuItemA(2, 0x5678, \"gfx_workbench item\", port, NULL)"))
        goto done;

    img_data = (UWORD *)AllocMem(DATA_BYTES, MEMF_CHIP | MEMF_CLEAR);
    if (!check(img_data != NULL, "AllocMem() of MEMF_CHIP image data for the DiskObjects"))
        goto done;
    init_diskobject(&icon, &icon_image, img_data);

    appicon = AddAppIconA(APPICON_ID, APPICON_USER, (CONST_STRPTR)"gfx_workbench",
                          port, 0, &icon, NULL);
    if (!check(appicon != NULL, "AddAppIconA(3, 0x9ABC, \"gfx_workbench\", port, 0, &icon, NULL) "
                                "of a DiskObject as the autodoc builds it"))
        goto done;
    if (mode == MODE_REGISTER || mode == MODE_APPICON)
        add_extra(&extra, &icon_image, img_data, port);
    if (mode == MODE_REGISTER)
        try_failing(port, &icon_image);

    msg = (struct AppMessage *)GetMsg(port);
    if (msg)
        ReplyMsg((struct Message *)msg);
    if (!check(msg == NULL, "GetMsg() returns NULL right after registration"))
        goto done;

    if (mode == MODE_APPICON)
        step_appicon(port);
    else if (mode == MODE_APPWINDOW)
        step_appwindow(port, &appwin);
    else if (mode == MODE_APPMENU)
        step_appmenu(port);

    check(RemoveAppIcon(appicon) != FALSE, "RemoveAppIcon() returns TRUE");
    appicon = NULL;
    for (i = 0; i < EXTRA_ICONS; i++)
        if (extra.icon[i])
        {
            check(RemoveAppIcon(extra.icon[i]) != FALSE, extra_remove[i]);
            extra.icon[i] = NULL;
        }
    check(RemoveAppMenuItem(appitem) != FALSE, "RemoveAppMenuItem() returns TRUE");
    appitem = NULL;
    if (appwin)
    {
        check(RemoveAppWindow(appwin) != FALSE, "RemoveAppWindow() returns TRUE");
        appwin = NULL;
    }

done:
    if (appicon)
        RemoveAppIcon(appicon);
    for (i = 0; i < EXTRA_ICONS; i++)
        if (extra.icon[i])
            RemoveAppIcon(extra.icon[i]);
    if (appitem)
        RemoveAppMenuItem(appitem);
    if (appwin)
        RemoveAppWindow(appwin);
    if (port)
        while ((msg = (struct AppMessage *)GetMsg(port)) != NULL)
            ReplyMsg((struct Message *)msg);
    if (win)
        CloseWindow(win);
    if (img_data)
        FreeMem(img_data, DATA_BYTES);
    if (port)
        DeleteMsgPort(port);
    if (WorkbenchBase)
        CloseLibrary(WorkbenchBase);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_workbench");
}
