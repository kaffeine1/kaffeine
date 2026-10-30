#pragma once
#include "../../dos_thunk_stubs/proto/exec.h"
#include <exec/tasks.h>
#ifdef __cplusplus
extern "C" {
#endif
struct Task *FindTask(CONST_STRPTR);
#ifdef __cplusplus
}
#endif
