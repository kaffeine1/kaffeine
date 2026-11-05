#pragma once
#include <dos/dosextens.h>
#ifdef __cplusplus
extern "C" {
#endif
SIPTR IoErr(void);
SIPTR SetIoErr(SIPTR error);
void UnLock(BPTR lock);
BPTR DupLock(BPTR lock);
LONG SameLock(BPTR first, BPTR second);
struct DosList *LockDosList(ULONG flags);
struct DosList *AttemptLockDosList(ULONG flags);
void UnLockDosList(ULONG flags);
void Delay(ULONG ticks);
BPTR CurrentDir(BPTR lock);
BPTR GetProgramDir(void);
BPTR SetProgramDir(BPTR lock);
#ifdef __cplusplus
}
#endif
