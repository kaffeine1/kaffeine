/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Production scheduler, packet adapters, queues and heap. Only backend CPUs
   and synchronous native Process transport are recording collaborators. */
#pragma once
#include "kaffeine_callback.h"
#include "kaffeine_dos_broker.h"
#include "kaffeine_domain_modules.h"
#include "kaffeine_layers.h"
#include "kaffeine_process.h"
#include "kaffeine_sched.h"

struct KaffeineDosBroker
{
    Process process = {};
    unsigned calls = 0, helpers = 0, finishes = 0, inits = 0, drops = 0;
    SIPTR error = 733;
    bool fail = false, wrong_owner = false;
};
struct KaffeineDomainModule { unsigned unloads = 0; };
static KaffeineDosBroker *packet_active_broker, *packet_expected_broker;
static KaffeineContext *packet_observe_ctx;
static unsigned packet_observations, packet_cpus_created, packet_cpus_destroyed;
static bool packet_try_guest_callback;

extern "C" BYTE SetTaskPri(Task *t, LONG pri)
{ assert(t == &native_task); BYTE old=t->tc_Node.ln_Pri;t->tc_Node.ln_Pri=pri;return old; }
extern "C" ULONG SetSignal(ULONG value, ULONG mask)
{ assert(!native_caller);ULONG old=pending_signals;pending_signals=(old&~mask)|(value&mask);return old; }
extern "C" void Delay(LONG) { std::abort(); }
extern "C" void Kaffeine_AppSignalGone(KaffeineContext *, BYTE) {}
extern "C" BOOL Kaffeine_DomainUnload(KaffeineContext *, KaffeineDomainModule *m)
{ check(!m->unloads,"packet Process module releases once after broker ACK");++m->unloads;return TRUE; }
extern "C" void Kaffeine_LockOwnerDrop(KaffeineContext *, Task *) {}
extern "C" BOOL Kaffeine_DosBrokerIsIdle(KaffeineDosBroker *) { return TRUE; }
extern "C" BOOL Kaffeine_DosBrokerCall(KaffeineDosBroker *b,KaffeineContext *ctx,
    void *cpu,ULONG lvo,KaffeineDosBrokerThunk thunk,IPTR *result,SIPTR *error)
{
    assert(!native_caller && ctx->broker_call && ctx->cpu==cpu);
    ++b->calls;
    if(!lvo && thunk!=Kaffeine_ProcessInitGuestDos)++b->helpers;
    if(b->fail) { *error=103;return FALSE; }
    auto *old_owner=ctx->broker_owner;auto *old_active=packet_active_broker;
    BPTR old_input=native_input,old_output=native_output;
    SIPTR caller_error=ioerr;
    native_caller=&b->process.pr_Task;packet_active_broker=b;ioerr=b->error;
    native_input=b->process.pr_CIS;native_output=b->process.pr_COS;
    ctx->broker_owner=b->wrong_owner ? &native_task : native_caller;
    if(thunk==Kaffeine_ProcessInitGuestDos)++b->inits;
    *result=thunk(ctx,cpu);
    if(ctx->after_thunk) { auto after=ctx->after_thunk;ctx->after_thunk=nullptr;after(ctx); }
    b->error=ioerr;ioerr=caller_error;ctx->broker_owner=old_owner;
    b->process.pr_CIS=native_input;b->process.pr_COS=native_output;
    native_input=old_input;native_output=old_output;
    native_caller=nullptr;packet_active_broker=old_active;*error=0;return TRUE;
}
extern "C" BOOL Kaffeine_DosBrokerFinish(KaffeineDosBroker *b,KaffeineContext *ctx,KaffeineDosBrokerThunk cleanup)
{
    assert(!b->finishes && !native_caller);
    if(ctx)
    {
        auto *old=ctx->broker_owner;SIPTR caller_error=ioerr;
        BPTR old_input=native_input,old_output=native_output;
        native_caller=&b->process.pr_Task;packet_active_broker=b;ioerr=b->error;
        native_input=b->process.pr_CIS;native_output=b->process.pr_COS;
        ctx->broker_owner=native_caller;
        if(cleanup) { if(cleanup==Kaffeine_ProcessDropGuestDos)++b->drops;cleanup(ctx,nullptr); }
        b->process.pr_CIS=native_input;b->process.pr_COS=native_output;
        native_input=old_input;native_output=old_output;
        ctx->broker_owner=old;native_caller=nullptr;packet_active_broker=nullptr;ioerr=caller_error;
    }
    ++b->finishes;return TRUE;
}
extern "C" BOOL Kaffeine_DosBrokerStop(KaffeineDosBroker *b)
{ return Kaffeine_DosBrokerFinish(b,nullptr,nullptr); }

static KaffeineCPU *packet_cpu_create(KaffeineContext *)
{ ++packet_cpus_created;return new KaffeineCPU{&ops,new CPUState{}}; }
static void packet_cpu_destroy(KaffeineContext *,KaffeineCPU *cpu)
{ ++packet_cpus_destroyed;delete static_cast<CPUState *>(cpu->backend_state);delete cpu; }
static const KaffeineCPUFactory packet_cpu_factory={packet_cpu_create,packet_cpu_destroy};

static void observe_packet_owner()
{
    ++packet_observations;
    if(packet_expected_broker)
        check(packet_active_broker==packet_expected_broker &&
              FindTask(nullptr)==&packet_expected_broker->process.pr_Task &&
              packet_observe_ctx->broker_call &&
              packet_observe_ctx->broker_owner==FindTask(nullptr) &&
              reinterpret_cast<Process *>(FindTask(nullptr))->pr_CurrentDir==reinterpret_cast<BPTR>(0x22220000) &&
              ioerr==packet_expected_broker->error,
              "native packet operation uses only the child's Process, directory and private IoErr");
    else
        check(!packet_active_broker && FindTask(nullptr)==&native_task && !packet_observe_ctx->broker_call &&
              native_process.pr_CurrentDir==reinterpret_cast<BPTR>(0x11110000),
              "ordinary root packet keeps its original native Process and directory");
    if(packet_try_guest_callback)
    {
        KaffeineGuestCall call={};
        check(!Kaffeine_CallGuest(packet_observe_ctx,packet_observe_ctx->cpu,0xC004,&call),
              "native packet worker cannot execute a guest CPU callback");
    }
}

struct ProcessPacketFixture
{
    Fixture f;
    KaffeineDosBroker broker;
    KaffeineDomainModule module;
    ULONG child=0,token=0,handler=0,segment=0;

    ProcessPacketFixture(bool start=true)
    {
        packet_active_broker=packet_expected_broker=nullptr;packet_observe_ctx=&f.ctx;
        packet_observations=packet_cpus_created=packet_cpus_destroyed=0;packet_try_guest_callback=false;
        native_task.tc_Node.ln_Pri=0;
        native_process.pr_CurrentDir=reinterpret_cast<BPTR>(0x11110000);
        broker.process.pr_Task.tc_Node.ln_Type=NT_PROCESS;
        broker.process.pr_CurrentDir=reinterpret_cast<BPTR>(0x22220000);
        f.ctx.cpu=&f.cpu;f.ctx.cpu_factory=&packet_cpu_factory;f.ctx.task_ops=&Kaffeine_GuestTaskOps;
        f.ctx.stack_bottom=f.stack;f.ctx.stack_top=f.stack+256;f.state.a[7]=f.stack+252;
        f.ctx.running=TRUE;f.ctx.libs[1].dos=TRUE;
        std::strcpy(f.ctx.libs[0].name,"exec.library");std::strcpy(f.ctx.libs[1].name,"dos.library");
        f.ctx.mem[f.task+8]=NT_PROCESS;
        f.ctx.mem[f.exec_base+294]=f.ctx.mem[f.exec_base+295]=0xFF;
        f.w(f.exec_base+316,0xFFFF);
        for(ULONG list:{f.exec_base+406,f.exec_base+420}) { f.w(list,list+4);f.w(list+4,0);f.w(list+8,list); }
        segment=Kaffeine_HeapAlloc(&f.ctx,16,MEMF_CLEAR);
        token=Kaffeine_FileImport(&f.ctx,open_result,FALSE);handler=f.r(token*4+8);
        assert(segment && token && handler);
        if(start)start_typed();
        packet_native_observer=observe_packet_owner;
    }
    void start_typed()
    {
        KaffeineProcessStart spec={};spec.name="packet child";spec.command="C:packet";
        spec.arguments="";spec.stack_size=1024;spec.priority=1;spec.backend="moira";
        child=Kaffeine_SchedStartProcess(&f.ctx,&f.cpu,&spec,segment>>2,segment+4,&broker,&module,nullptr);
        check(child && f.ctx.sched && broker.inits==1,"typed packet test starts a real scheduler Process");
        assert(child && f.ctx.sched);
    }
    void enter_child()
    {
        f.ctx.task_ops->boundary(&f.ctx);
        check(f.ctx.cpu!=&f.cpu && f.ctx.guest_exec_task==child && f.r(f.exec_base+276)==child,
              "packet sender is the scheduler's current child CPU and Process view");
        packet_expected_broker=&broker;
    }
    ~ProcessPacketFixture()
    {
        f.ctx.layer_ops=nullptr;f.ctx.callback_depth=0;f.ctx.broker_call=FALSE;f.ctx.broker_owner=nullptr;
        native_caller=nullptr;packet_native_observer=nullptr;
        if(f.ctx.sched)f.ctx.task_ops->end(&f.ctx);
        check(packet_cpus_created==packet_cpus_destroyed,"packet lifecycle retires each child CPU once");
        if(child)check(broker.finishes==1 && broker.drops==1 && module.unloads==1,
                       "packet lifecycle acknowledges native cleanup before module/CPU teardown");
        f.ctx.cpu=nullptr;f.ctx.cpu_factory=nullptr;f.ctx.task_ops=nullptr;f.ctx.num_shadows=0;
        packet_observe_ctx=nullptr;packet_expected_broker=nullptr;
    }
    ULONG call(bool dos,ULONG lvo,ULONG a0=0,ULONG a1=0,ULONG d1=0,ULONG d2=0,ULONG d3=0,ULONG d4=0)
    {
        auto *cpu=f.ctx.cpu;auto *state=static_cast<CPUState *>(cpu->backend_state);
        ULONG sp=state->a[7];if(!sp)sp=f.stack+252;
        state->a[0]=a0;state->a[1]=a1;state->a[7]=sp-4;
        state->d[0]=0x12345678;state->d[1]=d1;state->d[2]=d2;state->d[3]=d3;state->d[4]=d4;
        f.w(sp-4,0xABCD1234);KaffeineDispatchResult result={};
        check(Kaffeine_DispatchLibraryCall(&f.ctx,cpu,(dos?f.dos_base:f.exec_base)-lvo,&result),
              "packet operation reaches the real dispatcher");
        check(result.return_pc==0xABCD1234 && state->a[7]==sp,"packet route consumes one guest return slot");
        return state->d[0];
    }
    void packet(ULONG reply,LONG action=ACTION_READ)
    { zero_standard_packet(f,f.msg,reply,action,f.r(token*4+36),f.buffer,4); }
};

static void process_packet_success()
{
    for(bool root_reply:{false,true})
    {
        ProcessPacketFixture p;
        /* A live root with an active scheduler retains its direct adapter. */
        p.packet(p.f.reply);unsigned calls=p.broker.calls;
        check(p.call(false,366,p.handler,p.f.msg)==0x12345678 && reads==1 && p.broker.calls==calls,
              "root packet behavior is unchanged while a typed child exists");
        check(p.call(false,372,p.f.reply)==p.f.msg,"root removes its exact ordinary packet reply");
        positions[open_result]=0;p.enter_child();
        ULONG reply=(root_reply?Fixture::task:p.child)+M68K_PR_MSGPORT;
        p.packet(reply);SIPTR driver_error=ioerr;unsigned ns=signals;
        calls=p.broker.calls;
        check(p.call(false,366,p.handler,p.f.msg)==0x12345678 && p.f.ctx.running && reads==2 &&
              p.broker.calls==calls+1 && p.broker.helpers==1 && packet_observations==2,
              "typed PutMsg executes the real file packet once through its own compiled broker helper");
        check(!std::memcmp(p.f.ctx.mem+p.f.buffer,"ABCD",4) && p.f.r(p.f.msg+32)==4 &&
              !p.f.r(p.f.msg+36) && p.f.r(p.f.msg+24)==p.handler && p.f.r(reply+20)==p.f.msg &&
              p.f.ctx.mem[p.f.msg+8]==5 && m68k_read16(&p.f.ctx,p.f.msg+18)==68,
              "typed ACTION_READ preserves the original 68-byte type5 packet and BE reply queue");
        check(ioerr==driver_error && p.broker.error==733 && !p.f.ctx.broker_call && !p.f.ctx.broker_owner,
              "packet completion restores driver IoErr and leaves the broker's error private");
        check(root_reply ? (signals==ns+1 && signalled_mask==SIGF_DOS && (pending_signals&SIGF_DOS))
                         : (signals==ns && (p.f.r(p.child+26)&SIGF_DOS)),
              "reply signals reach the captured live root driver or only the child's virtual DOS bit");
        check(p.call(false,384,reply)==p.f.msg && p.call(false,372,reply)==p.f.msg,
              "typed packet WaitPort/GetMsg preserves exact reply identity");

        /* The real DOS DoPkt and SendPkt thunks already execute in this broker. */
        ULONG fs=Kaffeine_HandlerPort(&p.f.ctx,ram_port());calls=p.broker.calls;
        packet_result=17;packet_error=23;
        check(p.call(true,240,0,0,fs,ACTION_SCREEN_MODE,1)==17 && p.f.ctx.running &&
              p.broker.calls==calls+1 && p.broker.helpers==1 && native_packets==1,
              "typed DoPkt uses the real handler adapter without a recursive broker RPC");
        p.packet(p.f.reply,ACTION_READ);positions[open_result]=0;calls=p.broker.calls;
        check(p.call(true,246,0,0,p.f.msg+20,p.handler,p.f.reply)==0 && p.f.ctx.running &&
              p.broker.calls==calls+1 && p.broker.helpers==1 && reads==3,
              "typed SendPkt uses the real file adapter in the same selected broker");
        check(p.call(false,372,p.f.reply)==p.f.msg,"nested SendPkt queues the exact original reply");
        zero_standard_packet(p.f,p.f.msg,p.f.reply,ACTION_IS_FILESYSTEM);
        calls=p.broker.calls;
        check(p.call(false,366,fs,p.f.msg)==0x12345678 && p.f.ctx.running &&
              p.broker.calls==calls+1 && p.broker.helpers==2 && native_packets==2 &&
              p.f.r(p.f.msg+32)==17 && p.f.r(p.f.msg+36)==23,
              "direct typed handler PutMsg uses the same validated compiled broker route");
        check(p.call(false,372,p.f.reply)==p.f.msg,"handler reply is an ordinary guest FIFO node");
        read_override=-1;p.packet(p.f.reply);calls=p.broker.calls;SIPTR private_error=p.broker.error;
        check(p.call(false,366,p.handler,p.f.msg)==0x12345678 && p.f.ctx.running &&
              p.f.r(p.f.msg+32)==static_cast<ULONG>(-1) && p.f.r(p.f.msg+36)==static_cast<ULONG>(-123) &&
              ioerr==driver_error && p.broker.error==private_error && p.broker.calls==calls+1,
              "native packet errors stay in dp_Res2 and preserve each Process's IoErr");
    }
}

static ProcessPacketFixture *packet_run_fixture;
static void packet_callback_slice(void *)
{
    auto &p=*packet_run_fixture;
    check(p.call(false,366,p.handler,p.f.msg)==0 && p.f.ctx.callback_failed && p.f.ctx.exit_code==20,
          "packet callback refusal is marked even when its enclosing adapter returns success");
    Kaffeine_SetD(p.f.ctx.cpu,0,0); // an enclosing void thunk's ordinary result
}

static void process_packet_refusals()
{
    for(unsigned mode=0;mode<14;++mode)
    {
        ProcessPacketFixture p;p.enter_child();p.packet(p.f.reply);
        KaffeineLayerOps locked={};locked.info_locked=[](KaffeineContext *){return TRUE;};
        auto *cpu=p.f.ctx.cpu;unsigned calls=p.broker.calls;ULONG port=p.handler,msg=p.f.msg;
        if(mode==0)p.f.w(msg+20,msg+4);                 // forged link
        if(mode==1)p.f.w(msg+48,0x80000000);           // negative read
        if(mode==2)p.f.w(msg+44,p.f.reply);           // reply-queue alias
        if(mode==3)p.f.ctx.mem[port+14]=PA_SIGNAL;    // forged private handler
        if(mode==4){p.f.ctx.num_shadows=1;p.f.ctx.shadow_map[0]={msg,&native_task,KAFFEINE_SHADOW_WAITMESSAGE,0};}
        if(mode==5)p.f.ctx.callback_depth=1;
        if(mode==6)p.f.ctx.layer_ops=&locked;
        if(mode==7)p.f.w(p.f.exec_base+276,Fixture::task);
        if(mode==8)p.f.ctx.guest_exec_task=Fixture::task;
        if(mode==9)p.f.ctx.cpu=&p.f.cpu;
        if(mode==10){p.f.ctx.broker_call=TRUE;p.f.ctx.broker_owner=&native_task;}
        if(mode==11)p.broker.wrong_owner=true;
        if(mode==12)p.broker.fail=true;
        if(mode==13)port=p.f.port;                     // unproven ordinary port
        auto before=p.f.bytes();unsigned nr=reads,nw=writes,np=native_packets,ns=signals;
        ULONG result;
        if(mode==13)
        {
            IPTR out=0;p.f.ctx.task_ops->packet_call(&p.f.ctx,port,msg,TRUE,&out);result=static_cast<ULONG>(out);
        }
        else result=p.call(false,366,port,msg);
        check(result==20 && !p.f.ctx.running && p.f.ctx.exit_code==20 &&
              reads==nr && writes==nw && native_packets==np && signals==ns && packet_observations==0,
              "malformed/mixed/callback/LayerInfo/identity/transport packet is refused before native effects");
        /* Dispatch writes its own return slot, so compare packet, queue and
           target data rather than treating that trusted stack slot as payload. */
        check(!std::memcmp(p.f.ctx.mem+msg,before.data()+msg,68) &&
              !std::memcmp(p.f.ctx.mem+p.f.reply,before.data()+p.f.reply,34) &&
              !std::memcmp(p.f.ctx.mem+p.f.buffer,before.data()+p.f.buffer,32),
              "refused packet preserves every result, reply-queue and data byte");
        if(mode==11 || mode==12)
        {
            check(p.f.ctx.callback_failed,"worker/transport packet refusal has authoritative RETURN_FAIL");
            p.broker.fail=p.broker.wrong_owner=false;p.f.ctx.running=TRUE;p.f.ctx.callback_failed=FALSE;
            p.f.ctx.exit_code=0;p.packet(p.f.reply);
            check(p.call(false,366,p.handler,p.f.msg)==0x12345678 && reads==1 && p.f.ctx.running &&
                  p.broker.calls==calls+2,"failed packet clears its staged request before a later serial operation");
        }
        p.f.ctx.cpu=cpu;
    }
    {
        ProcessPacketFixture p;p.enter_child();p.packet(p.f.reply);IPTR result=0;
        check(p.f.ctx.task_ops->process_dos(&p.f.ctx,p.f.ctx.cpu,0,&result) && result==20 && !reads,
              "the typed packet route never makes a zero-LVO DOS capability general");
    }
    {
        ProcessPacketFixture p;p.enter_child();p.packet(p.f.reply);packet_try_guest_callback=true;
        auto *cpu=p.f.ctx.cpu;auto *saved_ops=cpu->ops;KaffeineCPUOps run_ops=*saved_ops;
        run_ops.execute=packet_callback_slice;cpu->ops=&run_ops;packet_run_fixture=&p;
        check(Kaffeine_RunCPU(&p.f.ctx,cpu)==20 && p.f.ctx.callback_failed &&
              Kaffeine_GetD(cpu,0)==0 && p.f.ctx.exit_code==20,
              "real RunCPU preserves RETURN_FAIL after packet worker callback refusal and void D0=0");
        cpu->ops=saved_ops;packet_run_fixture=nullptr;
        packet_try_guest_callback=false;
    }
}

static void process_packet_legacy()
{
    for(bool classic_process:{false,true})
    {
        ProcessPacketFixture p(false);auto &f=p.f;ULONG child=0;
        if(classic_process)
        {
            std::strcpy(reinterpret_cast<char *>(f.ctx.mem+0xC800),"classic packet child");
            f.state.d[1]=0xC800;f.state.d[2]=1;f.state.d[3]=p.segment>>2;f.state.d[4]=1024;
            LONG error=0;ULONG port=Kaffeine_ProcCreate(&f.ctx,&f.cpu,&error);child=port?port-M68K_PR_MSGPORT:0;
        }
        else
        {
            child=Kaffeine_HeapAlloc(&f.ctx,92,MEMF_CLEAR);ULONG stack=Kaffeine_HeapAlloc(&f.ctx,1024,MEMF_CLEAR);
            f.ctx.mem[child+9]=1;f.w(child+58,stack);f.w(child+62,stack+1024);
            f.state.a[1]=child;f.state.a[2]=p.segment+4;f.state.a[3]=0;
            check(Kaffeine_TaskAdd(&f.ctx,&f.cpu)==child,"legacy packet test adds a real guest Task");
        }
        assert(child && f.ctx.sched);f.ctx.task_ops->boundary(&f.ctx);p.packet(f.reply);
        check(p.call(false,366,p.handler,f.msg)==20 && !reads && !native_packets && !p.broker.calls,
              classic_process ? "CreateProc packet still has no native DOS broker/context"
                              : "ordinary AddTask preserves its foreign-Process packet refusal");
    }
}

static void typed_process_packets()
{ process_packet_success();process_packet_refusals();process_packet_legacy(); }
