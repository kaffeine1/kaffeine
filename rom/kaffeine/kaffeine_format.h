/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef KAFFEINE_FORMAT_H
#define KAFFEINE_FORMAT_H

#include "kaffeine_thunks.h"
#ifdef __cplusplus
extern "C" {
#endif
IPTR thunk_exec_RawDoFmt(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_VSNPrintf(struct KaffeineContext *ctx, void *cpu);
/* Checks a RawDoFmt template and its whole 68k data stream, then sends
   every character, the final NUL included, to put(). FALSE means invalid
   input, before the first character, or a put() that returned FALSE. */
typedef BOOL (*KaffeineFormatPut)(void *state, UBYTE ch);
BOOL Kaffeine_FormatToPut(struct KaffeineContext *ctx, ULONG fmt, ULONG args,
                         KaffeineFormatPut put, void *state);
/* The same, and *next is where the data of the next template would start,
   as the next argument after a RawDoFmt() call. */
BOOL Kaffeine_FormatToPutNext(struct KaffeineContext *ctx, ULONG fmt, ULONG args,
                             KaffeineFormatPut put, void *state, ULONG *next);
/* The checks alone: *count is the number of characters with the final NUL,
   *next as above. FALSE means invalid input. */
BOOL Kaffeine_FormatCount(struct KaffeineContext *ctx, ULONG fmt, ULONG args,
                         ULONG *count, ULONG *next);
#ifdef __cplusplus
}
#endif

#endif
