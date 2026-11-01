/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    asl.library of 68k code: the file requester subset of kaffeine_asl.c.
*/
#ifndef KAFFEINE_ASL_H
#define KAFFEINE_ASL_H

#include "kaffeine_intern.h"
#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The 68k FileRequester and the buffers of its strings, in one mirror */
#define KAFFEINE_ASL_FILE_BYTES    257     /* MAX_FILE_LEN of native ASL */
#define KAFFEINE_ASL_DRAWER_BYTES  1025    /* MAX_PATH_LEN */
#define KAFFEINE_ASL_PATTERN_BYTES 257     /* MAX_PATTERN_LEN */
#define KAFFEINE_ASL_MIRROR_SIZE   (56 + KAFFEINE_ASL_FILE_BYTES + KAFFEINE_ASL_DRAWER_BYTES + \
                                    KAFFEINE_ASL_PATTERN_BYTES)

/* Every vector of asl.library, manual: three of them work, the other five
   stop the program; no generated wrapper is ever reached. */
extern const struct M68KThunkEntry kaffeine_thunks_asl[];
extern const ULONG kaffeine_thunks_asl_count;

/* At the end of the program, in the task that ran it and before the
   screens are cleaned up: every requester that the program left is freed
   natively and its mirror released. Safe to call again. */
void Kaffeine_AslCleanup(struct KaffeineContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* KAFFEINE_ASL_H */
