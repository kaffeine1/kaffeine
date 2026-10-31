/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Guest FileHandle identities and the synchronous ACTION_READ boundary.
    Native BPTR width and all native handler pointers remain private.
*/
#include "kaffeine_filehandle.h"
#include "kaffeine_locks.h"
#include "kaffeine_thunks.h"
#include "kaffeine_messages.h"
#include "kaffeine_offsets.h"
#include <aros/debug.h>
#include <dos/dosextens.h>
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <string.h>
#include <stdint.h>

static struct KaffeineFile *entry(struct KaffeineContext *ctx, ULONG token)
{
    unsigned i;
    if (!token || token > UINT32_MAX / 4) return NULL;
    for (i = 0; i < KAFFEINE_MAX_FILES; ++i)
        if (ctx->files[i].guest == token * 4) return &ctx->files[i];
    return NULL;
}

static BOOL identity(struct KaffeineContext *ctx, struct KaffeineFile *f)
{
    return f && f->native && Kaffeine_GuestBytes(ctx, f->guest, 86) &&
        m68k_read32(ctx, f->guest + 8) == f->handler &&
        m68k_read32(ctx, f->guest + 36) == f->key;
}

BOOL Kaffeine_FileResolve(struct KaffeineContext *ctx, ULONG token, BPTR *out)
{
    struct KaffeineFile *f;
    *out = (BPTR)0;
    if (!token) return TRUE; /* Individual native APIs define NULL semantics. */
    f = entry(ctx, token);
    if ((ctx->guest_exec_task && (!ctx->m68k_sysbase || ctx->m68k_sysbase >= ctx->mem_size ||
        ctx->mem_size - ctx->m68k_sysbase < M68K_EXECBASE_THISTASK + 4 ||
        m68k_read32(ctx, ctx->m68k_sysbase + M68K_EXECBASE_THISTASK) != ctx->guest_exec_task)) ||
        !identity(ctx, f))
    {
        Kaffeine_MessageFailure(ctx, "file handle");
        return FALSE;
    }
    *out = f->native;
    return TRUE;
}

/* A snapshot is a read-only view of native buffering. Direct guest buffering
   is allowed for raw Read/Write and packets, but cannot be mixed with native
   buffered APIs. Never copy fh_Funcs, fh_Args or native pointer values. */
static BOOL snapshot(struct KaffeineContext *ctx, struct KaffeineFile *f)
{
    const struct FileHandle *n = (const struct FileHandle *)BADDR(f->native);
    ULONG a = f->guest, size = 0, initialized = 0;
    int64_t pos = (int64_t)n->fh_Pos, end = (int64_t)n->fh_End;
    if (pos < -1 || pos > INT32_MAX || end < -1 || end > INT32_MAX ||
        (n->fh_Buf && ((uint64_t)n->fh_BufSize > KAFFEINE_FILE_BUFFER_LIMIT || !f->mirror)))
        return FALSE;
    if (n->fh_Buf)
    {
        size = (ULONG)n->fh_BufSize;
        /* AROS FHF_WRITE: only [0, Pos) has been initialized in a write
           buffer. Read buffers initialize [0, End). Clear all other bytes. */
        if (n->fh_Flags & 0x80000000UL)
            initialized = pos > 0 ? (ULONG)pos : 0;
        else
            initialized = end > 0 ? (ULONG)end : 0;
        if (initialized > size) return FALSE;
        memset(ctx->mem + f->mirror, 0, KAFFEINE_FILE_BUFFER_LIMIT);
        if (initialized) memcpy(ctx->mem + f->mirror, BADDR(n->fh_Buf), initialized);
    }
    memset(ctx->mem + a, 0, KAFFEINE_FILE_SIZE);
    m68k_write32(ctx, a + 4, n->fh_Interactive ? 1 : 0);
    m68k_write32(ctx, a + 8, f->handler);
    m68k_write32(ctx, a + 12, size ? f->mirror >> 2 : 0);
    m68k_write32(ctx, a + 16, (ULONG)pos);
    m68k_write32(ctx, a + 20, (ULONG)end);
    m68k_write32(ctx, a + 36, f->key);
    m68k_write32(ctx, a + 44, size);
    m68k_write32(ctx, a + 48, size ? f->mirror >> 2 : 0);
    memcpy(f->snapshot, ctx->mem + a, KAFFEINE_FILE_SIZE);
    return TRUE;
}

ULONG Kaffeine_FileImport(struct KaffeineContext *ctx, BPTR native, BOOL owned)
{
    struct KaffeineFile temp = {0}, *f = NULL;
    struct KaffeineFileSlab *slab = ctx->file_slabs;
    BOOL new_slab = FALSE;
    const struct FileHandle *n;
    unsigned i;
    SIPTR error;
    if (!native) return 0;
    for (i = 0; i < KAFFEINE_MAX_FILES; ++i)
    {
        if (ctx->files[i].native == native)
        {
            if (!identity(ctx, &ctx->files[i]))
            {
                Kaffeine_MessageFailure(ctx, "reused file identity");
                return 0;
            }
            return ctx->files[i].guest >> 2;
        }
        if (!ctx->files[i].guest && !f) f = &ctx->files[i];
    }
    if (!f || ctx->file_serial == UINT32_MAX) goto fail;
    /* BPTRs have no generation bits. Quarantine 88 bytes per identity in
       guest slabs until teardown, so freed tokens/handler ports cannot alias
       a later file. Only the 32 active associations and live mirrors count
       against their tables. Slabs are bounded by the existing guest heap. */
    if (!slab || slab->used == KAFFEINE_FILE_SLAB_SIZE / KAFFEINE_FILE_SLOT_SIZE)
    {
        slab = AllocMem(sizeof(*slab), MEMF_PUBLIC | MEMF_CLEAR);
        if (!slab) goto fail;
        new_slab = TRUE;
        slab->guest = Kaffeine_HeapAlloc(ctx, KAFFEINE_FILE_SLAB_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
        if (!slab->guest) goto fail;
    }
    temp.guest = slab->guest + slab->used * KAFFEINE_FILE_SLOT_SIZE;
    temp.handler = temp.guest + KAFFEINE_FILE_SIZE;
    temp.key = ctx->file_serial + 1;
    temp.native = native;
    temp.owned = owned;
    n = (const struct FileHandle *)BADDR(native);
    if (n->fh_Buf)
    {
        temp.mirror = Kaffeine_HeapAlloc(ctx, KAFFEINE_FILE_BUFFER_LIMIT, MEMF_PUBLIC | MEMF_CLEAR);
        if (!temp.mirror) goto fail;
    }
    if (!snapshot(ctx, &temp)) goto fail;
    ctx->mem[temp.handler + 8] = 4;
    ctx->mem[temp.handler + 14] = 2;
    m68k_write32(ctx, temp.handler + 20, temp.handler + 24);
    m68k_write32(ctx, temp.handler + 28, temp.handler + 20);
    *f = temp;
    ++slab->used;
    if (new_slab)
    {
        slab->next = ctx->file_slabs;
        ctx->file_slabs = slab;
    }
    ctx->file_serial = temp.key;
    ctx->file_packet = Kaffeine_FilePacket;
    return temp.guest >> 2;
fail:
    error = ERROR_NO_FREE_STORE;
    if (temp.mirror) Kaffeine_HeapFree(ctx, temp.mirror, KAFFEINE_FILE_BUFFER_LIMIT);
    if (temp.guest) memset(ctx->mem + temp.guest, 0, KAFFEINE_FILE_SLOT_SIZE);
    if (new_slab)
    {
        if (slab->guest) Kaffeine_HeapFree(ctx, slab->guest, KAFFEINE_FILE_SLAB_SIZE);
        FreeMem(slab, sizeof(*slab));
    }
    if (owned) Close(native);
    SetIoErr(error);
    return 0;
}

BOOL Kaffeine_FileBufferPrepare(struct KaffeineContext *ctx, ULONG token)
{
    struct KaffeineFile *f = entry(ctx, token);
    const struct FileHandle *n;
    if (!identity(ctx, f) || memcmp(f->snapshot, ctx->mem + f->guest, KAFFEINE_FILE_SIZE))
        return Kaffeine_MessageFailure(ctx, "mixed guest/native file buffering"), FALSE;
    n = (const struct FileHandle *)BADDR(f->native);
    if (n->fh_Buf && f->mirror)
    {
        int64_t count = (n->fh_Flags & 0x80000000UL) ? (int64_t)n->fh_Pos : (int64_t)n->fh_End;
        if (count < 0) count = 0;
        if ((uint64_t)count > KAFFEINE_FILE_BUFFER_LIMIT ||
            memcmp(ctx->mem + f->mirror, BADDR(n->fh_Buf), (size_t)count))
            return Kaffeine_MessageFailure(ctx, "modified native file mirror"), FALSE;
    }
    if ((uint64_t)n->fh_BufSize > KAFFEINE_FILE_BUFFER_LIMIT)
        return Kaffeine_MessageFailure(ctx, "native file buffer capacity"), FALSE;
    if (!f->mirror)
    {
        f->mirror = Kaffeine_HeapAlloc(ctx, KAFFEINE_FILE_BUFFER_LIMIT, MEMF_PUBLIC | MEMF_CLEAR);
        if (!f->mirror) { SetIoErr(ERROR_NO_FREE_STORE); return FALSE; }
    }
    return TRUE;
}

BOOL Kaffeine_FileBufferFinish(struct KaffeineContext *ctx, ULONG token)
{
    struct KaffeineFile *f = entry(ctx, token);
    if (!token) return TRUE;
    if (!f || !snapshot(ctx, f))
    {
        Kaffeine_MessageFailure(ctx, "native file buffer snapshot");
        return FALSE;
    }
    return TRUE;
}

static void process_stream(struct KaffeineContext *ctx, ULONG offset, ULONG token)
{
    ULONG task;
    if (!ctx->m68k_sysbase || ctx->m68k_sysbase >= ctx->mem_size ||
        ctx->mem_size - ctx->m68k_sysbase < M68K_EXECBASE_THISTASK + 4) return;
    task = m68k_read32(ctx, ctx->m68k_sysbase + M68K_EXECBASE_THISTASK);
    if (task && task < ctx->mem_size && offset + 4 <= ctx->mem_size - task)
        m68k_write32(ctx, task + offset, token);
}

IPTR kaffeine_dos_Input(struct KaffeineContext *ctx, void *cpu)
{
    ULONG token = Kaffeine_FileImport(ctx, Input(), FALSE);
    (void)cpu;
    if (!ctx->running && ctx->exit_code == 20) return 20;
    process_stream(ctx, M68K_PR_CIS, token);
    return token;
}
IPTR kaffeine_dos_Output(struct KaffeineContext *ctx, void *cpu)
{
    ULONG token = Kaffeine_FileImport(ctx, Output(), FALSE);
    (void)cpu;
    if (!ctx->running && ctx->exit_code == 20) return 20;
    process_stream(ctx, M68K_PR_COS, token);
    return token;
}
/* A closed selected stream must not be imported from a freed native
   FileHandle later. Explicit Close may close a borrowed stream too; cleanup
   only closes owned files. Native DOS calls here always use native BPTRs. */
static void detach_streams(struct KaffeineContext *ctx, BPTR native)
{
    struct Task *task;
    if (Input() == native)
    {
        SelectInput((BPTR)0);
        process_stream(ctx, M68K_PR_CIS, 0);
    }
    if (Output() == native)
    {
        SelectOutput((BPTR)0);
        process_stream(ctx, M68K_PR_COS, 0);
    }
    task = FindTask(NULL);
    if (task && task->tc_Node.ln_Type == NT_PROCESS && ((struct Process *)task)->pr_CES == native)
    {
        ((struct Process *)task)->pr_CES = (BPTR)0;
        process_stream(ctx, M68K_PR_CES, 0);
    }
}

static void retire_file(struct KaffeineContext *ctx, struct KaffeineFile *f)
{
    if (f->mirror) Kaffeine_HeapFree(ctx, f->mirror, KAFFEINE_FILE_BUFFER_LIMIT);
    memset(ctx->mem + f->guest, 0, KAFFEINE_FILE_SLOT_SIZE);
    memset(f, 0, sizeof(*f));
}

void Kaffeine_FileDropNative(struct KaffeineContext *ctx, BPTR native, BOOL borrowed_only)
{
    SIPTR error = IoErr();
    unsigned i;
    if (!native) return;
    for (i = 0; i < KAFFEINE_MAX_FILES; ++i)
        if (ctx->files[i].native == native && (!borrowed_only || !ctx->files[i].owned))
            retire_file(ctx, &ctx->files[i]);
    SetIoErr(error);
}

void Kaffeine_FileDropBorrowed(struct KaffeineContext *ctx, ULONG token)
{
    SIPTR error = IoErr();
    struct KaffeineFile *f = entry(ctx, token);
    if (f && !f->owned)
    {
        detach_streams(ctx, f->native);
        retire_file(ctx, f);
    }
    SetIoErr(error);
}

IPTR kaffeine_dos_Close(struct KaffeineContext *ctx, void *cpu)
{
    ULONG token = THUNK_D(1);
    BPTR native;
    struct KaffeineFile *f;
    LONG result;
    if (!Kaffeine_FileResolve(ctx, token, &native)) return 20;
    f = entry(ctx, token);
    if (!f) return Kaffeine_MessageFailure(ctx, "Close");
    if (ctx->process_file_close && !ctx->process_file_close(ctx, native))
        return Kaffeine_MessageFailure(ctx, "Close startup stream");
    detach_streams(ctx, native);
    result = Close(native);
    /* AROS frees the FileHandle even when ACTION_END fails. The identity
       remains quarantined, while the native table and mirror are reusable. */
    retire_file(ctx, f);
    return result;
}

void Kaffeine_FileCleanup(struct KaffeineContext *ctx)
{
    struct KaffeineFileSlab *slab;
    unsigned i;
    for (i = 0; i < KAFFEINE_MAX_FILES; ++i)
    {
        struct KaffeineFile *f = &ctx->files[i];
        if (f->native && f->owned)
        {
            detach_streams(ctx, f->native);
            Close(f->native);
        }
        if (f->mirror) Kaffeine_HeapFree(ctx, f->mirror, KAFFEINE_FILE_BUFFER_LIMIT);
    }
    memset(ctx->files, 0, sizeof(ctx->files));
    while ((slab = ctx->file_slabs))
    {
        ctx->file_slabs = slab->next;
        Kaffeine_HeapFree(ctx, slab->guest, KAFFEINE_FILE_SLAB_SIZE);
        FreeMem(slab, sizeof(*slab));
    }
    ctx->file_packet = NULL;
}

/* A guest range that native I/O fills or reads must stay clear of the
   packet, its reply queue and every owned file structure. */
BOOL Kaffeine_PacketRange(struct KaffeineContext *ctx, ULONG msg, const struct KaffeineQueue *reply,
                          ULONG addr, ULONG length)
{
    unsigned i;
    if (length > INT32_MAX || !Kaffeine_MessageMemory(ctx, addr, length) ||
        Kaffeine_Overlap(msg, 68, addr, length) || !Kaffeine_QueueDisjoint(reply, addr, length))
        return FALSE;
    for (i = 0; i < KAFFEINE_MAX_FILES; ++i)
    {
        const struct KaffeineFile *other = &ctx->files[i];
        if (other->guest &&
            (Kaffeine_Overlap(addr, length, other->guest, 86) ||
             (other->mirror && Kaffeine_Overlap(addr, length, other->mirror, KAFFEINE_FILE_BUFFER_LIMIT))))
            return FALSE;
    }
    return TRUE;
}

/* The DOS reply: PutMsg, not ReplyMsg, so type 5, packet identity kept and
   dp_Port the handler. */
void Kaffeine_PacketReply(struct KaffeineContext *ctx, ULONG port, ULONG msg,
                          const struct KaffeineQueue *reply, LONG res1, LONG res2)
{
    m68k_write32(ctx, msg + 10, msg + 20);
    m68k_write32(ctx, msg + 24, port);
    m68k_write32(ctx, msg + 32, (ULONG)res1);
    m68k_write32(ctx, msg + 36, (ULONG)res2);
    /* A cleared StandardPacket may arrive through PutMsg with length zero.
       Commit its checked extent only after every preflight/native operation,
       so the reply queue can inspect it without accepting generic zero nodes. */
    m68k_write16(ctx, msg + 18, 68);
    Kaffeine_MessageAppend(ctx, reply, msg, 5);
}

/* A contiguous 68-byte StandardPacket with a reply queue that takes it.
   Classic callers may leave mn_Length zero when sending a cleared packet
   directly to a typed handler. Keep it unchanged until PacketReply commits
   the checked 68-byte extent; arbitrary zero-length messages stay invalid. DOS replies use dp_Port; mn_ReplyPort is unused
   and may retain arbitrary data in a packet reused by the caller. */
BOOL Kaffeine_PacketMessage(struct KaffeineContext *ctx, ULONG msg, struct KaffeineQueue *reply)
{
    return !(msg & 1) && Kaffeine_GuestBytes(ctx, msg, 68) &&
        (!m68k_read16(ctx, msg + 18) || m68k_read16(ctx, msg + 18) == 68) &&
        m68k_read32(ctx, msg + 20) == msg &&
        (!m68k_read32(ctx, msg + 10) || m68k_read32(ctx, msg + 10) == msg + 20) &&
        Kaffeine_QueueRead(ctx, m68k_read32(ctx, msg + 24), reply) &&
        Kaffeine_MessagePreflight(ctx, reply, msg, 68);
}

/* A synthetic handler port as the bridge made it: an empty PA_IGNORE port
   that the program has not repurposed. */
BOOL Kaffeine_PacketPort(struct KaffeineContext *ctx, ULONG port)
{
    return !m68k_read32(ctx, port) && !m68k_read32(ctx, port + 4) &&
        ctx->mem[port + 8] == 4 && !ctx->mem[port + 9] && !m68k_read32(ctx, port + 10) &&
        ctx->mem[port + 14] == 2 && !ctx->mem[port + 15] && !m68k_read16(ctx, port + 32) &&
        !m68k_read32(ctx, port + 16) && m68k_read32(ctx, port + 20) == port + 24 &&
        !m68k_read32(ctx, port + 24) && m68k_read32(ctx, port + 28) == port + 20;
}

/* Packets come from the program's process, not from a child task. */
BOOL Kaffeine_PacketTask(struct KaffeineContext *ctx)
{
    return ctx->guest_exec_task && ctx->m68k_sysbase &&
        m68k_read32(ctx, ctx->m68k_sysbase + M68K_EXECBASE_THISTASK) == ctx->guest_exec_task;
}

/* An InfoData as Info() gives it: no native volume node or window pointer
   reaches the program. A console handler ('CON\0' or 'RAW\0') puts its
   native read IORequest in id_InUse, which classic programs follow to find
   the console unit; there is no guest IORequest for it yet, so the program
   gets 0. */
void Kaffeine_InfoToGuest(struct KaffeineContext *ctx, ULONG out, const struct InfoData *info)
{
    m68k_write32(ctx, out, (ULONG)info->id_NumSoftErrors);
    m68k_write32(ctx, out + 4, (ULONG)info->id_UnitNumber);
    m68k_write32(ctx, out + 8, (ULONG)info->id_DiskState);
    m68k_write32(ctx, out + 12, (ULONG)info->id_NumBlocks);
    m68k_write32(ctx, out + 16, (ULONG)info->id_NumBlocksUsed);
    m68k_write32(ctx, out + 20, (ULONG)info->id_BytesPerBlock);
    m68k_write32(ctx, out + 24, (ULONG)info->id_DiskType);
    m68k_write32(ctx, out + 28, 0);
    m68k_write32(ctx, out + 32, (info->id_DiskType == 0x434F4E00 ||
        info->id_DiskType == 0x52415700) ? 0 : (ULONG)info->id_InUse);
}

/* The native handler that a FileHandle port of the program stands for, as
   SetConsoleTask(fh->fh_Type) uses it: its native fh_Type, NULL for NIL:. */
BOOL Kaffeine_FilePort(struct KaffeineContext *ctx, ULONG port, struct MsgPort **native)
{
    unsigned i;
    for (i = 0; i < KAFFEINE_MAX_FILES; ++i)
        if (ctx->files[i].guest && ctx->files[i].handler == port && identity(ctx, &ctx->files[i]))
        {
            *native = ((struct FileHandle *)BADDR(ctx->files[i].native))->fh_Type;
            return TRUE;
        }
    return FALSE;
}

/* ACTION_READ and ACTION_WRITE on the native file through a native buffer;
   only the transferred bytes reach guest memory. */
static IPTR file_transfer(struct KaffeineContext *ctx, struct KaffeineFile *f, ULONG port,
                          ULONG msg, const struct KaffeineQueue *reply, BOOL write)
{
    const char *op = write ? "ACTION_WRITE" : "ACTION_READ";
    ULONG buffer = m68k_read32(ctx, msg + 44), length = m68k_read32(ctx, msg + 48);
    LONG result, error;
    SIPTR caller_error;
    UBYTE *temporary;
    /* Handlers ignore the arguments an action does not use: DoPkt1() to
       DoPkt4() leave the other argument registers as they were. */
    if (m68k_read32(ctx, msg + 40) != f->key)
        return Kaffeine_MessageFailure(ctx, write ? "ACTION_WRITE packet" : "ACTION_READ packet");
    if (!Kaffeine_PacketRange(ctx, msg, reply, buffer, length))
        return Kaffeine_MessageFailure(ctx, write ? "ACTION_WRITE buffer" : "ACTION_READ buffer");
    caller_error = IoErr();
    temporary = AllocVec(length ? length : 1, MEMF_PUBLIC);
    if (!temporary) return Kaffeine_MessageFailure(ctx, write ? "ACTION_WRITE allocation" : "ACTION_READ allocation");
    if (write)
    {
        memcpy(temporary, ctx->mem + buffer, length);
        result = Write(f->native, temporary, (LONG)length);
    }
    else
        result = Read(f->native, temporary, (LONG)length);
    error = result < 0 ? (LONG)IoErr() : 0;
    SetIoErr(caller_error);
    if (result < -1 || (result >= 0 && (ULONG)result > length))
    {
        FreeVec(temporary);
        return Kaffeine_MessageFailure(ctx, write ? "ACTION_WRITE native result" : "ACTION_READ native result");
    }
    if (!write && result > 0) memcpy(ctx->mem + buffer, temporary, (ULONG)result);
    FreeVec(temporary);
    Kaffeine_PacketReply(ctx, port, msg, reply, result, error);
    /* Keep useful application-boundary evidence without unbounded serial I/O.
       This is private diagnostic state, independent for every context. */
    if (ctx->packet_trace_count < 16)
    {
        ++ctx->packet_trace_count;
        bug("[kaffeine] %s message=%08lx handler=%08lx reply=%08lx requested=%lu result=%ld error=%ld\n",
            op, (unsigned long)msg, (unsigned long)port, (unsigned long)reply->port,
            (unsigned long)length, (long)result, (long)error);
    }
    return 0;
}

IPTR Kaffeine_FilePacket(struct KaffeineContext *ctx, ULONG port, ULONG msg)
{
    struct KaffeineFile *f = NULL;
    struct KaffeineQueue reply;
    struct MsgPort *handler;
    LONG action, res1 = 0, res2 = 0;
    SIPTR caller_error;
    ULONG i;
    for (i = 0; i < KAFFEINE_MAX_FILES; ++i)
        if (ctx->files[i].guest && ctx->files[i].handler == port) f = &ctx->files[i];
    if (!identity(ctx, f) || !Kaffeine_PacketMessage(ctx, msg, &reply))
        return Kaffeine_MessageFailure(ctx, "file handler packet");
    /* Handler identities are immutable, private protocols. Reject reuse as
       queues, forged fields, and aliases with any owned file storage. */
    for (i = 0; i < KAFFEINE_MAX_FILES; ++i)
    {
        struct KaffeineFile *other = &ctx->files[i];
        if (other->guest &&
            (Kaffeine_Overlap(msg, 68, other->guest, 86) ||
             !Kaffeine_QueueDisjoint(&reply, other->guest, 86) ||
             (other->mirror && (Kaffeine_Overlap(msg, 68, other->mirror, KAFFEINE_FILE_BUFFER_LIMIT) ||
              !Kaffeine_QueueDisjoint(&reply, other->mirror, KAFFEINE_FILE_BUFFER_LIMIT)))))
            return Kaffeine_MessageFailure(ctx, "file handler packet overlap");
    }
    if (!Kaffeine_PacketPort(ctx, port))
        return Kaffeine_MessageFailure(ctx, "file handler");
    if (!Kaffeine_PacketTask(ctx))
        return Kaffeine_MessageFailure(ctx, "file handler packet from a foreign task");

    action = (LONG)m68k_read32(ctx, msg + 28);
    if (action == ACTION_READ || action == ACTION_WRITE)
        return file_transfer(ctx, f, port, msg, &reply, action == ACTION_WRITE);

    handler = (struct MsgPort *)((struct FileHandle *)BADDR(f->native))->fh_Type;
    caller_error = IoErr();
    switch (action)
    {
        /* The packet goes to the handler as it is: Seek() would also flush
           the write buffer of the handle, or drop its read buffer and count
           the bytes still unread there. A NIL: handle has no handler, and
           native Seek() answers for it. */
        case ACTION_SEEK:
        {
            LONG mode = (LONG)m68k_read32(ctx, msg + 48);
            LONG offset = (LONG)m68k_read32(ctx, msg + 44);
            if (m68k_read32(ctx, msg + 40) != f->key || mode < OFFSET_BEGINNING || mode > OFFSET_END)
                return Kaffeine_MessageFailure(ctx, "ACTION_SEEK packet");
            if (handler)
            {
                res1 = (LONG)DoPkt(handler, ACTION_SEEK,
                    ((struct FileHandle *)BADDR(f->native))->fh_Args, offset, mode, 0, 0);
                res2 = (LONG)IoErr();
            }
            else
            {
                res1 = Seek(f->native, offset, mode);
                res2 = res1 == -1 ? (LONG)IoErr() : 0;
            }
            break;
        }
        /* Actions with integer arguments go to the native handler as they
           are; a NIL: handle has none. */
        case ACTION_IS_FILESYSTEM:
        case ACTION_SCREEN_MODE:
        case ACTION_WAIT_CHAR:
            if (!handler)
            {
                res2 = ERROR_ACTION_NOT_KNOWN;
                break;
            }
            res1 = (LONG)DoPkt(handler, action, action == ACTION_IS_FILESYSTEM ? 0 :
                (SIPTR)(LONG)m68k_read32(ctx, msg + 40), 0, 0, 0, 0);
            res2 = (LONG)IoErr();
            break;
        /* The InfoData of the handler (Kaffeine_InfoToGuest()) */
        case ACTION_DISK_INFO:
        {
            struct InfoData info;
            ULONG out = m68k_read32(ctx, msg + 40) << 2;
            if ((m68k_read32(ctx, msg + 40) >> 30) || !Kaffeine_PacketRange(ctx, msg, &reply, out, 36))
                return Kaffeine_MessageFailure(ctx, "ACTION_DISK_INFO buffer");
            if (!handler)
            {
                res2 = ERROR_ACTION_NOT_KNOWN;
                break;
            }
            memset(&info, 0, sizeof(info));
            res1 = (LONG)DoPkt(handler, ACTION_DISK_INFO, (SIPTR)MKBADDR(&info), 0, 0, 0, 0);
            res2 = (LONG)IoErr();
            if (res1)
                Kaffeine_InfoToGuest(ctx, out, &info);
            break;
        }
        /* Every other action is answered as a handler answers one it does
           not know. ACTION_END in particular would close the native file
           behind the program's FileHandle, which Close() owns. */
        default:
            res2 = ERROR_ACTION_NOT_KNOWN;
            break;
    }
    SetIoErr(caller_error);
    Kaffeine_PacketReply(ctx, port, msg, &reply, res1, res2);
    return 0;
}

IPTR kaffeine_dos_WaitForChar(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh;
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    if (!fh) return Kaffeine_MessageFailure(ctx, "WaitForChar NULL handle");
    return (IPTR)WaitForChar(fh, (LONG)THUNK_D(2));
}

IPTR kaffeine_dos_IsInteractive(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh;
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    if (!fh) return Kaffeine_MessageFailure(ctx, "IsInteractive NULL handle");
    return (IPTR)IsInteractive(fh);
}

IPTR kaffeine_dos_DupLockFromFH(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh;
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    if (!fh) return Kaffeine_MessageFailure(ctx, "DupLockFromFH NULL handle");
    return Kaffeine_LockAdopt(ctx, DupLockFromFH(fh));
}

IPTR kaffeine_dos_ParentOfFH(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh;
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    if (!fh) return Kaffeine_MessageFailure(ctx, "ParentOfFH NULL handle");
    return Kaffeine_LockAdopt(ctx, ParentOfFH(fh));
}

IPTR kaffeine_dos_SetMode(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh;
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    if (!fh) return Kaffeine_MessageFailure(ctx, "SetMode NULL handle");
    return (IPTR)SetMode(fh, (LONG)THUNK_D(2));
}

IPTR kaffeine_dos_SetFileSize(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh;
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    if (!fh) return Kaffeine_MessageFailure(ctx, "SetFileSize NULL handle");
    return (IPTR)SetFileSize(fh, (LONG)THUNK_D(2), (LONG)THUNK_D(3));
}

IPTR kaffeine_dos_LockRecord(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh;
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    if (!fh) return Kaffeine_MessageFailure(ctx, "LockRecord NULL handle");
    return (IPTR)LockRecord(fh, THUNK_D(2), THUNK_D(3), THUNK_D(4), THUNK_D(5));
}

IPTR kaffeine_dos_UnLockRecord(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh;
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    if (!fh) return Kaffeine_MessageFailure(ctx, "UnLockRecord NULL handle");
    return (IPTR)UnLockRecord(fh, THUNK_D(2), THUNK_D(3));
}

IPTR kaffeine_dos_FGetC(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh;
    IPTR result;
    ULONG token = THUNK_D(1);
    if (!Kaffeine_FileResolve(ctx, token, &fh)) return 20;
    if (!Kaffeine_FileBufferPrepare(ctx, token)) return ctx->running ? -1 : 20;
    result = (IPTR)FGetC(fh);
    return Kaffeine_FileBufferFinish(ctx, token) ? result : 20;
}

IPTR kaffeine_dos_FPutC(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh;
    IPTR result;
    ULONG token = THUNK_D(1);
    if (!Kaffeine_FileResolve(ctx, token, &fh)) return 20;
    if (!Kaffeine_FileBufferPrepare(ctx, token)) return ctx->running ? -1 : 20;
    result = (IPTR)FPutC(fh, (LONG)THUNK_D(2));
    return Kaffeine_FileBufferFinish(ctx, token) ? result : 20;
}

IPTR kaffeine_dos_UnGetC(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh;
    IPTR result;
    ULONG token = THUNK_D(1);
    if (!Kaffeine_FileResolve(ctx, token, &fh)) return 20;
    if (!Kaffeine_FileBufferPrepare(ctx, token)) return ctx->running ? -1 : 20;
    result = (IPTR)UnGetC(fh, (LONG)THUNK_D(2));
    return Kaffeine_FileBufferFinish(ctx, token) ? result : 20;
}

IPTR kaffeine_dos_Flush(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh;
    IPTR result;
    ULONG token = THUNK_D(1);
    if (!Kaffeine_FileResolve(ctx, token, &fh)) return 20;
    if (!Kaffeine_FileBufferPrepare(ctx, token)) return ctx->running ? -1 : 20;
    result = (IPTR)Flush(fh);
    return Kaffeine_FileBufferFinish(ctx, token) ? result : 20;
}

IPTR kaffeine_dos_Seek(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh;
    IPTR result;
    struct KaffeineFile *f = entry(ctx, THUNK_D(1));
    BOOL buffered;
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    buffered = f && !memcmp(f->snapshot, ctx->mem + f->guest, KAFFEINE_FILE_SIZE);
    if (!fh) return Kaffeine_MessageFailure(ctx, "Seek NULL handle");
    if (!buffered && ((struct FileHandle *)BADDR(fh))->fh_Buf)
        return Kaffeine_MessageFailure(ctx, "mixed guest/native Seek buffering");
    if (buffered && !Kaffeine_FileBufferPrepare(ctx, THUNK_D(1))) return ctx->running ? -1 : 20;
    result = (IPTR)Seek(fh, (LONG)THUNK_D(2), (LONG)THUNK_D(3));
    if (buffered && !Kaffeine_FileBufferFinish(ctx, THUNK_D(1))) return 20;
    return result;
}
IPTR kaffeine_dos_OpenFromLock(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineLockTransfer lease;
    BPTR native;
    ULONG token;
    if (!Kaffeine_LockTransferBegin(ctx, THUNK_D(1), &lease)) return 20;
    native = OpenFromLock(lease.native);
    Kaffeine_LockTransferEnd(ctx, &lease, native != 0);
    token = Kaffeine_FileImport(ctx, native, TRUE);
    return !ctx->running && ctx->exit_code == 20 ? 20 : token;
}

IPTR kaffeine_dos_SelectInput(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh, old = Input();
    ULONG token = THUNK_D(1), previous;
    if (!Kaffeine_FileResolve(ctx, token, &fh)) return 20;
    previous = Kaffeine_FileImport(ctx, old, FALSE);
    if (old && !previous) return !ctx->running && ctx->exit_code == 20 ? 20 : 0;
    SelectInput(fh);
    process_stream(ctx, M68K_PR_CIS, token);
    return previous;
}

IPTR kaffeine_dos_SelectOutput(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh, old = Output();
    ULONG token = THUNK_D(1), previous;
    if (!Kaffeine_FileResolve(ctx, token, &fh)) return 20;
    previous = Kaffeine_FileImport(ctx, old, FALSE);
    if (old && !previous) return !ctx->running && ctx->exit_code == 20 ? 20 : 0;
    SelectOutput(fh);
    process_stream(ctx, M68K_PR_COS, token);
    return previous;
}

IPTR kaffeine_dos_LockRecords(struct KaffeineContext *ctx, void *cpu)
{
    (void)THUNK_D(1);
    (void)THUNK_D(2);
    return Kaffeine_MessageFailure(ctx, "LockRecords");
}

IPTR kaffeine_dos_UnLockRecords(struct KaffeineContext *ctx, void *cpu)
{
    (void)THUNK_D(1);
    return Kaffeine_MessageFailure(ctx, "UnLockRecords");
}

IPTR kaffeine_dos_ChangeMode(struct KaffeineContext *ctx, void *cpu)
{
    BPTR object;
    ULONG type = THUNK_D(1);
    if (type == 1)
    {
        if (!Kaffeine_FileResolve(ctx, THUNK_D(2), &object)) return 20;
        if (!object) return Kaffeine_MessageFailure(ctx, "ChangeMode NULL handle");
    }
    else if (type == 0)
    {
        if (!Kaffeine_LockResolve(ctx, THUNK_D(2), &object)) return 20;
        if (!object) return Kaffeine_MessageFailure(ctx, "ChangeMode NULL lock");
    }
    else return Kaffeine_MessageFailure(ctx, "ChangeMode type");
    LONG result = ChangeMode(type, object, (LONG)THUNK_D(3));
    if (result && type == 0 && !Kaffeine_LockRefreshAccess(ctx, THUNK_D(2))) return 20;
    return result;
}

BOOL Kaffeine_FileStreamPrepare(struct KaffeineContext *ctx, BOOL input, ULONG *token)
{
    BPTR native = input ? Input() : Output();
    *token = Kaffeine_FileImport(ctx, native, FALSE);
    if (!native) return TRUE;
    return *token && Kaffeine_FileBufferPrepare(ctx, *token);
}
BOOL Kaffeine_FileAllPrepare(struct KaffeineContext *ctx)
{
    unsigned i;
    for (i = 0; i < KAFFEINE_MAX_FILES; ++i)
        if (ctx->files[i].native && !Kaffeine_FileBufferPrepare(ctx, ctx->files[i].guest >> 2)) return FALSE;
    return TRUE;
}
BOOL Kaffeine_FileAllFinish(struct KaffeineContext *ctx)
{
    unsigned i;
    for (i = 0; i < KAFFEINE_MAX_FILES; ++i)
        if (ctx->files[i].native && !Kaffeine_FileBufferFinish(ctx, ctx->files[i].guest >> 2)) return FALSE;
    return TRUE;
}
