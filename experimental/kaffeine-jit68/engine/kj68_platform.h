/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    kaffeine-jit68 engine: what the engine asks of the system it runs on.
    kj68_platform.c answers for AROS and is built into the library. The host
    checks in developer/debug/test/kaffeine_jit68_host have their own file.
*/

#ifndef KJ68_PLATFORM_H
#define KJ68_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* 1 where code the translator writes can be run. A build for a host says
   so itself. Without that this is AROS: hosted AROS has another contract
   for executable memory, so only the standalone flavour runs code. */
#ifndef KJ68_PLAT_RUNS
#include <aros/config.h>
#define KJ68_PLAT_RUNS (AROS_FLAVOUR == AROS_FLAVOUR_STANDALONE)
#endif

void *kj68_plat_alloc(size_t size);             /* zeroed memory, or NULL */
void  kj68_plat_free(void *p);

/* memory that code is written to and run from, or NULL */
void *kj68_plat_code_alloc(size_t size);
void  kj68_plat_code_free(void *code, size_t size);
/* around every write into it. After write_end the len bytes at start are
   what the processor executes. */
void  kj68_plat_code_write_begin(void *code, size_t size);
void  kj68_plat_code_write_end(void *code, size_t size, void *start, size_t len);
#ifdef KJ68_TEST_SEAMS
const char *kj68_plat_code_method(void);        /* how that memory is made, for a line of the checks */
#endif

void  kj68_plat_dead_end(void);                 /* the engine found itself wrong; does not return */

#endif /* KJ68_PLATFORM_H */
