/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)
*/
#ifndef KAFFEINE_TASKS_H
#define KAFFEINE_TASKS_H

#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

IPTR Kaffeine_FindTask(struct KaffeineContext *ctx, void *cpu);

#ifdef __cplusplus
}
#endif

/* Shared by the production Exec table and host dispatch tests. */
#define KAFFEINE_EXEC_TASK_THUNKS { 294, Kaffeine_FindTask }

#endif /* KAFFEINE_TASKS_H */
