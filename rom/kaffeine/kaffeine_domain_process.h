/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef KAFFEINE_DOMAIN_PROCESS_H
#define KAFFEINE_DOMAIN_PROCESS_H

#include <exec/types.h>

struct KaffeineContext;
struct KaffeineDomainStartRecovery;

#ifdef __cplusplus
extern "C" {
#endif

/* One recovery pass, on ctx->exec_task with all guest execution and nested
 * native callbacks stopped. FALSE retains every pending native broker and
 * its staged guest CPU/TCB/module; keep the driver and Context alive and
 * retry. Each FALSE return yields one DOS tick before returning. TRUE means
 * all startup brokers acknowledged their owner-task cleanup. Context-owned
 * deferred module cleanup may remain.
 *
 * Call after a failed guest slice, BEFORE any scheduler boundary/end or CPU
 * destruction. A recovery may deactivate a newly-created scheduler: read
 * ctx->sched again afterwards. The sole serial coordinator must not submit
 * another RPC between the broker's metadata-only idle probe and Finish.
 * Preserves complete-width IoErr. An empty drain is idempotent.
 */
BOOL Kaffeine_DomainDrainStarts(struct KaffeineContext *ctx);

#ifdef __cplusplus
}
#endif
#endif
