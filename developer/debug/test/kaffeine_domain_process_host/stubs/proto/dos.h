/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#pragma once
#include <exec/types.h>
#include <dos/bptr.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#define GSLI_68KRelocs ((IPTR)0x80000001u)
#ifdef __cplusplus
extern "C" {
#endif
SIPTR IoErr(void);
SIPTR SetIoErr(SIPTR);
BOOL GetSegListInfo(BPTR, const struct TagItem *);
BPTR DupLock(BPTR);
BOOL UnLock(BPTR);
void UnLoadSeg(BPTR);
BPTR Input(void);
BPTR Output(void);
void Delay(ULONG);
#ifdef __cplusplus
}
#endif
