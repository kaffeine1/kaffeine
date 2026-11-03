/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Real startup adapter and Exec queue boundary; recording native peers.
   Heap/lock implementations have separate production-unit coverage. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <workbench/startup.h>
#include <proto/exec.h>
#include "kaffeine_startup.h"
#include "kaffeine_messages.h"
#include "kaffeine_locks.h"
#include "kaffeine_offsets.h"

static constexpr ULONG ARENA = 0x20000, BLOCK = 0x1000, TASK = 0x200, BASE = 0x400;
static unsigned cases, replies, drops, borrowed, allocations, aborts, signals;
static int borrow_failure;
static bool allocation_failure;
static Message *expected_native;
static KaffeineContext *active;
static std::vector<BPTR> seen_locks;

extern "C" ULONG Kaffeine_MirrorAlloc(KaffeineContext *ctx, ULONG size)
{
    ++allocations;
    assert(size >= 40 && size <= 65536 && BLOCK + size < ctx->mem_size);
    return allocation_failure ? 0 : BLOCK;
}
extern "C" BOOL Kaffeine_MirrorAbort(KaffeineContext *, ULONG address)
{ assert(address == BLOCK); ++aborts; return TRUE; }
extern "C" ULONG Kaffeine_LockBorrow(KaffeineContext *, BPTR native, KaffeineLockScope scope, ULONG owner)
{
    assert(scope == KAFFEINE_LOCK_WBARG && owner == BLOCK);
    seen_locks.push_back(native);
    if (borrow_failure == static_cast<int>(++borrowed)) return 0;
    return 0x6000 + borrowed * 4;
}
extern "C" void Kaffeine_LockScopeDrop(KaffeineContext *, KaffeineLockScope scope, ULONG owner)
{ assert(scope == KAFFEINE_LOCK_WBARG && owner == BLOCK); ++drops; }
extern "C" void ReplyMsg(Message *message)
{
    assert(message == expected_native && !active->startup_native);
    assert(!active->startup_message || drops > 0);
    assert(++replies == 1);
}
extern "C" Task *FindTask(const char *name)
{ assert(!name); return reinterpret_cast<Task *>(0x12345678); }
extern "C" void Signal(Task *task, ULONG mask)
{ assert(task == FindTask(nullptr) && mask == (1u << 8)); ++signals; }

struct Fixture
{
    std::unique_ptr<KaffeineContext> c = std::make_unique<KaffeineContext>();
    std::vector<UBYTE> memory = std::vector<UBYTE>(ARENA, 0xA5);
    WBStartup native{};
    WBArg args[3]{};
    char names[3][32] = {"tool", "one", "two"};
    Fixture()
    {
        ++cases;
        active = c.get();
        expected_native = &native.sm_Message;
        replies = drops = borrowed = allocations = aborts = signals = 0;
        borrow_failure = -1; allocation_failure = false; seen_locks.clear();
        c->mem = memory.data(); c->mem_size = memory.size();
        c->running = TRUE; c->guest_exec_task = TASK; c->m68k_sysbase = BASE;
        c->startup_native = &native.sm_Message; c->startup_seglist = 0x876;
        m68k_write32(c.get(), BASE + M68K_EXECBASE_THISTASK, TASK);
        ULONG p = TASK + M68K_PR_MSGPORT;
        std::memset(c->mem + p, 0, 34);
        c->mem[p + 8] = NT_MSGPORT; c->mem[p + 15] = 8;
        m68k_write32(c.get(), p + 16, TASK);
        m68k_write32(c.get(), p + 20, p + 24);
        m68k_write32(c.get(), p + 28, p + 20);
        native.sm_NumArgs = 3; native.sm_ArgList = args;
        for (unsigned i = 0; i < 3; ++i)
        {
            args[i].wa_Lock = reinterpret_cast<BPTR>(static_cast<IPTR>(0x123400 + i * 16));
            args[i].wa_Name = reinterpret_cast<BYTE *>(names[i]);
        }
    }
    ULONG port() const { return TASK + M68K_PR_MSGPORT; }
    void publish()
    {
        assert(Kaffeine_StartupPublish(c.get()));
        assert(c->startup_message == BLOCK && c->startup_port == port());
        assert(signals == 1 && replies == 0);
        assert(Kaffeine_GuestGetMsg(c.get(), port(), TRUE) == BLOCK);
    }
    void take() { assert(Kaffeine_GuestGetMsg(c.get(), port(), FALSE) == BLOCK); }
    void finish()
    {
        Kaffeine_StartupFinish(c.get());
        assert(replies == 1 && !c->startup_native);
        Kaffeine_StartupFinish(c.get()); assert(replies == 1);
        /* No free/reuse of the published mirror, including duplicate reply. */
        assert(aborts == 0);
        IPTR result = -1;
        assert(Kaffeine_StartupReply(c.get(), BLOCK, &result) && result == 0 && replies == 1);
    }
    void refused()
    {
        assert(!Kaffeine_StartupPublish(c.get()));
        assert(!c->startup_message && !c->startup_port && replies == 0 && signals == 0);
        Kaffeine_StartupFinish(c.get()); assert(replies == 1);
    }
};

int main()
{
    {
        Fixture f; f.c->startup_native = nullptr;
        assert(Kaffeine_StartupPublish(f.c.get()) && !allocations && !replies);
    }
    {
        Fixture f; auto original = f.native; WBArg saved[3]; std::memcpy(saved, f.args, sizeof(saved));
        f.publish(); auto c = f.c.get();
        assert(c->mem[BLOCK + 8] == NT_MESSAGE && m68k_read16(c, BLOCK + 18) == 40);
        assert(m68k_read32(c, BLOCK + 14) == 0 && m68k_read32(c, BLOCK + 20) == f.port());
        assert(m68k_read32(c, BLOCK + 24) == 0x876 && m68k_read32(c, BLOCK + 28) == 3);
        assert(m68k_read32(c, BLOCK + 32) == 0 && m68k_read32(c, BLOCK + 36) == BLOCK + 40);
        for (unsigned i = 0; i < 3; ++i)
        {
            assert(m68k_read32(c, BLOCK + 40 + i * 8) == 0x6000 + (i + 1) * 4);
            ULONG text = m68k_read32(c, BLOCK + 44 + i * 8);
            assert(text >= BLOCK + 64 && text < BLOCK + c->startup_size);
            assert(!std::strcmp(reinterpret_cast<char *>(c->mem + text), f.names[i]));
        }
        assert(!std::memcmp(&original, &f.native, sizeof(original)) && !std::memcmp(saved, f.args, sizeof(saved)));
        f.names[1][0] = 'X'; assert(c->mem[m68k_read32(c, BLOCK + 52)] == 'o');
        assert(c->mem[BLOCK - 1] == 0xA5 && c->mem[BLOCK + c->startup_size] == 0xA5);
        f.take(); IPTR result = -1;
        assert(!Kaffeine_StartupReply(c, BLOCK + 2, &result) && result == static_cast<IPTR>(-1) && !replies);
        assert(Kaffeine_StartupReply(c, BLOCK, &result) && result == 0 && replies == 1 && drops == 1);
        f.finish();
    }
    { Fixture f; f.publish(); f.finish(); KaffeineQueue q; assert(Kaffeine_QueueRead(f.c.get(), f.port(), &q) && !q.count); }
    { Fixture f; f.publish(); f.take(); f.finish(); }
    {
        Fixture f; f.publish(); IPTR result = 0;
        assert(Kaffeine_StartupReply(f.c.get(), BLOCK, &result) && result == 20 && !replies);
        assert(!f.c->running && f.c->exit_code == 20); f.finish();
    }
    {
        Fixture f; f.publish(); f.take();
        assert(Kaffeine_GuestPutMsg(f.c.get(), f.port(), BLOCK, 0) == 20 && !replies); f.finish();
    }
    { Fixture f; f.publish(); m68k_write32(f.c.get(), f.port() + 20, 0xFFFFFFFE); f.finish(); }
    { Fixture f; f.native.sm_NumArgs = -1; f.refused(); assert(!allocations); }
    { Fixture f; f.native.sm_NumArgs = 129; f.refused(); assert(!allocations); }
    { Fixture f; f.native.sm_ArgList = nullptr; f.refused(); assert(!allocations); }
    { Fixture f; f.c->guest_exec_task = 0; f.refused(); assert(!allocations); }
    { Fixture f; f.c->startup_seglist = 0; f.refused(); assert(!allocations); }
    { Fixture f; allocation_failure = true; f.refused(); assert(allocations == 1 && !aborts && !borrowed); }
    { Fixture f; borrow_failure = 1; f.refused(); assert(aborts == 1 && drops == 1); }
    { Fixture f; borrow_failure = 2; f.refused(); assert(aborts == 1 && drops == 1 && borrowed == 2); }
    { Fixture f; m68k_write32(f.c.get(), f.port() + 24, 1); f.refused(); assert(!allocations); }
    {
        Fixture f; char text[4096]; std::memset(text, 'x', sizeof(text));
        f.args[0].wa_Name = reinterpret_cast<BYTE *>(text); f.refused(); assert(!allocations);
    }
    {
        Fixture f; char text[4096]; std::memset(text, 'x', sizeof(text));
        f.native.sm_ToolWindow = text; f.refused(); assert(!allocations);
    }
    {
        Fixture f; std::vector<WBArg> args(128); char text[4096];
        std::memset(text, 'x', sizeof(text)); text[4095] = 0;
        for (auto &a : args) a.wa_Name = reinterpret_cast<BYTE *>(text);
        f.native.sm_ArgList = args.data(); f.native.sm_NumArgs = args.size(); f.refused(); assert(!allocations);
    }
    {
        Fixture f; f.native.sm_NumArgs = 0; f.native.sm_ArgList = nullptr;
        char tool[] = "CON:0/0/200/100/startup"; f.native.sm_ToolWindow = tool;
        f.publish(); assert(m68k_read32(f.c.get(), BLOCK + 36) == 0 && !borrowed);
        assert(!std::strcmp(reinterpret_cast<char *>(f.c->mem + m68k_read32(f.c.get(), BLOCK + 32)), tool));
        f.finish();
    }
    {
        Fixture f; f.args[1].wa_Name = nullptr; f.args[1].wa_Lock = 0;
        f.publish(); assert(m68k_read32(f.c.get(), BLOCK + 48) == 0 && m68k_read32(f.c.get(), BLOCK + 52) == 0);
        assert(borrowed == 2); f.finish();
    }
    {
        Fixture f; f.publish(); unsigned count = allocations;
        assert(!Kaffeine_StartupPublish(f.c.get()) && allocations == count && !replies); f.finish();
    }
    std::printf("PASS: typed WBStartup lifecycle and boundaries (%u cases)\n", cases);
}
