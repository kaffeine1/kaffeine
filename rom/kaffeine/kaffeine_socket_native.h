/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#ifndef KAFFEINE_SOCKET_NATIVE_H
#define KAFFEINE_SOCKET_NATIVE_H
#include <exec/ports.h>
#include <stdint.h>

#define KSOCKET_FDS 64
#define KSOCKET_SET_BYTES (KSOCKET_FDS / 8)
#define KSOCKET_MAX_BYTES (1024 * 1024)

struct KSocketNative;
/* No context, guest address, CPU, guest callback or borrowed arena pointer
 * enters this message. buffer, when present, is a private native allocation. */
struct KSocketPacket
{
    struct Message message;
    ULONG op;
    LONG arg[4], result, error;
    ULONG length, option_length, progress;
    UBYTE address[16], option[8];
    UBYTE sets[3][KSOCKET_SET_BYTES], ready[3][KSOCKET_SET_BYTES];
    BOOL has_set[3], has_timeout, nonblocking, again, connecting;
    uint64_t duration, deadline;
    BOOL timed;
    UBYTE *buffer;
};

struct KSocketNative *KSocketNativeStart(void);
void KSocketNativePost(struct KSocketNative *, struct KSocketPacket *);
void KSocketNativeCancel(struct KSocketNative *);
/* Only after the outstanding packet has replied. */
void KSocketNativeStop(struct KSocketNative *);
#endif
