/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "message_fixture.h"
static void be32(std::vector<UBYTE> &b,ULONG a,ULONG v)
{ for(unsigned i=0;i<4;++i)b[a+i]=static_cast<UBYTE>(v>>(24-8*i)); }
static void measured_reads()
{
    Fixture f;ULONG token=f.open(),fh=token*4,handler=f.r(fh+8);
    check(token && token<=f.ctx.mem_size/4 && fh!=static_cast<ULONG>(reinterpret_cast<IPTR>(open_result)),"Open returns a bounded guest BPTR");
    check(f.r(fh+36)!=0 && handler && f.r(fh+36)!=static_cast<ULONG>(f.native[2].fh_Args),"handler key and guest identity contain no native pointer");
    check(f.r(fh+12)==0 && f.r(fh+16)==0xFFFFFFFF && f.r(fh+20)==0xFFFFFFFF,"new FileHandle has no native buffer and signed initial positions");
    ULONG rp=f.exec(666);std::memset(f.ctx.mem+f.buffer-1,0xA5,32);
    const LONG requested[]={5,9,4},returned[]={5,7,0};
    size_t offset=0;
    for(unsigned i=0;i<3;++i)
    {
        f.packet(token,requested[i],f.buffer,f.msg,rp);auto before=f.bytes(), expected=before;
        for(LONG j=0;j<returned[i];++j)expected[f.buffer+j]=contents[offset+j];
        expected[f.msg+8]=5;be32(expected,f.msg,rp+24);be32(expected,f.msg+4,rp+20);
        be32(expected,f.msg+10,f.msg+20);be32(expected,f.msg+24,handler);
        be32(expected,f.msg+32,returned[i]);be32(expected,f.msg+36,0);
        be32(expected,rp+20,f.msg);be32(expected,rp+28,f.msg);
        check(f.exec(366,handler,f.msg)==0xD0000000 && f.ctx.running,"recognized ACTION_READ completes synchronously");
        check(f.bytes()==expected,"packet operation changes only measured result/data/reply bytes across the whole arena and guards");
        check(reads==i+1 && seen_handle==open_result && read_length==requested[i],"native Read receives private full-width handle and signed length");
        check(reinterpret_cast<uintptr_t>(read_buffer)<reinterpret_cast<uintptr_t>(f.ctx.mem) ||
              reinterpret_cast<uintptr_t>(read_buffer)>=reinterpret_cast<uintptr_t>(f.ctx.mem)+f.ctx.mem_size,"native Read uses a temporary buffer");
        check(f.r(f.msg+32)==static_cast<ULONG>(returned[i]) && f.r(f.msg+36)==0 && ioerr==137,"packet result/error match measured reads and preserve caller IoErr");
        check(!std::memcmp(f.ctx.mem+f.buffer,contents+offset,returned[i]),"packet copies actual data bytes");
        for(size_t j=returned[i];j<static_cast<size_t>(requested[i]);++j)check(f.ctx.mem[f.buffer+j]==before[f.buffer+j],"short read and EOF preserve bytes beyond actual count");
        check(f.ctx.mem[f.buffer-1]==0xA5 && f.ctx.mem[f.buffer+16]==0xA5,"read buffer guards preserved");
        check(f.r(f.msg+20)==f.msg && f.r(f.msg+10)==f.msg+20 && f.r(f.msg+24)==handler && f.r(f.msg+14)==0,"DOS reply preserves message/packet identity, NULL reply and sets handler dp_Port");
        check(f.ctx.mem[f.msg+8]==5 && f.ctx.mem[f.msg+9]==static_cast<UBYTE>(-7) && m68k_read16(&f.ctx,f.msg+18)==68,"DOS reply retains priority and length with measured type5");
        check(f.r(f.msg)==rp+24 && f.r(f.msg+4)==rp+20 && f.r(rp+20)==f.msg && f.r(rp+28)==f.msg,"DOS reply links use exact BE queue sentinels");
        check(f.exec(384,rp)==f.msg && f.exec(372,rp)==f.msg && f.exec(372,rp)==0,"WaitPort peeks and GetMsg removes exact packet reply");
        offset+=returned[i];
    }
    check(f.dos(66,token,0,0)==12,"direct Seek shares packet file position");
    check(f.dos(42,token,f.buffer,4)==0 && seen_handle==open_result,"direct Read observes packet EOF through same association");
    check(f.dos(36,token)==static_cast<ULONG>(-1) && closes==1,"Close resolves private native handle");
    auto before=f.bytes();unsigned nr=reads;
    check(f.dos(42,token,f.buffer,1)==20 && !f.ctx.running && reads==nr && f.bytes()==before,"closed token cannot reach native Read");
}
static void unused_message_reply_port()
{
    for(bool write:{false,true})
        for(ULONG unused:{0U,0x12345678U,0xFFFFFFFFU})
        {
            Fixture f;ULONG token=f.open(),handler=f.r(token*4+8);
            f.packet(token,1);
            f.w(f.msg+14,unused);
            if(write)f.w(f.msg+28,87);
            check(f.exec(366,handler,f.msg)==0xD0000000 && f.ctx.running,
                  "DOS read/write packet ignores an unused NULL or poisoned message reply pointer");
            check((write?writes:reads)==1 && f.r(f.msg+32)==1 && !f.r(f.msg+36) &&
                  f.r(f.msg+14)==unused && f.r(f.msg+24)==handler,
                  "only dp_Port selects the validated reply queue; unused header bytes survive");
            check(f.exec(372,f.reply)==f.msg && !f.exec(372,f.reply),
                  "packet reply reaches exactly its dp_Port queue");
            /* Reuse the returned message, including its reply type and stale
               mn_ReplyPort, while restoring the real DOS reply destination. */
            f.w(f.msg+24,f.reply);
            check(f.exec(366,handler,f.msg)==0xD0000000 && f.ctx.running &&
                  (write?writes:reads)==2 && f.r(f.msg+14)==unused && f.exec(372,f.reply)==f.msg,
                  "reused DOS packet keeps its unused message reply pointer opaque");
        }
}
static void packet_failures()
{
    for(unsigned which=0;which<31;++which)
    {
        Fixture f;ULONG token=f.open(),handler=f.r(token*4+8);f.packet(token);
        switch(which)
        {
            case 0:handler=0;break;
            case 1:handler+=2;break;
            case 2:f.w(f.msg+20,f.msg+20);break;
            case 3:m68k_write16(&f.ctx,f.msg+18,67);break;
            case 4:
                f.w(f.msg+28,83);
                check(f.exec(366,handler,f.msg)==0xD0000000 && f.ctx.running &&
                      !f.r(f.msg+32) && f.r(f.msg+36)==209 && ioerr==137 && !reads,
                      "an unknown packet action now replies 0/209 without native I/O or stopping");
                check(f.exec(372,f.reply)==f.msg,"unknown action reply is queued normally");
                continue;
            case 5:f.w(f.msg+40,0);break;
            case 6:f.w(f.msg+40,f.r(f.msg+40)+1);break;
            case 7:f.w(f.msg+24,0);break;
            case 8:f.w(f.msg+24,handler);break;
            case 9:f.w(f.msg+44,0);break;
            case 10:f.w(f.msg+44,0xFFFFFFFF);break;
            case 11:f.w(f.msg+48,0xFFFFFFFF);break;
            case 12:f.w(f.msg+48,0x80000000);break;
            case 13:f.w(f.msg+44,f.msg+50);break;
            case 14:f.w(f.msg+44,f.reply+20);break;
            case 15:f.w(f.msg+44,token*4);break;
            case 16:f.w(f.msg+44,handler+32);break;
            case 17:
                f.w(f.msg+14,handler);
                check(f.exec(366,handler,f.msg)==0xD0000000 && f.ctx.running && reads==1 &&
                      f.r(f.msg+14)==handler && f.exec(372,f.reply)==f.msg,
                      "DOS ignores and preserves unused mn_ReplyPort; dp_Port receives the reply");
                continue;
            case 18:f.w(f.msg+10,f.msg+22);break;
            case 19:
                /* DoPkt1() to DoPkt4() leave the other argument registers as
                   they were, and handlers ignore what an action does not use. */
                for(ULONG a=52;a<=64;a+=4)f.w(f.msg+a,1);
                check(f.exec(366,handler,f.msg)==0xD0000000 && f.ctx.running && reads==1,
                      "ACTION_READ ignores dp_Arg4 to dp_Arg7");
                check(f.exec(372,f.reply)==f.msg,"ACTION_READ with unused arguments replies normally");
                continue;
            case 20:f.ctx.mem[f.reply+14]=1;break;
            case 21:f.ctx.mem[f.reply+14]=0;f.w(f.reply+16,f.task);f.ctx.mem[f.reply+15]=4;break;
            case 22:f.w(f.reply+20,f.reply+20);break;
            case 23:f.w(f.reply+28,f.msg);break;
            case 24:f.ctx.mem[handler+14]=0;break;
            case 25:f.w(handler+16,f.task);break;
            case 26:f.w(token*4+8,0);break;
            case 27:f.w(token*4+36,0);break;
            case 28:f.w(f.exec_base+276,f.task+4);break;
            case 29:f.w(f.msg+44,f.ctx.mem_size-4);break;
            case 30:f.ctx.num_shadows=1;f.ctx.shadow_map[0]={f.buffer+2,reinterpret_cast<void *>(1),8};break;
        }
        f.rejected(handler);
    }
    { Fixture f;ULONG t=f.open(),h=f.r(t*4+8);f.packet(t);f.exec(366,h,f.msg);f.rejected(h); }
    { Fixture f;ULONG t=f.open(),h=f.r(t*4+8);f.packet(t);read_override=-1;check(f.exec(366,h,f.msg)==0xD0000000,"native read error still queues DOS reply");check(f.r(f.msg+32)==0xFFFFFFFF && f.r(f.msg+36)==static_cast<ULONG>(-123) && ioerr==137,"signed native error and caller IoErr are independent"); }
    { Fixture f;ULONG t=f.open(),h=f.r(t*4+8);f.packet(t,0,0xFFFFFFFF);check(f.exec(366,h,f.msg)==0xD0000000 && read_length==0,"zero-length packet does not dereference unused guest buffer"); }
    { Fixture f;ULONG t=f.open(),h=f.r(t*4+8);f.packet(t);allocation_countdown=0;f.rejected(h);allocation_countdown=-1; }
}
static void identities()
{
    Fixture f;ULONG in=f.dos(54),out=f.dos(60);
    check(in && out && in!=out && f.dos(54)==in && f.dos(60)==out,"Input/Output borrowed identities repeat consistently");
    check(f.r(f.task+156)==in && f.r(f.task+160)==out,"Process stream fields contain the same guest BPTRs");
    check(f.dos(294,out)==in && native_input==native_output && f.dos(54)==out && f.r(f.task+156)==out,"SelectInput maps arguments, old identity and Process field");
    ULONG t=f.open();
    for(ULONG lvo:{204U,216U,270U,282U,372U,384U,426U,456U})
    {
        seen_handle=nullptr;f.dos(lvo,t,7,0);
        check(seen_handle==open_result && f.ctx.running,"scalar handle thunk overrides generated native forwarding");
    }
    f.dos(450,1,t,3);check(seen_handle==open_result,"ChangeMode recognizes FH type");
    f.dos(450,0,0x123456,3);check(seen_handle==reinterpret_cast<BPTR>(0x123456),"lock BPTR remains a lock in ChangeMode");
    unsigned mode_calls=scalar_calls;
    check(f.dos(450,0,0,3)==20 && !f.ctx.running && scalar_calls==mode_calls,
          "ChangeMode refuses a null FileLock before native DOS can dereference it");
    seen_lock=nullptr;
    check(f.dos(378,0x765432)==t && seen_lock==reinterpret_cast<BPTR>(0x765432),"OpenFromLock consumes a lock and imports a file identity");
    check(f.open()==t,"repeated native handle has one guest identity");
    unsigned before_closes=closes;
    Kaffeine_FileCleanup(&f.ctx);
    check(closes==before_closes+1,"cleanup closes owned handle once and leaves all borrowed aliases open");
}

static void buffering()
{
    Fixture f;ULONG t=f.open(),a=t*4;
    native_buffer.fill(0xEA);
    check(f.dos(306,t)=='A' && seen_handle==open_result,"FGetC uses mapped native handle");
    ULONG b=f.r(a+12)*4;
    check(b && b<f.ctx.mem_size && b!=static_cast<ULONG>(reinterpret_cast<IPTR>(native_buffer.data())),"native buffer becomes a guest-owned mirror");
    check(f.r(a+16)==1 && f.r(a+20)==12 && f.r(a+44)==208 && f.r(a+48)==b/4 &&
          !std::memcmp(f.ctx.mem+b,contents,12),"FileHandle buffered snapshot refreshes scalar fields and initialized bytes");
    check(f.ctx.mem[b+12]==0 && f.ctx.mem[b+207]==0,"snapshot does not expose uninitialized native buffer bytes");
    for(ULONG lvo:{312U,318U,360U}) check(f.dos(lvo,t,'Q')!=20 && seen_handle==open_result,"buffered scalar overrides resolve native handle and refresh snapshots");
    check(f.dos(366,t,0,2,208)==0 && f.r(a+12)==0 && f.r(a+16)==0xFFFFFFFF,"SetVBuf refreshes guest fields after native buffer removal");
    f.w(a+12,0x2222);auto before=f.bytes();unsigned bc=buffer_calls;
    check(f.dos(306,t)==20 && !f.ctx.running && buffer_calls==bc && f.bytes()==before,"mixed guest/native buffering refuses before native calls or snapshot writes");
}
static void ownership_failures()
{
    { Fixture f;auto before=f.bytes();allocation_countdown=0;
      check(f.open()==0 && closes==1 && f.bytes()==before,"failed owned handle allocation closes native result and preserves guest bytes");allocation_countdown=-1; }
    { Fixture f;open_result=nullptr;check(f.open()==0 && !closes,"failed native Open creates no association"); }
    { Fixture f;for(unsigned i=0;i<32;++i){open_result=MKBADDR(&f.native[i]);check(f.open()!=0,"file table admits bounded identities");}
      open_result=MKBADDR(&f.native[32]);check(f.open()==0 && closes==1,"full file table closes a newly owned handle");
      open_result=MKBADDR(&f.native[0]);check(f.open()!=0 && closes==1,"full table still recognizes an existing identity"); }
    { Fixture f;f.native[0].fh_Buf=MKBADDR(native_buffer.data());f.native[0].fh_BufSize=208;f.native[0].fh_Pos=0;f.native[0].fh_End=12;
      allocation_countdown=1;check(f.dos(54)==0 && !closes,"borrowed buffer allocation failure never closes native stream");allocation_countdown=-1; }
    { Fixture f;f.native[2].fh_Pos=INT64_C(1)<<40;
      check(f.open()==0 && closes==1,"unrepresentable DOS64 position fails import without truncating it"); }
    { Fixture f;ULONG t=f.open();f.dos(36,t);auto before=f.bytes();unsigned bc=closes;
      check(f.dos(36,t)==20 && closes==bc && f.bytes()==before,"double Close refuses stale identity");
      open_result=MKBADDR(&f.native[3]);ULONG next=f.open();check(next && next!=t,"new Open never reuses a closed guest BPTR"); }
    { Fixture f;for(ULONG bad:{1U,0xFFFFFFFFU,0x80000000U,0x12345678U})
      { auto before=f.bytes();unsigned nr=reads;check(f.dos(42,bad,f.buffer,1)==20 && reads==nr && f.bytes()==before,"unknown/fake-looking/native-looking tokens never fall through to native DOS"); } }
    { Fixture a;ULONG ta=a.open();BPTR na=open_result;Fixture b;ULONG tb=b.open();BPTR nb=open_result;
      check(ta==tb && na!=nb,"contexts have independent native associations for equal guest addresses");
      b.dos(42,tb,b.buffer,1);check(seen_handle==nb,"second context resolves its own handle");
      a.dos(42,ta,a.buffer,1);check(seen_handle==na,"first context keeps its private handle"); }
}

static void sequential_handles_and_selected_close()
{
    {
        Fixture f;unsigned count=KAFFEINE_MAX_FILES*4+7;
        std::vector<ULONG> retired;
        native_process.pr_CES=native_output;
        f.initialize_shadows();
        ULONG current=f.ctx.guest_exec_task;
        check(f.r(current+156)==f.dos(54) && f.r(current+160)==f.dos(60) && f.r(current+224)==f.dos(60),"real Process snapshot and Input/Output/error streams share production fake identities");
        for(unsigned i=0;i<count;++i)
        {
            ULONG t=f.open();check(t!=0,"Open/Close capacity is concurrent, not cumulative");
            ULONG h=f.r(t*4+8);retired.push_back(t);
            if(i==0)
            {
                f.packet(t);check(f.exec(366,h,f.msg)==0xD0000000 && f.r(f.msg+32)==5,"ACTION_READ completes with actual production ExecBase/Process shadows");
                check(f.exec(372,f.reply)==f.msg,"real-shadow packet reply is dequeued normally");
            }
            if(i%3==0) { f.dos(306,t);check(std::any_of(std::begin(f.ctx.files),std::end(f.ctx.files),[](const KaffeineFile &v){return v.owned && v.mirror;}),"buffered handle owns a live guest mirror"); }
            check(f.dos(36,t)==static_cast<ULONG>(-1),"sequential Close resolves native handle");
            check(std::none_of(std::begin(f.ctx.files),std::end(f.ctx.files),[](const KaffeineFile &v){return v.owned || v.mirror;}),"Close releases owned active entry and mirror while borrowed streams survive");
            if(i==1 || i==count-1)
            {
                f.packet(t); // A stale identity has no live key.
                f.rejected(h);
                f.manual_port(h);f.rejected(h);
                unsigned nr=reads;check(f.dos(42,retired.front(),f.buffer,1)==20 && reads==nr,"old token stays invalid after many native address reuses");
            }
        }
        check(closes==count,"each successful native Open is closed exactly once");
        check(std::adjacent_find(retired.begin(),retired.end())==retired.end(),"quarantined guest identities are not reused");
        ULONG before=Kaffeine_HeapAvail(&f.ctx,0);
        Kaffeine_FileCleanup(&f.ctx);
        check(!f.ctx.file_slabs && Kaffeine_HeapAvail(&f.ctx,0)>before && closes==count,"teardown releases all quarantined guest identity slabs without double close");
    }
    {
        Fixture f;
        auto *allocated=static_cast<FileHandle *>(AllocMem(sizeof(FileHandle),MEMF_CLEAR));
        allocated->fh_Pos=allocated->fh_End=-1;
        native_input=native_output=MKBADDR(allocated);native_process.pr_CES=MKBADDR(allocated);
        close_frees=MKBADDR(allocated);
        f.initialize_shadows();ULONG current=f.ctx.guest_exec_task;
        ULONG t=f.dos(54);check(t && f.dos(60)==t,"selected borrowed aliases share one identity");
        check(f.dos(36,t)==static_cast<ULONG>(-1) && !close_frees,"explicit Close frees selected native stream");
        check(!native_input && !native_output && !native_process.pr_CES,"Close detaches all native stream aliases before freeing the handle");
        check(f.dos(54)==0 && f.dos(60)==0 && f.r(current+156)==0 && f.r(current+160)==0 && f.r(current+224)==0,"later Input/Output never dereference or import the freed FileHandle");
    }
}

static void all_handle_entry_points()
{
    Fixture f;ULONG t=f.open();
    const ULONG text=0x6300;
    std::strcpy(reinterpret_cast<char *>(f.ctx.mem+text),"plain");
    check(f.dos(300,t)!=20 && native_output==open_result && f.dos(60)==t,"SelectOutput resolves handle, return identity and subsequent Output");
    check(f.dos(324,t,f.buffer,3,2)==1 && seen_handle==open_result && block_length==3 && block_number==2,"FRead maps handle while preserving checked block dimensions");
    check(f.dos(330,t,f.buffer,3,2)==1 && seen_handle==open_result,"FWrite maps handle");
    check(f.dos(336,t,f.buffer,8)==f.buffer && seen_handle==open_result,"FGets returns guest buffer identity through mapped handle");
    check(f.dos(342,t,text)==0 && seen_handle==open_result,"FPuts maps handle");
    check(f.dos(348,t,text,0)==0 && seen_handle==open_result,"VFWritef maps handle before native formatting");
    check(f.dos(354,t,text,0)==5 && seen_handle==open_result,"VFPrintf maps handle with production checked formatter");
    check(f.dos(408,t,f.buffer,32)==static_cast<ULONG>(-1) && seen_handle==open_result,"NameFromFH maps handle");
    check(f.dos(390,t,0xA100)==static_cast<ULONG>(-1) && seen_handle==open_result && f.r(0xA100+124)==12,"ExamineFH maps handle through the production native FIB copy");
    check(f.dos(222,text,t,t)==static_cast<ULONG>(-1) && execute_input==open_result && execute_output==open_result,"Execute resolves both handles without remapping native internal calls");
    check(f.dos(942,text,5)==5 && f.dos(948,text)==0 && f.dos(954,text,0)==5,"implicit output APIs keep the native buffer snapshot current");
    for(ULONG lvo:{204U,216U,270U,282U,306U,312U,318U,360U,372U,384U,426U,456U,324U,330U,336U,342U,348U,354U,390U,408U})
    {
        unsigned nr=reads,nw=writes,nb=buffer_calls,nc=scalar_calls,na=alloc_calls;
        auto before=f.bytes();
        ULONG second=(lvo==342 || lvo==348 || lvo==354)?text:f.buffer;
        check(f.dos(lvo,0x12345678,second,8,2)==20 && !f.ctx.running,"every supported handle consumer refuses unknown native-looking tokens");
        check(reads==nr && writes==nw && buffer_calls==nb && scalar_calls==nc && alloc_calls==na && f.bytes()==before,"unknown handle refusal precedes native I/O, conversion, allocation and guest writes");
    }
    for(ULONG lvo:{276U,288U})
    {
        auto before=f.bytes();unsigned nc=scalar_calls;
        check(f.dos(lvo,f.buffer,1)==20 && f.bytes()==before && scalar_calls==nc,"native-width record arrays remain explicitly unsupported");
    }
    {
        Fixture bad;unsigned nc=scalar_calls;
        std::memset(bad.ctx.mem+bad.ctx.mem_size-8,'x',8);
        auto before=bad.bytes();
        check(bad.dos(30,bad.ctx.mem_size-8,1005)==0 && scalar_calls==nc && bad.bytes()==before,"unterminated Open path never reaches native DOS");
    }
}

static void packet_reply_queue_preflight()
{
    {
        Fixture f;ULONG t=f.open(),h=f.r(t*4+8),other=f.msg+0x100;
        f.message(other);f.exec(366,f.reply,other);f.packet(t);
        check(f.exec(366,h,f.msg)==0xD0000000 && f.r(f.reply+20)==other && f.r(other)==f.msg && f.r(f.msg+4)==other,"DOS reply appends behind an existing ordinary message");
        check(f.exec(372,f.reply)==other && f.exec(372,f.reply)==f.msg,"nonempty packet reply queue retains FIFO order");
    }
    for(unsigned variant=0;variant<6;++variant)
    {
        Fixture f;ULONG t=f.open(),h=f.r(t*4+8),other=f.msg+0x100;
        f.message(other);f.exec(366,f.reply,other);f.packet(t);
        switch(variant)
        {
            case 0:f.w(other,other);break;
            case 1:f.w(other+4,0xFFFFFFFF);break;
            case 2:f.w(f.msg+44,other+20);break;
            case 3:f.ctx.app_ports[0].guest=f.reply;f.ctx.app_ports[0].native=reinterpret_cast<MsgPort *>(1);break;
            case 4:f.ctx.io_requests[0].guest=f.msg;f.ctx.io_requests[0].capacity=68;break;
            case 5:f.w(h,other);break;
        }
        f.rejected(h);
    }
    {
        Fixture f;ULONG t=f.open(),h=f.r(t*4+8),other=f.msg+0x100;
        f.message(other);f.exec(366,f.port,other);f.packet(t,5,other+20);
        f.rejected(h);
    }
    {
        Fixture f;ULONG t=f.open(),h=f.r(t*4+8);f.packet(t);
        f.w(t*4+8,0);auto before=f.bytes();unsigned nr=reads,nc=scalar_calls;
        check(f.dos(54)!=20,"independent input identity remains usable when another file is corrupt");
        // Select an intact handle first, then corrupt its identity. Input must
        // stop without rewriting the Process field or touching native DOS I/O.
        f.w(t*4+8,h);f.dos(294,t);f.w(t*4+8,0);before=f.bytes();nc=scalar_calls;
        check(f.dos(54)==20 && f.bytes()==before && reads==nr && scalar_calls==nc,"corrupt selected Input stops with D0=20 before Process mutation or native I/O");
    }
}

static void bounded_packet_diagnostics()
{
    Fixture a;ULONG t=a.open(),h=a.r(t*4+8);
    for(unsigned i=0;i<24;++i)
    {
        a.packet(t,0,a.buffer);
        check(a.exec(366,h,a.msg)==0xD0000000 && a.exec(372,a.reply)==a.msg,"diagnostic cap does not change packet delivery");
    }
    check(a.ctx.packet_trace_count==16,"successful ACTION_READ diagnostics are bounded per context");
    Fixture b;check(b.ctx.packet_trace_count==0 && a.ctx.packet_trace_count==16,"packet diagnostic budgets are isolated by context");
}

static void application_read_then_reserved_waitport()
{
    Fixture f;f.initialize_shadows();
    const ULONG t=f.open(),h=f.r(t*4+8),p=f.msg+68,task=f.ctx.guest_exec_task;
    f.manual_port(p);
    read_override=4096;read_override_bytes.resize(4096);
    for(size_t i=0;i<read_override_bytes.size();++i)read_override_bytes[i]=static_cast<UBYTE>(i*13+7);
    std::memset(f.ctx.mem+f.buffer-1,0xA5,4098);
    f.packet(t,4096,f.buffer,f.msg,p);
    check(f.exec(366,h,f.msg)==0xD0000000 && f.r(f.msg+32)==4096 && f.r(f.msg+36)==0 && ioerr==137,"application ACTION_READ completes 4096 bytes before arrival metadata changes");
    check(reads==1 && seen_handle==open_result && read_length==4096 && !std::memcmp(f.ctx.mem+f.buffer,read_override_bytes.data(),4096),"native read arguments and complete result data match the independent fixture");
    check(f.r(p+20)==f.msg && f.r(p+24)==0 && f.r(p+28)==f.msg && f.r(f.msg)==p+24 && f.r(f.msg+4)==p+20 && f.ctx.mem[f.msg+8]==5 && m68k_read16(&f.ctx,f.msg+18)==68,"packet and adjacent reply port reproduce the live queue geometry");
    f.ctx.mem[p+14]=0;f.ctx.mem[p+15]=4;f.w(p+16,task);
    check(!f.ctx.guest_signals && !allocated_signals,"no guest signal bit was allocated for the reply port transition");
    pending_signals=1U<<31;
    const auto pending_before=pending_signals,allocated_before=allocated_signals;
    auto native_counts=[] { return std::array<unsigned,8>{signals,reads,writes,buffer_calls,scalar_calls,alloc_calls,free_calls,freed_signals}; };
    const auto native_before=native_counts();const auto positions_before=positions;
    auto expected=f.bytes();
    check(f.exec(384,p)==f.msg && f.ctx.running && f.bytes()==expected,"WaitPort peeks the completed read despite reserved bit4 without touching packet or buffer");
    be32(expected,p+20,p+24);be32(expected,p+28,p+20);
    check(f.exec(372,p)==f.msg && f.ctx.running && f.bytes()==expected,"GetMsg dequeues the exact completed packet and changes only queue sentinels");
    check(f.exec(372,p)==0 && f.ctx.running && f.bytes()==expected,"empty reply GetMsg returns NULL without consulting bit4");
    check(native_counts()==native_before && positions==positions_before && pending_signals==pending_before && allocated_signals==allocated_before && ioerr==137,"post-read WaitPort/GetMsg perform no native signal, I/O or allocation and preserve all signal state");
    check(f.ctx.mem[f.buffer-1]==0xA5 && f.ctx.mem[f.buffer+4096]==0xA5,"all 4096 read bytes retain both guards");
    f.packet(t,4096,f.buffer,f.msg,p);f.rejected(h);
    check(reads==1 && positions==positions_before && native_counts()==native_before && ioerr==137,"invalid reply arrival metadata is refused before a second native Read, allocation or guest write");
}

static void action_packet(Fixture &f,ULONG token,LONG action,ULONG a1,ULONG a2=0,ULONG a3=0)
{
    f.packet(token);f.w(f.msg+28,action);f.w(f.msg+40,a1);f.w(f.msg+44,a2);f.w(f.msg+48,a3);
}
static void action_reply(Fixture &f,ULONG handler,LONG res1,LONG res2)
{
    check(f.dos(246,f.msg+20,handler,f.reply)==0 && f.ctx.running,
          "SendPkt delivers a supported FileHandle action synchronously");
    check(f.r(f.msg+32)==static_cast<ULONG>(res1) && f.r(f.msg+36)==static_cast<ULONG>(res2) && ioerr==137,
          "FileHandle action returns its signed results and preserves caller IoErr");
    check(f.r(f.msg+24)==handler && f.r(f.msg+14)==f.reply && f.r(f.msg+10)==f.msg+20 &&
          f.ctx.mem[f.msg+8]==5 && f.exec(372,f.reply)==f.msg,
          "FileHandle action uses the common type5 packet reply and handler identity");
}
static void packet_writes()
{
    for(LONG result:{5,3,0,-1})
    {
        Fixture f;ULONG token=f.open(),handler=f.r(token*4+8);
        std::memcpy(f.ctx.mem+f.buffer,"abcde",5);
        action_packet(f,token,ACTION_WRITE,f.r(token*4+36),f.buffer,5);
        auto before=f.bytes();write_override=result;
        action_reply(f,handler,result,result==-1?211:0);
        check(writes==1 && !reads && seen_handle==open_result && read_length==5 &&
              written_bytes==std::vector<UBYTE>({'a','b','c','d','e'}),
              "ACTION_WRITE passes exact bytes and length through a private native buffer");
        check(!std::memcmp(before.data()+f.buffer-1,f.ctx.mem+f.buffer-1,7) &&
              (reinterpret_cast<uintptr_t>(read_buffer)<reinterpret_cast<uintptr_t>(f.ctx.mem) ||
               reinterpret_cast<uintptr_t>(read_buffer)>=reinterpret_cast<uintptr_t>(f.ctx.mem)+f.ctx.mem_size),
              "ACTION_WRITE preserves guest data and guards and exposes no guest pointer to native DOS");
    }
    {
        Fixture f;ULONG token=f.open(),handler=f.r(token*4+8);
        action_packet(f,token,ACTION_WRITE,f.r(token*4+36),0xFFFFFFFF,0);
        action_reply(f,handler,0,0);
        check(writes==1 && written_bytes.empty(),"zero-byte ACTION_WRITE ignores its unused buffer");
    }
    for(LONG action:{ACTION_READ,ACTION_WRITE})
        for(LONG result:{-2,6})
        {
            Fixture f;ULONG token=f.open(),handler=f.r(token*4+8);
            action_packet(f,token,action,f.r(token*4+36),f.buffer,5);
            read_override=write_override=result;
            auto before=f.bytes();
            check(f.exec(366,handler,f.msg)==20 && !f.ctx.running && f.ctx.exit_code==20,
                  "an impossible native transfer result stops with rc20");
            check(f.bytes()==before && (action==ACTION_READ?reads:writes)==1 && ioerr==137,
                  "impossible native transfer counts stop with rc20 before guest data or reply writes");
        }
    for(unsigned which=0;which<11;++which)
    {
        Fixture f;ULONG token=f.open(),handler=f.r(token*4+8);
        action_packet(f,token,ACTION_WRITE,f.r(token*4+36),f.buffer,5);
        switch(which)
        {
            case 0:f.w(f.msg+40,0);break;
            case 1:case 2:case 3:case 4:
                f.w(f.msg+48+4*which,1);
                check(f.exec(366,handler,f.msg)==0xD0000000 && f.ctx.running && writes==1,
                      "ACTION_WRITE ignores dp_Arg4 to dp_Arg7");
                continue;
            case 5:f.w(f.msg+44,f.msg+44);break;
            case 6:f.w(f.msg+44,f.reply);break;
            case 7:f.w(f.msg+44,token*4);break;
            case 8:f.w(f.msg+44,0xFFFFFFFF);break;
            case 9:f.w(f.msg+48,0x80000000);break;
            case 10:allocation_countdown=0;break;
        }
        f.rejected(handler);allocation_countdown=-1;
    }
}
static void packet_seeks()
{
    Fixture f;ULONG token=f.open(),handler=f.r(token*4+8),key=f.r(token*4+36);
    struct { LONG offset,mode,result,error,next; } cases[]={
        {3,OFFSET_BEGINNING,0,0,3},{2,OFFSET_CURRENT,3,0,5},{-2,OFFSET_END,5,0,10},
        {0,OFFSET_END,10,0,12},{1,OFFSET_CURRENT,-1,219,12},{-13,OFFSET_END,-1,219,12},
        {0,OFFSET_BEGINNING,12,0,0}
    };
    for(auto c:cases)
    {
        action_packet(f,token,ACTION_SEEK,key,c.offset,c.mode);
        action_reply(f,handler,c.result,c.error);
        check(positions[open_result]==static_cast<size_t>(c.next) && seen_handle==open_result,
              "ACTION_SEEK returns the old position and leaves position unchanged on -1/219");
        check(packet_handler==f.native[2].fh_Type && packet_action==ACTION_SEEK &&
              packet_args[0]==f.native[2].fh_Args && packet_args[1]==c.offset && packet_args[2]==c.mode &&
              !packet_args[3] && !packet_args[4],
              "ACTION_SEEK goes to the native handler with its fh_Arg1, offset and mode");
    }
    check(!seeks && native_packets==std::size(cases),
          "every valid ACTION_SEEK reaches the native handler exactly once, never the buffered Seek()");
    for(unsigned which=0;which<7;++which)
    {
        Fixture bad;ULONG t=bad.open(),h=bad.r(t*4+8);
        action_packet(bad,t,ACTION_SEEK,bad.r(t*4+36),0,OFFSET_CURRENT);
        if(which<2)bad.w(bad.msg+48,which?2:static_cast<ULONG>(-2));
        else if(which==2)bad.w(bad.msg+40,0);
        else
        {
            bad.w(bad.msg+52+4*(which-3),1);
            check(bad.exec(366,h,bad.msg)==0xD0000000 && bad.ctx.running && native_packets==1,
                  "ACTION_SEEK ignores dp_Arg4 to dp_Arg7");
            continue;
        }
        bad.rejected(h);
        check(!seeks && !native_packets,"invalid seek modes and keys stop before the native handler");
    }
}
static void packet_scalar_actions()
{
    for(LONG action:{ACTION_IS_FILESYSTEM,ACTION_SCREEN_MODE,ACTION_WAIT_CHAR})
    {
        Fixture f;ULONG token=f.open(),handler=f.r(token*4+8);
        action_packet(f,token,action,static_cast<ULONG>(-7),0x12345678,0x87654321);
        action_reply(f,handler,-1,37);
        check(native_packets==1 && packet_handler==f.native[2].fh_Type && packet_action==action &&
              packet_args[0]==(action==ACTION_IS_FILESYSTEM?0:-7) && !packet_args[1] &&
              !packet_args[2] && !packet_args[3] && !packet_args[4],
              "scalar packet actions preserve the full-width handler and sign-extend only their integer argument");
    }
    for(LONG action:{0,83,1007,0x7FFFFFFF})
    {
        Fixture f;ULONG token=f.open(),handler=f.r(token*4+8);
        action_packet(f,token,action,0xFFFFFFFF);
        action_reply(f,handler,0,209);
        check(!reads && !writes && !seeks && !closes && !native_packets,
              "unknown actions, including ACTION_END, answer 0/209 without closing or invoking native DOS");
    }
}
static void packet_disk_info()
{
    constexpr ULONG out=0xA100;
    for(bool success:{true,false})
    {
        Fixture f;ULONG token=f.open(),handler=f.r(token*4+8);
        packet_info={-7,2,3,400,51,512,0x444F5301,reinterpret_cast<BPTR>(0xFEDCBA9876543210ULL),-1};
        std::memset(f.ctx.mem+out-1,0xA5,38);
        action_packet(f,token,ACTION_DISK_INFO,out/4);
        packet_result=success?-1:0;action_reply(f,handler,packet_result,37);
        const ULONG expected[]={static_cast<ULONG>(-7),2,3,400,51,512,0x444F5301,0,0xFFFFFFFF};
        for(unsigned i=0;i<9;++i)
            check(f.r(out+4*i)==(success?expected[i]:0xA5A5A5A5),
                  "ACTION_DISK_INFO writes classic big-endian InfoData with no volume node, only on success");
        check(f.ctx.mem[out-1]==0xA5 && f.ctx.mem[out+36]==0xA5 && native_packets==1 &&
              packet_handler==f.native[2].fh_Type && packet_action==ACTION_DISK_INFO &&
              !packet_args[1] && !packet_args[2] && !packet_args[3] && !packet_args[4],
              "ACTION_DISK_INFO uses a private native InfoData and preserves both guest buffer guards");
    }
    for(LONG type:{0x434F4E00,0x52415700,0x434F4E01,0x444F5300})
    {
        Fixture f;ULONG token=f.open(),handler=f.r(token*4+8);
        const bool console=type==0x434F4E00 || type==0x52415700;
        packet_info={0,0,0,0,0,0,type,reinterpret_cast<BPTR>(0x12345678),0x13579BDF};
        action_packet(f,token,ACTION_DISK_INFO,out/4);
        packet_result=-1;action_reply(f,handler,packet_result,37);
        check(f.r(out+24)==static_cast<ULONG>(type) && !f.r(out+28) &&
              f.r(out+32)==(console?0:0x13579BDFU),
              "ACTION_DISK_INFO gives 0 for the native IORequest of a CON: or RAW: console in id_InUse, and copies it for other handlers");
    }
    for(unsigned which=0;which<11;++which)
    {
        Fixture f;ULONG token=f.open(),handler=f.r(token*4+8),addr=out;
        switch(which)
        {
            case 0:addr=0;break;
            case 1:addr=f.ctx.mem_size-32;break;
            case 2:addr=f.msg+32;break;
            case 3:addr=f.reply;break;
            case 4:addr=token*4;break;
            case 5:addr=handler;break;
            case 6:f.dos(306,token);addr=f.r(token*4+12)*4;break;
            case 7:f.message(f.msg+0x100);f.exec(366,f.reply,f.msg+0x100);addr=f.msg+0x100;break;
            case 8:f.ctx.num_shadows=1;f.ctx.shadow_map[0]={out,reinterpret_cast<void *>(1),8};break;
            case 9:f.ctx.io_requests[0].guest=out;f.ctx.io_requests[0].capacity=48;break;
            case 10:f.ctx.app_ports[0].guest=out;f.ctx.app_ports[0].native=reinterpret_cast<MsgPort *>(1);break;
        }
        action_packet(f,token,ACTION_DISK_INFO,addr/4);f.rejected(handler);
        check(!native_packets,"bad ACTION_DISK_INFO buffers stop before native DoPkt");
    }
    for(ULONG bptr:{0x40000000U,0xFFFFFFFFU})
    {
        Fixture f;ULONG token=f.open(),handler=f.r(token*4+8);
        action_packet(f,token,ACTION_DISK_INFO,bptr);f.rejected(handler);
        check(!native_packets,"ACTION_DISK_INFO rejects BPTR shift overflow before native DoPkt");
    }
}
static void nil_filehandle()
{
    Fixture f;f.native[2].fh_Type=nullptr;
    ULONG token=f.open(),handler=f.r(token*4+8),key=f.r(token*4+36);
    for(LONG action:{ACTION_IS_FILESYSTEM,ACTION_SCREEN_MODE,ACTION_WAIT_CHAR,ACTION_DISK_INFO,83})
    {
        action_packet(f,token,action,0xA100/4);
        action_reply(f,handler,0,209);
    }
    check(!native_packets,"a NIL FileHandle with no native handler answers scalar and disk actions without native DoPkt");
    action_packet(f,token,ACTION_READ,key,f.buffer,5);action_reply(f,handler,5,0);
    action_packet(f,token,ACTION_WRITE,key,f.buffer,5);action_reply(f,handler,5,0);
    action_packet(f,token,ACTION_SEEK,key,0,OFFSET_BEGINNING);action_reply(f,handler,5,0);
    check(reads==1 && writes==1 && seeks==1,"a FileHandle without a native port still routes Read, Write and Seek through native DOS");
}
int main()
{
    measured_reads();unused_message_reply_port();packet_failures();identities();buffering();ownership_failures();sequential_handles_and_selected_close();all_handle_entry_points();packet_reply_queue_preflight();bounded_packet_diagnostics();application_read_then_reserved_waitport();
    packet_writes();packet_seeks();packet_scalar_actions();packet_disk_info();nil_filehandle();
    check(allocated_signals==0,"all owned native signals are released");
    check(native_blocks.empty(),"all native heap records, mirrors and packet temporaries are released");
    std::printf("Guest DOS packet boundary: %u/%u checks passed\n",checks-failures,checks);
    return failures?1:0;
}
