/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#pragma once
#include <exec/ports.h>

/* Only the native identity fields read by the scheduler. Guest Task memory
   keeps its separate classic layout and is never cast to this type. */
struct Task { struct Node tc_Node; };
#define NT_PROCESS 13
