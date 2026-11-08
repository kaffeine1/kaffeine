/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Forced into the screens object of test_screens alone (Makefile): its
    AllocVec() goes to test_screens.cpp, which records each request and can
    fail a chosen one. The stub header comes first and is included once, so
    the definition below stays for kaffeine_screens.c; FreeVec() is still
    free(), as the collaborator allocates with calloc(). Every other object
    keeps the stub's calloc().
*/
#pragma once
#include <exec/types.h>
#include <proto/exec.h>

#ifdef __cplusplus
extern "C"
#endif
APTR Kaffeine_TestScreensAllocVec(ULONG size, ULONG flags);

#undef AllocVec
#define AllocVec(size, flags) Kaffeine_TestScreensAllocVec((size), (flags))
