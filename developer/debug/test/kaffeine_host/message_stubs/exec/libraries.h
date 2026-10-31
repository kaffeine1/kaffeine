/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#pragma once
#include <exec/tasks.h>
struct Library
{
    struct Node lib_Node;
    UBYTE lib_Flags, lib_pad;
    UWORD lib_NegSize, lib_PosSize, lib_Version, lib_Revision;
    char *lib_IdString;
    ULONG lib_Sum;
    UWORD lib_OpenCnt;
};
