#pragma once
#include <exec/types.h>
#include <exec/memory.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

APTR AllocMem(ULONG size, ULONG flags);
void FreeMem(APTR memory, ULONG size);

#ifdef __cplusplus
}
#endif

#define CopyMem(source, dest, size) memcpy(dest, source, size)

static inline void RawPutChar(UBYTE ch) { (void)ch; }

/* The linked Exec compatibility handlers forward only native messages, and
   nothing in these tests creates one. */
struct MsgPort;
struct Message;
static inline void PutMsg(struct MsgPort *port, struct Message *message)
{
    (void)port; (void)message; abort();
}
