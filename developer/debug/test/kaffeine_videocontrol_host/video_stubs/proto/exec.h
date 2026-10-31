/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#pragma once
#include "../../../kaffeine_host/memory_stubs/proto/exec.h"
#define SIGB_DOS 8
struct Task;
#ifdef __cplusplus
extern "C" {
#endif
struct Task *FindTask(CONST_STRPTR);
void Signal(struct Task *, ULONG);
#ifdef __cplusplus
}
#endif
