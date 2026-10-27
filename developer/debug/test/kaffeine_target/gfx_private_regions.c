/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Original owned-Region, classic rectangle list and planar clipping probe. */
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/clip.h>
#include <graphics/layers.h>
#include <graphics/regions.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/layers.h>
#include <string.h>
#include "m68ktest.h"
struct GfxBase *GfxBase;
struct Library *LayersBase;
static ULONG pixel(struct BitMap *bm,ULONG x,ULONG y)
{
    ULONG p,result=0;
    for(p=0;p<bm->Depth;p++)
        if(((UBYTE *)bm->Planes[p])[y*bm->BytesPerRow+(x>>3)] & (0x80>>(x&7))) result|=1UL<<p;
    return result;
}
static BOOL contains(struct Region *r,LONG x,LONG y)
{
    struct RegionRectangle *p;
    ULONG n=0;
    x-=r->bounds.MinX;y-=r->bounds.MinY;
    for(p=r->RegionRectangle;p && n<64;p=p->Next,n++)
        if(x>=p->bounds.MinX && x<=p->bounds.MaxX && y>=p->bounds.MinY && y<=p->bounds.MaxY) return TRUE;
    return FALSE;
}
static BOOL links(struct Region *r)
{
    struct RegionRectangle *p,*prev=NULL;
    ULONG n=0;
    for(p=r->RegionRectangle;p && n<64;p=p->Next,n++) {
        if(p->Prev!=prev || p->bounds.MinX>p->bounds.MaxX || p->bounds.MinY>p->bounds.MaxY) return FALSE;
        prev=p;
    }
    return p==NULL && n>0;
}
int main(void)
{
    struct BitMap bm;
    struct Layer_Info *info=NULL;
    struct Layer *layer=NULL;
    struct Region *one=NULL,*two=NULL;
    struct Rectangle a={0,0,63,47},b={32,24,95,71},c={80,60,95,79};
    ULONG p,pass,bytes=16*96;
    memset(&bm,0,sizeof(bm));
    GfxBase=(struct GfxBase *)OpenLibrary("graphics.library",39);
    LayersBase=OpenLibrary("layers.library",39);
    if(!check(GfxBase && LayersBase,"open graphics and layers")) goto done;
#ifdef __AROS__
    InitBitMap(&bm,3,128,96);
#else
    bm.BytesPerRow=16;bm.Rows=96;bm.Depth=3;
#endif
    for(p=0;p<3;p++) {
        bm.Planes[p]=AllocMem(bytes,MEMF_PUBLIC|MEMF_CLEAR);
        if(!check(bm.Planes[p]!=NULL,"allocate planar storage")) goto done;
    }
    for(pass=0;pass<2;pass++) {
        for(p=0;p<3;p++) memset(bm.Planes[p],0,bytes);
        one=NewRegion();two=NewRegion();
        if(!check(one && two && one!=two,"allocate independent owned Regions")) goto done;
        check(one->RegionRectangle==NULL && two->RegionRectangle==NULL,"new Regions have empty rectangle lists");
        check(OrRectRegion(one,&a) && OrRectRegion(one,&b),"union two rectangles through the classic ABI");
        check(one->bounds.MinX==0 && one->bounds.MinY==0 && one->bounds.MaxX==95 && one->bounds.MaxY==71,
              "Region bounds describe the union in signed words");
        check(links(one),"RegionRectangle next and previous links form a valid list");
        check(contains(one,8,8) && contains(one,80,60) && !contains(one,80,8),"rectangle snapshots preserve exact union geometry");
        check(OrRectRegion(two,&c),"make a Region with a nonzero origin");
        check(links(two) && contains(two,82,62) && !contains(two,81,59),"rectangle bounds are relative to their Region origin");
        info=NewLayerInfo();
        if(!check(info!=NULL,"allocate a private LayerInfo")) goto done;
        layer=CreateUpfrontHookLayer(info,&bm,0,0,127,95,LAYERSIMPLE|LAYERBACKDROP,LAYERS_NOBACKFILL,NULL);
        if(!check(layer && layer->rp,"create a private layer for clipping")) goto done;
        check(InstallClipRegion(layer,one)==NULL && layer->ClipRegion==one,"install an owned Region without native pointer leakage");
        SetAPen(layer->rp,3);RectFill(layer->rp,0,0,127,95);
        check_long("clipped fill reaches the first rectangle",pixel(&bm,8,8),3);
        check_long("clipped fill reaches the second rectangle",pixel(&bm,80,60),3);
        check_long("clipped fill preserves a hole in the union",pixel(&bm,80,8),0);
        check_long("clipped fill preserves pixels outside the Region",pixel(&bm,120,90),0);
        check(InstallClipRegion(layer,two)==one && layer->ClipRegion==two,"replacement returns the previous owned Region identity");
        DisposeRegion(one);one=NULL;
        SetAPen(layer->rp,5);RectFill(layer->rp,0,0,127,95);
        check_long("replacement clipping uses its nonzero origin",pixel(&bm,82,62),5);
        check_long("replacement leaves previous content outside it",pixel(&bm,8,8),3);
        check(InstallClipRegion(layer,NULL)==two && layer->ClipRegion==NULL,"clearing clipping returns its previous guest identity");
        DisposeRegion(two);two=NULL;
        SetAPen(layer->rp,6);RectFill(layer->rp,120,90,120,90);
        check_long("cleared clipping allows drawing outside former Regions",pixel(&bm,120,90),6);
        check(DeleteLayer(0,layer)!=0,"delete the unclipped private layer");layer=NULL;
        DisposeLayerInfo(info);info=NULL;
    }
done:
    if(layer) { InstallClipRegion(layer,NULL);DeleteLayer(0,layer); }
    if(info) DisposeLayerInfo(info);
    if(one) DisposeRegion(one);
    if(two) DisposeRegion(two);
    for(p=0;p<3;p++) if(bm.Planes[p]) FreeMem(bm.Planes[p],bytes);
    if(LayersBase) CloseLibrary(LayersBase);
    if(GfxBase) CloseLibrary((struct Library *)GfxBase);
    return test_done("gfx_private_regions");
}
