#pragma once
#include <exec/types.h>
struct Node
{
    struct Node *ln_Succ, *ln_Pred;
    UBYTE ln_Type;
    BYTE ln_Pri;
    char *ln_Name;
};
#define NT_MSGPORT 4
#define NT_MESSAGE 5
#define NT_REPLYMSG 7
#define NT_FONT 12

#define NT_GRAPHICS 18
