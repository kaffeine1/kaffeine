/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Author: Michele Dipace (@kaffeine1)
 * The runner inserts complete production Signal and startup/owner cleanup
 * functions. The threaded domain-driver fixture tests asynchronous lifetime. */
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

using ULONG=uint32_t; using LONG=int32_t; using UBYTE=uint8_t;
using IPTR=uintptr_t; using BPTR=uintptr_t; using BOOL=int;
using APTR=void *; using CONST_STRPTR=const char *; using STRPTR=char *;
constexpr BOOL TRUE=1,FALSE=0;
constexpr LONG RETURN_FAIL=20;
constexpr unsigned KAFFEINE_MAX_LVO=512;
constexpr BPTR BNULL=0;
struct Task { int id; } parent_task{1}, worker_task{2};
struct KaffeineContext;
struct CPU { ULONG a[8]{},d[8]{}; };
#define THUNK_A(n) (((CPU *)cpu)->a[n])
#define THUNK_D(n) (((CPU *)cpu)->d[n])
struct Ops { IPTR (*signal)(KaffeineContext *,void *); };
struct KaffeineLibBase { int sysBase; };
static KaffeineLibBase fixture_base{};
constexpr ULONG M68K_PR_CIS=156, M68K_PR_COS=160, M68K_PR_CES=224;
struct KaffeineContext {
    std::array<UBYTE,8192> mem{};
    ULONG mem_size{8192};
    KaffeineLibBase *emuBase{&fixture_base};
    ULONG root_streams[3]{};
    Task *exec_task{};
    ULONG guest_exec_task{},io_signals{},layer_signals{},guest_signals{},entry_point{},m68k_argptr{},m68k_argsize{},m68k_sysbase{};
    char program_name[256]{};
    BOOL running{TRUE}; LONG exit_code{};
    void *sched{}; const Ops *task_ops{};
    unsigned native_resources{};
};
static unsigned checks,failures,native_signals,scheduler_calls,refusals,cnp_calls,cpu_calls,autolibs;
static unsigned file_cleanup,handler_cleanup,lock_cleanup;
static unsigned destroy_calls;
static unsigned last_native_resources;
static ULONG captured_streams[3];
static Task *destroy_task,*shadow_task,*autolibs_task,*cpu_task;
static bool owner_bound;
static ULONG native_bits;
static Task *current_task=&parent_task,*cleanup_task;
static bool shadow_ok=true,library_ok=true;
static void check(bool ok,const char *what){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",what);}}
static void bug(const char *,...) {}
static Task *FindTask(const char *){return current_task;}
static void Signal(Task *task,ULONG bits){++native_signals;native_bits=bits;check(task==current_task,"self Signal uses actual native execution task");}
static ULONG guest_task(KaffeineContext *ctx){return ctx->guest_exec_task;}
/* Native layer metadata is a collaborator; the combined-mask helper below is production. */
static ULONG Kaffeine_LayerSignalMask(KaffeineContext *ctx){return ctx->layer_signals;}
static IPTR Kaffeine_IORefuse(KaffeineContext *ctx,const char *){++refusals;ctx->running=FALSE;return ctx->exit_code=20;}
static IPTR sched_signal(KaffeineContext *,void *){++scheduler_calls;return 0x42;}
static const Ops Kaffeine_GuestTaskOps{sched_signal};
static void noop(KaffeineContext *) {}
static void Kaffeine_FileCleanup(KaffeineContext *ctx){++file_cleanup;ctx->native_resources&=~1u;cleanup_task=current_task;}
static void Kaffeine_HandlerCleanup(KaffeineContext *ctx){++handler_cleanup;ctx->native_resources&=~2u;cleanup_task=current_task;}
static void Kaffeine_LockCleanup(KaffeineContext *ctx){++lock_cleanup;ctx->native_resources&=~4u;last_native_resources=ctx->native_resources;cleanup_task=current_task;}
#define Kaffeine_WindowsCleanup noop
#define Kaffeine_MenusCleanup noop
#define Kaffeine_GadgetsCleanup noop
#define Kaffeine_VisualInfosCleanup noop
#define Kaffeine_IOCleanup noop
#define Kaffeine_ScreensCleanup noop
#define Kaffeine_LayersCleanup noop
#define Kaffeine_RegionsCleanup noop
#define Kaffeine_SegmentsCleanup noop
#define Kaffeine_ReadArgsCleanup noop
#define Kaffeine_LocaleCleanup noop
#define Kaffeine_MatchCleanup noop
#define Kaffeine_ExAllCleanup noop
#define Kaffeine_FontsCleanup noop
#define Kaffeine_IconsCleanup noop
static void Kaffeine_DestroyContext(KaffeineContext *){++destroy_calls;destroy_task=current_task;}
static LONG GetVar(const char *,char *out,LONG,LONG){std::strcpy(out,"moira");return 5;}
static LONG Kaffeine_Execute(KaffeineContext *ctx){++cpu_calls;cpu_task=current_task;owner_bound&=ctx->exec_task==current_task;return 7;}
static LONG Kaffeine_ExecuteJIT(KaffeineContext *ctx){++cpu_calls;cpu_task=current_task;owner_bound&=ctx->exec_task==current_task;return 8;}
static ULONG Kaffeine_SetupLibBase(KaffeineContext *,unsigned,...){return library_ok?0x1000:0;}
static int kaffeine_thunks_exec,kaffeine_thunks_exec_count,kaffeine_thunks_exec_gen,kaffeine_thunks_exec_gen_count;
static void setup_dos_base(KaffeineContext *){}
static ULONG Kaffeine_HeapAlloc(KaffeineContext *,ULONG,ULONG){return 0x100;}
static void *m68k_to_host(KaffeineContext *ctx,ULONG at){return ctx->mem.data()+at;}
static void CopyMem(APTR src,APTR dst,ULONG size){std::memcpy(dst,src,size);}
static void m68k_write32(KaffeineContext *ctx,ULONG at,ULONG value){for(unsigned i=0;i<4;i++)ctx->mem[at+i]=value>>(24-i*8);}
static ULONG m68k_read32(KaffeineContext *ctx,ULONG at){ULONG v=0;for(unsigned i=0;i<4;++i)v=(v<<8)|ctx->mem[at+i];return v;}
static void shadow_init_execbase(KaffeineContext *ctx,ULONG,int){
    shadow_task=current_task;owner_bound&=ctx->exec_task==current_task;
    ctx->guest_exec_task=shadow_ok?0x500:0;ctx->native_resources=7;
    if(ctx->guest_exec_task){m68k_write32(ctx,ctx->guest_exec_task+M68K_PR_CIS,0x101);m68k_write32(ctx,ctx->guest_exec_task+M68K_PR_COS,0x102);m68k_write32(ctx,ctx->guest_exec_task+M68K_PR_CES,0x103);}
}
static void Kaffeine_AutoOpenLibs(KaffeineContext *ctx){++autolibs;autolibs_task=current_task;owner_bound&=ctx->exec_task==current_task;for(unsigned i=0;i<3;++i)captured_streams[i]=ctx->root_streams[i];}
/* The independent threaded domain-run fixture verifies async lifetime. This
 * collaborator only selects the execution owner for startup/cleanup routing. */
static BOOL Kaffeine_RunDomain(KaffeineContext *ctx,LONG (*run)(KaffeineContext *),
                              void (*dispose)(KaffeineContext *),LONG *result){
    ++cnp_calls;Task *old=current_task;current_task=&worker_task;
    *result=run(ctx);dispose(ctx);current_task=old;return TRUE;
}

/* PRODUCTION_FUNCTIONS */

static void reset()
{
    native_signals=scheduler_calls=refusals=cnp_calls=cpu_calls=autolibs=0;
    file_cleanup=handler_cleanup=lock_cleanup=0;native_bits=0;cleanup_task=nullptr;
    destroy_calls=0;destroy_task=shadow_task=autolibs_task=cpu_task=nullptr;last_native_resources=0xff;owner_bound=true;
    for(auto &v:captured_streams)v=0;
    current_task=&parent_task;shadow_ok=library_ok=true;
}
int main()
{
    for(bool scheduled:{false,true}){
        reset();KaffeineContext ctx;ctx.guest_exec_task=0x500;ctx.io_signals=0x80;ctx.layer_signals=0x10000;
        ctx.task_ops=&Kaffeine_GuestTaskOps;
        if(scheduled)ctx.sched=reinterpret_cast<void *>(1);
        CPU cpu;cpu.a[1]=ctx.guest_exec_task;cpu.d[0]=0x10190;
        IPTR result=Kaffeine_Signal(&ctx,&cpu);
        if(scheduled)check(result==0x42&&scheduler_calls==1&&!native_signals,"self/root Signal keeps scheduler routing");
        else check(result==0&&native_signals==1&&native_bits==0x110,"self Signal retains SINGLE/DOS while masking independent private I/O and paint wakes");
    }
    for(ULONG task:{0u,0x900u}){
        reset();KaffeineContext ctx;ctx.guest_exec_task=0x500;
        CPU cpu;cpu.a[1]=task;cpu.d[0]=0x40;
        check(Kaffeine_Signal(&ctx,&cpu)==20&&!native_signals&&refusals==1&&!ctx.running&&ctx.exit_code==20,
              "unowned or null Signal target cannot reach native Exec");
        reset();ctx.running=TRUE;ctx.sched=reinterpret_cast<void *>(1);ctx.task_ops=&Kaffeine_GuestTaskOps;
        check(Kaffeine_Signal(&ctx,&cpu)==0x42&&scheduler_calls==1&&!native_signals&&!refusals,
              "scheduled Signal targets are validated by the shared guest scheduler");
    }
    for(ULONG stack:{0u,65536u}){
        reset();KaffeineContext ctx;ctx.exec_task=&parent_task;shadow_ok=false;BOOL transferred=TRUE;
        check(setup_and_execute(&ctx,0x200,nullptr,0,stack,"original-helper",&transferred)==RETURN_FAIL,
              "startup failure retains its established return code");
        check(!ctx.native_resources&&file_cleanup==1&&handler_cleanup==1&&lock_cleanup==1,
              "early startup failure releases native stream/handler/lock metadata");
        check(cleanup_task==(stack?&worker_task:&parent_task)&&!cpu_calls&&cnp_calls==bool(stack)&&!autolibs,
              "failed shadow initialization cleans up in its actual direct or driver owner");
        check(owner_bound&&shadow_task==cleanup_task,"native owner is bound before shadow creation without a registry hook");
        check(transferred==bool(stack)&&destroy_calls==bool(stack),"only a published domain owns disposal after failed startup");
        check(!stack||destroy_task==&worker_task,"failed transferred startup disposes in the driver owner");
    }
    for(ULONG stack:{0u,65536u}){
        reset();KaffeineContext ctx;ctx.exec_task=&parent_task;BOOL transferred=FALSE;
        check(setup_and_execute(&ctx,0x200,nullptr,0,stack,"original-helper",&transferred)==7,
              "normal direct/domain run preserves guest return code");
        check(!last_native_resources&&file_cleanup==1&&handler_cleanup==1&&lock_cleanup==1&&cpu_calls==1&&autolibs==1,
              "normal execution still releases resources exactly once");
        check(owner_bound&&shadow_task==cleanup_task&&autolibs_task==cleanup_task&&cpu_task==cleanup_task,
              "shadow, AutoOpen and CPU keep the same directly bound native owner");
        check(cleanup_task==(stack?&worker_task:&parent_task)&&cnp_calls==(stack?1u:0u),
              "cleanup remains in actual execution owner for both modes");
        check(transferred==bool(stack)&&destroy_calls==(stack?1u:0u),"asynchronous domain alone transfers/disposes context ownership");
        check(!stack||destroy_task==&worker_task,"transferred context disposal stays in driver owner");
        check(captured_streams[0]==0x101&&captured_streams[1]==0x102&&captured_streams[2]==0x103,"startup captures typed borrowed streams before CPU/autolibs");
    }
    reset();KaffeineContext ctx;library_ok=false;BOOL transferred=TRUE;
    check(setup_and_execute(&ctx,0x200,nullptr,0,0,"original-helper",&transferred)==-1&&!ctx.native_resources&&!file_cleanup,
          "failure before shadow acquisition preserves earlier behavior");
    check(!transferred&&!destroy_calls,"failed setup cannot publish domain ownership");
    std::printf("process integration: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
