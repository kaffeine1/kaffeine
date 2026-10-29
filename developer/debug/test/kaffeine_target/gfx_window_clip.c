/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Original clipping probe for owned window and classic requester layers. */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <graphics/clip.h>
#include <graphics/regions.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/layers.h>
#include "m68ktest.h"
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *LayersBase;

static void clipping(struct Layer *layer, struct RastPort *rp,
                     struct Region *a, struct Region *b)
{
    LONG outside,inside;
    SetAPen(rp,0);RectFill(rp,0,0,31,31);
    outside=ReadPixel(rp,0,0);
    check(InstallClipRegion(layer,a)==NULL,"first clip has no predecessor");
    check(layer->ClipRegion==a,"Layer exposes the first guest Region identity");
    check(InstallClipRegion(layer,b)==a,"replacement returns the first Region");
    check(layer->ClipRegion==b,"Layer exposes replacement Region identity");
    SetAPen(rp,1);RectFill(rp,0,0,31,31);
    inside=ReadPixel(rp,10,10);
    check(InstallClipRegion(layer,NULL)==b,"clearing returns the replacement Region");
    check(layer->ClipRegion==NULL,"clearing removes guest clip identity");
    /* Compare native pixel values, which may be indexed or truecolor. */
    check(outside>=0&&ReadPixel(rp,0,0)==outside,"clip preserves outside pixel");
    check(inside>=0&&inside!=outside,"drawing reaches an inside pixel");
}
int main(void)
{
    struct Window *w=NULL;
    struct Region *a=NULL,*b=NULL;
    struct Requester req;
    struct Rectangle rect_a={2,2,20,20},rect_b={8,8,15,15};
    IntuitionBase=(struct IntuitionBase*)OpenLibrary("intuition.library",37);
    GfxBase=(struct GfxBase*)OpenLibrary("graphics.library",37);
    LayersBase=OpenLibrary("layers.library",37);
    if(!check(IntuitionBase&&GfxBase&&LayersBase,"three graphics libraries"))goto done;
    a=NewRegion();b=NewRegion();
    if(!check(a&&b,"two owned Regions"))goto done;
    if(!check(OrRectRegion(a,&rect_a)&&OrRectRegion(b,&rect_b),"two clipping rectangles"))goto done;
    w=OpenWindowTags(NULL,WA_Left,20,WA_Top,25,WA_Width,160,WA_Height,120,
                    WA_Title,(ULONG)"Window clip test",WA_Flags,WFLG_BORDERLESS|WFLG_SIMPLE_REFRESH,TAG_DONE);
    if(!check(w&&w->WLayer&&w->RPort,"owned window and typed Layer/RastPort"))goto done;
    clipping(w->WLayer,w->RPort,a,b);
    InitRequester(&req);req.Width=40;req.Height=40;req.Flags=SIMPLEREQ|NOREQBACKFILL;
    if(check(Request(&req,w),"owned classic requester"))
    {
        if(check(req.ReqLayer&&req.ReqLayer->rp,"typed requester Layer/RastPort"))
            clipping(req.ReqLayer,req.ReqLayer->rp,a,b);
        EndRequest(&req,w);
    }
done:
    if(w)CloseWindow(w);
    if(a)DisposeRegion(a);
    if(b)DisposeRegion(b);
    if(LayersBase)CloseLibrary(LayersBase);
    if(GfxBase)CloseLibrary((struct Library*)GfxBase);
    if(IntuitionBase)CloseLibrary((struct Library*)IntuitionBase);
    return test_done("gfx_window_clip");
}
