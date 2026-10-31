/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#pragma once
#include <graphics/gfx.h>
/* Native bitmap identity from the public AROS HIDD graphics header. */
#define HIDD_BM_PAD_MAGIC 0x6148
#define IS_HIDD_BM(bitmap) ((bitmap)->pad == HIDD_BM_PAD_MAGIC)
