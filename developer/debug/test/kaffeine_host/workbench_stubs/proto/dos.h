#pragma once
/* The locks of AppMessages that the bridge duplicates, defined by the test. */
#include_next <proto/dos.h>
#include <dos/dos.h>
#ifdef __cplusplus
extern "C" {
#endif
BPTR DupLock(BPTR lock);
void UnLock(BPTR lock);
#ifdef __cplusplus
}
#endif
