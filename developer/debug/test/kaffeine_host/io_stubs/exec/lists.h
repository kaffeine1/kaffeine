#pragma once
#include <exec/nodes.h>
/* exec/lists.h of AROS, as the listview bridge uses it */
struct List
{
    struct Node *lh_Head, *lh_Tail, *lh_TailPred;
    UBYTE lh_Type;
    UBYTE l_pad;
};
/* The tail sentinel supplies ln_Succ only; it is not a complete Node. */
#define ForeachNode(list, node) \
    for ((node) = (list)->lh_Head; (node)->ln_Succ; (node) = (node)->ln_Succ)
