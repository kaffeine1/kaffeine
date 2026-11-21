/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    kaffeine-jit68 engine: the private interface between the operations of
    the interface (kj68_engine.c) and the translator (kj68_translate.c).
    Nothing here is part of the interface with m68kemu.library.
*/

#ifndef KJ68_INTERN_H
#define KJ68_INTERN_H

#include <stddef.h>
#include <stdint.h>
#include "m68kemu_engine.h"         /* the interface, in rom/m68kemu */

/* The translator's state of a domain (code memory, glue, blocks) and of a
   CPU (registers, page flags, fast table); kj68_translate.c defines them. */
struct HJBackend;
struct HJCpu;

/* The inline access check works on 4 KiB pages of guest memory. */
#define KJ68_PAGE_SHIFT      12
#define KJ68_PAGE_READ_HOST  0x01u   /* a READ_HOST range touches the page */
#define KJ68_PAGE_WRITE_HOST 0x02u   /* a WRITE_HOST range touches the page */

/* The translator's largest block, which the engine declares as
   max_block_insns. */
#define KJ68_BLOCK_INSNS 64

/* Cumulative counts, 64 bits, saturating; get_stats saturates them again
   to the widths of struct M68KEngStats. */
struct KJ68Counters
{
    uint64_t blocks, insns, runs, invalidations, code_resets, retired;
};

struct M68KEngDomain
{
    const struct M68KEngHost *host;
    APTR host_domain;
    struct M68KEngMap *map;
    /* The map as last checked. base and size are those of domain_create
       for the whole life of the domain. */
    ULONG map_generation;
    UBYTE *base;
    ULONG size;
    ULONG num_ranges;
    struct M68KEngRange ranges[M68KENG_MAX_RANGES];
    UBYTE *pages;                 /* KJ68_PAGE_* of each 4 KiB page */
    ULONG npages;
    ULONG pages_generation;       /* changes whenever pages change */
    ULONG cpus;                   /* live CPU objects */
    int busy;                     /* inside run() */
    struct KJ68Counters counters;
    struct HJBackend *code;
};

struct M68KEngCPU
{
    struct M68KEngDomain *domain;
    APTR host_cpu;
    struct HJCpu *tr;
};

#ifdef __cplusplus
extern "C" {
#endif

/* The engine's table for an ABI major, or NULL: what the library function
   KJ68_GetEngine (LVO -30) returns on AROS; host builds call it directly. */
const struct M68KEngOps *KJ68_GetEngineTable(ULONG abi_major);

/* kj68_engine.c */
void *kj_alloc(size_t size);            /* zeroed; NULL on failure */
void kj_free(void *p, size_t size);     /* NULL is ignored */
void kj_count(uint64_t *counter, uint64_t n);

/* kj68_translate.c: the translator as the engine uses it */
int kj_tr_runs(void);
#ifdef KJ68_TEST_SEAMS
const char *kj_tr_exec_memory_kind(void);       /* for a line of the host checks */
int kj_tr_encoder_self_test(char *message, unsigned long size);
#endif
LONG kj_tr_domain_create(struct M68KEngDomain *dom);   /* M68KENG_OK, ERR_NOHOST, ERR_NOMEM */
void kj_tr_domain_destroy(struct M68KEngDomain *dom);
ULONG kj_tr_code_bytes(const struct M68KEngDomain *dom);
LONG kj_tr_cpu_create(struct M68KEngCPU *cpu);          /* M68KENG_OK, ERR_NOMEM */
void kj_tr_cpu_destroy(struct M68KEngCPU *cpu);
ULONG kj_tr_get_reg(const struct M68KEngCPU *cpu, ULONG reg);
LONG kj_tr_set_reg(struct M68KEngCPU *cpu, ULONG reg, ULONG value);
void kj_tr_invalidate(struct M68KEngDomain *dom);
/* After a new map generation is taken: drops every translation of the
   domain, whatever its guest bytes. */
void kj_tr_map_changed(struct M68KEngDomain *dom);
/* Writes every version 1 member of exit but size_of, which the caller has
   zeroed; the map and the pages are current. */
void kj_tr_run(struct M68KEngCPU *cpu, ULONG budget, struct M68KEngExit *exit);

#ifdef KJ68_TEST_SEAMS
/* Host test build only: the n-th allocation from now (kj_alloc or the
   executable memory) fails, 0 for none; live counts; a counter preset. */
extern long kj68_test_fail_alloc_at;
extern long kj68_test_allocs_live;
extern long kj68_test_exec_live;
int kj_test_fail_next(void);
void kj68_test_preset_counters(struct M68KEngDomain *dom, uint64_t counts, uint64_t retired);
#endif

#ifdef __cplusplus
}
#endif

#endif /* KJ68_INTERN_H */
