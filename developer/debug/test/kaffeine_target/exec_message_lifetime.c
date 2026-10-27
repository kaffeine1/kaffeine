/*
 * Copyright (C) 2026, The AROS Development Team.
 *
 * Original public-API regression: a manual port drained through list
 * operations may be freed, and its allocation reused for ordinary bytes.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/nodes.h>
#include <proto/exec.h>
#include <string.h>
#include "m68ktest.h"

#ifndef NewList
VOID NewList(struct List *list);
#endif

struct Payload
{
    struct Message msg;
    ULONG guard1, value, guard2;
};

static void init_port(struct MsgPort *port)
{
    memset(port, 0, sizeof(*port));
    port->mp_Node.ln_Type = NT_MSGPORT;
    port->mp_Flags = PA_IGNORE;
    NewList(&port->mp_MsgList);
}

static void init_message(struct Payload *payload)
{
    memset(payload, 0, sizeof(*payload));
    payload->msg.mn_Length = sizeof(*payload);
    payload->guard1 = 0x13579BDFUL;
    payload->value = 0x2468ACE0UL;
    payload->guard2 = 0xABCDEF01UL;
}

static BOOL empty(const struct MsgPort *port)
{
    const struct List *list = &port->mp_MsgList;
    return list->lh_Head == (struct Node *)&list->lh_Tail &&
           list->lh_Tail == NULL &&
           list->lh_TailPred == (struct Node *)&list->lh_Head;
}

static void lifetime_round(LONG method)
{
    UBYTE *storage, *reused;
    struct MsgPort *port, dest;
    struct Payload first, second;
    struct Node *removed;
    ULONG address, i;
    BOOL intact = TRUE;

    storage = (method & 1) ? AllocMem(256, MEMF_PUBLIC | MEMF_CLEAR)
                           : AllocVec(256, MEMF_PUBLIC | MEMF_CLEAR);
    if (!check(storage != NULL, "allocate containing storage for a manual port"))
        return;
    port = (struct MsgPort *)(storage + 16);
    init_port(port);
    init_port(&dest);
    init_message(&first);
    PutMsg(port, &first.msg);
    check(WaitPort(port) == &first.msg, "WaitPort observes the pending manual-port message");

    Disable();
    if (method == 0)
        removed = RemHead(&port->mp_MsgList);
    else if (method == 1)
        removed = RemTail(&port->mp_MsgList);
    else
    {
        removed = &first.msg.mn_Node;
        if (method == 2)
            Remove(removed);
        else
        {
            /* The same public Node links, updated inline by guest code. */
            removed->ln_Pred->ln_Succ = removed->ln_Succ;
            removed->ln_Succ->ln_Pred = removed->ln_Pred;
        }
    }
    Enable();
    check(removed == &first.msg.mn_Node && empty(port),
          "public list removal drains the manual port to canonical empty links");

    /* Reacquire an aligned interior range before touching the freed bytes.
       Forbid prevents another task from claiming the range in between. */
    address = ((ULONG)storage + 7) & ~7UL;
    Forbid();
    if (method & 1)
        FreeMem(storage, 256);
    else
        FreeVec(storage);
    reused = AllocAbs(248, (APTR)address);
    Permit();
    if (!check(reused == (UBYTE *)address,
               "AllocAbs reacquires the released port bytes before reuse"))
    {
        if (reused) FreeMem(reused, 248);
        return;
    }
    memset(reused, 0xD4, 248);
    init_message(&second);
    PutMsg(&dest, &second.msg);
    check(GetMsg(&dest) == &second.msg && empty(&dest),
          "unrelated message roundtrip succeeds after the old port storage is reused");
    check(second.guard1 == 0x13579BDFUL && second.value == 0x2468ACE0UL &&
          second.guard2 == 0xABCDEF01UL,
          "message payload and both guards survive the roundtrip");
    for (i = 0; i < 248; ++i)
        if (reused[i] != 0xD4) intact = FALSE;
    check(intact, "message operations preserve every byte of the reused allocation");
    FreeMem(reused, 248);
}

int main(void)
{
    LONG method;
    for (method = 0; method < 4; ++method)
        lifetime_round(method);
    return test_done("exec_message_lifetime");
}
