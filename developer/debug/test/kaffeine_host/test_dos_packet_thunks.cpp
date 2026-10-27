/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "message_fixture.h"

static void standard_packets()
{
    Fixture f;
    const ULONG available=Kaffeine_HeapAvail(&f.ctx,0);
    std::array<ULONG,32> packets={};
    for(unsigned i=0;i<packets.size();++i)
    {
        ULONG dp=f.dos(228,DOS_STDPKT,0xFFFFFFFF),msg=dp-20;
        packets[i]=dp;
        check(dp && !(dp&3) && f.ctx.stdpkt_map[i]==msg,"DOS_STDPKT returns sp_Pkt and records its Message");
        std::array<UBYTE,68> expected={};
        for(unsigned j=0;j<4;++j)
        {
            expected[10+j]=static_cast<UBYTE>(dp>>(24-8*j));
            expected[20+j]=static_cast<UBYTE>(msg>>(24-8*j));
        }
        check(!std::memcmp(f.ctx.mem+msg,expected.data(),expected.size()),"StandardPacket is cleared with ln_Name, dp_Link and zero mn_Length");
        check(ioerr==137,"successful DOS_STDPKT allocation preserves IoErr and ignores tags");
    }
    auto before=f.bytes();ULONG full=Kaffeine_HeapAvail(&f.ctx,0);
    check(f.dos(228,DOS_STDPKT)==0 && ioerr==ERROR_NO_FREE_STORE && f.bytes()==before &&
          Kaffeine_HeapAvail(&f.ctx,0)==full,"33rd StandardPacket fails without an allocation or guest write");
    for(ULONG unknown:{0U,20U,21U,0xFFFFFFFFU,packets[0]-20,packets[0]+1})
        check(f.dos(234,DOS_STDPKT,unknown)==0 && f.bytes()==before &&
              Kaffeine_HeapAvail(&f.ctx,0)==full,"FreeDosObject ignores unknown StandardPacket pointers");
    check(f.dos(234,DOS_STDPKT,packets[7])==0 && !f.ctx.stdpkt_map[7],"FreeDosObject releases exactly its owned packet slot");
    ULONG freed=Kaffeine_HeapAvail(&f.ctx,0);
    f.dos(234,DOS_STDPKT,packets[7]);
    check(Kaffeine_HeapAvail(&f.ctx,0)==freed,"double FreeDosObject does not free twice");
    packets[7]=f.dos(228,DOS_STDPKT);
    check(packets[7] && f.ctx.stdpkt_map[7]==packets[7]-20,"a freed StandardPacket table entry is reusable");
    for(ULONG dp:packets) f.dos(234,DOS_STDPKT,dp);
    check(Kaffeine_HeapAvail(&f.ctx,0)==available &&
          std::all_of(std::begin(f.ctx.stdpkt_map),std::end(f.ctx.stdpkt_map),[](ULONG p){return !p;}),
          "all 32 packets return their storage and clear the ownership table");
    allocation_countdown=0;
    check(f.dos(228,DOS_STDPKT)==0 && ioerr==ERROR_NO_FREE_STORE && !f.ctx.stdpkt_map[0],
          "heap allocation failure does not occupy a StandardPacket slot");
    allocation_countdown=-1;
}

static void send_and_wait()
{
    Fixture f;ULONG token=f.open(),handler=f.r(token*4+8),mine=f.process_port();
    ULONG dp=f.dos(228,DOS_STDPKT),msg=dp-20;
    f.w(dp+8,ACTION_READ);f.w(dp+20,f.r(token*4+36));f.w(dp+24,f.buffer);f.w(dp+28,5);
    check(f.dos(246,dp,handler,mine)==0 && f.ctx.running,"SendPkt reaches an owned FileHandle handler");
    check(f.r(dp)==msg && f.r(msg+10)==dp && f.r(msg+14)==mine && f.r(dp+4)==handler,
          "SendPkt and handler reply retain identities, set mn_ReplyPort and leave dp_Port the handler");
    check(f.ctx.mem[msg+8]==5 && m68k_read16(&f.ctx,msg+18)==68 && f.r(dp+12)==5 && !f.r(dp+16),
          "handler replies with PutMsg type5, normalized 68-byte message and byte count");
    check(reads==1 && !std::memcmp(f.ctx.mem+f.buffer,"ABCDE",5) && ioerr==137,
          "SendPkt copies read bytes while leaving IoErr alone");
    check(signals==1 && signalled_mask==SIGF_DOS && !allocated_signals && !f.ctx.guest_signals,
          "the process packet port uses SIGB_DOS without allocating it");
    check(f.dos(252)==dp && ioerr==137 && f.r(mine+20)==mine+24 && f.r(mine+28)==mine+20,
          "WaitPkt returns ln_Name, drains the process queue and preserves IoErr");
    f.dos(234,DOS_STDPKT,dp);

    // WaitPkt returns ln_Name even when it is not a contiguous DosPacket.
    f.message(f.msg,mine,20);f.w(f.msg+10,0xB246);
    f.exec(366,mine,f.msg);
    check(f.dos(252)==0xB246 && ioerr==137,"WaitPkt returns an arbitrary queued message's ln_Name");

    const ULONG separate=0xB000;
    f.message(f.msg,0,20);std::memset(f.ctx.mem+separate,0,48);
    m68k_write16(&f.ctx,f.msg+18,0);f.w(separate,f.msg);f.w(f.msg+10,separate);
    check(f.dos(246,separate,f.port,mine)==0 && f.r(separate+4)==mine && f.r(f.msg+14)==mine,
          "SendPkt writes both reply fields before delivery to a guest port");
    check(m68k_read16(&f.ctx,f.msg+18)==20 && f.exec(372,f.port)==f.msg && ioerr==137,
          "a noncontiguous packet normalizes only its bare 20-byte Message");
}

static void private_dopkt()
{
    for(bool pending:{false,true})
    {
        Fixture f;ULONG token=f.open(),handler=f.r(token*4+8),mine=f.process_port();
        if(pending)
        {
            f.message(f.msg,0,68);f.w(f.msg+10,0xB000);
            f.message(f.msg+0x100,0,20);f.w(f.msg+0x10A,0xB100);
            f.exec(366,mine,f.msg);f.exec(366,mine,f.msg+0x100);
        }
        std::array<UBYTE,34> port_before;
        std::memcpy(port_before.data(),f.ctx.mem+mine,34);
        const auto messages_before=f.bytes();
        const ULONG available=Kaffeine_HeapAvail(&f.ctx,0),n=signals;
        check(f.dos(240,handler,ACTION_READ,f.r(token*4+36),f.buffer,5)==5 && f.state.d[1]==0 && ioerr==0,
              "DoPkt returns dp_Res1 in D0 and dp_Res2 in D1 and IoErr");
        check(Kaffeine_HeapAvail(&f.ctx,0)==available && signals==n &&
              !std::memcmp(port_before.data(),f.ctx.mem+mine,34),
              "DoPkt frees its private PA_IGNORE reply block and leaves pr_MsgPort unchanged");
        if(pending)
        {
            check(!std::memcmp(messages_before.data()+f.msg,f.ctx.mem+f.msg,68) &&
                  !std::memcmp(messages_before.data()+f.msg+0x100,f.ctx.mem+f.msg+0x100,20),
                  "DoPkt leaves preexisting replies and their queue links untouched");
            check(f.dos(252)==0xB000 && f.dos(252)==0xB100,"WaitPkt still receives both older replies in FIFO order");
        }
        read_override=-1;
        check(f.dos(240,handler,ACTION_READ,f.r(token*4+36),f.buffer,5)==0xFFFFFFFF &&
              f.state.d[1]==static_cast<ULONG>(-123) && ioerr==-123 &&
              Kaffeine_HeapAvail(&f.ctx,0)==available,"DoPkt propagates signed native errors and frees its private port on an error reply");
        check(f.dos(240,handler,0x12345678,11,22,33,44,55)==0 &&
              f.state.d[1]==209 && ioerr==209,"DoPkt answers unknown actions with D0=0, D1=IoErr=209");
    }
    {
        Fixture f;ULONG token=f.open(),handler=f.r(token*4+8);
        allocation_countdown=0;
        check(f.dos(240,handler,ACTION_READ,1,2,3)==0 && f.state.d[1]==ERROR_NO_FREE_STORE &&
              ioerr==ERROR_NO_FREE_STORE && reads==0 && f.ctx.running,"DoPkt allocation failure reports D1 and IoErr without sending");
        allocation_countdown=-1;
    }
}

static void reply_ping_pong_and_abort()
{
    Fixture f;ULONG dp=f.msg+20,mine=f.process_port();
    f.packet(f.open());f.w(dp+4,mine);
    f.dos(246,dp,f.port,mine);
    check(f.exec(372,f.port)==f.msg,"guest handler receives the message sent by SendPkt");
    check(f.dos(258,dp,0xFEDCBA98,0x87654321)==0 && f.r(dp+4)==mine &&
          f.r(dp+12)==0xFEDCBA98 && f.r(dp+16)==0x87654321 && f.ctx.mem[f.msg+8]==5,
          "ReplyPkt writes results and the caller's process port, and uses PutMsg");
    check(f.dos(252)==dp && ioerr==137,"WaitPkt receives ReplyPkt without adopting its res2 as IoErr");
    f.w(dp+4,f.port);
    check(f.dos(258,dp,7,9)==0 && f.r(dp+4)==mine && f.r(f.port+20)==f.msg && f.r(f.msg+14)==mine,
          "ReplyPkt sends to the old dp_Port and replaces it with pr_MsgPort without changing mn_ReplyPort");
    check(f.exec(372,f.port)==f.msg,"guest ping-pong returns the same packet message");
    // Bounce it from the guest port to the process again.
    check(f.dos(258,dp,11,13)==0 && f.dos(252)==dp && f.r(dp+12)==11 && f.r(dp+16)==13,
          "ReplyPkt can repeatedly exchange a dequeued packet through guest queues");
    auto before=f.bytes();ULONG count=signals;
    check(f.dos(264,0xFFFFFFFF,0xFFFFFFFD)==0 && f.ctx.running && f.bytes()==before &&
          ioerr==137 && signals==count,"AbortPkt is a no-op even with invalid pointers");
    f.dos(246,dp,f.port,mine);before=f.bytes();count=signals;
    check(f.dos(264,f.port,dp)==0 && f.bytes()==before && signals==count &&
          f.exec(372,f.port)==f.msg,"AbortPkt neither unqueues nor modifies an in-flight packet");
}

static void nil_packets()
{
    for(LONG action:{ACTION_READ,ACTION_WRITE,ACTION_SEEK,ACTION_DISK_INFO,0x12345678})
    {
        Fixture f;LONG result=action==ACTION_WRITE?37:action==ACTION_READ?0:-1;
        auto before=f.bytes();ULONG available=Kaffeine_HeapAvail(&f.ctx,0);
        check(f.dos(240,0,action,0xFFFFFFF1,0xFFFFFFF2,37,0xFFFFFFF4,0xFFFFFFF5)==static_cast<ULONG>(result) &&
              f.state.d[1]==0 && ioerr==0 && f.bytes()==before && Kaffeine_HeapAvail(&f.ctx,0)==available,
              "DoPkt to NULL implements NIL WRITE count, READ EOF and DOSTRUE for other actions without allocation");
        f.packet(f.open());ULONG dp=f.msg+20,mine=f.process_port();
        f.w(dp+8,action);f.w(dp+28,37);m68k_write16(&f.ctx,f.msg+18,0);ioerr=137;
        check(f.dos(246,dp,0,mine)==0 && f.r(dp+4)==0 && f.r(f.msg+14)==mine &&
              f.r(dp+12)==static_cast<ULONG>(result) && !f.r(dp+16) && ioerr==137 &&
              f.ctx.mem[f.msg+8]==5 && m68k_read16(&f.ctx,f.msg+18)==68 && f.dos(252)==dp,
              "SendPkt to NULL queues a normalized NIL reply, clears dp_Port and preserves IoErr");
        check(!reads && !writes && !seeks && !native_packets,"NULL packet ports never reach native I/O");
    }
}

static void packet_failures()
{
    for(unsigned which=0;which<5;++which)
    {
        Fixture f;
        if(which)
        {
            f.w(f.msg+10,f.msg+20);
            f.exec(366,f.process_port(),f.msg);
        }
        switch(which)
        {
            case 1:f.w(f.task+M68K_PR_PKTWAIT,0x1234);break;
            case 2:f.w(f.exec_base+M68K_EXECBASE_THISTASK,f.task+0x100);break;
            case 3:f.ctx.guest_exec_task=0;break;
            case 4:f.ctx.m68k_sysbase=f.ctx.mem_size-4;break;
        }
        auto before=f.bytes();
        check(f.dos(252)==20 && !f.ctx.running && f.ctx.exit_code==20 && f.bytes()==before && ioerr==137,
              "WaitPkt stops with rc20 on an empty queue, pr_PktWait or outside the program's process");
    }
    for(ULONG lvo:{246U,258U})
        for(ULONG bad:{0U,1U,0xFFFFFFFFU,0xFFFFFFFEU,0x1FFFF0U})
        {
            Fixture f;auto before=f.bytes();
            check(f.dos(lvo,bad,f.port,f.reply)==20 && !f.ctx.running && f.ctx.exit_code==20 && f.bytes()==before,
                  "SendPkt and ReplyPkt reject bad DosPacket pointers with rc20 before writes");
        }
    for(ULONG lvo:{246U,258U})
        for(ULONG bad:{0U,1U,0xFFFFFFFEU,0x1FFFF0U})
        {
            Fixture f;f.w(f.msg+20,bad);auto before=f.bytes();
            check(f.dos(lvo,f.msg+20,f.port,f.reply)==20 && !f.ctx.running && f.ctx.exit_code==20 && f.bytes()==before,
                  "SendPkt and ReplyPkt reject bad dp_Link message pointers before writes");
        }
    {
        Fixture f;f.packet(f.open());f.w(f.msg+24,0);auto before=f.bytes();
        check(f.dos(258,f.msg+20,1,2)==20 && !f.ctx.running && f.ctx.exit_code==20 && f.bytes()==before,
              "ReplyPkt to no dp_Port stops with rc20 before changing results");
    }
    {
        Fixture f;auto before=f.bytes();
        check(f.dos(240,f.port,ACTION_READ,11,22,33,44,55)==20 && !f.ctx.running && f.ctx.exit_code==20,
              "DoPkt to a guest port with no immediate answer stops with rc20");
        ULONG msg=f.r(f.port+20),dp=msg+20;
        check(msg!=f.port+24 && f.r(dp+8)==ACTION_READ && f.r(dp+20)==11 && f.r(dp+24)==22 &&
              f.r(dp+28)==33 && f.r(dp+32)==44 && f.r(dp+36)==55 && f.r(dp+40)==0,
              "DoPkt reads D2 through D7 into its action and five arguments, with remaining arguments zero");
        check(!std::memcmp(before.data()+f.process_port(),f.ctx.mem+f.process_port(),34),
              "unsupported delayed DoPkt still leaves pr_MsgPort untouched");
    }
}

static void process_signal_scope()
{
    {
        Fixture f;ULONG mine=f.process_port();auto before=f.ctx.guest_signals;
        check(f.exec(366,mine,f.msg)==0xD0000000 && signals==1 && signalled_mask==SIGF_DOS &&
              f.ctx.guest_signals==before,"queue_arrival accepts SIGB_DOS on the program's process port");
        f.exec(372,mine);
    }
    for(unsigned which=0;which<5;++which)
    {
        Fixture f;ULONG p=f.process_port();
        switch(which)
        {
            case 0:p=f.port;f.manual_port(p,PA_SIGNAL,f.task,SIGB_DOS);break;
            case 1:f.ctx.mem[p+15]=SIGB_DOS+1;break;
            case 2:f.w(p+16,f.task+0x100);break;
            case 3:f.w(f.exec_base+M68K_EXECBASE_THISTASK,f.task+0x100);break;
            case 4:f.ctx.io_ports[0].guest=p;f.ctx.io_ports[0].sigbit=SIGB_DOS+1;break;
        }
        f.rejected(p);
        if(which==4) f.ctx.io_ports[0]={};
    }
    {
        Fixture f;f.initialize_shadows();ULONG p=f.process_port();
        check(f.ctx.mem[p+8]==NT_MSGPORT && f.ctx.mem[p+14]==PA_SIGNAL && f.ctx.mem[p+15]==SIGB_DOS &&
              f.r(p+16)==f.ctx.guest_exec_task && f.r(p+20)==p+24 && !f.r(p+24) && f.r(p+28)==p+20,
              "the real shadow builder initializes the Process packet port and its empty list");
        // Keep this regression executable with the actual Task shadow registered.
        check(f.exec(366,p,f.msg)==0xD0000000 && f.ctx.running,
              "the real shadow-built pr_MsgPort is usable by guest queues");
    }
}

int main()
{
    standard_packets();send_and_wait();private_dopkt();reply_ping_pong_and_abort();nil_packets();
    packet_failures();process_signal_scope();
    check(!allocated_signals && native_blocks.empty(),"all native allocations and owned signals are released");
    std::printf("DOS packet thunks: %u/%u checks passed\n",checks-failures,checks);
    return failures?1:0;
}
