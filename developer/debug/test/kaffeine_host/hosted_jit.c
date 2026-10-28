/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    A hosted AArch64 translation probe for kaffeine, behind struct
    KaffeineCPUOps (docs/kaffeine/hosted-jit-probe.md). It translates a small
    subset of 68k instructions into AArch64 blocks and runs them through the
    production run loop, library dispatcher and callback primitive. It was
    written for this experiment and contains no Emu68 code.
*/
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dos/dos.h>
#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_cpu.h"
#include "kaffeine_callback.h"
#include "kaffeine_dispatch.h"
#include "hosted_jit.h"

#if defined(__AROS__)
#include <aros/config.h>
#define HJ_AROS_STANDALONE (AROS_FLAVOUR == AROS_FLAVOUR_STANDALONE)
#else
#define HJ_AROS_STANDALONE 0
#endif

/* Hosted AROS has a different executable-memory contract. The AROS path
   below is only for the standalone flavour used by native AArch64. */
#if defined(__aarch64__) && (HJ_AROS_STANDALONE || \
    (!defined(__AROS__) && (defined(__APPLE__) || defined(__linux__))))
#define HJ_RUNS 1
#if defined(__AROS__)
#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>
#else
#include <sys/mman.h>
#if defined(__APPLE__)
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#endif
#endif
#else
#define HJ_RUNS 0
#endif

/* The program's return address, as in kaffeine_moira.cpp. */
#define SENTINEL_ADDR 0x000160u

/* ------------------------------------------------------------------
   A64 encodings. Register 31 is SP or ZR as each form defines it.
   ------------------------------------------------------------------ */

enum { A64_EQ, A64_NE, A64_HS, A64_LO, A64_MI, A64_PL, A64_VS, A64_VC,
       A64_HI, A64_LS, A64_GE, A64_LT, A64_GT, A64_LE, A64_AL };

#define ZR 31
#define SP 31

static uint32_t a64_addsub_imm(int sf, int sub, int s, int rd, int rn, uint32_t imm12, int lsl12)
{
    return (uint32_t)sf << 31 | (uint32_t)sub << 30 | (uint32_t)s << 29 | 0x11000000u |
           (uint32_t)lsl12 << 22 | (imm12 & 0xFFFu) << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}

/* shift: 0 LSL, 1 LSR, 2 ASR */
static uint32_t a64_addsub_reg(int sf, int sub, int s, int rd, int rn, int rm, int shift, int amount)
{
    return (uint32_t)sf << 31 | (uint32_t)sub << 30 | (uint32_t)s << 29 | 0x0B000000u |
           (uint32_t)shift << 22 | (uint32_t)rm << 16 | (uint32_t)(amount & 63) << 10 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}

/* opc: 0 AND, 1 ORR, 2 EOR, 3 ANDS; invert: BIC, ORN, EON, BICS */
static uint32_t a64_logic_reg(int sf, int opc, int invert, int rd, int rn, int rm, int shift, int amount)
{
    return (uint32_t)sf << 31 | (uint32_t)opc << 29 | 0x0A000000u | (uint32_t)shift << 22 |
           (uint32_t)invert << 21 | (uint32_t)rm << 16 | (uint32_t)(amount & 63) << 10 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}

/* N:immr:imms of a bitmask immediate, or -1 if the value has none. */
static int a64_bitmask(uint64_t value, int sf, uint32_t *field)
{
    unsigned size = 64, ones, r;
    uint64_t emask, elem, run;

    if (!sf)
    {
        value &= 0xFFFFFFFFu;
        value |= value << 32;
    }
    if (value == 0 || value == ~(uint64_t)0)
        return -1;
    while (size > 2)
    {
        unsigned half = size / 2;
        uint64_t mask = ((uint64_t)1 << half) - 1;

        if ((value & mask) != ((value >> half) & mask))
            break;
        size = half;
    }
    emask = size == 64 ? ~(uint64_t)0 : ((uint64_t)1 << size) - 1;
    elem = value & emask;
    ones = 0;
    for (r = 0; r < size; r++)
        ones += (unsigned)((elem >> r) & 1);
    run = ((uint64_t)1 << ones) - 1;
    for (r = 0; r < size; r++)
    {
        uint64_t rot = r ? ((elem >> r) | (elem << (size - r))) & emask : elem;

        if (rot == run)
        {
            uint32_t immr = (size - r) % size;
            uint32_t imms = ((~(size - 1) << 1) & 0x3Fu) | (ones - 1);

            *field = (uint32_t)(size == 64) << 12 | immr << 6 | imms;
            return 0;
        }
    }
    return -1;
}

static uint32_t a64_logic_imm(int sf, int opc, int rd, int rn, uint32_t field)
{
    return (uint32_t)sf << 31 | (uint32_t)opc << 29 | 0x12000000u | field << 10 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}

/* opc: 0 MOVN, 2 MOVZ, 3 MOVK */
static uint32_t a64_movw(int sf, int opc, int rd, uint32_t imm16, int hw)
{
    return (uint32_t)sf << 31 | (uint32_t)opc << 29 | 0x12800000u | (uint32_t)hw << 21 |
           (imm16 & 0xFFFFu) << 5 | (uint32_t)rd;
}

/* opc: 0 SBFM, 1 BFM, 2 UBFM */
static uint32_t a64_bitfield(int sf, int opc, int rd, int rn, int immr, int imms)
{
    return (uint32_t)sf << 31 | (uint32_t)opc << 29 | 0x13000000u | (uint32_t)sf << 22 |
           (uint32_t)(immr & 63) << 16 | (uint32_t)(imms & 63) << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}

static uint32_t a64_extr(int sf, int rd, int rn, int rm, int lsb)
{
    return (uint32_t)sf << 31 | 0x13800000u | (uint32_t)sf << 22 | (uint32_t)rm << 16 |
           (uint32_t)(lsb & 63) << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}

static uint32_t a64_rev32(int rd, int rn) { return 0x5AC00800u | (uint32_t)rn << 5 | (uint32_t)rd; }
static uint32_t a64_rev16(int rd, int rn) { return 0x5AC00400u | (uint32_t)rn << 5 | (uint32_t)rd; }
static uint32_t a64_clz(int rd, int rn) { return 0x5AC01000u | (uint32_t)rn << 5 | (uint32_t)rd; }

static uint32_t a64_csel(int sf, int rd, int rn, int rm, int cond)
{
    return (uint32_t)sf << 31 | 0x1A800000u | (uint32_t)rm << 16 | (uint32_t)cond << 12 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}

static uint32_t a64_csinc(int sf, int rd, int rn, int rm, int cond)
{
    return (uint32_t)sf << 31 | 0x1A800400u | (uint32_t)rm << 16 | (uint32_t)cond << 12 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}

static uint32_t a64_adc_sbc(int sf, int sub, int s, int rd, int rn, int rm)
{
    return (uint32_t)sf << 31 | (uint32_t)sub << 30 | (uint32_t)s << 29 | 0x1A000000u |
           (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rd;
}

/* op: 0 LSLV, 1 LSRV, 2 ASRV, 3 RORV */
static uint32_t a64_shiftv(int sf, int op, int rd, int rn, int rm)
{
    return (uint32_t)sf << 31 | 0x1AC02000u | (uint32_t)rm << 16 |
           (uint32_t)(op & 3) << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}

static uint32_t a64_lslv(int rd, int rn, int rm) { return a64_shiftv(0, 0, rd, rn, rm); }

static uint32_t a64_div(int sf, int is_signed, int rd, int rn, int rm)
{
    return (uint32_t)sf << 31 | 0x1AC00800u | (uint32_t)rm << 16 |
           (uint32_t)(is_signed ? 1 : 0) << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}

static uint32_t a64_mul(int rd, int rn, int rm)
{
    return 0x1B007C00u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rd;
}

static uint32_t a64_msub(int sf, int rd, int rn, int rm, int ra)
{
    return (uint32_t)sf << 31 | 0x1B008000u | (uint32_t)rm << 16 |
           (uint32_t)ra << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}

static uint32_t a64_smull(int rd, int rn, int rm)
{
    return 0x9B207C00u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rd;
}

static uint32_t a64_umull(int rd, int rn, int rm)
{
    return 0x9BA07C00u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rd;
}

static uint32_t a64_mrs_nzcv(int rt) { return 0xD53B4200u | (uint32_t)rt; }

/* size: 0 byte, 1 halfword, 2 word; address [Xn, Wm, UXTW] */
static uint32_t a64_ldst_reg(int size, int load, int rt, int rn, int rm)
{
    return (uint32_t)size << 30 | 0x38200800u | (uint32_t)load << 22 | (uint32_t)rm << 16 |
           2u << 13 | (uint32_t)rn << 5 | (uint32_t)rt;
}

/* size: 0 byte to 3 doubleword; unsigned scaled offset in bytes */
static uint32_t a64_ldst_uimm(int size, int load, int rt, int rn, uint32_t offset)
{
    return (uint32_t)size << 30 | 0x39000000u | (uint32_t)load << 22 |
           ((offset >> size) & 0xFFFu) << 10 | (uint32_t)rn << 5 | (uint32_t)rt;
}

/* mode: 1 post-index, 2 signed offset, 3 pre-index */
static uint32_t a64_ldstp(int x64, int load, int mode, int rt, int rt2, int rn, int offset)
{
    int scale = x64 ? 3 : 2;

    return (uint32_t)(x64 ? 2 : 0) << 30 | 0x28000000u | (uint32_t)mode << 23 |
           (uint32_t)load << 22 | (uint32_t)((offset >> scale) & 0x7F) << 15 |
           (uint32_t)rt2 << 10 | (uint32_t)rn << 5 | (uint32_t)rt;
}

static uint32_t a64_b(int32_t words) { return 0x14000000u | ((uint32_t)words & 0x03FFFFFFu); }
static uint32_t a64_bl(int32_t words) { return 0x94000000u | ((uint32_t)words & 0x03FFFFFFu); }
static uint32_t a64_bcond(int cond, int32_t words)
{
    return 0x54000000u | (((uint32_t)words & 0x7FFFFu) << 5) | (uint32_t)cond;
}
static uint32_t a64_cbz(int sf, int nz, int rt, int32_t words)
{
    return (uint32_t)sf << 31 | 0x34000000u | (uint32_t)nz << 24 |
           (((uint32_t)words & 0x7FFFFu) << 5) | (uint32_t)rt;
}
static uint32_t a64_tbz(int nz, int rt, int bit, int32_t words)
{
    return (uint32_t)(bit >> 5) << 31 | 0x36000000u | (uint32_t)nz << 24 |
           (uint32_t)(bit & 31) << 19 | (((uint32_t)words & 0x3FFFu) << 5) | (uint32_t)rt;
}
static uint32_t a64_br(int rn) { return 0xD61F0000u | (uint32_t)rn << 5; }
static uint32_t a64_blr(int rn) { return 0xD63F0000u | (uint32_t)rn << 5; }
#define A64_RET 0xD65F03C0u

/* Aliases, 32-bit unless the name says otherwise. */
static uint32_t a64_mov(int rd, int rm) { return a64_logic_reg(0, 1, 0, rd, ZR, rm, 0, 0); }
static uint32_t a64_mov64(int rd, int rm) { return a64_logic_reg(1, 1, 0, rd, ZR, rm, 0, 0); }
static uint32_t a64_cmp(int rn, int rm) { return a64_addsub_reg(0, 1, 1, ZR, rn, rm, 0, 0); }
static uint32_t a64_tst(int rn, int rm) { return a64_logic_reg(0, 3, 0, ZR, rn, rm, 0, 0); }
static uint32_t a64_lsl(int rd, int rn, int s) { return a64_bitfield(0, 2, rd, rn, (32 - s) & 31, 31 - s); }
static uint32_t a64_lsr(int rd, int rn, int s) { return a64_bitfield(0, 2, rd, rn, s, 31); }
static uint32_t a64_asr(int rd, int rn, int s) { return a64_bitfield(0, 0, rd, rn, s, 31); }
static uint32_t a64_ubfx(int rd, int rn, int lsb, int width) { return a64_bitfield(0, 2, rd, rn, lsb, lsb + width - 1); }
static uint32_t a64_bfi(int rd, int rn, int lsb, int width) { return a64_bitfield(0, 1, rd, rn, (32 - lsb) & 31, width - 1); }
static uint32_t a64_sxtb(int rd, int rn) { return a64_bitfield(0, 0, rd, rn, 0, 7); }
static uint32_t a64_sxth(int rd, int rn) { return a64_bitfield(0, 0, rd, rn, 0, 15); }
static uint32_t a64_uxtb(int rd, int rn) { return a64_bitfield(0, 2, rd, rn, 0, 7); }
static uint32_t a64_uxth(int rd, int rn) { return a64_bitfield(0, 2, rd, rn, 0, 15); }
static uint32_t a64_ror(int rd, int rn, int s) { return a64_extr(0, rd, rn, rn, s); }
static uint32_t a64_cset(int rd, int cond) { return a64_csinc(0, rd, ZR, ZR, cond ^ 1); }

/* Logical immediates the translator uses, all encodable. */
static uint32_t a64_logic_const(int opc, int rd, int rn, uint32_t value)
{
    uint32_t field = 0;

    if (a64_bitmask(value, 0, &field) != 0)
        abort();
    return a64_logic_imm(0, opc, rd, rn, field);
}

/* ------------------------------------------------------------------
   Code emission
   ------------------------------------------------------------------ */

struct HJEmit
{
    uint32_t *buf;
    unsigned n, cap;
    uintptr_t origin;   /* host address of buf[0] once copied */
    int bad;            /* too long, or a branch out of range */
};

static void put(struct HJEmit *e, uint32_t word)
{
    if (e->n < e->cap)
        e->buf[e->n] = word;
    else
        e->bad = 1;
    e->n++;
}

static int32_t words_to(const struct HJEmit *e, uintptr_t target)
{
    return (int32_t)(((intptr_t)target - (intptr_t)(e->origin + 4u * e->n)) / 4);
}

/* Point the local branch at index 'at' to index 'to'. */
static void patch(struct HJEmit *e, unsigned at, unsigned to)
{
    int32_t off = (int32_t)to - (int32_t)at;
    uint32_t w;

    if (at >= e->cap || to > e->cap)
    {
        e->bad = 1;
        return;
    }
    w = e->buf[at];
    if ((w & 0x7C000000u) == 0x14000000u)
        w = (w & 0xFC000000u) | ((uint32_t)off & 0x03FFFFFFu);
    else if ((w & 0x7E000000u) == 0x36000000u)
    {
        if (off < -8192 || off > 8191)
            e->bad = 1;
        w = (w & 0xFFF8001Fu) | (((uint32_t)off & 0x3FFFu) << 5);
    }
    else
    {
        if (off < -262144 || off > 262143)
            e->bad = 1;
        w = (w & 0xFF00001Fu) | (((uint32_t)off & 0x7FFFFu) << 5);
    }
    e->buf[at] = w;
}

static void emit_mov32(struct HJEmit *e, int rd, uint32_t v)
{
    uint32_t field;

    if ((v >> 16) == 0)
        put(e, a64_movw(0, 2, rd, v, 0));
    else if ((v & 0xFFFFu) == 0)
        put(e, a64_movw(0, 2, rd, v >> 16, 1));
    else if ((v >> 16) == 0xFFFFu)
        put(e, a64_movw(0, 0, rd, ~v & 0xFFFFu, 0));
    else if ((v & 0xFFFFu) == 0xFFFFu)
        put(e, a64_movw(0, 0, rd, (~v >> 16) & 0xFFFFu, 1));
    else if (a64_bitmask(v, 0, &field) == 0)
        put(e, a64_logic_imm(0, 1, rd, ZR, field));
    else
    {
        put(e, a64_movw(0, 2, rd, v & 0xFFFFu, 0));
        put(e, a64_movw(0, 3, rd, v >> 16, 1));
    }
}

/* ------------------------------------------------------------------
   State shared with translated code
   ------------------------------------------------------------------ */

/* Host registers inside translated code. D0-D7 live in w0-w7 and A0-A7 in
   w8-w15 from the enter trampoline to the exit trampoline. x18, the
   platform register that Darwin reserves, is never used. */
#define REG_D(n)  (n)
#define REG_A(n)  (8 + (n))
#define R_ADDR    16  /* guest address of an access; the slow path keeps it */
#define R_VAL     17  /* value loaded, or to be stored */
#define R_STATE   19
#define R_MEM     20
#define R_FLAGS   21  /* X N Z C V in bits 4..0: NZCV in the order of A64 */
#define R_BUDGET  22
#define R_SRC     23  /* immediate operand */
#define R_LIMIT   24  /* mem_size - 4 */
#define R_SCR     25  /* scratch inside an access; slow path kind */
#define R_F1      26  /* flag scratch */
#define R_F2      27  /* flag scratch; guest PC for the slow path */
#define R_RES     28  /* result kept across a memory access */

#define HJF_V 0x01u
#define HJF_C 0x02u
#define HJF_Z 0x04u
#define HJF_N 0x08u
#define HJF_X 0x10u
#define HJF_NZVC 0x0Fu
#define HJF_ALL 0x1Fu

struct HJState
{
    uint32_t d[8];              /*   0 */
    uint32_t a[8];              /*  32 */
    uint32_t pc;                /*  64 */
    uint32_t flags;             /*  68 */
    uint32_t sr_high;           /*  72: SR bits 15..8 */
    uint32_t abort;             /*  76 */
    uint32_t limit;             /*  80 */
    uint32_t spare;             /*  84 */
    UBYTE *mem;                 /*  88 */
    void *fast;                 /*  96 */
    void *slow;                 /* 104 */
    struct HJBackend *backend;  /* 112 */
};

#define ST_PC     64
#define ST_FLAGS  68
#define ST_SRHI   72
#define ST_ABORT  76
#define ST_LIMIT  80
#define ST_MEM    88
#define ST_FAST   96
#define ST_SLOW  104

#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
_Static_assert(offsetof(struct HJState, pc) == ST_PC, "state layout");
_Static_assert(offsetof(struct HJState, flags) == ST_FLAGS, "state layout");
_Static_assert(offsetof(struct HJState, sr_high) == ST_SRHI, "state layout");
_Static_assert(offsetof(struct HJState, abort) == ST_ABORT, "state layout");
_Static_assert(offsetof(struct HJState, limit) == ST_LIMIT, "state layout");
_Static_assert(offsetof(struct HJState, mem) == ST_MEM, "state layout");
_Static_assert(offsetof(struct HJState, fast) == ST_FAST, "state layout");
_Static_assert(offsetof(struct HJState, slow) == ST_SLOW, "state layout");
#endif

/* Accesses through the C slow path. */
enum { SLOW_R8, SLOW_R16, SLOW_R32, SLOW_W8, SLOW_W16, SLOW_W32, SLOW_KINDS };

#define HJ_FAST_BITS 12
#define HJ_FAST_SIZE (1u << HJ_FAST_BITS)
#define HJ_BUCKETS 4096u
#define HJ_CODE_BYTES (16u << 20)
#define HJ_MAX_INSNS 64
#define HJ_SCRATCH_WORDS 12288u
#define HJ_MAX_STUBS 1024u
#define HJ_MAX_BLOCKS 32768u
#define HJ_GUEST_BYTES (4u << 20)
#define HJ_BUDGET 65535u
#define HJ_EMPTY_TAG (~(uint64_t)0)

struct HJFast
{
    uint64_t pc;
    uint64_t entry;
};

struct HJBlock
{
    struct HJBlock *next;   /* bucket chain */
    uint32_t pc, end;       /* guest bytes [pc, end) */
    uint32_t epoch;
    uint32_t words;
    const uint32_t *code;
    const UBYTE *guest;     /* the guest bytes when translated */
};

struct HJBackend
{
    struct HJState st;
    struct KaffeineContext *ctx;
    struct KaffeineCPU handle;
    uint32_t *code;
    size_t code_words, code_used, glue_words;
    uintptr_t g_enter, g_exit, g_lookup, g_budget, g_miss, g_slow[SLOW_KINDS];
    uint32_t (*enter)(struct HJState *, const uint32_t *);
    struct HJFast *fast;
    struct HJBlock *buckets[HJ_BUCKETS];
    struct HJBlock *blocks;
    unsigned nblocks;
    UBYTE *guest_bytes;
    size_t guest_used;
    uint32_t epoch;
    uint32_t *scratch;
    struct HJTrans *trans;
    int faulted;
    unsigned depth;
    char fault[192];
    struct HJStats stats;
};

/* ------------------------------------------------------------------
   68k decoding
   ------------------------------------------------------------------ */

enum
{
    EA_DN, EA_AN, EA_IND, EA_POST, EA_PRE, EA_DISP, EA_IDX,
    EA_ABS, EA_PCDISP, EA_PCIDX, EA_IMM
};

#define M(x) (1u << (x))
#define M_ALL   0x7FFu
#define M_DATA  (M_ALL & ~M(EA_AN))
#define M_ALT   (M(EA_DN) | M(EA_AN) | M(EA_IND) | M(EA_POST) | M(EA_PRE) | M(EA_DISP) | M(EA_IDX) | M(EA_ABS))
#define M_DALT  (M_ALT & ~M(EA_AN))
#define M_MALT  (M_DALT & ~M(EA_DN))
#define M_CTRL  (M(EA_IND) | M(EA_DISP) | M(EA_IDX) | M(EA_ABS) | M(EA_PCDISP) | M(EA_PCIDX))
#define M_CALT  (M(EA_IND) | M(EA_DISP) | M(EA_IDX) | M(EA_ABS))

struct HJEA
{
    uint8_t mode, reg;
    uint8_t xreg, xlong, scale;   /* index register 0-15, .L index, scale shift */
    int32_t disp;
    uint32_t value;               /* address, immediate, or PC-relative address */
};

enum
{
    K_NOP, K_MOVEQ, K_MOVE, K_MOVEA, K_LEA, K_PEA,
    K_LINK, K_UNLK, K_EXG, K_CMPM,
    K_ALU,      /* <ea>,Dn: ADD SUB AND OR CMP */
    K_ALUM,     /* Dn,<ea>: ADD SUB AND OR EOR */
    K_ALUI,     /* #imm,<ea>: ORI ANDI SUBI ADDI EORI CMPI */
    K_ALUA,     /* ADDA SUBA CMPA */
    K_QUICK,    /* ADDQ SUBQ */
    K_ADDX,     /* ADDX SUBX NEGX */
    K_MULW,     /* MULU.W MULS.W */
    K_MULL,     /* MULU.L MULS.L */
    K_DIVL,     /* DIVU.L DIVS.L DIVUL.L DIVSL.L */
    K_BIT,      /* BTST BCHG BCLR BSET */
    K_BF,       /* BFTST BFEXTU BFCHG BFEXTS BFCLR BFFFO BFSET BFINS */
    K_CAS,      /* CAS.B CAS.W CAS.L */
    K_MOVEP,    /* MOVEP.W MOVEP.L */
    K_TAS,      /* TAS <ea> */
    K_CLR, K_TST, K_NEG, K_NOT, K_SWAP, K_EXTW, K_EXTL, K_EXTBL,
    K_SHIFT,    /* ASL ASR LSL LSR ROL ROR ROXL ROXR */
    K_SCC,
    K_BCC, K_BSR, K_DBCC, K_JSR, K_JMP, K_RTS, K_RTR, K_RTD,
    K_MOVEM,
    K_MOVEFROMSR, K_MOVEFROMCCR, K_MOVETOCCR, K_MOVETOSR, K_LOGICSR,
    K_LINEA, K_FAULT
};
enum { OP_ADD, OP_SUB, OP_AND, OP_OR, OP_EOR, OP_CMP };
enum { SH_LSL, SH_LSR, SH_ASR, SH_ASL, SH_ROL, SH_ROR, SH_ROXL, SH_ROXR };
enum { BIT_TST, BIT_CHG, BIT_CLR, BIT_SET };
enum { BF_TST, BF_EXTU, BF_CHG, BF_EXTS, BF_CLR, BF_FFO, BF_SET, BF_INS };

struct HJInsn
{
    uint32_t pc, next, target;
    uint16_t opcode, mask;          /* MOVEM: registers in D0..A7 bit order */
    uint8_t kind, op, size, cond, count;
    uint8_t def, use, need;         /* HJF_* */
    struct HJEA src, dst;
    const char *why;                /* K_FAULT */
};

struct HJDec
{
    struct KaffeineContext *ctx;
    uint32_t at;
    int bad;
};

static uint16_t fetch16(struct HJDec *d)
{
    uint16_t v;

    if (d->at >= d->ctx->mem_size || d->ctx->mem_size - d->at < 2)
        d->bad = 1;
    v = m68k_read16(d->ctx, d->at);
    d->at += 2;
    return v;
}

static uint32_t fetch32(struct HJDec *d)
{
    uint32_t hi = fetch16(d);

    return hi << 16 | fetch16(d);
}

/* One effective address, if its kind is among 'allowed'. */
static int decode_ea(struct HJDec *d, int mode, int reg, int size, unsigned allowed, struct HJEA *ea)
{
    uint32_t base;
    uint16_t ext;

    memset(ea, 0, sizeof(*ea));
    ea->reg = (uint8_t)reg;
    switch (mode)
    {
    case 0: ea->mode = EA_DN; break;
    case 1: ea->mode = EA_AN; break;
    case 2: ea->mode = EA_IND; break;
    case 3: ea->mode = EA_POST; break;
    case 4: ea->mode = EA_PRE; break;
    case 5: ea->mode = EA_DISP; ea->disp = (int16_t)fetch16(d); break;
    case 6:
    case 7:
        if (mode == 7 && reg == 0) { ea->mode = EA_ABS; ea->value = (uint32_t)(int32_t)(int16_t)fetch16(d); break; }
        if (mode == 7 && reg == 1) { ea->mode = EA_ABS; ea->value = fetch32(d); break; }
        if (mode == 7 && reg == 2) { base = d->at; ea->mode = EA_PCDISP; ea->value = base + (uint32_t)(int32_t)(int16_t)fetch16(d); break; }
        if (mode == 7 && reg == 4)
        {
            ea->mode = EA_IMM;
            ea->value = size == 4 ? fetch32(d) : size == 2 ? fetch16(d) : (fetch16(d) & 0xFFu);
            break;
        }
        if (mode == 7 && reg != 3)
            return 0;
        base = d->at;
        ext = fetch16(d);
        if (ext & 0x0100)
            return 0;           /* full extension word format */
        ea->mode = mode == 6 ? EA_IDX : EA_PCIDX;
        ea->xreg = (uint8_t)(ext >> 12);
        ea->xlong = (uint8_t)((ext >> 11) & 1);
        ea->scale = (uint8_t)((ext >> 9) & 3);
        ea->disp = (int8_t)(ext & 0xFF);
        if (mode == 7)
            ea->value = base + (uint32_t)ea->disp;
        break;
    }
    return (allowed & M(ea->mode)) != 0;
}

static const uint8_t cond_use[16] =
{
    0, 0, HJF_C | HJF_Z, HJF_C | HJF_Z, HJF_C, HJF_C, HJF_Z, HJF_Z,
    HJF_V, HJF_V, HJF_N, HJF_N, HJF_N | HJF_V, HJF_N | HJF_V,
    HJF_N | HJF_V | HJF_Z, HJF_N | HJF_V | HJF_Z
};

static int fault_insn(struct HJInsn *in, const char *why)
{
    in->kind = K_FAULT;
    in->why = why;
    in->def = 0;
    in->use = HJF_ALL;
    return 0;
}

static int std_size(uint16_t op)
{
    static const int sizes[4] = { 1, 2, 4, 0 };

    return sizes[(op >> 6) & 3];
}

/* Group 4: miscellaneous */
static int decode_misc(struct HJDec *d, uint16_t op, struct HJInsn *in)
{
    int mode = (op >> 3) & 7, reg = op & 7, size = std_size(op);

    if (op == 0x4E71) { in->kind = K_NOP; return 1; }
    if (op == 0x4E74) { in->kind = K_RTD; in->src.disp = (int16_t)fetch16(d); in->use = HJF_ALL; return 1; }
    if (op == 0x4E75) { in->kind = K_RTS; in->use = HJF_ALL; return 1; }
    if (op == 0x4E77) { in->kind = K_RTR; in->def = HJF_ALL; return 1; }
    if ((op & 0xFFF8) == 0x4E50)
    {
        in->kind = K_LINK; in->size = 2; in->dst.reg = (uint8_t)reg;
        in->src.disp = (int16_t)fetch16(d);
        return 1;
    }
    if ((op & 0xFFF8) == 0x4808)
    {
        in->kind = K_LINK; in->size = 4; in->dst.reg = (uint8_t)reg;
        in->src.disp = (int32_t)fetch32(d);
        return 1;
    }
    if ((op & 0xFFF8) == 0x4E58) { in->kind = K_UNLK; in->dst.reg = (uint8_t)reg; return 1; }
    if (op == 0x4AFC) return fault_insn(in, "ILLEGAL");
    if ((op & 0xFFC0) == 0x4E80 || (op & 0xFFC0) == 0x4EC0)
    {
        in->kind = (op & 0x0040) ? K_JMP : K_JSR;
        in->use = HJF_ALL;
        return decode_ea(d, mode, reg, 4, M_CTRL, &in->src) || fault_insn(in, "JSR/JMP mode");
    }
    if ((op & 0xFFF8) == 0x49C0) { in->kind = K_EXTBL; in->dst.reg = (uint8_t)reg; in->def = HJF_NZVC; return 1; }
    if ((op & 0xF1C0) == 0x41C0)
    {
        in->kind = K_LEA;
        in->dst.reg = (uint8_t)((op >> 9) & 7);
        return decode_ea(d, mode, reg, 4, M_CTRL, &in->src) || fault_insn(in, "LEA mode");
    }
    if ((op & 0xFFC0) == 0x40C0)
    {
        in->kind = K_MOVEFROMSR; in->size = 2; in->use = HJF_ALL;
        return decode_ea(d, mode, reg, 2, M_DALT, &in->dst) || fault_insn(in, "MOVE from SR mode");
    }
    if ((op & 0xFFC0) == 0x42C0)
    {
        in->kind = K_MOVEFROMCCR; in->size = 2; in->use = HJF_ALL;
        return decode_ea(d, mode, reg, 2, M_DALT, &in->dst) || fault_insn(in, "MOVE from CCR mode");
    }
    if ((op & 0xFFC0) == 0x44C0 || (op & 0xFFC0) == 0x46C0)
    {
        in->kind = (op & 0x0200) ? K_MOVETOSR : K_MOVETOCCR; in->size = 2; in->def = HJF_ALL;
        return decode_ea(d, mode, reg, 2, M_DATA, &in->src) || fault_insn(in, "MOVE to SR mode");
    }
    if (size && (op & 0xFF00) == 0x4000)
    {
        in->kind = K_ADDX; in->op = OP_SUB; in->count = 1; in->size = (uint8_t)size;
        in->use = HJF_X | HJF_Z; in->def = HJF_ALL;
        return decode_ea(d, mode, reg, size, M_DALT, &in->dst) || fault_insn(in, "NEGX mode");
    }
    if (size && ((op & 0xFF00) == 0x4200 || (op & 0xFF00) == 0x4400 ||
                 (op & 0xFF00) == 0x4600 || (op & 0xFF00) == 0x4A00))
    {
        unsigned allowed = M_DALT;

        switch (op & 0xFF00)
        {
        case 0x4200: in->kind = K_CLR; in->def = HJF_NZVC; break;
        case 0x4400: in->kind = K_NEG; in->def = HJF_ALL; break;
        case 0x4600: in->kind = K_NOT; in->def = HJF_NZVC; break;
        default:
            in->kind = K_TST; in->def = HJF_NZVC;
            allowed = size == 1 ? M_DATA : M_ALL;
            break;
        }
        in->size = (uint8_t)size;
        return decode_ea(d, mode, reg, size, allowed, &in->dst) || fault_insn(in, "unary mode");
    }
    if ((op & 0xFFC0) == 0x4AC0)
    {
        in->kind = K_TAS; in->size = 1; in->def = HJF_NZVC;
        return decode_ea(d, mode, reg, 1, M_DALT, &in->dst) || fault_insn(in, "TAS mode");
    }
    if ((op & 0xFFF8) == 0x4840) { in->kind = K_SWAP; in->dst.reg = (uint8_t)reg; in->def = HJF_NZVC; return 1; }
    if ((op & 0xFFC0) == 0x4840)
    {
        in->kind = K_PEA;
        return decode_ea(d, mode, reg, 4, M_CTRL, &in->src) || fault_insn(in, "PEA mode");
    }
    if ((op & 0xFFF8) == 0x4880) { in->kind = K_EXTW; in->dst.reg = (uint8_t)reg; in->def = HJF_NZVC; return 1; }
    if ((op & 0xFFF8) == 0x48C0) { in->kind = K_EXTL; in->dst.reg = (uint8_t)reg; in->def = HJF_NZVC; return 1; }
    if ((op & 0xFFC0) == 0x4C00)
    {
        uint16_t ext = fetch16(d);

        if (ext & 0x83F8u)
            return fault_insn(in, "MUL.L extension");
        in->kind = K_MULL;
        in->size = 4;
        in->op = (uint8_t)((ext >> 11) & 1);
        in->dst.reg = (uint8_t)((ext >> 12) & 7);
        in->count = (uint8_t)((ext & 7) | (((ext >> 10) & 1) << 3));
        in->def = HJF_NZVC;
        return decode_ea(d, mode, reg, 4, M_DATA, &in->src) || fault_insn(in, "MUL.L mode");
    }
    if ((op & 0xFFC0) == 0x4C40)
    {
        uint16_t ext = fetch16(d);

        if (ext & 0x83F8u)
            return fault_insn(in, "DIV.L extension");
        in->kind = K_DIVL;
        in->size = 4;
        in->op = (uint8_t)((ext >> 11) & 1);
        in->dst.reg = (uint8_t)((ext >> 12) & 7);
        in->count = (uint8_t)((ext & 7) | (((ext >> 10) & 1) << 3));
        in->def = HJF_NZVC;
        in->use = HJF_ALL;
        in->why = "divide by zero";
        return decode_ea(d, mode, reg, 4, M_DATA, &in->src) || fault_insn(in, "DIV.L mode");
    }
    if ((op & 0xFB80) == 0x4880)
    {
        uint16_t mask = fetch16(d);
        int load = (op >> 10) & 1;

        in->kind = K_MOVEM; in->count = (uint8_t)load; in->size = (op & 0x0040) ? 4 : 2;
        if (!decode_ea(d, mode, reg, in->size, load ? (M_CTRL | M(EA_POST)) : (M_CALT | M(EA_PRE)), &in->src))
            return fault_insn(in, "MOVEM mode");
        if (in->src.mode == EA_PRE)
        {
            uint16_t r = 0;
            int i;

            for (i = 0; i < 16; i++)
                if (mask & (1u << i))
                    r |= (uint16_t)(1u << (15 - i));
            mask = r;
        }
        in->mask = mask;
        if ((in->src.mode == EA_PRE || in->src.mode == EA_POST) && (mask & (1u << (8 + in->src.reg))))
            return fault_insn(in, "MOVEM with its address register");
        return 1;
    }
    return fault_insn(in, "opcode outside the subset");
}

static int decode_insn(struct KaffeineContext *ctx, uint32_t pc, struct HJInsn *in)
{
    struct HJDec d = { ctx, pc, 0 };
    uint16_t op;
    int mode, reg, size, ok = 1;

    memset(in, 0, sizeof(*in));
    in->pc = pc;
    op = fetch16(&d);
    in->opcode = op;
    mode = (op >> 3) & 7;
    reg = op & 7;
    size = std_size(op);

    switch (op >> 12)
    {
    case 0x0:
    {
        static const int8_t ops[8] = { OP_OR, OP_AND, OP_SUB, OP_ADD, -1, OP_EOR, OP_CMP, -1 };
        int which = ops[(op >> 9) & 7];

        if (op & 0x0100)
        {
            int bop = (op >> 6) & 3;
            unsigned allowed = bop == BIT_TST ? M_DATA : M_DALT;

            if (mode == 1)
            {
                in->kind = K_MOVEP;
                in->op = (uint8_t)((op >> 7) & 1);
                in->size = (op & 0x0040) ? 4 : 2;
                in->dst.reg = (uint8_t)((op >> 9) & 7);
                in->src.reg = (uint8_t)reg;
                in->src.disp = (int16_t)fetch16(&d);
                break;
            }
            in->kind = K_BIT; in->op = (uint8_t)bop;
            in->size = mode == 0 ? 4 : 1;
            in->src.mode = EA_DN; in->src.reg = (uint8_t)((op >> 9) & 7);
            in->def = HJF_Z;
            ok = decode_ea(&d, mode, reg, 1, allowed, &in->dst) || fault_insn(in, "bit mode");
            break;
        }
        if ((op & 0xFF00) == 0x0800)
        {
            int bop = (op >> 6) & 3;
            unsigned allowed = bop == BIT_TST ? (M_DATA & ~M(EA_IMM)) : M_DALT;

            in->kind = K_BIT; in->op = (uint8_t)bop;
            in->size = mode == 0 ? 4 : 1;
            in->src.mode = EA_IMM; in->src.value = fetch16(&d) & 0xFFu;
            in->def = HJF_Z;
            ok = decode_ea(&d, mode, reg, 1, allowed, &in->dst) || fault_insn(in, "bit mode");
            break;
        }
        if ((op & 0x3F) == 0x3C && (which == OP_OR || which == OP_AND || which == OP_EOR) &&
            (size == 1 || size == 2))
        {
            in->kind = K_LOGICSR; in->op = (uint8_t)which; in->size = (uint8_t)size;
            in->src.value = fetch16(&d);
            in->use = HJF_ALL; in->def = HJF_ALL;
            break;
        }
        if (!size && (op & 0x3F) != 0x3C &&
            ((op & 0xFF00) == 0x0A00 || (op & 0xFF00) == 0x0C00 || (op & 0xFF00) == 0x0E00))
        {
            uint16_t ext = fetch16(&d);

            if (ext & 0xFE38u)
            {
                ok = fault_insn(in, "CAS extension");
                break;
            }
            in->kind = K_CAS;
            in->size = (op >> 9) == 5 ? 1 : (op >> 9) == 6 ? 2 : 4;
            in->src.reg = (uint8_t)(ext & 7);
            in->count = (uint8_t)((ext >> 6) & 7);
            in->def = HJF_NZVC;
            ok = decode_ea(&d, mode, reg, in->size, M_MALT, &in->dst) || fault_insn(in, "CAS mode");
            break;
        }
        if (which < 0 || !size || (op & 0x3F) == 0x3C)
        {
            ok = fault_insn(in, "opcode outside the subset");
            break;
        }
        in->kind = K_ALUI; in->op = (uint8_t)which; in->size = (uint8_t)size;
        in->src.mode = EA_IMM;
        in->src.value = size == 4 ? fetch32(&d) : size == 2 ? fetch16(&d) : (fetch16(&d) & 0xFFu);
        in->def = (which == OP_ADD || which == OP_SUB) ? HJF_ALL : HJF_NZVC;
        ok = decode_ea(&d, mode, reg, size,
                       M_DALT | (which == OP_CMP ? (M(EA_PCDISP) | M(EA_PCIDX)) : 0),
                       &in->dst) || fault_insn(in, "immediate mode");
        break;
    }
    case 0x1: case 0x2: case 0x3:
    {
        int msize = (op >> 12) == 1 ? 1 : (op >> 12) == 3 ? 2 : 4;
        int dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;

        in->size = (uint8_t)msize;
        if (!decode_ea(&d, mode, reg, msize, msize == 1 ? M_DATA : M_ALL, &in->src))
        {
            ok = fault_insn(in, "MOVE source");
            break;
        }
        if (dmode == 1)
        {
            in->kind = K_MOVEA;
            in->dst.mode = EA_AN; in->dst.reg = (uint8_t)dreg;
            if (msize == 1)
                ok = fault_insn(in, "MOVEA.B");
            break;
        }
        in->kind = K_MOVE; in->def = HJF_NZVC;
        ok = decode_ea(&d, dmode, dreg, msize, M_DALT, &in->dst) || fault_insn(in, "MOVE destination");
        break;
    }
    case 0x4:
        ok = decode_misc(&d, op, in);
        break;
    case 0x5:
        if (size == 0)
        {
            in->cond = (uint8_t)((op >> 8) & 15);
            in->use = cond_use[in->cond];
            if (mode == 1)
            {
                in->kind = K_DBCC; in->dst.reg = (uint8_t)reg;
                in->next = d.at + 2;
                in->target = d.at + (uint32_t)(int32_t)(int16_t)fetch16(&d);
                in->use = HJF_ALL;
                break;
            }
            in->kind = K_SCC; in->size = 1;
            ok = decode_ea(&d, mode, reg, 1, M_DALT, &in->dst) || fault_insn(in, "Scc mode");
            break;
        }
        in->kind = K_QUICK; in->op = (op & 0x0100) ? OP_SUB : OP_ADD;
        in->size = (uint8_t)size; in->count = (uint8_t)(((op >> 9) & 7) ? ((op >> 9) & 7) : 8);
        ok = decode_ea(&d, mode, reg, size, size == 1 ? M_DALT : M_ALT, &in->dst) || fault_insn(in, "quick mode");
        if (ok)
            in->def = in->dst.mode == EA_AN ? 0 : HJF_ALL;
        break;
    case 0x6:
    {
        uint32_t base = d.at;
        int32_t disp = (int8_t)(op & 0xFF);

        if ((op & 0xFF) == 0x00)
            disp = (int16_t)fetch16(&d);
        else if ((op & 0xFF) == 0xFF)
            disp = (int32_t)fetch32(&d);
        in->target = base + (uint32_t)disp;
        in->cond = (uint8_t)((op >> 8) & 15);
        if (in->cond == 1)
        {
            in->kind = K_BSR; in->use = HJF_ALL;
        }
        else
        {
            in->kind = K_BCC; in->use = HJF_ALL;
        }
        break;
    }
    case 0x7:
        if (op & 0x0100)
        {
            ok = fault_insn(in, "opcode outside the subset");
            break;
        }
        in->kind = K_MOVEQ; in->dst.reg = (uint8_t)((op >> 9) & 7);
        in->src.value = (uint32_t)(int32_t)(int8_t)(op & 0xFF);
        in->def = HJF_NZVC;
        break;
    case 0x8: case 0x9: case 0xB: case 0xC: case 0xD:
    {
        int group = op >> 12, opmode = (op >> 6) & 7, dn = (op >> 9) & 7;
        int alu = group == 0x8 ? OP_OR : group == 0x9 ? OP_SUB : group == 0xC ? OP_AND :
                  group == 0xD ? OP_ADD : OP_CMP;

        if (opmode == 3 || opmode == 7)
        {
            if (group == 0xC)
            {
                in->kind = K_MULW; in->op = (uint8_t)(opmode == 7); in->size = 2;
                in->dst.mode = EA_DN; in->dst.reg = (uint8_t)dn;
                in->def = HJF_NZVC;
                ok = decode_ea(&d, mode, reg, 2, M_DATA, &in->src) || fault_insn(in, "MUL.W mode");
                break;
            }
            if (group == 0x8)
            {
                ok = fault_insn(in, "MUL/DIV");
                break;
            }
            in->kind = K_ALUA; in->op = (uint8_t)alu; in->size = opmode == 3 ? 2 : 4;
            in->dst.mode = EA_AN; in->dst.reg = (uint8_t)dn;
            in->def = alu == OP_CMP ? HJF_NZVC : 0;
            ok = decode_ea(&d, mode, reg, in->size, M_ALL, &in->src) || fault_insn(in, "address mode");
            break;
        }
        in->size = (uint8_t)(1 << (opmode & 3));
        if (opmode < 3)
        {
            unsigned allowed = (alu == OP_OR || alu == OP_AND) ? M_DATA : (in->size == 1 ? M_DATA : M_ALL);

            in->kind = K_ALU; in->op = (uint8_t)alu;
            in->dst.mode = EA_DN; in->dst.reg = (uint8_t)dn;
            in->def = (alu == OP_ADD || alu == OP_SUB) ? HJF_ALL : HJF_NZVC;
            ok = decode_ea(&d, mode, reg, in->size, allowed, &in->src) || fault_insn(in, "source mode");
            break;
        }
        if (group == 0xB)
        {
            if (mode == 1)
            {
                in->kind = K_CMPM;
                in->src.reg = (uint8_t)reg;
                in->dst.reg = (uint8_t)dn;
                in->def = HJF_NZVC;
                break;
            }
            alu = OP_EOR;
        }
        if ((group == 0x9 || group == 0xD) && (mode == 0 || mode == 1))
        {
            in->kind = K_ADDX; in->op = (uint8_t)alu; in->count = 0;
            in->src.mode = mode ? EA_PRE : EA_DN; in->src.reg = (uint8_t)reg;
            in->dst.mode = mode ? EA_PRE : EA_DN; in->dst.reg = (uint8_t)dn;
            in->use = HJF_X | HJF_Z; in->def = HJF_ALL;
            break;
        }
        if (group == 0xC && ((opmode == 5 && (mode == 0 || mode == 1)) || (opmode == 6 && mode == 1)))
        {
            in->kind = K_EXG;
            in->src.reg = (uint8_t)(opmode == 5 && mode == 1 ? REG_A(dn) : REG_D(dn));
            in->dst.reg = (uint8_t)(mode == 1 ? REG_A(reg) : REG_D(reg));
            break;
        }
        in->kind = K_ALUM; in->op = (uint8_t)alu;
        in->src.mode = EA_DN; in->src.reg = (uint8_t)dn;
        in->def = (alu == OP_ADD || alu == OP_SUB) ? HJF_ALL : HJF_NZVC;
        ok = decode_ea(&d, mode, reg, in->size, alu == OP_EOR ? M_DALT : M_MALT, &in->dst) ||
             fault_insn(in, "ADDX/SUBX/ABCD/SBCD/EXG");
        break;
    }
    case 0xA:
        in->kind = K_LINEA; in->use = HJF_ALL;
        break;
    case 0xE:
    {
        static const uint8_t sops[4][2] = {
            { SH_ASR, SH_ASL }, { SH_LSR, SH_LSL },
            { SH_ROXR, SH_ROXL }, { SH_ROR, SH_ROL }
        };
        int left = (op >> 8) & 1, type;

        if (size == 0)
        {
            if (op & 0x0800)
            {
                uint16_t ext = fetch16(&d);
                int bf_op = (op >> 8) & 7;
                int do_reg = (ext >> 11) & 1, of = (ext >> 6) & 31;
                int dw_reg = (ext >> 5) & 1, wi = ext & 31;
                unsigned allowed = (bf_op == BF_TST || bf_op == BF_EXTU ||
                                    bf_op == BF_EXTS || bf_op == BF_FFO)
                                   ? (M(EA_DN) | M_CTRL) : (M(EA_DN) | M_CALT);

                if ((ext & 0x8000u) || ((bf_op & 1) == 0 && (ext & 0x7000u)) ||
                    (do_reg && (of & 0x18)) || (dw_reg && (wi & 0x18)))
                {
                    ok = fault_insn(in, "bitfield extension");
                    break;
                }
                in->kind = K_BF;
                in->op = (uint8_t)bf_op;
                in->mask = ext;
                in->def = HJF_NZVC;
                ok = decode_ea(&d, mode, reg, 4, allowed, &in->dst) || fault_insn(in, "bitfield mode");
                break;
            }
            type = (op >> 9) & 3;
            in->kind = K_SHIFT; in->size = 2; in->count = 1;
            in->src.mode = EA_IMM;
            in->op = sops[type][left];
            in->use = type == 2 ? HJF_X : 0;
            in->def = type == 3 ? HJF_NZVC : HJF_ALL;
            ok = decode_ea(&d, mode, reg, 2, M_MALT, &in->dst) || fault_insn(in, "shift mode");
            break;
        }
        type = (op >> 3) & 3;
        in->kind = K_SHIFT; in->size = (uint8_t)size;
        in->dst.mode = EA_DN; in->dst.reg = (uint8_t)reg;
        in->op = sops[type][left];
        if (op & 0x0020)
        {
            in->src.mode = EA_DN;
            in->src.reg = (uint8_t)((op >> 9) & 7);
            in->use = type == 3 ? 0 : HJF_X;
        }
        else
        {
            in->src.mode = EA_IMM;
            in->count = (uint8_t)(((op >> 9) & 7) ? ((op >> 9) & 7) : 8);
            in->use = type == 2 ? HJF_X : 0;
        }
        in->def = type == 3 ? HJF_NZVC : HJF_ALL;
        break;
    }
    default:
        ok = fault_insn(in, (op >> 12) == 0xF ? "Line-F" : "opcode outside the subset");
        break;
    }
    if (d.bad)
        ok = fault_insn(in, "fetch outside guest memory");
    if (!in->next)
        in->next = d.at;
    if (in->kind == K_BSR || in->kind == K_BCC)
        in->next = d.at;
    return ok;
}

static int is_terminator(int kind)
{
    return kind == K_BCC || kind == K_BSR || kind == K_DBCC || kind == K_JSR ||
           kind == K_JMP || kind == K_RTS || kind == K_RTR || kind == K_RTD ||
           kind == K_LINEA || kind == K_FAULT;
}

/* Which flags each instruction must compute: those a later instruction of
   the block reads before another sets them. Everything is live at the end. */
static void flag_liveness(struct HJInsn *in, unsigned n)
{
    uint8_t live = HJF_ALL;

    while (n--)
    {
        in[n].need = in[n].def & live;
        live = (uint8_t)((live & ~in[n].def) | in[n].use);
    }
}

/* ------------------------------------------------------------------
   Translation of a block
   ------------------------------------------------------------------ */

struct HJStub
{
    unsigned br1, br2, back;
    uint8_t kind, areg;
    uint32_t pc;
};

struct HJTrans
{
    struct HJEmit e;
    uintptr_t g_exit, g_lookup, g_budget, g_slow[SLOW_KINDS];
    uint32_t start;
    const struct HJInsn *in;
    struct HJStub stub[HJ_MAX_STUBS];
    unsigned nstub;
};

static int step_of(int size, int reg) { return size == 1 && reg == 7 ? 2 : size; }

static void emit_glue_jump(struct HJTrans *t, uintptr_t glue)
{
    put(&t->e, a64_b(words_to(&t->e, glue)));
}

/* Continue at a known guest address: a back edge to this block stays in
   it while the budget lasts, anything else goes through the lookup. */
static void emit_exit_to(struct HJTrans *t, uint32_t target)
{
    struct HJEmit *e = &t->e;

    if (target == t->start)
    {
        put(e, a64_addsub_imm(0, 1, 1, R_BUDGET, R_BUDGET, 1, 0));
        put(e, a64_bcond(A64_NE, -(int32_t)e->n));
        emit_mov32(e, R_ADDR, target);
        emit_glue_jump(t, t->g_budget);
        return;
    }
    emit_mov32(e, R_ADDR, target);
    emit_glue_jump(t, t->g_lookup);
}

static void emit_exit_reason(struct HJTrans *t, uint32_t pc, int reason)
{
    emit_mov32(&t->e, R_ADDR, pc);
    emit_mov32(&t->e, R_VAL, (uint32_t)reason);
    emit_glue_jump(t, t->g_exit);
}

/* One guest access through host register 'areg'. Loads leave the value in
   w17; stores take it from w17. Addresses past mem_size - 4 and the custom
   chip page go to an out-of-line call of the C slow path, and so does a
   long with either word in that page, since the Moira adapter makes two
   word accesses of it. */
static void emit_access(struct HJTrans *t, int kind, int areg)
{
    struct HJEmit *e = &t->e;
    int store = kind >= SLOW_W8;
    int size = (kind == SLOW_R8 || kind == SLOW_W8) ? 0 : (kind == SLOW_R16 || kind == SLOW_W16) ? 1 : 2;
    int scr = store ? R_SCR : R_VAL;
    struct HJStub *s = NULL;

#ifndef HJ_MEASURE_UNCHECKED
    if (t->nstub >= HJ_MAX_STUBS)
    {
        e->bad = 1;
        return;
    }
    s = &t->stub[t->nstub++];
    put(e, a64_cmp(areg, R_LIMIT));
    s->br1 = e->n;
    put(e, a64_bcond(A64_HI, 0));
    put(e, a64_addsub_imm(0, 1, 0, scr, areg, CUSTOM_BASE >> 12, 1));
    if (size == 2)
    {
        /* CUSTOM_BASE - 2 to the end of the page: addr + 2 - CUSTOM_BASE
           below 0x1002, which no immediate holds, so halved; addr + 2
           cannot wrap after the first check */
        put(e, a64_addsub_imm(0, 0, 0, scr, scr, 2, 0));
        put(e, a64_lsr(scr, scr, 1));
        put(e, a64_addsub_imm(0, 1, 1, ZR, scr, 0x801, 0));
        s->br2 = e->n;
        put(e, a64_bcond(A64_LO, 0));
    }
    else
    {
        put(e, a64_lsr(scr, scr, 12));
        s->br2 = e->n;
        put(e, a64_cbz(0, 0, scr, 0));
    }
#else
    /* Only to measure what the checks cost: every address goes to host
       memory unchecked. Never for programs that need the checks. */
    (void)scr;
#endif
    if (!store)
    {
        put(e, a64_ldst_reg(size, 1, R_VAL, R_MEM, areg));
        if (size == 2)
            put(e, a64_rev32(R_VAL, R_VAL));
        else if (size == 1)
            put(e, a64_rev16(R_VAL, R_VAL));
    }
    else if (size == 0)
        put(e, a64_ldst_reg(0, 0, R_VAL, R_MEM, areg));
    else
    {
        put(e, size == 2 ? a64_rev32(R_SCR, R_VAL) : a64_rev16(R_SCR, R_VAL));
        put(e, a64_ldst_reg(size, 0, R_SCR, R_MEM, areg));
    }
    if (!s)
        return;
    s->back = e->n;
    s->kind = (uint8_t)kind;
    s->areg = (uint8_t)areg;
    s->pc = t->in->pc;
}

static void emit_stubs(struct HJTrans *t)
{
    struct HJEmit *e = &t->e;
    unsigned i;

    for (i = 0; i < t->nstub; i++)
    {
        const struct HJStub *s = &t->stub[i];

        patch(e, s->br1, e->n);
        patch(e, s->br2, e->n);
        if (s->areg != R_ADDR)
            put(e, a64_mov(R_ADDR, s->areg));
        emit_mov32(e, R_F2, s->pc);
        put(e, a64_bl(words_to(e, t->g_slow[s->kind])));
        put(e, a64_b((int32_t)s->back - (int32_t)e->n));
    }
}

/* rd = rn + c, 32-bit wrap. */
static void emit_add_const(struct HJEmit *e, int rd, int rn, int32_t c)
{
    uint32_t mag = c < 0 ? (uint32_t)(-(int64_t)c) : (uint32_t)c;

    if (c == 0)
    {
        if (rd != rn)
            put(e, a64_mov(rd, rn));
    }
    else if (mag < 4096)
        put(e, a64_addsub_imm(0, c < 0, 0, rd, rn, mag, 0));
    else
    {
        emit_mov32(e, R_SCR, (uint32_t)c);
        put(e, a64_addsub_reg(0, 0, 0, rd, rn, R_SCR, 0, 0));
    }
}

static void emit_index(struct HJTrans *t, int base, const struct HJEA *ea)
{
    struct HJEmit *e = &t->e;
    int x = ea->xreg < 8 ? REG_D(ea->xreg) : REG_A(ea->xreg - 8);

    if (!ea->xlong)
    {
        put(e, a64_sxth(R_SCR, x));
        x = R_SCR;
    }
    if (base < 0)
    {
        emit_mov32(e, R_ADDR, ea->value);
        put(e, a64_addsub_reg(0, 0, 0, R_ADDR, R_ADDR, x, 0, ea->scale));
        return;
    }
    put(e, a64_addsub_reg(0, 0, 0, R_ADDR, base, x, 0, ea->scale));
    emit_add_const(e, R_ADDR, R_ADDR, ea->disp);
}

/* The host register that holds the guest address of a memory operand.
   A predecrement happens here, a postincrement in emit_post(). */
static int emit_address(struct HJTrans *t, const struct HJEA *ea, int size)
{
    struct HJEmit *e = &t->e;
    int an = REG_A(ea->reg);

    switch (ea->mode)
    {
    case EA_IND:
    case EA_POST:
        return an;
    case EA_PRE:
        put(e, a64_addsub_imm(0, 1, 0, an, an, (uint32_t)step_of(size, ea->reg), 0));
        return an;
    case EA_DISP:
        emit_add_const(e, R_ADDR, an, ea->disp);
        return R_ADDR;
    case EA_IDX:
        emit_index(t, an, ea);
        return R_ADDR;
    case EA_PCIDX:
        emit_index(t, -1, ea);
        return R_ADDR;
    default:
        emit_mov32(e, R_ADDR, ea->value);
        return R_ADDR;
    }
}

static void emit_post(struct HJTrans *t, const struct HJEA *ea, int size)
{
    if (ea->mode == EA_POST)
        put(&t->e, a64_addsub_imm(0, 0, 0, REG_A(ea->reg), REG_A(ea->reg),
                                  (uint32_t)step_of(size, ea->reg), 0));
}

static int read_kind(int size) { return size == 1 ? SLOW_R8 : size == 2 ? SLOW_R16 : SLOW_R32; }
static int write_kind(int size) { return size == 1 ? SLOW_W8 : size == 2 ? SLOW_W16 : SLOW_W32; }

/* The host register with a source operand; its low 'size' bytes count. */
static int emit_read(struct HJTrans *t, const struct HJEA *ea, int size)
{
    int areg;

    switch (ea->mode)
    {
    case EA_DN: return REG_D(ea->reg);
    case EA_AN: return REG_A(ea->reg);
    case EA_IMM:
        emit_mov32(&t->e, R_SRC, ea->value);
        return R_SRC;
    default:
        areg = emit_address(t, ea, size);
        emit_access(t, read_kind(size), areg);
        emit_post(t, ea, size);
        return R_VAL;
    }
}

static void emit_insert(struct HJEmit *e, int dn, int val, int size)
{
    if (size == 4)
    {
        if (dn != val)
            put(e, a64_mov(dn, val));
    }
    else
        put(e, a64_bfi(dn, val, 0, 8 * size));
}

static void emit_write(struct HJTrans *t, const struct HJEA *ea, int size, int val)
{
    int areg;

    if (ea->mode == EA_DN)
    {
        emit_insert(&t->e, REG_D(ea->reg), val, size);
        return;
    }
    areg = emit_address(t, ea, size);
    if (val != R_VAL)
        put(&t->e, a64_mov(R_VAL, val));
    emit_access(t, write_kind(size), areg);
    emit_post(t, ea, size);
}

/* NZCV of A64 in bits 31..28 into the flags: bits 3..0 keep that order. */
static void emit_nzcv_to_flags(struct HJEmit *e, int sub, int setx)
{
    put(e, a64_mrs_nzcv(R_F1));
    put(e, a64_lsr(R_F1, R_F1, 28));
    if (sub)
        put(e, a64_logic_const(2, R_F1, R_F1, HJF_C));   /* borrow, not carry */
    put(e, a64_bfi(R_FLAGS, R_F1, 0, 4));
    if (setx)
    {
        put(e, a64_lsr(R_F2, R_F1, 1));
        put(e, a64_bfi(R_FLAGS, R_F2, 4, 1));
    }
}

/* After an ADDS or SUBS: what 'need' asks for. */
static void emit_flags_arith(struct HJEmit *e, int sub, unsigned need)
{
    if (need & HJF_NZVC)
        emit_nzcv_to_flags(e, sub, (need & HJF_X) != 0);
    else if (need & HJF_X)
    {
        put(e, a64_cset(R_F2, sub ? A64_LO : A64_HS));
        put(e, a64_bfi(R_FLAGS, R_F2, 4, 1));
    }
}

/* N and Z of the low 'size' bytes of reg; V and C cleared; X kept. */
static void emit_flags_logic(struct HJEmit *e, int reg, int size)
{
    if (size == 4)
        put(e, a64_tst(reg, reg));
    else
    {
        put(e, a64_lsl(R_F1, reg, 32 - 8 * size));
        put(e, a64_tst(R_F1, R_F1));
    }
    emit_nzcv_to_flags(e, 0, 0);
}

static void emit_flags_const(struct HJEmit *e, uint32_t nz)
{
    put(e, a64_logic_const(0, R_FLAGS, R_FLAGS, HJF_X));
    if (nz)
        put(e, a64_logic_const(1, R_FLAGS, R_FLAGS, nz));
}

/* out = dval OP sval on 'size' bytes. A D register out keeps its upper
   bytes; R_RES gets valid low bytes. CMP writes nothing. */
static void emit_alu(struct HJTrans *t, int op, int size, int dval, int sval, int out, unsigned need)
{
    struct HJEmit *e = &t->e;
    int arith = op == OP_ADD || op == OP_SUB || op == OP_CMP;
    int sub = op == OP_SUB || op == OP_CMP;
    unsigned want = need & (op == OP_CMP ? HJF_NZVC : HJF_ALL);
    int keep = op != OP_CMP, shift = 32 - 8 * size;
    int opc = op == OP_AND ? 0 : op == OP_OR ? 1 : 2;

    if (arith)
    {
        int s = want != 0;

        if (size == 4)
        {
            put(e, a64_addsub_reg(0, sub, s, keep ? out : ZR, dval, sval, 0, 0));
            emit_flags_arith(e, sub, want);
            return;
        }
        if (!s)
        {
            if (keep)
            {
                put(e, a64_addsub_reg(0, sub, 0, R_SCR, dval, sval, 0, 0));
                emit_insert(e, out, R_SCR, out == R_RES ? 4 : size);
            }
            return;
        }
        put(e, a64_lsl(R_SCR, dval, shift));
        put(e, a64_addsub_reg(0, sub, 1, keep ? R_SCR : ZR, R_SCR, sval, 0, shift));
        emit_flags_arith(e, sub, want);
        if (keep)
        {
            put(e, a64_lsr(R_SCR, R_SCR, shift));
            emit_insert(e, out, R_SCR, out == R_RES ? 4 : size);
        }
        return;
    }
    if (size == 4)
    {
        if (want && op == OP_AND)
        {
            put(e, a64_logic_reg(0, 3, 0, out, dval, sval, 0, 0));
            emit_nzcv_to_flags(e, 0, 0);
            return;
        }
        put(e, a64_logic_reg(0, opc, 0, out, dval, sval, 0, 0));
        if (want)
            emit_flags_logic(e, out, 4);
        return;
    }
    put(e, a64_logic_reg(0, opc, 0, R_SCR, dval, sval, 0, 0));
    if (want)
        emit_flags_logic(e, R_SCR, size);
    emit_insert(e, out, R_SCR, out == R_RES ? 4 : size);
}

/* A branch, still to be patched, taken when the 68k condition holds.
   Returns its index, or ~0u for F. */
static unsigned emit_cond_branch(struct HJEmit *e, int cond)
{
    unsigned at;
    static const int8_t bit[16] = { -1, -1, -1, -1, 1, 1, 2, 2, 0, 0, 3, 3, -1, -1, -1, -1 };

    switch (cond)
    {
    case 0:
        at = e->n;
        put(e, a64_b(0));
        return at;
    case 1:
        return ~0u;
    case 2: case 3:
        put(e, a64_logic_const(3, ZR, R_FLAGS, HJF_C | HJF_Z));
        at = e->n;
        put(e, a64_bcond(cond == 2 ? A64_EQ : A64_NE, 0));
        return at;
    case 12: case 13: case 14: case 15:
        put(e, a64_logic_reg(0, 2, 0, R_F1, R_FLAGS, R_FLAGS, 1, 3));      /* V ^ N */
        if (cond >= 14)
            put(e, a64_logic_reg(0, 1, 0, R_F1, R_F1, R_FLAGS, 1, 2));     /* | Z */
        at = e->n;
        put(e, a64_tbz(cond & 1, R_F1, 0, 0));
        return at;
    default:
        at = e->n;
        put(e, a64_tbz(cond & 1, R_FLAGS, bit[cond], 0));
        return at;
    }
}

static void emit_push(struct HJTrans *t, uint32_t value)
{
    struct HJEmit *e = &t->e;

    put(e, a64_addsub_imm(0, 1, 0, REG_A(7), REG_A(7), 4, 0));
    emit_mov32(e, R_VAL, value);
    emit_access(t, SLOW_W32, REG_A(7));
}

/* A Dn, the result register or w17 as the target of a RMW on 'ea'. */
static void emit_rmw(struct HJTrans *t, const struct HJInsn *in, int op, int sval)
{
    int areg = emit_address(t, &in->dst, in->size);

    emit_access(t, read_kind(in->size), areg);
    emit_alu(t, op, in->size, R_VAL, sval, R_RES, in->need);
    if (op != OP_CMP)
    {
        put(&t->e, a64_mov(R_VAL, R_RES));
        emit_access(t, write_kind(in->size), areg);
    }
    emit_post(t, &in->dst, in->size);
}

static void emit_addx(struct HJTrans *t, const struct HJInsn *in)
{
    struct HJEmit *e = &t->e;
    int size = in->size, shift = 32 - 8 * size, sub = in->op == OP_SUB;
    int dval, sval, areg = -1;

    if (in->count == 1)
    {
        dval = ZR;
        if (in->dst.mode == EA_DN)
            sval = REG_D(in->dst.reg);
        else
        {
            areg = emit_address(t, &in->dst, size);
            emit_access(t, read_kind(size), areg);
            sval = R_VAL;
        }
    }
    else if (in->dst.mode == EA_DN)
    {
        sval = REG_D(in->src.reg);
        dval = REG_D(in->dst.reg);
    }
    else
    {
        int ay = REG_A(in->src.reg), ax = REG_A(in->dst.reg);

        put(e, a64_addsub_imm(0, 1, 0, ay, ay, (uint32_t)step_of(size, in->src.reg), 0));
        emit_access(t, read_kind(size), ay);
        put(e, a64_mov(R_SRC, R_VAL));
        put(e, a64_addsub_imm(0, 1, 0, ax, ax, (uint32_t)step_of(size, in->dst.reg), 0));
        emit_access(t, read_kind(size), ax);
        sval = R_SRC;
        dval = R_VAL;
        areg = ax;
    }
    put(e, a64_ubfx(R_F1, R_FLAGS, 4, 1));
    if (size == 4)
    {
        if (sub)
            put(e, a64_addsub_reg(0, 1, 1, ZR, ZR, R_F1, 0, 0));
        else
            put(e, a64_addsub_imm(0, 1, 1, ZR, R_F1, 1, 0));
        put(e, a64_adc_sbc(0, sub, 1, R_SCR, dval, sval));
    }
    else
    {
        put(e, a64_lsl(R_SCR, dval, shift));
        put(e, a64_lsl(R_F2, sval, shift));
        put(e, a64_logic_reg(0, 1, 0, R_F2, R_F2, R_F1, 0, shift - 1));
        if (!sub)
            put(e, a64_logic_reg(0, 1, 0, R_SCR, R_SCR, R_F1, 0, shift - 1));
        put(e, a64_addsub_reg(0, sub, 1, R_SCR, R_SCR, R_F2, 0, 0));
    }
    if (in->need)
    {
        put(e, a64_mrs_nzcv(R_F1));
        put(e, a64_lsr(R_F1, R_F1, 28));
        if (sub)
            put(e, a64_logic_const(2, R_F1, R_F1, HJF_C));
    }
    if (size < 4)
        put(e, a64_lsr(R_SCR, R_SCR, shift));
    if (in->need)
    {
        put(e, a64_tst(R_SCR, R_SCR));
        put(e, a64_cset(R_F2, A64_EQ));
        put(e, a64_logic_reg(0, 0, 0, R_F2, R_F2, R_FLAGS, 1, 2));
        put(e, a64_bfi(R_F1, R_F2, 2, 1));
        put(e, a64_bfi(R_FLAGS, R_F1, 0, 4));
        put(e, a64_lsr(R_F2, R_F1, 1));
        put(e, a64_bfi(R_FLAGS, R_F2, 4, 1));
    }
    if (in->dst.mode == EA_DN)
        emit_insert(e, REG_D(in->dst.reg), R_SCR, size);
    else
    {
        put(e, a64_mov(R_VAL, R_SCR));
        emit_access(t, write_kind(size), areg);
        if (in->count == 1)
            emit_post(t, &in->dst, size);
    }
}

static void emit_bit(struct HJTrans *t, const struct HJInsn *in)
{
    struct HJEmit *e = &t->e;
    int v, areg;

    if (in->src.mode == EA_IMM)
    {
        unsigned bit = in->src.value & (in->dst.mode == EA_DN ? 31u : 7u);

        emit_mov32(e, R_RES, 1u << bit);
    }
    else
    {
        int sreg = REG_D(in->src.reg);

        if (in->dst.mode != EA_DN)
        {
            put(e, a64_logic_const(0, R_F1, sreg, 7u));
            sreg = R_F1;
        }
        emit_mov32(e, R_RES, 1u);
        put(e, a64_lslv(R_RES, R_RES, sreg));
    }
    if (in->op == BIT_TST)
    {
        v = emit_read(t, &in->dst, in->size);
        if (in->need & HJF_Z)
        {
            put(e, a64_tst(v, R_RES));
            put(e, a64_cset(R_F1, A64_EQ));
            put(e, a64_bfi(R_FLAGS, R_F1, 2, 1));
        }
        return;
    }
    v = in->dst.mode == EA_DN ? REG_D(in->dst.reg) : R_VAL;
    areg = in->dst.mode == EA_DN ? -1 : emit_address(t, &in->dst, 1);
    if (areg >= 0)
        emit_access(t, SLOW_R8, areg);
    if (in->need & HJF_Z)
    {
        put(e, a64_tst(v, R_RES));
        put(e, a64_cset(R_F1, A64_EQ));
        put(e, a64_bfi(R_FLAGS, R_F1, 2, 1));
    }
    if (in->op == BIT_CHG)
        put(e, a64_logic_reg(0, 2, 0, v, v, R_RES, 0, 0));
    else if (in->op == BIT_CLR)
        put(e, a64_logic_reg(0, 0, 1, v, v, R_RES, 0, 0));
    else
        put(e, a64_logic_reg(0, 1, 0, v, v, R_RES, 0, 0));
    if (areg >= 0)
    {
        emit_access(t, SLOW_W8, areg);
        emit_post(t, &in->dst, 1);
    }
}

static void emit_mull(struct HJTrans *t, const struct HJInsn *in)
{
    struct HJEmit *e = &t->e;
    int dl = REG_D(in->dst.reg), dh = REG_D(in->count & 7);
    int quad = (in->count >> 3) & 1, is_signed = in->op;
    int v = emit_read(t, &in->src, 4);

    put(e, is_signed ? a64_smull(R_SCR, v, dl) : a64_umull(R_SCR, v, dl));
    if (quad)
    {
        put(e, a64_bitfield(1, 2, dh, R_SCR, 32, 63));
        put(e, a64_mov(dl, R_SCR));
        if (in->need & HJF_NZVC)
        {
            put(e, a64_logic_reg(1, 3, 0, ZR, R_SCR, R_SCR, 0, 0));
            emit_nzcv_to_flags(e, 0, 0);
        }
        return;
    }
    put(e, a64_mov(dl, R_SCR));
    if (in->need & HJF_NZVC)
    {
        put(e, a64_tst(dl, dl));
        put(e, a64_mrs_nzcv(R_F1));
        put(e, a64_lsr(R_F1, R_F1, 28));
        if (is_signed)
        {
            put(e, a64_bitfield(1, 0, R_F2, dl, 0, 31));
            put(e, a64_addsub_reg(1, 1, 1, ZR, R_SCR, R_F2, 0, 0));
        }
        else
        {
            put(e, a64_bitfield(1, 2, R_F2, R_SCR, 32, 63));
            put(e, a64_addsub_imm(0, 1, 1, ZR, R_F2, 0, 0));
        }
        put(e, a64_cset(R_F2, A64_NE));
        put(e, a64_bfi(R_F1, R_F2, 0, 1));
        put(e, a64_bfi(R_FLAGS, R_F1, 0, 4));
    }
}

static void emit_divl(struct HJTrans *t, const struct HJInsn *in)
{
    struct HJEmit *e = &t->e;
    int dq = REG_D(in->dst.reg), dr = REG_D(in->count & 7);
    int quad = (in->count >> 3) & 1, is_signed = in->op;
    int v = emit_read(t, &in->src, 4);
    unsigned br_nz, br_ovf, br_done;

    br_nz = e->n;
    put(e, a64_cbz(0, 1, v, 0));
    emit_exit_reason(t, in->pc, HJ_EXIT_FAULT);
    patch(e, br_nz, e->n);
    if (!quad)
    {
        put(e, a64_div(0, is_signed, R_SCR, dq, v));
        put(e, a64_msub(0, R_F1, R_SCR, v, dq));
        put(e, a64_mov(dr, R_F1));
        put(e, a64_mov(dq, R_SCR));
        if (in->need & HJF_NZVC)
            emit_flags_logic(e, dq, 4);
        return;
    }
    put(e, a64_mov(R_RES, dq));
    put(e, a64_bitfield(1, 1, R_RES, dr, 32, 31));
    if (is_signed)
        put(e, a64_bitfield(1, 0, R_SRC, v, 0, 31));
    else
        put(e, a64_mov(R_SRC, v));
    put(e, a64_div(1, is_signed, R_SCR, R_RES, R_SRC));
    put(e, a64_msub(1, R_F1, R_SCR, R_SRC, R_RES));
    if (is_signed)
    {
        put(e, a64_bitfield(1, 0, R_F2, R_SCR, 0, 31));
        put(e, a64_addsub_reg(1, 1, 1, ZR, R_SCR, R_F2, 0, 0));
    }
    else
    {
        put(e, a64_bitfield(1, 2, R_F2, R_SCR, 32, 63));
        put(e, a64_addsub_imm(0, 1, 1, ZR, R_F2, 0, 0));
    }
    br_ovf = e->n;
    put(e, a64_bcond(A64_NE, 0));
    put(e, a64_mov(dr, R_F1));
    put(e, a64_mov(dq, R_SCR));
    if (in->need & HJF_NZVC)
        emit_flags_logic(e, dq, 4);
    br_done = e->n;
    put(e, a64_b(0));
    patch(e, br_ovf, e->n);
    if (in->need & HJF_V)
        put(e, a64_logic_const(1, R_FLAGS, R_FLAGS, HJF_V));
    patch(e, br_done, e->n);
}

static void emit_bf(struct HJTrans *t, const struct HJInsn *in)
{
    struct HJEmit *e = &t->e;
    uint16_t ext = in->mask;
    int dn = (ext >> 12) & 7;
    int do_reg = (ext >> 11) & 1, of = (ext >> 6) & 31;
    int dw_reg = (ext >> 5) & 1, wi = ext & 31;
    unsigned br_no5, br_no5_w;

    if (in->dst.mode != EA_DN)
    {
        int areg = emit_address(t, &in->dst, 1);

        if (areg != R_ADDR)
            put(e, a64_mov(R_ADDR, areg));
    }
    if (do_reg)
        put(e, a64_mov(R_SRC, REG_D(of & 7)));
    else
        emit_mov32(e, R_SRC, (uint32_t)of);
    if (dw_reg)
    {
        put(e, a64_addsub_imm(0, 1, 0, R_F1, REG_D(wi & 7), 1, 0));
        put(e, a64_logic_const(0, R_F1, R_F1, 31u));
        put(e, a64_addsub_imm(0, 0, 0, R_F1, R_F1, 1, 0));
    }
    else
        emit_mov32(e, R_F1, wi ? (uint32_t)wi : 32u);
    emit_mov32(e, R_F2, 32u);
    put(e, a64_addsub_reg(0, 1, 0, R_F2, R_F2, R_F1, 0, 0));

    if (in->dst.mode == EA_DN)
    {
        int dy = REG_D(in->dst.reg);

        put(e, a64_addsub_reg(0, 1, 0, R_SCR, ZR, R_SRC, 0, 0));
        put(e, a64_shiftv(0, 3, R_RES, dy, R_SCR));
        if (in->op == BF_INS)
        {
            put(e, a64_shiftv(0, 0, R_SCR, REG_D(dn), R_F2));
            put(e, a64_movw(0, 0, R_F1, 0, 0));
            put(e, a64_shiftv(0, 0, R_F1, R_F1, R_F2));
            put(e, a64_logic_reg(0, 0, 1, R_RES, R_RES, R_F1, 0, 0));
            put(e, a64_logic_reg(0, 1, 0, R_RES, R_RES, R_SCR, 0, 0));
            put(e, a64_shiftv(0, 3, dy, R_RES, R_SRC));
            if (in->need & HJF_NZVC)
                emit_flags_logic(e, R_SCR, 4);
            return;
        }
        put(e, a64_shiftv(0, 1, R_SCR, R_RES, R_F2));
        put(e, a64_shiftv(0, 0, R_SCR, R_SCR, R_F2));
        switch (in->op)
        {
        case BF_TST:
            break;
        case BF_EXTU:
            put(e, a64_shiftv(0, 1, REG_D(dn), R_RES, R_F2));
            break;
        case BF_EXTS:
            put(e, a64_shiftv(0, 2, REG_D(dn), R_RES, R_F2));
            break;
        case BF_FFO:
            put(e, a64_clz(R_F2, R_SCR));
            put(e, a64_cmp(R_F2, R_F1));
            put(e, a64_csel(0, R_F2, R_F2, R_F1, A64_LO));
            put(e, a64_addsub_reg(0, 0, 0, REG_D(dn), R_SRC, R_F2, 0, 0));
            break;
        default:
            put(e, a64_movw(0, 0, R_F1, 0, 0));
            put(e, a64_shiftv(0, 0, R_F1, R_F1, R_F2));
            if (in->op == BF_CHG)
                put(e, a64_logic_reg(0, 2, 0, R_RES, R_RES, R_F1, 0, 0));
            else if (in->op == BF_CLR)
                put(e, a64_logic_reg(0, 0, 1, R_RES, R_RES, R_F1, 0, 0));
            else
                put(e, a64_logic_reg(0, 1, 0, R_RES, R_RES, R_F1, 0, 0));
            put(e, a64_shiftv(0, 3, dy, R_RES, R_SRC));
            break;
        }
        if (in->need & HJF_NZVC)
            emit_flags_logic(e, R_SCR, 4);
        return;
    }

    put(e, a64_addsub_reg(0, 0, 0, R_ADDR, R_ADDR, R_SRC, 2, 3));
    emit_access(t, SLOW_R32, R_ADDR);
    put(e, a64_mov(R_RES, R_VAL));
    put(e, a64_bitfield(1, 2, R_RES, R_RES, 56, 55));
    put(e, a64_logic_const(0, R_SCR, R_SRC, 7u));
    put(e, a64_addsub_reg(0, 0, 0, R_SCR, R_SCR, R_F1, 0, 0));
    put(e, a64_addsub_imm(0, 1, 1, ZR, R_SCR, 32, 0));
    br_no5 = e->n;
    put(e, a64_bcond(A64_LS, 0));
    put(e, a64_addsub_imm(0, 0, 0, R_ADDR, R_ADDR, 4, 0));
    emit_access(t, SLOW_R8, R_ADDR);
    put(e, a64_addsub_imm(0, 1, 0, R_ADDR, R_ADDR, 4, 0));
    put(e, a64_logic_reg(1, 1, 0, R_RES, R_RES, R_VAL, 0, 0));
    patch(e, br_no5, e->n);
    emit_mov32(e, R_F2, 32u);
    put(e, a64_addsub_reg(0, 1, 0, R_F2, R_F2, R_F1, 0, 0));

    if (in->op == BF_TST || in->op == BF_EXTU || in->op == BF_EXTS || in->op == BF_FFO)
    {
        put(e, a64_logic_const(0, R_SCR, R_SRC, 7u));
        put(e, a64_shiftv(1, 0, R_SCR, R_RES, R_SCR));
        put(e, a64_bitfield(1, 2, R_SCR, R_SCR, 8, 63));
        put(e, a64_shiftv(0, 1, R_SCR, R_SCR, R_F2));
        if (in->op == BF_EXTU)
            put(e, a64_mov(REG_D(dn), R_SCR));
        put(e, a64_shiftv(0, 0, R_SCR, R_SCR, R_F2));
        if (in->op == BF_EXTS)
            put(e, a64_shiftv(0, 2, REG_D(dn), R_SCR, R_F2));
        else if (in->op == BF_FFO)
        {
            put(e, a64_clz(R_F2, R_SCR));
            put(e, a64_cmp(R_F2, R_F1));
            put(e, a64_csel(0, R_F2, R_F2, R_F1, A64_LO));
            put(e, a64_addsub_reg(0, 0, 0, REG_D(dn), R_SRC, R_F2, 0, 0));
        }
        if (in->need & HJF_NZVC)
            emit_flags_logic(e, R_SCR, 4);
        return;
    }

    if (in->op == BF_INS)
        put(e, a64_shiftv(0, 0, R_SCR, REG_D(dn), R_F2));
    else
    {
        put(e, a64_logic_const(0, R_SCR, R_SRC, 7u));
        put(e, a64_shiftv(1, 0, R_SCR, R_RES, R_SCR));
        put(e, a64_bitfield(1, 2, R_SCR, R_SCR, 8, 63));
        put(e, a64_shiftv(0, 1, R_SCR, R_SCR, R_F2));
        put(e, a64_shiftv(0, 0, R_SCR, R_SCR, R_F2));
    }
    if (in->need & HJF_NZVC)
    {
        put(e, a64_tst(R_SCR, R_SCR));
        put(e, a64_mrs_nzcv(R_VAL));
        put(e, a64_lsr(R_VAL, R_VAL, 28));
        put(e, a64_bfi(R_FLAGS, R_VAL, 0, 4));
    }
    put(e, a64_movw(0, 0, R_VAL, 0, 0));
    put(e, a64_shiftv(0, 0, R_VAL, R_VAL, R_F2));
    put(e, a64_bitfield(1, 2, R_VAL, R_VAL, 56, 55));
    put(e, a64_logic_const(0, R_F2, R_SRC, 7u));
    put(e, a64_shiftv(1, 1, R_VAL, R_VAL, R_F2));
    if (in->op == BF_CHG)
        put(e, a64_logic_reg(1, 2, 0, R_RES, R_RES, R_VAL, 0, 0));
    else if (in->op == BF_CLR)
        put(e, a64_logic_reg(1, 0, 1, R_RES, R_RES, R_VAL, 0, 0));
    else if (in->op == BF_SET)
        put(e, a64_logic_reg(1, 1, 0, R_RES, R_RES, R_VAL, 0, 0));
    else
    {
        put(e, a64_bitfield(1, 2, R_SCR, R_SCR, 56, 55));
        put(e, a64_shiftv(1, 1, R_SCR, R_SCR, R_F2));
        put(e, a64_logic_reg(1, 0, 1, R_RES, R_RES, R_VAL, 0, 0));
        put(e, a64_logic_reg(1, 1, 0, R_RES, R_RES, R_SCR, 0, 0));
    }
    put(e, a64_bitfield(1, 2, R_VAL, R_RES, 8, 63));
    emit_access(t, SLOW_W32, R_ADDR);
    put(e, a64_logic_const(0, R_SCR, R_SRC, 7u));
    put(e, a64_addsub_reg(0, 0, 0, R_SCR, R_SCR, R_F1, 0, 0));
    put(e, a64_addsub_imm(0, 1, 1, ZR, R_SCR, 32, 0));
    br_no5_w = e->n;
    put(e, a64_bcond(A64_LS, 0));
    put(e, a64_addsub_imm(0, 0, 0, R_ADDR, R_ADDR, 4, 0));
    put(e, a64_mov(R_VAL, R_RES));
    emit_access(t, SLOW_W8, R_ADDR);
    patch(e, br_no5_w, e->n);
}

static void emit_cas(struct HJTrans *t, const struct HJInsn *in)
{
    struct HJEmit *e = &t->e;
    int areg = emit_address(t, &in->dst, in->size);
    int dc = REG_D(in->src.reg), du = REG_D(in->count);
    int shift = 32 - 8 * in->size;
    unsigned br_ne, br_done;

    emit_access(t, read_kind(in->size), areg);
    put(e, a64_mov(R_RES, R_VAL));
    if (in->size == 4)
        put(e, a64_addsub_reg(0, 1, 1, ZR, R_RES, dc, 0, 0));
    else
    {
        put(e, a64_lsl(R_SCR, R_RES, shift));
        put(e, a64_addsub_reg(0, 1, 1, ZR, R_SCR, dc, 0, shift));
    }
    if (in->need & HJF_NZVC)
        emit_nzcv_to_flags(e, 1, 0);
    br_ne = e->n;
    put(e, a64_bcond(A64_NE, 0));
    put(e, a64_mov(R_VAL, du));
    emit_access(t, write_kind(in->size), areg);
    br_done = e->n;
    put(e, a64_b(0));
    patch(e, br_ne, e->n);
    emit_insert(e, dc, R_RES, in->size);
    patch(e, br_done, e->n);
    emit_post(t, &in->dst, in->size);
}

static void emit_movep(struct HJTrans *t, const struct HJInsn *in)
{
    struct HJEmit *e = &t->e;
    int dx = REG_D(in->dst.reg), size = in->size, i;

    emit_add_const(e, R_ADDR, REG_A(in->src.reg), in->src.disp);
    if (in->op)
    {
        for (i = 0; i < size; i++)
        {
            int shift = 8 * (size - 1 - i);

            if (shift)
                put(e, a64_lsr(R_VAL, dx, shift));
            else
                put(e, a64_mov(R_VAL, dx));
            emit_access(t, SLOW_W8, R_ADDR);
            if (i + 1 < size)
                put(e, a64_addsub_imm(0, 0, 0, R_ADDR, R_ADDR, 2, 0));
        }
        return;
    }
    for (i = 0; i < size; i++)
    {
        int shift = 8 * (size - 1 - i);

        emit_access(t, SLOW_R8, R_ADDR);
        if (i == 0)
            put(e, a64_lsl(R_RES, R_VAL, shift));
        else
            put(e, a64_bfi(R_RES, R_VAL, shift, 8));
        if (i + 1 < size)
            put(e, a64_addsub_imm(0, 0, 0, R_ADDR, R_ADDR, 2, 0));
    }
    emit_insert(e, dx, R_RES, size);
}

static void emit_shift(struct HJTrans *t, const struct HJInsn *in)
{
    struct HJEmit *e = &t->e;
    int dn = REG_D(in->dst.reg), n = in->count;
    unsigned need = in->need;

    if (in->size == 4 && in->src.mode == EA_IMM && in->dst.mode == EA_DN &&
        in->op != SH_ASL && in->op != SH_ROXL && in->op != SH_ROXR)
    {
        switch (in->op)
        {
        case SH_LSL:
            if (need) put(e, a64_ubfx(R_F2, dn, 32 - n, 1));
            put(e, a64_lsl(dn, dn, n));
            break;
        case SH_LSR:
        case SH_ASR:
            if (need) put(e, a64_ubfx(R_F2, dn, n - 1, 1));
            put(e, in->op == SH_LSR ? a64_lsr(dn, dn, n) : a64_asr(dn, dn, n));
            break;
        case SH_ROL:
            put(e, a64_ror(dn, dn, 32 - n));
            if (need) put(e, a64_ubfx(R_F2, dn, 0, 1));
            break;
        default:
            put(e, a64_ror(dn, dn, n));
            if (need) put(e, a64_lsr(R_F2, dn, 31));
            break;
        }
        if (need & HJF_NZVC)
        {
            put(e, a64_tst(dn, dn));
            put(e, a64_mrs_nzcv(R_F1));
            put(e, a64_lsr(R_F1, R_F1, 28));
            put(e, a64_bfi(R_F1, R_F2, 1, 1));
            put(e, a64_bfi(R_FLAGS, R_F1, 0, 4));
        }
        if ((need & HJF_X) && in->op != SH_ROL && in->op != SH_ROR)
            put(e, a64_bfi(R_FLAGS, R_F2, 4, 1));
        return;
    }
    {
        int w = 8 * in->size, shift = 32 - w, areg = -1;
        unsigned br_zero, loop_top;

        if (in->dst.mode == EA_DN)
            put(e, in->size == 1 ? a64_uxtb(R_RES, dn) :
                   in->size == 2 ? a64_uxth(R_RES, dn) : a64_mov(R_RES, dn));
        else
        {
            areg = emit_address(t, &in->dst, 2);
            emit_access(t, SLOW_R16, areg);
            put(e, a64_uxth(R_RES, R_VAL));
        }
        if (in->src.mode == EA_IMM)
            emit_mov32(e, R_SRC, in->count);
        else
            put(e, a64_logic_const(0, R_SRC, REG_D(in->src.reg), 0x3Fu));
        if (in->op == SH_ROXL || in->op == SH_ROXR)
            put(e, a64_ubfx(R_F1, R_FLAGS, 4, 1));
        else
            put(e, a64_mov(R_F1, ZR));
        if (in->op == SH_ASL)
            put(e, a64_mov(R_F2, ZR));
        br_zero = e->n;
        put(e, a64_cbz(0, 0, R_SRC, 0));
        loop_top = e->n;
        switch (in->op)
        {
        case SH_LSL:
            put(e, a64_ubfx(R_F1, R_RES, w - 1, 1));
            put(e, a64_lsl(R_RES, R_RES, 1));
            break;
        case SH_ASL:
            put(e, a64_ubfx(R_F1, R_RES, w - 1, 1));
            put(e, a64_lsl(R_SCR, R_RES, 1));
            put(e, a64_logic_reg(0, 2, 0, R_SCR, R_RES, R_SCR, 0, 0));
            put(e, a64_logic_reg(0, 1, 0, R_F2, R_F2, R_SCR, 0, 0));
            put(e, a64_lsl(R_RES, R_RES, 1));
            break;
        case SH_ROL:
            put(e, a64_ubfx(R_F1, R_RES, w - 1, 1));
            put(e, a64_lsl(R_RES, R_RES, 1));
            put(e, a64_logic_reg(0, 1, 0, R_RES, R_RES, R_F1, 0, 0));
            break;
        case SH_ROXL:
            put(e, a64_mov(R_SCR, R_F1));
            put(e, a64_ubfx(R_F1, R_RES, w - 1, 1));
            put(e, a64_lsl(R_RES, R_RES, 1));
            put(e, a64_logic_reg(0, 1, 0, R_RES, R_RES, R_SCR, 0, 0));
            break;
        case SH_LSR:
            put(e, a64_ubfx(R_F1, R_RES, 0, 1));
            put(e, a64_lsr(R_RES, R_RES, 1));
            break;
        case SH_ASR:
            put(e, a64_ubfx(R_F1, R_RES, 0, 1));
            put(e, a64_ubfx(R_SCR, R_RES, w - 1, 1));
            put(e, a64_lsr(R_RES, R_RES, 1));
            put(e, a64_bfi(R_RES, R_SCR, w - 1, 1));
            break;
        case SH_ROR:
            put(e, a64_ubfx(R_F1, R_RES, 0, 1));
            put(e, a64_lsr(R_RES, R_RES, 1));
            put(e, a64_bfi(R_RES, R_F1, w - 1, 1));
            break;
        default:
            put(e, a64_mov(R_SCR, R_F1));
            put(e, a64_ubfx(R_F1, R_RES, 0, 1));
            put(e, a64_lsr(R_RES, R_RES, 1));
            put(e, a64_bfi(R_RES, R_SCR, w - 1, 1));
            break;
        }
        put(e, a64_addsub_imm(0, 1, 1, R_SRC, R_SRC, 1, 0));
        put(e, a64_bcond(A64_NE, (int32_t)loop_top - (int32_t)e->n));
        if ((need & HJF_X) && in->op != SH_ROL && in->op != SH_ROR)
            put(e, a64_bfi(R_FLAGS, R_F1, 4, 1));
        patch(e, br_zero, e->n);
        if (need & HJF_NZVC)
        {
            if (in->size == 4)
                put(e, a64_tst(R_RES, R_RES));
            else
            {
                put(e, a64_lsl(R_SCR, R_RES, shift));
                put(e, a64_tst(R_SCR, R_SCR));
            }
            put(e, a64_mrs_nzcv(R_SCR));
            put(e, a64_lsr(R_SCR, R_SCR, 28));
            put(e, a64_bfi(R_SCR, R_F1, 1, 1));
            if (in->op == SH_ASL)
            {
                put(e, a64_ubfx(R_F2, R_F2, w - 1, 1));
                put(e, a64_bfi(R_SCR, R_F2, 0, 1));
            }
            put(e, a64_bfi(R_FLAGS, R_SCR, 0, 4));
        }
        if (in->dst.mode == EA_DN)
            emit_insert(e, dn, R_RES, in->size);
        else
        {
            put(e, a64_mov(R_VAL, R_RES));
            emit_access(t, SLOW_W16, areg);
            emit_post(t, &in->dst, 2);
        }
    }
}

static void emit_movem(struct HJTrans *t, const struct HJInsn *in)
{
    struct HJEmit *e = &t->e;
    int r, an = REG_A(in->src.reg), step = in->size;
    int rkind = read_kind(step), wkind = write_kind(step);

    if (in->count == 0 && in->src.mode == EA_PRE)
    {
        for (r = 15; r >= 0; r--)
            if (in->mask & (1u << r))
            {
                put(e, a64_addsub_imm(0, 1, 0, an, an, (uint32_t)step, 0));
                put(e, a64_mov(R_VAL, r));
                emit_access(t, wkind, an);
            }
        return;
    }
    if (in->count == 1 && in->src.mode == EA_POST)
    {
        for (r = 0; r < 16; r++)
            if (in->mask & (1u << r))
            {
                emit_access(t, rkind, an);
                put(e, a64_addsub_imm(0, 0, 0, an, an, (uint32_t)step, 0));
                put(e, step == 2 ? a64_sxth(r, R_VAL) : a64_mov(r, R_VAL));
            }
        return;
    }
    put(e, a64_mov(R_RES, emit_address(t, &in->src, step)));
    for (r = 0; r < 16; r++)
        if (in->mask & (1u << r))
        {
            if (in->count == 0)
            {
                put(e, a64_mov(R_VAL, r));
                emit_access(t, wkind, R_RES);
            }
            else
            {
                emit_access(t, rkind, R_RES);
                put(e, step == 2 ? a64_sxth(r, R_VAL) : a64_mov(r, R_VAL));
            }
            put(e, a64_addsub_imm(0, 0, 0, R_RES, R_RES, (uint32_t)step, 0));
        }
}

/* 68k CCR (C bit 0, V bit 1) to and from the flags (V bit 0, C bit 1). */
static uint32_t swap_cv(uint32_t f) { return (f & 0x1Cu) | ((f & 1u) << 1) | ((f >> 1) & 1u); }

static void emit_ccr_to_flags(struct HJEmit *e, int src)
{
    put(e, a64_logic_const(0, R_F1, src, 0x1C));
    put(e, a64_ubfx(R_F2, src, 0, 1));
    put(e, a64_logic_reg(0, 1, 0, R_F1, R_F1, R_F2, 0, 1));
    put(e, a64_ubfx(R_F2, src, 1, 1));
    put(e, a64_logic_reg(0, 1, 0, R_FLAGS, R_F1, R_F2, 0, 0));
}

static void emit_insn(struct HJTrans *t, const struct HJInsn *in)
{
    struct HJEmit *e = &t->e;
    int v, an, size = in->size;
    unsigned br, br2;

    t->in = in;
    switch (in->kind)
    {
    case K_NOP:
        break;
    case K_MOVEQ:
        emit_mov32(e, REG_D(in->dst.reg), in->src.value);
        if (in->need)
            emit_flags_const(e, in->src.value == 0 ? HJF_Z : (in->src.value >> 31) ? HJF_N : 0);
        break;
    case K_MOVE:
        if (in->src.mode == EA_IMM && in->dst.mode == EA_DN && size == 4)
        {
            emit_mov32(e, REG_D(in->dst.reg), in->src.value);
            if (in->need)
                emit_flags_const(e, in->src.value == 0 ? HJF_Z : (in->src.value >> 31) ? HJF_N : 0);
            break;
        }
        v = emit_read(t, &in->src, size);
        if (in->src.mode == EA_AN && (in->dst.mode == EA_PRE || in->dst.mode == EA_POST) &&
            in->dst.reg == in->src.reg)
        {
            put(e, a64_mov(R_SRC, v));
            v = R_SRC;
        }
        if (in->need)
            emit_flags_logic(e, v, size);
        emit_write(t, &in->dst, size, v);
        break;
    case K_MOVEA:
        v = emit_read(t, &in->src, size);
        an = REG_A(in->dst.reg);
        put(e, size == 2 ? a64_sxth(an, v) : a64_mov(an, v));
        break;
    case K_LEA:
        an = emit_address(t, &in->src, 4);
        if (an != REG_A(in->dst.reg))
            put(e, a64_mov(REG_A(in->dst.reg), an));
        break;
    case K_PEA:
        an = emit_address(t, &in->src, 4);
        put(e, a64_mov(R_RES, an));
        put(e, a64_addsub_imm(0, 1, 0, REG_A(7), REG_A(7), 4, 0));
        put(e, a64_mov(R_VAL, R_RES));
        emit_access(t, SLOW_W32, REG_A(7));
        break;
    case K_LINK:
        put(e, a64_addsub_imm(0, 1, 0, REG_A(7), REG_A(7), 4, 0));
        put(e, a64_mov(R_VAL, REG_A(in->dst.reg)));
        emit_access(t, SLOW_W32, REG_A(7));
        if (in->dst.reg != 7)
            put(e, a64_mov(REG_A(in->dst.reg), REG_A(7)));
        emit_add_const(e, REG_A(7), REG_A(7), in->src.disp);
        break;
    case K_UNLK:
        if (in->dst.reg != 7)
            put(e, a64_mov(REG_A(7), REG_A(in->dst.reg)));
        emit_access(t, SLOW_R32, REG_A(7));
        if (in->dst.reg != 7)
            put(e, a64_addsub_imm(0, 0, 0, REG_A(7), REG_A(7), 4, 0));
        put(e, a64_mov(REG_A(in->dst.reg), R_VAL));
        break;
    case K_EXG:
        if (in->src.reg != in->dst.reg)
        {
            put(e, a64_mov(R_SCR, in->src.reg));
            put(e, a64_mov(in->src.reg, in->dst.reg));
            put(e, a64_mov(in->dst.reg, R_SCR));
        }
        break;
    case K_CMPM:
    {
        int ay = REG_A(in->src.reg), ax = REG_A(in->dst.reg);

        emit_access(t, read_kind(size), ay);
        put(e, a64_addsub_imm(0, 0, 0, ay, ay, (uint32_t)step_of(size, in->src.reg), 0));
        put(e, a64_mov(R_SRC, R_VAL));
        emit_access(t, read_kind(size), ax);
        put(e, a64_addsub_imm(0, 0, 0, ax, ax, (uint32_t)step_of(size, in->dst.reg), 0));
        emit_alu(t, OP_CMP, size, R_VAL, R_SRC, ZR, in->need);
        break;
    }
    case K_ALU:
        v = emit_read(t, &in->src, size);
        emit_alu(t, in->op, size, REG_D(in->dst.reg), v, REG_D(in->dst.reg), in->need);
        break;
    case K_ALUM:
        if (in->dst.mode == EA_DN)
            emit_alu(t, in->op, size, REG_D(in->dst.reg), REG_D(in->src.reg), REG_D(in->dst.reg), in->need);
        else
            emit_rmw(t, in, in->op, REG_D(in->src.reg));
        break;
    case K_ALUI:
        emit_mov32(e, R_SRC, in->src.value);
        if (in->dst.mode == EA_DN)
            emit_alu(t, in->op, size, REG_D(in->dst.reg), R_SRC, REG_D(in->dst.reg), in->need);
        else
            emit_rmw(t, in, in->op, R_SRC);
        break;
    case K_QUICK:
        if (in->dst.mode == EA_AN)
        {
            an = REG_A(in->dst.reg);
            put(e, a64_addsub_imm(0, in->op == OP_SUB, 0, an, an, in->count, 0));
            break;
        }
        emit_mov32(e, R_SRC, in->count);
        if (in->dst.mode == EA_DN)
            emit_alu(t, in->op, size, REG_D(in->dst.reg), R_SRC, REG_D(in->dst.reg), in->need);
        else
            emit_rmw(t, in, in->op, R_SRC);
        break;
    case K_ADDX:
        emit_addx(t, in);
        break;
    case K_MULW:
        v = emit_read(t, &in->src, 2);
        an = REG_D(in->dst.reg);
        put(e, in->op ? a64_sxth(R_SCR, v) : a64_uxth(R_SCR, v));
        put(e, in->op ? a64_sxth(R_F1, an) : a64_uxth(R_F1, an));
        put(e, a64_mul(an, R_SCR, R_F1));
        if (in->need)
            emit_flags_logic(e, an, 4);
        break;
    case K_MULL:
        emit_mull(t, in);
        break;
    case K_DIVL:
        emit_divl(t, in);
        break;
    case K_BIT:
        emit_bit(t, in);
        break;
    case K_BF:
        emit_bf(t, in);
        break;
    case K_CAS:
        emit_cas(t, in);
        break;
    case K_MOVEP:
        emit_movep(t, in);
        break;
    case K_TAS:
        if (in->dst.mode == EA_DN)
        {
            int dn = REG_D(in->dst.reg);

            if (in->need)
                emit_flags_logic(e, dn, 1);
            put(e, a64_logic_const(1, dn, dn, 0x80u));
            break;
        }
        an = emit_address(t, &in->dst, 1);
        emit_access(t, SLOW_R8, an);
        if (in->need)
            emit_flags_logic(e, R_VAL, 1);
        put(e, a64_logic_const(1, R_VAL, R_VAL, 0x80u));
        emit_access(t, SLOW_W8, an);
        emit_post(t, &in->dst, 1);
        break;
    case K_ALUA:
        v = emit_read(t, &in->src, size);
        if (size == 2)
        {
            put(e, a64_sxth(R_SRC, v));
            v = R_SRC;
        }
        an = REG_A(in->dst.reg);
        if (in->op == OP_CMP)
        {
            put(e, a64_addsub_reg(0, 1, in->need != 0, ZR, an, v, 0, 0));
            emit_flags_arith(e, 1, in->need & HJF_NZVC);
        }
        else
            put(e, a64_addsub_reg(0, in->op == OP_SUB, 0, an, an, v, 0, 0));
        break;
    case K_CLR:
        if (in->dst.mode == EA_DN)
        {
            int dn = REG_D(in->dst.reg);

            if (size == 4)
                put(e, a64_mov(dn, ZR));
            else
                put(e, a64_logic_const(0, dn, dn, size == 2 ? 0xFFFF0000u : 0xFFFFFF00u));
        }
        else
            emit_write(t, &in->dst, size, ZR);
        if (in->need)
            emit_flags_const(e, HJF_Z);
        break;
    case K_TST:
        v = emit_read(t, &in->dst, size);
        if (in->need)
            emit_flags_logic(e, v, size);
        break;
    case K_NEG:
    case K_NOT:
    {
        int dn = in->dst.mode == EA_DN ? REG_D(in->dst.reg) : -1;
        int areg = -1, val;

        if (dn < 0)
        {
            areg = emit_address(t, &in->dst, size);
            emit_access(t, read_kind(size), areg);
            val = R_VAL;
        }
        else
            val = dn;
        if (in->kind == K_NEG)
            emit_alu(t, OP_SUB, size, ZR, val, dn < 0 ? R_RES : dn, in->need);
        else
        {
            put(e, a64_logic_reg(0, 1, 1, R_SCR, ZR, val, 0, 0));
            if (in->need)
                emit_flags_logic(e, R_SCR, size);
            emit_insert(e, dn < 0 ? R_RES : dn, R_SCR, dn < 0 ? 4 : size);
        }
        if (dn < 0)
        {
            put(e, a64_mov(R_VAL, R_RES));
            emit_access(t, write_kind(size), areg);
            emit_post(t, &in->dst, size);
        }
        break;
    }
    case K_SWAP:
        put(e, a64_ror(REG_D(in->dst.reg), REG_D(in->dst.reg), 16));
        if (in->need)
            emit_flags_logic(e, REG_D(in->dst.reg), 4);
        break;
    case K_EXTW:
        put(e, a64_sxtb(R_SCR, REG_D(in->dst.reg)));
        put(e, a64_bfi(REG_D(in->dst.reg), R_SCR, 0, 16));
        if (in->need)
            emit_flags_logic(e, R_SCR, 4);
        break;
    case K_EXTL:
        put(e, a64_sxth(REG_D(in->dst.reg), REG_D(in->dst.reg)));
        if (in->need)
            emit_flags_logic(e, REG_D(in->dst.reg), 4);
        break;
    case K_EXTBL:
        put(e, a64_sxtb(REG_D(in->dst.reg), REG_D(in->dst.reg)));
        if (in->need)
            emit_flags_logic(e, REG_D(in->dst.reg), 4);
        break;
    case K_SHIFT:
        emit_shift(t, in);
        break;
    case K_SCC:
        br = emit_cond_branch(e, in->cond);
        put(e, a64_mov(R_RES, ZR));
        br2 = e->n;
        put(e, a64_b(0));
        if (br != ~0u)
            patch(e, br, e->n);
        emit_mov32(e, R_RES, 0xFF);
        patch(e, br2, e->n);
        emit_write(t, &in->dst, 1, R_RES);
        break;
    case K_MOVEFROMSR:
    case K_MOVEFROMCCR:
        put(e, a64_logic_const(0, R_F1, R_FLAGS, 0x1C));
        put(e, a64_ubfx(R_F2, R_FLAGS, 1, 1));
        put(e, a64_logic_reg(0, 1, 0, R_F1, R_F1, R_F2, 0, 0));
        put(e, a64_ubfx(R_F2, R_FLAGS, 0, 1));
        put(e, a64_logic_reg(0, 1, 0, R_RES, R_F1, R_F2, 0, 1));
        if (in->kind == K_MOVEFROMSR)
        {
            put(e, a64_ldst_uimm(2, 1, R_F2, R_STATE, ST_SRHI));
            put(e, a64_logic_reg(0, 1, 0, R_RES, R_RES, R_F2, 0, 8));
        }
        emit_write(t, &in->dst, 2, R_RES);
        break;
    case K_MOVETOSR:
    case K_MOVETOCCR:
        v = emit_read(t, &in->src, 2);
        if (in->kind == K_MOVETOSR)
        {
            put(e, a64_ubfx(R_F1, v, 8, 8));
            put(e, a64_logic_const(0, R_F1, R_F1, ~0x08u));   /* no SR bit 11 */
            put(e, a64_ldst_uimm(2, 0, R_F1, R_STATE, ST_SRHI));
        }
        emit_ccr_to_flags(e, v);
        break;
    case K_LOGICSR:
    {
        int opc = in->op == OP_AND ? 0 : in->op == OP_OR ? 1 : 2;
        uint32_t fmask = swap_cv(in->src.value & 0x1Fu);

        emit_mov32(e, R_F1, fmask);
        put(e, a64_logic_reg(0, opc, 0, R_FLAGS, R_FLAGS, R_F1, 0, 0));
        if (size == 2)
        {
            uint32_t smask = (in->src.value >> 8) & 0xF7u;

            put(e, a64_ldst_uimm(2, 1, R_F1, R_STATE, ST_SRHI));
            emit_mov32(e, R_F2, smask);
            put(e, a64_logic_reg(0, opc, 0, R_F1, R_F1, R_F2, 0, 0));
            put(e, a64_logic_const(0, R_F1, R_F1, ~0x08u));
            put(e, a64_ldst_uimm(2, 0, R_F1, R_STATE, ST_SRHI));
        }
        break;
    }
    case K_MOVEM:
        emit_movem(t, in);
        break;
    case K_BCC:
        if (in->cond == 0)
        {
            emit_exit_to(t, in->target);
            break;
        }
        br = emit_cond_branch(e, in->cond);
        emit_exit_to(t, in->next);
        if (br != ~0u)
        {
            patch(e, br, e->n);
            emit_exit_to(t, in->target);
        }
        break;
    case K_BSR:
        emit_push(t, in->next);
        emit_exit_to(t, in->target);
        break;
    case K_DBCC:
    {
        int dn = REG_D(in->dst.reg);

        br = emit_cond_branch(e, in->cond);
        put(e, a64_logic_const(3, ZR, dn, 0xFFFF));
        put(e, a64_addsub_imm(0, 1, 0, R_F1, dn, 1, 0));
        put(e, a64_bfi(dn, R_F1, 0, 16));
        br2 = e->n;
        put(e, a64_bcond(A64_EQ, 0));
        emit_exit_to(t, in->target);
        if (br != ~0u)
            patch(e, br, e->n);
        patch(e, br2, e->n);
        emit_exit_to(t, in->next);
        break;
    }
    case K_JSR:
    case K_JMP:
        if (in->src.mode == EA_ABS || in->src.mode == EA_PCDISP)
        {
            if (in->kind == K_JSR)
                emit_push(t, in->next);
            emit_exit_to(t, in->src.value);
            break;
        }
        put(e, a64_mov(R_RES, emit_address(t, &in->src, 4)));
        if (in->kind == K_JSR)
            emit_push(t, in->next);
        put(e, a64_mov(R_ADDR, R_RES));
        emit_glue_jump(t, t->g_lookup);
        break;
    case K_RTS:
        emit_access(t, SLOW_R32, REG_A(7));
        put(e, a64_addsub_imm(0, 0, 0, REG_A(7), REG_A(7), 4, 0));
        put(e, a64_mov(R_ADDR, R_VAL));
        emit_glue_jump(t, t->g_lookup);
        break;
    case K_RTR:
        emit_access(t, SLOW_R16, REG_A(7));
        put(e, a64_addsub_imm(0, 0, 0, REG_A(7), REG_A(7), 2, 0));
        emit_ccr_to_flags(e, R_VAL);
        emit_access(t, SLOW_R32, REG_A(7));
        put(e, a64_addsub_imm(0, 0, 0, REG_A(7), REG_A(7), 4, 0));
        put(e, a64_mov(R_ADDR, R_VAL));
        emit_glue_jump(t, t->g_lookup);
        break;
    case K_RTD:
        emit_access(t, SLOW_R32, REG_A(7));
        emit_add_const(e, REG_A(7), REG_A(7), 4 + in->src.disp);
        put(e, a64_mov(R_ADDR, R_VAL));
        emit_glue_jump(t, t->g_lookup);
        break;
    case K_LINEA:
        emit_exit_reason(t, in->pc, HJ_EXIT_LINEA);
        break;
    default:
        emit_exit_reason(t, in->pc, HJ_EXIT_FAULT);
        break;
    }
}

/* Decode and emit a block of at most 'max' instructions at pc into t->e.
   Returns the number of 68k instructions and sets *end. */
static unsigned translate_into(struct KaffeineContext *ctx, struct HJTrans *t, uint32_t pc,
                               unsigned max, uint32_t *end)
{
    struct HJInsn insns[HJ_MAX_INSNS];
    unsigned n = 0, i;
    uint32_t at = pc;

    while (n < max)
    {
        decode_insn(ctx, at, &insns[n]);
        at = insns[n].next;
        if (is_terminator(insns[n++].kind))
            break;
    }
    flag_liveness(insns, n);
    t->start = pc;
    t->nstub = 0;
    for (i = 0; i < n; i++)
        emit_insn(t, &insns[i]);
    if (!is_terminator(insns[n - 1].kind))
        emit_exit_to(t, insns[n - 1].next);
    emit_stubs(t);
    /* After every word decoded, the extension words of a faulting
       instruction too: a new one can make it valid. translate() keeps the
       end inside guest memory. */
    *end = insns[n - 1].next;
    return n;
}

/* ------------------------------------------------------------------
   Code memory
   ------------------------------------------------------------------ */

static void code_write_begin(struct HJBackend *b)
{
    b->stats.write_windows++;
#if HJ_RUNS && defined(__APPLE__)
    pthread_jit_write_protect_np(0);
#elif HJ_RUNS && defined(__AROS__)
    /* Native AROS AArch64 ordinary RAM is writable and executable.
       Publication still needs its explicit data/instruction cache API. */
    (void)b;
#elif HJ_RUNS
    mprotect(b->code, HJ_CODE_BYTES, PROT_READ | PROT_WRITE);
#else
    (void)b;
#endif
}

static void code_write_end(struct HJBackend *b, const uint32_t *start, size_t words)
{
    b->stats.icache_syncs++;
#if HJ_RUNS && defined(__APPLE__)
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate((void *)(uintptr_t)start, words * 4);
#elif HJ_RUNS && defined(__AROS__)
    CacheClearE((APTR)start, words * sizeof(*start), CACRF_ClearD | CACRF_ClearI);
#elif HJ_RUNS
    mprotect(b->code, HJ_CODE_BYTES, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)(uintptr_t)start, (char *)(uintptr_t)(start + words));
#else
    (void)b; (void)start; (void)words;
#endif
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

#if HJ_RUNS
static void *code_alloc(void)
{
#if defined(__AROS__)
    return AllocMem(HJ_CODE_BYTES, MEMF_PUBLIC | MEMF_EXECUTABLE);
#else
    void *code;
#if defined(__APPLE__)
    code = mmap(NULL, HJ_CODE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
#else
    code = mmap(NULL, HJ_CODE_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    return code == MAP_FAILED ? NULL : code;
#endif
}

static void code_free(void *code)
{
#if defined(__AROS__)
    FreeMem(code, HJ_CODE_BYTES);
#else
    munmap(code, HJ_CODE_BYTES);
#endif
}
#endif

static void fast_clear(struct HJBackend *b)
{
    unsigned i;

    for (i = 0; i < HJ_FAST_SIZE; i++)
    {
        b->fast[i].pc = HJ_EMPTY_TAG;
        b->fast[i].entry = 0;
    }
}

static void reset_code(struct HJBackend *b)
{
    b->code_used = b->glue_words;
    memset(b->buckets, 0, sizeof(b->buckets));
    b->nblocks = 0;
    b->guest_used = 0;
    fast_clear(b);
    b->stats.resets++;
}

static void glue_regs(struct HJEmit *e, int load)
{
    int i;

    for (i = 0; i < 16; i += 2)
        put(e, a64_ldstp(0, load, 2, i, i + 1, R_STATE, 4 * i));
}

static void emit_glue(struct HJBackend *b, struct HJEmit *e)
{
    unsigned abort_at, i;
    uintptr_t common;

    /* enter(state, entry) */
    b->g_enter = e->origin + 4u * e->n;
    put(e, a64_ldstp(1, 0, 3, 29, 30, SP, -96));
    put(e, a64_addsub_imm(1, 0, 0, 29, SP, 0, 0));
    for (i = 0; i < 5; i++)
        put(e, a64_ldstp(1, 0, 2, 19 + 2 * i, 20 + 2 * i, SP, 16 + 16 * (int)i));
    put(e, a64_mov64(R_STATE, 0));
    put(e, a64_mov64(R_VAL, 1));
    put(e, a64_ldst_uimm(3, 1, R_MEM, R_STATE, ST_MEM));
    put(e, a64_ldst_uimm(2, 1, R_FLAGS, R_STATE, ST_FLAGS));
    put(e, a64_ldst_uimm(2, 1, R_LIMIT, R_STATE, ST_LIMIT));
    emit_mov32(e, R_BUDGET, HJ_BUDGET);
    glue_regs(e, 1);
    put(e, a64_br(R_VAL));

    /* exit: w16 = next PC, w17 = reason */
    b->g_exit = e->origin + 4u * e->n;
    put(e, a64_ldst_uimm(2, 0, R_ADDR, R_STATE, ST_PC));
    glue_regs(e, 0);
    put(e, a64_ldst_uimm(2, 0, R_FLAGS, R_STATE, ST_FLAGS));
    put(e, a64_mov(0, R_VAL));
    for (i = 0; i < 5; i++)
        put(e, a64_ldstp(1, 1, 2, 19 + 2 * i, 20 + 2 * i, SP, 16 + 16 * (int)i));
    put(e, a64_ldstp(1, 1, 1, 29, 30, SP, 96));
    put(e, A64_RET);

    b->g_budget = e->origin + 4u * e->n;
    emit_mov32(e, R_VAL, HJ_EXIT_BUDGET);
    put(e, a64_b(words_to(e, b->g_exit)));
    b->g_miss = e->origin + 4u * e->n;
    emit_mov32(e, R_VAL, HJ_EXIT_LOOKUP);
    put(e, a64_b(words_to(e, b->g_exit)));

    /* lookup: w16 = guest PC; one probe of the fast table */
    b->g_lookup = e->origin + 4u * e->n;
    put(e, a64_addsub_imm(0, 1, 1, R_BUDGET, R_BUDGET, 1, 0));
    put(e, a64_bcond(A64_EQ, words_to(e, b->g_budget)));
    put(e, a64_ldst_uimm(3, 1, R_VAL, R_STATE, ST_FAST));
    put(e, a64_bitfield(1, 2, R_SCR, R_ADDR, 1, HJ_FAST_BITS));
    put(e, a64_addsub_reg(1, 0, 0, R_VAL, R_VAL, R_SCR, 0, 4));
    put(e, a64_ldstp(1, 1, 2, R_SCR, R_VAL, R_VAL, 0));
    put(e, a64_addsub_reg(1, 1, 1, ZR, R_SCR, R_ADDR, 0, 0));
    put(e, a64_bcond(A64_NE, words_to(e, b->g_miss)));
    put(e, a64_br(R_VAL));

    /* slow access: w16 address (kept), w17 value, w25 kind, w27 guest PC */
    common = e->origin + 4u * e->n;
    put(e, a64_ldstp(1, 0, 3, 29, 30, SP, -32));
    put(e, a64_addsub_imm(1, 0, 0, 29, SP, 0, 0));
    put(e, a64_ldst_uimm(3, 0, R_ADDR, SP, 16));
    glue_regs(e, 0);
    put(e, a64_mov64(0, R_STATE));
    put(e, a64_mov(1, R_ADDR));
    put(e, a64_mov(2, R_VAL));
    put(e, a64_mov(3, R_SCR));
    put(e, a64_mov(4, R_F2));
    put(e, a64_ldst_uimm(3, 1, R_ADDR, R_STATE, ST_SLOW));
    put(e, a64_blr(R_ADDR));
    put(e, a64_mov(R_VAL, 0));
    glue_regs(e, 1);
    put(e, a64_ldst_uimm(2, 1, R_ADDR, R_STATE, ST_ABORT));
    abort_at = e->n;
    put(e, a64_cbz(0, 1, R_ADDR, 0));
    put(e, a64_ldst_uimm(3, 1, R_ADDR, SP, 16));
    put(e, a64_ldstp(1, 1, 1, 29, 30, SP, 32));
    put(e, A64_RET);
    patch(e, abort_at, e->n);
    put(e, a64_ldstp(1, 1, 1, 29, 30, SP, 32));
    put(e, a64_mov(R_ADDR, ZR));
    emit_mov32(e, R_VAL, HJ_EXIT_ABORT);
    put(e, a64_b(words_to(e, b->g_exit)));
    for (i = 0; i < SLOW_KINDS; i++)
    {
        b->g_slow[i] = e->origin + 4u * e->n;
        emit_mov32(e, R_SCR, i);
        put(e, a64_b(words_to(e, common)));
    }
}

/* ------------------------------------------------------------------
   The C side: faults, slow accesses, blocks
   ------------------------------------------------------------------ */

static void stop_fault(struct HJBackend *b, const char *kind, uint32_t pc)
{
    struct KaffeineContext *ctx = b->ctx;

    b->faulted = 1;
    ctx->running = FALSE;
    ctx->exit_code = RETURN_FAIL;
    b->st.d[0] = RETURN_FAIL;
    if (ctx->callback_depth)
        ctx->callback_failed = TRUE;
    b->st.abort = 1;
    snprintf(b->fault, sizeof(b->fault), "%s at PC=0x%08lx", kind, (unsigned long)pc);
}

/* The custom chip registers as AROSMoira emulates them. */
static void beam_advance(struct KaffeineContext *ctx, int ticks)
{
    ctx->beam_h = (UBYTE)(ctx->beam_h + ticks);
    while (ctx->beam_h >= PAL_CLOCKS_PER_LINE)
    {
        ctx->beam_h = (UBYTE)(ctx->beam_h - PAL_CLOCKS_PER_LINE);
        ctx->beam_v++;
        if (ctx->beam_v >= PAL_LINES_PER_FRAME)
        {
            ctx->beam_v = 0;
            ctx->beam_lof ^= 1;
        }
    }
}

static uint16_t custom_read16(struct KaffeineContext *ctx, uint16_t reg)
{
    beam_advance(ctx, 2);
    switch (reg)
    {
    case 0x004: return (uint16_t)((uint16_t)ctx->beam_lof << 15 | ((ctx->beam_v >> 8) & 1));
    case 0x006: return (uint16_t)((uint16_t)(ctx->beam_v & 0xFF) << 8 | ctx->beam_h);
    case 0x016: return POTGOR_DEFAULT;
    default: return 0;
    }
}

static void custom_write16(struct KaffeineContext *ctx, uint16_t reg, uint16_t val)
{
    if (reg == 0x02A)
    {
        ctx->beam_lof = (UBYTE)((val >> 15) & 1);
        ctx->beam_v = (UWORD)((ctx->beam_v & 0xFF) | ((val & 1) << 8));
    }
    else if (reg == 0x02C)
    {
        ctx->beam_v = (UWORD)((ctx->beam_v & 0x100) | (val >> 8));
        ctx->beam_h = (UBYTE)(val & 0xFF);
    }
}

static int guest_access(struct HJBackend *b, uint32_t addr, uint32_t size, uint32_t pc)
{
    struct KaffeineContext *ctx = b->ctx;

    if (ctx->callback_failed || b->st.abort)
        return 0;
    if (addr >= ctx->mem_size || ctx->mem_size - addr < size)
    {
        if (ctx->callback_depth)
            stop_fault(b, "callback memory access", pc);
        return 0;
    }
    return 1;
}

static uint32_t slow_read8(struct HJBackend *b, uint32_t addr, uint32_t pc)
{
    if (b->ctx->callback_failed || b->st.abort)
        return 0;
    if ((addr & CUSTOM_MASK) == CUSTOM_BASE)
    {
        uint16_t w = custom_read16(b->ctx, (uint16_t)(addr & 0xFFE));
        return (addr & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
    }
    return guest_access(b, addr, 1, pc) ? b->ctx->mem[addr] : 0;
}

static uint32_t slow_read16(struct HJBackend *b, uint32_t addr, uint32_t pc)
{
    if (b->ctx->callback_failed || b->st.abort)
        return 0;
    if ((addr & CUSTOM_MASK) == CUSTOM_BASE)
        return custom_read16(b->ctx, (uint16_t)(addr & 0xFFE));
    return guest_access(b, addr, 2, pc) ? m68k_read16(b->ctx, addr) : 0;
}

static void slow_write8(struct HJBackend *b, uint32_t addr, uint32_t val, uint32_t pc)
{
    if (b->ctx->callback_failed || b->st.abort || (addr & CUSTOM_MASK) == CUSTOM_BASE)
        return;
    if (guest_access(b, addr, 1, pc))
        b->ctx->mem[addr] = (UBYTE)val;
}

static void slow_write16(struct HJBackend *b, uint32_t addr, uint32_t val, uint32_t pc)
{
    if (b->ctx->callback_failed || b->st.abort)
        return;
    if ((addr & CUSTOM_MASK) == CUSTOM_BASE)
    {
        custom_write16(b->ctx, (uint16_t)(addr & 0xFFE), (uint16_t)val);
        return;
    }
    if (guest_access(b, addr, 2, pc))
        m68k_write16(b->ctx, addr, (UWORD)val);
}

/* Called from translated code. Longwords are two word accesses, as in
   the Moira adapter, so a valid first word at the end still counts. */
static uint32_t slow_access(struct HJState *st, uint32_t addr, uint32_t value, uint32_t kind, uint32_t pc)
{
    struct HJBackend *b = st->backend;
    uint32_t hi;

    b->stats.slow_accesses++;
    switch (kind)
    {
    case SLOW_R8: return slow_read8(b, addr, pc);
    case SLOW_R16: return slow_read16(b, addr, pc);
    case SLOW_R32:
        hi = slow_read16(b, addr, pc);
        return hi << 16 | slow_read16(b, addr + 2, pc);
    case SLOW_W8: slow_write8(b, addr, value, pc); return 0;
    case SLOW_W16: slow_write16(b, addr, value & 0xFFFFu, pc); return 0;
    default:
        slow_write16(b, addr, value >> 16, pc);
        slow_write16(b, addr + 2, value & 0xFFFFu, pc);
        return 0;
    }
}

static unsigned bucket_of(uint32_t pc) { return (pc >> 1) & (HJ_BUCKETS - 1); }

static void fast_insert(struct HJBackend *b, const struct HJBlock *blk)
{
    struct HJFast *f = &b->fast[(blk->pc >> 1) & (HJ_FAST_SIZE - 1)];

    f->pc = blk->pc;
    f->entry = (uint64_t)(uintptr_t)blk->code;
}

/* Called only from hj_execute(), between blocks: no translated code is
   running, so the code memory can be emptied here. Not reentrant. */
static struct HJBlock *translate(struct HJBackend *b, uint32_t pc)
{
    struct HJTrans *t = b->trans;
    uint64_t t0 = now_ns();
    unsigned max = HJ_MAX_INSNS, n = 0, attempt;
    uint32_t end = pc;
    struct HJBlock *blk;

    for (attempt = 0; attempt < 2; attempt++)
    {
        if (b->code_words - b->code_used < HJ_SCRATCH_WORDS || b->nblocks >= HJ_MAX_BLOCKS ||
            HJ_GUEST_BYTES - b->guest_used < 4u * HJ_MAX_INSNS * 10u)
            reset_code(b);
        for (;;)
        {
            memset(&t->e, 0, sizeof(t->e));
            t->e.buf = b->scratch;
            t->e.cap = HJ_SCRATCH_WORDS;
            t->e.origin = (uintptr_t)(b->code + b->code_used);
            t->g_exit = b->g_exit; t->g_lookup = b->g_lookup; t->g_budget = b->g_budget;
            memcpy(t->g_slow, b->g_slow, sizeof(t->g_slow));
            n = translate_into(b->ctx, t, pc, max, &end);
            if (!t->e.bad || max == 1)
                break;
            max = n > 1 ? n / 2 : 1;
        }
        if (!t->e.bad)
            break;
    }
    if (end > b->ctx->mem_size)
        end = b->ctx->mem_size;
    if (t->e.bad || end <= pc)
        return NULL;

    code_write_begin(b);
    memcpy(b->code + b->code_used, b->scratch, 4u * t->e.n);
    code_write_end(b, b->code + b->code_used, t->e.n);

    blk = &b->blocks[b->nblocks++];
    blk->pc = pc;
    blk->end = end;
    blk->epoch = b->epoch;
    blk->words = t->e.n;
    blk->code = b->code + b->code_used;
    b->code_used += t->e.n;
    memcpy(b->guest_bytes + b->guest_used, b->ctx->mem + pc, end - pc);
    blk->guest = b->guest_bytes + b->guest_used;
    b->guest_used += (end - pc + 3u) & ~3u;
    blk->next = b->buckets[bucket_of(pc)];
    b->buckets[bucket_of(pc)] = blk;

    b->stats.blocks++;
    b->stats.guest_insns += n;
    b->stats.host_words += t->e.n;
    b->stats.translate_ns += (unsigned long)(now_ns() - t0);
    return blk;
}

/* A block for pc that matches the guest code, translated if needed. A
   block from before a flush is kept when its guest bytes are unchanged. */
static struct HJBlock *get_block(struct HJBackend *b, uint32_t pc)
{
    struct HJBlock **link = &b->buckets[bucket_of(pc)], *blk;

    for (blk = *link; blk; link = &blk->next, blk = blk->next)
        if (blk->pc == pc)
            break;
    if (blk && blk->epoch != b->epoch)
    {
        if (memcmp(b->ctx->mem + blk->pc, blk->guest, blk->end - blk->pc) == 0)
        {
            blk->epoch = b->epoch;
            b->stats.revalidated++;
        }
        else
        {
            *link = blk->next;
            blk = NULL;
        }
    }
    if (!blk)
        blk = translate(b, pc);
    if (blk)
        fast_insert(b, blk);
    return blk;
}

/* ------------------------------------------------------------------
   struct KaffeineCPUOps
   ------------------------------------------------------------------ */

static ULONG hj_get_d(void *s, int r) { return ((struct HJBackend *)s)->st.d[r]; }
static ULONG hj_get_a(void *s, int r) { return ((struct HJBackend *)s)->st.a[r]; }
static void hj_set_d(void *s, int r, ULONG v) { ((struct HJBackend *)s)->st.d[r] = v; }
static void hj_set_a(void *s, int r, ULONG v) { ((struct HJBackend *)s)->st.a[r] = v; }
static ULONG hj_get_pc(void *s) { return ((struct HJBackend *)s)->st.pc; }
static void hj_set_pc(void *s, ULONG v) { ((struct HJBackend *)s)->st.pc = v; }

static UWORD hj_get_sr(void *s)
{
    struct HJBackend *b = s;

    return (UWORD)(b->st.sr_high << 8 | swap_cv(b->st.flags));
}

static void hj_set_sr(void *s, UWORD v)
{
    struct HJBackend *b = s;

    b->st.sr_high = (v >> 8) & 0xF7u;
    b->st.flags = swap_cv(v & 0x1Fu);
}

static void dispatch_linea(struct HJBackend *b, uint32_t pc)
{
    struct KaffeineContext *ctx = b->ctx;
    struct KaffeineDispatchResult result;

    b->stats.exits[HJ_EXIT_LINEA]++;
    if (!Kaffeine_DispatchLibraryCall(ctx, &b->handle, pc, &result))
    {
        stop_fault(b, "Line-A outside the library vectors", pc);
        return;
    }
    if (!ctx->running)
    {
        if (ctx->callback_depth || ctx->callback_failed)
            Kaffeine_StopGuest(ctx, &b->handle, (LONG)b->st.d[0]);
        return;
    }
    if (result.supervisor)
    {
        ctx->sv_redirect = 0;
        stop_fault(b, "Supervisor() is outside the probe", pc);
        return;
    }
    b->st.pc = result.return_pc;
}

static void hj_execute(void *state)
{
    struct HJBackend *b = state;
    struct KaffeineContext *ctx = b->ctx;
    uint32_t pc = b->st.pc, reason;
    struct HJBlock *blk;

    if (Kaffeine_IsCallbackReturn(pc) ||
        (ctx->callback_depth && ((pc & 1) || pc >= ctx->mem_size || ctx->mem_size - pc < 2)))
    {
        stop_fault(b, "invalid callback PC", pc);
        Kaffeine_StopGuest(ctx, &b->handle, RETURN_FAIL);
        return;
    }
    if (pc >= SENTINEL_ADDR && pc < SENTINEL_ADDR + 2)
    {
        ctx->running = FALSE;
        if (ctx->callback_depth)
        {
            ctx->callback_failed = TRUE;
            ctx->exit_code = (LONG)b->st.d[0];
        }
        return;
    }
    if ((pc & 1) || pc >= ctx->mem_size || ctx->mem_size - pc < 2)
    {
        stop_fault(b, "invalid PC", pc);
        return;
    }
    blk = get_block(b, pc);
    if (!blk)
    {
        stop_fault(b, "translation failed", pc);
        return;
    }
    if (++b->depth > b->stats.max_depth)
        b->stats.max_depth = b->depth;
    b->stats.enters++;
    b->st.abort = 0;
    reason = b->enter(&b->st, blk->code);
    if (reason == HJ_EXIT_LINEA)
        dispatch_linea(b, b->st.pc);
    else
    {
        if (reason < HJ_EXIT_COUNT)
            b->stats.exits[reason]++;
        if (reason == HJ_EXIT_FAULT)
        {
            struct HJInsn in;

            decode_insn(ctx, b->st.pc, &in);
            stop_fault(b, in.why ? in.why : "unexpected fault exit", b->st.pc);
        }
    }
    b->depth--;
    if (ctx->callback_failed || b->faulted)
        b->st.d[0] = (uint32_t)ctx->exit_code;
}

/* CacheClearU(), CacheClearE() and the bridge's loaders: with a new epoch
   every block compares its guest bytes before it runs again, and nothing
   is left in the fast table, so the range is not needed. */
static void hj_flush_code(void *state, ULONG addr, ULONG size)
{
    struct HJBackend *b = state;

    (void)addr;
    (void)size;
    b->epoch++;
    b->stats.flushes++;
    fast_clear(b);
}

static const struct KaffeineCPUOps hj_ops =
{
    hj_get_d, hj_get_a, hj_set_d, hj_set_a,
    hj_get_sr, hj_set_sr, hj_get_pc, hj_set_pc, hj_execute, hj_flush_code
};

/* ------------------------------------------------------------------
   Public functions
   ------------------------------------------------------------------ */

/* Words that clang assembled from the same text (Apple clang 21,
   -arch arm64): every encoder and alias above, with negative offsets,
   shifted operands, bitmask immediates and branches both ways. */
int HJ_EncoderSelfTest(char *message, unsigned long size)
{
    const struct { uint32_t want, got; const char *text; } enc[] =
    {
        { 0x113ffc41, a64_addsub_imm(0, 0, 0, 1, 2, 4095, 0), "add w1, w2, #4095" },
        { 0x51000483, a64_addsub_imm(0, 1, 0, 3, 4, 1, 0), "sub w3, w4, #1" },
        { 0x31001cc5, a64_addsub_imm(0, 0, 1, 5, 6, 7, 0), "adds w5, w6, #7" },
        { 0x710006d6, a64_addsub_imm(0, 1, 1, 22, 22, 1, 0), "subs w22, w22, #1" },
        { 0x910003fd, a64_addsub_imm(1, 0, 0, 29, SP, 0, 0), "add x29, sp, #0" },
        { 0x5177fe19, a64_addsub_imm(0, 1, 0, 25, 16, 0xDFF, 1), "sub w25, w16, #0xdff, lsl #12" },
        { 0x5177fd11, a64_addsub_imm(0, 1, 0, 17, 8, 0xDFF, 1), "sub w17, w8, #0xdff, lsl #12" },
        { 0x11001108, a64_addsub_imm(0, 0, 0, 8, 8, 4, 0), "add w8, w8, #4" },
        { 0x11000a31, a64_addsub_imm(0, 0, 0, 17, 17, 2, 0), "add w17, w17, #2" },
        { 0x7120063f, a64_addsub_imm(0, 1, 1, ZR, 17, 0x801, 0), "cmp w17, #0x801" },
        { 0x0b030041, a64_addsub_reg(0, 0, 0, 1, 2, 3, 0, 0), "add w1, w2, w3" },
        { 0x4b0600a4, a64_addsub_reg(0, 1, 0, 4, 5, 6, 0, 0), "sub w4, w5, w6" },
        { 0x2b116339, a64_addsub_reg(0, 0, 1, 25, 25, 17, 0, 24), "adds w25, w25, w17, lsl #24" },
        { 0x6b03433f, a64_addsub_reg(0, 1, 1, ZR, 25, 3, 0, 16), "subs wzr, w25, w3, lsl #16" },
        { 0x6b18011f, a64_cmp(8, 24), "cmp w8, w24" },
        { 0xeb10033f, a64_addsub_reg(1, 1, 1, ZR, 25, 16, 0, 0), "cmp x25, x16" },
        { 0x8b191231, a64_addsub_reg(1, 0, 0, 17, 17, 25, 0, 4), "add x17, x17, x25, lsl #4" },
        { 0x0b190910, a64_addsub_reg(0, 0, 0, 16, 8, 25, 0, 2), "add w16, w8, w25, lsl #2" },
        { 0x4b0343f9, a64_addsub_reg(0, 1, 0, 25, ZR, 3, 0, 16), "neg w25, w3, lsl #16" },
        { 0x6b1103fc, a64_addsub_reg(0, 1, 1, 28, ZR, 17, 0, 0), "negs w28, w17" },
        { 0x0a030041, a64_logic_reg(0, 0, 0, 1, 2, 3, 0, 0), "and w1, w2, w3" },
        { 0x2a0203e1, a64_mov(1, 2), "mov w1, w2" },
        { 0x2a1f03f1, a64_mov(17, ZR), "mov w17, wzr" },
        { 0x4a550eba, a64_logic_reg(0, 2, 0, 26, 21, 21, 1, 3), "eor w26, w21, w21, lsr #3" },
        { 0x2a550b5a, a64_logic_reg(0, 1, 0, 26, 26, 21, 1, 2), "orr w26, w26, w21, lsr #2" },
        { 0x6a03007f, a64_tst(3, 3), "tst w3, w3" },
        { 0x6a0600a4, a64_logic_reg(0, 3, 0, 4, 5, 6, 0, 0), "ands w4, w5, w6" },
        { 0x2a3103f9, a64_logic_reg(0, 1, 1, 25, ZR, 17, 0, 0), "mvn w25, w17" },
        { 0xaa0003f3, a64_mov64(19, 0), "mov x19, x0" },
        { 0xaa0103f1, a64_mov64(17, 1), "mov x17, x1" },
        { 0x2a1b075a, a64_logic_reg(0, 1, 0, 26, 26, 27, 0, 1), "orr w26, w26, w27, lsl #1" },
        { 0x2a1b235c, a64_logic_reg(0, 1, 0, 28, 26, 27, 0, 8), "orr w28, w26, w27, lsl #8" },
        { 0x2a1b0355, a64_logic_reg(0, 1, 0, 21, 26, 27, 0, 0), "orr w21, w26, w27" },
        { 0x121c02b5, a64_logic_const(0, 21, 21, 0x10), "and w21, w21, #0x10" },
        { 0x321d02b5, a64_logic_const(1, 21, 21, 0x8), "orr w21, w21, #0x8" },
        { 0x321e02b5, a64_logic_const(1, 21, 21, 0x4), "orr w21, w21, #0x4" },
        { 0x521f035a, a64_logic_const(2, 26, 26, 0x2), "eor w26, w26, #0x2" },
        { 0x721f06bf, a64_logic_const(3, ZR, 21, 0x6), "tst w21, #0x6" },
        { 0x72003c7f, a64_logic_const(3, ZR, 3, 0xFFFF), "tst w3, #0xffff" },
        { 0x12103c63, a64_logic_const(0, 3, 3, 0xFFFF0000u), "and w3, w3, #0xffff0000" },
        { 0x12185c63, a64_logic_const(0, 3, 3, 0xFFFFFF00u), "and w3, w3, #0xffffff00" },
        { 0x121e0a3a, a64_logic_const(0, 26, 17, 0x1C), "and w26, w17, #0x1c" },
        { 0x121c7b5a, a64_logic_const(0, 26, 26, ~0x08u), "and w26, w26, #0xfffffff7" },
        { 0x32089fe1, a64_logic_const(1, 1, ZR, 0xFF00FF00u), "orr w1, wzr, #0xff00ff00" },
        { 0x3200f3e1, a64_logic_const(1, 1, ZR, 0x55555555u), "orr w1, wzr, #0x55555555" },
        { 0x12010442, a64_logic_const(0, 2, 2, 0x80000001u), "and w2, w2, #0x80000001" },
        { 0x52824681, a64_movw(0, 2, 1, 0x1234, 0), "movz w1, #0x1234" },
        { 0x52a24681, a64_movw(0, 2, 1, 0x1234, 1), "movz w1, #0x1234, lsl #16" },
        { 0x12824681, a64_movw(0, 0, 1, 0x1234, 0), "movn w1, #0x1234" },
        { 0x12aacf02, a64_movw(0, 0, 2, 0x5678, 1), "movn w2, #0x5678, lsl #16" },
        { 0x72b579a3, a64_movw(0, 3, 3, 0xABCD, 1), "movk w3, #0xabcd, lsl #16" },
        { 0x529ffff6, a64_movw(0, 2, 22, 0xFFFF, 0), "movz w22, #0xffff" },
        { 0x53081c41, a64_lsl(1, 2, 24), "lsl w1, w2, #24" },
        { 0x531f7800, a64_lsl(0, 0, 1), "lsl w0, w0, #1" },
        { 0x53103ffa, a64_lsl(26, ZR, 16), "lsl w26, wzr, #16" },
        { 0x531c7f5a, a64_lsr(26, 26, 28), "lsr w26, w26, #28" },
        { 0x53087c00, a64_lsr(0, 0, 8), "lsr w0, w0, #8" },
        { 0x53017e31, a64_lsr(17, 17, 1), "lsr w17, w17, #1" },
        { 0x13037c41, a64_asr(1, 2, 3), "asr w1, w2, #3" },
        { 0x531f7c7b, a64_ubfx(27, 3, 31, 1), "ubfx w27, w3, #31, #1" },
        { 0x5300007b, a64_ubfx(27, 3, 0, 1), "ubfx w27, w3, #0, #1" },
        { 0x53083e3a, a64_ubfx(26, 17, 8, 8), "ubfx w26, w17, #8, #8" },
        { 0xd3413219, a64_bitfield(1, 2, 25, 16, 1, 12), "ubfx x25, x16, #1, #12" },
        { 0x33000f55, a64_bfi(21, 26, 0, 4), "bfi w21, w26, #0, #4" },
        { 0x331c0375, a64_bfi(21, 27, 4, 1), "bfi w21, w27, #4, #1" },
        { 0x331f037a, a64_bfi(26, 27, 1, 1), "bfi w26, w27, #1, #1" },
        { 0x33003f23, a64_bfi(3, 25, 0, 16), "bfi w3, w25, #0, #16" },
        { 0x33001f23, a64_bfi(3, 25, 0, 8), "bfi w3, w25, #0, #8" },
        { 0x13001c79, a64_sxtb(25, 3), "sxtb w25, w3" },
        { 0x13003ee8, a64_sxth(8, 23), "sxth w8, w23" },
        { 0x53001c7c, a64_uxtb(28, 3), "uxtb w28, w3" },
        { 0x53003e3c, a64_uxth(28, 17), "uxth w28, w17" },
        { 0x93407c1b, a64_bitfield(1, 0, 27, 0, 0, 31), "sxtw x27, w0" },
        { 0xd360ff3b, a64_bitfield(1, 2, 27, 25, 32, 63), "lsr x27, x25, #32" },
        { 0xd378df9c, a64_bitfield(1, 2, 28, 28, 56, 55), "lsl x28, x28, #8" },
        { 0xd348ff39, a64_bitfield(1, 2, 25, 25, 8, 63), "lsr x25, x25, #8" },
        { 0xb3607c1c, a64_bitfield(1, 1, 28, 0, 32, 31), "bfi x28, x0, #32, #32" },
        { 0x0b970e10, a64_addsub_reg(0, 0, 0, 16, 16, 23, 2, 3), "add w16, w16, w23, asr #3" },
        { 0xea19033f, a64_logic_reg(1, 3, 0, ZR, 25, 25, 0, 0), "tst x25, x25" },
        { 0x0a550b7b, a64_logic_reg(0, 0, 0, 27, 27, 21, 1, 2), "and w27, w27, w21, lsr #2" },
        { 0x0a3c0000, a64_logic_reg(0, 0, 1, 0, 0, 28, 0, 0), "bic w0, w0, w28" },
        { 0x13834063, a64_ror(3, 3, 16), "ror w3, w3, #16" },
        { 0x13837c63, a64_ror(3, 3, 31), "ror w3, w3, #31" },
        { 0x5ac00a31, a64_rev32(17, 17), "rev w17, w17" },
        { 0x5ac00639, a64_rev16(25, 17), "rev16 w25, w17" },
        { 0x5ac01339, a64_clz(25, 25), "clz w25, w25" },
        { 0x1a9a3339, a64_csel(0, 25, 25, 26, A64_LO), "csel w25, w25, w26, lo" },
        { 0x1a9f37fb, a64_cset(27, A64_HS), "cset w27, hs" },
        { 0x1a9f27fb, a64_cset(27, A64_LO), "cset w27, lo" },
        { 0x1a9f17fa, a64_cset(26, A64_EQ), "cset w26, eq" },
        { 0x1a9f07fb, a64_cset(27, A64_NE), "cset w27, ne" },
        { 0x3a020039, a64_adc_sbc(0, 0, 1, 25, 1, 2), "adcs w25, w1, w2" },
        { 0x7a020039, a64_adc_sbc(0, 1, 1, 25, 1, 2), "sbcs w25, w1, w2" },
        { 0x1ada239c, a64_lslv(28, 28, 26), "lsl w28, w28, w26" },
        { 0x1adb2799, a64_shiftv(0, 1, 25, 28, 27), "lsr w25, w28, w27" },
        { 0x1adb2b80, a64_shiftv(0, 2, 0, 28, 27), "asr w0, w28, w27" },
        { 0x1ad92c1c, a64_shiftv(0, 3, 28, 0, 25), "ror w28, w0, w25" },
        { 0x9adb2339, a64_shiftv(1, 0, 25, 25, 27), "lsl x25, x25, x27" },
        { 0x9ad72739, a64_shiftv(1, 1, 25, 25, 23), "lsr x25, x25, x23" },
        { 0x1ad10819, a64_div(0, 0, 25, 0, 17), "udiv w25, w0, w17" },
        { 0x1ad10c19, a64_div(0, 1, 25, 0, 17), "sdiv w25, w0, w17" },
        { 0x9ad70b99, a64_div(1, 0, 25, 28, 23), "udiv x25, x28, x23" },
        { 0x9ad70f99, a64_div(1, 1, 25, 28, 23), "sdiv x25, x28, x23" },
        { 0x1b1a7f20, a64_mul(0, 25, 26), "mul w0, w25, w26" },
        { 0x1b11833a, a64_msub(0, 26, 25, 17, 0), "msub w26, w25, w17, w0" },
        { 0x9b17f33a, a64_msub(1, 26, 25, 23, 28), "msub x26, x25, x23, x28" },
        { 0x9b207e39, a64_smull(25, 17, 0), "smull x25, w17, w0" },
        { 0x9ba07e39, a64_umull(25, 17, 0), "umull x25, w17, w0" },
        { 0xd53b421a, a64_mrs_nzcv(26), "mrs x26, nzcv" },
        { 0xb8684a91, a64_ldst_reg(2, 1, 17, 20, 8), "ldr w17, [x20, w8, uxtw]" },
        { 0x78704a91, a64_ldst_reg(1, 1, 17, 20, 16), "ldrh w17, [x20, w16, uxtw]" },
        { 0x387c4a91, a64_ldst_reg(0, 1, 17, 20, 28), "ldrb w17, [x20, w28, uxtw]" },
        { 0xb82f4a99, a64_ldst_reg(2, 0, 25, 20, 15), "str w25, [x20, w15, uxtw]" },
        { 0x78304a99, a64_ldst_reg(1, 0, 25, 20, 16), "strh w25, [x20, w16, uxtw]" },
        { 0x38284a91, a64_ldst_reg(0, 0, 17, 20, 8), "strb w17, [x20, w8, uxtw]" },
        { 0xf9402e74, a64_ldst_uimm(3, 1, 20, 19, 88), "ldr x20, [x19, #88]" },
        { 0xb9404675, a64_ldst_uimm(2, 1, 21, 19, 68), "ldr w21, [x19, #68]" },
        { 0xb9004270, a64_ldst_uimm(2, 0, 16, 19, 64), "str w16, [x19, #64]" },
        { 0xf9000bf0, a64_ldst_uimm(3, 0, 16, SP, 16), "str x16, [sp, #16]" },
        { 0xf9403670, a64_ldst_uimm(3, 1, 16, 19, 104), "ldr x16, [x19, #104]" },
        { 0xb9404a7b, a64_ldst_uimm(2, 1, 27, 19, 72), "ldr w27, [x19, #72]" },
        { 0xb9004a7a, a64_ldst_uimm(2, 0, 26, 19, 72), "str w26, [x19, #72]" },
        { 0xa9ba7bfd, a64_ldstp(1, 0, 3, 29, 30, SP, -96), "stp x29, x30, [sp, #-96]!" },
        { 0xa8c67bfd, a64_ldstp(1, 1, 1, 29, 30, SP, 96), "ldp x29, x30, [sp], #96" },
        { 0xa90153f3, a64_ldstp(1, 0, 2, 19, 20, SP, 16), "stp x19, x20, [sp, #16]" },
        { 0xa94573fb, a64_ldstp(1, 1, 2, 27, 28, SP, 80), "ldp x27, x28, [sp, #80]" },
        { 0x29000660, a64_ldstp(0, 0, 2, 0, 1, 19, 0), "stp w0, w1, [x19]" },
        { 0x29473e6e, a64_ldstp(0, 1, 2, 14, 15, 19, 56), "ldp w14, w15, [x19, #56]" },
        { 0xa9404639, a64_ldstp(1, 1, 2, 25, 17, 17, 0), "ldp x25, x17, [x17]" },
        { 0xa9be7bfd, a64_ldstp(1, 0, 3, 29, 30, SP, -32), "stp x29, x30, [sp, #-32]!" },
        { 0xa8c27bfd, a64_ldstp(1, 1, 1, 29, 30, SP, 32), "ldp x29, x30, [sp], #32" },
        { 0x14000002, a64_b(2), "b .+8" },
        { 0x17ffffff, a64_b(-1), "b .-4" },
        { 0x94000064, a64_bl(100), "bl .+400" },
        { 0x54ffffa1, a64_bcond(A64_NE, -3), "b.ne .-12" },
        { 0x54000080, a64_bcond(A64_EQ, 4), "b.eq .+16" },
        { 0x54000208, a64_bcond(A64_HI, 16), "b.hi .+64" },
        { 0x54000063, a64_bcond(A64_LO, 3), "b.lo .+12" },
        { 0x34000111, a64_cbz(0, 0, 17, 8), "cbz w17, .+32" },
        { 0x35000070, a64_cbz(0, 1, 16, 3), "cbnz w16, .+12" },
        { 0x360800b5, a64_tbz(0, 21, 1, 5), "tbz w21, #1, .+20" },
        { 0x3707ffda, a64_tbz(1, 26, 0, -2), "tbnz w26, #0, .-8" },
        { 0x37180035, a64_tbz(1, 21, 3, 1), "tbnz w21, #3, .+4" },
        { 0xd61f0220, a64_br(17), "br x17" },
        { 0xd63f0200, a64_blr(16), "blr x16" },
        { 0xd65f03c0, A64_RET, "ret" },
    };
    unsigned i, bad = 0;

    for (i = 0; i < sizeof(enc) / sizeof(enc[0]); i++)
        if (enc[i].want != enc[i].got && bad++ == 0 && message)
            snprintf(message, size, "%s: %08lx, want %08lx", enc[i].text,
                     (unsigned long)enc[i].got, (unsigned long)enc[i].want);
    if (!bad && message)
        snprintf(message, size, "%u encodings", i);
    return (int)bad;
}

int HJ_Available(void)
{
    return HJ_RUNS;
}

const char *HJ_ExecMemoryKind(void)
{
#if HJ_RUNS && defined(__APPLE__)
    return "MAP_JIT";
#elif HJ_RUNS && defined(__AROS__)
    return "AROS AllocMem/CacheClearE";
#elif HJ_RUNS
    return "mprotect";
#else
    return "none";
#endif
}

struct HJBackend *HJ_Create(struct KaffeineContext *ctx)
{
#if HJ_RUNS
    struct HJBackend *b;
    struct HJEmit e;
    void *code;

    if (!ctx->mem || ctx->mem_size < 0x1000)
        return NULL;
    b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    code = code_alloc();
    b->fast = calloc(HJ_FAST_SIZE, sizeof(*b->fast));
    b->blocks = calloc(HJ_MAX_BLOCKS, sizeof(*b->blocks));
    b->guest_bytes = malloc(HJ_GUEST_BYTES);
    b->scratch = malloc(4u * HJ_SCRATCH_WORDS);
    b->trans = malloc(sizeof(*b->trans));
    if (!code || !b->fast || !b->blocks || !b->guest_bytes || !b->scratch || !b->trans)
    {
        if (code)
            code_free(code);
        free(b->fast); free(b->blocks); free(b->guest_bytes); free(b->scratch); free(b->trans);
        free(b);
        return NULL;
    }
    b->code = code;
    b->code_words = HJ_CODE_BYTES / 4;
    b->ctx = ctx;
    b->handle.ops = &hj_ops;
    b->handle.backend_state = b;
    b->st.mem = ctx->mem;
    b->st.limit = ctx->mem_size - 4;
    b->st.fast = b->fast;
    b->st.slow = (void *)(uintptr_t)slow_access;
    b->st.backend = b;
    fast_clear(b);

    memset(&e, 0, sizeof(e));
    e.buf = b->scratch;
    e.cap = HJ_SCRATCH_WORDS;
    e.origin = (uintptr_t)b->code;
    emit_glue(b, &e);
    if (e.bad)
        abort();
    code_write_begin(b);
    memcpy(b->code, b->scratch, 4u * e.n);
    code_write_end(b, b->code, e.n);
    b->glue_words = b->code_used = e.n;
    b->stats.write_windows = b->stats.icache_syncs = 0;
    {
        uintptr_t entry = b->g_enter;

        memcpy(&b->enter, &entry, sizeof(entry));
    }
    return b;
#else
    (void)ctx;
    (void)slow_access;
    (void)hj_ops;
    return NULL;
#endif
}

void HJ_Destroy(struct HJBackend *b)
{
    if (!b)
        return;
#if HJ_RUNS
    code_free(b->code);
#endif
    free(b->fast);
    free(b->blocks);
    free(b->guest_bytes);
    free(b->scratch);
    free(b->trans);
    free(b);
}

LONG HJ_Execute(struct HJBackend *b)
{
    struct KaffeineContext *ctx = b->ctx;
    ULONG sp = ctx->stack_top - 4;
    LONG result;

    memset(b->st.d, 0, sizeof(b->st.d));
    memset(b->st.a, 0, sizeof(b->st.a));
    b->faulted = 0;
    b->fault[0] = 0;
    /* What reset leaves: supervisor mode, IPL 7, the vectors in place. */
    b->st.sr_high = 0x27;
    b->st.flags = 0;
    m68k_write32(ctx, 0, ctx->stack_top);
    m68k_write32(ctx, 4, ctx->m68k_sysbase);
    m68k_write32(ctx, sp, SENTINEL_ADDR);
    b->st.a[7] = sp;
    b->st.a[0] = ctx->m68k_argptr;
    b->st.d[0] = ctx->m68k_argsize;
    b->st.a[6] = ctx->m68k_sysbase;
    b->st.pc = ctx->entry_point;
    result = Kaffeine_RunCPU(ctx, &b->handle);
    ctx->exit_code = b->faulted ? RETURN_FAIL : result;
    return ctx->exit_code;
}

const struct HJStats *HJ_GetStats(const struct HJBackend *b)
{
    return &b->stats;
}

const char *HJ_LastFault(const struct HJBackend *b)
{
    return b->fault;
}

unsigned HJ_TranslateOnly(struct KaffeineContext *ctx, ULONG pc, unsigned *words)
{
    static struct HJBackend b;
    static struct HJTrans t;
    static uint32_t buf[HJ_SCRATCH_WORDS];
    struct HJEmit glue;
    uint32_t end;
    unsigned n;

    /* The glue and the block as HJ_Create() would lay them out, at a host
       address that nothing executes. */
    memset(&glue, 0, sizeof(glue));
    glue.buf = buf;
    glue.cap = HJ_SCRATCH_WORDS;
    glue.origin = (uintptr_t)0x40000000u;
    emit_glue(&b, &glue);
    memset(&t.e, 0, sizeof(t.e));
    t.e.buf = buf + glue.n;
    t.e.cap = HJ_SCRATCH_WORDS - glue.n;
    t.e.origin = glue.origin + 4u * glue.n;
    t.g_exit = b.g_exit; t.g_lookup = b.g_lookup; t.g_budget = b.g_budget;
    memcpy(t.g_slow, b.g_slow, sizeof(t.g_slow));
    n = translate_into(ctx, &t, pc, HJ_MAX_INSNS, &end);
    if (words)
        *words = t.e.n;
    return glue.bad || t.e.bad ? 0 : n;
}
