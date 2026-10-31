/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#pragma once
#include <exec/types.h>
#ifdef __cplusplus
extern "C" {
#endif
SIPTR IoErr(void);
SIPTR SetIoErr(SIPTR);
#ifdef __cplusplus
}
#endif
