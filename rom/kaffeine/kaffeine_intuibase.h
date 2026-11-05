/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#ifndef KAFFEINE_INTUIBASE_H
#define KAFFEINE_INTUIBASE_H
#include "kaffeine_thunks.h"
struct Screen;
struct BitMap;
struct RastPort;
#ifdef __cplusplus
extern "C" {
#endif
IPTR Kaffeine_LockIBase(struct KaffeineContext *, void *);
IPTR Kaffeine_UnlockIBase(struct KaffeineContext *, void *);
void Kaffeine_IBaseCleanup(struct KaffeineContext *);
void Kaffeine_IBaseTaskRemoved(struct KaffeineContext *, ULONG);
BOOL Kaffeine_IBaseLocked(struct KaffeineContext *);
struct BitMap *Kaffeine_IBaseBitmap(struct KaffeineContext *, ULONG);
struct RastPort *Kaffeine_IBaseRastPort(struct KaffeineContext *, ULONG);
IPTR Kaffeine_IBaseScreenToFront(struct KaffeineContext *, void *);
IPTR Kaffeine_IBaseVPMode(struct KaffeineContext *, void *);
/* Synchronous input must not hold IBase. Borrowed screens require a real
   public visitor lease. Drop always balances a successful hold. */
BOOL Kaffeine_IBaseScreenHold(struct KaffeineContext *, ULONG, struct Screen **);
void Kaffeine_IBaseScreenDrop(struct Screen *);
IPTR Kaffeine_GetCyberMapAttr(struct KaffeineContext *, void *);
IPTR Kaffeine_ReadPixelArray(struct KaffeineContext *, void *);
#ifdef __cplusplus
}
#endif
#endif
