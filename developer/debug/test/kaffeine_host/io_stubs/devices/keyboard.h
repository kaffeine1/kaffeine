/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#pragma once
/* The commands of compiler/include/devices/keyboard.h. */
#include <exec/io.h>
#define KBD_READEVENT (CMD_NONSTD + 0)
#define KBD_READMATRIX (CMD_NONSTD + 1)
#define KBD_ADDRESETHANDLER (CMD_NONSTD + 2)
#define KBD_REMRESETHANDLER (CMD_NONSTD + 3)
#define KBD_RESETHANDLERDONE (CMD_NONSTD + 4)
