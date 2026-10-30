/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef KAFFEINE_PAINT_MEMORY_H
#define KAFFEINE_PAINT_MEMORY_H
#include <exec/types.h>
struct KaffeineContext;
#ifdef __cplusplus
extern "C" {
#endif
/* Query the private heap interval containing a library base before a guest
 * Close/Expunge can destroy state required by an installed recipe. */
BOOL Kaffeine_HeapPaintReleasable(struct KaffeineContext *, ULONG interior);
#ifdef __cplusplus
}
#endif
#endif
