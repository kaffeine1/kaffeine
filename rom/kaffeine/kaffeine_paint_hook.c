/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Author: Michele Dipace (@kaffeine1)
 * Bounded native backfill recipes. Guest code is compiled on the owner;
 * a foreign native callback never enters a guest CPU or a guest service. */
#include <aros/asmcall.h>
#include <aros/libcall.h>
#include <exec/nodes.h>
#include <aros/debug.h>
#include <exec/memory.h>
#include <utility/hooks.h>
#include <graphics/gfx.h>
#include <graphics/layers.h>
#include <graphics/rastport.h>
#include <hidd/gfx.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/layers.h>
#include <string.h>
#include "kaffeine_intern.h"
#include "kaffeine_callback.h"
#include "kaffeine_messages.h"
#include "kaffeine_paint_compile.h"
#include "kaffeine_paint_hook.h"
#include "kaffeine_sched.h"

#define PAINT_BINDINGS 16u
#define BREAK_CAPABILITIES 0x0000f000u

struct PaintOwner {
    ULONG task;
    uint64_t cookie;
    BOOL live;
};
struct PaintBinding {
    struct PaintBinding *next;
    struct KaffeinePaintHooks *state;
    struct Hook hook;
    struct Layer *layer;
    struct Hook *baseline;
    ULONG guest_layer, guest_hook, owner_task, native_signal_lease;
    uint64_t owner_cookie;
    /* Immutable storage identities: release preflights keep every referenced
     * interval alive. The callback never looks through allocator metadata. */
    const UBYTE *arena;
    ULONG arena_size;
    struct KaffeinePaintPlan *plan;
    struct KaffeinePaintInputs *input;
    struct KaffeinePaintOutput *output;
    void *evaluate;
    ULONG evaluate_size;
    ULONG enabled, active, fault, pending;
};
struct KaffeinePaintHooks {
    struct PaintBinding *bindings;
    struct Task *driver;
    ULONG root_task, wake_mask, count;
    LONG wake_bit;
    uint64_t serial;
    BOOL root_retired;
    struct PaintOwner owners[PAINT_BINDINGS + 1];
};

static BOOL owner_driver(struct KaffeineContext *ctx, const struct KaffeinePaintHooks *s)
{
    return ctx && ctx->exec_task && !ctx->broker_call && !ctx->broker_owner &&
           FindTask(NULL) == ctx->exec_task && (!s || s->driver == ctx->exec_task);
}
static BOOL fail(struct KaffeineContext *ctx, const char *why)
{
    bug("[kaffeine] bounded paint hook: %s, stopping (20)\n", why);
    if (ctx && ctx->cpu) Kaffeine_StopGuest(ctx, ctx->cpu, 20);
    else if (ctx) { ctx->running = FALSE; ctx->callback_failed = TRUE; ctx->exit_code = 20; }
    return FALSE;
}
/* kernel.resource starts with a Node, not a Library. The public kernel
 * vector gives the registered processor count without guessing a version
 * or reading a Library header before the resource. */
static BOOL one_cpu(void)
{
    struct Node *kernel = (struct Node *)OpenResource("kernel.resource");
    return kernel && AROS_LC0(unsigned int, KrnGetCPUCount,
                              struct Node *, kernel, 40, Kernel) == 1;
}
static BOOL span(ULONG arena, ULONG address, ULONG width)
{ return address && address < arena && width && width <= arena - address; }
static uint32_t number(const UBYTE *p, unsigned n)
{
    uint32_t v = 0;
    while (n--) v = (v << 8) | *p++;
    return v;
}
static void word(UBYTE *p, UWORD n) { p[0] = (UBYTE)(n >> 8); p[1] = (UBYTE)n; }
static BOOL overlaps(ULONG a, ULONG n, ULONG b, ULONG m)
{ return n && m && (a <= b ? b - a < n : a - b < m); }
static struct PaintBinding *binding_layer(struct KaffeinePaintHooks *s, const struct Layer *l)
{
    struct PaintBinding *b;
    for (b = s ? s->bindings : NULL; b; b = b->next) if (b->layer == l) return b;
    return NULL;
}
static struct PaintOwner *live_owner(struct KaffeinePaintHooks *s, ULONG task)
{
    unsigned i;
    for (i = 0; i < PAINT_BINDINGS + 1; ++i)
        if (s->owners[i].live && s->owners[i].task == task) return &s->owners[i];
    return NULL;
}
static struct PaintOwner *reserve_owner(struct KaffeinePaintHooks *s, ULONG task)
{
    struct PaintOwner *o = live_owner(s, task);
    unsigned i;
    if (o) return o;
    if (!task || s->serial == UINT64_MAX) return NULL;
    for (i = 0; i < PAINT_BINDINGS + 1; ++i)
        if (!s->owners[i].live)
        {
            o = &s->owners[i];
            o->task = task; o->cookie = ++s->serial; o->live = TRUE;
            return o;
        }
    return NULL;
}
/* Empty owners carry no pending event after their last binding was drained.
 * Rebinding gets a new cookie, including reuse of the same numeric TCB. */
static void forget_unused_owners(struct KaffeinePaintHooks *s)
{
    unsigned i;
    for (i = 0; i < PAINT_BINDINGS + 1; ++i)
    {
        struct PaintBinding *b;
        if (!s->owners[i].live) continue;
        for (b = s->bindings; b; b = b->next)
            if (b->owner_task == s->owners[i].task && b->owner_cookie == s->owners[i].cookie) break;
        if (!b) s->owners[i].live = FALSE;
    }
}
static ULONG private_task(struct KaffeineContext *ctx)
{
    if (ctx->sched)
        return ctx->task_ops && ctx->task_ops->caller_task ?
            ctx->task_ops->caller_task(ctx, ctx->cpu) : 0;
    return ctx->guest_exec_task;
}
static BOOL can_signal(struct KaffeineContext *ctx, struct KaffeinePaintHooks *s,
                       ULONG task, uint64_t cookie, ULONG bits)
{
    struct PaintOwner *o = live_owner(s, task);
    if (!o || o->cookie != cookie || (bits & s->wake_mask)) return FALSE;
    if (ctx->sched)
        return ctx->task_ops && ctx->task_ops->can_signal &&
               ctx->task_ops->can_signal(ctx, task, bits);
    return !s->root_retired && task == s->root_task &&
           !(bits & ~(ctx->guest_signals | BREAK_CAPABILITIES));
}
static BOOL deliver(struct KaffeineContext *ctx, struct PaintBinding *b)
{
    ULONG bits = __atomic_exchange_n(&b->pending, 0, __ATOMIC_ACQ_REL);
    if (!bits) return TRUE;
    if (!can_signal(ctx, b->state, b->owner_task, b->owner_cookie, bits))
        return fail(ctx, "deferred signal capability retired");
    if (ctx->sched) ctx->task_ops->send_signal(ctx, b->owner_task, bits);
    else Signal(b->state->driver, bits);
    return TRUE;
}
static void wake(struct PaintBinding *b, ULONG fault, ULONG signals)
{
    BOOL notify = FALSE;
    if (fault && !__atomic_exchange_n(&b->fault, fault, __ATOMIC_ACQ_REL)) notify = TRUE;
    if (signals && ((__atomic_fetch_or(&b->pending, signals, __ATOMIC_ACQ_REL) & signals) != signals))
        notify = TRUE;
    /* Captured driver and signal outlive every installed Hook/callback.
     * This is the only native notification; it is never a guest signal. */
    if (notify) Signal(b->state->driver, b->state->wake_mask);
}

/* Only the public scalar bytes can be observed by an accepted program.
 * Pointer/private bytes are copy-only symbolic capabilities in the compiler;
 * their integer representation cannot influence a path or an operation. */
static void rp_scalars(UBYTE *g, const struct RastPort *rp)
{
    unsigned i;
    memset(g, 0, KPAINT_RASTPORT_BYTES);
    g[24] = rp->Mask; g[25] = (UBYTE)rp->FgPen; g[26] = (UBYTE)rp->BgPen;
    g[27] = (UBYTE)rp->AOlPen; g[28] = (UBYTE)rp->DrawMode;
    g[29] = (UBYTE)rp->AreaPtSz; g[30] = (UBYTE)rp->linpatcnt;
    word(g + 32, rp->Flags & (FRST_DOT | ONE_DOT | DBUFFER | AREAOUTLINE | NOCROSSFILL));
    word(g + 34, rp->LinePtrn); word(g + 36, (UWORD)rp->cp_x); word(g + 38, (UWORD)rp->cp_y);
    for (i = 0; i < 8; ++i) g[40 + i] = rp->minterms[i];
    word(g + 48, (UWORD)rp->PenWidth); word(g + 50, (UWORD)rp->PenHeight);
    g[56] = rp->AlgoStyle; g[57] = rp->TxFlags;
    word(g + 58, rp->TxHeight); word(g + 60, rp->TxWidth);
    word(g + 62, rp->TxBaseline); word(g + 64, (UWORD)rp->TxSpacing);
}

static BOOL snapshot(struct PaintBinding *b, const struct RastPort *rp,
                     const struct BackFillMessage *m, struct RastPort *captured)
{
    struct KaffeinePaintInputs *in = b->input;
    unsigned i;
    BOOL okay = TRUE;
    /* UP-only: bounded memory reads, arithmetic and local copies. No OS
     * allocation, wait, graphics or Signal is executed in this section. */
    Forbid();
    if (!__atomic_load_n(&b->enabled, __ATOMIC_ACQUIRE) || !rp || !m ||
        m->Layer != b->layer || !rp->BitMap || !IS_HIDD_BM(rp->BitMap) || rp->AreaPtrn ||
        m->Bounds.MinX > m->Bounds.MaxX || m->Bounds.MinY > m->Bounds.MaxY) okay = FALSE;
    for (i = 0; okay && i < b->plan->dependencies; ++i)
    {
        const struct KaffeinePaintDependency *d = &b->plan->dependency[i];
        if (!span(b->arena_size, d->address, d->width) ||
            (d->width != 1 && d->width != 2 && d->width != 4)) okay = FALSE;
        else in->dependencies[i] = number(b->arena + d->address, d->width);
    }
    for (i = 0; okay && i < b->plan->cells; ++i)
    {
        const struct KaffeinePaintCell *c = &b->plan->cell[i];
        if (!span(b->arena_size, c->address, c->width)) { okay = FALSE; break; }
        if (c->role & KPAINT_CELL_IMMUTABLE)
            in->cells[i] = number(b->arena + c->address, c->width);
        else if (c->role & KPAINT_CELL_OWNER_CAP)
            in->cells[i] = number(b->arena + c->address, c->width);
        else if ((c->role & KPAINT_CELL_BYTE) && !(c->used_mask & ~255u))
            in->cells[i] = b->arena[c->address + c->width - 1];
        else okay = FALSE;
    }
    if (okay)
    {
        *captured = *rp;
        rp_scalars(in->rp, captured);
        in->bounds[0] = m->Bounds.MinX; in->bounds[1] = m->Bounds.MinY;
        in->bounds[2] = m->Bounds.MaxX; in->bounds[3] = m->Bounds.MaxY;
        in->offset_x = m->OffsetX; in->offset_y = m->OffsetY;
        in->layer_token = b->guest_layer; in->owner_task = b->owner_task;
        in->owner_cookie = b->owner_cookie; in->allowed_signals = b->plan->signal_mask;
    }
    Permit();
    return okay;
}

static BOOL native_output(struct PaintBinding *b)
{
    unsigned i;
    UBYTE made[KPAINT_MAX_CLONES] = {0};
    if (b->output->count > KPAINT_MAX_COMMANDS) return FALSE;
    for (i = 0; i < b->output->count; ++i)
    {
        const struct KaffeinePaintCommand *c = &b->output->commands[i];
        if (c->kind == KPAINT_DEFER_SIGNAL)
        {
            if (c->owner_cookie != b->owner_cookie || c->value[0] != b->owner_task ||
                (c->value[1] & ~b->plan->signal_mask)) return FALSE;
            continue;
        }
        if (c->clone >= KPAINT_MAX_CLONES) return FALSE;
        if (c->kind == KPAINT_CLONE) { made[c->clone] = TRUE; continue; }
        if (!made[c->clone]) return FALSE;
        if (c->kind == KPAINT_RECTFILL)
        {
            WORD x0 = (WORD)c->value[0], y0 = (WORD)c->value[1];
            WORD x1 = (WORD)c->value[2], y1 = (WORD)c->value[3];
            if (x0 > x1 || y0 > y1 || x0 < b->input->bounds[0] || y0 < b->input->bounds[1] ||
                x1 > b->input->bounds[2] || y1 > b->input->bounds[3]) return FALSE;
        }
        else if ((c->kind != KPAINT_APEN && c->kind != KPAINT_DRAWMODE && c->kind != KPAINT_MASK) ||
                 c->value[0] > 255) return FALSE;
    }
    return TRUE;
}

AROS_UFH3S(IPTR, kaffeine_paint_backfill,
    AROS_UFHA(struct Hook *, h, A0), AROS_UFHA(struct RastPort *, rp, A2),
    AROS_UFHA(struct BackFillMessage *, m, A1))
{
    AROS_USERFUNC_INIT
    struct PaintBinding *b;
    struct RastPort captured, clones[KPAINT_MAX_CLONES];
    ULONG expected = 0, signals = 0;
    unsigned i;
    enum KaffeinePaintStatus status;
    /* The native caller owns this Layer throughout CallHookPkt(). That is
     * the storage/quiescence capability, checked before publishing active.
     * On UP, no driver retirement can interleave with this short check.
     * An unowned invocation cannot safely retain a driver/wake capability:
     * reject it without an event and touch no Hook storage after Permit(). */
    Forbid();
    b = h ? h->h_Data : NULL;
    if (!b || !b->layer || b->layer->Lock.ss_Owner != FindTask(NULL) ||
        b->layer->Lock.ss_NestCount < 0)
    { Permit(); return 0; }
    /* A recursive valid invocation still has the outer Layer lifetime.
     * Refuse before either invocation can share mutable workspaces. */
    if (!__atomic_compare_exchange_n(&b->active, &expected, 1, FALSE,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    { Permit(); wake(b, KPAINT_LIMIT, 0); return 0; }
    Permit();
    if (__atomic_load_n(&b->fault, __ATOMIC_ACQUIRE)) goto done;
    if (!snapshot(b, rp, m, &captured)) { wake(b, KPAINT_CHANGED, 0); goto done; }
    status = Kaffeine_PaintEvaluate(b->plan, b->input, b->evaluate,
                                    b->evaluate_size, b->output);
    if (status != KPAINT_OK || !native_output(b))
    { wake(b, status == KPAINT_OK ? KPAINT_INPUT : (ULONG)status, 0); goto done; }
    /* Every dependency, scalar, capability and paint rectangle has passed.
     * These calls operate solely on callback-local native RastPorts. */
    for (i = 0; i < b->output->count; ++i)
    {
        const struct KaffeinePaintCommand *c = &b->output->commands[i];
        struct RastPort *clone = &clones[c->clone < KPAINT_MAX_CLONES ? c->clone : 0];
        switch (c->kind)
        {
        case KPAINT_CLONE: *clone = captured; clone->Layer = NULL; break;
        case KPAINT_APEN: SetAPen(clone, (UBYTE)c->value[0]); break;
        case KPAINT_DRAWMODE: SetDrMd(clone, (UBYTE)c->value[0]); break;
        case KPAINT_MASK: clone->Mask = (UBYTE)c->value[0]; break;
        case KPAINT_RECTFILL:
            RectFill(clone, (WORD)c->value[0], (WORD)c->value[1], (WORD)c->value[2], (WORD)c->value[3]);
            break;
        case KPAINT_DEFER_SIGNAL: signals |= c->value[1]; break;
        }
    }
    if (signals) wake(b, 0, signals);
done:
    __atomic_store_n(&b->active, 0, __ATOMIC_RELEASE);
    return 0;
    AROS_USERFUNC_EXIT
}

static int source_read(void *opaque, uint32_t a, void *out, uint32_t n)
{
    struct KaffeineContext *ctx = opaque;
    if (!Kaffeine_GuestBytes(ctx, a, n) || !Kaffeine_HeapGuestRange(ctx, a, n)) return 0;
    memcpy(out, ctx->mem + a, n);
    return 1;
}
static int source_ordinary(void *opaque, uint32_t a, uint32_t n)
{ return Kaffeine_MessageMemory(opaque, a, n) && Kaffeine_HeapGuestRange(opaque, a, n); }
static void destroy_binding(struct PaintBinding *b)
{
    if (!b) return;
    if (b->plan) FreeMem(b->plan, sizeof(*b->plan));
    if (b->input) FreeMem(b->input, sizeof(*b->input));
    if (b->output) FreeMem(b->output, sizeof(*b->output));
    if (b->evaluate) FreeMem(b->evaluate, b->evaluate_size);
    FreeMem(b, sizeof(*b));
}
static struct PaintBinding *compile_binding(struct KaffeineContext *ctx,
    struct KaffeinePaintHooks *s, struct Layer *layer, ULONG gl, ULONG gh, ULONG task)
{
    struct PaintBinding *b;
    struct KaffeinePaintSource source;
    struct KaffeinePaintReport report;
    struct PaintOwner *o;
    void *work;
    size_t bytes = Kaffeine_PaintCompileWorkspaceSize();
    size_t evaluate_bytes = Kaffeine_PaintEvaluateWorkspaceSize();
    unsigned i;
    enum KaffeinePaintStatus status;
    /* This profile deliberately relies on the uniprocessor owner model.
     * Forbid cannot give its coherent dependency snapshot on EXECSMP. */
#if defined(__AROSEXEC_SMP__)
    return NULL;
#endif
    if (!bytes || bytes > UINT32_MAX || !evaluate_bytes || evaluate_bytes > UINT32_MAX) return NULL;
    b = AllocMem(sizeof(*b), MEMF_PUBLIC | MEMF_CLEAR);
    if (!b) return NULL;
    b->state = s; b->layer = layer; b->guest_layer = gl; b->guest_hook = gh; b->owner_task = task;
    b->arena = ctx->mem; b->arena_size = ctx->mem_size;
    b->evaluate_size = (ULONG)evaluate_bytes;
    b->plan = AllocMem(sizeof(*b->plan), MEMF_PUBLIC | MEMF_CLEAR);
    b->input = AllocMem(sizeof(*b->input), MEMF_PUBLIC | MEMF_CLEAR);
    b->output = AllocMem(sizeof(*b->output), MEMF_PUBLIC | MEMF_CLEAR);
    b->evaluate = AllocMem(b->evaluate_size, MEMF_PUBLIC | MEMF_CLEAR);
    work = AllocMem((ULONG)bytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (!b->plan || !b->input || !b->output || !b->evaluate || !work ||
        ((IPTR)work % Kaffeine_PaintWorkspaceAlignment()) ||
        ((IPTR)b->evaluate % Kaffeine_PaintWorkspaceAlignment()))
    { if (work) FreeMem(work, (ULONG)bytes); destroy_binding(b); return NULL; }
    memset(&source, 0, sizeof(source));
    source.opaque = ctx; source.read = source_read; source.ordinary = source_ordinary;
    source.arena_size = ctx->mem_size; source.hook = gh; source.exec_base = ctx->m68k_sysbase;
    source.graphics_base = Kaffeine_FindLibBase(ctx, "graphics.library");
    status = Kaffeine_PaintCompile(&source, work, bytes, b->plan, &report);
    FreeMem(work, (ULONG)bytes);
    if (status != KPAINT_OK || b->plan->version != KPAINT_VERSION ||
        b->plan->dependencies > KPAINT_MAX_DEPS || b->plan->cells > KPAINT_MAX_CELLS)
    { destroy_binding(b); return NULL; }
    /* The dummy byte and nonpublic native Flags have no classic scalar
     * contract. The compiler may copy them, but must not observe them. */
    if (b->plan->rp_used[31] || b->plan->rp_used[32] ||
        (b->plan->rp_used[33] & ~(FRST_DOT | ONE_DOT | DBUFFER | AREAOUTLINE | NOCROSSFILL)))
    { destroy_binding(b); return NULL; }
    for (i = 0; i < b->plan->cells; ++i)
    {
        struct KaffeinePaintCell *c = &b->plan->cell[i];
        if (!span(ctx->mem_size, c->address, c->width) ||
            (c->width != 1 && c->width != 2 && c->width != 4) ||
            !(c->role & (KPAINT_CELL_BYTE | KPAINT_CELL_OWNER_CAP | KPAINT_CELL_IMMUTABLE)) ||
            ((c->role & KPAINT_CELL_BYTE) && !(c->role & (KPAINT_CELL_OWNER_CAP | KPAINT_CELL_IMMUTABLE)) &&
             (c->used_mask & ~255u)))
        { destroy_binding(b); return NULL; }
    }
    o = reserve_owner(s, task);
    /* caller_task already proved a live installing owner. A pure paint
     * recipe needs no notification capability; can_signal rejects zero. */
    if (!o || (b->plan->signal_mask &&
               !can_signal(ctx, s, task, o->cookie, b->plan->signal_mask)))
    { destroy_binding(b); return NULL; }
    b->owner_cookie = o->cookie;
    b->native_signal_lease = b->plan->signal_mask & ctx->guest_signals;
    b->hook.h_Entry = (APTR)kaffeine_paint_backfill; b->hook.h_Data = b;
    return b;
}
static struct KaffeinePaintHooks *new_state(struct KaffeineContext *ctx)
{
    struct KaffeinePaintHooks *s = AllocMem(sizeof(*s), MEMF_PUBLIC | MEMF_CLEAR);
    LONG bit;
    if (!s) return NULL;
    bit = AllocSignal(-1);
    if (bit < 0) { FreeMem(s, sizeof(*s)); return NULL; }
    s->driver = ctx->exec_task;
    /* Capture the scheduler's private root, never its current typed DOS
     * view. A live root may outlast the last child and scheduler itself.
     * A retired root must never be replaced by a child's numeric identity. */
    if (ctx->sched)
    {
        if (ctx->task_ops && ctx->task_ops->root_task)
        {
            s->root_task = ctx->task_ops->root_task(ctx);
            s->root_retired = !s->root_task;
        }
    }
    else s->root_task = ctx->guest_exec_task;
    s->wake_bit = bit; s->wake_mask = 1UL << bit;
    return s;
}

BOOL Kaffeine_PaintHookChange(struct KaffeineContext *ctx,
    struct KaffeinePaintHooks **at, struct Layer *layer, ULONG gl, ULONG gh, ULONG *previous)
{
    struct KaffeinePaintHooks *s = at ? *at : NULL;
    struct PaintBinding *old, *b = NULL, **link;
    struct Hook *native_old, *wanted;
    ULONG task;
    BOOL fresh = FALSE;
    if (!at || !previous || !layer || !gl || !owner_driver(ctx, s) || !ctx->running ||
        ctx->callback_depth || !(task = private_task(ctx)))
        return fail(ctx, "install outside the private driver owner");
    if (s && !Kaffeine_PaintHookPoll(ctx, s)) return FALSE;
    if (gh > 1 && !one_cpu()) return fail(ctx, "no single-processor kernel proof");
    old = binding_layer(s, layer);
    if (!s && gh <= 1) return FALSE; /* not a recipe operation */
    if (!s) { s = new_state(ctx); if (!s) return fail(ctx, "no event owner"); fresh = TRUE; }
    if (!ctx->sched && !s->root_task && !s->root_retired)
        s->root_task = task; /* exact private root, outside a scheduler */
    if (gh > 1)
    {
        if ((!old && s->count == PAINT_BINDINGS) || !(b = compile_binding(ctx, s, layer, gl, gh, task)))
            goto failed;
        wanted = &b->hook;
    }
    else wanted = (struct Hook *)(IPTR)gh;
    /* This native API/lock is the retirement barrier. It may not be held
     * together with a private metadata semaphore or a wait for guest work. */
    if (!AttemptSemaphore(&layer->Lock)) goto failed;
    native_old = layer->BackFill;
    if ((old && (native_old != &old->hook || __atomic_load_n(&old->fault, __ATOMIC_ACQUIRE))) ||
        (!old && (IPTR)native_old > 1))
    { UnlockLayer(layer); goto failed; }
    if (old) __atomic_store_n(&old->enabled, 0, __ATOMIC_RELEASE);
    if (b)
    {
        b->baseline = old ? old->baseline : native_old;
        __atomic_store_n(&b->enabled, 1, __ATOMIC_RELEASE);
    }
    InstallLayerHook(layer, wanted);
    *previous = old ? old->guest_hook : (ULONG)(IPTR)native_old;
    if (old)
    {
        for (link = &s->bindings; *link != old; link = &(*link)->next) ;
        *link = old->next; --s->count;
    }
    if (b) { b->next = s->bindings; s->bindings = b; ++s->count; }
    *at = s;
    UnlockLayer(layer);
    if (old)
    {
        /* A Signal already emitted by the old invocation remains valid
         * across replacement; it does not become a signal of the new owner. */
        BOOL okay = deliver(ctx, old);
        ULONG fault = __atomic_load_n(&old->fault, __ATOMIC_ACQUIRE);
        destroy_binding(old);
        forget_unused_owners(s);
        if (!okay || fault) return fail(ctx, "retired recipe failed");
    }
    return TRUE;
failed:
    destroy_binding(b);
    forget_unused_owners(s);
    if (fresh) { SetSignal(0, s->wake_mask); FreeSignal(s->wake_bit); FreeMem(s, sizeof(*s)); }
    return fail(ctx, "unsupported recipe, capability or native baseline");
}

ULONG Kaffeine_PaintHookGuest(const struct KaffeinePaintHooks *s, const struct Layer *l,
                              const struct Hook *h)
{
    const struct PaintBinding *b;
    for (b = s ? s->bindings : NULL; b; b = b->next)
        if (b->layer == l && h == &b->hook) return b->guest_hook;
    return 0;
}
BOOL Kaffeine_PaintHookHasLayer(const struct KaffeinePaintHooks *s, const struct Layer *l)
{ return binding_layer((struct KaffeinePaintHooks *)s, l) != NULL; }
static BOOL retire(struct KaffeineContext *ctx, struct KaffeinePaintHooks *s,
                   struct PaintBinding *b, BOOL ending)
{
    struct PaintBinding **at;
    ULONG fault;
    BOOL okay;
    if (!owner_driver(ctx, s)) return fail(ctx, "retirement outside native owner");
    LockLayer(0, b->layer);
    __atomic_store_n(&b->enabled, 0, __ATOMIC_RELEASE);
    if (b->layer->BackFill == &b->hook) InstallLayerHook(b->layer, b->baseline);
    /* Every accepted entry owns this semaphore until after it clears active.
     * Unowned entries never publish active and access no storage after their
     * first Permit(). A nonzero count here would violate the native kernel
     * call contract; it cannot arise from either legal or rejected entries. */
    if (__atomic_load_n(&b->active, __ATOMIC_ACQUIRE))
    { UnlockLayer(b->layer); return fail(ctx, "native callback did not quiesce"); }
    for (at = &s->bindings; *at != b; at = &(*at)->next) ;
    *at = b->next; --s->count;
    UnlockLayer(b->layer);
    fault = __atomic_load_n(&b->fault, __ATOMIC_ACQUIRE);
    okay = ending || deliver(ctx, b);
    destroy_binding(b);
    forget_unused_owners(s);
    return fault ? fail(ctx, "retiring recipe failed validation") : okay;
}
BOOL Kaffeine_PaintHookRetireLayer(struct KaffeineContext *ctx, struct KaffeinePaintHooks *s,
                                  struct Layer *l)
{
    struct PaintBinding *b = binding_layer(s, l);
    return !b || retire(ctx, s, b, FALSE);
}
BOOL Kaffeine_PaintHookRetireTask(struct KaffeineContext *ctx, struct KaffeinePaintHooks *s,
                                 ULONG task)
{
    struct PaintBinding *b;
    struct PaintOwner *o;
    BOOL okay = TRUE;
    if (!s) return TRUE;
    if (!owner_driver(ctx, s)) return fail(ctx, "task retirement outside native owner");
    for (;;)
    {
        for (b = s->bindings; b && b->owner_task != task; b = b->next) ;
        if (!b) break;
        {
            struct Layer *layer = b->layer;
            if (!retire(ctx, s, b, TRUE))
            {
            /* A semantic fault must not leave sibling bindings installed.
             * A failed native quiescence barrier, however, retains storage. */
            if (binding_layer(s, layer) == b) return FALSE;
            okay = FALSE;
            }
        }
    }
    o = live_owner(s, task);
    if (o) o->live = FALSE;
    if (task == s->root_task) s->root_retired = TRUE;
    return okay;
}
BOOL Kaffeine_PaintHooksCleanup(struct KaffeineContext *ctx, struct KaffeinePaintHooks **at)
{
    struct KaffeinePaintHooks *s = at ? *at : NULL;
    BOOL okay = TRUE;
    if (!s) return TRUE;
    if (!owner_driver(ctx, s)) return fail(ctx, "cleanup outside native owner");
    while (s->bindings)
    {
        struct PaintBinding *b = s->bindings;
        if (!retire(ctx, s, b, TRUE))
        {
            if (s->bindings == b) return FALSE;
            okay = FALSE;
        }
    }
    SetSignal(0, s->wake_mask); FreeSignal(s->wake_bit);
    *at = NULL; FreeMem(s, sizeof(*s));
    return okay;
}
BOOL Kaffeine_PaintHookMemoryReleasable(const struct KaffeinePaintHooks *s, ULONG a, ULONG n)
{
    const struct PaintBinding *b;
    unsigned i;
    for (b = s ? s->bindings : NULL; b; b = b->next)
    {
        for (i = 0; i < b->plan->dependencies; ++i)
            if (overlaps(a, n, b->plan->dependency[i].address, b->plan->dependency[i].width)) return FALSE;
        for (i = 0; i < b->plan->cells; ++i)
            if (overlaps(a, n, b->plan->cell[i].address, b->plan->cell[i].width)) return FALSE;
    }
    return TRUE;
}
BOOL Kaffeine_PaintHookSignalReleasable(const struct KaffeinePaintHooks *s, ULONG bits)
{
    const struct PaintBinding *b;
    for (b = s ? s->bindings : NULL; b; b = b->next)
        if (b->native_signal_lease & bits) return FALSE;
    return TRUE;
}
ULONG Kaffeine_PaintHookSignalMask(const struct KaffeinePaintHooks *s)
{ return s ? s->wake_mask : 0; }
BOOL Kaffeine_PaintHookPoll(struct KaffeineContext *ctx, struct KaffeinePaintHooks *s)
{
    struct PaintBinding *b;
    if (!s) return TRUE;
    if (!owner_driver(ctx, s)) return fail(ctx, "poll outside native owner");
    SetSignal(0, s->wake_mask);
    for (b = s->bindings; b; b = b->next)
        if (__atomic_load_n(&b->fault, __ATOMIC_ACQUIRE))
            return fail(ctx, "asynchronous recipe validation failed");
    for (b = s->bindings; b; b = b->next) if (!deliver(ctx, b)) return FALSE;
    return TRUE;
}
