/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef KAFFEINE_DOS_BROKER_H
#define KAFFEINE_DOS_BROKER_H

#include <exec/types.h>
#include <dos/bptr.h>

struct Task;
struct KaffeineContext;
struct KaffeineDosBroker;

#define KAFFEINE_DOS_BROKER_MAX_NAME 255
#define KAFFEINE_DOS_BROKER_MAX_ARGS 4095
#define KAFFEINE_DOS_BROKER_MAX_STACK (256 * 1024)

/* This has the same signature as a compiled library thunk. Neither the
 * callback nor any other native pointer in this interface comes from guest
 * memory. The scheduler selects the callback from its private thunk tables. */
typedef IPTR (*KaffeineDosBrokerThunk)(struct KaffeineContext *, void *);

struct KaffeineDosBrokerStart
{
    const char *name, *command, *arguments, *backend;
    BPTR home_dir;
    ULONG stack_size;
    BOOL cli, copy_vars;
    BYTE priority;
};

#ifdef __cplusplus
extern "C" {
#endif

/* driver is the stable native Exec task of the domain, not the caller's
 * FindTask(NULL) when a parent broker creates another guest Process. Start
 * copies all strings, duplicates home_dir for native DOS, and installs an
 * ARM-only NP_Entry with private CLI/streams/variables. On success it consumes
 * the ORIGINAL home_dir reference; failure leaves it with the caller. Native
 * DOS owns only the distinct duplicate and its inherited current directory.
 * A NULL home_dir omits the native tag and inherits the native caller's home.
 * No module or guest code is executed by this native process.
 *
 * Every API preserves the caller's complete-width IoErr. Startup/transport
 * errors are returned through error, never installed in the driver Process.
 * Successful calls use zero there. No Context is retained after a Call.
 */
struct KaffeineDosBroker *Kaffeine_DosBrokerStart(
    struct Task *driver, const struct KaffeineDosBrokerStart *spec,
    SIPTR *error);

/* Normally only the driver may Call/Stop, serially. For lvo zero only, a
 * parent broker with ctx->broker_call and ctx->broker_owner==FindTask(NULL)
 * may coordinate a child's private helper while the driver waits for that
 * parent. Broker's driver must always equal ctx->exec_task. The prior parent
 * broker_owner is restored after that nested reply. While Call is outstanding
 * the driver MUST NOT execute or change this ctx/cpu, switch guest ThisTask, or
 * allow a guest callback to reenter its CPU on the native worker. The worker
 * runs exactly thunk(ctx,cpu), then clears/runs ctx->after_thunk on that same
 * native Process. Its own DOS IoErr persists between calls. ctx->exec_task is
 * never changed: Exec I/O and native-backed signals still belong to driver.
 * Only for the callback and its cleanup, ctx->broker_owner is the worker's
 * native Task; it is cleared before reply. The scheduler keeps broker_call
 * TRUE throughout the RPC and prevents guest reentry on this worker.
 * lvo is private diagnostic metadata; zero is allowed for a compiled helper
 * selected by the scheduler before the first guest instruction.
 *
 * A FALSE transport result is a refusal, never permission to invoke thunk on
 * the root Process. Stop/native Exit must be intercepted by the scheduler;
 * a compiled callback must return. Native DOS waits are synchronous and may
 * pause the whole guest driver. This is not a general asynchronous IPC API.
 */
BOOL Kaffeine_DosBrokerCall(struct KaffeineDosBroker *broker,
    struct KaffeineContext *ctx, void *cpu, ULONG lvo,
    KaffeineDosBrokerThunk thunk, IPTR *result, SIPTR *error);

/* Success consumes the broker handle. It acknowledges only after deleting
 * the worker inbox and balancing the worker's private library reference.
 * The caller must not destroy its domain while a Call is outstanding. */
BOOL Kaffeine_DosBrokerStop(struct KaffeineDosBroker *broker);

/* Like Stop, first execute the trusted cleanup(ctx,NULL) and any after_thunk
 * in this worker, before retiring its inbox or allowing native DOS to close
 * streams/directories. The scheduler must install this Process's guest view,
 * hold broker_call TRUE, and keep ctx alive through the acknowledgement.
 * ctx and cleanup must either both be non-NULL or both NULL. No ctx is kept
 * after acknowledgement.
 * A parent broker may coordinate cleanup with the same private capability
 * check as an LVO-zero Call, restoring its owner after the child reply.
 * FALSE never consumes broker and must not be treated
 * as successful cleanup: keep the domain alive until outstanding work is
 * known to have finished. Once this request is submitted, Finish cannot
 * return before its cleanup reply. Original Stop is Finish(broker,NULL,NULL).
 */
BOOL Kaffeine_DosBrokerFinish(struct KaffeineDosBroker *broker,
    struct KaffeineContext *ctx, KaffeineDosBrokerThunk cleanup);

/* Metadata-only probe on an OWNED, unconsumed handle. No Context, native DOS
 * state or IoErr is inspected or changed. FALSE means do not install a DOS
 * view or release any recovery resource; keep the driver/context alive and
 * retry later. Recovery uses this only in the sole driver after guest stop,
 * with every nested coordinator returned and no independent submitters.
 * Those serial invariants make the following DosScope+Finish safe. This is
 * not a claim/reservation and does not support arbitrary concurrent callers.
 */
BOOL Kaffeine_DosBrokerIsIdle(struct KaffeineDosBroker *broker);

#ifdef __cplusplus
}
#endif
#endif
