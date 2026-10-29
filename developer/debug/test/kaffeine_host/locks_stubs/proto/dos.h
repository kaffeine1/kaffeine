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
void UnLockDosList(ULONG flags);
BPTR CurrentDir(BPTR lock);
BPTR GetProgramDir(void);
BPTR SetProgramDir(BPTR lock);
#ifdef __cplusplus
}
#endif
