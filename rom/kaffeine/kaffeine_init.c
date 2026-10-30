/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Fabian Schmieder (@metaneutrons)

    kaffeine.library — library init/open/close
*/

#include <aros/symbolsets.h>
#include <proto/exec.h>

#include "kaffeine_intern.h"
#include "kaffeine_segments.h"

static int Kaffeine_Init(struct KaffeineLibBase *base)
{
    base->sysBase = (struct ExecBase *)SysBase;
    base->dosBase = NULL;
    Kaffeine_SegmentsInit(base);
    return TRUE;
}

static int Kaffeine_Open(struct KaffeineLibBase *base)
{
    if (!base->dosBase)
        base->dosBase = (struct DosLibrary *)OpenLibrary("dos.library", 0);
    return base->dosBase != NULL;
}

static void Kaffeine_Close(struct KaffeineLibBase *base)
{
    /* Keep dos.library open for reuse */
}

static int Kaffeine_Expunge(struct KaffeineLibBase *base)
{
    if (base->dosBase)
    {
        CloseLibrary((struct Library *)base->dosBase);
        base->dosBase = NULL;
    }
    return TRUE;
}

ADD2INITLIB(Kaffeine_Init, 0)
ADD2OPENLIB(Kaffeine_Open, 0)
ADD2CLOSELIB(Kaffeine_Close, 0)
ADD2EXPUNGELIB(Kaffeine_Expunge, 0)
