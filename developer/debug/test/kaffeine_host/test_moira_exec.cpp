/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Fabian Schmieder (@metaneutrons)

    Host-side test: full Moira execution of m68k code.
    Tests the complete path: memory setup → load code → execute → trap → exit.
*/
#include "kaffeine_intern.h"
#include "kaffeine_thunks.h"

/* Stub thunk tables */
const struct M68KThunkEntry kaffeine_thunks_exec[] = { { 0, nullptr } };
const ULONG kaffeine_thunks_exec_count = 0;
const struct M68KThunkEntry kaffeine_thunks_dos[] = { { 0, nullptr } };
const ULONG kaffeine_thunks_dos_count = 0;

extern "C" {
ULONG Kaffeine_OpenLibrary(struct KaffeineContext *, const char *) { return 0; }
ULONG Kaffeine_OpenM68KLibrary(struct KaffeineContext *, void *, const char *, ULONG) { return 0; }
BOOL Kaffeine_CloseM68KLibrary(struct KaffeineContext *, void *, ULONG) { return 0; }
}

/* Include the C sources */
#include "kaffeine_memory.c"
#include "kaffeine_cpu.c"
#include "kaffeine_dispatch.c"

/* Include Moira */
#include "Moira/Moira.cpp"
#include "Moira/MoiraDebugger.cpp"

/* Include our Moira subclass */
#include "kaffeine_moira.cpp"

#include <cstdio>
#include <stdexcept>
static int pass = 0, fail = 0;
#define TEST(n) printf("  %-50s ", n)
#define OK() do { printf("PASS\n"); pass++; } while(0)
#define NG(m) do { printf("FAIL: %s\n", m); fail++; return; } while(0)
#define CHK(c,m) do { if(!(c)) NG(m); } while(0)

/* AttnFlags may advertise an FPU only if the CPU executes FPU code. Run
   FMOVE.L FPCR,D0, the first FPU instruction of the libnix FPU startup
   code (___fpucheck), with the model Kaffeine_Execute() configures. */
void t_fpu_flags_match_cpu()
{
    TEST("AttnFlags has an FPU flag only if FPU code runs");
    struct KaffeineLibBase base = {};
    auto *ctx = Kaffeine_CreateContext(&base);
    CHK(ctx, "ctx null");

    ULONG code = Kaffeine_HeapAlloc(ctx, 8, 0);
    m68k_write16(ctx, code,     0xF200);    /* FMOVE.L FPCR,D0 */
    m68k_write16(ctx, code + 2, 0xB000);
    m68k_write16(ctx, code + 4, 0x7000);    /* MOVEQ #0,D0 */
    m68k_write16(ctx, code + 6, 0x4E75);    /* RTS */
    ctx->entry_point = code;
    ctx->m68k_argptr = ctx->m68k_argsize = ctx->m68k_sysbase = 0;

    bool escaped = false;
    LONG result = RETURN_FAIL;
    try {
        result = Kaffeine_Execute(ctx);
    } catch (...) {
        escaped = true;
    }
    Kaffeine_DestroyContext(ctx);

    const UWORD fpu_flags = (1 << 4) | (1 << 5) | (1 << 6);   /* AFF_68881, AFF_68882, AFF_FPU40 */
    CHK(!escaped, "backend exception escaped into its caller");
    CHK(result == 0 || result == RETURN_FAIL, "undefined FPU execution result");
    CHK(result == 0 || (M68K_ATTNFLAGS_68040 & fpu_flags) == 0,
        "AttnFlags advertises an FPU that Moira does not execute");
    CHK((M68K_ATTNFLAGS_68040 & (1 << 1)) != 0, "AttnFlags lost AFF_68020");
    OK();
}

void t_exec_moveq_rts()
{
    TEST("Execute MOVEQ #42,D0; RTS");
    struct KaffeineLibBase base = {};
    auto *ctx = Kaffeine_CreateContext(&base);
    CHK(ctx, "ctx null");

    /* Write m68k code: MOVEQ #42,D0 (0x702A) + RTS (0x4E75) */
    ULONG code_addr = Kaffeine_HeapAlloc(ctx, 4, 0);
    CHK(code_addr, "alloc failed");
    m68k_write16(ctx, code_addr,     0x702A);  /* MOVEQ #42,D0 */
    m68k_write16(ctx, code_addr + 2, 0x4E75);  /* RTS */

    ctx->entry_point  = code_addr;
    ctx->m68k_argptr  = 0;
    ctx->m68k_argsize = 0;
    ctx->m68k_sysbase = 0;

    LONG result = Kaffeine_Execute(ctx);
    CHK(result == 42, "expected 42");

    Kaffeine_DestroyContext(ctx);
    OK();
}

void t_exec_add()
{
    TEST("Execute MOVEQ #10,D0; ADDI.W #32,D0; RTS");
    struct KaffeineLibBase base = {};
    auto *ctx = Kaffeine_CreateContext(&base);

    ULONG code_addr = Kaffeine_HeapAlloc(ctx, 8, 0);
    m68k_write16(ctx, code_addr,     0x700A);  /* MOVEQ #10,D0 */
    m68k_write16(ctx, code_addr + 2, 0x0640);  /* ADDI.W #xx,D0 */
    m68k_write16(ctx, code_addr + 4, 0x0020);  /* immediate: 32 */
    m68k_write16(ctx, code_addr + 6, 0x4E75);  /* RTS */

    ctx->entry_point = code_addr;
    ctx->m68k_argptr = ctx->m68k_argsize = ctx->m68k_sysbase = 0;

    LONG result = Kaffeine_Execute(ctx);
    CHK(result == 42, "expected 42");

    Kaffeine_DestroyContext(ctx);
    OK();
}

void t_exec_loop()
{
    TEST("Execute loop: sum 1..10 = 55");
    struct KaffeineLibBase base = {};
    auto *ctx = Kaffeine_CreateContext(&base);

    /* D0 = 0 (accumulator), D1 = 10 (counter)
       loop: ADD.W D1,D0; SUBQ.W #1,D1; BNE loop; RTS */
    ULONG a = Kaffeine_HeapAlloc(ctx, 16, 0);
    m68k_write16(ctx, a,      0x7000);  /* MOVEQ #0,D0 */
    m68k_write16(ctx, a + 2,  0x720A);  /* MOVEQ #10,D1 */
    /* loop: */
    m68k_write16(ctx, a + 4,  0xD041);  /* ADD.W D1,D0 */
    m68k_write16(ctx, a + 6,  0x5341);  /* SUBQ.W #1,D1 */
    m68k_write16(ctx, a + 8,  0x66FA);  /* BNE.S loop (-6) */
    m68k_write16(ctx, a + 10, 0x4E75);  /* RTS */

    ctx->entry_point = a;
    ctx->m68k_argptr = ctx->m68k_argsize = ctx->m68k_sysbase = 0;

    LONG result = Kaffeine_Execute(ctx);
    CHK(result == 55, "expected 55");

    Kaffeine_DestroyContext(ctx);
    OK();
}

void t_exec_memory_access()
{
    TEST("Execute: write to memory, read back");
    struct KaffeineLibBase base = {};
    auto *ctx = Kaffeine_CreateContext(&base);

    /* Allocate a data area and code area */
    ULONG data = Kaffeine_HeapAlloc(ctx, 4, 0);
    ULONG code = Kaffeine_HeapAlloc(ctx, 18, 0);

    /* LEA data,A0; MOVE.L #0xCAFE,D0; MOVE.L D0,(A0); MOVE.L (A0),D0; RTS */
    m68k_write16(ctx, code,      0x207C);  /* MOVEA.L #imm,A0 */
    m68k_write32(ctx, code + 2,  data);    /* immediate = data addr */
    m68k_write16(ctx, code + 6,  0x203C);  /* MOVE.L #imm,D0 */
    m68k_write32(ctx, code + 8,  0xCAFE);  /* immediate = 0xCAFE */
    m68k_write16(ctx, code + 12, 0x2080);  /* MOVE.L D0,(A0) */
    m68k_write16(ctx, code + 14, 0x2010);  /* MOVE.L (A0),D0 */
    m68k_write16(ctx, code + 16, 0x4E75);  /* RTS */

    ctx->entry_point = code;
    ctx->m68k_argptr = ctx->m68k_argsize = ctx->m68k_sysbase = 0;

    LONG result = Kaffeine_Execute(ctx);
    CHK(result == (LONG)0xCAFE, "expected 0xCAFE");

    Kaffeine_DestroyContext(ctx);
    OK();
}

/* Thunks for the condition code test: one sets N and V and clears Z, the
   other leaves the CCR alone. */
static IPTR ccr_set_thunk(struct KaffeineContext *, void *cpu)
{
    Kaffeine_SetSR(cpu, (UWORD)((Kaffeine_GetSR(cpu) & ~0x000E) | 0x000A));
    return 0x1234;
}

static IPTR ccr_keep_thunk(struct KaffeineContext *, void *)
{
    return 0x5678;
}

static const struct M68KThunkEntry ccr_thunks[] = {
    { 30, ccr_set_thunk }, { 36, ccr_keep_thunk }, { 0, nullptr }
};

/* JSR to a library vector with the CCR set to ccr_in, then MOVE CCR,D0.
   The call goes through the LINEA trap, willExecute and the RTE at
   RTE_ADDR, as every library call from 68k code does. */
static LONG run_ccr_call(UWORD lvo, UWORD ccr_in)
{
    struct KaffeineLibBase base = {};
    auto *ctx = Kaffeine_CreateContext(&base);
    ULONG lib = Kaffeine_SetupLibBase(ctx, 2, "ccr.library", 8, ccr_thunks, 2, nullptr, 0);
    ULONG code = Kaffeine_HeapAlloc(ctx, 20, 0);

    m68k_write16(ctx, code,      0x2C7C);   /* MOVEA.L #lib,A6 */
    m68k_write32(ctx, code + 2,  lib);
    m68k_write16(ctx, code + 6,  0x44FC);   /* MOVE.W #ccr_in,CCR */
    m68k_write16(ctx, code + 8,  ccr_in);
    m68k_write16(ctx, code + 10, 0x4EAE);   /* JSR -lvo(A6) */
    m68k_write16(ctx, code + 12, (UWORD)-lvo);
    m68k_write16(ctx, code + 14, 0x42C0);   /* MOVE CCR,D0 */
    m68k_write16(ctx, code + 16, 0x4E75);   /* RTS */

    ctx->entry_point = code;
    ctx->m68k_argptr = ctx->m68k_argsize = ctx->m68k_sysbase = 0;
    LONG result = lib && code ? Kaffeine_Execute(ctx) : -1;
    Kaffeine_DestroyContext(ctx);
    return result;
}

void t_thunk_ccr_return()
{
    TEST("JSR to a thunk returns the CCR the thunk set");
    CHK((run_ccr_call(30, 0x1F) & 0xFFFF) == 0x1B, "N and V set, Z cleared, X and C kept");
    CHK((run_ccr_call(30, 0x00) & 0xFFFF) == 0x0A, "N and V set from a clear CCR");
    CHK((run_ccr_call(36, 0x15) & 0xFFFF) == 0x15, "a thunk that leaves the CCR keeps the caller's");
    OK();
}

int main()
{
    printf("kaffeine Moira execution tests\n");
    printf("=============================\n");
    t_fpu_flags_match_cpu();
    t_exec_moveq_rts();
    t_exec_add();
    t_exec_loop();
    t_exec_memory_access();
    t_thunk_ccr_return();
    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
