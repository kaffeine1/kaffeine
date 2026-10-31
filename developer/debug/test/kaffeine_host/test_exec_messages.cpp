/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#include "message_fixture.h"
#include "kaffeine_sched.h"
static void be32(std::vector<UBYTE> &b,ULONG a,ULONG v)
{ for(unsigned i=0;i<4;++i)b[a+i]=static_cast<UBYTE>(v>>(24-8*i)); }
static void fifo()
{
    Fixture f;
    ULONG p=f.exec(666),rp=f.exec(666);
    check(p && rp && p!=rp,"CreateMsgPort distinct guest identities");
    check(f.ctx.mem[p+8]==4 && f.ctx.mem[p+14]==0 && f.r(p+16)==f.task,"owned port type, signal action and task");
    check(f.ctx.mem[p+15]<32 && f.ctx.mem[rp+15]<32 && f.ctx.mem[p+15]!=f.ctx.mem[rp+15],"owned signal bits distinct");
    pending_signals=1U<<31;
    f.message(f.msg,rp,44);f.message(f.msg+0x100,rp,36);
    auto expected=f.bytes();
    be32(expected,f.msg,p+24);be32(expected,f.msg+4,p+20);expected[f.msg+8]=5;
    be32(expected,p+20,f.msg);be32(expected,p+28,f.msg);
    check(f.exec(366,p,f.msg)==0xD0000000 && f.bytes()==expected,"PutMsg exact BE links/type, payload and guards preserved");
    check(signals==1 && signalled_mask==(1U<<f.ctx.mem[p+15]) && (pending_signals&(1U<<31)),"enqueue signals owned task and preserves unrelated bits");
    be32(expected,f.msg+0x100,p+24);be32(expected,f.msg+0x104,f.msg);expected[f.msg+0x108]=5;
    be32(expected,f.msg,f.msg+0x100);be32(expected,p+28,f.msg+0x100);
    f.exec(366,p,f.msg+0x100);
    check(f.bytes()==expected && signals==2,"second enqueue is FIFO and signals again");
    auto before=f.bytes();ULONG pending=pending_signals;
    check(f.exec(384,p)==f.msg && f.bytes()==before && pending_signals==pending,"nonempty WaitPort peeks and retains signal");
    check(f.exec(372,p)==f.msg && f.ctx.mem[f.msg+8]==5 && pending_signals==pending,"GetMsg retains type and signal");
    check(f.exec(378,0,f.msg)==0 && f.ctx.mem[f.msg+8]==7 && f.r(rp+20)==f.msg,"ReplyMsg enqueues type7 at reply port");
    check(f.exec(372,rp)==f.msg && f.ctx.mem[f.msg+8]==7,"GetMsg retains reply type");
    check(f.exec(372,p)==f.msg+0x100 && f.r(p+20)==p+24 && f.r(p+28)==p+20,"FIFO drains to exact empty sentinels");
    check(f.exec(372,p)==0,"empty GetMsg returns NULL");
    f.exec(366,p,f.msg);check(f.r(p+20)==f.msg && f.ctx.mem[f.msg+8]==5,"dequeued message can be reused with stale links");
    f.exec(372,p);f.exec(672,p);f.exec(672,rp);
    check(freed_signals==2 && !f.ctx.guest_signals,"DeleteMsgPort releases owned signals");
}
static void ignore_and_null_reply()
{
    Fixture f;f.message(f.msg,0,36);
    auto expected=f.bytes();be32(expected,f.msg,f.port+24);be32(expected,f.msg+4,f.port+20);
    be32(expected,f.port+20,f.msg);be32(expected,f.port+28,f.msg);expected[f.msg+8]=5;
    check(f.exec(366,f.port,f.msg)==0xD0000000 && f.bytes()==expected && !signals,"manual PA_IGNORE needs no native task and never signals");
    f.exec(372,f.port);expected=f.bytes();expected[f.msg+8]=6;
    check(f.exec(378,0,f.msg)==0 && f.bytes()==expected && !signals,"NULL ReplyMsg changes only type to measured NT_FREEMSG");
    auto before=f.bytes();check(f.exec(384,f.port)==20 && !f.ctx.running && f.bytes()==before,"empty ordinary WaitPort refuses without mutation or waiting");
}

static void retired_port_signal()
{
    Fixture f;
    ULONG old=f.exec(666);
    BYTE bit=(BYTE)f.ctx.mem[old+15];
    for(auto &entry:f.ctx.io_ports)
        if(entry.guest==old) entry.sigbit=-1;
    f.ctx.guest_signals &= ~(1UL<<bit);
    FreeSignal(bit);
    ULONG replacement=f.exec(666);
    check(replacement && f.ctx.mem[replacement+15]==(UBYTE)bit,
          "a retired task's port bit can be reused by a new port");
    unsigned before=freed_signals;
    f.exec(672,old);
    check(freed_signals==before && (allocated_signals&(1UL<<bit)) &&
          (f.ctx.guest_signals&(1UL<<bit)),
          "deleting a retired port cannot free its replacement's signal");
    f.exec(672,replacement);
    check(!allocated_signals && !f.ctx.guest_signals,
          "replacement port still releases its own signal exactly once");
}
static void invalid()
{
    for(ULONG bad:{0U,1U,0x200001U,0xFFFFFFFFU,0x1FFFF0U})
    { Fixture f;f.rejected(bad,f.msg); }
    for(ULONG bad:{0U,1U,0x200001U,0xFFFFFFFFU,0x1FFFF0U})
    { Fixture f;f.rejected(f.port,bad); }
    for(unsigned which=0;which<16;++which)
    {
        Fixture f;
        switch(which)
        {
            case 0:f.ctx.mem[f.port+14]=1;break;
            case 1:f.ctx.mem[f.port+14]=3;break;
            case 2:f.ctx.mem[f.port+14]=0x80;break;
            case 3:f.ctx.mem[f.port+8]=0;break;
            case 4:f.w(f.port+24,0x123);break;
            case 5:f.w(f.port+20,0);break;
            case 6:f.w(f.port+28,f.msg);break;
            case 7:m68k_write16(&f.ctx,f.msg+18,19);break;
            case 8:break;
            case 9:f.ctx.mem[f.port+14]=0;f.ctx.mem[f.port+15]=32;break;
            case 10:f.ctx.mem[f.port+14]=0;f.ctx.mem[f.port+15]=2;f.w(f.port+16,f.task);break;
            case 11:f.ctx.mem[f.port+14]=0;f.ctx.guest_signals=4;f.ctx.mem[f.port+15]=2;f.w(f.port+16,f.task+2);break;
            case 12:f.w(f.port+16,f.task+2);break;
            case 13:f.ctx.num_shadows=1;f.ctx.shadow_map[0]={f.msg+30,reinterpret_cast<void *>(1),8, 0 };break;
            case 14:f.ctx.num_shadows=1;f.ctx.shadow_map[0]={f.port+2,reinterpret_cast<void *>(1),4, 0 };break;
            case 15:f.w(f.port+20,f.port+24+2);break;
        }
        if(which==8){f.message(f.ctx.mem_size-32,0,20);m68k_write16(&f.ctx,f.ctx.mem_size-14,68);f.rejected(f.port,f.ctx.mem_size-32);}
        else f.rejected();
    }
    { Fixture f;f.message(f.port,0,34);f.rejected(f.port,f.port); }
    { Fixture f;f.exec(366,f.port,f.msg);f.rejected();f.rejected(f.reply,f.msg); }
    for(unsigned which=0;which<8;++which)
    {
        Fixture f;f.message(f.msg+0x100);f.exec(366,f.port,f.msg);f.exec(366,f.port,f.msg+0x100);
        switch(which)
        {
            case 0:f.w(f.msg,f.msg);break;
            case 1:f.w(f.msg+0x100,f.msg);break;
            case 2:f.w(f.msg+0x104,f.port+20);break;
            case 3:f.w(f.msg,0xFFFFFFFF);break;
            case 4:f.w(f.msg,f.port+20);break;
            case 5:f.w(f.port+28,f.msg);break;
            case 6:f.ctx.mem[f.msg+0x108]=0;break;
            case 7:m68k_write16(&f.ctx,f.msg+18,0x200);break;
        }
        f.rejected(f.port,f.msg+0x200);
        auto before=f.bytes();check(f.exec(372,f.port)==20 && f.bytes()==before,"GetMsg validates entire malformed queue before removal");
    }
    { Fixture f;f.message(f.msg,f.port);f.exec(366,f.port,f.msg);auto b=f.bytes();check(f.exec(378,0,f.msg)==20 && f.bytes()==b,"ReplyMsg refuses a message still queued"); }
    { Fixture a;Fixture b;a.exec(366,a.port,a.msg);check(b.exec(372,b.port)==0,"queue state is isolated by context"); }
}
static void allocation_failures()
{
    Fixture f;auto before=f.bytes();allocation_countdown=0;
    check(f.exec(666)==0 && f.bytes()==before && !f.ctx.guest_signals,"CreateMsgPort heap-record allocation failure is atomic");
    allocation_countdown=-1;signal_failure=true;
    check(f.exec(666)==0 && !f.ctx.guest_signals,"CreateMsgPort signal allocation failure releases guest storage");
    signal_failure=false;
    std::vector<ULONG> ports;
    for(unsigned i=0;i<16;++i) ports.push_back(f.exec(666));
    check(f.exec(666)==0,"owned port table is bounded");
    for(ULONG p:ports)f.exec(672,p);
}

static void real_shadows_and_port_lifetimes()
{
    Fixture f;
    // Generated ExecBase and current Task shadows are trusted identities,
    // while queue/message/buffer storage must remain ordinary guest bytes.
    f.initialize_shadows();
    ULONG p=f.exec(666);f.message(f.msg);
    check(f.exec(366,p,f.msg)==0xD0000000 && signals==1,"PA_SIGNAL works with actual ExecBase/Task shadow registrations");
    check(f.exec(372,p)==f.msg,"GetMsg works with registered current Task");
    f.exec(672,p);
    for(unsigned i=0;i<160;++i)
    {
        ULONG local=0xB000+i*40;
        f.manual_port(local);f.message(f.msg);
        check(f.exec(366,local,f.msg)==0xD0000000 && f.exec(372,local)==f.msg,"sequential manual ports do not consume cumulative capacity");
        std::memset(f.ctx.mem+local,0xD7,34);
    }
    check(std::all_of(std::begin(f.ctx.message_ports),std::end(f.ctx.message_ports),[](ULONG v){return v==0;}),"drained manual ports are no longer retained");
    f.manual_port(f.port);f.exec(366,f.port,f.msg);
    f.w(f.msg,f.msg);
    f.message(f.msg+0x100);f.rejected(f.reply,f.msg+0x100);
}

static void queue_capacity_and_membership()
{
    {
        Fixture f;f.exec(366,f.port,f.msg);
        for(unsigned i=0;i<160;++i)check(f.exec(384,f.port)==f.msg,"repeated WaitPort peeks do not consume registry capacity");
        check(std::count(std::begin(f.ctx.message_ports),std::end(f.ctx.message_ports),f.port)==1,"one active port has exactly one registry entry");
    }
    {
        Fixture f;
        for(unsigned i=0;i<64;++i)
        {
            ULONG p=0xB000+i*40,m=0x14000+i*80;
            f.manual_port(p);f.message(m);check(f.exec(366,p,m)==0xD0000000,"active manual port capacity is usable");
        }
        f.manual_port(f.port);f.message(f.msg);f.rejected();
        f.exec(372,0xB000);check(f.exec(366,f.port,f.msg)==0xD0000000,"draining one active port restores registry capacity");
    }
    {
        Fixture f;ULONG first=0x14000;
        for(unsigned i=0;i<256;++i)
        {
            ULONG m=first+i*80;f.message(m);f.ctx.mem[m+8]=5;
            f.w(m,i==255?f.port+24:m+80);f.w(m+4,i?m-80:f.port+20);
        }
        f.w(f.port+20,first);f.w(f.port+28,first+255*80);
        f.rejected();
        check(f.exec(372,f.port)==first,"full bounded queue still permits dequeue");
        check(f.exec(366,f.port,f.msg)==0xD0000000,"dequeue restores message capacity");
    }
    {
        Fixture f;f.message(f.msg,0);f.ctx.mem[f.msg+8]=5;
        f.w(f.port+20,f.msg);f.w(f.port+28,f.msg);f.w(f.msg,f.port+24);f.w(f.msg+4,f.port+20);
        auto before=f.bytes();check(f.exec(378,0,f.msg)==20 && f.bytes()==before,"NULL ReplyMsg refuses a message linked on an unseen manual port");
    }
}

static void manual_port_identity_and_owned_signal()
{
    {
        Fixture f;f.ctx.guest_exec_task=0;f.ctx.m68k_sysbase=0;
        f.w(f.msg+10,0x6300);std::strcpy(reinterpret_cast<char *>(f.ctx.mem+0x6300),"message name");
        check(f.exec(366,f.port,f.msg)==0xD0000000 && f.exec(372,f.port)==f.msg && !signals,"manual PA_IGNORE uses no task or native signal");
        check(f.r(f.msg+10)==0x6300 && !std::strcmp(reinterpret_cast<char *>(f.ctx.mem+0x6300),"message name"),"message node name is preserved verbatim");
    }
    {
        Fixture f;ULONG a=f.exec(666),b=f.exec(666);
        f.ctx.mem[a+15]=f.ctx.mem[b+15];f.rejected(a);
    }
}

static void read_after_arrival_metadata_change()
{
    /* The public message and port have the exact adjacent geometry from the
       application trace. Their arrival action is changed only after enqueue. */
    Fixture f;
    f.initialize_shadows();
    const ULONG p=f.msg+68, task=f.ctx.guest_exec_task;
    f.manual_port(p);
    f.message(f.msg,0,68);
    check(f.exec(366,p,f.msg)==0xD0000000,"enqueue to adjacent manual PA_IGNORE port");
    f.ctx.mem[p+14]=0;f.ctx.mem[p+15]=4;f.w(p+16,task);
    check(!f.ctx.guest_signals && !allocated_signals,"reserved SIGB_SINGLE was not allocated");
    pending_signals=1U<<31;
    auto native_counts=[] { return std::array<unsigned,8>{signals,reads,writes,buffer_calls,scalar_calls,alloc_calls,free_calls,freed_signals}; };
    const auto native_before=native_counts();const auto position_before=positions;
    const ULONG pending_before=pending_signals, sent_before=signalled_mask;
    const SIPTR error_before=ioerr;
    auto expected=f.bytes();
    check(f.exec(384,p)==f.msg && f.ctx.running && f.bytes()==expected,"nonempty WaitPort ignores unallocated arrival bit4 without changing any guest byte");
    be32(expected,p+20,p+24);be32(expected,p+28,p+20);
    check(f.exec(372,p)==f.msg && f.ctx.running && f.bytes()==expected,"GetMsg removes the packet-shaped message using only FIFO geometry");
    check(f.exec(372,p)==0 && f.ctx.running && f.bytes()==expected,"empty GetMsg ignores the unchanged unallocated arrival bit");
    check(native_counts()==native_before && positions==position_before && pending_signals==pending_before && signalled_mask==sent_before && ioerr==error_before,"queue reads make no native signal, I/O or allocation calls and preserve signal/IoErr state");
    check(f.exec(384,p)==20 && !f.ctx.running && f.bytes()==expected && native_counts()==native_before,"empty ordinary WaitPort still refuses without native waiting or mutation");
    f.message(f.msg,0,68);f.rejected(p);
    f.message(f.msg,p,68);const auto before_reply=f.bytes();
    check(f.exec(378,0,f.msg)==20 && !f.ctx.running && f.bytes()==before_reply && native_counts()==native_before,"ReplyMsg still refuses enqueue to an unowned reserved signal");
}

static void inspection_ignores_other_arrival_metadata()
{
    /* GetMsg and a nonempty WaitPort do not perform any arrival action,
       including unsupported ones or metadata referring to another task. */
    for(UBYTE action:{UBYTE(1),UBYTE(3),UBYTE(0x80)})
    {
        Fixture f;f.exec(366,f.port,f.msg);
        f.ctx.mem[f.port+14]=action;f.ctx.mem[f.port+15]=255;f.w(f.port+16,0xFFFFFFFE);
        auto before=f.bytes();
        check(f.exec(384,f.port)==f.msg && f.ctx.running && f.bytes()==before,"nonempty WaitPort does not interpret unsupported arrival metadata");
        check(f.exec(372,f.port)==f.msg && f.exec(372,f.port)==0 && f.ctx.running && !signals && !reads && !writes,"GetMsg does not consult action, bit or arrival task");
        f.message(f.msg);f.rejected();
    }
    {
        Fixture f;f.exec(366,f.port,f.msg);
        f.ctx.mem[f.port+14]=0;f.ctx.mem[f.port+15]=4;f.w(f.port+16,f.task);
        f.message(f.msg+0x100);
        check(f.exec(366,f.reply,f.msg+0x100)==0xD0000000,"enqueue validates its destination rather than another active queue's changed arrival metadata");
        f.rejected(f.reply,f.msg);
        f.w(f.msg,f.msg);auto before=f.bytes();
        check(f.exec(384,f.port)==20 && f.exec(372,f.port)==20 && f.bytes()==before,"changed arrival metadata never bypasses corrupt active-queue protection");
    }
}
static void released_manual_port_storage()
{
    {
        Fixture f;
        ULONG block=Kaffeine_HeapAlloc(&f.ctx,256,MEMF_CLEAR), p=block+16;
        check(block!=0,"manual port containing allocation succeeds");
        f.manual_port(p);f.message(f.msg);
        f.exec(366,p,f.msg);
        check(f.exec(384,p)==f.msg,"manual port WaitPort retains the queued message");
        /* Guest inline Remove(), without notifying the message bridge. */
        ULONG next=f.r(f.msg),prev=f.r(f.msg+4);
        f.w(prev,next);f.w(next+4,prev);
        check(f.r(p+20)==p+24 && f.r(p+24)==0 && f.r(p+28)==p+20,
              "inline dequeue leaves a canonical empty manual port");
        check(std::count(std::begin(f.ctx.message_ports),std::end(f.ctx.message_ports),p)==1,
              "inline removal still has a tracked port before storage release");
        f.message(f.msg+0x100);f.exec(366,f.port,f.msg+0x100);
        auto before=f.bytes();
        std::array<ULONG,KAFFEINE_MAX_MESSAGE_PORTS> saved;
        std::copy(std::begin(f.ctx.message_ports),std::end(f.ctx.message_ports),saved.begin());
        Kaffeine_HeapFree(&f.ctx,block+8,256);
        check(f.bytes()==before && std::equal(saved.begin(),saved.end(),std::begin(f.ctx.message_ports)),
              "interior free preserves guest bytes and active port registrations");
        Kaffeine_HeapFree(&f.ctx,block,0xFFFFFFFF);
        check(std::count(std::begin(f.ctx.message_ports),std::end(f.ctx.message_ports),p)==0,
              "successful containing free retires an inline-drained manual port using the real allocation size");
        check(std::count(std::begin(f.ctx.message_ports),std::end(f.ctx.message_ports),f.port)==1,
              "freeing one allocation retains an unrelated live queue");
        ULONG reused=Kaffeine_HeapAlloc(&f.ctx,256,0);
        check(reused==block,"the released storage is reacquired before reuse");
        std::memset(f.ctx.mem+reused,0xD4,256);
        f.message(f.msg+0x200);
        check(f.exec(366,f.reply,f.msg+0x200)==0xD0000000 && f.ctx.running,
              "reusing freed manual-port bytes does not block an unrelated PutMsg");
        check(f.exec(372,f.reply)==f.msg+0x200 && f.exec(372,f.port)==f.msg+0x100,
              "both unrelated queues retain message identity and FIFO links");
        check(std::all_of(f.ctx.mem+reused,f.ctx.mem+reused+256,[](UBYTE b){return b==0xD4;}),
              "later queue operations preserve all bytes in the reused allocation");
        Kaffeine_HeapFree(&f.ctx,reused,256);
    }
    for(unsigned malformed=0;malformed<4;++malformed)
    {
        Fixture f;ULONG block=Kaffeine_HeapAlloc(&f.ctx,256,MEMF_CLEAR),p=block+16;
        f.manual_port(p);f.exec(366,p,f.msg);
        if(malformed)
        {
            f.w(p+20,p+24);f.w(p+28,p+20);
            if(malformed==1)f.w(p+24,2);
            if(malformed==2)f.w(p+28,p+22);
            if(malformed==3)f.ctx.mem[p+8]=0;
        }
        Kaffeine_HeapFree(&f.ctx,block,256);
        check(std::count(std::begin(f.ctx.message_ports),std::end(f.ctx.message_ports),p)==1,
              "free never silently drops a nonempty or malformed tracked queue");
        ULONG reused=Kaffeine_HeapAlloc(&f.ctx,256,0);
        check(reused==block,"negative-control storage is reacquired before overwriting");
        std::memset(f.ctx.mem+reused,0xD4,256);
        f.message(f.msg+0x100);f.rejected(f.reply,f.msg+0x100);
        Kaffeine_HeapFree(&f.ctx,reused,256);
    }
    {
        Fixture f;ULONG block=Kaffeine_HeapAlloc(&f.ctx,256,MEMF_CLEAR),p=block+16;
        f.manual_port(p);f.exec(366,p,f.msg);f.w(p+20,p+24);f.w(p+28,p+20);
        ULONG mirror=f.ctx.heap_free, size=f.r(mirror);
        f.w(mirror,size+8);auto before=f.bytes();
        Kaffeine_HeapFree(&f.ctx,block,256);
        check(f.bytes()==before && std::count(std::begin(f.ctx.message_ports),std::end(f.ctx.message_ports),p)==1,
              "failed free with a corrupt free mirror does not retire an empty port");
        f.w(mirror,size);Kaffeine_HeapFree(&f.ctx,block,256);
    }
    for(ULONG requested:{32U,33U})
    {
        Fixture f;ULONG block=Kaffeine_HeapAlloc(&f.ctx,requested,MEMF_CLEAR);
        f.manual_port(block);f.exec(366,block,f.msg);
        f.w(block+20,block+24);f.w(block+28,block+20);
        Kaffeine_HeapFree(&f.ctx,block,requested);
        check(std::count(std::begin(f.ctx.message_ports),std::end(f.ctx.message_ports),block)==1,
              "a header extending beyond the requested payload is not silently retired");
    }
    for(unsigned delete_pool=0;delete_pool<2;++delete_pool)
    {
        Fixture f;ULONG pool=Kaffeine_PoolCreate(&f.ctx,MEMF_CLEAR,1024,512);
        ULONG block=Kaffeine_PoolAlloc(&f.ctx,pool,256),p=block+16;
        f.manual_port(p);f.exec(366,p,f.msg);f.w(p+20,p+24);f.w(p+28,p+20);
        auto before=f.bytes();
        Kaffeine_HeapFree(&f.ctx,block,256);
        Kaffeine_PoolFree(&f.ctx,pool,block,255);
        check(f.bytes()==before && std::count(std::begin(f.ctx.message_ports),std::end(f.ctx.message_ports),p)==1,
              "wrong-kind and wrong-size pool frees preserve the port registration and storage");
        if(delete_pool)Kaffeine_PoolDelete(&f.ctx,pool);
        else Kaffeine_PoolFree(&f.ctx,pool,block,256);
        check(std::count(std::begin(f.ctx.message_ports),std::end(f.ctx.message_ports),p)==0,
              "successful pool release retires its contained empty manual port");
        if(!delete_pool)Kaffeine_PoolDelete(&f.ctx,pool);
    }
    {
        Fixture f;ULONG p=f.exec(666);
        f.exec(366,p,f.msg);f.w(p+20,p+24);f.w(p+28,p+20);
        Kaffeine_HeapFree(&f.ctx,p,0);
        check(std::count(std::begin(f.ctx.message_ports),std::end(f.ctx.message_ports),p)==1,
              "direct storage free does not discard a CreateMsgPort signal owner's queue registration");
    }
}

/* Records of kaffeine_exec_io.c as its asynchronous timer I/O leaves them;
   the queue layer alone decides what they allow. */
static KaffeineIORequest &io_record(Fixture &f,unsigned slot,ULONG guest,ULONG port,bool pending,bool replied,bool copy)
{
    KaffeineIORequest &e=f.ctx.io_requests[slot];
    e={};e.guest=guest;e.port=port;e.pending=pending;e.replied=replied;e.copy=copy;
    e.kind=KAFFEINE_IO_TIMER;
    return e;
}
static void link_raw(Fixture &f,ULONG port,ULONG msg)
{
    f.w(port+20,msg);f.w(port+28,msg);f.w(msg,port+24);f.w(msg+4,port+20);f.ctx.mem[msg+8]=7;
}
static void async_requests()
{
    const ULONG req=0x9400;
    {
        Fixture f;f.message(req,f.port,40);
        io_record(f,0,req,f.port,true,false,true);
        f.rejected(f.port,req);
        auto before=f.bytes();
        check(f.exec(378,0,req)==20 && f.bytes()==before,"ReplyMsg of a tracked request in flight refuses");
        link_raw(f,f.port,req);
        before=f.bytes();
        check(f.exec(372,f.port)==20 && f.bytes()==before,"a request in flight linked raw makes GetMsg refuse");
        f.ctx.io_requests[0]={};
    }
    {
        Fixture f;f.message(req,f.port,40);
        io_record(f,0,req,f.port,false,true,true);
        link_raw(f,f.port,req);
        check(f.exec(372,f.port)==req && f.ctx.running,"a replied request is a node of its own captured port");
        f.message(req,f.port,40);
        f.rejected(f.port,req);
        link_raw(f,f.reply,req);
        auto before=f.bytes();
        check(f.exec(372,f.reply)==20 && f.bytes()==before,"a replied request on another port refuses");
        f.ctx.io_requests[0]={};
    }
    {
        Fixture f;f.message(req,f.port,48);
        io_record(f,0,req,f.port,false,true,true);
        link_raw(f,f.port,req);
        auto before=f.bytes();
        check(f.exec(372,f.port)==20 && f.bytes()==before,"a copied node larger than its 40 bytes refuses");
        f.ctx.io_requests[0]={};
    }
    {
        /* 48 -> 40: copies reserve exactly their 40 bytes */
        Fixture f;
        io_record(f,0,req,f.port,false,false,true);io_record(f,1,req+40,f.port,false,false,true);
        check(Kaffeine_MessageMemory(&f.ctx,req+80,16) && Kaffeine_MessageMemory(&f.ctx,req-16,16),
              "buffers right before and after two copies 40 bytes apart are ordinary memory");
        check(!Kaffeine_MessageMemory(&f.ctx,req+39,1) && !Kaffeine_MessageMemory(&f.ctx,req+79,1),
              "each copy keeps its own 40 bytes");
        f.message(req+80,0,20);
        check(f.exec(366,f.port,req+80)==0xD0000000,"a message right after a copy is sent");
        f.exec(372,f.port);
        io_record(f,2,0x9800,f.port,false,false,false);
        check(!Kaffeine_MessageMemory(&f.ctx,0x9800+39,1) && Kaffeine_MessageMemory(&f.ctx,0x9800+40,8),
              "an original caller-built timer reserves exactly its trusted forty bytes");
        f.ctx.io_requests[2].kind=KAFFEINE_IO_INPUT;
        check(!Kaffeine_MessageMemory(&f.ctx,0x9800+40,8) && Kaffeine_MessageMemory(&f.ctx,0x9800+48,8),
              "another original adapter retains its forty-eight-byte reservation");
        f.ctx.io_requests[2].kind=KAFFEINE_IO_NONE;
        f.ctx.io_requests[2].stack_message=1;
        check(!Kaffeine_MessageMemory(&f.ctx,0x9800+39,1) && Kaffeine_MessageMemory(&f.ctx,0x9800+40,8),
              "a retained closed zero-length timer keeps its trusted forty-byte extent");
        f.ctx.io_requests[2].stack_message=0;
        f.ctx.io_requests[2].kind=KAFFEINE_IO_TIMER;
        f.ctx.io_requests[2].capacity=64;
        check(!Kaffeine_MessageMemory(&f.ctx,0x9800+60,4) && Kaffeine_MessageMemory(&f.ctx,0x9800+64,4),
              "and CreateIORequest() storage its capacity");
        check(Kaffeine_ReleasableMemory(&f.ctx,req,80) && !Kaffeine_ReleasableMemory(&f.ctx,0x9800,64),
              "memory about to be freed leaves copies, and only copies, to the release hook");
        f.ctx.io_requests[0].orphan=1;
        check(Kaffeine_MessageMemory(&f.ctx,req,40),"an orphan reserves nothing");
        for(auto &e:f.ctx.io_requests)e={};
    }
    {
        /* The bridge's own preflight skips only its own record. */
        Fixture f;f.message(req,f.port,40);
        KaffeineIORequest &self=io_record(f,0,req,f.port,false,false,true);
        KaffeineQueue q;
        check(Kaffeine_QueueRead(&f.ctx,f.port,&q) && Kaffeine_IOMessagePreflight(&f.ctx,&q,req,40,&self) &&
              !Kaffeine_MessagePreflight(&f.ctx,&q,req,40),"only the bridge's preflight passes for its own record");
        io_record(f,1,req+8,f.port,false,false,true);
        check(!Kaffeine_IOMessagePreflight(&f.ctx,&q,req,40,&self),"another record over it still refuses");
        for(auto &e:f.ctx.io_requests)e={};
    }
    {
        /* Remove(): head, middle, tail; nothing else changes. */
        Fixture f;const ULONG m[4]={0x9400,0x9500,0x9600,0x9700};
        for(ULONG x:m){f.message(x,0,20);f.exec(366,f.port,x);}
        pending_signals=0x5000;const unsigned sent=signals;
        check(Kaffeine_GuestRemoveMsg(&f.ctx,f.port,m[1]) && f.r(m[0])==m[2] && f.r(m[2]+4)==m[0],
              "Remove() of a middle node links its neighbours");
        check(Kaffeine_GuestRemoveMsg(&f.ctx,f.port,m[3]) && f.r(m[2])==f.port+24 && f.r(f.port+28)==m[2],
              "Remove() of the tail");
        check(Kaffeine_GuestRemoveMsg(&f.ctx,f.port,m[0]) && f.r(f.port+20)==m[2] && f.r(m[2]+4)==f.port+20,
              "Remove() of the head");
        auto before=f.bytes();
        check(Kaffeine_GuestRemoveMsg(&f.ctx,f.port,m[1]) && f.bytes()==before,"a node not there changes nothing");
        check(signals==sent && pending_signals==0x5000 &&
              std::count(std::begin(f.ctx.message_ports),std::end(f.ctx.message_ports),f.port)==1,
              "no signal changes, and the port with a node stays registered");
        check(Kaffeine_GuestRemoveMsg(&f.ctx,f.port,m[2]) &&
              std::count(std::begin(f.ctx.message_ports),std::end(f.ctx.message_ports),f.port)==0 &&
              f.r(f.port+20)==f.port+24 && f.r(f.port+28)==f.port+20,"removing the last node empties the port and its registration");
        f.exec(366,f.port,m[0]);f.w(m[0],m[0]);before=f.bytes();
        check(!Kaffeine_GuestRemoveMsg(&f.ctx,f.port,m[0]) && f.bytes()==before,"a malformed queue fails unchanged");
    }
}

static unsigned dos_gate_calls, adapter_calls;
static BOOL deny_packet;
static BOOL process_dos_gate(KaffeineContext *ctx, void *, ULONG lvo, IPTR *result)
{
    ++dos_gate_calls;
    assert(lvo==0);
    if(!deny_packet)return FALSE;
    ctx->running=FALSE;ctx->exit_code=20;*result=20;return TRUE;
}
static IPTR recording_adapter(KaffeineContext *, ULONG, ULONG)
{
    ++adapter_calls;return 0;
}
static void process_packet_gate()
{
    for(unsigned handler=0;handler<2;handler++)
        for(unsigned denied=0;denied<2;denied++)
        {
            Fixture f;
            KaffeineTaskOps ops={};ops.process_dos=process_dos_gate;
            f.ctx.sched=reinterpret_cast<KaffeineScheduler*>(&ops);f.ctx.task_ops=&ops;
            if(handler)
            { f.ctx.handlers[0].guest=f.port;f.ctx.handler_packet=recording_adapter; }
            else
            { f.ctx.files[0].guest=0x9000;f.ctx.files[0].handler=f.port;f.ctx.file_packet=recording_adapter; }
            deny_packet=denied;dos_gate_calls=adapter_calls=0;
            const auto before=f.bytes();const ULONG result=f.exec(366,f.port,f.msg);
            check(dos_gate_calls==1&&adapter_calls==!denied&&f.bytes()==before,
                  "process DOS guard runs before either native packet adapter");
            check(denied?(result==20&&!f.ctx.running&&f.ctx.exit_code==20):
                         (result==0xD0000000&&f.ctx.running),
                  "denied process packets stop while root packet dispatch stays available");
            f.ctx.sched=nullptr;f.ctx.task_ops=nullptr;
            f.ctx.files[0]={};f.ctx.handlers[0]={};
        }
}

static void io_request_cannot_be_its_own_port()
{
    Fixture f;
    auto &request=f.ctx.io_requests[0];
    request.guest=f.port; request.port=f.port; request.capacity=40; request.replied=1;
    KaffeineQueue queue{};
    auto before=f.bytes();
    check(!Kaffeine_QueueRead(&f.ctx,f.port,&queue) && f.bytes()==before,
          "a replied typed IORequest cannot impersonate its own MsgPort through the node exemption");
    request={};
}

int main()
{
    fifo();ignore_and_null_reply();invalid();allocation_failures();real_shadows_and_port_lifetimes();queue_capacity_and_membership();manual_port_identity_and_owned_signal();read_after_arrival_metadata_change();inspection_ignores_other_arrival_metadata();
    released_manual_port_storage();
    retired_port_signal();
    process_packet_gate();
    io_request_cannot_be_its_own_port();
    async_requests();
    check(allocated_signals==0,"all owned native signals are released");
    check(native_blocks.empty(),"all native heap records and allocations are released");
    std::printf("Guest Exec message boundary: %u/%u checks passed\n",checks-failures,checks);
    return failures?1:0;
}
