/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    exec.library list functions on a list and nodes in 68k memory, called
    from a libnix C program that uses neither stdio nor malloc. NewList()
    is the inline amiga.lib macro, so only the exec calls go through
    kaffeine. After the first call the program prints the list header and
    the node as the 68k code sees them, and after every call it walks the
    list from the head, at most eight nodes, and compares the names.
*/

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <inline/alib.h>
#include <string.h>

#include "m68ktest.h"

static struct List list;
static struct Node *node[4];
static CONST_STRPTR names[4] = { "one", "two", "three", "four" };

static void dump(CONST_STRPTR when, struct Node *n)
{
    Printf("     %s: lh_Head 0x%08lx lh_Tail 0x%08lx lh_TailPred 0x%08lx\n",
           (LONG)when, (LONG)list.lh_Head, (LONG)list.lh_Tail, (LONG)list.lh_TailPred);
    Printf("     node 0x%08lx: ln_Succ 0x%08lx ln_Pred 0x%08lx\n",
           (LONG)n, (LONG)n->ln_Succ, (LONG)n->ln_Pred);
    test_flush();
}

/* The node names from the head to the tail, separated by commas */
static BOOL walk(CONST_STRPTR what, CONST_STRPTR expected)
{
    static char seen[96];
    struct Node *n;
    LONG count, len = 0, i;

    seen[0] = 0;
    for (n = list.lh_Head, count = 0; n && n->ln_Succ && count < 8; n = n->ln_Succ, count++)
    {
        const char *name = n->ln_Name ? n->ln_Name : "?";

        if (count)
            seen[len++] = ',';
        for (i = 0; name[i] && i < 8 && len < (LONG)sizeof(seen) - 2; i++)
            seen[len++] = name[i];
        seen[len] = 0;
    }
    if (count == 8)
        strcpy(seen + len, ",...");
    return check_str(what, seen, expected);
}

int main(void)
{
    LONG i;

    NewList(&list);
    check(IsListEmpty(&list), "NewList() gives an empty list");

    for (i = 0; i < 4; i++)
    {
        node[i] = AllocVec(sizeof(struct Node), MEMF_PUBLIC | MEMF_CLEAR);
        if (!node[i])
        {
            check(FALSE, "AllocVec() of a node");
            return test_done("exec_lists");
        }
        node[i]->ln_Name = (char *)names[i];
    }

    AddTail(&list, node[0]);
    dump("after AddTail(one)", node[0]);
    walk("after AddTail(one)", "one");
    AddTail(&list, node[1]);
    walk("after AddTail(two)", "one,two");
    AddHead(&list, node[2]);
    walk("after AddHead(three)", "three,one,two");

    check_long("RemHead() returns node three", (LONG)RemHead(&list), (LONG)node[2]);
    walk("after RemHead()", "one,two");
    check_long("RemTail() returns node two", (LONG)RemTail(&list), (LONG)node[1]);
    walk("after RemTail()", "one");

    Insert(&list, node[1], node[0]);
    walk("after Insert(two after one)", "one,two");
    Remove(node[0]);
    walk("after Remove(one)", "two");

    node[1]->ln_Pri = 0;
    node[2]->ln_Pri = 5;
    node[3]->ln_Pri = -5;
    Enqueue(&list, node[3]);
    Enqueue(&list, node[2]);
    walk("after Enqueue() of three (5) and four (-5)", "three,two,four");

    check_long("FindName(\"two\")", (LONG)FindName(&list, "two"), (LONG)node[1]);

    return test_done("exec_lists");
}
