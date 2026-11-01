/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
/* AllocVec() and FreeVec() as functions of test_asl.cpp, which counts and
   fails them on demand; the rest as the other stubs give it. */
#pragma once
#include_next <proto/exec.h>
#undef AllocVec
#undef FreeVec
#ifdef __cplusplus
extern "C" {
#endif
void *AllocVec(unsigned long size, unsigned long flags);
void FreeVec(void *memory);
#ifdef __cplusplus
}
#endif
