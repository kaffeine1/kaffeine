/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#include <workbench/workbench.h>
#include <proto/icon.h>
#include <strings.h>
#include <set>
#include "message_fixture.h"
#include "kaffeine_icon.h"
#include "kaffeine_sched.h"

static std::set<DiskObject *> objects;
static unsigned icon_gets, frees, finds, matches, icon_dups, icon_puts;
static bool icon_duplicate_failure, cleanup_changes_error;
static BOOL put_result;
static SIPTR put_error;
static KaffeineContext *active_context;
static Task *seen_put_owner;
static DiskObject *seen_source;
static std::map<DiskObject *, DiskObject> clone_saved;
static std::map<DiskObject *, DiskObject *> clone_sources;
static std::string saved_name, saved_tool, saved_window;
static std::vector<std::string> saved_types;
static bool null_name, null_tool, null_window, null_types;
static LONG saved_x, saved_y, saved_stack;
static APTR saved_image, saved_selected;
static DrawerData *saved_drawer;
static bool outside_guest(const void *p)
{
    auto v=reinterpret_cast<uintptr_t>(p),base=reinterpret_cast<uintptr_t>(active_context->mem);
    return !p || v<base || v-base>=active_context->mem_size;
}
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
{
    if(!d)return;
    if(clone_saved.count(d))
    {
        auto &old=clone_saved.at(d);
        check(d->do_DefaultTool==old.do_DefaultTool && d->do_ToolTypes==old.do_ToolTypes &&
              d->do_ToolWindow==old.do_ToolWindow && d->do_CurrentX==old.do_CurrentX &&
              d->do_CurrentY==old.do_CurrentY && d->do_StackSize==old.do_StackSize,
              "temporary native clone fields restored before release");
        clone_saved.erase(d);clone_sources.erase(d);
    }
    assert(objects.erase(d)==1);++frees;delete d;
    if(cleanup_changes_error)ioerr=0xBADF00D;
}
extern "C" DiskObject *DupDiskObjectA(DiskObject *source,TagItem *tags)
{
    ++icon_dups;assert(source && objects.count(source) && !clone_saved.count(source) && !tags);
    seen_source=source;
    if(icon_duplicate_failure)return nullptr;
    auto *copy=new DiskObject(*source);objects.insert(copy);clone_saved[copy]=*copy;clone_sources[copy]=source;
    return copy;
}
extern "C" BOOL PutDiskObject(CONST_STRPTR name,DiskObject *d)
{
    ++icon_puts;assert(d && objects.count(d) && clone_saved.count(d));
    check(outside_guest(name) && outside_guest(d) && outside_guest(d->do_DefaultTool) &&
          outside_guest(d->do_ToolWindow) && outside_guest(d->do_ToolTypes),
          "native write receives only private native structures and copied strings");
    null_name=!name;null_tool=!d->do_DefaultTool;null_window=!d->do_ToolWindow;null_types=!d->do_ToolTypes;
    saved_name=name?name:"";saved_tool=d->do_DefaultTool?d->do_DefaultTool:"";
    saved_window=d->do_ToolWindow?d->do_ToolWindow:"";saved_types.clear();
    if(d->do_ToolTypes)for(unsigned i=0;d->do_ToolTypes[i];++i)
    { assert(i<128);check(outside_guest(d->do_ToolTypes[i]),"tool types use native-width private strings");saved_types.emplace_back(d->do_ToolTypes[i]); }
    saved_x=d->do_CurrentX;saved_y=d->do_CurrentY;saved_stack=d->do_StackSize;
    saved_image=d->do_Gadget.GadgetRender;saved_selected=d->do_Gadget.SelectRender;saved_drawer=d->do_DrawerData;
    auto *original=clone_sources.at(d);
    check(d!=original && d->do_Gadget.GadgetRender==original->do_Gadget.GadgetRender &&
          d->do_Gadget.SelectRender==original->do_Gadget.SelectRender && d->do_DrawerData==original->do_DrawerData,
          "native registered duplicate retains original private image/drawer provenance");
    seen_put_owner=FindTask(nullptr);ioerr=put_error;return put_result;
}
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
        assert(objects.empty());icon_gets=frees=finds=matches=icon_dups=icon_puts=0;getter_failure=empty_metadata=false;
        icon_duplicate_failure=cleanup_changes_error=false;put_result=-1;put_error=0;active_context=&ctx;
        seen_put_owner=nullptr;seen_source=nullptr;clone_saved.clear();clone_sources.clear();
        saved_name.clear();saved_tool.clear();saved_window.clear();saved_types.clear();
        oversized=0;forced_find=nullptr;last_name=nullptr;last_array=nullptr;
        for(unsigned i=0;i<generated.size();++i)generated[i]={icon_lvos[i],poison};
        auto &lib=ctx.libs[1];lib.thunks=icon_manual;lib.num_thunks=std::size(icon_manual);
        lib.gen_thunks=generated.data();lib.num_gen_thunks=generated.size();
        std::strcpy(lib.name,"icon.library");
        ctx.libs[0].pos_size=632;lib.pos_size=34;
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
        auto before=bytes();unsigned total=icon_gets+frees+finds+matches+icon_dups+icon_puts;
        check(icon(lvo,a0,a1)==20 && !ctx.running && ctx.exit_code==20,"invalid icon boundary stops20");
        check(bytes()==before && icon_gets+frees+finds+matches+icon_dups+icon_puts==total,"rejected icon call preserves all guest bytes and native owners");
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
        if(lvo!=78 && lvo!=84 && lvo!=90 && lvo!=96 && lvo!=102 && lvo!=120 && lvo!=132 && lvo!=174)
        {IconFixture f;f.rejected(lvo,0xFFFFFFFF,0xFFFFFFFE);}
    {IconFixture f;f.rejected(90,f.msg);f.ctx.mem[f.ctx.mem_size-2]='X';f.ctx.mem[f.ctx.mem_size-1]='Y';f.rejected(78,f.ctx.mem_size-2);f.rejected(132,0xFFFFFFFF);f.rejected(102,0,f.msg);}
    {IconFixture f;std::memset(f.ctx.mem+f.ctx.mem_size-4096,'X',4096);f.rejected(78,f.ctx.mem_size-4096);}
    {IconFixture f;f.w(0x6800,0);f.rejected(96,0x6801,0x6200);f.rejected(96,f.ctx.mem_size-2,0x6200);}
    {IconFixture f;f.w(0x6800,0x6200);f.w(0x6804,0);std::strcpy(reinterpret_cast<char *>(f.ctx.mem+0x6840),"fixture");
        auto before=f.bytes();forced_find=reinterpret_cast<UBYTE *>(0x123456789ABCULL);
        check(f.icon(96,0x6800,0x6840)==20 && !f.ctx.running && finds==1 && before==f.bytes(),"FindToolType never exposes an unrelated native result pointer");}
}
static ULONG name_at(IconFixture &f,const char *s="RAM:OriginalProject")
{
    std::strcpy(reinterpret_cast<char *>(f.ctx.mem+0x6800),s);return 0x6800;
}
static void put_success()
{
    IconFixture f;ULONG g=f.icon(78,name_at(f));auto *original=f.ctx.icons[0].native;
    auto original_copy=*original;auto before=f.bytes();auto blocks=native_blocks.size();
    cleanup_changes_error=true;put_error=205;
    check(f.icon(84,0x6800,g)==0xFFFFFFFF && f.ctx.running && ioerr==205,
          "PutDiskObject returns native BOOL bits and preserves IoErr across native cleanup");
    check(icon_puts==1 && icon_dups==1 && seen_source==original && objects.size()==1 && native_blocks.size()==blocks,
          "Put uses a real duplicate of its exact owner and releases all temporary storage");
    check(saved_name=="RAM:OriginalProject" && saved_tool==default_tool && saved_window==tool_window && saved_types.size()==3,
          "unchanged snapshot strings and BE32 array marshal to native metadata");
    check(f.bytes()==before && original->do_DefaultTool==original_copy.do_DefaultTool && original->do_ToolTypes==original_copy.do_ToolTypes &&
          original->do_CurrentX==original_copy.do_CurrentX,"Put does not mutate guest bytes or the private original");
    std::strcpy(reinterpret_cast<char *>(f.ctx.mem+0x6900),"SYS:C/ReplacementTool");
    std::strcpy(reinterpret_cast<char *>(f.ctx.mem+0x6940),"CON:////Edited");
    std::strcpy(reinterpret_cast<char *>(f.ctx.mem+0x6980),"MODE=edited");
    std::strcpy(reinterpret_cast<char *>(f.ctx.mem+0x69C0),"LABEL=public fixture");
    f.w(g+50,0x6900);f.w(g+70,0x6940);f.w(g+54,0x6A00);
    f.w(0x6A00,0x6980);f.w(0x6A04,0x69C0);f.w(0x6A08,0);
    f.w(g+58,0x80000000);f.w(g+62,0xFFFFFFFF);f.w(g+74,0xFEDCBA98);
    before=f.bytes();put_result=0;put_error=221;
    check(f.icon(84,0x6800,g)==0 && f.ctx.running && f.ctx.exit_code==0 && ioerr==221,
          "native FALSE and native error are ordinary failure, never unsupported-stop");
    check(saved_tool=="SYS:C/ReplacementTool" && saved_window=="CON:////Edited" &&
          saved_types==std::vector<std::string>{"MODE=edited","LABEL=public fixture"},"guest metadata edits are copied, not silently discarded");
    check(static_cast<ULONG>(saved_x)==0x80000000 && saved_y==-1 && static_cast<ULONG>(saved_stack)==0xFEDCBA98,
          "signed classic position/stack bits retain exact values");
    check(f.bytes()==before && objects.size()==1 && native_blocks.size()==blocks,"native FALSE leaves ownership and guest snapshot unchanged");
    f.w(g+50,0);f.w(g+70,0);f.w(g+54,0);put_result=-1;
    check(f.icon(84,0,g)==0xFFFFFFFF && null_name && null_tool && null_window && null_types,"NULL metadata remains NULL at the native boundary");
    f.w(g+54,0x6A00);f.w(0x6A00,0);
    check(f.icon(84,0x6800,g)==0xFFFFFFFF && !null_types && saved_types.empty(),"empty non-NULL ToolTypes array remains distinct from NULL");
    auto heap=Kaffeine_HeapAvail(&f.ctx,0);ULONG size=f.ctx.icons[0].size;
    Kaffeine_HeapFree(&f.ctx,g,size);
    check(Kaffeine_HeapAvail(&f.ctx,0)==heap && f.ctx.icons[0].guest==g && f.icon(84,0x6800,g)==0xFFFFFFFF,
          "generic FreeMem cannot retire/reuse a live typed icon token");
}
static void put_refusals_and_oom()
{
    {IconFixture f;ULONG g=f.icon(78,name_at(f));f.ctx.mem[0x6800]=0;
     f.rejected(78,0x6800);f.rejected(132,0x6800);f.rejected(84,0x6800,g);}
    for(ULONG offset:{0U,2U,4U,8U,12U,16U,18U,20U,22U,26U,30U,34U,38U,42U,44U,48U,66U})
    {
        IconFixture f;ULONG g=f.icon(78,name_at(f));f.ctx.mem[g+offset]^=1;
        f.rejected(84,0x6800,g);
    }
    {IconFixture f;ULONG g=f.icon(78,name_at(f));f.rejected(84,0x6800,g+2);f.rejected(84,0x6800,0);f.rejected(84,0x6800,0xFFFFFFFF);
     f.icon(90,g);ULONG ordinary=Kaffeine_HeapAlloc(&f.ctx,128,MEMF_PUBLIC);check(ordinary!=0,"ordinary reuse allocation exists");
     f.rejected(84,0x6800,g);Kaffeine_HeapFree(&f.ctx,ordinary,128);}
    for(unsigned mode=0;mode<9;++mode)
    {
        IconFixture f;ULONG g=f.icon(78,name_at(f));
        switch(mode)
        {
            case 0:f.ctx.mem[f.ctx.mem_size-1]='X';f.rejected(84,f.ctx.mem_size-1,g);break;
            case 1:f.w(g+50,0xFFFFFFFF);f.rejected(84,0x6800,g);break;
            case 2:f.w(g+54,0x6A01);f.rejected(84,0x6800,g);break;
            case 3:f.w(g+54,f.ctx.mem_size-2);f.rejected(84,0x6800,g);break;
            case 4:f.w(g+70,Fixture::exec_base);f.rejected(84,0x6800,g);break;
            case 5:f.w(g+54,0x6A00);for(unsigned i=0;i<129;++i)f.w(0x6A00+4*i,0x6800);f.rejected(84,0x6800,g);break;
            case 6:{ULONG other=f.icon(174);f.w(g+50,other);f.rejected(84,0x6800,g);break;}
            case 7:f.w(g+50,f.ctx.heap_start);f.rejected(84,0x6800,g);break;
            case 8:f.w(g+50,f.ctx.mem_size-4096);std::memset(f.ctx.mem+f.ctx.mem_size-4096,'X',4096);f.rejected(84,0x6800,g);break;
        }
    }
    {IconFixture f;ULONG g=f.icon(78,name_at(f));f.w(g+54,0x6A00);
     ULONG text=Kaffeine_HeapAlloc(&f.ctx,4096,MEMF_PUBLIC);assert(text);
     std::memset(f.ctx.mem+text,'X',4095);f.ctx.mem[text+4095]=0;
     for(unsigned i=0;i<17;++i)f.w(0x6A00+i*4,text);f.w(0x6A00+17*4,0);
     f.rejected(84,0x6800,g);Kaffeine_HeapFree(&f.ctx,text,4096);}
    {IconFixture f;ULONG g=f.icon(78,name_at(f));f.w(g+54,0x6A00);
     std::strcpy(reinterpret_cast<char *>(f.ctx.mem+0x6900),"FLAG");
     for(unsigned i=0;i<128;++i)f.w(0x6A00+i*4,0x6900);f.w(0x6A00+128*4,0);
     check(f.icon(84,0x6800,g)==0xFFFFFFFF && saved_types.size()==128,"exact 128-entry tool-type limit is accepted");
     ULONG text=Kaffeine_HeapAlloc(&f.ctx,4096,MEMF_PUBLIC);assert(text);
     std::memset(f.ctx.mem+text,'X',4095);f.ctx.mem[text+4095]=0;f.w(g+54,0);f.w(g+50,text);
     check(f.icon(84,0x6800,g)==0xFFFFFFFF && saved_tool.size()==4095,"4096-byte NUL-terminated ordinary string is accepted");
     f.ctx.mem[text+4095]='X';f.rejected(84,0x6800,g);Kaffeine_HeapFree(&f.ctx,text,4096);}
    {IconFixture f;ULONG g=f.icon(78,name_at(f));f.ctx.memory_info=0x6900;f.w(g+50,0x6900);f.rejected(84,0x6800,g);f.ctx.memory_info=0;}
    {IconFixture f;ULONG g=f.icon(78,name_at(f));f.ctx.fonts[0].guest=0x6900;f.ctx.fonts[0].allocation_size=40;
     f.w(g+50,0x6900);f.rejected(84,0x6800,g);f.ctx.fonts[0]={};}
    for(bool duplicate:{false,true})
    {
        IconFixture f;ULONG g=f.icon(78,name_at(f));auto before=f.bytes();auto blocks=native_blocks.size();
        icon_duplicate_failure=duplicate;if(!duplicate)allocation_countdown=0;cleanup_changes_error=true;
        check(f.icon(84,0x6800,g)==0 && f.ctx.running && ioerr==ERROR_NO_FREE_STORE,"OOM is ordinary FALSE with deterministic no-free-store error");
        allocation_countdown=-1;
        check(!icon_puts && icon_dups==(duplicate?1U:0U) && objects.size()==1 && native_blocks.size()==blocks && f.bytes()==before,
              "scratch/duplicate failure rolls back without native write or ownership loss");
    }
}
static unsigned icon_hook_calls;
static Task icon_owner;
static BOOL icon_route(KaffeineContext *ctx,void *cpu,ULONG lvo,IPTR *result)
{
    ++icon_hook_calls;auto fn=Kaffeine_IconIOThunk(lvo);assert(fn);
    auto *old=native_caller;native_caller=&icon_owner;*result=fn(ctx,cpu);native_caller=old;return TRUE;
}
static void icon_routes()
{
    for(ULONG lvo=0;lvo<205;++lvo)
        check(bool(Kaffeine_IconIOThunk(lvo))==(lvo==78||lvo==84||lvo==120||lvo==132),"icon broker lookup has an exact four-LVO allowlist");
    IconFixture f;KaffeineTaskOps ops{};ops.icon_call=icon_route;
    f.ctx.task_ops=&ops;f.ctx.sched=reinterpret_cast<KaffeineScheduler *>(1);icon_hook_calls=0;
    ULONG g=f.icon(78,name_at(f));check(g && icon_hook_calls==1 && icon_gets==1,"getter wrapper routes once to the compiled native helper");
    check(f.icon(84,0x6800,g)==0xFFFFFFFF && icon_hook_calls==2 && seen_put_owner==&icon_owner,
          "Put wrapper is consumed once by the caller-owner route");
    check(f.icon(120) && f.icon(132,0x6800) && icon_hook_calls==4,"all file-reading icon getters use the fixed route");
    f.ctx.task_ops=nullptr;f.rejected(84,0x6800,g);f.ctx.sched=nullptr;
}
int main()
{
    lifecycle();failures_and_guards();put_success();put_refusals_and_oom();icon_routes();
    check(native_blocks.empty() && objects.empty(),"all native records, snapshots and icons are released");
    std::printf("Guest icon boundary: %u/%u checks passed\n",checks-failures,checks);
    return failures?1:0;
}
