/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Guest tasks of AddTask() through literal Exec LVOs, with the production
    scheduler, run loop and dispatcher compiled as C. Part one runs mock
    CPUs from a mock backend factory, whose instructions are literal
    register and memory effects, over a recording native Exec. Part two
    runs an original 68k parent and child (guest_tasks_68k.s) on real
    Moira CPUs, instruction by instruction.
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
#include <proto/exec.h>
#include "kaffeine_intern.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_callback.h"
#include "kaffeine_sched.h"
#include "kaffeine_messages.h"

#include "Moira/Moira.cpp"
#include "Moira/MoiraDebugger.cpp"
#include "kaffeine_moira.cpp"

static unsigned checks, failures;
static void check(bool ok, const char *what)
{
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
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
    freed_entries.push_back(entry);
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
static constexpr ULONG TCB(unsigned i) { return 0x20000 + 0x100 * i; }
static constexpr ULONG LOWER(unsigned i) { return 0x28000 + 0x400 * i; }
static constexpr ULONG UPPER(unsigned i) { return LOWER(i) + 0x400; }
static constexpr size_t SLICE_LIMIT = 1000000;

static IPTR alloc_signal(KaffeineContext *, void *);
static IPTR free_signal(KaffeineContext *, void *);
static IPTR port_wait(KaffeineContext *, void *);
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
    { 384, port_wait }, { 390, guest_wait }
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

static void test_moira_parent_and_child()
{
    World w;
    w.child(0, 0, "moira child");
    ULONG at = ROOT_CODE;
    for (UWORD word : moira_parent) { m68k_write16(&w.ctx, at, word); at += 2; }
    at = CODE;
    for (UWORD word : moira_child) { m68k_write16(&w.ctx, at, word); at += 2; }
    w.ctx.entry_point = ROOT_CODE;
    LONG result = Kaffeine_Execute(&w.ctx);
    check(result == 42, "Moira: the parent returns the value that the child stored");
    check(w.at(0xA000) == TCB(0) && w.at(0xA010) == ROOT && w.at(0xA014) == TCB(0),
          "Moira: AddTask() returns the Task, FindTask(NULL) is each task's own");
    check(w.at(0xA004) == 0x8000 && w.at(0xA008) == 0x8000 && w.at(0xA018) == 0x4000,
          "Moira: parent and child each resume from Wait() with the other's signal");
    check(!w.ctx.sched && !w.ctx.cpu_factory && allocmems == freemems && !native_recvd &&
          w.at(SYSBASE + 276) == ROOT && w.mem[TCB(0) + 15] == 6 && !native_waits,
          "Moira: the child ended and the program is the root alone again");
}

int main()
{
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
    test_moira_parent_and_child();
    std::printf("Guest task scheduler: %u/%u checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
