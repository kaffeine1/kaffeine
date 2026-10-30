/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#pragma once
#include <exec/types.h>
#define SIGB_DOS 8
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/tasks.h>
#include <string.h>
#define CopyMem(source,dest,size) memcpy(dest,source,size)
#define NT_PROCESS 13
struct Process { struct Task pr_Task; APTR pr_WindowPtr; };
#ifdef __cplusplus
extern "C" {
#endif
APTR OpenResource(CONST_STRPTR);
APTR AllocVec(ULONG,ULONG);
void FreeVec(APTR);
APTR AllocMem(ULONG,ULONG);
void FreeMem(APTR,ULONG);
BYTE AllocSignal(LONG);
void FreeSignal(LONG);
void Signal(struct Task *,ULONG);
struct Task *FindTask(CONST_STRPTR);
ULONG SetSignal(ULONG,ULONG);
void Forbid(void);
void Permit(void);
struct MsgPort *CreateMsgPort(void);
void DeleteMsgPort(struct MsgPort *);
APTR CreateIORequest(struct MsgPort *,ULONG);
void DeleteIORequest(APTR);
LONG OpenDevice(CONST_STRPTR,IPTR,struct IORequest *,ULONG);
void CloseDevice(struct IORequest *);
LONG DoIO(struct IORequest *);
void SendIO(struct IORequest *);
LONG AbortIO(struct IORequest *);
LONG WaitIO(struct IORequest *);
struct Message *GetMsg(struct MsgPort *);
ULONG Wait(ULONG);

struct SignalSemaphore;
ULONG AttemptSemaphore(struct SignalSemaphore *);
#ifdef __cplusplus
}
#endif
