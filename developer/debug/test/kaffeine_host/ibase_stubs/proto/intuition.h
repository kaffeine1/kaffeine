/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   The graphics stubs' Intuition, plus the depth call of the IBase bridge. */
#pragma once
#include_next <proto/intuition.h>
#ifdef __cplusplus
extern "C" {
#endif
void ScreenToFront(struct Screen *screen);
#ifdef __cplusplus
}
#endif
