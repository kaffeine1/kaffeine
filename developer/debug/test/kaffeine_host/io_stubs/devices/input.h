/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#pragma once
/* The commands of compiler/include/devices/input.h. */
#include <exec/devices.h>
#include <exec/io.h>
#define IND_ADDHANDLER (CMD_NONSTD + 0)
#define IND_REMHANDLER (CMD_NONSTD + 1)
#define IND_WRITEEVENT (CMD_NONSTD + 2)
#define IND_SETTHRESH (CMD_NONSTD + 3)
#define IND_SETPERIOD (CMD_NONSTD + 4)
#define IND_SETMPORT (CMD_NONSTD + 5)
#define IND_SETMTYPE (CMD_NONSTD + 6)
#define IND_SETMTRIG (CMD_NONSTD + 7)
#define IND_ADDEVENT (CMD_NONSTD + 15)
