/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Exec ports, I/O requests and signals of 68k code, and device adapters.
*/
#ifndef KAFFEINE_EXEC_IO_H
#define KAFFEINE_EXEC_IO_H

#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

IPTR Kaffeine_Signal(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_AllocSignal(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_FreeSignal(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_OpenDevice(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_CloseDevice(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_DoIO(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_SendIO(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_CheckIO(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_WaitIO(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_AbortIO(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_CreateIORequest(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_DeleteIORequest(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_CreateMsgPort(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_DeleteMsgPort(struct KaffeineContext *ctx, void *cpu);

/* Stop the guest with return code 20 and say why. */
IPTR Kaffeine_IORefuse(struct KaffeineContext *ctx, const char *why);

/* Close what the guest left open and free its signals. Call it from the
   native task that ran the guest, before that task ends: the signals and
   the native reply ports belong to it. */
void Kaffeine_IOCleanup(struct KaffeineContext *ctx);

/* timer.device */
IPTR kaffeine_timer_BeginIO(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_timer_AbortIO(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_timer_AddTime(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_timer_SubTime(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_timer_CmpTime(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_timer_ReadEClock(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_timer_GetSysTime(struct KaffeineContext *ctx, void *cpu);
extern const struct M68KThunkEntry kaffeine_thunks_timer[];
extern const ULONG kaffeine_thunks_timer_count;
/* Vectors -6 to -72: the library ones, BeginIO, AbortIO, AddTime, SubTime,
   CmpTime, ReadEClock, GetSysTime and AROS's GetUpTime. */
#define KAFFEINE_TIMER_VECTORS 12

#ifdef __cplusplus
}
#endif

/* Shared by the production manual table and the host dispatch tests. */
#define KAFFEINE_EXEC_IO_THUNKS \
    { 324, Kaffeine_Signal }, \
    { 330, Kaffeine_AllocSignal }, \
    { 336, Kaffeine_FreeSignal }, \
    { 444, Kaffeine_OpenDevice }, \
    { 450, Kaffeine_CloseDevice }, \
    { 456, Kaffeine_DoIO }, \
    { 462, Kaffeine_SendIO }, \
    { 468, Kaffeine_CheckIO }, \
    { 474, Kaffeine_WaitIO }, \
    { 480, Kaffeine_AbortIO }, \
    { 654, Kaffeine_CreateIORequest }, \
    { 660, Kaffeine_DeleteIORequest }, \
    { 666, Kaffeine_CreateMsgPort }, \
    { 672, Kaffeine_DeleteMsgPort }

#endif /* KAFFEINE_EXEC_IO_H */
