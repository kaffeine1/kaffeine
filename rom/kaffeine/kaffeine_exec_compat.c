/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Keep containment objects and public guest lists away from native Exec.
*/

#include "kaffeine_intern.h"
#include "kaffeine_exec_compat.h"
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"
#include "kaffeine_messages.h"

#include <aros/debug.h>
#include <proto/exec.h>
#include <string.h>

/* -702: DeletePool(A0=poolHeader) */
IPTR Kaffeine_DeletePool(struct KaffeineContext *ctx, void *cpu)
{
    Kaffeine_PoolDelete(ctx, THUNK_A(0));
    return 0;
}

/* Classic SignalSemaphore fields, never a native Exec structure. */
enum
{
    SS_TYPE = 8, SS_PRI = 9, SS_NAME = 10, SS_NEST = 14,
    SS_WAIT = 16, SS_OWNER = 40, SS_QUEUE = 44, SS_SIZE = 46,
    LIST_SIZE = 14, NT_SIGNALSEM_68K = 15,
    PORT_SIZE = 34, PORT_MESSAGES = 20, NT_MSGPORT_68K = 4,
    /* A bounded snapshot makes corrupt lists fail before any member changes,
       without allocating or following guest links after starting writes. */
    MAX_EXEC_LIST = 256
};

static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    if (!ctx->mem || !addr || addr >= ctx->mem_size || ctx->mem_size - addr < size)
        return FALSE;
    return TRUE;
}

static BOOL guest_object(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    UWORD i;

    if (!guest_range(ctx, addr, size))
        return FALSE;
    for (i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].m68k_addr >= addr &&
            ctx->shadow_map[i].m68k_addr - addr < size)
            return FALSE;
    return TRUE;
}

static BOOL guest_overlap(ULONG a, ULONG asize, ULONG b, ULONG bsize)
{
    return a <= b ? b - a < asize : a - b < bsize;
}

static const char *guest_string(struct KaffeineContext *ctx, ULONG addr)
{
    if (!guest_range(ctx, addr, 1) ||
        !memchr(ctx->mem + addr, 0, ctx->mem_size - addr))
        return NULL;
    return (const char *)ctx->mem + addr;
}

static ULONG semaphore_task(struct KaffeineContext *ctx)
{
    ULONG task;

    if (!guest_range(ctx, ctx->m68k_sysbase, M68K_EXECBASE_THISTASK + 4))
        return 0;
    task = m68k_read32(ctx, ctx->m68k_sysbase + M68K_EXECBASE_THISTASK);
    return guest_range(ctx, task, 92) ? task : 0;
}

static IPTR semaphore_failure(struct KaffeineContext *ctx, const char *operation)
{
    /* One guest task cannot wait for a different owner or service a message
       protocol. Stop through the C run-loop flag, never through an exception.
       D0 must also be 20 because Kaffeine_RunCPU returns that register. */
    bug("[kaffeine] %s: unsupported or invalid guest semaphore operation, stopping (20)\n",
        operation);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

static void semaphore_init(struct KaffeineContext *ctx, ULONG sem)
{
    ctx->mem[sem + SS_TYPE] = NT_SIGNALSEM_68K;
    m68k_write16(ctx, sem + SS_NEST, 0);
    m68k_write32(ctx, sem + SS_WAIT, sem + SS_WAIT + 4);
    m68k_write32(ctx, sem + SS_WAIT + 4, 0);
    m68k_write32(ctx, sem + SS_WAIT + 8, sem + SS_WAIT);
    m68k_write32(ctx, sem + SS_OWNER, 0);
    m68k_write16(ctx, sem + SS_QUEUE, 0xFFFF);
}

static BOOL semaphore_valid(struct KaffeineContext *ctx, ULONG sem)
{
    UWORD nest;

    if (!guest_object(ctx, sem, SS_SIZE))
        return FALSE;
    nest = m68k_read16(ctx, sem + SS_NEST);
    /* There can be no queued task or message in this context. In AROS the
       queue count equals acquisitions minus one when the wait queue is empty. */
    return ctx->mem[sem + SS_TYPE] == NT_SIGNALSEM_68K && nest <= 32767 &&
        m68k_read16(ctx, sem + SS_QUEUE) == (UWORD)(nest - 1) &&
        (nest || !m68k_read32(ctx, sem + SS_OWNER)) &&
        m68k_read32(ctx, sem + SS_WAIT) == sem + SS_WAIT + 4 &&
        !m68k_read32(ctx, sem + SS_WAIT + 4) &&
        m68k_read32(ctx, sem + SS_WAIT + 8) == sem + SS_WAIT;
}

static BOOL semaphore_can_obtain(struct KaffeineContext *ctx, ULONG sem,
                                 ULONG task, BOOL shared)
{
    ULONG owner;

    if (!task || !semaphore_valid(ctx, sem) || m68k_read16(ctx, sem + SS_NEST) == 32767)
        return FALSE;
    owner = m68k_read32(ctx, sem + SS_OWNER);
    return !m68k_read16(ctx, sem + SS_NEST) || owner == task || (shared && !owner);
}

static void semaphore_obtain(struct KaffeineContext *ctx, ULONG sem,
                             ULONG task, BOOL shared)
{
    UWORD nest = m68k_read16(ctx, sem + SS_NEST);

    if (!nest)
        m68k_write32(ctx, sem + SS_OWNER, shared ? 0 : task);
    m68k_write16(ctx, sem + SS_NEST, nest + 1);
    m68k_write16(ctx, sem + SS_QUEUE, nest);
}

static BOOL semaphore_can_release(struct KaffeineContext *ctx, ULONG sem, ULONG task)
{
    ULONG owner;

    if (!task || !semaphore_valid(ctx, sem) || !m68k_read16(ctx, sem + SS_NEST))
        return FALSE;
    owner = m68k_read32(ctx, sem + SS_OWNER);
    return !owner || owner == task;
}

static void semaphore_release(struct KaffeineContext *ctx, ULONG sem)
{
    UWORD nest = m68k_read16(ctx, sem + SS_NEST) - 1;

    m68k_write16(ctx, sem + SS_NEST, nest);
    m68k_write16(ctx, sem + SS_QUEUE, (UWORD)(nest - 1));
    if (!nest)
        m68k_write32(ctx, sem + SS_OWNER, 0);
}

/* -558: InitSemaphore(A0=semaphore), preserving D0 on success. */
IPTR Kaffeine_InitSemaphore(struct KaffeineContext *ctx, void *cpu)
{
    ULONG sem = THUNK_A(0);

    if (!guest_object(ctx, sem, SS_SIZE))
        return semaphore_failure(ctx, "InitSemaphore");
    semaphore_init(ctx, sem);
    return THUNK_D(0);
}

/* -564: ObtainSemaphore(A0=semaphore). */
IPTR Kaffeine_ObtainSemaphore(struct KaffeineContext *ctx, void *cpu)
{
    ULONG sem = THUNK_A(0), task = semaphore_task(ctx);

    if (!semaphore_can_obtain(ctx, sem, task, FALSE))
        return semaphore_failure(ctx, "ObtainSemaphore");
    semaphore_obtain(ctx, sem, task, FALSE);
    return THUNK_D(0);
}

/* -570: ReleaseSemaphore(A0=semaphore). */
IPTR Kaffeine_ReleaseSemaphore(struct KaffeineContext *ctx, void *cpu)
{
    ULONG sem = THUNK_A(0);

    if (!semaphore_can_release(ctx, sem, semaphore_task(ctx)))
        return semaphore_failure(ctx, "ReleaseSemaphore");
    semaphore_release(ctx, sem);
    return THUNK_D(0);
}

/* -576: AttemptSemaphore(A0=semaphore) -> D0=1 or 0, as in AROS. */
IPTR Kaffeine_AttemptSemaphore(struct KaffeineContext *ctx, void *cpu)
{
    ULONG sem = THUNK_A(0), task = semaphore_task(ctx);

    if (!semaphore_can_obtain(ctx, sem, task, FALSE))
        return FALSE;
    semaphore_obtain(ctx, sem, task, FALSE);
    return TRUE;
}

/* -678: ObtainSemaphoreShared(A0=semaphore). */
IPTR Kaffeine_ObtainSemaphoreShared(struct KaffeineContext *ctx, void *cpu)
{
    ULONG sem = THUNK_A(0), task = semaphore_task(ctx);

    if (!semaphore_can_obtain(ctx, sem, task, TRUE))
        return semaphore_failure(ctx, "ObtainSemaphoreShared");
    semaphore_obtain(ctx, sem, task, TRUE);
    return THUNK_D(0);
}

/* -720: AttemptSemaphoreShared(A0=semaphore) -> D0=1 or 0. */
IPTR Kaffeine_AttemptSemaphoreShared(struct KaffeineContext *ctx, void *cpu)
{
    ULONG sem = THUNK_A(0), task = semaphore_task(ctx);

    if (!semaphore_can_obtain(ctx, sem, task, TRUE))
        return FALSE;
    semaphore_obtain(ctx, sem, task, TRUE);
    return TRUE;
}

static BOOL exec_list(struct KaffeineContext *ctx, ULONG list, ULONG node_size,
                      ULONG *nodes, unsigned *count)
{
    ULONG node, pred = list;

    *count = 0;
    if (!guest_object(ctx, list, LIST_SIZE) || m68k_read32(ctx, list + 4))
        return FALSE;
    node = m68k_read32(ctx, list);
    while (node != list + 4)
    {
        unsigned i;

        if (*count == MAX_EXEC_LIST || !guest_object(ctx, node, node_size) ||
            guest_overlap(node, node_size, list, LIST_SIZE) ||
            m68k_read32(ctx, node + 4) != pred)
            return FALSE;
        for (i = 0; i < *count; i++)
            if (guest_overlap(node, node_size, nodes[i], node_size))
                return FALSE;
        nodes[(*count)++] = node;
        pred = node;
        node = m68k_read32(ctx, node);
    }
    return m68k_read32(ctx, list + 8) == pred;
}

static BOOL semaphore_list(struct KaffeineContext *ctx, ULONG list,
                           ULONG *nodes, unsigned *count)
{
    return exec_list(ctx, list, SS_SIZE, nodes, count);
}

/* -582: ObtainSemaphoreList(A0=list). Validate every member before writing. */
IPTR Kaffeine_ObtainSemaphoreList(struct KaffeineContext *ctx, void *cpu)
{
    ULONG nodes[MAX_EXEC_LIST], task = semaphore_task(ctx);
    unsigned count, i;

    if (!semaphore_list(ctx, THUNK_A(0), nodes, &count))
        return semaphore_failure(ctx, "ObtainSemaphoreList");
    for (i = 0; i < count; i++)
        if (!semaphore_can_obtain(ctx, nodes[i], task, FALSE))
            return semaphore_failure(ctx, "ObtainSemaphoreList");
    for (i = 0; i < count; i++)
        semaphore_obtain(ctx, nodes[i], task, FALSE);
    return THUNK_D(0);
}

/* -588: ReleaseSemaphoreList(A0=list). */
IPTR Kaffeine_ReleaseSemaphoreList(struct KaffeineContext *ctx, void *cpu)
{
    ULONG nodes[MAX_EXEC_LIST], task = semaphore_task(ctx);
    unsigned count, i;

    if (!semaphore_list(ctx, THUNK_A(0), nodes, &count))
        return semaphore_failure(ctx, "ReleaseSemaphoreList");
    for (i = 0; i < count; i++)
        if (!semaphore_can_release(ctx, nodes[i], task))
            return semaphore_failure(ctx, "ReleaseSemaphoreList");
    for (i = 0; i < count; i++)
        semaphore_release(ctx, nodes[i]);
    return THUNK_D(0);
}

static ULONG semaphore_public_list(struct KaffeineContext *ctx)
{
    if (!guest_range(ctx, ctx->m68k_sysbase, M68K_EXECBASE_SEMAPHORELIST + LIST_SIZE))
        return 0;
    return ctx->m68k_sysbase + M68K_EXECBASE_SEMAPHORELIST;
}

/* -594: FindSemaphore(A1=name) -> D0=guest semaphore or NULL. */
IPTR Kaffeine_FindSemaphore(struct KaffeineContext *ctx, void *cpu)
{
    ULONG nodes[MAX_EXEC_LIST], found = 0;
    const char *name = guest_string(ctx, THUNK_A(1));
    unsigned count, i;

    if (!name || !semaphore_list(ctx, semaphore_public_list(ctx), nodes, &count))
        return 0;
    for (i = 0; i < count; i++)
    {
        const char *candidate = guest_string(ctx, m68k_read32(ctx, nodes[i] + SS_NAME));

        if (!candidate)
            return 0;
        if (!found && !strcmp(candidate, name))
            found = nodes[i];
    }
    return found;
}

/* -600: AddSemaphore(A1=semaphore). Public only within this guest context. */
IPTR Kaffeine_AddSemaphore(struct KaffeineContext *ctx, void *cpu)
{
    ULONG sem = THUNK_A(1), list = semaphore_public_list(ctx), nodes[MAX_EXEC_LIST];
    ULONG pred, next;
    unsigned count, i, pos;
    int priority;

    if (!guest_object(ctx, sem, SS_SIZE) ||
        !guest_string(ctx, m68k_read32(ctx, sem + SS_NAME)) ||
        !semaphore_list(ctx, list, nodes, &count) || count == MAX_EXEC_LIST ||
        guest_overlap(sem, SS_SIZE, list, LIST_SIZE))
        return semaphore_failure(ctx, "AddSemaphore");
    priority = (int)(signed char)ctx->mem[sem + SS_PRI];
    pos = count;
    for (i = 0; i < count; i++)
    {
        if (guest_overlap(sem, SS_SIZE, nodes[i], SS_SIZE))
            return semaphore_failure(ctx, "AddSemaphore");
        if (pos == count && priority > (int)(signed char)ctx->mem[nodes[i] + SS_PRI])
            pos = i;
    }
    pred = pos ? nodes[pos - 1] : list;
    next = pos < count ? nodes[pos] : list + 4;
    semaphore_init(ctx, sem);
    m68k_write32(ctx, sem, next);
    m68k_write32(ctx, sem + 4, pred);
    m68k_write32(ctx, pred, sem);
    m68k_write32(ctx, next + 4, sem);
    return THUNK_D(0);
}

/* -606: RemSemaphore(A1=semaphore). Preserve the removed node's own links. */
IPTR Kaffeine_RemSemaphore(struct KaffeineContext *ctx, void *cpu)
{
    ULONG sem = THUNK_A(1), list = semaphore_public_list(ctx), nodes[MAX_EXEC_LIST];
    unsigned count, i;

    if (!semaphore_list(ctx, list, nodes, &count))
        return semaphore_failure(ctx, "RemSemaphore");
    for (i = 0; i < count; i++)
        if (nodes[i] == sem)
        {
            m68k_write32(ctx, i ? nodes[i - 1] : list,
                         i + 1 < count ? nodes[i + 1] : list + 4);
            m68k_write32(ctx, (i + 1 < count ? nodes[i + 1] : list + 4) + 4,
                         i ? nodes[i - 1] : list);
            return THUNK_D(0);
        }
    return semaphore_failure(ctx, "RemSemaphore");
}

static IPTR port_failure(struct KaffeineContext *ctx, const char *operation)
{
    /* A void mutation cannot claim success on an invalid list. Use the C
       run-loop stop convention, as for invalid public semaphore operations. */
    bug("[kaffeine] %s: invalid guest public port operation, stopping (20)\n", operation);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

static ULONG port_public_list(struct KaffeineContext *ctx)
{
    if (!guest_range(ctx, ctx->m68k_sysbase, M68K_EXECBASE_PORTLIST + LIST_SIZE))
        return 0;
    return ctx->m68k_sysbase + M68K_EXECBASE_PORTLIST;
}

/* -354: AddPort(A1=port). Never publish a guest structure to native Exec. */
IPTR Kaffeine_AddPort(struct KaffeineContext *ctx, void *cpu)
{
    ULONG port = THUNK_A(1), list = port_public_list(ctx), nodes[MAX_EXEC_LIST];
    ULONG pred, next, messages;
    unsigned count, i, pos;
    int priority;

    if (!guest_object(ctx, port, PORT_SIZE) ||
        !guest_string(ctx, m68k_read32(ctx, port + SS_NAME)) ||
        !exec_list(ctx, list, PORT_SIZE, nodes, &count) || count == MAX_EXEC_LIST ||
        guest_overlap(port, PORT_SIZE, list, LIST_SIZE))
        return port_failure(ctx, "AddPort");
    priority = (int)(signed char)ctx->mem[port + SS_PRI];
    pos = count;
    for (i = 0; i < count; i++)
    {
        if (guest_overlap(port, PORT_SIZE, nodes[i], PORT_SIZE) ||
            !guest_string(ctx, m68k_read32(ctx, nodes[i] + SS_NAME)))
            return port_failure(ctx, "AddPort");
        if (pos == count && priority > (int)(signed char)ctx->mem[nodes[i] + SS_PRI])
            pos = i;
    }
    pred = pos ? nodes[pos - 1] : list;
    next = pos < count ? nodes[pos] : list + 4;
    messages = port + PORT_MESSAGES;
    ctx->mem[port + SS_TYPE] = NT_MSGPORT_68K;
    m68k_write32(ctx, messages, messages + 4);
    m68k_write32(ctx, messages + 4, 0);
    m68k_write32(ctx, messages + 8, messages);
    m68k_write32(ctx, port, next);
    m68k_write32(ctx, port + 4, pred);
    m68k_write32(ctx, pred, port);
    m68k_write32(ctx, next + 4, port);
    return THUNK_D(0);
}

/* -360: RemPort(A1=port). Check membership before using any node links. */
IPTR Kaffeine_RemPort(struct KaffeineContext *ctx, void *cpu)
{
    ULONG port = THUNK_A(1), list = port_public_list(ctx), nodes[MAX_EXEC_LIST];
    unsigned count, i;

    if (!exec_list(ctx, list, PORT_SIZE, nodes, &count))
        return port_failure(ctx, "RemPort");
    for (i = 0; i < count; i++)
        if (nodes[i] == port)
        {
            ULONG pred = i ? nodes[i - 1] : list;
            ULONG next = i + 1 < count ? nodes[i + 1] : list + 4;

            m68k_write32(ctx, pred, next);
            m68k_write32(ctx, next + 4, pred);
            return THUNK_D(0);
        }
    return port_failure(ctx, "RemPort");
}

/* -390: FindPort(A1=name) -> D0=guest port or NULL, scoped to this context. */
IPTR Kaffeine_FindPort(struct KaffeineContext *ctx, void *cpu)
{
    ULONG nodes[MAX_EXEC_LIST], found = 0;
    const char *name = guest_string(ctx, THUNK_A(1));
    unsigned count, i;

    if (!name || !exec_list(ctx, port_public_list(ctx), PORT_SIZE, nodes, &count))
        return 0;
    for (i = 0; i < count; i++)
    {
        const char *candidate = guest_string(ctx, m68k_read32(ctx, nodes[i] + SS_NAME));

        if (!candidate)
            return 0;
        if (!found && !strcmp(candidate, name))
            found = nodes[i];
    }
    return found;
}

/* The shadow entry at an exact shadow address, or -1 for guest bytes. */
static int shadow_entry(struct KaffeineContext *ctx, ULONG addr)
{
    UWORD i;

    for (i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].m68k_addr == addr)
            return ctx->shadow_map[i].native_ptr ? i : -1;
    return -1;
}

/* -366: PutMsg(A0=port, A1=message). Only a known native UserPort and a
   taken IntuiMessage can use native Exec. An exact shadow of a Window,
   Task or another object is not a Message or MsgPort. Guest queues and
   owned DOS handlers use separate adapters. */
IPTR Kaffeine_PutMsg(struct KaffeineContext *ctx, void *cpu)
{
    ULONG gp = THUNK_A(0), gm = THUNK_A(1), i;
    int port = shadow_entry(ctx, gp), message = shadow_entry(ctx, gm);
    UWORD type = message >= 0 ? ctx->shadow_map[message].type : 0;

    if (port >= 0 && message >= 0 &&
        ctx->shadow_map[port].type == KAFFEINE_SHADOW_USERPORT &&
        type == KAFFEINE_SHADOW_INTUIMESSAGE)
    {
        PutMsg((struct MsgPort *)ctx->shadow_map[port].native_ptr,
               (struct Message *)ctx->shadow_map[message].native_ptr);
        if (type == KAFFEINE_SHADOW_INTUIMESSAGE)
            ctx->shadow_map[message].type = KAFFEINE_SHADOW_WAITMESSAGE;
        return THUNK_D(0);
    }
    if (port >= 0 || message >= 0)
        return Kaffeine_MessageFailure(ctx, "mixed PutMsg");
    if (gp && ctx->file_packet)
        for (i = 0; i < KAFFEINE_MAX_FILES; ++i)
            if (ctx->files[i].guest && ctx->files[i].handler == gp)
            {
                IPTR result = ctx->file_packet(ctx, gp, gm);
                return ctx->running ? THUNK_D(0) : result;
            }
    if (!gp && guest_range(ctx, gm, 68) && m68k_read16(ctx, gm + 18) == 68 &&
        m68k_read32(ctx, gm + 20) == gm)
        bug("[kaffeine] PutMsg NULL StandardPacket message=%08lx reply=%08lx action=%ld key=%08lx buffer=%08lx length=%ld\n",
            (unsigned long)gm, (unsigned long)m68k_read32(ctx, gm + 24),
            (long)(LONG)m68k_read32(ctx, gm + 28), (unsigned long)m68k_read32(ctx, gm + 40),
            (unsigned long)m68k_read32(ctx, gm + 44), (long)(LONG)m68k_read32(ctx, gm + 48));
    return Kaffeine_GuestPutMsg(ctx, gp, gm, THUNK_D(0));
}

/* -96: FindResident(A1=name). Native Residents include native code addresses. */
IPTR Kaffeine_FindResident(struct KaffeineContext *ctx, void *cpu)
{
    if (!guest_string(ctx, THUNK_A(1)))
        bug("[kaffeine] FindResident: invalid guest name\n");
    return 0;
}

/* -498: OpenResource(A1=resName). No native resource ABI is exposed. */
IPTR Kaffeine_OpenResource(struct KaffeineContext *ctx, void *cpu)
{
    if (!guest_string(ctx, THUNK_A(1)))
        bug("[kaffeine] OpenResource: invalid guest name\n");
    return 0;
}

/* -486/-492: the guest resource registry is deliberately unsupported. */
IPTR Kaffeine_AddResource(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    return THUNK_D(0);
}

IPTR Kaffeine_RemResource(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    return THUNK_D(0);
}

/* -636: CacheClearU(). The backend forgets what it derived from guest code
   anywhere (Kaffeine_FlushCode()); no native cache operation, D0 kept. */
IPTR Kaffeine_CacheClearU(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    Kaffeine_FlushCode(cpu, 0, KAFFEINE_FLUSH_ALL);
    return THUNK_D(0);
}

/* -642: CacheClearE(A0=address, D0=length, D1=caches). With CACRF_ClearI
   the backend forgets what it derived from guest code in the range, or
   anywhere for the length 0xFFFFFFFF that the autodoc gives for all
   addresses and for a range outside guest memory, which is logged. No
   native cache operation; D0 is the length. */
IPTR Kaffeine_CacheClearE(struct KaffeineContext *ctx, void *cpu)
{
    ULONG addr = THUNK_A(0), size = THUNK_D(0), caches = THUNK_D(1);
    BOOL range = size != KAFFEINE_FLUSH_ALL && guest_range(ctx, addr, size);

    if (!range && size != KAFFEINE_FLUSH_ALL)
        bug("[kaffeine] CacheClearE: invalid guest range %08lx/%lu, caches %08lx\n",
            (unsigned long)addr, (unsigned long)size, (unsigned long)caches);
    if (caches & M68K_CACRF_CLEARI)
        Kaffeine_FlushCode(cpu, range ? addr : 0, range ? size : KAFFEINE_FLUSH_ALL);
    return size;
}

/* -540: Procure(A0=semaphore, A1=bidMsg). No guest message/reply protocol yet. */
IPTR Kaffeine_Procure(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return semaphore_failure(ctx, "Procure");
}

/* -546: Vacate(A0=semaphore, A1=bidMsg). Paired with refused Procure. */
IPTR Kaffeine_Vacate(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return semaphore_failure(ctx, "Vacate");
}

/* -852: AVL_AddNode(A0=root, A1=node, A2=func) -> D0=NULL (unsupported) */
IPTR thunk_exec_AVL_AddNode(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    (void)cpu;
    return 0;
}

/* -858: AVL_RemNodeByAddress(A0=root, A1=node) -> D0=NULL (unsupported) */
IPTR thunk_exec_AVL_RemNodeByAddress(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    (void)cpu;
    return 0;
}

/* -870: AVL_FindNode(A0=root, A1=key, A2=func) -> D0=NULL (unsupported) */
IPTR thunk_exec_AVL_FindNode(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    (void)cpu;
    return 0;
}
