#pragma once
#include <exec/types.h>
#include "../../stubs/proto/exec.h"

/* Count the menu bridge's native blocks, and fail a chosen allocation. */
#undef AllocVec
#undef FreeVec
#ifdef __cplusplus
extern "C" {
#endif
APTR AllocVec(ULONG size, ULONG flags);
void FreeVec(APTR memory);
#ifdef __cplusplus
}
#endif
