/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef KAFFEINE_DOMAIN_RUN_H
#define KAFFEINE_DOMAIN_RUN_H

#include <exec/types.h>
struct KaffeineContext;

#ifdef __cplusplus
extern "C" {
#endif

/* Transfer the complete domain to a controlled ARM driver. A TRUE result
 * means that the driver owns ctx, including its final disposal; the caller
 * must never inspect or destroy it again. The result is the root program's
 * result. Independent guest Processes may continue on the driver afterwards.
 * FALSE leaves ctx with the caller and reports the native error in IoErr().
 * Both callbacks are compiled library functions, never guest addresses.
 */
BOOL Kaffeine_RunDomain(struct KaffeineContext *ctx, LONG (*run)(struct KaffeineContext *),
                       void (*dispose)(struct KaffeineContext *), LONG *result);

#ifdef __cplusplus
}
#endif

#endif
