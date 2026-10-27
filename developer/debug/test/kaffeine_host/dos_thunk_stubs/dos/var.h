#pragma once
/* Public flags from compiler/include/dos/var.h. */
#define LV_VAR 0
#define LV_ALIAS 1
#define GVF_GLOBAL_ONLY (1L << 8)
#define GVF_LOCAL_ONLY (1L << 9)
#define GVF_BINARY_VAR (1L << 10)
#define GVF_DONT_NULL_TERM (1L << 11)
#define GVF_SAVE_VAR (1L << 12)
