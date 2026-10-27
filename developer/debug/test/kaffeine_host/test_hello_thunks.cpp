/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Author: Fabian Schmieder (@metaneutrons)
 */
#include "kaffeine_intern.h"
#include "kaffeine_thunks.h"
#include <cstdio>
#include <cstring>

static unsigned open_calls = 0;
static unsigned putstr_calls = 0;
static bool thunk_arguments_ok = true;
static const char expected_hello[] = "Hello from m68k!\n";

static IPTR thunk_OpenLibrary(struct KaffeineContext *ctx, void *cpu)
{
    const char *name = static_cast<const char *>(m68k_to_host(ctx, Kaffeine_GetA(cpu, 1)));
    open_calls++;
    thunk_arguments_ok &= name && strcmp(name, "dos.library") == 0;
    printf("  [THUNK] OpenLibrary(\"%s\")\n", name ? name : "NULL");
    ULONG base = Kaffeine_FindLibBase(ctx, name);
    printf("  [THUNK] -> 0x%08X\n", base);
    return (IPTR)base;
}

static IPTR thunk_PutStr(struct KaffeineContext *ctx, void *cpu)
{
    const char *str = static_cast<const char *>(m68k_to_host(ctx, Kaffeine_GetD(cpu, 1)));
    putstr_calls++;
    thunk_arguments_ok &= str && strcmp(str, expected_hello) == 0;
    printf("  [THUNK] PutStr(\"%s\")\n", str ? str : "NULL");
    return 0;
}

const struct M68KThunkEntry kaffeine_thunks_exec[] = { { 552, thunk_OpenLibrary }, { 0, NULL } };
const ULONG kaffeine_thunks_exec_count = 1;

const struct M68KThunkEntry kaffeine_thunks_dos[] = { { 948, thunk_PutStr }, { 0, NULL } };
const ULONG kaffeine_thunks_dos_count = 1;

extern "C" {
ULONG Kaffeine_OpenLibrary(struct KaffeineContext *, const char *) { return 0; }
ULONG Kaffeine_OpenM68KLibrary(struct KaffeineContext *, void *, const char *, ULONG) { return 0; }
BOOL Kaffeine_CloseM68KLibrary(struct KaffeineContext *, void *, ULONG) { return 0; }
}

#include "kaffeine_memory.c"
#include "kaffeine_cpu.c"
#include "kaffeine_dispatch.c"
#include "Moira/Moira.cpp"
#include "Moira/MoiraDebugger.cpp"
#include "kaffeine_moira.cpp"

int main()
{
    printf("=== test_hello thunk test ===\n");
    struct KaffeineLibBase base = {};
    auto *ctx = Kaffeine_CreateContext(&base);
    if (!ctx) {
        printf("FAIL: cannot allocate emulator context\n");
        return 1;
    }

    ULONG sysbase = Kaffeine_SetupLibBase(ctx, 0, "exec.library", 512, kaffeine_thunks_exec, 1, nullptr, 0);
    printf("ExecBase=0x%08X jt_start=0x%08X\n", sysbase, ctx->libs[0].jt_start);

    ULONG dosbase = Kaffeine_SetupLibBase(ctx, 1, "dos.library", 512, kaffeine_thunks_dos, 1, nullptr, 0);
    printf("DOSBase=0x%08X jt_start=0x%08X\n", dosbase, ctx->libs[1].jt_start);
    if (!sysbase || !dosbase) {
        printf("FAIL: cannot create library bases\n");
        Kaffeine_DestroyContext(ctx);
        return 1;
    }

    m68k_write32(ctx, 4, sysbase);

    ULONG code = Kaffeine_HeapAlloc(ctx, 128, 0);
    if (!code) {
        printf("FAIL: cannot allocate guest code\n");
        Kaffeine_DestroyContext(ctx);
        return 1;
    }
    ULONG p = code;

    m68k_write16(ctx, p, 0x2C78); p += 2;
    m68k_write16(ctx, p, 0x0004); p += 2;

    ULONG lea_a1 = p;
    m68k_write16(ctx, p, 0x43FA); p += 2;
    m68k_write16(ctx, p, 0x0000); p += 2;

    m68k_write16(ctx, p, 0x7000); p += 2;

    m68k_write16(ctx, p, 0x4EAE); p += 2;
    m68k_write16(ctx, p, 0xFDD8); p += 2;

    m68k_write16(ctx, p, 0x4A80); p += 2;

    ULONG beq = p;
    m68k_write16(ctx, p, 0x6700); p += 2;

    m68k_write16(ctx, p, 0x2C40); p += 2;

    ULONG lea_a0 = p;
    m68k_write16(ctx, p, 0x41FA); p += 2;
    m68k_write16(ctx, p, 0x0000); p += 2;

    m68k_write16(ctx, p, 0x2208); p += 2;

    m68k_write16(ctx, p, 0x4EAE); p += 2;
    m68k_write16(ctx, p, 0xFC4C); p += 2;

    m68k_write16(ctx, p, 0x7000); p += 2;
    m68k_write16(ctx, p, 0x4E75); p += 2;

    ULONG fail = p;
    m68k_write16(ctx, p, 0x7014); p += 2;
    m68k_write16(ctx, p, 0x4E75); p += 2;

    ULONG dosname = p;
    const char *dn = "dos.library";
    for (int i = 0; dn[i]; i++) ctx->mem[p++] = dn[i];
    ctx->mem[p++] = 0;

    ULONG hello = p;
    const char *hs = expected_hello;
    for (int i = 0; hs[i]; i++) ctx->mem[p++] = hs[i];
    ctx->mem[p++] = 0;

    int16_t off;
    off = (int16_t)(dosname - (lea_a1 + 2));
    m68k_write16(ctx, lea_a1 + 2, (uint16_t)off);
    off = (int16_t)(fail - (beq + 2));
    ctx->mem[beq + 1] = (uint8_t)(off & 0xFF);
    off = (int16_t)(hello - (lea_a0 + 2));
    m68k_write16(ctx, lea_a0 + 2, (uint16_t)off);

    ULONG ol_slot = sysbase - 552;
    printf("OpenLibrary slot @0x%08X: %04X\n", ol_slot, m68k_read16(ctx, ol_slot));

    ULONG ps_slot = dosbase - 948;
    printf("PutStr slot @0x%08X: %04X\n", ps_slot, m68k_read16(ctx, ps_slot));

    ctx->entry_point = code;
    ctx->m68k_argptr = 0;
    ctx->m68k_argsize = 0;
    ctx->m68k_sysbase = sysbase;

    printf("Executing...\n");
    LONG result = Kaffeine_Execute(ctx);
    printf("Result: %d\n", (int)result);

    bool passed = result == 0 && open_calls == 1 && putstr_calls == 1 && thunk_arguments_ok;
    printf("%s: OpenLibrary calls=%u, PutStr calls=%u, arguments=%s\n",
           passed ? "PASS" : "FAIL", open_calls, putstr_calls,
           thunk_arguments_ok ? "correct" : "incorrect");
    Kaffeine_DestroyContext(ctx);
    return passed ? 0 : 1;
}
