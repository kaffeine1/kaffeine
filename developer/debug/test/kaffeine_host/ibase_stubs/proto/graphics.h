/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   The graphics stubs, plus the mode query of the IBase bridge. */
#pragma once
#include_next <proto/graphics.h>
#include <graphics/view.h>
#ifdef __cplusplus
extern "C" {
#endif
ULONG GetVPModeID(struct ViewPort *vp);
#ifdef __cplusplus
}
#endif
