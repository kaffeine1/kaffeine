#pragma once
#include <dos/bptr.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#ifdef __cplusplus
extern "C" {
#endif
BPTR DupLock(BPTR);
BOOL UnLock(BPTR);
SIPTR IoErr(void);
SIPTR SetIoErr(SIPTR);
BOOL SetVar(CONST_STRPTR, CONST_STRPTR, LONG, ULONG);
BOOL SetProgramName(CONST_STRPTR);
struct Process *CreateNewProc(const struct TagItem *);
void Delay(ULONG);
void UnLoadSeg(BPTR);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif
BPTR CurrentDir(BPTR);
BPTR Input(void);
BPTR Output(void);
#ifdef __cplusplus
}
#endif
