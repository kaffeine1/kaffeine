/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#pragma once
#include <exec/types.h>
#ifdef __cplusplus
extern "C"
#endif
unsigned int fixture_kernel_count(APTR);
#define AROS_LC0(type,name,basetype,base,number,library) \
    ((void)sizeof(char[(number)==40?1:-1]),(type)fixture_kernel_count((APTR)(base)))
