/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    kaffeine-jit68 engine: the nine operations of the interface of
    m68kemu.library for an engine (m68kemu_engine.h and ENGINE.md in
    rom/m68kemu), with their checks in the contract's order, the guest
    memory map and the counters. The translation itself is in
    kj68_translate.c.
*/

#include <string.h>
#include "kj68_intern.h"
#include "kj68_platform.h"

/* ---- allocation ------------------------------------------------------- */

#ifdef KJ68_TEST_SEAMS
long kj68_test_fail_alloc_at, kj68_test_allocs_live, kj68_test_exec_live;

int kj_test_fail_next(void)
{
    return kj68_test_fail_alloc_at > 0 && --kj68_test_fail_alloc_at == 0;
}
#endif

void *kj_alloc(size_t size)
{
    void *p;

#ifdef KJ68_TEST_SEAMS
    if (kj_test_fail_next())
        return NULL;
#endif
    p = kj68_plat_alloc(size);
#ifdef KJ68_TEST_SEAMS
    if (p)
        kj68_test_allocs_live++;
#endif
    return p;
}

void kj_free(void *p, size_t size)
{
    (void)size;
    if (!p)
        return;
#ifdef KJ68_TEST_SEAMS
    kj68_test_allocs_live--;
#endif
    kj68_plat_free(p);
}

/* ---- counters ----------------------------------------------------------- */

void kj_count(uint64_t *counter, uint64_t n)
{
    *counter = *counter > UINT64_MAX - n ? UINT64_MAX : *counter + n;
}

static ULONG kj_sat32(uint64_t v)
{
    return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (ULONG)v;
}

#ifdef KJ68_TEST_SEAMS
void kj68_test_preset_counters(struct M68KEngDomain *dom, uint64_t counts, uint64_t retired)
{
    dom->counters.blocks = dom->counters.insns = dom->counters.runs = counts;
    dom->counters.invalidations = dom->counters.code_resets = counts;
    dom->counters.retired = retired;
}
#endif

/* ---- the map ------------------------------------------------------------- */

/* The contents rule of the contract: base, size at least 0x1000, at most
   M68KENG_MAX_RANGES ranges, each non-empty, inside memory, with known flags
   and at least one, sorted and disjoint. */
static int kj_map_valid(const struct M68KEngMap *map)
{
    ULONG i;

    if (!map->base || map->size < 0x1000 || map->num_ranges > M68KENG_MAX_RANGES)
        return 0;
    for (i = 0; i < map->num_ranges; i++)
    {
        const struct M68KEngRange *r = &map->ranges[i];

        if (r->lo >= r->hi || r->hi > map->size)
            return 0;
        if (!(r->flags & (M68KENG_RANGE_READ_HOST | M68KENG_RANGE_WRITE_HOST)) ||
            (r->flags & ~(M68KENG_RANGE_READ_HOST | M68KENG_RANGE_WRITE_HOST)))
            return 0;
        if (i && r->lo < map->ranges[i - 1].hi)
            return 0;
    }
    return 1;
}

/* Takes a valid map whose base and size are the domain's: the ranges, and
   the flags of every page a range touches. */
static void kj_map_take(struct M68KEngDomain *dom)
{
    const struct M68KEngMap *map = dom->map;
    ULONG i, p;

    dom->map_generation = map->generation;
    dom->num_ranges = map->num_ranges;
    memcpy(dom->ranges, map->ranges, sizeof(dom->ranges[0]) * map->num_ranges);
    memset(dom->pages, 0, dom->npages);
    for (i = 0; i < map->num_ranges; i++)
    {
        const struct M68KEngRange *r = &map->ranges[i];
        UBYTE flag = (UBYTE)(((r->flags & M68KENG_RANGE_READ_HOST) ? KJ68_PAGE_READ_HOST : 0) |
                             ((r->flags & M68KENG_RANGE_WRITE_HOST) ? KJ68_PAGE_WRITE_HOST : 0));

        for (p = r->lo >> KJ68_PAGE_SHIFT; p <= (r->hi - 1) >> KJ68_PAGE_SHIFT; p++)
            dom->pages[p] |= flag;
    }
    dom->pages_generation++;
}

/* ---- the operations --------------------------------------------------------- */

static const struct M68KEngOps kj_ops;

static LONG kj_domain_create(const struct M68KEngHost *host, APTR host_domain,
                             struct M68KEngMap *map, ULONG required_caps,
                             struct M68KEngDomain **domain)
{
    struct M68KEngDomain *dom;
    LONG rc;

    if (domain)
        *domain = NULL;
    if (!host || !map || !domain)
        return M68KENG_ERR_ARG;
    if (host->size_of < M68KENG_V1_SIZE_HOST)
        return M68KENG_ERR_SIZE;
    if (host->abi_major != M68KENG_ABI_MAJOR)
        return M68KENG_ERR_VERSION;
    if (!host->read || !host->write)
        return M68KENG_ERR_ARG;
    if (map->size_of < M68KENG_V1_SIZE_MAP)
        return M68KENG_ERR_SIZE;
    if (!kj_map_valid(map))
        return M68KENG_ERR_ARG;
    if (required_caps & ~kj_ops.caps)
        return M68KENG_ERR_CAPS;
    if (!kj_tr_runs())
        return M68KENG_ERR_NOHOST;

    dom = kj_alloc(sizeof(*dom));
    if (!dom)
        return M68KENG_ERR_NOMEM;
    dom->host = host;
    dom->host_domain = host_domain;
    dom->map = map;
    dom->base = map->base;
    dom->size = map->size;
    dom->npages = (ULONG)(((uint64_t)map->size + (1u << KJ68_PAGE_SHIFT) - 1) >> KJ68_PAGE_SHIFT);
    dom->pages = kj_alloc(dom->npages);
    if (!dom->pages)
    {
        kj_free(dom, sizeof(*dom));
        return M68KENG_ERR_NOMEM;
    }
    kj_map_take(dom);
    rc = kj_tr_domain_create(dom);
    if (rc != M68KENG_OK)
    {
        kj_free(dom->pages, dom->npages);
        kj_free(dom, sizeof(*dom));
        return rc;
    }
    *domain = dom;
    return M68KENG_OK;
}

static LONG kj_domain_destroy(struct M68KEngDomain *dom)
{
    if (!dom)
        return M68KENG_ERR_ARG;
    if (dom->busy || dom->cpus)
        return M68KENG_ERR_STATE;
    kj_tr_domain_destroy(dom);
    kj_free(dom->pages, dom->npages);
    kj_free(dom, sizeof(*dom));
    return M68KENG_OK;
}

/* Every translation of the domain compares its guest bytes again before
   it runs, and one whose bytes changed is dropped; the range only matters
   for its size: 0 changes nothing. A range past 0xFFFFFFFF would end at
   the top of the address space; checking everything covers it. A map
   change is not this: run() drops all translations for it. */
static LONG kj_invalidate(struct M68KEngDomain *dom, ULONG addr, ULONG size)
{
    (void)addr;
    if (!dom)
        return M68KENG_ERR_ARG;
    if (dom->busy)
        return M68KENG_ERR_STATE;
    if (size == 0)
        return M68KENG_OK;
    kj_tr_invalidate(dom);
    kj_count(&dom->counters.invalidations, 1);
    return M68KENG_OK;
}

static LONG kj_cpu_create(struct M68KEngDomain *dom, APTR host_cpu, struct M68KEngCPU **cpu)
{
    struct M68KEngCPU *c;
    LONG rc;

    if (cpu)
        *cpu = NULL;
    if (!dom || !cpu)
        return M68KENG_ERR_ARG;
    if (dom->busy)
        return M68KENG_ERR_STATE;
    c = kj_alloc(sizeof(*c));
    if (!c)
        return M68KENG_ERR_NOMEM;
    c->domain = dom;
    c->host_cpu = host_cpu;
    rc = kj_tr_cpu_create(c);
    if (rc != M68KENG_OK)
    {
        kj_free(c, sizeof(*c));
        return rc;
    }
    dom->cpus++;
    *cpu = c;
    return M68KENG_OK;
}

static LONG kj_cpu_destroy(struct M68KEngCPU *c)
{
    if (!c)
        return M68KENG_ERR_ARG;
    if (c->domain->busy)
        return M68KENG_ERR_STATE;
    kj_tr_cpu_destroy(c);
    c->domain->cpus--;
    kj_free(c, sizeof(*c));
    return M68KENG_OK;
}

static LONG kj_get_reg(struct M68KEngCPU *c, ULONG reg, ULONG *value)
{
    if (!c || !value || reg >= M68KENG_REG_COUNT)
        return M68KENG_ERR_ARG;
    if (c->domain->busy)
        return M68KENG_ERR_STATE;
    *value = kj_tr_get_reg(c, reg);
    return M68KENG_OK;
}

static LONG kj_set_reg(struct M68KEngCPU *c, ULONG reg, ULONG value)
{
    if (!c || reg >= M68KENG_REG_COUNT)
        return M68KENG_ERR_ARG;
    if (c->domain->busy)
        return M68KENG_ERR_STATE;
    return kj_tr_set_reg(c, reg, value);
}

/* Checks in the contract's order; on any error nothing runs and exit is
   untouched. A changed map is checked again: base and size must stay
   those of domain_create and the contents must be valid, or the call is
   M68KENG_ERR_ARG with nothing run (an implementation rule the contract does
   not state yet). A valid new generation drops every translation of the
   domain, so the instruction fetches of this run and of every other CPU
   of the domain follow the new map; the host needs no invalidate. */
static LONG kj_run(struct M68KEngCPU *c, ULONG budget, struct M68KEngExit *exit)
{
    struct M68KEngExit out;
    struct M68KEngDomain *dom;
    const size_t from = offsetof(struct M68KEngExit, reason);

    if (!c || !exit)
        return M68KENG_ERR_ARG;
    if (exit->size_of < M68KENG_V1_SIZE_EXIT)
        return M68KENG_ERR_SIZE;
    if (budget == 0 || budget > M68KENG_MAX_BUDGET)
        return M68KENG_ERR_ARG;
    dom = c->domain;
    if (dom->busy)
        return M68KENG_ERR_STATE;
    if (dom->map->generation != dom->map_generation)
    {
        if (dom->map->base != dom->base || dom->map->size != dom->size || !kj_map_valid(dom->map))
            return M68KENG_ERR_ARG;
        kj_map_take(dom);
        kj_tr_map_changed(dom);
    }
    memset(&out, 0, sizeof(out));
    dom->busy = 1;
    kj_tr_run(c, budget, &out);
    dom->busy = 0;
    kj_count(&dom->counters.runs, 1);
    kj_count(&dom->counters.retired, out.retired);
    /* the version 1 members only: size_of and any tail stay the caller's */
    memcpy((UBYTE *)exit + from, (const UBYTE *)&out + from, M68KENG_V1_SIZE_EXIT - from);
    return M68KENG_OK;
}

static LONG kj_get_stats(struct M68KEngDomain *dom, struct M68KEngStats *stats)
{
    struct M68KEngStats s;
    const size_t from = offsetof(struct M68KEngStats, cpus);

    if (!dom || !stats)
        return M68KENG_ERR_ARG;
    if (stats->size_of < M68KENG_V1_SIZE_STATS)
        return M68KENG_ERR_SIZE;
    if (dom->busy)
        return M68KENG_ERR_STATE;
    memset(&s, 0, sizeof(s));
    s.cpus = dom->cpus;
    s.blocks_translated = kj_sat32(dom->counters.blocks);
    s.insns_translated = kj_sat32(dom->counters.insns);
    s.runs = kj_sat32(dom->counters.runs);
    s.invalidations = kj_sat32(dom->counters.invalidations);
    s.code_resets = kj_sat32(dom->counters.code_resets);
    s.code_bytes = kj_tr_code_bytes(dom);
    s.retired = dom->counters.retired;
    memcpy((UBYTE *)stats + from, (const UBYTE *)&s + from, M68KENG_V1_SIZE_STATS - from);
    return M68KENG_OK;
}

static const struct M68KEngOps kj_ops =
{
    sizeof(struct M68KEngOps), M68KENG_ABI_MAJOR, M68KENG_ABI_MINOR, M68KENG_CAP_BASE,
    KJ68_BLOCK_INSNS, (CONST_STRPTR)"kaffeine-jit68 1.0 (AArch64 translator)",
    kj_domain_create, kj_domain_destroy, kj_invalidate, kj_cpu_create, kj_cpu_destroy,
    kj_get_reg, kj_set_reg, kj_run, kj_get_stats
};

const struct M68KEngOps *KJ68_GetEngineTable(ULONG abi_major)
{
    return abi_major == M68KENG_ABI_MAJOR ? &kj_ops : NULL;
}
