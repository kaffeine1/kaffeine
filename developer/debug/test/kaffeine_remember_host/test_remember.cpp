/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <exec/memory.h>
#include "kaffeine_intern.h"
#include "kaffeine_cpu.h"
#include "kaffeine_messages.h"
#include "kaffeine_remember.h"
#include "kaffeine_remember_memory.h"

static unsigned checks, failures;
static void check(bool okay, const char *label)
{
    ++checks;
    if (!okay) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}
static std::map<void *,ULONG> native_blocks;
static int allocation_countdown = -1;
static unsigned native_allocations;
static KaffeineContext *poison_queue;
static SIPTR native_error;
static constexpr SIPTR caller_error = (SIPTR)0x12345678000000abULL;
extern "C" APTR AllocMem(ULONG size, ULONG flags)
{
    ++native_allocations;
    if (!allocation_countdown)
    {
        if (poison_queue) poison_queue->message_ports[0] = 0x8000;
        return nullptr;
    }
    if (allocation_countdown > 0) --allocation_countdown;
    void *p = std::malloc(size ? size : 1);
    if (p) { std::memset(p, flags & MEMF_CLEAR ? 0 : 0xd9, size); native_blocks[p] = size; }
    native_error = 0x101;
    return p;
}
extern "C" void FreeMem(APTR pointer, ULONG size)
{
    auto found = native_blocks.find(pointer);
    check(found != native_blocks.end() && found->second == size,
          "native allocation has its exact size and is released only once");
    if (found != native_blocks.end()) native_blocks.erase(found);
    std::free(pointer);
    native_error = 0x102;
}
extern "C" APTR AllocVec(ULONG size, ULONG flags) { return AllocMem(size, flags); }
extern "C" void FreeVec(APTR p) { if (p) FreeMem(p, native_blocks.at(p)); }
extern "C" SIPTR IoErr(void) { return native_error; }
extern "C" SIPTR SetIoErr(SIPTR n) { SIPTR old = native_error; native_error = n; return old; }
extern "C" BOOL Kaffeine_LockProtected(KaffeineContext *, ULONG, ULONG) { return FALSE; }

struct Registers
{
    std::array<ULONG,8> d{}, a{};
    ULONG pc{};
    UWORD sr{};
};
static ULONG get_d(void *p, int n) { return static_cast<Registers *>(p)->d[n]; }
static ULONG get_a(void *p, int n) { return static_cast<Registers *>(p)->a[n]; }
static const KaffeineCPUOps ops = {get_d, get_a, nullptr, nullptr, nullptr, nullptr,
                                    nullptr, nullptr, nullptr, nullptr};
static const M68KThunkEntry table[] = {KAFFEINE_INTUITION_REMEMBER_THUNKS};

struct Guest
{
    KaffeineContext *ctx;
    KaffeineCPU cpu{};
    Registers regs{};
    ULONG key, available;
    Guest()
    {
        ctx = static_cast<KaffeineContext *>(AllocMem(sizeof(*ctx), MEMF_CLEAR));
        ctx->mem_size = 1024 * 1024;
        ctx->mem = static_cast<UBYTE *>(AllocMem(ctx->mem_size + KAFFEINE_MEM_GUARD, MEMF_CLEAR));
        ctx->heap_start = KAFFEINE_HEAP_BASE;
        ctx->heap_end = ctx->stack_bottom = ctx->mem_size - 65536;
        ctx->stack_top = ctx->mem_size;
        check(Kaffeine_HeapInit(ctx), "production guest heap initialized");
        key = Kaffeine_HeapAlloc(ctx, 64, MEMF_PUBLIC | MEMF_CLEAR);
        check(key != 0, "caller owns ordinary guest Remember key storage");
        cpu.ops = &ops; cpu.backend_state = &regs; ctx->cpu = &cpu;
        ctx->running = TRUE;
        for (unsigned i = 0; i < 8; ++i) { regs.d[i] = 0x12340000 + i; regs.a[i] = 0x56780000 + i; }
        regs.pc = 0x1234; regs.sr = 0x251f;
        available = Kaffeine_HeapAvail(ctx, 0);
    }
    IPTR invoke(unsigned index, ULONG cell, ULONG d0, ULONG d1 = 0)
    {
        regs.a[0] = cell; regs.d[0] = d0; regs.d[1] = d1;
        Registers before = regs;
        native_error = caller_error;
        IPTR result = table[index].thunk(ctx, &cpu);
        check(regs.a == before.a && regs.d == before.d && regs.pc == before.pc && regs.sr == before.sr,
              "complete production thunk preserves all CPU registers, PC and CCR/X");
        check(native_error == caller_error, "Intuition adapter preserves full-width native IoErr");
        return result;
    }
    ULONG allocate(ULONG size, ULONG flags = MEMF_PUBLIC | MEMF_CLEAR, ULONG cell = 0)
    { return (ULONG)invoke(0, cell ? cell : key, size, flags); }
    void release(ULONG forget, ULONG cell = 0) { invoke(1, cell ? cell : key, forget); }
    ULONG head(ULONG cell = 0) { return m68k_read32(ctx, cell ? cell : key); }
    void reset_fault() { ctx->running = TRUE; ctx->exit_code = 0; }
    ~Guest()
    {
        allocation_countdown = -1; poison_queue = nullptr;
        ctx->io_release = nullptr; ctx->cpu = nullptr;
        Kaffeine_DestroyContext(ctx);
    }
};

static void basic_and_ownership()
{
    Guest g;
    ULONG first = g.allocate(17), first_node = g.head();
    check(first && first_node && first != first_node && first_node < g.ctx->mem_size,
          "allocation returns guest data, distinct from bounded classic node");
    check(m68k_read32(g.ctx, first_node) == 0 && m68k_read32(g.ctx, first_node + 4) == 17 &&
          m68k_read32(g.ctx, first_node + 8) == first, "classic 12-byte Next/Size/Memory layout published");
    check(Kaffeine_HeapGuestRange(g.ctx, first, 17) && !Kaffeine_HeapGuestRange(g.ctx, first, 18) &&
          Kaffeine_MessageMemory(g.ctx, first, 17), "data is an ordinary readable DOS buffer with exact requested bounds");
    check(Kaffeine_HeapGuestRange(g.ctx, first_node, 12) && !Kaffeine_HeapGuestRange(g.ctx, first_node, 13),
          "classic Remember node has 12 accessible bytes, never native pointer layout");
    for (ULONG i = 0; i < 17; ++i) check(g.ctx->mem[first + i] == 0, "MEMF_CLEAR clears every requested data byte");
    Kaffeine_HeapFree(g.ctx, first, 17); Kaffeine_HeapFree(g.ctx, first_node, 12);
    check(!Kaffeine_HeapFreeExact(g.ctx, first, 17) && !Kaffeine_HeapFreeExact(g.ctx, first_node, 12) &&
          !Kaffeine_HeapAllocAbs(g.ctx, 8, first), "ordinary FreeMem/FreeVec/AllocAbs cannot steal Remember ownership");
    ULONG second = g.allocate(31), second_node = g.head();
    check(second && m68k_read32(g.ctx, second_node) == first_node, "later allocation prepends to existing live chain");
    g.release(0x80000000U);
    check(!g.head() && !Kaffeine_HeapGuestRange(g.ctx, first, 17) && !Kaffeine_HeapGuestRange(g.ctx, second, 31),
          "any nonzero BOOL frees all data and clears the key");
    check(g.ctx->remember_count == 2 &&
          Kaffeine_RememberMemoryCheck(g.ctx, first_node, 12, FALSE) &&
          Kaffeine_RememberMemoryCheck(g.ctx, second_node, 12, FALSE),
          "both bounded node identities remain quarantined after their data is released");
    ULONG now = Kaffeine_HeapAvail(g.ctx, 0);
    g.release(TRUE);
    check(g.ctx->running && Kaffeine_HeapAvail(g.ctx, 0) == now, "empty FreeRemember is an idempotent no-op");
}

static void forget_and_stale(bool forget_data)
{
    Guest g;
    ULONG data = g.allocate(64), node = g.head(), alias = g.key + 8;
    std::memset(g.ctx->mem + data, 0x6b, 64);
    m68k_write32(g.ctx, alias, node);
    g.release(forget_data, alias);
    check(!g.head(alias) && g.head() == node, "a copied live key is accepted and only that cell is reset");
    if (!forget_data)
    {
        check(Kaffeine_HeapGuestRange(g.ctx, data, 64) && g.ctx->mem[data + 63] == 0x6b,
              "FreeRemember(FALSE) retains data bytes and transfers ordinary guest ownership");
        check(Kaffeine_HeapFreeExact(g.ctx, data, 64), "caller can normally free forgotten data");
    }
    ULONG reused = Kaffeine_HeapAlloc(g.ctx, 64, MEMF_CLEAR);
    check(reused == data, "real allocator deterministically reuses the released data interval");
    std::memset(g.ctx->mem + reused, 0xa9, 64);
    g.release(TRUE);
    check(!g.ctx->running && g.ctx->exit_code == 20 && g.head() == node &&
          g.ctx->mem[reused] == 0xa9 && Kaffeine_HeapFreeExact(g.ctx, reused, 64),
          "stale original key cannot free a new allocation at the old data address");
    check(!Kaffeine_HeapFreeExact(g.ctx, node, 12) && !Kaffeine_HeapAllocAbs(g.ctx, 12, node),
          "retired node identity cannot be released or reused by the guest");
}

static void malformed()
{
    for (ULONG offset : {0U, 4U, 8U})
    {
        Guest g;
        ULONG older = g.allocate(32), newer = g.allocate(40), head = g.head();
        ULONG value = m68k_read32(g.ctx, head + offset);
        m68k_write32(g.ctx, head + offset, offset ? value ^ 4 : head);
        ULONG available = Kaffeine_HeapAvail(g.ctx, 0);
        g.release(TRUE);
        check(!g.ctx->running && g.head() == head && Kaffeine_HeapAvail(g.ctx, 0) == available &&
              Kaffeine_RememberMemoryCheck(g.ctx, older, 32, FALSE) &&
              Kaffeine_RememberMemoryCheck(g.ctx, newer, 40, FALSE),
              "corrupt/cyclic Next, Size or Memory refuses the entire chain before release");
    }
    for (ULONG key_mode = 0; key_mode < 5; ++key_mode)
    {
        Guest g;
        ULONG invalid = key_mode == 0 ? 0 : key_mode == 1 ? g.key + 1 :
            key_mode == 2 ? g.ctx->mem_size - 2 : key_mode == 3 ? g.key + 62 : g.ctx->heap_start;
        ULONG before = Kaffeine_HeapAvail(g.ctx, 0);
        check(!g.invoke(0, invalid, 32) && !g.ctx->running &&
              Kaffeine_HeapAvail(g.ctx, 0) == before && !g.ctx->remember_count,
              "NULL/odd/out-of-arena/straddling/header key is rejected before allocation");
    }
    {
        Guest g;
        ULONG forged = Kaffeine_HeapAlloc(g.ctx, 12, MEMF_CLEAR);
        m68k_write32(g.ctx, forged + 4, 32); m68k_write32(g.ctx, forged + 8, g.key + 16);
        m68k_write32(g.ctx, g.key, forged); g.release(TRUE);
        check(!g.ctx->running && Kaffeine_HeapFreeExact(g.ctx, forged, 12),
              "plausible guest-written node has no private allocation provenance");
    }
    {
        Guest g;
        ULONG data = g.allocate(64), head = g.head();
        g.invoke(1, head, TRUE);
        check(!g.ctx->running && g.head() == head, "key cannot alias immutable Remember link fields");
        g.reset_fault(); m68k_write32(g.ctx, data, head); g.release(TRUE, data);
        check(!g.ctx->running && Kaffeine_RememberMemoryCheck(g.ctx, data, 64, FALSE),
              "freeing data that contains the key is rejected before a post-free key write");
    }
    {
        Guest one, two;
        ULONG data = one.allocate(32); m68k_write32(two.ctx, two.key, one.head()); two.release(TRUE);
        check(!two.ctx->running && Kaffeine_RememberMemoryCheck(one.ctx, data, 32, FALSE),
              "equal numeric guest addresses confer no ownership in another Context");
    }
}

static void allocation_failures()
{
    for (int failure = 0; failure < 4; ++failure)
    {
        Guest g;
        ULONG old = g.allocate(24), head = g.head(), available = Kaffeine_HeapAvail(g.ctx, 0);
        auto blocks = native_blocks.size(); auto count = g.ctx->remember_count;
        allocation_countdown = failure;
        ULONG result = g.allocate(56);
        allocation_countdown = -1;
        if (!result)
            check(g.head() == head && Kaffeine_HeapAvail(g.ctx, 0) == available &&
                  native_blocks.size() == blocks && g.ctx->remember_count == count && g.ctx->running &&
                  Kaffeine_RememberMemoryCheck(g.ctx, old, 24, FALSE),
                  "record/node/data OOM restores allocator, chain, capacity and native ownership completely");
        else check(g.head() != head && g.ctx->remember_count == count + 1, "allocation succeeds beyond all failure points");
    }
    {
        Guest g;
        for (ULONG size : {0U, 0xffffffffU, 0xfffffff8U})
        {
            ULONG before = Kaffeine_HeapAvail(g.ctx, 0); auto blocks = native_blocks.size();
            check(!g.allocate(size) && !g.head() && g.ctx->running &&
                  Kaffeine_HeapAvail(g.ctx, 0) == before && native_blocks.size() == blocks &&
                  !g.ctx->remember_count, "zero/overflow allocation leaves key, heap and capacity unchanged");
        }
    }
    {
        Guest g;
        ULONG before = Kaffeine_HeapAvail(g.ctx, 0);
        allocation_countdown = 2; poison_queue = g.ctx;
        check(!g.allocate(80), "data metadata allocation fails after node reservation");
        allocation_countdown = -1; poison_queue = nullptr;
        check(g.ctx->message_ports[0] && Kaffeine_HeapAvail(g.ctx, 0) == before &&
              !g.ctx->remember_count && g.ctx->running && !g.head(),
              "unpublished rollback succeeds even when an unrelated queue becomes malformed");
        g.ctx->message_ports[0] = 0;
    }
}

static ULONG blocked_data;
static unsigned commits;
static BOOL guard(KaffeineContext *, ULONG address, ULONG size, BOOL commit)
{
    if (commit) ++commits;
    native_error = 0x109;
    return !Kaffeine_Overlap(address, size, blocked_data, 32);
}
static void pending_data()
{
    Guest g;
    ULONG first = g.allocate(32), second = g.allocate(48), head = g.head();
    blocked_data = first; commits = 0; g.ctx->io_release = guard;
    ULONG available = Kaffeine_HeapAvail(g.ctx, 0);
    g.release(TRUE);
    check(!g.ctx->running && !commits && g.head() == head &&
          Kaffeine_HeapAvail(g.ctx, 0) == available &&
          Kaffeine_RememberMemoryCheck(g.ctx, second, 48, FALSE),
          "one retained data block refuses the whole chain without a release commit");
    g.reset_fault(); g.release(FALSE);
    check(!g.head() && !commits && Kaffeine_HeapGuestRange(g.ctx, first, 32),
          "forgetting ownership keeps pending data live without native close/release");
    check(!Kaffeine_HeapFreeExact(g.ctx, first, 32), "normal guest free retains the transferred in-flight block");
    blocked_data = 0; g.ctx->io_release = nullptr;
    check(Kaffeine_HeapFreeExact(g.ctx, first, 32) && Kaffeine_HeapFreeExact(g.ctx, second, 48),
          "forgotten data is releasable normally once external ownership ends");
}

static void cap_and_cleanup()
{
    Guest g;
    for (unsigned i = 0; i < KAFFEINE_REMEMBER_LIMIT; ++i)
    {
        ULONG data = g.allocate(1);
        check(data != 0, "one-shot Remember identity fits the documented bound");
        g.release(FALSE);
        check(Kaffeine_HeapFreeExact(g.ctx, data, 1), "forgotten payload returns to ordinary allocation ownership");
    }
    unsigned calls = native_allocations;
    check(g.ctx->remember_count == KAFFEINE_REMEMBER_LIMIT && !g.allocate(1) &&
          native_allocations == calls && !g.head() && g.ctx->running,
          "identity cap refuses before allocating and preserves the empty key");
    check(Kaffeine_HeapFreeExact(g.ctx, g.key, 64), "caller key may disappear before Context cleanup");
    UBYTE *memory = g.ctx->mem;
    g.ctx->mem = nullptr;
    native_error = caller_error;
    Kaffeine_RememberCleanup(g.ctx);
    check(!g.ctx->remembers && !g.ctx->remember_count && g.ctx->remember_closing &&
          native_error == caller_error, "cleanup drops all private records without any guest key/node access");
    Kaffeine_RememberCleanup(g.ctx);
    g.ctx->mem = memory;
    ULONG before = Kaffeine_HeapAvail(g.ctx, 0);
    check(!g.allocate(4) && Kaffeine_HeapAvail(g.ctx, 0) == before,
          "closed Remember registry cannot publish new allocations");
}

struct RunStep
{
    Guest *guest;
    unsigned thunk, steps{}, following{};
    bool ordinary_oom;
    IPTR thunk_result{~(IPTR)0};
};
static RunStep *active_step;

static void execute_step(void *state)
{
    auto &step = *active_step;
    auto &g = *step.guest;
    check(state == &g.regs, "production RunCPU executes the selected CPU instance");
    if (++step.steps == 1)
    {
        step.thunk_result = g.invoke(step.thunk, step.ordinary_oom ? g.key : 0,
                                     step.thunk ? TRUE : 32, MEMF_PUBLIC);
        /* Model the dispatcher's ordinary D0 result write after the thunk.
         * Both a refused call and a legitimate allocation failure return 0. */
        g.regs.d[0] = (ULONG)step.thunk_result;
    }
    else
    {
        ++step.following;
        g.regs.d[0] = 7;
        g.ctx->running = FALSE; /* normal end of the next fake instruction */
    }
}

static void run_loop_result()
{
    for (unsigned thunk : {0U, 1U})
    {
        Guest g;
        KaffeineCPUOps run_ops = ops;
        run_ops.execute = execute_step;
        g.cpu.ops = &run_ops;
        g.ctx->cpu = nullptr;
        RunStep step{&g, thunk, 0, 0, false};
        active_step = &step;
        LONG result = Kaffeine_RunCPU(g.ctx, &g.cpu);
        check(!step.thunk_result && !g.regs.d[0], "refused Remember thunk and dispatcher both produce D0 zero");
        check(result == 20 && g.ctx->exit_code == 20 && g.ctx->callback_failed,
              "real RunCPU preserves the Remember refusal's authoritative rc20");
        check(step.steps == 1 && !step.following && !g.ctx->cpu,
              "refused Remember stops before a following guest instruction and restores outer CPU");
        g.cpu.ops = &ops;
    }
    Guest g;
    KaffeineCPUOps run_ops = ops;
    run_ops.execute = execute_step;
    g.cpu.ops = &run_ops;
    g.ctx->cpu = nullptr;
    RunStep step{&g, 0, 0, 0, true};
    active_step = &step;
    allocation_countdown = 0;
    LONG result = Kaffeine_RunCPU(g.ctx, &g.cpu);
    allocation_countdown = -1;
    check(!step.thunk_result && !g.ctx->callback_failed && result == 7,
          "ordinary Remember OOM returns NULL without forcing a process failure");
    check(step.steps == 2 && step.following == 1 && !g.head() && !g.ctx->cpu,
          "ordinary OOM permits the next instruction and normal process result");
    g.cpu.ops = &ops;
    active_step = nullptr;
}

int main()
{
    check(sizeof(table) / sizeof(table[0]) == 2 && table[0].lvo == 396 && table[1].lvo == 408,
          "production manual registration uses independent native slots 66 and 68");
    basic_and_ownership();
    forget_and_stale(false); forget_and_stale(true);
    malformed(); allocation_failures(); pending_data(); cap_and_cleanup(); run_loop_result();
    check(native_blocks.empty(), "every native Context, heap interval and private Remember record is balanced");
    std::printf("Remember production adapter: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
