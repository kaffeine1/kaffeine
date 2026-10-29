/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Exec ports, I/O requests and signals of 68k code, timer.device,
    keyboard.device and input.device, through literal LVOs and the real
    dispatcher and heap. The native Exec and devices below record every
    call, native signal and native object.
*/
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <proto/exec.h>
#include <proto/timer.h>
#include <proto/keymap.h>
#include <exec/errors.h>
#include <devices/keyboard.h>
#include <devices/input.h>
#include "kaffeine_intern.h"
#include "kaffeine_exec_io.h"
#include "kaffeine_dispatch.h"

/* ── The native side ─────────────────────────────────────────────── */

static std::map<void *, ULONG> native_blocks;
static Task owner_task, other_task, *current_task = &owner_task;
static ULONG task_signals;              /* allocated bits of the native task */
static bool fail_signal, fail_port, fail_request;
static LONG open_error;
static BYTE doio_error;
static unsigned alloc_signal_calls, free_signal_calls, signal_calls;
static Task *signal_task;
static ULONG signal_mask;
static int live_ports, live_requests, live_opens;
static unsigned open_calls, close_calls, doio_calls, getsystime_calls;
static std::string open_name;
static IPTR open_unit;
static ULONG open_flags, open_size, request_size;
static UWORD doio_command;
static ULONG doio_secs, doio_micro;
static Device native_timer, native_keyboard, native_input;
/* keyboard.device: what its DoIO() got, and an io_Actual it may report
   instead of AROS's min(32, io_Length) */
static unsigned kbd_doio_calls, input_doio_calls;
static ULONG kbd_length;
static const void *kbd_data;
static long kbd_actual = -1;
static const UBYTE *arena_lo, *arena_hi;    /* guest memory, never a native address */

static bool in_arena(const void *p)
{
    const UBYTE *b = static_cast<const UBYTE *>(p);
    return b >= arena_lo && b < arena_hi;
}

/* workbench.library is not linked here: no port has App objects. */
extern "C" void Kaffeine_AppPortGone(KaffeineContext *, ULONG) {}
extern "C" void Kaffeine_AppSignalGone(KaffeineContext *, BYTE) {}
extern "C" void Kaffeine_WorkbenchCleanup(KaffeineContext *) {}
extern "C" WORD MapRawKey(InputEvent *, STRPTR, LONG, KeyMap *)
{
    assert(false && "timer fixture must not reach native keymap conversion");
    return -1;
}

extern "C" APTR AllocMem(ULONG size, ULONG)
{
    void *p = calloc(1, size ? size : 1);
    native_blocks[p] = size;
    return p;
}

extern "C" void FreeMem(APTR memory, ULONG size)
{
    if (!memory) return;
    auto it = native_blocks.find(memory);
    assert(it != native_blocks.end() && it->second == size);
    native_blocks.erase(it);
    free(memory);
}

extern "C" BYTE AllocSignal(LONG n)
{
    ++alloc_signal_calls;
    if (fail_signal) return -1;
    if (n >= 0)
    {
        if (n > 31 || (task_signals & (1u << n))) return -1;
        task_signals |= 1u << n;
        return (BYTE)n;
    }
    for (int bit = 31; bit >= 16; --bit)
        if (!(task_signals & (1u << bit)))
        {
            task_signals |= 1u << bit;
            return (BYTE)bit;
        }
    return -1;
}

extern "C" void FreeSignal(LONG n)
{
    ++free_signal_calls;
    if (n < 0) return;
    assert(task_signals & (1u << n));
    task_signals &= ~(1u << n);
}

extern "C" void Signal(Task *task, ULONG mask)
{
    ++signal_calls;
    signal_task = task;
    signal_mask = mask;
}

extern "C" Task *FindTask(CONST_STRPTR name)
{
    assert(!name);
    return current_task;
}

extern "C" MsgPort *CreateMsgPort(void)
{
    if (fail_port) return nullptr;
    auto *port = static_cast<MsgPort *>(AllocMem(sizeof(MsgPort), 0));
    BYTE sig = AllocSignal(-1);
    if (sig < 0)
    {
        FreeMem(port, sizeof(MsgPort));
        return nullptr;
    }
    port->mp_SigBit = (UBYTE)sig;
    port->mp_SigTask = FindTask(nullptr);
    ++live_ports;
    return port;
}

extern "C" void DeleteMsgPort(MsgPort *port)
{
    if (!port) return;
    FreeSignal(port->mp_SigBit);
    FreeMem(port, sizeof(MsgPort));
    --live_ports;
}

extern "C" APTR CreateIORequest(MsgPort *port, ULONG size)
{
    if (fail_request || !port) return nullptr;
    auto *req = static_cast<IORequest *>(AllocMem(size, 0));
    req->io_Message.mn_ReplyPort = port;
    req->io_Message.mn_Length = (UWORD)size;
    request_size = size;
    ++live_requests;
    return req;
}

extern "C" void DeleteIORequest(APTR request)
{
    if (!request) return;
    FreeMem(request, static_cast<IORequest *>(request)->io_Message.mn_Length);
    --live_requests;
}

extern "C" LONG OpenDevice(CONST_STRPTR name, IPTR unit, IORequest *req, ULONG flags)
{
    ++open_calls;
    open_name = name;
    open_unit = unit;
    open_flags = flags;
    open_size = req->io_Message.mn_Length;
    if (open_error)
    {
        req->io_Error = (BYTE)open_error;
        req->io_Device = nullptr;
        return open_error;
    }
    assert(!in_arena(req));
    req->io_Device = open_name == "keyboard.device" ? &native_keyboard :
                     open_name == "input.device" ? &native_input : &native_timer;
    req->io_Unit = reinterpret_cast<void *>(unit);
    req->io_Error = 0;
    ++live_opens;
    return 0;
}

extern "C" void CloseDevice(IORequest *req)
{
    ++close_calls;
    assert(req->io_Device == &native_timer || req->io_Device == &native_keyboard ||
           req->io_Device == &native_input);
    req->io_Device = reinterpret_cast<Device *>(-1);
    --live_opens;
}

/* AROS keyboard.device's KBD_READMATRIX: min(32, io_Length) bytes of the
   matrix, here 0x40, 0x41, ... */
static LONG keyboard_doio(IOStdReq *req)
{
    ++kbd_doio_calls;
    assert(req->io_Command == KBD_READMATRIX && !in_arena(req) && !in_arena(req->io_Data));
    kbd_length = req->io_Length;
    kbd_data = req->io_Data;
    req->io_Actual = req->io_Length < 32 ? req->io_Length : 32;
    for (ULONG i = 0; i < req->io_Actual; ++i)
        static_cast<UBYTE *>(req->io_Data)[i] = (UBYTE)(0x40 + i);
    if (kbd_actual >= 0)
        req->io_Actual = (ULONG)kbd_actual;
    req->io_Flags = IOF_QUICK;
    req->io_Message.mn_Node.ln_Type = NT_MESSAGE;
    req->io_Error = doio_error;
    return doio_error;
}

extern "C" LONG DoIO(IORequest *req)
{
    auto *tr = reinterpret_cast<timerequest *>(req);
    ++doio_calls;
    if (req->io_Device == &native_input)
    {
        ++input_doio_calls;
        return IOERR_NOCMD;
    }
    if (req->io_Device == &native_keyboard)
        return keyboard_doio(reinterpret_cast<IOStdReq *>(req));
    assert(req->io_Device == &native_timer);
    doio_command = req->io_Command;
    doio_secs = tr->tr_time.tv_secs;
    doio_micro = tr->tr_time.tv_micro;
    req->io_Flags = IOF_QUICK;
    req->io_Message.mn_Node.ln_Type = 0;
    if (doio_command == TR_GETSYSTIME)
        tr->tr_time = { 12345, 678901 };
    else if (doio_command == TR_ADDREQUEST)
    {
        /* Queued, then replied: no longer QUICK. */
        req->io_Flags = 0;
        req->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
        tr->tr_time = { 0, 0 };
    }
    req->io_Error = doio_error;
    return doio_error;
}

extern "C" void stub_GetSysTime(Device *base, timeval *dest)
{
    assert(base == &native_timer);
    ++getsystime_calls;
    dest->tv_secs = 100;
    dest->tv_micro = 200;
}

extern "C" ULONG stub_ReadEClock(Device *base, EClockVal *dest)
{
    assert(base == &native_timer);
    dest->ev_hi = 0x12345678;
    dest->ev_lo = 0x9ABCDEF0;
    return 709379;
}

extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *)
{
    assert(false);
    return 0;
}

/* ── The guest side ──────────────────────────────────────────────── */

static unsigned checks, failures, generated_calls;
static void check(bool ok, const char *what)
{
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
}

struct TestCPU
{
    ULONG d[8] = {}, a[8] = {};
    UWORD sr = 0xA715;
    ULONG pc = 0x12345678;
};
static ULONG get_d(void *s, int n) { return static_cast<TestCPU *>(s)->d[n]; }
static ULONG get_a(void *s, int n) { return static_cast<TestCPU *>(s)->a[n]; }
static void set_d(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->d[n] = v; }
static void set_a(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->a[n] = v; }
static UWORD get_sr(void *s) { return static_cast<TestCPU *>(s)->sr; }
static void set_sr(void *s, UWORD v) { static_cast<TestCPU *>(s)->sr = v; }
static ULONG get_pc(void *s) { return static_cast<TestCPU *>(s)->pc; }
static void set_pc(void *s, ULONG v) { static_cast<TestCPU *>(s)->pc = v; }
static const KaffeineCPUOps ops =
    { get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, nullptr, nullptr };

static IPTR poison(KaffeineContext *, void *) { ++generated_calls; return 0xBAD0F00D; }
static const M68KThunkEntry manual[] = { KAFFEINE_EXEC_IO_THUNKS };
static const M68KThunkEntry generated[] =
{
    {324, poison}, {330, poison}, {336, poison}, {444, poison}, {450, poison},
    {456, poison}, {462, poison}, {468, poison}, {474, poison}, {480, poison},
    {654, poison}, {660, poison}, {666, poison}, {672, poison}
};

/* Guest offsets of the NDK structures. */
enum
{
    LN_TYPE = 8, LN_NAME = 10, MN_REPLYPORT = 14, MN_LENGTH = 18,
    IO_DEVICE = 20, IO_UNIT = 24, IO_COMMAND = 28, IO_FLAGS = 30, IO_ERROR = 31,
    TV_SECS = 32, TV_MICRO = 36,
    IO_ACTUAL = 32, IO_LENGTH = 36, IO_DATA = 40, IO_OFFSET = 44, IOSTDREQ = 48,
    MP_FLAGS = 14, MP_SIGBIT = 15, MP_SIGTASK = 16, MP_MSGLIST = 20
};

struct Fixture
{
    static constexpr ULONG base = 0x1000, stack = 0xF00, task = 0x2000;
    static constexpr ULONG name = 0x2400, storage = 0x2500, copy = 0x2600;
    static constexpr ULONG kbd_name = 0x2420, input_name = 0x2440;
    static constexpr ULONG initial_d0 = 0xD0123456;
    KaffeineContext ctx = {};
    std::array<UBYTE, 0x10000> memory;
    TestCPU state;
    KaffeineCPU cpu = { &ops, &state };
    ULONG heap_avail;

    Fixture()
    {
        task_signals = 0xFFFF;
        current_task = &owner_task;
        fail_signal = fail_port = fail_request = false;
        open_error = 0; doio_error = 0;
        alloc_signal_calls = free_signal_calls = signal_calls = 0;
        signal_task = nullptr; signal_mask = 0;
        live_ports = live_requests = live_opens = 0;
        open_calls = close_calls = doio_calls = getsystime_calls = 0;
        native_timer.dd_Library.lib_Version = 41;
        native_timer.dd_Library.lib_Revision = 2;
        native_keyboard.dd_Library.lib_Version = 41;
        native_keyboard.dd_Library.lib_Revision = 3;
        native_input.dd_Library.lib_Version = 41;
        native_input.dd_Library.lib_Revision = 4;
        kbd_doio_calls = input_doio_calls = 0;
        kbd_length = 0; kbd_data = nullptr; kbd_actual = -1;
        memory.fill(0xA5);
        ctx.mem = memory.data();
        ctx.mem_size = memory.size();
        arena_lo = memory.data();
        arena_hi = memory.data() + memory.size();
        ctx.m68k_sysbase = base;
        ctx.heap_start = 0x4000;
        ctx.heap_end = 0xF000;
        assert(Kaffeine_HeapInit(&ctx));
        heap_avail = Kaffeine_HeapAvail(&ctx, 0);
        ctx.num_libs = 1;
        auto &lib = ctx.libs[0];
        lib.m68k_addr = base;
        lib.jt_start = base - 6 * 512;
        lib.pos_size = 632;
        std::strcpy(lib.name, "exec.library");
        lib.thunks = manual;
        lib.num_thunks = std::size(manual);
        lib.gen_thunks = generated;
        lib.num_gen_thunks = std::size(generated);
        m68k_write32(&ctx, base + 276, task);
        m68k_write32(&ctx, stack, 0x76543210);
        text(name, "timer.device");
        text(kbd_name, "keyboard.device");
        text(input_name, "input.device");
    }

    ~Fixture()
    {
        current_task = &owner_task;
        ctx.exec_task = nullptr;
        Kaffeine_IOCleanup(&ctx);
        assert(!live_ports && !live_requests && !live_opens);
        assert(task_signals == 0xFFFF);
        arena_lo = arena_hi = nullptr;
    }

    void text(ULONG addr, const char *s)
    {
        std::strcpy(reinterpret_cast<char *>(memory.data() + addr), s);
    }

    /* A literal LVO through the dispatcher; only D0 may change. */
    ULONG call_at(ULONG lib_base, ULONG lvo, ULONG a0 = 0xA0A0A0A0, ULONG a1 = 0xA1A1A1A1,
                  ULONG d0 = initial_d0, ULONG d1 = 0xD1D1D1D1)
    {
        for (unsigned i = 0; i < 8; ++i)
        {
            state.d[i] = 0xD0000000 + i;
            state.a[i] = 0xA0000000 + i;
        }
        state.d[0] = d0; state.d[1] = d1;
        state.a[0] = a0; state.a[1] = a1; state.a[7] = stack;
        const auto before = state;
        ctx.running = TRUE; ctx.exit_code = 0;
        generated_calls = 0;
        KaffeineDispatchResult result = {};
        assert(Kaffeine_DispatchLibraryCall(&ctx, &cpu, lib_base - lvo, &result));
        assert(result.return_pc == 0x76543210 && !result.supervisor);
        assert(state.a[7] == stack + 4);
        for (unsigned i = 1; i < 8; ++i) assert(state.d[i] == before.d[i]);
        for (unsigned i = 0; i < 7; ++i) assert(state.a[i] == before.a[i]);
        assert(state.sr == before.sr && state.pc == before.pc);
        assert(generated_calls == 0);
        return state.d[0];
    }
    ULONG call(ULONG lvo, ULONG a0 = 0xA0A0A0A0, ULONG a1 = 0xA1A1A1A1,
               ULONG d0 = initial_d0, ULONG d1 = 0xD1D1D1D1)
    {
        return call_at(base, lvo, a0, a1, d0, d1);
    }
    ULONG timer(ULONG lvo, ULONG a0 = 0xA0A0A0A0, ULONG a1 = 0xA1A1A1A1)
    {
        assert(ctx.timer_base);
        return call_at(ctx.timer_base, lvo, a0, a1);
    }
    bool stopped() const { return !ctx.running && ctx.exit_code == 20; }

    ULONG port() { return call(666); }
    ULONG request(ULONG reply, ULONG size) { return call(654, reply, 0xA1A1A1A1, size); }
    ULONG open(ULONG req, ULONG unit = UNIT_MICROHZ, ULONG dev = name, ULONG flags = 0)
    {
        return call(444, dev, req, unit, flags);
    }
    ULONG doio(ULONG req, UWORD command, ULONG secs = 0, ULONG micro = 0)
    {
        m68k_write16(&ctx, req + IO_COMMAND, command);
        m68k_write32(&ctx, req + TV_SECS, secs);
        m68k_write32(&ctx, req + TV_MICRO, micro);
        return call(456, 0xA0A0A0A0, req);
    }
    BYTE error(ULONG req) const { return (BYTE)memory[req + IO_ERROR]; }
    /* DoIO() of an IOStdReq: only the command, io_Length and io_Data change. */
    ULONG std_io(ULONG req, UWORD command, ULONG length, ULONG data)
    {
        m68k_write16(&ctx, req + IO_COMMAND, command);
        m68k_write32(&ctx, req + IO_LENGTH, length);
        m68k_write32(&ctx, req + IO_DATA, data);
        return call(456, 0xA0A0A0A0, req);
    }
    bool fill_is(ULONG addr, ULONG size, UBYTE value) const
    {
        for (ULONG i = 0; i < size; ++i)
            if (memory[addr + i] != value) return false;
        return true;
    }
    bool matrix_is(ULONG addr, ULONG size) const
    {
        for (ULONG i = 0; i < size; ++i)
            if (memory[addr + i] != (UBYTE)(0x40 + i)) return false;
        return true;
    }
};

static void test_signals()
{
    Fixture f;
    check(f.call(330, 0, 0, 0xFFFFFFFF) == 31 && f.ctx.guest_signals == 0x80000000u,
          "AllocSignal(-1) takes the highest free native bit for the guest");
    check(f.call(330, 0, 0, 20) == 20 && f.ctx.guest_signals == 0x80100000u,
          "AllocSignal(20) takes that bit");
    check(f.call(330, 0, 0, 20) == 0xFFFFFFFF && f.ctx.guest_signals == 0x80100000u,
          "AllocSignal() of a taken bit returns -1 in all of D0");
    check(f.call(330, 0, 0, 0x1FF) == 30,
          "AllocSignal() reads the low byte of D0 as signed: 0x1FF is -1");
    const unsigned frees = free_signal_calls;
    f.call(336, 0, 0, 5);
    check(free_signal_calls == frees && (task_signals & (1u << 5)),
          "FreeSignal() of a bit the guest did not allocate leaves the native task alone");
    f.call(336, 0, 0, 0x114);
    check(free_signal_calls == frees + 1 && !(task_signals & (1u << 20)) &&
          f.ctx.guest_signals == 0xC0000000u,
          "FreeSignal(0x114) frees bit 20, from the low byte of D0");
    f.call(336, 0, 0, 20);
    f.call(336, 0, 0, 0xFFFFFFFF);
    check(free_signal_calls == frees + 1, "FreeSignal() of a freed bit or -1 does nothing");
    fail_signal = true;
    check(f.call(330, 0, 0, 0xFFFFFFFF) == 0xFFFFFFFF && f.ctx.guest_signals == 0xC0000000u,
          "a failed native AllocSignal() returns -1 and records no bit");
}

static void test_signal_task()
{
    Fixture f;
    f.call(324, 0, Fixture::task, 0x80000000);
    check(signal_calls == 1 && signal_task == &owner_task && signal_mask == 0x80000000,
          "Signal() of the current guest task signals the native task that runs the guest");
    current_task = &other_task;
    f.call(324, 0, Fixture::task, 1);
    check(signal_calls == 2 && signal_task == &other_task,
          "the running native task is the one signalled, even when it changed");
    current_task = &owner_task;
    check(f.call(324, 0, 0x3000, 1) == 20 && f.stopped() && signal_calls == 2,
          "Signal() of another task stops the guest with 20");
    check(f.call(324, 0, 0, 1) == 20 && f.stopped() && signal_calls == 2,
          "Signal() of NULL stops the guest with 20");
}

static void test_ports()
{
    Fixture f;
    const ULONG p = f.port();
    const ULONG list = p + MP_MSGLIST;
    check(p && f.memory[p + LN_TYPE] == NT_MSGPORT && f.memory[p + 9] == 0 &&
          !m68k_read32(&f.ctx, p + LN_NAME) && f.memory[p + MP_FLAGS] == PA_SIGNAL &&
          f.memory[p + MP_SIGBIT] == 31 && m68k_read32(&f.ctx, p + MP_SIGTASK) == Fixture::task,
          "CreateMsgPort() makes a cleared signalling 68k port of the current task");
    check(m68k_read32(&f.ctx, list) == list + 4 && !m68k_read32(&f.ctx, list + 4) &&
          m68k_read32(&f.ctx, list + 8) == list && f.memory[list + 12] == 0 &&
          f.memory[list + 13] == 0,
          "its message list is empty, as NEWLIST leaves it");
    check(f.ctx.guest_signals == 0x80000000u && (task_signals & 0x80000000u) &&
          !live_ports, "the port owns a native signal of the guest, and no native port");
    f.memory[p + MP_SIGBIT] = 5;
    f.call(672, p);
    check(!(task_signals & 0x80000000u) && (task_signals & (1u << 5)) && !f.ctx.guest_signals &&
          Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail,
          "DeleteMsgPort() frees the signal it gave, not the one the guest wrote, and the port");
    const unsigned frees = free_signal_calls;
    f.call(672, 0);
    f.call(672, p);
    f.call(672, 0x3000);
    check(free_signal_calls == frees && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail,
          "DeleteMsgPort() of NULL, a deleted port or a guest port frees nothing");

    const ULONG q = f.port();
    f.call(336, 0, 0, f.memory[q + MP_SIGBIT]);
    f.call(672, q);
    check(free_signal_calls == frees + 1, "a signal the guest freed first is not freed twice");

    fail_signal = true;
    check(f.port() == 0 && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail,
          "CreateMsgPort() without a free signal returns NULL and frees the port");
    fail_signal = false;
    std::vector<ULONG> ports;
    for (unsigned i = 0; i < KAFFEINE_MAX_IO_PORTS; ++i)
        ports.push_back(f.port());
    bool all = true;
    for (ULONG port : ports) all = all && port;
    const unsigned allocs = alloc_signal_calls;
    check(all && f.port() == 0 && alloc_signal_calls == allocs,
          "a full port table makes CreateMsgPort() return NULL before allocating");
    for (ULONG port : ports) f.call(672, port);
    check(task_signals == 0xFFFF && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail,
          "deleting every port returns every signal and every byte");
}

static void test_requests()
{
    Fixture f;
    const ULONG p = f.port();
    check(f.request(0, 48) == 0 && f.request(p, 19) == 0 && f.request(p, 0x10000) == 0,
          "CreateIORequest() refuses a NULL port and sizes outside 20 to 65535");
    const ULONG r = f.request(p, 40);
    bool cleared = true;
    for (ULONG i = 0; i < 40; ++i)
        if (i < MN_REPLYPORT || i >= MN_LENGTH + 2) cleared = cleared && !f.memory[r + i];
    check(r && m68k_read32(&f.ctx, r + MN_REPLYPORT) == p && m68k_read16(&f.ctx, r + MN_LENGTH) == 40 &&
          cleared, "CreateIORequest() makes a cleared request with its port and length");
    const ULONG s = f.request(p, 20);
    check(s && m68k_read16(&f.ctx, s + MN_LENGTH) == 20, "a Message-sized request is allowed");
    const ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);
    f.call(660, 0);
    f.call(660, Fixture::storage);
    check(Kaffeine_HeapAvail(&f.ctx, 0) == avail,
          "DeleteIORequest() of NULL or of guest storage frees nothing");
    f.call(660, r);
    f.call(660, s);
    f.call(660, r);
    f.call(672, p);
    check(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail, "DeleteIORequest() frees each request once");
}

static void test_open_close()
{
    Fixture f;
    const ULONG p = f.port();
    const ULONG r = f.request(p, 40);
    f.text(0x2300, "serial.device");
    m68k_write32(&f.ctx, r + IO_DEVICE, 0x55555555);
    m68k_write32(&f.ctx, r + IO_UNIT, 0x66666666);
    check(f.open(r, 0, 0x2300) == 0xFFFFFFFF && f.error(r) == IOERR_OPENFAIL &&
          !m68k_read32(&f.ctx, r + IO_DEVICE) && !m68k_read32(&f.ctx, r + IO_UNIT) &&
          !open_calls && !live_ports, "OpenDevice() of another device fails with IOERR_OPENFAIL");
    check(f.open(r, 0, 1) == 0xFFFFFFFF && !open_calls,
          "OpenDevice() name 1, input.device, fails on a request smaller than an IOStdReq");
    check(f.open(r, 0, 0x7FFFFFFF) == 0xFFFFFFFF && !open_calls,
          "OpenDevice() with a name outside guest memory fails");

    const ULONG small = f.request(p, 32);
    check(f.open(small) == 0xFFFFFFFF && f.error(small) == IOERR_OPENFAIL && !open_calls,
          "a request smaller than a timerequest cannot open timer.device");

    check(f.open(r, UNIT_MICROHZ, Fixture::name, 0x55) == 0 && f.error(r) == 0, "OpenDevice(timer.device)");
    const ULONG tb = f.ctx.timer_base;
    check(open_calls == 1 && open_name == "timer.device" && open_unit == UNIT_MICROHZ &&
          open_flags == 0x55 && request_size == sizeof(timerequest) && open_size == sizeof(timerequest) &&
          live_ports == 1 && live_requests == 1 && live_opens == 1,
          "the native open gets a native timerequest of the native size and its own reply port");
    check(tb && m68k_read32(&f.ctx, r + IO_DEVICE) == tb && m68k_read32(&f.ctx, r + IO_UNIT) == 0,
          "the guest request gets the guest timer.device base and unit 0");
    check(f.memory[tb + LN_TYPE] == NT_DEVICE &&
          !std::strcmp(reinterpret_cast<char *>(f.memory.data() + m68k_read32(&f.ctx, tb + LN_NAME)),
                       "timer.device") &&
          m68k_read16(&f.ctx, tb + 16) == 72 && m68k_read16(&f.ctx, tb + 18) == 34 &&
          m68k_read16(&f.ctx, tb + 20) == 41 && m68k_read16(&f.ctx, tb + 22) == 2,
          "the guest base is a Device named timer.device, with the native version");
    check(!Kaffeine_FindLibBase(&f.ctx, "timer.device") && !std::strcmp(f.ctx.libs[1].name, "timer.device"),
          "OpenLibrary() does not find the device base, which the call trace names");

    const ULONG storage = Fixture::storage;
    std::memset(f.memory.data() + storage, 0, 40);
    check(f.open(storage, UNIT_VBLANK) == 0 && f.ctx.timer_base == tb &&
          m68k_read32(&f.ctx, storage + IO_UNIT) == UNIT_VBLANK && live_opens == 2,
          "a request in guest storage opens unit VBLANK on the same guest base");
    check(f.call(444, 0, 0xFFDC, 0, 0) == 0xFFFFFFFF && open_calls == 2,
          "guest storage without room for a timerequest cannot open it");
    check(f.call(444, 0, 0xFFF8, 0, 0) == 20 && f.stopped() && open_calls == 2,
          "a request outside guest memory stops the guest");

    const ULONG k = f.request(p, 40);
    check(f.call(444, 0, k, UNIT_ECLOCK, 0) == 0 && open_name == "timer.device" &&
          m68k_read32(&f.ctx, k + IO_UNIT) == UNIT_ECLOCK,
          "OpenDevice() name 0 opens timer.device, as Exec's V40 kludge");

    open_error = IOERR_OPENFAIL;
    const ULONG e = f.request(p, 40);
    check(f.open(e, 9) == 0xFFFFFFFF && f.error(e) == IOERR_OPENFAIL &&
          !m68k_read32(&f.ctx, e + IO_DEVICE) && live_ports == 3 && live_requests == 3,
          "a failed native open deletes its native request and port");
    open_error = 0;
    fail_port = true;
    check(f.open(e) == 0xFFFFFFFF && live_ports == 3, "without a native port the open fails");
    fail_port = false;
    fail_request = true;
    check(f.open(e) == 0xFFFFFFFF && live_ports == 3 && live_requests == 3,
          "without a native request the open fails and deletes the port");
    fail_request = false;

    const unsigned closes = close_calls;
    check(f.open(r, UNIT_WAITUNTIL) == 0 && close_calls == closes + 1 && live_opens == 3 &&
          m68k_read32(&f.ctx, r + IO_UNIT) == UNIT_WAITUNTIL,
          "opening an open request again closes the native open behind it first");

    f.call(450, 0xA0A0A0A0, r);
    check(close_calls == closes + 2 && live_opens == 2 && live_requests == 2 && live_ports == 2 &&
          m68k_read32(&f.ctx, r + IO_DEVICE) == 0xFFFFFFFF,
          "CloseDevice() closes and deletes the native request and port; io_Device becomes -1");
    f.call(450, 0xA0A0A0A0, r);
    f.call(450, 0xA0A0A0A0, 0);
    f.call(450, 0xA0A0A0A0, e);
    check(close_calls == closes + 2, "CloseDevice() of a closed, NULL or failed request does nothing");
    f.call(660, k);
    check(close_calls == closes + 3 && live_opens == 1,
          "DeleteIORequest() of an open request closes the device first");
    f.call(450, 0xA0A0A0A0, storage);
    check(live_opens == 0 && !live_ports && !live_requests, "every native object is gone");
    for (ULONG req : { r, small, e }) f.call(660, req);
    f.call(672, p);
    check(task_signals == 0xFFFF && Kaffeine_HeapAvail(&f.ctx, 0) < f.heap_avail,
          "the guest base and its name stay in the heap; everything else is returned");
}

static void test_doio()
{
    Fixture f;
    const ULONG p = f.port();
    const ULONG r = f.request(p, 40);
    f.open(r);
    check(f.doio(r, TR_GETSYSTIME, 7, 8) == 0 && doio_calls == 1 && doio_command == TR_GETSYSTIME &&
          m68k_read32(&f.ctx, r + TV_SECS) == 12345 && m68k_read32(&f.ctx, r + TV_MICRO) == 678901 &&
          f.memory[r + IO_FLAGS] == IOF_QUICK && f.memory[r + LN_TYPE] == 0 && f.error(r) == 0,
          "DoIO(TR_GETSYSTIME) returns both time fields, completed QUICK");
    check(f.doio(r, TR_ADDREQUEST, 3, 250000) == 0 && doio_calls == 2 &&
          doio_command == TR_ADDREQUEST && doio_secs == 3 && doio_micro == 250000 &&
          !m68k_read32(&f.ctx, r + TV_SECS) && !m68k_read32(&f.ctx, r + TV_MICRO) &&
          f.memory[r + IO_FLAGS] == 0 && f.memory[r + LN_TYPE] == NT_REPLYMSG,
          "DoIO(TR_ADDREQUEST) passes both time fields and returns the replied request");
    doio_error = IOERR_ABORTED;
    check(f.doio(r, TR_GETSYSTIME) == 0xFFFFFFFE && f.error(r) == IOERR_ABORTED,
          "a native error comes back in io_Error and sign-extended in D0");
    doio_error = 0;
    check(f.doio(r, TR_SETSYSTIME, 9, 10) == 0xFFFFFFFD && f.error(r) == IOERR_NOCMD &&
          doio_calls == 3 && m68k_read32(&f.ctx, r + TV_SECS) == 9 &&
          f.memory[r + IO_FLAGS] == IOF_QUICK && f.memory[r + LN_TYPE] == 0,
          "TR_SETSYSTIME ends with IOERR_NOCMD and never sets the system clock");
    check(f.doio(r, 2) == 0xFFFFFFFD && f.doio(r, 0x4000) == 0xFFFFFFFD && doio_calls == 3,
          "CMD_READ and NSCMD_DEVICEQUERY end with IOERR_NOCMD");

    const ULONG e = f.request(p, 40);
    f.open(e, UNIT_ECLOCK);
    check(f.doio(e, TR_ADDREQUEST, 0, 100) == 0xFFFFFFFD && doio_calls == 3,
          "an EClock delay ends with IOERR_NOCMD, as the AROS driver never replies to it");
    check(f.doio(e, TR_GETSYSTIME) == 0 && doio_calls == 4, "TR_GETSYSTIME works on any unit");

    check(f.call(456, 0xA0A0A0A0, 0) == 0xFFFFFFFF && doio_calls == 4, "DoIO(NULL) returns -1");
    const ULONG unopened = f.request(p, 40);
    check(f.doio(unopened, TR_GETSYSTIME) == 0xFFFFFFFF && f.memory[unopened + IO_FLAGS] == IOF_QUICK &&
          doio_calls == 4, "DoIO() without a device returns -1, as Exec");
    std::memcpy(f.memory.data() + Fixture::copy, f.memory.data() + r, 40);
    check(f.call(456, 0xA0A0A0A0, Fixture::copy) == 20 && f.stopped() && doio_calls == 4,
          "DoIO() on a copy of an open request stops the guest with 20");
    check(f.call(456, 0xA0A0A0A0, 0xFFF0) == 20 && f.stopped(),
          "DoIO() on a request outside guest memory stops the guest");
    for (ULONG req : { r, e, unopened }) f.call(660, req);
    f.call(672, p);
}

static void test_async()
{
    Fixture f;
    const ULONG p = f.port();
    const ULONG r = f.request(p, 40);
    f.open(r);
    f.doio(r, TR_ADDREQUEST, 1, 0);
    check(f.call(462, 0xA0A0A0A0, r) == 20 && f.stopped() && doio_calls == 1,
          "SendIO() stops the guest with 20");
    check(f.call(468, 0xA0A0A0A0, r) == r, "CheckIO() of a completed request returns it");
    check(f.call(474, 0xA0A0A0A0, r) == 0, "WaitIO() of a completed request returns io_Error");
    f.memory[r + IO_ERROR] = 0xFD;
    check(f.call(474, 0xA0A0A0A0, r) == 0xFFFFFFFD, "WaitIO() sign-extends io_Error");
    check(f.call(480, 0xA0A0A0A0, r) == 0xFFFFFFFF, "AbortIO() finds nothing in flight: -1");
    f.memory[r + IO_FLAGS] = 0;
    f.memory[r + LN_TYPE] = NT_MESSAGE;
    check(f.call(468, 0xA0A0A0A0, r) == 0, "CheckIO() of a request in flight returns NULL");
    check(f.call(474, 0xA0A0A0A0, r) == 20 && f.stopped(),
          "WaitIO() of a request in flight stops the guest with 20");
    for (ULONG lvo : { 468, 474, 480 })
        check(f.call(lvo, 0xA0A0A0A0, 0xFFF0) == 20 && f.stopped(),
              "CheckIO(), WaitIO() and AbortIO() refuse a request outside guest memory");
    f.call(660, r);
    f.call(672, p);
}

static void timeval_at(Fixture &f, ULONG addr, ULONG secs, ULONG micro)
{
    m68k_write32(&f.ctx, addr, secs);
    m68k_write32(&f.ctx, addr + 4, micro);
}
static bool timeval_is(Fixture &f, ULONG addr, ULONG secs, ULONG micro)
{
    return m68k_read32(&f.ctx, addr) == secs && m68k_read32(&f.ctx, addr + 4) == micro;
}

static void test_timer_base()
{
    Fixture f;
    const ULONG p = f.port();
    const ULONG r = f.request(p, 40);
    f.open(r);
    const ULONG a = 0x2700, b = 0x2708;

    timeval_at(f, a, 1, 900000); timeval_at(f, b, 2, 300000);
    f.timer(42, a, b);
    check(timeval_is(f, a, 4, 200000) && timeval_is(f, b, 2, 300000), "AddTime() adds and normalizes");
    timeval_at(f, a, 1, 600000);
    f.timer(42, a, a);
    check(timeval_is(f, a, 3, 200000), "AddTime() of a timeval to itself doubles it");
    timeval_at(f, a, 0, 3500000); timeval_at(f, b, 0, 0);
    f.timer(42, a, b);
    check(timeval_is(f, a, 3, 500000), "AddTime() normalizes several seconds of microseconds");

    timeval_at(f, a, 5, 100000); timeval_at(f, b, 2, 300000);
    f.timer(48, a, b);
    check(timeval_is(f, a, 2, 800000), "SubTime() borrows a second");
    timeval_at(f, a, 5, 0); timeval_at(f, b, 1, 1500000);
    f.timer(48, a, b);
    check(timeval_is(f, b, 2, 500000) && timeval_is(f, a, 2, 500000),
          "SubTime() normalizes src in place, as AROS does");
    timeval_at(f, a, 4, 2500000);
    f.timer(48, a, a);
    check(timeval_is(f, a, 0, 0), "SubTime() of a timeval from itself gives zero");

    timeval_at(f, a, 2, 5); timeval_at(f, b, 1, 9);
    check(f.timer(54, a, b) == 0xFFFFFFFF && f.timer(54, b, a) == 1,
          "CmpTime() returns -1 when dest is later and 1 when src is");
    timeval_at(f, b, 2, 9);
    check(f.timer(54, a, b) == 1 && f.timer(54, a, a) == 0, "CmpTime() compares microseconds on equal seconds");

    f.timer(66, a);
    check(timeval_is(f, a, 100, 200) && getsystime_calls == 1, "GetSysTime() of the native timer");
    check(f.timer(60, b) == 709379 && timeval_is(f, b, 0x12345678, 0x9ABCDEF0),
          "ReadEClock() returns the frequency and both halves");

    check(f.timer(30, 0xA0A0A0A0, r) == 20 && f.stopped(), "BeginIO() on the device stops the guest");
    check(f.timer(36, 0xA0A0A0A0, r) == 0xFFFFFFFF, "AbortIO() on the device returns -1");
    for (ULONG lvo : { 42, 48, 54 })
        check(f.timer(lvo, 0xFFFC, b) == 20 && f.stopped(), "a timeval outside guest memory stops the guest");
    for (ULONG lvo : { 60, 66 })
        check(f.timer(lvo, 0xFFFC) == 20 && f.stopped(), "an output outside guest memory stops the guest");
    f.call(660, r);
    f.call(672, p);
}

/* The low 32 bits of no native device or request anywhere in a guest
   Device and its jump table. */
static bool no_native_pointer(Fixture &f, ULONG base, ULONG vectors)
{
    std::vector<uintptr_t> natives = { (uintptr_t)&native_timer, (uintptr_t)&native_keyboard,
                                       (uintptr_t)&native_input };
    for (const auto &entry : f.ctx.io_requests)
        if (entry.native) natives.push_back((uintptr_t)entry.native);
    for (ULONG a = base - vectors * 6; a + 4 <= base + 34; a += 2)
        for (uintptr_t p : natives)
            if (m68k_read32(&f.ctx, a) == (ULONG)p)
                return false;
    return true;
}

static bool device_named(Fixture &f, ULONG base, const char *name, UWORD neg, UWORD revision)
{
    return base && f.memory[base + LN_TYPE] == NT_DEVICE &&
           !std::strcmp(reinterpret_cast<char *>(f.memory.data() + m68k_read32(&f.ctx, base + LN_NAME)),
                        name) &&
           m68k_read16(&f.ctx, base + 16) == neg && m68k_read16(&f.ctx, base + 18) == 34 &&
           m68k_read16(&f.ctx, base + 20) == 41 && m68k_read16(&f.ctx, base + 22) == revision;
}

static void test_input_open_close()
{
    Fixture f;
    const ULONG p = f.port();
    const ULONG r = f.request(p, IOSTDREQ), small = f.request(p, 40);

    for (ULONG unit : { 1u, 2u, 0xFFFFFFFFu })
    {
        m68k_write32(&f.ctx, r + IO_DEVICE, 0x55555555);
        check(f.open(r, unit, Fixture::kbd_name) == 0xFFFFFFFF && f.error(r) == IOERR_OPENFAIL &&
              !m68k_read32(&f.ctx, r + IO_DEVICE) && !open_calls && !live_ports && !live_requests,
              "keyboard.device units other than 0 fail to open before any native call");
        check(f.open(r, unit, Fixture::input_name) == 0xFFFFFFFF && f.call(444, 1, r, unit, 0) == 0xFFFFFFFF &&
              !open_calls && !live_ports, "input.device units other than 0 fail to open too");
    }
    check(f.open(small, 0, Fixture::kbd_name) == 0xFFFFFFFF && f.error(small) == IOERR_OPENFAIL &&
          f.open(small, 0, Fixture::input_name) == 0xFFFFFFFF && !open_calls && !live_ports,
          "a request smaller than an IOStdReq opens neither device");
    check(f.call(444, Fixture::kbd_name, 0xFFF0, 0, 0) == 20 && f.stopped() && !open_calls,
          "a request outside guest memory stops the guest");
    check(f.call(444, Fixture::kbd_name, 0xFFD8, 0, 0) == 0xFFFFFFFF && !open_calls,
          "guest storage without room for an IOStdReq at the end of memory cannot open it");

    m68k_write32(&f.ctx, r + IO_UNIT, 0x66666666);
    check(f.open(r, 0, Fixture::kbd_name, 0x12) == 0 && f.error(r) == 0 && open_calls == 1 &&
          open_name == "keyboard.device" && open_unit == 0 && open_flags == 0x12 &&
          request_size == sizeof(IOStdReq) && open_size == sizeof(IOStdReq) &&
          live_ports == 1 && live_requests == 1 && live_opens == 1,
          "OpenDevice(keyboard.device, 0) opens a native IOStdReq with its own reply port");
    const ULONG kb = f.ctx.keyboard_base;
    check(m68k_read32(&f.ctx, r + IO_DEVICE) == kb && !m68k_read32(&f.ctx, r + IO_UNIT) &&
          device_named(f, kb, "keyboard.device", 36, 3) && no_native_pointer(f, kb, 6),
          "the request gets a guest keyboard.device of six vectors and unit 0, with no native pointer");
    check(f.ctx.io_requests[0].guest == r && f.ctx.io_requests[0].kind == KAFFEINE_IO_KEYBOARD &&
          f.ctx.io_requests[0].unit == 0 && !in_arena(f.ctx.io_requests[0].native),
          "the private record holds the kind and the native request");
    check(!Kaffeine_FindLibBase(&f.ctx, "keyboard.device"), "OpenLibrary() does not find the device");

    const ULONG s = f.request(p, IOSTDREQ);
    check(f.call(444, 1, s, 0, 0) == 0 && open_name == "input.device" && open_unit == 0 &&
          live_opens == 2, "OpenDevice() name 1 opens input.device unit 0");
    const ULONG in = f.ctx.input_base;
    check(in && in != kb && m68k_read32(&f.ctx, s + IO_DEVICE) == in &&
          device_named(f, in, "input.device", 42, 4) && no_native_pointer(f, in, 7) &&
          f.ctx.io_requests[2].kind == KAFFEINE_IO_INPUT,
          "input.device gets its own guest base of seven vectors");
    const ULONG storage = Fixture::storage;
    std::memset(f.memory.data() + storage, 0, IOSTDREQ);
    check(f.open(storage, 0, Fixture::input_name) == 0 && f.ctx.input_base == in &&
          m68k_read32(&f.ctx, storage + IO_DEVICE) == in && live_opens == 3,
          "an IOStdReq in guest storage opens input.device on the same guest base");

    const unsigned closes = close_calls;
    check(f.open(r, 0, Fixture::input_name) == 0 && close_calls == closes + 1 && live_opens == 3 &&
          m68k_read32(&f.ctx, r + IO_DEVICE) == in && f.ctx.io_requests[0].kind == KAFFEINE_IO_INPUT,
          "opening an open keyboard request on input.device closes the keyboard first");
    check(f.open(r, 0, Fixture::kbd_name) == 0 && close_calls == closes + 2 &&
          m68k_read32(&f.ctx, r + IO_DEVICE) == kb && f.ctx.keyboard_base == kb, "and back");

    /* A failed native open, port or request leaves nothing behind. */
    const ULONG e = f.request(p, IOSTDREQ);
    open_error = IOERR_OPENFAIL;
    check(f.open(e, 0, Fixture::kbd_name) == 0xFFFFFFFF && f.error(e) == IOERR_OPENFAIL &&
          !m68k_read32(&f.ctx, e + IO_DEVICE) && live_ports == 3 && live_requests == 3 && live_opens == 3,
          "a failed native keyboard open deletes its native request and port");
    std::memset(f.memory.data() + Fixture::copy, 0, IOSTDREQ);
    bool kept = false;
    check(f.open(Fixture::copy, 0, Fixture::input_name) == 0xFFFFFFFF && live_ports == 3,
          "a failed input open deletes its native request and port");
    for (const auto &entry : f.ctx.io_requests) kept = kept || entry.guest == Fixture::copy;
    check(!kept, "a failed open in guest storage keeps no record");
    open_error = 0;
    fail_port = true;
    check(f.open(e, 0, Fixture::input_name) == 0xFFFFFFFF && live_ports == 3, "without a native port");
    fail_port = false;
    fail_request = true;
    check(f.open(e, 0, Fixture::kbd_name) == 0xFFFFFFFF && live_ports == 3 && live_requests == 3,
          "without a native request");
    fail_request = false;

    f.call(450, 0xA0A0A0A0, r);
    check(close_calls == closes + 3 && live_opens == 2 && live_ports == 2 && live_requests == 2 &&
          m68k_read32(&f.ctx, r + IO_DEVICE) == 0xFFFFFFFF && f.ctx.io_requests[0].kind == KAFFEINE_IO_NONE,
          "CloseDevice() of keyboard closes and deletes everything; io_Device becomes -1");
    f.call(450, 0xA0A0A0A0, r);
    check(close_calls == closes + 3, "a second CloseDevice() does nothing");
    f.call(660, s);
    check(close_calls == closes + 4 && live_opens == 1, "DeleteIORequest() of an open input request closes it");
    f.call(450, 0xA0A0A0A0, storage);
    check(!live_opens && !live_ports && !live_requests && f.ctx.keyboard_base == kb && f.ctx.input_base == in,
          "every native object is gone; the guest bases stay");
    for (ULONG req : { r, small, e }) f.call(660, req);
    f.call(672, p);
}

static void test_input_base_rollback()
{
    Fixture f;
    const ULONG p = f.port();
    const ULONG r = f.request(p, IOSTDREQ);
    const ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);
    f.ctx.num_libs = KAFFEINE_MAX_LIBS;
    check(f.open(r, 0, Fixture::kbd_name) == 0xFFFFFFFF && f.error(r) == IOERR_OPENFAIL &&
          open_calls == 1 && close_calls == 1 && !live_opens && live_ports == 0 && live_requests == 0 &&
          !f.ctx.keyboard_base && !m68k_read32(&f.ctx, r + IO_DEVICE) &&
          Kaffeine_HeapAvail(&f.ctx, 0) == avail,
          "without room for the guest base the native open is closed and the name freed");
    check(f.open(r, 0, Fixture::input_name) == 0xFFFFFFFF && close_calls == 2 && !f.ctx.input_base &&
          Kaffeine_HeapAvail(&f.ctx, 0) == avail, "the same for input.device");
    f.ctx.num_libs = 1;
    check(f.open(r, 0, Fixture::kbd_name) == 0 && f.ctx.keyboard_base && f.ctx.num_libs == 2,
          "with room the next open makes the base");
    f.call(660, r);
    f.call(672, p);
}

static void test_keyboard_matrix()
{
    Fixture f;
    const ULONG p = f.port();
    const ULONG r = f.request(p, IOSTDREQ);
    f.open(r, 0, Fixture::kbd_name);
    const ULONG kb = f.ctx.keyboard_base, buf = 0x2810;
    m68k_write32(&f.ctx, r + IO_ACTUAL, 0x11111111);
    m68k_write32(&f.ctx, r + IO_OFFSET, 0x13579BDF);
    /* Classic alib CreateExtIO sets NT_MESSAGE before the first DoIO. */
    f.memory[r + LN_TYPE] = NT_MESSAGE;
    f.memory[r + IO_FLAGS] = 0;

    check(f.std_io(r, KBD_READMATRIX, 16, buf) == 0 && kbd_doio_calls == 1 && kbd_length == 16 &&
          kbd_data && !in_arena(kbd_data) && f.matrix_is(buf, 16) && f.fill_is(buf - 16, 16, 0xA5) &&
          f.fill_is(buf + 16, 32, 0xA5),
          "KBD_READMATRIX of 16 bytes reads a native buffer and copies exactly 16 bytes");
    check(m68k_read32(&f.ctx, r + IO_ACTUAL) == 16 && f.error(r) == 0 &&
          m68k_read32(&f.ctx, r + IO_LENGTH) == 16 && m68k_read32(&f.ctx, r + IO_DATA) == buf &&
          m68k_read32(&f.ctx, r + IO_OFFSET) == 0x13579BDF &&
          m68k_read16(&f.ctx, r + IO_COMMAND) == KBD_READMATRIX &&
          m68k_read32(&f.ctx, r + IO_DEVICE) == kb && !m68k_read32(&f.ctx, r + IO_UNIT) &&
          m68k_read32(&f.ctx, r + MN_REPLYPORT) == p && m68k_read16(&f.ctx, r + MN_LENGTH) == IOSTDREQ &&
          f.memory[r + IO_FLAGS] == IOF_QUICK && f.memory[r + LN_TYPE] == NT_MESSAGE,
          "io_Actual and io_Error come back; length, data, offset, device, unit and port stay");
    check(f.ctx.io_requests[0].native && !static_cast<IOStdReq *>(
              static_cast<void *>(f.ctx.io_requests[0].native))->io_Data,
          "the native request keeps no pointer to the temporary buffer");

    std::memset(f.memory.data() + buf, 0xA5, 64);
    check(f.std_io(r, KBD_READMATRIX, 13, buf) == 0 && kbd_length == 13 && f.matrix_is(buf, 13) &&
          f.fill_is(buf + 13, 32, 0xA5) && m68k_read32(&f.ctx, r + IO_ACTUAL) == 13,
          "a classic 13-byte matrix gets 13 bytes");
    for (ULONG length : { 33u, 0x100u, 0x7FFFFFFFu, 0xFFFFFFFFu })
    {
        std::memset(f.memory.data() + buf, 0xA5, 64);
        check(f.std_io(r, KBD_READMATRIX, length, buf) == 0 && kbd_length == 32 &&
              f.matrix_is(buf, 32) && f.fill_is(buf + 32, 16, 0xA5) &&
              m68k_read32(&f.ctx, r + IO_ACTUAL) == 32 && m68k_read32(&f.ctx, r + IO_LENGTH) == length,
              "a longer io_Length passes the native maximum of 32 bytes and gets 32");
    }
    const unsigned calls = kbd_doio_calls;
    check(f.std_io(r, KBD_READMATRIX, 0, 0) == 0 && kbd_doio_calls == calls + 1 && kbd_length == 0 &&
          !m68k_read32(&f.ctx, r + IO_ACTUAL), "a zero length needs no io_Data and copies nothing");
    check(f.std_io(r, KBD_READMATRIX, 32, 0xFFE0) == 0 && f.matrix_is(0xFFE0, 32),
          "the matrix may end at the last byte of guest memory");

    std::memset(f.memory.data() + buf, 0xA5, 64);
    kbd_actual = 99;
    check(f.std_io(r, KBD_READMATRIX, 8, buf) == 0 && m68k_read32(&f.ctx, r + IO_ACTUAL) == 8 &&
          f.matrix_is(buf, 8) && f.fill_is(buf + 8, 32, 0xA5),
          "a native io_Actual past the length is clamped, and no more bytes are copied");
    kbd_actual = -1;
    doio_error = IOERR_ABORTED;
    std::memset(f.memory.data() + buf, 0xA5, 64);
    check(f.std_io(r, KBD_READMATRIX, 16, buf) == 0xFFFFFFFE && f.error(r) == IOERR_ABORTED &&
          m68k_read32(&f.ctx, r + IO_ACTUAL) == 16 && f.fill_is(buf, 64, 0xA5),
          "a native error comes back with io_Actual, and no bytes");
    doio_error = 0;

    /* Malformed buffers: io_Error IOERR_BADADDRESS, before the native device. */
    const ULONG storage = Fixture::storage;
    std::memset(f.memory.data() + storage, 0, IOSTDREQ);
    f.open(storage, 0, Fixture::kbd_name);
    /* Output storage cannot overwrite bridge snapshots or active queues. */
    const ULONG queue=0x2900, message=0x2A00, task_shadow=0x2C00, font=0x2E00;
    std::memset(f.memory.data()+queue,0,34);
    std::memset(f.memory.data()+message,0,20);
    f.memory[queue+LN_TYPE]=4;f.memory[queue+14]=2;
    m68k_write32(&f.ctx,queue+20,message);m68k_write32(&f.ctx,queue+28,message);
    m68k_write32(&f.ctx,message,queue+24);m68k_write32(&f.ctx,message+4,queue+20);
    f.memory[message+LN_TYPE]=NT_MESSAGE;m68k_write16(&f.ctx,message+MN_LENGTH,20);
    f.ctx.message_ports[0]=queue;
    f.ctx.shadow_map[0]={task_shadow,&owner_task,258};f.ctx.num_shadows=1;
    f.ctx.fonts[0].guest=font;f.ctx.fonts[0].allocation_size=64;
    struct { ULONG req, length, data; const char *what; } bad[] = {
        { r, 4, 0, "io_Data NULL" },
        { r, 16, 0xFFFC, "a matrix past the end of guest memory" },
        { r, 32, 0xFFFFFFF0, "an io_Data that wraps around" },
        { r, 1, 0x10000, "an io_Data at the end of guest memory" },
        { r, 0xFFFFFFFF, 0xFFF0, "a clamped length still past the end" },
        { r, 1, p, "an idle owned port" },
        { r, 1, queue+20, "an active queue header" },
        { r, 1, message+8, "an active message body" },
        { r, 1, task_shadow+227, "the final byte of a Process snapshot" },
        { r, 1, font+63, "the final byte of a guest font block" },
        { storage, 16, storage + 40, "a matrix over the end of the request" },
        { storage, 16, storage - 8, "a matrix over the start of the request" },
        { storage, 1, storage, "a matrix at the request" },
        { storage, 1, storage + IOSTDREQ - 1, "a matrix at its last byte" },
    };
    for (const auto &b : bad)
    {
        std::memset(f.memory.data() + 0xFFC0, 0xA5, 0x40);
        std::memset(f.memory.data() + buf, 0xA5, 64);
        m68k_write32(&f.ctx, b.req + IO_ACTUAL, 0x22222222);
        const unsigned before = kbd_doio_calls;
        check(f.std_io(b.req, KBD_READMATRIX, b.length, b.data) == 0xFFFFFFFB &&
              f.error(b.req) == -5 && kbd_doio_calls == before &&
              m68k_read32(&f.ctx, b.req + IO_ACTUAL) == 0x22222222 &&
              m68k_read32(&f.ctx, b.req + IO_DATA) == b.data &&
              m68k_read32(&f.ctx, b.req + IO_LENGTH) == b.length &&
              f.fill_is(0xFFC0, 0x40, 0xA5) && f.fill_is(buf, 64, 0xA5), b.what);
    }
    f.ctx.message_ports[0]=0;f.ctx.num_shadows=0;f.ctx.fonts[0]={};
    check(f.std_io(storage, KBD_READMATRIX, 16, storage + IOSTDREQ) == 0 &&
          f.matrix_is(storage + IOSTDREQ, 16) && f.std_io(storage, KBD_READMATRIX, 16, storage - 16) == 0 &&
          f.matrix_is(storage - 16, 16) && m68k_read16(&f.ctx, storage + IO_COMMAND) == KBD_READMATRIX,
          "a matrix right before or after the request is fine");

    /* Other keyboard commands: IOERR_NOCMD, no native call, io_Data untouched. */
    for (UWORD command : { (UWORD)CMD_INVALID, (UWORD)CMD_READ, (UWORD)CMD_CLEAR, (UWORD)KBD_READEVENT,
                           (UWORD)KBD_ADDRESETHANDLER, (UWORD)KBD_REMRESETHANDLER,
                           (UWORD)KBD_RESETHANDLERDONE, (UWORD)0x4000 })
    {
        std::memset(f.memory.data() + buf, 0xA5, 64);
        const unsigned before = doio_calls;
        m68k_write32(&f.ctx, r + IO_ACTUAL, 0x33333333);
        check(f.std_io(r, command, 22, buf) == 0xFFFFFFFD && f.error(r) == IOERR_NOCMD &&
              doio_calls == before && f.fill_is(buf, 64, 0xA5) &&
              m68k_read32(&f.ctx, r + IO_ACTUAL) == 0x33333333 && f.memory[r + IO_FLAGS] == IOF_QUICK,
              "another keyboard command ends with IOERR_NOCMD before the native device");
    }
    f.call(450, 0xA0A0A0A0, storage);
    f.call(660, r);
    f.call(672, p);
}

static void test_keyboard_identity()
{
    Fixture f;
    const ULONG p = f.port();
    const ULONG r = f.request(p, IOSTDREQ), t = f.request(p, 40), s = f.request(p, IOSTDREQ);
    f.open(t);
    f.open(r, 0, Fixture::kbd_name);
    f.open(s, 0, Fixture::input_name);
    const ULONG kb = f.ctx.keyboard_base, buf = 0x2810;
    const unsigned before = doio_calls;
    auto refused = [&](const char *what) {
        std::memset(f.memory.data() + buf, 0xA5, 64);
        f.memory[r + IO_ERROR] = 0x77;
        check(f.std_io(r, KBD_READMATRIX, 16, buf) == 20 && f.stopped() && doio_calls == before &&
              f.memory[r + IO_ERROR] == 0x77 && f.fill_is(buf, 64, 0xA5), what);
    };
    for (ULONG device : { f.ctx.timer_base, f.ctx.input_base, 0u, kb + 2, 0xFFFFFFFFu })
    {
        m68k_write32(&f.ctx, r + IO_DEVICE, device);
        refused("a request that names another device stops the guest before the native device");
    }
    m68k_write32(&f.ctx, r + IO_DEVICE, kb);
    m68k_write32(&f.ctx, r + IO_UNIT, 1);
    refused("a request that names another unit stops the guest");
    m68k_write32(&f.ctx, r + IO_UNIT, 0);

    /* Timer, copies and forged requests that name the keyboard base. */
    m68k_write32(&f.ctx, t + IO_DEVICE, kb);
    check(f.doio(t, TR_GETSYSTIME) == 0 && doio_calls == before + 1 && !kbd_doio_calls,
          "a timer request stays a timer request, whatever device it names");
    std::memcpy(f.memory.data() + Fixture::copy, f.memory.data() + r, IOSTDREQ);
    check(f.std_io(Fixture::copy, KBD_READMATRIX, 16, buf) == 20 && f.stopped() && !kbd_doio_calls,
          "DoIO() on a copy of an open keyboard request stops the guest");
    const ULONG forged = f.request(p, IOSTDREQ);
    m68k_write32(&f.ctx, forged + IO_DEVICE, kb);
    check(f.std_io(forged, KBD_READMATRIX, 16, buf) == 20 && f.stopped() && !kbd_doio_calls,
          "a request never opened that names the keyboard base stops the guest");
    m68k_write32(&f.ctx, forged + IO_DEVICE, 0);
    check(f.std_io(forged, KBD_READMATRIX, 16, buf) == 0xFFFFFFFF && !kbd_doio_calls,
          "one without a device returns -1, as Exec");
    check(f.std_io(r, KBD_READMATRIX, 16, buf) == 0 && kbd_doio_calls == 1,
          "the real request still works");
    check(f.std_io(s, KBD_READMATRIX, 16, buf) == 0xFFFFFFFD && kbd_doio_calls == 1 && !input_doio_calls,
          "the record's kind decides: the matrix command of an input request is IOERR_NOCMD");
    f.call(450, 0xA0A0A0A0, r);
    check(f.std_io(r, KBD_READMATRIX, 16, buf) == 20 && f.stopped() && kbd_doio_calls == 1,
          "a closed keyboard request stops the guest");
    for (ULONG req : { r, t, s, forged }) f.call(660, req);
    f.call(672, p);
}

static void test_input_commands()
{
    Fixture f;
    const ULONG p = f.port();
    const ULONG r = f.request(p, IOSTDREQ);
    f.open(r, 0, Fixture::input_name);
    const ULONG hook = 0x2900;          /* a guest Interrupt and InputEvent */
    for (UWORD command : { (UWORD)IND_ADDHANDLER, (UWORD)IND_REMHANDLER, (UWORD)IND_WRITEEVENT,
                           (UWORD)IND_SETTHRESH, (UWORD)IND_SETPERIOD, (UWORD)IND_SETMPORT,
                           (UWORD)IND_SETMTYPE, (UWORD)IND_SETMTRIG, (UWORD)IND_ADDEVENT,
                           (UWORD)CMD_READ, (UWORD)CMD_WRITE, (UWORD)CMD_RESET, (UWORD)0x4000 })
    {
        std::memset(f.memory.data() + hook, 0xA5, 0x40);
        m68k_write32(&f.ctx, hook + 18, 0x2950);     /* is_Code */
        m68k_write32(&f.ctx, r + IO_ACTUAL, 0x44444444);
        m68k_write32(&f.ctx, r + IO_OFFSET, 0x55555555);
        const unsigned before = doio_calls;
        const ULONG ret = f.std_io(r, command, 22, hook);
        check(ret == 0xFFFFFFFD && f.error(r) == IOERR_NOCMD && doio_calls == before &&
              !input_doio_calls && f.memory[r + IO_FLAGS] == IOF_QUICK && f.memory[r + LN_TYPE] == 0 &&
              m68k_read32(&f.ctx, r + IO_ACTUAL) == 0x44444444 && m68k_read32(&f.ctx, r + IO_LENGTH) == 22 &&
              m68k_read32(&f.ctx, r + IO_DATA) == hook && m68k_read32(&f.ctx, r + IO_OFFSET) == 0x55555555 &&
              m68k_read32(&f.ctx, hook + 18) == 0x2950 && f.fill_is(hook, 18, 0xA5) &&
              f.fill_is(hook + 22, 0x40 - 22, 0xA5) && f.ctx.running,
              "an input.device command ends with IOERR_NOCMD; the guest hook is never touched");
    }
    m68k_write32(&f.ctx, r + IO_UNIT, 1);
    check(f.std_io(r, IND_ADDHANDLER, 22, hook) == 20 && f.stopped() && !input_doio_calls,
          "an input request that names another unit stops the guest");
    m68k_write32(&f.ctx, r + IO_UNIT, 0);
    f.memory[r + IO_FLAGS] = 0;
    f.memory[r + LN_TYPE] = NT_MESSAGE;
    check(f.std_io(r, IND_WRITEEVENT, 22, hook) == 0xFFFFFFFD && !f.stopped() && !input_doio_calls,
          "an initial CreateExtIO marker is not native pending I/O; unsupported input stays NOCMD");
    f.memory[r + IO_FLAGS] = IOF_QUICK;
    f.call(660, r);
    f.call(672, p);
}

static void test_input_vectors()
{
    Fixture f;
    const ULONG p = f.port();
    const ULONG k = f.request(p, IOSTDREQ), i = f.request(p, IOSTDREQ);
    f.open(k, 0, Fixture::kbd_name);
    f.open(i, 0, Fixture::input_name);
    const unsigned opens = open_calls, closes = close_calls, doios = doio_calls;
    for (ULONG lvo : { 6, 12, 18, 24, 30, 36 })
        check(f.call_at(f.ctx.keyboard_base, lvo, 0xA0A0A0A0, k) == 20 && f.stopped(),
              "every keyboard.device vector called directly stops the guest");
    for (ULONG lvo : { 6, 12, 18, 24, 30, 36, 42 })
        check(f.call_at(f.ctx.input_base, lvo, 0xA0A0A0A0, i) == 20 && f.stopped(),
              "every input.device vector, PeekQualifier too, stops the guest");
    check(open_calls == opens && close_calls == closes && doio_calls == doios && live_opens == 2,
          "no direct vector reaches a native device");
    check(f.call(462, 0xA0A0A0A0, k) == 20 && f.call(462, 0xA0A0A0A0, i) == 20 &&
          doio_calls == doios, "SendIO() stops the guest for both devices");
    check(f.call(468, 0xA0A0A0A0, k) == k && f.call(480, 0xA0A0A0A0, i) == 0xFFFFFFFF,
          "CheckIO() and AbortIO() of complete requests as for timer");

    /* Cleanup closes both; the guest bases stay for the next open. */
    const ULONG kb = f.ctx.keyboard_base, in = f.ctx.input_base;
    const UWORD libs = f.ctx.num_libs;
    Kaffeine_IOCleanup(&f.ctx);
    check(!live_opens && !live_ports && !live_requests && close_calls == closes + 2,
          "cleanup closes the keyboard and input opens");
    bool clear = true;
    for (const auto &entry : f.ctx.io_requests)
        clear = clear && !entry.guest && !entry.native && entry.kind == KAFFEINE_IO_NONE;
    check(clear, "cleanup clears every record");
    const ULONG q = f.port();
    const ULONG r = f.request(q, IOSTDREQ);
    check(f.open(r, 0, Fixture::kbd_name) == 0 && f.ctx.keyboard_base == kb && f.ctx.input_base == in &&
          f.ctx.num_libs == libs && m68k_read32(&f.ctx, r + IO_DEVICE) == kb,
          "a later open reuses the cached guest base");
}

static void test_cleanup()
{
    {
        Fixture f;
        const ULONG p = f.port();
        const ULONG r = f.request(p, 40), s = f.request(p, 40);
        f.open(r);
        f.open(s, UNIT_VBLANK);
        /* The native reply ports took bits 30 and 29: the guest gets 28. */
        const ULONG own = 0x80000000u | (1u << f.call(330, 0, 0, 0xFFFFFFFF));
        current_task = &other_task;
        f.ctx.exec_task = &owner_task;
        const unsigned closes = close_calls, frees = free_signal_calls;
        Kaffeine_IOCleanup(&f.ctx);
        check(own == 0x90000000u && close_calls == closes && free_signal_calls == frees &&
              live_opens == 2 && f.ctx.guest_signals == own,
              "cleanup outside the task that ran the guest touches nothing");
        current_task = &owner_task;
        Kaffeine_IOCleanup(&f.ctx);
        check(close_calls == closes + 2 && !live_opens && !live_ports && !live_requests &&
              !f.ctx.guest_signals && task_signals == 0xFFFF,
              "cleanup in that task closes every device and frees every guest signal");
        bool clear = true;
        for (const auto &entry : f.ctx.io_requests) clear = clear && !entry.guest;
        for (const auto &entry : f.ctx.io_ports) clear = clear && !entry.guest;
        check(clear, "cleanup empties the port and request tables");
    }
}

int main()
{
    test_signals();
    test_signal_task();
    test_ports();
    test_requests();
    test_open_close();
    test_doio();
    test_async();
    test_timer_base();
    test_input_open_close();
    test_input_base_rollback();
    test_keyboard_matrix();
    test_keyboard_identity();
    test_input_commands();
    test_input_vectors();
    test_cleanup();
    std::printf("Exec I/O, timer.device, keyboard.device and input.device: %u/%u checks passed\n",
                checks - failures, checks);
    return failures ? 1 : 0;
}
