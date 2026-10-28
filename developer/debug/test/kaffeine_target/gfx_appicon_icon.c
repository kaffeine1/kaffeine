/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Original AppIcon registration from an icon.library DiskObject. */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/icon.h>
#ifdef __AROS__
#include <proto/workbench.h>
#else
#include <proto/wb.h>
#endif
#include <stdio.h>
#include "m68ktest.h"

struct Library *IconBase;
struct Library *WorkbenchBase;

int main(void)
{
    struct MsgPort *port = NULL;
    struct DiskObject *icon = NULL;
    struct AppIcon *app = NULL;
    BYTE signal = -1;
    unsigned pass;

    IconBase = OpenLibrary("icon.library", 44);
    WorkbenchBase = OpenLibrary("workbench.library", 39);
    if (!check(IconBase && WorkbenchBase, "open icon and Workbench libraries")) goto done;
    port = AllocMem(sizeof(*port), MEMF_PUBLIC | MEMF_CLEAR);
    if (!check(port != NULL, "allocate an ordinary message port")) goto done;
    signal = AllocSignal(-1);
    if (!check(signal >= 0, "allocate the port signal")) goto done;
    port->mp_Node.ln_Type = NT_MSGPORT;
    port->mp_Flags = PA_SIGNAL;
    port->mp_SigBit = signal;
    port->mp_SigTask = FindTask(NULL);
    port->mp_MsgList.lh_Head = (struct Node *)&port->mp_MsgList.lh_Tail;
    port->mp_MsgList.lh_Tail = NULL;
    port->mp_MsgList.lh_TailPred = (struct Node *)&port->mp_MsgList.lh_Head;
    for (pass = 0; pass < 2; pass++) {
        icon = GetDefDiskObject(WBTOOL);
        if (!check(icon != NULL, "get an icon-library-owned default DiskObject")) goto done;
        check(icon->do_Gadget.Width > 0 && icon->do_Gadget.Height > 0,
              "default icon publishes readable geometry");
        icon->do_CurrentX = NO_ICON_POSITION;
        icon->do_CurrentY = NO_ICON_POSITION;
        puts("STEP: register a library-owned icon as an AppIcon");
        app = AddAppIconA(100 + pass, 0x1234 + pass, (STRPTR)"AppIcon bridge oracle",
                          port, 0, icon, NULL);
        check(app != NULL, "register an AppIcon from the owned DiskObject");
        if (app) {
            check(GetMsg(port) == NULL, "registration leaves the AppMessage queue empty");
            check(RemoveAppIcon(app) != 0, "remove the AppIcon before releasing its DiskObject");
            app = NULL;
        }
        check(icon->do_Gadget.Width > 0, "AppIcon removal preserves caller icon metadata");
        FreeDiskObject(icon);
        icon = NULL;
    }
done:
    if (app) RemoveAppIcon(app);
    if (icon) FreeDiskObject(icon);
    if (signal >= 0) FreeSignal(signal);
    if (port) FreeMem(port, sizeof(*port));
    if (WorkbenchBase) CloseLibrary(WorkbenchBase);
    if (IconBase) CloseLibrary(IconBase);
    return test_done("gfx_appicon_icon");
}
