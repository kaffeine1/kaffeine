/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#pragma once
#include <exec/nodes.h>
struct Task { struct Node tc_Node; UBYTE snapshot[144]; };
#define NT_PROCESS 13
#define NT_SIGNALSEM 15
#define SIGB_DOS 8
#define SIGF_DOS (1UL << SIGB_DOS)
