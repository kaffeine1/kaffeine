/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    EasyRequests, the way C programs show them, with no input needed. The
    text and the gadgets come from printf-style formats with arguments.
    BuildEasyRequestArgs() gives the requester window, SysReqHandler()
    without waiting finds no answer yet, and FreeSysRequest() closes it.
    The usual loop of WaitPort() on the port of the requester and
    SysReqHandler(), which takes the message that WaitPort() showed, and
    EasyRequestArgs(), both with IDCMP_INTUITICKS among their IDCMP flags,
    return -1 at the first tick of their active requester. The guest memory
    is back after each.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;

static const char title[] = "gfx_request";

int main(void)
{
    struct EasyStruct es = {
        sizeof(struct EasyStruct), 0, (UBYTE *)title,
        (UBYTE *)"Hello %s, %ld%% done", (UBYTE *)"%s|Cancel"
    };
    ULONG args[3] = { (ULONG)"world", 42, (ULONG)"OK" };
    struct Window *req;
    ULONG idcmp, avail;
    LONG result, i;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
    if (!check(IntuitionBase != NULL, "intuition.library 37"))
        return test_done("gfx_request");

    /* A first requester shows the bridge the screen font, which it keeps. */
    req = BuildEasyRequestArgs(NULL, &es, 0, args);
    if (req)
        FreeSysRequest(req);
    avail = AvailMem(MEMF_ANY);

    req = BuildEasyRequestArgs(NULL, &es, 0, args);
    if (check(req != NULL, "BuildEasyRequestArgs() gives a requester window"))
    {
        check(req->Width > 0 && req->Height > 0 && req->RPort != NULL,
              "the requester window has a size and a RastPort");
        check(req->Title == (UBYTE *)title, "the requester window has the title of the EasyStruct");
        check_long("SysReqHandler() without waiting finds no answer yet",
                   SysReqHandler(req, NULL, FALSE), -2);
        FreeSysRequest(req);
    }
    check_long("a requester returns all of its memory", AvailMem(MEMF_ANY), avail);

    /* WaitPort() first, then SysReqHandler() without waiting, as programs
       that wait for their requester do */
    req = BuildEasyRequestArgs(NULL, &es, IDCMP_INTUITICKS, args);
    if (check(req != NULL, "BuildEasyRequestArgs() with IDCMP_INTUITICKS"))
    {
        idcmp = 0;
        i = 0;
        do
        {
            WaitPort(req->UserPort);
            result = SysReqHandler(req, &idcmp, FALSE);
        } while (result == -2 && ++i < 50);
        check_long("WaitPort() and SysReqHandler() end at an IDCMP flag of the program", result, -1);
        check_long("and name it", idcmp, IDCMP_INTUITICKS);
        FreeSysRequest(req);
    }
    check_long("the requester of WaitPort() returns all of its memory", AvailMem(MEMF_ANY), avail);

    idcmp = IDCMP_INTUITICKS;
    result = EasyRequestArgs(NULL, &es, &idcmp, args);
    check_long("EasyRequestArgs() ends at an IDCMP flag of the program", result, -1);
    check_long("and names it", idcmp, IDCMP_INTUITICKS);
    check_long("EasyRequestArgs() returns all of its memory", AvailMem(MEMF_ANY), avail);

    CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_request");
}
