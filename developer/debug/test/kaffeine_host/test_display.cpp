/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <graphics/displayinfo.h>
#include <proto/graphics.h>
#include "message_fixture.h"
#include "kaffeine_display.h"

static unsigned queries;
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
static const M68KThunkEntry generated[] = { {726,poison}, {756,poison} };
struct DisplayFixture:Fixture
{
    DisplayFixture()
    {
        queries=0; backend_result=-1;
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
        f.reject(88,DTAG_MNTR); f.reject(88,DTAG_VEC); f.reject(88,0x80005000);
        f.reject(0,DTAG_DIMS,0,0,726);
        check(f.query(0,DTAG_NAME,0)==0 && f.ctx.running,"zero size accepts null output");
        check(f.query(UINT32_MAX,DTAG_DIMS,f.ctx.mem_size-66)==66,"clipped complete output at arena end");
        check(f.query(33,DTAG_DIMS,f.ctx.mem_size-26)==26,"partial rectangle needs only actual output bytes");
        auto *mem=f.ctx.mem; f.ctx.mem=nullptr;
        f.state.a[0]=0; f.state.a[1]=1; f.state.d[0]=88; f.state.d[1]=DTAG_DIMS;
        check(thunk_gfx_GetDisplayInfoData(&f.ctx,&f.cpu)==20,"null arena refuses before native query");
        f.ctx.mem=mem;
    }
    check(native_blocks.empty(),"all fixture native allocations released");
    std::printf("display: %u checks, %u failed\n",checks,failures);
    return failures?1:0;
}
