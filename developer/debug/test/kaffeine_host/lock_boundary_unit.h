/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Numeric collaborators for legacy DOS/Workbench unit fixtures. These
   fixtures exercise wrapper arguments, not lock identity or ownership.
   test_locks links the production module instead, with native lock records. */
#include "kaffeine_locks.h"
#define LOCK_UNIT __attribute__((weak))
#ifndef KAFFEINE_LOCK_UNIT_RECORD_PUBLICATION
LOCK_UNIT ULONG Kaffeine_LockAdopt(struct KaffeineContext *c, BPTR b)
{ (void)c; return (ULONG)(IPTR)b; }
LOCK_UNIT ULONG Kaffeine_LockBorrow(struct KaffeineContext *c, BPTR b, enum KaffeineLockScope s, ULONG o)
{ (void)c; (void)s; (void)o; return (ULONG)(IPTR)b; }
#endif
LOCK_UNIT void Kaffeine_LockScopeDrop(struct KaffeineContext *c, enum KaffeineLockScope s, ULONG o)
{ (void)c; (void)s; (void)o; }
LOCK_UNIT BOOL Kaffeine_LockResolve(struct KaffeineContext *c, ULONG b, BPTR *n)
{ (void)c; *n=(BPTR)(IPTR)b; return TRUE; }
LOCK_UNIT BOOL Kaffeine_LockProtected(struct KaffeineContext *c, ULONG a, ULONG n)
{ (void)c; (void)a; (void)n; return FALSE; }
LOCK_UNIT BOOL Kaffeine_LockGetInstalled(struct KaffeineContext *c, enum KaffeineLockInstalled s, ULONG *b)
{ (void)c; (void)s; *b=0; return TRUE; }
LOCK_UNIT BOOL Kaffeine_LockTransferBegin(struct KaffeineContext *c, ULONG b, struct KaffeineLockTransfer *l)
{ (void)c; l->native=(BPTR)(IPTR)b; l->token=b; l->active=TRUE; return TRUE; }
LOCK_UNIT void Kaffeine_LockTransferEnd(struct KaffeineContext *c, struct KaffeineLockTransfer *l, BOOL consumed)
{ (void)c; (void)consumed; l->active=FALSE; }
LOCK_UNIT void Kaffeine_LockCleanup(struct KaffeineContext *c) { (void)c; }
#undef LOCK_UNIT

__attribute__((weak)) BOOL Kaffeine_LockRefreshAccess(struct KaffeineContext *c, ULONG t)
{ (void)c; (void)t; return TRUE; }

__attribute__((weak)) BOOL Kaffeine_LockForHandler(struct KaffeineContext *c, ULONG t, struct MsgPort *p, BPTR *out)
{ (void)p; return Kaffeine_LockResolve(c,t,out); }
