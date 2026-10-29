/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Run the hosted translation probe on native AROS AArch64, with the
    production CPU loop, library dispatcher and guest-call primitive.
    This is a native test executable, not a kaffeine.library backend.
*/
#if defined(__AROS__)
#include <aros/config.h>
#endif
#if !defined(__AROS__) || !defined(__aarch64__) || AROS_FLAVOUR != AROS_FLAVOUR_STANDALONE
#error "This probe must run on native AROS AArch64"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kaffeine_intern.h"
#include "kaffeine_callback.h"
#include "kaffeine_cpu.h"
#include "kaffeine_thunks.h"
#include "kaffeine_jit.h"

enum { ENTRY = 0x2000, HOOK = 0x3000, DATA = 0x4000, SYSBASE = 0x1000 };
static unsigned checks, failures, echo_calls;
static ULONG echo_input, callback_result;

static int check(int ok, const char *what)
{
    ++checks;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
    return ok;
}

static IPTR echo(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    ++echo_calls;
    echo_input = Kaffeine_GetD(cpu, 0);
    Kaffeine_SetA(cpu, 2, 0x12345678);
    return echo_input + 42;
}

static IPTR callback(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineGuestCall call;
    memset(&call, 0, sizeof(call));
    if (!Kaffeine_CallGuest(ctx, cpu, HOOK, &call))
        return Kaffeine_GetD(cpu, 0);
    callback_result = call.d[0];
    return call.d[0] + 1;
}

static IPTR patch_hook(struct KaffeineContext *ctx, void *cpu)
{
    m68k_write16(ctx, HOOK, 0x7009); /* MOVEQ #9,D0 */
    Kaffeine_FlushCode(cpu, HOOK, 2);
    return 0;
}

static const struct M68KThunkEntry thunks[] = {
    {30, echo}, {36, callback}, {42, patch_hook}
};

static struct KaffeineContext *context(void)
{
    struct KaffeineContext *ctx = calloc(1, sizeof(*ctx));
    unsigned i;
    if (!ctx) return NULL;
    ctx->mem_size = 0x10000;
    ctx->mem = calloc(1, ctx->mem_size + KAFFEINE_MEM_GUARD);
    if (!ctx->mem) { free(ctx); return NULL; }
    ctx->stack_top = 0xF000;
    ctx->entry_point = ENTRY;
    ctx->m68k_sysbase = SYSBASE;
    ctx->num_libs = 1;
    ctx->libs[0].m68k_addr = SYSBASE;
    ctx->libs[0].jt_start = SYSBASE - 0x100;
    strcpy(ctx->libs[0].name, "native-jit-probe.library");
    ctx->libs[0].thunks = thunks;
    ctx->libs[0].num_thunks = sizeof(thunks) / sizeof(thunks[0]);
    for (i = 0; i < sizeof(thunks) / sizeof(thunks[0]); ++i) {
        m68k_write16(ctx, SYSBASE - thunks[i].lvo, 0xA000);
        m68k_write16(ctx, SYSBASE - thunks[i].lvo + 2, 0x4E71);
    }
    return ctx;
}

static void code(struct KaffeineContext *ctx, ULONG at, const UWORD *words, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i) m68k_write16(ctx, at + 2 * i, words[i]);
}

static void run_case(const char *name, const UWORD *words, unsigned n, LONG want,
                     unsigned kind)
{
    struct KaffeineContext *ctx = context();
    struct HJBackend *jit;
    const struct HJStats *stats;
    LONG result;
    printf("STEP: %s\n", name);
    if (!check(ctx != NULL, "allocate a guest context")) return;
    code(ctx, ENTRY, words, n);
    if (kind == 3) {
        const UWORD hook[] = {0x7005, 0x4EAE, 0xFFE2, 0x4E75};
        code(ctx, HOOK, hook, sizeof(hook) / sizeof(hook[0]));
    } else if (kind == 4) {
        const UWORD hook[] = {0x7005, 0x4E75};
        code(ctx, HOOK, hook, sizeof(hook) / sizeof(hook[0]));
    }
    jit = HJ_Create(ctx);
    if (!check(jit != NULL, "allocate executable JIT memory")) goto done;
    echo_calls = 0; echo_input = callback_result = 0;
    result = HJ_Execute(jit);
    check(result == want, "translated program returns the expected result");
    stats = HJ_GetStats(jit);
    check(stats->blocks && stats->enters && stats->icache_syncs,
          "execute published AArch64 blocks");
    if (kind == 1)
        check(ctx->mem[DATA] == 0x12 && ctx->mem[DATA + 1] == 0x34 &&
              ctx->mem[DATA + 2] == 0x56 && ctx->mem[DATA + 3] == 0x78,
              "guest longword memory remains big-endian");
    if (kind == 2)
        check(echo_calls == 1 && echo_input == 7, "real dispatcher receives live guest registers");
    if (kind == 3) {
        check(echo_calls == 1 && echo_input == 5 && callback_result == 47,
              "callback re-enters translated code and the native dispatcher");
        check(stats->max_depth >= 2, "nested execution has its own return boundary");
    }
    if (kind == 4) {
        check(stats->flushes == 1 && m68k_read16(ctx, HOOK) == 0x7009,
              "guest code mutation invalidates translations");
        check(stats->blocks >= 3, "changed code is translated and published again");
    }
    check((HJ_LastFault(jit)[0] != 0) == (kind == 5),
          "unsupported instructions report an explicit fault");
    printf("STATS: %lu blocks, %lu enters, %lu cache publications, %lu flushes\n",
           stats->blocks, stats->enters, stats->icache_syncs, stats->flushes);
    HJ_Destroy(jit);
done:
    free(ctx->mem);
    free(ctx);
}

int main(void)
{
    char message[128];
    const UWORD simple[] = {0x702A, 0x4E75};
    const UWORD memory[] = {0x41F9, 0, DATA, 0x20BC, 0x1234, 0x5678, 0x2010, 0x4E75};
    const UWORD native[] = {0x7007, 0x4EAE, 0xFFE2, 0x5280, 0x4E75};
    const UWORD nested[] = {0x4EAE, 0xFFDC, 0x4E75};
    const UWORD changed[] = {0x4EB9, 0, HOOK, 0x4EAE, 0xFFD6, 0x4EB9, 0, HOOK, 0x4E75};
    const UWORD unsupported[] = {0x4AFC};
    check(HJ_EncoderSelfTest(message, sizeof(message)) == 0, "AArch64 encoder reference words");
    puts(message);
    if (!check(HJ_Available(), "native AROS AArch64 execution is enabled")) goto done;
    printf("EXEC: %s\n", HJ_ExecMemoryKind());
    run_case("return", simple, sizeof(simple) / sizeof(simple[0]), 42, 0);
    run_case("big-endian memory", memory, sizeof(memory) / sizeof(memory[0]), 0x12345678, 1);
    run_case("native thunk", native, sizeof(native) / sizeof(native[0]), 50, 2);
    run_case("nested callback", nested, sizeof(nested) / sizeof(nested[0]), 48, 3);
    run_case("modified guest code", changed, sizeof(changed) / sizeof(changed[0]), 9, 4);
    run_case("unsupported instruction", unsupported, sizeof(unsupported) / sizeof(unsupported[0]), 20, 5);
done:
    printf("jit_probe: %u checks, %u failed\n", checks, failures);
    return failures ? 10 : 0;
}
