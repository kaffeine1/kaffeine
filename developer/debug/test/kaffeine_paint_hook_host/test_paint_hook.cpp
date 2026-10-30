/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Original synthetic hooks and native collaborators, without app contents. */
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <utility/hooks.h>
#include <graphics/layers.h>
#include <graphics/rastport.h>
#include <hidd/gfx.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/layers.h>
#include "kaffeine_intern.h"
#include "kaffeine_sched.h"
#include "kaffeine_callback.h"
#include "kaffeine_paint_hook.h"
#include "kaffeine_paint_memory.h"
#include "kaffeine_layers.h"
#include "kaffeine_messages.h"
#include "kaffeine_remember_memory.h"
#include "kaffeine_exec_io.h"
#include "kaffeine_cpu.h"

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while (0)
static Task *const driver = reinterpret_cast<Task *>(0x100000UL);
static Task *const foreign = reinterpret_cast<Task *>(0x200000UL);
static thread_local Task *native_task = driver;
static thread_local unsigned forbidden;
static std::recursive_mutex up;
static std::map<APTR, ULONG> allocations;
static std::map<APTR, std::recursive_mutex> locks;
static std::atomic<ULONG> native_signals;
static ULONG signal_alloc = 0xffff, guest_delivered;
static unsigned allocation_calls, fail_allocation, native_signal_calls, graphics_calls, stop_calls;
static bool no_signal, lock_busy, no_kernel;
static unsigned cpu_count=1;
static std::atomic<bool> pause_fill, fill_entered, finish_fill;
static bool reenter_fill;
static ULONG selected_task, live_task, allowed_signals, root_identity;
static unsigned delivered_calls, signal_freeable_calls, app_port_gone, app_signal_gone, guest_calls, can_signal_calls;
static ULONG register_a0, register_d0, guest_call_result;
static KaffeinePaintHooks **registered_state;
static void (*permit_hook)(void *);
static void *permit_data;
static BOOL signal_freeable(KaffeineContext *,LONG) { ++signal_freeable_calls; return TRUE; }
extern "C" void Kaffeine_AppPortGone(KaffeineContext *,ULONG) { ++app_port_gone; }
extern "C" void Kaffeine_AppSignalGone(KaffeineContext *,BYTE) { ++app_signal_gone; }
extern "C" ULONG Kaffeine_GetD(void *,int n) { assert(n==0); return register_d0; }
extern "C" ULONG Kaffeine_GetA(void *,int n) { assert(n==0); return register_a0; }
extern "C" BOOL Kaffeine_CallGuest(KaffeineContext *,void *,ULONG,KaffeineGuestCall *call)
{ ++guest_calls; call->d[0]=guest_call_result; return TRUE; }

struct Painted { RastPort rp; WORD x0,y0,x1,y1; };
static Painted painted[64];
static unsigned paints;
static Hook *reenter_hook;
static BackFillMessage *reenter_message;

extern "C" APTR OpenResource(CONST_STRPTR name)
{ assert(native_task==driver && !forbidden && !std::strcmp(name,"kernel.resource")); return no_kernel?nullptr:reinterpret_cast<APTR>(0x77770000); }
extern "C" unsigned int fixture_kernel_count(APTR kernel)
{ assert(native_task==driver && !forbidden && kernel==reinterpret_cast<APTR>(0x77770000)); return cpu_count; }
extern "C" APTR AllocMem(ULONG bytes, ULONG flags)
{
    assert(native_task == driver && !forbidden);
    if (++allocation_calls == fail_allocation) return nullptr;
    APTR p = (flags & MEMF_CLEAR) ? std::calloc(1, bytes) : std::malloc(bytes);
    if (p) assert(allocations.emplace(p,bytes).second);
    return p;
}
extern "C" void FreeMem(APTR p, ULONG bytes)
{
    assert(native_task == driver && !forbidden && p);
    auto at = allocations.find(p);
    assert(at != allocations.end() && at->second == bytes);
    allocations.erase(at); std::free(p);
}
extern "C" BYTE AllocSignal(LONG requested)
{
    assert(native_task == driver && !forbidden && requested == -1);
    if (no_signal) return -1;
    for (unsigned i=16; i<32; ++i)
        if (!(signal_alloc & (1u<<i))) { signal_alloc |= 1u<<i; return static_cast<BYTE>(i); }
    return -1;
}
extern "C" void FreeSignal(LONG bit)
{
    assert(native_task == driver && !forbidden && bit>=16 && bit<32 && (signal_alloc & (1u<<bit)));
    signal_alloc &= ~(1u<<bit);
}
extern "C" void Signal(Task *task, ULONG bits)
{
    assert(!forbidden && task == driver);
    ++native_signal_calls; native_signals.fetch_or(bits);
}
extern "C" Task *FindTask(CONST_STRPTR name) { assert(!name); return native_task; }
extern "C" ULONG SetSignal(ULONG bits, ULONG mask)
{
    assert(native_task == driver && !forbidden);
    ULONG old = native_signals.load();
    native_signals.store((old & ~mask) | (bits & mask)); return old;
}
extern "C" void Forbid() { up.lock(); ++forbidden; }
extern "C" void Permit() {
    assert(forbidden); --forbidden; up.unlock();
    if (!forbidden && permit_hook) {
        auto hook=permit_hook; permit_hook=nullptr; hook(permit_data);
    }
}
extern "C" ULONG AttemptSemaphore(SignalSemaphore *sem)
{
    assert(!forbidden && native_task == driver);
    if (lock_busy || !locks[sem].try_lock()) return FALSE;
    if (sem->ss_Owner==native_task) ++sem->ss_NestCount;
    else { sem->ss_Owner=native_task; sem->ss_NestCount=0; }
    return TRUE;
}
extern "C" void LockLayer(LONG, Layer *l) {
    assert(!forbidden); locks[&l->Lock].lock();
    if (l->Lock.ss_Owner==native_task) ++l->Lock.ss_NestCount;
    else { l->Lock.ss_Owner=native_task; l->Lock.ss_NestCount=0; }
}
extern "C" void UnlockLayer(Layer *l) {
    assert(!forbidden && l->Lock.ss_Owner==native_task && l->Lock.ss_NestCount>=0);
    if (--l->Lock.ss_NestCount<0) l->Lock.ss_Owner=nullptr;
    locks[&l->Lock].unlock();
}
extern "C" Hook *InstallLayerHook(Layer *l, Hook *h)
{
    assert(native_task == driver && !forbidden);
    LockLayer(0,l); Hook *old=l->BackFill; l->BackFill=h; UnlockLayer(l); return old;
}
extern "C" void SetAPen(RastPort *rp, ULONG pen)
{
    assert(!forbidden && !rp->Layer); ++graphics_calls;
    rp->FgPen=static_cast<BYTE>(pen); rp->linpatcnt=15; rp->Flags |= FRST_DOT;
}
extern "C" void SetDrMd(RastPort *rp, ULONG mode)
{ assert(!forbidden && !rp->Layer); ++graphics_calls; rp->DrawMode=static_cast<BYTE>(mode); }
static IPTR invoke(Hook *h, RastPort *rp, BackFillMessage *m)
{
    using Entry = IPTR (*)(Hook *,RastPort *,BackFillMessage *);
    return reinterpret_cast<Entry>(h->h_Entry)(h,rp,m);
}
extern "C" void RectFill(RastPort *rp, WORD x0, WORD y0, WORD x1, WORD y1)
{
    assert(!forbidden && !rp->Layer && paints < 64); ++graphics_calls;
    painted[paints++]={*rp,x0,y0,x1,y1};
    if (reenter_fill) { reenter_fill=false; invoke(reenter_hook,rp,reenter_message); }
    if (pause_fill) {
        fill_entered.store(true);
        while (!finish_fill.load()) std::this_thread::yield();
    }
}
extern "C" void Kaffeine_StopGuest(KaffeineContext *ctx, void *, LONG result)
{
    assert(native_task == driver && !forbidden); ++stop_calls;
    ctx->running=FALSE; ctx->callback_failed=TRUE; ctx->exit_code=result;
}
extern "C" BOOL Kaffeine_LockProtected(KaffeineContext *, ULONG, ULONG) { return FALSE; }
static ULONG caller(KaffeineContext *,void *) { return selected_task; }
static ULONG private_root(KaffeineContext *) { return root_identity; }
static BOOL can_signal(KaffeineContext *ctx,ULONG task,ULONG bits)
{ ++can_signal_calls; return ctx->running && bits && task==live_task && !(bits & ~allowed_signals); }
static void send_signal(KaffeineContext *,ULONG task,ULONG bits)
{ assert(native_task==driver && task==live_task && !(bits & ~allowed_signals)); guest_delivered|=bits; ++delivered_calls; }

enum { ARENA=131072, HOOK=0x1000, CODE=0x2000, DATA=0x8000, EXEC=0xc000, GFX=0xd000, OWNER=0x9000, LAYER=0xa000 };
struct Fixture {
    std::vector<UBYTE> bytes = std::vector<UBYTE>(ARENA);
    KaffeineContext ctx{};
    KaffeineTaskOps ops{};
    KaffeineLayerOps layer_ops{};
    KaffeineCPU cpu{};
    KaffeineCPUOps cpu_ops{};
    KaffeinePaintHooks *state{};
    Layer layer{};
    BitMap bitmap{};
    RastPort rp{};
    BackFillMessage message{};
    ULONG pc=CODE, code_base=CODE, data_base=DATA, code_end{}, mask=1u<<13;
    Fixture() {
        assert(allocations.empty()); allocation_calls=fail_allocation=0; no_signal=lock_busy=false;
        cpu_count=1; no_kernel=false; signal_alloc=0xffff|(1u<<20); native_signals=0; native_signal_calls=graphics_calls=paints=stop_calls=0;
        guest_delivered=delivered_calls=signal_freeable_calls=app_port_gone=app_signal_gone=guest_calls=can_signal_calls=0; pause_fill=false; fill_entered=false; finish_fill=false; reenter_fill=false;
        selected_task=live_task=root_identity=OWNER; allowed_signals=0xf000|(1u<<20);
        ctx.mem=bytes.data(); ctx.mem_size=ARENA; ctx.exec_task=driver; ctx.running=TRUE;
        cpu.ops=&cpu_ops; ctx.cpu=&cpu; ctx.guest_exec_task=OWNER;
        ctx.m68k_sysbase=EXEC; ctx.guest_signals=1u<<20;
        ctx.num_libmap=1; std::strcpy(ctx.libmap[0].name,"graphics.library"); ctx.libmap[0].m68k_base=GFX;
        ops.caller_task=caller; ops.root_task=private_root; ops.can_signal=can_signal; ops.send_signal=send_signal; ops.signal_freeable=signal_freeable;
        registered_state=&state;
        layer_ops.memory_releasable=[](KaffeineContext *,ULONG a,ULONG n) { return Kaffeine_PaintHookMemoryReleasable(*registered_state,a,n); };
        layer_ops.signal_releasable=[](KaffeineContext *,ULONG bits) { return Kaffeine_PaintHookSignalReleasable(*registered_state,bits); };
        layer_ops.signal_mask=[](KaffeineContext *) { return Kaffeine_PaintHookSignalMask(*registered_state); };
        layer_ops.poll=[](KaffeineContext *c) { return Kaffeine_PaintHookPoll(c,*registered_state); };
        ctx.layer_ops=&layer_ops;
        bitmap.pad=HIDD_BM_PAD_MAGIC; rp.Layer=&layer; rp.BitMap=&bitmap; rp.Mask=0x7f;
        rp.Font=reinterpret_cast<TextFont *>(0x55550000); rp.cp_x=13; rp.Flags=0x6000;
        message.Layer=&layer; message.Bounds={-7,-4,18,23}; message.OffsetX=3; message.OffsetY=-2;
        put32(4,EXEC); put32(HOOK+8,CODE); put32(HOOK+16,DATA);
        put32(DATA,0x11223307); put32(DATA+4,OWNER);
    }
    ~Fixture() {
        native_task=driver; fail_allocation=0; lock_busy=false; ctx.broker_call=FALSE; ctx.broker_owner=nullptr;
        Kaffeine_PaintHooksCleanup(&ctx,&state);
        CHECK(!state); CHECK(allocations.empty()); CHECK(signal_alloc==(0xffff|ctx.guest_signals));
    }
    void put16(ULONG a,UWORD n) { bytes[a]=n>>8; bytes[a+1]=n; }
    void put32(ULONG a,ULONG n) { put16(a,n>>16); put16(a+2,n); }
    void w(UWORD n) { put16(pc,n); pc+=2; }
    void l(ULONG n) { put32(pc,n); pc+=4; }
    void move(unsigned size,unsigned sm,unsigned sr,unsigned dm,unsigned dr) {
        w(((size==4?2:3)<<12)|(dr<<9)|(dm<<6)|(sm<<3)|sr);
    }
    void movea(unsigned sr,unsigned dr) { move(4,1,sr,1,dr); }
    void constant(ULONG value,unsigned mode,unsigned reg) { move(4,7,4,mode,reg); l(value); }
    void call(unsigned lvo) { w(0x4eae); w(-static_cast<int>(lvo)); }
    void program(bool signal=true) {
        pc=code_base; w(0x48e7); w(0xfffe); movea(1,5); w(0x4fef); w(-100);
        movea(2,3); movea(7,4); w(0x7e18); ULONG loop=pc;
        move(4,3,3,3,4); ULONG db=pc; w(0x51cf); w(loop-(db+2));
        movea(7,4); w(0x4294); constant(GFX,1,6); movea(4,1);
        move(4,7,1,0,0); l(data_base); call(342); w(0x7001); call(354); w(0x50ec); w(24);
        for (unsigned i=0;i<4;++i) { move(2,5,5,0,i); w(4+2*i); }
        call(306);
        if (signal) {
            move(4,7,1,0,0); l(data_base+4); ULONG branch=pc; w(0x6700); w(0);
            move(4,0,0,1,1); constant(mask,0,0); constant(EXEC,1,6); call(324);
            put16(branch+2,pc-(branch+2));
        }
        w(0x4fef); w(100); w(0x4cdf); w(0x7fff); w(0x4e75); code_end=pc;
    }
    BOOL install(ULONG hook=HOOK,ULONG *old=nullptr) {
        ULONG ignored=0xfeedface; return Kaffeine_PaintHookChange(&ctx,&state,&layer,LAYER,hook,old?old:&ignored);
    }
    void callback(bool on_foreign=true) {
        auto body=[&] {
            native_task=on_foreign?foreign:driver;
            LockLayer(0,&layer); Hook *h=layer.BackFill; assert(reinterpret_cast<IPTR>(h)>1);
            CHECK(invoke(h,&rp,&message)==0); UnlockLayer(&layer); native_task=driver;
        };
        if (on_foreign) { std::thread thread(body); thread.join(); } else body();
    }
    void init_heap() { ctx.heap_start=0x10000; ctx.heap_end=ARENA; CHECK(Kaffeine_HeapInit(&ctx)); }
    void destroy_heap() {
        CHECK(Kaffeine_PaintHooksCleanup(&ctx,&state));
        // Transfer private heap metadata to the public context destructor.
        // The fixture's byte vector itself is not a native OS allocation.
        auto *owned=static_cast<KaffeineContext *>(AllocMem(sizeof(ctx),MEMF_CLEAR));
        *owned=ctx; owned->mem=static_cast<UBYTE *>(AllocMem(ARENA+KAFFEINE_MEM_GUARD,MEMF_CLEAR));
        ctx.heap_allocations=nullptr; ctx.heap_first_free=nullptr;
        Kaffeine_DestroyContext(owned);
    }
    void scheduler() { ctx.sched=reinterpret_cast<KaffeineScheduler *>(1); ctx.task_ops=&ops; }
    void resume() { ctx.running=TRUE; ctx.callback_failed=FALSE; ctx.exit_code=0; }
};

static void positive_and_replacement() {
    Fixture f; f.program(); ULONG old=9;
    CHECK(f.install(HOOK,&old)); CHECK(old==0); CHECK(f.state && f.layer.BackFill);
    ULONG wake=Kaffeine_PaintHookSignalMask(f.state); CHECK(wake==(1u<<16));
    CHECK(Kaffeine_PaintHookGuest(f.state,&f.layer,f.layer.BackFill)==HOOK);
    CHECK(!Kaffeine_PaintHookMemoryReleasable(f.state,CODE,2));
    CHECK(!Kaffeine_PaintHookMemoryReleasable(f.state,HOOK+8,4));
    CHECK(!Kaffeine_PaintHookMemoryReleasable(f.state,DATA+3,1));
    CHECK(Kaffeine_PaintHookMemoryReleasable(f.state,DATA+24,4));
    CHECK(Kaffeine_PaintHookSignalReleasable(f.state,1u<<20));
    RastPort original=f.rp; f.callback(); CHECK(paints==1 && graphics_calls==3);
    CHECK(painted[0].rp.Font==original.Font && painted[0].rp.BitMap==original.BitMap);
    CHECK(painted[0].rp.Layer==nullptr && painted[0].rp.FgPen==7 && painted[0].rp.Mask==255);
    CHECK(painted[0].rp.linpatcnt==15 && (painted[0].rp.Flags&FRST_DOT));
    CHECK(painted[0].x0==-7 && painted[0].y0==-4 && painted[0].x1==18 && painted[0].y1==23);
    CHECK(!std::memcmp(&f.rp,&original,sizeof(original)));
    CHECK((native_signals.load()&wake) && !(native_signals.load()&(1u<<13)));
    f.bytes[DATA+3]=112;
    f.callback(); CHECK(paints==2 && painted[1].rp.FgPen==112);
    CHECK(f.install(HOOK,&old)); CHECK(old==HOOK); // emitted old Signal survives replacement
    CHECK(native_signals.load()&(1u<<13));
    CHECK(Kaffeine_PaintHookPoll(&f.ctx,f.state));
    CHECK(f.install(1,&old)); CHECK(old==HOOK && f.layer.BackFill==reinterpret_cast<Hook *>(1));
    CHECK(Kaffeine_PaintHookMemoryReleasable(f.state,CODE,2));
    CHECK(f.install(HOOK,&old)); CHECK(old==1);
    CHECK(Kaffeine_PaintHookRetireLayer(&f.ctx,f.state,&f.layer));
    CHECK(f.layer.BackFill==reinterpret_cast<Hook *>(1)); // baseline restored exactly
}

static void rejected_snapshots() {
    for (unsigned variant=0;variant<8;++variant) {
        Fixture f; f.program(); CHECK(f.install());
        switch (variant) {
        case 0:f.bytes[CODE]^=1;break;
        case 1:f.put32(HOOK+16,DATA+32);break;
        case 2:f.put32(DATA+4,OWNER+2);break;
        case 3:f.bitmap.pad=0;break;
        case 4:f.rp.AreaPtrn=reinterpret_cast<UWORD *>(0x1230);break;
        case 5:f.message.Layer=nullptr;break;
        case 6:f.message.Bounds.MinX=25;break;
        case 7:f.put32(HOOK+8,CODE+2);break;
        }
        f.callback(); CHECK(paints==0 && graphics_calls==0 && stop_calls==0);
        CHECK(native_signals.load()&Kaffeine_PaintHookSignalMask(f.state));
        CHECK(!Kaffeine_PaintHookPoll(&f.ctx,f.state));
        CHECK(!f.ctx.running && f.ctx.exit_code==20 && stop_calls==1);
    }
    {
        Fixture f; f.program(); CHECK(f.install());
        native_task=foreign; invoke(f.layer.BackFill,&f.rp,&f.message); native_task=driver;
        CHECK(paints==0 && graphics_calls==0 && native_signals.load()==0);
        CHECK(Kaffeine_PaintHookPoll(&f.ctx,f.state) && f.ctx.running);
    }
    Fixture f; f.program(); CHECK(f.install()); f.put32(DATA+4,0); f.callback();
    CHECK(paints==1 && native_signals.load()==0); CHECK(Kaffeine_PaintHookPoll(&f.ctx,f.state));
}

static void fail_before_swap() {
    for (unsigned nth=1;nth<=7;++nth) {
        Fixture f; f.program(); fail_allocation=nth; CHECK(!f.install());
        CHECK(!f.state && f.layer.BackFill==nullptr && !f.ctx.running);
        CHECK(allocations.empty() && signal_alloc==(0xffff|f.ctx.guest_signals));
    }
    for (unsigned variant=0;variant<7;++variant) {
        Fixture f; f.program(); ULONG old=0xabcdef;
        if (variant==0) no_signal=true;
        if (variant==1) lock_busy=true;
        if (variant==2) f.layer.BackFill=reinterpret_cast<Hook *>(0x77770000);
        if (variant==3) f.ctx.broker_call=TRUE;
        if (variant==4) no_kernel=true;
        if (variant==5) cpu_count=0;
        if (variant==6) cpu_count=2;
        Hook *original=f.layer.BackFill;
        CHECK(!f.install(HOOK,&old)); CHECK(old==0xabcdef && f.layer.BackFill==original);
        CHECK(!f.state && allocations.empty() && signal_alloc==(0xffff|f.ctx.guest_signals));
    }
    Fixture f; f.program(); CHECK(f.install()); Hook *original=f.layer.BackFill;
    fail_allocation=allocation_calls+1; ULONG old=0xabcdef;
    CHECK(!f.install(HOOK,&old)); CHECK(old==0xabcdef && f.layer.BackFill==original);
    CHECK(!Kaffeine_PaintHookMemoryReleasable(f.state,CODE,2));
}

static void task_retirement_and_signal_lease() {
    Fixture f; f.scheduler(); f.mask=1u<<20; f.program(); CHECK(f.install());
    CHECK(!Kaffeine_PaintHookSignalReleasable(f.state,1u<<20));
    CHECK(Kaffeine_PaintHookSignalReleasable(f.state,1u<<21));
    f.callback(); CHECK(guest_delivered==0 && paints==1);
    CHECK(Kaffeine_PaintHookPoll(&f.ctx,f.state)); CHECK(guest_delivered==(1u<<20) && delivered_calls==1);
    f.callback(); CHECK(Kaffeine_PaintHookRetireTask(&f.ctx,f.state,OWNER));
    CHECK(f.layer.BackFill==nullptr && delivered_calls==1); // ending owner discards stale pending
    CHECK(Kaffeine_PaintHookSignalReleasable(f.state,1u<<20));
    CHECK(Kaffeine_PaintHookMemoryReleasable(f.state,CODE,2));
    live_task=selected_task=OWNER; f.program(); CHECK(f.install());
    f.callback(); CHECK(Kaffeine_PaintHookPoll(&f.ctx,f.state)); CHECK(delivered_calls==2);
    CHECK(Kaffeine_PaintHookRetireTask(&f.ctx,f.state,OWNER));
    live_task=0; f.resume(); CHECK(!f.install()); CHECK(f.layer.BackFill==nullptr);
}

static void scheduled_paint_without_signal() {
    Fixture f; f.scheduler(); f.program(false);
    BOOL installed=f.install(); CHECK(installed);
    CHECK(can_signal_calls==0); // The real scheduler rejects a zero mask.
    if (installed) {
        f.callback(); CHECK(paints==1 && graphics_calls==3);
        CHECK(!native_signals.load() && !guest_delivered && !delivered_calls);
        CHECK(Kaffeine_PaintHookPoll(&f.ctx,f.state) && f.ctx.running);
        CHECK(can_signal_calls==0);
        CHECK(Kaffeine_PaintHookRetireTask(&f.ctx,f.state,OWNER));
        CHECK(!f.layer.BackFill);
    }
}

static void scheduler_to_root_transition() {
    {
        Fixture f; f.scheduler(); f.program(); CHECK(f.install());
        f.callback(); CHECK(!guest_delivered && !(native_signals.load()&(1u<<13)));
        // The root remains live after the last child and scheduler retire.
        f.ctx.sched=nullptr; f.ctx.guest_exec_task=OWNER;
        CHECK(Kaffeine_PaintHookPoll(&f.ctx,f.state));
        CHECK(native_signals.load()&(1u<<13));
        CHECK(!delivered_calls && f.ctx.running);
    }
    {
        Fixture f; f.scheduler();
        root_identity=0; // Root already retired; a child can reuse its numeric TCB.
        selected_task=live_task=OWNER; f.ctx.guest_exec_task=OWNER;
        f.program(); CHECK(f.install()); f.callback();
        CHECK(!(native_signals.load()&(1u<<13)) && !guest_delivered);
        f.ctx.sched=nullptr;
        CHECK(!Kaffeine_PaintHookPoll(&f.ctx,f.state));
        CHECK(!(native_signals.load()&(1u<<13)) && !delivered_calls && f.ctx.exit_code==20);
    }
    {
        Fixture f; f.scheduler(); selected_task=live_task=OWNER+0x200;
        f.ctx.guest_exec_task=selected_task; f.put32(DATA+4,selected_task);
        f.program(); CHECK(f.install());
        // Root retirement does not retire a live child's installed Hook.
        CHECK(Kaffeine_PaintHookRetireTask(&f.ctx,f.state,OWNER));
        f.callback(); f.ctx.sched=nullptr;
        CHECK(!Kaffeine_PaintHookPoll(&f.ctx,f.state));
        CHECK(!(native_signals.load()&(1u<<13)) && !delivered_calls && f.ctx.exit_code==20);
    }
}

static void wrong_owner_and_recursion() {
    Fixture f; f.program(); CHECK(f.install());
    CHECK(Kaffeine_PaintHookPoll(nullptr,nullptr));
    // Polling a real installed state on another native owner must never deliver a guest event.
    f.callback(); unsigned old=delivered_calls; f.ctx.broker_call=TRUE;
    CHECK(!Kaffeine_PaintHookPoll(&f.ctx,f.state)); CHECK(delivered_calls==old);
    f.ctx.broker_call=FALSE; f.resume();
    CHECK(Kaffeine_PaintHookRetireLayer(&f.ctx,f.state,&f.layer));
    CHECK(f.install()); reenter_fill=true; reenter_hook=f.layer.BackFill; reenter_message=&f.message;
    f.callback(); CHECK(paints==2); CHECK(!Kaffeine_PaintHookPoll(&f.ctx,f.state));
    CHECK(f.ctx.exit_code==20);
}

static void native_quiescence() {
    Fixture f; f.program(); CHECK(f.install()); pause_fill=true;
    std::thread callback([&] { native_task=foreign; LockLayer(0,&f.layer);
        invoke(f.layer.BackFill,&f.rp,&f.message); UnlockLayer(&f.layer); native_task=driver; });
    while (!fill_entered.load()) std::this_thread::yield();
    std::atomic<bool> release_requested=false;
    std::thread release([&] { while (!release_requested.load()) std::this_thread::yield(); finish_fill=true; });
    release_requested=true;
    CHECK(Kaffeine_PaintHookRetireLayer(&f.ctx,f.state,&f.layer));
    callback.join(); release.join(); pause_fill=false;
    CHECK(paints==1 && f.layer.BackFill==nullptr);
    CHECK(native_signals.load()&(1u<<13));
}

/* Model a UP preemption exactly when an unowned callback permits the
 * driver again. The driver retires its binding and deletes the native Layer;
 * ASan verifies the rejected entry touches neither object on return. */
static void unlocked_entry_retirement() {
    Fixture f; f.program(false); auto *layer=new Layer{}; ULONG old;
    CHECK(Kaffeine_PaintHookChange(&f.ctx,&f.state,layer,LAYER,HOOK,&old));
    struct Retirement { Fixture *f; Layer *layer; bool retired; } retirement{&f,layer,false};
    permit_data=&retirement;
    permit_hook=[](void *opaque) {
        auto *r=static_cast<Retirement *>(opaque); native_task=driver;
        r->retired=Kaffeine_PaintHookRetireLayer(&r->f->ctx,r->f->state,r->layer);
        CHECK(r->retired && !r->layer->BackFill);
        if (r->retired) { delete r->layer; r->layer=nullptr; }
        native_task=foreign;
    };
    native_task=foreign;
    invoke(layer->BackFill,&f.rp,&f.message);
    native_task=driver;
    CHECK(retirement.retired && !retirement.layer);
    CHECK(!paints && !graphics_calls && !native_signals.load() && f.ctx.running);
    CHECK(Kaffeine_PaintHookMemoryReleasable(f.state,CODE,2));
    permit_hook=nullptr; permit_data=nullptr;
    if (!retirement.retired) { Kaffeine_PaintHookRetireLayer(&f.ctx,f.state,layer); delete layer; }
}

static void independent_layers_and_capacity() {
    Fixture f; f.program(false);
    Layer layers[17]{};
    ULONG old=9;
    layers[1].BackFill=reinterpret_cast<Hook *>(1);
    CHECK(Kaffeine_PaintHookChange(&f.ctx,&f.state,&layers[0],LAYER,HOOK,&old) && old==0);
    Hook *first=layers[0].BackFill;
    CHECK(Kaffeine_PaintHookChange(&f.ctx,&f.state,&layers[1],LAYER+0x200,HOOK,&old) && old==1);
    CHECK(layers[0].BackFill!=layers[1].BackFill);
    CHECK(Kaffeine_PaintHookGuest(f.state,&layers[1],first)==0);
    CHECK(Kaffeine_PaintHookGuest(f.state,&layers[1],layers[1].BackFill)==HOOK);
    CHECK(Kaffeine_PaintHookRetireLayer(&f.ctx,f.state,&layers[0]));
    CHECK(!layers[0].BackFill && Kaffeine_PaintHookHasLayer(f.state,&layers[1]));
    CHECK(!Kaffeine_PaintHookMemoryReleasable(f.state,CODE,2));
    for (unsigned i=0;i<17;++i) {
        if (i==1) continue;
        old=0xfeed;
        BOOL accepted=Kaffeine_PaintHookChange(&f.ctx,&f.state,&layers[i],LAYER+0x200*i,HOOK,&old);
        CHECK(accepted==(i<16));
        CHECK(accepted?old==0:old==0xfeed);
    }
    CHECK(!layers[16].BackFill && !f.ctx.running);
    CHECK(Kaffeine_PaintHooksCleanup(&f.ctx,&f.state));
    for (unsigned i=0;i<17;++i) CHECK(layers[i].BackFill==(i==1?reinterpret_cast<Hook *>(1):nullptr));
    CHECK(!f.state && native_signals.load()==0);
}

static void heap_and_multi_object_leases() {
    {
        Fixture f; f.init_heap();
        ULONG code=Kaffeine_HeapAlloc(&f.ctx,512,MEMF_CLEAR);
        ULONG data=Kaffeine_HeapAlloc(&f.ctx,64,MEMF_CLEAR);
        ULONG hook=Kaffeine_HeapAlloc(&f.ctx,20,MEMF_CLEAR);
        ULONG other=Kaffeine_HeapAlloc(&f.ctx,64,MEMF_CLEAR);
        CHECK(code && data && hook && other);
        f.code_base=code; f.data_base=data; f.program();
        f.put32(hook+8,code); f.put32(hook+16,data); f.put32(data,7); f.put32(data+4,OWNER);
        CHECK(f.install(hook)); f.callback(); CHECK(paints==1);
        auto before=f.bytes;
        ULONG available=Kaffeine_HeapAvail(&f.ctx,MEMF_PUBLIC);
        CHECK(!Kaffeine_HeapFreeExact(&f.ctx,code,512));
        CHECK(!Kaffeine_HeapFreeExact(&f.ctx,data,64));
        CHECK(!Kaffeine_HeapFreeExact(&f.ctx,hook,20));
        CHECK(!Kaffeine_ReleasableMemory(&f.ctx,data,64));
        CHECK(!Kaffeine_HeapPaintReleasable(&f.ctx,data+7));
        CHECK(Kaffeine_HeapPaintReleasable(&f.ctx,other+7));
        CHECK(before==f.bytes && available==Kaffeine_HeapAvail(&f.ctx,MEMF_PUBLIC));
        CHECK(Kaffeine_HeapFreeExact(&f.ctx,other,64));
        CHECK(Kaffeine_PaintHookRetireLayer(&f.ctx,f.state,&f.layer));
        CHECK(Kaffeine_HeapFreeExact(&f.ctx,code,512));
        CHECK(Kaffeine_HeapFreeExact(&f.ctx,data,64));
        CHECK(Kaffeine_HeapFreeExact(&f.ctx,hook,20));
        f.destroy_heap();
    }
    {
        Fixture f; f.init_heap();
        ULONG pool=Kaffeine_PoolCreate(&f.ctx,MEMF_CLEAR,1024,128);
        ULONG data=Kaffeine_PoolAlloc(&f.ctx,pool,64);
        ULONG other=Kaffeine_PoolAlloc(&f.ctx,pool,64);
        CHECK(pool && data && other);
        f.data_base=data; f.program(); f.put32(HOOK+16,data); f.put32(data,7); f.put32(data+4,OWNER);
        CHECK(f.install()); auto before=f.bytes;
        Kaffeine_PoolDelete(&f.ctx,pool);
        CHECK(before==f.bytes);
        CHECK(Kaffeine_HeapGuestRange(&f.ctx,other,64) && Kaffeine_HeapGuestRange(&f.ctx,data,64));
        CHECK(Kaffeine_PaintHookRetireLayer(&f.ctx,f.state,&f.layer));
        Kaffeine_PoolDelete(&f.ctx,pool);
        CHECK(!Kaffeine_HeapGuestRange(&f.ctx,other,64) && !Kaffeine_HeapGuestRange(&f.ctx,data,64));
        f.destroy_heap();
    }
    {
        Fixture f; f.init_heap();
        const ULONG spec[]={MEMF_PUBLIC|MEMF_CLEAR,64,MEMF_PUBLIC|MEMF_CLEAR,64};
        ULONG list=Kaffeine_HeapAllocList(&f.ctx,spec,2);
        CHECK(list && !(list&0x80000000u));
        ULONG data=m68k_read32(&f.ctx,list+16), other=m68k_read32(&f.ctx,list+24);
        CHECK(data && other);
        f.data_base=data; f.program(); f.put32(HOOK+16,data); f.put32(data,7); f.put32(data+4,OWNER);
        CHECK(f.install()); auto before=f.bytes;
        Kaffeine_HeapFreeEntry(&f.ctx,list); CHECK(before==f.bytes);
        CHECK(Kaffeine_HeapGuestRange(&f.ctx,data,64) && Kaffeine_HeapGuestRange(&f.ctx,other,64));
        CHECK(Kaffeine_PaintHookRetireLayer(&f.ctx,f.state,&f.layer));
        Kaffeine_HeapFreeEntry(&f.ctx,list);
        CHECK(!Kaffeine_HeapGuestRange(&f.ctx,data,64) && !Kaffeine_HeapGuestRange(&f.ctx,other,64));
        f.destroy_heap();
    }
}

static void signal_and_port_atomicity() {
    {
        Fixture f; f.scheduler(); f.mask=1u<<20; f.program(); CHECK(f.install());
        register_d0=20; ULONG signals=f.ctx.guest_signals;
        auto before=f.bytes;
        CHECK(Kaffeine_FreeSignal(&f.ctx,&f.cpu)==20);
        CHECK(!signal_freeable_calls && !app_signal_gone && f.ctx.guest_signals==signals);
        CHECK(before==f.bytes && (signal_alloc&(1u<<20)));
        CHECK(Kaffeine_PaintHookRetireLayer(&f.ctx,f.state,&f.layer)); f.resume();
        CHECK(Kaffeine_FreeSignal(&f.ctx,&f.cpu)==0);
        CHECK(signal_freeable_calls==1 && app_signal_gone==1 && !(signal_alloc&(1u<<20)));
    }
    {
        Fixture f; f.init_heap(); f.scheduler();
        ULONG port=Kaffeine_HeapAlloc(&f.ctx,34,MEMF_CLEAR); CHECK(port);
        f.ctx.io_ports[0].guest=port; f.ctx.io_ports[0].sigbit=20;
        f.bytes[port+8]=4; f.bytes[port+15]=20;
        f.put32(port+16,OWNER); f.put32(port+20,port+24); f.put32(port+28,port+20);
        f.data_base=port+8; f.program(false); f.put32(HOOK+16,port+8);
        CHECK(f.install()); register_a0=port;
        auto before=f.bytes;
        CHECK(Kaffeine_DeleteMsgPort(&f.ctx,&f.cpu)==20);
        CHECK(before==f.bytes && f.ctx.io_ports[0].guest==port);
        CHECK(!signal_freeable_calls && !app_port_gone && !app_signal_gone && (signal_alloc&(1u<<20)));
        CHECK(Kaffeine_PaintHookRetireLayer(&f.ctx,f.state,&f.layer)); f.resume();
        CHECK(Kaffeine_DeleteMsgPort(&f.ctx,&f.cpu)==0);
        CHECK(!f.ctx.io_ports[0].guest && signal_freeable_calls==1 && app_port_gone==1 && app_signal_gone==1);
        CHECK(!Kaffeine_HeapGuestRange(&f.ctx,port,34));
        f.destroy_heap();
    }
}

static void library_close_before_guest_code() {
    for (unsigned which=0;which<2;++which) {
        Fixture f; f.init_heap();
        ULONG first=Kaffeine_HeapAlloc(&f.ctx,64,MEMF_CLEAR);
        ULONG second=Kaffeine_HeapAlloc(&f.ctx,512,MEMF_CLEAR);
        ULONG library=Kaffeine_HeapAlloc(&f.ctx,64,MEMF_CLEAR);
        CHECK(first && second && library);
        ULONG seg=(first+4)>>2;
        f.put32(first,64); f.put32(first+4,(second+4)>>2);
        f.put32(second,512); f.put32(second+4,0);
        f.ctx.num_m68klibs=1; f.ctx.m68klibs[0].base=library+16; f.ctx.m68klibs[0].seglist=seg;
        std::strcpy(f.ctx.m68klibs[0].name,"synthetic.library");
        if (!which) f.code_base=second+8;
        else f.data_base=library+24;
        f.program(); f.put32(HOOK+8,f.code_base); f.put32(HOOK+16,f.data_base);
        f.put32(f.data_base,7); f.put32(f.data_base+4,OWNER);
        CHECK(f.install()); guest_call_result=seg; auto before=f.bytes;
        CHECK(Kaffeine_CloseM68KLibrary(&f.ctx,&f.cpu,library+16));
        CHECK(!guest_calls && !f.ctx.running && f.ctx.exit_code==20);
        CHECK(before==f.bytes && f.ctx.num_m68klibs==1 && f.ctx.m68klibs[0].base==library+16);
        CHECK(Kaffeine_HeapGuestRange(&f.ctx,first,64) && Kaffeine_HeapGuestRange(&f.ctx,second,512));
        CHECK(Kaffeine_PaintHookRetireLayer(&f.ctx,f.state,&f.layer)); f.resume();
        CHECK(Kaffeine_CloseM68KLibrary(&f.ctx,&f.cpu,library+16));
        CHECK(guest_calls==1 && !f.ctx.num_m68klibs && !f.ctx.m68klibs[0].base);
        CHECK(!Kaffeine_HeapGuestRange(&f.ctx,first,64) && !Kaffeine_HeapGuestRange(&f.ctx,second,512));
        CHECK(Kaffeine_HeapFreeExact(&f.ctx,library,64)); f.destroy_heap();
    }
}

int main() {
    positive_and_replacement(); rejected_snapshots(); fail_before_swap();
    task_retirement_and_signal_lease(); scheduled_paint_without_signal(); scheduler_to_root_transition(); wrong_owner_and_recursion(); native_quiescence();
    unlocked_entry_retirement(); independent_layers_and_capacity();
    heap_and_multi_object_leases(); signal_and_port_atomicity(); library_close_before_guest_code();
    std::printf("native paint-hook bridge: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
