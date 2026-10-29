#pragma once
#include <exec/types.h>
struct Node { struct Node *ln_Succ, *ln_Pred; UBYTE ln_Type; BYTE ln_Pri; char *ln_Name; };
#define NT_PROCESS 13
