/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The custom chip blitter of a guest context, in area mode, as the
    Amiga Hardware Reference Manual (3rd edition, chapter 6) describes it.
    68k programs may own the blitter with OwnBlitter() and start blits by
    writing its registers; both CPU backends pass their word writes to the
    custom chip window here (Kaffeine_BlitterWrite()) and take DMACONR
    from Kaffeine_BlitterDmaconr(). Inline, as Kaffeine_ScreenBitMapOf()
    is, so that every backend and test that compiles a CPU adapter gets it
    without another object.

    A write of BLTSIZE, or of BLTSIZH with BLTSIZV (the ECS big blitter),
    starts a blit, which runs to its end before the write returns: BBUSY
    is set only while a started blit waits for DMAEN and BLTEN in DMACON,
    and it runs as soon as a DMACON write sets both. A context starts with
    DMAEN and BLTEN set, as the running OS leaves them for its programs
    (Kaffeine_BlitterInit()); later DMACON writes set and clear bits 10-0.

    Area mode, per word, the first word of each row first:
    - A comes from BLTAPT if USEA, else from BLTADAT; the first word of a
      row is ANDed with BLTAFWM, the last with BLTALWM, both for a row of
      one word. The barrel shifter then shifts it right by ASH (left in
      descending mode), with the bits shifted out of the previous A word
      shifted in: zeros for the first word of the blit, and the carry runs
      on across rows, as on the hardware.
    - B comes from BLTBPT if USEB, shifted by BSH in the same way without
      masks; with B off it is the held value, which a BLTBDAT write sets
      by shifting the data once with the BSH and direction of that moment
      ("loading one of the data registers draws the data through the
      machine"), and which a B fetch replaces.
    - C comes from BLTCPT if USEC, else from BLTCDAT.
    - D is the minterm BLTCON0 bits 7-0 select (bit 7 ABC ... bit 0 not A
      not B not C). It is written to BLTDPT if USED, one word late, after
      the next sources are fetched, as the hardware pipeline does, which
      decides overlapping copies. BZERO stays set while every D word is
      zero, also with D off.
    - Pointers move by 2 per word and by their modulo per row (both
      subtracted in descending mode); those of channels that are off do
      not move. After a blit the pointers, BLTADAT, BLTBDAT, BLTCDAT and the
      held B are those the last words left.

    Pointers are guest addresses with bit 0 ignored and no 19- or 21-bit
    chip wrap, since the whole guest arena stands for chip memory. Before
    a blit touches memory, the whole span each enabled channel reaches is
    computed from its pointer, modulo, size and direction and checked
    against the guest arena without the custom chip window, as for CPU
    accesses, and its size against the arena's word count (the work bound).
    A blit that fails a check, line mode (BLTCON1 bit 0) and area fill
    (IFE or EFE) are not run at all: nothing is read or written, and the
    caller stops the program with the 20 it gets. After a blit that wrote
    D, the written span is announced to the CPU backend
    (Kaffeine_FlushCode()), so that guest code a blit changed runs anew.

    Only word writes reach here (a long is two word writes); both
    backends ignore byte writes to the window, and every blitter register
    but DMACONR reads as 0. No copper, display, sprite, audio, disk or
    interrupt behaviour is modelled, and no blit timing.
*/

#ifndef KAFFEINE_BLITTER_H
#define KAFFEINE_BLITTER_H

#include <aros/debug.h>

#include "kaffeine_intern.h"
#include "kaffeine_cpu.h"
#include "kaffeine_offsets.h"

#define KAFFEINE_DMACON_SETCLR    0x8000
#define KAFFEINE_DMACON_BBUSY     0x4000
#define KAFFEINE_DMACON_BZERO     0x2000
#define KAFFEINE_DMACON_DMAEN     0x0200
#define KAFFEINE_DMACON_BLTEN     0x0040
#define KAFFEINE_DMACON_WRITABLE  0x07FF

/* BLTCON0 and BLTCON1 bits */
#define KAFFEINE_BLT_USEA 0x0800
#define KAFFEINE_BLT_USEB 0x0400
#define KAFFEINE_BLT_USEC 0x0200
#define KAFFEINE_BLT_USED 0x0100
#define KAFFEINE_BLT_LINE 0x0001
#define KAFFEINE_BLT_DESC 0x0002
#define KAFFEINE_BLT_FILL 0x0018  /* IFE and EFE */

/* A new context's blitter: idle, with DMAEN and BLTEN set */
static inline void Kaffeine_BlitterInit(struct KaffeineBlitter *blt)
{
    blt->dmacon = KAFFEINE_DMACON_DMAEN | KAFFEINE_DMACON_BLTEN;
}

static inline UWORD Kaffeine_BlitterMinterm(UWORD a, UWORD b, UWORD c, UWORD lf)
{
    UWORD d = 0;

    if (lf & 0x80) d |= a & b & c;
    if (lf & 0x40) d |= a & b & (UWORD)~c;
    if (lf & 0x20) d |= a & (UWORD)~b & c;
    if (lf & 0x10) d |= a & (UWORD)~b & (UWORD)~c;
    if (lf & 0x08) d |= (UWORD)~a & b & c;
    if (lf & 0x04) d |= (UWORD)~a & b & (UWORD)~c;
    if (lf & 0x02) d |= (UWORD)~a & (UWORD)~b & c;
    if (lf & 0x01) d |= (UWORD)~a & (UWORD)~b & (UWORD)~c;
    return d;
}

/* The bytes [*lo, *hi] a channel reaches in a w by h blit: its rows start
   one stride apart, so the first and the last row bound them. */
static inline void Kaffeine_BlitterSpan(ULONG ptr, WORD mod, ULONG w, ULONG h, int desc,
                                        long long *lo, long long *hi)
{
    long long first = (long long)(ptr & ~1UL);
    long long stride = 2 * (long long)w + mod;
    long long last = first + (desc ? -stride : stride) * (long long)(h - 1);

    *lo = first < last ? first : last;
    *hi = first < last ? last : first;
    if (desc)
    {
        *lo -= 2 * ((long long)w - 1);
        *hi += 1;
    }
    else
        *hi += 2 * (long long)w - 1;
}

/* Whether [lo, hi] lies in guest memory and outside the custom chips */
static inline int Kaffeine_BlitterSpanOk(const struct KaffeineContext *ctx, long long lo, long long hi)
{
    return lo >= 0 && hi < (long long)ctx->mem_size &&
           (hi < (long long)CUSTOM_BASE || lo >= (long long)CUSTOM_BASE + CUSTOM_SIZE);
}

static inline int Kaffeine_BlitterRefuse(const struct KaffeineBlitter *blt, const char *why,
                                         ULONG w, ULONG h)
{
    bug("[kaffeine] blitter: %s, BLTCON0 %04x BLTCON1 %04x size %lux%lu, stopping (20)\n",
        why, (unsigned)blt->con0, (unsigned)blt->con1, (unsigned long)w, (unsigned long)h);
    return 20;
}

/* Run a w by h area blit now: 0, or 20 with nothing read or written */
static inline int Kaffeine_BlitterRun(struct KaffeineContext *ctx, ULONG w, ULONG h)
{
    struct KaffeineBlitter *blt = &ctx->blitter;
    const UWORD con0 = blt->con0, con1 = blt->con1, lf = con0 & 0xFF;
    const unsigned ash = con0 >> 12, bsh = con1 >> 12;
    const int desc = (con1 & KAFFEINE_BLT_DESC) != 0;
    const int use_a = (con0 & KAFFEINE_BLT_USEA) != 0, use_b = (con0 & KAFFEINE_BLT_USEB) != 0;
    const int use_c = (con0 & KAFFEINE_BLT_USEC) != 0, use_d = (con0 & KAFFEINE_BLT_USED) != 0;
    const ULONG step = desc ? (ULONG)-2L : 2UL;
    long long alo, ahi, blo, bhi, clo, chi, dlo = 0, dhi = 0;
    ULONG ap = blt->apt & ~1UL, bp = blt->bpt & ~1UL, cp = blt->cpt & ~1UL, dp = blt->dpt & ~1UL;
    ULONG x, y, dst = 0;
    UWORD prev_a = 0, prev_b = 0, b = blt->bhold, c = blt->cdat, out = 0;
    int pending = 0, zero = 1;

    if (con1 & KAFFEINE_BLT_LINE)
        return Kaffeine_BlitterRefuse(blt, "line mode is not implemented", w, h);
    if (con1 & KAFFEINE_BLT_FILL)
        return Kaffeine_BlitterRefuse(blt, "area fill is not implemented", w, h);
    if ((unsigned long long)w * h > ctx->mem_size / 2)
        return Kaffeine_BlitterRefuse(blt, "more words than the guest arena holds", w, h);
    Kaffeine_BlitterSpan(blt->apt, blt->amod, w, h, desc, &alo, &ahi);
    Kaffeine_BlitterSpan(blt->bpt, blt->bmod, w, h, desc, &blo, &bhi);
    Kaffeine_BlitterSpan(blt->cpt, blt->cmod, w, h, desc, &clo, &chi);
    Kaffeine_BlitterSpan(blt->dpt, blt->dmod, w, h, desc, &dlo, &dhi);
    if ((use_a && !Kaffeine_BlitterSpanOk(ctx, alo, ahi)) ||
        (use_b && !Kaffeine_BlitterSpanOk(ctx, blo, bhi)) ||
        (use_c && !Kaffeine_BlitterSpanOk(ctx, clo, chi)) ||
        (use_d && !Kaffeine_BlitterSpanOk(ctx, dlo, dhi)))
        return Kaffeine_BlitterRefuse(blt, "a channel reaches outside guest memory", w, h);

    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            UWORD a;

            if (use_a)
            {
                blt->adat = m68k_read16(ctx, ap);
                ap += step;
            }
            a = blt->adat;
            if (x == 0)
                a &= blt->afwm;
            if (x == w - 1)
                a &= blt->alwm;
            {
                const UWORD masked = a;

                a = desc ? (UWORD)((((ULONG)masked << 16) | prev_a) >> (16 - ash))
                         : (UWORD)((((ULONG)prev_a << 16) | masked) >> ash);
                prev_a = masked;
            }
            if (use_b)
            {
                const UWORD fetched = m68k_read16(ctx, bp);

                bp += step;
                blt->bdat = fetched;
                b = desc ? (UWORD)((((ULONG)fetched << 16) | prev_b) >> (16 - bsh))
                         : (UWORD)((((ULONG)prev_b << 16) | fetched) >> bsh);
                prev_b = fetched;
            }
            if (use_c)
            {
                c = m68k_read16(ctx, cp);
                cp += step;
            }
            if (pending)
                m68k_write16(ctx, dst, out);
            out = Kaffeine_BlitterMinterm(a, b, c, lf);
            if (out)
                zero = 0;
            pending = use_d;
            if (use_d)
            {
                dst = dp;
                dp += step;
            }
        }
        ap += desc ? (ULONG)-(LONG)blt->amod : (ULONG)(LONG)blt->amod;
        bp += desc ? (ULONG)-(LONG)blt->bmod : (ULONG)(LONG)blt->bmod;
        cp += desc ? (ULONG)-(LONG)blt->cmod : (ULONG)(LONG)blt->cmod;
        dp += desc ? (ULONG)-(LONG)blt->dmod : (ULONG)(LONG)blt->dmod;
    }
    if (pending)
        m68k_write16(ctx, dst, out);

    if (use_a) blt->apt = ap;
    if (use_b)
    {
        blt->bpt = bp;
        blt->bhold = b;
    }
    if (use_c)
    {
        blt->cpt = cp;
        blt->cdat = c;
    }
    if (use_d)
    {
        blt->dpt = dp;
        Kaffeine_FlushCode(ctx->cpu, (ULONG)dlo, (ULONG)(dhi - dlo + 1));
    }
    blt->zero = (UBYTE)zero;
    return 0;
}

/* A blit start: run it, or let it wait for DMAEN and BLTEN */
static inline int Kaffeine_BlitterStart(struct KaffeineContext *ctx, ULONG w, ULONG h)
{
    struct KaffeineBlitter *blt = &ctx->blitter;
    const UWORD on = KAFFEINE_DMACON_DMAEN | KAFFEINE_DMACON_BLTEN;

    if ((blt->dmacon & on) != on)
    {
        blt->pending = 1;
        blt->pend_w = (UWORD)w;
        blt->pend_h = (UWORD)h;
        return 0;
    }
    blt->pending = 0;
    return Kaffeine_BlitterRun(ctx, w, h);
}

/* A 68k word write of a custom chip register (offset reg, 0-$1FE).
   Registers other than the blitter's and DMACON are not touched. 0, or
   20 when a blit that this write started or released had to be refused:
   the caller then stops the program. */
static inline int Kaffeine_BlitterWrite(struct KaffeineContext *ctx, UWORD reg, UWORD val)
{
    struct KaffeineBlitter *blt = &ctx->blitter;

    switch (reg)
    {
        case 0x040: blt->con0 = val; break;
        case 0x042: blt->con1 = val; break;
        case 0x044: blt->afwm = val; break;
        case 0x046: blt->alwm = val; break;
        case 0x048: blt->cpt = (blt->cpt & 0x0000FFFFUL) | ((ULONG)val << 16); break;
        case 0x04A: blt->cpt = (blt->cpt & 0xFFFF0000UL) | (ULONG)(val & 0xFFFE); break;
        case 0x04C: blt->bpt = (blt->bpt & 0x0000FFFFUL) | ((ULONG)val << 16); break;
        case 0x04E: blt->bpt = (blt->bpt & 0xFFFF0000UL) | (ULONG)(val & 0xFFFE); break;
        case 0x050: blt->apt = (blt->apt & 0x0000FFFFUL) | ((ULONG)val << 16); break;
        case 0x052: blt->apt = (blt->apt & 0xFFFF0000UL) | (ULONG)(val & 0xFFFE); break;
        case 0x054: blt->dpt = (blt->dpt & 0x0000FFFFUL) | ((ULONG)val << 16); break;
        case 0x056: blt->dpt = (blt->dpt & 0xFFFF0000UL) | (ULONG)(val & 0xFFFE); break;
        case 0x058: /* BLTSIZE: H 15-6 (0 = 1024), W 5-0 (0 = 64) */
            return Kaffeine_BlitterStart(ctx, (val & 0x3F) ? (ULONG)(val & 0x3F) : 64,
                                         (val >> 6) ? (ULONG)(val >> 6) : 1024);
        case 0x05A: blt->con0 = (UWORD)((blt->con0 & 0xFF00) | (val & 0x00FF)); break;
        case 0x05C: blt->sizv = (UWORD)(val & 0x7FFF); break;
        case 0x05E: /* BLTSIZH: W 10-0 (0 = 2048), H from BLTSIZV (0 = 32768) */
            return Kaffeine_BlitterStart(ctx, (val & 0x7FF) ? (ULONG)(val & 0x7FF) : 2048,
                                         blt->sizv ? (ULONG)blt->sizv : 32768);
        case 0x060: blt->cmod = (WORD)(val & 0xFFFE); break;
        case 0x062: blt->bmod = (WORD)(val & 0xFFFE); break;
        case 0x064: blt->amod = (WORD)(val & 0xFFFE); break;
        case 0x066: blt->dmod = (WORD)(val & 0xFFFE); break;
        case 0x070: blt->cdat = val; break;
        case 0x072:
            blt->bdat = val;
            blt->bhold = (blt->con1 & KAFFEINE_BLT_DESC) ? (UWORD)(val << (blt->con1 >> 12))
                                                         : (UWORD)(val >> (blt->con1 >> 12));
            break;
        case 0x074: blt->adat = val; break;
        case 0x096: /* DMACON */
        {
            const UWORD on = KAFFEINE_DMACON_DMAEN | KAFFEINE_DMACON_BLTEN;

            if (val & KAFFEINE_DMACON_SETCLR)
                blt->dmacon = (UWORD)(blt->dmacon | (val & KAFFEINE_DMACON_WRITABLE));
            else
                blt->dmacon = (UWORD)(blt->dmacon & ~(val & KAFFEINE_DMACON_WRITABLE));
            if (blt->pending && (blt->dmacon & on) == on)
            {
                blt->pending = 0;
                return Kaffeine_BlitterRun(ctx, blt->pend_w, blt->pend_h);
            }
            break;
        }
        default:
            break;
    }
    return 0;
}

/* DMACONR: the DMACON bits, BBUSY while a blit waits for DMA, BZERO */
static inline UWORD Kaffeine_BlitterDmaconr(const struct KaffeineContext *ctx)
{
    const struct KaffeineBlitter *blt = &ctx->blitter;

    return (UWORD)((blt->dmacon & KAFFEINE_DMACON_WRITABLE) |
                   (blt->pending ? KAFFEINE_DMACON_BBUSY : 0) |
                   (blt->zero ? KAFFEINE_DMACON_BZERO : 0));
}

#endif /* KAFFEINE_BLITTER_H */
