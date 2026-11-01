/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
/* The part of a native Task that the ASL gate reads: the type in its
   node, a DOS Process or a plain Task. */
#pragma once
#include <exec/nodes.h>
struct Task
{
    struct Node tc_Node;
};
#ifndef NT_TASK
#define NT_TASK 1
#endif
#ifndef NT_PROCESS
#define NT_PROCESS 13
#endif
