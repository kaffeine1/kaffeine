/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    kaffeine-jit68 engine: its memory, its code memory and its dead end on
    AROS (kj68_platform.h).
*/
#include <exec/alerts.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "kj68_platform.h"

void *kj68_plat_alloc(size_t size)
{
    return AllocVec(size, MEMF_ANY | MEMF_CLEAR);
}

void kj68_plat_free(void *p)
{
    FreeVec(p);
}

void *kj68_plat_code_alloc(size_t size)
{
    return AllocMem(size, MEMF_PUBLIC | MEMF_EXECUTABLE);
}

void kj68_plat_code_free(void *code, size_t size)
{
    FreeMem(code, size);
}

/* Ordinary RAM of native AROS AArch64 can be written and run. Code that
   was written still has to be published through the cache call. */
void kj68_plat_code_write_begin(void *code, size_t size)
{
    (void)code;
    (void)size;
}

void kj68_plat_code_write_end(void *code, size_t size, void *start, size_t len)
{
    (void)code;
    (void)size;
    CacheClearE((APTR)start, len, CACRF_ClearD | CACRF_ClearI);
}

/* The translator asked for a constant that has no AArch64 encoding. That
   is a defect of the translator; the host checks of its encodings are there
   to find it before the library is built. */
void kj68_plat_dead_end(void)
{
    Alert(AT_DeadEnd | AN_Unknown);
    for (;;)
        ;
}
