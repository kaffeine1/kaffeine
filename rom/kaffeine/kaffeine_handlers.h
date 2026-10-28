/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef KAFFEINE_HANDLERS_H
#define KAFFEINE_HANDLERS_H
#include "kaffeine_intern.h"
#ifdef __cplusplus
extern "C" {
#endif
ULONG Kaffeine_HandlerPort(struct KaffeineContext *, struct MsgPort *);
IPTR Kaffeine_HandlerPacket(struct KaffeineContext *, ULONG, ULONG);
void Kaffeine_HandlerCleanup(struct KaffeineContext *);
IPTR kaffeine_dos_DeviceProc(struct KaffeineContext *, void *);
IPTR kaffeine_dos_GetConsoleTask(struct KaffeineContext *, void *);
IPTR kaffeine_dos_SetConsoleTask(struct KaffeineContext *, void *);
IPTR kaffeine_dos_GetFileSysTask(struct KaffeineContext *, void *);
IPTR kaffeine_dos_SetFileSysTask(struct KaffeineContext *, void *);
IPTR kaffeine_dos_GetDeviceProc(struct KaffeineContext *, void *);
IPTR kaffeine_dos_FreeDeviceProc(struct KaffeineContext *, void *);
#define KAFFEINE_DOS_HANDLER_THUNKS \
    {174, kaffeine_dos_DeviceProc}, {510, kaffeine_dos_GetConsoleTask}, \
    {516, kaffeine_dos_SetConsoleTask}, {522, kaffeine_dos_GetFileSysTask}, \
    {528, kaffeine_dos_SetFileSysTask}, {642, kaffeine_dos_GetDeviceProc}, \
    {648, kaffeine_dos_FreeDeviceProc}
#ifdef __cplusplus
}
#endif
#endif
