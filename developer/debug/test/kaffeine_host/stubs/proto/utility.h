#pragma once
#include <exec/types.h>
#include <utility/date.h>

#ifdef __cplusplus
extern "C" {
#endif

void Amiga2Date(ULONG seconds, struct ClockData *result);
ULONG Date2Amiga(struct ClockData *date);
ULONG CheckDate(struct ClockData *date);

#ifdef __cplusplus
}
#endif
