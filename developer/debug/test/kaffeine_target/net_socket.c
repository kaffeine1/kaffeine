/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Loopback-only contracts of the typed AmiTCP bridge. */
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
static struct Library *parent_base;
static struct Task *parent;
static ULONG done_mask;
static volatile LONG client_done, receiver_done;
static volatile ULONG transfer_id;
static struct KSocketIn endpoint;
static const char request[] = "typed-socket-request", answer[] = "socket-reply", moved[] = "transferred";
struct JumpSegment { BPTR next; UWORD jump; ULONG entry; } __attribute__((packed,aligned(4)));
static struct JumpSegment client_segment, receiver_segment;

static BOOL read_all(struct Library *SocketBase, LONG fd, char *buffer, LONG length)
{
    LONG n;
    while (length > 0)
    {
        n = recv(fd, buffer, length, 0);
        if (n <= 0) return FALSE;
        buffer += n; length -= n;
    }
    return TRUE;
}

static void timeout(struct Library *SocketBase, LONG fd)
{
    struct KSocketTime tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static LONG client(void)
{
    struct Library *SocketBase = OpenLibrary("bsdsocket.library", 4);
    struct Library *saved = SocketBase;
    LONG fd = -1, wrong;
    char buffer[sizeof(answer)];
    if (!SocketBase) goto done;
    SocketBase = parent_base;
    wrong = socket(AF_INET, SOCK_STREAM, 0); /* opener identity must be enforced */
    SocketBase = saved;
    if (wrong != -1) goto close;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) goto close;
    timeout(SocketBase, fd);
    if (connect(fd, (struct KSocketAddress *)&endpoint, sizeof(endpoint)) < 0) goto close;
    if (send(fd, request, sizeof(request), 0) != sizeof(request)) goto close;
    if (!read_all(SocketBase, fd, buffer, sizeof(answer)) || memcmp(buffer, answer, sizeof(answer))) goto close;
    if (send(fd, moved, sizeof(moved), 0) != sizeof(moved)) goto close;
    client_done = 1;
close:
    if (fd >= 0) CloseSocket(fd);
    CloseLibrary(SocketBase);
done:
    if (!client_done) client_done = -1;
    Signal(parent, done_mask);
    return 0;
}

static LONG receiver(void)
{
    struct Library *SocketBase = OpenLibrary("bsdsocket.library", 4);
    LONG fd = -1;
    char buffer[sizeof(moved)];
    if (SocketBase)
    {
        fd = ObtainSocket(transfer_id, AF_INET, SOCK_STREAM, 0);
        if (fd >= 0)
        {
            timeout(SocketBase, fd);
            if (read_all(SocketBase, fd, buffer, sizeof(buffer)) && !memcmp(buffer, moved, sizeof(buffer)))
                receiver_done = 1;
            CloseSocket(fd);
        }
        CloseLibrary(SocketBase);
    }
    if (!receiver_done) receiver_done = -1;
    Signal(parent, done_mask);
    return 0;
}

int main(void)
{
    struct Library *other;
    struct MsgPort *child, *reader;
    LONG server = -1, accepted = -1, second, one = 1, length, got, bit, cycles;
    KSocketSet set;
    struct KSocketTime zero = {0, 0}, tv = {3, 0};
    ULONG wanted, received;
    unsigned char tail[sizeof(KSocketSet)];
    char buffer[sizeof(request)];
    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (!check(SocketBase != NULL, "opens typed bsdsocket.library v4")) return test_done("net_socket");
    parent_base = SocketBase; parent = FindTask(NULL);
    check(SocketBase->lib_Version >= 4, "guest base has a classic Library version");
    check(!OpenLibrary("bsdsocket.library", 5), "unsupported later version takes no reference");
    other = OpenLibrary("bsdsocket.library", 4);
    check(other && other == SocketBase, "repeated task opens share one base and descriptor namespace");
    if (other)
    {
        SocketBase = other; second = socket(AF_INET, SOCK_DGRAM, 0);
        Printf("observed second fd=%ld errno=%ld\n", second, Errno()); test_flush();
        check(second == 0, "native global descriptor maps to first classic descriptor zero");
        CloseLibrary(other); SocketBase = parent_base;
        if (second >= 0)
        {
            length = sizeof(endpoint);
            check(getsockname(second, (struct KSocketAddress *)&endpoint, &length) == 0,
                  "closing one reference preserves the shared socket");
            CloseSocket(second);
        }
    }
    server = socket(AF_INET, SOCK_STREAM, 0);
    Printf("observed first fd=%ld errno=%ld\n", server, Errno()); test_flush();
    if (!check(server == 0, "closed classic descriptor zero is reusable")) goto end;
    check(setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0, "converts a big-endian integer socket option");
    memset(&endpoint, 0, sizeof(endpoint)); endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(bind(server, (struct KSocketAddress *)&endpoint, sizeof(endpoint)) == 0, "binds loopback with a classic sockaddr_in");
    length = sizeof(endpoint);
    check(getsockname(server, (struct KSocketAddress *)&endpoint, &length) == 0 && length == 16 && endpoint.sin_port,
          "returns assigned network-order port and big-endian length");
    if (!check(listen(server, 2) == 0, "listens on an ephemeral loopback port")) goto end;
    check(IoctlSocket(server, FIONBIO, (char *)&one) == 0, "maps classic FIONBIO independently of LP64 request size");
    check(accept(server, NULL, NULL) == -1 && Errno() == EWOULDBLOCK, "nonblocking accept reports classic errno");
    one = 0; check(IoctlSocket(server, FIONBIO, (char *)&one) == 0, "restores logical blocking mode");
    memset(&set, 0xa5, sizeof(set)); memset(&set, 0, 8); memcpy(tail, &set, sizeof(set));
    got = WaitSelect(256, &set, NULL, NULL, &zero, NULL);
    check(got == 0 && !memcmp((char *)&set + 8, tail + 8, sizeof(set) - 8), "select clamps nfds and preserves the unused classic set tail");
    check(WaitSelect(1, (KSocketSet *)0xfffffff0UL, NULL, NULL, &zero, NULL) == -1 && Errno() == EFAULT,
          "rejects an overflowing guest fd_set before native access");
    bit = AllocSignal(-1);
    if (!check(bit >= 0, "allocates an owned signal for select and process completion")) goto end;
    done_mask = 1UL << bit;
    SetSignal(done_mask, done_mask); wanted = done_mask;
    got = WaitSelect(0, NULL, NULL, NULL, NULL, &wanted);
    check(got == 0 && wanted == done_mask && !(SetSignal(0, 0) & done_mask), "select immediately consumes a pending requested signal");
    wanted = 0; SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
    got = WaitSelect(0, NULL, NULL, NULL, &zero, &wanted);
    received = SetSignal(0, SIGBREAKF_CTRL_C);
    check(got == -1 && Errno() == EINTR && (received & SIGBREAKF_CTRL_C), "unrequested break interrupts select and stays pending");
    client_segment.next = 0; client_segment.jump = 0x4ef9; client_segment.entry = (ULONG)client;
    child = CreateProc((CONST_STRPTR)"socket-fixture-client", 0, (BPTR)((ULONG)&client_segment >> 2), 16384);
    if (!check(child != NULL, "creates an independent guest socket client")) { FreeSignal(bit); goto end; }
    KFD_ZERO(&set); KFD_SET(server, &set);
    check(WaitSelect(server + 1, &set, NULL, NULL, &tv, NULL) == 1 && KFD_ISSET(server, &set),
          "select lets another guest process connect while the owner waits");
    accepted = accept(server, NULL, NULL);
    if (!check(accepted >= 0, "accepts the guest process connection")) { FreeSignal(bit); goto end; }
    timeout(SocketBase, accepted);
    check(read_all(SocketBase, accepted, buffer, sizeof(buffer)) && !memcmp(buffer, request, sizeof(buffer)), "receives exact bytes from the other guest process");
    check(send(accepted, answer, sizeof(answer), 0) == sizeof(answer), "sends exact response bytes");
    transfer_id = ReleaseSocket(accepted, -1);
    check(transfer_id >= 65536 && transfer_id != (ULONG)-1, "releases into a domain-owned unique ID");
    accepted = -1;
    receiver_segment.next = 0; receiver_segment.jump = 0x4ef9; receiver_segment.entry = (ULONG)receiver;
    reader = CreateProc((CONST_STRPTR)"socket-fixture-receiver", 0, (BPTR)((ULONG)&receiver_segment >> 2), 16384);
    check(reader != NULL, "creates a new owner for the released connection");
    while (!client_done || (reader && !receiver_done)) Wait(done_mask);
    check(client_done == 1, "client exchanges all bytes and closes its private base");
    check(reader && receiver_done == 1, "another process obtains the socket and reads exact buffered bytes");
    FreeSignal(bit);
end:
    if (accepted >= 0) CloseSocket(accepted);
    if (server >= 0) CloseSocket(server);
    CloseLibrary(SocketBase);
    for (cycles = 0; cycles < 40; ++cycles)
    {
        SocketBase = OpenLibrary("bsdsocket.library", 4);
        if (!SocketBase || SocketBase->lib_OpenCnt != 1) break;
        CloseLibrary(SocketBase);
    }
    check(cycles == 40, "closed task bases are safely reused across forty open/close cycles");
    return test_done("net_socket");
}
