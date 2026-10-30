/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#pragma once
#include <exec/types.h>
struct Task;
struct SignalSemaphore { struct Task *ss_Owner; WORD ss_NestCount; };
