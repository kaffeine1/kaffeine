#pragma once
#include <exec/types.h>
#include "../../stubs/proto/exec.h"

/* Count the gadget bridge's copies, and fail a chosen allocation. */
#undef AllocVec
#undef FreeVec
#undef Forbid
#undef Permit
#ifdef __cplusplus
extern "C" {
#endif
APTR AllocVec(ULONG size, ULONG flags);
void FreeVec(APTR memory);
/* Observable only in the gadget fixture and its counted bridge objects. */
void kaffeine_gadget_test_forbid(void);
void kaffeine_gadget_test_permit(void);
#ifdef __cplusplus
}
#endif
#define Forbid() kaffeine_gadget_test_forbid()
#define Permit() kaffeine_gadget_test_permit()
