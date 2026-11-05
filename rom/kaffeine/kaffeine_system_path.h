/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#ifndef KAFFEINE_SYSTEM_PATH_H
#define KAFFEINE_SYSTEM_PATH_H
#include "kaffeine_intern.h"
struct TagItem;
#ifdef __cplusplus
extern "C" {
#endif
LONG Kaffeine_SystemPathRun(struct KaffeineContext *, const char *, struct TagItem *, SIPTR *);
#ifdef __cplusplus
}
#endif
#endif
