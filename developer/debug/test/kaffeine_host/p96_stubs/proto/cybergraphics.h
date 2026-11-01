/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The cybergraphics.library calls of kaffeine_p96.c, which test_p96.cpp
    records (signatures of workbench/libs/cgfx/cybergraphics.conf).
*/
#pragma once
#include <exec/types.h>
#include <utility/tagitem.h>

struct RastPort;

#ifdef __cplusplus
extern "C" {
#endif

ULONG BestCModeIDTagList(struct TagItem *tags);
BOOL IsCyberModeID(ULONG modeID);
ULONG GetCyberIDAttr(ULONG attribute, ULONG DisplayModeID);
ULONG WritePixelArray(APTR src, UWORD srcx, UWORD srcy, UWORD srcmod, struct RastPort *rp,
                      UWORD destx, UWORD desty, UWORD width, UWORD height, UBYTE srcformat);

#ifdef __cplusplus
}
#endif
