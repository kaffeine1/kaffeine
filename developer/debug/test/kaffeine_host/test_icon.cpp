/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <workbench/workbench.h>
#include <proto/icon.h>
#include <strings.h>
#include <set>
#include "message_fixture.h"
#include "kaffeine_icon.h"

static std::set<DiskObject *> objects;
static unsigned icon_gets, frees, finds, matches;
static bool getter_failure, empty_metadata;
static unsigned oversized;
static UBYTE *forced_find;
static CONST_STRPTR last_name;
static STRPTR *last_array;
static char mode[]="MODE=one|TWO|three",flag[]="FLAG",count_value[]="COUNT=17";
static char default_tool[]="SYS:C/OriginalTool",tool_window[]="CON:0/0/320/100/Original";
static STRPTR types[]={mode,flag,count_value,nullptr};
static char long_string[4096];
static STRPTR long_array[129];
static DiskObject *acquire(ULONG type)
{
    ++icon_gets;if(getter_failure)return nullptr;
    auto *d=new DiskObject{};objects.insert(d);
    d->do_Magic=0xE310;d->do_Version=1;d->do_Type=static_cast<UBYTE>(type);
    d->do_Gadget.LeftEdge=-3;d->do_Gadget.TopEdge=5;d->do_Gadget.Width=17;d->do_Gadget.Height=19;
    d->do_Gadget.Flags=0x1234;d->do_Gadget.Activation=0x2345;d->do_Gadget.GadgetType=0x3456;
    d->do_Gadget.MutualExclude=-7;d->do_Gadget.GadgetID=0x4567;
    d->do_Gadget.GadgetRender=reinterpret_cast<APTR>(0x123456789ABCULL);
    d->do_CurrentX=-123;d->do_CurrentY=456;d->do_StackSize=8192;
    if(!empty_metadata){d->do_DefaultTool=default_tool;d->do_ToolWindow=tool_window;d->do_ToolTypes=types;}
    if(oversized==1){std::memset(long_string,'X',sizeof(long_string));d->do_DefaultTool=long_string;}
    if(oversized==2){std::fill(std::begin(long_array),std::end(long_array),static_cast<STRPTR>(mode));d->do_ToolTypes=long_array;}
    if(oversized==3){std::memset(long_string,'X',sizeof(long_string));long_string[4095]=0;
        std::fill(std::begin(long_array),std::end(long_array),static_cast<STRPTR>(long_string));
        long_array[128]=nullptr;d->do_ToolTypes=long_array;}
    return d;
}
extern "C" DiskObject *GetDiskObject(CONST_STRPTR name){last_name=name;return acquire(4);}
extern "C" DiskObject *GetDiskObjectNew(CONST_STRPTR name){last_name=name;return acquire(4);}
extern "C" DiskObject *GetDefDiskObject(LONG type){return acquire(type);}
extern "C" DiskObject *NewDiskObject(ULONG type){return acquire(type);}
extern "C" void FreeDiskObject(DiskObject *d)
{ if(!d)return;assert(objects.erase(d)==1);++frees;delete d; }
extern "C" UBYTE *FindToolType(const STRPTR *array,const STRPTR name)
{
    ++finds;last_array=const_cast<STRPTR *>(array);
    if(forced_find)return forced_find;
    size_t n=std::strlen(name);
    for(;*array;++array)if(!strncasecmp(*array,name,n))
        return reinterpret_cast<UBYTE *>(*array+n+((*array)[n]=='='));
    return nullptr;
}
extern "C" BOOL MatchToolValue(UBYTE *text,UBYTE *value)
{
    ++matches;const char *p=reinterpret_cast<char *>(text),*v=reinterpret_cast<char *>(value);
    if(std::strchr(v,'|'))return 0;
    size_t n=std::strlen(v);
    while(*p){if(!strncasecmp(p,v,n)&&(p[n]=='|'||p[n]==0))return -1;
        const char *next=std::strchr(p,'|');if(!next)break;p=next+1;}
    return 0;
}
static const M68KThunkEntry icon_manual[]={KAFFEINE_ICON_THUNKS};
static const ULONG icon_lvos[]={54,72,78,84,90,96,102,108,120,126,132,138,150,156,162,168,174,180,186,192,198};
struct IconFixture:Fixture
{
    std::array<M68KThunkEntry,21> generated;
    IconFixture()
    {
        assert(objects.empty());icon_gets=frees=finds=matches=0;getter_failure=empty_metadata=false;
        oversized=0;forced_find=nullptr;last_name=nullptr;last_array=nullptr;
        for(unsigned i=0;i<generated.size();++i)generated[i]={icon_lvos[i],poison};
        auto &lib=ctx.libs[1];lib.thunks=icon_manual;lib.num_thunks=std::size(icon_manual);
        lib.gen_thunks=generated.data();lib.num_gen_thunks=generated.size();
        std::strcpy(lib.name,"icon.library");
    }
    ~IconFixture(){Kaffeine_IconsCleanup(&ctx);assert(objects.empty());}
    ULONG icon(ULONG lvo,ULONG a0=0,ULONG a1=0,ULONG d0=4)
    {
        for(unsigned i=0;i<8;++i){state.d[i]=0xD0000000+i;state.a[i]=0xA0000000+i;}
        state.d[0]=d0;state.a[0]=a0;state.a[1]=a1;state.a[7]=stack;
        state.sr=0xA715;state.pc=0x12345678;auto saved=state;
        ctx.running=TRUE;ctx.exit_code=0;KaffeineDispatchResult result={};
        check(Kaffeine_DispatchLibraryCall(&ctx,&cpu,dos_base-lvo,&result),"icon LVO dispatch succeeds");
        check(result.return_pc==0x1234ABCD && state.a[7]==stack+4,"icon dispatch preserves return ABI");
        check(state.sr==saved.sr && state.pc==saved.pc,"icon preserves SR/PC");
        for(unsigned i=1;i<8;++i)assert(state.d[i]==saved.d[i]);
        for(unsigned i=0;i<7;++i)assert(state.a[i]==saved.a[i]);
        check(!generated_calls,"unsafe generated icon fallback was not reached");
        return state.d[0];
    }
    void rejected(ULONG lvo,ULONG a0,ULONG a1=0)
    {
        auto before=bytes();unsigned total=icon_gets+frees+finds+matches;
        check(icon(lvo,a0,a1)==20 && !ctx.running && ctx.exit_code==20,"invalid icon boundary stops20");
        check(bytes()==before && icon_gets+frees+finds+matches==total,"rejected icon call preserves all guest bytes and native owners");
    }
};
static void lifecycle()
{
    IconFixture f;ULONG before=Kaffeine_HeapAvail(&f.ctx,0);
    for(ULONG lvo:{78U,120U,132U,174U})
    {
        ULONG g=f.icon(lvo);check(g && g<f.ctx.mem_size,"getter publishes a guest identity");
        check(m68k_read16(&f.ctx,g)==0xE310 && m68k_read16(&f.ctx,g+2)==1 && f.ctx.mem[g+48]==4,"classic scalar header has BE fields");
        check(m68k_read16(&f.ctx,g+8)==0xFFFD && m68k_read16(&f.ctx,g+42)==0x4567 && f.r(g+34)==0xFFFFFFF9,"Gadget scalars retain signed values and ABI offsets");
        for(ULONG off:{4U,22U,26U,30U,38U,44U,66U})check(f.r(g+off)==0,"native object pointers are absent from guest metadata");
        check(f.r(g+58)==0xFFFFFF85 && f.r(g+62)==456 && f.r(g+74)==8192,"position and stack metadata match native fields");
        check(!std::strcmp(reinterpret_cast<char *>(f.ctx.mem+f.r(g+50)),default_tool),"DefaultTool snapshot is complete");
        ULONG arr=f.r(g+54),str=f.r(arr);f.w(0x6800,0);std::strcpy(reinterpret_cast<char *>(f.ctx.mem+0x6800),"mOdE");
        auto snapshot=f.bytes();check(f.icon(96,arr,0x6800)==str+5 && f.bytes()==snapshot,"FindToolType returns original guest value pointer without mutation");
        check(reinterpret_cast<UBYTE *>(last_array)!=f.ctx.mem+arr,"BE32 pointer array was decoded to a native-width array");
        std::strcpy(reinterpret_cast<char *>(f.ctx.mem+0x6820),"two");snapshot=f.bytes();
        check(f.icon(102,str+5,0x6820)==0xFFFFFFFF && f.bytes()==snapshot,"MatchToolValue preserves native BOOL bits and guest strings");
        f.rejected(90,g+2);std::memset(f.ctx.mem+g,0xD4,78);
        check(f.icon(90,g)==0 && frees==icon_gets && objects.empty(),"free uses the stored native owner despite guest edits");
        f.rejected(90,g);check(Kaffeine_HeapAvail(&f.ctx,0)==before,"snapshot release restores guest heap");
    }
    for(unsigned i=0;i<96;++i){ULONG g=f.icon(174);check(g && f.icon(90,g)==0,"sequential icon identities reuse bounded capacity");}
    check(f.icon(90)==0 && frees==icon_gets,"NULL icon free is a no-op");
    for(unsigned i=0;i<32;++i)check(f.icon(174)!=0,"all bounded owner slots are usable");
    unsigned old_gets=icon_gets,old_frees=frees;
    check(f.icon(174)==0 && icon_gets==old_gets+1 && frees==old_frees+1,"owner-table exhaustion releases only the newly acquired object");
    f.ctx.running=FALSE;f.ctx.exit_code=20;Kaffeine_IconsCleanup(&f.ctx);Kaffeine_IconsCleanup(&f.ctx);
    check(objects.empty() && frees==icon_gets && Kaffeine_HeapAvail(&f.ctx,0)==before,"cleanup after a stop releases every owner exactly once");
}
static void failures_and_guards()
{
    for(unsigned mode=0;mode<4;++mode)
    {
        IconFixture f;ULONG before=Kaffeine_HeapAvail(&f.ctx,0);
        if(mode)oversized=mode;else allocation_countdown=0;
        check(f.icon(174)==0 && objects.empty() && icon_gets==1 && frees==1 && Kaffeine_HeapAvail(&f.ctx,0)==before,"failed snapshot publication is leak-free and atomic");
        allocation_countdown=-1;
    }
    {IconFixture f;getter_failure=true;auto before=f.bytes();check(!f.icon(78) && icon_gets==1 && !frees && before==f.bytes(),"native getter failure publishes nothing");}
    {IconFixture f;empty_metadata=true;ULONG g=f.icon(174);check(g && !f.r(g+50) && !f.r(g+54) && !f.r(g+70),"NULL optional metadata remains NULL");}
    for(ULONG lvo:icon_lvos)
        if(lvo!=78 && lvo!=90 && lvo!=96 && lvo!=102 && lvo!=120 && lvo!=132 && lvo!=174)
        {IconFixture f;f.rejected(lvo,0xFFFFFFFF,0xFFFFFFFE);}
    {IconFixture f;f.rejected(90,f.msg);f.ctx.mem[f.ctx.mem_size-2]='X';f.ctx.mem[f.ctx.mem_size-1]='Y';f.rejected(78,f.ctx.mem_size-2);f.rejected(132,0xFFFFFFFF);f.rejected(102,0,f.msg);}
    {IconFixture f;std::memset(f.ctx.mem+f.ctx.mem_size-4096,'X',4096);f.rejected(78,f.ctx.mem_size-4096);}
    {IconFixture f;f.w(0x6800,0);f.rejected(96,0x6801,0x6200);f.rejected(96,f.ctx.mem_size-2,0x6200);}
    {IconFixture f;f.w(0x6800,0x6200);f.w(0x6804,0);std::strcpy(reinterpret_cast<char *>(f.ctx.mem+0x6840),"fixture");
        auto before=f.bytes();forced_find=reinterpret_cast<UBYTE *>(0x123456789ABCULL);
        check(f.icon(96,0x6800,0x6840)==20 && !f.ctx.running && finds==1 && before==f.bytes(),"FindToolType never exposes an unrelated native result pointer");}
}
int main()
{
    lifecycle();failures_and_guards();
    check(native_blocks.empty() && objects.empty(),"all native records, snapshots and icons are released");
    std::printf("Guest icon boundary: %u/%u checks passed\n",checks-failures,checks);
    return failures?1:0;
}
