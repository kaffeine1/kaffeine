/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    workbench.library of 68k code (kaffeine_workbench.c).
*/
#ifndef KAFFEINE_WORKBENCH_H
#define KAFFEINE_WORKBENCH_H

#include "kaffeine_intern.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const struct M68KThunkEntry kaffeine_thunks_workbench[];
extern const ULONG kaffeine_thunks_workbench_count;

/* A 68k port that an AppWindow, AppIcon or AppMenuItem of the program sends
   to: with no message in its list, the next native AppMessage goes to the
   tail of the list as a 68k copy, after a native WaitPort() when wait is
   TRUE. FALSE for any other port, whose list stays as it is. */
BOOL Kaffeine_AppPortFill(struct KaffeineContext *ctx, ULONG port, BOOL wait);
/* ReplyMsg() of a 68k copy of an AppMessage: the native message goes back
   to Workbench and the copy is freed. FALSE for any other message. */
BOOL Kaffeine_AppReplyMsg(struct KaffeineContext *ctx, ULONG message);
/* The 68k port goes, or its signal: the App objects on it go first, then
   every AppMessage of it goes back. */
void Kaffeine_AppPortGone(struct KaffeineContext *ctx, ULONG port);
void Kaffeine_AppSignalGone(struct KaffeineContext *ctx, BYTE sigbit);
/* The end of the program: every App object goes and every AppMessage goes
   back, in the task that ran the guest. */
void Kaffeine_WorkbenchCleanup(struct KaffeineContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* KAFFEINE_WORKBENCH_H */
