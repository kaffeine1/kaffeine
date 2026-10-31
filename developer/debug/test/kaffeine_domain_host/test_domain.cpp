/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Author: Michele Dipace (@kaffeine1)
 * The actual heap, copier, guest-memory guard and domain adapter are linked.
 * Native segment images and relocation expectations are original test inputs.
 */
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>
#include <dos/dos.h>
#include <dos/doshunks.h>
#include <exec/memory.h>
#include "kaffeine_intern.h"
#include "kaffeine_cpu.h"
#include "kaffeine_domain_modules.h"
#include "kaffeine_domain_memory.h"
#include "kaffeine_messages.h"

static unsigned checks, failures;
static void check(bool ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}
static std::map<void *, ULONG> native_blocks;
static int allocation_countdown = -1;
static unsigned allocation_calls;
static SIPTR io_error;
static KaffeineContext *close_on_allocation;
static int close_countdown = -1;
extern "C" APTR AllocMem(ULONG size, ULONG flags)
{
    ++allocation_calls;
    if (!allocation_countdown) return nullptr;
    if (allocation_countdown > 0) --allocation_countdown;
    if (close_on_allocation && !close_countdown)
    {
        auto *ctx = close_on_allocation; close_on_allocation = nullptr;
        Kaffeine_DomainCleanup(ctx);
    }
    else if (close_countdown > 0) --close_countdown;
    void *p = std::malloc(size ? size : 1);
    if (p) { std::memset(p, flags & MEMF_CLEAR ? 0 : 0xd9, size); native_blocks[p] = size; }
    return p;
}
extern "C" void FreeMem(APTR p, ULONG size)
{
    check(native_blocks.count(p) && native_blocks[p] == size, "native allocation size and single release");
    native_blocks.erase(p); std::free(p); io_error = 0x777;
}
extern "C" APTR AllocVec(ULONG size, ULONG flags) { return AllocMem(size, flags); }
extern "C" void FreeVec(APTR p) { if (p) FreeMem(p, native_blocks.at(p)); }
extern "C" SIPTR IoErr() { return io_error; }
extern "C" SIPTR SetIoErr(SIPTR n) { SIPTR old = io_error; io_error = n; return old; }
extern "C" BOOL Kaffeine_LockProtected(KaffeineContext *, ULONG, ULONG) { return FALSE; }

struct Registers { std::array<ULONG, 8> d, a; ULONG pc; UWORD sr; };
struct Flush { ULONG base, size; };
static std::vector<Flush> flushes;
static void flush(void *, ULONG at, ULONG size) { flushes.push_back({at, size}); }
static KaffeineCPUOps cpu_ops = {};
static ULONG be32(const UBYTE *p) { return ULONG(p[0]) << 24 | ULONG(p[1]) << 16 | ULONG(p[2]) << 8 | p[3]; }
static void put32(UBYTE *p, ULONG n) { for (unsigned i = 0; i < 4; ++i) p[i] = UBYTE(n >> (24 - 8 * i)); }

struct NativeImage
{
    std::vector<std::vector<UBYTE>> data;
    std::vector<std::unique_ptr<UBYTE[]>> memory;
    std::vector<BPTR> links;
    std::vector<HunkRelocation> records;
    HunkRelocTable table{};
    explicit NativeImage(std::vector<ULONG> sizes = {32, 24, 4})
    {
        for (ULONG size : sizes) data.emplace_back(size, 0x5a);
        if (!data.empty() && data[0].size() >= 4)
        { data[0][0] = 0x70; data[0][1] = 7; data[0][2] = 0x4e; data[0][3] = 0x75; }
        if (sizes == std::vector<ULONG>{32, 24, 4})
        {
            records = {{4,0,1,HRT_ABSOLUTE,0}, {8,0,1,HRT_RELATIVE,0},
                       {0,1,0,HRT_ABSOLUTE,0}, {12,0,2,HRT_ABSOLUTE,0},
                       {16,0,2,HRT_ABSOLUTE,0}, {16,0,2,HRT_ABSOLUTE,0}};
            put32(data[0].data()+4, 0x20); put32(data[0].data()+8, 0xfffffff0);
            put32(data[1].data(), 0xabc00000); put32(data[0].data()+12, 0xffff0001);
            put32(data[0].data()+16, 0x1234);
        }
        for (const auto &payload : data)
        {
            auto p = std::make_unique<UBYTE[]>(payload.size()+16);
            std::memset(p.get(), 0, payload.size()+16);
            ULONG size = payload.size()+sizeof(ULONG)+sizeof(BPTR);
            std::memcpy(p.get()+4, &size, 4);
            std::memcpy(p.get()+8+sizeof(BPTR), payload.data(), payload.size());
            links.push_back(MKBADDR(p.get()+8)); memory.push_back(std::move(p));
        }
        for (unsigned i=0; i+1<links.size(); ++i) std::memcpy(BADDR(links[i]), &links[i+1], sizeof(BPTR));
        for (const auto &r : records)
        {
            UBYTE *site = payload(r.hr_Source)+r.hr_Offset;
            ULONG value = be32(site)+ULONG(IPTR(payload(r.hr_Target)));
            if (r.hr_Type==HRT_RELATIVE) value -= ULONG(IPTR(payload(r.hr_Source)));
            put32(site, value);
        }
        table={ULONG(links.size()),ULONG(records.size()),records.data()};
    }
    UBYTE *payload(unsigned i) { return static_cast<UBYTE *>(BADDR(links[i]))+sizeof(BPTR); }
    BPTR head() const { return links.empty()?BNULL:links[0]; }
    void next(unsigned i, BPTR value) { std::memcpy(BADDR(links[i]), &value, sizeof(value)); }
    void size(unsigned i, ULONG n) { std::memcpy(memory[i].get()+4,&n,4); }
};

struct Guest
{
    KaffeineContext *ctx;
    Registers regs{}, saved_regs{};
    KaffeineCPU cpu{};
    ULONG root, available;
    std::array<UBYTE,256> root_bytes;
    Guest()
    {
        ctx=static_cast<KaffeineContext *>(AllocMem(sizeof(*ctx),MEMF_CLEAR));
        ctx->mem_size=1024*1024;
        ctx->mem=static_cast<UBYTE *>(AllocMem(ctx->mem_size+KAFFEINE_MEM_GUARD,MEMF_CLEAR));
        ctx->heap_start=KAFFEINE_HEAP_BASE;ctx->heap_end=ctx->stack_bottom=ctx->mem_size-65536;
        ctx->stack_top=ctx->mem_size;
        check(Kaffeine_HeapInit(ctx),"production heap initialized");
        root=Kaffeine_HeapAlloc(ctx,root_bytes.size(),MEMF_CLEAR);
        root_bytes.fill(0xa7);std::memcpy(ctx->mem+root,root_bytes.data(),root_bytes.size());
        ctx->entry_point=root+4;ctx->seg_start=root;ctx->m68k_argptr=root+100;
        ctx->m68k_argsize=17;ctx->m68k_sysbase=0x1000;ctx->running=TRUE;
        for(unsigned i=0;i<8;++i){regs.d[i]=0x11110000+i;regs.a[i]=0x22220000+i;}
        regs.pc=ctx->entry_point;regs.sr=0x251f;saved_regs=regs;
        cpu_ops.flush_code=flush;cpu.ops=&cpu_ops;cpu.backend_state=&regs;ctx->cpu=&cpu;
        available=Kaffeine_HeapAvail(ctx,0);flushes.clear();
    }
    void unchanged()
    {
        check(ctx->entry_point==root+4&&ctx->seg_start==root&&ctx->stack_top==ctx->mem_size&&
              ctx->stack_bottom==ctx->mem_size-65536&&ctx->m68k_argptr==root+100&&ctx->m68k_argsize==17&&
              ctx->m68k_sysbase==0x1000&&ctx->running&&!ctx->callback_failed&&ctx->cpu==&cpu,
              "root module/entry/stack/arguments and execution state preserved");
        check(regs.d==saved_regs.d&&regs.a==saved_regs.a&&regs.pc==saved_regs.pc&&regs.sr==saved_regs.sr,
              "all data/address registers, PC, CCR/X and mode preserved");
        check(std::equal(root_bytes.begin(),root_bytes.end(),ctx->mem+root),"live root module bytes unchanged");
    }
    ~Guest(){allocation_countdown=-1;close_on_allocation=nullptr;if(ctx){ctx->io_release=nullptr;ctx->cpu=nullptr;Kaffeine_DestroyContext(ctx);}}
};

static std::vector<ULONG> addresses(Guest &g,const NativeImage &native,ULONG seglist)
{
    std::vector<ULONG> out;
    for(unsigned i=0;i<native.data.size();++i)
    {
        check(seglist&&seglist<g.ctx->mem_size/4,"classic guest BPTR bounded and nonzero");
        ULONG data=(seglist<<2)+4;out.push_back(data);
        check(m68k_read32(g.ctx,data-8)==native.data[i].size()+8,"classic size counts its own 8-byte header");
        seglist=m68k_read32(g.ctx,data-4);
    }
    check(!seglist,"classic successor chain terminates exactly");
    return out;
}
static void relocated(Guest &g,const NativeImage &native,const std::vector<ULONG> &at)
{
    auto expected=native.data;
    for(const auto &r:native.records)
    {
        ULONG value=be32(expected[r.hr_Source].data()+r.hr_Offset)+at[r.hr_Target];
        if(r.hr_Type==HRT_RELATIVE)value-=at[r.hr_Source];
        put32(expected[r.hr_Source].data()+r.hr_Offset,value);
    }
    for(unsigned i=0;i<expected.size();++i)
        check(std::equal(expected[i].begin(),expected[i].end(),g.ctx->mem+at[i]),
              "entire hunk matches independent modulo-32 relocation oracle");
}
static ULONG held_at;
static bool deny_all;
static BOOL io_guard(KaffeineContext *,ULONG base,ULONG size,BOOL)
{return !deny_all&&!(held_at>=base&&held_at-base<size);}

int main()
{
    {
        Guest g;NativeImage native;KaffeineDomainImage first{},second{};
        io_error=0x1234567890LL;
        check(Kaffeine_DomainLoad(g.ctx,native.head(),&native.table,&first)&&IoErr()==0x1234567890LL,
              "first domain image loads and preserves full native IoErr");
        auto one=addresses(g,native,first.seglist);relocated(g,native,one);
        check(first.entry==one[0]&&first.module&&g.ctx->domain_module_count==1,"publishes entry/BPTR/native owner together");
        check(flushes.size()==native.data.size()&&g.ctx->jit_generation!=0,"new code notifies backend and shared epoch");
        for(unsigned i=0;i<one.size();++i)
        {
            ULONG base=one[i]-8,size=native.data[i].size()+8,before=Kaffeine_HeapAvail(g.ctx,0);
            Kaffeine_HeapFree(g.ctx,base,size);
            check(!Kaffeine_HeapFreeExact(g.ctx,base,size)&&Kaffeine_HeapAvail(g.ctx,0)==before,
                  "ordinary guest FreeMem/FreeVec cannot release owned hunks");
            check(Kaffeine_HeapGuestRange(g.ctx,one[i],native.data[i].size())&&
                  Kaffeine_MessageMemory(g.ctx,one[i],native.data[i].size()),
                  "static module data remains a real graphics/DOS/message buffer");
        }
        check(Kaffeine_DomainLoad(g.ctx,native.head(),&native.table,&second),"same source imports a second independent image");
        auto two=addresses(g,native,second.seglist);relocated(g,native,two);
        check(one[0]!=two[0]&&first.seglist!=second.seglist&&first.module!=second.module,
              "images have distinct code, seglist and ownership identities");
        for(unsigned i=0;i<one.size();++i)
        {m68k_write32(g.ctx,one[i]-8,0xffffffff);m68k_write32(g.ctx,one[i]-4,first.seglist);}
        auto before=flushes.size();
        check(Kaffeine_DomainUnload(g.ctx,first.module)&&g.ctx->domain_module_count==1&&flushes.size()==before+one.size(),
              "unload ignores edited guest headers and flushes every owned allocation");
        check(!Kaffeine_DomainUnload(g.ctx,first.module)&&IoErr()==ERROR_OBJECT_WRONG_TYPE,
              "already retired opaque record is not dereferenced");
        relocated(g,native,two);g.unchanged();
        check(Kaffeine_DomainUnload(g.ctx,second.module)&&Kaffeine_HeapAvail(g.ctx,0)==g.available,
              "both independent imports release all guest capacity");
    }
    check(native_blocks.empty(),"all native records/heap metadata released after successful lifecycle");

    {
        Guest g;KaffeineDomainImage image{};std::vector<UBYTE> bytes;
        {
            NativeImage native({8});bytes=native.data[0];
            check(Kaffeine_DomainLoad(g.ctx,native.head(),&native.table,&image),"native source borrowed only while importing");
        }
        check(std::equal(bytes.begin(),bytes.end(),g.ctx->mem+image.entry),"guest code remains independent after native source disposal");
        auto epoch=g.ctx->jit_generation;
        auto callbacks=flushes.size();g.ctx->cpu=nullptr;
        check(Kaffeine_DomainUnload(g.ctx,image.module)&&g.ctx->jit_generation>epoch&&flushes.size()==callbacks,
              "retirement advances shared code epoch even after CPU destruction");
        g.ctx->cpu=&g.cpu;g.unchanged();
    }

    {
        Guest g;NativeImage native(std::vector<ULONG>(KAFFEINE_DOMAIN_MAX_HUNKS,4));KaffeineDomainImage image{};
        check(Kaffeine_DomainLoad(g.ctx,native.head(),&native.table,&image),"maximum bounded hunk count is accepted");
        auto at=addresses(g,native,image.seglist);
        check(at.size()==KAFFEINE_DOMAIN_MAX_HUNKS&&image.entry==at[0],"all maximum-count hunks belong to one image");
        check(Kaffeine_DomainUnload(g.ctx,image.module)&&Kaffeine_HeapAvail(g.ctx,0)==g.available,
              "maximum hunk ownership releases without following guest headers");
        g.unchanged();
    }

    for(unsigned mode=0;mode<14;++mode)
    {
        Guest g;NativeImage native;KaffeineDomainImage out{0x1234,0x5678,reinterpret_cast<KaffeineDomainModule *>(1)},saved=out;
        auto table=native.table;const HunkRelocTable *metadata=&table;BPTR source=native.head();
        switch(mode)
        {
            case 0:source=BNULL;break;
            case 1:table.hrt_NumHunks=0;break;
            case 2:table.hrt_NumHunks=2;break;
            case 3:native.next(2,native.head());break;
            case 4:native.size(1,1);break;
            case 5:table.hrt_Relocs=nullptr;break;
            case 6:native.records[0].hr_Source=3;break;
            case 7:native.records[0].hr_Target=3;break;
            case 8:native.records[0].hr_Offset=29;break;
            case 9:native.records[0].hr_Type=2;break;
            case 10:table.hrt_NumHunks=KAFFEINE_DOMAIN_MAX_HUNKS+1;source=reinterpret_cast<BPTR>(1);break;
            case 11:table.hrt_NumRelocs=KAFFEINE_DOMAIN_MAX_RELOCS+1;break;
            case 12:native.size(0,sizeof(ULONG)+sizeof(BPTR));break;
            case 13:metadata=nullptr;break;
        }
        auto records=native_blocks.size();
        check(!Kaffeine_DomainLoad(g.ctx,source,metadata,&out),"invalid/capped native image refused");
        check(out.entry==saved.entry&&out.seglist==saved.seglist&&out.module==saved.module,
              "failure leaves publication output unchanged");
        check(Kaffeine_HeapAvail(g.ctx,0)==g.available&&native_blocks.size()==records&&
              !g.ctx->domain_module_count&&!g.ctx->domain_modules&&flushes.empty(),
              "validation/cap failure changes no guest capacity, native ownership or code cache");
        g.unchanged();
    }
    check(native_blocks.empty(),"invalid metadata leaves no private allocations");

    {
        Guest first,second;NativeImage native;KaffeineDomainImage one{},two{};
        check(Kaffeine_DomainLoad(first.ctx,native.head(),&native.table,&one)&&
              Kaffeine_DomainLoad(second.ctx,native.head(),&native.table,&two),"separate arenas own independent imports");
        auto before=Kaffeine_HeapAvail(second.ctx,0);
        check(!Kaffeine_DomainUnload(second.ctx,one.module)&&IoErr()==ERROR_OBJECT_WRONG_TYPE&&
              Kaffeine_HeapAvail(second.ctx,0)==before&&second.ctx->domain_module_count==1,
              "foreign-context opaque record cannot release same-address guest blocks");
        check(Kaffeine_DomainUnload(first.ctx,one.module)&&Kaffeine_DomainUnload(second.ctx,two.module),
              "each private owner releases its own arena only");
    }

    bool reached_success=false;unsigned oom_cases=0;
    for(int failure=0;failure<20&&!reached_success;++failure)
    {
        Guest g;NativeImage native;KaffeineDomainImage out{};
        auto records=native_blocks.size();
        g.ctx->io_release=io_guard;deny_all=true;
        allocation_countdown=failure;
        bool loaded=Kaffeine_DomainLoad(g.ctx,native.head(),&native.table,&out);
        allocation_countdown=-1;deny_all=false;
        if(loaded)
        {reached_success=true;check(Kaffeine_DomainUnload(g.ctx,out.module),"successful allocation sweep endpoint unloads");}
        else
        {
            ++oom_cases;
            check(IoErr()==ERROR_NO_FREE_STORE&&!out.entry&&!out.seglist&&!out.module,
                  "OOM returns no partial image and deterministic error");
            check(!g.ctx->domain_module_count&&!g.ctx->domain_modules&&native_blocks.size()==records&&
                  Kaffeine_HeapAvail(g.ctx,0)==g.available,
                  "OOM at each native/guest allocation fully rolls back even with conservative reference guards");
            g.unchanged();
        }
    }
    check(reached_success&&oom_cases>=7,"sweep covers record, copy arrays and individual heap interval allocations");
    check(native_blocks.empty(),"OOM sweep leaves no native allocation");

    {
        Guest g;NativeImage native;KaffeineDomainImage image{};
        check(Kaffeine_DomainLoad(g.ctx,native.head(),&native.table,&image),"busy-unload image loaded");
        auto at=addresses(g,native,image.seglist);auto avail=Kaffeine_HeapAvail(g.ctx,0);
        auto cache=flushes.size();held_at=at[1]+4;g.ctx->io_release=io_guard;
        check(!Kaffeine_DomainUnload(g.ctx,image.module)&&IoErr()==ERROR_OBJECT_IN_USE&&
              Kaffeine_HeapAvail(g.ctx,0)==avail&&flushes.size()==cache&&g.ctx->domain_module_count==1,
              "a referenced member prevents any batch release or cache mutation");
        io_error=0x1234567890LL;Kaffeine_DomainCleanup(g.ctx);
        check(g.ctx->domain_module_count==1&&g.ctx->domain_modules_closing&&IoErr()==0x1234567890LL,
              "cleanup retains busy ownership and preserves IoErr");
        held_at=0;Kaffeine_DomainCleanup(g.ctx);
        check(!g.ctx->domain_module_count&&!g.ctx->domain_modules&&Kaffeine_HeapAvail(g.ctx,0)==g.available,
              "later owner cleanup releases deferred image after I/O retires");
        check(!Kaffeine_DomainLoad(g.ctx,native.head(),&native.table,&image)&&IoErr()==ERROR_OBJECT_WRONG_TYPE,
              "closed context rejects new imports");
    }
    {
        Guest g;NativeImage native;std::vector<KaffeineDomainImage> images;
        for(unsigned i=0;i<KAFFEINE_DOMAIN_MAX_MODULES;++i)
        {KaffeineDomainImage image{};check(Kaffeine_DomainLoad(g.ctx,native.head(),&native.table,&image),"live image slot available");images.push_back(image);}
        auto before=Kaffeine_HeapAvail(g.ctx,0);KaffeineDomainImage out{};
        check(!Kaffeine_DomainLoad(g.ctx,native.head(),&native.table,&out)&&IoErr()==ERROR_NO_FREE_STORE&&
              Kaffeine_HeapAvail(g.ctx,0)==before,"live module cap refuses before changing existing images");
        Kaffeine_DomainCleanup(g.ctx);check(!g.ctx->domain_module_count&&Kaffeine_HeapAvail(g.ctx,0)==g.available,
              "final cleanup releases all maximum live modules");
    }
    for(int point:{0,1,4})
    {
        Guest g;NativeImage native;KaffeineDomainImage out{};
        close_on_allocation=g.ctx;close_countdown=point;
        check(!Kaffeine_DomainLoad(g.ctx,native.head(),&native.table,&out),"reentrant closing prevents publication");
        close_on_allocation=nullptr;close_countdown=-1;
        check(!g.ctx->domain_module_count&&!g.ctx->domain_modules&&Kaffeine_HeapAvail(g.ctx,0)==g.available,
              "closing during native allocation rolls back pending ownership");
    }
    {
        auto guest=std::make_unique<Guest>();NativeImage native;KaffeineDomainImage image{};
        check(Kaffeine_DomainLoad(guest->ctx,native.head(),&native.table,&image),"final destroy fixture image loaded");
        guest->ctx->io_release=io_guard;deny_all=true;
        Kaffeine_DomainCleanup(guest->ctx);
        check(guest->ctx->domain_module_count==1,"conservative retired-queue guard defers ordinary cleanup");
        guest->ctx->cpu=nullptr;
        Kaffeine_DestroyContext(guest->ctx);
        guest->ctx=nullptr;guest.reset();
        deny_all=false;
        check(native_blocks.empty(),"whole-arena destruction also releases deferred native module records");
    }
    check(native_blocks.empty(),"all cases finish without leaked native ownership");
    std::printf("domain modules: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
