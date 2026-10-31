/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Keep Cli() and the guest Process.pr_CLI fields on the same partial CLI.
    Its strings and BPTRs must stay entirely within guest memory.
*/

#include <proto/dos.h>
#include <string.h>

#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_cli.h"

ULONG Kaffeine_GetCLI(struct KaffeineContext *ctx)
{
    ULONG len, size, cli, name;

    /* The context may be prepared in one native process and executed in
       another. Keep the guest CLI chosen at setup, including guest writes. */
    if (ctx->m68k_cli)
        return ctx->m68k_cli;
    if (!Cli())
    {
        SetIoErr(ERROR_OBJECT_WRONG_TYPE);
        return 0;
    }

    for (len = 0; len < sizeof(ctx->program_name) - 1; len++)
        if (!ctx->program_name[len])
            break;

    /* Fixed-capacity BSTRs let the guest setters change names without
       reallocating or writing past the startup command's shorter string.
       These are guest state: prompt and directory name initially are empty. */
    size = M68K_SIZEOF_CLI + 3 * M68K_CLI_STRING_SLOT;
    cli = Kaffeine_HeapAlloc(ctx, size, 0);
    if (!cli)
    {
        SetIoErr(ERROR_NO_FREE_STORE);
        return 0;
    }
    if (cli >= ctx->mem_size || ctx->mem_size - cli < size)
    {
        SetIoErr(ERROR_BAD_NUMBER);
        return 0;
    }
    memset(ctx->mem + cli, 0, size);

    /* Native paths and stream handles remain unsupported. */
    m68k_write32(ctx, cli + M68K_CLI_DEFAULTSTACK, 4096);
    m68k_write32(ctx, cli + M68K_CLI_MODULE,
                 ctx->seg_start ? ctx->seg_start >> 2 : 1);
    name = cli + M68K_SIZEOF_CLI;
    ctx->mem[name] = (UBYTE)len;
    memcpy(ctx->mem + name + 1, ctx->program_name, len);
    m68k_write32(ctx, cli + M68K_CLI_COMMANDNAME, name >> 2);
    m68k_write32(ctx, cli + M68K_CLI_SETNAME, (name + M68K_CLI_STRING_SLOT) >> 2);
    m68k_write32(ctx, cli + M68K_CLI_PROMPT, (name + 2 * M68K_CLI_STRING_SLOT) >> 2);

    ctx->m68k_cli = cli;
    return cli;
}

/* -492: Cli() has no register arguments and returns an APTR, not a BPTR. */
IPTR kaffeine_dos_Cli(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return Kaffeine_GetCLI(ctx);
}
