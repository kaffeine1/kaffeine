/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    ReadArgs(), FreeArgs() and GetProgramName() for 68k callers.
*/
#ifndef KAFFEINE_READARGS_H
#define KAFFEINE_READARGS_H

#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

IPTR Kaffeine_ReadArgs(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_FreeArgs(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_GetProgramName(struct KaffeineContext *ctx, void *cpu);

/* AllocDosObject(DOS_RDARGS) and FreeDosObject(DOS_RDARGS) for 68k
   callers: a cleared 68k RDArgs in guest memory, which the program fills
   in itself. Freeing it also releases results still held for it. */
ULONG Kaffeine_AllocRDArgs(struct KaffeineContext *ctx);
void Kaffeine_FreeRDArgs(struct KaffeineContext *ctx, ULONG rdargs);

/* Free the native RDArgs that a program did not release with FreeArgs(). */
void Kaffeine_ReadArgsCleanup(struct KaffeineContext *ctx);

#ifdef __cplusplus
}
#endif

/* Shared by the production DOS table and the host test. */
#define KAFFEINE_DOS_READARGS_THUNKS \
    { 576, Kaffeine_GetProgramName }, \
    { 798, Kaffeine_ReadArgs }, \
    { 858, Kaffeine_FreeArgs }

#endif /* KAFFEINE_READARGS_H */
