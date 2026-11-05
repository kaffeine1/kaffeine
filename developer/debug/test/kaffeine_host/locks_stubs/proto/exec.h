#pragma once
#include "../../memory_stubs/proto/exec.h"
#include <exec/tasks.h>
#ifdef __cplusplus
extern "C" {
#endif
struct Task *FindTask(CONST_STRPTR name);
void Forbid(void);
void Permit(void);
#ifdef __cplusplus
}
#endif
