/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Picasso96API.library of 68k code (kaffeine_p96.c).
*/
#ifndef KAFFEINE_P96_H
#define KAFFEINE_P96_H

#include "kaffeine_intern.h"
#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The version that the base gives and the highest that an opener gets. */
#define KAFFEINE_P96_VERSION 2

extern const struct M68KThunkEntry kaffeine_thunks_p96[];
extern const ULONG kaffeine_thunks_p96_count;

/* OpenLibrary("Picasso96API.library", version): the base of the context,
   made at the first open, or 0 for a version above KAFFEINE_P96_VERSION or
   when the context has no room for one more base, leaving it unchanged. */
ULONG Kaffeine_P96Open(struct KaffeineContext *ctx, ULONG version);

#ifdef __cplusplus
}
#endif

#endif /* KAFFEINE_P96_H */
