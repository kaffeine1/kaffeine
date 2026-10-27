#pragma once
#include <exec/types.h>
struct Node
{
    struct Node *ln_Succ, *ln_Pred;
    UBYTE ln_Type;
    BYTE ln_Pri;
    char *ln_Name;
};
#define NT_TASK 1
#define NT_DEVICE 3
#define NT_MSGPORT 4
#define NT_MESSAGE 5
#define NT_REPLYMSG 7
