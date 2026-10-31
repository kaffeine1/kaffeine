/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#include <graphics/displayinfo.h>
#include <proto/graphics.h>
#include "message_fixture.h"
#include "kaffeine_display.h"

static unsigned queries;
static MonitorSpec native_monitors[KAFFEINE_MAX_MONITORS + 1];
static unsigned monitor_choice;
static bool null_monitor;
static char monitor_name[] = "original.monitor";
static char too_long_monitor_name[512];
static void reset_monitors()
{
    monitor_choice=0; null_monitor=false;
    for(auto &m:native_monitors)
    {
        std::memset(&m,0xBD,sizeof(m));
        m.ms_Node.xln_Type=18; m.ms_Node.xln_Pri=-7; m.ms_Node.xln_Name=monitor_name;
        m.ms_Node.xln_Subsystem=2; m.ms_Node.xln_Subtype=4;
        m.ms_Flags=0x1234; m.ratioh=-123; m.ratiov=0x12345678;
        m.total_rows=312; m.total_colorclocks=226; m.DeniseMaxDisplayColumn=455;
        m.BeamCon0=0x20; m.min_row=29; m.ms_OpenCount=3;
        m.ms_xoffset=9; m.ms_yoffset=0; m.ms_LegalView={-2,-3,135,56};
        m.DeniseMinDisplayColumn=93; m.DisplayCompatible=0x3456789A;
    }
}
static LONG backend_result = -1;
static ULONG backend_id, backend_tag;
extern "C" ULONG GetDisplayInfoData(DisplayInfoHandle h, UBYTE *buf, ULONG size, ULONG tag, ULONG id)
{
    ++queries; backend_id = id; backend_tag = tag;
    assert(!h);
    std::memset(buf, 0xBD, size); // Poison padding and reserved native pointers.
    QueryHeader *q;
    if (tag == DTAG_DISP)
    {
        assert(size == sizeof(DisplayInfo)); auto *d = reinterpret_cast<DisplayInfo *>(buf); q = &d->Header;
        d->NotAvailable=0x0102; d->PropertyFlags=0x03040506;
        d->Resolution={-2,0x090A}; d->PixelSpeed=0x0B0C;
        d->NumStdSprites=0x0D0E; d->PaletteRange=0x0F10;
        d->SpriteResolution={0x1112,0x1314}; d->RedBits=0x15; d->GreenBits=0x16; d->BlueBits=0x17;
    }
    else if (tag == DTAG_DIMS)
    {
        assert(size == sizeof(DimensionInfo)); auto *d = reinterpret_cast<DimensionInfo *>(buf); q = &d->Header;
        d->MaxDepth=0x0102; d->MinRasterWidth=0x0304; d->MinRasterHeight=0x0506;
        d->MaxRasterWidth=0x0708; d->MaxRasterHeight=0x090A;
        d->Nominal={-2,-3,0x1234,0x5678}; d->MaxOScan={0x1112,0x1314,0x1516,0x1718};
        d->VideoOScan={0x2122,0x2324,0x2526,0x2728}; d->TxtOScan={0x3132,0x3334,0x3536,0x3738};
        d->StdOScan={0x4142,0x4344,0x4546,0x4748};
    }
    else if(tag==DTAG_MNTR)
    {
        assert(size==sizeof(MonitorInfo)); auto *m=reinterpret_cast<MonitorInfo *>(buf); q=&m->Header;
        m->Mspc=null_monitor?nullptr:&native_monitors[monitor_choice];
        m->ViewPosition={-1,44}; m->ViewResolution={22,33}; m->ViewPositionRange={-2,-3,640,480};
        m->TotalRows=312; m->TotalColorClocks=226; m->MinRow=29; m->Compatibility=-1;
        m->MouseTicks={22,22}; m->DefaultViewPosition={129,44}; m->PreferredModeID=0x23456789;
    }
    else
    {
        assert(tag == DTAG_NAME && size == sizeof(NameInfo));
        auto *d = reinterpret_cast<NameInfo *>(buf); q = &d->Header;
        for (unsigned i=0;i<32;i++) d->Name[i]=0x41+i;
    }
    *q={tag,0x13579BDF,0xBADC0DE,0xFEEDFACE};
    return backend_result < 0 ? size : static_cast<ULONG>(backend_result);
}
static const M68KThunkEntry manual[] = { KAFFEINE_GRAPHICS_DISPLAY_THUNKS };
static const M68KThunkEntry generated[] = { {714,poison}, {720,poison}, {726,poison}, {756,poison} };
struct DisplayFixture:Fixture
{
    DisplayFixture()
    {
        queries=0; backend_result=-1; reset_monitors();
        auto &lib=ctx.libs[1]; lib.thunks=manual; lib.num_thunks=std::size(manual);
        lib.gen_thunks=generated; lib.num_gen_thunks=std::size(generated); std::strcpy(lib.name,"graphics.library");
    }
    ULONG query(ULONG size,ULONG tag=DTAG_DIMS,ULONG out=0x6801,ULONG handle=0,ULONG lvo=756)
    {
        for (unsigned i=0;i<8;i++) { state.d[i]=0xD0000000+i; state.a[i]=0xA0000000+i; }
        state.d[0]=size; state.d[1]=tag; state.d[2]=0x12345678;
        state.a[0]=handle; state.a[1]=out; state.a[7]=stack;
        state.sr=0xA715; state.pc=0x12345678; auto saved=state;
        ctx.running=TRUE; ctx.exit_code=0; KaffeineDispatchResult result={};
        check(Kaffeine_DispatchLibraryCall(&ctx,&cpu,dos_base-lvo,&result),"literal graphics LVO dispatch");
        check(result.return_pc==0x1234ABCD && state.a[7]==stack+4,"display query return ABI");
        check(state.sr==saved.sr && state.pc==saved.pc,"display query preserves SR and PC");
        for(unsigned i=1;i<8;i++) check(state.d[i]==saved.d[i],"non-result D register preserved");
        for(unsigned i=0;i<7;i++) check(state.a[i]==saved.a[i],"A register preserved");
        check(!generated_calls,"generated display fallback never reached");
        return state.d[0];
    }
    void reject(ULONG size,ULONG tag=DTAG_DIMS,ULONG out=0x6801,ULONG handle=0,ULONG lvo=756)
    {
        auto before=bytes(); unsigned n=queries;
        check(query(size,tag,out,handle,lvo)==20 && !ctx.running && ctx.exit_code==20,"unsupported display query stops20");
        check(bytes()==before && queries==n,"rejected query preserves whole arena and never calls native backend");
    }
};
static std::vector<UBYTE> expected(ULONG tag)
{
    std::vector<UBYTE> v={0x80,0,0,0,0x13,0x57,0x9B,0xDF,0,0,0,3,0,0,0,4};
    if(tag==DTAG_DIMS)
    {
        v[2]=0x10; v[15]=8;
        const UBYTE body[]={1,2,3,4,5,6,7,8,9,10,0xFF,0xFE,0xFF,0xFD,0x12,0x34,0x56,0x78,
            0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,
            0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48};
        v.insert(v.end(),std::begin(body),std::end(body));
    }
    else if(tag==DTAG_DISP)
    {
        const UBYTE body[]={1,2,3,4,5,6,0xFF,0xFE,9,10,11,12,13,14,15,16,17,18,19,20,0,0,0,0,21,22,23,0,0,0,0,0};
        v.insert(v.end(),std::begin(body),std::end(body));
    }
    else { v[2]=0x30; for(unsigned i=0;i<32;i++) v.push_back(0x41+i); }
    return v;
}
int main()
{
    for(ULONG tag:{DTAG_DISP,DTAG_DIMS,DTAG_NAME})
    {
        DisplayFixture f; auto want=expected(tag);
        for(ULONG size=0;size<=104;size++)
        {
            std::memset(f.ctx.mem+0x67F9,0xA5,128);
            ULONG n=std::min<ULONG>(size,want.size());
            if(tag==DTAG_DIMS && n>26) n=26+(n-26)/8*8;
            unsigned calls=queries;
            check(f.query(size,tag)==n,"native-measured classic transfer count");
            check(queries==calls+(n?1:0),"zero size makes no native query");
            check(!n || (backend_id==0x12345678 && backend_tag==tag),"mode ID and tag use D2/D1");
            check(!std::memcmp(f.ctx.mem+0x6801,want.data(),n),"golden big-endian field bytes");
            bool intact=true;
            for(ULONG i=0;i<128;i++) if((i<8 || i>=8+n) && f.ctx.mem[0x67F9+i]!=0xA5) intact=false;
            check(intact,"all prefix, reserved tail and suffix guards preserved");
        }
        std::memset(f.ctx.mem+0x6801,0xA5,100); backend_result=0;
        check(f.query(88,tag)==0 && f.ctx.running,"missing mode returns zero without stopping");
        check(std::all_of(f.ctx.mem+0x6801,f.ctx.mem+0x6865,[](UBYTE x){return x==0xA5;}),"missing mode preserves buffer");
        backend_result=1; auto before=f.bytes();
        check(f.query(88,tag)==20 && !f.ctx.running,"incomplete native metadata stops20");
        check(f.bytes()==before,"incomplete native reply leaves arena untouched");
    }
    {
        DisplayFixture f;
        f.reject(88,DTAG_DIMS,0); f.reject(88,DTAG_DIMS,f.ctx.mem_size-65);
        f.reject(88,DTAG_DIMS,UINT32_MAX); f.reject(88,DTAG_DIMS,0x6801,0x12345678);
        f.reject(88,DTAG_VEC); f.reject(88,0x80005000);
        f.reject(0,DTAG_DIMS,0,0,726);
        f.reject(0,DTAG_DIMS,0,0,714); f.reject(0,DTAG_DIMS,0,0,720);
        check(f.query(0,DTAG_NAME,0)==0 && f.ctx.running,"zero size accepts null output");
        check(f.query(UINT32_MAX,DTAG_DIMS,f.ctx.mem_size-66)==66,"clipped complete output at arena end");
        check(f.query(33,DTAG_DIMS,f.ctx.mem_size-26)==26,"partial rectangle needs only actual output bytes");
        auto *mem=f.ctx.mem; f.ctx.mem=nullptr;
        f.state.a[0]=0; f.state.a[1]=1; f.state.d[0]=88; f.state.d[1]=DTAG_DIMS;
        check(thunk_gfx_GetDisplayInfoData(&f.ctx,&f.cpu)==20,"null arena refuses before native query");
        f.ctx.mem=mem;
    }

    {
        DisplayFixture f; const ULONG out=0x6801;
        check(f.query(160,DTAG_MNTR,out)==88,"full monitor transfer is 88 bytes");
        ULONG g=f.r(out+16);
        check(g && g<f.ctx.mem_size && f.ctx.mem_size-g>=160,"monitor identity is inside guest arena");
        check(f.ctx.monitors[0].native==&native_monitors[0] && f.ctx.monitors[0].guest==g,"native identity stays in private monitor table");
        check(f.ctx.num_shadows==0,"borrowed monitor is not registered for generic native resolution");
        auto owned_before=f.bytes(); ULONG available=Kaffeine_HeapAvail(&f.ctx,0);
        Kaffeine_HeapFree(&f.ctx,g,f.ctx.monitors[0].size);
        Kaffeine_HeapFree(&f.ctx,g+4,f.ctx.monitors[0].size-4);
        check(f.bytes()==owned_before && Kaffeine_HeapAvail(&f.ctx,0)==available,"guest FreeMem cannot retire a borrowed monitor snapshot");
        ULONG unrelated=Kaffeine_HeapAlloc(&f.ctx,f.ctx.monitors[0].size,0);
        check(unrelated && unrelated!=g,"later guest allocations cannot reuse the borrowed identity");
        Kaffeine_HeapFree(&f.ctx,unrelated,f.ctx.monitors[0].size);
        check(f.r(g+26)==static_cast<ULONG>(-123) && f.r(g+30)==0x12345678,"monitor ratios preserve LONG bits");
        check(m68k_read16(&f.ctx,g+24)==0x1234 && m68k_read16(&f.ctx,g+34)==312 && m68k_read16(&f.ctx,g+36)==226,"monitor flags and timing fields use classic offsets");
        check(!std::strcmp(reinterpret_cast<char *>(f.ctx.mem+f.r(g+10)),monitor_name),"monitor name copied with guest pointer and terminator");
        for(ULONG a:{0U,4U,16U,20U,44U,50U,54U,58U,74U,78U,102U,106U,112U,130U,134U,138U,142U,148U,152U,156U})
            check(f.r(g+a)==0,"native private/function/owner pointer is absent");
        check(f.r(g+88)==g+92 && !f.r(g+92) && f.r(g+96)==g+88,"monitor database list is canonically empty");
        check(f.r(g+118)==g+122 && !f.r(g+122) && f.r(g+126)==g+118 && m68k_read16(&f.ctx,g+146)==0xFFFF,"monitor semaphore contains no native waiters");
        std::vector<UBYTE> full(f.ctx.mem+out,f.ctx.mem+out+88);
        check(f.r(out)==DTAG_MNTR && f.r(out+12)==9,"monitor header tag and local length");
        check(m68k_read16(&f.ctx,out+20)==0xFFFF && m68k_read16(&f.ctx,out+28)==0xFFFE && m68k_read16(&f.ctx,out+42)==0xFFFF,"signed monitor geometry retains bits");
        check(m68k_read16(&f.ctx,out+36)==312 && m68k_read16(&f.ctx,out+38)==226 && f.r(out+84)==0x23456789,"monitor scalar fields are explicitly encoded");
        bool padding=true; for(ULONG i=44;i<76;i++) if(f.ctx.mem[out+i]) padding=false;
        check(padding,"monitor private padding is zeroed");
        for(ULONG size=0;size<=120;size++)
        {
            std::memset(f.ctx.mem+out-8,0xA5,128); ULONG n=std::min<ULONG>(size,88);
            check(f.query(size,DTAG_MNTR,out)==n,"all monitor transfer lengths match classic oracle");
            check(!std::memcmp(f.ctx.mem+out,full.data(),n),"all monitor prefixes agree including guest pointer bytes");
            bool guards=true; for(ULONG i=0;i<128;i++) if((i<8 || i>=8+n) && f.ctx.mem[out-8+i]!=0xA5) guards=false;
            check(guards,"monitor query preserves every untransferred byte");
            check(f.ctx.monitors[0].guest==g && !f.ctx.monitors[1].native,"repeated monitor queries retain one stable identity");
        }
        f.reject(88,DTAG_MNTR,g); f.reject(88,DTAG_DISP,g+159);
        monitor_choice=1; check(f.query(88,DTAG_MNTR,out)==88 && f.r(out+16)!=g,"distinct native monitors get distinct guest identities");
        native_monitors[0].ms_Node.xln_Name=too_long_monitor_name; std::memset(too_long_monitor_name,'X',sizeof(too_long_monitor_name));
        monitor_choice=0; check(f.query(88,DTAG_MNTR,out)==88 && f.r(out+16)==g,"existing immutable snapshot does not rescan a changed native name");
    }
    {
        DisplayFixture f;
        check(f.query(16,DTAG_MNTR)==16 && !f.ctx.monitors[0].native,"header-only prefix allocates no monitor snapshot");
        null_monitor=true; check(f.query(88,DTAG_MNTR)==88 && !f.r(0x6801+16) && !f.ctx.monitors[0].native,"native NULL monitor remains NULL");
        null_monitor=false;
        std::memset(too_long_monitor_name,'X',sizeof(too_long_monitor_name)); native_monitors[0].ms_Node.xln_Name=too_long_monitor_name;
        std::memset(f.ctx.mem+0x6801,0xA5,88);
        check(f.query(88,DTAG_MNTR)==0 && f.ctx.running && !f.ctx.monitors[0].native,"unterminated native monitor name fails publication");
        check(std::all_of(f.ctx.mem+0x6801,f.ctx.mem+0x6801+88,[](UBYTE b){return b==0xA5;}),"failed monitor publication preserves output");
        native_monitors[0].ms_Node.xln_Name=monitor_name; allocation_countdown=0;
        check(f.query(88,DTAG_MNTR)==0 && !f.ctx.monitors[0].native,"allocation failure publishes no monitor identity");
        allocation_countdown=-1;
        for(unsigned i=0;i<KAFFEINE_MAX_MONITORS;i++) {monitor_choice=i;check(f.query(88,DTAG_MNTR)==88,"monitor cache supports all bounded identities");}
        monitor_choice=KAFFEINE_MAX_MONITORS;
        check(f.query(88,DTAG_MNTR)==0 && f.ctx.running,"exhausted monitor cache fails without exposing native pointer");
        monitor_choice=0; check(f.query(88,DTAG_MNTR)==88,"full monitor cache still resolves existing identity");
    }
    check(native_blocks.empty(),"all fixture native allocations released");
    std::printf("display: %u checks, %u failed\n",checks,failures);
    return failures?1:0;
}
