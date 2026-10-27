#pragma once
#include <exec/types.h>
#include <exec/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

APTR AllocVec(ULONG byteSize, ULONG requirements);
APTR AllocMem(ULONG byteSize, ULONG requirements);
void FreeMem(APTR memory, ULONG size);
void FreeVec(APTR memory);
void RawPutChar(UBYTE character);

#ifdef __cplusplus
}
#endif
