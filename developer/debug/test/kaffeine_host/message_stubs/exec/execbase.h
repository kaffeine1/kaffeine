/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#pragma once
#include <exec/libraries.h>
struct ExecBase { struct Library LibNode; UBYTE snapshot[1240]; };
