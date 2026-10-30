/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#pragma once
#include <exec/libraries.h>
#include <exec/lists.h>
#include <graphics/text.h>
/* The public native GfxBase prefix through DefaultFont. The rest is not
 * read by this bridge. Real native headers remain the target-build oracle. */
struct GfxBaseFixtureInterrupt
{
    struct Node node;
    APTR data;
    void (*code)(void);
};
struct GfxBase
{
    struct Library LibNode;
    APTR ActiView, copinit, cia, blitter, LOFlist, SHFlist;
    APTR blthd, blttl, bsblthd, bsblttl;
    struct GfxBaseFixtureInterrupt vbsrv, timsrv, bltsrv;
    struct List TextFonts;
    struct TextFont *DefaultFont;
};
