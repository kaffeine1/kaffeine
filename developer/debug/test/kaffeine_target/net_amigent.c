/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Protocol test of the unmodified released Amigent 1.1.1 daemon.
   Loopback only; no SETUP, screen request, input, EXEC or target hardware. */
#include <exec/types.h>
#include <exec/tasks.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "socket_fixture_abi.h"
#include <string.h>
#include "m68ktest.h"

struct Library *SocketBase;
static const char daemon_name[] = "amigent-loopback-fixture";
static const char key[] = "public-loopback-fixture-key";
static const char proof_path[] = "SYS:EMU68/socket-proof.txt";
static const char proof[] = "exact Amigent channel bytes\n";

static BOOL read_all(LONG fd, void *data, LONG length)
{
    char *p = data;
    while (length)
    {
        LONG n = recv(fd, p, length, 0);
        if (n <= 0) return FALSE;
        p += n; length -= n;
    }
    return TRUE;
}

static BOOL command(LONG fd, UBYTE which, UWORD x)
{
    UBYTE header[10];
    memset(header, 0, sizeof(header)); header[0] = which;
    header[2] = x >> 8; header[3] = x;
    return send(fd, header, sizeof(header), 0) == sizeof(header);
}

static LONG connection(void)
{
    struct KSocketIn address;
    struct KSocketTime tv = {3, 0};
    LONG fd, tries;
    memset(&address, 0, sizeof(address)); address.sin_family = AF_INET;
    address.sin_port = htons(44125); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (tries = 0; tries < 40; ++tries)
    {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (!connect(fd, (struct KSocketAddress *)&address, sizeof(address))) return fd;
        CloseSocket(fd); Delay(2);
    }
    return -1;
}

static BOOL hello(LONG fd)
{
    UBYTE bytes[8];
    return read_all(fd, bytes, sizeof(bytes)) && !memcmp(bytes, "AMGT", 4) &&
           bytes[4] == 0 && bytes[5] == 3 && (bytes[7] & 1);
}

static BOOL authenticate(LONG fd)
{
    UBYTE status = 255;
    return hello(fd) && command(fd, 17, sizeof(key) - 1) &&
           send(fd, key, sizeof(key) - 1, 0) == sizeof(key) - 1 &&
           read_all(fd, &status, 1) && status == 0;
}

int main(void)
{
    BPTR segment = 0;
    struct Process *daemon = NULL;
    struct Task *live;
    LONG fd = -1, channel = -1, tries, old_priority;
    UBYTE status = 255, size[4];
    char bytes[sizeof(proof)];
    ULONG length;
    old_priority = SetTaskPri(FindTask(NULL), -5);
    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (!check(SocketBase != NULL, "client opens the typed socket bridge")) goto end;
    segment = LoadSeg((CONST_STRPTR)"SYS:EMU68/amigentd");
    if (!check(segment != 0, "loads the original released Amigent daemon hunk")) goto end;
    daemon = CreateNewProcTags(NP_Seglist, segment, NP_FreeSeglist, TRUE,
        NP_Name, (ULONG)daemon_name, NP_CommandName, (ULONG)"amigentd",
        NP_Arguments, (ULONG)"PORT 44125 KEY SYS:EMU68/amigent-test.key QUIET\n",
        NP_Cli, TRUE, NP_StackSize, 65536, NP_CopyVars, FALSE, TAG_DONE);
    if (!check(daemon != NULL, "starts the unmodified daemon as an independent guest Process")) goto end;
    segment = 0; /* successful NP_FreeSeglist transfers our loaded reference */
    fd = connection();
    if (!check(fd >= 0, "original daemon listens on its dedicated loopback port")) goto end;
    check(hello(fd), "original protocol hello is v3 and requires the fixture key");
    status = 255;
    check(command(fd, 6, 0) && read_all(fd, &status, 1) && status == 1,
          "unauthenticated PING is refused");
    CloseSocket(fd); fd = connection();
    if (!check(fd >= 0 && authenticate(fd), "authenticates with the nonempty isolated fixture key")) goto end;
    status = 255;
    check(command(fd, 6, 0) && read_all(fd, &status, 1) && status == 0,
          "authenticated PING completes without requesting graphics or input");
    channel = connection();
    if (!check(channel >= 0 && authenticate(channel), "authenticates an independent command connection")) goto end;
    status = 255;
    if (!check(command(channel, 20, 0) && read_all(channel, &status, 1) && status == 0,
               "CHANNEL transfers the socket to the daemon's guest child")) goto end;
    status = 255; memset(bytes, 0, sizeof(bytes));
    if (command(channel, 22, sizeof(proof_path) - 1) &&
        send(channel, proof_path, sizeof(proof_path) - 1, 0) == sizeof(proof_path) - 1 &&
        read_all(channel, &status, 1) && status == 0 && read_all(channel, size, 4))
    {
        length = (ULONG)size[0] << 24 | (ULONG)size[1] << 16 | (ULONG)size[2] << 8 | size[3];
        check(length == sizeof(proof) - 1 && read_all(channel, bytes, length) &&
              !memcmp(bytes, proof, sizeof(proof) - 1), "READ returns every expected byte through the transferred socket");
    }
    else check(FALSE, "READ returns every expected byte through the transferred socket");
    status = 255;
    check(command(fd, 6, 0) && read_all(fd, &status, 1) && status == 0,
          "original main connection remains responsive beside the command child");
end:
    if (channel >= 0) CloseSocket(channel);
    if (fd >= 0) CloseSocket(fd);
    if (daemon)
    {
        live = FindTask((CONST_STRPTR)daemon_name);
        if (live) Signal(live, SIGBREAKF_CTRL_C);
        for (tries = 0; tries < 100 && FindTask((CONST_STRPTR)daemon_name); ++tries) Delay(1);
        check(FindTask((CONST_STRPTR)daemon_name) == NULL, "original daemon handles Ctrl-C and its children terminate");
    }
    if (segment && !FindTask((CONST_STRPTR)daemon_name)) UnLoadSeg(segment);
    if (SocketBase) CloseLibrary(SocketBase);
    SetTaskPri(FindTask(NULL), old_priority);
    return test_done("net_amigent");
}
