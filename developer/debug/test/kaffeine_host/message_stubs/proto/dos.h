/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#pragma once
#include_next <proto/dos.h>
#ifdef __cplusplus
extern "C" {
#endif
LONG Close(BPTR);
LONG Seek(BPTR, LONG, LONG);
LONG WaitForChar(BPTR, LONG);
LONG IsInteractive(BPTR);
BPTR DupLockFromFH(BPTR);
BPTR ParentOfFH(BPTR);
BPTR OpenFromLock(BPTR);
LONG SetMode(BPTR, LONG);
LONG SetFileSize(BPTR, LONG, LONG);
LONG LockRecord(BPTR, ULONG, ULONG, ULONG, ULONG);
LONG UnLockRecord(BPTR, ULONG, ULONG);
LONG ChangeMode(ULONG, BPTR, LONG);
BPTR SelectInput(BPTR);
BPTR SelectOutput(BPTR);
LONG Flush(BPTR);
BOOL UnLock(BPTR);
struct MsgPort *DeviceProc(CONST_STRPTR);
struct DevProc *GetDeviceProc(CONST_STRPTR, struct DevProc *);
void FreeDeviceProc(struct DevProc *);
struct MsgPort *GetConsoleTask(void);
struct MsgPort *SetConsoleTask(struct MsgPort *);
struct MsgPort *GetFileSysTask(void);
struct MsgPort *SetFileSysTask(struct MsgPort *);
#ifdef __cplusplus
}
#endif
