/* Copyright (C) 2026, The AROS Development Team. All rights reserved.

   Guest ports of native DOS handlers: DeviceProc(), GetDeviceProc(),
   FreeDeviceProc(), GetConsoleTask(), GetFileSysTask(), their Set
   functions, and the filesystem packets that go to the native handler. */
#include "message_fixture.h"
static constexpr ULONG NAME=0xC000,BSTR1=0xC100,BSTR2=0xC200,STAMP=0xC300,FIB=0xC400,INFO=0xC600;
static ULONG name(Fixture &f,ULONG a,const char *s)
{ std::strcpy(reinterpret_cast<char *>(f.ctx.mem+a),s);return a; }
static ULONG bstr(Fixture &f,ULONG a,const char *s)
{ size_t n=std::strlen(s);f.ctx.mem[a]=static_cast<UBYTE>(n);std::memcpy(f.ctx.mem+a+1,s,n);return a>>2; }
static bool empty_port(Fixture &f,ULONG p)
{
    return f.ctx.mem[p+8]==NT_MSGPORT && f.ctx.mem[p+14]==PA_IGNORE && !f.r(p+16) &&
           f.r(p+20)==p+24 && !f.r(p+24) && f.r(p+28)==p+20;
}
/* DoPkt() of the program: D0 is res1, D1 res2 */
static LONG dopkt(Fixture &f,ULONG port,LONG action,ULONG a1=0,ULONG a2=0,ULONG a3=0,ULONG a4=0,LONG *res2=nullptr)
{
    ULONG res1=f.dos(240,port,static_cast<ULONG>(action),a1,a2,a3,a4,0);
    if(res2)*res2=static_cast<LONG>(f.state.d[1]);
    return static_cast<LONG>(res1);
}
static void ports()
{
    Fixture f;
    ULONG con=f.dos(510),fs=f.dos(522);
    check(con && f.dos(510)==con && empty_port(f,con),"GetConsoleTask() is one empty PA_IGNORE port of the bridge");
    check(fs && fs!=con && empty_port(f,fs),"GetFileSysTask() has a port of its own");
    check(f.r(f.task+M68K_PR_CONSOLETASK)==con && f.r(f.task+M68K_PR_FILESYSTEMTASK)==fs,"pr_ConsoleTask and pr_FileSystemTask hold the ports");
    ULONG ram=f.dos(174,name(f,NAME,"RAM:"));
    check(ram && ram!=con && ram!=fs && f.dos(174,NAME)==ram && ioerr==0,"DeviceProc() gives one port for each native handler");
    check(f.dos(174,name(f,NAME,"WORK:"))==ram && ioerr==0x1234560,"DeviceProc() of an assign leaves the native lock in IoErr()");
    unsigned calls=deviceprocs;
    check(!f.dos(174,name(f,NAME,"NONE:")) && ioerr==ERROR_DEVICE_NOT_MOUNTED && deviceprocs==calls+1,
          "DeviceProc() of no handler is 0 with the IoErr() of native DOS");
    std::memset(f.ctx.mem+f.ctx.mem_size-4,'X',4);calls=deviceprocs;
    check(!f.dos(174,f.ctx.mem_size-4) && ioerr==ERROR_BAD_NUMBER && deviceprocs==calls && f.ctx.running,
          "a name that does not end in guest memory never reaches native DOS");
    check(!f.dos(174,0) && deviceprocs==calls+1,"a NULL name goes to native DOS");
    console_task=nullptr;
    check(!f.dos(510) && !f.r(f.task+M68K_PR_CONSOLETASK),"no native console is no port");
    for(unsigned i=3;i<32;++i)
    {
        char n[16];std::snprintf(n,sizeof n,"PORT%u:",i);
        check(f.dos(174,name(f,NAME,n))!=0,"one more handler port");
    }
    check(!f.dos(174,name(f,NAME,"PORT32:")) && ioerr==ERROR_NO_FREE_STORE,"a 33rd native handler gets no port");
    check(f.dos(174,name(f,NAME,"RAM:"))==ram,"the ports that are there stay the same");
}
static void set_tasks()
{
    Fixture f;
    ULONG con=f.dos(510),token=f.open(),fh_type=f.r(token*4+8);
    check(f.dos(516,fh_type)==con && console_task==f.native[2].fh_Type,"SetConsoleTask(fh_Type) sets the native fh_Type of the file");
    check(f.dos(510)==fh_type && f.r(f.task+M68K_PR_CONSOLETASK)==fh_type,"GetConsoleTask() then gives back that fh_Type, as on AmigaOS");
    check(f.dos(516,con)==fh_type && console_task==&native_ports[1] && f.dos(510)==con,"SetConsoleTask(previous) restores the console");
    check(f.dos(516,0)==con && !console_task && !f.dos(510),"SetConsoleTask(0)");
    check(f.dos(516,f.port)==20 && !f.ctx.running && !console_task,"SetConsoleTask() of a port that stands for no handler stops the program");
    ULONG fs=f.dos(522),ram=f.dos(174,name(f,NAME,"RAM:"));
    check(f.dos(528,ram)==fs && filesys_task==ram_port() && f.dos(522)==ram && f.r(f.task+M68K_PR_FILESYSTEMTASK)==ram,
          "SetFileSysTask() of a handler port");
    check(f.dos(528,fs)==ram && filesys_task==&native_ports[2] && f.dos(522)==fs,"SetFileSysTask(previous)");
}
static void process_fields()
{
    Fixture f;
    native_process.pr_ConsoleTask=&native_ports[1];native_process.pr_FileSystemTask=&native_ports[2];
    f.initialize_shadows();
    ULONG task=f.ctx.guest_exec_task,con=f.r(task+M68K_PR_CONSOLETASK),fs=f.r(task+M68K_PR_FILESYSTEMTASK);
    check(con && fs && con!=fs && empty_port(f,con) && empty_port(f,fs),
          "the guest Process starts with the ports of its native console and filesystem tasks");
    check(f.dos(510)==con && f.dos(522)==fs,"GetConsoleTask() and GetFileSysTask() give the same ports");
}
static void packets()
{
    Fixture f;
    ULONG ram=f.dos(174,name(f,NAME,"RAM:"));
    LONG res2=0;
    unsigned n=native_packets;
    packet_result=-1;packet_error=0;
    check(dopkt(f,ram,ACTION_IS_FILESYSTEM,0xAAAA,0xBBBB,0xCCCC,0xDDDD,&res2)==-1 && !res2 && native_packets==n+1 &&
          packet_handler==ram_port() && packet_action==ACTION_IS_FILESYSTEM && packet_args==std::array<SIPTR,5>{0,0,0,0,0},
          "ACTION_IS_FILESYSTEM goes to the native handler without arguments");
    check(dopkt(f,ram,ACTION_FLUSH,0xAAAA)==-1 && packet_action==ACTION_FLUSH && !packet_args[0],"ACTION_FLUSH");
    check(dopkt(f,ram,ACTION_SCREEN_MODE,0xFFFFFFFF)==-1 && packet_args[0]==-1,"ACTION_SCREEN_MODE takes its integer, signed");
    check(dopkt(f,ram,ACTION_WAIT_CHAR,250000)==-1 && packet_args[0]==250000,"ACTION_WAIT_CHAR takes its timeout");

    packet_result=0x7770;
    check(dopkt(f,ram,ACTION_LOCATE_OBJECT,0x5550,bstr(f,BSTR1,"dir/file"),0xFFFFFFFE)==0x7770 &&
          packet_args[0]==0x5550 && packet_name1=="dir/file" && packet_args[2]==-2 && !packet_args[3],
          "ACTION_LOCATE_OBJECT: the lock, the BSTR as a C string and the mode; the lock comes back");
    check(dopkt(f,ram,ACTION_LOCATE_OBJECT,0,0,0xFFFFFFFE)==0x7770 && packet_name1.empty() && !packet_args[0],"a BSTR of 0 is the empty name");
    check(dopkt(f,ram,ACTION_CREATE_DIR,0x5550,bstr(f,BSTR1,"sub"))==0x7770 && packet_name1=="sub" && packet_args[0]==0x5550,"ACTION_CREATE_DIR");
    packet_result=0x8880;
    check(dopkt(f,ram,ACTION_COPY_DIR,0x5550)==0x8880 && packet_args[0]==0x5550 &&
          dopkt(f,ram,ACTION_PARENT,0x6660)==0x8880 && packet_args[0]==0x6660,"ACTION_COPY_DIR and ACTION_PARENT give a lock");
    packet_result=-1;
    check(dopkt(f,ram,ACTION_DELETE_OBJECT,0x5550,bstr(f,BSTR1,"old"))==-1 && packet_name1=="old","ACTION_DELETE_OBJECT");
    check(dopkt(f,ram,ACTION_RENAME_OBJECT,0x5550,bstr(f,BSTR1,"a"),0x6660,bstr(f,BSTR2,"b"))==-1 && packet_args[0]==0x5550 &&
          packet_args[2]==0x6660 && packet_name1=="a" && packet_name2=="b","ACTION_RENAME_OBJECT: two locks and two names");
    check(dopkt(f,ram,ACTION_FREE_LOCK,0x5550)==-1 && packet_args[0]==0x5550,"ACTION_FREE_LOCK");
    check(dopkt(f,ram,ACTION_SAME_LOCK,0x5550,0x6660)==-1 && packet_args[0]==0x5550 && packet_args[1]==0x6660,"ACTION_SAME_LOCK");
    check(dopkt(f,ram,ACTION_SET_PROTECT,0x1111,0x5550,bstr(f,BSTR1,"file"),0x30)==-1 && !packet_args[0] &&
          packet_args[1]==0x5550 && packet_name1=="file" && packet_args[3]==0x30,"ACTION_SET_PROTECT: dp_Arg1 is not used");
    check(dopkt(f,ram,ACTION_SET_COMMENT,0x1111,0x5550,bstr(f,BSTR1,"file"),bstr(f,BSTR2,"a comment"))==-1 &&
          !packet_args[0] && packet_name1=="file" && packet_name2=="a comment","ACTION_SET_COMMENT");
    f.w(STAMP,7000);f.w(STAMP+4,600);f.w(STAMP+8,1000);
    check(dopkt(f,ram,ACTION_SET_DATE,0x1111,0x5550,bstr(f,BSTR1,"file"),STAMP)==-1 && packet_name1=="file" &&
          packet_stamp.ds_Days==7000 && packet_stamp.ds_Minute==600 && packet_stamp.ds_Tick==1000,
          "ACTION_SET_DATE reads the DateStamp of the program");
    std::string longest(255,'n');
    check(dopkt(f,ram,ACTION_DELETE_OBJECT,0x5550,bstr(f,BSTR1,longest.c_str()))==-1 && packet_name1==longest,"a BSTR of 255 characters");

    packet_fib={};packet_fib.fib_DiskKey=0x13579;packet_fib.fib_DirEntryType=-3;
    packet_fib.fib_FileName[0]=4;std::memcpy(packet_fib.fib_FileName+1,"file",4);
    packet_fib.fib_Size=10;packet_fib.fib_Protection=0x30;packet_fib.fib_Date={7000,600,1000};
    packet_fib.fib_Comment[0]=1;packet_fib.fib_Comment[1]='c';
    std::memset(f.ctx.mem+FIB,0xEE,M68K_SIZEOF_FILEINFOBLK);
    check(dopkt(f,ram,ACTION_EXAMINE_OBJECT,0x5550,FIB>>2)==-1 && packet_args[0]==0x5550,"ACTION_EXAMINE_OBJECT");
    check(!packet_fib_in.fib_DiskKey && !packet_fib_in.fib_FileName[0],"ACTION_EXAMINE_OBJECT gives the handler a cleared FileInfoBlock");
    check(f.r(FIB)==0x13579 && f.r(FIB+4)==0xFFFFFFFD && f.ctx.mem[FIB+8]==4 && !std::memcmp(f.ctx.mem+FIB+9,"file",4) &&
          f.r(FIB+116)==0x30 && f.r(FIB+124)==10 && f.r(FIB+132)==7000 && f.r(FIB+136)==600 && f.r(FIB+140)==1000 &&
          f.ctx.mem[FIB+144]==1 && f.ctx.mem[FIB+145]=='c',
          "the guest FileInfoBlock gets the entry, its name and comment BSTRs");
    packet_fib.fib_DiskKey=0x2468;f.w(FIB,0x97531);
    check(dopkt(f,ram,ACTION_EXAMINE_NEXT,0x5550,FIB>>2)==-1 && packet_fib_in.fib_DiskKey==0x97531 &&
          packet_fib_in.fib_FileName[0]==4 && packet_fib_in.fib_Size==10 && packet_fib_in.fib_Comment[1]=='c',
          "ACTION_EXAMINE_NEXT goes on from the guest FileInfoBlock");
    check(f.r(FIB)==0x2468,"and gives the next entry");
    std::vector<UBYTE> fib_before(f.ctx.mem+FIB,f.ctx.mem+FIB+M68K_SIZEOF_FILEINFOBLK);
    packet_result=0;packet_error=ERROR_NO_MORE_ENTRIES;
    check(dopkt(f,ram,ACTION_EXAMINE_NEXT,0x5550,FIB>>2,0,0,&res2)==0 && res2==ERROR_NO_MORE_ENTRIES &&
          std::equal(fib_before.begin(),fib_before.end(),f.ctx.mem+FIB),"a failed ACTION_EXAMINE_NEXT leaves the guest FileInfoBlock alone");

    packet_result=-1;packet_error=0;packet_info={};
    packet_info.id_NumBlocks=100;packet_info.id_BytesPerBlock=1024;packet_info.id_DiskType=0x444F5301;
    packet_info.id_VolumeNode=reinterpret_cast<BPTR>(0x123456789ULL);packet_info.id_InUse=5;
    std::memset(f.ctx.mem+INFO,0xEE,36);
    check(dopkt(f,ram,ACTION_INFO,0x5550,INFO>>2)==-1 && packet_args[0]==0x5550 && f.r(INFO+12)==100 && f.r(INFO+20)==1024 &&
          f.r(INFO+24)==0x444F5301 && !f.r(INFO+28) && f.r(INFO+32)==5,"ACTION_INFO fills the guest InfoData without the volume node");
    packet_info.id_DiskType=0x434F4E00;packet_info.id_InUse=0x13579BDF;
    check(dopkt(f,ram,ACTION_DISK_INFO,INFO>>2)==-1 && f.r(INFO+24)==0x434F4E00 && !f.r(INFO+32),
          "ACTION_DISK_INFO of a console: no native IORequest in id_InUse");

    n=native_packets;
    check(!dopkt(f,ram,ACTION_CURRENT_VOLUME,0,0,0,0,&res2) && res2==ERROR_ACTION_NOT_KNOWN && native_packets==n,
          "an action that the bridge does not convert never reaches the handler");
    check(!dopkt(f,ram,12345,0,0,0,0,&res2) && res2==ERROR_ACTION_NOT_KNOWN && native_packets==n,"nor does an unknown one");
}
/* A StandardPacket of the program at f.msg, its reply port f.reply */
static void packet_msg(Fixture &f,LONG action)
{
    f.message(f.msg,0,68);f.ctx.mem[f.msg+8]=5;f.w(f.msg+10,f.msg+20);f.w(f.msg+20,f.msg);
    f.w(f.msg+24,f.reply);f.w(f.msg+28,static_cast<ULONG>(action));
    for(ULONG a=32;a<68;a+=4)f.w(f.msg+a,0);
}
static void put_packet()
{
    Fixture f;
    ULONG ram=f.dos(174,name(f,NAME,"RAM:"));
    packet_msg(f,ACTION_IS_FILESYSTEM);
    ioerr=137;packet_result=-1;packet_error=55;
    check(f.exec(366,ram,f.msg)==0xD0000000 && f.ctx.running,"PutMsg() of a packet to a handler port");
    check(f.r(f.reply+20)==f.msg && f.ctx.mem[f.msg+8]==5 && f.r(f.msg+24)==ram && f.r(f.msg+32)==0xFFFFFFFF &&
          f.r(f.msg+36)==55 && ioerr==137,
          "the reply waits on the reply port, type 5, dp_Port the handler port, the results set and IoErr() kept");
    check(f.exec(372,f.reply)==f.msg,"GetMsg() takes the reply");
}
static void packet_failures()
{
    for(unsigned which=0;which<10;++which)
    {
        Fixture f;
        ULONG ram=f.dos(174,name(f,NAME,"RAM:")),res=0,top=f.ctx.mem_size-4;
        unsigned n=native_packets+(which==9),u=unlocks;
        packet_result=-1;
        switch(which)
        {
            case 0:res=f.dos(240,ram,ACTION_LOCATE_OBJECT,0,0x40000000,0);break;
            case 1:f.ctx.mem[top]=200;res=f.dos(240,ram,ACTION_LOCATE_OBJECT,0,top>>2,0);break;
            case 2:res=f.dos(240,ram,ACTION_EXAMINE_OBJECT,0,(f.ctx.mem_size-100)>>2);break;
            case 3:res=f.dos(240,ram,ACTION_INFO,0,(f.ctx.mem_size-20)>>2);break;
            case 4:res=f.dos(240,ram,ACTION_SET_DATE,0,0,bstr(f,BSTR1,"file"),top);break;
            case 5:res=f.dos(240,ram,ACTION_DISK_INFO,0x40000000);break;
            case 6:f.ctx.mem[ram+15]=3;res=f.dos(240,ram,ACTION_IS_FILESYSTEM);break;
            case 7:packet_msg(f,ACTION_IS_FILESYSTEM);f.w(f.exec_base+276,f.task+0x400);res=f.exec(366,ram,f.msg);break;
            case 8:res=f.exec(372,ram);break;
            case 9:
                packet_result=static_cast<SIPTR>(0x100000000ULL);
                res=f.dos(240,ram,ACTION_LOCATE_OBJECT,0,bstr(f,BSTR1,"x"),0);
                check(unlocks==u+1 && unlocked==reinterpret_cast<BPTR>(0x100000000ULL),"a native lock past 32 bits is given back");
                break;
        }
        check(res==20 && !f.ctx.running && f.ctx.exit_code==20 && native_packets==n,"an invalid handler packet stops the program before the handler");
    }
    Fixture f;
    ULONG ram=f.dos(174,name(f,NAME,"RAM:"));
    packet_result=-1;packet_fib={};packet_fib.fib_DiskKey=static_cast<IPTR>(0x100000000ULL);
    check(f.dos(240,ram,ACTION_EXAMINE_OBJECT,0,FIB>>2)==20 && !f.ctx.running,"a native DiskKey past 32 bits stops the program");
}
static void devprocs()
{
    Fixture f;
    ULONG ram=f.dos(174,name(f,NAME,"RAM:"));
    ULONG dp=f.dos(642,name(f,NAME,"RAM:"),0);
    check(dp && f.r(dp)==ram && !f.r(dp+4) && !f.r(dp+8) && !f.r(dp+12),
          "GetDeviceProc(RAM:): the handler port, no lock, no flags and no DosList node");
    f.dos(648,dp);
    check(devproc_frees==1,"FreeDeviceProc() frees the native DevProc");
    f.dos(648,dp);f.dos(648,f.port);f.dos(648,0);
    check(devproc_frees==1 && f.ctx.running,"FreeDeviceProc() of anything else does nothing");
    dp=f.dos(642,name(f,NAME,"MULTI:"),0);
    check(dp && f.r(dp)==ram && f.r(dp+4)==0x1000 && f.r(dp+8)==DVPF_ASSIGN,"the first drawer of a multi-assign");
    check(f.dos(642,NAME,dp)==dp && f.r(dp+4)==0x2000,"GetDeviceProc() with the DevProc moves on to the next drawer");
    ioerr=137;
    check(!f.dos(642,NAME,dp) && ioerr==ERROR_NO_MORE_ENTRIES,"after the last drawer native DOS frees the DevProc, with its IoErr()");
    f.dos(648,dp);
    check(devproc_frees==1 && f.ctx.running,"and the guest DevProc went with it");
    check(f.dos(642,NAME,f.port)==20 && !f.ctx.running,"GetDeviceProc() with a DevProc that is not one stops the program");
    unsigned frees=devproc_frees;
    check(!f.dos(642,name(f,NAME,"BIG:"),0) && ioerr==ERROR_NO_FREE_STORE && devproc_frees==frees+1,
          "a lock past 32 bits: the native DevProc is freed, 0 and ERROR_NO_FREE_STORE");
    check(!f.dos(642,name(f,NAME,"NONE:"),0) && ioerr==ERROR_DEVICE_NOT_MOUNTED,"GetDeviceProc() of no device");
    for(unsigned i=0;i<KAFFEINE_MAX_DEVPROCS;++i)check(f.dos(642,name(f,NAME,"RAM:"),0)!=0,"one more DevProc");
    check(!f.dos(642,NAME,0) && ioerr==ERROR_NO_FREE_STORE,"one DevProc too many fails");
    frees=devproc_frees;
    Kaffeine_HandlerCleanup(&f.ctx);
    check(devproc_frees==frees+KAFFEINE_MAX_DEVPROCS,"cleanup frees the native DevProcs left");
}
int main()
{
    ports();set_tasks();process_fields();packets();put_packet();packet_failures();devprocs();
    check(native_blocks.empty(),"all native DevProcs, FileInfoBlocks and packet temporaries are released");
    std::printf("Guest DOS handler ports: %u/%u checks passed\n",checks-failures,checks);
    return failures?1:0;
}
