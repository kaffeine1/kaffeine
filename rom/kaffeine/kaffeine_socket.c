/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)

   Typed AmiTCP v4 bridge. The driver alone reads/writes guest memory.
   Native processes own socket bases and receive copied packets only.
*/
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include "kaffeine_socket.h"
#include "kaffeine_socket_native.h"
#include "kaffeine_socket_codec.h"
#include "kaffeine_thunks.h"
#include "kaffeine_sched.h"
#include "kaffeine_messages.h"
#include "kaffeine_offsets.h"

#define SOCKET_OWNERS 32
#define SOCKET_TRANSFERS 64
struct KSocketFD { BOOL used, nonblocking; LONG family, type, protocol;
                   uint64_t receive_timeout, send_timeout; };
struct KSocketOwner
{
    struct KSocketOwner *next;
    ULONG task, base, opens, errno_address, errno_size;
    LONG error;
    BOOL retired, pending, ready;
    struct KSocketNative *native;
    struct KSocketPacket packet;
    struct KSocketFD fd[KSOCKET_FDS], transfer_fd;
    ULONG destination, length_address, set_address[3], signals_address, signal_mask;
    ULONG transfer_id;
    unsigned transfer_slot;
};
struct KSocketTransfer
{
    ULONG id;
    BOOL used;
    LONG native;
    struct KSocketFD fd;
    struct KSocketOwner *reserved;
};
struct KaffeineSockets
{
    struct MsgPort *reply;
    struct KSocketOwner *owners;
    struct KSocketTransfer transfers[SOCKET_TRANSFERS];
    ULONG base, serial, owner_count;
    BOOL closing;
};

static IPTR invoke(struct KaffeineContext *, void *, ULONG);

static ULONG caller(struct KaffeineContext *ctx, void *cpu)
{
    return ctx->sched && ctx->task_ops && ctx->task_ops->caller_task ?
        ctx->task_ops->caller_task(ctx, cpu) : ctx->guest_exec_task;
}

static BOOL storage(struct KaffeineContext *ctx, ULONG address, ULONG bytes)
{
    return KSocketRange(ctx->mem_size, address, bytes) &&
           Kaffeine_MessageMemory(ctx, address, bytes);
}

static struct KSocketOwner *owner(struct KaffeineContext *ctx, void *cpu)
{
    struct KSocketOwner *w;
    ULONG task = caller(ctx, cpu);
    if (!ctx->sockets || !task || ctx->broker_call || FindTask(NULL) != ctx->exec_task)
        return NULL;
    for (w = ctx->sockets->owners; w; w = w->next)
        if (!w->retired && w->task == task && w->base == Kaffeine_GetA(cpu, 6)) return w;
    return NULL;
}

static IPTR error(struct KaffeineContext *ctx, struct KSocketOwner *w, LONG code)
{
    if (w)
    {
        w->error = code;
        if (w->errno_address)
        {
            if (!storage(ctx, w->errno_address, w->errno_size)) w->errno_address = 0;
            else if (w->errno_size == 1) ctx->mem[w->errno_address] = (UBYTE)code;
            else if (w->errno_size == 2) m68k_write16(ctx, w->errno_address, code);
            else m68k_write32(ctx, w->errno_address, code);
        }
    }
    return (IPTR)(LONG)-1;
}

static BOOL poll_replies(struct KaffeineContext *ctx)
{
    struct KaffeineSockets *s = ctx->sockets;
    struct Message *message;
    struct KSocketOwner *w;
    if (!s || FindTask(NULL) != ctx->exec_task || ctx->broker_call) return !s;
    while ((message = GetMsg(s->reply)))
    {
        for (w = s->owners; w && message != &w->packet.message; w = w->next) {}
        if (!w || !w->pending || w->ready) return FALSE;
        w->ready = TRUE;
        if (!s->closing && !w->retired && ctx->sched && ctx->task_ops->socket_ready)
            ctx->task_ops->socket_ready(ctx, &w->packet, w->task);
    }
    return TRUE;
}

static void post(struct KaffeineContext *ctx, struct KSocketOwner *w)
{
    w->ready = FALSE; w->pending = TRUE;
    w->packet.message.mn_ReplyPort = ctx->sockets->reply;
    KSocketNativePost(w->native, &w->packet);
}

static void await(struct KaffeineContext *ctx, struct KSocketOwner *w)
{
    while (!w->ready)
    {
        if (!poll_replies(ctx))
        { ctx->running = FALSE; ctx->callback_failed = TRUE; ctx->exit_code = 20; }
        if (!w->ready) Delay(1);
    }
}

static void clear_packet(struct KSocketOwner *w)
{
    if (w->packet.buffer) FreeVec(w->packet.buffer);
    memset(&w->packet, 0, sizeof(w->packet));
    w->pending = w->ready = FALSE;
}

/* Removal never writes the departed task's stack, errno or output buffers.
 * Keep the idle native owner until domain cleanup can reclaim released IDs. */
static void retire(struct KaffeineContext *ctx, ULONG task)
{
    struct KSocketOwner *w;
    unsigned i;
    if (!ctx->sockets) return;
    for (w = ctx->sockets->owners; w; w = w->next)
        if (w->task == task && !w->retired)
        {
            w->retired = TRUE; w->opens = 0; w->errno_address = 0;
            if (w->pending)
            {
                KSocketNativeCancel(w->native); await(ctx, w);
                /* A completed transfer must remain domain-owned even if its
                   guest caller departed before receiving the scalar result. */
                if (w->packet.op == 150 && w->packet.result >= 0)
                {
                    struct KSocketTransfer *t = &ctx->sockets->transfers[w->transfer_slot];
                    t->native = w->packet.result; t->fd = w->transfer_fd; t->reserved = NULL;
                }
                else if (w->packet.op == 144)
                {
                    struct KSocketTransfer *t = &ctx->sockets->transfers[w->transfer_slot];
                    if (w->packet.result >= 0) memset(t, 0, sizeof(*t));
                    else t->reserved = NULL;
                }
                clear_packet(w);
            }
            for (i = 0; i < SOCKET_TRANSFERS; ++i)
                if (ctx->sockets->transfers[i].reserved == w)
                    memset(&ctx->sockets->transfers[i], 0, sizeof(struct KSocketTransfer));
            memset(&w->packet, 0, sizeof(w->packet));
            w->packet.op = 0x1001; /* close this worker's entire descriptor table */
            post(ctx, w); await(ctx, w); clear_packet(w);
            memset(w->fd, 0, sizeof(w->fd));
        }
}

void Kaffeine_SocketCleanup(struct KaffeineContext *ctx)
{
    struct KaffeineSockets *s = ctx->sockets;
    struct KSocketOwner *w, *next;
    unsigned i;
    if (!s) return;
    s->closing = TRUE;
    for (w = s->owners; w; w = w->next) retire(ctx, w->task);
    w = s->owners;
    if (w)
        for (i = 0; i < SOCKET_TRANSFERS; ++i)
            if (s->transfers[i].used)
            {
                struct KSocketTransfer *t = &s->transfers[i];
                memset(&w->packet, 0, sizeof(w->packet));
                w->packet.op = 0x1000; w->packet.arg[0] = t->native;
                w->packet.arg[1] = t->fd.family; w->packet.arg[2] = t->fd.type;
                w->packet.arg[3] = t->fd.protocol;
                post(ctx, w); await(ctx, w); clear_packet(w);
            }
    for (w = s->owners; w; w = next)
    {
        next = w->next;
        KSocketNativeStop(w->native);
        FreeVec(w);
    }
    DeleteMsgPort(s->reply);
    FreeVec(s);
    ctx->sockets = NULL; ctx->socket_signals = 0;
    ctx->socket_poll = NULL; ctx->socket_cleanup = NULL; ctx->socket_task_removed = NULL;
}

ULONG Kaffeine_SocketOpen(struct KaffeineContext *ctx)
{
    struct KaffeineSockets *s = ctx->sockets;
    struct KSocketOwner *w;
    ULONG task = caller(ctx, ctx->cpu);
    if (!task || ctx->broker_call || FindTask(NULL) != ctx->exec_task) return 0;
    if (!s)
    {
        s = AllocVec(sizeof(*s), MEMF_PUBLIC | MEMF_CLEAR);
        if (!s) return 0;
        s->reply = CreateMsgPort();
        if (!s->reply) { FreeVec(s); return 0; }
        ctx->sockets = s; ctx->socket_signals = 1UL << s->reply->mp_SigBit;
        ctx->socket_poll = poll_replies; ctx->socket_cleanup = Kaffeine_SocketCleanup;
        ctx->socket_task_removed = retire;
    }
    if (s->closing) return 0;
    for (w = s->owners; w; w = w->next)
        if (!w->retired && w->task == task && w->opens)
        {
            if (w->opens == 65535) return 0;
            m68k_write16(ctx, w->base + M68K_LIB_OPENCNT, ++w->opens);
            return w->base;
        }
    /* The previous request and worker cleanup have acknowledged before a
       retired slot can be rebound. Reuse its vector identity/native owner,
       never a live guest reference. This bounds repeated channel opens. */
    for (w = s->owners; w; w = w->next)
        if (w->retired && !w->pending)
        {
            struct KSocketNative *native = w->native;
            struct KSocketOwner *next = w->next;
            ULONG base = w->base;
            memset(w, 0, sizeof(*w));
            w->native = native; w->next = next; w->base = base;
            w->task = task; w->opens = 1;
            m68k_write16(ctx, base + M68K_LIB_VERSION, 4);
            m68k_write16(ctx, base + M68K_LIB_OPENCNT, 1);
            return base;
        }
    if (s->owner_count >= SOCKET_OWNERS || ctx->num_libs >= KAFFEINE_MAX_LIBS) return 0;
    w = AllocVec(sizeof(*w), MEMF_PUBLIC | MEMF_CLEAR);
    if (!w) return 0;
    w->native = KSocketNativeStart();
    if (!w->native) { FreeVec(w); return 0; }
    w->base = Kaffeine_SetupLibBaseSized(ctx, ctx->num_libs, "bsdsocket.library",
                                   KAFFEINE_MAX_LVO, M68K_SIZEOF_LIBRARY + 18, kaffeine_thunks_socket,
                                   kaffeine_thunks_socket_count, NULL, 0);
    if (!w->base) { KSocketNativeStop(w->native); FreeVec(w); return 0; }
    w->task = task; w->next = s->owners; s->owners = w; ++s->owner_count;
    m68k_write16(ctx, w->base + 20, 4);
    m68k_write16(ctx, w->base + 22, 0);
    ctx->mem[w->base + 8] = NT_LIBRARY;
    m68k_write32(ctx, w->base + 10, w->base + M68K_SIZEOF_LIBRARY);
    memcpy(ctx->mem + w->base + M68K_SIZEOF_LIBRARY, "bsdsocket.library", 18);
    m68k_write16(ctx, w->base + 16, KAFFEINE_MAX_LVO * 6);
    m68k_write16(ctx, w->base + 18, M68K_SIZEOF_LIBRARY + 18);
    m68k_write16(ctx, w->base + M68K_LIB_OPENCNT, 1);
    w->opens = 1;
    return w->base;
}

BOOL Kaffeine_SocketClose(struct KaffeineContext *ctx, ULONG base)
{
    struct KSocketOwner *w;
    unsigned fd;
    if (!ctx->sockets) return FALSE;
    for (w = ctx->sockets->owners; w && w->base != base; w = w->next) {}
    if (!w) return FALSE;
    if (w->task != caller(ctx, ctx->cpu)) return TRUE;
    if (!w || !w->opens) return TRUE;
    m68k_write16(ctx, w->base + M68K_LIB_OPENCNT, --w->opens);
    if (w->opens) return TRUE;
    w->errno_address = 0;
    for (fd = 0; fd < KSOCKET_FDS; ++fd)
        if (w->fd[fd].used)
        {
            memset(&w->packet, 0, sizeof(w->packet));
            w->packet.op = 120; w->packet.arg[0] = fd;
            post(ctx, w); await(ctx, w); clear_packet(w);
            memset(&w->fd[fd], 0, sizeof(w->fd[fd]));
        }
    w->retired = TRUE;
    return TRUE;
}

static ULONG signals(struct KaffeineContext *ctx, void *cpu, ULONG mask)
{
    if (ctx->sched && ctx->task_ops->socket_take_signals)
        return ctx->task_ops->socket_take_signals(ctx, cpu, mask);
    return SetSignal(0, mask & ~ctx->socket_signals) & mask;
}

static BOOL option_supported(LONG level, LONG opt, BOOL getting)
{
    if (level == IPPROTO_TCP) return opt == TCP_NODELAY;
    if (level != SOL_SOCKET) return FALSE;
    if (getting && (opt == SO_TYPE || opt == SO_ERROR)) return TRUE;
    return opt == SO_REUSEADDR || opt == SO_KEEPALIVE || opt == SO_BROADCAST ||
           opt == SO_OOBINLINE || opt == SO_SNDBUF || opt == SO_RCVBUF ||
           opt == SO_SNDLOWAT || opt == SO_RCVLOWAT;
}

static LONG prepare(struct KaffeineContext *ctx, void *cpu, struct KSocketOwner *w, ULONG op)
{
    struct KSocketPacket *p = &w->packet;
    struct KaffeineSockets *s = ctx->sockets;
    ULONG address = THUNK_A(0), size, i;
    LONG fd = (LONG)THUNK_D(0);
    memset(p, 0, sizeof(*p)); p->op = op;
    for (i = 0; i < 4; ++i) p->arg[i] = (LONG)Kaffeine_GetD(cpu, i);
    w->destination = w->length_address = w->signals_address = w->signal_mask = 0;
    w->transfer_slot = SOCKET_TRANSFERS;
    if (op == 30)
    {
        if (fd != AF_INET) return EAFNOSUPPORT;
        if (p->arg[1] != SOCK_STREAM && p->arg[1] != SOCK_DGRAM) return EPROTONOSUPPORT;
        return 0;
    }
    if (op == 126)
    {
        if ((ULONG)fd > KSOCKET_FDS) fd = KSOCKET_FDS;
        p->arg[0] = fd;
        size = KSocketSetBytes(fd);
        for (i = 0; i < 3; ++i)
        {
            ULONG a = Kaffeine_GetA(cpu, i), j;
            w->set_address[i] = a; p->has_set[i] = a != 0;
            if (!a || !size) continue;
            if (!storage(ctx, a, size)) return EFAULT;
            memcpy(p->sets[i], ctx->mem + a, size);
            for (j = 0; j < (ULONG)fd; ++j)
                if (KSocketIsSet(p->sets[i], j) && !w->fd[j].used) return EBADF;
        }
        address = THUNK_A(3); p->has_timeout = address != 0;
        if (address)
        {
            ULONG sec, usec;
            if (!storage(ctx, address, 8)) return EFAULT;
            sec = m68k_read32(ctx, address); usec = m68k_read32(ctx, address + 4);
            if (sec > 100000000UL || usec >= 1000000) return EINVAL;
            p->duration = (uint64_t)sec * UINT64_C(1000000) + usec;
        }
        w->signals_address = THUNK_D(1);
        if (w->signals_address)
        {
            ULONG allowed = ctx->guest_signals | 0xf000;
            if (!storage(ctx, w->signals_address, 4)) return EFAULT;
            w->signal_mask = m68k_read32(ctx, w->signals_address);
            if (w->signal_mask & ~allowed) return EINVAL;
            if (ctx->sched && w->signal_mask && !ctx->task_ops->can_signal(ctx, w->task, w->signal_mask))
                return EINVAL;
        }
        return 0;
    }
    if (op == 144)
    {
        for (i = 0; i < SOCKET_TRANSFERS; ++i)
        {
            struct KSocketTransfer *t = &s->transfers[i];
            if (t->id != (ULONG)fd || !t->used || t->reserved) continue;
            if (p->arg[1] == 0)
            { if ((ULONG)fd < 65536) return EPFNOSUPPORT; }
            else if (p->arg[1] != t->fd.family || p->arg[2] != t->fd.type ||
                (p->arg[3] && t->fd.protocol && p->arg[3] != t->fd.protocol)) continue;
            t->reserved = w; w->transfer_slot = i; w->transfer_fd = t->fd;
            p->arg[0] = t->native;
            return 0;
        }
        return EWOULDBLOCK;
    }
    if (fd < 0 || fd >= KSOCKET_FDS || !w->fd[fd].used) return EBADF;
    p->nonblocking = w->fd[fd].nonblocking;
    if (op == 150)
    {
        ULONG requested = THUNK_D(1);
        if (requested == (ULONG)-1)
        {
            if (s->serial < 65536) s->serial = 65536;
            /* Auto IDs must skip explicit domain IDs, including wraparound. */
            for (;;)
            {
                BOOL busy = FALSE;
                if (++s->serial < 65536) s->serial = 65536;
                requested = s->serial;
                for (i = 0; i < SOCKET_TRANSFERS; ++i)
                    if (s->transfers[i].used && s->transfers[i].id == requested)
                    { busy = TRUE; break; }
                if (!busy) break;
            }
        }
        for (i = 0; i < SOCKET_TRANSFERS; ++i)
            if (requested >= 65536 && s->transfers[i].used && s->transfers[i].id == requested) return EINVAL;
        for (i = 0; i < SOCKET_TRANSFERS; ++i)
            if (!s->transfers[i].used)
            {
                s->transfers[i].id = requested; s->transfers[i].used = TRUE; s->transfers[i].reserved = w;
                w->transfer_id = requested; w->transfer_slot = i; w->transfer_fd = w->fd[fd];
                return 0;
            }
        return ENOBUFS;
    }
    if (op == 36 || op == 54)
    {
        if (THUNK_D(1) != 16 || !storage(ctx, address, 16)) return EFAULT;
        /* Native sockArgs derives sa_len from namelen, not guest sin_len. */
        if (ctx->mem[address + 1] != AF_INET) return EAFNOSUPPORT;
        memcpy(p->address, ctx->mem + address, 16);
    }
    else if (op == 48 || op == 102 || op == 108)
    {
        ULONG length = THUNK_A(1);
        if (op == 48 && !address && !length) return 0;
        if (!storage(ctx, length, 4)) return EFAULT;
        size = m68k_read32(ctx, length); if (size > 16) size = 16;
        if (!storage(ctx, address, size)) return EFAULT;
        w->destination = address; w->length_address = length; p->length = size;
    }
    else if (op == 66 || op == 78)
    {
        p->length = THUNK_D(1); p->arg[1] = (LONG)THUNK_D(2);
        p->duration = op == 66 ? w->fd[fd].send_timeout : w->fd[fd].receive_timeout;
        p->has_timeout = p->duration != 0;
        if (p->length > KSOCKET_MAX_BYTES) return EMSGSIZE;
        if (p->length && !storage(ctx, address, p->length)) return EFAULT;
        if (p->arg[1] & ~(MSG_PEEK | MSG_OOB | MSG_DONTROUTE)) return EOPNOTSUPP;
        if (p->length)
        {
            p->buffer = AllocVec(p->length, MEMF_PUBLIC | MEMF_CLEAR);
            if (!p->buffer) return ENOMEM;
            if (op == 66) memcpy(p->buffer, ctx->mem + address, p->length);
        }
        w->destination = address;
    }
    else if (op == 90 || op == 96)
    {
        if (p->arg[1] == SOL_SOCKET && (p->arg[2] == SO_RCVTIMEO || p->arg[2] == SO_SNDTIMEO))
        {
            uint64_t *limit = p->arg[2] == SO_RCVTIMEO ? &w->fd[fd].receive_timeout : &w->fd[fd].send_timeout;
            if (!storage(ctx, address, 8)) return EFAULT;
            if (op == 90)
            {
                ULONG sec = m68k_read32(ctx, address), usec = m68k_read32(ctx, address + 4);
                if (THUNK_D(3) != 8 || sec > 100000000UL || usec >= 1000000) return EINVAL;
                *limit = (uint64_t)sec * UINT64_C(1000000) + usec;
            }
            else
            {
                ULONG len = THUNK_A(1);
                if (!storage(ctx, len, 4) || m68k_read32(ctx, len) < 8) return EFAULT;
                m68k_write32(ctx, address, *limit / 1000000);
                m68k_write32(ctx, address + 4, *limit % 1000000);
                m68k_write32(ctx, len, 8);
            }
            p->op = 0;
        }
        else if (op == 90 && p->arg[1] == SOL_SOCKET && p->arg[2] == SO_LINGER)
        {
            if (THUNK_D(3) != 8 || !storage(ctx, address, 8)) return EFAULT;
            /* A positive linger would make close uninterruptible on some
               stacks. Refuse it explicitly rather than freezing the driver. */
            if (m68k_read32(ctx, address) && m68k_read32(ctx, address + 4)) return EOPNOTSUPP;
            memcpy(p->option, ctx->mem + address, 8); p->option_length = 8;
        }
        else
        {
            if (!option_supported(p->arg[1], p->arg[2], op == 96)) return ENOPROTOOPT;
            if (op == 90)
            {
                if (THUNK_D(3) != 4 || !storage(ctx, address, 4)) return EFAULT;
                memcpy(p->option, ctx->mem + address, 4); p->option_length = 4;
            }
            else
            {
                ULONG len = THUNK_A(1);
                if (!storage(ctx, len, 4) || m68k_read32(ctx, len) < 4 ||
                    !storage(ctx, address, 4)) return EFAULT;
                w->destination = address; w->length_address = len;
            }
        }
    }
    else if (op == 114)
    {
        if ((ULONG)p->arg[1] != UINT32_C(0x8004667e) && (ULONG)p->arg[1] != UINT32_C(0x4004667f))
            return ENOTTY;
        if (!storage(ctx, address, 4)) return EFAULT;
        if ((ULONG)p->arg[1] == UINT32_C(0x8004667e))
        { w->fd[fd].nonblocking = m68k_read32(ctx, address) != 0; p->op = 0; }
        else w->destination = address;
    }
    return 0;
}

static IPTR deliver(struct KaffeineContext *ctx, struct KSocketOwner *w)
{
    struct KSocketPacket *p = &w->packet;
    LONG result = p->result, code = p->error;
    ULONG op = p->op, i;
    if (result >= 0)
    {
        if (op == 30 || op == 48 || op == 144)
        {
            if (result >= KSOCKET_FDS) code = EMFILE;
            else
            {
                struct KSocketFD *fd = &w->fd[result];
                if (op == 30)
                { memset(fd, 0, sizeof(*fd)); fd->family = p->arg[0]; fd->type = p->arg[1]; fd->protocol = p->arg[2]; fd->nonblocking = FALSE; }
                else if (op == 48) { *fd = w->fd[p->arg[0]]; fd->nonblocking = w->fd[p->arg[0]].nonblocking; }
                else *fd = w->transfer_fd;
                fd->used = TRUE;
            }
        }
        if ((op == 48 || op == 102 || op == 108) && w->destination)
        {
            if (!storage(ctx, w->destination, p->length) || !storage(ctx, w->length_address, 4)) code = EFAULT;
            else
            { memcpy(ctx->mem + w->destination, p->address, p->length); m68k_write32(ctx, w->length_address, p->length); }
        }
        if (op == 78 && result)
        {
            if ((ULONG)result > p->length || !storage(ctx, w->destination, result)) code = EFAULT;
            else memcpy(ctx->mem + w->destination, p->buffer, result);
        }
        if (op == 96 || op == 114)
        {
            if (!storage(ctx, w->destination, 4) ||
                (op == 96 && !storage(ctx, w->length_address, 4))) code = EFAULT;
            else
            { memcpy(ctx->mem + w->destination, p->option, 4); if (op == 96) m68k_write32(ctx, w->length_address, 4); }
        }
        if (op == 120 || op == 150) memset(&w->fd[p->arg[0]], 0, sizeof(w->fd[0]));
    }
    if (op == 144 || op == 150)
    {
        struct KSocketTransfer *t = &ctx->sockets->transfers[w->transfer_slot];
        if (op == 150 && result >= 0)
        { t->native = result; t->fd = w->transfer_fd; t->reserved = NULL; result = w->transfer_id; }
        else if (op == 144 && result < 0) t->reserved = NULL;
        else memset(t, 0, sizeof(*t));
    }
    if (op == 126 && result >= 0)
    {
        ULONG bytes = KSocketSetBytes(p->arg[0]);
        for (i = 0; i < 3; ++i)
            if (w->set_address[i] && bytes)
            {
                if (!storage(ctx, w->set_address[i], bytes)) code = EFAULT;
                else memcpy(ctx->mem + w->set_address[i], p->ready[i], bytes);
            }
        if (w->signals_address)
        {
            if (!storage(ctx, w->signals_address, 4)) code = EFAULT;
            else m68k_write32(ctx, w->signals_address, w->signal_mask);
        }
    }
    clear_packet(w);
    return code && result < 0 ? error(ctx, w, code) : code ? error(ctx, w, code) : (IPTR)result;
}

static IPTR retry(struct KaffeineContext *ctx, void *cpu)
{
    struct KSocketOwner *w = owner(ctx, cpu);
    IPTR result = 0;
    if (!w || !w->pending) return (IPTR)(LONG)-1;
    for (;;)
    {
    if (w->ready)
    {
        ULONG got = signals(ctx, cpu, w->packet.op == 126 ? w->signal_mask :
                            w->packet.again ? SIGBREAKF_CTRL_C : 0);
        if (got)
        {
            w->packet.again = FALSE;
            if (w->packet.op == 126)
            {
                unsigned i;
                w->packet.result = 0; w->packet.error = 0;
                for (i = 0; i < 3; ++i) memset(w->packet.ready[i], 0, KSOCKET_SET_BYTES);
                w->signal_mask = got;
            }
            else
            {
                if (ctx->sched) ctx->task_ops->send_signal(ctx, w->task, got);
                else SetSignal(got, got);
                w->packet.result = w->packet.progress ? (LONG)w->packet.progress : -1;
                w->packet.error = w->packet.progress ? 0 : EINTR;
            }
        }
        else if (w->packet.op == 126)
        {
            ULONG interrupted = signals(ctx, cpu, SIGBREAKF_CTRL_C & ~w->signal_mask);
            if (interrupted)
            {
                if (ctx->sched) ctx->task_ops->send_signal(ctx, w->task, interrupted);
                else SetSignal(interrupted, interrupted);
                w->packet.again = FALSE; w->packet.result = -1; w->packet.error = EINTR;
            }
            else if (!w->packet.again) w->signal_mask = 0;
        }
        if (!w->packet.again) return deliver(ctx, w);
        post(ctx, w);
    }
    if (ctx->sched && ctx->task_ops->socket_wait)
    {
        ctx->task_ops->socket_wait(ctx, cpu, &w->packet, retry, &result);
        return result;
    }
    await(ctx, w);
    }
}

static IPTR invoke(struct KaffeineContext *ctx, void *cpu, ULONG op)
{
    struct KSocketOwner *w = owner(ctx, cpu);
    LONG code;
    if (!w || !w->opens) return (IPTR)(LONG)-1;
    if (w->pending) return error(ctx, w, EBUSY);
    if (op == 162) return w->error;
    if (op == 138) return KSOCKET_FDS;
    if (op == 168)
    {
        ULONG address = THUNK_A(0), size = THUNK_D(0);
        if (address && ((size != 1 && size != 2 && size != 4) || !storage(ctx, address, size)))
            return error(ctx, w, EFAULT);
        w->errno_address = address; w->errno_size = size;
        return 0;
    }
    code = prepare(ctx, cpu, w, op);
    if (code) { clear_packet(w); return error(ctx, w, code); }
    if (!w->packet.op) return 0; /* logical FIONBIO */
    if (op == 126)
    {
        ULONG got = signals(ctx, cpu, w->signal_mask);
        if (got)
        {
            w->signal_mask = got; w->packet.result = 0;
            return deliver(ctx, w);
        }
    }
    post(ctx, w);
    return retry(ctx, cpu);
}

#define SOCKET_THUNK(n) static IPTR socket_##n(struct KaffeineContext *ctx, void *cpu) \
    { return invoke(ctx, cpu, n); }
SOCKET_THUNK(30) SOCKET_THUNK(36) SOCKET_THUNK(42) SOCKET_THUNK(48)
SOCKET_THUNK(54) SOCKET_THUNK(66) SOCKET_THUNK(78) SOCKET_THUNK(84)
SOCKET_THUNK(90) SOCKET_THUNK(96) SOCKET_THUNK(102) SOCKET_THUNK(108)
SOCKET_THUNK(114) SOCKET_THUNK(120) SOCKET_THUNK(126) SOCKET_THUNK(138)
SOCKET_THUNK(144) SOCKET_THUNK(150) SOCKET_THUNK(162) SOCKET_THUNK(168)
#define SOCKET_ENTRY(n) { n, socket_##n }
const struct M68KThunkEntry kaffeine_thunks_socket[] = {
    SOCKET_ENTRY(30), SOCKET_ENTRY(36), SOCKET_ENTRY(42), SOCKET_ENTRY(48),
    SOCKET_ENTRY(54), SOCKET_ENTRY(66), SOCKET_ENTRY(78), SOCKET_ENTRY(84),
    SOCKET_ENTRY(90), SOCKET_ENTRY(96), SOCKET_ENTRY(102), SOCKET_ENTRY(108),
    SOCKET_ENTRY(114), SOCKET_ENTRY(120), SOCKET_ENTRY(126), SOCKET_ENTRY(138),
    SOCKET_ENTRY(144), SOCKET_ENTRY(150), SOCKET_ENTRY(162), SOCKET_ENTRY(168), { 0, NULL }
};
const ULONG kaffeine_thunks_socket_count = sizeof(kaffeine_thunks_socket) / sizeof(kaffeine_thunks_socket[0]) - 1;
