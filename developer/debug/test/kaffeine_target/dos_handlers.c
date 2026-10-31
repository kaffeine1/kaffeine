/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Handler ports of dos.library: DeviceProc(), GetDeviceProc(),
    GetConsoleTask(), GetFileSysTask(), SetConsoleTask() and
    SetFileSysTask(), and the filesystem actions that a program sends to
    those ports, written for both AmigaOS 3.2.3 and AROS through kaffeine.
*/

#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stddef.h>
#include <string.h>

#include "m68ktest.h"

#define LAYOUT(n, e) typedef char layout_##n[(e) ? 1 : -1]
#define OFFSET(t, f, o) LAYOUT(t##_##f, offsetof(struct t, f) == (o))

LAYOUT(devproc_size, sizeof(struct DevProc) == 16);
OFFSET(DevProc, dvp_Port, 0);
OFFSET(DevProc, dvp_Lock, 4);
OFFSET(DevProc, dvp_Flags, 8);
OFFSET(DevProc, dvp_DevNode, 12);
OFFSET(Process, pr_ConsoleTask, 164);
OFFSET(Process, pr_FileSystemTask, 168);
OFFSET(FileInfoBlock, fib_FileName, 8);
OFFSET(FileInfoBlock, fib_Comment, 144);
OFFSET(InfoData, id_DiskType, 24);

#define DIR_NAME       "RAM:dos_handlers"
#define DIR2_NAME      "RAM:dos_handlers2"
#define FILE_NAME      "RAM:dos_handlers/file"
#define SUB2_NAME      "RAM:dos_handlers/sub2"
#define SUB_NAME       "RAM:dos_handlers/sub"
#define ASSIGN_NAME    "DOSHANDLERS"
#define CON_NAME       "CON:0/0/320/80/dos_handlers"
#define FILE_DATA      "0123456789"
#define COMMENT        "a comment"
#define PROTECTION     (FIBF_ARCHIVE | FIBF_PURE)
#define UNKNOWN_ACTION 12345
#define ID_CON         0x434F4E00
#define ID_RAW         0x52415700

static struct FileInfoBlock fib __attribute__((aligned(4)));
static struct FileInfoBlock dos_fib __attribute__((aligned(4)));
static struct InfoData info __attribute__((aligned(4)));
static struct InfoData dos_info __attribute__((aligned(4)));
static struct DateStamp stamp __attribute__((aligned(4)));
static UBYTE bstr1[64] __attribute__((aligned(4)));
static UBYTE bstr2[64] __attribute__((aligned(4)));

static BSTR bstr(UBYTE *buffer, const char *s)
{
    ULONG n = strlen(s);

    buffer[0] = (UBYTE)n;
    memcpy(buffer + 1, s, n);
    return MKBADDR(buffer);
}

static BOOL bstr_is(const UBYTE *b, const char *s)
{
    ULONG n = strlen(s);

    return b[0] == n && !memcmp(b + 1, s, n);
}

static BOOL write_fixture(CONST_STRPTR path, const char *data, LONG len)
{
    BPTR fh = Open(path, MODE_NEWFILE);
    LONG n;

    if (!fh)
        return FALSE;
    n = Write(fh, (APTR)data, len);
    return Close(fh) && n == len;
}

static BOOL same_lock(BPTR a, BPTR b)
{
    return a && b && SameLock(a, b) == LOCK_SAME;
}

static void info_long(const char *what, LONG value)
{
    Printf("info %s: %ld (0x%08lx)\n", (LONG)what, value, value);
    test_flush();
}

int main(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    struct MsgPort *con, *fs, *ram = NULL, *port, *old, *field;
    struct DevProc *dvp = NULL;
    struct DosPacket *dp = NULL;
    struct FileHandle *con_fh;
    BPTR dir = 0, dir2 = 0, con_file = 0, file = 0, lock = 0, flock = 0, l;
    LONG res, err;
    BOOL assigned = FALSE, setup_ok = FALSE;

    AssignLock((CONST_STRPTR)ASSIGN_NAME, 0);
    SetProtection((CONST_STRPTR)FILE_NAME, 0);
    DeleteFile((CONST_STRPTR)SUB2_NAME);
    DeleteFile((CONST_STRPTR)SUB_NAME);
    DeleteFile((CONST_STRPTR)FILE_NAME);
    DeleteFile((CONST_STRPTR)DIR_NAME);
    DeleteFile((CONST_STRPTR)DIR2_NAME);

    /* 1. The console of the process. */
    con = GetConsoleTask();
    info_long("the program has a console", con != NULL);
    check(con == me->pr_ConsoleTask, "GetConsoleTask() is pr_ConsoleTask");
    if (con)
        check_long("DoPkt(console, ACTION_IS_FILESYSTEM)",
                   DoPkt(con, ACTION_IS_FILESYSTEM, 0, 0, 0, 0, 0), DOSFALSE);

    /* 2. A console of the program as the console task. */
    con_file = Open((CONST_STRPTR)CON_NAME, MODE_NEWFILE);
    if (!check(con_file != 0, "Open(" CON_NAME ")"))
        goto cleanup;
    con_fh = (struct FileHandle *)BADDR(con_file);
    old = SetConsoleTask(con_fh->fh_Type);
    check(old == con, "SetConsoleTask(fh_Type) returns the previous console");
    check(GetConsoleTask() == con_fh->fh_Type, "GetConsoleTask() is the fh_Type that was set");
    check(me->pr_ConsoleTask == con_fh->fh_Type, "pr_ConsoleTask is the fh_Type that was set");
    check_long("DoPkt(GetConsoleTask(), ACTION_SCREEN_MODE, DOSTRUE)",
               DoPkt(GetConsoleTask(), ACTION_SCREEN_MODE, DOSTRUE, 0, 0, 0, 0), DOSTRUE);
    memset(&info, 0, sizeof(info));
    check_long("DoPkt(fh_Type, ACTION_DISK_INFO) in raw mode",
               DoPkt(con_fh->fh_Type, ACTION_DISK_INFO, MKBADDR(&info), 0, 0, 0, 0), DOSTRUE);
    check_long("id_DiskType in raw mode", info.id_DiskType, ID_RAW);
    check_long("DoPkt(GetConsoleTask(), ACTION_SCREEN_MODE, DOSFALSE)",
               DoPkt(GetConsoleTask(), ACTION_SCREEN_MODE, DOSFALSE, 0, 0, 0, 0), DOSTRUE);
    memset(&info, 0, sizeof(info));
    check_long("DoPkt(GetConsoleTask(), ACTION_DISK_INFO)",
               DoPkt(GetConsoleTask(), ACTION_DISK_INFO, MKBADDR(&info), 0, 0, 0, 0), DOSTRUE);
    check_long("id_DiskType in cooked mode", info.id_DiskType, ID_CON);
    info_long("console id_VolumeNode", (LONG)info.id_VolumeNode);
    info_long("console id_InUse", (LONG)info.id_InUse);
    old = SetConsoleTask(con);
    check(old == con_fh->fh_Type, "SetConsoleTask() gives back the fh_Type");
    check(GetConsoleTask() == con, "GetConsoleTask() after SetConsoleTask(previous)");

    /* 3. The filesystem task. */
    fs = GetFileSysTask();
    check(fs != NULL, "the program has a filesystem task");
    check(fs == me->pr_FileSystemTask, "GetFileSysTask() is pr_FileSystemTask");
    if (fs)
        check_long("DoPkt(GetFileSysTask(), ACTION_IS_FILESYSTEM)",
                   DoPkt(fs, ACTION_IS_FILESYSTEM, 0, 0, 0, 0, 0), DOSTRUE);

    /* 4. DeviceProc() of a device and of a path. */
    SetIoErr(1234);
    ram = DeviceProc((CONST_STRPTR)"RAM:");
    err = IoErr();
    if (!check(ram != NULL, "DeviceProc(RAM:)"))
        goto cleanup;
    check_long("IoErr() after DeviceProc(RAM:)", err, 0);
    check(DeviceProc((CONST_STRPTR)"RAM:dos_handlers/file") == ram,
          "DeviceProc() of a path in RAM: is the same port");

    /* The drawers and the file the actions work on. */
    l = CreateDir((CONST_STRPTR)DIR_NAME);
    if (l)
        UnLock(l);
    l = CreateDir((CONST_STRPTR)DIR2_NAME);
    if (l)
        UnLock(l);
    dir = Lock((CONST_STRPTR)DIR_NAME, SHARED_LOCK);
    dir2 = Lock((CONST_STRPTR)DIR2_NAME, SHARED_LOCK);
    if (!check(dir && dir2 && write_fixture((CONST_STRPTR)FILE_NAME, FILE_DATA, 10),
               "two drawers and a file of 10 bytes in RAM:"))
        goto cleanup;
    setup_ok = TRUE;
    file = Open((CONST_STRPTR)FILE_NAME, MODE_OLDFILE);
    if (file)
    {
        info_long("DeviceProc(RAM:) is fh_Type of a file in RAM:",
                  ((struct FileHandle *)BADDR(file))->fh_Type == ram);
        Close(file);
    }

    /* 5. DeviceProc() and GetDeviceProc() of assigns. */
    l = DupLock(dir);
    if (!check(AssignLock((CONST_STRPTR)ASSIGN_NAME, l) != 0, "AssignLock(" ASSIGN_NAME ")"))
    {
        UnLock(l);
        goto cleanup;
    }
    assigned = TRUE;
    SetIoErr(1234);
    port = DeviceProc((CONST_STRPTR)ASSIGN_NAME ":");
    err = IoErr();
    check(port == ram, "DeviceProc() of an assign is the port of its handler");
    check(err != 1234 && same_lock((BPTR)err, dir),
          "IoErr() after DeviceProc() of an assign is a lock on its drawer");

    dvp = GetDeviceProc((CONST_STRPTR)"RAM:", NULL);
    if (check(dvp != NULL, "GetDeviceProc(RAM:)"))
    {
        check(dvp->dvp_Port == ram, "dvp_Port of RAM: is the port of DeviceProc()");
        check_long("dvp_Lock of RAM:", (LONG)dvp->dvp_Lock, 0);
        info_long("dvp_Flags of RAM:", dvp->dvp_Flags);
        info_long("dvp_DevNode of RAM: is set", dvp->dvp_DevNode != NULL);
        FreeDeviceProc(dvp);
        dvp = NULL;
    }
    dvp = GetDeviceProc((CONST_STRPTR)ASSIGN_NAME ":", NULL);
    if (check(dvp != NULL, "GetDeviceProc(" ASSIGN_NAME ":)"))
    {
        check(dvp->dvp_Port == ram, "dvp_Port of the assign");
        check(same_lock(dvp->dvp_Lock, dir), "dvp_Lock of the assign is its drawer");
        info_long("dvp_Flags of the assign", dvp->dvp_Flags);
        SetIoErr(1234);
        dvp = GetDeviceProc((CONST_STRPTR)ASSIGN_NAME ":", dvp);
        err = IoErr();
        check(dvp == NULL, "GetDeviceProc() again on an assign of one drawer");
        info_long("IoErr() after the last drawer", err);
        if (dvp)
            FreeDeviceProc(dvp);
        dvp = NULL;
    }
    l = DupLock(dir2);
    if (check(AssignAdd((CONST_STRPTR)ASSIGN_NAME, l) != 0, "AssignAdd(" ASSIGN_NAME ")"))
    {
        dvp = GetDeviceProc((CONST_STRPTR)ASSIGN_NAME ":", NULL);
        check(dvp && same_lock(dvp->dvp_Lock, dir), "first drawer of the multi-assign");
        if (dvp)
        {
            check((dvp->dvp_Flags & DVPF_ASSIGN) != 0, "dvp_Flags of a multi-assign has DVPF_ASSIGN");
            dvp = GetDeviceProc((CONST_STRPTR)ASSIGN_NAME ":", dvp);
        }
        check(dvp && same_lock(dvp->dvp_Lock, dir2) && dvp->dvp_Port == ram,
              "second drawer of the multi-assign");
        if (dvp)
        {
            SetIoErr(1234);
            dvp = GetDeviceProc((CONST_STRPTR)ASSIGN_NAME ":", dvp);
            err = IoErr();
            check(dvp == NULL, "no third drawer");
            info_long("IoErr() after the multi-assign", err);
        }
        if (dvp)
            FreeDeviceProc(dvp);
        dvp = NULL;
    }
    else
        UnLock(l);

    /* 6. ACTION_LOCATE_OBJECT. */
    lock = (BPTR)DoPkt(ram, ACTION_LOCATE_OBJECT, 0, bstr(bstr1, DIR_NAME), SHARED_LOCK, 0, 0);
    if (!check(lock != 0, "ACTION_LOCATE_OBJECT of RAM:dos_handlers"))
        goto cleanup;
    check(same_lock(lock, dir), "the packet lock is the drawer that Lock() gives");
    SetIoErr(0);
    res = DoPkt(ram, ACTION_LOCATE_OBJECT, lock, bstr(bstr1, "missing"), SHARED_LOCK, 0, 0);
    err = IoErr();
    check_long("ACTION_LOCATE_OBJECT of a missing name", res, 0);
    check_long("its IoErr()", err, ERROR_OBJECT_NOT_FOUND);

    /* 7. ACTION_EXAMINE_OBJECT and ACTION_EXAMINE_NEXT. */
    memset(&fib, 0, sizeof(fib));
    check_long("ACTION_EXAMINE_OBJECT of the drawer",
               DoPkt(ram, ACTION_EXAMINE_OBJECT, lock, MKBADDR(&fib), 0, 0, 0), DOSTRUE);
    check(bstr_is(fib.fib_FileName, "dos_handlers"), "the drawer's fib_FileName is a BSTR");
    check(fib.fib_DirEntryType > 0, "the drawer's fib_DirEntryType is positive");
    check_long("ACTION_EXAMINE_NEXT", DoPkt(ram, ACTION_EXAMINE_NEXT, lock, MKBADDR(&fib), 0, 0, 0), DOSTRUE);
    check(bstr_is(fib.fib_FileName, "file"), "ACTION_EXAMINE_NEXT gives the file, its name a BSTR");
    check_long("the file's fib_Size", fib.fib_Size, 10);
    check(fib.fib_DirEntryType < 0, "the file's fib_DirEntryType is negative");
    SetIoErr(0);
    res = DoPkt(ram, ACTION_EXAMINE_NEXT, lock, MKBADDR(&fib), 0, 0, 0);
    err = IoErr();
    check_long("ACTION_EXAMINE_NEXT at the end", res, DOSFALSE);
    check_long("its IoErr()", err, ERROR_NO_MORE_ENTRIES);

    /* 8. A lock on the file: ACTION_COPY_DIR, ACTION_SAME_LOCK, ACTION_PARENT
          and ACTION_FREE_LOCK, and the locks with DOS functions. */
    flock = (BPTR)DoPkt(ram, ACTION_LOCATE_OBJECT, lock, bstr(bstr1, "file"), SHARED_LOCK, 0, 0);
    if (!check(flock != 0, "ACTION_LOCATE_OBJECT of the file in the drawer"))
        goto cleanup;
    l = (BPTR)DoPkt(ram, ACTION_COPY_DIR, flock, 0, 0, 0, 0);
    check(l != 0, "ACTION_COPY_DIR");
    if (l)
    {
        check_long("ACTION_SAME_LOCK of the copy", DoPkt(ram, ACTION_SAME_LOCK, flock, l, 0, 0, 0), DOSTRUE);
        check_long("ACTION_FREE_LOCK of the copy", DoPkt(ram, ACTION_FREE_LOCK, l, 0, 0, 0, 0), DOSTRUE);
    }
    check_long("ACTION_SAME_LOCK of the file and the drawer",
               DoPkt(ram, ACTION_SAME_LOCK, flock, lock, 0, 0, 0), DOSFALSE);
    l = (BPTR)DoPkt(ram, ACTION_PARENT, flock, 0, 0, 0, 0);
    check(same_lock(l, dir), "ACTION_PARENT of the file is the drawer");
    if (l)
        check_long("ACTION_FREE_LOCK of the parent", DoPkt(ram, ACTION_FREE_LOCK, l, 0, 0, 0, 0), DOSTRUE);
    check(Examine(flock, &dos_fib) != 0, "Examine() of the packet lock");
    check_str("its fib_FileName", (const char *)dos_fib.fib_FileName, "file");

    /* 9. ACTION_CREATE_DIR, ACTION_RENAME_OBJECT and ACTION_DELETE_OBJECT. */
    l = (BPTR)DoPkt(ram, ACTION_CREATE_DIR, lock, bstr(bstr1, "sub"), 0, 0, 0);
    check(l != 0, "ACTION_CREATE_DIR");
    if (l)
        check_long("ACTION_FREE_LOCK of the new drawer", DoPkt(ram, ACTION_FREE_LOCK, l, 0, 0, 0, 0), DOSTRUE);
    check_long("ACTION_RENAME_OBJECT",
               DoPkt(ram, ACTION_RENAME_OBJECT, lock, bstr(bstr1, "sub"), lock, bstr(bstr2, "sub2"), 0),
               DOSTRUE);
    l = Lock((CONST_STRPTR)SUB2_NAME, SHARED_LOCK);
    check(l != 0, "Lock() of the new name");
    if (l)
        UnLock(l);
    l = Lock((CONST_STRPTR)SUB_NAME, SHARED_LOCK);
    err = IoErr();
    check(l == 0 && err == ERROR_OBJECT_NOT_FOUND, "the old name is gone");
    if (l)
        UnLock(l);
    check_long("ACTION_DELETE_OBJECT", DoPkt(ram, ACTION_DELETE_OBJECT, lock, bstr(bstr1, "sub2"), 0, 0, 0),
               DOSTRUE);
    SetIoErr(0);
    res = DoPkt(ram, ACTION_DELETE_OBJECT, lock, bstr(bstr1, "sub2"), 0, 0, 0);
    err = IoErr();
    check_long("ACTION_DELETE_OBJECT again", res, DOSFALSE);
    check_long("its IoErr()", err, ERROR_OBJECT_NOT_FOUND);

    /* 10. ACTION_SET_PROTECT, ACTION_SET_COMMENT and ACTION_SET_DATE. */
    check_long("ACTION_SET_PROTECT",
               DoPkt(ram, ACTION_SET_PROTECT, 0, lock, bstr(bstr1, "file"), PROTECTION, 0), DOSTRUE);
    check_long("ACTION_SET_COMMENT",
               DoPkt(ram, ACTION_SET_COMMENT, 0, lock, bstr(bstr1, "file"), bstr(bstr2, COMMENT), 0),
               DOSTRUE);
    stamp.ds_Days = 7000;
    stamp.ds_Minute = 600;
    stamp.ds_Tick = 1000;
    check_long("ACTION_SET_DATE",
               DoPkt(ram, ACTION_SET_DATE, 0, lock, bstr(bstr1, "file"), (LONG)&stamp, 0), DOSTRUE);
    if (check(Examine(flock, &dos_fib) != 0, "Examine() after the changes"))
    {
        check_long("fib_Protection", dos_fib.fib_Protection, PROTECTION);
        check_str("fib_Comment", (const char *)dos_fib.fib_Comment, COMMENT);
        check(dos_fib.fib_Date.ds_Days == 7000 && dos_fib.fib_Date.ds_Minute == 600 &&
              dos_fib.fib_Date.ds_Tick == 1000, "fib_Date");
    }

    /* 11. ACTION_INFO, ACTION_DISK_INFO, ACTION_FLUSH and an unknown action. */
    memset(&info, 0, sizeof(info));
    memset(&dos_info, 0, sizeof(dos_info));
    check_long("ACTION_INFO", DoPkt(ram, ACTION_INFO, lock, MKBADDR(&info), 0, 0, 0), DOSTRUE);
    check(Info(lock, &dos_info) != 0, "Info() of the drawer");
    check_long("id_DiskType of ACTION_INFO", info.id_DiskType, dos_info.id_DiskType);
    check_long("id_BytesPerBlock of ACTION_INFO", info.id_BytesPerBlock, dos_info.id_BytesPerBlock);
    info_long("id_VolumeNode of ACTION_INFO is set", info.id_VolumeNode != 0);
    memset(&info, 0, sizeof(info));
    check_long("ACTION_DISK_INFO", DoPkt(ram, ACTION_DISK_INFO, MKBADDR(&info), 0, 0, 0, 0), DOSTRUE);
    check_long("id_DiskType of ACTION_DISK_INFO", info.id_DiskType, dos_info.id_DiskType);
    check_long("ACTION_FLUSH", DoPkt(ram, ACTION_FLUSH, 0, 0, 0, 0, 0), DOSTRUE);
    SetIoErr(0);
    res = DoPkt(ram, UNKNOWN_ACTION, 0, 0, 0, 0, 0);
    err = IoErr();
    check_long("unknown action", res, 0);
    check_long("its IoErr()", err, ERROR_ACTION_NOT_KNOWN);

    /* 12. SendPkt() to the handler, and WaitPkt(). */
    dp = (struct DosPacket *)AllocDosObject(DOS_STDPKT, NULL);
    if (check(dp != NULL, "AllocDosObject(DOS_STDPKT)"))
    {
        dp->dp_Type = ACTION_IS_FILESYSTEM;
        SendPkt(dp, ram, &me->pr_MsgPort);
        check(WaitPkt() == dp, "WaitPkt() gives the packet back");
        check_long("its dp_Res1", dp->dp_Res1, DOSTRUE);
        check(dp->dp_Port == ram, "its dp_Port is the handler");
        check_long("its ln_Type", dp->dp_Link->mn_Node.ln_Type, NT_MESSAGE);
        FreeDosObject(DOS_STDPKT, dp);
    }

    /* 13. ACTION_FREE_LOCK of the packet locks. */
    check_long("ACTION_FREE_LOCK of the file", DoPkt(ram, ACTION_FREE_LOCK, flock, 0, 0, 0, 0), DOSTRUE);
    flock = 0;
    check_long("ACTION_FREE_LOCK of the drawer", DoPkt(ram, ACTION_FREE_LOCK, lock, 0, 0, 0, 0), DOSTRUE);
    lock = 0;

    /* 14. SetFileSysTask(). */
    old = SetFileSysTask(ram);
    port = GetFileSysTask();
    field = me->pr_FileSystemTask;
    SetFileSysTask(old);
    check(old == fs, "SetFileSysTask() returns the previous task");
    check(port == ram, "GetFileSysTask() after SetFileSysTask()");
    check(field == ram, "pr_FileSystemTask after SetFileSysTask()");
    check(GetFileSysTask() == fs, "GetFileSysTask() after SetFileSysTask(previous)");

cleanup:
    if (flock)
        UnLock(flock);
    if (lock)
        UnLock(lock);
    if (dvp)
        FreeDeviceProc(dvp);
    if (assigned)
        check(AssignLock((CONST_STRPTR)ASSIGN_NAME, 0) != 0, "AssignLock(" ASSIGN_NAME ", 0)");
    if (dir)
        UnLock(dir);
    if (dir2)
        UnLock(dir2);
    if (con_file)
    {
        SetConsoleTask(con);
        Close(con_file);
    }
    DeleteFile((CONST_STRPTR)SUB2_NAME);
    DeleteFile((CONST_STRPTR)SUB_NAME);
    if (setup_ok)
    {
        SetProtection((CONST_STRPTR)FILE_NAME, 0);
        check(DeleteFile((CONST_STRPTR)FILE_NAME) != 0, "DeleteFile(" FILE_NAME ")");
        check(DeleteFile((CONST_STRPTR)DIR_NAME) != 0, "DeleteFile(" DIR_NAME ")");
        check(DeleteFile((CONST_STRPTR)DIR2_NAME) != 0, "DeleteFile(" DIR2_NAME ")");
    }
    else
    {
        SetProtection((CONST_STRPTR)FILE_NAME, 0);
        DeleteFile((CONST_STRPTR)FILE_NAME);
        DeleteFile((CONST_STRPTR)DIR_NAME);
        DeleteFile((CONST_STRPTR)DIR2_NAME);
    }

    return test_done("dos_handlers");
}
