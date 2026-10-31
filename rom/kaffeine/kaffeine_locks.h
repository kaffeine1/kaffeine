/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#ifndef KAFFEINE_LOCKS_H
#define KAFFEINE_LOCKS_H

#include "kaffeine_intern.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Active references are reusable; published BPTR addresses are quarantined. */
#define KAFFEINE_LOCK_LIMIT 64
#define KAFFEINE_LOCK_VOLUME_LIMIT 32
#define KAFFEINE_LOCK_BORROW_LIMIT 128
#define KAFFEINE_LOCK_IDENTITY_LIMIT 4096
#define KAFFEINE_LOCK_OWNER_LIMIT 17

struct Task;

enum KaffeineLockScope
{
    KAFFEINE_LOCK_DEVPROC = 1,
    KAFFEINE_LOCK_MATCH,
    KAFFEINE_LOCK_WBARG,
    KAFFEINE_LOCK_PROCESS,
    KAFFEINE_LOCK_LEGACY
};

enum KaffeineLockInstalled
{
    KAFFEINE_LOCK_CURRENT = 0,
    KAFFEINE_LOCK_PROGRAM = 1
};

/* A synchronous consuming native call owns this reservation until End().
   Keep the lease private to native code and do not copy or reuse it. */
struct KaffeineLockTransfer
{
    BPTR native;
    ULONG token;
    unsigned slot;
    BOOL active;
};

/* Adopt consumes one acquired reference, also when publication fails.
   Borrow never unlocks its input; owner is a nonzero guest owner identity.
   Successful metadata operations preserve native IoErr(). */
ULONG Kaffeine_LockAdopt(struct KaffeineContext *, BPTR native);
ULONG Kaffeine_LockBorrow(struct KaffeineContext *, BPTR native,
                          enum KaffeineLockScope scope, ULONG owner);
void Kaffeine_LockScopeDrop(struct KaffeineContext *, enum KaffeineLockScope, ULONG owner);
BOOL Kaffeine_LockResolve(struct KaffeineContext *, ULONG token, BPTR *native);
BOOL Kaffeine_LockForHandler(struct KaffeineContext *, ULONG token, struct MsgPort *, BPTR *native);
BOOL Kaffeine_LockRelease(struct KaffeineContext *, ULONG token);
/* After a successful native CHANGE_LOCK operation, update only fl_Access.
   Other guest edits are rejected before reading the trusted native field. */
BOOL Kaffeine_LockRefreshAccess(struct KaffeineContext *, ULONG token);

/* Only caller-owned references can be consumed; zero represents BNULL.
   Begin reserves the reference; End(FALSE) restores it without allocation.
   End(TRUE) retires that reference without calling UnLock(). */
BOOL Kaffeine_LockTransferBegin(struct KaffeineContext *, ULONG token,
                                 struct KaffeineLockTransfer *);
void Kaffeine_LockTransferEnd(struct KaffeineContext *, struct KaffeineLockTransfer *, BOOL consumed);

/* Get reads the actual native process field, including its initial lock.
   If native code replaced it, the obsolete installed reference is retired
   before publication; failure leaves the new native process field intact.
   Swap reserves/publishes both identities before changing the native field.
   A scoped borrowed input is duplicated first; the installed token may then
   differ from the argument, so use Get to publish the actual process field.
   The displaced process reference becomes caller-owned. A same-slot no-op
   returns its installed token without inventing a caller-owned reference;
   any existing caller ref remains, exchanging with the displaced reference. */
BOOL Kaffeine_LockGetInstalled(struct KaffeineContext *, enum KaffeineLockInstalled, ULONG *token);
void Kaffeine_LockOwnerDrop(struct KaffeineContext *, struct Task *);
BOOL Kaffeine_LockSwapInstalled(struct KaffeineContext *, enum KaffeineLockInstalled,
                                 ULONG token, ULONG *previous);

/* TRUE includes retired views and unused bytes of their private slabs.
   Do not call GuestBytes() from this module: callers may use this predicate
   inside GuestBytes()/ordinary-storage checks. */
BOOL Kaffeine_LockProtected(struct KaffeineContext *, ULONG address, ULONG size);

/* End transfer leases first. Installed and scoped owners keep their native
   references; only caller-owned references are unlocked here. */
void Kaffeine_LockCleanup(struct KaffeineContext *);

#ifdef __cplusplus
}
#endif
#endif
