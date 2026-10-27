#pragma once
/* exec.library for the native argument parser and the kaffeine objects of
   the ReadArgs test. The test provides AllocVec(), FreeVec() and
   FindTask(), and counts the blocks that are still allocated. */
#include <exec/types.h>
#include <exec/memory.h>
#include <string.h>

struct Task;

#ifdef __cplusplus
extern "C" {
#endif

APTR AllocVec(ULONG size, ULONG flags);
void FreeVec(APTR memory);
struct Task *FindTask(CONST_STRPTR name);

#ifdef __cplusplus
}
#endif

#define AllocMem(size, flags) AllocVec(size, flags)
#define FreeMem(memory, size) FreeVec(memory)
#define CopyMem(source, dest, size) memmove(dest, source, size)
#define CopyMemQuick(source, dest, size) memmove(dest, source, size)
