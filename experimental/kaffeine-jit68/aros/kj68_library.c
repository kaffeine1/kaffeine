/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    kaffeine-jit68.library: the one function of the library.
*/
#include <aros/libcall.h>
#include <exec/types.h>
#include <proto/exec.h>

#include "kj68_intern.h"

/* LVO -30, the first function after the standard vectors: the engine's
   table of operations for an interface major, or NULL. */
AROS_LH1(APTR, KJ68_GetEngine,
         AROS_LHA(ULONG, abiMajor, D0),
         struct Library *, KJ68Base, 5, KJ68)
{
    AROS_LIBFUNC_INIT

    return (APTR)KJ68_GetEngineTable(abiMajor);

    AROS_LIBFUNC_EXIT
}
