/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    dos.library packet functions DoPkt(), SendPkt(), WaitPkt(), ReplyPkt()
    and AbortPkt(), written for both AmigaOS 3.2.3 and AROS through kaffeine.
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

LAYOUT(message_size, sizeof(struct Message) == 20);
LAYOUT(dospacket_size, sizeof(struct DosPacket) == 48);
LAYOUT(stdpacket_size, sizeof(struct StandardPacket) == 68);
OFFSET(StandardPacket, sp_Msg, 0);
OFFSET(StandardPacket, sp_Pkt, 20);
OFFSET(DosPacket, dp_Link, 0);
OFFSET(DosPacket, dp_Port, 4);
OFFSET(DosPacket, dp_Type, 8);
OFFSET(DosPacket, dp_Res1, 12);
OFFSET(DosPacket, dp_Res2, 16);
OFFSET(DosPacket, dp_Arg1, 20);
OFFSET(DosPacket, dp_Arg2, 24);
OFFSET(DosPacket, dp_Arg3, 28);
OFFSET(DosPacket, dp_Arg4, 32);
OFFSET(DosPacket, dp_Arg5, 36);
OFFSET(DosPacket, dp_Arg6, 40);
OFFSET(DosPacket, dp_Arg7, 44);
OFFSET(FileHandle, fh_Type, 8);
OFFSET(FileHandle, fh_Arg1, 36);
OFFSET(Process, pr_MsgPort, 92);

#define FILE_NAME       "RAM:dos_packets-file"
#define WRITE_FILE_NAME "RAM:dos_packets-write"
#define CON_NAME        "CON:0/0/320/80/dos_packets"
#define FILE_DATA       "0123456789"
#define WRITE_DATA      "abc"
#define UNKNOWN_ACTION  12345

static struct FileInfoBlock fib __attribute__((aligned(4)));
static struct StandardPacket sp_copy __attribute__((aligned(4)));
static char read_buf[16];
static char write_buf[4] = WRITE_DATA;

static BOOL write_fixture(CONST_STRPTR path, const char *data, LONG len)
{
    BPTR fh = Open(path, MODE_NEWFILE);
    LONG n = 0;

    if (!fh)
        return FALSE;
    if (len > 0)
        n = Write(fh, (APTR)data, len);
    return Close(fh) && n == len;
}

int main(void)
{
    struct Process *me;
    struct MsgPort *pr_port;
    struct DosPacket *dp = NULL, *got_dp;
    struct Message *got_msg;
    struct MsgPort *port_p = NULL;
    struct MsgPort *got_port, *got_replyport;
    struct MsgPort *port_after_get, *replyport_after_get, *port_after_reply;
    BPTR file = 0, wfile = 0, con_file = 0, lock = 0;
    struct FileHandle *fh, *wfh, *con_fh;
    LONG res, err, res1, res2, got_lntype;
    LONG arg1_after_get, res1_after_reply, res2_after_reply;
    LONG res1_after_wait, res2_after_wait;
    BOOL setup_ok = FALSE;

    DeleteFile((CONST_STRPTR)FILE_NAME);
    DeleteFile((CONST_STRPTR)WRITE_FILE_NAME);

    me = (struct Process *)FindTask(NULL);
    if (!check(me != NULL && me->pr_Task.tc_Node.ln_Type == NT_PROCESS,
               "FindTask(NULL) returns a Process"))
        goto cleanup;
    pr_port = &me->pr_MsgPort;

    /* 1. AllocDosObject(DOS_STDPKT, NULL) returns sp_Pkt of a StandardPacket. */
    dp = (struct DosPacket *)AllocDosObject(DOS_STDPKT, NULL);
    if (!check(dp != NULL, "AllocDosObject(DOS_STDPKT, NULL)"))
        goto cleanup;
    check(dp->dp_Link != NULL &&
          (UBYTE *)dp->dp_Link + sizeof(struct Message) == (UBYTE *)dp,
          "dp_Link is the Message right before dp");
    check((struct DosPacket *)dp->dp_Link->mn_Node.ln_Name == dp,
          "dp_Link->mn_Node.ln_Name points to dp");
    check_long("fresh dp_Res1", dp->dp_Res1, 0);
    check_long("fresh dp_Res2", dp->dp_Res2, 0);
    check(dp->dp_Port == NULL, "fresh dp_Port is 0");

    /* 2. DoPkt() on an open MODE_OLDFILE handle in RAM:. */
    if (!check(write_fixture((CONST_STRPTR)FILE_NAME, FILE_DATA, 10),
               "write 0123456789 to RAM:dos_packets-file"))
        goto cleanup;

    file = Open((CONST_STRPTR)FILE_NAME, MODE_OLDFILE);
    if (!check(file != 0, "Open(RAM:dos_packets-file, MODE_OLDFILE)"))
        goto cleanup;
    fh = (struct FileHandle *)BADDR(file);
    if (!check(fh != NULL && fh->fh_Type != NULL,
               "FileHandle has non-NULL fh_Type"))
        goto cleanup;

    memset(read_buf, 0, sizeof(read_buf));
    SetIoErr(1234);
    res = DoPkt(fh->fh_Type, ACTION_READ, fh->fh_Arg1, (LONG)read_buf, 4, 0, 0);
    err = IoErr();
    check_long("DoPkt(ACTION_READ, 4) return", res, 4);
    check_long("DoPkt(ACTION_READ, 4) IoErr()", err, 0);
    check_str("DoPkt(ACTION_READ, 4) data", read_buf, "0123");

    SetIoErr(1234);
    res = DoPkt(fh->fh_Type, ACTION_SEEK, fh->fh_Arg1, 0, OFFSET_BEGINNING, 0, 0);
    err = IoErr();
    check_long("DoPkt(ACTION_SEEK, 0, OFFSET_BEGINNING) old position", res, 4);
    check_long("DoPkt(ACTION_SEEK, 0, OFFSET_BEGINNING) IoErr()", err, 0);

    memset(read_buf, 0, sizeof(read_buf));
    SetIoErr(1234);
    res = DoPkt(fh->fh_Type, ACTION_READ, fh->fh_Arg1, (LONG)read_buf, 4, 0, 0);
    err = IoErr();
    check_long("second DoPkt(ACTION_READ, 4) return", res, 4);
    check_long("second DoPkt(ACTION_READ, 4) IoErr()", err, 0);
    check_str("second DoPkt(ACTION_READ, 4) data", read_buf, "0123");

    SetIoErr(0);
    res = DoPkt(fh->fh_Type, ACTION_SEEK, fh->fh_Arg1, 100, OFFSET_BEGINNING, 0, 0);
    err = IoErr();
    check_long("DoPkt(ACTION_SEEK, 100, OFFSET_BEGINNING) return", res, -1);
    check_long("DoPkt(ACTION_SEEK, 100, OFFSET_BEGINNING) IoErr()", err, ERROR_SEEK_ERROR);

    SetIoErr(1234);
    res = DoPkt(fh->fh_Type, ACTION_IS_FILESYSTEM, 0, 0, 0, 0, 0);
    err = IoErr();
    check_long("DoPkt(ACTION_IS_FILESYSTEM) return", res, DOSTRUE);
    check_long("DoPkt(ACTION_IS_FILESYSTEM) IoErr()", err, 0);

    SetIoErr(0);
    res = DoPkt(fh->fh_Type, UNKNOWN_ACTION, 0, 0, 0, 0, 0);
    err = IoErr();
    check_long("DoPkt(12345) unknown action return", res, 0);
    check_long("DoPkt(12345) unknown action IoErr()", err, ERROR_ACTION_NOT_KNOWN);

    /* 3. MODE_NEWFILE: ACTION_WRITE of "abc" returns 3; 3 bytes after Close(). */
    wfile = Open((CONST_STRPTR)WRITE_FILE_NAME, MODE_NEWFILE);
    if (!check(wfile != 0, "Open(RAM:dos_packets-write, MODE_NEWFILE)"))
        goto cleanup;
    wfh = (struct FileHandle *)BADDR(wfile);
    if (!check(wfh != NULL && wfh->fh_Type != NULL,
               "MODE_NEWFILE FileHandle has non-NULL fh_Type"))
        goto cleanup;

    SetIoErr(1234);
    res = DoPkt(wfh->fh_Type, ACTION_WRITE, wfh->fh_Arg1, (LONG)write_buf, 3, 0, 0);
    err = IoErr();
    check_long("DoPkt(ACTION_WRITE, \"abc\", 3) return", res, 3);
    check_long("DoPkt(ACTION_WRITE, \"abc\", 3) IoErr()", err, 0);
    check(Close(wfile) != 0, "Close() after ACTION_WRITE");
    wfile = 0;

    lock = Lock((CONST_STRPTR)WRITE_FILE_NAME, SHARED_LOCK);
    if (!check(lock != 0, "Lock(RAM:dos_packets-write, SHARED_LOCK)"))
        goto cleanup;
    memset(&fib, 0, sizeof(fib));
    if (check(Examine(lock, &fib) != 0, "Examine(RAM:dos_packets-write)"))
        check_long("Examine() fib_Size after ACTION_WRITE", fib.fib_Size, 3);
    UnLock(lock);
    lock = 0;

    /* 4. SendPkt() + WaitPkt() of ACTION_IS_FILESYSTEM to fh->fh_Type.
          No DOS I/O calls between SendPkt() and WaitPkt(). */
    dp->dp_Type = ACTION_IS_FILESYSTEM;
    dp->dp_Arg1 = 0;
    dp->dp_Arg2 = 0;
    dp->dp_Arg3 = 0;
    dp->dp_Arg4 = 0;
    dp->dp_Arg5 = 0;
    dp->dp_Arg6 = 0;
    dp->dp_Arg7 = 0;
    dp->dp_Res1 = 0;
    dp->dp_Res2 = 0;

    SetIoErr(1234);
    SendPkt(dp, fh->fh_Type, pr_port);
    got_dp = WaitPkt();
    err = IoErr();
    res1 = dp->dp_Res1;
    res2 = dp->dp_Res2;
    got_port = dp->dp_Port;
    got_replyport = dp->dp_Link->mn_ReplyPort;
    got_lntype = (LONG)dp->dp_Link->mn_Node.ln_Type;

    check(got_dp == dp, "WaitPkt() returns the packet sent with SendPkt()");
    check_long("SendPkt/WaitPkt ACTION_IS_FILESYSTEM dp_Res1", res1, DOSTRUE);
    check_long("SendPkt/WaitPkt ACTION_IS_FILESYSTEM dp_Res2", res2, 0);
    check(got_port == fh->fh_Type, "SendPkt/WaitPkt dp_Port is fh_Type");
    check(got_replyport == pr_port, "SendPkt/WaitPkt mn_ReplyPort is pr_MsgPort");
    check_long("SendPkt/WaitPkt mn_Node.ln_Type", got_lntype, NT_MESSAGE);
    check_long("SendPkt/WaitPkt leaves IoErr() at 1234", err, 1234);

    /* 5. Ping-pong through a port from CreateMsgPort().
          No DOS I/O calls between SendPkt()/ReplyPkt() and WaitPkt(). */
    port_p = CreateMsgPort();
    if (!check(port_p != NULL, "CreateMsgPort() for ping-pong"))
        goto cleanup;

    dp->dp_Type = 999;
    dp->dp_Arg1 = 5;
    dp->dp_Res1 = 0;
    dp->dp_Res2 = 0;
    dp->dp_Port = NULL;
    dp->dp_Link->mn_ReplyPort = NULL;

    SendPkt(dp, port_p, pr_port);
    got_msg = GetMsg(port_p);
    port_after_get = dp->dp_Port;
    replyport_after_get = dp->dp_Link->mn_ReplyPort;
    arg1_after_get = dp->dp_Arg1;

    ReplyPkt(dp, 77, 88);
    port_after_reply = dp->dp_Port;
    res1_after_reply = dp->dp_Res1;
    res2_after_reply = dp->dp_Res2;

    got_dp = WaitPkt();
    res1_after_wait = dp->dp_Res1;
    res2_after_wait = dp->dp_Res2;

    check(got_msg == dp->dp_Link, "ping-pong GetMsg(P) returns dp_Link");
    check(port_after_get == pr_port, "ping-pong dp_Port after GetMsg(P) is pr_MsgPort");
    check(replyport_after_get == pr_port, "ping-pong mn_ReplyPort after GetMsg(P) is pr_MsgPort");
    check_long("ping-pong dp_Arg1 after GetMsg(P)", arg1_after_get, 5);
    check(port_after_reply == pr_port, "ping-pong dp_Port after ReplyPkt() is pr_MsgPort");
    check_long("ping-pong dp_Res1 after ReplyPkt()", res1_after_reply, 77);
    check_long("ping-pong dp_Res2 after ReplyPkt()", res2_after_reply, 88);
    check(got_dp == dp, "ping-pong WaitPkt() returns dp");
    check_long("ping-pong dp_Res1 after WaitPkt()", res1_after_wait, 77);
    check_long("ping-pong dp_Res2 after WaitPkt()", res2_after_wait, 88);

    /* 6. AbortPkt(fh_Type, dp) returns and changes neither IoErr() nor the packet. */
    memcpy(&sp_copy, dp->dp_Link, sizeof(sp_copy));
    SetIoErr(4321);
    AbortPkt(fh->fh_Type, dp);
    err = IoErr();
    check_long("AbortPkt() preserves IoErr()", err, 4321);
    check(memcmp(&sp_copy, dp->dp_Link, sizeof(sp_copy)) == 0,
          "AbortPkt() leaves the packet unchanged");

    /* 7. Console packets on CON:0/0/320/80/dos_packets. */
    con_file = Open((CONST_STRPTR)CON_NAME, MODE_NEWFILE);
    if (!check(con_file != 0, "Open(CON:0/0/320/80/dos_packets, MODE_NEWFILE)"))
        goto cleanup;
    con_fh = (struct FileHandle *)BADDR(con_file);
    if (!check(con_fh != NULL && con_fh->fh_Type != NULL,
               "console FileHandle has non-NULL fh_Type"))
        goto cleanup;

    res = DoPkt(con_fh->fh_Type, ACTION_SCREEN_MODE, 1, 0, 0, 0, 0);
    check(res != 0, "console DoPkt(ACTION_SCREEN_MODE, 1) returns non-zero");

    res = DoPkt(con_fh->fh_Type, ACTION_SCREEN_MODE, 0, 0, 0, 0, 0);
    check(res != 0, "console DoPkt(ACTION_SCREEN_MODE, 0) returns non-zero");

    res = DoPkt(con_fh->fh_Type, ACTION_WAIT_CHAR, 1000, 0, 0, 0, 0);
    check_long("console DoPkt(ACTION_WAIT_CHAR, 1000)", res, 0);

    SetIoErr(0);
    res = DoPkt(con_fh->fh_Type, UNKNOWN_ACTION, 0, 0, 0, 0, 0);
    err = IoErr();
    check_long("console DoPkt(12345) unknown action return", res, 0);
    check_long("console DoPkt(12345) unknown action IoErr()", err, ERROR_ACTION_NOT_KNOWN);

    setup_ok = TRUE;

    /* 8. FreeDosObject(DOS_STDPKT, dp); close and delete everything. */
cleanup:
    /* The result is not checked: the AROS con-handler answers the last
       ACTION_END with DOSFALSE, so Close() of a console returns 0 there,
       DOSTRUE on AmigaOS 3.2.3. */
    if (con_file)
        Close(con_file);
    if (wfile)
        Close(wfile);
    if (lock)
        UnLock(lock);
    if (file)
    {
        if (setup_ok)
            check(Close(file) != 0, "Close(RAM:dos_packets-file)");
        else
            Close(file);
    }
    if (port_p)
    {
        if (port_p->mp_SigBit < 32)
            SetSignal(0, 1UL << port_p->mp_SigBit);
        DeleteMsgPort(port_p);
    }
    if (dp)
    {
        FreeDosObject(DOS_STDPKT, dp);
        if (setup_ok)
            check(TRUE, "FreeDosObject(DOS_STDPKT, dp)");
    }
    if (setup_ok)
    {
        check(DeleteFile((CONST_STRPTR)WRITE_FILE_NAME) != 0,
              "DeleteFile(RAM:dos_packets-write)");
        check(DeleteFile((CONST_STRPTR)FILE_NAME) != 0,
              "DeleteFile(RAM:dos_packets-file)");
    }
    else
    {
        DeleteFile((CONST_STRPTR)WRITE_FILE_NAME);
        DeleteFile((CONST_STRPTR)FILE_NAME);
    }

    return test_done("dos_packets");
}
