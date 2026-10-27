/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Read an original fixture through valid public DOS packets and file APIs.
    This program never writes a file or sends malformed packets to native DOS.
*/
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "m68ktest.h"

static const UBYTE fixture[] =
    {0x41,0x52,0x4f,0x53,0x00,0x80,0x7f,0x20,0x31,0x32,0x33,0x0a};

struct GuardedBuffer
{
    ULONG before[2];
    UBYTE bytes[16];
    ULONG after[2];
};

static BOOL empty_port(struct MsgPort *port)
{
    return port->mp_MsgList.lh_Head == (struct Node *)&port->mp_MsgList.lh_Tail &&
           port->mp_MsgList.lh_Tail == NULL &&
           port->mp_MsgList.lh_TailPred == (struct Node *)&port->mp_MsgList.lh_Head;
}

static BOOL packet_read(struct MsgPort *handler, LONG key, struct MsgPort *reply,
                        struct StandardPacket *packet, struct GuardedBuffer *buffer,
                        LONG round, LONG requested, LONG offset, LONG expected,
                        BOOL probe)
{
    ULONG mask = 1UL << reply->mp_SigBit, i, arrival;
    struct Message *shown, *taken, *extra;
    BOOL bytes_ok = TRUE, untouched = TRUE, guards, same, signaled, no_extra;
    LONG ioerr;

    memset(packet, 0, sizeof(*packet));
    memset(buffer, 0xa5, sizeof(*buffer));
    buffer->before[0] = 0x01234567;
    buffer->before[1] = 0x89abcdef;
    buffer->after[0] = 0x76543210;
    buffer->after[1] = 0xfedcba98;
    packet->sp_Msg.mn_Node.ln_Type = NT_MESSAGE;
    packet->sp_Msg.mn_Node.ln_Pri = -7;
    packet->sp_Msg.mn_Node.ln_Name = (STRPTR)&packet->sp_Pkt;
    packet->sp_Msg.mn_Length = sizeof(*packet);
    packet->sp_Pkt.dp_Link = &packet->sp_Msg;
    packet->sp_Pkt.dp_Port = reply;
    packet->sp_Pkt.dp_Type = ACTION_READ;
    packet->sp_Pkt.dp_Arg1 = key;
    packet->sp_Pkt.dp_Arg2 = (LONG)buffer->bytes;
    packet->sp_Pkt.dp_Arg3 = requested;

    SetSignal(0, mask);
    SetIoErr(137);
    PutMsg(handler, &packet->sp_Msg);
    arrival = Wait(mask);
    shown = WaitPort(reply);
    taken = GetMsg(reply);
    extra = GetMsg(reply);
    ioerr = IoErr();
    signaled = (SetSignal(0, 0) & mask) != 0;
    same = shown == &packet->sp_Msg && taken == &packet->sp_Msg;
    no_extra = extra == NULL && empty_port(reply);
    for (i = 0; i < (ULONG)expected; i++)
        if (buffer->bytes[i] != fixture[offset+i])
            bytes_ok = FALSE;
    for (i = expected; i < sizeof(buffer->bytes); i++)
        if (buffer->bytes[i] != 0xa5)
            untouched = FALSE;
    guards = buffer->before[0] == 0x01234567 && buffer->before[1] == 0x89abcdef &&
             buffer->after[0] == 0x76543210 && buffer->after[1] == 0xfedcba98;

    if (probe)
        Printf("PACKET round=%ld requested=%ld result=%ld error=%ld type=%ld link=%ld handler=%ld name=%ld replynull=%ld priority=%ld length=%ld args=%ld bytes=%ld untouched=%ld guards=%ld same=%ld empty=%ld arrival=%ld signal=%ld ioerr=%ld\n",
               round, requested, (LONG)packet->sp_Pkt.dp_Res1,
               (LONG)packet->sp_Pkt.dp_Res2, (LONG)packet->sp_Msg.mn_Node.ln_Type,
               (LONG)(packet->sp_Pkt.dp_Link == &packet->sp_Msg),
               (LONG)(packet->sp_Pkt.dp_Port == handler),
               (LONG)((APTR)packet->sp_Msg.mn_Node.ln_Name == (APTR)&packet->sp_Pkt),
               (LONG)(packet->sp_Msg.mn_ReplyPort == NULL),
               (LONG)packet->sp_Msg.mn_Node.ln_Pri,
               (LONG)packet->sp_Msg.mn_Length,
               (LONG)(packet->sp_Pkt.dp_Type == ACTION_READ &&
                      packet->sp_Pkt.dp_Arg1 == key &&
                      packet->sp_Pkt.dp_Arg2 == (LONG)buffer->bytes &&
                      packet->sp_Pkt.dp_Arg3 == requested),
               (LONG)bytes_ok, (LONG)untouched, (LONG)guards,
               (LONG)same, (LONG)no_extra, (LONG)((arrival & mask) != 0),
               (LONG)signaled, ioerr);

    check(same && no_extra, "DOS packet reply keeps message identity and drains the reply queue");
    check_long("ACTION_READ byte count", packet->sp_Pkt.dp_Res1, expected);
    check(bytes_ok && untouched && guards,
          "packet read copies only returned bytes and preserves buffer guards");
    check(packet->sp_Pkt.dp_Link == &packet->sp_Msg &&
          (APTR)packet->sp_Msg.mn_Node.ln_Name == (APTR)&packet->sp_Pkt &&
          packet->sp_Msg.mn_ReplyPort == NULL &&
          packet->sp_Msg.mn_Node.ln_Pri == -7 &&
          packet->sp_Msg.mn_Length == sizeof(*packet),
          "DOS packet preserves linkage identity and message metadata");
    check(packet->sp_Pkt.dp_Type == ACTION_READ &&
          packet->sp_Pkt.dp_Arg1 == key &&
          packet->sp_Pkt.dp_Arg2 == (LONG)buffer->bytes &&
          packet->sp_Pkt.dp_Arg3 == requested,
          "DOS packet preserves action and all read arguments");
    check(ioerr == 137, "packet delivery preserves the caller IoErr value");
    check((arrival & mask) != 0 && !signaled,
          "packet reply signals its port and Wait consumes the arrival bit");
    return same && no_extra;
}

int main(int argc, char **argv)
{
    BOOL probe = argc > 1, ok = FALSE;
    BPTR handle = 0;
    struct FileHandle *view;
    struct MsgPort *reply = NULL, *handler;
    struct StandardPacket *packet = NULL;
    struct GuardedBuffer *buffer = NULL;
    UBYTE direct[3];
    LONG key, n, old, pos;
    (void)argv;

    handle = Open((CONST_STRPTR)"PROGDIR:putmsg-read.bin", MODE_OLDFILE);
    check(handle != 0, "Open provides a file handle for the original packet fixture");
    if (!handle)
        goto cleanup;
    view = (struct FileHandle *)BADDR(handle);
    handler = view->fh_Type;
    key = view->fh_Arg1;
    check(handler != NULL, "file handle publishes its packet handler port");
    if (!handler)
        goto cleanup;
    reply = CreateMsgPort();
    packet = AllocVec(sizeof(*packet), MEMF_PUBLIC | MEMF_CLEAR);
    buffer = AllocVec(sizeof(*buffer), MEMF_PUBLIC | MEMF_CLEAR);
    check(reply != NULL && packet != NULL && buffer != NULL,
          "allocate an owned reply port, packet and guarded public buffer");
    if (!reply || !packet || !buffer)
        goto cleanup;
    check(reply->mp_SigBit < 32, "reply port signal bit is in range");
    if (reply->mp_SigBit >= 32)
        goto cleanup;

    n = Read(handle, direct, sizeof(direct));
    old = Seek(handle, 0, OFFSET_BEGINNING);
    check(n == sizeof(direct) && memcmp(direct, fixture, sizeof(direct)) == 0 &&
          old == sizeof(direct), "ordinary Read and Seek share the file position");
    if (old == -1 || n != sizeof(direct))
        goto cleanup;
    if (!packet_read(handler, key, reply, packet, buffer, 1, 5, 0, 5, probe))
        goto cleanup;
    if (!packet_read(handler, key, reply, packet, buffer, 2, 9, 5, 7, probe))
        goto cleanup;
    if (!packet_read(handler, key, reply, packet, buffer, 3, 4, 12, 0, probe))
        goto cleanup;
    pos = Seek(handle, 0, OFFSET_CURRENT);
    n = Read(handle, direct, 1);
    check(pos == sizeof(fixture) && n == 0,
          "packet reads and ordinary DOS calls share position and EOF");
    ok = TRUE;
    if (probe)
        Printf("PACKET_APIS pos=%ld eof=%ld completed=%ld\n", pos, n, (LONG)ok);
cleanup:
    if (buffer) FreeVec(buffer);
    if (packet) FreeVec(packet);
    if (reply)
    {
        if (reply->mp_SigBit < 32)
            SetSignal(0, 1UL << reply->mp_SigBit);
        DeleteMsgPort(reply);
    }
    if (handle)
        check(Close(handle) != 0, "Close releases the fixture handle");
    if (probe)
        PutStr((CONST_STRPTR)"PACKETS_PROBE_DONE\n");
    return test_done("exec_packets");
}
