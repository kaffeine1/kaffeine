/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    console.device through literal Exec and Device LVOs, the production CPU
    accessors, dispatcher, heap and adapters. Native collaborators record
    calls and allocations; guest layouts use independent literal bytes.
*/
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <exec/errors.h>
#include <proto/exec.h>
#include <proto/keymap.h>
#include <proto/timer.h>
#include "kaffeine_dispatch.h"
#include "kaffeine_exec_io.h"
#include "kaffeine_intern.h"
#include "kaffeine_thunks_console.h"

static unsigned checks;
static void require(bool ok, const char *message)
{
    ++checks;
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

enum NativeKind { TIMER, CONSOLE };
struct NativeRequest { ULONG size; NativeKind kind; bool open; };
static std::map<APTR, ULONG> blocks;
static std::map<IORequest *, NativeRequest> requests;
static std::map<MsgPort *, Task *> ports;
static Task owner, other, *current = &owner;
static Device timer_device, console_device;
static ULONG signals;
static int fail_allocation;
static unsigned allocation_calls, open_calls, close_calls, doio_calls, poison_calls, key_calls;
static bool fail_port, fail_request, fail_signal;
static LONG open_error;
static IPTR opened_unit;
static ULONG opened_size, opened_flags;
static std::string opened_name;

extern "C" void Kaffeine_AppPortGone(KaffeineContext *, ULONG) {}
extern "C" void Kaffeine_AppSignalGone(KaffeineContext *, BYTE) {}
extern "C" void Kaffeine_WorkbenchCleanup(KaffeineContext *) {}
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *)
{
    require(false, "native library fallback is poisoned");
    return 0;
}

extern "C" APTR AllocMem(ULONG size, ULONG flags)
{
    ++allocation_calls;
    if (fail_allocation > 0 && --fail_allocation == 0) return nullptr;
    APTR p = std::malloc(size ? size : 1);
    require(p != nullptr, "host allocation succeeds");
    std::memset(p, flags & MEMF_CLEAR ? 0 : 0xA5, size);
    require(blocks.emplace(p, size).second, "native allocation is unique");
    return p;
}
extern "C" void FreeMem(APTR p, ULONG size)
{
    if (!p) return;
    auto it = blocks.find(p);
    require(it != blocks.end() && it->second == size, "native free matches its allocation");
    blocks.erase(it);
    std::free(p);
}
extern "C" BYTE AllocSignal(LONG bit)
{
    if (fail_signal) return -1;
    if (bit < 0)
        for (bit = 31; bit >= 0 && (signals & (1u << bit)); --bit) {}
    if (bit < 0 || bit > 31 || (signals & (1u << bit))) return -1;
    signals |= 1u << bit;
    return static_cast<BYTE>(bit);
}
extern "C" void FreeSignal(LONG bit)
{
    require(bit >= 0 && bit < 32 && (signals & (1u << bit)), "only an allocated signal is freed");
    signals &= ~(1u << bit);
}
extern "C" Task *FindTask(CONST_STRPTR name)
{
    require(name == nullptr, "only the current task is queried");
    return current;
}
extern "C" void Signal(Task *, ULONG) { require(false, "unexpected Signal"); }
extern "C" MsgPort *CreateMsgPort()
{
    if (fail_port) return nullptr;
    auto *p = static_cast<MsgPort *>(AllocMem(sizeof(MsgPort), MEMF_CLEAR));
    if (!p) return nullptr;
    BYTE bit = AllocSignal(-1);
    if (bit < 0) { FreeMem(p, sizeof(MsgPort)); return nullptr; }
    p->mp_SigBit = static_cast<UBYTE>(bit);
    p->mp_SigTask = current;
    require(ports.emplace(p, current).second, "reply port is new");
    return p;
}
extern "C" void DeleteMsgPort(MsgPort *p)
{
    require(p && ports.count(p) && ports.at(p) == current, "reply port is freed by its owner");
    for (const auto &[request, info] : requests)
        require(request->io_Message.mn_ReplyPort != p, "native request goes before its reply port");
    ports.erase(p);
    FreeSignal(p->mp_SigBit);
    FreeMem(p, sizeof(MsgPort));
}
extern "C" APTR CreateIORequest(MsgPort *p, ULONG size)
{
    require(p && ports.count(p), "native request has a native reply port");
    if (fail_request) return nullptr;
    auto *r = static_cast<IORequest *>(AllocMem(size, MEMF_CLEAR));
    if (!r) return nullptr;
    r->io_Message.mn_ReplyPort = p;
    r->io_Message.mn_Length = static_cast<UWORD>(size);
    require(requests.emplace(r, NativeRequest{size, CONSOLE, false}).second, "native request is new");
    return r;
}
extern "C" void DeleteIORequest(APTR p)
{
    auto *r = static_cast<IORequest *>(p);
    auto it = requests.find(r);
    require(it != requests.end() && !it->second.open, "native request is closed before deletion");
    ULONG size = it->second.size;
    requests.erase(it);
    FreeMem(p, size);
}
extern "C" LONG OpenDevice(CONST_STRPTR name, IPTR unit, IORequest *r, ULONG flags)
{
    ++open_calls;
    require(requests.count(r) && !requests.at(r).open, "OpenDevice receives a native request");
    opened_name = name; opened_unit = unit; opened_flags = flags;
    opened_size = requests.at(r).size;
    require(r->io_Message.mn_Length == opened_size && !r->io_Device && !r->io_Unit &&
            !r->io_Command && !r->io_Flags, "native request starts cleared with its native length");
    auto &info = requests.at(r);
    if (opened_name == "console.device")
    {
        require(unit == static_cast<IPTR>(-1), "CONU_LIBRARY is -1 at native pointer width");
        require(opened_size == sizeof(IOStdReq), "console opens a native IOStdReq of native size");
        info.kind = CONSOLE;
    }
    else
    {
        require(opened_name == "timer.device" && opened_size == sizeof(timerequest),
                "other supported native opens use a native timerequest");
        info.kind = TIMER;
    }
    if (open_error) { r->io_Error = static_cast<BYTE>(open_error); return open_error; }
    r->io_Device = info.kind == CONSOLE ? &console_device : &timer_device;
    /* Deliberately pointer-like: neither this nor the native Device may escape. */
    r->io_Unit = reinterpret_cast<APTR>(static_cast<IPTR>(0xABCDEF0123456789ull));
    info.open = true;
    return 0;
}
extern "C" void CloseDevice(IORequest *r)
{
    ++close_calls;
    require(requests.count(r) && requests.at(r).open, "native open is balanced exactly once");
    auto &info = requests.at(r);
    require(r->io_Device == (info.kind == CONSOLE ? &console_device : &timer_device),
            "close uses the original native device despite guest edits");
    info.open = false;
}
extern "C" LONG DoIO(IORequest *r)
{
    ++doio_calls;
    require(requests.count(r) && requests.at(r).kind == TIMER && requests.at(r).open,
            "console requests never reach native DoIO or a timerequest cast");
    require(r->io_Command == TR_GETSYSTIME || r->io_Command == TR_ADDREQUEST,
            "native timer gets only supported commands");
    auto *tr = reinterpret_cast<timerequest *>(r);
    tr->tr_time = {0x10203040, 0x000ABCDE};
    r->io_Flags = IOF_QUICK;
    r->io_Message.mn_Node.ln_Type = 0;
    return 0;
}
extern "C" void stub_GetSysTime(Device *, timeval *) { require(false, "unexpected GetSysTime"); }
extern "C" ULONG stub_ReadEClock(Device *, EClockVal *) { require(false, "unexpected ReadEClock"); return 0; }

struct KeyCall
{
    InputEvent event;
    IPTR event_address, output_address;
    LONG length;
    KeyMap *keymap;
};
static KeyCall key_call;
static std::vector<UBYTE> key_bytes;
static WORD key_result;
extern "C" WORD MapRawKey(InputEvent *event, STRPTR buffer, LONG length, KeyMap *keymap)
{
    ++key_calls;
    key_call = {*event, reinterpret_cast<IPTR>(event), reinterpret_cast<IPTR>(buffer), length, keymap};
    require(key_bytes.size() <= static_cast<ULONG>(length), "collaborator writes within its given length");
    if (!key_bytes.empty()) std::memcpy(buffer, key_bytes.data(), key_bytes.size());
    return key_result;
}

struct State { ULONG d[8], a[8]; UWORD sr; ULONG pc; };
static ULONG get_d(void *s, int n) { return static_cast<State *>(s)->d[n]; }
static ULONG get_a(void *s, int n) { return static_cast<State *>(s)->a[n]; }
static void set_d(void *s, int n, ULONG v) { static_cast<State *>(s)->d[n] = v; }
static void set_a(void *s, int n, ULONG v) { static_cast<State *>(s)->a[n] = v; }
static UWORD get_sr(void *s) { return static_cast<State *>(s)->sr; }
static void set_sr(void *s, UWORD v) { static_cast<State *>(s)->sr = v; }
static ULONG get_pc(void *s) { return static_cast<State *>(s)->pc; }
static void set_pc(void *s, ULONG v) { static_cast<State *>(s)->pc = v; }
static const KaffeineCPUOps ops = {get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, nullptr, nullptr};
static IPTR poison(KaffeineContext *, void *) { ++poison_calls; return 0xBAD0F00D; }
static const M68KThunkEntry exec_manual[] = { KAFFEINE_EXEC_IO_THUNKS };
static const M68KThunkEntry exec_poison[] = {
    {444, poison}, {450, poison}, {456, poison}, {462, poison}, {468, poison},
    {474, poison}, {480, poison}, {654, poison}, {660, poison}
};
static const M68KThunkEntry device_poison[] = {
    {6, poison}, {12, poison}, {18, poison}, {24, poison}, {30, poison}, {36, poison},
    {42, poison}, {48, poison}, {54, poison}, {60, poison}, {66, poison}, {72, poison}
};

struct Fixture
{
    static constexpr ULONG arena_size = 0x20000, stack = 0x200;
    static constexpr ULONG name = 0x3100, timer_name = 0x3120, storage = 0x3400;
    static constexpr ULONG event = 0x3500, output = 0x3600;
    KaffeineContext &ctx;
    UBYTE *memory;
    State state = {};
    KaffeineCPU cpu = {&ops, &state};
    UWORD sr = 0xA71F;

    Fixture() : ctx(*static_cast<KaffeineContext *>(AllocMem(sizeof(KaffeineContext), MEMF_CLEAR))),
                memory(static_cast<UBYTE *>(AllocMem(arena_size + KAFFEINE_MEM_GUARD, 0)))
    {
        ctx.mem = memory; ctx.mem_size = arena_size; ctx.cpu = &cpu;
        ctx.heap_start = 0x11000; ctx.heap_end = 0x1F000;
        require(Kaffeine_HeapInit(&ctx), "production heap initializes");
        ctx.m68k_sysbase = Kaffeine_SetupLibBase(&ctx, 0, "exec.library", 512,
            exec_manual, std::size(exec_manual), exec_poison, std::size(exec_poison));
        require(ctx.m68k_sysbase != 0, "production Exec base initializes");
        m68k_write32(&ctx, ctx.m68k_sysbase + 276, 0x3000);
        std::strcpy(reinterpret_cast<char *>(memory + name), "console.device");
        std::strcpy(reinterpret_cast<char *>(memory + timer_name), "timer.device");
        timer_device.dd_Library.lib_Version = 41; timer_device.dd_Library.lib_Revision = 2;
        console_device.dd_Library.lib_Version = 41; console_device.dd_Library.lib_Revision = 9;
        std::memset(memory + storage, 0, 48);
    }
    ~Fixture()
    {
        current = &owner;
        Kaffeine_IOCleanup(&ctx);
        Kaffeine_DestroyContext(&ctx);
    }
    ULONG call_at(ULONG base, ULONG lvo, ULONG a0 = 0, ULONG a1 = 0,
                  ULONG d0 = 0xD0123456, ULONG d1 = 0, ULONG a2 = 0)
    {
        for (unsigned n = 0; n < 8; ++n) { state.d[n] = 0xD0000000 + n; state.a[n] = 0xA0000000 + n; }
        state.d[0] = d0; state.d[1] = d1;
        state.a[0] = a0; state.a[1] = a1; state.a[2] = a2; state.a[7] = stack;
        state.sr = sr; state.pc = 0x12345678;
        State saved = state;
        m68k_write32(&ctx, stack, 0x87654320);
        ctx.running = TRUE; ctx.exit_code = 0;
        KaffeineDispatchResult result = {};
        unsigned poisons = poison_calls;
        require(Kaffeine_DispatchLibraryCall(&ctx, &cpu, base - lvo, &result), "literal LVO is dispatched");
        require(poison_calls == poisons, "no generated native fallback is used");
        require(result.return_pc == 0x87654320 && !result.supervisor && state.a[7] == stack + 4,
                "dispatcher preserves its return convention");
        require(state.sr == saved.sr && state.pc == saved.pc, "SR and PC are preserved");
        for (unsigned n = 1; n < 8; ++n) require(state.d[n] == saved.d[n], "all other data registers are preserved");
        for (unsigned n = 0; n < 7; ++n) require(state.a[n] == saved.a[n], "all other address registers are preserved");
        return state.d[0];
    }
    ULONG exec(ULONG lvo, ULONG a0 = 0, ULONG a1 = 0, ULONG d0 = 0, ULONG d1 = 0)
    { return call_at(ctx.m68k_sysbase, lvo, a0, a1, d0, d1); }
    ULONG open(ULONG req = storage, ULONG unit = 0xFFFFFFFF, ULONG flags = 0, bool timer = false)
    { return exec(444, timer ? timer_name : name, req, unit, flags); }
    ULONG request(ULONG size) { return exec(654, 0x3300, 0, size); }
    void close(ULONG req = storage) { exec(450, 0, req); }
    void erase(ULONG req) { exec(660, req); }
    ULONG console(ULONG lvo, ULONG a0 = event, ULONG a1 = output, ULONG length = 8, ULONG keymap = 0)
    {
        require(ctx.console_base != 0, "console base exists");
        auto &lib = ctx.libs[ctx.num_libs - 1];
        require(lib.m68k_addr == ctx.console_base, "console is the last base in this fixture");
        lib.gen_thunks = device_poison; lib.num_gen_thunks = std::size(device_poison);
        return call_at(ctx.console_base, lvo, a0, a1, 0xD0123456, length, keymap);
    }
    bool stopped() const { return !ctx.running && ctx.exit_code == 20; }
};

static void test_library_opens()
{
    Fixture f;
    const unsigned opens = open_calls;
    const std::array<UBYTE, 4> success_device = {0, 0, 0x12, 0xC0};
    require(f.open() == 0 && open_calls == opens + 1 && opened_name == "console.device" && !opened_flags,
            "zeroed stack IOStdReq without port or mn_Length opens CONU_LIBRARY");
    require(opened_size == sizeof(IOStdReq) && sizeof(IOStdReq) > 48 &&
            sizeof(void *) > 4, "host exercises a wider native IOStdReq");
    require(f.ctx.console_base == 0x12C0 && !std::memcmp(f.memory + Fixture::storage + 20,
            success_device.data(), 4), "io_Device contains the canonical guest Device bytes");
    const std::array<UBYTE, 4> guest_unit = {0xFF, 0xFF, 0xFF, 0xFF};
    require(!std::memcmp(f.memory + Fixture::storage + 24, guest_unit.data(), 4) &&
            f.memory[Fixture::storage + 31] == 0, "io_Unit is the guest -1 token and io_Error is zero");
    require(!m68k_read16(&f.ctx, Fixture::storage + 18) && !m68k_read32(&f.ctx, Fixture::storage + 14),
            "guest mn_Length and NULL reply port remain untouched");
    ULONG base = f.ctx.console_base;
    require(f.memory[base + 8] == NT_DEVICE && m68k_read16(&f.ctx, base + 16) == 72 &&
            m68k_read16(&f.ctx, base + 18) == 34 && m68k_read16(&f.ctx, base + 20) == 41 &&
            m68k_read16(&f.ctx, base + 22) == 9, "console base has a Device layout and native version");
    require(!std::strcmp(reinterpret_cast<char *>(f.memory + m68k_read32(&f.ctx, base + 10)), "console.device"),
            "console base has the canonical guest name");
    require(!Kaffeine_FindLibBase(&f.ctx, "console.device") && f.ctx.num_libmap == 1 &&
            !std::strcmp(f.ctx.libs[1].name, "console.device"), "Device is traceable outside OpenLibrary's namespace");
    for (ULONG vector = 6; vector <= 72; vector += 6)
        require(m68k_read16(&f.ctx, base - vector) == 0xA000 &&
                m68k_read16(&f.ctx, base - vector + 2) == 0x4E71,
                "all device vectors use the production guest trap layout");
    ULONG name_addr = m68k_read32(&f.ctx, base + 10);
    ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);
    f.memory[Fixture::storage + 31] = 0xF9;
    f.memory[Fixture::storage + 30] = 0xA5;
    std::array<UBYTE, 48> before_close;
    std::memcpy(before_close.data(), f.memory + Fixture::storage, before_close.size());
    std::memset(before_close.data() + 20, 0, 8);
    f.close();
    require(requests.empty() && ports.empty() && !signals &&
            !m68k_read32(&f.ctx, Fixture::storage + 20) && !m68k_read32(&f.ctx, Fixture::storage + 24),
            "console close balances native objects and clears both guest identities");
    require(!std::memcmp(before_close.data(), f.memory + Fixture::storage, before_close.size()),
            "console close preserves error, flags, mn_Length and every other byte");
    f.close();
    require(f.open(Fixture::storage, 0xFFFFFFFF, 0x5A) == 0 && opened_flags == 0x5A &&
            f.ctx.console_base == base && f.ctx.num_libs == 2 &&
            m68k_read32(&f.ctx, base + 10) == name_addr && Kaffeine_HeapAvail(&f.ctx, 0) == avail,
            "reopen passes flags and reuses the cached Device and name");
    ULONG second = Fixture::storage + 0x80;
    std::memset(f.memory + second, 0, 32);
    require(f.open(second) == 0 && requests.size() == 2 && ports.size() == 2 &&
            m68k_read32(&f.ctx, second + 20) == base, "two independent opens share only the guest Device identity");
    f.close(second);
}

static void test_request_bounds()
{
    Fixture f;
    for (ULONG size : {20u, 21u, 24u, 28u, 31u})
    {
        ULONG r = f.request(size);
        require(r != 0, "short owned request is allocated");
        m68k_write16(&f.ctx, r + 18, 0xFFFF);
        std::array<UBYTE, 64> before;
        std::memcpy(before.data(), f.memory + r, before.size());
        const unsigned opens = open_calls, native = doio_calls;
        require(f.open(r) == 0xFFFFFFFF && open_calls == opens && f.ctx.running,
                "owned short request fails OpenDevice without native effects");
        require(!std::memcmp(before.data(), f.memory + r, before.size()),
                "short failure changes neither the owned bytes nor any adjacent guard bytes");
        for (ULONG lvo : {456, 468, 474, 480})
        {
            require(f.exec(lvo, 0, r) == 20 && f.stopped(), "I/O rejects an owned short prefix");
            require(!std::memcmp(before.data(), f.memory + r, before.size()) && doio_calls == native,
                    "short I/O refusal leaves all bytes and native backend untouched");
        }
        f.close(r);
        require(!std::memcmp(before.data(), f.memory + r, before.size()), "close of an unopened short request writes nothing");
        f.erase(r);
    }
    ULONG r = f.request(32);
    m68k_write16(&f.ctx, r + 18, 0);
    std::array<UBYTE, 16> guard;
    std::memcpy(guard.data(), f.memory + r + 32, guard.size());
    require(f.open(r) == 0 && !std::memcmp(f.memory + r + 32, guard.data(), guard.size()),
            "minimal owned IORequest opens console with zero mn_Length and preserves its suffix guard");
    require(f.open(r, UNIT_MICROHZ, 0, true) == 0xFFFFFFFF && requests.empty() && ports.empty() &&
            !std::memcmp(f.memory + r + 32, guard.data(), guard.size()),
            "reopening a minimal console request as timer closes console and refuses the insufficient capacity");
    require(f.open(r) == 0, "minimal request retains its owned capacity after failed mixed-kind reopen");
    fail_request = true;
    require(f.open(r) == 0xFFFFFFFF && requests.empty() && ports.empty(),
            "failed owned reopen closes its prior native open and rolls back the new reply port");
    fail_request = false;
    require(f.open(r) == 0, "owned request remains reusable after native allocation failure");
    f.erase(r);
    require(requests.empty() && ports.empty(), "deleting an open minimal request closes its native open");

    const ULONG end = Fixture::arena_size - 32;
    std::memset(f.memory + end, 0, 32);
    require(f.open(end) == 0, "32 guest bytes exactly at the arena end suffice");
    f.close(end);
    const unsigned opens = open_calls;
    for (ULONG address : {0u, 0xFFFFFFFFu, Fixture::arena_size - 31, Fixture::arena_size})
        require(f.open(address) == 20 && f.stopped() && open_calls == opens,
                "invalid or truncated arena prefixes are refused before any native allocation");
    require(f.request(0) == 0, "zero-size owned request is refused by CreateIORequest");
    r = f.request(40);
    require(f.open(r) == 0 && f.open(r, UNIT_MICROHZ, 0, true) == 0 && f.open(r) == 0,
            "an owned request can reopen across both device kinds in either direction");
    f.erase(r);
}

static void test_refusals_and_kinds()
{
    Fixture f;
    const unsigned opens = open_calls;
    for (ULONG unit : {0u, 1u, 2u, 3u, 0xFFFFFFFEu, 0xFFFFu})
        require(f.open(Fixture::storage, unit) == 0xFFFFFFFF && f.memory[Fixture::storage + 31] == 0xFF &&
                !m68k_read32(&f.ctx, Fixture::storage + 20) && !m68k_read32(&f.ctx, Fixture::storage + 24) &&
                open_calls == opens, "other console units fail honestly without opening a native device");
    require(f.open() == 0, "console opens for mixed-kind tests");
    ULONG base = f.ctx.console_base;
    for (ULONG command : {0u, 1u, 2u, 3u, 8u, 9u, 10u, 11u, 0x4000u, 0xFFFFu})
    {
        m68k_write16(&f.ctx, Fixture::storage + 28, static_cast<UWORD>(command));
        m68k_write32(&f.ctx, Fixture::storage + 20, 0xBAD0BEEF);
        m68k_write32(&f.ctx, Fixture::storage + 24, UNIT_MICROHZ);
        std::array<UBYTE, 16> guard;
        std::memcpy(guard.data(), f.memory + Fixture::storage + 32, guard.size());
        const unsigned native = doio_calls;
        require(f.exec(456, 0, Fixture::storage) == 0xFFFFFFFD && f.ctx.running &&
                f.memory[Fixture::storage + 31] == 0xFD && f.memory[Fixture::storage + 30] == IOF_QUICK &&
                f.memory[Fixture::storage + 8] == 0 && doio_calls == native &&
                !std::memcmp(guard.data(), f.memory + Fixture::storage + 32, guard.size()),
                "console DoIO rejects even timer command numbers without a cast, writes past 32, or native effects");
    }
    for (ULONG vector : {6, 12, 18, 24, 30, 36, 42, 54, 60, 66, 72})
        require(f.console(vector, 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFF, 0xFFFFFFFC) == 20 && f.stopped(),
                "unsupported device vectors explicitly refuse poisoned guest pointers");
    require(f.exec(462, 0, Fixture::storage) == 20 && f.stopped(), "asynchronous console SendIO remains refused");
    require(f.exec(474, 0, Fixture::storage) == 0xFFFFFFFD &&
            f.exec(468, 0, Fixture::storage) == Fixture::storage &&
            f.exec(480, 0, Fixture::storage) == 0xFFFFFFFF, "complete console request retains safe Exec wait/check/abort behavior");

    unsigned closes = close_calls;
    require(f.open(Fixture::storage, UNIT_MICROHZ, 0, true) == 0 && close_calls == closes + 1,
            "reopening console as timer closes the previous native open");
    m68k_write32(&f.ctx, Fixture::storage + 20, base);
    m68k_write32(&f.ctx, Fixture::storage + 24, 0xFFFFFFFF);
    m68k_write16(&f.ctx, Fixture::storage + 28, TR_GETSYSTIME);
    unsigned native = doio_calls;
    require(f.exec(456, 0, Fixture::storage) == 0 && doio_calls == native + 1 &&
            m68k_read32(&f.ctx, Fixture::storage + 32) == 0x10203040,
            "trusted timer kind dispatches timer even when guest fields claim console");
    require(f.open() == 0 && close_calls == closes + 2 && f.ctx.console_base == base,
            "reopening timer as console closes timer and reuses the console identity");
    m68k_write32(&f.ctx, Fixture::storage + 20, f.ctx.timer_base);
    m68k_write16(&f.ctx, Fixture::storage + 28, TR_GETSYSTIME);
    require(f.exec(456, 0, Fixture::storage) == 0xFFFFFFFD && doio_calls == native + 1,
            "guest timer identity cannot turn a native console request into a timer");
    require(f.open(Fixture::storage, 0) == 0xFFFFFFFF && requests.empty() && ports.empty(),
            "failed reopen closes the old native request and forgets unowned storage");
    require(f.open() == 0, "unowned request slot is reusable after failure");
}

static void test_failures()
{
    for (int failure = 1; failure <= 3; ++failure)
    {
        Fixture f;
        ULONG available = Kaffeine_HeapAvail(&f.ctx, 0);
        size_t allocated = blocks.size();
        unsigned closes = close_calls;
        fail_allocation = failure;
        ULONG result = f.open();
        fail_allocation = 0;
        require(result == 0xFFFFFFFF && !f.ctx.console_base && f.ctx.num_libs == 1 &&
                Kaffeine_HeapAvail(&f.ctx, 0) == available && blocks.size() == allocated &&
                requests.empty() && ports.empty() && !signals,
                "each native port/request/name allocation failure rolls back all resources and base publication");
        require(close_calls == closes + (failure >= 3 ? 1 : 0),
                "rollback closes the device only if the native open succeeded");
        require(f.open() == 0, "retry after allocation rollback succeeds");
    }
    {
        Fixture empty;
        ULONG available = Kaffeine_HeapAvail(&empty.ctx, MEMF_LARGEST);
        ULONG held = Kaffeine_HeapAlloc(&empty.ctx, available, 0);
        require(held && !Kaffeine_HeapAvail(&empty.ctx, 0), "fixture exhausts guest heap storage");
        size_t allocated = blocks.size();
        unsigned closes = close_calls;
        require(empty.open() == 0xFFFFFFFF && !empty.ctx.console_base && empty.ctx.num_libs == 1 &&
                blocks.size() == allocated && requests.empty() && ports.empty() && close_calls == closes + 1,
                "lack of guest name storage closes the native device and rolls back all native objects");
        Kaffeine_HeapFree(&empty.ctx, held, available);
        require(empty.open() == 0, "open succeeds after guest storage is returned");
    }
    Fixture f;
    const size_t allocated = blocks.size();
    const ULONG available = Kaffeine_HeapAvail(&f.ctx, 0);
    for (bool *fail : {&fail_port, &fail_request, &fail_signal})
    {
        *fail = true;
        require(f.open() == 0xFFFFFFFF && requests.empty() && ports.empty() && !signals &&
                blocks.size() == allocated && Kaffeine_HeapAvail(&f.ctx, 0) == available,
                "port, request and signal failures roll back without leaks");
        *fail = false;
    }
    open_error = -7;
    unsigned closes = close_calls;
    require(f.open() == 0xFFFFFFF9 && f.memory[Fixture::storage + 31] == 0xF9 &&
            !m68k_read32(&f.ctx, Fixture::storage + 20) && !m68k_read32(&f.ctx, Fixture::storage + 24) &&
            close_calls == closes && blocks.size() == allocated && requests.empty() && ports.empty(),
            "native open failure sign-extends its error and deletes objects without closing an unopened device");
    open_error = 0;
    UWORD used = f.ctx.num_libs;
    f.ctx.num_libs = KAFFEINE_MAX_LIBS;
    require(f.open() == 0xFFFFFFFF && !f.ctx.console_base &&
            Kaffeine_HeapAvail(&f.ctx, 0) == available && blocks.size() == allocated,
            "full Device registration table rolls back the name and native open");
    f.ctx.num_libs = used;
    f.ctx.libs[used] = {};
    f.ctx.libs[used].m68k_addr = KAFFEINE_LIBBASE_REGION + KAFFEINE_LIBBASE_SIZE - 4;
    f.ctx.libs[used].jt_start = f.ctx.libs[used].m68k_addr;
    f.ctx.libs[used].pos_size = 4;
    f.ctx.num_libs = used + 1;
    require(f.open() == 0xFFFFFFFF && !f.ctx.console_base && blocks.size() == allocated &&
            Kaffeine_HeapAvail(&f.ctx, 0) == available,
            "exhausted fake base region also rolls back every allocation");
    f.ctx.num_libs = used;
    require(f.open() == 0, "open succeeds once registration space is available");
    f.close();
    unsigned allocations = allocation_calls;
    require(f.open() == 0 && allocation_calls == allocations + 2,
            "cached Device reuse allocates only the native port and native request");
}

static void test_capacity_cleanup_contexts()
{
    Fixture f;
    std::vector<ULONG> owned;
    for (unsigned i = 0; i < KAFFEINE_MAX_IO_REQUESTS; ++i)
    {
        owned.push_back(f.request(32));
        require(owned.back() != 0, "each owned request table entry is available");
    }
    unsigned opens = open_calls;
    require(f.request(32) == 0 && f.open() == 0xFFFFFFFF && open_calls == opens && requests.empty(),
            "full request table refuses allocation and open before native effects");
    f.erase(owned.back()); owned.pop_back();
    require(f.open() == 0, "freed request slot can hold unowned console storage");
    f.close();
    for (ULONG r : owned) f.erase(r);
    require(f.open() == 0, "console opens before cleanup");
    unsigned closes = close_calls;
    current = &other; f.ctx.exec_task = &owner;
    Kaffeine_IOCleanup(&f.ctx);
    require(close_calls == closes && requests.size() == 1 && ports.size() == 1,
            "cleanup on another task leaves native objects and signals intact");
    current = &owner;
    Kaffeine_IOCleanup(&f.ctx);
    require(requests.empty() && ports.empty() && !signals && close_calls == closes + 1,
            "owner cleanup balances every native open and signal");
    for (const auto &entry : f.ctx.io_requests)
        require(!entry.guest && !entry.native && entry.kind == KAFFEINE_IO_NONE, "cleanup clears trusted kind and request records");
    require(f.open() == 0 && f.ctx.num_libs == 2, "context reuses its cached Device after cleanup");
    Fixture second;
    require(second.open() == 0 && requests.size() == 2 && ports.size() == 2,
            "distinct contexts hold independent native resources concurrently");
    require(second.ctx.console_base == f.ctx.console_base && second.ctx.mem != f.ctx.mem,
            "same guest address in distinct arenas identifies distinct Device storage");
    second.memory[second.ctx.console_base + 20] = 0xFE;
    require(f.memory[f.ctx.console_base + 20] == 0, "Device bytes are never shared across contexts");
    second.close();
    require(requests.size() == 1 && ports.size() == 1, "closing one context leaves the other's open intact");
}

/* Fixed 68k bytes, including unaligned timestamp fields and all four
   dead-key bytes. This is independent of host struct offsets. */
static const std::array<UBYTE, 22> event_bytes = {
    0, 0, 0, 0, 1, 0x7B, 0x12, 0x34, 0x89, 0xAB, 0xDE, 0xAD, 0xBE, 0xEF,
    0x11, 0x22, 0x33, 0x44, 0xA1, 0xB2, 0xC3, 0xD4
};

static void test_event_marshalling()
{
    static_assert(sizeof(InputEvent) > 22 && offsetof(InputEvent, ie_Class) != 4);
    static_assert(offsetof(InputEvent, ie_TimeStamp) != 14);
    Fixture f;
    require(f.open() == 0, "console opens for event marshalling");
    std::memcpy(f.memory + Fixture::event, event_bytes.data(), event_bytes.size());
    std::memset(f.memory + Fixture::output - 1, 0xA5, 11);
    key_bytes = {0x9B, 0x41}; key_result = 2;
    const unsigned calls = key_calls;
    for (UWORD sr : {0u, 0x001Fu, 0xA700u, 0xFFFFu})
    {
        f.sr = sr;
        require(f.console(48) == 2 && key_calls > calls && f.ctx.running,
                "RawKeyConvert delegates its result without changing guest SR or other registers");
    }
    const auto &ie = key_call.event;
    require(!ie.ie_NextEvent && ie.ie_Class == 1 && ie.ie_SubClass == 0x7B &&
            ie.ie_Code == 0x1234 && ie.ie_Qualifier == 0x89AB &&
            ie.ie_Prev1DownCode == 0xDE && ie.ie_Prev1DownQual == 0xAD &&
            ie.ie_Prev2DownCode == 0xBE && ie.ie_Prev2DownQual == 0xEF &&
            ie.ie_TimeStamp.tv_secs == 0x11223344 && ie.ie_TimeStamp.tv_micro == 0xA1B2C3D4,
            "every scalar InputEvent field is read from its exact 68k byte offset");
    const auto *position = reinterpret_cast<const UBYTE *>(&ie.ie_position);
    for (size_t i = 4; i < sizeof(ie.ie_position); ++i)
        require(!position[i], "native union bytes beyond dead-key history are zero, never a guest pointer");
    const IPTR arena = reinterpret_cast<IPTR>(f.memory);
    require(key_call.event_address % alignof(InputEvent) == 0 &&
            (key_call.event_address < arena || key_call.event_address - arena >= Fixture::arena_size) &&
            key_call.output_address == arena + Fixture::output && key_call.length == 8 && !key_call.keymap,
            "native MapRawKey gets an aligned native event copy, the validated output and a NULL map");
    require(!std::memcmp(event_bytes.data(), f.memory + Fixture::event, event_bytes.size()) &&
            f.memory[Fixture::output - 1] == 0xA5 && f.memory[Fixture::output] == 0x9B &&
            f.memory[Fixture::output + 1] == 0x41 && f.memory[Fixture::output + 2] == 0xA5 &&
            f.memory[Fixture::output + 8] == 0xA5, "input bytes and output guards survive without an added terminator");

    key_bytes.clear(); key_result = -1;
    require(f.console(48) == 0xFFFFFFFF, "native WORD overflow is sign-extended to all of guest D0");
    key_result = -32768;
    require(f.console(48) == 0xFFFF8000, "native signed WORD return width is preserved");
    /* A backend may report an error after writing bytes. The adapter must
       preserve those actual buffer effects, never synthesize success. */
    key_bytes = {0x23}; key_result = -1;
    require(f.console(48) == 0xFFFFFFFF && f.memory[Fixture::output] == 0x23,
            "backend error and any backend buffer effects are returned coherently");
}

static void test_single_event_results()
{
    Fixture f;
    require(f.open() == 0, "console opens for measured single-event results");
    struct Case { UWORD code, qualifier; UBYTE cls; ULONG length; WORD result; std::vector<UBYTE> bytes; };
    /* The collaborator supplies measured answers, not a second conversion
       algorithm. Actual country-specific mapping remains native backend work. */
    const Case cases[] = {
        {0x40, 0, 1, 8, 1, {0x20}}, {0x42, 0, 1, 8, 1, {0x09}},
        {0x44, 0, 1, 8, 1, {0x0D}}, {0x45, 0, 1, 8, 1, {0x1B}},
        {0x41, 0, 1, 8, 1, {0x08}}, {0x46, 0, 1, 8, 1, {0x7F}},
        {0xC0, 0, 1, 8, 0, {}}, {0x44, 0, 0, 8, 0, {}},
        {0x40, 0, 1, 0, -1, {}}, {0x40, 0, 1, 1, 1, {0x20}},
        {0x4C, 0, 1, 8, 2, {0x9B, 0x41}}, {0x4C, 0, 1, 1, -1, {}},
        {0x4C, 0, 1, 0, -1, {}}, {0x20, 0, 1, 8, 1, {0x61}},
        {0x20, 1, 1, 8, 1, {0x41}}, {0x20, 4, 1, 8, 1, {0x41}},
        {0x20, 8, 1, 8, 1, {0x01}}
    };
    for (const auto &c : cases)
    {
        std::array<UBYTE, 22> input = {};
        input[4] = c.cls; input[6] = static_cast<UBYTE>(c.code >> 8); input[7] = static_cast<UBYTE>(c.code);
        input[8] = static_cast<UBYTE>(c.qualifier >> 8); input[9] = static_cast<UBYTE>(c.qualifier);
        std::memcpy(f.memory + Fixture::event, input.data(), input.size());
        std::memset(f.memory + Fixture::output - 1, 0xA5, 11);
        key_bytes = c.bytes; key_result = c.result;
        unsigned calls = key_calls;
        require(f.console(48, Fixture::event, Fixture::output, c.length) == static_cast<ULONG>(static_cast<LONG>(c.result)) &&
                key_calls == calls + 1 && f.ctx.running, "single-event scalar, zero and overflow results come from native MapRawKey");
        require(key_call.event.ie_Code == c.code && key_call.event.ie_Qualifier == c.qualifier &&
                key_call.event.ie_Class == c.cls && key_call.length == static_cast<LONG>(c.length),
                "measured code, class, qualifier and buffer length reach the native default mapper");
        for (unsigned i = 0; i < 10; ++i)
            require(f.memory[Fixture::output + i] == (i < c.bytes.size() ? c.bytes[i] : 0xA5),
                    "exact collaborator output bytes and untouched suffix match the contract");
        require(f.memory[Fixture::output - 1] == 0xA5 &&
                !std::memcmp(f.memory + Fixture::event, input.data(), input.size()),
                "single-event conversion preserves the event and both output guards");
    }
}

static void test_conversion_refusals()
{
    Fixture f;
    require(f.open() == 0, "console opens for conversion boundary tests");
    std::memcpy(f.memory + Fixture::event, event_bytes.data(), event_bytes.size());
    std::memset(f.memory + Fixture::output - 1, 0xA5, 11);
    key_bytes = {0x20}; key_result = 1;
    auto refused = [&](ULONG event, ULONG output, ULONG length, ULONG map) {
        std::array<UBYTE, 22> before_event;
        std::array<UBYTE, 11> before_output;
        std::memcpy(before_event.data(), f.memory + Fixture::event, before_event.size());
        std::memcpy(before_output.data(), f.memory + Fixture::output - 1, before_output.size());
        unsigned calls = key_calls, allocs = allocation_calls;
        require(f.console(48, event, output, length, map) == 20 && f.stopped() && key_calls == calls && allocation_calls == allocs,
                "unsupported or unsafe conversion explicitly stops before any native call or allocation");
        require(!std::memcmp(before_event.data(), f.memory + Fixture::event, before_event.size()) &&
                !std::memcmp(before_output.data(), f.memory + Fixture::output - 1, before_output.size()),
                "conversion refusal leaves event and output bytes untouched");
    };
    for (ULONG map : {0x3800u, 0xFFFFFFFFu, Fixture::arena_size})
        refused(Fixture::event, Fixture::output, 8, map);
    for (ULONG next : {Fixture::event, Fixture::event + 22, 0xFFFFFFFFu, Fixture::arena_size - 1})
    {
        m68k_write32(&f.ctx, Fixture::event, next);
        refused(Fixture::event, Fixture::output, 8, 0);
    }
    m68k_write32(&f.ctx, Fixture::event, 0);
    for (ULONG event : {0u, Fixture::arena_size - 21, Fixture::arena_size, 0xFFFFFFFFu})
        refused(event, Fixture::output, 8, 0);
    for (ULONG output : {0u, Fixture::arena_size, 0xFFFFFFFFu})
        refused(Fixture::event, output, 8, 0);
    refused(Fixture::event, Fixture::arena_size - 1, 2, 0);
    refused(Fixture::event, Fixture::output, 0x7FFFFFFF, 0);
    refused(Fixture::event, Fixture::output, 0xFFFFFFFF, 0);
    refused(Fixture::event, Fixture::output, 0x80000000, 0);
    refused(Fixture::event, 0, 0, 0);
    for (auto [output, length] : {std::pair{Fixture::event, 1u}, {Fixture::event + 21, 1u},
                                 {Fixture::event - 1, 2u}, {Fixture::event - 8, 9u}})
        refused(Fixture::event, output, length, 0);
    /* Adjacent, empty and final-byte buffers are valid. */
    require(f.console(48, Fixture::event, Fixture::event - 1, 1) == 1 &&
            f.memory[Fixture::event - 1] == 0x20, "output ending at the event boundary is valid");
    require(f.console(48, Fixture::event, Fixture::event + 22, 1) == 1 &&
            f.memory[Fixture::event + 22] == 0x20, "output starting after the event is valid");
    key_bytes.clear(); key_result = -1;
    require(f.console(48, Fixture::event, Fixture::event, 0) == 0xFFFFFFFF && f.ctx.running,
            "an empty output span does not overlap the event");
    key_bytes = {0x20}; key_result = 1;
    require(f.console(48, Fixture::event, Fixture::arena_size - 1, 1) == 1 &&
            f.memory[Fixture::arena_size - 1] == 0x20 && f.memory[Fixture::arena_size] == 0xA5,
            "one final arena byte is valid and the external guard remains intact");
    require(f.console(48, Fixture::event, Fixture::output, 0x10001) == 1 && key_call.length == 0x10001,
            "buffer lengths above 65536 are supported when the remaining arena has room");
    require(f.console(48, Fixture::event, Fixture::output, Fixture::arena_size - Fixture::output) == 1 &&
            key_call.length == static_cast<LONG>(Fixture::arena_size - Fixture::output),
            "validation uses the exact remaining arena length without overflow addition");
}

int main()
{
    test_library_opens();
    require(blocks.empty() && requests.empty() && ports.empty() && !signals, "lifecycle fixture frees its complete heap and native objects");
    test_request_bounds();
    test_refusals_and_kinds();
    test_failures();
    test_capacity_cleanup_contexts();
    test_event_marshalling();
    test_single_event_results();
    test_conversion_refusals();
    require(blocks.empty() && requests.empty() && ports.empty() && !signals, "all fixtures leave no native allocations, opens, ports or signals");
    std::printf("console.device: %u checks passed\n", checks);
    return 0;
}
