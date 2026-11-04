/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#ifndef KAFFEINE_SOCKET_H
#define KAFFEINE_SOCKET_H
#include "kaffeine_intern.h"
#include "kaffeine_thunks.h"

/* Only the driver accesses the arena. Native socket workers receive typed,
 * copied arguments and own their own library bases and descriptor tables. */
ULONG Kaffeine_SocketOpen(struct KaffeineContext *ctx);
BOOL Kaffeine_SocketClose(struct KaffeineContext *ctx, ULONG base);
void Kaffeine_SocketCleanup(struct KaffeineContext *ctx);
extern const struct M68KThunkEntry kaffeine_thunks_socket[];
extern const ULONG kaffeine_thunks_socket_count;
#endif
