/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Production typed parser with recording lease/startup collaborators. */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/tasks.h>
extern "C" {
#include "kaffeine_process_tags.h"
#include "kaffeine_process.h"
#include "kaffeine_segments.h"
#include "kaffeine_locks.h"
}
static unsigned checks, failures, mb, hb, me, he, starts;
static SIPTR error;
static Task current;
static bool module_ok, home_ok, ma, ha, mc, hc;
static const char *backend;
static LONG backend_length;
static ULONG result;
static KaffeineProcessStart captured;
static std::string name, command, args, engine;
static constexpr ULONG module_token=0x20000001, home_token=0x123456;
static constexpr IPTR native_module=static_cast<IPTR>(0x1122334455667788ULL);
static constexpr IPTR native_home=static_cast<IPTR>(0x2233445566778899ULL);
static constexpr SIPTR startup_error=static_cast<SIPTR>(0x1122334455667788ULL);
static void check(bool ok,const char *what)
{ ++checks; if(!ok){++failures;std::printf("FAIL %s\n",what);} }
extern "C" SIPTR IoErr(void){return error;}
extern "C" SIPTR SetIoErr(SIPTR v){SIPTR old=error;error=v;return old;}
extern "C" Task *FindTask(CONST_STRPTR){return &current;}
extern "C" LONG GetVar(CONST_STRPTR,STRPTR dst,LONG size,LONG)
{
    if(backend_length<0){SetIoErr(ERROR_OBJECT_NOT_FOUND);return -1;}
    std::strncpy(dst,backend,size-1);dst[size-1]=0;return backend_length;
}
extern "C" BOOL Kaffeine_SegmentTransferBegin(KaffeineContext *,ULONG token,KaffeineSegmentTransfer *lease)
{
    ++mb;
    if(!module_ok||token!=module_token){SetIoErr(ERROR_OBJECT_WRONG_TYPE);return FALSE;}
    *lease={};lease->native=reinterpret_cast<BPTR>(native_module);lease->active=TRUE;ma=true;return TRUE;
}
extern "C" void Kaffeine_SegmentTransferEnd(KaffeineContext *,KaffeineSegmentTransfer *lease,BOOL consumed)
{
    check(lease->active&&ma,"one active module transfer ends once");
    ++me;mc=consumed;ma=false;lease->active=FALSE;SetIoErr(-999);
}
extern "C" BOOL Kaffeine_LockTransferBegin(KaffeineContext *,ULONG token,KaffeineLockTransfer *lease)
{
    ++hb;
    if(!home_ok||(token&&token!=home_token)){SetIoErr(ERROR_OBJECT_WRONG_TYPE);return FALSE;}
    *lease={};lease->native=token?reinterpret_cast<BPTR>(native_home):nullptr;lease->active=TRUE;ha=true;return TRUE;
}
extern "C" void Kaffeine_LockTransferEnd(KaffeineContext *,KaffeineLockTransfer *lease,BOOL consumed)
{
    check(lease->active&&ha,"one active home transfer ends once");
    ++he;hc=consumed;ha=false;lease->active=FALSE;SetIoErr(-888);
}
extern "C" ULONG Kaffeine_DomainStartProcess(KaffeineContext *ctx,const KaffeineProcessStart *spec)
{
    ++starts;check(ma&&ha,"reserve both resources before native startup");captured=*spec;
    name=spec->name;command=spec->command;args=spec->arguments;engine=spec->backend;
    auto guest=[ctx](const void *p){auto a=reinterpret_cast<uintptr_t>(p),base=reinterpret_cast<uintptr_t>(ctx->mem);return a>=base&&a-base<ctx->mem_size;};
    check(!guest(spec->name)&&!guest(spec->command)&&!guest(spec->arguments)&&!guest(spec->backend),
          "native startup receives copied strings outside guest storage");
    SetIoErr(startup_error);return result;
}
struct Fixture
{
    KaffeineContext ctx{};std::vector<UBYTE> bytes=std::vector<UBYTE>(16384);
    Fixture(){ctx.mem=bytes.data();ctx.mem_size=bytes.size();module_ok=home_ok=true;ma=ha=mc=hc=false;
        mb=hb=me=he=starts=0;backend="jit";backend_length=3;current.tc_Node.ln_Pri=5;result=0x800;error=77;}
    void text(ULONG at,const std::string &s){std::memcpy(bytes.data()+at,s.c_str(),s.size()+1);}
    void tags(std::initializer_list<std::pair<ULONG,ULONG>> values){ULONG at=0x100;
        for(auto v:values){m68k_write32(&ctx,at,v.first);m68k_write32(&ctx,at+4,v.second);at+=8;}
        m68k_write32(&ctx,at,TAG_DONE);m68k_write32(&ctx,at+4,0);}
    ULONG run(ULONG at=0x100){auto before=bytes;ULONG ret=Kaffeine_ProcessFromTags(&ctx,at);
        check(before==bytes,"startup does not modify guest bytes");return ret;}
    void refused(SIPTR expected,ULONG at=0x100){check(!run(at)&&IoErr()==expected,"deterministic refusal/error");
        check(!mb&&!hb&&!starts,"refuse before native resource reservation");}
};
int main()
{
    {Fixture f;f.text(0x800,"TurboText");
     f.tags({{NP_StackSize,5000},{NP_Seglist,module_token},{NP_FreeSeglist,1},{NP_Name,0x800},
         {NP_CommandName,0x800},{NP_ConsoleTask,0},{NP_WindowPtr,0},{NP_CopyVars,0},{NP_Cli,1},{NP_HomeDir,home_token}});
     check(f.run()==result&&starts==1,"accept observed independent-program shape");
     check(captured.stack_size==5000&&captured.cli&&!captured.copy_vars&&captured.priority==5,"preserve typed scalars/default priority");
     check(reinterpret_cast<IPTR>(captured.seglist)==native_module&&reinterpret_cast<IPTR>(captured.home_dir)==native_home,"pass complete native lease references");
     check(name=="TurboText"&&command==name&&args.empty()&&engine=="jit","copy names/arguments/engine");
     check(mc&&hc&&me==1&&he==1&&IoErr()==startup_error,"commit both transfers and preserve full IoErr");}
    for(ULONG stack:{256U,0x1234U,65536U}){Fixture f;f.tags({{NP_Seglist,module_token},{NP_StackSize,stack}});
        check(f.run()==result&&captured.stack_size==stack,"scalar in arena range is never pointer-converted");
        check(name=="68k process"&&command==name&&captured.copy_vars&&!captured.cli&&!captured.home_dir,"supported defaults");}
    for(ULONG stack:{0U,255U,65537U,0xFFFFFFFFU}){Fixture f;f.tags({{NP_Seglist,module_token},{NP_StackSize,stack}});f.refused(ERROR_BAD_NUMBER);}
    for(ULONG tag:std::initializer_list<ULONG>{NP_Entry,NP_ExitCode,NP_ExitData,NP_Dummy+26,NP_Input,NP_CurrentDir,0xFFFF0001U}){
        Fixture f;f.tags({{NP_Seglist,module_token},{tag,0xFFFFFFFC}});f.refused(ERROR_NOT_IMPLEMENTED);}
    for(ULONG tag:{NP_Cli,NP_CopyVars}){Fixture f;f.tags({{NP_Seglist,module_token},{tag,2}});f.refused(ERROR_BAD_NUMBER);}
    for(ULONG tag:{NP_ConsoleTask,NP_WindowPtr}){Fixture f;f.tags({{NP_Seglist,module_token},{tag,0x800}});f.refused(ERROR_NOT_IMPLEMENTED);}
    for(ULONG tag:{NP_FreeSeglist,NP_HomeDir}){Fixture f;f.tags({{NP_Seglist,module_token},{tag,0}});f.refused(ERROR_NOT_IMPLEMENTED);}
    {Fixture f;f.tags({{NP_Seglist,module_token},{NP_Seglist,module_token}});f.refused(ERROR_BAD_NUMBER);}
    for(ULONG at:{0U,0x3FFCU,0x4000U,0xFFFFFFFFU}){Fixture f;if(at)m68k_write32(&f.ctx,0x3FFC,NP_Seglist);f.refused(at?ERROR_BAD_NUMBER:ERROR_NOT_IMPLEMENTED,at);}
    {Fixture f;f.tags({{TAG_MORE,0x100}});f.refused(ERROR_BAD_NUMBER);}
    for(ULONG tag:{NP_Name,NP_CommandName,NP_Arguments}){Fixture f;f.tags({{NP_Seglist,module_token},{tag,0x3FFC}});
        std::memset(f.bytes.data()+0x3FFC,'X',4);f.refused(ERROR_BAD_NUMBER);}
    for(unsigned len:{255U,256U}){Fixture f;f.tags({{NP_Seglist,module_token},{NP_Name,0x800}});f.text(0x800,std::string(len,'n'));
        if(len==255)check(f.run()==result&&name.size()==255,"bounded name limit");else f.refused(ERROR_BAD_NUMBER);}
    for(unsigned len:{4095U,4096U}){Fixture f;f.tags({{NP_Seglist,module_token},{NP_Arguments,0x1000}});f.text(0x1000,std::string(len,'a'));
        if(len==4095)check(f.run()==result&&args.size()==4095,"bounded arguments limit");else f.refused(ERROR_BAD_NUMBER);}
    for(LONG pri:{-129,-128,127,128}){Fixture f;f.tags({{NP_Seglist,module_token},{NP_Priority,static_cast<ULONG>(pri)}});
        if(pri==-129||pri==128)f.refused(ERROR_BAD_NUMBER);else check(f.run()==result&&captured.priority==pri,"signed priority boundaries");}
    for(const char *selection:{"moira","","invalid"}){Fixture f;f.tags({{NP_Seglist,module_token}});backend=selection;backend_length=std::strlen(selection);
        if(*selection&&std::strcmp(selection,"moira"))f.refused(ERROR_BAD_NUMBER);else check(f.run()==result&&engine=="moira","explicit/default engine");}
    {Fixture f;f.tags({{NP_Seglist,module_token}});module_ok=false;
     check(!f.run()&&IoErr()==ERROR_OBJECT_WRONG_TYPE&&mb==1&&!hb&&!starts,"module refusal before startup");}
    {Fixture f;f.tags({{NP_Seglist,module_token},{NP_HomeDir,home_token}});home_ok=false;
     check(!f.run()&&IoErr()==ERROR_OBJECT_WRONG_TYPE&&me==1&&!mc&&!starts,"home failure restores module");}
    {Fixture f;f.tags({{NP_Seglist,module_token},{NP_HomeDir,home_token}});result=0;
     check(!f.run()&&starts==1&&!mc&&!hc&&me==1&&he==1&&IoErr()==startup_error,"native failure restores both references and full IoErr");}
    std::printf("process tags: %u checks, %u failed\n",checks,failures);return failures?1:0;
}
