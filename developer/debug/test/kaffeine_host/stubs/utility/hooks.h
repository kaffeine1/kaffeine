#pragma once
#include <exec/types.h>

struct Hook
{
    struct { APTR mln_Succ, mln_Pred; } h_MinNode;
    APTR h_Entry, h_SubEntry, h_Data;
};
