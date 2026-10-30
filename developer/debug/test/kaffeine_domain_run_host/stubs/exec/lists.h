#pragma once
#include <exec/nodes.h>
struct List { struct Node *lh_Head, *lh_Tail, *lh_TailPred; UBYTE lh_Type, pad; };
#define NEWLIST(l) do { (l)->lh_Head=(struct Node*)&(l)->lh_Tail; (l)->lh_Tail=0; (l)->lh_TailPred=(struct Node*)&(l)->lh_Head; } while (0)
