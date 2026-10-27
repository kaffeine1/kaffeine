/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Poison collaborators for older unit tests that do not send guest signals.
   Tests exercising signals supply strong recording definitions instead. */
#include <stdlib.h>
struct Task;
__attribute__((weak)) void Signal(struct Task *task, unsigned int bits)
{ (void)task; (void)bits; abort(); }
__attribute__((weak)) struct Task *FindTask(const char *name)
{ (void)name; abort(); }
