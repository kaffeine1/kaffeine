/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "fixture_intern.h"
#include "kaffeine_process.h"
#include "kaffeine_domain_modules.h"
#include "kaffeine_dos_broker.h"
#include "kaffeine_callback.h"
#include "kaffeine_offsets.h"
#ifndef KAFFEINE_FIXTURE_BASELINE
#include "kaffeine_domain_process.h"
#endif
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <exec/memory.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

static unsigned checks, failures;
static void check(bool okay, const char *label)
{
    ++checks;
    if (!okay) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}

struct HunkRelocTable { unsigned fixture; };
struct KaffeineDomainModule
{
    bool live{true}, deferred{}, scheduled{};
};
struct KaffeineDosBroker
{
    Process native{};
    KaffeineDomainModule *module{};
    BPTR home{};
    bool live{true}, idle{true}, staged{}, scheduled{}, child_cpu_live{};
    unsigned refuse_finish{}, drops{};
};

static constexpr SIPTR caller_error = (SIPTR)0x1234567800000042ULL;
static constexpr SIPTR operation_error = (SIPTR)0x23456789000000cdULL;
static constexpr BPTR trusted_source = (BPTR)0x3456789000001000ULL;
static constexpr ULONG root_tcb = 0x100, child_tcb = 0x500;
static constexpr ULONG child_seglist = 0x800, child_entry = 0x2004;
static Process driver_process, parent_broker_process;
static Task *current_task = &driver_process.pr_Task;
static SIPTR native_error;
static HunkRelocTable relocation_table;
static KaffeineCPUFactory cpu_factory{};

struct Fixture
{
    KaffeineContext ctx{};
    KaffeineCPU root_cpu{}, child_cpu{};
    std::array<UBYTE,4096> memory{};
    KaffeineProcessStart spec{};
    std::map<APTR,ULONG> allocations;
    std::set<BPTR> locks;
    std::vector<std::unique_ptr<KaffeineDomainModule>> modules;
    std::vector<std::unique_ptr<KaffeineDosBroker>> brokers;
    std::vector<std::string> events;
    BPTR next_lock{(BPTR)0x4567890000000000ULL};
    unsigned alloc_calls{}, query_calls{}, load_calls{}, unload_calls{}, dup_calls{};
    unsigned unlock_calls{}, source_unloads{}, starts{}, finishes{}, idle_probes{};
    unsigned sched_calls{}, has_calls{}, recover_calls{}, stop_calls{}, delays{};
    unsigned file_drops{}, owner_drops{}, imported{}, installed{};
    ULONG stopped_d0{};
    bool fail_alloc{}, fail_info{}, missing_relocs{}, fail_load{}, fail_dup{};
    bool fail_broker{}, sched_success{}, stage_failure{}, fail_alloc_after_sched{};
    bool broker_initially_busy{}, module_deferred{};
    unsigned initial_finish_refusals{};

    Fixture();
    ~Fixture();
    KaffeineDosBroker *broker() { return brokers.empty() ? nullptr : brokers.back().get(); }
    bool source_intact() const { return !source_unloads && locks.count(spec.home_dir); }
    void parent_view() const
    {
        check(ctx.cpu == &root_cpu && ctx.guest_exec_task == root_tcb,
              "caller CPU and guest Process view remain intact");
    }
    void record_released() const
    {
        check(allocations.empty() && !ctx.domain_start_recovery && !ctx.domain_drain,
              "startup record and drain hook are retired exactly once");
    }
};
static Fixture *active;

Fixture::Fixture()
{
    check(!active, "fixtures execute serially");
    active = this;
    driver_process.pr_Task.tc_Node.ln_Type = NT_PROCESS;
    parent_broker_process.pr_Task.tc_Node.ln_Type = NT_PROCESS;
    current_task = &driver_process.pr_Task;
    ctx.cpu = &root_cpu;
    ctx.cpu_factory = &cpu_factory;
    ctx.exec_task = &driver_process.pr_Task;
    ctx.running = TRUE;
    ctx.mem = memory.data();
    ctx.mem_size = (ULONG)memory.size();
    ctx.guest_exec_task = root_tcb;
    spec.seglist = trusted_source;
    spec.stack_size = 8192;
    spec.name = "public-domain-child";
    spec.command = "domain-child";
    spec.arguments = "original public fixture\n";
    spec.home_dir = ++next_lock;
    locks.insert(spec.home_dir);
    spec.cli = TRUE;
    spec.backend = "moira";
    native_error = caller_error;
}

Fixture::~Fixture()
{
    check(allocations.empty(), "no native startup allocation leaks from fixture");
    for (auto [pointer, bytes] : allocations) { (void)bytes; std::free(pointer); }
    active = nullptr;
    current_task = &driver_process.pr_Task;
}

extern "C" SIPTR IoErr(void) { return native_error; }
extern "C" SIPTR SetIoErr(SIPTR value)
{
    SIPTR previous = native_error;
    native_error = value;
    return previous;
}
extern "C" Task *FindTask(CONST_STRPTR name)
{
    check(!name, "only current native owner is queried");
    return current_task;
}
extern "C" APTR AllocVec(ULONG bytes, ULONG flags)
{
    auto &f = *active;
    ++f.alloc_calls;
    native_error = 0x101;
    if (f.fail_alloc) return nullptr;
    void *pointer = std::malloc(bytes);
    check(pointer && bytes && (flags & MEMF_CLEAR), "recovery reservation is cleared native storage");
    if (pointer)
    {
        std::memset(pointer, flags & MEMF_CLEAR ? 0 : 0xd9, bytes);
        f.allocations[pointer] = bytes;
    }
    return pointer;
}
extern "C" void FreeVec(APTR pointer)
{
    auto &f = *active;
    check(f.allocations.erase(pointer) == 1, "native startup storage is released once");
    std::free(pointer);
    native_error = 0x102;
}
extern "C" void Delay(ULONG ticks)
{
    check(ticks == 1, "each pending drain yields one DOS tick");
    ++active->delays;
    native_error = 0x103;
}
extern "C" BOOL GetSegListInfo(BPTR source, const TagItem *query)
{
    auto &f = *active;
    ++f.query_calls;
    check(source == trusted_source && query[0].ti_Tag == GSLI_68KRelocs &&
          query[1].ti_Tag == TAG_DONE, "loader probe uses trusted full-width native identity");
    if (f.fail_info) return FALSE;
    *(IPTR *)query[0].ti_Data = f.missing_relocs ? 0 : (IPTR)&relocation_table;
    native_error = 0x104;
    return TRUE;
}
extern "C" BOOL Kaffeine_DomainLoad(KaffeineContext *, BPTR source,
    const HunkRelocTable *table, KaffeineDomainImage *out)
{
    auto &f = *active;
    ++f.load_calls;
    check(source == trusted_source && table == &relocation_table,
          "domain importer receives only trusted native loader storage");
#ifndef KAFFEINE_FIXTURE_BASELINE
    check(f.allocations.size() == 1, "recovery record exists before domain import");
#endif
    if (f.fail_load) { native_error = operation_error; return FALSE; }
    auto module = std::make_unique<KaffeineDomainModule>();
    module->deferred = f.module_deferred;
    *out = {child_entry, child_seglist, module.get()};
    f.modules.push_back(std::move(module));
    native_error = 0x105;
    return TRUE;
}
extern "C" BOOL Kaffeine_DomainUnload(KaffeineContext *, KaffeineDomainModule *module)
{
    auto &f = *active;
    ++f.unload_calls;
    check(module && module->live, "module ownership is released at most once");
    for (auto &broker : f.brokers)
        if (broker->module == module)
            check(!broker->live && !broker->child_cpu_live,
                  "module remains live until broker ACK and staged CPU destruction");
    f.events.emplace_back("unload");
    if (!module->deferred) module->live = false;
    native_error = 0x106;
    return !module->deferred;
}
extern "C" BPTR DupLock(BPTR original)
{
    auto &f = *active;
    ++f.dup_calls;
    check(f.locks.count(original), "only a live native home reference is duplicated");
    if (f.fail_dup) { native_error = operation_error; return BNULL; }
    BPTR result = ++f.next_lock;
    f.locks.insert(result);
    native_error = 0x107;
    return result;
}
extern "C" BOOL UnLock(BPTR lock)
{
    auto &f = *active;
    ++f.unlock_calls;
    check(f.locks.erase(lock) == 1, "each distinct native home reference is released once");
    native_error = 0x108;
    return TRUE;
}
extern "C" void UnLoadSeg(BPTR source)
{
    check(source == trusted_source && !active->source_unloads,
          "successful launch consumes original native seglist once");
    ++active->source_unloads;
    native_error = 0x109;
}
extern "C" KaffeineDosBroker *Kaffeine_DosBrokerStart(Task *driver,
    const KaffeineDosBrokerStart *spec, SIPTR *error)
{
    auto &f = *active;
    ++f.starts;
    check(driver == f.ctx.exec_task && spec->home_dir != f.spec.home_dir &&
          spec->stack_size == f.spec.stack_size && spec->arguments == f.spec.arguments,
          "broker gets a distinct home lease and the typed native launch spec");
    if (f.fail_broker) { *error = operation_error; return nullptr; }
    auto broker = std::make_unique<KaffeineDosBroker>();
    broker->native.pr_Task.tc_Node.ln_Type = NT_PROCESS;
    broker->native.pr_CES = 0x7300;
    broker->home = spec->home_dir ? DupLock(spec->home_dir) : BNULL;
    broker->idle = !f.broker_initially_busy;
    broker->refuse_finish = f.initial_finish_refusals;
    if (spec->home_dir) UnLock(spec->home_dir);
    auto *result = broker.get();
    f.brokers.push_back(std::move(broker));
    *error = 0;
    return result;
}
extern "C" BOOL Kaffeine_DosBrokerIsIdle(KaffeineDosBroker *broker)
{
    ++active->idle_probes;
    check(broker && broker->live, "idle metadata probe owns a live broker reference");
    return broker->idle;
}
extern "C" BOOL Kaffeine_DosBrokerFinish(KaffeineDosBroker *broker,
    KaffeineContext *ctx, KaffeineDosBrokerThunk cleanup)
{
    auto &f = *active;
    ++f.finishes;
    check(broker && broker->live && ctx == &f.ctx && ctx->broker_call && cleanup,
          "Finish uses a live broker and trusted owner cleanup capability");
    if (!broker->idle || broker->refuse_finish)
    {
        if (broker->refuse_finish) --broker->refuse_finish;
        return FALSE;
    }
    if (broker->staged)
        check(ctx->cpu == &f.child_cpu && ctx->guest_exec_task == child_tcb &&
              broker->child_cpu_live, "staged cleanup executes with its live child DOS view");
    auto *previous_task = current_task;
    auto *previous_owner = ctx->broker_owner;
    current_task = &broker->native.pr_Task;
    ctx->broker_owner = current_task;
    check(cleanup(ctx, nullptr) == 1, "owner cleanup job completes before ACK");
    ++broker->drops;
    ctx->broker_owner = previous_owner;
    current_task = previous_task;
    if (broker->home) UnLock(broker->home);
    broker->home = BNULL;
    broker->live = false;
    f.events.emplace_back("ack");
    native_error = 0x110;
    return TRUE;
}
extern "C" ULONG Kaffeine_SchedStartProcess(KaffeineContext *ctx, void *cpu,
    const KaffeineProcessStart *spec, ULONG seglist, ULONG entry,
    KaffeineDosBroker *broker, KaffeineDomainModule *module, SIPTR *error)
{
    auto &f = *active;
    ++f.sched_calls;
    check(ctx == &f.ctx && cpu == &f.root_cpu && spec == &f.spec &&
          seglist == child_seglist && entry == child_entry && module->live,
          "scheduler receives the imported classic image and live caller CPU");
    broker->module = module;
    if (f.fail_alloc_after_sched) f.fail_alloc = true;
    *error = f.sched_success ? 0 : operation_error;
    if (f.sched_success)
    {
        broker->scheduled = true;
        module->scheduled = true;
        return child_tcb;
    }
    broker->staged = f.stage_failure;
    broker->child_cpu_live = f.stage_failure;
    return 0;
}
extern "C" BOOL Kaffeine_SchedHasRecoveryStart(KaffeineContext *, KaffeineDosBroker *broker)
{
    ++active->has_calls;
    check(broker->idle, "scheduler recovery is not probed before broker idle");
    return broker->staged;
}
extern "C" BOOL Kaffeine_SchedRecoverStart(KaffeineContext *ctx, KaffeineDosBroker *broker)
{
    auto &f = *active;
    ++f.recover_calls;
    check(broker->staged && broker->child_cpu_live && broker->idle,
          "only an idle, fully retained staged startup is recovered");
    auto *old_cpu = ctx->cpu;
    auto old_tcb = ctx->guest_exec_task;
    auto old_call = ctx->broker_call;
    ctx->cpu = &f.child_cpu;
    ctx->guest_exec_task = child_tcb;
    ctx->broker_call = TRUE;
    BOOL okay = Kaffeine_DosBrokerFinish(broker, ctx, Kaffeine_ProcessDropGuestDos);
    ctx->cpu = old_cpu;
    ctx->guest_exec_task = old_tcb;
    ctx->broker_call = old_call;
    if (okay)
    {
        check(ctx->cpu == &f.root_cpu, "live caller view restored before child CPU destruction");
        broker->staged = false;
        broker->child_cpu_live = false;
        f.events.emplace_back("destroy");
    }
    return okay;
}
extern "C" void Kaffeine_StopGuest(KaffeineContext *ctx, void *cpu, LONG code)
{
    auto &f = *active;
    ++f.stop_calls;
    check(ctx == &f.ctx && cpu == &f.root_cpu && code == 20,
          "refused cleanup stops the live caller, never a freed staged CPU");
    ctx->running = FALSE;
    ctx->callback_failed = TRUE;
    ctx->exit_code = code;
    f.stopped_d0 = (ULONG)code;
}
extern "C" BPTR Input(void) { return 0x7100; }
extern "C" BPTR Output(void) { return 0x7200; }
extern "C" void Kaffeine_FileDropNative(KaffeineContext *ctx, BPTR native, BOOL borrowed_only)
{
    ++active->file_drops;
    check(ctx->broker_owner == current_task && current_task != ctx->exec_task &&
          !borrowed_only && native >= 0x7100 && native <= 0x7300,
          "native stream aliases retire on their broker owner before native close");
}
extern "C" void Kaffeine_LockOwnerDrop(KaffeineContext *ctx, Task *owner)
{
    ++active->owner_drops;
    check(owner == current_task && ctx->broker_owner == owner,
          "installed lock aliases retire on their native owner");
}
extern "C" ULONG Kaffeine_FileImport(KaffeineContext *, BPTR native, BOOL owned)
{
    ++active->imported;
    check(!owned, "startup borrows native DOS stream ownership");
    return (ULONG)native;
}
extern "C" BOOL Kaffeine_LockGetInstalled(KaffeineContext *,
    KaffeineLockInstalled slot, ULONG *token)
{
    ++active->installed;
    *token = 0x9000 + (ULONG)slot;
    return TRUE;
}

static void refused_cleanup_contract()
{
    Fixture f;
    f.initial_finish_refusals = 1;
    f.fail_alloc_after_sched = true;
    check(!Kaffeine_DomainStartProcess(&f.ctx, &f.spec), "scheduler refusal returns NULL Process");
    check(native_error == operation_error && f.source_intact(),
          "failure restores caller error and original home/seglist ownership");
    check(f.broker() && f.broker()->live && !f.unload_calls,
          "refused Finish retains broker and imported module");
    check(!f.ctx.running && f.stop_calls == 1 && f.stopped_d0 == 20,
          "refused Finish stops guest before another instruction can run");
#ifndef KAFFEINE_FIXTURE_BASELINE
    check(f.ctx.domain_start_recovery && f.ctx.domain_drain && f.allocations.size() == 1,
          "failure retains the pre-reserved recovery record without allocating");
    check(f.alloc_calls == 1, "failing cleanup needs no new native allocation");
    native_error = caller_error;
    check(f.ctx.domain_drain(&f.ctx), "owner driver retries and acknowledges cleanup");
    check(native_error == caller_error && !f.delays && !f.broker()->live &&
          f.unload_calls == 1, "ACK releases module once and preserves full-width IoErr");
    f.record_released();
#endif
    f.parent_view();
}

#ifndef KAFFEINE_FIXTURE_BASELINE
static void prepublication_failures()
{
    for (unsigned mode = 0; mode < 6; ++mode)
    {
        Fixture f;
        if (mode == 0) f.fail_alloc = true;
        if (mode == 1) f.fail_info = true;
        if (mode == 2) f.missing_relocs = true;
        if (mode == 3) f.fail_load = true;
        if (mode == 4) f.fail_dup = true;
        if (mode == 5) f.fail_broker = true;
        check(!Kaffeine_DomainStartProcess(&f.ctx, &f.spec), "prepublication failure returns no Process");
        check(f.source_intact() && f.ctx.running && !f.stop_calls,
              "ordinary failure leaves original leases and guest execution intact");
        SIPTR expected = mode == 0 ? ERROR_NO_FREE_STORE :
            mode < 3 ? ERROR_INVALID_RESIDENT_LIBRARY : operation_error;
        check(native_error == expected, "ordinary failure preserves the original native failure");
        check(!f.sched_calls && !f.finishes, "no absent native owner receives Finish");
        check(f.locks.size() == 1, "extra home reference is balanced on every prepublication failure");
        check(f.unload_calls == (mode >= 4 ? 1u : 0u), "only imported modules need rollback");
        if (mode == 0)
            check(!f.query_calls && !f.load_calls && !f.starts,
                  "reservation OOM happens before loader or broker work");
        f.parent_view();
        f.record_released();
    }
    Fixture f;
    f.ctx.callback_depth = 1;
    check(!Kaffeine_DomainStartProcess(&f.ctx, &f.spec) && !f.alloc_calls &&
          native_error == ERROR_BAD_NUMBER, "callback start is refused before acquiring resources");
    f.ctx.callback_depth = 0;
    check(!Kaffeine_DomainStartProcess(&f.ctx, nullptr) && !f.alloc_calls,
          "NULL spec acquires no resources");
}

static void successful_publication()
{
    Fixture f;
    f.sched_success = true;
    check(Kaffeine_DomainStartProcess(&f.ctx, &f.spec) == child_tcb,
          "successful launch returns the scheduler Process identity");
    check(native_error == caller_error && f.source_unloads == 1 &&
          !f.locks.count(f.spec.home_dir), "success alone consumes source seglist and original home");
    check(f.broker()->scheduled && f.broker()->live && f.modules.back()->scheduled &&
          !f.finishes && !f.unload_calls && !f.stop_calls,
          "published scheduler owns broker/module without startup rollback");
    check(f.locks.size() == 1 && f.ctx.running, "only native DOS's distinct home reference survives");
    f.parent_view();
    f.record_released();
}

static void immediate_cleanup(bool staged, bool nested)
{
    Fixture f;
    f.stage_failure = staged;
    if (nested)
    {
        current_task = &parent_broker_process.pr_Task;
        f.ctx.broker_call = TRUE;
        f.ctx.broker_owner = current_task;
    }
    auto *owner = f.ctx.broker_owner;
    check(!Kaffeine_DomainStartProcess(&f.ctx, &f.spec), "unpublished scheduler failure returns NULL");
    check(f.source_intact() && native_error == operation_error && f.ctx.running,
          "acknowledged rollback restores caller-owned leases and permits normal error handling");
    check(f.finishes == 1 && !f.broker()->live && f.unload_calls == 1 &&
          f.file_drops == 0 && f.owner_drops == 1 && !f.ctx.process_streams &&
          !f.ctx.process_file_close, "rollback without Init retires no uncaptured streams before owner ACK");
    check(!f.broker()->child_cpu_live && f.recover_calls == (staged ? 1u : 0u),
          "staged startup uses scoped scheduler recovery");
    check(f.ctx.broker_call == (nested ? TRUE : FALSE) && f.ctx.broker_owner == owner,
          "nested parent broker capability is restored after child cleanup");
    check(f.events.front() == "ack" && f.events.back() == "unload",
          "module release follows native cleanup ACK");
    f.parent_view();
    f.record_released();
}

static void deferred_cleanup(bool staged, bool nested, bool deferred_module)
{
    Fixture f;
    f.stage_failure = staged;
    f.module_deferred = deferred_module;
    f.broker_initially_busy = true;
    f.fail_alloc_after_sched = true;
    if (nested)
    {
        current_task = &parent_broker_process.pr_Task;
        f.ctx.broker_call = TRUE;
        f.ctx.broker_owner = current_task;
    }
    check(!Kaffeine_DomainStartProcess(&f.ctx, &f.spec), "busy startup is deferred and returns no Process");
    check(native_error == operation_error && f.ctx.domain_start_recovery &&
          f.ctx.domain_drain && !f.ctx.running, "busy owner retains recovery and stops guest execution");
    check(!f.has_calls && !f.recover_calls && !f.finishes && !f.unload_calls,
          "busy metadata probe cannot change DOS scope or free resources");
    f.parent_view();
    auto *record = f.ctx.domain_start_recovery;
    auto *broker = f.broker();
    unsigned allocs = f.alloc_calls;
    check(!Kaffeine_DomainStartProcess(&f.ctx, &f.spec) && native_error == ERROR_BREAK &&
          f.alloc_calls == allocs && f.ctx.domain_start_recovery == record,
          "a pending recovery cannot allocate or launch a second child");

    auto rejected = [&] (const char *label)
    {
        unsigned before = f.delays, probes = f.idle_probes;
        native_error = caller_error;
        check(!Kaffeine_DomainDrainStarts(&f.ctx), label);
        check(native_error == caller_error && f.delays == before + 1 &&
              f.idle_probes == probes && f.ctx.domain_start_recovery == record,
              "invalid drain yields without changing context, records or full-width IoErr");
    };
    current_task = &parent_broker_process.pr_Task;
    rejected("foreign native owner cannot drain startup resources");
    current_task = &driver_process.pr_Task;
    f.ctx.broker_call = FALSE;
    f.ctx.broker_owner = nullptr;
    f.ctx.running = TRUE;
    rejected("running guest cannot drain startup resources");
    f.ctx.running = FALSE;
    f.ctx.callback_depth = 1;
    rejected("active guest callback cannot drain startup resources");
    f.ctx.callback_depth = 0;
    f.ctx.broker_call = TRUE;
    rejected("nested broker callback cannot enter driver drain");
    f.ctx.broker_call = FALSE;
    f.ctx.broker_owner = &parent_broker_process.pr_Task;
    rejected("outstanding native owner cannot enter driver drain");
    f.ctx.broker_owner = nullptr;

    native_error = caller_error;
    unsigned before = f.delays;
    check(!Kaffeine_DomainDrainStarts(&f.ctx), "busy owner returns pending without a cleanup submission");
    check(f.delays == before + 1 && native_error == caller_error &&
          !f.has_calls && !f.finishes && !f.unload_calls && broker->live,
          "busy driver drain yields once and retains all native ownership");
    if (staged) check(broker->child_cpu_live, "staged CPU remains alive across busy drain");
    f.parent_view();

    broker->idle = true;
    broker->refuse_finish = 1;
    before = f.delays;
    check(!Kaffeine_DomainDrainStarts(&f.ctx), "idle Finish refusal still retains recovery");
    check(f.delays == before + 1 && broker->live && !f.unload_calls &&
          f.ctx.domain_start_recovery == record && native_error == caller_error,
          "transport refusal never substitutes for owner ACK");
    if (staged) check(broker->child_cpu_live, "refused scoped cleanup retains staged CPU/TCB");
    f.parent_view();

    before = f.delays;
    check(Kaffeine_DomainDrainStarts(&f.ctx), "owner ACK drains retained startup resources");
    check(f.delays == before && native_error == caller_error && !broker->live &&
          !broker->child_cpu_live && f.unload_calls == 1 && f.alloc_calls == allocs,
          "successful retry releases once, without allocation or an extra delay");
    check(f.modules.back()->live == deferred_module,
          "deferred module remains owned by the Context module registry");
    check(f.events.front() == "ack" && f.events.back() == "unload" &&
          (staged ? f.events[1] == "destroy" : true),
          "ACK precedes CPU destruction, which precedes module unload");
    f.parent_view();
    f.record_released();
    check(Kaffeine_DomainDrainStarts(&f.ctx) && native_error == caller_error &&
          f.unload_calls == 1 && f.delays == before, "completed drain is idempotent");
}

static void init_guest_dos_contract()
{
    Fixture f;
    Process owner{};
    owner.pr_Task.tc_Node.ln_Type = NT_PROCESS;
    owner.pr_CES = 0x7300;
    owner.pr_Flags = PRF_CLOSEINPUT | PRF_CLOSEOUTPUT | PRF_CLOSEERROR;
    current_task = &owner.pr_Task;
    f.ctx.broker_owner = current_task;
    f.ctx.broker_call = TRUE;
    check(Kaffeine_ProcessInitGuestDos(&f.ctx, &f.root_cpu) == 1 &&
          f.imported == 3 && f.installed == 2,
          "complete production startup job imports borrowed streams and installed locks");
    f.ctx.guest_exec_task = f.ctx.mem_size - M68K_SIZEOF_PROCESS + 1;
    unsigned imports = f.imported;
    check(!Kaffeine_ProcessInitGuestDos(&f.ctx, &f.root_cpu) && f.imported == imports,
          "invalid guest Process extent is rejected before native imports");
    check(Kaffeine_ProcessDropGuestDos(&f.ctx, nullptr) == 1 && f.file_drops == 3 &&
          !f.ctx.process_streams && !f.ctx.process_file_close,
          "startup stream capture retires before the native owner's exit");
}
#endif

int main()
{
    refused_cleanup_contract();
#ifndef KAFFEINE_FIXTURE_BASELINE
    prepublication_failures();
    successful_publication();
    for (bool staged : {false, true})
        for (bool nested : {false, true})
        {
            immediate_cleanup(staged, nested);
            deferred_cleanup(staged, nested, false);
        }
    deferred_cleanup(true, false, true);
    init_guest_dos_contract();
#endif
    std::printf("domain startup recovery: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
