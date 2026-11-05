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
#define ie_X ie_position.ie_xy.ie_x
#define ie_Y ie_position.ie_xy.ie_y
#define ie_EventAddress ie_position.ie_addr
/* The classes and subclasses of compiler/include/devices/inputevent.h */
#define IECLASS_NULL 0
#define IECLASS_RAWKEY 1
#define IECLASS_RAWMOUSE 2
#define IECLASS_EVENT 3
#define IECLASS_POINTERPOS 4
#define IECLASS_TIMER 6
#define IECLASS_GADGETDOWN 7
#define IECLASS_GADGETUP 8
#define IECLASS_REQUESTER 9
#define IECLASS_MENULIST 10
#define IECLASS_CLOSEWINDOW 11
#define IECLASS_SIZEWINDOW 12
#define IECLASS_REFRESHWINDOW 13
#define IECLASS_NEWPREFS 14
#define IECLASS_DISKREMOVED 15
#define IECLASS_DISKINSERTED 16
#define IECLASS_ACTIVEWINDOW 17
#define IECLASS_INACTIVEWINDOW 18
#define IECLASS_NEWPOINTERPOS 19
#define IECLASS_MENUHELP 20
#define IECLASS_CHANGEWINDOW 21
#define IECLASS_NEWMOUSE 22
#define IESUBCLASS_COMPATIBLE 0
#define IESUBCLASS_PIXEL 1
#define IESUBCLASS_TABLET 2
#define IESUBCLASS_NEWTABLET 3
#define IECODE_UP_PREFIX 0x80
#define IECODE_LBUTTON 0x68
#define IECODE_NOBUTTON 0xFF
#define IEQUALIFIER_LEFTBUTTON (1 << 14)
#define IEQUALIFIER_RELATIVEMOUSE (1 << 15)

struct Screen;
struct IEPointerPixel
{
    struct Screen *iepp_Screen;
    struct { WORD X, Y; } iepp_Position;
};
