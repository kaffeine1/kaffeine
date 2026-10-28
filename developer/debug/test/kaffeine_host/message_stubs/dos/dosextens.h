/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#pragma once
#include_next <dos/dosextens.h>
#include <exec/types.h>
#include <dos/bptr.h>
#include <stdint.h>
#include <exec/tasks.h>
#define NT_PROCESS 13
struct MsgPort;
struct Process { struct Task pr_Task; BPTR pr_CES; struct MsgPort *pr_ConsoleTask, *pr_FileSystemTask; };
struct DosList;
struct DevProc { struct MsgPort *dvp_Port; BPTR dvp_Lock; ULONG dvp_Flags; struct DosList *dvp_DevNode; };
#define DVPF_ASSIGN 2
struct FileHandle
{
    ULONG fh_Flags;
    LONG fh_Interactive;
    struct MsgPort *fh_Type;
    BPTR fh_Buf;
    int64_t fh_Pos, fh_End;
    SIPTR fh_Funcs, fh_Func2, fh_Func3, fh_Args, fh_Arg2;
    IPTR fh_BufSize;
    BPTR fh_OrigBuf;
};
