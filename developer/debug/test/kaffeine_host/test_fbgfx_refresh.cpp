/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include "fbgfx_refresh.h"
static unsigned checks, failures;
static void check(bool ok,const char *what)
{ ++checks; if(!ok){++failures;std::printf("FAIL %s\n",what);} }
static uint32_t state=0xA73142E9;
static uint32_t random32(){state^=state<<13;state^=state>>17;state^=state<<5;return state;}
static void compare(unsigned bpp,int32_t xo,int32_t yo,int32_t x1,int32_t y1,int32_t x2,int32_t y2)
{
    const unsigned sw=1+random32()%11,sh=1+random32()%8,dw=1+random32()%13,dh=1+random32()%10;
    const unsigned sp=sw*bpp+random32()%9,dp=dw*4+random32()%11;
    std::vector<uint8_t> sb(16+sp*sh+16),db(16+dp*dh+16,0xC7);
    std::array<uint8_t,768> palette;
    for(auto &v:sb)v=random32(); for(auto &v:palette)v=random32();
    auto before=sb,expected=db;
    size_t sn=(random32()&1)?sp*sh:(sh-1)*sp+sw*bpp;
    size_t dn=(random32()&1)?dp*dh:(dh-1)*dp+dw*4;
    FBGfxSurface src={sb.data()+16,sn,sw,sh,sp,static_cast<uint8_t>(bpp)};
    FBGfxSurface dst={db.data()+16,dn,dw,dh,dp,4};
    bool touched=false;
    // Independent oracle: visit each source pixel, without rectangle clipping.
    for(unsigned y=0;y<sh;y++) for(unsigned x=0;x<sw;x++)
    {
        int64_t dx=int64_t(x)+xo,dy=int64_t(y)+yo;
        if(int64_t(x)<x1 || int64_t(x)>=x2 || int64_t(y)<y1 || int64_t(y)>=y2 ||
           dx<0 || dy<0 || dx>=dw || dy>=dh) continue;
        touched=true;size_t so=16+y*sp+x*bpp,off=16+dy*dp+dx*4;
        if(bpp==4) for(unsigned c=0;c<4;c++) expected[off+c]=sb[so+c];
        else {for(unsigned c=0;c<3;c++) expected[off+c]=palette[sb[so]*3+c];expected[off+3]=0;}
    }
    int result=fbgfx_refresh_rect(&src,&dst,palette.data(),xo,yo,x1,y1,x2,y2);
    check((result!=0)==touched,"return reports actual clipped pixels");
    check(db==expected,"all pixels, strides and canaries match independent oracle");
    check(sb==before,"source pixels and all source guards unchanged");
}
int main()
{
    for(unsigned bpp:{1U,4U})
    {
        for(int32_t xo:{INT32_MIN,-9,-1,0,1,8,INT32_MAX})
            for(int32_t yo:{INT32_MIN,-7,0,3,INT32_MAX})
                compare(bpp,xo,yo,INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX);
        for(unsigned i=0;i<1800;i++) compare(bpp,int(random32()%25)-12,int(random32()%21)-10,
             int(random32()%26)-8,int(random32()%20)-6,int(random32()%28)-8,int(random32()%22)-6);
    }
    std::array<uint8_t,128> srcbytes{},dstbytes{};
    std::array<uint8_t,768> palette{};
    srcbytes[0]=1;srcbytes[1]=2;palette[3]=255;palette[7]=255;
    FBGfxSurface s={srcbytes.data(),128,2,1,2,1},d={dstbytes.data(),128,2,1,8,4};
    check(fbgfx_refresh_rect(&s,&d,palette.data(),0,0,0,0,2,1)==1 && dstbytes[0]==255 && dstbytes[5]==255,"indexed RGB byte order");
    palette[3]=0;palette[5]=255;
    check(fbgfx_refresh_rect(&s,&d,palette.data(),0,0,0,0,2,1)==1 && dstbytes[0]==0 && dstbytes[2]==255,"palette changes recolor unchanged indices");
    const auto valid_s=s,valid_d=d;
    auto reject=[&](const FBGfxSurface *sp,const FBGfxSurface *dp,const uint8_t *pal)
    {auto before=dstbytes,source=srcbytes;FBGfxSurface mutable_d=dp?*dp:valid_d;
     check(fbgfx_refresh_rect(sp,dp?&mutable_d:nullptr,pal,0,0,0,0,2,1)==0,"invalid surface is refused");
     check(dstbytes==before && srcbytes==source,"refusal preserves source, destination and guards");};
    reject(nullptr,&d,palette.data());reject(&s,nullptr,palette.data());reject(&s,&d,nullptr);
    for(unsigned field=0;field<9;field++)
    {
        s=valid_s;d=valid_d;
        switch(field){case 0:s.pixels=nullptr;break;case 1:s.width=0;break;case 2:s.height=0;break;
          case 3:s.pitch=1;break;case 4:s.bytes=1;break;case 5:s.bytes_per_pixel=3;break;
          case 6:d.bytes_per_pixel=1;break;case 7:d.pitch=7;break;case 8:d.bytes=7;break;}
        reject(&s,&d,palette.data());
    }
    s=valid_s;d=valid_d;s.pixels=reinterpret_cast<uint8_t *>(UINTPTR_MAX-8);s.bytes=128;reject(&s,&d,palette.data());
    s=valid_s;d=valid_d;s.width=UINT32_MAX;s.height=UINT32_MAX;s.pitch=UINT32_MAX;s.bytes_per_pixel=4;s.bytes=SIZE_MAX;reject(&s,&d,palette.data());
    s=valid_s;d=valid_d;d.pixels=srcbytes.data();reject(&s,&d,palette.data());
    std::printf("Framebuffer refresh: %u checks, %u failed\n",checks,failures);
    return failures?1:0;
}
