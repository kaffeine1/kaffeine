#pragma once
#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/semaphores.h>
#include <exec/ports.h>
#ifdef __cplusplus
extern "C" {
#endif
void InitSemaphore(struct SignalSemaphore *);
void ObtainSemaphore(struct SignalSemaphore *);
void ReleaseSemaphore(struct SignalSemaphore *);
APTR AllocVec(ULONG, ULONG);
void FreeVec(APTR);
struct Library *OpenLibrary(CONST_STRPTR, ULONG);
void CloseLibrary(struct Library *);
struct Task *FindTask(CONST_STRPTR);
ULONG SetSignal(ULONG, ULONG);
void Signal(struct Task *, ULONG);
struct MsgPort *CreateMsgPort(void);
void DeleteMsgPort(struct MsgPort *);
void PutMsg(struct MsgPort *, struct Message *);
struct Message *GetMsg(struct MsgPort *);
struct Message *WaitPort(struct MsgPort *);
void ReplyMsg(struct Message *);
#ifdef __cplusplus
}
#endif
