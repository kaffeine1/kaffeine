#pragma once
#include <exec/types.h>

/* Native seglists need pointer-sized tokens; DOS guest arguments remain
   32-bit numeric BPTRs, including values that the guard must reject. */
typedef IPTR BPTR;
#define BADDR(x) ((void *)(IPTR)(x))
#define MKBADDR(x) ((BPTR)(IPTR)(x))
