/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#pragma once
#define D(x)
#ifdef __cplusplus
extern "C" {
#endif
void kaffeine_dos_trace_bug(const char *format, ...);
#ifdef __cplusplus
}
#endif
#define bug kaffeine_dos_trace_bug
