/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#pragma once
#include "../../../kaffeine_host/memory_stubs/proto/exec.h"
#include <exec/libraries.h>
#define SIGB_DOS 8
struct Task;
#ifdef __cplusplus
extern "C" {
#endif
struct Task *FindTask(CONST_STRPTR);
void Signal(struct Task *, ULONG);
struct Library *OpenLibrary(CONST_STRPTR, ULONG);
void CloseLibrary(struct Library *);
#ifdef __cplusplus
}
#endif
