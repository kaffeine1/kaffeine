/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Original offscreen planar-layer and synchronous backfill ABI probe. */
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/clip.h>
#include <graphics/layers.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/layers.h>
#include <string.h>
#include "m68ktest.h"
#ifdef __AROS__
#include <aros/asmcall.h>
#endif
struct GfxBase *GfxBase;
struct Library *LayersBase;
struct FillMessage { struct Layer *layer; struct Rectangle bounds; LONG x,y; };
struct FillState { struct Task *owner; struct Hook *hook; ULONG calls,invalid; struct Layer *last; };
static ULONG fill(struct Hook *hook,struct RastPort *rp,struct FillMessage *msg)
{
    struct FillState *s=hook->h_Data;
    s->calls++;
    if(hook!=s->hook || !rp || !msg || !msg->layer || rp->Layer!=msg->layer || msg->layer->rp!=rp || !msg->layer->LayerInfo ||
       FindTask(NULL)!=s->owner || msg->bounds.MinX<0 || msg->bounds.MinY<0 ||
       msg->bounds.MaxX>=128 || msg->bounds.MaxY>=96 || msg->x<0 || msg->y<0) {
        s->invalid++;return 0;
    }
    s->last=msg->layer;
    SetAPen(rp,2);
    RectFill(rp,msg->bounds.MinX,msg->bounds.MinY,msg->bounds.MaxX,msg->bounds.MaxY);
    return 0;
}
#ifdef __AROS__
AROS_UFH3S(IPTR, fill_entry,
    AROS_UFHA(struct Hook *,hook,A0),AROS_UFHA(struct RastPort *,rp,A2),
    AROS_UFHA(struct FillMessage *,msg,A1))
{
    AROS_USERFUNC_INIT
    return fill(hook,rp,msg);
    AROS_USERFUNC_EXIT
}
#else
extern ULONG fill_entry(void);
__asm__(".text\n.even\n.globl _fill_entry\n_fill_entry:\n"
        "move.l %a1,-(%sp)\nmove.l %a2,-(%sp)\nmove.l %a0,-(%sp)\n"
        "move.l 12(%a0),%a0\njsr (%a0)\nlea 12(%sp),%sp\nrts\n");
#endif
static ULONG pixel(struct BitMap *bm,ULONG x,ULONG y)
{
    ULONG p,result=0;
    for(p=0;p<bm->Depth;p++)
        if(((UBYTE *)bm->Planes[p])[y*bm->BytesPerRow+(x>>3)] & (0x80>>(x&7)))
            result |= 1UL<<p;
    return result;
}
int main(void)
{
    struct BitMap bm;
    struct Hook hook;
    struct FillState state;
    struct Layer_Info *info=NULL;
    struct Layer *one=NULL,*two=NULL,*native_root=NULL;
    struct Hook *old;
    ULONG p,bytes=16*96,pass;
    memset(&bm,0,sizeof(bm));memset(&hook,0,sizeof(hook));memset(&state,0,sizeof(state));
    GfxBase=(struct GfxBase *)OpenLibrary("graphics.library",39);
    LayersBase=OpenLibrary("layers.library",39);
    if(!check(GfxBase && LayersBase,"open graphics and layers")) goto done;
#ifdef __AROS__
    InitBitMap(&bm,3,128,96);
    hook.h_Entry=(APTR)fill_entry;
#else
    bm.BytesPerRow=16;bm.Rows=96;bm.Depth=3; /* ordinary classic planar bitmap */
    hook.h_Entry=fill_entry;hook.h_SubEntry=(ULONG (*)())fill;
#endif
    state.owner=FindTask(NULL);state.hook=&hook;hook.h_Data=&state;
    for(p=0;p<3;p++) {
        bm.Planes[p]=AllocMem(bytes,MEMF_PUBLIC|MEMF_CLEAR);
        if(!check(bm.Planes[p]!=NULL,"allocate a guest-accessible bitplane")) goto done;
    }
    for(pass=0;pass<2;pass++) {
        state.calls=state.invalid=0;state.last=NULL;
        info=NewLayerInfo();
        if(!check(info!=NULL,"allocate an owned offscreen LayerInfo")) goto done;
        one=CreateUpfrontHookLayer(info,&bm,0,0,127,95,LAYERSIMPLE|LAYERBACKDROP,&hook,NULL);
        if(!check(one!=NULL,"create a private layer with a synchronous backfill hook")) goto done;
#ifdef __AROS__
        native_root=one->back; /* AROS keeps an internal root until DisposeLayerInfo. */
#endif
        check(state.calls>0 && !state.invalid,"constructor callback receives classic hook, object, message and task identity");
        check(state.last==one,"callback and returned Layer share identity");
        check(one->rp && one->rp->Layer==one,"private Layer and typed RastPort share identity");
        check(info->top_layer==one && one->LayerInfo==info,"owned LayerInfo names the returned layer");
        check(InstallClipRegion(one,NULL)==NULL,"clear private clipping without exposing a native Region");
        check_long("backfill writes the native planar layer",ReadPixel(one->rp,8,8),2);
        check_long("backfill reaches the original guest bitplanes",pixel(&bm,8,8),2);
        SetAPen(one->rp,3);RectFill(one->rp,4,4,12,12);
        check_long("drawing through the layer updates guest planes",pixel(&bm,8,8),3);
        /* This AROS BSP returns 8 from its planar driver path, even with
           fewer selected planes. Pixel checks below verify the operation;
           these checks verify that the bridge preserves the native LONG. */
        check_long("AROS planar driver copy return",BltBitMap(&bm,4,4,&bm,20,20,9,9,0xC0,0xFF,NULL),8);
        WaitBlit();
        check_long("planar bitmap copy updates the original destination planes",pixel(&bm,24,24),3);
        check_long("AROS masked copy native return",BltBitMap(&bm,4,4,&bm,40,40,1,1,0xC0,1,NULL),8);
        WaitBlit();
        check_long("masked bitmap copy preserves unselected planes",pixel(&bm,40,40),3);
        check_long("AROS masked clear native return",BltBitMap(&bm,4,4,&bm,40,40,1,1,0,1,NULL),8);
        WaitBlit();
        check_long("masked clear preserves the second plane",pixel(&bm,40,40),2);
        check_long("AROS planar driver clear return",BltBitMap(&bm,4,4,&bm,4,4,9,9,0,0xFF,NULL),8);
        WaitBlit();
        check_long("planar bitmap clear reaches guest planes",pixel(&bm,8,8),0);
        old=InstallLayerHook(one,LAYERS_NOBACKFILL);
        check(old==&hook,"replacing a bridged hook returns its original guest identity");
        old=InstallLayerHook(one,&hook);
        check(old==LAYERS_NOBACKFILL,"restore a custom hook after no-backfill sentinel");
        LockLayer(0,one);UnlockLayer(one);
        two=CreateUpfrontHookLayer(info,&bm,32,24,95,71,LAYERSIMPLE,LAYERS_NOBACKFILL,NULL);
        if(!check(two!=NULL,"create a second private layer on the same planar bitmap")) goto done;
        check(info->top_layer==two && two->back==one && one->front==two,"private list publishes the real front-to-back order");
        check(DeleteLayer(0,two)!=0,"delete second private layer");two=NULL;
        check(info->top_layer==one && one->front==NULL && one->back==native_root,"deletion preserves remaining identities and links");
        check(!state.invalid,"later backfill preserves callback argument identity");
        check(DeleteLayer(0,one)!=0,"delete first private layer");one=NULL;
        check(info->top_layer==native_root,"only the native internal root remains after user deletion");
        one=CreateUpfrontHookLayer(info,&bm,0,0,127,95,LAYERSIMPLE,&hook,NULL);
        if(!check(one!=NULL,"reuse an owned LayerInfo after deleting all user layers")) goto done;
        check_long("reused LayerInfo keeps its common bitmap valid",pixel(&bm,8,8),2);
        check(DeleteLayer(0,one)!=0,"delete recreated private layer");one=NULL;
        check(info->top_layer==native_root,"recreated layer retires before LayerInfo disposal");
        DisposeLayerInfo(info);info=NULL;
    }
done:
    if(two) DeleteLayer(0,two);
    if(one) DeleteLayer(0,one);
    if(info) DisposeLayerInfo(info);
    for(p=0;p<3;p++) if(bm.Planes[p]) FreeMem(bm.Planes[p],bytes);
    if(LayersBase) CloseLibrary(LayersBase);
    if(GfxBase) CloseLibrary((struct Library *)GfxBase);
    return test_done("gfx_private_layers");
}
