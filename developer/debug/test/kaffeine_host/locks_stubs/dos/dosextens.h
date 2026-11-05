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
#ifndef BSTR
typedef BPTR BSTR;
#endif
struct DosList {
    BPTR dol_Next; LONG dol_Type; struct MsgPort *dol_Task; BPTR dol_Lock;
    union {
        struct { BSTR dol_Handler; LONG dol_StackSize, dol_Priority;
                 BPTR dol_Startup, dol_SegList, dol_GlobVec; } dol_handler;
        struct { struct DateStamp dol_VolumeDate; BPTR dol_LockList;
                 LONG dol_DiskType; BPTR dol_unused; } dol_volume;
        struct { UBYTE *dol_AssignName; void *dol_List; } dol_assign;
    } dol_misc;
    BSTR dol_Name;
};
#define DLT_DEVICE 0
#define DLT_DIRECTORY 1
#define DLT_VOLUME 2
#define DLT_LATE 3
#define DLT_NONBINDING 4
#define LDF_READ 1
#define LDF_WRITE 2
#define LDF_DEVICES 4
#define LDF_VOLUMES 8
#define LDF_ASSIGNS 16
#define LDF_ENTRY 32
#define LDF_DELETE 64
#define LDF_ALL (LDF_DEVICES | LDF_VOLUMES | LDF_ASSIGNS)
