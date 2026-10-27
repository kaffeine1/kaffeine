#pragma once
/* The fields that the native argument parser uses */
#include <exec/types.h>
#include <dos/bptr.h>

struct Process { SIPTR pr_Result2; };
struct FileHandle { LONG fh_Pos; LONG fh_End; };
