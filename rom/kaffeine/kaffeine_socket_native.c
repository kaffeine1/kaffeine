/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)

   SocketBase and descriptors belong exclusively to this native Process.
   A blocking guest operation is split into short native readiness waits;
   every reply lets the driver deliver the guest's own signals and schedule
   its other CPUs. No worker executes a guest instruction or sees its arena.
*/
#include <exec/memory.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#define __BSDSOCKET_NOLIBBASE__
#include <proto/bsdsocket.h>
#include <string.h>
#include "kaffeine_socket_native.h"
#include "kaffeine_socket_codec.h"

struct KSocketNative
{
    LONG fd[KSOCKET_FDS]; /* classic logical descriptor -> native global number */
    struct Task *task;
    struct MsgPort *inbox;
    struct Library *keep;
    struct Message startup;
    BOOL opened;
};

static void ignored_port(struct MsgPort *port)
{
    memset(port, 0, sizeof(*port));
    port->mp_Node.ln_Type = NT_MSGPORT;
    port->mp_Flags = PA_IGNORE;
    NEWLIST(&port->mp_MsgList);
}

static uint64_t microseconds(struct Device *TimerBase)
{
    struct EClockVal clock;
    ULONG frequency = ReadEClock(&clock);
    uint64_t ticks = (uint64_t)clock.ev_hi << 32 | clock.ev_lo;
    if (frequency)
        return ticks / frequency * UINT64_C(1000000) +
               ticks % frequency * UINT64_C(1000000) / frequency;
    {
        struct timeval now;
        GetSysTime(&now);
        return (uint64_t)now.tv_secs * UINT64_C(1000000) + now.tv_micro;
    }
}

static BOOL transient(LONG error)
{
    return error == EWOULDBLOCK || error == EAGAIN;
}

static BOOL nonblock(struct Library *SocketBase, int fd)
{
    int one = 1;
    return IoctlSocket(fd, FIONBIO, (char *)&one) == 0;
}

/* All masks are constructed bit by bit; native fd_mask width is immaterial. */
static int readiness(struct Library *SocketBase, struct KSocketPacket *p,
                     int fd, BOOL writing, ULONG usec)
{
    size_t words;
    uint32_t *bits;
    struct timeval timeout;
    ULONG stop = SIGBREAKF_CTRL_C;
    int result;
    if (fd < 0 || fd >= 65536) { p->error = EBADF; return -1; }
    words = ((ULONG)fd + 32) / 32;
    bits = AllocVec(words * sizeof(*bits), MEMF_PUBLIC | MEMF_CLEAR);
    if (!bits) { p->error = ENOMEM; return -1; }
    KSocketNativeSet(bits, words, fd);
    timeout.tv_secs = 0; timeout.tv_micro = usec;
    result = WaitSelect(fd + 1, writing ? NULL : (fd_set *)bits,
                        writing ? (fd_set *)bits : NULL, NULL, &timeout, &stop);
    if (result < 0) p->error = Errno();
    FreeVec(bits);
    return result;
}

static void execute(struct KSocketNative *worker, struct Library *SocketBase, struct Device *TimerBase,
                    struct KSocketPacket *p)
{
    struct sockaddr_in address;
    socklen_t size = sizeof(address);
    int i, j, one = 1, value = 0;
    LONG logical = p->arg[0], fd = logical, slot = -1;
    p->again = FALSE;
    p->result = -1;
    p->error = 0;
    if (p->op == 30 || p->op == 48 || p->op == 144)
    {
        for (i = 0; i < KSOCKET_FDS; ++i)
            if (worker->fd[i] < 0) { slot = i; break; }
        if (slot < 0) { p->error = EMFILE; return; }
    }
    if (p->op != 30 && p->op != 126 && p->op != 144 && p->op < 0x1000)
    {
        if (logical < 0 || logical >= KSOCKET_FDS || worker->fd[logical] < 0)
        { p->error = EBADF; return; }
        fd = worker->fd[logical];
    }
    memset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    memcpy(&address.sin_port, p->address + 2, 2);
    memcpy(&address.sin_addr, p->address + 4, 4);
    if (p->has_timeout && p->op != 126 && !p->timed)
    {
        p->deadline = microseconds(TimerBase) + p->duration;
        p->timed = TRUE;
    }
    switch (p->op)
    {
    case 30: /* socket */
        p->result = socket(fd, p->arg[1], p->arg[2]);
        if (p->result >= 0)
        {
            if (IoctlSocket(p->result, FIONBIO, (char *)&one) < 0)
            { p->error = Errno(); CloseSocket(p->result); p->result = -1; }
        }
        break;
    case 36: p->result = bind(fd, (struct sockaddr *)&address, sizeof(address)); break;
    case 42: p->result = listen(fd, p->arg[1]); break;
    case 48:
        p->result = accept(fd, (struct sockaddr *)&address, &size);
        if (p->result >= 0)
        {
            if (!nonblock(SocketBase, p->result))
            { p->error = Errno(); CloseSocket(p->result); p->result = -1; }
        }
        break;
    case 54:
        if (!p->connecting)
        {
            p->result = connect(fd, (struct sockaddr *)&address, sizeof(address));
            p->error = p->result < 0 ? Errno() : 0;
            if (p->result < 0 && (p->error == EINPROGRESS || transient(p->error)))
                p->connecting = TRUE;
            else break;
            if (p->nonblocking) break;
        }
        i = readiness(SocketBase, p, fd, TRUE, 20000);
        if (i > 0)
        {
            size = sizeof(value);
            p->result = getsockopt(fd, SOL_SOCKET, SO_ERROR, &value, &size);
            p->error = p->result < 0 ? Errno() : value;
            if (p->error) p->result = -1;
        }
        else if (i == 0)
        {
            p->again = !p->has_timeout || microseconds(TimerBase) < p->deadline;
            p->error = p->again ? EINPROGRESS : EWOULDBLOCK;
        }
        return;
    case 66:
        p->result = send(fd, p->buffer ? p->buffer + p->progress : NULL,
                         p->length - p->progress, p->arg[1]);
        if (p->result >= 0 && !p->nonblocking)
        {
            p->progress += p->result;
            if (p->progress < p->length) p->again = TRUE;
            p->result = p->progress;
        }
        break;
    case 78: p->result = recv(fd, p->buffer, p->length, p->arg[1]); break;
    case 84: p->result = shutdown(fd, p->arg[1]); break;
    case 90:
        if (p->option_length == 8)
        {
            struct linger linger;
            linger.l_onoff = (LONG)KSocketRead32(p->option);
            linger.l_linger = (LONG)KSocketRead32(p->option + 4);
            p->result = setsockopt(fd, p->arg[1], p->arg[2], &linger, sizeof(linger));
        }
        else
        {
            value = (LONG)KSocketRead32(p->option);
            p->result = setsockopt(fd, p->arg[1], p->arg[2], &value, sizeof(value));
        }
        break;
    case 96:
        size = sizeof(value);
        p->result = getsockopt(fd, p->arg[1], p->arg[2], &value, &size);
        if (p->result == 0) { KSocketWrite32(p->option, value); p->option_length = 4; }
        break;
    case 102: p->result = getsockname(fd, (struct sockaddr *)&address, &size); break;
    case 108: p->result = getpeername(fd, (struct sockaddr *)&address, &size); break;
    case 114: /* FIONREAD; FIONBIO is a logical driver flag */
        p->result = IoctlSocket(fd, FIONREAD, (char *)&value);
        if (p->result == 0) KSocketWrite32(p->option, value);
        break;
    case 120:
        p->result = CloseSocket(fd);
        if (p->result == 0) worker->fd[logical] = -1;
        break;
    case 126:
    {
        uint32_t *sets;
        size_t words;
        int highest = 0;
        struct timeval timeout;
        uint64_t now = microseconds(TimerBase), remaining;
        ULONG stop = SIGBREAKF_CTRL_C;
        if (!p->timed)
        {
            p->deadline = now + p->duration;
            p->timed = TRUE;
        }
        remaining = !p->has_timeout ? 20000 : now >= p->deadline ? 0 : p->deadline - now;
        if (remaining > 20000) remaining = 20000;
        timeout.tv_secs = 0; timeout.tv_micro = (ULONG)remaining;
        for (i = 0; i < 3; ++i)
        {
            memset(p->ready[i], 0, sizeof(p->ready[i]));
            for (j = 0; j < fd; ++j)
                if (p->has_set[i] && KSocketIsSet(p->sets[i], j))
                {
                    int native = worker->fd[j];
                    if (native < 0 || native >= 65536) { p->error = EBADF; return; }
                    if (native + 1 > highest) highest = native + 1;
                }
        }
        words = ((ULONG)highest + 31) / 32;
        if (!words) words = 1;
        sets = AllocVec(3 * words * sizeof(*sets), MEMF_PUBLIC | MEMF_CLEAR);
        if (!sets) { p->error = ENOMEM; return; }
        for (i = 0; i < 3; ++i)
            for (j = 0; j < fd; ++j)
                if (p->has_set[i] && KSocketIsSet(p->sets[i], j))
                    KSocketNativeSet(sets + i * words, words, worker->fd[j]);
        p->result = WaitSelect(highest, p->has_set[0] ? (fd_set *)sets : NULL,
                              p->has_set[1] ? (fd_set *)(sets + words) : NULL,
                              p->has_set[2] ? (fd_set *)(sets + 2 * words) : NULL, &timeout, &stop);
        p->error = p->result < 0 ? Errno() : 0;
        if (p->result > 0)
            for (i = 0; i < 3; ++i)
                for (j = 0; j < fd; ++j)
                    if (p->has_set[i] && KSocketIsSet(p->sets[i], j) &&
                        KSocketNativeIsSet(sets + i * words, words, worker->fd[j]))
                        KSocketSet(p->ready[i], j);
        FreeVec(sets);
        if (p->result == 0 && (!p->has_timeout || microseconds(TimerBase) < p->deadline))
            p->again = TRUE;
        return;
    }
    case 144:
        p->result = ObtainSocket(fd, p->arg[1], p->arg[2], p->arg[3]);
        if (p->result >= 0 && !nonblock(SocketBase, p->result))
        { p->error = Errno(); CloseSocket(p->result); p->result = -1; }
        break;
    case 150:
        p->result = ReleaseSocket(fd, -1);
        if (p->result >= 0) worker->fd[logical] = -1;
        break;
    case 0x1000: /* reclaim one domain-owned released socket during cleanup */
        i = ObtainSocket(fd, p->arg[1], p->arg[2], p->arg[3]);
        p->result = i < 0 ? -1 : CloseSocket(i);
        break;
    case 0x1001:
        for (i = 0; i < KSOCKET_FDS; ++i)
            if (worker->fd[i] >= 0) { CloseSocket(worker->fd[i]); worker->fd[i] = -1; }
        p->result = 0;
        break;
    default: p->error = ENOSYS; return;
    }
    if (p->result < 0 && !p->error) p->error = Errno();
    /* A blocking send that already moved bytes reports them, as BSD send()
       does; a lasting error such as EPIPE comes back at the next call */
    if (p->op == 66 && p->result < 0 && p->progress && !transient(p->error))
    {
        p->result = p->progress;
        p->error = 0;
    }
    if (p->result >= 0 && slot >= 0)
    {
        if (p->result >= 65536)
        { CloseSocket(p->result); p->result = -1; p->error = EMFILE; }
        else { worker->fd[slot] = p->result; p->result = slot; }
    }
    if (p->result >= 0 && (p->op == 48 || p->op == 102 || p->op == 108))
    {
        memset(p->address, 0, sizeof(p->address));
        p->address[0] = 16; p->address[1] = AF_INET;
        memcpy(p->address + 2, &address.sin_port, 2);
        memcpy(p->address + 4, &address.sin_addr, 4);
    }
    if (p->again && p->has_timeout && microseconds(TimerBase) >= p->deadline)
        p->again = FALSE;
    if (p->result < 0 && transient(p->error) && !p->nonblocking &&
        (p->op == 48 || p->op == 66 || p->op == 78))
    {
        uint64_t now = microseconds(TimerBase), remaining = 20000;
        if (p->has_timeout)
        {
            remaining = now >= p->deadline ? 0 : p->deadline - now;
            if (remaining > 20000) remaining = 20000;
        }
        if (p->has_timeout && !remaining)
        {
            if (p->progress) { p->result = p->progress; p->error = 0; }
        }
        else
        {
            i = readiness(SocketBase, p, fd, p->op == 66, remaining);
            if (i >= 0) p->again = TRUE;
        }
    }
}

static LONG worker(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    struct KSocketNative *w = me->pr_Task.tc_UserData;
    struct Library *SocketBase = OpenLibrary("bsdsocket.library", 4);
    struct MsgPort *timer_port = CreateMsgPort();
    struct timerequest *timer = timer_port ?
        (struct timerequest *)CreateIORequest(timer_port, sizeof(*timer)) : NULL;
    struct Device *TimerBase = NULL;
    struct Message *stop = NULL, *msg;
    if (timer && !OpenDevice("timer.device", UNIT_MICROHZ, &timer->tr_node, 0))
        TimerBase = timer->tr_node.io_Device;
    w->task = FindTask(NULL);
    w->inbox = CreateMsgPort();
    w->opened = SocketBase && TimerBase && w->inbox;
    if (SocketBase) SetSocketSignals(SIGBREAKF_CTRL_C, 0, 0);
    if (w->opened)
    {
        ReplyMsg(&w->startup);
        for (;;)
        {
            WaitPort(w->inbox);
            msg = GetMsg(w->inbox);
            if (!msg) continue;
            if (msg->mn_Length == sizeof(struct Message)) { stop = msg; break; }
            SetSignal(0, SIGBREAKF_CTRL_C);
            execute(w, SocketBase, TimerBase, (struct KSocketPacket *)msg);
            ReplyMsg(msg);
        }
    }
    if (SocketBase) CloseLibrary(SocketBase);
    if (TimerBase) CloseDevice(&timer->tr_node);
    if (timer) DeleteIORequest(&timer->tr_node);
    if (timer_port) DeleteMsgPort(timer_port);
    if (w->inbox) DeleteMsgPort(w->inbox);
    me->pr_Task.tc_UserData = NULL;
    CloseLibrary(w->keep);
    /* No access to w after the final reply permits the driver to free it. */
    if (stop) ReplyMsg(stop);
    else ReplyMsg(&w->startup); /* failed start: cleanup completed first */
    return 0;
}

struct KSocketNative *KSocketNativeStart(void)
{
    struct KSocketNative *w = AllocVec(sizeof(*w), MEMF_PUBLIC | MEMF_CLEAR);
    struct MsgPort reply;
    unsigned i;
    if (!w) return NULL;
    for (i = 0; i < KSOCKET_FDS; ++i) w->fd[i] = -1;
    w->keep = OpenLibrary("kaffeine.library", 0);
    if (!w->keep) { FreeVec(w); return NULL; }
    ignored_port(&reply);
    w->startup.mn_ReplyPort = &reply;
    w->startup.mn_Length = sizeof(w->startup);
    if (!CreateNewProcTags(NP_Entry, (IPTR)worker, NP_UserData, (IPTR)w,
                          NP_Name, (IPTR)"Kaffeine socket worker", NP_StackSize, 65536,
                          NP_Input, (IPTR)0, NP_Output, (IPTR)0,
                          NP_CloseInput, FALSE, NP_CloseOutput, FALSE, TAG_DONE))
    { CloseLibrary(w->keep); FreeVec(w); return NULL; }
    while (GetMsg(&reply) != &w->startup) Delay(1);
    if (!w->opened)
    {
        FreeVec(w);
        return NULL;
    }
    return w;
}

void KSocketNativePost(struct KSocketNative *w, struct KSocketPacket *p)
{
    p->message.mn_Length = sizeof(*p);
    PutMsg(w->inbox, &p->message);
}

void KSocketNativeCancel(struct KSocketNative *w)
{
    Signal(w->task, SIGBREAKF_CTRL_C);
}

void KSocketNativeStop(struct KSocketNative *w)
{
    struct MsgPort reply;
    struct Message stop;
    ignored_port(&reply);
    memset(&stop, 0, sizeof(stop));
    stop.mn_Length = sizeof(stop); stop.mn_ReplyPort = &reply;
    PutMsg(w->inbox, &stop);
    while (GetMsg(&reply) != &stop) Delay(1);
    FreeVec(w);
}
