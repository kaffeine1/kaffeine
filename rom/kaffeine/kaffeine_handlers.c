/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Guest ports of native DOS handlers. DeviceProc(), GetDeviceProc(),
    GetConsoleTask() and GetFileSysTask() give the program one guest port
    for each native handler port, the same one at every call; the packets
    it puts there go to the native handler with DoPkt(), their names, locks
    and structures converted. Native ports, DosList nodes and windows stay
    private.
*/
#include <dos/dosextens.h>
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <string.h>
#include "kaffeine_handlers.h"
#include "kaffeine_filehandle.h"
#include "kaffeine_locks.h"
#include "kaffeine_messages.h"
#include "kaffeine_offsets.h"
#include "kaffeine_thunks.h"
#include "kaffeine_thunks_dos.h"

#define PORT_SIZE      34
#define DEVPROC_SIZE   16
#define INFO_SIZE      36
#define DATESTAMP_SIZE 12

static BOOL arena(struct KaffeineContext *ctx, ULONG a, ULONG n)
{
    return ctx->mem && a && a < ctx->mem_size && n <= ctx->mem_size - a;
}

/* A native lock or DiskKey that the 32 bits of the opaque BPTR convention
   of the other thunks can carry */
static BOOL fits(IPTR value)
{
    return value == (IPTR)(ULONG)value;
}

/* The guest port of a native handler port: an empty PA_IGNORE port, which
   the program cannot free, the same one at every call. */
ULONG Kaffeine_HandlerPort(struct KaffeineContext *ctx, struct MsgPort *native)
{
    unsigned i, slot = KAFFEINE_MAX_HANDLERS;
    ULONG port;

    if (!native)
        return 0;
    for (i = 0; i < KAFFEINE_MAX_HANDLERS; ++i)
    {
        if (ctx->handlers[i].guest && ctx->handlers[i].native == native)
            return ctx->handlers[i].guest;
        if (!ctx->handlers[i].guest && slot == KAFFEINE_MAX_HANDLERS)
            slot = i;
    }
    port = slot < KAFFEINE_MAX_HANDLERS ? Kaffeine_MirrorAlloc(ctx, PORT_SIZE) : 0;
    if (!port)
    {
        SetIoErr(ERROR_NO_FREE_STORE);
        return 0;
    }
    memset(ctx->mem + port, 0, PORT_SIZE);
    ctx->mem[port + 8] = NT_MSGPORT;
    ctx->mem[port + 14] = PA_IGNORE;
    m68k_write32(ctx, port + 20, port + 24);
    m68k_write32(ctx, port + 28, port + 20);
    ctx->handlers[slot].guest = port;
    ctx->handlers[slot].native = native;
    ctx->handler_packet = Kaffeine_HandlerPacket;
    return port;
}

/* The native port behind a guest one: none for 0, the native port of a
   handler port, and the handler of a FileHandle of the program for its
   fh_Type, as SetConsoleTask(fh->fh_Type) passes it. */
static BOOL handler_native(struct KaffeineContext *ctx, ULONG port, struct MsgPort **native)
{
    unsigned i;

    *native = NULL;
    if (!port)
        return TRUE;
    for (i = 0; i < KAFFEINE_MAX_HANDLERS; ++i)
        if (ctx->handlers[i].guest && ctx->handlers[i].guest == port)
        {
            *native = ctx->handlers[i].native;
            return TRUE;
        }
    return Kaffeine_FilePort(ctx, port, native);
}

void Kaffeine_HandlerCleanup(struct KaffeineContext *ctx)
{
    unsigned i;

    for (i = 0; i < KAFFEINE_MAX_DEVPROCS; ++i)
        if (ctx->devprocs[i].native)
        {
            Kaffeine_LockScopeDrop(ctx, KAFFEINE_LOCK_DEVPROC, ctx->devprocs[i].guest);
            if (ctx->devprocs[i].view_lock) UnLock(ctx->devprocs[i].view_lock);
            FreeDeviceProc(ctx->devprocs[i].native);
        }
    Kaffeine_LockScopeDrop(ctx, KAFFEINE_LOCK_LEGACY, 1);
    if (ctx->legacy_device_lock) UnLock(ctx->legacy_device_lock);
    ctx->legacy_device_lock = 0;
    memset(ctx->devprocs, 0, sizeof(ctx->devprocs));
    memset(ctx->handlers, 0, sizeof(ctx->handlers));
    ctx->handler_packet = NULL;
}

/* A name of the program: NULL, or a string that ends in guest memory */
static BOOL guest_name(struct KaffeineContext *ctx, ULONG addr, const char **name)
{
    *name = NULL;
    if (!addr)
        return TRUE;
    if (!arena(ctx, addr, 1) || !memchr(ctx->mem + addr, 0, ctx->mem_size - addr))
        return FALSE;
    *name = (const char *)ctx->mem + addr;
    return TRUE;
}

/* pr_ConsoleTask or pr_FileSystemTask of the program's Process while that
   runs; 0 in a child task of the program */
static ULONG process_field(struct KaffeineContext *ctx, ULONG offset)
{
    ULONG task = ctx->guest_exec_task;

    if (!task || !arena(ctx, ctx->m68k_sysbase, M68K_EXECBASE_THISTASK + 4) ||
        m68k_read32(ctx, ctx->m68k_sysbase + M68K_EXECBASE_THISTASK) != task ||
        !arena(ctx, task, M68K_SIZEOF_PROCESS))
        return 0;
    return task + offset;
}

/* The guest port of the native console or filesystem task: the one that
   the program set, as long as it stands for that native port, else the
   port of the handler. AmigaOS returns what was set, so that
   GetConsoleTask() is the fh_Type given to SetConsoleTask(). */
static ULONG task_port(struct KaffeineContext *ctx, ULONG field, struct MsgPort *native)
{
    struct MsgPort *set;
    ULONG own = field ? m68k_read32(ctx, field) : 0;

    if (!native)
        return 0;
    if (own && handler_native(ctx, own, &set) && set == native)
        return own;
    return Kaffeine_HandlerPort(ctx, native);
}

static IPTR get_task(struct KaffeineContext *ctx, ULONG offset, struct MsgPort *native)
{
    ULONG field = process_field(ctx, offset), port = task_port(ctx, field, native);

    if (field && (port || !native))
        m68k_write32(ctx, field, port);
    return port;
}

/* The port goes to native DOS as the native port it stands for; the
   Process keeps the guest one. Anything else stops the program. */
static IPTR set_task(struct KaffeineContext *ctx, ULONG offset, ULONG port, BOOL console)
{
    ULONG field = process_field(ctx, offset), previous;
    struct MsgPort *native, *old;

    if (!handler_native(ctx, port, &native))
        return Kaffeine_MessageFailure(ctx, console ? "SetConsoleTask port" : "SetFileSysTask port");
    old = console ? SetConsoleTask(native) : SetFileSysTask(native);
    previous = task_port(ctx, field, old);
    if (field)
        m68k_write32(ctx, field, port);
    return previous;
}

/* -510: GetConsoleTask() -> D0=port */
IPTR kaffeine_dos_GetConsoleTask(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return get_task(ctx, M68K_PR_CONSOLETASK, GetConsoleTask());
}

/* -516: SetConsoleTask(D1=handler) -> D0=previous */
IPTR kaffeine_dos_SetConsoleTask(struct KaffeineContext *ctx, void *cpu)
{
    return set_task(ctx, M68K_PR_CONSOLETASK, THUNK_D(1), TRUE);
}

/* -522: GetFileSysTask() -> D0=port */
IPTR kaffeine_dos_GetFileSysTask(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return get_task(ctx, M68K_PR_FILESYSTEMTASK, GetFileSysTask());
}

/* -528: SetFileSysTask(D1=task) -> D0=previous */
IPTR kaffeine_dos_SetFileSysTask(struct KaffeineContext *ctx, void *cpu)
{
    return set_task(ctx, M68K_PR_FILESYSTEMTASK, THUNK_D(1), FALSE);
}

static BOOL devproc_guard(struct DevProc *, BOOL *);

/* -174: DeviceProc(D1=name): legacy assign locks need a retained typed
   view too. Use the native DevProc form to validate its live node before
   duplicating a borrowed assign reference under the DosList read lock. */
IPTR kaffeine_dos_DeviceProc(struct KaffeineContext *ctx, void *cpu)
{
    const char *name;
    struct DevProc *dp;
    ULONG port = 0, token = 0;
    BOOL held = FALSE;
    SIPTR error;
    if (!guest_name(ctx, THUNK_D(1), &name))
    {
        SetIoErr(ERROR_BAD_NUMBER);
        return 0;
    }
    Kaffeine_LockScopeDrop(ctx, KAFFEINE_LOCK_LEGACY, 1);
    if (ctx->legacy_device_lock) UnLock(ctx->legacy_device_lock);
    ctx->legacy_device_lock = 0;
    dp = GetDeviceProc((CONST_STRPTR)name, NULL);
    if (!dp) return 0;
    if ((dp->dvp_Flags & DVPF_UNLOCK) || !devproc_guard(dp, &held))
    {
        if (held) UnLockDosList(LDF_ALL | LDF_READ);
        FreeDeviceProc(dp);
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return 0;
    }
    port = Kaffeine_HandlerPort(ctx, dp->dvp_Port);
    if (port && dp->dvp_Lock)
    {
        ctx->legacy_device_lock = DupLock(dp->dvp_Lock);
        if (ctx->legacy_device_lock)
            token = Kaffeine_LockBorrow(ctx, ctx->legacy_device_lock, KAFFEINE_LOCK_LEGACY, 1);
        if (!token)
        {
            error = IoErr();
            if (ctx->legacy_device_lock) UnLock(ctx->legacy_device_lock);
            ctx->legacy_device_lock = 0;
            port = 0;
            SetIoErr(error);
        }
    }
    error = port ? (SIPTR)token : IoErr();
    if (held) UnLockDosList(LDF_ALL | LDF_READ);
    FreeDeviceProc(dp);
    SetIoErr(error);
    return port;
}

static int devproc_entry(struct KaffeineContext *ctx, ULONG guest)
{
    int i;

    for (i = 0; i < KAFFEINE_MAX_DEVPROCS; ++i)
        if (guest && ctx->devprocs[i].guest == guest)
            return i;
    return -1;
}

/* The guest DevProc of a native one: the guest port of its handler, the
   borrowed FileLock view, the flags, and no DosList node */
static BOOL devproc_fill(struct KaffeineContext *ctx, ULONG guest, struct DevProc *dp, BPTR *view)
{
    SIPTR error = IoErr();
    ULONG port = Kaffeine_HandlerPort(ctx, dp->dvp_Port), lock = 0;
    *view = 0;
    if (dp->dvp_Port && !port) return FALSE;
    /* Native DevProc does not retain normal assign/current-directory locks.
       The guest view needs its own reference until advance or release. */
    if (dp->dvp_Lock)
    {
        *view = DupLock(dp->dvp_Lock);
        if (!*view) return FALSE;
        lock = Kaffeine_LockBorrow(ctx, *view, KAFFEINE_LOCK_DEVPROC, guest);
        if (!lock)
        {
            SIPTR failed = IoErr();
            UnLock(*view); *view = 0;
            SetIoErr(failed);
            return FALSE;
        }
    }
    m68k_write32(ctx, guest, port);
    m68k_write32(ctx, guest + 4, lock);
    m68k_write32(ctx, guest + 8, dp->dvp_Flags);
    m68k_write32(ctx, guest + 12, 0);
    SetIoErr(error);
    return TRUE;
}

/* DevProc does not retain its DosList node. Hold the native list while
   validating a returned assign lock, and while advancing a multi-assign.
   Compare potentially expired pointers only; never dereference them until
   the live list has supplied the same node/reference under its read lock. */
static BOOL devproc_guard(struct DevProc *dp, BOOL *held)
{
    struct DosList *node;
    struct AssignList *assign;
    unsigned count = 0;
    *held = FALSE;
    if (!dp->dvp_DevNode) return TRUE; /* current directory / PROGDIR */
    node = LockDosList(LDF_ALL | LDF_READ);
    *held = TRUE;
    while ((node = NextDosEntry(node, LDF_ALL)) && ++count <= 65536)
        if (node == dp->dvp_DevNode)
        {
            if (!dp->dvp_Lock || (dp->dvp_Flags & DVPF_UNLOCK)) return TRUE;
            if (node->dol_Type != DLT_DIRECTORY) return FALSE;
            if (node->dol_Lock == dp->dvp_Lock) return TRUE;
            for (assign = node->dol_misc.dol_assign.dol_List;
                 assign && ++count <= 65536; assign = assign->al_Next)
                if (assign->al_Lock == dp->dvp_Lock) return TRUE;
            return FALSE;
        }
    return FALSE;
}

/* -642: GetDeviceProc(D1=name, D2=dp) -> D0=dp. A DevProc of an earlier
   call goes to native DOS as its native one, which moves on to the next
   drawer of the assign or is freed when there is none; the guest DevProc
   follows it. Any other DevProc stops the program. */
IPTR kaffeine_dos_GetDeviceProc(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_D(2);
    struct DevProc *old = NULL, *dp;
    const char *name;
    BOOL held = FALSE, valid;
    int i;

    if (!guest_name(ctx, THUNK_D(1), &name))
    {
        SetIoErr(ERROR_BAD_NUMBER);
        return 0;
    }
    if (guest)
    {
        i = devproc_entry(ctx, guest);
        if (i < 0)
            return Kaffeine_MessageFailure(ctx, "GetDeviceProc DevProc");
        old = ctx->devprocs[i].native;
    }
    else
    {
        for (i = 0; i < KAFFEINE_MAX_DEVPROCS && ctx->devprocs[i].guest; ++i)
            ;
        guest = i < KAFFEINE_MAX_DEVPROCS ? Kaffeine_MirrorAlloc(ctx, DEVPROC_SIZE) : 0;
        if (!guest)
        {
            SetIoErr(ERROR_NO_FREE_STORE);
            return 0;
        }
    }
    if (old)
    {
        Kaffeine_LockScopeDrop(ctx, KAFFEINE_LOCK_DEVPROC, guest);
        if (ctx->devprocs[i].view_lock) UnLock(ctx->devprocs[i].view_lock);
        ctx->devprocs[i].view_lock = 0;
    }
    if (old)
    {
        valid = (old->dvp_Flags & DVPF_ASSIGN) && old->dvp_DevNode && devproc_guard(old, &held);
        if (valid && old->dvp_DevNode->dol_Type != DLT_DIRECTORY) valid = FALSE;
        if (!valid)
        {
            if (held) UnLockDosList(LDF_ALL | LDF_READ);
            held = FALSE;
            FreeDeviceProc(old);
            old = NULL;
            dp = NULL;
            SetIoErr(ERROR_NO_MORE_ENTRIES);
        }
        else dp = GetDeviceProc((CONST_STRPTR)name, old);
    }
    else dp = GetDeviceProc((CONST_STRPTR)name, NULL);
    if (dp)
    {
        SIPTR error;
        valid = held || devproc_guard(dp, &held);
        if (!valid) SetIoErr(ERROR_NO_MORE_ENTRIES);
        else valid = devproc_fill(ctx, guest, dp, &ctx->devprocs[i].view_lock);
        error = IoErr();
        if (held) UnLockDosList(LDF_ALL | LDF_READ);
        held = FALSE;
        if (valid)
        {
            ctx->devprocs[i].guest = guest;
            ctx->devprocs[i].native = dp;
            SetIoErr(error);
            return guest;
        }
        Kaffeine_LockScopeDrop(ctx, KAFFEINE_LOCK_DEVPROC, guest);
        FreeDeviceProc(dp);
        SetIoErr(error);
    }
    else if (held) UnLockDosList(LDF_ALL | LDF_READ);
    ctx->devprocs[i].guest = 0;
    ctx->devprocs[i].native = NULL;
    Kaffeine_MirrorRelease(ctx, guest);
    return 0;
}

/* -648: FreeDeviceProc(D1=dp): only DevProcs of GetDeviceProc(), as
   FreeDosObject() frees only its own objects */
IPTR kaffeine_dos_FreeDeviceProc(struct KaffeineContext *ctx, void *cpu)
{
    int i = devproc_entry(ctx, THUNK_D(1));

    if (i >= 0)
    {
        Kaffeine_LockScopeDrop(ctx, KAFFEINE_LOCK_DEVPROC, ctx->devprocs[i].guest);
        if (ctx->devprocs[i].view_lock) UnLock(ctx->devprocs[i].view_lock);
        ctx->devprocs[i].view_lock = 0;
        FreeDeviceProc(ctx->devprocs[i].native);
        Kaffeine_MirrorRelease(ctx, ctx->devprocs[i].guest);
        ctx->devprocs[i].guest = 0;
        ctx->devprocs[i].native = NULL;
    }
    return 0;
}

/* A name of a packet, a BSTR of the program, as the native handler takes
   it: a C string where AROS has AROS_FAST_BSTR, else a BSTR. A BSTR of 0
   is the empty name. */
struct PacketName
{
    union { LONG align; UBYTE text[258]; } u;
};

static BOOL packet_name(struct KaffeineContext *ctx, ULONG bstr, struct PacketName *name, SIPTR *arg)
{
    ULONG addr = bstr << 2, length = 0;

    if ((bstr >> 30) || (bstr && !Kaffeine_GuestBytes(ctx, addr, 1)))
        return FALSE;
    if (bstr)
    {
        length = ctx->mem[addr];
        if (!Kaffeine_GuestBytes(ctx, addr, length + 1))
            return FALSE;
    }
#ifdef AROS_FAST_BSTR
    if (length)
        memcpy(name->u.text, ctx->mem + addr + 1, length);
    name->u.text[length] = 0;
    *arg = (SIPTR)name->u.text;
#else
    name->u.text[0] = (UBYTE)length;
    if (length)
        memcpy(name->u.text + 1, ctx->mem + addr + 1, length);
    name->u.text[length + 1] = 0;
    *arg = (SIPTR)MKBADDR(name->u.text);
#endif
    return TRUE;
}

/* The FileInfoBlock of the program for ACTION_EXAMINE_NEXT, which goes on
   from the entry that it holds */
static void fib_from_guest(struct KaffeineContext *ctx, ULONG guest, struct FileInfoBlock *fib)
{
    fib->fib_DiskKey = m68k_read32(ctx, guest + M68K_FIB_DISKKEY);
    fib->fib_DirEntryType = (LONG)m68k_read32(ctx, guest + M68K_FIB_DIRENTRYTYPE);
    memcpy(fib->fib_FileName, ctx->mem + guest + M68K_FIB_FILENAME, M68K_FIB_FILENAME_SIZE);
    fib->fib_Protection = (LONG)m68k_read32(ctx, guest + M68K_FIB_PROTECTION);
    fib->fib_EntryType = (LONG)m68k_read32(ctx, guest + M68K_FIB_ENTRYTYPE);
    fib->fib_Size = m68k_read32(ctx, guest + M68K_FIB_SIZE);
    fib->fib_NumBlocks = m68k_read32(ctx, guest + M68K_FIB_NUMBLOCKS);
    fib->fib_Date.ds_Days = (LONG)m68k_read32(ctx, guest + M68K_FIB_DATE_DAYS);
    fib->fib_Date.ds_Minute = (LONG)m68k_read32(ctx, guest + M68K_FIB_DATE_MINUTE);
    fib->fib_Date.ds_Tick = (LONG)m68k_read32(ctx, guest + M68K_FIB_DATE_TICK);
    memcpy(fib->fib_Comment, ctx->mem + guest + M68K_FIB_COMMENT, M68K_FIB_COMMENT_SIZE);
    fib->fib_OwnerUID = m68k_read16(ctx, guest + M68K_FIB_OWNERUID);
    fib->fib_OwnerGID = m68k_read16(ctx, guest + M68K_FIB_OWNERGID);
}

/* The packets of a handler port go to the native handler with DoPkt(),
   each argument of the action converted: BSTR names, locks with the
   opaque BPTR convention, the FileInfoBlock and InfoData that the handler
   fills, and the DateStamp of ACTION_SET_DATE. An action that the bridge
   does not convert gets 0 and ERROR_ACTION_NOT_KNOWN, as a handler answers
   one it does not know, and never reaches the native handler. */
/* A live token is still specific to its filesystem handler. Never feed a
   foreign native filesystem key to a different packet implementation. */
static BOOL packet_lock(struct KaffeineContext *ctx, ULONG token,
                         struct MsgPort *handler, SIPTR *out)
{
    BPTR lock;
    if (!Kaffeine_LockForHandler(ctx, token, handler, &lock)) return FALSE;
    *out = (SIPTR)lock;
    return TRUE;
}

IPTR Kaffeine_HandlerPacket(struct KaffeineContext *ctx, ULONG port, ULONG msg)
{
    struct KaffeineQueue reply;
    struct MsgPort *native = NULL;
    struct PacketName name, name2;
    struct FileInfoBlock *fib = NULL;
    struct InfoData info;
    struct DateStamp stamp;
    SIPTR arg[4] = { 0, 0, 0, 0 }, caller_error, result;
    ULONG a[4], out = 0;
    LONG action, res1 = 0, res2 = 0;
    BOOL ok = TRUE, call = TRUE, lock_result = FALSE;
    unsigned i;

    for (i = 0; i < KAFFEINE_MAX_HANDLERS; ++i)
        if (ctx->handlers[i].guest && ctx->handlers[i].guest == port)
            native = ctx->handlers[i].native;
    if (!native || !Kaffeine_PacketMessage(ctx, msg, &reply))
        return Kaffeine_MessageFailure(ctx, "handler packet");
    if (!Kaffeine_PacketPort(ctx, port))
        return Kaffeine_MessageFailure(ctx, "handler port");
    if (!Kaffeine_PacketTask(ctx))
        return Kaffeine_MessageFailure(ctx, "handler packet from a foreign task");

    action = (LONG)m68k_read32(ctx, msg + 28);
    for (i = 0; i < 4; ++i)
        a[i] = m68k_read32(ctx, msg + 40 + 4 * i);
    memset(&info, 0, sizeof(info));
    switch (action)
    {
        case ACTION_SCREEN_MODE:
        case ACTION_WAIT_CHAR:
            arg[0] = (SIPTR)(LONG)a[0];
            break;
        case ACTION_IS_FILESYSTEM:
        case ACTION_FLUSH:
            break;
        case ACTION_DISK_INFO:
            out = a[0] << 2;
            ok = !(a[0] >> 30) && Kaffeine_PacketRange(ctx, msg, &reply, out, INFO_SIZE);
            arg[0] = (SIPTR)MKBADDR(&info);
            break;
        case ACTION_INFO:
            out = a[1] << 2;
            ok = !(a[1] >> 30) && Kaffeine_PacketRange(ctx, msg, &reply, out, INFO_SIZE);
            if (!packet_lock(ctx, a[0], native, &arg[0])) return 20;
            arg[1] = (SIPTR)MKBADDR(&info);
            break;
        case ACTION_LOCATE_OBJECT:
            arg[2] = (SIPTR)(LONG)a[2];
            /* fall through */
        case ACTION_CREATE_DIR:
            lock_result = TRUE;
            /* fall through */
        case ACTION_DELETE_OBJECT:
            if (!packet_lock(ctx, a[0], native, &arg[0])) return 20;
            ok = packet_name(ctx, a[1], &name, &arg[1]);
            break;
        case ACTION_FREE_LOCK:
            if (!Kaffeine_LockRelease(ctx, a[0])) return 20;
            call = FALSE;
            res1 = DOSTRUE;
            break;
        case ACTION_COPY_DIR:
        case ACTION_PARENT:
            lock_result = TRUE;
            if (!packet_lock(ctx, a[0], native, &arg[0])) return 20;
            break;
        case ACTION_SAME_LOCK:
            if (!packet_lock(ctx, a[0], native, &arg[0])) return 20;
            if (!packet_lock(ctx, a[1], native, &arg[1])) return 20;
            break;
        case ACTION_RENAME_OBJECT:
            if (!packet_lock(ctx, a[0], native, &arg[0])) return 20;
            if (!packet_lock(ctx, a[2], native, &arg[2])) return 20;
            ok = packet_name(ctx, a[1], &name, &arg[1]) && packet_name(ctx, a[3], &name2, &arg[3]);
            break;
        case ACTION_EXAMINE_OBJECT:
        case ACTION_EXAMINE_NEXT:
            out = a[1] << 2;
            ok = !(a[1] >> 30) && Kaffeine_PacketRange(ctx, msg, &reply, out, M68K_SIZEOF_FILEINFOBLK);
            if (!ok)
                break;
            if (!packet_lock(ctx, a[0], native, &arg[0])) return 20;
            fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
            if (!fib)
            {
                call = FALSE;
                res2 = ERROR_NO_FREE_STORE;
                break;
            }
            if (action == ACTION_EXAMINE_NEXT)
                fib_from_guest(ctx, out, fib);
            arg[1] = (SIPTR)MKBADDR(fib);
            break;
        /* dp_Arg1 of the three is not used */
        case ACTION_SET_PROTECT:
            if (!packet_lock(ctx, a[1], native, &arg[1])) return 20;
            arg[3] = (SIPTR)(LONG)a[3];
            ok = packet_name(ctx, a[2], &name, &arg[2]);
            break;
        case ACTION_SET_COMMENT:
            if (!packet_lock(ctx, a[1], native, &arg[1])) return 20;
            ok = packet_name(ctx, a[2], &name, &arg[2]) && packet_name(ctx, a[3], &name2, &arg[3]);
            break;
        case ACTION_SET_DATE:
            if (!packet_lock(ctx, a[1], native, &arg[1])) return 20;
            ok = packet_name(ctx, a[2], &name, &arg[2]) && Kaffeine_GuestBytes(ctx, a[3], DATESTAMP_SIZE);
            if (!ok)
                break;
            stamp.ds_Days = (LONG)m68k_read32(ctx, a[3]);
            stamp.ds_Minute = (LONG)m68k_read32(ctx, a[3] + 4);
            stamp.ds_Tick = (LONG)m68k_read32(ctx, a[3] + 8);
            arg[3] = (SIPTR)&stamp;
            break;
        default:
            call = FALSE;
            res2 = ERROR_ACTION_NOT_KNOWN;
            break;
    }
    if (!ok)
        return Kaffeine_MessageFailure(ctx, "handler packet argument");
    if (call)
    {
        caller_error = IoErr();
        result = DoPkt(native, action, arg[0], arg[1], arg[2], arg[3], 0);
        res2 = (LONG)IoErr();
        if (lock_result)
        {
            BPTR acquired = (BPTR)result;
            result = Kaffeine_LockAdopt(ctx, acquired);
            if (acquired && !result) res2 = (LONG)IoErr();
        }
        else if (fib && result && !fits(fib->fib_DiskKey))
            ok = FALSE;
        else if (fib && result)
            Kaffeine_FIBToGuest(ctx, out, fib);
        else if (result && (action == ACTION_INFO || action == ACTION_DISK_INFO))
            Kaffeine_InfoToGuest(ctx, out, &info);
        res1 = (LONG)result;
        SetIoErr(caller_error);
    }
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    if (!ok)
        return Kaffeine_MessageFailure(ctx, "native lock or DiskKey outside 32 bits");
    Kaffeine_PacketReply(ctx, port, msg, &reply, res1, res2);
    return 0;
}
