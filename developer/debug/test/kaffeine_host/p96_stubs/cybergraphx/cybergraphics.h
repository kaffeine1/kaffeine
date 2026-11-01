/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The cybergraphics constants of kaffeine_p96.c, with the values of
    workbench/libs/cgfx/include/cybergraphics.h.
*/
#pragma once
#include <utility/tagitem.h>

#define CYBRIDATTR_PIXFMT       0x80000001
#define CYBRIDATTR_BPPIX        0x80000005

#define CYBRBIDTG_TB            (TAG_USER + 0x50000)
#define CYBRBIDTG_Depth         (CYBRBIDTG_TB + 0)
#define CYBRBIDTG_NominalWidth  (CYBRBIDTG_TB + 1)
#define CYBRBIDTG_NominalHeight (CYBRBIDTG_TB + 2)

#define PIXFMT_LUT8     0UL
#define PIXFMT_RGB15    1UL
#define PIXFMT_BGR15    2UL
#define PIXFMT_RGB15PC  3UL
#define PIXFMT_BGR15PC  4UL
#define PIXFMT_RGB16    5UL
#define PIXFMT_BGR16    6UL
#define PIXFMT_RGB16PC  7UL
#define PIXFMT_BGR16PC  8UL
#define PIXFMT_RGB24    9UL
#define PIXFMT_BGR24    10UL
#define PIXFMT_ARGB32   11UL
#define PIXFMT_BGRA32   12UL
#define PIXFMT_RGBA32   13UL
#define PIXFMT_ABGR32   100UL
#define PIXFMT_0RGB32   101UL
#define PIXFMT_BGR032   102UL
#define PIXFMT_RGB032   103UL
#define PIXFMT_0BGR32   104UL

#define RECTFMT_RGB     0UL
#define RECTFMT_RGBA    1UL
#define RECTFMT_ARGB    2UL
#define RECTFMT_RGB24   RECTFMT_RGB
#define RECTFMT_BGR24   109UL
#define RECTFMT_ARGB32  RECTFMT_ARGB
#define RECTFMT_BGRA32  111UL
#define RECTFMT_RGBA32  RECTFMT_RGBA
#define RECTFMT_ABGR32  113UL
