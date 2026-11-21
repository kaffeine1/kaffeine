/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Host checks of kaffeine-jit68: the host's side of kj68_platform.h, for
    macOS and Linux on AArch64. The AROS library has its own in
    workbench/libs/kaffeine-jit68/kj68_platform.c.
*/
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdlib.h>
#include <sys/mman.h>
#if defined(__APPLE__)
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#endif

#include "kj68_platform.h"

void *kj68_plat_alloc(size_t size)
{
    return calloc(1, size);
}

void kj68_plat_free(void *p)
{
    free(p);
}

void *kj68_plat_code_alloc(size_t size)
{
    void *code;

#if defined(__APPLE__)
    code = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
#else
    code = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    return code == MAP_FAILED ? NULL : code;
}

void kj68_plat_code_free(void *code, size_t size)
{
    munmap(code, size);
}

void kj68_plat_code_write_begin(void *code, size_t size)
{
#if defined(__APPLE__)
    (void)code;
    (void)size;
    pthread_jit_write_protect_np(0);
#else
    mprotect(code, size, PROT_READ | PROT_WRITE);
#endif
}

void kj68_plat_code_write_end(void *code, size_t size, void *start, size_t len)
{
#if defined(__APPLE__)
    (void)code;
    (void)size;
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(start, len);
#else
    mprotect(code, size, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)start, (char *)start + len);
#endif
}

const char *kj68_plat_code_method(void)
{
#if defined(__APPLE__)
    return "MAP_JIT";
#else
    return "mprotect";
#endif
}

void kj68_plat_dead_end(void)
{
    abort();
}
