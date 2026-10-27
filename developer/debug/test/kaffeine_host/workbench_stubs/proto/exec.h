#pragma once
/* Counted native allocations and a controllable Exec queue, defined by the test. */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/tasks.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
APTR AllocMem(ULONG size, ULONG flags);
void FreeMem(APTR memory, ULONG size);
APTR AllocVec(ULONG size, ULONG flags);
void FreeVec(APTR memory);
BYTE AllocSignal(LONG signalNum);
void FreeSignal(LONG signalNum);
void Signal(struct Task *task, ULONG signalSet);
struct Task *FindTask(CONST_STRPTR name);
struct MsgPort *CreateMsgPort(void);
void DeleteMsgPort(struct MsgPort *port);
APTR CreateIORequest(struct MsgPort *port, ULONG size);
void DeleteIORequest(APTR request);
LONG OpenDevice(CONST_STRPTR name, IPTR unit, struct IORequest *request, ULONG flags);
void CloseDevice(struct IORequest *request);
LONG DoIO(struct IORequest *request);
struct Message *GetMsg(struct MsgPort *port);
struct Message *WaitPort(struct MsgPort *port);
void ReplyMsg(struct Message *message);
void PutMsg(struct MsgPort *port, struct Message *message);
void RawPutChar(UBYTE ch);
#ifdef __cplusplus
}
#endif
#define CopyMem(source, dest, size) memcpy(dest, source, size)
#define Forbid() ((void)0)
#define Permit() ((void)0)
