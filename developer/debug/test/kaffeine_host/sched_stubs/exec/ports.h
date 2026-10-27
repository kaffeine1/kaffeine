#ifndef KAFFEINE_TEST_SCHED_EXEC_PORTS_H
#define KAFFEINE_TEST_SCHED_EXEC_PORTS_H
#include <exec/types.h>

/* The native port fields that the guest task scheduler reads */
struct Node
{
    struct Node *ln_Succ, *ln_Pred;
    UBYTE ln_Type;
    BYTE ln_Pri;
    char *ln_Name;
};
struct List
{
    struct Node *lh_Head, *lh_Tail, *lh_TailPred;
    UBYTE lh_Type, l_pad;
};
struct MsgPort
{
    struct Node mp_Node;
    UBYTE mp_Flags;
    UBYTE mp_SigBit;
    void *mp_SigTask;
    struct List mp_MsgList;
};
#endif
