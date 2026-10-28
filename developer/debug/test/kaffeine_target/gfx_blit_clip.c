/* Original bitmap clipping probe for native AROS and classic 68k. */
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <stdio.h>
#include <string.h>
struct GfxBase *GfxBase;
static unsigned checks,failed;
#define CHECK(x) do {checks++;if(!(x)){failed++;printf("FAIL line %u: %s\n",(unsigned)__LINE__,#x);}}while(0)
static int bit(const UBYTE *p,unsigned x,unsigned y){return (p[y*2+x/8]>>(7-(x&7)))&1;}
static void setbit(UBYTE *p,unsigned x,unsigned y,int value)
{UBYTE m=(UBYTE)(0x80u>>(x&7));if(value)p[y*2+x/8]|=m;else p[y*2+x/8]&=(UBYTE)~m;}
static void one(WORD xs,WORD ys,WORD xd,WORD yd,WORD w,WORD h,UBYTE mask)
{
    struct BitMap src,dst;
    UBYTE *s=AllocMem(24,MEMF_PUBLIC|MEMF_CHIP),*d=AllocMem(24,MEMF_PUBLIC|MEMF_CHIP);
    UBYTE expected[8];unsigned x,y;int changed=0;LONG result;
    CHECK(s!=NULL && d!=NULL);if(!s||!d)goto done;
    memset(s,0xA5,24);memset(d,0xA5,24);memset(d+8,0,8);memset(expected,0,8);
    for(x=0;x<8;x++)s[8+x]=(UBYTE)(0xF1u+x*23u);
    InitBitMap(&src,1,16,4);InitBitMap(&dst,1,16,4);src.Planes[0]=s+8;dst.Planes[0]=d+8;
    for(y=0;y<(unsigned)h;y++)for(x=0;x<(unsigned)w;x++)
        if(mask && xs+x<16 && xd+x<16 && ys+y<4 && yd+y<4){setbit(expected,xd+x,yd+y,bit(s+8,xs+x,ys+y));changed=1;}
    result=BltBitMap(&src,xs,ys,&dst,xd,yd,w,h,0xC0,mask,NULL);WaitBlit();
    CHECK(changed?result>0:result==0);
    CHECK(!memcmp(d+8,expected,8));
    for(x=0;x<8;x++){CHECK(s[x]==0xA5 && s[16+x]==0xA5);CHECK(d[x]==0xA5 && d[16+x]==0xA5);}
    for(x=0;x<8;x++)CHECK(s[8+x]==(UBYTE)(0xF1u+x*23u));
done:
    if(s)FreeMem(s,24);if(d)FreeMem(d,24);
}
int main(void)
{
    GfxBase=(struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library",0);CHECK(GfxBase!=NULL);
    if(GfxBase){
        one(12,2,0,0,8,4,1);one(0,0,14,3,8,3,1);one(14,3,15,3,9,2,1);
        one(16,0,0,0,4,1,1);one(0,0,16,0,4,1,1);one(0,4,0,0,1,1,1);one(0,0,0,4,1,1,1);
        one(0,0,0,0,0,4,1);one(0,0,0,0,16,4,0);
        CloseLibrary((struct Library *)GfxBase);
    }
    printf("gfx_blit_clip: %u checks, %u failed\n",checks,failed);return failed?10:0;
}
