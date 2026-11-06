/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Hardware blitter area blits as classic programs start them: the
   blitter owned with OwnBlitter(), registers written through struct
   Custom, WaitBlit() before the results are used, DisownBlitter(). The
   expected words are those of the Hardware Reference Manual rules: the
   chapter 6 copy with a shift of 4 and masks, the cookie-cut function
   $CA with A off as a row mask, a descending overlapping copy, the
   one-word late D write of an ascending overlap, BZERO with D off, a
   clear with A and B constant, and the held BLTBDAT value. The last case
   blits a MOVEQ over a small function that already ran and calls it
   again: Kaffeine announces the written span to its CPU backend, so the
   new instruction must run without a CacheClearU() after the blit. */
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <hardware/blit.h>
#include <hardware/custom.h>
#include <hardware/dmabits.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <string.h>
#include "m68ktest.h"

#define CUSTOM ((volatile struct Custom *)0xdff000)

struct GfxBase *GfxBase;
static UWORD *buf;

static void setup(UWORD con0, UWORD con1)
{
    WaitBlit();
    CUSTOM->bltcon0 = con0;
    CUSTOM->bltcon1 = con1;
    CUSTOM->bltafwm = 0xFFFF;
    CUSTOM->bltalwm = 0xFFFF;
    CUSTOM->bltamod = 0;
    CUSTOM->bltbmod = 0;
    CUSTOM->bltcmod = 0;
    CUSTOM->bltdmod = 0;
}

static BOOL equal(const UWORD *at, const UWORD *want, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (at[i] != want[i])
            return FALSE;
    return TRUE;
}

static void fill(UWORD *at, UWORD v, int n)
{
    while (n--)
        *at++ = v;
}

int main(void)
{
    static const UWORD src[6] = { 0x1234, 0x5678, 0x9ABC, 0xDEF0, 0x1357, 0x2468 };
    static const UWORD shifted[6] = { 0x0023, 0x4567, 0x89A0, 0x00EF, 0x0135, 0x7240 };
    static const UWORD cut[6] = { 0x1AAA, 0xBBBB, 0xCC11, 0x1DDD, 0xEEEE, 0xFF11 };
    static const UWORD moved[8] = { 1, 2, 1, 2, 3, 4, 5, 6 };
    static const UWORD smeared[8] = { 1, 2, 1, 2, 1, 2, 1, 2 };
    static const UWORD code[2] = { 0x7001, 0x4E75 };   /* MOVEQ #1,D0; RTS */
    LONG (*fn)(void);
    LONG before, after;
    UWORD zero_and, zero_or;
    int i;

    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    if (!check(GfxBase != NULL, "open graphics.library 39"))
        return test_done("hw_blitter");
    buf = (UWORD *)AllocMem(512, MEMF_CHIP | MEMF_CLEAR);
    if (!check(buf != NULL, "chip buffer for the blits"))
    {
        CloseLibrary((struct Library *)GfxBase);
        return test_done("hw_blitter");
    }
    OwnBlitter();

    /* the HRM copy: 3 by 2 words, A shift 4, masks $0FFF/$FF00 */
    memcpy(buf, src, sizeof(src));
    setup(0x49F0, 0);
    CUSTOM->bltafwm = 0x0FFF;
    CUSTOM->bltalwm = 0xFF00;
    CUSTOM->bltapt = buf;
    CUSTOM->bltdpt = buf + 16;
    CUSTOM->bltsize = (2 << 6) | 3;
    WaitBlit();
    check(equal(buf + 16, shifted, 6), "A to D copy with shift 4, masks and row carry");

    /* cookie cut: B source, C and D the destination, A off as a row mask */
    buf[32] = 0xAAAA; buf[33] = 0xBBBB; buf[34] = 0xCCCC;
    buf[35] = 0xDDDD; buf[36] = 0xEEEE; buf[37] = 0xFFFF;
    fill(buf + 48, 0x1111, 6);
    setup(0x07CA, 0);
    CUSTOM->bltadat = 0xFFFF;
    CUSTOM->bltafwm = 0x0FFF;
    CUSTOM->bltalwm = 0xFF00;
    CUSTOM->bltbpt = buf + 32;
    CUSTOM->bltcpt = buf + 48;
    CUSTOM->bltdpt = buf + 48;
    CUSTOM->bltsize = (2 << 6) | 3;
    WaitBlit();
    check(equal(buf + 48, cut, 6), "cookie cut $CA with A off as a masked row");

    /* descending overlapping copy, and the late D write of an ascending one */
    for (i = 0; i < 8; i++) buf[64 + i] = (UWORD)(i + 1);
    setup(0x09F0, BLITREVERSE);
    CUSTOM->bltapt = buf + 64 + 5;
    CUSTOM->bltdpt = buf + 64 + 7;
    CUSTOM->bltsize = (1 << 6) | 6;
    WaitBlit();
    check(equal(buf + 64, moved, 8), "descending overlapping copy");
    for (i = 0; i < 8; i++) buf[64 + i] = (UWORD)(i + 1);
    setup(0x09F0, 0);
    CUSTOM->bltapt = buf + 64;
    CUSTOM->bltdpt = buf + 64 + 2;
    CUSTOM->bltsize = (1 << 6) | 6;
    WaitBlit();
    check(equal(buf + 64, smeared, 8), "ascending overlap writes D one word late");

    /* BZERO with D off: A AND B */
    buf[80] = 0xF000; buf[81] = 0x0F00; buf[82] = 0x1F00;
    setup(0x0C80, 0);
    CUSTOM->bltapt = buf + 80;
    CUSTOM->bltbpt = buf + 81;
    CUSTOM->bltsize = (1 << 6) | 1;
    WaitBlit();
    zero_and = CUSTOM->dmaconr & DMAF_BLTNZERO;
    setup(0x0C80, 0);
    CUSTOM->bltapt = buf + 80;
    CUSTOM->bltbpt = buf + 82;
    CUSTOM->bltsize = (1 << 6) | 1;
    WaitBlit();
    zero_or = CUSTOM->dmaconr & DMAF_BLTNZERO;
    check(zero_and && !zero_or && buf[80] == 0xF000, "BZERO with D off");

    /* a clear with C and D only: A = $FFFF, B = 0 */
    fill(buf + 96, 0x5A5A, 8);
    setup(0x03CA, 0);
    CUSTOM->bltadat = 0xFFFF;
    CUSTOM->bltbdat = 0;
    CUSTOM->bltcpt = buf + 96;
    CUSTOM->bltdpt = buf + 96;
    CUSTOM->bltsize = (2 << 6) | 4;
    WaitBlit();
    check(buf[96] == 0 && buf[103] == 0 && (CUSTOM->dmaconr & DMAF_BLTNZERO), "clear with constant A and B");

    /* BLTBDAT is shifted when written: a later BSH does not change it */
    setup(0x01CC, 0x4000);
    CUSTOM->bltbdat = 0x1234;
    CUSTOM->bltcon1 = 0x8000;
    CUSTOM->bltdpt = buf + 112;
    CUSTOM->bltsize = (1 << 6) | 3;
    WaitBlit();
    check(buf[112] == 0x0123 && buf[114] == 0x0123, "BLTBDAT keeps the shift of its write");

    /* guest code changed by a blit runs anew */
    memcpy(buf + 128, code, sizeof(code));
    CacheClearU();
    fn = (LONG (*)(void))(buf + 128);
    before = fn();
    setup(0x01F0, 0);
    CUSTOM->bltadat = 0x7002;           /* MOVEQ #2,D0 */
    CUSTOM->bltdpt = buf + 128;
    CUSTOM->bltsize = (1 << 6) | 1;
    WaitBlit();
    after = fn();
    check(before == 1 && after == 2, "an instruction written by a blit runs");

    DisownBlitter();
    FreeMem(buf, 512);
    CloseLibrary((struct Library *)GfxBase);
    return test_done("hw_blitter");
}
