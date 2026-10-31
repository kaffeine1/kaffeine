/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#ifndef KAFFEINE_DOMAIN_MODULES_H
#define KAFFEINE_DOMAIN_MODULES_H

#include <exec/types.h>
#include <dos/bptr.h>

struct KaffeineContext;
struct HunkRelocTable;
struct KaffeineDomainModule;

#define KAFFEINE_DOMAIN_MAX_MODULES 16
#define KAFFEINE_DOMAIN_MAX_HUNKS 1000
#define KAFFEINE_DOMAIN_MAX_RELOCS (1024 * 1024)

struct KaffeineDomainImage
{
    ULONG entry;   /* first hunk's data, in the existing guest arena */
    ULONG seglist; /* classic BPTR to [next][data], preceded by [ULONG size] */
    struct KaffeineDomainModule *module; /* native private owner, NEVER guest */
};

#ifdef __cplusplus
extern "C" {
#endif

/* The owner of ctx calls this synchronously between guest instructions.
 * Native seglist + relocation storage must come from a trusted live DOS loader
 * and remain immutable/live for this call. This borrows them, never UnLoadSegs
 * them: after success the caller may release that native source independently.
 * Counts, chain length, allocation sizes and relocation shape are bounded.
 *
 * Success publishes independent heap-owned code/data and changes only allocator,
 * module ownership and code-cache state. Entry/stack/args/registers of the running
 * root stay intact. Failure restores capacity/ownership and leaves *out intact.
 * IoErr is preserved on success; failure sets BAD_HUNK, NO_FREE_STORE or WRONG_TYPE.
 */
BOOL Kaffeine_DomainLoad(struct KaffeineContext *ctx, BPTR native,
                        const struct HunkRelocTable *relocs,
                        struct KaffeineDomainImage *out);

/* Only after all CPUs/callbacks executing this module have retired and the
 * task's I/O has been quarantined. Ownership is checked without dereferencing
 * the supplied opaque pointer or any guest seglist/size header. A FALSE/busy
 * result keeps the remaining blocks/record owned by ctx for final cleanup;
 * do not resume the module once unloading begins. No native source is freed.
 * An opaque module pointer expires after successful Unload.
 */
BOOL Kaffeine_DomainUnload(struct KaffeineContext *ctx,
                          struct KaffeineDomainModule *module);

/* Final owner-task cleanup, after scheduler/I/O teardown and before arena
 * destruction. Rejects new loads and retries context-owned deferred modules.
 * A blocked module remains recorded; do not destroy a context with live I/O.
 * The separate domain_destroy hook frees residual native bookkeeping only
 * during quiescent whole-arena destruction (e.g. stale guest queue references).
 */
void Kaffeine_DomainCleanup(struct KaffeineContext *ctx);

#ifdef __cplusplus
}
#endif
#endif
