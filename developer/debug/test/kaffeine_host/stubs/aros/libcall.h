#pragma once
#include <exec/types.h>
#define AROS_LH4(...)
#define AROS_LHA(...)
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT
#ifdef __cplusplus
extern "C"
#endif
unsigned int Kaffeine_Host_KrnGetCPUCount(APTR resource);
/* Only this kernel vector has a host collaborator. */
#define AROS_LC0(type,name,basetype,base,number,library) \
    ((void)sizeof(char[(number)==40?1:-1]), (type)Kaffeine_Host_##name((APTR)(base)))
