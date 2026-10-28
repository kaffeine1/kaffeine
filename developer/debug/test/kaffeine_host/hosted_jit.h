/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    A hosted AArch64 translation probe behind struct KaffeineCPUOps. See
    docs/kaffeine/hosted-jit-probe.md. It is not Emu68 and not a backend of
    kaffeine.library: it checks the hosting mechanics such a backend needs.
*/
#ifndef HOSTED_JIT_H
#define HOSTED_JIT_H

#include <exec/types.h>

struct KaffeineContext;
struct HJBackend;

/* Exit reasons of translated code, as counted in HJStats.exits[]. */
enum
{
    HJ_EXIT_LOOKUP = 1, /* next block not in the fast table */
    HJ_EXIT_BUDGET,     /* back edges and lookups used up for this slice */
    HJ_EXIT_LINEA,      /* A-line opcode: to the bridge dispatcher */
    HJ_EXIT_FAULT,      /* instruction outside the subset, ILLEGAL, Line-F */
    HJ_EXIT_ABORT,      /* a slow access stopped the program */
    HJ_EXIT_COUNT
};

struct HJStats
{
    unsigned long blocks;        /* blocks translated */
    unsigned long guest_insns;   /* 68k instructions in them */
    unsigned long host_words;    /* AArch64 words emitted for them */
    unsigned long translate_ns;  /* decoding, emission, copy and cache sync */
    unsigned long write_windows; /* code publication windows */
    unsigned long icache_syncs;  /* host instruction cache maintenance */
    unsigned long enters;        /* calls from C into translated code */
    unsigned long exits[HJ_EXIT_COUNT];
    unsigned long slow_accesses; /* guest accesses through the C slow path */
    unsigned long flushes;       /* flush_code() calls (CacheClearU/E, loaders) */
    unsigned long revalidated;   /* blocks kept after a flush, code unchanged */
    unsigned long resets;        /* code memory emptied */
    unsigned long max_depth;     /* deepest nesting of execute() */
};

#ifdef __cplusplus
extern "C" {
#endif

/* Nonzero on AArch64 Darwin, Linux or standalone native AROS. */
int HJ_Available(void);
/* "MAP_JIT", "mprotect", "AROS AllocMem/CacheClearE" or "none". */
const char *HJ_ExecMemoryKind(void);
/* Encoder words against words assembled by clang; 0 on success. */
int HJ_EncoderSelfTest(char *message, unsigned long size);

/* A backend for one context, with its own code memory. NULL without an
   AArch64 host or memory. */
struct HJBackend *HJ_Create(struct KaffeineContext *ctx);
void HJ_Destroy(struct HJBackend *b);
/* As Kaffeine_Execute(): entry registers, sentinel, Kaffeine_RunCPU(). */
LONG HJ_Execute(struct HJBackend *b);
const struct HJStats *HJ_GetStats(const struct HJBackend *b);
/* The last fault, "" if none. */
const char *HJ_LastFault(const struct HJBackend *b);
/* Translation only, also without an AArch64 host: 68k instructions of the
   block at pc and AArch64 words emitted, or 0 for an empty block. */
unsigned HJ_TranslateOnly(struct KaffeineContext *ctx, ULONG pc, unsigned *words);

#ifdef __cplusplus
}
#endif

#endif
