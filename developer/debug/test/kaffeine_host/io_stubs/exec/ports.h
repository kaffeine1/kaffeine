#pragma once
#include <exec/nodes.h>
#include <exec/tasks.h>
struct MsgPort
{
    struct Node mp_Node;
    UBYTE mp_Flags;
    UBYTE mp_SigBit;
    struct Task *mp_SigTask;
};
struct Message
{
    struct Node mn_Node;
    struct MsgPort *mn_ReplyPort;
    UWORD mn_Length;
};
#define PA_SIGNAL 0
