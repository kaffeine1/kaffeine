#pragma once
/* The native Exec of the exec I/O tests: each function is defined by the
   test, which records every call and every native allocation. */
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
BYTE AllocSignal(LONG signalNum);
void FreeSignal(LONG signalNum);
void Signal(struct Task *task, ULONG signalSet);
struct Task *FindTask(CONST_STRPTR name);
struct MsgPort *CreateMsgPort(void);
void DeleteMsgPort(struct MsgPort *port);
APTR CreateIORequest(struct MsgPort *ioReplyPort, ULONG size);
void DeleteIORequest(APTR iorequest);
LONG OpenDevice(CONST_STRPTR devName, IPTR unitNumber, struct IORequest *iORequest, ULONG flags);
void CloseDevice(struct IORequest *iORequest);
LONG DoIO(struct IORequest *iORequest);
/* Asynchronous I/O: the tests that do none link message_native_unused.c's
   poisoned definitions. */
void SendIO(struct IORequest *iORequest);
LONG AbortIO(struct IORequest *iORequest);
LONG WaitIO(struct IORequest *iORequest);
struct Message *GetMsg(struct MsgPort *port);
ULONG SetSignal(ULONG newSignals, ULONG signalSet);
ULONG Wait(ULONG signalSet);

#ifdef __cplusplus
}
#endif

#define CopyMem(source, dest, size) memcpy(dest, source, size)
static inline void RawPutChar(UBYTE ch) { (void)ch; }
