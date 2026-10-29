/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Independent widened-integer oracle for emitted DIVU.W / DIVS.W. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "kaffeine_intern.h"
#include "kaffeine_jit.h"

struct Vector { ULONG dividend; UWORD divisor, flags; int sign; };
static const struct Vector vectors[] = {
    {0,1,0,0}, {5,2,31,0}, {65535,1,1,0}, {65536,1,31,0},
    {0xFFFFFFFF,0xFFFF,0,0}, {0x80000000,0x8000,31,0},
    {0x7FFFFFFF,0xFFFF,16,0}, {0xFFFF0000,0xFFFF,31,0},
    {1,0xFFFF,31,1}, {0xFFFFFFFF,1,0,1}, {0xFFFF0001,2,31,1},
    {0xFFFF0000,2,0,1}, {0xFFFEFFFE,2,31,1},
    {0x80000000,0xFFFF,31,1}, {0x80000000,0x8000,0,1},
    {0x80000000,0x7FFF,31,1}, {0x7FFFFFFF,0x7FFF,0,1},
    {32768,1,31,1}, {32767,1,0,1}, {0,0xFFFF,31,1},
    {0x12345678,0,31,0}, {0x87654321,0,0,1}
};

int main(void)
{
    unsigned i, checks = 0, failed = 0;
    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i)
    {
        const struct Vector *v = &vectors[i];
        struct KaffeineContext *ctx = calloc(1, sizeof(*ctx));
        struct HJBackend *jit;
        int64_t dividend, divisor, quotient, remainder;
        ULONG expected = 20;
        UWORD flags = 0, mask = 0;
        int overflow = 0, ok;
        LONG result;
        const UWORD program[] = {
            0x203C, v->dividend >> 16, v->dividend, /* MOVE.L dividend,D0 */
            0x223C, 0xC0DE, v->divisor,             /* dirty upper source word */
            0x44FC, v->flags,                       /* MOVE flags,CCR */
            v->sign ? 0x81C1 : 0x80C1,
            0x40F9, 0, 0x4004,                     /* MOVE.W SR,$4004 */
            0x23C0, 0, 0x4000,                     /* MOVE.L changes flags: save SR first */
            0x4E75
        };
        unsigned j;
        ++checks;
        if (!ctx) { ++failed; continue; }
        ctx->mem_size = 0x10000;
        ctx->mem = calloc(1, ctx->mem_size + KAFFEINE_MEM_GUARD);
        if (!ctx->mem) { free(ctx); ++failed; continue; }
        ctx->entry_point = 0x2000; ctx->stack_top = 0xF000;
        for (j = 0; j < sizeof(program) / sizeof(program[0]); ++j)
            m68k_write16(ctx, ctx->entry_point + 2*j, program[j]);
        m68k_write32(ctx, 0x4000, 0xA5A5A5A5);
        jit = HJ_CreateLibrary(ctx);
        if (!jit) { free(ctx->mem); free(ctx); ++failed; continue; }
        if (v->divisor)
        {
            dividend = v->sign ? (int32_t)v->dividend : (int64_t)v->dividend;
            divisor = v->sign ? (int16_t)v->divisor : (int64_t)v->divisor;
            quotient = dividend / divisor; remainder = dividend % divisor;
            overflow = quotient < (v->sign ? -32768 : 0) || quotient > (v->sign ? 32767 : 65535);
            expected = overflow ? v->dividend : (ULONG)(UWORD)remainder << 16 | (UWORD)quotient;
            mask = overflow ? 0x13 : 0x1F;
            flags = (v->flags & 16) | (overflow ? 2 :
                ((quotient & 0x8000) ? 8 : 0) | (quotient == 0 ? 4 : 0));
        }
        result = HJ_Execute(jit);
        if (v->divisor)
            ok = (ULONG)result == expected && m68k_read32(ctx, 0x4000) == expected &&
                 (m68k_read16(ctx, 0x4004) & mask) == flags && !HJ_LastFault(jit)[0];
        else
            ok = result == 20 && m68k_read32(ctx, 0x4000) == 0xA5A5A5A5 && HJ_LastFault(jit)[0];
        ok = ok && HJ_GetStats(jit)->enters && HJ_GetStats(jit)->icache_syncs;
        if (!ok) ++failed;
        printf("%s DIV%s.W vector %u: result %08lx flags %02x\n", ok ? "ok" : "FAIL",
               v->sign ? "S" : "U", i, (unsigned long)result, m68k_read16(ctx,0x4004)&31);
        HJ_Destroy(jit); free(ctx->mem); free(ctx);
    }
    printf("word_division: %u checks, %u failed\n", checks, failed);
    return failed ? 10 : 0;
}
