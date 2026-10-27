/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    The partial CLI exposed to the guest belongs to its emulation context.
*/
#ifndef KAFFEINE_CLI_H
#define KAFFEINE_CLI_H

#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Return the current guest CLI address, creating it once if native Cli()
   reports a CLI. Use the same address in guest Process.pr_CLI (as a BPTR).
   The object owns three 260-byte BSTR slots (command, directory name,
   prompt), not native shadow mappings. Failure sets IoErr(): absent CLI,
   allocation failure or an invalid allocation range. */
ULONG Kaffeine_GetCLI(struct KaffeineContext *ctx);
IPTR kaffeine_dos_Cli(struct KaffeineContext *ctx, void *cpu);

#define KAFFEINE_DOS_CLI_THUNKS \
    { 492, kaffeine_dos_Cli }

#ifdef __cplusplus
}
#endif

#endif /* KAFFEINE_CLI_H */
