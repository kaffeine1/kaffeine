/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#pragma once
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <string.h>
#include <utility/tagitem.h>
#ifdef __cplusplus
extern "C" {
#endif
APTR AllocMem(ULONG, ULONG);
void FreeMem(APTR, ULONG);
APTR AllocVec(ULONG, ULONG);
void FreeVec(APTR);
BYTE AllocSignal(LONG);
void FreeSignal(LONG);
void Signal(struct Task *, ULONG);
struct Task *FindTask(CONST_STRPTR);
BYTE SetTaskPri(struct Task *, LONG);
void RawPutChar(UBYTE);
void PutMsg(struct MsgPort *, struct Message *);
struct Message *GetMsg(struct MsgPort *);
struct Message *WaitPort(struct MsgPort *);
ULONG Wait(ULONG);
ULONG SetSignal(ULONG, ULONG);
void ReplyMsg(struct Message *);
#ifdef __cplusplus
}
#endif
#define CopyMem(s,d,n) memcpy(d,s,n)
#define Forbid() ((void)0)
#define Permit() ((void)0)
