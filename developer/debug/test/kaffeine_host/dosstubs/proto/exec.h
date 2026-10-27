#pragma once
#include <stdlib.h>
#include <string.h>
#define CopyMem(src, dst, n) memcpy(dst, src, n)
#define AllocVec(size, flags) calloc(1, size)
#define FreeVec(ptr) free(ptr)
extern struct Process host_process;
#define FindTask(name) ((void *)&host_process)
