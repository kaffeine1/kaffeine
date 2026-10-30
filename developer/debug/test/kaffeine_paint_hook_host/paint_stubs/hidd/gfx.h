/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#pragma once
#include <graphics/gfx.h>
#define HIDD_BM_PAD_MAGIC 0x6148
#define IS_HIDD_BM(bm) ((bm)->pad == HIDD_BM_PAD_MAGIC)
