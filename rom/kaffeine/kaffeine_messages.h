/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef KAFFEINE_MESSAGES_H
#define KAFFEINE_MESSAGES_H
#include "kaffeine_intern.h"
#ifdef __cplusplus
extern "C" {
#endif
#define KAFFEINE_MESSAGE_LIMIT 256
struct KaffeineQueue
{
    ULONG port, nodes[KAFFEINE_MESSAGE_LIMIT], sizes[KAFFEINE_MESSAGE_LIMIT];
    unsigned count;
};
BOOL Kaffeine_GuestBytes(struct KaffeineContext *, ULONG, ULONG);
BOOL Kaffeine_Overlap(ULONG, ULONG, ULONG, ULONG);
/* Inspection only: arrival metadata is not used by GetMsg/nonempty WaitPort. */
BOOL Kaffeine_QueueRead(struct KaffeineContext *, ULONG, struct KaffeineQueue *);
BOOL Kaffeine_QueueDisjoint(const struct KaffeineQueue *, ULONG, ULONG);
BOOL Kaffeine_MessageMemory(struct KaffeineContext *, ULONG, ULONG);
/* Enqueue preflight includes destination arrival permissions, also before DOS Read. */
BOOL Kaffeine_MessagePreflight(struct KaffeineContext *, struct KaffeineQueue *, ULONG, ULONG);
void Kaffeine_MessageAppend(struct KaffeineContext *, const struct KaffeineQueue *, ULONG, UBYTE);
IPTR Kaffeine_MessageFailure(struct KaffeineContext *, const char *);
IPTR Kaffeine_GuestPutMsg(struct KaffeineContext *, ULONG, ULONG, ULONG);
IPTR Kaffeine_GuestGetMsg(struct KaffeineContext *, ULONG, BOOL);
IPTR Kaffeine_GuestReplyMsg(struct KaffeineContext *, ULONG);
#ifdef __cplusplus
}
#endif
#endif
