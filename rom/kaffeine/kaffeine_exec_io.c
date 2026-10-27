/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Exec ports, I/O requests and signals of 68k code, timer.device
    through DoIO(), and console.device's no-window library unit.
    Ports and requests stay 68k structures in the guest
    arena. Behind each open request stand a native request of the native
    size and its own reply port, which only this file passes to native
    Exec; DoIO() copies the fields of the command across. Asynchronous I/O
    and the other devices need guest reply queues and adapters that do not
    exist yet: until then the guest stops with return code 20, or the open
    fails as it does for an unknown device.
*/

#include "kaffeine_intern.h"
#include "kaffeine_exec_io.h"
#include "kaffeine_thunks_console.h"
#include "kaffeine_offsets.h"
#include "kaffeine_workbench.h"

#include <aros/debug.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <string.h>

/* The 68k Message, IORequest, timerequest, MsgPort and Library of the NDK */
enum
{
    G_LN_TYPE = 8, G_LN_NAME = 10, G_MN_REPLYPORT = 14, G_MN_LENGTH = 18,
    G_MESSAGE_SIZE = 20,
    G_IO_DEVICE = 20, G_IO_UNIT = 24, G_IO_COMMAND = 28, G_IO_FLAGS = 30,
    G_IO_ERROR = 31, G_IOREQUEST_SIZE = 32,
    G_TV_SECS = 32, G_TV_MICRO = 36, G_TIMEREQUEST_SIZE = 40,
    G_MP_FLAGS = 14, G_MP_SIGBIT = 15, G_MP_SIGTASK = 16, G_MP_MSGLIST = 20,
    G_LH_TAIL = 4, G_LH_TAILPRED = 8,
    G_LIB_NEGSIZE = 16, G_LIB_POSSIZE = 18, G_LIB_VERSION = 20, G_LIB_REVISION = 22
};

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
        ctx->guest_signals |= 1UL << got;
    return (IPTR)(LONG)got;
}

/* -336: FreeSignal(D0=signalNum). Only a bit that the guest allocated goes
   back: the other bits of the native task are not the guest's to free. */
IPTR Kaffeine_FreeSignal(struct KaffeineContext *ctx, void *cpu)
{
    BYTE sig = (BYTE)THUNK_D(0);

    if (sig < 0)
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
   behind its Task snapshot. Other tasks cannot be signalled yet. */
IPTR Kaffeine_Signal(struct KaffeineContext *ctx, void *cpu)
{
    ULONG task = THUNK_A(1);

    if (!task || task != guest_task(ctx))
    {
        bug("[kaffeine] Signal(%08lx): not the current task\n", (unsigned long)task);
        return Kaffeine_IORefuse(ctx, "Signal(): only the current task can be signalled so far");
    }
    Signal(FindTask(NULL), THUNK_D(0));
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
   one that CreateMsgPort() gave, whatever the guest wrote since. */
IPTR Kaffeine_DeleteMsgPort(struct KaffeineContext *ctx, void *cpu)
{
    ULONG port = THUNK_A(0);
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
    Kaffeine_AppPortGone(ctx, port);
    if (ctx->guest_signals & (1UL << entry->sigbit))
    {
        ctx->guest_signals &= ~(1UL << entry->sigbit);
        FreeSignal(entry->sigbit);
    }
    {
        ULONG i;
        for (i = 0; i < KAFFEINE_MAX_MESSAGE_PORTS; ++i)
            if (ctx->message_ports[i] == port) ctx->message_ports[i] = 0;
    }
    entry->guest = 0;
    Kaffeine_HeapFree(ctx, port, M68K_SIZEOF_MSGPORT);
    return 0;
}

/* ── I/O requests ────────────────────────────────────────────────── */

static struct KaffeineIORequest *request_entry(struct KaffeineContext *ctx, ULONG req)
{
    ULONG i;

    for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS; i++)
        if (ctx->io_requests[i].guest == req)
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

        if (entry->capacity && req >= entry->guest && req - entry->guest < entry->capacity)
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

static void close_native(struct KaffeineIORequest *entry)
{
    CloseDevice(entry->native);
    DeleteIORequest(entry->native);
    DeleteMsgPort(entry->reply);
    entry->native = NULL;
    entry->reply = NULL;
    entry->unit = 0;
    entry->kind = KAFFEINE_IO_NONE;
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
   native device stays open behind freed memory. */
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
    if (entry->native)
    {
        bug("[kaffeine] DeleteIORequest(%08lx): still open, closing it\n", (unsigned long)req);
        close_native(entry);
    }
    Kaffeine_HeapFree(ctx, req, entry->capacity);
    memset(entry, 0, sizeof(*entry));
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

/* Allocate the name first: a failed Device registration must leave no
   name allocation or partially published console identity behind. */
static BOOL console_base(struct KaffeineContext *ctx, struct Device *native)
{
    static const char name[] = "console.device";
    ULONG base, text;

    if (ctx->console_base)
        return TRUE;
    text = Kaffeine_HeapAlloc(ctx, sizeof(name), MEMF_PUBLIC);
    if (!text)
        return FALSE;
    base = Kaffeine_SetupDeviceBase(ctx, name, KAFFEINE_CONSOLE_VECTORS,
                                   kaffeine_thunks_console, kaffeine_thunks_console_count);
    if (!base)
    {
        Kaffeine_HeapFree(ctx, text, sizeof(name));
        return FALSE;
    }
    memcpy(ctx->mem + text, name, sizeof(name));
    m68k_write32(ctx, base + G_LN_NAME, text);
    ctx->mem[base + G_LN_TYPE] = NT_DEVICE;
    m68k_write16(ctx, base + G_LIB_NEGSIZE, KAFFEINE_CONSOLE_VECTORS * 6);
    m68k_write16(ctx, base + G_LIB_POSSIZE, M68K_SIZEOF_DEVICE);
    m68k_write16(ctx, base + G_LIB_VERSION, native->dd_Library.lib_Version);
    m68k_write16(ctx, base + G_LIB_REVISION, native->dd_Library.lib_Revision);
    ctx->console_base = base;
    return TRUE;
}

/* -444: OpenDevice(A0=devName, D0=unitNumber, A1=iORequest, D1=flags) -> D0.
   timer.device and console.device CONU_LIBRARY have native adapters.
   As Exec, a name of 0 means timer.device and 1 input.device. The guest
   request needs a timerequest for timer, only the IORequest prefix for the
   no-window console unit, including a zero mn_Length and no reply port.
   It gets a guest Device base and unit number; the native request, of the
   native size, never leaves the bridge. Opening a request again closes the
   native open behind it first, which Exec would leak. */
IPTR Kaffeine_OpenDevice(struct KaffeineContext *ctx, void *cpu)
{
    ULONG name_addr = THUNK_A(0), unit = THUNK_D(0), req = THUNK_A(1), flags = THUNK_D(1);
    ULONG size, base;
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
    m68k_write32(ctx, req + G_IO_UNIT, 0);
    entry = request_entry(ctx, req);
    if (entry && entry->native)
    {
        bug("[kaffeine] OpenDevice(%08lx): already open, closing it first\n", (unsigned long)req);
        close_native(entry);
    }
    kind = name && strcmp(name, "timer.device") == 0 ? KAFFEINE_IO_TIMER :
           name && strcmp(name, "console.device") == 0 && unit == 0xFFFFFFFFu ?
           KAFFEINE_IO_CONSOLE : KAFFEINE_IO_NONE;
    if (kind == KAFFEINE_IO_NONE)
    {
        bug("[kaffeine] OpenDevice(%s): no adapter for this device\n", name ? name : "(bad name)");
        if (entry)
            request_forget(entry);
        return open_failed(ctx, req, IOERR_OPENFAIL);
    }
    if (!request_range(ctx, req, kind == KAFFEINE_IO_TIMER ? G_TIMEREQUEST_SIZE : G_IOREQUEST_SIZE))
    {
        if (entry)
            request_forget(entry);
        return open_failed(ctx, req, IOERR_OPENFAIL);
    }
    if (!entry && !(entry = request_entry(ctx, 0)))
        return open_failed(ctx, req, IOERR_OPENFAIL);

    size = kind == KAFFEINE_IO_TIMER ? sizeof(struct timerequest) : sizeof(struct IOStdReq);
    reply = CreateMsgPort();
    native = reply ? (struct IORequest *)CreateIORequest(reply, size) : NULL;
    if (!native)
    {
        if (reply)
            DeleteMsgPort(reply);
        error = IOERR_OPENFAIL;
    }
    else
    {
        error = OpenDevice(kind == KAFFEINE_IO_TIMER ? "timer.device" : "console.device",
                           kind == KAFFEINE_IO_TIMER ? (IPTR)unit : (IPTR)-1, native, flags);
        if (!error && !(kind == KAFFEINE_IO_TIMER ? timer_base(ctx, native->io_Device) :
                                                  console_base(ctx, native->io_Device)))
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
    base = kind == KAFFEINE_IO_TIMER ? ctx->timer_base : ctx->console_base;
    m68k_write32(ctx, req + G_IO_DEVICE, base);
    m68k_write32(ctx, req + G_IO_UNIT, unit);
    ctx->mem[req + G_IO_ERROR] = 0;
    return 0;
}

/* -450: CloseDevice(A1=iORequest) -> D0. Console's library unit clears
   both guest identities; timer retains its existing closed marker. */
IPTR Kaffeine_CloseDevice(struct KaffeineContext *ctx, void *cpu)
{
    ULONG req = THUNK_A(1);
    struct KaffeineIORequest *entry = req ? request_entry(ctx, req) : NULL;
    enum KaffeineIODeviceKind kind;

    if (!entry || !entry->native)
        return 0;
    kind = entry->kind;
    close_native(entry);
    if (request_range(ctx, req, G_IOREQUEST_SIZE))
    {
        m68k_write32(ctx, req + G_IO_DEVICE, kind == KAFFEINE_IO_CONSOLE ? 0 : 0xFFFFFFFF);
        if (kind == KAFFEINE_IO_CONSOLE)
            m68k_write32(ctx, req + G_IO_UNIT, 0);
    }
    request_forget(entry);
    return 0;
}

/* -456: DoIO(A1=iORequest) -> D0=io_Error. As Exec, the request gets
   IOF_QUICK and ln_Type 0 first, and without a device the result is -1.
   For timer.device, TR_ADDREQUEST and TR_GETSYSTIME copy both time fields
   to the native request and the results back. TR_SETSYSTIME would set the
   clock of the whole system, and the AROS driver never replies to an EClock
   delay; they and every other command end with IOERR_NOCMD without
   reaching the device. Console requests always return IOERR_NOCMD, using
   the native record's kind rather than guest-editable device/unit fields. */
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
    ctx->mem[req + G_IO_FLAGS] = IOF_QUICK;
    ctx->mem[req + G_LN_TYPE] = 0;
    entry = request_entry(ctx, req);
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

/* -462: SendIO(A1=iORequest). Asynchronous I/O needs guest reply queues
   that the bridge does not have yet. */
IPTR Kaffeine_SendIO(struct KaffeineContext *ctx, void *cpu)
{
    bug("[kaffeine] SendIO(%08lx)\n", (unsigned long)THUNK_A(1));
    return Kaffeine_IORefuse(ctx, "SendIO(): asynchronous device I/O is not supported yet");
}

/* As Exec: a request is in flight when it is not QUICK and is still an
   unreplied message. Nothing goes in flight through the bridge yet, so only
   a guest edit can make it so. */
static BOOL in_flight(struct KaffeineContext *ctx, ULONG req)
{
    return !(ctx->mem[req + G_IO_FLAGS] & IOF_QUICK) &&
           ctx->mem[req + G_LN_TYPE] == NT_MESSAGE;
}

/* -468: CheckIO(A1=iORequest) -> D0: the request when it is complete. */
IPTR Kaffeine_CheckIO(struct KaffeineContext *ctx, void *cpu)
{
    ULONG req = THUNK_A(1);

    if (!request_range(ctx, req, G_IOREQUEST_SIZE))
        return Kaffeine_IORefuse(ctx, "CheckIO(): the request is not in guest memory");
    return in_flight(ctx, req) ? 0 : req;
}

/* -474: WaitIO(A1=iORequest) -> D0=io_Error. A complete request returns at
   once. Its node is in no guest list, so unlike Exec nothing is removed. */
IPTR Kaffeine_WaitIO(struct KaffeineContext *ctx, void *cpu)
{
    ULONG req = THUNK_A(1);

    if (!request_range(ctx, req, G_IOREQUEST_SIZE))
        return Kaffeine_IORefuse(ctx, "WaitIO(): the request is not in guest memory");
    if (in_flight(ctx, req))
        return Kaffeine_IORefuse(ctx, "WaitIO(): a request in flight needs asynchronous I/O, "
                                     "which is not supported yet");
    return (IPTR)(LONG)(BYTE)ctx->mem[req + G_IO_ERROR];
}

/* -480: AbortIO(A1=iORequest) -> D0. Nothing is in flight through the
   bridge: -1, as timer.device answers for a request it no longer holds. */
IPTR Kaffeine_AbortIO(struct KaffeineContext *ctx, void *cpu)
{
    ULONG req = THUNK_A(1);

    if (!request_range(ctx, req, G_IOREQUEST_SIZE))
        return Kaffeine_IORefuse(ctx, "AbortIO(): the request is not in guest memory");
    return (IPTR)(LONG)-1;
}

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
    memset(ctx->io_requests, 0, sizeof(ctx->io_requests));
    memset(ctx->io_ports, 0, sizeof(ctx->io_ports));
}
