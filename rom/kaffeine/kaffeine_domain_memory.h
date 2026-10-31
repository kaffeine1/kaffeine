/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Author: Michele Dipace (@kaffeine1)
 * Private allocator/copy interface. Descriptors are native storage, not guest.
 */
#ifndef KAFFEINE_DOMAIN_MEMORY_H
#define KAFFEINE_DOMAIN_MEMORY_H
#include <exec/types.h>
#include <dos/bptr.h>
struct KaffeineContext;
struct HunkRelocTable;
struct KaffeineDomainBlock { ULONG base, size; };
#ifdef __cplusplus
extern "C" {
#endif
ULONG Kaffeine_LoadDomainHunks(struct KaffeineContext *, BPTR,
                              const struct HunkRelocTable *,
                              struct KaffeineDomainBlock *, ULONG capacity);
BOOL Kaffeine_ReleaseDomainHunks(struct KaffeineContext *,
                                struct KaffeineDomainBlock *, ULONG count);
/* Fresh, unpublished allocations only: never use this after DomainLoad succeeds. */
BOOL Kaffeine_AbortDomainHunks(struct KaffeineContext *,
                              struct KaffeineDomainBlock *, ULONG count);
#ifdef __cplusplus
}
#endif
#endif
