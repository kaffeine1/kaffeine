/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Bounded classic Exec queues. Native objects never enter these lists.
*/
#include "kaffeine_messages.h"
#include "kaffeine_offsets.h"
#include <aros/debug.h>
#include <proto/exec.h>
#include <string.h>

BOOL Kaffeine_Overlap(ULONG a, ULONG an, ULONG b, ULONG bn)
{
    return an && bn && (a <= b ? b - a < an : a - b < bn);
}

static BOOL arena(struct KaffeineContext *ctx, ULONG a, ULONG n)
{
    return ctx->mem && a && a < ctx->mem_size && n <= ctx->mem_size - a;
}

BOOL Kaffeine_GuestBytes(struct KaffeineContext *ctx, ULONG a, ULONG n)
{
    unsigned i;
    if (!arena(ctx, a, n)) return FALSE;
    for (i = 0; i < ctx->num_shadows; ++i)
    {
        ULONG size = 20;
        /* Explicit bridge types and the generated ExecBase/Task layouts.
           None of their interior bytes may become a guest queue or buffer. */
        switch (ctx->shadow_map[i].type)
        {
            case 1: case 278: size = 136; break;
            case 3: case 224: size = 100; break;
            case 4: case 193: size = 34; break;
            case 5: case 13: case 235: size = 346; break;
            case 8: case 9: case 10: case 166: size = 52; break;
            case 110: size = 632; break;
            case 258: size = 228; break;
        }
        if (Kaffeine_Overlap(a, n, ctx->shadow_map[i].m68k_addr, size)) return FALSE;
    }
    return TRUE;
}

IPTR Kaffeine_MessageFailure(struct KaffeineContext *ctx, const char *op)
{
    bug("[kaffeine] %s: invalid or unsupported guest message boundary, stopping (20)\n", op);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

static BOOL ordinary(struct KaffeineContext *ctx, ULONG port, ULONG size)
{
    unsigned i;
    const struct KaffeineFileSlab *slab;
    for (slab = ctx->file_slabs; slab; slab = slab->next)
        if (Kaffeine_Overlap(port, size, slab->guest, KAFFEINE_FILE_SLAB_SIZE)) return FALSE;
    for (i = 0; i < KAFFEINE_MAX_FILES; ++i)
        if (ctx->files[i].guest &&
            (Kaffeine_Overlap(port, size, ctx->files[i].guest, 86) ||
             (ctx->files[i].mirror && Kaffeine_Overlap(port, size, ctx->files[i].mirror, KAFFEINE_FILE_BUFFER_LIMIT))))
            return FALSE;
    for (i = 0; i < KAFFEINE_MAX_APP_PORTS; ++i)
        if (ctx->app_ports[i].guest && Kaffeine_Overlap(port, size, ctx->app_ports[i].guest, 34)) return FALSE;
    for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS; ++i)
        if (ctx->io_requests[i].guest && Kaffeine_Overlap(port, size, ctx->io_requests[i].guest,
            ctx->io_requests[i].capacity ? ctx->io_requests[i].capacity : 48)) return FALSE;
    return TRUE;
}

static BOOL message_size(struct KaffeineContext *ctx, ULONG msg, ULONG *size)
{
    if ((msg & 1) || !Kaffeine_GuestBytes(ctx, msg, 20))
        return FALSE;
    *size = m68k_read16(ctx, msg + 18);
    return *size >= 20 && Kaffeine_GuestBytes(ctx, msg, *size);
}

BOOL Kaffeine_QueueDisjoint(const struct KaffeineQueue *q, ULONG a, ULONG n)
{
    unsigned i;
    if (Kaffeine_Overlap(q->port, 34, a, n))
        return FALSE;
    for (i = 0; i < q->count; ++i)
        if (Kaffeine_Overlap(q->nodes[i], q->sizes[i], a, n))
            return FALSE;
    return TRUE;
}

/* Arrival permissions are needed only when adding a message. Inspecting or
   removing an existing queue must not consult its action, signal bit or task. */
static BOOL queue_arrival(struct KaffeineContext *ctx, ULONG port)
{
    ULONG task, flags, bit;
    unsigned i;
    if (!arena(ctx, port, 34)) return FALSE;
    flags = ctx->mem[port + 14];
    if (flags != 0 && flags != 2)
        return FALSE;
    task = ctx->guest_exec_task;
    if (task && (!arena(ctx, ctx->m68k_sysbase, M68K_EXECBASE_THISTASK + 4) ||
        m68k_read32(ctx, ctx->m68k_sysbase + M68K_EXECBASE_THISTASK) != task)) return FALSE;
    if (flags == 2 && m68k_read32(ctx, port + 16) &&
        m68k_read32(ctx, port + 16) != task)
        return FALSE;
    if (!flags)
    {
        bit = ctx->mem[port + 15];
        if (!arena(ctx, ctx->m68k_sysbase, M68K_EXECBASE_THISTASK + 4))
            return FALSE;
        task = m68k_read32(ctx, ctx->m68k_sysbase + M68K_EXECBASE_THISTASK);
        if ((ctx->guest_exec_task && task != ctx->guest_exec_task) || !task || !arena(ctx, task, 92) ||
            m68k_read32(ctx, port + 16) != task || bit > 31 || !(ctx->guest_signals & (1UL << bit)))
            return FALSE;
        for (i = 0; i < KAFFEINE_MAX_IO_PORTS; ++i)
            if (ctx->io_ports[i].guest == port && ctx->io_ports[i].sigbit != (BYTE)bit)
                return FALSE;
    }
    return TRUE;
}

/* Validate guest ownership and the complete FIFO geometry without performing
   or authorizing any arrival action. */
BOOL Kaffeine_QueueRead(struct KaffeineContext *ctx, ULONG port, struct KaffeineQueue *q)
{
    ULONG list = port + 20, node, pred;
    q->port = port;
    q->count = 0;
    if ((port & 1) || !Kaffeine_GuestBytes(ctx, port, 34) || !ordinary(ctx, port, 34) || ctx->mem[port + 8] != 4)
        return FALSE;
    if (m68k_read32(ctx, list + 4))
        return FALSE;
    pred = list;
    node = m68k_read32(ctx, list);
    while (node != list + 4)
    {
        ULONG size;
        if (q->count == KAFFEINE_MESSAGE_LIMIT || !message_size(ctx, node, &size) ||
            !ordinary(ctx, node, size) || !Kaffeine_QueueDisjoint(q, node, size) ||
            m68k_read32(ctx, node + 4) != pred ||
            (ctx->mem[node + 8] != 5 && ctx->mem[node + 8] != 7))
            return FALSE;
        q->nodes[q->count] = node;
        q->sizes[q->count++] = size;
        pred = node;
        node = m68k_read32(ctx, node);
    }
    return m68k_read32(ctx, list + 8) == pred;
}

/* Authorize the destination's arrival action before any write or native I/O.
   Other active ports need geometry checks only. Validate those queues too, to
   catch duplicate sends even though RemHead leaves old node links. */
BOOL Kaffeine_MessagePreflight(struct KaffeineContext *ctx, struct KaffeineQueue *q,
                               ULONG msg, ULONG size)
{
    struct KaffeineQueue other;
    unsigned i, free_slot = KAFFEINE_MAX_MESSAGE_PORTS;
    BOOL known = FALSE;
    if (!queue_arrival(ctx, q->port) ||
        !Kaffeine_GuestBytes(ctx, msg, size) || !ordinary(ctx, msg, size) ||
        !Kaffeine_QueueDisjoint(q, msg, size) || q->count == KAFFEINE_MESSAGE_LIMIT)
        return FALSE;
    /* A node still linked by both neighbours is queued even if its manual
       port has not been seen before. Removed nodes retain stale links. */
    {
        ULONG prev = m68k_read32(ctx, msg + 4), next = m68k_read32(ctx, msg);
        if (arena(ctx, prev, 4) && arena(ctx, next, 8) &&
            m68k_read32(ctx, prev) == msg && m68k_read32(ctx, next + 4) == msg) return FALSE;
    }
    for (i = 0; i < KAFFEINE_MAX_MESSAGE_PORTS; ++i)
    {
        ULONG p = ctx->message_ports[i];
        if (!p) { free_slot = i; continue; }
        if (p == q->port) { known = TRUE; continue; }
        if (!Kaffeine_QueueRead(ctx, p, &other) || !Kaffeine_QueueDisjoint(&other, msg, size) ||
            !Kaffeine_QueueDisjoint(&other, q->port, 34))
            return FALSE;
        {
            unsigned j;
            for (j = 0; j < q->count; ++j)
                if (!Kaffeine_QueueDisjoint(&other, q->nodes[j], q->sizes[j])) return FALSE;
        }
    }
    return known || free_slot < KAFFEINE_MAX_MESSAGE_PORTS;
}

void Kaffeine_MessageAppend(struct KaffeineContext *ctx, const struct KaffeineQueue *q,
                            ULONG msg, UBYTE type)
{
    ULONG list = q->port + 20, tail = q->count ? q->nodes[q->count - 1] : list;
    unsigned i, slot = KAFFEINE_MAX_MESSAGE_PORTS;
    for (i = 0; i < KAFFEINE_MAX_MESSAGE_PORTS; ++i)
    {
        if (ctx->message_ports[i] == q->port) { slot = i; break; }
        if (!ctx->message_ports[i]) slot = i;
    }
    if (slot < KAFFEINE_MAX_MESSAGE_PORTS) ctx->message_ports[slot] = q->port;
    ctx->mem[msg + 8] = type;
    m68k_write32(ctx, msg, list + 4);
    m68k_write32(ctx, msg + 4, tail);
    m68k_write32(ctx, tail, msg);
    m68k_write32(ctx, list + 8, msg);
    if (ctx->mem[q->port + 14] == 0)
        Signal(FindTask(NULL), 1UL << ctx->mem[q->port + 15]);
}

IPTR Kaffeine_GuestPutMsg(struct KaffeineContext *ctx, ULONG port, ULONG msg, ULONG d0)
{
    struct KaffeineQueue q;
    ULONG size;
    if (!message_size(ctx, msg, &size) || !Kaffeine_QueueRead(ctx, port, &q) ||
        !Kaffeine_MessagePreflight(ctx, &q, msg, size))
        return Kaffeine_MessageFailure(ctx, "PutMsg");
    Kaffeine_MessageAppend(ctx, &q, msg, 5);
    return d0;
}

IPTR Kaffeine_GuestGetMsg(struct KaffeineContext *ctx, ULONG port, BOOL wait)
{
    struct KaffeineQueue q, other;
    ULONG first, next;
    unsigned i, j, slot = KAFFEINE_MAX_MESSAGE_PORTS;
    BOOL known = FALSE;
    if (!Kaffeine_QueueRead(ctx, port, &q))
    {
        if (arena(ctx, port, 34))
        {
            ULONG head = m68k_read32(ctx, port + 20);
            bug("[kaffeine] %s port=%08lx type=%lu flags=%02lx bit=%lu task=%08lx list=%08lx/%08lx/%08lx\n",
                wait ? "WaitPort" : "GetMsg", (unsigned long)port,
                (unsigned long)ctx->mem[port + 8], (unsigned long)ctx->mem[port + 14],
                (unsigned long)ctx->mem[port + 15],
                (unsigned long)m68k_read32(ctx, port + 16),
                (unsigned long)m68k_read32(ctx, port + 20),
                (unsigned long)m68k_read32(ctx, port + 24),
                (unsigned long)m68k_read32(ctx, port + 28));
            bug("[kaffeine] queue state signals=%08lx trusted-task=%08lx count=%lu\n",
                (unsigned long)ctx->guest_signals, (unsigned long)ctx->guest_exec_task,
                (unsigned long)q.count);
            if (arena(ctx, head, 20))
                bug("[kaffeine] queue head=%08lx type=%lu length=%lu links=%08lx/%08lx\n",
                    (unsigned long)head, (unsigned long)ctx->mem[head + 8],
                    (unsigned long)m68k_read16(ctx, head + 18),
                    (unsigned long)m68k_read32(ctx, head),
                    (unsigned long)m68k_read32(ctx, head + 4));
        }
        return Kaffeine_MessageFailure(ctx, wait ? "WaitPort" : "GetMsg");
    }
    for (i = 0; i < KAFFEINE_MAX_MESSAGE_PORTS; ++i)
    {
        ULONG p = ctx->message_ports[i];
        if (p == port) { slot = i; known = TRUE; continue; }
        if (!p) { if (!known) slot = i; continue; }
        if (!Kaffeine_QueueRead(ctx, p, &other) || !Kaffeine_QueueDisjoint(&other, port, 34))
            return Kaffeine_MessageFailure(ctx, "active guest queue");
        for (j = 0; j < q.count; ++j)
            if (!Kaffeine_QueueDisjoint(&other, q.nodes[j], q.sizes[j]))
                return Kaffeine_MessageFailure(ctx, "overlapping guest queues");
    }
    if (!q.count)
    {
        if (wait) return Kaffeine_MessageFailure(ctx, "empty WaitPort");
        for (i = 0; i < KAFFEINE_MAX_MESSAGE_PORTS; ++i)
            if (ctx->message_ports[i] == port) ctx->message_ports[i] = 0;
        return 0;
    }
    if ((wait || q.count > 1) && slot == KAFFEINE_MAX_MESSAGE_PORTS)
        return Kaffeine_MessageFailure(ctx, "active guest port capacity");
    first = q.nodes[0];
    if (!wait)
    {
        next = q.count > 1 ? q.nodes[1] : port + 24;
        m68k_write32(ctx, port + 20, next);
        m68k_write32(ctx, next + 4, port + 20);
        if (q.count == 1)
        {
            for (i = 0; i < KAFFEINE_MAX_MESSAGE_PORTS; ++i)
                if (ctx->message_ports[i] == port) ctx->message_ports[i] = 0;
        }
        else ctx->message_ports[slot] = port;
    }
    else ctx->message_ports[slot] = port;
    return first;
}

IPTR Kaffeine_GuestReplyMsg(struct KaffeineContext *ctx, ULONG msg)
{
    struct KaffeineQueue q;
    ULONG size, reply;
    if (!message_size(ctx, msg, &size) || !ordinary(ctx, msg, size))
        return Kaffeine_MessageFailure(ctx, "ReplyMsg");
    reply = m68k_read32(ctx, msg + 14);
    if (!reply)
    {
        unsigned i;
        ULONG prev = m68k_read32(ctx, msg + 4), next = m68k_read32(ctx, msg);
        if (arena(ctx, prev, 4) && arena(ctx, next, 8) &&
            m68k_read32(ctx, prev) == msg && m68k_read32(ctx, next + 4) == msg)
            return Kaffeine_MessageFailure(ctx, "ReplyMsg queued node");
        for (i = 0; i < KAFFEINE_MAX_MESSAGE_PORTS; ++i)
            if (ctx->message_ports[i] &&
                (!Kaffeine_QueueRead(ctx, ctx->message_ports[i], &q) ||
                 !Kaffeine_QueueDisjoint(&q, msg, size)))
                return Kaffeine_MessageFailure(ctx, "ReplyMsg");
        ctx->mem[msg + 8] = 6; /* NT_FREEMSG */
        return 0;
    }
    if (!Kaffeine_QueueRead(ctx, reply, &q) || !Kaffeine_MessagePreflight(ctx, &q, msg, size))
        return Kaffeine_MessageFailure(ctx, "ReplyMsg");
    Kaffeine_MessageAppend(ctx, &q, msg, 7);
    return 0;
}

BOOL Kaffeine_MessageMemory(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    struct KaffeineQueue q;
    unsigned i;
    if (size && (!Kaffeine_GuestBytes(ctx, addr, size) || !ordinary(ctx, addr, size))) return FALSE;
    for (i = 0; i < KAFFEINE_MAX_MESSAGE_PORTS; ++i)
        if (ctx->message_ports[i] &&
            (!Kaffeine_QueueRead(ctx, ctx->message_ports[i], &q) || !Kaffeine_QueueDisjoint(&q, addr, size))) return FALSE;
    return TRUE;
}
