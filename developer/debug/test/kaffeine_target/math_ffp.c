/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    68k mathffp.library probe and regression test.

    Built for the 68000 without FPU or C floating-point operations. Every
    Motorola Fast Floating Point (FFP) value is represented as an exact 32-bit
    ULONG bit pattern:
        bits 31..8 : 24-bit normalized unsigned mantissa M in [0x800000, 0xFFFFFF],
                     representing fraction f = M / 2^24 in [0.5, 1.0)
        bit  7     : sign S (0 = positive, 1 = negative)
        bits 6..0  : 7-bit excess-64 exponent E (power of two is 2^(E - 64))
        value      : (-1)^S * (M / 2^24) * 2^(E - 64) = (-1)^S * M * 2^(E - 88)
        zero       : canonical 0x00000000

    All twelve v33 mathffp.library vectors (-30 through -96) are invoked via
    a top-level assembly wrapper that loads D0/D1, seeds D2-D7 and A2-A5 with
    distinct sentinels, sets CCR (0x00 and 0x1F), calls the vector through A6,
    and captures Z/N/V/C with Scc and X with ADDX before any flag-modifying
    instruction executes.

    Default mode runs deterministic m68ktest checks on well-defined cases and
    verifies D2-D7/A2-A5 preservation. Passing any command-line argument
    (e.g. "math_ffp probe" or "math_ffp raw") selects raw probe mode, which
    prints one hexadecimal record per call (for both incoming CCR=0x00 and
    CCR=0x1F) across all normal and edge-probe cases without baking unmeasured
    assumptions into the test.
*/

#include <stddef.h>
#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "m68ktest.h"

/* 68000 CCR bit masks */
#define CC_X 0x10U
#define CC_N 0x08U
#define CC_Z 0x04U
#define CC_V 0x02U
#define CC_C 0x01U
#define CC_ALL (CC_X | CC_N | CC_Z | CC_V | CC_C)

/* Literal mathffp.library LVOs (bias -30, step -6, v33 has 12 vectors) */
#define LVO_SPFix   (-30)
#define LVO_SPFlt   (-36)
#define LVO_SPCmp   (-42)
#define LVO_SPTst   (-48)
#define LVO_SPAbs   (-54)
#define LVO_SPNeg   (-60)
#define LVO_SPAdd   (-66)
#define LVO_SPSub   (-72)
#define LVO_SPMul   (-78)
#define LVO_SPDiv   (-84)
#define LVO_SPFloor (-90)
#define LVO_SPCeil  (-96)

/* Preserved-register sentinel values */
#define SENTINEL_D0 0xD0D0D0D0UL
#define SENTINEL_D1 0xD1D1D1D1UL
#define SENTINEL_D2 0xD2D2D2D2UL
#define SENTINEL_D3 0xD3D3D3D3UL
#define SENTINEL_D4 0xD4D4D4D4UL
#define SENTINEL_D5 0xD5D5D5D5UL
#define SENTINEL_D6 0xD6D6D6D6UL
#define SENTINEL_D7 0xD7D7D7D7UL
#define SENTINEL_A2 0xA2A2A2A2UL
#define SENTINEL_A3 0xA3A3A3A3UL
#define SENTINEL_A4 0xA4A4A4A4UL
#define SENTINEL_A5 0xA5A5A5A5UL

/*
    Exact Motorola FFP bit patterns:
      value = (-1)^S * (M / 2^24) * 2^(E - 64)
*/
#define FFP_ZERO        0x00000000UL /*  0.0                         */
#define FFP_P0_25       0x8000003FUL /* +0.25      (M=0x800000 E=63) */
#define FFP_N0_25       0x800000BFUL /* -0.25      (M=0x800000 E=63) */
#define FFP_P0_5        0x80000040UL /* +0.5       (M=0x800000 E=64) */
#define FFP_N0_5        0x800000C0UL /* -0.5       (M=0x800000 E=64) */
#define FFP_P0_75       0xC0000040UL /* +0.75      (M=0xC00000 E=64) */
#define FFP_N0_75       0xC00000C0UL /* -0.75      (M=0xC00000 E=64) */
#define FFP_P1          0x80000041UL /* +1.0       (M=0x800000 E=65) */
#define FFP_N1          0x800000C1UL /* -1.0       (M=0x800000 E=65) */
#define FFP_P1_25       0xA0000041UL /* +1.25      (M=0xA00000 E=65) */
#define FFP_N1_25       0xA00000C1UL /* -1.25      (M=0xA00000 E=65) */
#define FFP_P1_5        0xC0000041UL /* +1.5       (M=0xC00000 E=65) */
#define FFP_N1_5        0xC00000C1UL /* -1.5       (M=0xC00000 E=65) */
#define FFP_P1_75       0xE0000041UL /* +1.75      (M=0xE00000 E=65) */
#define FFP_N1_75       0xE00000C1UL /* -1.75      (M=0xE00000 E=65) */
#define FFP_P2          0x80000042UL /* +2.0       (M=0x800000 E=66) */
#define FFP_N2          0x800000C2UL /* -2.0       (M=0x800000 E=66) */
#define FFP_P2_5        0xA0000042UL /* +2.5       (M=0xA00000 E=66) */
#define FFP_N2_5        0xA00000C2UL /* -2.5       (M=0xA00000 E=66) */
#define FFP_P3          0xC0000042UL /* +3.0       (M=0xC00000 E=66) */
#define FFP_N3          0xC00000C2UL /* -3.0       (M=0xC00000 E=66) */
#define FFP_P3_5        0xE0000042UL /* +3.5       (M=0xE00000 E=66) */
#define FFP_N3_5        0xE00000C2UL /* -3.5       (M=0xE00000 E=66) */
#define FFP_P4          0x80000043UL /* +4.0       (M=0x800000 E=67) */
#define FFP_N4          0x800000C3UL /* -4.0       (M=0x800000 E=67) */
#define FFP_P5          0xA0000043UL /* +5.0       (M=0xA00000 E=67) */
#define FFP_N5          0xA00000C3UL /* -5.0       (M=0xA00000 E=67) */
#define FFP_P6          0xC0000043UL /* +6.0       (M=0xC00000 E=67) */
#define FFP_N6          0xC00000C3UL /* -6.0       (M=0xC00000 E=67) */
#define FFP_P7          0xE0000043UL /* +7.0       (M=0xE00000 E=67) */
#define FFP_N7          0xE00000C3UL /* -7.0       (M=0xE00000 E=67) */
#define FFP_P10         0xA0000044UL /* +10.0      (M=0xA00000 E=68) */
#define FFP_N10         0xA00000C4UL /* -10.0      (M=0xA00000 E=68) */
#define FFP_P42         0xA8000046UL /* +42.0      (M=0xA80000 E=70) */
#define FFP_N42         0xA80000C6UL /* -42.0      (M=0xA80000 E=70) */
#define FFP_P16777215   0xFFFFFF58UL /* +16777215  (2^24 - 1, E=88)  */
#define FFP_N16777215   0xFFFFFFD8UL /* -16777215 -(2^24 - 1, E=88)  */
#define FFP_P16777216   0x80000059UL /* +16777216  (2^24, E=89)      */
#define FFP_N16777216   0x800000D9UL /* -16777216 -(2^24, E=89)      */
#define FFP_P2147483520 0xFFFFFF5FUL /* +2147483520 (2^31 - 128)     */
#define FFP_P2147483648 0x80000060UL /* +2147483648 (+2^31)          */
#define FFP_N2147483648 0x800000E0UL /* -2147483648 (-2^31)          */
#define FFP_MIN_POS     0x80000000UL /* E=0 raw probe below positive normal range */
#define FFP_MIN_NEG     0x80000080UL /* -2^-65     (M=0x800000 E=0)  */
#define FFP_MAX_POS     0xFFFFFF7FUL /* +(1-2^-24)*2^63 (E=127, S=0) */
#define FFP_MAX_NEG     0xFFFFFFFFUL /* -(1-2^-24)*2^63 (E=127, S=1) */

/* Offsets are used directly by the assembly helper below. */
struct FFPCall
{
    ULONG in_d0;      /*  0 */
    ULONG in_d1;      /*  4 */
    UWORD in_ccr;     /*  8 */
    UWORD pad0;       /* 10 */
    ULONG d0;         /* 12 */
    ULONG d1;         /* 16 */
    UBYTE z;          /* 20, from SEQ */
    UBYTE n;          /* 21, from SMI */
    UBYTE v;          /* 22, from SVS */
    UBYTE c;          /* 23, from SCS */
    UBYTE x;          /* 24, from ADDX */
    UBYTE pad1[3];    /* 25..27 */
    ULONG d2;         /* 28 */
    ULONG d3;         /* 32 */
    ULONG d4;         /* 36 */
    ULONG d5;         /* 40 */
    ULONG d6;         /* 44 */
    ULONG d7;         /* 48 */
    ULONG a2;         /* 52 */
    ULONG a3;         /* 56 */
    ULONG a4;         /* 60 */
    ULONG a5;         /* 64 */
};

/* Compile-time layout assertions for every assembly structure offset. */
#define ASSERT_FFPCALL_OFFSET(field, expected) \
    typedef char assert_ffpcall_##field[(offsetof(struct FFPCall, field) == (expected)) ? 1 : -1]

ASSERT_FFPCALL_OFFSET(in_d0,   0);
ASSERT_FFPCALL_OFFSET(in_d1,   4);
ASSERT_FFPCALL_OFFSET(in_ccr,  8);
ASSERT_FFPCALL_OFFSET(pad0,   10);
ASSERT_FFPCALL_OFFSET(d0,     12);
ASSERT_FFPCALL_OFFSET(d1,     16);
ASSERT_FFPCALL_OFFSET(z,      20);
ASSERT_FFPCALL_OFFSET(n,      21);
ASSERT_FFPCALL_OFFSET(v,      22);
ASSERT_FFPCALL_OFFSET(c,      23);
ASSERT_FFPCALL_OFFSET(x,      24);
ASSERT_FFPCALL_OFFSET(pad1,   25);
ASSERT_FFPCALL_OFFSET(d2,     28);
ASSERT_FFPCALL_OFFSET(d3,     32);
ASSERT_FFPCALL_OFFSET(d4,     36);
ASSERT_FFPCALL_OFFSET(d5,     40);
ASSERT_FFPCALL_OFFSET(d6,     44);
ASSERT_FFPCALL_OFFSET(d7,     48);
ASSERT_FFPCALL_OFFSET(a2,     52);
ASSERT_FFPCALL_OFFSET(a3,     56);
ASSERT_FFPCALL_OFFSET(a4,     60);
ASSERT_FFPCALL_OFFSET(a5,     64);
typedef char assert_ffpcall_size[(sizeof(struct FFPCall) == 68) ? 1 : -1];

void ffp_call(struct Library *base, LONG lvo, struct FFPCall *call);

/*
    Stack layout after movem.l d2-d7/a2-a6,-(sp) (44 bytes) and subq.l #8,sp:
      (0..3,sp) : scratch bytes for SEQ/SMI/SVS/SCS
      (4..7,sp) : scratch longword for D7 before X capture
      (8..51,sp): saved caller D2-D7/A2-A6
      (52,sp)   : return PC
      (56,sp)   : struct Library *base
      (60,sp)   : LONG lvo (negative byte offset from base)
      (64,sp)   : struct FFPCall *call

    Using A0 as the index register in jsr (0,a6,a0.l) leaves D2-D7 and A2-A5
    all holding live sentinel values across the library call.
*/
__asm__(
"	.text\n"
"	.align	2\n"
"	.globl	_ffp_call\n"
"	.globl	ffp_call\n"
"_ffp_call:\n"
"ffp_call:\n"
"	movem.l d2-d7/a2-a6,-(sp)\n"
"	subq.l #8,sp\n"
"	move.l (56,sp),a6\n"
"	move.l (60,sp),a0\n"
"	move.l (64,sp),a1\n"
"	move.l (0,a1),d0\n"
"	move.l (4,a1),d1\n"
"	move.l #0xD2D2D2D2,d2\n"
"	move.l #0xD3D3D3D3,d3\n"
"	move.l #0xD4D4D4D4,d4\n"
"	move.l #0xD5D5D5D5,d5\n"
"	move.l #0xD6D6D6D6,d6\n"
"	move.l #0xD7D7D7D7,d7\n"
"	move.l #0xA2A2A2A2,a2\n"
"	move.l #0xA3A3A3A3,a3\n"
"	move.l #0xA4A4A4A4,a4\n"
"	move.l #0xA5A5A5A5,a5\n"
"	move.w (8,a1),ccr\n"
"	jsr (0,a6,a0.l)\n"
"	seq (0,sp)\n"
"	smi (1,sp)\n"
"	svs (2,sp)\n"
"	scs (3,sp)\n"
"	move.l d7,(4,sp)\n"
"	moveq #0,d7\n"
"	addx.l d7,d7\n"
"	move.l (64,sp),a1\n"
"	move.l d0,(12,a1)\n"
"	move.l d1,(16,a1)\n"
"	move.b (0,sp),(20,a1)\n"
"	move.b (1,sp),(21,a1)\n"
"	move.b (2,sp),(22,a1)\n"
"	move.b (3,sp),(23,a1)\n"
"	move.b d7,(24,a1)\n"
"	move.l d2,(28,a1)\n"
"	move.l d3,(32,a1)\n"
"	move.l d4,(36,a1)\n"
"	move.l d5,(40,a1)\n"
"	move.l d6,(44,a1)\n"
"	move.l (4,sp),(48,a1)\n"
"	move.l a2,(52,a1)\n"
"	move.l a3,(56,a1)\n"
"	move.l a4,(60,a1)\n"
"	move.l a5,(64,a1)\n"
"	addq.l #8,sp\n"
"	movem.l (sp)+,d2-d7/a2-a6\n"
"	rts\n");

struct FFPCase
{
    WORD lvo;
    const char *func;
    const char *what;
    ULONG d0;
    ULONG d1;
    UBYTE raw_only;
    ULONG expected_d0;
};

static const struct FFPCase cases[] =
{
    /*
        SPFix (LVO -30): D0 = fnum -> D0 = truncated signed 32-bit integer
    */
    { LVO_SPFix, "SPFix", "SPFix(0.0) -> 0",
      FFP_ZERO, SENTINEL_D1, 0, 0x00000000UL },
    { LVO_SPFix, "SPFix", "SPFix(1.0) -> 1",
      FFP_P1, SENTINEL_D1, 0, 0x00000001UL },
    { LVO_SPFix, "SPFix", "SPFix(-1.0) -> -1",
      FFP_N1, SENTINEL_D1, 0, 0xFFFFFFFFUL },
    { LVO_SPFix, "SPFix", "SPFix(0.5) -> 0",
      FFP_P0_5, SENTINEL_D1, 0, 0x00000000UL },
    { LVO_SPFix, "SPFix", "SPFix(-0.5) -> 0",
      FFP_N0_5, SENTINEL_D1, 0, 0x00000000UL },
    { LVO_SPFix, "SPFix", "SPFix(1.75) -> 1",
      FFP_P1_75, SENTINEL_D1, 0, 0x00000001UL },
    { LVO_SPFix, "SPFix", "SPFix(-1.75) -> -1",
      FFP_N1_75, SENTINEL_D1, 0, 0xFFFFFFFFUL },
    { LVO_SPFix, "SPFix", "SPFix(42.0) -> 42",
      FFP_P42, SENTINEL_D1, 0, 0x0000002AUL },
    { LVO_SPFix, "SPFix", "SPFix(16777215.0) -> 16777215",
      FFP_P16777215, SENTINEL_D1, 0, 0x00FFFFFFUL },
    { LVO_SPFix, "SPFix", "SPFix(-16777215.0) -> -16777215",
      FFP_N16777215, SENTINEL_D1, 0, 0xFF000001UL },
    { LVO_SPFix, "SPFix", "SPFix(2147483520.0) -> 2147483520",
      FFP_P2147483520, SENTINEL_D1, 0, 0x7FFFFF80UL },
    { LVO_SPFix, "SPFix", "SPFix(-2^31 boundary probe)",
      FFP_N2147483648, SENTINEL_D1, 1, 0x80000000UL },
    { LVO_SPFix, "SPFix", "SPFix(+2^31 overflow probe)",
      FFP_P2147483648, SENTINEL_D1, 1, 0x00000000UL },
    { LVO_SPFix, "SPFix", "SPFix(FFP_MAX_POS overflow probe)",
      FFP_MAX_POS, SENTINEL_D1, 1, 0x00000000UL },
    { LVO_SPFix, "SPFix", "SPFix(FFP_MAX_NEG overflow probe)",
      FFP_MAX_NEG, SENTINEL_D1, 1, 0x00000000UL },

    /*
        SPFlt (LVO -36): D0 = signed 32-bit integer -> D0 = FFP float
    */
    { LVO_SPFlt, "SPFlt", "SPFlt(0) -> 0.0",
      0x00000000UL, SENTINEL_D1, 0, FFP_ZERO },
    { LVO_SPFlt, "SPFlt", "SPFlt(1) -> 1.0",
      0x00000001UL, SENTINEL_D1, 0, FFP_P1 },
    { LVO_SPFlt, "SPFlt", "SPFlt(-1) -> -1.0",
      0xFFFFFFFFUL, SENTINEL_D1, 0, FFP_N1 },
    { LVO_SPFlt, "SPFlt", "SPFlt(2) -> 2.0",
      0x00000002UL, SENTINEL_D1, 0, FFP_P2 },
    { LVO_SPFlt, "SPFlt", "SPFlt(3) -> 3.0",
      0x00000003UL, SENTINEL_D1, 0, FFP_P3 },
    { LVO_SPFlt, "SPFlt", "SPFlt(-42) -> -42.0",
      0xFFFFFFD6UL, SENTINEL_D1, 0, FFP_N42 },
    { LVO_SPFlt, "SPFlt", "SPFlt(16777215) -> 2^24-1",
      0x00FFFFFFUL, SENTINEL_D1, 0, FFP_P16777215 },
    { LVO_SPFlt, "SPFlt", "SPFlt(-16777215) -> -(2^24-1)",
      0xFF000001UL, SENTINEL_D1, 0, FFP_N16777215 },
    { LVO_SPFlt, "SPFlt", "SPFlt(16777216) -> 2^24",
      0x01000000UL, SENTINEL_D1, 0, FFP_P16777216 },
    { LVO_SPFlt, "SPFlt", "SPFlt(2147483520) -> 2^31-128",
      0x7FFFFF80UL, SENTINEL_D1, 0, FFP_P2147483520 },
    { LVO_SPFlt, "SPFlt", "SPFlt(-2147483648) -> -2^31",
      0x80000000UL, SENTINEL_D1, 0, FFP_N2147483648 },
    { LVO_SPFlt, "SPFlt", "SPFlt(2^24+1 rounding probe)",
      0x01000001UL, SENTINEL_D1, 1, FFP_P16777216 },
    { LVO_SPFlt, "SPFlt", "SPFlt(-(2^24+1) rounding probe)",
      0xFEFFFFFFUL, SENTINEL_D1, 1, FFP_N16777216 },
    { LVO_SPFlt, "SPFlt", "SPFlt(INT32_MAX rounding probe)",
      0x7FFFFFFFUL, SENTINEL_D1, 1, FFP_P2147483520 },

    /*
        SPTst (LVO -48): D1 = fnum -> D0 = -1, 0, +1
    */
    { LVO_SPTst, "SPTst", "SPTst(0.0) -> 0",
      SENTINEL_D0, FFP_ZERO, 0, 0x00000000UL },
    { LVO_SPTst, "SPTst", "SPTst(1.0) -> +1",
      SENTINEL_D0, FFP_P1, 0, 0x00000001UL },
    { LVO_SPTst, "SPTst", "SPTst(-1.0) -> -1",
      SENTINEL_D0, FFP_N1, 0, 0xFFFFFFFFUL },
    { LVO_SPTst, "SPTst", "SPTst(0.5) -> +1",
      SENTINEL_D0, FFP_P0_5, 0, 0x00000001UL },
    { LVO_SPTst, "SPTst", "SPTst(-0.5) -> -1",
      SENTINEL_D0, FFP_N0_5, 0, 0xFFFFFFFFUL },
    { LVO_SPTst, "SPTst", "SPTst(FFP_MAX_POS) -> +1",
      SENTINEL_D0, FFP_MAX_POS, 0, 0x00000001UL },
    { LVO_SPTst, "SPTst", "SPTst(FFP_MAX_NEG) -> -1",
      SENTINEL_D0, FFP_MAX_NEG, 0, 0xFFFFFFFFUL },
    { LVO_SPTst, "SPTst", "SPTst(FFP_MIN_POS E=0 probe)",
      SENTINEL_D0, FFP_MIN_POS, 1, 0x00000001UL },
    { LVO_SPTst, "SPTst", "SPTst(FFP_MIN_NEG E=0 probe)",
      SENTINEL_D0, FFP_MIN_NEG, 1, 0xFFFFFFFFUL },

    /*
        SPCmp (LVO -42): D1 = fnum1, D0 = fnum2 -> D0 = -1 (D1<D0), 0, +1 (D1>D0)
    */
    { LVO_SPCmp, "SPCmp", "SPCmp(D1=0.0, D0=0.0) -> 0",
      FFP_ZERO, FFP_ZERO, 0, 0x00000000UL },
    { LVO_SPCmp, "SPCmp", "SPCmp(D1=1.0, D0=1.0) -> 0",
      FFP_P1, FFP_P1, 0, 0x00000000UL },
    { LVO_SPCmp, "SPCmp", "SPCmp(D1=-1.5, D0=-1.5) -> 0",
      FFP_N1_5, FFP_N1_5, 0, 0x00000000UL },
    { LVO_SPCmp, "SPCmp", "SPCmp(D1=2.0, D0=1.0) -> +1",
      FFP_P1, FFP_P2, 0, 0x00000001UL },
    { LVO_SPCmp, "SPCmp", "SPCmp(D1=1.0, D0=2.0) -> -1",
      FFP_P2, FFP_P1, 0, 0xFFFFFFFFUL },
    { LVO_SPCmp, "SPCmp", "SPCmp(D1=1.5, D0=1.0) -> +1",
      FFP_P1, FFP_P1_5, 0, 0x00000001UL },
    { LVO_SPCmp, "SPCmp", "SPCmp(D1=1.0, D0=1.5) -> -1",
      FFP_P1_5, FFP_P1, 0, 0xFFFFFFFFUL },
    { LVO_SPCmp, "SPCmp", "SPCmp(D1=1.0, D0=-1.0) -> +1",
      FFP_N1, FFP_P1, 0, 0x00000001UL },
    { LVO_SPCmp, "SPCmp", "SPCmp(D1=-1.0, D0=1.0) -> -1",
      FFP_P1, FFP_N1, 0, 0xFFFFFFFFUL },
    { LVO_SPCmp, "SPCmp", "SPCmp(D1=-1.0, D0=-2.0) -> +1",
      FFP_N2, FFP_N1, 0, 0x00000001UL },
    { LVO_SPCmp, "SPCmp", "SPCmp(D1=-2.0, D0=-1.0) -> -1",
      FFP_N1, FFP_N2, 0, 0xFFFFFFFFUL },
    { LVO_SPCmp, "SPCmp", "SPCmp(D1=-1.0, D0=-1.5) -> +1",
      FFP_N1_5, FFP_N1, 0, 0x00000001UL },

    /*
        SPAbs (LVO -54): D0 = fnum -> D0 = |fnum|
    */
    { LVO_SPAbs, "SPAbs", "SPAbs(0.0) -> 0.0",
      FFP_ZERO, SENTINEL_D1, 0, FFP_ZERO },
    { LVO_SPAbs, "SPAbs", "SPAbs(1.0) -> 1.0",
      FFP_P1, SENTINEL_D1, 0, FFP_P1 },
    { LVO_SPAbs, "SPAbs", "SPAbs(-1.0) -> 1.0",
      FFP_N1, SENTINEL_D1, 0, FFP_P1 },
    { LVO_SPAbs, "SPAbs", "SPAbs(-0.5) -> 0.5",
      FFP_N0_5, SENTINEL_D1, 0, FFP_P0_5 },
    { LVO_SPAbs, "SPAbs", "SPAbs(FFP_MAX_NEG) -> FFP_MAX_POS",
      FFP_MAX_NEG, SENTINEL_D1, 0, FFP_MAX_POS },

    /*
        SPNeg (LVO -60): D0 = fnum -> D0 = -fnum
    */
    { LVO_SPNeg, "SPNeg", "SPNeg(0.0) -> 0.0",
      FFP_ZERO, SENTINEL_D1, 0, FFP_ZERO },
    { LVO_SPNeg, "SPNeg", "SPNeg(1.0) -> -1.0",
      FFP_P1, SENTINEL_D1, 0, FFP_N1 },
    { LVO_SPNeg, "SPNeg", "SPNeg(-1.0) -> 1.0",
      FFP_N1, SENTINEL_D1, 0, FFP_P1 },
    { LVO_SPNeg, "SPNeg", "SPNeg(FFP_MAX_POS) -> FFP_MAX_NEG",
      FFP_MAX_POS, SENTINEL_D1, 0, FFP_MAX_NEG },
    { LVO_SPNeg, "SPNeg", "SPNeg(FFP_MAX_NEG) -> FFP_MAX_POS",
      FFP_MAX_NEG, SENTINEL_D1, 0, FFP_MAX_POS },

    /*
        SPAdd (LVO -66): D1 = fnum1, D0 = fnum2 -> D0 = fnum1 + fnum2
    */
    { LVO_SPAdd, "SPAdd", "SPAdd(D1=0.0, D0=0.0) -> 0.0",
      FFP_ZERO, FFP_ZERO, 0, FFP_ZERO },
    { LVO_SPAdd, "SPAdd", "SPAdd(D1=1.0, D0=0.0) -> 1.0",
      FFP_ZERO, FFP_P1, 0, FFP_P1 },
    { LVO_SPAdd, "SPAdd", "SPAdd(D1=1.0, D0=2.0) -> 3.0",
      FFP_P2, FFP_P1, 0, FFP_P3 },
    { LVO_SPAdd, "SPAdd", "SPAdd(D1=0.5, D0=0.25) -> 0.75",
      FFP_P0_25, FFP_P0_5, 0, FFP_P0_75 },
    { LVO_SPAdd, "SPAdd", "SPAdd(D1=1.5, D0=2.5) -> 4.0",
      FFP_P2_5, FFP_P1_5, 0, FFP_P4 },
    { LVO_SPAdd, "SPAdd", "SPAdd(D1=1.0, D0=-1.0) -> 0.0",
      FFP_N1, FFP_P1, 0, FFP_ZERO },
    { LVO_SPAdd, "SPAdd", "SPAdd(D1=-2.0, D0=-3.0) -> -5.0",
      FFP_N3, FFP_N2, 0, FFP_N5 },
    { LVO_SPAdd, "SPAdd", "SPAdd(D1=1.0, D0=-3.0) -> -2.0",
      FFP_N3, FFP_P1, 0, FFP_N2 },
    { LVO_SPAdd, "SPAdd", "SPAdd(D1=2^24-1, D0=1.0) -> 2^24",
      FFP_P1, FFP_P16777215, 0, FFP_P16777216 },
    { LVO_SPAdd, "SPAdd", "SPAdd(FFP_MAX_POS, FFP_MAX_POS overflow probe)",
      FFP_MAX_POS, FFP_MAX_POS, 1, FFP_MAX_POS },
    { LVO_SPAdd, "SPAdd", "SPAdd(FFP_MAX_NEG, FFP_MAX_NEG overflow probe)",
      FFP_MAX_NEG, FFP_MAX_NEG, 1, FFP_MAX_NEG },

    /*
        SPSub (LVO -72): D1 = fnum1, D0 = fnum2 -> D0 = fnum2 - fnum1 (D0 - D1)
    */
    { LVO_SPSub, "SPSub", "SPSub(D0=0.0, D1=0.0) -> 0.0",
      FFP_ZERO, FFP_ZERO, 0, FFP_ZERO },
    { LVO_SPSub, "SPSub", "SPSub(D0=3.0, D1=1.0) -> 2.0",
      FFP_P3, FFP_P1, 0, FFP_P2 },
    { LVO_SPSub, "SPSub", "SPSub(D0=1.0, D1=3.0) -> -2.0",
      FFP_P1, FFP_P3, 0, FFP_N2 },
    { LVO_SPSub, "SPSub", "SPSub(D0=1.5, D1=1.5) -> 0.0",
      FFP_P1_5, FFP_P1_5, 0, FFP_ZERO },
    { LVO_SPSub, "SPSub", "SPSub(D0=0.5, D1=1.25) -> -0.75",
      FFP_P0_5, FFP_P1_25, 0, FFP_N0_75 },
    { LVO_SPSub, "SPSub", "SPSub(D0=-1.0, D1=2.0) -> -3.0",
      FFP_N1, FFP_P2, 0, FFP_N3 },
    { LVO_SPSub, "SPSub", "SPSub(D0=0.0, D1=1.5) -> -1.5",
      FFP_ZERO, FFP_P1_5, 0, FFP_N1_5 },
    { LVO_SPSub, "SPSub", "SPSub(FFP_MAX_POS, FFP_MAX_NEG overflow probe)",
      FFP_MAX_POS, FFP_MAX_NEG, 1, FFP_MAX_POS },

    /*
        SPMul (LVO -78): D1 = fnum1, D0 = fnum2 -> D0 = fnum1 * fnum2
    */
    { LVO_SPMul, "SPMul", "SPMul(D1=0.0, D0=1.0) -> 0.0",
      FFP_P1, FFP_ZERO, 0, FFP_ZERO },
    { LVO_SPMul, "SPMul", "SPMul(D1=1.0, D0=1.0) -> 1.0",
      FFP_P1, FFP_P1, 0, FFP_P1 },
    { LVO_SPMul, "SPMul", "SPMul(D1=1.0, D0=-1.0) -> -1.0",
      FFP_N1, FFP_P1, 0, FFP_N1 },
    { LVO_SPMul, "SPMul", "SPMul(D1=-1.5, D0=-2.0) -> 3.0",
      FFP_N2, FFP_N1_5, 0, FFP_P3 },
    { LVO_SPMul, "SPMul", "SPMul(D1=2.0, D0=3.0) -> 6.0",
      FFP_P3, FFP_P2, 0, FFP_P6 },
    { LVO_SPMul, "SPMul", "SPMul(D1=0.5, D0=0.5) -> 0.25",
      FFP_P0_5, FFP_P0_5, 0, FFP_P0_25 },
    { LVO_SPMul, "SPMul", "SPMul(D1=2.5, D0=4.0) -> 10.0",
      FFP_P4, FFP_P2_5, 0, FFP_P10 },
    { LVO_SPMul, "SPMul", "SPMul(FFP_MAX_POS, 2.0 overflow probe)",
      FFP_P2, FFP_MAX_POS, 1, FFP_MAX_POS },
    { LVO_SPMul, "SPMul", "SPMul(FFP_MIN_POS, 0.5 underflow probe)",
      FFP_P0_5, FFP_MIN_POS, 1, FFP_ZERO },

    /*
        SPDiv (LVO -84): D1 = fnum1, D0 = fnum2 -> D0 = fnum2 / fnum1 (D0 / D1)
    */
    { LVO_SPDiv, "SPDiv", "SPDiv(D0=0.0, D1=1.0) -> 0.0",
      FFP_ZERO, FFP_P1, 0, FFP_ZERO },
    { LVO_SPDiv, "SPDiv", "SPDiv(D0=6.0, D1=2.0) -> 3.0",
      FFP_P6, FFP_P2, 0, FFP_P3 },
    { LVO_SPDiv, "SPDiv", "SPDiv(D0=1.0, D1=2.0) -> 0.5",
      FFP_P1, FFP_P2, 0, FFP_P0_5 },
    { LVO_SPDiv, "SPDiv", "SPDiv(D0=2.0, D1=1.0) -> 2.0",
      FFP_P2, FFP_P1, 0, FFP_P2 },
    { LVO_SPDiv, "SPDiv", "SPDiv(D0=3.0, D1=2.0) -> 1.5",
      FFP_P3, FFP_P2, 0, FFP_P1_5 },
    { LVO_SPDiv, "SPDiv", "SPDiv(D0=-7.0, D1=2.0) -> -3.5",
      FFP_N7, FFP_P2, 0, FFP_N3_5 },
    { LVO_SPDiv, "SPDiv", "SPDiv(D0=-3.0, D1=-1.5) -> 2.0",
      FFP_N3, FFP_N1_5, 0, FFP_P2 },
    { LVO_SPDiv, "SPDiv", "SPDiv(D0=1.0, D1=3.0 1/3 rounding probe)",
      FFP_P1, FFP_P3, 1, 0xAAAAAA3FUL },
    { LVO_SPDiv, "SPDiv", "SPDiv(D0=1.0, D1=0.0 pos div0 probe)",
      FFP_P1, FFP_ZERO, 1, FFP_MAX_POS },
    { LVO_SPDiv, "SPDiv", "SPDiv(D0=-1.0, D1=0.0 neg div0 probe)",
      FFP_N1, FFP_ZERO, 1, FFP_MAX_NEG },
    { LVO_SPDiv, "SPDiv", "SPDiv(FFP_MAX_POS, 0.5 overflow probe)",
      FFP_MAX_POS, FFP_P0_5, 1, FFP_MAX_POS },
    { LVO_SPDiv, "SPDiv", "SPDiv(FFP_MIN_POS, 2.0 underflow probe)",
      FFP_MIN_POS, FFP_P2, 1, FFP_ZERO },

    /*
        SPFloor (LVO -90): D0 = y -> D0 = floor(y)
    */
    { LVO_SPFloor, "SPFloor", "SPFloor(0.0) -> 0.0",
      FFP_ZERO, SENTINEL_D1, 0, FFP_ZERO },
    { LVO_SPFloor, "SPFloor", "SPFloor(1.0) -> 1.0",
      FFP_P1, SENTINEL_D1, 0, FFP_P1 },
    { LVO_SPFloor, "SPFloor", "SPFloor(0.5) -> 0.0",
      FFP_P0_5, SENTINEL_D1, 0, FFP_ZERO },
    { LVO_SPFloor, "SPFloor", "SPFloor(1.75) -> 1.0",
      FFP_P1_75, SENTINEL_D1, 0, FFP_P1 },
    { LVO_SPFloor, "SPFloor", "SPFloor(-0.5) -> -1.0",
      FFP_N0_5, SENTINEL_D1, 0, FFP_N1 },
    { LVO_SPFloor, "SPFloor", "SPFloor(-1.25) -> -2.0",
      FFP_N1_25, SENTINEL_D1, 0, FFP_N2 },

    /*
        SPCeil (LVO -96): D0 = y -> D0 = ceil(y)
    */
    { LVO_SPCeil, "SPCeil", "SPCeil(0.0) -> 0.0",
      FFP_ZERO, SENTINEL_D1, 0, FFP_ZERO },
    { LVO_SPCeil, "SPCeil", "SPCeil(-1.0) -> -1.0",
      FFP_N1, SENTINEL_D1, 0, FFP_N1 },
    { LVO_SPCeil, "SPCeil", "SPCeil(0.5) -> 1.0",
      FFP_P0_5, SENTINEL_D1, 0, FFP_P1 },
    { LVO_SPCeil, "SPCeil", "SPCeil(1.25) -> 2.0",
      FFP_P1_25, SENTINEL_D1, 0, FFP_P2 },
    { LVO_SPCeil, "SPCeil", "SPCeil(-0.5) -> 0.0",
      FFP_N0_5, SENTINEL_D1, 0, FFP_ZERO },
    { LVO_SPCeil, "SPCeil", "SPCeil(-1.75) -> -1.0",
      FFP_N1_75, SENTINEL_D1, 0, FFP_N1 }
};

#define NUM_CASES ((LONG)(sizeof(cases) / sizeof(cases[0])))

static ULONG pack_ccr(const struct FFPCall *call)
{
    ULONG ccr = 0;

    if (call->x)
        ccr |= CC_X;
    if (call->n)
        ccr |= CC_N;
    if (call->z)
        ccr |= CC_Z;
    if (call->v)
        ccr |= CC_V;
    if (call->c)
        ccr |= CC_C;
    return ccr;
}

static void format_ccr(ULONG ccr, char out[6])
{
    out[0] = (ccr & CC_X) ? 'X' : '-';
    out[1] = (ccr & CC_N) ? 'N' : '-';
    out[2] = (ccr & CC_Z) ? 'Z' : '-';
    out[3] = (ccr & CC_V) ? 'V' : '-';
    out[4] = (ccr & CC_C) ? 'C' : '-';
    out[5] = '\0';
}

/*
    Returns a 10-bit mask of clobbered preserved registers:
      bits 0..5 : D2..D7
      bits 6..9 : A2..A5
    0 means all ten preserved registers survived intact.
*/
static ULONG check_preserved_regs(const struct FFPCall *call)
{
    ULONG mask = 0;

    if (call->d2 != SENTINEL_D2) mask |= (1UL << 0);
    if (call->d3 != SENTINEL_D3) mask |= (1UL << 1);
    if (call->d4 != SENTINEL_D4) mask |= (1UL << 2);
    if (call->d5 != SENTINEL_D5) mask |= (1UL << 3);
    if (call->d6 != SENTINEL_D6) mask |= (1UL << 4);
    if (call->d7 != SENTINEL_D7) mask |= (1UL << 5);
    if (call->a2 != SENTINEL_A2) mask |= (1UL << 6);
    if (call->a3 != SENTINEL_A3) mask |= (1UL << 7);
    if (call->a4 != SENTINEL_A4) mask |= (1UL << 8);
    if (call->a5 != SENTINEL_A5) mask |= (1UL << 9);
    return mask;
}

static void invoke_case(struct Library *base, const struct FFPCase *c,
                        UWORD in_ccr, struct FFPCall *out)
{
    out->in_d0  = c->d0;
    out->in_d1  = c->d1;
    out->in_ccr = in_ccr;
    out->pad0   = 0;
    ffp_call(base, (LONG)c->lvo, out);
}

static void print_probe_record(LONG idx, const struct FFPCase *c,
                               const struct FFPCall *call)
{
    ULONG out_ccr = pack_ccr(call);
    ULONG clobber = check_preserved_regs(call);
    char in_str[6], out_str[6];

    format_ccr((ULONG)call->in_ccr, in_str);
    format_ccr(out_ccr, out_str);

    Printf("RAW %02ld %-7s lvo=%ld raw=%ld in_d0=%08lx in_d1=%08lx "
           "in_cc=%02lx(%s) out_d0=%08lx out_d1=%08lx out_cc=%02lx(%s) "
           "clob=%03lx d2..d7=%08lx/%08lx/%08lx/%08lx/%08lx/%08lx "
           "a2..a5=%08lx/%08lx/%08lx/%08lx # %s\n",
           idx,
           (LONG)c->func,
           (LONG)c->lvo,
           (LONG)c->raw_only,
           call->in_d0,
           call->in_d1,
           (ULONG)call->in_ccr,
           (LONG)in_str,
           call->d0,
           call->d1,
           out_ccr,
           (LONG)out_str,
           clobber,
           call->d2,
           call->d3,
           call->d4,
           call->d5,
           call->d6,
           call->d7,
           call->a2,
           call->a3,
           call->a4,
           call->a5,
           (LONG)c->what);
    test_flush();
}

/* Check only the documented conditions of the exact normal cases. D0/D1,
   A0/A1 and X/C are scratch in the native mathffp interface. SPCmp's
   relational conditions compare D0 with D1, opposite to its integer result;
   use N xor V, so a signed comparison overflow is not mistaken for N. */
static BOOL normal_flags_match(const struct FFPCase *c, const struct FFPCall *call)
{
    ULONG flags = pack_ccr(call), expected;

    if (c->lvo == LVO_SPCmp)
        return ((flags & CC_Z) != 0) == (c->expected_d0 == 0) &&
               (((flags & CC_N) != 0) != ((flags & CC_V) != 0)) ==
                   (c->expected_d0 == 1);
    if (c->expected_d0 == 0)
        expected = CC_Z;
    else if (c->lvo == LVO_SPFix || c->lvo == LVO_SPTst)
        expected = c->expected_d0 & 0x80000000UL ? CC_N : 0;
    else
        expected = c->expected_d0 & 0x80UL ? CC_N : 0;
    return (flags & (c->lvo == LVO_SPFloor || c->lvo == LVO_SPCeil ?
                     CC_N | CC_Z : CC_N | CC_Z | CC_V)) == expected;
}

int main(int argc, char **argv)
{
    struct Library *MathBase;
    BOOL probe_mode = FALSE;
    LONG i;

    if (argc > 1 && argv[1] && argv[1][0] != '\0')
        probe_mode = TRUE;

    MathBase = OpenLibrary((CONST_STRPTR)"mathffp.library", 33);
    if (!check(MathBase != NULL, "OpenLibrary(mathffp.library, 33)"))
        return test_done("math_ffp");

    Printf("# mathffp.library v%ld.%ld (%ld total cases, mode=%s)\n",
           (LONG)MathBase->lib_Version,
           (LONG)MathBase->lib_Revision,
           NUM_CASES,
           (LONG)(probe_mode ? "PROBE" : "TEST"));
    test_flush();

    for (i = 0; i < NUM_CASES; i++)
    {
        const struct FFPCase *c = &cases[i];
        struct FFPCall c0, c1;
        ULONG clob0, clob1;
        BOOL ok;

        invoke_case(MathBase, c, 0x0000U, &c0);
        invoke_case(MathBase, c, CC_ALL, &c1);

        if (probe_mode)
        {
            print_probe_record(i, c, &c0);
            print_probe_record(i, c, &c1);
        }

        clob0 = check_preserved_regs(&c0);
        clob1 = check_preserved_regs(&c1);

        if (c->raw_only)
        {
            /*
                For probe-only edge cases, do not bake unmeasured return-value
                or flag assumptions into normal mode; only verify that the C
                callee-saved registers D2-D7/A2-A5 survived both calls.
            */
            ok = (clob0 == 0 && clob1 == 0);
            if (!check(ok, c->what))
            {
                Printf("#   edge clobber: ccr00=%03lx ccr1f=%03lx "
                       "d0=%08lx/%08lx d1=%08lx/%08lx\n",
                       clob0, clob1, c0.d0, c1.d0, c0.d1, c1.d1);
                test_flush();
            }
        }
        else
        {
            ok = (c0.d0 == c->expected_d0) &&
                 (c1.d0 == c->expected_d0) &&
                 (clob0 == 0) &&
                 (clob1 == 0) &&
                 normal_flags_match(c, &c0) && normal_flags_match(c, &c1);
            if (!check(ok, c->what))
            {
                Printf("#   got d0=%08lx/%08lx exp=%08lx "
                       "ccr=%02lx/%02lx clob=%03lx/%03lx\n",
                       c0.d0, c1.d0, c->expected_d0,
                       pack_ccr(&c0), pack_ccr(&c1), clob0, clob1);
                test_flush();
            }
        }
    }

    CloseLibrary(MathBase);
    return test_done("math_ffp");
}
