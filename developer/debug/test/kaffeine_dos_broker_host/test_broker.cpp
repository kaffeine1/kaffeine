/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#include "fixture_intern.h"
#include "kaffeine_dos_broker.h"
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <exec/memory.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

static std::atomic<unsigned> checks, failures;
static void check(bool ok, const char *label)
{
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}

struct Child
{
    Process process{};
    std::thread thread;
    std::string name, command, arguments, backend;
    std::map<std::string, std::string> variables;
    bool cli{}, copy_vars{}, deferred{}, release_start{}, finished{};
    bool fail_port{}, fail_var{}, fail_command{};
    ULONG stack{};
    BYTE priority{};
    LONG result{};
};

static std::mutex fixture_lock;
static std::condition_variable changed;
static std::vector<std::unique_ptr<Child>> children;
static std::map<void *, ULONG> allocations;
static std::set<SignalSemaphore *> semaphores;
static std::map<BPTR, unsigned> locks, streams;
static std::map<MsgPort *, Task *> inboxes;
static Process root_process;
static Task foreign_task;
static Library native_base;
static thread_local Task *current_task = &root_process.pr_Task;
static thread_local Child *current_child;
static thread_local SIPTR native_error;
static thread_local unsigned ioerr_reads;
static thread_local unsigned semaphore_depth;
static BPTR next_native = (BPTR)0x100000000ULL;
static unsigned opens, closes, cnp_calls, callbacks, replies, dynamic_ports, request_sends;
static bool fail_open, fail_dup, fail_alloc, fail_cnp;
static bool next_defer, next_fail_port, next_fail_var, next_fail_command;
static bool next_shared_streams;
static constexpr ULONG close_stream_flags[] = { PRF_CLOSEINPUT, PRF_CLOSEOUTPUT, PRF_CLOSEERROR };
static constexpr SIPTR injected_error = (SIPTR)0x12345678000000abULL;
static constexpr SIPTR driver_error = (SIPTR)0x23456789000000cdULL;
static constexpr SIPTR process_error = (SIPTR)0x3456789a000000efULL;

static BPTR make_lock_locked()
{
    BPTR value = ++next_native;
    locks[value] = 1;
    return value;
}

static BPTR make_lock()
{
    std::lock_guard<std::mutex> lock(fixture_lock);
    return make_lock_locked();
}

static unsigned live_locks()
{
    std::lock_guard<std::mutex> lock(fixture_lock);
    unsigned sum = 0;
    for (auto [address, count] : locks) { (void)address; sum += count; }
    return sum;
}

static unsigned refs(BPTR value)
{
    std::lock_guard<std::mutex> lock(fixture_lock);
    return locks[value];
}

extern "C" void InitSemaphore(SignalSemaphore *s)
{
    pthread_mutex_init(&s->mutex, nullptr);
    std::lock_guard<std::mutex> lock(fixture_lock);
    semaphores.insert(s);
}
extern "C" void ObtainSemaphore(SignalSemaphore *s)
{
    pthread_mutex_lock(&s->mutex);
    ++semaphore_depth;
}
extern "C" void ReleaseSemaphore(SignalSemaphore *s)
{
    --semaphore_depth;
    pthread_mutex_unlock(&s->mutex);
}
extern "C" APTR AllocVec(ULONG size, ULONG flags)
{
    check(!semaphore_depth, "record allocation holds no broker semaphore");
    if (fail_alloc) return nullptr;
    void *p = std::malloc(size);
    if (p)
    {
        std::memset(p, flags & MEMF_CLEAR ? 0 : 0xd9, size);
        std::lock_guard<std::mutex> lock(fixture_lock);
        allocations[p] = size;
    }
    return p;
}
extern "C" void FreeVec(APTR p)
{
    check(!semaphore_depth, "record is freed after releasing its semaphore");
    std::lock_guard<std::mutex> lock(fixture_lock);
    check(allocations.erase(p) == 1, "record is freed exactly once");
    auto s = static_cast<SignalSemaphore *>(p); /* semaphore is first field */
    if (semaphores.erase(s))
        check(!pthread_mutex_destroy(&s->mutex), "final record mutex has no waiter");
    std::free(p);
    native_error = 0x1101;
}
extern "C" Library *OpenLibrary(CONST_STRPTR name, ULONG version)
{
    check(!semaphore_depth, "library pin holds no broker semaphore");
    check(!std::strcmp(name, "kaffeine.library") && !version, "pins only controlled resident library");
    if (fail_open) return nullptr;
    std::lock_guard<std::mutex> lock(fixture_lock);
    ++opens;
    return &native_base;
}
extern "C" void CloseLibrary(Library *base)
{
    check(!semaphore_depth, "library unpin holds no broker semaphore");
    std::lock_guard<std::mutex> lock(fixture_lock);
    check(base == &native_base && closes < opens, "balances one existing private library pin");
    ++closes;
    native_error = 0x1102;
}
extern "C" Task *FindTask(CONST_STRPTR name)
{
    check(!name, "never finds a worker by a reusable native name");
    return current_task;
}
extern "C" SIPTR IoErr(void) { ++ioerr_reads; return native_error; }
extern "C" SIPTR SetIoErr(SIPTR value)
{
    SIPTR old = native_error;
    native_error = value;
    return old;
}
extern "C" BPTR DupLock(BPTR original)
{
    check(!semaphore_depth, "native DupLock holds no broker semaphore");
    if (fail_dup) { native_error = injected_error; return 0; }
    std::lock_guard<std::mutex> lock(fixture_lock);
    check(locks[original] == 1, "duplicates only a live full-width native reference");
    return make_lock_locked();
}
extern "C" BOOL UnLock(BPTR value)
{
    check(!semaphore_depth, "native UnLock holds no broker semaphore");
    std::lock_guard<std::mutex> lock(fixture_lock);
    check(locks[value] == 1, "each original or duplicate native lock is released once");
    if (locks[value]) --locks[value];
    native_error = 0x1103;
    return TRUE;
}
extern "C" BOOL SetVar(CONST_STRPTR name, CONST_STRPTR value, LONG size, ULONG flags)
{
    check(!semaphore_depth, "SetVar holds no broker semaphore");
    check(current_child && !std::strcmp(name, "KAFFEINE_CPU") && size == -1 &&
          flags == GVF_LOCAL_ONLY, "backend is installed only in worker local variables");
    if (current_child->fail_var) { native_error = injected_error; return FALSE; }
    current_child->backend = value;
    current_child->variables[name] = value;
    return TRUE;
}
extern "C" BOOL SetProgramName(CONST_STRPTR command)
{
    check(!semaphore_depth, "SetProgramName holds no broker semaphore");
    check(current_child && current_child->cli, "command is installed in the worker private CLI");
    if (current_child->fail_command) { native_error = injected_error; return FALSE; }
    current_child->command = command;
    return TRUE;
}
extern "C" void Delay(ULONG ticks)
{
    if (semaphore_depth) check(false, "native DOS wait holds no broker semaphore");
    if (ticks != 1) check(false, "polling advances in bounded single ticks");
    native_error = 0x1104; /* prove that broker-internal polling preserves IoErr */
    std::this_thread::sleep_for(std::chrono::microseconds(150));
}

static void add_tail(List *list, Node *node)
{
    node->ln_Succ = reinterpret_cast<Node *>(&list->lh_Tail);
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
}
static Message *head(MsgPort *port)
{
    return port->mp_MsgList.lh_Head->ln_Succ ?
        reinterpret_cast<Message *>(port->mp_MsgList.lh_Head) : nullptr;
}
static Message *remove_head(MsgPort *port)
{
    Message *message = head(port);
    if (message)
    {
        Node *node = &message->mn_Node;
        node->ln_Pred->ln_Succ = node->ln_Succ;
        node->ln_Succ->ln_Pred = node->ln_Pred;
        node->ln_Succ = node->ln_Pred = nullptr;
    }
    return message;
}
extern "C" MsgPort *CreateMsgPort(void)
{
    check(!semaphore_depth && current_child, "private inbox is allocated on worker owner only");
    if (current_child->fail_port) return nullptr; /* Exec failure does not set IoErr */
    auto port = new MsgPort{};
    port->mp_Node.ln_Type = NT_MSGPORT;
    port->mp_Flags = PA_SIGNAL;
    port->mp_SigBit = 17;
    port->mp_SigTask = current_task;
    NEWLIST(&port->mp_MsgList);
    std::lock_guard<std::mutex> lock(fixture_lock);
    inboxes[port] = current_task;
    ++dynamic_ports;
    return port;
}
extern "C" void DeleteMsgPort(MsgPort *port)
{
    check(!semaphore_depth, "inbox deletion holds no broker semaphore");
    std::lock_guard<std::mutex> lock(fixture_lock);
    check(inboxes.count(port) && inboxes[port] == current_task,
          "inbox is deleted on its still-live native owner");
    check(!head(port), "private inbox is empty before deletion");
    inboxes.erase(port);
    --dynamic_ports;
    delete port;
    native_error = 0x1105;
}
extern "C" void PutMsg(MsgPort *port, Message *message)
{
    std::lock_guard<std::mutex> lock(fixture_lock);
    if (port->mp_Flags == PA_SIGNAL)
    {
        check(inboxes.count(port) && port->mp_SigTask == inboxes[port],
              "request targets a live private inbox, never a stale CNP pointer");
        check(current_task == &root_process.pr_Task ||
              (current_child && current_task == &current_child->process.pr_Task),
              "requests come only from driver or its live private helper coordinator");
        ++request_sends;
    }
    else
    {
        check(port->mp_Flags == PA_IGNORE && !port->mp_SigTask,
              "reply port needs no driver signal or foreign SignalTask");
        check(!semaphore_depth, "reply holds no broker semaphore");
    }
    message->mn_Node.ln_Type = NT_MESSAGE;
    add_tail(&port->mp_MsgList, &message->mn_Node);
    changed.notify_all();
}
extern "C" Message *GetMsg(MsgPort *port)
{
    std::lock_guard<std::mutex> lock(fixture_lock);
    return remove_head(port);
}
extern "C" Message *WaitPort(MsgPort *port)
{
    if (semaphore_depth) check(false, "waiting worker holds no broker semaphore");
    std::unique_lock<std::mutex> lock(fixture_lock);
    if (!inboxes.count(port) || inboxes[port] != current_task)
        check(false, "worker waits only on its own private native inbox");
    changed.wait(lock, [port] { return head(port) != nullptr; });
    return head(port);
}
extern "C" void ReplyMsg(Message *message)
{
    {
        std::lock_guard<std::mutex> lock(fixture_lock);
        ++replies;
    }
    PutMsg(message->mn_ReplyPort, message);
}

extern "C" Process *CreateNewProc(const TagItem *tags)
{
    check(!semaphore_depth, "native CNP holds no broker semaphore");
    ++cnp_calls;
    if (fail_cnp) { native_error = injected_error; return nullptr; }
    auto child = std::make_unique<Child>();
    LONG (*entry)(void) = nullptr;
    bool home_given = false;
    for (const TagItem *tag = tags; tag->ti_Tag != TAG_DONE; ++tag)
    {
        switch (tag->ti_Tag)
        {
            case TAG_IGNORE: break;
            case NP_Entry: entry = reinterpret_cast<LONG (*)(void)>(tag->ti_Data); break;
            case NP_UserData: child->process.pr_Task.tc_UserData = reinterpret_cast<void *>(tag->ti_Data); break;
            case NP_Name: child->name = reinterpret_cast<const char *>(tag->ti_Data); break;
            case NP_StackSize: child->stack = static_cast<ULONG>(tag->ti_Data); break;
            case NP_Priority: child->priority = static_cast<BYTE>(tag->ti_Data); break;
            case NP_Cli: child->cli = tag->ti_Data != 0; break;
            case NP_CopyVars: child->copy_vars = tag->ti_Data != 0; break;
            case NP_Arguments: child->arguments = reinterpret_cast<const char *>(tag->ti_Data); break;
            case NP_HomeDir: child->process.pr_HomeDir = static_cast<BPTR>(tag->ti_Data); home_given = true; break;
            case NP_ConsoleTask: case NP_WindowPtr: check(!tag->ti_Data, "no inherited console/window pointer"); break;
            case NP_FreeSeglist: check(tag->ti_Data, "native DOS owns only artificial ARM entry segment"); break;
            default: check(false, "no guest seglist, raw guest entry, or inherited streams in CNP tags"); break;
        }
    }
    check(entry && child->process.pr_Task.tc_UserData,
          "native CNP receives controlled compiled entry and private userdata");
    child->process.pr_Arguments = child->arguments.data();
    child->process.pr_CLI = child->cli ? (BPTR)0x900000001ULL : 0;
    child->process.pr_Flags = PRF_CLOSEINPUT | PRF_CLOSEOUTPUT | PRF_CLOSEERROR;
    Process *parent = current_child ? &current_child->process : &root_process;
    {
        std::lock_guard<std::mutex> lock(fixture_lock);
        if (!home_given && parent->pr_HomeDir) child->process.pr_HomeDir = make_lock_locked();
        if (parent->pr_CurrentDir) child->process.pr_CurrentDir = make_lock_locked();
        BPTR *stream_fields[] = { &child->process.pr_CIS, &child->process.pr_COS, &child->process.pr_CES };
        for (BPTR *stream : stream_fields) { *stream = ++next_native; streams[*stream] = 1; }
        if (next_shared_streams)
        {
            /* Model caller-supplied aliases without leaking the unused NILs. */
            streams[child->process.pr_COS] = streams[child->process.pr_CES] = 0;
            child->process.pr_COS = child->process.pr_CES = child->process.pr_CIS;
        }
    }
    next_shared_streams = false;
    if (child->copy_vars)
        child->variables = current_child ? current_child->variables : std::map<std::string, std::string>{{"ROOT_ONLY", "root"}};
    child->deferred = next_defer;
    child->fail_port = next_fail_port;
    child->fail_var = next_fail_var;
    child->fail_command = next_fail_command;
    next_defer = next_fail_port = next_fail_var = next_fail_command = false;
    Child *raw = child.get();
    {
        std::lock_guard<std::mutex> lock(fixture_lock);
        children.push_back(std::move(child));
    }
    raw->thread = std::thread([raw, entry] {
        current_child = raw;
        current_task = &raw->process.pr_Task;
        native_error = 0; /* native DosEntry starts with an independent IoErr */
        {
            std::unique_lock<std::mutex> lock(fixture_lock);
            if (raw->deferred) changed.wait(lock, [raw] { return raw->release_start; });
        }
        /* DosEntry saves these before initialPC, then reads the close flags
         * live after it returns. SelectInput/Output do not change the saved BPTRs. */
        const BPTR startup_streams[] = { raw->process.pr_CIS, raw->process.pr_COS, raw->process.pr_CES };
        raw->result = entry();
        check(!raw->process.pr_Task.tc_UserData,
              "worker clears private userdata before native DOS cleanup");
        if (raw->process.pr_HomeDir) UnLock(raw->process.pr_HomeDir);
        if (raw->process.pr_CurrentDir) UnLock(raw->process.pr_CurrentDir);
        {
            std::lock_guard<std::mutex> lock(fixture_lock);
            for (unsigned i = 0; i < 3; ++i)
            {
                BPTR stream = startup_streams[i];
                if ((raw->process.pr_Flags & close_stream_flags[i]) && stream)
                {
                    check(streams[stream] == 1, "native DOS closes each armed saved startup stream once");
                    if (streams[stream]) --streams[stream];
                }
            }
            raw->finished = true;
            changed.notify_all();
        }
    });
    return reinterpret_cast<Process *>(1); /* deliberately never dereferenceable */
}

static void join_finished(bool release_deferred = false)
{
    if (release_deferred)
    {
        std::lock_guard<std::mutex> lock(fixture_lock);
        for (auto &child : children) child->release_start = true;
        changed.notify_all();
    }
    for (auto &child : children)
        if (child->thread.joinable()) child->thread.join();
}

static KaffeineDosBrokerStart spec_for(BPTR home)
{
    KaffeineDosBrokerStart spec{};
    spec.name = "guest-worker";
    spec.command = "GuestCommand";
    spec.arguments = "one two\n";
    spec.backend = "jit";
    spec.home_dir = home;
    spec.stack_size = 5000;
    spec.cli = TRUE;
    spec.copy_vars = FALSE;
    spec.priority = -2;
    return spec;
}

struct Cpu { ULONG value; };
struct CallbackState
{
    KaffeineDosBroker *nested{};
    BPTR nested_home{};
    Task *worker{};
    ULONG last_value{};
    bool cleaned{}, cleanup_entered{}, release_cleanup{}, idle_entered{}, release_idle{};
};

static void cleanup(KaffeineContext *ctx)
{
    auto state = static_cast<CallbackState *>(ctx->fixture);
    check(current_child && current_task == state->worker,
          "after_thunk runs on exactly the callback's native Process");
    check(ctx->broker_owner == current_task && ctx->broker_call,
          "after_thunk retains only the private scoped native DOS capability");
    check(!ctx->after_thunk, "after_thunk is cleared before invoking cleanup");
    ++ctx->cleanup_count;
    state->cleaned = true;
    native_error = 0x1106;
}
static IPTR set_error(KaffeineContext *ctx, void *opaque_cpu)
{
    auto cpu = static_cast<Cpu *>(opaque_cpu);
    auto state = static_cast<CallbackState *>(ctx->fixture);
    check(!semaphore_depth, "compiled callback runs outside broker semaphore");
    check(current_child && current_task != ctx->exec_task,
          "compiled thunk executes on a separate native Process");
    check(ctx->exec_task == &root_process.pr_Task,
          "broker preserves stable driver Exec-I/O owner");
    check(ctx->broker_owner == current_task && ctx->broker_call,
          "compiled callback has scoped broker owner without changing driver");
    check(current_child->cli && current_child->process.pr_CLI &&
          current_child->command == "GuestCommand", "native Process has private CLI and command");
    check(current_child->arguments == "one two\n" &&
          current_child->process.pr_Arguments == current_child->arguments.data(),
          "worker has native CNP's copied arguments");
    check(current_child->backend == "jit" && current_child->variables.count("ROOT_ONLY") == 0,
          "copy_vars false and local backend do not inherit or change root variables");
    check(current_child->process.pr_HomeDir > UINT32_MAX &&
          current_child->process.pr_CurrentDir > UINT32_MAX,
          "native current/home directories retain full-width private handles");
    check(current_child->process.pr_CIS != current_child->process.pr_COS &&
          current_child->process.pr_CIS != root_process.pr_CIS,
          "native Process owns independent NIL input/output/error streams");
    state->worker = current_task;
    state->last_value = cpu->value;
    ctx->cpu_value = cpu->value + 1;
    ctx->after_thunk = cleanup;
    native_error = process_error + cpu->value;
    ++callbacks;
    return (IPTR)0x7654321000000000ULL | cpu->value;
}
static IPTR initialize_view(KaffeineContext *ctx, void *)
{
    check(current_child && current_task != ctx->exec_task && ctx->guest_task == 0x2000,
          "LVO-zero private initialization helper sees selected guest view on native DOS owner");
    check(ctx->broker_owner == current_task && ctx->broker_call,
          "private initialization helper has native DOS owner capability");
    check(!ctx->after_thunk, "initialization has no pending foreign-process cleanup");
    ++callbacks;
    return 1;
}
static IPTR get_error(KaffeineContext *ctx, void *)
{
    auto state = static_cast<CallbackState *>(ctx->fixture);
    check(state->cleaned && current_task == state->worker,
          "next call observes completed cleanup on the same native Process");
    check(ctx->broker_owner == current_task && ctx->broker_call,
          "later call reestablishes the worker capability only during execution");
    ++callbacks;
    return static_cast<IPTR>(IoErr());
}
static IPTR inspect_nested(KaffeineContext *ctx, void *);
static IPTR rollback_init(KaffeineContext *ctx, void *)
{
    check(current_child && current_child->name == "rollback-worker" &&
          ctx->broker_owner == current_task, "failed child init runs only on its native DOS owner");
    ++callbacks;
    return 0;
}
static IPTR rollback_drop(KaffeineContext *ctx, void *cpu)
{
    check(!cpu && current_child && current_child->name == "rollback-worker" &&
          ctx->broker_owner == current_task, "rollback Finish drops metadata on child owner before native cleanup");
    ++callbacks;
    return 1;
}
static IPTR nested_start(KaffeineContext *ctx, void *cpu)
{
    auto state = static_cast<CallbackState *>(ctx->fixture);
    KaffeineDosBrokerStart spec = spec_for(state->nested_home);
    spec.name = "grandchild-worker";
    spec.copy_vars = TRUE;
    SIPTR error = -1;
    native_error = process_error;
    state->nested = Kaffeine_DosBrokerStart(ctx->exec_task, &spec, &error);
    check(state->nested && !error && IoErr() == process_error,
          "nested Start in parent broker preserves its own IoErr and uses fixed driver");
    Task *parent_owner = ctx->broker_owner;
    IPTR result = 0;
    check(Kaffeine_DosBrokerCall(state->nested, ctx, cpu, 0, inspect_nested, &result, &error) &&
          result == 23 && ctx->broker_owner == parent_owner && IoErr() == process_error,
          "parent broker may coordinate trusted child initialization and restores its own capability");
    check(!Kaffeine_DosBrokerCall(state->nested, ctx, cpu, 132, inspect_nested, &result, &error),
          "parent broker cannot invoke ordinary child DOS thunks through helper exception");
    ctx->broker_call = FALSE;
    check(!Kaffeine_DosBrokerCall(state->nested, ctx, cpu, 0, inspect_nested, &result, &error),
          "nested helper without outstanding driver RPC capability is refused");
    ctx->broker_call = TRUE;
    ctx->broker_owner = &foreign_task;
    check(!Kaffeine_DosBrokerCall(state->nested, ctx, cpu, 0, inspect_nested, &result, &error),
          "nested helper cannot claim another private native owner");
    ctx->broker_owner = parent_owner;
    ctx->exec_task = &foreign_task;
    check(!Kaffeine_DosBrokerCall(state->nested, ctx, cpu, 0, inspect_nested, &result, &error),
          "nested helper cannot coordinate a broker for another native domain driver");
    ctx->exec_task = &root_process.pr_Task;
    auto failed_spec = spec_for(make_lock());
    failed_spec.name = "rollback-worker";
    auto failed = Kaffeine_DosBrokerStart(ctx->exec_task, &failed_spec, &error);
    check(failed && Kaffeine_DosBrokerCall(failed, ctx, cpu, 0, rollback_init, &result, &error) && !result,
          "child init failure remains a callback result, not a transport failure");
    check(Kaffeine_DosBrokerFinish(failed, ctx, rollback_drop) &&
          ctx->broker_owner == parent_owner && IoErr() == process_error,
          "parent broker coordinates failed child cleanup before publication and retains its capability");
    ++callbacks;
    return state->nested ? 1 : 0;
}
static IPTR inspect_nested(KaffeineContext *ctx, void *)
{
    check(current_child && current_child->name == "grandchild-worker" &&
          current_child->variables.count("KAFFEINE_CPU") == 1,
          "grandchild has copied parent local vars and separate native Process");
    check(ctx->exec_task == &root_process.pr_Task, "nested Start never rebinds driver to parent broker");
    check(ctx->broker_owner == current_task && ctx->broker_call,
          "child helper runs with its own capability while parent broker and driver are paused");
    ++callbacks;
    return 23;
}

static void test_positive()
{
    const unsigned initial_locks = live_locks();
    BPTR home = make_lock();
    auto spec = spec_for(home);
    char name[] = "guest-worker";
    char command[] = "GuestCommand";
    char arguments[] = "one two\n";
    char backend[] = "jit";
    spec.name = name; spec.command = command; spec.arguments = arguments; spec.backend = backend;
    native_error = driver_error;
    SIPTR error = -1;
    auto broker = Kaffeine_DosBrokerStart(&root_process.pr_Task, &spec, &error);
    check(broker && !error && IoErr() == driver_error,
          "Start succeeds using CNP sentinel 1 and preserves full-width caller IoErr");
    check(refs(home) == 0, "successful Start consumes original home reference exactly once");
    std::memset(name, 'x', sizeof(name)-1);
    std::memset(command, 'x', sizeof(command)-1);
    std::memset(arguments, 'x', sizeof(arguments)-1);
    std::memset(backend, 'x', sizeof(backend)-1);
    Cpu cpu{};
    CallbackState state;
    KaffeineContext ctx{};
    ctx.exec_task = &root_process.pr_Task; ctx.fixture = &state; ctx.guest_task = 0x2000;
    ctx.broker_call = TRUE;
    IPTR init_result = 0;
    check(Kaffeine_DosBrokerCall(broker, &ctx, &cpu, 0, initialize_view, &init_result, &error) &&
          init_result == 1 && !error && IoErr() == driver_error,
          "scheduler can invoke trusted initialization helper with diagnostic LVO zero");
    for (ULONG i = 0; i < 64; ++i)
    {
        cpu.value = i;
        state.cleaned = false;
        IPTR result = 0;
        check(Kaffeine_DosBrokerCall(broker, &ctx, &cpu, 30, set_error, &result, &error),
              "compiled callback completes synchronous native message RPC");
        check(!error && result == ((IPTR)0x7654321000000000ULL | i),
              "RPC preserves complete native return width and correct request result");
        check(ctx.cpu_value == i+1 && ctx.cleanup_count == i+1 && state.last_value == i,
              "driver resumes after callback state and cleanup are published");
        check(!ctx.after_thunk && !ctx.broker_owner && IoErr() == driver_error,
              "driver sees cleared after_thunk/native capability and its own unchanged IoErr");
        check(Kaffeine_DosBrokerCall(broker, &ctx, &cpu, 132, get_error, &result, &error) &&
              result == static_cast<IPTR>(process_error + i),
              "worker IoErr persists across calls and cleanup that changes IoErr");
        check(IoErr() == driver_error, "worker IoErr is never installed in root native Process");
    }
    unsigned before_callbacks = callbacks;
    IPTR result = 9;
    current_task = &foreign_task;
    native_error = injected_error;
    check(!Kaffeine_DosBrokerCall(broker, &ctx, &cpu, 30, set_error, &result, &error) &&
          !result && error == ERROR_BAD_NUMBER && IoErr() == injected_error,
          "foreign native caller is refused without executing callback or changing its IoErr");
    check(!Kaffeine_DosBrokerStop(broker) && IoErr() == injected_error,
          "foreign task cannot stop a driver-owned broker");
    current_task = &root_process.pr_Task;
    native_error = driver_error;
    check(callbacks == before_callbacks, "refused transport never calls a thunk on root or worker");
    check(!Kaffeine_DosBrokerCall(broker, nullptr, &cpu, 30, set_error, &result, &error),
          "missing trusted context is refused before publication");
    check(!Kaffeine_DosBrokerCall(broker, &ctx, nullptr, 30, set_error, &result, &error),
          "missing trusted CPU is refused before publication");
    check(!Kaffeine_DosBrokerCall(broker, &ctx, &cpu, 30, nullptr, &result, &error),
          "missing compiled callback is refused before publication");
    check(IoErr() == driver_error, "transport refusal preserves complete-width driver error");
    check(Kaffeine_DosBrokerStop(broker) && IoErr() == driver_error,
          "owner Stop acknowledges cleanup and preserves driver IoErr");
    check(!dynamic_ports && closes == opens,
          "Stop reply follows inbox deletion and private library unpin");
    join_finished();
    check(live_locks() == initial_locks, "native DOS balances worker duplicate home and current dir");
}

static void test_start_failures()
{
    enum Failure { OPEN, DUP, ALLOC, CNP, PORT, VAR, COMMAND };
    for (Failure which : { OPEN, DUP, ALLOC, CNP, PORT, VAR, COMMAND })
    {
        unsigned initial_locks = live_locks();
        BPTR home = make_lock();
        auto spec = spec_for(home);
        fail_open = which == OPEN;
        fail_dup = which == DUP;
        fail_alloc = which == ALLOC;
        fail_cnp = which == CNP;
        next_fail_port = which == PORT;
        next_fail_var = which == VAR;
        next_fail_command = which == COMMAND;
        SIPTR error = -1;
        native_error = driver_error;
        auto broker = Kaffeine_DosBrokerStart(&root_process.pr_Task, &spec, &error);
        check(!broker && IoErr() == driver_error, "startup failure leaves caller IoErr untouched");
        SIPTR expected = which == OPEN ? ERROR_INVALID_RESIDENT_LIBRARY :
                         which == ALLOC || which == PORT ? ERROR_NO_FREE_STORE : injected_error;
        check(error == expected, "startup failure reports exact captured native error or explicit allocation error");
        check(refs(home) == 1, "every failed Start retains original caller home reference");
        fail_open = fail_dup = fail_alloc = fail_cnp = false;
        next_fail_port = next_fail_var = next_fail_command = false;
        join_finished();
        UnLock(home);
        check(live_locks() == initial_locks, "failure rollback balances only worker/native duplicate directories");
        check(allocations.empty() && !dynamic_ports && opens == closes,
              "early worker exit/OOM/CNP failure leaves no record, inbox or library pin");
    }
    BPTR home = make_lock();
    auto spec = spec_for(home);
    spec.backend = "unsafe";
    unsigned cnp_before = cnp_calls;
    SIPTR error = -1;
    native_error = driver_error;
    check(!Kaffeine_DosBrokerStart(&root_process.pr_Task, &spec, &error) &&
          error == ERROR_BAD_NUMBER && IoErr() == driver_error,
          "invalid copied spec is refused before native resources or CNP");
    check(cnp_calls == cnp_before && refs(home) == 1, "preflight cannot consume home or create process");
    UnLock(home);
}

static void test_timeout()
{
    unsigned initial_locks = live_locks();
    BPTR home = make_lock();
    auto spec = spec_for(home);
    next_defer = true;
    SIPTR error = -1;
    native_error = driver_error;
    auto broker = Kaffeine_DosBrokerStart(&root_process.pr_Task, &spec, &error);
    check(!broker && error == ERROR_BREAK && IoErr() == driver_error,
          "bounded Start aborts when native process has not entered yet");
    check(refs(home) == 1 && !allocations.empty() && opens == closes + 1,
          "timeout retains caller home and worker's future userdata/library lease");
    join_finished(true);
    check(allocations.empty() && opens == closes && !dynamic_ports,
          "late worker observes abort before startup and retires its remaining reference");
    UnLock(home);
    check(live_locks() == initial_locks, "late abort and native DOS cleanup balance all duplicate directories");
}

static void test_nested()
{
    unsigned initial_locks = live_locks();
    BPTR home = make_lock();
    auto spec = spec_for(home);
    SIPTR error = -1;
    native_error = driver_error;
    auto broker = Kaffeine_DosBrokerStart(&root_process.pr_Task, &spec, &error);
    CallbackState state;
    state.nested_home = make_lock();
    KaffeineContext ctx{};
    ctx.exec_task = &root_process.pr_Task; ctx.fixture = &state;
    ctx.broker_call = TRUE;
    Cpu cpu{};
    IPTR result = 0;
    check(broker && Kaffeine_DosBrokerCall(broker, &ctx, &cpu, 498,
          nested_start, &result, &error) && result == 1,
          "DOS callback may synchronously Start grandchild broker while driver is paused");
    check(refs(state.nested_home) == 0 && IoErr() == driver_error,
          "nested success consumes original home and leaves root IoErr alone");
    check(Kaffeine_DosBrokerCall(state.nested, &ctx, &cpu, 132,
          inspect_nested, &result, &error) && result == 23,
          "fixed root driver can RPC to grandchild created inside parent broker");
    check(Kaffeine_DosBrokerStop(state.nested), "driver stops grandchild independently");
    check(Kaffeine_DosBrokerStop(broker), "driver stops parent independently");
    join_finished();
    check(live_locks() == initial_locks && allocations.empty() && opens == closes,
          "nested private Process resources and library pins are balanced");
}

static void test_zero_home_and_no_cli()
{
    unsigned initial_locks = live_locks();
    auto spec = spec_for(0);
    spec.cli = FALSE;
    spec.command = nullptr;
    spec.arguments = nullptr;
    spec.copy_vars = TRUE;
    spec.stack_size = KAFFEINE_DOS_BROKER_MAX_STACK;
    SIPTR error = -1;
    native_error = driver_error;
    auto broker = Kaffeine_DosBrokerStart(&root_process.pr_Task, &spec, &error);
    check(broker && !error && IoErr() == driver_error,
          "zero home inherits a private native duplicate; absent optional strings and CLI false work");
    check(Kaffeine_DosBrokerStop(broker), "zero-home non-CLI worker stops without a segment payload");
    join_finished();
    check(live_locks() == initial_locks, "inherited native home/current refs are independently cleaned");
}

static void test_process_isolation()
{
    const unsigned initial_locks = live_locks();
    auto spec_a = spec_for(make_lock());
    auto spec_b = spec_for(make_lock());
    spec_a.name = "isolated-a";
    spec_b.name = "isolated-b";
    SIPTR error = -1;
    native_error = driver_error;
    auto a = Kaffeine_DosBrokerStart(&root_process.pr_Task, &spec_a, &error);
    auto b = Kaffeine_DosBrokerStart(&root_process.pr_Task, &spec_b, &error);
    CallbackState state_a, state_b;
    KaffeineContext ctx{};
    ctx.exec_task = &root_process.pr_Task;
    ctx.broker_call = TRUE;
    Cpu cpu{};
    IPTR result = 0;
    ctx.fixture = &state_a; cpu.value = 0x100;
    check(a && b && Kaffeine_DosBrokerCall(a, &ctx, &cpu, 30, set_error, &result, &error),
          "first guest Process performs DOS call through its own broker");
    ctx.fixture = &state_b; cpu.value = 0x200;
    check(Kaffeine_DosBrokerCall(b, &ctx, &cpu, 30, set_error, &result, &error),
          "second guest Process performs DOS call through a distinct broker");
    check(state_a.worker != state_b.worker && state_a.worker != ctx.exec_task &&
          state_b.worker != ctx.exec_task, "two guest Processes have distinct live native DOS owners");
    ctx.fixture = &state_a;
    check(Kaffeine_DosBrokerCall(a, &ctx, &cpu, 132, get_error, &result, &error) &&
          result == static_cast<IPTR>(process_error + 0x100),
          "second Process DOS error does not overwrite first Process IoErr");
    check(Kaffeine_DosBrokerStop(a), "first Process may exit while second stays live");
    ctx.fixture = &state_b;
    check(Kaffeine_DosBrokerCall(b, &ctx, &cpu, 132, get_error, &result, &error) &&
          result == static_cast<IPTR>(process_error + 0x200),
          "surviving Process retains native DOS state after peer Stop");
    check(Kaffeine_DosBrokerStop(b) && IoErr() == driver_error,
          "last Process Stop still preserves native root error");
    join_finished();
    check(live_locks() == initial_locks && allocations.empty() && !dynamic_ports && opens == closes,
          "interleaved Process owners retire independent resources exactly once");
}

static IPTR final_cleanup(KaffeineContext *ctx, void *cpu)
{
    auto state = static_cast<CallbackState *>(ctx->fixture);
    check(!cpu && current_child && current_task == state->worker &&
          ctx->broker_owner == current_task && ctx->broker_call,
          "Finish cleanup(ctx,NULL) runs with this Process's scoped native DOS capability");
    check(!semaphore_depth, "final native-owner cleanup holds no broker semaphore");
    {
        std::unique_lock<std::mutex> lock(fixture_lock);
        bool own_port = false;
        for (auto [port, owner] : inboxes) { (void)port; own_port |= owner == current_task; }
        check(own_port && opens > closes, "Finish cleanup precedes inbox deletion and library unpin");
        check(locks[current_child->process.pr_HomeDir] == 1 &&
              streams[current_child->process.pr_CIS] == 1,
              "native DOS still owns its streams and directories during typed metadata cleanup");
        state->cleanup_entered = true;
        changed.notify_all();
        changed.wait(lock, [state] { return state->release_cleanup; });
    }
    ++ctx->cleanup_count;
    ctx->after_thunk = cleanup;
    native_error = injected_error;
    ++callbacks;
    return 1;
}

static void test_finish_cleanup()
{
    const unsigned initial_locks = live_locks();
    auto spec = spec_for(make_lock());
    SIPTR error = -1;
    native_error = driver_error;
    auto broker = Kaffeine_DosBrokerStart(&root_process.pr_Task, &spec, &error);
    CallbackState state;
    KaffeineContext ctx{};
    ctx.exec_task = &root_process.pr_Task; ctx.broker_call = TRUE; ctx.fixture = &state;
    Cpu cpu{};
    IPTR result = 0;
    check(broker && Kaffeine_DosBrokerCall(broker, &ctx, &cpu, 30, set_error, &result, &error),
          "broker completes last guest call before final typed resource cleanup");
    check(!Kaffeine_DosBrokerFinish(broker, nullptr, final_cleanup) &&
          !Kaffeine_DosBrokerFinish(broker, &ctx, nullptr) && IoErr() == driver_error,
          "incomplete Finish cleanup pair refuses before submitting or consuming broker");
    std::thread release([&] {
        std::unique_lock<std::mutex> lock(fixture_lock);
        changed.wait(lock, [&] { return state.cleanup_entered; });
        check(ctx.broker_owner == state.worker && ctx.exec_task == &root_process.pr_Task &&
              dynamic_ports && opens > closes,
              "driver cannot receive Finish ack while worker cleanup is still active");
        state.release_cleanup = true;
        changed.notify_all();
    });
    check(Kaffeine_DosBrokerFinish(broker, &ctx, final_cleanup),
          "Finish acknowledges only after compiled cleanup and worker retirement");
    release.join();
    check(!ctx.broker_owner && !ctx.after_thunk && ctx.cleanup_count == 3 &&
          IoErr() == driver_error, "Finish publishes cleanup/after_thunk and clears capability before ack");
    check(!dynamic_ports && opens == closes,
          "Finish reply is later than native inbox deletion and library unpin");
    join_finished();
    check(live_locks() == initial_locks && allocations.empty(),
          "native DOS cleanup after Finish closes only its private resources once");
}

static IPTR wait_for_idle_probe(KaffeineContext *ctx, void *)
{
    auto state = static_cast<CallbackState *>(ctx->fixture);
    std::unique_lock<std::mutex> lock(fixture_lock);
    state->idle_entered = true;
    changed.notify_all();
    changed.wait(lock, [state] { return state->release_idle; });
    ++callbacks;
    return 17;
}

static void test_idle_probe()
{
    const unsigned initial_locks = live_locks();
    auto spec = spec_for(make_lock());
    SIPTR error = -1;
    native_error = driver_error;
    auto broker = Kaffeine_DosBrokerStart(&root_process.pr_Task, &spec, &error);
    check(broker && !Kaffeine_DosBrokerIsIdle(nullptr) && Kaffeine_DosBrokerIsIdle(broker) &&
          IoErr() == driver_error, "new owned broker is idle; metadata probe leaves caller DOS state unchanged");
    CallbackState state;
    KaffeineContext ctx{};
    ctx.exec_task = &root_process.pr_Task; ctx.broker_call = TRUE; ctx.fixture = &state;
    Cpu cpu{};
    IPTR result = 0;
    std::thread observer([&] {
        current_task = &foreign_task;
        native_error = injected_error;
        std::unique_lock<std::mutex> lock(fixture_lock);
        changed.wait(lock, [&] { return state.idle_entered; });
        unsigned char before[sizeof(ctx)];
        std::memcpy(before, &ctx, sizeof(ctx));
        const unsigned reads_before = ioerr_reads;
        check(!Kaffeine_DosBrokerIsIdle(broker), "metadata-only probe detects the actual outstanding native callback");
        check(ioerr_reads == reads_before && native_error == injected_error,
              "busy probe uses no native IoErr or other DOS operation on observer");
        check(!std::memcmp(before, &ctx, sizeof(ctx)), "busy probe cannot install or mutate a Context view");
        state.release_idle = true;
        changed.notify_all();
    });
    check(Kaffeine_DosBrokerCall(broker, &ctx, &cpu, 30, wait_for_idle_probe, &result, &error) && result == 17,
          "driver remains paused until callback and busy probe barrier complete");
    observer.join();
    check(Kaffeine_DosBrokerIsIdle(broker) && !ctx.broker_owner && IoErr() == driver_error,
          "probe becomes ready only after RPC returns and driver regains ownership");
    check(Kaffeine_DosBrokerStop(broker), "serial driver may stop after idle probe without another submitter");
    join_finished();
    check(live_locks() == initial_locks && allocations.empty() && opens == closes,
          "busy-then-ready probe changes no ownership or cleanup accounting");
}

struct StreamExitState
{
    Child *child{};
    BPTR startup[3]{}, replacement{}, reused{};
    SIPTR error{process_error};
    ULONG disarmed{};
    bool close_original{};
};

static IPTR replace_selected_streams(KaffeineContext *ctx, void *)
{
    auto state = static_cast<StreamExitState *>(ctx->fixture);
    check(current_child && ctx->broker_owner == current_task && ctx->broker_call,
          "stream selections change only in the selected native broker Process");
    state->child = current_child;
    Process &process = current_child->process;
    state->startup[0] = process.pr_CIS;
    state->startup[1] = process.pr_COS;
    state->startup[2] = process.pr_CES;
    check(process.pr_Flags == (PRF_CLOSEINPUT | PRF_CLOSEOUTPUT | PRF_CLOSEERROR),
          "native CNP starts with each default startup close flag armed");
    {
        std::lock_guard<std::mutex> lock(fixture_lock);
        state->replacement = ++next_native;
        streams[state->replacement] = 1;
        process.pr_CIS = process.pr_COS = process.pr_CES = state->replacement;
        if (state->close_original)
        {
            /* Model a typed Close preflight: all matching saved slots must be
             * disarmed before Close, including slots no longer selected. */
            state->reused = state->startup[1];
            for (unsigned i = 0; i < 3; ++i)
                if (state->startup[i] == state->reused)
                    state->disarmed |= close_stream_flags[i];
            process.pr_Flags &= ~state->disarmed;
            check(streams[state->reused] == 1, "explicit Close sees one live original native stream");
            --streams[state->reused];
            check(!streams[state->reused], "explicit Close retires the original before address reuse");
            streams[state->reused] = 1; /* a different owned Open reuses its BPTR */
        }
    }
    native_error = state->error;
    ++callbacks;
    return state->replacement;
}

static IPTR inspect_selected_streams(KaffeineContext *ctx, void *)
{
    auto state = static_cast<StreamExitState *>(ctx->fixture);
    check(current_child == state->child && ctx->broker_owner == current_task &&
          ctx->broker_call && IoErr() == state->error,
          "surviving broker keeps its selected streams and full-width native error");
    check(current_child->process.pr_CIS == state->replacement &&
          current_child->process.pr_COS == state->replacement &&
          current_child->process.pr_CES == state->replacement,
          "shared selected replacement remains independent of the startup close identities");
    ++callbacks;
    return 1;
}

static IPTR finish_selected_streams(KaffeineContext *ctx, void *cpu)
{
    auto state = static_cast<StreamExitState *>(ctx->fixture);
    check(!cpu && current_child == state->child && ctx->broker_owner == current_task && ctx->broker_call,
          "selected-stream Finish cleanup runs on its captured native Process before exit");
    {
        std::lock_guard<std::mutex> lock(fixture_lock);
        for (BPTR stream : state->startup)
            check(streams[stream] == 1, "saved originals or their fresh replacements remain live before native exit");
        check(streams[state->replacement] == 1 &&
              !(current_child->process.pr_Flags & state->disarmed),
              "Finish sees live owned selection and preserves all explicit-Close disarms");
    }
    native_error = injected_error;
    ++callbacks;
    return 1;
}

static void retire_test_stream(BPTR stream)
{
    check(streams[stream] == 1, "test-owned replacement survives until its explicit owner cleanup");
    if (streams[stream]) --streams[stream];
}

static void test_saved_startup_streams()
{
    const unsigned initial_locks = live_locks();
    auto spec_a = spec_for(make_lock());
    auto spec_b = spec_for(make_lock());
    SIPTR error = -1;
    native_error = driver_error;
    auto a = Kaffeine_DosBrokerStart(&root_process.pr_Task, &spec_a, &error);
    auto b = Kaffeine_DosBrokerStart(&root_process.pr_Task, &spec_b, &error);
    StreamExitState state_a, state_b;
    state_b.error = process_error + 1;
    KaffeineContext ctx{};
    ctx.exec_task = &root_process.pr_Task; ctx.broker_call = TRUE;
    Cpu cpu{};
    IPTR result = 0;
    ctx.fixture = &state_a;
    check(a && b && Kaffeine_DosBrokerCall(a, &ctx, &cpu, 0, replace_selected_streams, &result, &error),
          "first broker selects a shared owned replacement without changing startup ownership");
    ctx.fixture = &state_b;
    check(Kaffeine_DosBrokerCall(b, &ctx, &cpu, 0, replace_selected_streams, &result, &error),
          "second broker independently replaces its selected streams");
    ctx.fixture = &state_a;
    check(Kaffeine_DosBrokerFinish(a, &ctx, finish_selected_streams),
          "first broker acknowledges selected-stream metadata cleanup");
    state_a.child->thread.join(); /* Finish ACK precedes native DosEntry's final stream close. */
    {
        std::lock_guard<std::mutex> lock(fixture_lock);
        for (BPTR stream : state_a.startup)
            check(!streams[stream], "first native exit closes the saved originals instead of current selections");
        for (BPTR stream : state_b.startup)
            check(streams[stream] == 1, "peer startup streams remain live after first broker exits");
        check(streams[state_a.replacement] == 1 && streams[state_b.replacement] == 1,
              "neither broker's owned replacement is consumed by its peer's native exit");
    }
    ctx.fixture = &state_b;
    check(Kaffeine_DosBrokerCall(b, &ctx, &cpu, 0, inspect_selected_streams, &result, &error),
          "second broker remains callable after its peer's native DOS cleanup");
    check(Kaffeine_DosBrokerFinish(b, &ctx, finish_selected_streams),
          "second broker finishes without consuming its current replacement");
    state_b.child->thread.join();
    {
        std::lock_guard<std::mutex> lock(fixture_lock);
        for (BPTR stream : state_b.startup)
            check(!streams[stream], "second native exit closes its own saved startup streams");
        retire_test_stream(state_a.replacement);
        retire_test_stream(state_b.replacement);
    }
    check(IoErr() == driver_error && !ctx.broker_owner && live_locks() == initial_locks,
          "interleaved selected-stream retirement preserves driver error and native owner lifetimes");
}

static void test_disarmed_startup_streams()
{
    for (bool shared : {false, true})
    {
        const unsigned initial_locks = live_locks();
        auto spec = spec_for(make_lock());
        next_shared_streams = shared;
        SIPTR error = -1;
        native_error = driver_error;
        auto broker = Kaffeine_DosBrokerStart(&root_process.pr_Task, &spec, &error);
        StreamExitState state;
        state.close_original = true;
        KaffeineContext ctx{};
        ctx.exec_task = &root_process.pr_Task; ctx.broker_call = TRUE; ctx.fixture = &state;
        Cpu cpu{};
        IPTR result = 0;
        check(broker && Kaffeine_DosBrokerCall(broker, &ctx, &cpu, 0, replace_selected_streams, &result, &error),
              "worker explicitly closes a displaced original and a fresh owned file reuses its BPTR");
        check(state.disarmed == (shared ? PRF_CLOSEINPUT | PRF_CLOSEOUTPUT | PRF_CLOSEERROR : PRF_CLOSEOUTPUT),
              "explicit close disarms every saved matching slot, including duplicate startup aliases");
        check(Kaffeine_DosBrokerFinish(broker, &ctx, finish_selected_streams),
              "Finish keeps live close flags disarmed after original native stream reuse");
        state.child->thread.join();
        {
            std::lock_guard<std::mutex> lock(fixture_lock);
            for (BPTR stream : state.startup)
                if (stream != state.reused)
                    check(!streams[stream], "native exit still closes unrelated armed startup slots");
            check(streams[state.reused] == 1 && streams[state.replacement] == 1,
                  "native exit cannot close a reused original identity or owned current selection");
            retire_test_stream(state.reused);
            retire_test_stream(state.replacement);
        }
        check(IoErr() == driver_error && live_locks() == initial_locks,
              "disarmed saved-stream cleanup preserves full-width driver error and balances directories");
    }
}

int main()
{
    root_process.pr_HomeDir = make_lock();
    root_process.pr_CurrentDir = make_lock();
    root_process.pr_CIS = (BPTR)0xf00000001ULL;
    test_positive();
    test_start_failures();
    test_timeout();
    test_nested();
    test_zero_home_and_no_cli();
    test_process_isolation();
    test_finish_cleanup();
    test_idle_probe();
    test_saved_startup_streams();
    test_disarmed_startup_streams();
    check(request_sends == replies, "every private RPC request has one reply before reclamation");
    check(callbacks == 150, "only explicitly selected compiled callbacks execute");
    check(allocations.empty() && semaphores.empty() && !dynamic_ports && inboxes.empty(),
          "all records, semaphores and native inboxes are retired");
    check(opens == closes, "all worker library pins are balanced");
    for (auto [stream, count] : streams) { (void)stream; check(!count, "no native NIL stream remains owned"); }
    UnLock(root_process.pr_HomeDir);
    UnLock(root_process.pr_CurrentDir);
    check(!live_locks(), "no original or duplicate native directory reference remains");
    std::printf("dos_broker checks=%u failures=%u callbacks=%u rpc=%u replies=%u\n",
                checks.load(), failures.load(), callbacks, request_sends, replies);
    return failures ? 1 : 0;
}
