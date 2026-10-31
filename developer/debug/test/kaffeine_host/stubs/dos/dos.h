#pragma once

/* AmigaDOS command result for a complete or severe failure. */
#define RETURN_FAIL 20
#define ERROR_NO_FREE_STORE 103

#include <exec/types.h>
struct DateStamp
{
    LONG ds_Days, ds_Minute, ds_Tick;
};
