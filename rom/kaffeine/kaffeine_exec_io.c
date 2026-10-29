/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Exec ports, I/O requests and signals of 68k code, timer.device
    through DoIO(), console.device's no-window library unit,
    keyboard.device's KBD_READMATRIX and the open of input.device, both on
    unit 0. Ports and requests stay 68k structures in the guest
    arena. Behind each open request stand a native request of the native
    size and its own reply port, which only this file passes to native
    Exec; DoIO() copies the fields of the command across.

    Asynchronous I/O is timer.device's, of the root task alone: SendIO() of
    an open request, and of a 40-byte copy of one, which gets a native open
    of its own. The native request stays at the device until native Exec
    gives it back; only then do its results reach the guest request, which
    is replied to its guest port as Exec would, or finished in place for a
    WaitIO() that waits for it. The pump that does it runs at the Exec
    calls that can see a completion and at the scheduler's waits. Other
    devices and other asynchronous uses stop the guest with return code 20,
    or the open fails as it does for an unknown device.
*/

#include "kaffeine_intern.h"
#include "kaffeine_exec_io.h"
#include "kaffeine_thunks_console.h"
#include "kaffeine_offsets.h"
#include "kaffeine_workbench.h"
#include "kaffeine_sched.h"
#include "kaffeine_messages.h"

#include <aros/debug.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <devices/keyboard.h>
#include <devices/input.h>
#include <proto/exec.h>
#include <string.h>

/* The 68k Message, IORequest, IOStdReq, timerequest, MsgPort and Library
   of the NDK, and the I/O errors of its exec/errors.h that go to the guest */
enum
{
    G_LN_TYPE = 8, G_LN_NAME = 10, G_MN_REPLYPORT = 14, G_MN_LENGTH = 18,
    G_MESSAGE_SIZE = 20,
    G_IO_DEVICE = 20, G_IO_UNIT = 24, G_IO_COMMAND = 28, G_IO_FLAGS = 30,
    G_IO_ERROR = 31, G_IOREQUEST_SIZE = 32,
    G_IO_ACTUAL = 32, G_IO_LENGTH = 36, G_IO_DATA = 40, G_IOSTDREQ_SIZE = 48,
    G_IOERR_BADADDRESS = -5,
    G_TV_SECS = 32, G_TV_MICRO = 36, G_TIMEREQUEST_SIZE = 40,
    G_MP_FLAGS = 14, G_MP_SIGBIT = 15, G_MP_SIGTASK = 16, G_MP_MSGLIST = 20, G_PA_SIGNAL = 0,
    G_LH_TAIL = 4, G_LH_TAILPRED = 8,
    G_LIB_NEGSIZE = 16, G_LIB_POSSIZE = 18, G_LIB_VERSION = 20, G_LIB_REVISION = 22
};

/* AROS keyboard.device copies min(KB_MATRIXSIZE, io_Length) bytes of its
   matrix, a bit for each of 256 keys (rom/devs/keyboard/keyboard_intern.h).
   Classic matrices are 13 or 16 bytes. */
#define KBD_MATRIX_MAX 32

static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return ctx->mem && addr && addr < ctx->mem_size && ctx->mem_size - addr >= size;
}

static const char *guest_string(struct KaffeineContext *ctx, ULONG addr)
{
    if (!guest_range(ctx, addr, 1) || !memchr(ctx->mem + addr, 0, ctx->mem_size - addr))
        return NULL;
    return (const char *)ctx->mem + addr;
}

/* The stable guest identity that FindTask(NULL) returns. */
static ULONG guest_task(struct KaffeineContext *ctx)
{
    if (!guest_range(ctx, ctx->m68k_sysbase, M68K_EXECBASE_THISTASK + 4))
        return 0;
    return m68k_read32(ctx, ctx->m68k_sysbase + M68K_EXECBASE_THISTASK);
}

IPTR Kaffeine_IORefuse(struct KaffeineContext *ctx, const char *why)
{
    bug("[kaffeine] %s, stopping (20)\n", why);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

/* ── Signals ─────────────────────────────────────────────────────── */

/* -330: AllocSignal(D0=signalNum) -> D0. Exec reads the low byte as a
   signed number, -1 for any free bit. The bit is a native one of the task
   that runs the guest, which Wait() and the reply ports use as well, and
   the guest owns it until FreeSignal() or the end of the program. */
IPTR Kaffeine_AllocSignal(struct KaffeineContext *ctx, void *cpu)
{
    BYTE got = AllocSignal((BYTE)THUNK_D(0));

    if (got >= 0)
    {
        ctx->guest_signals |= 1UL << got;
        /* with AddTask() children, also the guest task that has it */
        if (ctx->sched)
            ctx->task_ops->signal_allocated(ctx, got);
    }
    return (IPTR)(LONG)got;
}

/* -336: FreeSignal(D0=signalNum). Only a bit that the guest allocated goes
   back: the other bits of the native task are not the guest's to free. With
   AddTask() children, only the guest task that allocated it frees it. */
IPTR Kaffeine_FreeSignal(struct KaffeineContext *ctx, void *cpu)
{
    BYTE sig = (BYTE)THUNK_D(0);

    if (sig < 0)
        return 0;
    if (ctx->sched && !ctx->task_ops->signal_freeable(ctx, sig))
        return 0;
    if (sig > 31 || !(ctx->guest_signals & (1UL << sig)))
    {
        bug("[kaffeine] FreeSignal(%ld): not a signal of the 68k program\n", (long)sig);
        return 0;
    }
    /* the App objects that send to a port with this signal go first */
    Kaffeine_AppSignalGone(ctx, sig);
    ctx->guest_signals &= ~(1UL << sig);
    FreeSignal(sig);
    return 0;
}

/* -324: Signal(A1=task, D0=signalSet). The current guest task stands for
   the native task that runs the guest, which may be a child of the one
   behind its Task snapshot. With AddTask() children the scheduler signals
   any task of the program; otherwise other tasks cannot be signalled. */
IPTR Kaffeine_Signal(struct KaffeineContext *ctx, void *cpu)
{
    ULONG task = THUNK_A(1);

    if (ctx->sched)
        return ctx->task_ops->signal(ctx, cpu);
    if (!task || task != guest_task(ctx))
    {
        bug("[kaffeine] Signal(%08lx): not the current task\n", (unsigned long)task);
        return Kaffeine_IORefuse(ctx, "Signal(): only the current task can be signalled so far");
    }
    /* The private bits of asynchronous I/O are not the guest's to send. */
    Signal(FindTask(NULL), THUNK_D(0) & ~ctx->io_signals);
    return 0;
}

/* ── Message ports ───────────────────────────────────────────────── */

static struct KaffeineIOPort *port_entry(struct KaffeineContext *ctx, ULONG port)
{
    ULONG i;

    for (i = 0; i < KAFFEINE_MAX_IO_PORTS; i++)
        if (ctx->io_ports[i].guest == port)
            return &ctx->io_ports[i];
    return NULL;
}

/* -666: CreateMsgPort() -> D0. A 68k MsgPort as AROS makes one: cleared,
   signalling, with a signal of its own and the current task. No native port
   stands behind it. */
IPTR Kaffeine_CreateMsgPort(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineIOPort *entry = port_entry(ctx, 0);
    ULONG port, list;
    BYTE sig;

    (void)cpu;
    if (!entry)
        return 0;
    port = Kaffeine_HeapAlloc(ctx, M68K_SIZEOF_MSGPORT, MEMF_PUBLIC | MEMF_CLEAR);
    if (!port)
        return 0;
    sig = AllocSignal(-1);
    if (sig < 0)
    {
        Kaffeine_HeapFree(ctx, port, M68K_SIZEOF_MSGPORT);
        return 0;
    }
    ctx->guest_signals |= 1UL << sig;
    if (ctx->sched)
        ctx->task_ops->signal_allocated(ctx, sig);
    list = port + G_MP_MSGLIST;
    ctx->mem[port + G_LN_TYPE] = NT_MSGPORT;
    ctx->mem[port + G_MP_FLAGS] = PA_SIGNAL;
    ctx->mem[port + G_MP_SIGBIT] = (UBYTE)sig;
    m68k_write32(ctx, port + G_MP_SIGTASK, guest_task(ctx));
    m68k_write32(ctx, list, list + G_LH_TAIL);
    m68k_write32(ctx, list + G_LH_TAILPRED, list);
    entry->guest = port;
    entry->sigbit = sig;
    return port;
}

/* -672: DeleteMsgPort(A0=port). Only a port from CreateMsgPort(): Exec
   would free the memory and the signal of any other one. The signal is the
   one that CreateMsgPort() gave, whatever the guest wrote since. A port
   that a request in flight replies to stays. */
IPTR Kaffeine_DeleteMsgPort(struct KaffeineContext *ctx, void *cpu)
{
    ULONG port = THUNK_A(0), i;
    struct KaffeineIOPort *entry;

    if (!port)
        return 0;
    entry = port_entry(ctx, port);
    if (!entry)
    {
        bug("[kaffeine] DeleteMsgPort(%08lx): not a port from CreateMsgPort()\n",
            (unsigned long)port);
        return 0;
    }
    if (ctx->io_poll && !ctx->io_poll(ctx))
        return 20;
    for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS; i++)
        if (ctx->io_requests[i].guest && ctx->io_requests[i].pending &&
            !ctx->io_requests[i].orphan && ctx->io_requests[i].port == port)
            return Kaffeine_IORefuse(ctx, "DeleteMsgPort(): an I/O request in flight replies to it");
    if (ctx->sched && !ctx->task_ops->signal_freeable(ctx, entry->sigbit))
        return Kaffeine_IORefuse(ctx, "DeleteMsgPort(): signal belongs to another guest task");
    Kaffeine_AppPortGone(ctx, port);
    if (entry->sigbit >= 0 && entry->sigbit < 32 &&
        (ctx->guest_signals & (1UL << entry->sigbit)))
    {
        ctx->guest_signals &= ~(1UL << entry->sigbit);
        FreeSignal(entry->sigbit);
    }
    for (i = 0; i < KAFFEINE_MAX_MESSAGE_PORTS; ++i)
        if (ctx->message_ports[i] == port) ctx->message_ports[i] = 0;
    entry->guest = 0;
    Kaffeine_HeapFree(ctx, port, M68K_SIZEOF_MSGPORT);
    return 0;
}

/* ── I/O requests ────────────────────────────────────────────────── */

/* The record of req, or a free one for 0. An orphan is neither: its
   storage belongs to the guest heap again, but its slot is still taken. */
static struct KaffeineIORequest *request_entry(struct KaffeineContext *ctx, ULONG req)
{
    ULONG i;

    for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS; i++)
        if (ctx->io_requests[i].guest == req && !ctx->io_requests[i].orphan)
            return &ctx->io_requests[i];
    return NULL;
}

/* mn_Length is guest-editable and may be zero. Only CreateIORequest's
   native capacity limits owned storage; other requests need arena bounds.
   Also recognize pointers into an owned request, so an offset cannot bypass
   its capacity check. */
static BOOL request_range(struct KaffeineContext *ctx, ULONG req, ULONG size)
{
    ULONG i;

    if (!guest_range(ctx, req, size))
        return FALSE;
    for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS; i++)
    {
        struct KaffeineIORequest *entry = &ctx->io_requests[i];

        if (entry->capacity && !entry->orphan && req >= entry->guest &&
            req - entry->guest < entry->capacity)
            return size <= entry->capacity - (req - entry->guest);
    }
    return TRUE;
}

/* An entry stays while the guest request is open or came from
   CreateIORequest(). */
static void request_forget(struct KaffeineIORequest *entry)
{
    if (!entry->native && !entry->capacity)
        memset(entry, 0, sizeof(*entry));
}

/* The native side of a record. A request still in flight, which only the
   end of the program leaves, is aborted and taken back first; its results
   go nowhere. replied and port stay with a record that CreateIORequest()
   storage keeps, so that a reply still queued stays valid. */
static void close_native(struct KaffeineIORequest *entry)
{
    if (entry->pending)
    {
        AbortIO(entry->native);
        WaitIO(entry->native);
        entry->pending = 0;
    }
    CloseDevice(entry->native);
    DeleteIORequest(entry->native);
    if (entry->reply)
        DeleteMsgPort(entry->reply);
    entry->native = NULL;
    entry->reply = NULL;
    entry->unit = 0;
    entry->kind = KAFFEINE_IO_NONE;
    entry->waiting = entry->copy = entry->orphan = 0;
}

/* ── Asynchronous timer.device I/O ───────────────────────────────── */

/* The bytes that a record keeps from other guest use, as
   kaffeine_messages.c reserves them */
static ULONG request_span(const struct KaffeineIORequest *entry)
{
    return entry->capacity ? entry->capacity : entry->copy ? G_TIMEREQUEST_SIZE : G_IOSTDREQ_SIZE;
}

/* The root task on the native task that runs the guest, from trusted state
   alone: guest tasks share that native task, and the guest ThisTask can
   name any of them. */
static BOOL root_caller(struct KaffeineContext *ctx, void *cpu)
{
    if (!ctx->exec_task || FindTask(NULL) != ctx->exec_task)
        return FALSE;
    return !ctx->sched || (ctx->task_ops->is_root && ctx->task_ops->is_root(ctx, cpu));
}

/* A replied request that is still a node of the queue of its reply port.
   A port that is no queue any more holds no node. */
static BOOL linked(struct KaffeineContext *ctx, const struct KaffeineIORequest *entry)
{
    struct KaffeineQueue q;
    unsigned i;

    if (entry->pending || !entry->replied || !Kaffeine_QueueRead(ctx, entry->port, &q))
        return FALSE;
    for (i = 0; i < q.count; i++)
        if (q.nodes[i] == entry->guest)
            return TRUE;
    return FALSE;
}

/* Native reply-port bits stay private for the ports' entire lifetime.
   In particular a scheduler WaitIO retry must not expose its wakeup bit
   to children once the request has completed. */
static void io_signals_update(struct KaffeineContext *ctx)
{
    ULONG bits = ctx->io_copy_port ? 1UL << ctx->io_copy_port->mp_SigBit : 0;
    unsigned i;

    for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS; i++)
        if (ctx->io_requests[i].native && ctx->io_requests[i].reply)
            bits |= 1UL << ctx->io_requests[i].reply->mp_SigBit;
    ctx->io_signals = bits;
}

/* The copy port goes with the last copy. */
static void copy_port_release(struct KaffeineContext *ctx)
{
    unsigned i;

    for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS; i++)
        if (ctx->io_requests[i].copy)
            return;
    if (ctx->io_copy_port)
    {
        DeleteMsgPort(ctx->io_copy_port);
        ctx->io_copy_port = NULL;
    }
    io_signals_update(ctx);
}

/* A copy that is not in flight goes: the native open that the bridge made
   for it, and its record unless CreateIORequest() storage keeps that. */
static void copy_drop(struct KaffeineContext *ctx, struct KaffeineIORequest *entry)
{
    close_native(entry);
    if (!entry->capacity)
        memset(entry, 0, sizeof(*entry));
    copy_port_release(ctx);
}

/* Whether the open of source is the last one of its timer.device unit,
   which its copies need */
static BOOL last_open(struct KaffeineContext *ctx, const struct KaffeineIORequest *source)
{
    unsigned i;

    if (source->copy || source->kind != KAFFEINE_IO_TIMER)
        return FALSE;
    for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS; i++)
    {
        const struct KaffeineIORequest *e = &ctx->io_requests[i];

        if (e != source && e->native && !e->copy && e->kind == KAFFEINE_IO_TIMER &&
            e->unit == source->unit)
            return FALSE;
    }
    return TRUE;
}

/* Closing source would take the last open from a copy in flight. */
static BOOL cascade_blocked(struct KaffeineContext *ctx, const struct KaffeineIORequest *source)
{
    unsigned i;

    if (!last_open(ctx, source))
        return FALSE;
    for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS; i++)
        if (ctx->io_requests[i].copy && !ctx->io_requests[i].orphan &&
            ctx->io_requests[i].pending && ctx->io_requests[i].unit == source->unit)
            return TRUE;
    return FALSE;
}

/* Before source closes: the copies of its unit go with its last open. */
static void cascade_close(struct KaffeineContext *ctx, const struct KaffeineIORequest *source)
{
    unsigned i;

    if (!last_open(ctx, source))
        return;
    for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS; i++)
    {
        struct KaffeineIORequest *e = &ctx->io_requests[i];

        if (e->copy && !e->orphan && e->unit == source->unit)
        {
            bug("[kaffeine] closing copied timer request %08lx with the last open of unit %lu\n",
                (unsigned long)e->guest, (unsigned long)e->unit);
            copy_drop(ctx, e);
        }
    }
}

/* The guest structures that a request the guest sends must not overlap,
   besides those of kaffeine_messages.c: CreateMsgPort() ports, library and
   device bases, and the program's Process. */
static BOOL io_storage(struct KaffeineContext *ctx, ULONG req, ULONG size)
{
    unsigned i;

    for (i = 0; i < KAFFEINE_MAX_IO_PORTS; i++)
        if (ctx->io_ports[i].guest &&
            Kaffeine_Overlap(req, size, ctx->io_ports[i].guest, M68K_SIZEOF_MSGPORT))
            return FALSE;
    for (i = 0; i < ctx->num_libs; i++)
        if (Kaffeine_Overlap(req, size, ctx->libs[i].jt_start,
                             ctx->libs[i].m68k_addr + ctx->libs[i].pos_size - ctx->libs[i].jt_start))
            return FALSE;
    return !ctx->guest_exec_task ||
           !Kaffeine_Overlap(req, size, ctx->guest_exec_task, M68K_SIZEOF_PROCESS);
}

/* The results of a native timerequest that came back, in the guest one */
static void results(struct KaffeineContext *ctx, ULONG req, const struct timerequest *native)
{
    m68k_write32(ctx, req + G_TV_SECS, native->tr_time.tv_secs);
    m68k_write32(ctx, req + G_TV_MICRO, native->tr_time.tv_micro);
    ctx->mem[req + G_IO_ERROR] = (UBYTE)native->tr_node.io_Error;
    ctx->mem[req + G_IO_FLAGS] = native->tr_node.io_Flags & ~IOF_QUICK;
}

/* The completed request is replied to the port that SendIO() saw, which
   must still take it as a node of mn_Length bytes: 40 for a copy, which
   owns no more, else within what the record keeps. Nothing is written
   otherwise, and the guest stops. */
static BOOL io_deliver(struct KaffeineContext *ctx, struct KaffeineIORequest *entry)
{
    struct KaffeineQueue q;
    ULONG req = entry->guest, length = m68k_read16(ctx, req + G_MN_LENGTH);

    if (!request_range(ctx, req, G_TIMEREQUEST_SIZE) ||
        !Kaffeine_GuestBytes(ctx, req, G_TIMEREQUEST_SIZE) ||
        m68k_read32(ctx, req + G_MN_REPLYPORT) != entry->port ||
        (entry->copy ? length != G_TIMEREQUEST_SIZE :
                       length < G_TIMEREQUEST_SIZE || length > request_span(entry)) ||
        !request_range(ctx, req, length) || !Kaffeine_QueueRead(ctx, entry->port, &q) ||
        !Kaffeine_IOMessagePreflight(ctx, &q, req, length, entry))
    {
        bug("[kaffeine] I/O request %08lx completed, but its reply port %08lx cannot take it\n",
            (unsigned long)req, (unsigned long)entry->port);
        Kaffeine_MessageFailure(ctx, "I/O completion");
        return FALSE;
    }
    results(ctx, req, (const struct timerequest *)entry->native);
    Kaffeine_MessageAppend(ctx, &q, req, NT_REPLYMSG);
    entry->replied = 1;
    return TRUE;
}

/* The native request of entry is back. An orphan goes without any guest
   access. One that the root waits for in WaitIO() gets its results in
   place, neither queued nor signalled, as Exec's WaitIO() takes it from
   its port; a root blocked for it wakes on the private bit again. Any
   other one is replied. */
static BOOL io_finish(struct KaffeineContext *ctx, struct KaffeineIORequest *entry)
{
    ULONG req = entry->guest;

    entry->pending = 0;
    if (entry->orphan)
    {
        bug("[kaffeine] freed I/O request %08lx is back, closing it\n", (unsigned long)req);
        close_native(entry);
        memset(entry, 0, sizeof(*entry));
        return TRUE;
    }
    if (!entry->waiting)
        return io_deliver(ctx, entry);
    if (!request_range(ctx, req, G_TIMEREQUEST_SIZE) ||
        !Kaffeine_GuestBytes(ctx, req, G_TIMEREQUEST_SIZE))
    {
        Kaffeine_MessageFailure(ctx, "WaitIO completion");
        return FALSE;
    }
    results(ctx, req, (const struct timerequest *)entry->native);
    ctx->mem[req + G_LN_TYPE] = NT_REPLYMSG;
    entry->replied = 1;
    if (entry->waiting == 2)
        Signal(FindTask(NULL), 1UL << (entry->copy ? ctx->io_copy_port : entry->reply)->mp_SigBit);
    return TRUE;
}

/* ctx->io_poll: every native request that came back, the copies in the
   order of their shared port. Each private bit is cleared before its port
   is read, so that a reply after the last GetMsg() sets it again. */
static BOOL io_pump(struct KaffeineContext *ctx)
{
    struct Message *msg;
    unsigned i;
    BOOL ok = TRUE;

    if (!ctx->running)
        return TRUE;
    if (ctx->io_copy_port)
    {
        SetSignal(0, 1UL << ctx->io_copy_port->mp_SigBit);
        while (ok && (msg = GetMsg(ctx->io_copy_port)))
        {
            for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS; i++)
                if (ctx->io_requests[i].copy && ctx->io_requests[i].pending &&
                    (struct Message *)ctx->io_requests[i].native == msg)
                    break;
            if (i == KAFFEINE_MAX_IO_REQUESTS)
            {
                Kaffeine_IORefuse(ctx, "I/O completion: an unknown native message on the copy port");
                ok = FALSE;
            }
            else
                ok = io_finish(ctx, &ctx->io_requests[i]);
        }
    }
    for (i = 0; ok && i < KAFFEINE_MAX_IO_REQUESTS; i++)
    {
        struct KaffeineIORequest *entry = &ctx->io_requests[i];

        if (!entry->pending || entry->copy)
            continue;
        SetSignal(0, 1UL << entry->reply->mp_SigBit);
        msg = GetMsg(entry->reply);
        if (!msg)
            continue;
        if (msg != (struct Message *)entry->native)
        {
            Kaffeine_IORefuse(ctx, "I/O completion: an unknown native message on a reply port");
            ok = FALSE;
        }
        else
            ok = io_finish(ctx, entry);
    }
    copy_port_release(ctx);
    io_signals_update(ctx);
    return ok;
}

/* ctx->io_wait: without guest tasks, the root waits natively for an empty
   guest port that a request in flight replies to, outside callbacks. */
static BOOL io_wait_port(struct KaffeineContext *ctx, ULONG port)
{
    unsigned i;

    if (ctx->callback_depth || !root_caller(ctx, ctx->cpu))
        return FALSE;
    for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS; i++)
        if (ctx->io_requests[i].pending && !ctx->io_requests[i].orphan &&
            !ctx->io_requests[i].waiting && ctx->io_requests[i].port == port)
        {
            Wait(ctx->io_signals);
            return TRUE;
        }
    return FALSE;
}

/* ctx->io_release: the heap is about to free size bytes at addr. A request
   in flight there, or one replying to a port there, and a reply still
   queued there keep them, and nothing changes. Only the root's own copies
   in flight there are let go: aborted and orphaned, they are reaped when
   they come back and never touch the guest again. Idle copies there go. */
static BOOL io_release(struct KaffeineContext *ctx, ULONG addr, ULONG size, BOOL commit)
{
    BOOL root = root_caller(ctx, ctx->cpu);
    unsigned i;

    for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS; i++)
    {
        const struct KaffeineIORequest *e = &ctx->io_requests[i];
        BOOL inside;

        if (!e->guest || e->orphan)
            continue;
        inside = Kaffeine_Overlap(addr, size, e->guest, request_span(e));
        if (e->pending ? (inside ? !(e->copy && root) :
                                   Kaffeine_Overlap(addr, size, e->port, M68K_SIZEOF_MSGPORT)) :
                         inside && linked(ctx, e))
        {
            bug("[kaffeine] freeing %lu bytes at %08lx: I/O request %08lx is %s, keeping them\n",
                (unsigned long)size, (unsigned long)addr, (unsigned long)e->guest,
                e->pending ? "in flight" : "still queued");
            return FALSE;
        }
    }
    if (!commit)
        return TRUE;
    for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS; i++)
    {
        struct KaffeineIORequest *e = &ctx->io_requests[i];

        if (!e->guest || e->orphan || !e->copy ||
            !Kaffeine_Overlap(addr, size, e->guest, request_span(e)))
            continue;
        if (e->pending)
        {
            bug("[kaffeine] copied timer request %08lx freed in flight, aborting it\n",
                (unsigned long)e->guest);
            AbortIO(e->native);
            e->orphan = 1;
        }
        else
            copy_drop(ctx, e);
    }
    return TRUE;
}

static void io_hooks(struct KaffeineContext *ctx)
{
    ctx->io_poll = io_pump;
    ctx->io_wait = io_wait_port;
    ctx->io_release = io_release;
}

/* The pump before a guard, which then finds a request still in flight or
   not. FALSE when it stopped the guest. */
static BOOL io_pumped(struct KaffeineContext *ctx)
{
    return !ctx->io_poll || ctx->io_poll(ctx);
}

/* -654: CreateIORequest(A0=ioReplyPort, D0=size) -> D0. A cleared 68k
   request with its reply port and length, as Exec makes it. The size goes
   from a Message, 20 bytes, where Exec would write past a smaller block, to
   65535, the most that mn_Length records. */
IPTR Kaffeine_CreateIORequest(struct KaffeineContext *ctx, void *cpu)
{
    ULONG port = THUNK_A(0), size = THUNK_D(0), req;
    struct KaffeineIORequest *entry;

    if (!port || size < G_MESSAGE_SIZE || size > 0xFFFF)
        return 0;
    entry = request_entry(ctx, 0);
    if (!entry)
        return 0;
    req = Kaffeine_HeapAlloc(ctx, size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!req)
        return 0;
    m68k_write32(ctx, req + G_MN_REPLYPORT, port);
    m68k_write16(ctx, req + G_MN_LENGTH, (UWORD)size);
    entry->guest = req;
    entry->capacity = size;
    return req;
}

/* -660: DeleteIORequest(A0=ioRequest). Only a request from
   CreateIORequest(). One that is still open is closed first, so that no
   native device stays open behind freed memory, and one still queued on
   its reply port leaves it. One in flight, or the last open of a unit
   whose copies are in flight, stops the guest. */
IPTR Kaffeine_DeleteIORequest(struct KaffeineContext *ctx, void *cpu)
{
    ULONG req = THUNK_A(0);
    struct KaffeineIORequest *entry;

    if (!req)
        return 0;
    entry = request_entry(ctx, req);
    if (!entry || !entry->capacity)
    {
        bug("[kaffeine] DeleteIORequest(%08lx): not a request from CreateIORequest()\n",
            (unsigned long)req);
        return 0;
    }
    if (!io_pumped(ctx))
        return 20;
    if (entry->pending)
        return Kaffeine_IORefuse(ctx, "DeleteIORequest(): the request is still in flight");
    if (cascade_blocked(ctx, entry))
        return Kaffeine_IORefuse(ctx, "DeleteIORequest(): copies of its unit are still in flight");
    if (linked(ctx, entry))
        Kaffeine_GuestRemoveMsg(ctx, entry->port, req);
    if (entry->native)
    {
        bug("[kaffeine] DeleteIORequest(%08lx): still open, closing it\n", (unsigned long)req);
        cascade_close(ctx, entry);
        close_native(entry);
    }
    io_signals_update(ctx);
    Kaffeine_HeapFree(ctx, req, entry->capacity);
    memset(entry, 0, sizeof(*entry));
    copy_port_release(ctx);
    return 0;
}

/* ── Devices ─────────────────────────────────────────────────────── */

static IPTR open_failed(struct KaffeineContext *ctx, ULONG req, BYTE error)
{
    if (request_range(ctx, req, G_IOREQUEST_SIZE))
    {
        m68k_write32(ctx, req + G_IO_DEVICE, 0);
        ctx->mem[req + G_IO_ERROR] = (UBYTE)error;
    }
    return (IPTR)(LONG)error;
}

/* The guest timer.device base, made at the first successful open: a 68k
   Device with the name, the type and the version of the native one. */
static BOOL timer_base(struct KaffeineContext *ctx, struct Device *native)
{
    static const char name[] = "timer.device";
    ULONG base, text;

    if (ctx->timer_base)
        return TRUE;
    base = Kaffeine_SetupDeviceBase(ctx, name, KAFFEINE_TIMER_VECTORS,
                                   kaffeine_thunks_timer, kaffeine_thunks_timer_count);
    if (!base)
        return FALSE;
    text = Kaffeine_HeapAlloc(ctx, sizeof(name), MEMF_PUBLIC);
    if (text)
    {
        memcpy(ctx->mem + text, name, sizeof(name));
        m68k_write32(ctx, base + G_LN_NAME, text);
    }
    ctx->mem[base + G_LN_TYPE] = NT_DEVICE;
    m68k_write16(ctx, base + G_LIB_NEGSIZE, KAFFEINE_TIMER_VECTORS * 6);
    m68k_write16(ctx, base + G_LIB_POSSIZE, M68K_SIZEOF_DEVICE);
    m68k_write16(ctx, base + G_LIB_VERSION, native->dd_Library.lib_Version);
    m68k_write16(ctx, base + G_LIB_REVISION, native->dd_Library.lib_Revision);
    ctx->timer_base = base;
    ctx->native_timer = native;
    return TRUE;
}

/* A guest Device with the name, the type and the version of the native
   one, and no native pointer. Allocate the name first: a failed Device
   registration must leave no name allocation or partially published
   identity behind. */
static ULONG device_base(struct KaffeineContext *ctx, const char *name, UWORD vectors,
                         const struct M68KThunkEntry *thunks, ULONG count,
                         const struct Device *native)
{
    ULONG size = strlen(name) + 1, base, text;

    text = Kaffeine_HeapAlloc(ctx, size, MEMF_PUBLIC);
    if (!text)
        return 0;
    base = Kaffeine_SetupDeviceBase(ctx, name, vectors, thunks, count);
    if (!base)
    {
        Kaffeine_HeapFree(ctx, text, size);
        return 0;
    }
    memcpy(ctx->mem + text, name, size);
    m68k_write32(ctx, base + G_LN_NAME, text);
    ctx->mem[base + G_LN_TYPE] = NT_DEVICE;
    m68k_write16(ctx, base + G_LIB_NEGSIZE, vectors * 6);
    m68k_write16(ctx, base + G_LIB_POSSIZE, M68K_SIZEOF_DEVICE);
    m68k_write16(ctx, base + G_LIB_VERSION, native->dd_Library.lib_Version);
    m68k_write16(ctx, base + G_LIB_REVISION, native->dd_Library.lib_Revision);
    return base;
}

/* The guest base of an adapter, made at its first successful open; 0 when
   it cannot be made. */
static ULONG guest_device(struct KaffeineContext *ctx, enum KaffeineIODeviceKind kind,
                          struct Device *native)
{
    switch (kind)
    {
    case KAFFEINE_IO_TIMER:
        return timer_base(ctx, native) ? ctx->timer_base : 0;
    case KAFFEINE_IO_CONSOLE:
        if (!ctx->console_base)
            ctx->console_base = device_base(ctx, "console.device", KAFFEINE_CONSOLE_VECTORS,
                                            kaffeine_thunks_console,
                                            kaffeine_thunks_console_count, native);
        return ctx->console_base;
    case KAFFEINE_IO_KEYBOARD:
        if (!ctx->keyboard_base)
            ctx->keyboard_base = device_base(ctx, "keyboard.device", KAFFEINE_KEYBOARD_VECTORS,
                                             kaffeine_thunks_keyboard,
                                             kaffeine_thunks_keyboard_count, native);
        return ctx->keyboard_base;
    case KAFFEINE_IO_INPUT:
        if (!ctx->input_base)
            ctx->input_base = device_base(ctx, "input.device", KAFFEINE_INPUT_VECTORS,
                                          kaffeine_thunks_input,
                                          kaffeine_thunks_input_count, native);
        return ctx->input_base;
    default:
        return 0;
    }
}

/* The adapter for a device and unit: timer.device on any unit, console's
   library unit, and unit 0 of keyboard.device and input.device. */
static enum KaffeineIODeviceKind device_kind(const char *name, ULONG unit)
{
    if (!name)
        return KAFFEINE_IO_NONE;
    if (strcmp(name, "timer.device") == 0)
        return KAFFEINE_IO_TIMER;
    if (strcmp(name, "console.device") == 0)
        return unit == 0xFFFFFFFFu ? KAFFEINE_IO_CONSOLE : KAFFEINE_IO_NONE;
    if (strcmp(name, "keyboard.device") == 0)
        return unit == 0 ? KAFFEINE_IO_KEYBOARD : KAFFEINE_IO_NONE;
    if (strcmp(name, "input.device") == 0)
        return unit == 0 ? KAFFEINE_IO_INPUT : KAFFEINE_IO_NONE;
    return KAFFEINE_IO_NONE;
}

/* By kind: the native device, the guest request it needs and the native
   one behind it. */
static const struct
{
    const char *name;
    ULONG guest_size, native_size;
} adapters[] =
{
    [KAFFEINE_IO_TIMER]    = { "timer.device", G_TIMEREQUEST_SIZE, sizeof(struct timerequest) },
    [KAFFEINE_IO_CONSOLE]  = { "console.device", G_IOREQUEST_SIZE, sizeof(struct IOStdReq) },
    [KAFFEINE_IO_KEYBOARD] = { "keyboard.device", G_IOSTDREQ_SIZE, sizeof(struct IOStdReq) },
    [KAFFEINE_IO_INPUT]    = { "input.device", G_IOSTDREQ_SIZE, sizeof(struct IOStdReq) }
};

/* -444: OpenDevice(A0=devName, D0=unitNumber, A1=iORequest, D1=flags) -> D0.
   timer.device, console.device CONU_LIBRARY and unit 0 of keyboard.device
   and input.device have native adapters; other units fail to open before
   any native call. As Exec, a name of 0 means timer.device and 1
   input.device. The guest request needs a timerequest for timer, an
   IOStdReq for keyboard and input, and only the IORequest prefix for the
   no-window console unit, including a zero mn_Length and no reply port.
   It gets a guest Device base and unit number; the native request, of the
   native size, never leaves the bridge. Opening a request again closes the
   native open behind it first, which Exec would leak, and the copies of
   its unit with its last open; a request in flight, or a last open whose
   copies are, stops the guest instead. */
IPTR Kaffeine_OpenDevice(struct KaffeineContext *ctx, void *cpu)
{
    ULONG name_addr = THUNK_A(0), unit = THUNK_D(0), req = THUNK_A(1), flags = THUNK_D(1);
    ULONG base = 0;
    enum KaffeineIODeviceKind kind;
    struct KaffeineIORequest *entry;
    struct IORequest *native;
    struct MsgPort *reply;
    const char *name;
    BYTE error;

    if (!guest_range(ctx, req, G_IOREQUEST_SIZE))
        return Kaffeine_IORefuse(ctx, "OpenDevice(): the request is not in guest memory");
    if (!request_range(ctx, req, G_IOREQUEST_SIZE))
        return open_failed(ctx, req, IOERR_OPENFAIL);
    name = name_addr == 0 ? "timer.device" :
           name_addr == 1 ? "input.device" : guest_string(ctx, name_addr);
    bug("[kaffeine] OpenDevice(%s): unit=%ld flags=%08lx request=%08lx length=%lu\n",
        name ? name : "(bad name)", (long)(LONG)unit, (unsigned long)flags,
        (unsigned long)req, (unsigned long)m68k_read16(ctx, req + G_MN_LENGTH));
    entry = request_entry(ctx, req);
    if (entry && entry->native)
    {
        if (!io_pumped(ctx))
            return 20;
        if (entry->pending)
            return Kaffeine_IORefuse(ctx, "OpenDevice(): the request is still in flight");
        if (cascade_blocked(ctx, entry))
            return Kaffeine_IORefuse(ctx, "OpenDevice(): copies of its unit are still in flight");
    }
    m68k_write32(ctx, req + G_IO_UNIT, 0);
    if (entry && entry->native)
    {
        bug("[kaffeine] OpenDevice(%08lx): already open, closing it first\n", (unsigned long)req);
        if (entry->copy)
            copy_drop(ctx, entry);
        else
        {
            cascade_close(ctx, entry);
            close_native(entry);
        }
        io_signals_update(ctx);
    }
    kind = device_kind(name, unit);
    if (kind == KAFFEINE_IO_NONE)
    {
        bug("[kaffeine] OpenDevice(%s): no adapter for this device or unit\n",
            name ? name : "(bad name)");
        if (entry)
            request_forget(entry);
        return open_failed(ctx, req, IOERR_OPENFAIL);
    }
    if (!request_range(ctx, req, adapters[kind].guest_size))
    {
        if (entry)
            request_forget(entry);
        return open_failed(ctx, req, IOERR_OPENFAIL);
    }
    if (!entry && !(entry = request_entry(ctx, 0)))
        return open_failed(ctx, req, IOERR_OPENFAIL);

    reply = CreateMsgPort();
    native = reply ? (struct IORequest *)CreateIORequest(reply, adapters[kind].native_size) : NULL;
    if (!native)
    {
        if (reply)
            DeleteMsgPort(reply);
        error = IOERR_OPENFAIL;
    }
    else
    {
        error = OpenDevice(adapters[kind].name,
                           kind == KAFFEINE_IO_CONSOLE ? (IPTR)-1 : (IPTR)unit, native, flags);
        if (!error && !(base = guest_device(ctx, kind, native->io_Device)))
        {
            CloseDevice(native);
            error = IOERR_OPENFAIL;
        }
        if (error)
        {
            DeleteIORequest(native);
            DeleteMsgPort(reply);
        }
    }
    if (error)
    {
        request_forget(entry);
        return open_failed(ctx, req, error);
    }
    entry->guest = req;
    entry->native = native;
    entry->reply = reply;
    entry->unit = unit;
    entry->kind = kind;
    entry->open_flags = flags;
    io_signals_update(ctx);
    m68k_write32(ctx, req + G_IO_DEVICE, base);
    m68k_write32(ctx, req + G_IO_UNIT, unit);
    ctx->mem[req + G_IO_ERROR] = 0;
    return 0;
}

/* -450: CloseDevice(A1=iORequest) -> D0. Console's library unit clears
   both guest identities; timer, keyboard and input get Exec's closed
   marker, an io_Device of -1. A copy closes the native open that the
   bridge made for it and leaves its reply port's queue; the last open of
   a unit closes its copies too. A request in flight, or a last open whose
   copies are, stops the guest. */
IPTR Kaffeine_CloseDevice(struct KaffeineContext *ctx, void *cpu)
{
    ULONG req = THUNK_A(1);
    struct KaffeineIORequest *entry = req ? request_entry(ctx, req) : NULL;
    enum KaffeineIODeviceKind kind;

    if (!entry || !entry->native)
        return 0;
    if (!io_pumped(ctx))
        return 20;
    if (entry->pending)
        return Kaffeine_IORefuse(ctx, "CloseDevice(): the request is still in flight");
    if (cascade_blocked(ctx, entry))
        return Kaffeine_IORefuse(ctx, "CloseDevice(): copies of its unit are still in flight");
    if (entry->copy)
    {
        if (linked(ctx, entry))
            Kaffeine_GuestRemoveMsg(ctx, entry->port, req);
        copy_drop(ctx, entry);
        if (request_range(ctx, req, G_IOREQUEST_SIZE))
            m68k_write32(ctx, req + G_IO_DEVICE, 0xFFFFFFFF);
        return 0;
    }
    kind = entry->kind;
    cascade_close(ctx, entry);
    close_native(entry);
    io_signals_update(ctx);
    if (request_range(ctx, req, G_IOREQUEST_SIZE))
    {
        m68k_write32(ctx, req + G_IO_DEVICE, kind == KAFFEINE_IO_CONSOLE ? 0 : 0xFFFFFFFF);
        if (kind == KAFFEINE_IO_CONSOLE)
            m68k_write32(ctx, req + G_IO_UNIT, 0);
    }
    request_forget(entry);
    return 0;
}

/* As Exec: a request is in flight when it is not QUICK and is still an
   unreplied message. Only for requests that the bridge has no record of,
   whose markers no I/O here can have set. */
static BOOL in_flight(struct KaffeineContext *ctx, ULONG req)
{
    return !(ctx->mem[req + G_IO_FLAGS] & IOF_QUICK) &&
           ctx->mem[req + G_LN_TYPE] == NT_MESSAGE;
}

/* KBD_READMATRIX into a native buffer, of at most the native matrix: the
   device copies min(matrix, io_Length) bytes. io_Data must hold that many
   bytes in guest memory, apart from the request, or the native device is
   not called and io_Error reports the failure. Back come io_Actual, at most the
   length passed, and those bytes; io_Length, io_Data and io_Offset stay. */
static BOOL matrix_storage(struct KaffeineContext *ctx, ULONG data, ULONG length)
{
    unsigned i;
    if (!Kaffeine_MessageMemory(ctx, data, length)) return FALSE;
    for (i = 0; i < KAFFEINE_MAX_IO_PORTS; i++)
        if (ctx->io_ports[i].guest && Kaffeine_Overlap(data, length, ctx->io_ports[i].guest, 34))
            return FALSE;
    for (i = 0; i < KAFFEINE_MAX_FONTS; i++)
        if (ctx->fonts[i].guest && Kaffeine_Overlap(data, length, ctx->fonts[i].guest,
                                                  ctx->fonts[i].allocation_size))
            return FALSE;
    return TRUE;
}

static BYTE read_matrix(struct KaffeineContext *ctx, ULONG req, struct KaffeineIORequest *entry)
{
    struct IOStdReq *native = (struct IOStdReq *)entry->native;
    ULONG length = m68k_read32(ctx, req + G_IO_LENGTH), data = m68k_read32(ctx, req + G_IO_DATA);
    ULONG actual;
    UBYTE matrix[KBD_MATRIX_MAX];
    BYTE error;

    if (length > sizeof(matrix))
        length = sizeof(matrix);
    if (length && (!matrix_storage(ctx, data, length) ||
                   Kaffeine_Overlap(data, length, req, G_IOSTDREQ_SIZE)))
    {
        bug("[kaffeine] KBD_READMATRIX: %lu bytes at %08lx are not free guest memory\n",
            (unsigned long)length, (unsigned long)data);
        return G_IOERR_BADADDRESS;
    }
    memset(matrix, 0, sizeof(matrix));
    native->io_Command = KBD_READMATRIX;
    native->io_Flags = 0;
    native->io_Actual = 0;
    native->io_Length = length;
    native->io_Data = matrix;
    native->io_Offset = 0;
    error = DoIO((struct IORequest *)native);
    actual = native->io_Actual < length ? native->io_Actual : length;
    native->io_Data = NULL;
    native->io_Length = 0;
    if (!error && actual)
        memcpy(ctx->mem + data, matrix, actual);
    m68k_write32(ctx, req + G_IO_ACTUAL, actual);
    ctx->mem[req + G_IO_FLAGS] = native->io_Flags & IOF_QUICK;
    ctx->mem[req + G_LN_TYPE] = native->io_Message.mn_Node.ln_Type;
    return error;
}

/* DoIO() of an IOStdReq open on keyboard.device or input.device. The
   record decides: the guest request must still name its guest base and
   unit. KBD_READMATRIX is the one command that
   reaches the native device; every other one, input's handler and event
   commands among them, ends with IOERR_NOCMD before any native call and
   without reading io_Data. */
static IPTR std_doio(struct KaffeineContext *ctx, ULONG req, struct KaffeineIORequest *entry)
{
    ULONG base = entry->kind == KAFFEINE_IO_KEYBOARD ? ctx->keyboard_base : ctx->input_base;
    BYTE error;

    if (!request_range(ctx, req, G_IOSTDREQ_SIZE))
        return Kaffeine_IORefuse(ctx, "DoIO(): the request is smaller than an IOStdReq");
    /* CreateExtIO initializes ln_Type to NT_MESSAGE even before the first
       submission. These adapters have no asynchronous submit path, so that
       guest marker alone cannot mean a native request is pending. */
    if (!base || m68k_read32(ctx, req + G_IO_DEVICE) != base ||
        m68k_read32(ctx, req + G_IO_UNIT) != entry->unit)
        return Kaffeine_IORefuse(ctx, "DoIO(): the request names another device or unit "
                                     "than the one it was opened on");
    ctx->mem[req + G_IO_FLAGS] = IOF_QUICK;
    ctx->mem[req + G_LN_TYPE] = 0;
    if (entry->kind == KAFFEINE_IO_KEYBOARD &&
        m68k_read16(ctx, req + G_IO_COMMAND) == KBD_READMATRIX)
        error = read_matrix(ctx, req, entry);
    else
        error = IOERR_NOCMD;
    ctx->mem[req + G_IO_ERROR] = (UBYTE)error;
    return (IPTR)(LONG)error;
}

/* -456: DoIO(A1=iORequest) -> D0=io_Error. As Exec, the request gets
   IOF_QUICK and ln_Type 0 first, and without a device the result is -1.
   For timer.device, TR_ADDREQUEST and TR_GETSYSTIME copy both time fields
   to the native request and the results back. TR_SETSYSTIME would set the
   clock of the whole system, and the AROS driver never replies to an EClock
   delay; they and every other command end with IOERR_NOCMD without
   reaching the device. Console requests always return IOERR_NOCMD, using
   the native record's kind rather than guest-editable device/unit fields.
   Keyboard and input requests go to std_doio(). A request in flight or
   still queued on its reply port, and a copy, stop the guest before any
   write. */
IPTR Kaffeine_DoIO(struct KaffeineContext *ctx, void *cpu)
{
    ULONG req = THUNK_A(1);
    struct KaffeineIORequest *entry;
    struct timerequest *native;
    UWORD command;
    BYTE error;

    if (!req)
        return (IPTR)(LONG)-1;
    if (!request_range(ctx, req, G_IOREQUEST_SIZE))
        return Kaffeine_IORefuse(ctx, "DoIO(): the request is not in guest memory");
    entry = request_entry(ctx, req);
    if (entry && (entry->pending || entry->replied || entry->copy))
    {
        if (!io_pumped(ctx))
            return 20;
        if (entry->pending)
            return Kaffeine_IORefuse(ctx, "DoIO(): the request is still in flight");
        if (entry->copy)
            return Kaffeine_IORefuse(ctx, "DoIO(): a copied timer request is supported with SendIO() only");
        if (linked(ctx, entry))
            return Kaffeine_IORefuse(ctx, "DoIO(): the request is still queued on its reply port");
        entry->replied = 0;
    }
    if (entry && entry->native &&
        (entry->kind == KAFFEINE_IO_KEYBOARD || entry->kind == KAFFEINE_IO_INPUT))
        return std_doio(ctx, req, entry);
    ctx->mem[req + G_IO_FLAGS] = IOF_QUICK;
    ctx->mem[req + G_LN_TYPE] = 0;
    if (!entry || !entry->native)
    {
        if (!m68k_read32(ctx, req + G_IO_DEVICE))
            return (IPTR)(LONG)-1;
        return Kaffeine_IORefuse(ctx, "DoIO(): the request was not opened here, "
                                     "and a copy of an open request is not supported yet");
    }
    command = m68k_read16(ctx, req + G_IO_COMMAND);
    if (entry->kind == KAFFEINE_IO_TIMER &&
        (command == TR_GETSYSTIME ||
         (command == TR_ADDREQUEST && entry->unit != UNIT_ECLOCK && entry->unit != UNIT_WAITECLOCK)))
    {
        if (!request_range(ctx, req, G_TIMEREQUEST_SIZE))
            return Kaffeine_IORefuse(ctx, "DoIO(): the timer request is too small");
        native = (struct timerequest *)entry->native;
        native->tr_node.io_Command = command;
        native->tr_time.tv_secs = m68k_read32(ctx, req + G_TV_SECS);
        native->tr_time.tv_micro = m68k_read32(ctx, req + G_TV_MICRO);
        error = DoIO(&native->tr_node);
        m68k_write32(ctx, req + G_TV_SECS, native->tr_time.tv_secs);
        m68k_write32(ctx, req + G_TV_MICRO, native->tr_time.tv_micro);
        ctx->mem[req + G_IO_FLAGS] = native->tr_node.io_Flags & IOF_QUICK;
        ctx->mem[req + G_LN_TYPE] = native->tr_node.io_Message.mn_Node.ln_Type;
    }
    else
        error = IOERR_NOCMD;
    ctx->mem[req + G_IO_ERROR] = (UBYTE)error;
    return (IPTR)(LONG)error;
}

/* -462: SendIO(A1=iORequest). timer.device alone: of a request opened
   here, TR_ADDREQUEST on any unit but the EClock ones, or TR_GETSYSTIME;
   of a 40-byte copy of one, TR_ADDREQUEST on UNIT_MICROHZ or UNIT_VBLANK
   while that unit has an open. A copy gets a native request and open of
   its own, with the OpenDevice() flags of an open of its unit, at its
   first SendIO(). Only the root sends, outside callbacks, to a reply port
   of its own or one that ignores arrivals, which must take the request as
   a node. Every check comes before any write and native call, and SendIO()
   has no way to report a failure: what the bridge cannot do, a failed
   native setup included, stops the guest with 20. */
IPTR Kaffeine_SendIO(struct KaffeineContext *ctx, void *cpu)
{
    ULONG req = THUNK_A(1), device, unit, port, length;
    struct KaffeineIORequest *entry, *source = NULL;
    struct KaffeineQueue q;
    struct timerequest *native;
    UWORD command;
    BOOL copy, fresh;
    unsigned i;

    bug("[kaffeine] SendIO(%08lx)\n", (unsigned long)req);
    if (!request_range(ctx, req, G_IOREQUEST_SIZE))
        return Kaffeine_IORefuse(ctx, "SendIO(): the request is not in guest memory");
    entry = request_entry(ctx, req);
    command = m68k_read16(ctx, req + G_IO_COMMAND);
    if (entry && entry->native && !entry->copy && entry->kind != KAFFEINE_IO_TIMER)
    {
        bug("[kaffeine] SendIO(%08lx): %s command %u\n", (unsigned long)req,
            adapters[entry->kind].name, (unsigned)command);
        return Kaffeine_IORefuse(ctx, "SendIO(): asynchronous I/O of this device is not supported");
    }
    if ((req & 1) || !request_range(ctx, req, G_TIMEREQUEST_SIZE) ||
        !Kaffeine_GuestBytes(ctx, req, G_TIMEREQUEST_SIZE))
        return Kaffeine_IORefuse(ctx, "SendIO(): not a timerequest in guest memory at an even address");
    if (!root_caller(ctx, cpu) || ctx->callback_depth)
        return Kaffeine_IORefuse(ctx, "SendIO(): asynchronous I/O is the root task's, outside callbacks");
    if (entry && entry->pending)
        return Kaffeine_IORefuse(ctx, "SendIO(): the request is still in flight");
    device = m68k_read32(ctx, req + G_IO_DEVICE);
    unit = m68k_read32(ctx, req + G_IO_UNIT);
    length = m68k_read16(ctx, req + G_MN_LENGTH);
    copy = !(entry && entry->native && !entry->copy);
    if (!copy)
    {
        if (device != ctx->timer_base || unit != entry->unit)
            return Kaffeine_IORefuse(ctx, "SendIO(): the request names another device or unit "
                                         "than the one it was opened on");
        if (command != TR_GETSYSTIME &&
            (command != TR_ADDREQUEST || unit == UNIT_ECLOCK || unit == UNIT_WAITECLOCK))
        {
            bug("[kaffeine] SendIO(%08lx): timer.device command %u on unit %lu\n",
                (unsigned long)req, (unsigned)command, (unsigned long)unit);
            return Kaffeine_IORefuse(ctx, "SendIO(): timer.device supports TR_ADDREQUEST on "
                                         "non-EClock units and TR_GETSYSTIME");
        }
    }
    else if (entry && entry->copy)
    {
        if (device != ctx->timer_base || unit != entry->unit)
            return Kaffeine_IORefuse(ctx, "SendIO(): a copied timer request changed its device or unit");
    }
    else
    {
        if (!ctx->timer_base || device != ctx->timer_base)
        {
            if (device && device == ctx->console_base)
                return Kaffeine_IORefuse(ctx, "SendIO(): a copied console.device request is not supported");
            return Kaffeine_IORefuse(ctx, "SendIO(): the request was not opened here "
                                         "and is no copy of an open timer.device request");
        }
        if (unit != UNIT_MICROHZ && unit != UNIT_VBLANK)
            return Kaffeine_IORefuse(ctx, "SendIO(): a copied timer request must be on "
                                         "UNIT_MICROHZ or UNIT_VBLANK");
        for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS && !source; i++)
            if (ctx->io_requests[i].native && !ctx->io_requests[i].copy &&
                ctx->io_requests[i].kind == KAFFEINE_IO_TIMER && ctx->io_requests[i].unit == unit)
                source = &ctx->io_requests[i];
        if (!source)
            return Kaffeine_IORefuse(ctx, "SendIO(): a copied timer request whose unit has no open");
    }
    if (copy && command != TR_ADDREQUEST)
        return Kaffeine_IORefuse(ctx, "SendIO(): a copied timer request supports TR_ADDREQUEST only");
    if (copy ? length != G_TIMEREQUEST_SIZE :
               length < G_TIMEREQUEST_SIZE || length > request_span(entry) || !request_range(ctx, req, length))
        return Kaffeine_IORefuse(ctx, "SendIO(): mn_Length must hold the timerequest, "
                                     "and be exactly 40 for a copy");
    if (!io_storage(ctx, req, length))
        return Kaffeine_IORefuse(ctx, "SendIO(): the request overlaps a port, a base or the Process");
    port = m68k_read32(ctx, req + G_MN_REPLYPORT);
    if (!port || (ctx->guest_exec_task && port == ctx->guest_exec_task + M68K_PR_MSGPORT))
        return Kaffeine_IORefuse(ctx, "SendIO(): the reply port must be a port of the program "
                                     "other than its pr_MsgPort");
    /* The port of the root, or one that ignores arrivals, that takes the
       request as a node now: neither still queued there nor anywhere else,
       and overlapping no other request, port or message. */
    if (!Kaffeine_QueueRead(ctx, port, &q) ||
        (ctx->mem[port + G_MP_FLAGS] == G_PA_SIGNAL && ctx->guest_exec_task &&
         m68k_read32(ctx, port + G_MP_SIGTASK) != ctx->guest_exec_task) ||
        !Kaffeine_IOMessagePreflight(ctx, &q, req, length, entry))
        return Kaffeine_IORefuse(ctx, "SendIO(): the reply port cannot take the request, "
                                     "or it is still queued");
    fresh = copy && !(entry && entry->copy);
    if (fresh && !entry && !(entry = request_entry(ctx, 0)))
        return Kaffeine_IORefuse(ctx, "SendIO(): no free I/O request record for a copy");

    if (fresh)
    {
        /* All or nothing: what was made goes again in reverse order. */
        struct MsgPort *reply = ctx->io_copy_port;
        struct IORequest *made;
        LONG error;

        if (!reply && !(reply = CreateMsgPort()))
            return Kaffeine_IORefuse(ctx, "SendIO(): no native reply port for copies");
        made = (struct IORequest *)CreateIORequest(reply, sizeof(struct timerequest));
        if (!made)
        {
            if (reply != ctx->io_copy_port)
                DeleteMsgPort(reply);
            return Kaffeine_IORefuse(ctx, "SendIO(): no native request for a copy");
        }
        error = OpenDevice("timer.device", (IPTR)unit, made, source->open_flags);
        if (error)
        {
            bug("[kaffeine] SendIO(%08lx): native timer.device unit %lu: error %ld\n",
                (unsigned long)req, (unsigned long)unit, (long)error);
            DeleteIORequest(made);
            if (reply != ctx->io_copy_port)
                DeleteMsgPort(reply);
            return Kaffeine_IORefuse(ctx, "SendIO(): the native open of a copy failed");
        }
        ctx->io_copy_port = reply;
        entry->guest = req;
        entry->native = made;
        entry->reply = NULL;
        entry->unit = unit;
        entry->kind = KAFFEINE_IO_TIMER;
        entry->open_flags = source->open_flags;
        entry->copy = 1;
        bug("[kaffeine] SendIO(%08lx): a copy of %08lx on unit %lu\n", (unsigned long)req,
            (unsigned long)source->guest, (unsigned long)unit);
    }

    native = (struct timerequest *)entry->native;
    native->tr_node.io_Command = command;
    native->tr_node.io_Flags = 0;
    native->tr_time.tv_secs = m68k_read32(ctx, req + G_TV_SECS);
    native->tr_time.tv_micro = m68k_read32(ctx, req + G_TV_MICRO);
    entry->port = port;
    entry->pending = 1;
    entry->replied = entry->waiting = 0;
    ctx->mem[req + G_IO_FLAGS] = 0;
    ctx->mem[req + G_LN_TYPE] = NT_MESSAGE;
    ctx->mem[req + G_IO_ERROR] = 0;
    io_hooks(ctx);
    io_signals_update(ctx);
    SendIO(&native->tr_node);
    return 0;
}

/* -468: CheckIO(A1=iORequest) -> D0: the request when it is complete. For
   a request that the bridge knows, whether its native request is at the
   device, never the guest's markers, which CreateExtIO() sets before any
   I/O; for another one, the markers, as Exec. */
IPTR Kaffeine_CheckIO(struct KaffeineContext *ctx, void *cpu)
{
    ULONG req = THUNK_A(1);
    struct KaffeineIORequest *entry;

    if (!request_range(ctx, req, G_IOREQUEST_SIZE))
        return Kaffeine_IORefuse(ctx, "CheckIO(): the request is not in guest memory");
    if (!io_pumped(ctx))
        return 20;
    entry = request_entry(ctx, req);
    if (entry)
        return entry->pending ? 0 : req;
    return in_flight(ctx, req) ? 0 : req;
}

/* -474: WaitIO(A1=iORequest) -> D0=io_Error. A request in flight is waited
   for by the root alone, outside callbacks: natively, or with guest tasks
   blocked on its private port while the others run; it is finished in
   place, neither queued nor signalled. One already replied leaves its
   port, as Exec's Remove(), and the port's signal stays. Other complete
   requests return at once. */
IPTR Kaffeine_WaitIO(struct KaffeineContext *ctx, void *cpu)
{
    ULONG req = THUNK_A(1);
    struct KaffeineIORequest *entry;
    BOOL root;
    IPTR result;

    if (!request_range(ctx, req, G_IOREQUEST_SIZE))
        return Kaffeine_IORefuse(ctx, "WaitIO(): the request is not in guest memory");
    entry = request_entry(ctx, req);
    if (!entry)
    {
        if (in_flight(ctx, req))
            return Kaffeine_IORefuse(ctx, "WaitIO(): no I/O of this request is in flight here "
                                         "for its markers to wait for");
        return (IPTR)(LONG)(BYTE)ctx->mem[req + G_IO_ERROR];
    }
    root = root_caller(ctx, cpu) && !ctx->callback_depth;
    for (;;)
    {
        /* A reply that the pump finds now is finished in place as well. */
        if (entry->pending && root)
            entry->waiting = 1;
        if (!io_pumped(ctx))
            return 20;
        if (!entry->pending)
            break;
        if (!root)
            return Kaffeine_IORefuse(ctx, "WaitIO(): only the root task waits for its I/O, "
                                         "outside callbacks");
        if (ctx->sched)
        {
            entry->waiting = 2;
            if (ctx->task_ops->block_port(ctx, cpu, entry->copy ? ctx->io_copy_port : entry->reply,
                                          Kaffeine_WaitIO, &result))
                return result;
            /* a message is there, perhaps another copy's: pump again */
            continue;
        }
        /* AROS's WaitIO() waits while the device marks the request
           NT_MESSAGE, and takes it from the port once it is replied. */
        WaitIO(entry->native);
        if (entry->native->io_Message.mn_Node.ln_Type != NT_REPLYMSG)
            return Kaffeine_IORefuse(ctx, "WaitIO(): the native request came back unreplied");
        if (!io_finish(ctx, entry))
            return 20;
        /* another copy's reply may have arrived on the shared port */
    }
    if (linked(ctx, entry))
        Kaffeine_GuestRemoveMsg(ctx, entry->port, req);
    entry->waiting = 0;
    return (IPTR)(LONG)(BYTE)ctx->mem[req + G_IO_ERROR];
}

/* -480: AbortIO(A1=iORequest) -> D0, also timer.device's -36. Of a request
   in flight, the native AbortIO() of the root; its reply comes as any
   other. Of any other request -1, as timer.device answers for a request
   it no longer holds. */
IPTR Kaffeine_AbortIO(struct KaffeineContext *ctx, void *cpu)
{
    ULONG req = THUNK_A(1);
    struct KaffeineIORequest *entry;

    if (!request_range(ctx, req, G_IOREQUEST_SIZE))
        return Kaffeine_IORefuse(ctx, "AbortIO(): the request is not in guest memory");
    if (!io_pumped(ctx))
        return 20;
    entry = request_entry(ctx, req);
    if (!entry || !entry->pending)
        return (IPTR)(LONG)-1;
    if (!root_caller(ctx, cpu))
        return Kaffeine_IORefuse(ctx, "AbortIO(): only the root task aborts its I/O");
    return (IPTR)(LONG)AbortIO(entry->native);
}

/* ── keyboard.device and input.device bases ──────────────────────── */

/* Their requests go through Exec's DoIO() above. Called on the device
   directly, Open, Close, Expunge, BeginIO, AbortIO and input's
   PeekQualifier would act on native state or asynchronous I/O that the
   bridge does not have yet, so they stop the guest. */
static IPTR device_unsupported(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return Kaffeine_IORefuse(ctx, "keyboard.device/input.device: this device vector is not supported yet");
}

const struct M68KThunkEntry kaffeine_thunks_keyboard[] = {
    { 6, device_unsupported }, { 12, device_unsupported }, { 18, device_unsupported },
    { 24, device_unsupported }, { 30, device_unsupported }, { 36, device_unsupported },
    { 0, NULL }
};
const ULONG kaffeine_thunks_keyboard_count =
    sizeof(kaffeine_thunks_keyboard) / sizeof(kaffeine_thunks_keyboard[0]) - 1;

const struct M68KThunkEntry kaffeine_thunks_input[] = {
    { 6, device_unsupported }, { 12, device_unsupported }, { 18, device_unsupported },
    { 24, device_unsupported }, { 30, device_unsupported }, { 36, device_unsupported },
    { 42, device_unsupported },
    { 0, NULL }
};
const ULONG kaffeine_thunks_input_count =
    sizeof(kaffeine_thunks_input) / sizeof(kaffeine_thunks_input[0]) - 1;

/* ── End of the program ──────────────────────────────────────────── */

void Kaffeine_IOCleanup(struct KaffeineContext *ctx)
{
    ULONG i;

    if (ctx->exec_task && FindTask(NULL) != ctx->exec_task)
    {
        /* The reply ports and the signals belong to the task that ran the
           guest; another task must not free them. */
        bug("[kaffeine] I/O cleanup outside the task that ran the guest, leaving it\n");
        return;
    }
    /* Nothing reaches the guest from here on. Copies and orphans first,
       which share the copy port: one in flight is aborted and taken back
       before its open closes, and the port goes empty. Then the opens. */
    ctx->io_poll = NULL;
    ctx->io_wait = NULL;
    ctx->io_release = NULL;
    for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS; i++)
        if (ctx->io_requests[i].copy)
        {
            bug("[kaffeine] closing a copied timer request that the 68k program left\n");
            close_native(&ctx->io_requests[i]);
        }
    if (ctx->io_copy_port)
    {
        DeleteMsgPort(ctx->io_copy_port);
        ctx->io_copy_port = NULL;
    }
    for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS; i++)
        if (ctx->io_requests[i].native)
        {
            bug("[kaffeine] closing a device that the 68k program left open\n");
            close_native(&ctx->io_requests[i]);
        }
    /* the App objects send to ports with the signals of the program */
    Kaffeine_WorkbenchCleanup(ctx);
    for (i = 0; i < 32; i++)
        if (ctx->guest_signals & (1UL << i))
            FreeSignal((LONG)i);
    ctx->guest_signals = 0;
    ctx->io_signals = 0;
    memset(ctx->io_requests, 0, sizeof(ctx->io_requests));
    memset(ctx->io_ports, 0, sizeof(ctx->io_ports));
}
