/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#pragma once
#include <exec/types.h>
#include <exec/nodes.h>
/* Public 64-bit native Library prefix, as in exec/libraries.h. */
struct Library
{
    struct Node lib_Node;
    UBYTE lib_Flags, lib_pad;
    UWORD lib_NegSize, lib_PosSize, lib_Version, lib_Revision;
    APTR lib_IdString;
    ULONG lib_Sum;
    UWORD lib_OpenCnt;
};
