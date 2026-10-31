/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#include "fixture_intern.h"
#include "kaffeine_domain_run.h"
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <exec/memory.h>
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
    std::string command;
    bool cli{}, close_input{true}, close_output{true}, close_error{true};
    bool deferred{}, release_start{}, fail_command{}, finished{};
    LONG result{};
};
struct State
{
    enum Mode { FAST, CHILD, REPEAT } mode{FAST};
    bool run_entered{}, parent_returned{}, release_children{}, disposed{};
    unsigned run_calls{}, dispose_calls{}, drop_calls{}, effective_drops{}, live_children{};
    bool borrowed_live[3]{true,true,true};
    BPTR native_streams[3]{};
    Task *owner{};
};
static std::mutex fixture_lock;
static std::condition_variable changed;
static std::vector<std::unique_ptr<Child>> children;
static std::map<APTR, ULONG> allocations;
static std::set<SignalSemaphore *> semaphores;
static std::map<BPTR, unsigned> locks, streams;
static BPTR next_native = (BPTR)0x100000000ULL;
static Process parent_process;
static Library native_base;
static thread_local Task *current_task = &parent_process.pr_Task;
static thread_local Child *current_child;
static thread_local SIPTR native_error;
static thread_local unsigned semaphore_depth;
static unsigned opens, closes, cnp_calls, contexts_created, contexts_disposed;
static std::atomic<unsigned> ioerr_reads;
static bool fail_alloc, fail_open, fail_dup, fail_cnp, next_defer, next_fail_command, next_early_exit;
static constexpr SIPTR caller_error = (SIPTR)0x12345678000000abULL;
static constexpr SIPTR injected_error = (SIPTR)0x23456789000000cdULL;

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
extern "C" void InitSemaphore(SignalSemaphore *s)
{
    pthread_mutex_init(&s->mutex, nullptr);
    std::lock_guard<std::mutex> lock(fixture_lock);
    semaphores.insert(s);
}
extern "C" void ObtainSemaphore(SignalSemaphore *s) { pthread_mutex_lock(&s->mutex); ++semaphore_depth; }
extern "C" void ReleaseSemaphore(SignalSemaphore *s) { --semaphore_depth; pthread_mutex_unlock(&s->mutex); }
extern "C" APTR AllocVec(ULONG size, ULONG flags)
{
    check(!semaphore_depth, "allocation holds no driver semaphore");
    if (fail_alloc) return nullptr;
    APTR p = std::malloc(size);
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
    check(!semaphore_depth, "driver record is freed outside its semaphore");
    std::lock_guard<std::mutex> lock(fixture_lock);
    check(allocations.erase(p) == 1, "driver record is freed exactly once");
    auto sem = static_cast<SignalSemaphore *>(p);
    if (semaphores.erase(sem)) check(!pthread_mutex_destroy(&sem->mutex), "record has no live semaphore waiter");
    std::free(p);
    native_error = 0x101;
}
extern "C" Library *OpenLibrary(CONST_STRPTR name, ULONG version)
{
    check(!semaphore_depth && !std::strcmp(name,"kaffeine.library") && !version,
          "pins only controlled resident library without holding driver semaphore");
    if (fail_open) return nullptr;
    std::lock_guard<std::mutex> lock(fixture_lock);
    ++opens;
    return &native_base;
}
extern "C" void CloseLibrary(Library *base)
{
    check(!semaphore_depth, "library unpin holds no driver semaphore");
    std::lock_guard<std::mutex> lock(fixture_lock);
    check(base == &native_base && opens > closes, "private library pin is balanced exactly once");
    ++closes;
    native_error = 0x102;
}
extern "C" Task *FindTask(CONST_STRPTR name)
{
    check(!name, "never finds a driver by a reusable native name");
    return current_task;
}
extern "C" SIPTR IoErr(void)
{
    ++ioerr_reads;
    if (!current_task || current_task->tc_Node.ln_Type != NT_PROCESS)
        check(false, "native IoErr must never inspect a non-Process Exec Task");
    return native_error;
}
extern "C" SIPTR SetIoErr(SIPTR value) { SIPTR old = native_error; native_error = value; return old; }
extern "C" BPTR DupLock(BPTR lock)
{
    check(!semaphore_depth, "DupLock holds no driver semaphore");
    if (fail_dup) { native_error = injected_error; return 0; }
    std::lock_guard<std::mutex> guard(fixture_lock);
    check(locks[lock] == 1 && lock > UINT32_MAX, "duplicates complete live native directory reference");
    return make_lock_locked();
}
extern "C" BOOL UnLock(BPTR lock)
{
    check(!semaphore_depth, "UnLock holds no driver semaphore");
    std::lock_guard<std::mutex> guard(fixture_lock);
    check(locks[lock] == 1, "native directory reference is released exactly once");
    if (locks[lock]) --locks[lock];
    native_error = 0x103;
    return TRUE;
}
static Process *process() { return current_child ? &current_child->process : &parent_process; }
extern "C" BPTR CurrentDir(BPTR lock)
{
    check(!semaphore_depth, "CurrentDir holds no driver semaphore");
    BPTR old = process()->pr_CurrentDir;
    process()->pr_CurrentDir = lock;
    native_error = 0x104;
    return old;
}
extern "C" BPTR Input(void) { return process()->pr_CIS; }
extern "C" BPTR Output(void) { return process()->pr_COS; }
extern "C" BOOL SetProgramName(CONST_STRPTR command)
{
    check(!semaphore_depth && current_child && current_child->cli,
          "command is installed on private driver CLI outside its semaphore");
    if (current_child->fail_command) { native_error = injected_error; return FALSE; }
    current_child->command = command;
    return TRUE;
}
extern "C" void Delay(ULONG ticks)
{
    if (semaphore_depth || ticks != 1) check(false, "polling uses single ticks outside driver semaphore");
    native_error = 0x105;
    std::this_thread::sleep_for(std::chrono::microseconds(150));
}

extern "C" Process *CreateNewProc(const TagItem *tags)
{
    check(!semaphore_depth, "native CNP holds no driver semaphore");
    ++cnp_calls;
    if (fail_cnp) { native_error = injected_error; return nullptr; }
    auto child = std::make_unique<Child>();
    LONG (*entry)(void) = nullptr;
    bool current_given = false, saw_input = false, saw_output = false, saw_error = false;
    for (const TagItem *tag = tags; tag->ti_Tag != TAG_DONE; ++tag)
    {
        switch (tag->ti_Tag)
        {
            case TAG_IGNORE: break;
            case NP_Entry: entry = reinterpret_cast<LONG (*)(void)>(tag->ti_Data); break;
            case NP_UserData: child->process.pr_Task.tc_UserData = reinterpret_cast<APTR>(tag->ti_Data); break;
            case NP_Name: check(!std::strcmp(reinterpret_cast<const char *>(tag->ti_Data),"kaffeine"), "native driver has controlled name"); break;
            case NP_StackSize: check(tag->ti_Data >= 128*1024, "ARM driver has its own adequate native stack"); break;
            case NP_Synchronous: check(!tag->ti_Data, "native CNP is asynchronous for surviving guest Processes"); break;
            case NP_FreeSeglist: check(tag->ti_Data, "native DOS owns artificial ARM entry segment"); break;
            case NP_Cli: child->cli = tag->ti_Data != 0; break;
            case NP_CopyVars: check(tag->ti_Data, "root driver receives private copies of caller variables/backend"); break;
            case NP_Input: saw_input = true; child->process.pr_CIS = static_cast<BPTR>(tag->ti_Data); break;
            case NP_Output: saw_output = true; child->process.pr_COS = static_cast<BPTR>(tag->ti_Data); break;
            case NP_Error: saw_error = true; child->process.pr_CES = static_cast<BPTR>(tag->ti_Data); break;
            case NP_CloseInput: child->close_input = tag->ti_Data != 0; break;
            case NP_CloseOutput: child->close_output = tag->ti_Data != 0; break;
            case NP_CloseError: child->close_error = tag->ti_Data != 0; break;
            case NP_CurrentDir: current_given = true; child->process.pr_CurrentDir = static_cast<BPTR>(tag->ti_Data); break;
            case NP_Arguments: check(false, "CTX already owns arguments: native CNP must not inject them into borrowed input"); break;
            default: check(false, "no guest seglist or untrusted native tag is forwarded"); break;
        }
    }
    check(entry && child->process.pr_Task.tc_UserData, "CNP has compiled entry and private gate userdata");
    check(saw_input && saw_output && saw_error && !child->close_input && !child->close_output && !child->close_error,
          "all three full-width caller streams are explicitly borrowed with CloseFALSE");
    check(child->process.pr_CIS == Input() && child->process.pr_COS == Output() &&
          child->process.pr_CES == process()->pr_CES,
          "native driver gets exact complete caller stream values");
    child->process.pr_CLI = child->cli ? (BPTR)0x900000001ULL : 0;
    child->process.pr_Task.tc_Node.ln_Type = NT_PROCESS;
    {
        std::lock_guard<std::mutex> lock(fixture_lock);
        if (!current_given && process()->pr_CurrentDir) child->process.pr_CurrentDir = make_lock_locked();
        if (process()->pr_HomeDir) child->process.pr_HomeDir = make_lock_locked();
    }
    child->deferred = next_defer;
    child->fail_command = next_fail_command;
    bool early_exit = next_early_exit;
    next_defer = next_fail_command = next_early_exit = false;
    Child *raw = child.get();
    {
        std::lock_guard<std::mutex> lock(fixture_lock);
        children.push_back(std::move(child));
    }
    raw->thread = std::thread([raw, entry] {
        current_child = raw;
        current_task = &raw->process.pr_Task;
        native_error = 0;
        {
            std::unique_lock<std::mutex> lock(fixture_lock);
            if (raw->deferred) changed.wait(lock, [raw] { return raw->release_start; });
        }
        raw->result = entry();
        check(!raw->process.pr_Task.tc_UserData, "private gate userdata cleared before native DOS exit");
        if (raw->process.pr_CurrentDir) UnLock(raw->process.pr_CurrentDir);
        if (raw->process.pr_HomeDir) UnLock(raw->process.pr_HomeDir);
        {
            std::lock_guard<std::mutex> lock(fixture_lock);
            BPTR handles[] = {raw->process.pr_CIS,raw->process.pr_COS,raw->process.pr_CES};
            bool close_flags[] = {raw->close_input,raw->close_output,raw->close_error};
            for (unsigned i=0;i<3;++i)
                if (close_flags[i] && handles[i])
                {
                    check(streams[handles[i]]==1,"native DOS cannot close caller stream twice");
                    --streams[handles[i]];
                }
            raw->finished = true;
            changed.notify_all();
        }
    });
    if (early_exit)
    {
        std::unique_lock<std::mutex> lock(fixture_lock);
        changed.wait(lock,[raw]{return raw->finished;});
    }
    return reinterpret_cast<Process *>(1); /* intentionally never readable */
}

extern "C" void Kaffeine_FileDropBorrowed(KaffeineContext *ctx, ULONG token)
{
    auto state = static_cast<State *>(ctx->fixture);
    check(current_child && current_task == state->owner && !semaphore_depth,
          "borrowed guest stream association is retired on native driver owner before gate lock");
    std::lock_guard<std::mutex> lock(fixture_lock);
    ++state->drop_calls;
    for (unsigned i=0;i<3;++i)
        if (ctx->root_streams[i] == token && state->borrowed_live[i])
        {
            state->borrowed_live[i] = false;
            ++state->effective_drops;
            if (current_child->process.pr_CIS == state->native_streams[i]) current_child->process.pr_CIS = 0;
            if (current_child->process.pr_COS == state->native_streams[i]) current_child->process.pr_COS = 0;
            if (current_child->process.pr_CES == state->native_streams[i]) current_child->process.pr_CES = 0;
            check(streams[state->native_streams[i]] == 1 && !state->parent_returned,
                  "effective retirement precedes parent return/close and never closes caller native handle");
        }
    native_error = 0x106;
}

static void open_parent_streams(State &state)
{
    std::lock_guard<std::mutex> lock(fixture_lock);
    BPTR *fields[] = {&parent_process.pr_CIS,&parent_process.pr_COS,&parent_process.pr_CES};
    for (unsigned i=0;i<3;++i)
    {
        state.native_streams[i] = ++next_native;
        streams[state.native_streams[i]] = 1;
        *fields[i] = state.native_streams[i];
    }
}
static void close_parent_streams(State &state, bool early_return)
{
    std::lock_guard<std::mutex> lock(fixture_lock);
    if (early_return) check(state.effective_drops == 3, "all borrowed identities retire before RunDomain returns TRUE");
    state.parent_returned = true;
    for (BPTR stream : state.native_streams)
    {
        check(streams[stream] == 1, "parent still owns its original native stream on return");
        --streams[stream];
    }
    parent_process.pr_CIS = parent_process.pr_COS = parent_process.pr_CES = 0;
}
static KaffeineContext *make_context(State &state)
{
    auto ctx = new KaffeineContext{};
    std::strcpy(ctx->program_name,"Original68kProgram");
    std::strcpy(ctx->arguments,"already copied arguments\n");
    ctx->root_streams[0]=0x4001;ctx->root_streams[1]=0x4002;ctx->root_streams[2]=0x4003;
    ctx->fixture = &state;
    ++contexts_created;
    return ctx;
}
static LONG run_domain(KaffeineContext *ctx)
{
    auto state = static_cast<State *>(ctx->fixture);
    {
        std::lock_guard<std::mutex> lock(fixture_lock);
        state->run_entered = true;
        ++state->run_calls;
        state->owner = current_task;
    }
    check(current_child && current_task != &parent_process.pr_Task,
          "compiled run callback executes only in native driver");
    check(!semaphore_depth && ctx->root_complete && ctx->root_completion_data,
          "run sees committed ownership and complete private gate outside semaphore");
    check(!std::strcmp(ctx->arguments,"already copied arguments\n") && !current_child->process.pr_Arguments,
          "guest arguments stay in CTX without native borrowed-input injection");
    check(!current_child->cli || current_child->command == "Original68kProgram",
          "native driver command is copied before guest execution");
    check(current_child->process.pr_CurrentDir != parent_process.pr_CurrentDir &&
          current_child->process.pr_CurrentDir > UINT32_MAX,
          "driver has a distinct complete-width native current-directory reference");
    if (state->mode == State::FAST) return 11;
    state->live_children = 1;
    ctx->root_complete(ctx,7);
    if (state->mode == State::REPEAT) ctx->root_complete(ctx,99);
    {
        std::unique_lock<std::mutex> lock(fixture_lock);
        changed.notify_all();
        changed.wait(lock,[state]{return state->release_children;});
        check(state->parent_returned && !state->disposed && state->live_children == 1,
              "child continues in live domain after native parent returns");
        check(!current_child->process.pr_CIS && !current_child->process.pr_COS && !current_child->process.pr_CES,
              "surviving driver retains no caller stream after parent is allowed to close it");
        check(!std::strcmp(ctx->arguments,"already copied arguments\n"),
              "owned context arguments remain live after original caller return");
        state->live_children = 0;
    }
    return 9; /* final child/domain result must not overwrite first root result */
}
static void dispose_domain(KaffeineContext *ctx)
{
    auto state = static_cast<State *>(ctx->fixture);
    check(current_child && current_task == state->owner && !semaphore_depth,
          "only the executing native driver disposes the transferred context");
    check(!ctx->root_complete && !ctx->root_completion_data,
          "completion capability is cleared before context disposal");
    {
        std::lock_guard<std::mutex> lock(fixture_lock);
        check(!state->disposed && !state->live_children && state->effective_drops == 3,
              "context disposes exactly once after last child and borrowed retirement");
        ++state->dispose_calls;
        state->disposed = true;
        changed.notify_all();
    }
    ++contexts_disposed;
    delete ctx;
}
static void destroy_untransferred(KaffeineContext *ctx)
{
    ++contexts_disposed;
    delete ctx;
}
static void join_all(bool release = false)
{
    if (release)
    {
        std::lock_guard<std::mutex> lock(fixture_lock);
        for (auto &child:children) child->release_start=true;
        changed.notify_all();
    }
    for (auto &child:children) if(child->thread.joinable()) child->thread.join();
}

static void test_early_return(State::Mode mode)
{
    State state;state.mode=mode;
    open_parent_streams(state);
    KaffeineContext *ctx=make_context(state);
    LONG result=-99;
    native_error=caller_error;
    const unsigned locks_before=live_locks();
    BPTR cwd=parent_process.pr_CurrentDir;
    check(Kaffeine_RunDomain(ctx,run_domain,dispose_domain,&result) && result==7 && IoErr()==caller_error,
          "RunDomain returns first root result before independent child completes and preserves caller IoErr");
    /* TRUE transferred ownership: no ctx access is permitted below. */
    {
        std::lock_guard<std::mutex> lock(fixture_lock);
        check(!state.disposed && state.run_calls==1 && state.live_children==1,
              "native parent can return while worker alone owns live context and child");
        check(opens==closes+1 && !allocations.empty(), "worker keeps its private gate and library pin after early return");
    }
    check(parent_process.pr_CurrentDir==cwd, "parent current directory is restored before native child startup");
    close_parent_streams(state,true);
    {
        std::lock_guard<std::mutex> lock(fixture_lock);
        state.release_children=true;
        changed.notify_all();
    }
    join_all();
    check(state.disposed && state.dispose_calls==1 && state.effective_drops==3,
          "last child ending triggers exactly one native-owner disposal and three effective borrowed releases");
    check(allocations.empty() && opens==closes && live_locks()==locks_before,
          "early-return worker balances gate, library and native-owned directory duplicates");
}
static void test_fast(bool cli)
{
    State state;
    open_parent_streams(state);
    auto ctx=make_context(state);
    parent_process.pr_CLI=cli?(BPTR)0x900000002ULL:0;
    native_error=caller_error;LONG result=-99;
    check(Kaffeine_RunDomain(ctx,run_domain,dispose_domain,&result) && result==11 && IoErr()==caller_error,
          "fallback completion for immediate run return preserves result and caller IoErr");
    close_parent_streams(state,true);
    join_all();
    check(state.dispose_calls==1 && state.run_calls==1 && state.effective_drops==3 && allocations.empty(),
          "fast run (with or without CLI) transfers/disposes exactly once without inspecting CNP sentinel");
    parent_process.pr_CLI=(BPTR)0x900000002ULL;
}
static void test_failures()
{
    enum Failure { ALLOC, OPEN, DUP, CNP, COMMAND };
    for(Failure which:{ALLOC,OPEN,DUP,CNP,COMMAND})
    {
        State state;
        open_parent_streams(state);
        auto ctx=make_context(state);
        LONG result=-99;
        unsigned locks_before=live_locks();
        fail_alloc=which==ALLOC;fail_open=which==OPEN;fail_dup=which==DUP;fail_cnp=which==CNP;
        next_fail_command=next_early_exit=which==COMMAND;
        native_error=caller_error;
        bool okay=Kaffeine_RunDomain(ctx,run_domain,dispose_domain,&result);
        SIPTR expected=which==ALLOC?ERROR_NO_FREE_STORE:which==OPEN?ERROR_INVALID_RESIDENT_LIBRARY:injected_error;
        check(!okay && IoErr()==expected && result==-99, "precommit failure preserves exact numeric error and result output");
        check(!ctx->root_complete && !ctx->root_completion_data && !state.run_calls && !state.dispose_calls,
              "every failed startup leaves context/callback ownership with caller");
        fail_alloc=fail_open=fail_dup=fail_cnp=next_fail_command=next_early_exit=false;
        close_parent_streams(state,false);
        destroy_untransferred(ctx);
        join_all();
        check(live_locks()==locks_before && opens==closes && allocations.empty(),
              "all precommit failure resources balance even when worker exits before CNP returns 1");
    }
}
static void test_timeout()
{
    State state;
    open_parent_streams(state);
    auto ctx=make_context(state);
    unsigned locks_before=live_locks();
    next_defer=true;native_error=caller_error;LONG result=-99;
    check(!Kaffeine_RunDomain(ctx,run_domain,dispose_domain,&result) && IoErr()==ERROR_BREAK && result==-99,
          "bounded startup timeout does not commit or replace result");
    check(!ctx->root_complete && !ctx->root_completion_data && !state.run_calls && !state.dispose_calls,
          "late worker has never acquired context ownership");
    check(opens==closes+1 && !allocations.empty(), "late worker's private userdata and library pin remain live");
    close_parent_streams(state,false);
    destroy_untransferred(ctx); /* ASan catches any later native use of ctx. */
    join_all(true);
    check(!state.run_calls && !state.dispose_calls && allocations.empty() && opens==closes && live_locks()==locks_before,
          "late entry observes abort without touching already-freed caller context");
}
static void test_preflight()
{
    State state;
    auto ctx=make_context(state);
    LONG result=-99;
    const unsigned cnp_before=cnp_calls;
    check(!Kaffeine_RunDomain(nullptr,run_domain,dispose_domain,&result) && IoErr()==ERROR_BAD_NUMBER,
          "NULL context is refused before allocation or worker publication");
    check(!Kaffeine_RunDomain(ctx,nullptr,dispose_domain,&result) && IoErr()==ERROR_BAD_NUMBER,
          "missing compiled run callback is refused");
    check(!Kaffeine_RunDomain(ctx,run_domain,nullptr,&result) && IoErr()==ERROR_BAD_NUMBER,
          "missing disposer is refused without context transfer");
    check(!Kaffeine_RunDomain(ctx,run_domain,dispose_domain,nullptr) && IoErr()==ERROR_BAD_NUMBER,
          "missing caller result pointer is refused");
    parent_process.pr_Task.tc_Node.ln_Type=1;
    const unsigned before_reads=ioerr_reads;
    check(!Kaffeine_RunDomain(ctx,run_domain,dispose_domain,&result),
          "native Exec Task without DOS Process is refused");
    check(ioerr_reads==before_reads,"non-Process refusal happens before any native IoErr query");
    parent_process.pr_Task.tc_Node.ln_Type=NT_PROCESS;
    check(cnp_calls==cnp_before && !ctx->root_complete && result==-99,
          "all preflight refusals leave context, result and process count unchanged");
    destroy_untransferred(ctx);
}
int main()
{
    parent_process.pr_Task.tc_Node.ln_Type=NT_PROCESS;
    parent_process.pr_CLI=(BPTR)0x900000002ULL;
    parent_process.pr_CurrentDir=make_lock();
    parent_process.pr_HomeDir=make_lock();
    test_early_return(State::CHILD);
    test_early_return(State::REPEAT);
    test_fast(true);test_fast(false);
    test_failures();test_timeout();test_preflight();
    check(contexts_created==contexts_disposed && allocations.empty() && semaphores.empty() && opens==closes,
          "every context/record/library is disposed once by its actual owner");
    for(auto [stream,count]:streams){(void)stream;check(!count,"all borrowed stream ownership remains solely with parent");}
    UnLock(parent_process.pr_CurrentDir);UnLock(parent_process.pr_HomeDir);
    check(!live_locks(),"no original or duplicated native directory remains");
    std::printf("domain_run checks=%u failures=%u contexts=%u disposed=%u cnp=%u\n",
                checks.load(),failures.load(),contexts_created,contexts_disposed,cnp_calls);
    return failures?1:0;
}
