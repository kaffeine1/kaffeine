#pragma once
#include <exec/tasks.h>
#include <dos/bptr.h>
#include <dos/dos.h>
struct MsgPort { ULONG identity; };
struct Process { struct Task pr_Task; BPTR pr_CurrentDir, pr_HomeDir; };
struct FileLock { BPTR fl_Link; IPTR fl_Key; LONG fl_Access; struct MsgPort *fl_Task; BPTR fl_Volume; };
struct DeviceList {
    BPTR dl_Next; LONG dl_Type; struct MsgPort *dl_Task; BPTR dl_Lock;
    struct DateStamp dl_VolumeDate; BPTR dl_LockList; LONG dl_DiskType;
    BPTR dl_unused, dl_Name;
};
#define DLT_VOLUME 2
#define LDF_READ 1
#define LDF_VOLUMES 8
