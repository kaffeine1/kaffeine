/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#pragma once
#include <devices/timer.h>

/* Native pointer width and alignment, deliberately different from 68k. */
struct InputEvent
{
    struct InputEvent *ie_NextEvent;
    UBYTE ie_Class, ie_SubClass;
    UWORD ie_Code, ie_Qualifier;
    union
    {
        struct { WORD ie_x, ie_y; } ie_xy;
        APTR ie_addr;
        struct
        {
            UBYTE ie_prev1DownCode, ie_prev1DownQual;
            UBYTE ie_prev2DownCode, ie_prev2DownQual;
        } ie_dead;
    } ie_position;
    struct timeval ie_TimeStamp;
};
#define ie_Prev1DownCode ie_position.ie_dead.ie_prev1DownCode
#define ie_Prev1DownQual ie_position.ie_dead.ie_prev1DownQual
#define ie_Prev2DownCode ie_position.ie_dead.ie_prev2DownCode
#define ie_Prev2DownQual ie_position.ie_dead.ie_prev2DownQual
#define IECLASS_RAWKEY 1
