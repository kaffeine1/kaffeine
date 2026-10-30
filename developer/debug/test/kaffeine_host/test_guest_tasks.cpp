/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Guest tasks of AddTask() and guest processes of CreateProc() through
    literal Exec and DOS LVOs, with the production scheduler, run loop and
    dispatcher compiled as C. Part one runs mock
    CPUs from a mock backend factory, whose instructions are literal
    register and memory effects, over a recording native Exec. Part two
    runs an original 68k parent and child (guest_tasks_68k.s) on Moira and
    JIT CPUs, and checks explicit JIT faults in a child.
*/
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <vector>

#include <exec/ports.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include "kaffeine_intern.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_callback.h"
#include "kaffeine_sched.h"
#include "kaffeine_messages.h"
#include "kaffeine_jit.h"
#include "kaffeine_process.h"
#include "kaffeine_dos_broker.h"
#include "kaffeine_domain_modules.h"
#include "kaffeine_layers.h"
#include "kaffeine_async.h"

#include "Moira/Moira.cpp"
#include "Moira/MoiraDebugger.cpp"
#include "kaffeine_moira.cpp"

struct KaffeineDosBroker
{
    unsigned id = 1, calls = 0, stops = 0, inits = 0, drops = 0;
    bool fail = false, fail_init = false;
    bool busy = false;
    unsigned drop_refusals = 0, finish_refusals = 0;
};
struct KaffeineDomainModule { unsigned unloads = 0; bool busy = false; };
static KaffeineDosBroker *active_broker;
static std::vector<char> lifetime_order;
static std::vector<ULONG> broker_lvos;
static unsigned native_delays;
static std::function<void()> delay_event;
static std::function<void(KaffeineDosBroker *)> idle_probe;
static std::function<BOOL(KaffeineContext *)> drain_action;
extern "C" BOOL Kaffeine_DosBrokerIsIdle(KaffeineDosBroker *broker)
{
    if (idle_probe) idle_probe(broker);
    return !broker->busy;
}
extern "C" BOOL Kaffeine_DosBrokerCall(KaffeineDosBroker *broker, KaffeineContext *ctx,
    void *cpu, ULONG lvo, KaffeineDosBrokerThunk thunk, IPTR *result, SIPTR *error)
{
    assert(ctx->broker_call && ctx->cpu == cpu);
    if (!lvo && thunk != Kaffeine_ProcessInitGuestDos && broker->drop_refusals)
    {
        --broker->drop_refusals;
        *error = 103;
        return FALSE;
    }
    if (lvo)
    {
        ++broker->calls;
        broker_lvos.push_back(lvo);
    }
    if (broker->fail && lvo) { *error = 103; return FALSE; }
    auto *previous_broker = active_broker;
    auto *previous_owner = ctx->broker_owner;
    active_broker = broker;
    ctx->broker_owner = reinterpret_cast<Task *>(broker);
    *result = thunk(ctx, cpu);
    if (ctx->after_thunk)
    {
        auto after = ctx->after_thunk;
        ctx->after_thunk = nullptr;
        after(ctx);
    }
    active_broker = previous_broker;
    ctx->broker_owner = previous_owner;
    *error = 0;
    return TRUE;
}
extern "C" BOOL Kaffeine_DosBrokerFinish(KaffeineDosBroker *broker, KaffeineContext *ctx,
                                        KaffeineDosBrokerThunk cleanup)
{
    assert(!broker->stops);
    if (broker->finish_refusals) { --broker->finish_refusals; return FALSE; }
    auto *previous_broker = active_broker;
    Task *previous_owner = ctx ? ctx->broker_owner : nullptr;
    active_broker = broker;
    if (ctx) ctx->broker_owner = reinterpret_cast<Task *>(broker);
    if (cleanup) { assert(ctx->broker_call); cleanup(ctx, nullptr); }
    active_broker = previous_broker;
    if (ctx) ctx->broker_owner = previous_owner;
    ++broker->stops;
    lifetime_order.push_back('B');
    return TRUE;
}
extern "C" BOOL Kaffeine_DosBrokerStop(KaffeineDosBroker *broker)
{ return Kaffeine_DosBrokerFinish(broker, nullptr, nullptr); }
extern "C" IPTR Kaffeine_ProcessInitGuestDos(KaffeineContext *ctx, void *cpu)
{
    assert(active_broker && ctx->broker_call && ctx->cpu == cpu && ctx->guest_exec_task &&
           m68k_read32(ctx, ctx->m68k_sysbase + 276) == ctx->guest_exec_task &&
           m68k_read32(ctx, ctx->guest_exec_task + 204) == ctx->m68k_argptr);
    ++active_broker->inits;
    m68k_write32(ctx, ctx->guest_exec_task + 156, 0x4100 + active_broker->id);
    m68k_write32(ctx, ctx->guest_exec_task + 160, 0x4200 + active_broker->id);
    m68k_write32(ctx, ctx->guest_exec_task + 224, 0x4300 + active_broker->id);
    return active_broker->fail_init ? 0 : 1;
}
extern "C" IPTR Kaffeine_ProcessDropGuestDos(KaffeineContext *ctx, void *cpu)
{
    assert(active_broker && ctx->broker_call && !cpu && ctx->guest_exec_task &&
           m68k_read32(ctx, ctx->m68k_sysbase + 276) == ctx->guest_exec_task);
    ++active_broker->drops;
    lifetime_order.push_back('D');
    m68k_write32(ctx, ctx->guest_exec_task + 156, 0);
    m68k_write32(ctx, ctx->guest_exec_task + 160, 0);
    m68k_write32(ctx, ctx->guest_exec_task + 224, 0);
    return 1;
}
extern "C" BOOL Kaffeine_DomainUnload(KaffeineContext *, KaffeineDomainModule *module)
{
    assert(!active_broker && !module->unloads);
    ++module->unloads;
    lifetime_order.push_back('M');
    return !module->busy;
}

static unsigned checks, failures;
static void check(bool ok, const char *what)
{
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
}

extern "C" void Delay(LONG ticks)
{
    assert(ticks == 1);
    ++native_delays;
    assert(native_delays < 100);
    if (delay_event) delay_event();
}

/* ── The native side: one native task runs every guest task ─────── */

struct Task { UBYTE pad[16]; };
static Task native_task;
static ULONG native_alloc, native_recvd, native_wait_mask;
static BYTE native_pri;
static unsigned native_calls, native_waits, allocmems, freemems;
static bool fail_allocmem;
static std::function<void()> native_event;  /* arrives while the native task waits */
static std::vector<ULONG> freed_entries;
static unsigned retired_signals;

/* This scheduler-only fixture creates no DOS lock views. The production
   lock module has its own identity/lifetime tests in test_locks. */
extern "C" BOOL Kaffeine_LockProtected(KaffeineContext *ctx, ULONG, ULONG)
{
    assert(!ctx->locks);
    return FALSE;
}

extern "C" void Kaffeine_AppSignalGone(KaffeineContext *, BYTE)
{
    ++retired_signals;
}

extern "C" APTR AllocMem(ULONG size, ULONG)
{
    if (fail_allocmem) return nullptr;
    ++allocmems;
    return std::calloc(1, size);
}
extern "C" void FreeMem(APTR memory, ULONG)
{
    if (!memory) return;
    ++freemems;
    std::free(memory);
}
extern "C" ULONG SetSignal(ULONG signals, ULONG set)
{
    ULONG old = native_recvd;
    ++native_calls;
    native_recvd = (old & ~set) | (signals & set);
    return old;
}
extern "C" void Signal(Task *task, ULONG set)
{
    ++native_calls;
    assert(task == &native_task);
    native_recvd |= set;
}
extern "C" ULONG Wait(ULONG mask)
{
    ++native_calls;
    ++native_waits;
    native_wait_mask = mask;
    if (!(native_recvd & mask) && native_event)
    {
        auto event = native_event;
        native_event = nullptr;
        event();
    }
    ULONG got = native_recvd & mask;
    if (!got)
    {
        std::printf("FAIL: the native task would wait forever for %08x\n", mask);
        std::exit(1);
    }
    native_recvd &= ~got;
    return got;
}
extern "C" Task *FindTask(CONST_STRPTR name)
{
    ++native_calls;
    return name ? nullptr : &native_task;
}
extern "C" BYTE SetTaskPri(Task *task, LONG pri)
{
    ++native_calls;
    assert(task == &native_task);
    BYTE old = native_pri;
    native_pri = (BYTE)pri;
    return old;
}
extern "C" BYTE AllocSignal(LONG n)
{
    ++native_calls;
    if (n >= 0)
    {
        if (n > 31 || (native_alloc & (1u << n))) return -1;
        native_alloc |= 1u << n;
        return (BYTE)n;
    }
    for (int bit = 31; bit >= 16; --bit)
        if (!(native_alloc & (1u << bit)))
        {
            native_alloc |= 1u << bit;
            native_recvd &= ~(1u << bit);
            return (BYTE)bit;
        }
    return -1;
}
extern "C" void FreeSignal(LONG n)
{
    ++native_calls;
    assert(n >= 0 && n < 32 && (native_alloc & (1u << n)));
    native_alloc &= ~(1u << n);
}
/* The guest heap is not linked: record which MemLists RemTask() frees. */
extern "C" void Kaffeine_HeapFreeEntry(KaffeineContext *, ULONG entry)
{
    lifetime_order.push_back('F');
    freed_entries.push_back(entry);
}

/* CreateProc()'s MemList from a bump heap of 1 KiB slots above the
   messages of these tests, below the root's stack. heap_force puts the
   next first member there instead, as a corrupt heap would. */
static constexpr ULONG HEAP_START = 0x33000, HEAP_END = 0x3F000;
static ULONG heap_next, heap_force;
static unsigned heap_lists;
static bool fail_heap;
extern "C" ULONG Kaffeine_HeapAllocList(KaffeineContext *ctx, const ULONG *spec, UWORD count)
{
    ULONG need = 0, entry, i;

    for (i = 0; i <= count; ++i)
        need += ((i ? spec[2 * i - 1] : 16 + 8 * count) + 0x3FF) & ~0x3FFu;
    if (fail_heap || heap_next + need > HEAP_END) return 0x80000001u;
    ++heap_lists;
    entry = heap_next;
    heap_next += (16 + 8 * count + 0x3FF) & ~0x3FFu;
    std::memset(ctx->mem + entry, 0, 16 + 8 * count);
    m68k_write16(ctx, entry + 14, count);
    for (i = 0; i < count; ++i)
    {
        ULONG length = spec[2 * i + 1], member = heap_next;

        if (!i && heap_force)
        {
            member = heap_force;
            heap_force = 0;
        }
        else
        {
            heap_next += (length + 0x3FF) & ~0x3FFu;
            std::memset(ctx->mem + member, 0, length);
        }
        m68k_write32(ctx, entry + 16 + 8 * i, member);
        m68k_write32(ctx, entry + 20 + 8 * i, length);
    }
    return entry;
}

/* ── Mock CPUs ───────────────────────────────────────────────────── */

struct Mock
{
    KaffeineCPU handle;
    ULONG d[8], a[8];
    UWORD sr;
    ULONG pc;
    unsigned id;
    bool executing, zombie;
};

static ULONG m_get_d(void *s, int n) { return static_cast<Mock *>(s)->d[n]; }
static ULONG m_get_a(void *s, int n) { return static_cast<Mock *>(s)->a[n]; }
static void m_set_d(void *s, int n, ULONG v) { static_cast<Mock *>(s)->d[n] = v; }
static void m_set_a(void *s, int n, ULONG v) { static_cast<Mock *>(s)->a[n] = v; }
static UWORD m_get_sr(void *s) { return static_cast<Mock *>(s)->sr; }
static void m_set_sr(void *s, UWORD v) { static_cast<Mock *>(s)->sr = v; }
static ULONG m_get_pc(void *s) { return static_cast<Mock *>(s)->pc; }
static void m_set_pc(void *s, ULONG v) { static_cast<Mock *>(s)->pc = v; }
static void m_execute(void *s);
static const KaffeineCPUOps mock_ops =
    { m_get_d, m_get_a, m_set_d, m_set_a, m_get_sr, m_set_sr, m_get_pc, m_set_pc, m_execute, nullptr };

static constexpr ULONG ARENA = 0x40000, SYSBASE = 0x8000, ROOT = 0x10800, ROOT_NAME = 0x10A00;
static constexpr ULONG SHARED = 0xA000, NAMES = 0xB000, ROOT_CODE = 0x11000, CODE = 0x12000;
static constexpr ULONG STACK_TOP = 0x40000, STACK_BOTTOM = 0x3F000, SENTINEL = 0x160;
static constexpr ULONG TASK_READY = SYSBASE + 406, TASK_WAIT = SYSBASE + 420;
static constexpr ULONG TDNEST = SYSBASE + 295;
static constexpr ULONG DOSBASE = 0x7000;
static constexpr ULONG TCB(unsigned i) { return 0x20000 + 0x100 * i; }
/* A seglist of one segment whose code is CODE + 0x1000 * i */
static constexpr ULONG SEG(unsigned i) { return 0x12000 + 0x1000 * i - 4; }
static constexpr ULONG SEGLIST(unsigned i) { return SEG(i) >> 2; }
static constexpr ULONG LOWER(unsigned i) { return 0x28000 + 0x400 * i; }
static constexpr ULONG UPPER(unsigned i) { return LOWER(i) + 0x400; }
static constexpr size_t SLICE_LIMIT = 1000000;

static IPTR alloc_signal(KaffeineContext *, void *);
static IPTR free_signal(KaffeineContext *, void *);
static IPTR port_wait(KaffeineContext *, void *);
static IPTR private_io_wait(KaffeineContext *, void *);
/* Native fallback of the production dispatcher when the last child ended. */
static IPTR exec_wait(KaffeineContext *c, void *p)
{ return c->sched ? Kaffeine_TaskWait(c,p) : (IPTR)Wait(Kaffeine_GetD(p,0)); }
static IPTR guest_put(KaffeineContext *c, void *p)
{ return Kaffeine_GuestPutMsg(c, Kaffeine_GetA(p,0), Kaffeine_GetA(p,1), Kaffeine_GetD(p,0)); }
static IPTR guest_get(KaffeineContext *c, void *p)
{ return Kaffeine_GuestGetMsg(c, Kaffeine_GetA(p,0), FALSE); }
static IPTR guest_wait(KaffeineContext *c, void *p)
{ return Kaffeine_GuestGetMsg(c, Kaffeine_GetA(p,0), TRUE); }
static IPTR guest_reply(KaffeineContext *c, void *p)
{ return Kaffeine_GuestReplyMsg(c, Kaffeine_GetA(p,1)); }
static const M68KThunkEntry exec_table[] =
{
    { 120, Kaffeine_TaskDisable }, { 126, Kaffeine_TaskEnable },
    { 132, Kaffeine_TaskForbid }, { 138, Kaffeine_TaskPermit },
    { 282, Kaffeine_TaskAdd }, { 288, Kaffeine_TaskRemove }, { 294, Kaffeine_TaskFindTask },
    { 300, Kaffeine_TaskSetPri }, { 306, Kaffeine_TaskSetSignal }, { 318, exec_wait },
    { 324, Kaffeine_TaskSignal }, { 330, alloc_signal }, { 336, free_signal },
    { 366, guest_put }, { 372, guest_get }, { 378, guest_reply },
    { 474, private_io_wait }, { 384, port_wait }, { 390, guest_wait }
};

/* dos.library: CreateProc() as kaffeine_dos_CreateProc() calls it, and two
   calls that would act for the native process, which count when reached */
static unsigned dos_calls, dos_exits;
static LONG dos_ioerr;
static IPTR dos_output(KaffeineContext *, void *)
{ ++dos_calls; return active_broker ? 0x1000 + active_broker->id : 0x1234; }
static IPTR dos_cli_probe(KaffeineContext *ctx, void *) { return ctx->m68k_cli; }
static std::function<IPTR(KaffeineContext *, void *)> typed_create_thunk;
static IPTR dos_typed_create(KaffeineContext *ctx, void *cpu)
{ return typed_create_thunk ? typed_create_thunk(ctx, cpu) : 0; }
static IPTR dos_callback_probe(KaffeineContext *ctx, void *cpu)
{
    KaffeineGuestCall call = {};
    return Kaffeine_CallGuest(ctx, cpu, CODE + 0x2000, &call) ? 1 : 20;
}
static IPTR dos_exit(KaffeineContext *c, void *p)
{
    ++dos_exits;
    c->running = FALSE;
    c->exit_code = (LONG)Kaffeine_GetD(p, 1);
    return (IPTR)c->exit_code;
}
static IPTR dos_create_proc(KaffeineContext *c, void *p)
{
    LONG ioerr = ERROR_NOT_IMPLEMENTED;
    IPTR port = c->task_ops->create_proc(c, p, &ioerr);
    if (!port) dos_ioerr = ioerr;
    return port;
}
static const M68KThunkEntry dos_table[] =
{
    { 60, dos_output }, { 72, dos_callback_probe }, { 138, dos_create_proc }, { 144, dos_exit },
    { 492, dos_cli_probe }, { 498, dos_typed_create }
};

static KaffeineCPU *mock_create(KaffeineContext *);
static void mock_destroy(KaffeineContext *, KaffeineCPU *);
static const KaffeineCPUFactory mock_factory = { mock_create, mock_destroy };

struct World;
static World *world;

struct World
{
    std::vector<UBYTE> mem = std::vector<UBYTE>(ARENA, 0);
    KaffeineContext ctx = {};
    std::map<ULONG, std::function<void(Mock &)>> code;
    Mock root = {};
    std::vector<unsigned> trace;            /* the CPU of each slice */
    std::vector<Mock *> mocks;
    unsigned created = 0, destroyed = 0, in_flight = 0, faults = 0, port_calls = 0;
    unsigned port_messages = 0;
    UBYTE port_flags = 0, port_sigbit = 20;
    bool fail_create = false;
    ULONG seen[64] = {};

    World()
    {
        world = this;
        native_alloc = 0xFFFF; native_recvd = 0; native_pri = 0; native_wait_mask = 0;
        native_calls = native_waits = allocmems = freemems = 0;
        fail_allocmem = false; native_event = nullptr; freed_entries.clear(); retired_signals=0;
        heap_next = HEAP_START; heap_force = 0; heap_lists = 0; fail_heap = false;
        dos_calls = dos_exits = 0; dos_ioerr = 0;
        active_broker = nullptr; lifetime_order.clear(); broker_lvos.clear();
        typed_create_thunk = nullptr;
        native_delays = 0; delay_event = nullptr; idle_probe = nullptr;
        drain_action = nullptr;
        ctx.mem = mem.data(); ctx.mem_size = ARENA;
        ctx.m68k_sysbase = SYSBASE; ctx.guest_exec_task = ROOT;
        ctx.stack_top = STACK_TOP; ctx.stack_bottom = STACK_BOTTOM;
        ctx.task_ops = &Kaffeine_GuestTaskOps; ctx.cpu_factory = &mock_factory;
        ctx.num_libs = 1;
        auto &lib = ctx.libs[0];
        lib.m68k_addr = SYSBASE; lib.jt_start = SYSBASE - 6 * 512;
        std::strcpy(lib.name, "exec.library");
        lib.thunks = exec_table; lib.num_thunks = sizeof(exec_table) / sizeof(exec_table[0]);
        for (ULONG at = lib.jt_start; at < SYSBASE; at += 2)
            m68k_write16(&ctx, at, 0xA000);
        /* The guest ExecBase: switches allowed, classic TaskSigAlloc,
           the root as ThisTask and empty task lists. */
        mem[SYSBASE + 294] = mem[SYSBASE + 295] = 0xFF;
        put(SYSBASE + 316, 0xFFFF);
        put(SYSBASE + 276, ROOT);
        for (ULONG list : { TASK_READY, TASK_WAIT })
        {
            put(list, list + 4); put(list + 4, 0); put(list + 8, list);
            mem[list + 12] = 1;
        }
        std::strcpy(reinterpret_cast<char *>(mem.data() + ROOT_NAME), "root");
        put(ROOT + 10, ROOT_NAME);
        mem[ROOT + 8] = 13;
        code[SENTINEL] = [](Mock &) { world->ctx.running = FALSE; };
        root.handle = { &mock_ops, &root };
        root.sr = 0x2700; root.pc = ROOT_CODE; root.a[6] = SYSBASE;
        root.a[7] = STACK_TOP - 4;
        put(STACK_TOP - 4, SENTINEL);
    }
    ~World()
    {
        for (Mock *m : mocks) delete m;
        native_event = nullptr;
        world = nullptr;
    }
    void put(ULONG addr, ULONG value) { m68k_write32(&ctx, addr, value); }
    ULONG at(ULONG addr) { return m68k_read32(&ctx, addr); }
    void child(unsigned i, int pri, const char *name = nullptr)
    {
        mem[TCB(i) + 9] = (UBYTE)pri;
        if (name)
        {
            std::strcpy(reinterpret_cast<char *>(mem.data() + NAMES + 32 * i), name);
            put(TCB(i) + 10, NAMES + 32 * i);
        }
        put(TCB(i) + 58, LOWER(i));
        put(TCB(i) + 62, UPPER(i));
    }
    /* dos.library, marked as Kaffeine_SetupLibBase() marks it */
    void dos()
    {
        auto &lib = ctx.libs[1];
        ctx.num_libs = 2;
        lib.m68k_addr = DOSBASE; lib.jt_start = DOSBASE - 6 * 256;
        std::strcpy(lib.name, "dos.library");
        lib.dos = TRUE;
        lib.thunks = dos_table; lib.num_thunks = sizeof(dos_table) / sizeof(dos_table[0]);
    }
    void name(ULONG at, const char *text)
    {
        std::strcpy(reinterpret_cast<char *>(mem.data() + at), text);
    }
    LONG run() { return Kaffeine_RunCPU(&ctx, &root.handle); }
    bool lists_empty()
    {
        return at(TASK_READY) == TASK_READY + 4 && at(TASK_READY + 8) == TASK_READY &&
               at(TASK_WAIT) == TASK_WAIT + 4 && at(TASK_WAIT + 8) == TASK_WAIT;
    }
    /* Runs of consecutive slices of one CPU: (cpu, length) */
    std::vector<std::pair<unsigned, size_t>> runs()
    {
        std::vector<std::pair<unsigned, size_t>> out;
        for (unsigned id : trace)
            if (!out.empty() && out.back().first == id) ++out.back().second;
            else out.push_back({ id, 1 });
        return out;
    }
};

static void m_execute(void *s)
{
    Mock &m = *static_cast<Mock *>(s);
    auto it = world->code.find(m.pc);

    assert(!m.zombie);
    world->trace.push_back(m.id);
    if (world->trace.size() > SLICE_LIMIT)
    {
        std::printf("FAIL: the guest did not finish\n");
        ++failures;
        world->ctx.running = FALSE;
        return;
    }
    m.executing = true;
    if (it == world->code.end())
    {
        /* An instruction the backend cannot execute: as the Moira adapter. */
        ++world->faults;
        world->ctx.running = FALSE;
        world->ctx.exit_code = 20;
        m.d[0] = 20;
    }
    else
    {
        auto op = it->second;
        op(m);
    }
    m.executing = false;
}

static KaffeineCPU *mock_create(KaffeineContext *)
{
    if (world->fail_create) return nullptr;
    Mock *m = new Mock();
    m->handle = { &mock_ops, m };
    /* What a reset leaves: the scheduler must set every register. */
    for (unsigned i = 0; i < 8; ++i)
        m->d[i] = m->a[i] = 0xBAD00000 + i;
    m->sr = 0x2700;
    m->pc = 0xBAD0BAD0;
    m->id = ++world->created;
    world->mocks.push_back(m);
    return &m->handle;
}

static void mock_destroy(KaffeineContext *, KaffeineCPU *handle)
{
    Mock *m = static_cast<Mock *>(handle->backend_state);
    ++world->destroyed;
    lifetime_order.push_back('C');
    if (m->executing)
    {
        ++world->in_flight;
        m->zombie = true;
        return;
    }
    world->mocks.erase(std::find(world->mocks.begin(), world->mocks.end(), m));
    delete m;
}

/* The order of kaffeine_exec_io.c's AllocSignal() and FreeSignal(), whose
   devices this test does not link. */
static IPTR alloc_signal(KaffeineContext *ctx, void *cpu)
{
    BYTE got = AllocSignal((BYTE)Kaffeine_GetD(cpu, 0));
    if (got >= 0)
    {
        ctx->guest_signals |= 1u << got;
        if (ctx->sched) ctx->task_ops->signal_allocated(ctx, got);
    }
    return (IPTR)(LONG)got;
}
static IPTR free_signal(KaffeineContext *ctx, void *cpu)
{
    BYTE sig = (BYTE)Kaffeine_GetD(cpu, 0);
    if (sig < 0) return 0;
    if (ctx->sched && !ctx->task_ops->signal_freeable(ctx, sig)) return 0;
    if (sig > 31 || !(ctx->guest_signals & (1u << sig))) return 0;
    if (ctx->idcmp_signal_gone) ctx->idcmp_signal_gone(ctx, sig);
    ctx->guest_signals &= ~(1u << sig);
    FreeSignal(sig);
    return 0;
}

/* A native port with signal bit 20, waited for as kaffeine_window.c's
   IDCMP WaitPort() does: with guest tasks an empty port blocks only the
   caller. Its list holds a node while a message is there. */
static constexpr ULONG PORT_BIT = 1u << 20, PORT_MESSAGE = 0x00ABCD00;
static ::MsgPort native_port;
static ::Node native_message;
static IPTR port_wait(KaffeineContext *ctx, void *cpu)
{
    ::List &list = native_port.mp_MsgList;
    IPTR blocked;

    ++world->port_calls;
    native_port.mp_Flags = world->port_flags;
    native_port.mp_SigBit = world->port_sigbit;
    list.lh_Head = world->port_messages ? &native_message : reinterpret_cast< ::Node *>(&list.lh_Tail);
    list.lh_Tail = nullptr;
    list.lh_TailPred = world->port_messages ? &native_message : reinterpret_cast< ::Node *>(&list);
    if (ctx->sched && ctx->task_ops->block_port(ctx, cpu, &native_port, port_wait, &blocked))
        return blocked;
    if (!world->port_messages)
    {
        std::printf("FAIL: an empty port would block the native task\n");
        ++failures;
        return 0;
    }
    --world->port_messages;
    return PORT_MESSAGE;
}

/* Instructions, two bytes each, as literal effects */
struct Program
{
    ULONG pc;
    explicit Program(ULONG start) : pc(start) {}
    Program &raw(std::function<void(Mock &)> op) { world->code[pc] = std::move(op); pc += 2; return *this; }
    Program &op(std::function<void(Mock &)> effect)
    {
        return raw([effect](Mock &m) { effect(m); m.pc += 2; });
    }
    Program &d(int r, ULONG v) { return op([r, v](Mock &m) { m.d[r] = v; }); }
    Program &a(int r, ULONG v) { return op([r, v](Mock &m) { m.a[r] = v; }); }
    Program &store(ULONG addr, int r) { return op([addr, r](Mock &m) { world->put(addr, m.d[r]); }); }
    Program &mark(unsigned slot) { return op([slot](Mock &) { world->seen[slot] = (ULONG)world->trace.size(); }); }
    Program &see(unsigned slot, std::function<ULONG()> what) { return op([slot, what](Mock &) { world->seen[slot] = what(); }); }
    /* JSR -lvo(A6) to an A-line slot: the backend dispatches and returns. */
    Program &call(UWORD lvo)
    {
        return raw([lvo](Mock &m) {
            m.a[7] -= 4;
            world->put(m.a[7], m.pc + 2);
            KaffeineDispatchResult result = {};
            bool dispatched = Kaffeine_DispatchLibraryCall(&world->ctx, &m.handle, m.a[6] - lvo, &result);
            assert(dispatched);
            (void)dispatched;
            m.pc = result.return_pc;
        });
    }
    Program &add_task(ULONG tcb, ULONG initial, ULONG final = 0)
    {
        return a(1, tcb).a(2, initial).a(3, final).call(282);
    }
    /* JSR -lvo(A6) with A6 dos.library, then SysBase again */
    Program &dos(UWORD lvo) { return a(6, DOSBASE).call(lvo).a(6, SYSBASE); }
    Program &create_proc(ULONG name, LONG pri, ULONG seglist, LONG stack)
    {
        return op([=](Mock &m) {
            m.d[1] = name; m.d[2] = (ULONG)pri; m.d[3] = seglist; m.d[4] = (ULONG)stack;
        }).dos(138);
    }
    /* A<r> = the port of CreateProc() that the root stored at SHARED */
    Program &to_port(int r = 0) { return op([r](Mock &m) { m.a[r] = world->at(SHARED); }); }
    /* A<r> = pr_MsgPort of ThisTask, in a child Process */
    Program &own_port(int r = 0)
    {
        return op([r](Mock &m) { m.a[r] = world->at(SYSBASE + 276) + 92; });
    }
    Program &signal(ULONG task, ULONG set) { return a(1, task).d(0, set).call(324); }
    Program &wait(ULONG set) { return d(0, set).call(318); }
    Program &rts() { return raw([](Mock &m) { m.pc = world->at(m.a[7]); m.a[7] += 4; }); }
    Program &jump(ULONG to) { return raw([to](Mock &m) { m.pc = to; }); }
    Program &loop(int r, ULONG count)
    {
        ULONG here = pc;
        return raw([here, r, count](Mock &m) { m.pc = ++m.d[r] < count ? here : here + 2; });
    }
};

/* ── Mock scheduler tests ────────────────────────────────────────── */

static void test_no_children()
{
    World w;
    Program(ROOT_CODE).d(2, 5).op([](Mock &m) { m.d[0] = m.d[2] * 3; })
        .call(132).see(0, [] { return (ULONG)world->mem[TDNEST]; })
        .call(138).see(1, [] { return (ULONG)world->mem[TDNEST]; }).d(0, 15).rts();
    auto before = w.mem;
    LONG result = w.run();
    check(result == 15 && w.root.d[0] == 15, "a program without AddTask() returns its D0");
    check(w.seen[0] == 0 && w.seen[1] == 0xFF,
          "Forbid() and Permit() count in the guest ExecBase TDNestCnt");
    before[STACK_TOP - 8] = w.mem[STACK_TOP - 8]; before[STACK_TOP - 7] = w.mem[STACK_TOP - 7];
    before[STACK_TOP - 6] = w.mem[STACK_TOP - 6]; before[STACK_TOP - 5] = w.mem[STACK_TOP - 5];
    check(!w.ctx.sched && !w.created && !allocmems && !native_calls && w.mem == before,
          "without children: no scheduler, CPU, allocation, native Exec call or other guest write");

    World refused;
    refused.ctx.cpu_factory = nullptr;
    refused.child(0, 0);
    Program(ROOT_CODE).add_task(TCB(0), CODE).d(0, 99).rts();
    auto task = std::vector<UBYTE>(refused.mem.begin() + TCB(0), refused.mem.begin() + TCB(0) + 0x100);
    result = refused.run();
    check(result == 20 && refused.root.d[0] == 20 && !refused.ctx.running &&
          refused.ctx.exit_code == 20, "AddTask() without a backend CPU factory stops with 20");
    check(std::equal(task.begin(), task.end(), refused.mem.begin() + TCB(0)) &&
          !refused.created && !allocmems && !refused.ctx.sched,
          "the refused AddTask() changes no Task byte and makes no scheduler");
}

static void test_root_priority()
{
    World w;
    Program(ROOT_CODE).a(1,0).d(0,3).call(300).store(SHARED,0)
        .a(1,ROOT).d(0,5).call(300).store(SHARED+4,0).d(0,0).rts();
    check(w.run()==0 && w.at(SHARED)==0 && w.at(SHARED+4)==3 &&
          native_pri==5 && w.mem[ROOT+9]==5 && !w.ctx.sched,
          "SetTaskPri validates and updates the current root before AddTask too");
    World bad;
    Program(ROOT_CODE).a(1,TCB(0)).d(0,7).call(300).rts();
    check(bad.run()==20 && native_pri==0 && !native_calls,
          "SetTaskPri refuses an unregistered guest Task before any native call");
}

static void test_add_wait_return()
{
    World w;
    w.child(0, 0, "kid");
    static Mock first;
    Program(ROOT_CODE)
        .add_task(TCB(0), CODE).store(SHARED, 0)
        .see(0, [] { return (ULONG)world->mem[TCB(0) + 15]; })
        .see(1, [] { return (ULONG)(world->at(TASK_READY) == TCB(0) && world->at(TCB(0)) == TASK_READY + 4 &&
                                   world->at(TCB(0) + 4) == TASK_READY && world->at(TASK_READY + 8) == TCB(0) &&
                                   world->at(TASK_WAIT) == TASK_WAIT + 4); })
        .see(2, [] { return (ULONG)(world->mem[TCB(0) + 8] == 1 && world->mem[TCB(0) + 16] == 0xFF &&
                                   world->mem[TCB(0) + 17] == 0xFF && world->at(TCB(0) + 18) == 0xFFFF &&
                                   world->at(TCB(0) + 54) == UPPER(0) - 4 &&
                                   world->at(TCB(0) + 74) == TCB(0) + 78 && world->at(TCB(0) + 78) == 0 &&
                                   world->at(TCB(0) + 82) == TCB(0) + 74 &&
                                   world->at(SYSBASE + 276) == ROOT); })
        .a(1, NAMES).call(294).store(SHARED + 0x20, 0)
        .wait(0x1000).store(SHARED + 4, 0)
        .see(3, [] { return world->at(SYSBASE + 276); })
        .op([](Mock &m) { m.d[0] = world->at(SHARED + 8); })
        .rts();
    Program(CODE)
        .op([](Mock &m) { first = m; first.d[0] = world->at(m.a[7]); })
        .a(6, SYSBASE).a(1, 0).call(294).store(SHARED + 12, 0)
        .d(2, 0x1234).store(SHARED + 8, 2)
        .signal(ROOT, 0x1000)
        .rts();
    LONG result = w.run();
    check(result == 0x1234, "the root returns what the child computed");
    check(w.at(SHARED) == TCB(0), "AddTask() returns the task");
    check(w.seen[0] == 3 && w.seen[1] && w.seen[2],
          "the added Task is TS_READY, in the guest TaskReady list, with NewAddTask() fields");
    check(w.at(SHARED + 0x20) == TCB(0), "FindTask() of the child's name returns its Task");
    bool zero = first.a[0] == 0 && first.sr == 0x2700 && first.pc == CODE;
    for (unsigned i = 1; i < 8; ++i) zero = zero && first.d[i] == 0;
    for (unsigned i = 0; i < 7; ++i) zero = zero && first.a[i] == 0;
    check(zero && first.a[7] == UPPER(0) - 4 && first.d[0] == KAFFEINE_TASK_EXIT_ADDR,
          "the child starts at initialPC with zero registers, supervisor SR and the end address on its stack");
    check(w.at(SHARED + 12) == TCB(0), "FindTask(NULL) in the child is the child");
    check(w.at(SHARED + 4) == 0x1000 && w.seen[3] == ROOT,
          "the root resumes from Wait() with its signal in D0 and as ThisTask");
    check(w.created == 1 && w.destroyed == 1 && !w.in_flight && w.mocks.empty(),
          "the child's return ends it, and its CPU goes after its last slice");
    check(!w.ctx.sched && allocmems == 1 && freemems == 1 && w.lists_empty() &&
          w.at(SYSBASE + 276) == ROOT && w.mem[TCB(0) + 15] == 6,
          "the root alone again: no scheduler, empty lists, the child TS_REMOVED");
    check(!native_recvd && !native_waits, "the native signal was taken and no native Wait() was needed");
}

static void test_signals()
{
    World w;
    w.child(0, 0);
    Program(ROOT_CODE)
        .add_task(TCB(0), CODE)
        .wait(0x1000).store(SHARED + 4, 0)
        .d(0, 31).call(336).see(5, [] { return native_alloc; })
        .signal(TCB(0), 0x4000)
        .see(0, [] { return (ULONG)world->mem[TCB(0) + 15]; })
        .see(1, [] { return world->at(TCB(0) + 26); })
        .see(2, [] { return world->at(TCB(0) + 22); })
        .see(3, [] { return world->at(TASK_WAIT); })
        .signal(TCB(0), 0x80000000)
        .see(4, [] { return (ULONG)world->mem[TCB(0) + 15]; })
        .wait(0x1000).store(SHARED + 28, 0)
        .rts();
    Program(CODE)
        .a(6, SYSBASE).d(0, 0xFFFFFFFF).call(330).store(SHARED + 16, 0)
        .see(6, [] { return world->at(TCB(0) + 18); })
        .op([](Mock &m) { m.d[3] = m.d[0]; m.d[4] = 1u << m.d[0]; })
        .signal(ROOT, 0x1000)
        .op([](Mock &m) { m.d[0] = m.d[4]; }).call(318).store(SHARED + 36, 0)
        .d(0, 0).d(1, 0x4000).call(306).store(SHARED + 40, 0)
        .see(7, [] { return world->at(TCB(0) + 26) | (ULONG)world->mem[TCB(0) + 15] << 24; })
        .signal(ROOT, 0x1000)
        .op([](Mock &m) { m.d[0] = m.d[3]; }).call(336)
        .rts();
    LONG result = w.run();
    check(result == 0x1000 && w.at(SHARED + 4) == 0x1000 && w.at(SHARED + 28) == 0x1000,
          "the root's Wait() results are its native signals");
    check(w.at(SHARED + 16) == 31 && w.seen[6] == 0x8000FFFF,
          "a child's AllocSignal() takes a free native bit into its tc_SigAlloc");
    check((w.seen[5] & 0x80000000u) != 0, "the root cannot free a child's signal");
    check(w.seen[0] == 4 && w.seen[1] == 0x4000 && w.seen[2] == 0x80000000 && w.seen[3] == TCB(0),
          "a signal outside the wait set leaves the child TS_WAIT in TaskWait, in tc_SigRecvd");
    check(w.seen[4] == 3, "a signal in the wait set makes the child TS_READY");
    check(w.at(SHARED + 36) == 0x80000000,
          "the child's Wait() D0 is written before it resumes, without the other bit");
    check(w.at(SHARED + 40) == 0x4000 && w.seen[7] == (2u << 24),
          "SetSignal() of the child returns and clears its own received bit, TS_RUN");
    check(native_alloc == 0xFFFF && !w.ctx.guest_signals && !native_recvd,
          "the child freed its native bit; no native signal is left");
}

static void test_round_robin()
{
    World w;
    w.child(0, 0); w.child(1, 0);
    Program(ROOT_CODE)
        .add_task(TCB(0), CODE).add_task(TCB(1), CODE + 0x1000)
        .wait(0x1000).wait(0x2000).d(0, 0).rts();
    Program(CODE).a(6, SYSBASE).loop(2, 10000).signal(ROOT, 0x1000).rts();
    Program(CODE + 0x1000).a(6, SYSBASE).loop(2, 10000).signal(ROOT, 0x2000).rts();
    LONG result = w.run();
    auto runs = w.runs();
    check(result == 0 && w.destroyed == 2 && !w.ctx.sched, "both busy children finish");
    check(runs.size() == 9 &&
          runs[1].first == 1 && runs[1].second == KAFFEINE_TASK_QUANTUM &&
          runs[2].first == 2 && runs[2].second == KAFFEINE_TASK_QUANTUM &&
          runs[3].first == 1 && runs[3].second == KAFFEINE_TASK_QUANTUM &&
          runs[4].first == 2 && runs[4].second == KAFFEINE_TASK_QUANTUM &&
          runs[5].first == 1 && runs[6].first == 0 && runs[7].first == 2 && runs[8].first == 0,
          "equal priorities take turns of exactly one quantum; the longest waiting goes next");
}

static void test_priorities()
{
    World w;
    w.child(0, -5); w.child(1, 5);
    Program(ROOT_CODE)
        .add_task(TCB(0), CODE).mark(1)
        .add_task(TCB(1), CODE + 0x1000).mark(2)
        .wait(0x1000).mark(3).wait(0x2000).mark(5)
        .a(1, ROOT).d(0, 3).call(300).store(SHARED, 0)
        .rts();
    Program(CODE).a(6, SYSBASE).signal(ROOT, 0x1000).mark(4).signal(ROOT, 0x2000).rts();
    Program(CODE + 0x1000).mark(6).rts();
    w.run();
    check(w.seen[6] == w.seen[1] + 5 && w.seen[6] < w.seen[2],
          "a higher-priority child runs in the slice after its AddTask()");
    check(w.seen[1] < w.seen[4] && w.seen[3] < w.seen[4] && w.seen[4] < w.seen[5],
          "a lower-priority child runs only while the root waits, and the root preempts it");
    check(w.at(SHARED) == 0 && native_pri == 3, "SetTaskPri() of the root changes its native task");
    check(w.created == 2 && w.destroyed == 2 && !w.ctx.sched,
          "the child that did not finish ends with the program");

    World raise;
    raise.child(0, -5);
    Program(ROOT_CODE)
        .add_task(TCB(0), CODE).mark(1)
        .a(1, TCB(0)).d(0, 5).call(300).store(SHARED, 0).mark(2).rts();
    Program(CODE).mark(3).rts();
    raise.run();
    check(raise.at(SHARED) == 0xFFFFFFFB && raise.mem[TCB(0) + 9] == 5 &&
          raise.seen[1] < raise.seen[3] && raise.seen[3] < raise.seen[2],
          "SetTaskPri() of a child returns its old priority and lets it preempt");
}

static void test_forbid()
{
    World w;
    w.child(0, 5); w.child(1, 5); w.child(2, 5);
    Program(ROOT_CODE)
        .call(132).add_task(TCB(0), CODE).mark(1).mark(2).call(138).mark(3)
        .op([](Mock &) { ++world->mem[TDNEST]; })
        .add_task(TCB(1), CODE + 0x1000).mark(4)
        .op([](Mock &) { --world->mem[TDNEST]; }).mark(5)
        .call(132).add_task(TCB(2), CODE + 0x2000).wait(0x1000)
        .see(10, [] { return (ULONG)world->mem[TDNEST]; })
        .call(138).see(11, [] { return (ULONG)world->mem[TDNEST]; })
        .rts();
    Program(CODE).mark(6).see(12, [] { return (ULONG)world->mem[TDNEST]; }).rts();
    Program(CODE + 0x1000).mark(7).rts();
    Program(CODE + 0x2000).a(6, SYSBASE).see(13, [] { return (ULONG)world->mem[TDNEST]; })
        .signal(ROOT, 0x1000).rts();
    w.run();
    check(w.seen[1] + 1 == w.seen[2] && w.seen[6] == w.seen[2] + 2 && w.seen[6] < w.seen[3],
          "Forbid() keeps a higher-priority child out until the slice after Permit()");
    check(w.seen[12] == 0xFF, "the child runs with its own TDNestCnt");
    check(w.seen[7] == w.seen[4] + 2 && w.seen[7] < w.seen[5],
          "an inline FORBID of TDNestCnt in the guest ExecBase is respected too");
    check(w.seen[13] == 0xFF && w.seen[10] == 0 && w.seen[11] == 0xFF,
          "Wait() breaks Forbid(), and the waiter gets its nesting back");
    check(w.destroyed == 3 && !w.in_flight && !w.ctx.sched, "all three children ended");
}

static void test_disable()
{
    World w;
    w.child(0, 5);
    Program(ROOT_CODE)
        .call(120).add_task(TCB(0), CODE).mark(1).mark(2)
        .call(126).mark(3).wait(0x1000).rts();
    Program(CODE).mark(4).a(6,SYSBASE)
        .see(5, [] { return (ULONG)world->mem[TDNEST - 1]; })
        .a(1, 0).d(0, 6).call(300)
        .signal(ROOT, 0x1000).rts();
    w.run();
    check(w.seen[1] + 1 == w.seen[2] && w.seen[4] == w.seen[2] + 2 &&
          w.seen[4] < w.seen[3],
          "guest Disable keeps children out until Enable without native interrupt calls");
    check(w.seen[5] == 0xff && w.mem[TCB(0) + 9] == 6,
          "child has independent interrupt nesting and SetTaskPri(NULL) selects itself");
}

static void guest_port(World &w, ULONG port, ULONG owner, UBYTE bit)
{
    std::memset(w.ctx.mem + port, 0, 34);
    w.mem[port+8]=4; w.mem[port+15]=bit;
    m68k_write32(&w.ctx,port+16,owner);
    m68k_write32(&w.ctx,port+20,port+24);
    m68k_write32(&w.ctx,port+28,port+20);
}

static void test_guest_messages()
{
    constexpr ULONG rp=0x31000, cp=0x31100, msg=0x31200;
    World w;
    w.child(0,5);
    guest_port(w,rp,ROOT,20);
    m68k_write32(&w.ctx,msg+14,rp);
    m68k_write16(&w.ctx,msg+18,20);
    Program(ROOT_CODE).d(0,20).call(330).add_task(TCB(0),CODE).wait(0x1000)
        .a(0,cp).a(1,msg).call(366)
        .a(0,rp).call(390).store(SHARED,0)
        .a(0,rp).call(372).store(SHARED+4,0)
        .d(0,20).call(336).d(0,0).rts();
    Program(CODE).a(6,SYSBASE).d(0,21).call(330)
        .op([&w](Mock &) { guest_port(w,cp,TCB(0),21); })
        .signal(ROOT,0x1000)
        .a(0,cp).call(390).store(SHARED+8,0)
        .a(0,cp).call(372).store(SHARED+12,0)
        .a(1,msg).call(378).d(0,21).call(336).rts();
    LONG result=w.run();
    check(result==0 && w.at(SHARED)==msg && w.at(SHARED+4)==msg &&
          w.at(SHARED+8)==msg && w.at(SHARED+12)==msg,
          "PutMsg and ReplyMsg wake the correct waiting guest task, and WaitPort peeks");
    check(w.mem[msg+8]==7 && w.at(rp+20)==rp+24 && w.at(cp+20)==cp+24,
          "both guest queues drain and the replied message keeps its type");
    check(!native_waits && !w.ctx.sched && !w.ctx.guest_signals && native_alloc==0xffff,
          "guest message exchange needs no blocking native Wait and returns allocated bits");
}

static void test_process_dos_signal()
{
    constexpr ULONG process_port=ROOT+92, other=0x31000, msg=0x31200;
    World w;
    w.child(0,-1);
    guest_port(w,process_port,ROOT,SIGB_DOS);
    m68k_write16(&w.ctx,msg+18,20);
    Program(ROOT_CODE).add_task(TCB(0),CODE)
        .op([&w](Mock &) {
            auto *ops=w.ctx.task_ops;
            /* SIGB_DOS stays the native process's: the guest queues take it on
               pr_MsgPort alone, and the scheduler neither owns nor waits on it,
               since a root suspended on it would never be woken. */
            check(!ops->owns_signal(&w.ctx,ROOT,SIGF_DOS) && !w.ctx.guest_signals,
                  "the scheduler does not own the root's reserved DOS signal");
            check(!ops->owns_signal(&w.ctx,TCB(0),SIGF_DOS) &&
                  !ops->owns_signal(&w.ctx,0xDEADBEEF,SIGF_DOS) &&
                  !ops->owns_signal(&w.ctx,ROOT,SIGF_DOS|0x10000) &&
                  !ops->owns_signal(&w.ctx,ROOT,0) && !ops->owns_signal(&w.ctx,ROOT,1U<<9),
                  "SIGB_DOS ownership applies only to the root and the exact DOS signal mask");
            check(Kaffeine_GuestPutMsg(&w.ctx,process_port,msg,0xCAFEBABE)==0xCAFEBABE &&
                  Kaffeine_GuestGetMsg(&w.ctx,process_port,FALSE)==msg,
                  "queue_arrival accepts and drains the root process's DOS port with a scheduler");
            check(!w.ctx.guest_signals && native_alloc==0xFFFF,
                  "the process port neither allocates nor frees SIGB_DOS");
            guest_port(w,other,TCB(0),SIGB_DOS);
            KaffeineQueue queue;
            check(Kaffeine_QueueRead(&w.ctx,other,&queue) &&
                  !Kaffeine_MessagePreflight(&w.ctx,&queue,msg,20),
                  "a child cannot borrow the root's reserved DOS signal for a guest port");
            guest_port(w,other,ROOT,SIGB_DOS);
            bool scoped=Kaffeine_QueueRead(&w.ctx,other,&queue) &&
                        !Kaffeine_MessagePreflight(&w.ctx,&queue,msg,20);
            check(scoped,"scheduler queue_arrival restricts SIGB_DOS to pr_MsgPort");
        }).d(0,0).rts();
    Program(CODE).rts();
    check(w.run()==0 && !w.ctx.sched && native_alloc==0xFFFF,
          "scheduler teardown leaves the reserved DOS signal allocated to the native process");
}

static void memlists(World &w, ULONG tcb, bool valid)
{
    const ULONG list = tcb + 74, one = 0x30000, two = 0x30100;
    w.put(list, one); w.put(list + 4, 0); w.put(list + 8, two);
    w.put(one, two); w.put(one + 4, valid ? list : 0x1234);
    w.put(two, list + 4); w.put(two + 4, one);
}

static void test_removal()
{
    World self;
    self.child(0, 0);
    memlists(self, TCB(0), true);
    Program(ROOT_CODE).add_task(TCB(0), CODE).wait(0x1000)
        .see(0, [] { return world->destroyed; }).d(0, 0).rts();
    Program(CODE).a(6, SYSBASE).signal(ROOT, 0x1000).a(1, 0).call(288)
        .see(1, [] { return 0xBADu; }).rts();
    self.run();
    check(self.seen[0] == 1 && !self.seen[1] && !self.in_flight,
          "RemTask(NULL) ends the child before its next instruction, never in its own thunk");
    check(freed_entries == std::vector<ULONG>({ 0x30000, 0x30100 }) && self.mem[TCB(0) + 15] == 6,
          "its tc_MemEntry MemLists go to the guest heap in list order");

    World other;
    other.child(0, 0, "kid");
    memlists(other, TCB(0), false);
    Program(ROOT_CODE).add_task(TCB(0), CODE).wait(0x1000)
        .a(1, TCB(0)).call(288)
        .see(0, [] { return world->destroyed; })
        .see(1, [] { return (ULONG)world->mem[TCB(0) + 15]; })
        .see(2, [] { return world->at(TASK_WAIT); })
        .a(1, NAMES).call(294).store(SHARED, 0).d(0, 0).rts();
    Program(CODE).a(6, SYSBASE).signal(ROOT, 0x1000).wait(0x4000).rts();
    other.run();
    check(other.seen[0] == 1 && other.seen[1] == 6 && other.seen[2] == TASK_WAIT + 4,
          "RemTask() of a waiting child ends it at once and unlinks it");
    check(freed_entries.empty() && !other.at(SHARED) && !other.ctx.sched,
          "a malformed tc_MemEntry frees nothing; the name is gone");

    World by_child;
    by_child.child(0, 0);
    Program(ROOT_CODE).add_task(TCB(0), CODE).wait(0x1000).d(0, 0).rts();
    Program(CODE).a(6, SYSBASE).a(1, ROOT).call(288).d(0, 0).rts();
    LONG result = by_child.run();
    check(result == 20 && by_child.root.d[0] == 20 && by_child.destroyed == 1 &&
          !by_child.ctx.sched, "RemTask() of the root by a child stops with 20");

    /* The child waits, so that the scheduler is there for the root's call. */
    for (ULONG target : { 0x30000u, 0u })
    {
        World w;
        w.child(0, 0);
        Program(ROOT_CODE).add_task(TCB(0), CODE).wait(0x1000)
            .a(1, target).call(288).d(0, 0).rts();
        Program(CODE).a(6, SYSBASE).signal(ROOT, 0x1000).wait(0x4000).rts();
        result = w.run();
        check(result == 20 && w.ctx.exit_code == 20 && w.root.d[0] == 20 &&
              w.destroyed == 1 && !w.ctx.sched,
              target ? "RemTask() of a foreign Task stops with 20"
                     : "RemTask(NULL) of the root stops with 20");
    }
}

static void test_faults()
{
    World fault;
    fault.child(0, 0);
    Program(ROOT_CODE).add_task(TCB(0), CODE).wait(0x1000).d(0, 0).rts();
    Program(CODE).a(6, SYSBASE).jump(0x13000);
    LONG result = fault.run();
    check(result == 20 && fault.root.d[0] == 20 && fault.faults == 1,
          "a backend fault in a child stops the program with 20 in the root's D0");
    check(fault.destroyed == 1 && !fault.in_flight && !fault.ctx.sched &&
          fault.at(SYSBASE + 276) == ROOT && allocmems == freemems,
          "after a child's fault every CPU and the scheduler go, ThisTask is the root");

    World stop;
    stop.child(0, 0);
    Program(ROOT_CODE).add_task(TCB(0), CODE).wait(0x1000).d(0, 0).rts();
    Program(CODE).d(0, 7).jump(SENTINEL);
    result = stop.run();
    check(result == 7 && stop.root.d[0] == 7, "a stop in a child returns that child's D0");

    World callback;
    callback.child(0, 0);
    Program(ROOT_CODE).add_task(TCB(0), CODE).wait(0x1000).d(0, 0).rts();
    Program(CODE).a(6, SYSBASE).op([](Mock &) { world->ctx.callback_depth = 1; })
        .wait(0x4000).rts();
    result = callback.run();
    check(result == 20 && !native_waits, "a Wait() that would block inside a callback stops with 20");

    World foreign;
    foreign.child(0, 0);
    Program(ROOT_CODE).add_task(TCB(0), CODE).wait(0x1000).d(0, 0).rts();
    Program(CODE).a(6, SYSBASE).signal(0x30000, 1).rts();
    result = foreign.run();
    check(result == 20 && foreign.destroyed == 1 && !native_recvd,
          "Signal() of a foreign Task stops with 20 and signals nothing");

    World deadlock;
    deadlock.child(0, 0);
    Program(ROOT_CODE).add_task(TCB(0), CODE).wait(0).d(0, 0).rts();
    Program(CODE).a(6, SYSBASE).wait(0).rts();
    result = deadlock.run();
    check(result == 20 && !native_waits, "tasks that wait for nothing stop with 20, not a hang");
}

static void test_limits()
{
    World w;
    for (unsigned i = 0; i <= KAFFEINE_MAX_GUEST_TASKS; ++i) w.child(i, -1);
    Program p(ROOT_CODE);
    for (unsigned i = 0; i <= KAFFEINE_MAX_GUEST_TASKS; ++i)
        p.add_task(TCB(i), CODE).store(SHARED + 4 * i, 0);
    p.d(0, 3).rts();
    Program(CODE).rts();
    auto last = std::vector<UBYTE>(w.mem.begin() + TCB(KAFFEINE_MAX_GUEST_TASKS),
                                   w.mem.begin() + TCB(KAFFEINE_MAX_GUEST_TASKS) + 0x100);
    LONG result = w.run();
    bool added = true;
    for (unsigned i = 0; i < KAFFEINE_MAX_GUEST_TASKS; ++i) added = added && w.at(SHARED + 4 * i) == TCB(i);
    check(result == 3 && added && !w.at(SHARED + 4 * KAFFEINE_MAX_GUEST_TASKS) &&
          w.created == KAFFEINE_MAX_GUEST_TASKS,
          "16 children are added; the 17th AddTask() returns NULL without a CPU");
    check(std::equal(last.begin(), last.end(), w.mem.begin() + TCB(KAFFEINE_MAX_GUEST_TASKS)),
          "the refused Task is unchanged");
    check(w.destroyed == KAFFEINE_MAX_GUEST_TASKS && !w.ctx.sched && allocmems == freemems,
          "children that never ran end with the program");

    World nocpu;
    nocpu.child(0, 0);
    nocpu.fail_create = true;
    Program(ROOT_CODE).add_task(TCB(0), CODE).store(SHARED, 0).d(0, 0x55).rts();
    auto task = std::vector<UBYTE>(nocpu.mem.begin() + TCB(0), nocpu.mem.begin() + TCB(0) + 0x100);
    result = nocpu.run();
    check(result == 0x55 && !nocpu.at(SHARED) && !allocmems && !nocpu.ctx.sched &&
          std::equal(task.begin(), task.end(), nocpu.mem.begin() + TCB(0)),
          "a failed CPU makes AddTask() return NULL and change nothing");

    World nomem;
    nomem.child(0, 0);
    fail_allocmem = true;
    Program(ROOT_CODE).add_task(TCB(0), CODE).store(SHARED, 0).d(0, 0x56).rts();
    result = nomem.run();
    check(result == 0x56 && !nomem.at(SHARED) && nomem.created == 1 && nomem.destroyed == 1 &&
          !nomem.ctx.sched, "a failed scheduler allocation destroys the new CPU again");
}

static void test_malformed()
{
    struct Case
    {
        const char *what;
        std::function<void(World &, ULONG &, ULONG &, ULONG &)> setup;
    };
    const Case cases[] =
    {
        { "an odd Task", [](World &, ULONG &t, ULONG &, ULONG &) { t += 1; } },
        { "a Task below the heap", [](World &, ULONG &t, ULONG &, ULONG &) { t = 0xC000; } },
        { "a Task over the root's", [](World &, ULONG &t, ULONG &, ULONG &) { t = ROOT + 0x10; } },
        { "a Task in the root's free stack", [](World &, ULONG &t, ULONG &, ULONG &) { t = 0x3F800; } },
        { "a stack of 128 bytes", [](World &w, ULONG &t, ULONG &, ULONG &) { w.put(t + 62, LOWER(0) + 128); } },
        { "a stack in the root's free stack", [](World &w, ULONG &t, ULONG &, ULONG &)
            { w.put(t + 58, 0x3F100); w.put(t + 62, 0x3F500); } },
        { "a Task in its own free stack", [](World &w, ULONG &t, ULONG &, ULONG &)
            { t = LOWER(0) + 0x10; w.put(t + 58, LOWER(0)); w.put(t + 62, UPPER(0)); } },
        { "tc_Switch", [](World &w, ULONG &t, ULONG &, ULONG &) { w.put(t + 66, CODE); } },
        { "tc_Launch", [](World &w, ULONG &t, ULONG &, ULONG &) { w.put(t + 70, CODE); } },
        { "tc_TrapCode", [](World &w, ULONG &t, ULONG &, ULONG &) { w.put(t + 50, CODE); } },
        { "tc_ExceptCode", [](World &w, ULONG &t, ULONG &, ULONG &) { w.put(t + 42, CODE); } },
        { "an NT_PROCESS", [](World &w, ULONG &t, ULONG &, ULONG &) { w.mem[t + 8] = 13; } },
        { "an unterminated name", [](World &w, ULONG &t, ULONG &, ULONG &)
            { std::memset(w.mem.data() + ARENA - 4, 'x', 4); w.put(t + 10, ARENA - 4); } },
        { "an odd initialPC", [](World &, ULONG &, ULONG &i, ULONG &) { i = CODE + 1; } },
        { "an initialPC in the vectors", [](World &, ULONG &, ULONG &i, ULONG &) { i = 0x100; } },
        { "an odd finalPC", [](World &, ULONG &, ULONG &, ULONG &f) { f = CODE + 3; } },
        { "a shadowed Task", [](World &w, ULONG &t, ULONG &, ULONG &)
            { w.ctx.shadow_map[0].m68k_addr = t + 8; w.ctx.shadow_map[0].native_ptr = &native_task;
              w.ctx.num_shadows = 1; } },
        { "a Task inside a shadow's trailing bytes", [](World &w, ULONG &t, ULONG &, ULONG &)
            { w.ctx.shadow_map[0].m68k_addr=t-40; w.ctx.shadow_map[0].type=110;
              w.ctx.num_shadows=1; } },
    };
    for (const Case &c : cases)
    {
        World w;
        ULONG tcb = TCB(0), initial = CODE, final = 0;
        w.child(0, 0);
        c.setup(w, tcb, initial, final);
        Program(ROOT_CODE).add_task(tcb, initial, final).d(0, 0x77).rts();
        auto before = w.mem;
        LONG result = w.run();
        std::memcpy(before.data() + STACK_TOP - 8, w.mem.data() + STACK_TOP - 8, 4);
        char what[160];
        std::snprintf(what, sizeof(what), "AddTask() of %s stops with 20 and changes nothing", c.what);
        check(result == 20 && w.ctx.exit_code == 20 && !w.created && !allocmems &&
              !w.ctx.sched && w.mem == before, what);
    }

    World twice;
    twice.child(0, -1);
    Program(ROOT_CODE).add_task(TCB(0), CODE).add_task(TCB(0), CODE).d(0, 0x77).rts();
    LONG result = twice.run();
    check(result == 20 && twice.created == 1 && twice.destroyed == 1 && !twice.ctx.sched,
          "AddTask() of a live child's Task stops with 20");

    World frames;
    frames.child(0,5); frames.child(1,0);
    frames.put(TCB(1)+58,LOWER(0)+256);
    frames.put(TCB(1)+62,UPPER(0));
    Program(ROOT_CODE).add_task(TCB(0),CODE).wait(0x1000)
        .add_task(TCB(1),CODE+0x1000).rts();
    Program(CODE).a(6,SYSBASE).a(7,LOWER(0)+64).signal(ROOT,0x1000).wait(0x4000).rts();
    check(frames.run()==20 && frames.created==1 && frames.destroyed==1,
          "AddTask refuses overlap with a child's live stack frames above its saved SP");
}

static void test_reaped_signals()
{
    World w;
    w.child(0,5);
    Program parent(ROOT_CODE);
    for(unsigned i=0;i<32;i++)
        parent.add_task(TCB(0),CODE).wait(0x1000)
              .op([](Mock &) { if(world->at(SHARED)==0xffffffffu) world->seen[0]++; });
    parent.d(0,0).rts();
    Program(CODE).a(6,SYSBASE).d(0,0xffffffffu).call(330).store(SHARED,0)
        .op([](Mock &m) { world->ctx.io_ports[0].guest=0x31000;
                         world->ctx.io_ports[0].sigbit=(BYTE)m.d[0]; })
        .signal(ROOT,0x1000).rts();
    check(w.run()==0 && !w.seen[0] && w.destroyed==32 && retired_signals==32 &&
          native_alloc==0xffff && !w.ctx.guest_signals && w.ctx.io_ports[0].sigbit==-1,
          "32 consecutive children return their unfreed native signal bits when reaped");
}

static void test_reaped_message_storage()
{
    constexpr ULONG entry=0x30000, msg=0x31000, port=0x32000;
    World w;
    w.child(0,5);
    guest_port(w,port,ROOT,20); w.mem[port+14]=2;
    w.ctx.message_ports[0]=port;
    w.put(port+20,msg); w.put(port+28,msg);
    w.put(msg,port+24); w.put(msg+4,port+20); w.mem[msg+8]=5;
    m68k_write16(&w.ctx,msg+18,20);
    w.put(TCB(0)+74,entry); w.put(TCB(0)+82,entry);
    w.put(entry,TCB(0)+78); w.put(entry+4,TCB(0)+74);
    m68k_write16(&w.ctx,entry+14,1);
    w.put(entry+16,msg); w.put(entry+20,64);
    Program(ROOT_CODE).add_task(TCB(0),CODE).wait(0x1000).a(0,port).call(372).rts();
    Program(CODE).a(6,SYSBASE).signal(ROOT,0x1000).rts();
    check(w.run()==(LONG)msg && freed_entries.empty() && !w.ctx.sched,
          "reaping keeps a child's memory that contains a message queued for its parent");
}

static void test_unroutable_native_port()
{
    for(unsigned mode=0;mode<3;mode++)
    {
        World w;
        w.child(0,mode==2 ? 5 : -1);
        if(mode==0) w.port_flags=2;
        if(mode==1) w.port_sigbit=32;
        if(mode==2)
        {
            Program(ROOT_CODE).d(0,20).call(330).add_task(TCB(0),CODE).rts();
            Program(CODE).a(6,SYSBASE).call(384).rts();
        }
        else
        {
            Program(ROOT_CODE).add_task(TCB(0),CODE).call(384).rts();
            Program(CODE).rts();
        }
        check(w.run()==20 && !native_waits,
              "an empty native port with invalid flags, bit, or foreign signal ownership stops safely");
    }
}

static void test_end_cleanup()
{
    World w;
    w.child(0, -1); w.child(1, -1);
    Program(ROOT_CODE).add_task(TCB(0), CODE).add_task(TCB(1), CODE).d(0, 5).rts();
    Program(CODE).rts();
    LONG result = w.run();
    check(result == 5 && w.created == 2 && w.destroyed == 2 && w.mocks.empty() &&
          !w.ctx.sched && allocmems == freemems && w.lists_empty() &&
          w.at(SYSBASE + 276) == ROOT && w.mem[ROOT + 15] == 2 && w.ctx.cpu == nullptr,
          "the root's return ends its children, their CPUs and the scheduler");
}

static void test_native_routing()
{
    World w;
    w.child(0, 0);
    native_event = [] { native_recvd |= 0x80000000; };
    Program(ROOT_CODE)
        .add_task(TCB(0), CODE).a(0, 0x30000).call(384).store(SHARED + 4, 0).mark(1)
        .see(2, [] { return native_recvd & PORT_BIT; })
        .wait(0x1000).store(SHARED + 8, 0).wait(0x2000).store(SHARED + 12, 0).rts();
    Program(CODE)
        .a(6, SYSBASE)
        /* Intuition queues a message and signals the native task. */
        .op([](Mock &) { world->port_messages = 1; native_recvd |= PORT_BIT; })
        .loop(2, 3 * KAFFEINE_TASK_QUANTUM).mark(3)
        .d(0, 0xFFFFFFFF).call(330)
        .op([](Mock &m) { m.d[3] = m.d[0]; m.d[4] = 1u << m.d[0]; })
        .signal(ROOT, 0x1000)
        .op([](Mock &m) { m.d[0] = m.d[4]; }).call(318).store(SHARED + 16, 0)
        .signal(ROOT, 0x2000).rts();
    LONG result = w.run();
    check(w.at(SHARED + 4) == PORT_MESSAGE && w.port_calls == 2 && !w.seen[2],
          "a blocked port wait runs again for its task and returns the message, its signal taken");
    check(w.seen[1] < w.seen[3] && w.seen[1] < KAFFEINE_TASK_QUANTUM + 32,
          "the root wakes for its native port at the busy child's turn, without waiting for it");
    check(native_waits == 1 && native_wait_mask == 0x80002000,
          "with every task waiting the native task waits for the root's and the child's bits");
    check(w.at(SHARED + 16) == 0x80000000 && w.at(SHARED + 8) == 0x1000 &&
          result == 0x2000 && !native_recvd,
          "a native signal on a child's bit wakes that child alone");
}

/* ── Asynchronous I/O hooks ──────────────────────────────────────── */

/* kaffeine_exec_io.c is not linked here: its hooks as it sets them. A
   native request of the root is in flight behind the private bit IO_BIT;
   io_back says that native Exec has it back, which the pump finds by
   GetMsg(), never by the bit, and replies a guest message to the root's
   port as the bridge replies the guest request. */
static constexpr ULONG IO_BIT = 1u << 25, IO_PORT = 0x31000, IO_MSG = 0x31200;
static unsigned io_polls;
static bool io_back, io_fail;
static BOOL fake_io_poll(KaffeineContext *ctx)
{
    KaffeineQueue q;
    ++io_polls;
    if (io_fail)
        return (BOOL)!Kaffeine_MessageFailure(ctx, "I/O completion");
    SetSignal(0, IO_BIT);
    if (!io_back)
        return TRUE;
    io_back = false;
    if (!Kaffeine_QueueRead(ctx, IO_PORT, &q) || !Kaffeine_MessagePreflight(ctx, &q, IO_MSG, 20))
        return (BOOL)!Kaffeine_MessageFailure(ctx, "I/O completion");
    Kaffeine_MessageAppend(ctx, &q, IO_MSG, 7);
    ctx->io_signals = 0;
    return TRUE;
}
/* The device gives the request back: native Exec replies and signals. */
static void io_complete()
{
    io_back = true;
    native_recvd |= IO_BIT;
}
static void io_world(World &w)
{
    io_polls = 0; io_back = io_fail = false;
    native_alloc |= IO_BIT;
    guest_port(w, IO_PORT, ROOT, 20);
    std::memset(w.mem.data() + IO_MSG, 0, 20);
    m68k_write16(&w.ctx, IO_MSG + 18, 20);
}
static void io_private(Mock &)
{
    world->ctx.io_signals = IO_BIT;
}
static void io_start(Mock &)
{
    world->ctx.io_poll = fake_io_poll;
    world->ctx.io_signals = IO_BIT;
}

static void test_io_root_wait_busy_child()
{
    World w;
    w.child(0, 0);
    io_world(w);
    Program(ROOT_CODE).d(0, 20).call(330).add_task(TCB(0), CODE).op(io_start)
        .wait((1u << 20) | 0x1000).store(SHARED, 0).mark(1)
        .a(0, IO_PORT).call(372).store(SHARED + 4, 0)
        .d(0, 0).rts();
    Program(CODE).a(6, SYSBASE).op([](Mock &) { io_complete(); })
        .loop(2, 3 * KAFFEINE_TASK_QUANTUM).mark(2).rts();
    LONG result = w.run();
    check(result == 0 && w.at(SHARED) == (1u << 20) && w.at(SHARED + 4) == IO_MSG && io_polls,
          "the root's Wait() returns its port's bit once the pump replied the completion, never the private bit");
    check(w.seen[1] && !w.seen[2] && w.seen[1] < 2 * KAFFEINE_TASK_QUANTUM + 64 && !native_waits,
          "the root wakes at the busy child's next turn, without a native wait");
}

static void test_io_idle_wait()
{
    World w;
    w.child(0, 0);
    io_world(w);
    Program(ROOT_CODE).d(0, 20).call(330).add_task(TCB(0), CODE).op(io_start)
        .wait(1u << 20).store(SHARED, 0)
        .signal(TCB(0), 0x80000000u).wait(0x1000).store(SHARED + 8, 0)
        .a(0, IO_PORT).call(372).store(SHARED + 12, 0)
        .d(0, 0).rts();
    Program(CODE).a(6, SYSBASE).d(0, 0xFFFFFFFF).call(330)
        .wait(0x80000000u | IO_BIT).store(SHARED + 4, 0)
        .signal(ROOT, 0x1000).rts();
    native_event = io_complete;
    LONG result = w.run();
    check(native_waits == 1 && native_wait_mask == ((1u << 20) | 0x80000000u | IO_BIT),
          "with every task waiting the native task also waits for the private bit of I/O in flight");
    check(result == 0 && w.at(SHARED) == (1u << 20) && w.at(SHARED + 12) == IO_MSG,
          "the completion wakes the root through its guest port");
    check(w.at(SHARED + 4) == 0x80000000u && w.at(SHARED + 8) == 0x1000,
          "a child waiting for the private bit never gets it");
}

static void test_io_private_bits()
{
    World w;
    w.child(0, -1);
    io_world(w);
    Program(ROOT_CODE).add_task(TCB(0), CODE).op(io_private)
        .op([](Mock &m) {
            native_recvd |= IO_BIT;
            world->seen[0] = world->ctx.task_ops->is_root(&world->ctx, &m.handle);
        })
        .d(0, 0).d(1, 0).call(306).store(SHARED, 0)
        .d(0, 0).d(1, 0xFFFFFFFF).call(306)
        .see(1, [] { return native_recvd & IO_BIT; })
        .wait(IO_BIT | 0x1000).store(SHARED + 4, 0)
        .see(2, [] { return native_recvd & IO_BIT; })
        .d(0, 0).rts();
    /* The child runs once the root waits. */
    Program(CODE).a(6, SYSBASE)
        .op([](Mock &m) {
            world->put(SYSBASE + 276, ROOT);            /* a forged ThisTask */
            world->seen[3] = world->ctx.task_ops->is_root(&world->ctx, &m.handle) ||
                             world->ctx.task_ops->is_root(&world->ctx, &world->root.handle);
            world->put(SYSBASE + 276, TCB(0));
        })
        .see(4, [] { return native_recvd & IO_BIT; })
        .op([](Mock &) { native_recvd &= ~IO_BIT; })
        .signal(ROOT, IO_BIT | 0x1000).rts();
    LONG result = w.run();
    check(result == 0 && w.seen[0] && !w.seen[3],
          "is_root() holds for the root's CPU while it runs, never for a child, whatever ThisTask says");
    check(!(w.at(SHARED) & IO_BIT) && w.seen[1],
          "the root's SetSignal() neither shows nor clears a private bit");
    check(w.at(SHARED + 4) == 0x1000 && w.seen[4],
          "the root's Wait() neither returns nor takes a private bit that is set");
    check(!w.seen[2], "a guest Signal() of the root never sends a private bit");
}

static void test_io_blocked_on_private_port()
{
    World w;
    w.child(0, 0);
    io_world(w);
    w.port_sigbit = 25;
    Program(ROOT_CODE).add_task(TCB(0), CODE).op(io_private)
        .call(384).store(SHARED, 0).mark(1).d(0, 0).rts();
    Program(CODE).a(6, SYSBASE)
        .op([](Mock &) { world->port_messages = 1; native_recvd |= IO_BIT; })
        .loop(2, 3 * KAFFEINE_TASK_QUANTUM).mark(2).rts();
    LONG result = w.run();
    check(result == 0 && w.at(SHARED) == PORT_MESSAGE && w.port_calls == 2 && w.seen[1] && !w.seen[2] &&
          !native_waits && !(native_recvd & IO_BIT),
          "the root blocks on a private native port while a child runs, as WaitIO() does, and wakes on its bit");
}

/* A child's tc_MemEntry over an I/O request record: an idle copy is the
   heap's release hook's to close, so it does not keep the memory; the
   reservation of any other record still does. */
static void test_io_reaped_copy_storage()
{
    for (bool copy : { true, false })
    {
        constexpr ULONG entry = 0x30000, storage = 0x30100;
        World w;
        w.child(0, 5);
        w.put(TCB(0) + 74, entry); w.put(TCB(0) + 82, entry);
        w.put(entry, TCB(0) + 78); w.put(entry + 4, TCB(0) + 74);
        m68k_write16(&w.ctx, entry + 14, 1);
        w.put(entry + 16, storage); w.put(entry + 20, 64);
        KaffeineIORequest &record = w.ctx.io_requests[0];
        record = {};
        record.guest = storage + 8;
        record.kind = KAFFEINE_IO_TIMER;
        record.copy = copy;
        Program(ROOT_CODE).add_task(TCB(0), CODE).wait(0x1000).d(0, 0).rts();
        Program(CODE).a(6, SYSBASE).signal(ROOT, 0x1000).rts();
        check(w.run() == 0 && !w.ctx.sched &&
              freed_entries == (copy ? std::vector<ULONG>({ entry }) : std::vector<ULONG>()),
              copy ? "reaping frees a child's memory that holds an idle copied request"
                   : "reaping keeps a child's memory that holds another I/O request");
    }
}

static void test_io_poll_failure()
{
    World w;
    w.child(0, 0);
    io_world(w);
    Program(ROOT_CODE).add_task(TCB(0), CODE).op(io_start)
        .op([](Mock &) { io_fail = true; }).wait(0x1000).d(0, 5).rts();
    Program(CODE).rts();
    check(w.run() == 20 && !w.ctx.running && w.ctx.exit_code == 20,
          "a completion that cannot reach its guest port stops the program with 20");
}

/* The actual scheduler's private IO wait route. Native device stand-ins
   complete records in reverse order; no guest queue or signal is involved. */
static unsigned private_io_ready;
static BOOL private_io_poll(KaffeineContext *ctx)
{
    SetSignal(0,IO_BIT);
    for(int i=2;i>=0;--i)
        if(private_io_ready&(1u<<i))
        {
            auto &e=ctx->io_requests[i];e.pending=0;e.replied=1;
            ctx->task_ops->io_completed(ctx,e.guest,e.owner);
        }
    private_io_ready=0;return TRUE;
}
static IPTR private_io_wait(KaffeineContext *ctx,void *cpu)
{
    ULONG req=Kaffeine_GetA(cpu,1);unsigned i=(req-IO_MSG)/40;
    assert(i<3);auto &e=ctx->io_requests[i];IPTR out;
    if(e.pending && ctx->task_ops->block_io(ctx,cpu,req,private_io_wait,&out))return out;
    return 0x5500+i;
}
static void private_io_start(Mock &m,unsigned i)
{
    auto &c=world->ctx;auto &e=c.io_requests[i];
    e.guest=IO_MSG+40*i;e.native=reinterpret_cast<IORequest*>(uintptr_t(0x1000+i*8));
    e.owner=c.task_ops->caller_task(&c,&m.handle);e.pending=1;
    world->put(SHARED+16+i*4,e.owner);
}
static void test_private_io_waiters()
{
    World w;w.child(0,1);w.child(1,1);private_io_ready=0;
    native_alloc|=IO_BIT;w.ctx.io_signals=IO_BIT;w.ctx.io_poll=private_io_poll;
    Program(ROOT_CODE).d(0,0).call(132).add_task(TCB(0),CODE).add_task(TCB(1),CODE+0x100)
        .call(138).op([](Mock &m){private_io_start(m,2);})
        .a(1,IO_MSG+80).call(474).store(SHARED+8,0).d(0,0).rts();
    Program(CODE).a(6,SYSBASE).op([](Mock &m){
            auto &c=world->ctx;world->put(SYSBASE+276,ROOT);
            world->seen[20]=c.task_ops->caller_task(&c,&m.handle)==TCB(0)&&
                           !c.task_ops->caller_task(&c,&world->root.handle);
            world->put(SYSBASE+276,TCB(0));private_io_start(m,0);
        }).a(1,IO_MSG).call(474).store(SHARED,0).rts();
    Program(CODE+0x100).a(6,SYSBASE).op([](Mock &m){private_io_start(m,1);})
        .a(1,IO_MSG+40).call(474).store(SHARED+4,0).rts();
    native_event=[] {private_io_ready=7;native_recvd|=IO_BIT;};
    check(w.run()==0&&w.at(SHARED)==0x5500&&w.at(SHARED+4)==0x5501&&w.at(SHARED+8)==0x5502,
          "two children and the root resume their distinct private IO retries");
    check(native_waits==1&&native_wait_mask==IO_BIT,
          "when all tasks wait only on IO, the native task waits on the private completion bit");
    check(w.seen[20]&&w.at(SHARED+16)==TCB(0)&&w.at(SHARED+20)==TCB(1)&&w.at(SHARED+24)==ROOT,
          "IO callers come from current scheduler CPUs despite a forged guest ThisTask");
    check(!(native_recvd&IO_BIT)&&!(w.at(ROOT+26)&IO_BIT),
          "private IO retries publish no guest signal or arrival");
}

/* ── Guest processes of CreateProc() ─────────────────────────────── */

static void message(World &w, ULONG msg, ULONG reply)
{
    std::memset(w.mem.data() + msg, 0, 20);
    w.put(msg + 14, reply);
    m68k_write16(&w.ctx, msg + 18, 20);
}

/* The Process that CreateProc() made, from its pr_MsgPort: every field
   that it sets, and every DOS field zero, none of the native process */
static bool process_ok(ULONG port, const char *name, int pri, ULONG seglist, ULONG stack)
{
    World &w = *world;
    ULONG p = port - 92, lower = w.at(p + 58), upper = w.at(p + 62), entry = w.at(p + 74);
    ULONG text = w.at(p + 10), segarray = w.at(p + 128) << 2;
    bool ok = w.mem[p + 8] == 13 && (BYTE)w.mem[p + 9] == pri && text && text != NAMES &&
        !std::strcmp(reinterpret_cast<const char *>(w.mem.data() + text), name) &&
        w.mem[p + 16] == 0xFF && w.mem[p + 17] == 0xFF && w.at(p + 18) == 0xFFFF &&
        upper - lower == stack && w.at(p + 54) == upper - 8 &&
        w.at(upper - 8) == KAFFEINE_TASK_EXIT_ADDR && w.at(upper - 4) == stack &&
        /* tc_MemEntry: one MemList of Process, stack, name and SegArray */
        entry && w.at(p + 82) == entry && w.at(entry) == p + 78 && w.at(entry + 4) == p + 74 &&
        w.mem[entry + 8] == 10 && m68k_read16(&w.ctx, entry + 14) == 4 &&
        w.at(entry + 16) == p && w.at(entry + 20) == 228 &&
        w.at(entry + 24) == lower && w.at(entry + 28) == stack &&
        w.at(entry + 32) == text && w.at(entry + 36) == std::strlen(name) + 1 &&
        w.at(entry + 40) == segarray && w.at(entry + 44) == 20 &&
        /* pr_MsgPort: PA_SIGNAL, SIGB_DOS, the Process, empty */
        w.mem[port + 8] == 4 && w.mem[port + 14] == 0 && w.mem[port + 15] == SIGB_DOS &&
        w.at(port + 16) == p && w.at(port + 20) == port + 24 && !w.at(port + 24) &&
        w.at(port + 28) == port + 20 &&
        w.at(segarray) == 3 && !w.at(segarray + 4) && !w.at(segarray + 8) &&
        w.at(segarray + 12) == seglist && !w.at(segarray + 16) &&
        w.at(p + 132) == stack && w.at(p + 144) == upper >> 2 &&
        w.at(p + 208) == p + 212 && !w.at(p + 212) && w.at(p + 216) == p + 208;
    /* GlobVec, TaskNum, Result2, CurrentDir, CIS, COS, ConsoleTask,
       FileSystemTask, CLI, ReturnAddr, PktWait, WindowPtr, HomeDir, Flags,
       ExitCode, ExitData, Arguments, ShellPrivate, CES */
    for (ULONG at : { 136u, 140u, 148u, 152u, 156u, 160u, 164u, 168u, 172u, 176u, 180u, 184u,
                      188u, 192u, 196u, 200u, 204u, 220u, 224u })
        ok = ok && !w.at(p + at);
    return ok;
}

/* The classic startup protocol: the root sends a startup message to the
   new pr_MsgPort, the child replies it, then a message that tells it to
   go, which it replies under Forbid() before it returns. */
static void test_process_handshake()
{
    constexpr ULONG rp = 0x31000, startup = 0x31200, remove = 0x31300;
    static Mock first;
    World w;
    w.dos();
    w.name(NAMES, "worker");
    guest_port(w, rp, ROOT, 20);
    message(w, startup, rp);
    message(w, remove, rp);
    Program(ROOT_CODE)
        .d(0, 20).call(330)
        .create_proc(NAMES, 0, SEGLIST(0), 1024).store(SHARED, 0)
        .see(0, [] { return (ULONG)process_ok(world->at(SHARED), "worker", 0, SEGLIST(0), 1024); })
        .see(1, [] { ULONG p = world->at(SHARED) - 92;
                     return (ULONG)(world->mem[p + 15] == 3 && world->at(TASK_READY) == p &&
                                    world->at(p) == TASK_READY + 4); })
        .a(1, NAMES).call(294).store(SHARED + 24, 0)
        .to_port().a(1, startup).call(366)
        .a(0, rp).call(390).a(0, rp).call(372).store(SHARED + 4, 0)
        .see(2, [] { ULONG p = world->at(SHARED) - 92;
                     return (ULONG)(world->mem[p + 15] == 4 && world->at(p + 22) == SIGF_DOS &&
                                    world->at(TASK_WAIT) == p && !(native_recvd & SIGF_DOS)); })
        .to_port().a(1, remove).call(366)
        .a(0, rp).call(390).a(0, rp).call(372).store(SHARED + 8, 0)
        .see(3, [] { return world->at(SYSBASE + 276); })
        .see(4, [] { return (ULONG)(world->mem[TDNEST]); })
        .d(0, 20).call(336)
        .d(0, 0).rts();
    Program(CODE)
        .op([](Mock &m) { first = m; })
        .a(1, 0).call(294).store(SHARED + 12, 0)
        .op([](Mock &) { world->put(world->at(SHARED) - 92 + 184, 0xFFFFFFFF); })
        .own_port().call(390).store(SHARED + 16, 0)
        .own_port().call(372).op([](Mock &m) { m.a[1] = m.d[0]; }).call(378)
        .own_port().call(390).own_port().call(372).store(SHARED + 20, 0)
        .call(132).a(1, remove).call(378)
        .rts();
    LONG result = w.run();
    ULONG port = w.at(SHARED), proc = port - 92, upper = w.at(proc + 62);
    check(result == 0 && port && w.seen[0] && w.seen[1],
          "CreateProc() returns pr_MsgPort of a ready NT_PROCESS with its own MemList and no DOS context");
    check(w.at(SHARED + 24) == proc && w.at(SHARED + 12) == proc,
          "FindTask() of its name and FindTask(NULL) in the child are the guest Process");
    bool zero = first.pc == CODE && first.sr == 0x2700 && first.a[6] == SYSBASE &&
                first.a[7] == upper - 8 && first.a[0] == (w.at(proc + 128) << 2) + 16 &&
                !w.at(first.a[0]);
    for (unsigned i = 0; i < 8; ++i) zero = zero && first.d[i] == 0;
    for (unsigned i = 1; i < 6; ++i) zero = zero && first.a[i] == 0;
    check(zero, "the child starts at seg+4: D0 0, A0 an empty string, A6 SysBase, others 0");
    check(w.at(SHARED + 16) == startup && w.at(SHARED + 4) == startup &&
          w.at(SHARED + 20) == remove && w.at(SHARED + 8) == remove && w.mem[remove + 8] == 7,
          "the startup message, sent before the child ran, and the remove message are replied");
    check(w.seen[2], "the child waits for its empty pr_MsgPort on its guest SIGB_DOS alone");
    check(w.at(proc + 184) == 0xFFFFFFFF, "pr_WindowPtr = -1 of the child is a guest write");
    check(freed_entries == std::vector<ULONG>({ HEAP_START }) && w.mem[proc + 15] == 6,
          "the child's return ends it alone and frees its MemList");
    check(w.seen[3] == ROOT && w.seen[4] == 0xFF && w.created == 1 && w.destroyed == 1 &&
          !w.ctx.sched && w.lists_empty(),
          "the root resumes as ThisTask without the child's Forbid(); the scheduler goes");
    check(!native_waits && native_alloc == 0xFFFF && !w.ctx.guest_signals &&
          !(native_recvd & SIGF_DOS) && !dos_calls,
          "no native Wait(), and SIGB_DOS of the child never was a native bit");
}

/* Priorities as Exec's: a higher one preempts at the next slice, a lower
   one runs while the root waits; both live at once, found by name */
static void test_process_priorities()
{
    constexpr ULONG m1 = 0x31200;
    World w;
    w.dos();
    w.name(NAMES, "alpha");
    w.name(NAMES + 32, "beta");
    message(w, m1, 0);
    Program(ROOT_CODE)
        .create_proc(NAMES, 1, SEGLIST(0), 1024).store(SHARED, 0).mark(1)
        .create_proc(NAMES + 32, -1, SEGLIST(1), 1024).store(SHARED + 4, 0).mark(2)
        .a(1, NAMES).call(294).store(SHARED + 8, 0)
        .a(1, NAMES + 32).call(294).store(SHARED + 12, 0)
        .see(7, [] { return (ULONG)world->mem[world->at(SHARED) - 92 + 9] |
                            (ULONG)world->mem[world->at(SHARED + 4) - 92 + 9] << 8; })
        .to_port().a(1, m1).call(366).mark(3)
        .wait(0x1000).mark(8)
        .d(0, 0).rts();
    Program(CODE).mark(4).own_port().call(390).mark(6).own_port().call(372).rts();
    Program(CODE + 0x1000).mark(5).signal(ROOT, 0x1000).rts();
    LONG result = w.run();
    check(result == 0 && w.seen[4] < w.seen[1] && w.seen[1] < w.seen[2],
          "a process of higher priority runs right after its CreateProc()");
    check(w.seen[6] < w.seen[3] && w.seen[3] < w.seen[5] && w.seen[5] < w.seen[8],
          "PutMsg() wakes it at once; the lower one runs only while the root waits");
    check(w.at(SHARED + 8) == w.at(SHARED) - 92 && w.at(SHARED + 12) == w.at(SHARED + 4) - 92 &&
          w.seen[7] == 0xFF01, "two processes live at once with their names and priorities");
    check(w.created == 2 && w.destroyed == 2 && !w.ctx.sched && freed_entries.size() == 1,
          "the returned one frees its memory; the other ends with the program");
}

/* Forbid() keeps a new process out until Permit(); its Wait() breaks its
   own Forbid(), which it gets back, and which ends with it */
static void test_process_forbid_wait()
{
    constexpr ULONG m1 = 0x31200;
    World w;
    w.dos();
    w.name(NAMES, "forbid");
    message(w, m1, 0);
    Program(ROOT_CODE)
        .call(132).create_proc(NAMES, 1, SEGLIST(0), 1024).store(SHARED, 0)
        .mark(1).mark(2).call(138).mark(3)
        .see(10, [] { return (ULONG)world->mem[TDNEST]; })
        .to_port().a(1, m1).call(366)
        .see(12, [] { return (ULONG)world->mem[TDNEST]; })
        .d(0, 0).rts();
    Program(CODE)
        .mark(4).call(132).d(0, SIGF_DOS).call(318).store(SHARED + 4, 0)
        .see(11, [] { return (ULONG)world->mem[TDNEST]; })
        .own_port().call(372).rts();
    LONG result = w.run();
    check(result == 0 && w.seen[4] > w.seen[2] && w.seen[4] < w.seen[3],
          "Forbid() keeps a higher-priority process out until the slice after Permit()");
    check(w.at(SHARED + 4) == SIGF_DOS && w.seen[10] == 0xFF && w.seen[11] == 0 && w.seen[12] == 0xFF,
          "Wait() of the guest SIGB_DOS breaks the child's Forbid(), which it gets back and keeps to its end");
    check(!native_waits && freed_entries.size() == 1 && !w.ctx.sched,
          "PutMsg() to its pr_MsgPort wakes it without native signals");
}

/* A native SIGB_DOS stays the native process's; the child's is virtual */
static void test_process_signals()
{
    constexpr ULONG other = 0x31000, msg = 0x31200;
    World w;
    w.dos();
    w.name(NAMES, "signals");
    message(w, msg, 0);
    Program(ROOT_CODE)
        .create_proc(NAMES, 1, SEGLIST(0), 1024).store(SHARED, 0)
        .op([&w](Mock &) {
            auto *ops = w.ctx.task_ops;
            ULONG port = w.at(SHARED), p = port - 92;
            KaffeineQueue queue;
            check(ops->process_port(&w.ctx, port, p) && !ops->process_port(&w.ctx, port, ROOT) &&
                  !ops->process_port(&w.ctx, port + 4, p) && !ops->process_port(&w.ctx, ROOT + 92, ROOT),
                  "process_port() is the pr_MsgPort of a live child Process alone");
            check(!ops->owns_signal(&w.ctx, p, SIGF_DOS) && !(w.ctx.guest_signals & SIGF_DOS),
                  "the child's SIGB_DOS is neither a native nor an owned bridge signal");
            guest_port(w, other, p, SIGB_DOS);
            check(Kaffeine_QueueRead(&w.ctx, other, &queue) &&
                  !Kaffeine_MessagePreflight(&w.ctx, &queue, msg, 20),
                  "a child Process's SIGB_DOS serves its pr_MsgPort alone");
            native_event = [] { native_recvd |= 0x1000; };
            native_recvd |= SIGF_DOS;
        })
        .wait(0x1000).store(SHARED + 4, 0)
        .see(0, [] { ULONG p = world->at(SHARED) - 92;
                     return (ULONG)(world->mem[p + 15] == 4 && !(world->at(p + 26) & SIGF_DOS)); })
        .see(1, [] { return native_wait_mask; })
        .to_port().a(1, msg).call(366)
        .see(2, [] { return native_recvd; })
        .wait(0x2000).store(SHARED + 8, 0)
        .d(0, 0).rts();
    Program(CODE).own_port().call(390).store(SHARED + 12, 0).own_port().call(372)
        .signal(ROOT, 0x2000).rts();
    LONG result = w.run();
    check(result == 0 && w.at(SHARED + 4) == 0x1000 && w.seen[0] && w.seen[1] == 0x1000,
          "a native SIGB_DOS neither wakes the child nor joins the native wait");
    check((w.seen[2] & SIGF_DOS) && w.at(SHARED + 12) == msg && w.at(SHARED + 8) == 0x2000,
          "the child wakes for its message alone; the native SIGB_DOS stays the root's");
    check(native_waits == 2 && native_wait_mask == 0x2000,
          "only the root's two ordinary signal waits reach native Exec");
    check(native_alloc == 0xFFFF && !w.ctx.guest_signals && !w.ctx.sched,
          "no native signal was allocated for the child");
}

/* No dos.library call of a child Process reaches its thunk; its Exit()
   ends it alone. Other tasks call DOS as before. */
static void test_process_dos_guard()
{
    for (UWORD lvo : { 60, 72, 138 })
    {
        World w;
        w.dos();
        Program(ROOT_CODE).create_proc(NAMES, 1, SEGLIST(0), 1024).d(0, 0x55).rts();
        Program(CODE).op([](Mock &m) { m.d[1] = NAMES; m.d[3] = SEGLIST(1); }).dos(lvo)
            .see(9, [] { return 1u; }).rts();
        LONG result = w.run();
        check(result == 20 && w.ctx.exit_code == 20 && !w.seen[9] && !dos_calls &&
              w.created == 1 && heap_lists == 1 && w.destroyed == 1 && !w.ctx.sched,
              "any dos.library call of a child Process stops the program before its thunk");
    }

    World task;
    task.dos();
    task.child(0, 1);
    Program(ROOT_CODE).add_task(TCB(0), CODE).d(0, 0x55).rts();
    Program(CODE).dos(60).store(SHARED, 0).rts();
    check(task.run() == 0x55 && dos_calls == 1 && task.at(SHARED) == 0x1234,
          "an AddTask() child calls dos.library as before");

    World parent;
    parent.dos();
    Program(ROOT_CODE).create_proc(NAMES, -1, SEGLIST(0), 1024).dos(60).store(SHARED, 0)
        .d(0, 0x56).rts();
    Program(CODE).rts();
    check(parent.run() == 0x56 && dos_calls == 1 && parent.at(SHARED) == 0x1234,
          "the root calls dos.library as before while a child Process lives");

    World child_exit;
    child_exit.dos();
    child_exit.child(0, 1);
    Program(ROOT_CODE).add_task(TCB(0), CODE).d(0, 0x55).rts();
    Program(CODE).d(1, 7).dos(144).rts();
    check(child_exit.run() == 7 && dos_exits == 1,
          "Exit() of an AddTask() child still reaches DOS and ends the program");

    for (bool forbid : { false, true })
    {
        World w;
        w.dos();
        Program(ROOT_CODE).create_proc(NAMES, 1, SEGLIST(0), 1024)
            .see(0, [] { return (ULONG)freed_entries.size(); })
            .see(1, [] { return (ULONG)world->mem[TDNEST]; })
            .see(2, [] { return world->destroyed; })
            .a(1, 0).call(294).store(SHARED, 0).d(0, 0x66).rts();
        Program p(CODE);
        if (forbid) p.call(132);
        p.d(1, 5).dos(144).see(9, [] { return 1u; }).rts();
        LONG result = w.run();
        check(result == 0x66 && !dos_exits && w.seen[0] == 1 && w.seen[1] == 0xFF &&
              w.seen[2] == 1 && !w.seen[9] && w.at(SHARED) == ROOT && !w.ctx.sched,
              forbid ? "Exit() of a child Process under Forbid() ends that child alone"
                     : "Exit() of a child Process ends that child alone, never the root");
    }
}

/* A message still queued on the child's pr_MsgPort keeps its memory; its
   old port takes no message once it has gone */
static void test_process_quarantine()
{
    constexpr ULONG m1 = 0x31200, m2 = 0x31300, m3 = 0x31400;
    World w;
    w.dos();
    message(w, m1, 0); message(w, m2, 0); message(w, m3, 0);
    Program(ROOT_CODE)
        .create_proc(NAMES, 1, SEGLIST(0), 1024).store(SHARED, 0)
        .call(132).to_port().a(1, m1).call(366).to_port().a(1, m2).call(366).call(138)
        .see(0, [] { return (ULONG)freed_entries.size() + 1; })
        .see(1, [] { return world->destroyed; })
        .to_port().a(1, m3).call(366)
        .see(2, [] { return 1u; })
        .d(0, 0).rts();
    Program(CODE).own_port().call(390).own_port().call(372).store(SHARED + 4, 0).rts();
    LONG result = w.run();
    check(w.seen[0] == 1 && w.seen[1] == 1 && w.at(SHARED + 4) == m1,
          "a child Process that ends with a queued message keeps its MemList");
    check(result == 20 && !w.seen[2] && !w.ctx.sched,
          "PutMsg() to the pr_MsgPort of a removed process stops with 20");
}

/* Another task's memory never overlaps any of the 228 bytes of a Process */
static void test_process_overlap()
{
    for (ULONG offset : { 100u, 228u })
    {
        World w;
        w.dos();
        w.child(1, -1);
        Program(ROOT_CODE)
            .create_proc(NAMES, -1, SEGLIST(0), 1024).store(SHARED, 0)
            .op([offset](Mock &) {
                ULONG p = world->at(SHARED) - 92;
                world->put(TCB(1) + 58, p + offset);
                world->put(TCB(1) + 62, p + offset + 0x200);
            })
            .add_task(TCB(1), CODE + 0x2000).store(SHARED + 4, 0).d(0, 0x44).rts();
        Program(CODE).rts();
        Program(CODE + 0x2000).rts();
        LONG result = w.run();
        check(offset == 100 ? result == 20 && w.created == 1 && !w.ctx.sched
                            : result == 0x44 && w.at(SHARED + 4) == TCB(1) && w.created == 2,
              offset == 100 ? "AddTask() of a stack over a Process past its Task stops with 20"
                            : "a stack right after the Process is free");
    }

    World heap;
    heap.dos();
    Program(ROOT_CODE)
        .create_proc(NAMES, -1, SEGLIST(0), 1024).store(SHARED, 0)
        .op([](Mock &) { heap_force = world->at(SHARED) - 92 + 100; })
        .create_proc(NAMES, -1, SEGLIST(1), 1024).d(0, 0).rts();
    Program(CODE).rts();
    LONG result = heap.run();
    check(result == 20 && heap_lists == 2 && freed_entries.size() == 1 &&
          freed_entries[0] != HEAP_START && heap.created == 2 && heap.destroyed == 2 && !heap.ctx.sched,
          "memory of the heap over another Process stops with 20 and rolls the new one back");
}

/* What cannot be made returns NULL with ERROR_NO_FREE_STORE and leaves
   nothing behind; what Exec could not use stops with 20 and changes nothing */
static void test_process_failures()
{
    for (unsigned mode = 0; mode < 4; ++mode)
    {
        World w;
        w.dos();
        if (mode == 0) w.fail_create = true;
        if (mode == 1) fail_heap = true;
        if (mode == 2) fail_allocmem = true;
        w.put(SHARED, 0xFFFFFFFF);
        Program(ROOT_CODE).create_proc(NAMES, 0, SEGLIST(0), mode == 3 ? 0x40004 : 1024)
            .store(SHARED, 0).d(0, 0x31).rts();
        Program(CODE).rts();
        LONG result = w.run();
        static const char *const what[] =
        {
            "a failed CPU", "a guest heap without room",
            "a failed scheduler allocation", "a stack over 256 KiB"
        };
        char text[160];
        std::snprintf(text, sizeof(text),
                      "CreateProc() with %s returns NULL, ERROR_NO_FREE_STORE and leaves nothing", what[mode]);
        check(result == 0x31 && !w.at(SHARED) && dos_ioerr == ERROR_NO_FREE_STORE &&
              w.created == w.destroyed && w.created == (unsigned)(mode == 1 || mode == 2) &&
              heap_lists == (unsigned)(mode == 2) && freed_entries.size() == (size_t)(mode == 2) &&
              !w.ctx.sched && w.mocks.empty(), text);
    }

    World full;
    full.dos();
    for (unsigned i = 0; i < KAFFEINE_MAX_GUEST_TASKS; ++i) full.child(i, -1);
    full.put(SHARED, 0xFFFFFFFF);
    Program p(ROOT_CODE);
    for (unsigned i = 0; i < KAFFEINE_MAX_GUEST_TASKS; ++i) p.add_task(TCB(i), CODE + 0x2000);
    p.create_proc(NAMES, 0, SEGLIST(0), 1024).store(SHARED, 0).d(0, 0x32).rts();
    Program(CODE + 0x2000).rts();
    check(full.run() == 0x32 && !full.at(SHARED) && dos_ioerr == ERROR_NO_FREE_STORE &&
          full.created == KAFFEINE_MAX_GUEST_TASKS && !heap_lists && !full.ctx.sched,
          "CreateProc() with 16 guest tasks returns NULL without a CPU or memory");

    World nested;
    nested.dos();
    nested.child(0, 1);
    nested.put(SHARED, 0xFFFFFFFF);
    Program(ROOT_CODE).add_task(TCB(0), CODE + 0x2000).d(0, 0x33).rts();
    Program(CODE + 0x2000).create_proc(NAMES, 0, SEGLIST(0), 1024).store(SHARED, 0).rts();
    check(nested.run() == 0x33 && !nested.at(SHARED) && dos_ioerr == ERROR_NOT_IMPLEMENTED &&
          nested.created == 1 && !heap_lists,
          "CreateProc() of an AddTask() child fails as before, without a process");

    struct Case
    {
        const char *what;
        std::function<void(World &, ULONG &, ULONG &)> setup;
    };
    const Case cases[] =
    {
        { "a NULL name", [](World &, ULONG &n, ULONG &) { n = 0; } },
        { "an unterminated name", [](World &w, ULONG &n, ULONG &)
            { std::memset(w.mem.data() + ARENA - 4, 'x', 4); n = ARENA - 4; } },
        { "a name of 256 bytes", [](World &w, ULONG &n, ULONG &)
            { std::memset(w.mem.data() + 0xC000, 'n', 256); n = 0xC000; } },
        { "a NULL seglist", [](World &, ULONG &, ULONG &s) { s = 0; } },
        { "a seglist past the arena", [](World &, ULONG &, ULONG &s) { s = ARENA >> 2; } },
        { "a seglist in the vectors", [](World &, ULONG &, ULONG &s) { s = 0x100 >> 2; } },
        { "a seglist BPTR that wraps", [](World &, ULONG &, ULONG &s) { s = 0x40000000 | SEGLIST(0); } },
        { "a seglist that links to itself", [](World &w, ULONG &, ULONG &s)
            { w.put(SEG(0), SEGLIST(0)); s = SEGLIST(0); } },
        { "a seglist cycle of two", [](World &w, ULONG &, ULONG &s)
            { w.put(SEG(0), 0x30000 >> 2); w.put(0x30000, SEGLIST(0)); s = SEGLIST(0); } },
        { "a seglist of 65 segments", [](World &w, ULONG &, ULONG &s)
            { for (ULONG i = 0; i < 64; ++i) w.put(0x30000 + 8 * i, (0x30008 + 8 * i) >> 2);
              s = 0x30000 >> 2; } },
        { "a seglist whose next is out of the arena", [](World &w, ULONG &, ULONG &s)
            { w.put(SEG(0), ARENA >> 2); s = SEGLIST(0); } },
    };
    for (const Case &c : cases)
    {
        World w;
        ULONG name = NAMES, seglist = SEGLIST(0);
        w.dos();
        c.setup(w, name, seglist);
        Program(ROOT_CODE).create_proc(name, 0, seglist, 1024).d(0, 0x77).rts();
        Program(CODE).rts();
        auto before = w.mem;
        LONG result = w.run();
        std::memcpy(before.data() + STACK_TOP - 8, w.mem.data() + STACK_TOP - 8, 4);
        char what[160];
        std::snprintf(what, sizeof(what), "CreateProc() of %s stops with 20 and changes nothing", c.what);
        check(result == 20 && w.ctx.exit_code == 20 && !w.created && !heap_lists &&
              !w.ctx.sched && w.mem == before, what);
    }
}

/* ── Real Moira CPUs ─────────────────────────────────────────────── */

/* guest_tasks_68k.s, assembled */
static const UWORD moira_parent[] =
{
    0x93C9,                         /* SUBA.L  A1,A1            */
    0x4EAE, 0xFEDA,                 /* JSR     -294(A6)  FindTask */
    0x23C0, 0x0000, 0xA010,         /* MOVE.L  D0,$A010         */
    0x43F9, 0x0002, 0x0000,         /* LEA     $20000,A1        */
    0x45F9, 0x0001, 0x2000,         /* LEA     $12000,A2        */
    0x97CB,                         /* SUBA.L  A3,A3            */
    0x4EAE, 0xFEE6,                 /* JSR     -282(A6)  AddTask */
    0x23C0, 0x0000, 0xA000,         /* MOVE.L  D0,$A000         */
    0x203C, 0x0000, 0x8000,         /* MOVE.L  #$8000,D0        */
    0x4EAE, 0xFEC2,                 /* JSR     -318(A6)  Wait   */
    0x23C0, 0x0000, 0xA004,         /* MOVE.L  D0,$A004         */
    0x43F9, 0x0002, 0x0000,         /* LEA     $20000,A1        */
    0x203C, 0x0000, 0x4000,         /* MOVE.L  #$4000,D0        */
    0x4EAE, 0xFEBC,                 /* JSR     -324(A6)  Signal */
    0x203C, 0x0000, 0x8000,         /* MOVE.L  #$8000,D0        */
    0x4EAE, 0xFEC2,                 /* JSR     -318(A6)  Wait   */
    0x23C0, 0x0000, 0xA008,         /* MOVE.L  D0,$A008         */
    0x2039, 0x0000, 0xA00C,         /* MOVE.L  $A00C,D0         */
    0x4E75                          /* RTS                      */
};
static const UWORD moira_child[] =
{
    0x2C78, 0x0004,                 /* MOVEA.L 4.W,A6           */
    0x93C9,                         /* SUBA.L  A1,A1            */
    0x4EAE, 0xFEDA,                 /* JSR     -294(A6)  FindTask */
    0x23C0, 0x0000, 0xA014,         /* MOVE.L  D0,$A014         */
    0x722A,                         /* MOVEQ   #42,D1           */
    0x23C1, 0x0000, 0xA00C,         /* MOVE.L  D1,$A00C         */
    0x2279, 0x0000, 0xA010,         /* MOVEA.L $A010,A1         */
    0x203C, 0x0000, 0x8000,         /* MOVE.L  #$8000,D0        */
    0x4EAE, 0xFEBC,                 /* JSR     -324(A6)  Signal */
    0x203C, 0x0000, 0x4000,         /* MOVE.L  #$4000,D0        */
    0x4EAE, 0xFEC2,                 /* JSR     -318(A6)  Wait   */
    0x23C0, 0x0000, 0xA018,         /* MOVE.L  D0,$A018         */
    0x2279, 0x0000, 0xA010,         /* MOVEA.L $A010,A1         */
    0x203C, 0x0000, 0x8000,         /* MOVE.L  #$8000,D0        */
    0x4EAE, 0xFEBC,                 /* JSR     -324(A6)  Signal */
    0x4E75                          /* RTS                      */
};

static void test_backend_parent_and_child(bool jit)
{
    World w;
    w.child(0, 0, "moira child");
    ULONG at = ROOT_CODE;
    for (UWORD word : moira_parent) { m68k_write16(&w.ctx, at, word); at += 2; }
    at = CODE;
    for (UWORD word : moira_child) { m68k_write16(&w.ctx, at, word); at += 2; }
    w.ctx.entry_point = ROOT_CODE;
    LONG result = jit ? Kaffeine_ExecuteJIT(&w.ctx) : Kaffeine_Execute(&w.ctx);
    check(result == 42, "backend: the parent returns the value that the child stored");
    check(w.at(0xA000) == TCB(0) && w.at(0xA010) == ROOT && w.at(0xA014) == TCB(0),
          "backend: AddTask() returns the Task, FindTask(NULL) is each task's own");
    check(w.at(0xA004) == 0x8000 && w.at(0xA008) == 0x8000 && w.at(0xA018) == 0x4000,
          "backend: parent and child each resume from Wait() with the other's signal");
    check(!w.ctx.sched && !w.ctx.cpu_factory && allocmems == freemems && !native_recvd &&
          w.at(SYSBASE + 276) == ROOT && w.mem[TCB(0) + 15] == 6 && !native_waits,
          "backend: the child ended and the program is the root alone again");
}

static void test_jit_child_faults()
{
    for (bool full_sr : { false, true })
    {
        World w;
        w.child(0, 0, "faulting JIT child");
        ULONG at = ROOT_CODE;
        for (UWORD word : moira_parent) { m68k_write16(&w.ctx, at, word); at += 2; }
        m68k_write16(&w.ctx, CODE, full_sr ? 0x46FC : 0x4AFC);
        m68k_write16(&w.ctx, CODE + 2, 0); /* MOVE #0,SR or unreachable data */
        m68k_write16(&w.ctx, CODE + 4, 0x23FC); /* unreachable MOVE.L #0,$A00C */
        w.put(CODE + 6, 0); w.put(CODE + 10, 0xA00C);
        w.put(0xA00C, 0x12345678);
        w.ctx.entry_point = ROOT_CODE;
        check(Kaffeine_ExecuteJIT(&w.ctx) == 20,
              "a JIT fault in a child ends the whole program with RETURN_FAIL");
        check(w.at(0xA00C) == 0x12345678,
              "the child never resumes after an unsupported opcode or full SR write");
        check(!w.ctx.cpu && !w.ctx.cpu_factory && !w.ctx.sched && allocmems == freemems,
              "a child's JIT fault releases the scheduler and every CPU");
    }
}

/* ── Typed Processes in the same guest arena ───────────────────── */

static ULONG typed_start(Mock &m, KaffeineDosBroker &broker, KaffeineDomainModule &module,
                         unsigned segment, BYTE pri = 1, bool cli = true)
{
    KaffeineProcessStart spec = {};
    spec.stack_size = 1024;
    spec.name = "typed worker";
    spec.command = "C:typed";
    spec.arguments = "one two\n";
    spec.cli = cli;
    spec.priority = pri;
    spec.backend = "moira";
    return Kaffeine_SchedStartProcess(&world->ctx, &m.handle, &spec,
                                      SEGLIST(segment), CODE + 0x1000 * segment,
                                      &broker, &module, nullptr);
}

static void test_typed_process_startup_messages()
{
    constexpr ULONG reply = 0x31000, msg = 0x31200, root_cli = 0xA400;
    World w;
    w.dos();
    KaffeineDosBroker broker;
    KaffeineDomainModule module;
    Mock first = {};
    std::strcpy(w.ctx.program_name, "root command");
    w.ctx.m68k_cli = root_cli;
    w.ctx.m68k_argptr = NAMES + 100;
    w.ctx.m68k_argsize = 9;
    guest_port(w, reply, ROOT, 20);
    message(w, msg, reply);
    w.ctx.io_task_removed = [](KaffeineContext *, ULONG) { lifetime_order.push_back('Q'); };
    Program(ROOT_CODE).d(0, 20).call(330)
        .op([&](Mock &m) { m.d[0] = typed_start(m, broker, module, 0); })
        .store(SHARED, 0).op([](Mock &m) { m.a[0] = world->at(SHARED) + 92; })
        .a(1, msg).call(366).a(0, reply).call(390).a(0, reply).call(372).store(SHARED + 4, 0)
        .dos(492).store(SHARED + 20, 0)
        .see(15, [] { return !std::strcmp(world->ctx.program_name, "root command") &&
                            world->ctx.guest_exec_task == ROOT && world->ctx.m68k_argptr == NAMES + 100 &&
                            world->ctx.m68k_argsize == 9; })
        .d(0, 20).call(336).d(0, 0x66).rts();
    Program(CODE).op([&](Mock &m) {
        first = m;
        ULONG p = w.ctx.guest_exec_task;
        w.seen[22] = w.at(p + 156) == 0x4101 && w.at(p + 160) == 0x4201 &&
                     w.at(p + 224) == 0x4301 && w.ctx.m68k_argptr == w.at(p + 204) &&
                     w.ctx.m68k_argsize == 8 && p == w.at(SYSBASE + 276);
    })
        .dos(60).store(SHARED + 8, 0).dos(492).store(SHARED + 12, 0)
        .see(14, [] { return !std::strcmp(world->ctx.program_name, "C:typed"); })
        .own_port().call(390).store(SHARED + 16, 0).own_port().call(372)
        .call(132).a(1, msg).call(378).d(1, 5).dos(144).see(19, [] { return 1u; }).rts();
    check(w.run() == 0x66 && !w.seen[19] && !dos_exits,
          "typed Process Exit ends that Process and preserves the root return code");
    ULONG proc = w.at(SHARED), cli = w.at(SHARED + 12), lower = w.at(proc + 58);
    ULONG args = w.at(proc + 204), segarray = w.at(proc + 128) << 2;
    check(proc >= HEAP_START && w.mem[proc + 8] == 13 && w.mem[proc + 92 + 15] == SIGB_DOS &&
          w.at(proc + 92 + 16) == proc && w.at(segarray + 12) == SEGLIST(0),
          "typed startup returns a real guest Process with its own pr_MsgPort and SegArray");
    check(first.d[0] == 8 && first.a[0] == args && first.a[6] == SYSBASE &&
          first.a[7] == lower + 1024 - 8 && first.pc == CODE &&
          !std::strcmp(reinterpret_cast<char *>(w.mem.data() + args), "one two\n"),
          "typed startup copies arguments and initializes its independent CPU and stack");
    check(w.seen[22],
          "private broker standard streams and current Process argument view exist before its first instruction");
    check(cli && cli == (w.at(proc + 172) << 2) && w.at(cli + 60) == SEGLIST(0) &&
          w.at(cli + 52) == 256 && w.at(SHARED + 20) == root_cli && w.seen[14] && w.seen[15],
          "typed guest CLI and program name remain separate from the root across switches");
    check(w.at(SHARED + 4) == msg && w.at(SHARED + 16) == msg && !native_waits,
          "typed root and child exchange the original message pointer through one arena");
    check(w.at(SHARED + 8) == 0x1001 && broker.calls == 2 && broker_lvos == std::vector<ULONG>({60, 492}),
          "typed DOS selects compiled thunks once and runs them through the child's broker");
    check(broker.stops == 1 && module.unloads == 1 && w.destroyed == 1 &&
          lifetime_order == std::vector<char>({'D', 'B', 'Q', 'C', 'M', 'F'}) && !w.ctx.broker_call &&
          broker.inits == 1 && broker.drops == 1,
          "task cleanup waits for broker ACK, quarantines IO, destroys CPU, releases module, then frees startup memory");
}

static void test_typed_nested_broker()
{
    World w;
    w.dos();
    KaffeineDosBroker parent, child;
    child.id = 2;
    KaffeineDomainModule parent_module, child_module;
    ULONG nested = 0, stack = 0;
    typed_create_thunk = [&](KaffeineContext *ctx, void *cpu) -> IPTR {
        check(active_broker == &parent && ctx->broker_call && ctx->cpu == cpu,
              "a nested typed create runs with the parent CPU stable on its broker");
        auto &m = *static_cast<Mock *>(static_cast<KaffeineCPU *>(cpu)->backend_state);
        ctx->after_thunk = [](KaffeineContext *c) {
            check(c->broker_call && active_broker && active_broker->id == 1,
                  "the whole native DOS thunk and its after-thunk action share the parent broker");
        };
        nested = typed_start(m, child, child_module, 1, 2, false);
        return nested;
    };
    Program(ROOT_CODE).op([&](Mock &m) { m.d[0] = typed_start(m, parent, parent_module, 0); })
        .store(SHARED, 0).d(0, 0x77).rts();
    Program(CODE).op([&](Mock &m) { stack = m.a[7]; }).dos(498).store(SHARED + 4, 0)
        .see(20, [&] { return world->ctx.cpu && Kaffeine_GetA(world->ctx.cpu, 7) == stack; })
        .dos(60).store(SHARED + 8, 0).d(1, 0).dos(144).rts();
    Program(CODE + 0x1000).dos(60).store(SHARED + 12, 0)
        .see(21, [] { return world->ctx.m68k_cli == 0; }).rts();
    check(w.run() == 0x77 && nested && w.at(SHARED + 4) == nested && w.seen[20],
          "a nested typed process preserves its parent's single dispatcher return and stack pop");
    check(w.at(SHARED + 8) == 0x1001 && w.at(SHARED + 12) == 0x1002 && w.seen[21],
          "nested Processes keep distinct DOS brokers and optional guest CLI state");
    check(parent.calls == 2 && child.calls == 1 && parent.stops == 1 && child.stops == 1 &&
          parent_module.unloads == 1 && child_module.unloads == 1 && w.destroyed == 2,
          "nested Process resources retire once without stopping their parent");
}

static void test_typed_generated_and_lifetime_limits()
{
    World generated;
    generated.dos();
    KaffeineDosBroker broker;
    KaffeineDomainModule module;
    static const M68KThunkEntry gen[] = { {66, dos_output}, {60, dos_cli_probe} };
    generated.ctx.libs[1].gen_thunks = gen;
    generated.ctx.libs[1].num_gen_thunks = 2;
    Program(ROOT_CODE).op([&](Mock &m) { m.d[0] = typed_start(m, broker, module, 0); })
        .d(0, 0x61).rts();
    Program(CODE).dos(66).store(SHARED, 0).dos(60).store(SHARED + 4, 0).rts();
    check(generated.run() == 0x61 && generated.at(SHARED) == 0x1001 &&
          generated.at(SHARED + 4) == 0x1001 && broker_lvos == std::vector<ULONG>({66, 60}),
          "generated DOS thunks use the broker and manual entries retain dispatch precedence");

    World limit;
    KaffeineDosBroker retained;
    KaffeineDomainModule retained_module;
    Program root(ROOT_CODE);
    for (unsigned i = 0; i < KAFFEINE_MAX_GUEST_TASKS; ++i)
    {
        limit.child(i, -1);
        root.add_task(TCB(i), CODE);
    }
    root.op([&](Mock &m) {
        check(!typed_start(m, retained, retained_module, 0) &&
              world->created == KAFFEINE_MAX_GUEST_TASKS,
              "a full scheduler rejects typed startup before allocating another CPU");
        check(!world->ctx.task_ops->root_exit(&world->ctx, &m.handle, 7),
              "classic tasks alone preserve their original root program-end lifetime");
    }).d(0, 0x62).rts();
    Program(CODE).rts();
    check(limit.run() == 0x62 && !retained.stops && !retained_module.unloads &&
          limit.created == limit.destroyed,
          "table exhaustion retains typed resource ownership and cleans existing classic CPUs");

    World stop;
    stop.dos();
    KaffeineDosBroker stopped;
    KaffeineDomainModule stopped_module;
    Program(ROOT_CODE).op([&](Mock &m) { m.d[0] = typed_start(m, stopped, stopped_module, 0, -1); })
        .op([](Mock &m) { Kaffeine_StopGuest(&world->ctx, &m.handle, 20); }).rts();
    Program(CODE).see(52, [] { return 1u; }).rts();
    check(stop.run() == 20 && !stop.seen[52] && stopped.stops == 1 && stopped_module.unloads == 1,
          "domain failure cleans a never-scheduled typed Process without running global resource cleanup");
}

static void test_typed_root_process_port()
{
    constexpr ULONG msg = 0x31200, root_port = ROOT + 92;
    World w;
    w.dos();
    KaffeineDosBroker broker;
    KaffeineDomainModule module;
    w.ctx.num_shadows = 1;
    w.ctx.shadow_map[0].m68k_addr = ROOT;
    w.ctx.shadow_map[0].type = 258;
    guest_port(w, root_port, ROOT, SIGB_DOS);
    message(w, msg, 0);
    Program(ROOT_CODE).op([&](Mock &m) { m.d[0] = typed_start(m, broker, module, 0); })
        .store(SHARED, 0).a(0, root_port).call(390).a(0, root_port).call(372).store(SHARED + 4, 0)
        .a(1, msg).call(378).d(0, 0x63).rts();
    Program(CODE).op([](Mock &) { world->put(msg + 14, world->ctx.guest_exec_task + 92); })
        .a(0, root_port).a(1, msg).call(366).own_port().call(390).own_port().call(372)
        .store(SHARED + 8, 0).rts();
    check(w.run() == 0x63 && w.at(SHARED + 4) == msg && w.at(SHARED + 8) == msg,
          "a typed Process exchanges direct messages with the root's shadowed pr_MsgPort");
    check(w.ctx.guest_exec_task == ROOT && broker.stops == 1 && module.unloads == 1,
          "root Process identity stays trusted while the current DOS Process view changes");
}

static void test_typed_callback_dos()
{
    World w;
    w.dos();
    KaffeineDosBroker broker;
    KaffeineDomainModule module;
    Program(ROOT_CODE).op([&](Mock &m) { m.d[0] = typed_start(m, broker, module, 0); })
        .d(0, 0x64).rts();
    Program(CODE).op([](Mock &m) {
        auto before = m;
        KaffeineGuestCall call = {};
        check(Kaffeine_CallGuest(&world->ctx, &m.handle, CODE + 0x2000, &call),
              "a driver-side guest callback can make a synchronous selected DOS broker call");
        check(call.d[0] == 0x1001 && !world->ctx.callback_depth && !world->ctx.broker_call &&
              m.pc == before.pc && m.sr == before.sr &&
              std::equal(m.a, m.a + 8, before.a) && std::equal(m.d, m.d + 8, before.d),
              "synchronous DOS in a guest callback preserves its registers, stack, depth and current CPU");
    }).rts();
    Program(CODE + 0x2000).dos(60).see(53, [] { return world->ctx.callback_depth == 1; }).rts();
    check(w.run() == 0x64 && w.seen[53] && broker.calls == 1,
          "callback DOS execution resumes on the same guest CPU without a scheduler switch");

    World exit;
    exit.dos();
    KaffeineDosBroker exiting;
    KaffeineDomainModule exit_module;
    Program(ROOT_CODE).op([&](Mock &m) { m.d[0] = typed_start(m, exiting, exit_module, 0); })
        .d(0, 0).rts();
    Program(CODE).op([](Mock &m) {
        KaffeineGuestCall call = {};
        (void)Kaffeine_CallGuest(&world->ctx, &m.handle, CODE + 0x2000, &call);
    }).rts();
    Program(CODE + 0x2000).d(1, 7).dos(144).rts();
    check(exit.run() == 20 && !dos_exits && !exiting.calls && exiting.stops == 1,
          "a callback cannot Exit its Process or forward native DOS Exit to its broker");
}

static void test_typed_root_lifetime()
{
    for (bool dos_exit_root : { false, true })
    {
        World w;
        w.dos();
        KaffeineDosBroker broker;
        KaffeineDomainModule module;
        w.ctx.io_task_removed = [](KaffeineContext *, ULONG task) {
            if (task == ROOT) ++world->seen[44];
        };
        w.ctx.idcmp_task_gone = [](KaffeineContext *, ULONG task) {
            ++world->seen[task == ROOT ? 45 : 47];
        };
        w.ctx.root_complete = [](KaffeineContext *ctx, LONG result) {
            ++world->seen[40]; world->seen[41] = result;
            world->seen[42] = (ULONG)world->trace.size();
            world->seen[46] = !ctx->broker_call && !ctx->broker_owner &&
                              ctx->cpu == &world->root.handle && world->seen[44] == 1 && world->seen[45] == 1;
        };
        Program root(ROOT_CODE);
        root.op([&](Mock &m) { m.d[0] = typed_start(m, broker, module, 0, -1); }).store(SHARED, 0);
        if (dos_exit_root) root.d(1, 0x68).dos(144);
        else root.op([](Mock &m) {
            check(world->ctx.task_ops->root_exit(&world->ctx, &m.handle, 0x68),
                  "root normal completion becomes a domain lifetime event");
        });
        root.see(49, [] { return 1u; }).rts();
        Program(CODE).mark(43).dos(60).store(SHARED + 4, 0).d(0, 0x9999).rts();
        check(w.run() == 0x68 && w.seen[40] == 1 && w.seen[41] == 0x68 &&
              w.seen[42] < w.seen[43] && w.seen[44] == 1 && w.seen[46] && !w.seen[49] && !dos_exits,
              dos_exit_root ? "root DOS Exit publishes completion once while the child keeps running"
                            : "root return publishes completion once while the child keeps running");
        check(w.at(SHARED + 4) == 0x1001 && broker.stops == 1 && module.unloads == 1 &&
              !w.ctx.sched && w.root.d[0] == 0x68 && w.ctx.exit_code == 0x68 && w.seen[47] == 1,
              "the last child retires IDCMP once and drains with the saved root result rather than its own D0");
    }

    World fault;
    fault.dos();
    KaffeineDosBroker broker;
    KaffeineDomainModule module;
    Program(ROOT_CODE).op([&](Mock &m) { m.d[0] = typed_start(m, broker, module, 0, -1); })
        .op([](Mock &m) { world->ctx.task_ops->root_exit(&world->ctx, &m.handle, 0x68); }).rts();
    Program(CODE).jump(0x17FFE);
    check(fault.run() == 20 && fault.root.d[0] == 20 && broker.stops == 1 && module.unloads == 1,
          "a child fault after root completion remains a domain failure, never the saved root result");
}

static void test_typed_dos_refusals()
{
    for (unsigned mode = 0; mode < 4; ++mode)
    {
        World w;
        w.dos();
        KaffeineDosBroker broker;
        KaffeineDomainModule module;
        broker.fail = mode == 0;
        module.busy = mode == 3;
        Program(ROOT_CODE).op([&](Mock &m) { m.d[0] = typed_start(m, broker, module, 0); })
            .d(0, 0).rts();
        Program child(CODE);
        if (mode == 0) child.dos(60);
        if (mode == 1) child.dos(72);
        if (mode == 2) child.op([](Mock &m) {
            IPTR ret = 0;
            world->ctx.task_ops->process_dos(&world->ctx, &m.handle, 0, &ret);
            m.d[0] = (ULONG)ret;
        });
        if (mode == 3) child.op([](Mock &m) {
            IPTR ret = 0;
            check(!world->ctx.task_ops->process_dos(&world->ctx, &m.handle, 60, &ret),
                  "typed guard leaves selected DOS calls for dos_call");
            world->ctx.task_ops->dos_call(&world->ctx, &m.handle, dos_cli_probe, &ret);
            m.d[0] = (ULONG)ret;
        });
        child.see(50, [] { return 1u; }).rts();
        Program(CODE + 0x2000).see(51, [] { return 1u; }).rts();
        check(w.run() == 20 && !w.seen[50] && !w.seen[51] && !dos_calls,
              mode == 0 ? "broker transport failure has no native root fallback"
              : mode == 1 ? "a native broker thunk cannot reenter the guest CPU through a callback"
              : mode == 2 ? "native packet adapters without a selected DOS thunk remain refused"
                          : "a different compiled thunk cannot replace the dispatcher's selected DOS call");
        check(broker.stops == 1 && module.unloads == 1 && !w.ctx.broker_call && !w.ctx.sched,
              "broker refusal retires task resources once and leaves busy module ownership with the domain");
    }
}

static void test_typed_start_failures()
{
    for (unsigned mode = 0; mode < 6; ++mode)
    {
        World w;
        w.dos();
        KaffeineDosBroker broker;
        KaffeineDomainModule module;
        Program(ROOT_CODE).op([&](Mock &m) {
            w.fail_create = mode == 0;
            fail_heap = mode == 1;
            fail_allocmem = mode == 2;
            broker.fail_init = mode == 5;
            KaffeineProcessStart spec = {};
            spec.stack_size = mode == 3 ? KAFFEINE_PROCESS_MAX_STACK + 4 : 1024;
            spec.name = "typed"; spec.arguments = "";
            SIPTR error = 0;
            ULONG result = Kaffeine_SchedStartProcess(&w.ctx, &m.handle, &spec, SEGLIST(0), CODE,
                                                      mode == 4 ? nullptr : &broker, &module, &error);
            fail_allocmem = false;
            check(!result && !w.ctx.sched && !broker.stops && !module.unloads,
                  "typed startup failure publishes no task and leaves broker/module with the caller");
            check(w.created == w.destroyed,
                  "typed startup failure rolls back every CPU it allocated");
            check(error == (mode == 3 ? ERROR_BAD_NUMBER : mode == 4 ? ERROR_NOT_IMPLEMENTED
                                                                 : ERROR_NO_FREE_STORE),
                  "typed startup reports its validation, capability or allocation failure to the native caller");
            if (mode == 5)
                check(broker.inits == 1 && broker.drops == 1 && !broker.stops &&
                      w.ctx.guest_exec_task == ROOT && !w.ctx.broker_call && !w.ctx.broker_owner,
                      "partial broker startup rolls back DOS metadata and restores parent view without consuming broker");
        }).d(0, 0x44).rts();
        check(w.run() == 0x44, "a typed startup allocation or validation failure leaves its root runnable");
    }
}

static void test_typed_backend_domain(bool jit)
{
    World w;
    w.dos();
    KaffeineDosBroker broker;
    KaffeineDomainModule module;
    typed_create_thunk = [&](KaffeineContext *ctx, void *cpu) -> IPTR {
        KaffeineProcessStart spec = {};
        spec.stack_size = 1024; spec.name = "backend typed"; spec.arguments = "args\n";
        spec.priority = -1; spec.cli = TRUE;
        SIPTR error;
        return Kaffeine_SchedStartProcess(ctx, cpu, &spec, SEGLIST(0), CODE,
                                          &broker, &module, &error);
    };
    w.ctx.root_complete = [](KaffeineContext *, LONG result) {
        ++world->seen[60]; world->seen[61] = result;
    };
    static const UWORD root_code[] = {
        0x2C7C, 0x0000, 0x7000,     /* MOVEA.L #dosbase,A6 */
        0x4EAE, 0xFE0E,             /* JSR -498(A6), typed CreateNewProc */
        0x23C0, 0x0000, 0xA010,     /* MOVE.L D0,$A010 */
        0x7055, 0x4E75              /* MOVEQ #$55,D0; RTS */
    };
    static const UWORD child_code[] = {
        0x2C7C, 0x0000, 0x7000,     /* MOVEA.L #dosbase,A6 */
        0x4EAE, 0xFFC4,             /* JSR -60(A6), Output */
        0x23C0, 0x0000, 0xA000,     /* MOVE.L D0,$A000 */
        0x7007, 0x4E75              /* MOVEQ #7,D0; RTS */
    };
    ULONG at = ROOT_CODE;
    for (UWORD word : root_code) { m68k_write16(&w.ctx, at, word); at += 2; }
    at = CODE;
    for (UWORD word : child_code) { m68k_write16(&w.ctx, at, word); at += 2; }
    for (at = w.ctx.libs[1].jt_start; at < DOSBASE; at += 2)
        m68k_write16(&w.ctx, at, 0xA000);
    w.ctx.entry_point = ROOT_CODE;
    LONG result = jit ? Kaffeine_ExecuteJIT(&w.ctx) : Kaffeine_Execute(&w.ctx);
    check(result == 0x55 && w.ctx.exit_code == 0x55 && w.seen[60] == 1 && w.seen[61] == 0x55 &&
          w.at(SHARED) == 0x1001 && w.at(SHARED + 16) >= HEAP_START,
          jit ? "JIT typed Process continues after the real root sentinel and preserves its root result"
              : "Moira typed Process continues after the real root sentinel and preserves its root result");
    check(broker.inits == 1 && broker.calls == 1 && broker.drops == 1 && broker.stops == 1 &&
          module.unloads == 1 && !w.ctx.cpu && !w.ctx.sched && !w.ctx.cpu_factory &&
          w.ctx.guest_exec_task == ROOT && allocmems == freemems,
          "backend typed domain retires its native owners and restores root context after the last real CPU");
}

static void test_empty_root_process_port()
{
    constexpr ULONG msg = 0x31200, root_port = ROOT + 92, other = 0x31000;
    World w;
    w.dos();
    KaffeineDosBroker broker;
    KaffeineDomainModule module;
    guest_port(w, root_port, ROOT, SIGB_DOS);
    message(w, msg, root_port);
    Program(ROOT_CODE).op([&](Mock &m) { m.d[0] = typed_start(m, broker, module, 0, -1); })
        .store(SHARED, 0).op([](Mock &m) { m.a[0] = world->at(SHARED) + 92; })
        .a(1, msg).call(366).a(0, root_port).call(390).a(0, root_port).call(372)
        .store(SHARED + 4, 0).d(0, 0x65).rts();
    Program(CODE).own_port().call(390).own_port().call(372).a(1, msg).call(378).rts();
    check(w.run() == 0x65 && w.at(SHARED + 4) == msg && !native_waits &&
          native_alloc == 0xffff && !w.ctx.guest_signals,
          "empty root pr_MsgPort WaitPort yields to a lower-priority child reply without allocating SIGB_DOS");

    World invalid;
    invalid.dos();
    KaffeineDosBroker another;
    KaffeineDomainModule another_module;
    guest_port(invalid, other, ROOT, SIGB_DOS);
    Program(ROOT_CODE).op([&](Mock &m) { m.d[0] = typed_start(m, another, another_module, 0, -1); })
        .a(0, other).call(390).d(0, 0).rts();
    Program(CODE).rts();
    check(invalid.run() == 20 && !native_waits && native_alloc == 0xffff,
          "a different root-owned port cannot borrow the root Process's reserved SIGB_DOS wait permission");
}

static void test_authoritative_stop_code()
{
    World w;
    w.dos();
    typed_create_thunk = [](KaffeineContext *ctx, void *cpu) -> IPTR {
        Kaffeine_StopGuest(ctx, cpu, 20);
        return 0;
    };
    Program(ROOT_CODE).dos(498).see(59, [] { return 1u; }).rts();
    check(w.run() == 20 && w.ctx.exit_code == 20 && !w.seen[59],
          "a stopped thunk returning API failure zero retains authoritative domain failure 20");
}

static void test_staged_start_recovery()
{
    for (bool busy_before_init : { false, true })
    {
        World w;
        w.dos();
        KaffeineDosBroker broker;
        KaffeineDomainModule module;
        broker.fail_init = !busy_before_init;
        broker.drop_refusals = busy_before_init ? 0 : 1;
        broker.finish_refusals = 1;
        broker.busy = busy_before_init;
        std::strcpy(w.ctx.program_name, "recovery parent");
        w.ctx.m68k_cli = 0xA400;
        ULONG staged_tcb = 0;
        bool recovery_done = false;
        idle_probe = [&](KaffeineDosBroker *b) {
            if (b->busy)
                check(w.ctx.guest_exec_task == ROOT && w.ctx.m68k_cli == 0xA400 &&
                      w.at(SYSBASE + 276) == ROOT && !w.ctx.broker_call && !w.ctx.broker_owner &&
                      !std::strcmp(w.ctx.program_name, "recovery parent"),
                      "a busy staged broker is probed without changing any parent DOS/CPU view");
        };
        delay_event = [&] {
            check(w.created == 1 && !w.destroyed && freed_entries.empty() && !broker.stops && !module.unloads,
                  "busy or refused recovery keeps staged CPU, TCB, heap, broker and module alive");
            broker.busy = false;
        };
        drain_action = [&](KaffeineContext *ctx) -> BOOL {
            if (recovery_done) return TRUE;
            check(Kaffeine_SchedHasRecoveryStart(ctx, &broker) &&
                  !ctx->task_ops->known_task(ctx, staged_tcb),
                  "a recovery slot remains private and cannot become a guest task or port owner");
            if (!Kaffeine_SchedRecoverStart(ctx, &broker))
            {
                /* Match DomainDrainStarts: the owner yields before retrying
                   an unacknowledged recovery record. */
                Delay(1);
                return FALSE;
            }
            check(broker.stops == 1 && w.destroyed == 1 && freed_entries.size() == 1 && !module.unloads &&
                  !Kaffeine_SchedHasRecoveryStart(ctx, &broker) && ctx->cpu == &w.root.handle &&
                  ctx->guest_exec_task == ROOT && !ctx->broker_call && !ctx->broker_owner,
                  "only recovery Finish ACK releases staged CPU/memory and restores a live parent view");
            Kaffeine_DomainUnload(ctx, &module);
            recovery_done = true;
            return TRUE;
        };
        w.ctx.domain_drain = [](KaffeineContext *ctx) -> BOOL { return drain_action(ctx); };
        Program(ROOT_CODE).op([&](Mock &m) {
            ULONG result = typed_start(m, broker, module, 0);
            staged_tcb = w.at(HEAP_START + 16);
            check(!result && Kaffeine_SchedHasRecoveryStart(&w.ctx, &broker) && !w.ctx.running &&
                  w.ctx.callback_failed && w.ctx.exit_code == 20 && !w.destroyed && freed_entries.empty(),
                  "failed startup retains its preallocated recovery slot and stops every guest CPU");
            check(!w.ctx.task_ops->root_exit(&w.ctx, &m.handle, 55),
                  "an unpublished recovery slot does not count as a live typed child");
            broker.busy = true;
            auto before = w.mem;
            auto cpu = m;
            check(!Kaffeine_SchedRecoverStart(&w.ctx, &broker) && w.mem == before &&
                  std::equal(m.d, m.d + 8, cpu.d) && std::equal(m.a, m.a + 8, cpu.a),
                  "busy recovery does not touch guest memory, registers or the staged CPU");
            m.d[0] = result; /* dispatcher also writes zero after API failure */
        }).see(58, [] { return 1u; }).rts();
        Program(CODE).see(57, [] { return 1u; }).rts();
        check(w.run() == 20 && !w.seen[58] && !w.seen[57] && native_delays == 2 &&
              broker.stops == 1 && module.unloads == 1 && w.created == w.destroyed && !w.ctx.sched,
              "driver drains staged recovery before scheduler/backend teardown and never resumes guest code");
    }
}

static void test_published_finish_recovery()
{
    for (bool root_stops : { false, true })
    {
        World w;
        w.dos();
        KaffeineDosBroker broker;
        KaffeineDomainModule module;
        broker.finish_refusals = 1;
        delay_event = [&] {
            check(!w.destroyed && freed_entries.empty() && !broker.stops && !module.unloads,
                  "published Process retirement retains its CPU, TCB and module until Finish ACK");
            broker.busy = false;
        };
        Program root(ROOT_CODE);
        root.op([&](Mock &m) { m.d[0] = typed_start(m, broker, module, 0, root_stops ? -1 : 1); });
        if (root_stops) root.op([&](Mock &m) {
            broker.busy = true; Kaffeine_StopGuest(&w.ctx, &m.handle, 20);
        });
        root.d(0, 0).rts();
        Program(CODE).op([&](Mock &) { broker.busy = true; }).rts();
        check(w.run() == 20 && native_delays == 2 && broker.stops == 1 && module.unloads == 1 &&
              w.created == w.destroyed && !w.ctx.sched,
              root_stops ? "domain end retries broker retirement through ACK before global cleanup"
                         : "ordinary task reaping retries broker retirement through ACK before releasing its CPU");
    }
}

static bool info_scope_held;
static ULONG info_scope_owner;
static unsigned info_scope_releases;
static KaffeineLayerOps info_scope_ops()
{
    KaffeineLayerOps ops = {};
    ops.info_locked=[](KaffeineContext *) { return info_scope_held ? TRUE : FALSE; };
    ops.release_locks=[](KaffeineContext *) {
        if(info_scope_held) { info_scope_held=false;++info_scope_releases;lifetime_order.push_back('L'); }
    };
    ops.task_removed=[](KaffeineContext *ctx,ULONG task) {
        if(info_scope_held && task==info_scope_owner) ctx->layer_ops->release_locks(ctx);
    };
    return ops;
}
static void test_info_lock_scheduler_lifetime()
{
    for(unsigned mode=0;mode<5;++mode)
    {
        World w;w.dos();KaffeineDosBroker broker;KaffeineDomainModule module;
        KaffeineLayerOps layers=info_scope_ops();w.ctx.layer_ops=&layers;
        info_scope_held=false;info_scope_owner=0;info_scope_releases=0;
        idle_probe=[](KaffeineDosBroker*) {
            check(!info_scope_held,"no broker idle/Finish path runs inside the caller's Info scope");
        };
        Program root(ROOT_CODE);
        root.op([&](Mock&m) {
            m.d[0]=typed_start(m,broker,module,0,mode==2?1:mode==0?0:-1);
            w.put(SHARED,m.d[0]);
            check(m.d[0]!=0,"Info-scope fixture starts a real staged scheduler Process");
            if(mode!=2) { info_scope_held=true;info_scope_owner=ROOT; }
        });
        if(mode==0)
        {
            root.d(0,0).loop(0,5000).op([&](Mock&) {
                check(!w.seen[0],"Info owner remains on its CPU beyond the normal round-robin quantum");
                info_scope_held=false;
            }).d(1,0).dos(144);
            Program(CODE).mark(0).d(0,0).rts();
        }
        if(mode==1)
        {
            root.op([&](Mock&m) {
                ULONG child=w.at(SHARED);UBYTE state=w.mem[child+15];
                m.a[1]=child;
                IPTR result=Kaffeine_TaskRemove(&w.ctx,&m.handle);
                m.d[0]=(ULONG)result; // the real dispatcher writes the thunk result to D0
                check(result==20 && !w.ctx.running && !broker.stops && !module.unloads &&
                      !w.destroyed && w.mem[child+15]==state,
                      "RemTask(other) refuses before changing its child or submitting Finish inside an Info scope");
            }).mark(1).rts();
            Program(CODE).d(0,0).rts();
        }
        if(mode==2)
        {
            root.d(0,0).rts();
            Program(CODE).op([&](Mock&) {
                info_scope_held=true;info_scope_owner=w.ctx.guest_exec_task;
            }).a(1,0).call(288).mark(1).rts();
        }
        if(mode==3)
        {
            w.ctx.root_complete=[](KaffeineContext*,LONG result) {
                check(!info_scope_held && result==77,"root completion releases its Info owner before opening the native return gate");
            };
            root.op([&](Mock&m) {
                check(w.ctx.task_ops->root_exit(&w.ctx,&m.handle,77),"normal root completion is accepted with a typed child");
            }).mark(1).rts();
            Program(CODE).d(0,0).rts();
        }
        if(mode==4)
        {
            root.op([&](Mock&m) {
                m.d[0]=1;
                IPTR refused=Kaffeine_TaskWait(&w.ctx,&m.handle);
                m.d[0]=(ULONG)refused;
                check(refused==20 && !w.ctx.running && !native_waits,
                      "direct scheduler Wait refuses without entering native Wait or a blocked slot inside an Info scope");
            }).mark(1).rts();
            Program(CODE).d(0,0).rts();
        }
        LONG result=w.run();
        check(result==(mode==1 || mode==4?20:mode==3?77:0) && !info_scope_held &&
              broker.stops==1 && module.unloads==1 && w.created==w.destroyed && !w.ctx.sched,
              "Info scope retirement precedes broker ACK, module release and CPU destruction");
        if(mode) check(!w.seen[1] && info_scope_releases==1,"stopped or retired Info owners execute no following guest instruction and release once");
        w.ctx.layer_ops=nullptr;
    }
}

/* Deterministic collaborator for owner delivery, not a foreign-thread or
   recipe-lifetime proof. The native recipe bridge has its own tests. */
static constexpr ULONG PAINT_WAKE = 1UL << 30, PAINT_BREAK = 1UL << 13;
struct PendingPaint
{
    ULONG owner = 0, signals = PAINT_BREAK;
    bool pending = false, error = false, alive = true;
    unsigned polls = 0;
    std::vector<ULONG> retired;
};
static PendingPaint *pending_paint;
static KaffeineLayerOps pending_paint_ops()
{
    KaffeineLayerOps ops = {};
    ops.signal_mask = [](KaffeineContext *) { return PAINT_WAKE; };
    ops.poll = [](KaffeineContext *ctx) {
        auto &p = *pending_paint;
        ++p.polls;
        if (!p.pending) return TRUE;
        SetSignal(0, PAINT_WAKE);
        p.pending = false;
        if (p.error) return FALSE;
        if (p.alive && p.owner && ctx->sched &&
            ctx->task_ops->can_signal(ctx, p.owner, p.signals))
            ctx->task_ops->send_signal(ctx, p.owner, p.signals);
        return TRUE;
    };
    ops.task_removed = [](KaffeineContext *, ULONG task) {
        auto &p = *pending_paint;
        p.retired.push_back(task);
        if (task == p.owner) { p.alive = false; p.pending = false; }
    };
    return ops;
}
static void test_async_virtual_break_wake()
{
    World w; w.dos(); KaffeineDosBroker broker; KaffeineDomainModule module;
    PendingPaint paint; pending_paint = &paint;
    auto ops = pending_paint_ops(); w.ctx.layer_ops = &ops;
    native_alloc |= PAINT_WAKE; // captured native driver bit, never guest-owned
    native_event = [&] { paint.pending = true; native_recvd |= PAINT_WAKE; };
    Program(ROOT_CODE).op([&](Mock &m) {
        paint.owner = typed_start(m, broker, module, 0, -1);
        check(paint.owner != 0, "paint wake fixture starts a typed guest Process");
        check(w.ctx.task_ops->can_signal(&w.ctx, paint.owner, PAINT_BREAK) &&
              !w.ctx.task_ops->owns_signal(&w.ctx, paint.owner, PAINT_BREAK),
              "CTRL_D is a private per-task capability, not a native GUI-port signal");
        w.put(paint.owner + 18, 0); // writable tc_SigAlloc cannot revoke private capability
        check(w.ctx.task_ops->can_signal(&w.ctx, paint.owner, PAINT_BREAK),
              "paint break capability does not read the writable guest tc_SigAlloc");
        w.put(paint.owner + 18, 0xFFFFFFFF);
        check(!w.ctx.task_ops->can_signal(&w.ctx, paint.owner, 1UL << 8) &&
              !w.ctx.task_ops->can_signal(&w.ctx, paint.owner, PAINT_WAKE) &&
              !w.ctx.task_ops->can_signal(&w.ctx, paint.owner, 1UL << 19),
              "forged allocation fields cannot authorize DOS, private wake or unallocated bits");
        check(w.ctx.task_ops->root_exit(&w.ctx, &m.handle, 55),
              "root completion leaves the paint owner alive in the shared domain");
    }).mark(9).rts();
    Program(CODE).op([&](Mock &m) {
        m.d[0] = PAINT_WAKE; m.d[1] = PAINT_WAKE;
        check(Kaffeine_TaskSetSignal(&w.ctx, &m.handle) == 0 && !w.at(paint.owner + 26),
              "child SetSignal cannot set or reveal the private layer wake bit");
        w.ctx.task_ops->send_signal(&w.ctx, paint.owner, PAINT_WAKE);
        check(!w.at(paint.owner + 26), "child Signal cannot synthesize a private wake");
    }).wait(PAINT_BREAK | PAINT_WAKE).store(SHARED + 4, 0)
        .op([&](Mock &) {
            check(!(native_recvd & PAINT_BREAK),
                  "child CTRL_D delivery never becomes a native signal or unowned broadcast");
        }).d(1, 7).dos(144).mark(8).rts();
    check(w.run() == 55 && native_waits == 1 && native_wait_mask == PAINT_WAKE &&
          w.at(SHARED + 4) == PAINT_BREAK && !w.seen[8] && !w.seen[9],
          "a sole child waiting on virtual CTRL_D wakes through the private layer bit and keeps the root result");
    check(std::count(paint.retired.begin(), paint.retired.end(), ROOT) == 1 &&
          std::count(paint.retired.begin(), paint.retired.end(), paint.owner) == 1 &&
          !paint.alive && !w.ctx.sched && w.created == w.destroyed &&
          !(w.ctx.guest_signals & PAINT_WAKE),
          "root and child retire before domain teardown without giving the private bit to the guest");
    w.ctx.layer_ops = nullptr;
}
static void test_async_hidden_signals_and_retirement()
{
    World w; w.child(0, -1);
    PendingPaint paint; pending_paint = &paint; paint.owner = TCB(0);
    auto ops = pending_paint_ops(); w.ctx.layer_ops = &ops;
    native_alloc |= PAINT_WAKE;
    Program(ROOT_CODE).add_task(TCB(0), CODE).op([&](Mock &m) {
        m.d[0] = PAINT_WAKE; m.d[1] = PAINT_WAKE;
        check(Kaffeine_TaskSetSignal(&w.ctx, &m.handle) == 0 && !(native_recvd & PAINT_WAKE),
              "root SetSignal cannot set or reveal the private layer wake bit");
        w.ctx.task_ops->send_signal(&w.ctx, ROOT, PAINT_WAKE);
        check(!(native_recvd & PAINT_WAKE), "root Signal cannot synthesize a private wake");
        check(!w.ctx.task_ops->can_signal(&w.ctx, TCB(0), 1UL << 19),
              "a live task cannot receive an arbitrary unallocated paint signal");
    }).a(1, TCB(0)).call(288).op([&](Mock &) {
        check(!w.ctx.task_ops->can_signal(&w.ctx, TCB(0), PAINT_BREAK) && !paint.alive,
              "task removal revokes its captured owner before the CPU and TCB may be reused");
        check(std::count(paint.retired.begin(), paint.retired.end(), TCB(0)) == 1 && w.destroyed == 1,
              "ordinary reap notifies layer ownership before dropping the task's CPU");
        w.child(0, -1);
    }).add_task(TCB(0), CODE + 0x1000).op([&](Mock &) {
        check(w.ctx.task_ops->can_signal(&w.ctx, TCB(0), PAINT_BREAK),
              "a new task at the same TCB has its own current break capability");
        paint.pending = true; native_recvd |= PAINT_WAKE;
        check(Kaffeine_AsyncPoll(&w.ctx) && !w.at(TCB(0) + 26) && !(native_recvd & PAINT_BREAK),
              "the already-retired captured record cannot target a reused TCB or fall back to root");
    }).d(0, 0).rts();
    Program(CODE).mark(7).rts();
    Program(CODE + 0x1000).mark(6).rts();
    check(w.run() == 0 && !w.seen[6] && !w.seen[7] && w.created == w.destroyed,
          "domain end retires a still-live replacement task without executing either child");
    check(std::count(paint.retired.begin(), paint.retired.end(), ROOT) == 1 &&
          std::count(paint.retired.begin(), paint.retired.end(), TCB(0)) == 2,
          "final sched_end notifies the root and every remaining child lifetime");
    w.ctx.layer_ops = nullptr;
}
static void test_async_owner_poll_without_scheduler()
{
    World w;
    PendingPaint paint; pending_paint = &paint; paint.error = true;
    auto ops = pending_paint_ops(); w.ctx.layer_ops = &ops;
    Program(ROOT_CODE).op([&](Mock &) { paint.pending = true; }).mark(4).d(0, 0).rts();
    check(w.run() == 20 && w.ctx.callback_failed && w.ctx.exit_code == 20 &&
          !w.seen[4] && !w.ctx.sched && paint.polls == 1,
          "a layer-poll error after a root slice forces authoritative20 before another instruction, without a scheduler");
    w.ctx.running = true; w.ctx.callback_failed = false; w.ctx.broker_call = true;
    paint.pending = true; unsigned polls = paint.polls;
    check(Kaffeine_AsyncPoll(&w.ctx) && paint.polls == polls && paint.pending,
          "a compiled DOS broker cannot pump driver-owned layer state");
    w.ctx.broker_call = false;
    check(std::count(paint.retired.begin(), paint.retired.end(), ROOT) == 1,
          "a no-scheduler root retires layer ownership before its backend and native signals go");
    w.ctx.layer_ops = nullptr;
}

int main()
{
    test_async_virtual_break_wake();
    test_async_hidden_signals_and_retirement();
    test_async_owner_poll_without_scheduler();
    test_info_lock_scheduler_lifetime();
    test_no_children();
    test_root_priority();
    test_add_wait_return();
    test_signals();
    test_round_robin();
    test_priorities();
    test_forbid();
    test_disable();
    test_guest_messages();
    test_process_dos_signal();
    test_removal();
    test_faults();
    test_limits();
    test_malformed();
    test_reaped_signals();
    test_reaped_message_storage();
    test_unroutable_native_port();
    test_end_cleanup();
    test_native_routing();
    test_io_root_wait_busy_child();
    test_io_idle_wait();
    test_io_private_bits();
    test_io_blocked_on_private_port();
    test_io_reaped_copy_storage();
    test_io_poll_failure();
    test_private_io_waiters();
    test_process_handshake();
    test_process_priorities();
    test_process_forbid_wait();
    test_process_signals();
    test_process_dos_guard();
    test_process_quarantine();
    test_process_overlap();
    test_process_failures();
    test_typed_process_startup_messages();
    test_typed_nested_broker();
    test_typed_generated_and_lifetime_limits();
    test_typed_root_process_port();
    test_typed_callback_dos();
    test_typed_root_lifetime();
    test_typed_dos_refusals();
    test_typed_start_failures();
    test_typed_backend_domain(false);
    test_empty_root_process_port();
    test_authoritative_stop_code();
    test_staged_start_recovery();
    test_published_finish_recovery();
    test_backend_parent_and_child(false);
    if (HJ_Available())
    {
        test_backend_parent_and_child(true);
        test_jit_child_faults();
        test_typed_backend_domain(true);
    }
    else
        std::printf("SKIP: JIT parent/child execution needs an AArch64 JIT host\n");
    std::printf("Guest task scheduler: %u/%u checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
