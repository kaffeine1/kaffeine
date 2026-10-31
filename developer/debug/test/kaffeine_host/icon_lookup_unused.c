/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Legacy scheduler fixtures never use native icon IO. Strong production
   icon lookup wins in the dedicated adapter/broker fixture. */
#include <stdlib.h>
#include "kaffeine_thunks.h"

__attribute__((weak)) KaffeineThunkFn Kaffeine_IconIOThunk(ULONG lvo)
{
    (void)lvo;
    abort();
}
