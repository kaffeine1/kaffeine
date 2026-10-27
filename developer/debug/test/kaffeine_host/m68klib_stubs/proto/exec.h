#pragma once
#include "../../memory_stubs/proto/exec.h"

/* Only the fields touched by requester suppression. This is native host
   state, never a guest structure. */
#define NT_PROCESS 13
struct Node { UBYTE ln_Type; };
struct Task { struct Node tc_Node; };
struct Process { struct Task pr_Task; APTR pr_WindowPtr; };
#ifdef __cplusplus
extern "C"
#endif
struct Task *FindTask(CONST_STRPTR name);
