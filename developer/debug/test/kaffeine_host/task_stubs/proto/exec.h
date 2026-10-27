#ifndef KAFFEINE_TEST_NATIVE_PROTO_EXEC_H
#define KAFFEINE_TEST_NATIVE_PROTO_EXEC_H
#include <stdlib.h>
#include <string.h>
#include <exec/tasks.h>
#include <exec/memory.h>
#include <utility/tagitem.h>
#define AllocMem(size, flags) calloc(1, size)
#define FreeMem(ptr, size) free(ptr)
#define AllocVec(size, flags) calloc(1, size)
#define FreeVec(ptr) free(ptr)
#define CopyMem(src, dst, size) memcpy(dst, src, size)
#ifdef __cplusplus
extern "C" {
#endif
struct Task *FindTask(CONST_STRPTR name);
#ifdef __cplusplus
}
#endif
#endif
