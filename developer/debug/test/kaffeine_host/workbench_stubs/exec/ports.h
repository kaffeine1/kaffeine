#pragma once
#include_next <exec/ports.h>
#define PF_ACTION 7
#define PA_IGNORE 2
#define PA_CALL 3
#define NEWLIST(list) do { \
    (list)->lh_Head = (struct Node *)&(list)->lh_Tail; \
    (list)->lh_Tail = NULL; \
    (list)->lh_TailPred = (struct Node *)&(list)->lh_Head; \
} while (0)
