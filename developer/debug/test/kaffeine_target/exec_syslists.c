/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Read the embedded Exec lists with the classic head/successor protocol.
    Node counts may differ between native AmigaOS and the guest view.
*/
#include <exec/execbase.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <stddef.h>
#include "m68ktest.h"

typedef char classic_list_size[sizeof(struct List) == 14 ? 1 : -1];
typedef char classic_memlist_offset[offsetof(struct ExecBase, MemList) == 322 ? 1 : -1];

struct Observation
{
    BOOL header, terminated, predecessors, tail;
    ULONG nodes;
};

static BOOL aligned(const void *p)
{
    return p != NULL && !((ULONG)p & 1);
}

static struct Observation observe(const struct List *list)
{
    struct Observation result = { FALSE, FALSE, TRUE, FALSE, 0 };
    const struct Node *node = list->lh_Head;
    const struct Node *previous = (const struct Node *)&list->lh_Head;
    ULONG remaining = 4096;

    result.header = aligned(node) && list->lh_Tail == NULL &&
                    aligned(list->lh_TailPred);
    if (!result.header)
        return result;
    while (remaining--)
    {
        const struct Node *next;

        if (!aligned(node))
            return result;
        next = node->ln_Succ;
        if (node->ln_Pred != previous)
            result.predecessors = FALSE;
        if (!next)
        {
            result.terminated = node == (const struct Node *)&list->lh_Tail;
            result.tail = list->lh_TailPred == previous;
            return result;
        }
        result.nodes++;
        previous = node;
        node = next;
    }
    return result;
}

int main(void)
{
    const struct List *lists[7];
    const char *names[7] = { "MemList", "ResourceList", "DeviceList", "IntrList",
                            "LibList", "TaskReady", "TaskWait" };
    struct Observation results[7];
    ULONG i;
    ULONG capacity = 0, chip_capacity = 0;
    BOOL memory_bounds = TRUE;

    lists[0] = &SysBase->MemList;
    lists[1] = &SysBase->ResourceList;
    lists[2] = &SysBase->DeviceList;
    lists[3] = &SysBase->IntrList;
    lists[4] = &SysBase->LibList;
    lists[5] = &SysBase->TaskReady;
    lists[6] = &SysBase->TaskWait;

    /* No allocation or DOS output while native list mutation is excluded. */
    Forbid();
    Disable();
    for (i = 0; i < 7; i++)
        results[i] = observe(lists[i]);
    if (results[0].terminated)
    {
        struct MemHeader *header = (struct MemHeader *)lists[0]->lh_Head;
        for (i = 0; i < results[0].nodes; ++i)
        {
            ULONG lower = (ULONG)header->mh_Lower, upper = (ULONG)header->mh_Upper;
            if (lower >= upper) memory_bounds = FALSE;
            else
            {
                capacity += upper - lower;
                if (header->mh_Attributes & MEMF_CHIP) chip_capacity += upper - lower;
            }
            header = (struct MemHeader *)header->mh_Node.ln_Succ;
        }
    }
    Enable();
    Permit();

    for (i = 0; i < 7; i++)
    {
        Printf("INFO %s nodes=%ld\n", (LONG)names[i], results[i].nodes);
        check(results[i].header, "embedded list header has a head, zero tail and predecessor");
        check(results[i].terminated, "bounded successor walk reaches the embedded tail sentinel");
        check(results[i].predecessors, "successor nodes retain the expected predecessor");
        check(results[i].tail, "embedded tail predecessor matches the last visited node");
    }
    Printf("INFO MemList capacity=%ld chip-accessible=%ld\n", capacity, chip_capacity);
    check(results[0].nodes && memory_bounds && capacity,
          "MemList describes installed capacity with nonempty memory bounds");
    return test_done("exec_syslists");
}
