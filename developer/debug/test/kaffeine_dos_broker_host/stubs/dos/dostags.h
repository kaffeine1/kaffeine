#pragma once
#include <exec/types.h>
struct TagItem { IPTR ti_Tag, ti_Data; };
enum { TAG_DONE=0, TAG_IGNORE=1, NP_Entry=100, NP_UserData, NP_Name,
NP_StackSize, NP_Priority, NP_Cli, NP_CopyVars, NP_Arguments, NP_HomeDir,
NP_ConsoleTask, NP_WindowPtr, NP_FreeSeglist, NP_Seglist };
