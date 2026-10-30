#pragma once
#include_next <proto/exec.h>
#include <exec/ports.h>
#ifdef __cplusplus
extern "C" {
#endif
struct Message *GetMsg(struct MsgPort *port);
struct Message *WaitPort(struct MsgPort *port);
void DeleteMsgPort(struct MsgPort *port);
ULONG Wait(ULONG signals);
ULONG SetSignal(ULONG signals, ULONG mask);
void ReplyMsg(struct Message *message);
void RawPutChar(UBYTE ch);
#ifdef __cplusplus
}
#endif
