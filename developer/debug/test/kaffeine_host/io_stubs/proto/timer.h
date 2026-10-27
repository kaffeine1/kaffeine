#pragma once
/* The native timer.device functions take the base that the caller names
   TimerBase, as the AROS proto macros do. */
#include <devices/timer.h>

#ifdef __cplusplus
extern "C" {
#endif
void stub_GetSysTime(struct Device *base, struct timeval *dest);
ULONG stub_ReadEClock(struct Device *base, struct EClockVal *dest);
#ifdef __cplusplus
}
#endif

#define GetSysTime(dest) stub_GetSysTime(TimerBase, (dest))
#define ReadEClock(dest) stub_ReadEClock(TimerBase, (dest))
