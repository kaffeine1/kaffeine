#pragma once
#include <dos/bptr.h>
#include <exec/ports.h>
struct WBArg
{
    BPTR wa_Lock;
    BYTE *wa_Name;
};
struct WBStartup
{
    struct Message sm_Message;
    struct MsgPort *sm_Process;
    BPTR sm_Segment;
    LONG sm_NumArgs;
    char *sm_ToolWindow;
    struct WBArg *sm_ArgList;
};
