/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Actual icon adapter and scheduler with recording native icon/Process IO. */
#pragma once
#include "kaffeine_icon.h"
#include <proto/icon.h>
#include <map>
#include <string>

struct ProcessNativeIcon
{
    ULONG private_cookie=0x68CAFE01;
    DiskObject disk={};
    DiskObject saved={};
    bool duplicate=false;
};
static std::map<DiskObject *,ProcessNativeIcon *> process_icons;
static KaffeineContext *icon_observe_ctx;
static KaffeineDosBroker *icon_expected_broker;
static unsigned icon_get_calls,icon_dup_calls,icon_put_calls,icon_free_calls;
static bool icon_put_failure,icon_attempt_callback;
static char icon_default_tool[]="SYS:C/OriginalEditor",icon_tool_window[]="CON:0/0/80/25/Icon";
static char icon_tooltype[]="PROJECT=yes";
static STRPTR icon_tooltypes[]={icon_tooltype,nullptr};
static ULONG icon_native_render;

static void icon_native_owner()
{
    check(icon_observe_ctx && FindTask(nullptr)->tc_Node.ln_Type==NT_PROCESS,
          "native icon IO has a real Process owner");
    if(icon_expected_broker)
        check(packet_active_broker==icon_expected_broker &&
              FindTask(nullptr)==&icon_expected_broker->process.pr_Task &&
              icon_observe_ctx->broker_call && icon_observe_ctx->broker_owner==FindTask(nullptr) &&
              reinterpret_cast<Process *>(FindTask(nullptr))->pr_CurrentDir==reinterpret_cast<BPTR>(0x22220000) &&
              ioerr==icon_expected_broker->error,
              "icon IO uses the child's native Process, relative directory and private IoErr");
    else
        check(!packet_active_broker && FindTask(nullptr)==&native_task && !icon_observe_ctx->broker_call &&
              native_process.pr_CurrentDir==reinterpret_cast<BPTR>(0x11110000),
              "root icon IO keeps its own native Process and directory");
    if(icon_attempt_callback)
    {
        KaffeineGuestCall call={};
        check(!Kaffeine_CallGuest(icon_observe_ctx,icon_observe_ctx->cpu,0xC004,&call),
              "native icon worker cannot execute guest CPU callbacks");
    }
}

static DiskObject *process_icon_acquire(ULONG type,bool observe)
{
    if(observe)icon_native_owner();
    ++icon_get_calls;
    auto *owner=new ProcessNativeIcon;
    auto &d=owner->disk;
    d.do_Magic=0xE310;d.do_Version=1;d.do_Type=static_cast<UBYTE>(type);
    d.do_Gadget.LeftEdge=-3;d.do_Gadget.TopEdge=5;d.do_Gadget.Width=17;d.do_Gadget.Height=19;
    d.do_Gadget.Flags=0x1234;d.do_Gadget.Activation=0x2345;d.do_Gadget.GadgetType=0x3456;
    d.do_Gadget.MutualExclude=-7;d.do_Gadget.GadgetID=0x4567;
    d.do_Gadget.GadgetRender=&icon_native_render;
    d.do_DefaultTool=icon_default_tool;d.do_ToolWindow=icon_tool_window;d.do_ToolTypes=icon_tooltypes;
    d.do_CurrentX=-123;d.do_CurrentY=456;d.do_StackSize=8192;
    owner->saved=d;process_icons.emplace(&d,owner);
    ioerr=217;
    return &d;
}
extern "C" DiskObject *GetDiskObject(CONST_STRPTR name)
{ check(name && !std::strcmp(name,"relative-project"),"relative GetDiskObject name reaches native IO unchanged");return process_icon_acquire(4,true); }
extern "C" DiskObject *GetDiskObjectNew(CONST_STRPTR name)
{ check(name && !std::strcmp(name,"relative-project"),"relative GetDiskObjectNew name reaches native IO unchanged");return process_icon_acquire(4,true); }
extern "C" DiskObject *GetDefDiskObject(LONG type)
{ check(type==4,"GetDefDiskObject preserves its scalar type argument");return process_icon_acquire(type,true); }
extern "C" DiskObject *NewDiskObject(ULONG type)
{ return process_icon_acquire(type,false); }
extern "C" DiskObject *DupDiskObjectA(DiskObject *source,TagItem *tags)
{
    ++icon_dup_calls;
    auto found=process_icons.find(source);
    check(!tags && found!=process_icons.end() && !found->second->duplicate,
          "native duplication receives only the exact owned original, never a shallow stack copy");
    if(found==process_icons.end())return nullptr;
    auto *owner=new ProcessNativeIcon;owner->duplicate=true;
    owner->private_cookie=found->second->private_cookie;owner->disk=*source;owner->saved=owner->disk;
    process_icons.emplace(&owner->disk,owner);
    return &owner->disk;
}
static bool icon_native_pointer(const void *pointer)
{
    auto value=reinterpret_cast<uintptr_t>(pointer),base=reinterpret_cast<uintptr_t>(icon_observe_ctx->mem);
    return !pointer || value<base || value-base>=icon_observe_ctx->mem_size;
}
extern "C" BOOL PutDiskObject(CONST_STRPTR name,DiskObject *disk)
{
    ++icon_put_calls;icon_native_owner();
    auto found=process_icons.find(disk);
    check(found!=process_icons.end() && found->second->duplicate && found->second->private_cookie==0x68CAFE01,
          "native write receives the registered duplicate with private image state intact");
    check(name && !std::strcmp(name,"relative-project") && icon_native_pointer(name),
          "native write owns a copied relative pathname outside the guest arena");
    check(icon_native_pointer(disk->do_DefaultTool) && icon_native_pointer(disk->do_ToolWindow) &&
          icon_native_pointer(disk->do_ToolTypes),"all metadata pointers have native width and private storage");
    if(disk->do_ToolTypes)
        for(unsigned i=0;disk->do_ToolTypes[i];++i)
        { check(i<128 && icon_native_pointer(disk->do_ToolTypes[i]),"ToolType pointer array is native and bounded");if(i>=128)break; }
    check(disk->do_Gadget.GadgetRender==&icon_native_render && disk->do_CurrentX==-111 &&
          disk->do_CurrentY==222 && disk->do_StackSize==16384,
          "guest position/stack edits are marshalled while trusted native rendering is retained");
    check(disk->do_DefaultTool && !std::strcmp(disk->do_DefaultTool,"SYS:C/OriginalEditor"),
          "unchanged snapshot metadata remains readable through the owned mirror");
    ioerr=icon_put_failure?205:0;
    return icon_put_failure?0:static_cast<BOOL>(-1);
}
extern "C" void FreeDiskObject(DiskObject *disk)
{
    if(!disk)return;
    auto found=process_icons.find(disk);check(found!=process_icons.end(),"each native icon frees exactly once");
    if(found==process_icons.end())return;
    auto *owner=found->second;
    if(owner->duplicate)
    {
        check(disk->do_DefaultTool==owner->saved.do_DefaultTool && disk->do_ToolWindow==owner->saved.do_ToolWindow &&
              disk->do_ToolTypes==owner->saved.do_ToolTypes && disk->do_CurrentX==owner->saved.do_CurrentX &&
              disk->do_CurrentY==owner->saved.do_CurrentY && disk->do_StackSize==owner->saved.do_StackSize,
              "temporary clone metadata is restored before native destruction");
        ioerr=911; // cleanup must not replace the native PutDiskObject result's IoErr
    }
    ++icon_free_calls;process_icons.erase(found);delete owner;
}
extern "C" UBYTE *FindToolType(const STRPTR *,const STRPTR) { std::abort(); }
extern "C" BOOL MatchToolValue(UBYTE *,UBYTE *) { std::abort(); }

struct ProcessIconFixture
{
    ProcessPacketFixture p;
    ULONG name=0;
    explicit ProcessIconFixture(bool start=true):p(start)
    {
        check(process_icons.empty(),"previous icon test retained no native owners");
        icon_get_calls=icon_dup_calls=icon_put_calls=icon_free_calls=0;
        icon_put_failure=icon_attempt_callback=false;icon_observe_ctx=&p.f.ctx;icon_expected_broker=nullptr;
        auto &lib=p.f.ctx.libs[1];lib.dos=FALSE;std::strcpy(lib.name,"icon.library");
        lib.thunks=kaffeine_thunks_icon_manual;lib.num_thunks=kaffeine_thunks_icon_manual_count;
        lib.gen_thunks=nullptr;lib.num_gen_thunks=0;
        name=Kaffeine_HeapAlloc(&p.f.ctx,32,MEMF_CLEAR);assert(name);
        std::strcpy(reinterpret_cast<char *>(p.f.ctx.mem+name),"relative-project");
    }
    ~ProcessIconFixture()
    {
        icon_attempt_callback=false;
        Kaffeine_IconsCleanup(&p.f.ctx);
        check(process_icons.empty(),"icon originals and temporary native clones are all released");
        icon_observe_ctx=nullptr;icon_expected_broker=nullptr;
    }
    void child(){p.enter_child();icon_expected_broker=&p.broker;}
    ULONG call(ULONG lvo,ULONG a0=0,ULONG a1=0,ULONG d0=4)
    {
        auto *cpu=p.f.ctx.cpu;auto *state=static_cast<CPUState *>(cpu->backend_state);
        ULONG sp=state->a[7];if(!sp)sp=p.f.stack+252;
        state->a[0]=a0;state->a[1]=a1;state->d[0]=d0;state->a[7]=sp-4;
        p.f.w(sp-4,0xABCD1234);KaffeineDispatchResult result={};
        check(Kaffeine_DispatchLibraryCall(&p.f.ctx,cpu,p.f.dos_base-lvo,&result),"actual icon public wrapper is selected by production dispatch");
        check(result.return_pc==0xABCD1234 && state->a[7]==sp,"icon broker routing pops exactly one guest return slot");
        return state->d[0];
    }
    ULONG get(ULONG lvo=78){return call(lvo,lvo==120?0:name);}
    void edit(ULONG guest)
    {p.f.w(guest+58,static_cast<ULONG>(-111));p.f.w(guest+62,222);p.f.w(guest+74,16384);}
};

static void process_icon_success()
{
    for(ULONG lvo:{78U,120U,132U})
    {
        ProcessIconFixture f;f.child();ioerr=899;unsigned before=f.p.broker.calls;
        ULONG guest=f.get(lvo);
        check(guest && guest!=20 && guest<f.p.f.ctx.mem_size && icon_get_calls==1 && f.p.broker.calls==before+1,
              "each whitelisted icon getter runs in exactly one private native broker call");
        check(f.p.broker.error==217 && ioerr==899 && !f.p.f.ctx.broker_call && !f.p.f.ctx.broker_owner,
              "native icon getter keeps child IoErr private and restores caller owner state");
        if(!guest || guest==20)continue;
        f.edit(guest);auto bytes=f.p.f.bytes();before=f.p.broker.calls;
        check(f.call(84,f.name,guest)==0xFFFFFFFF && f.p.broker.calls==before+1 && icon_put_calls==1 && icon_dup_calls==1,
              "actual PutDiskObject preserves native BOOL bits and uses one typed broker request");
        check(f.p.broker.error==0 && ioerr==899 && f.p.f.bytes()==bytes && f.p.f.ctx.running,
              "PutDiskObject restores its native IoErr after clone cleanup without changing guest metadata");
        icon_put_failure=true;check(f.call(84,f.name,guest)==0 && f.p.broker.error==205 && ioerr==899 && f.p.f.ctx.running,
              "synchronous native write failure returns FALSE and the child's own error without stopping the domain");
        icon_put_failure=false;
    }
    {
        ProcessIconFixture f;unsigned before=f.p.broker.calls;ULONG guest=f.get();
        check(guest && f.p.broker.calls==before && icon_get_calls==1,"validated live root keeps the direct native icon route");
        if(guest && guest!=20){f.edit(guest);check(f.call(84,f.name,guest)==0xFFFFFFFF && f.p.broker.calls==before,"root PutDiskObject does not enter a child broker");}
    }
    check(Kaffeine_IconIOThunk(0)==nullptr && Kaffeine_IconIOThunk(90)==nullptr && Kaffeine_IconIOThunk(174)==nullptr,
          "production helper lookup stays closed and never falls through a weak poison or generated thunk");
    check(Kaffeine_IconIOThunk(78)!=kaffeine_icon_GetDiskObject && Kaffeine_IconIOThunk(84)!=kaffeine_icon_PutDiskObject,
          "broker lookup resolves private native helpers rather than recursively invoking public wrappers");
}

static void process_icon_refusals()
{
    for(bool child:{false,true})
        for(unsigned mode=0;mode<12;++mode)
        {
            ProcessIconFixture f;if(child)f.child();auto &ctx=f.p.f.ctx;auto *cpu=ctx.cpu;
            KaffeineLayerOps locked={};locked.info_locked=[](KaffeineContext *){return TRUE;};
            Task foreign={};foreign.tc_Node.ln_Type=NT_PROCESS;
            void *passed=cpu;ULONG lvo=78;unsigned calls=f.p.broker.calls;
            if(mode==0)passed=nullptr;
            if(mode==1)ctx.cpu=child?&f.p.f.cpu:nullptr;
            if(mode==2)f.p.f.w(f.p.f.exec_base+276,0xDEAD1234);
            if(mode==3)ctx.guest_exec_task=0xDEAD1234;
            if(mode==4)ctx.callback_depth=1;
            if(mode==5)ctx.layer_ops=&locked;
            if(mode==6)native_caller=&foreign;
            if(mode==7){ctx.broker_call=TRUE;ctx.broker_owner=&foreign;}
            if(mode==8)ctx.broker_owner=&native_task;
            if(mode==9)native_task.tc_Node.ln_Type=1 /* NT_TASK */;
            if(mode==10)lvo=96;
            if(mode==11)lvo=0;
            if(child)f.p.f.state.d[0]=0xC0FFEE01;
            auto before=f.p.f.bytes();IPTR result=0;
            check(ctx.task_ops->icon_call && ctx.task_ops->icon_call(&ctx,passed,lvo,&result) && result==20 &&
                  !ctx.running && ctx.exit_code==20 && !icon_get_calls && !icon_put_calls &&
                  f.p.broker.calls==calls && f.p.f.bytes()==before,
                  "wrong CPU/view/owner/callback/LayerInfo/unknown LVO refuses before native icon or guest-memory effects");
            check(Kaffeine_GetD(cpu,0)==20,"refusal writes RETURN_FAIL only to the trusted current private CPU");
            if(child)check(f.p.f.state.d[0]==0xC0FFEE01,"a wrong/null context CPU never overwrites the noncurrent root CPU");
            ctx.cpu=cpu;ctx.layer_ops=nullptr;ctx.callback_depth=0;ctx.broker_call=FALSE;ctx.broker_owner=nullptr;
            native_caller=nullptr;native_task.tc_Node.ln_Type=NT_PROCESS;
        }
    for(bool transport:{false,true})
    {
        ProcessIconFixture f;f.child();unsigned calls=f.p.broker.calls;
        f.p.broker.fail=transport;f.p.broker.wrong_owner=!transport;
        check(f.get()==20 && !f.p.f.ctx.running && f.p.f.ctx.callback_failed && !icon_get_calls,
              "icon worker-owner/transport failure is authoritative RETURN_FAIL before native IO");
        f.p.broker.fail=f.p.broker.wrong_owner=false;f.p.f.ctx.running=TRUE;f.p.f.ctx.exit_code=0;f.p.f.ctx.callback_failed=FALSE;
        check(f.get()!=20 && icon_get_calls==1 && f.p.broker.calls==calls+2,
              "failed icon broker request clears its staged LVO before the next serial operation");
    }
}

static ProcessIconFixture *icon_run_fixture;
static void icon_callback_slice(void *)
{
    auto &f=*icon_run_fixture;
    f.get();
    Kaffeine_SetD(f.p.f.ctx.cpu,0,0); // an enclosing ordinary/void thunk result
}
static void process_icon_callback()
{
    ProcessIconFixture f;f.child();icon_attempt_callback=true;
    auto *cpu=f.p.f.ctx.cpu;auto *old_ops=cpu->ops;KaffeineCPUOps run_ops=*old_ops;
    run_ops.execute=icon_callback_slice;cpu->ops=&run_ops;icon_run_fixture=&f;
    check(Kaffeine_RunCPU(&f.p.f.ctx,cpu)==20 && f.p.f.ctx.callback_failed &&
          f.p.f.ctx.exit_code==20 && Kaffeine_GetD(cpu,0)==0,
          "real RunCPU preserves icon worker callback RETURN_FAIL despite an ordinary D0=0 result");
    cpu->ops=old_ops;icon_run_fixture=nullptr;icon_attempt_callback=false;
}

static void process_icon_legacy()
{
    for(bool classic:{false,true})
    {
        ProcessIconFixture f(false);auto &p=f.p;ULONG child=0;
        if(classic)
        {
            std::strcpy(reinterpret_cast<char *>(p.f.ctx.mem+0xC800),"classic icon child");
            p.f.state.d[1]=0xC800;p.f.state.d[2]=1;p.f.state.d[3]=p.segment>>2;p.f.state.d[4]=1024;
            LONG error=0;ULONG port=Kaffeine_ProcCreate(&p.f.ctx,&p.f.cpu,&error);child=port?port-M68K_PR_MSGPORT:0;
        }
        else
        {
            child=Kaffeine_HeapAlloc(&p.f.ctx,92,MEMF_CLEAR);ULONG stack=Kaffeine_HeapAlloc(&p.f.ctx,1024,MEMF_CLEAR);
            p.f.ctx.mem[child+9]=1;p.f.w(child+58,stack);p.f.w(child+62,stack+1024);
            p.f.state.a[1]=child;p.f.state.a[2]=p.segment+4;p.f.state.a[3]=0;
            check(Kaffeine_TaskAdd(&p.f.ctx,&p.f.cpu)==child,"classic icon fixture adds a real guest Task");
        }
        assert(child && p.f.ctx.sched);p.f.ctx.task_ops->boundary(&p.f.ctx);
        check(f.get()==20 && !icon_get_calls && !icon_put_calls && !p.broker.calls,
              "AddTask and classic CreateProc children cannot use the driver's native icon DOS context");
    }
}

static void typed_process_icons()
{process_icon_success();process_icon_refusals();process_icon_legacy();process_icon_callback();}
