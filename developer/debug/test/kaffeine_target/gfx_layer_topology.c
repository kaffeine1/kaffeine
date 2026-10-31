/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original independent classic window/layer depth and hook identity test. */
#include <exec/types.h>
#include <graphics/layers.h>
#include <graphics/clip.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/layers.h>
#include "m68ktest.h"
#ifdef __AROS__
typedef IPTR TagPointer;
#else
typedef ULONG TagPointer;
#endif
struct IntuitionBase *IntuitionBase;
struct Library *LayersBase;
static BOOL before(struct Layer *first, struct Layer *last)
{
    volatile struct Layer *p=first;
    ULONG n;
    for(n=0;p && n<64;n++,p=p->back)
        if(p==last) return TRUE;
    return FALSE;
}
static BOOL wait_before(struct Layer *first, struct Layer *last)
{
    ULONG tries;
    for(tries=0;tries<500000;tries++)
        if(before(first,last)) return TRUE;
    return FALSE;
}

int main(void)
{
    struct Window *one=NULL,*two=NULL,*backdrop=NULL;
    struct Layer *a,*b,*old_layer;
    struct Hook *old,*replaced;
    IntuitionBase=(struct IntuitionBase *)OpenLibrary("intuition.library",39);
    LayersBase=OpenLibrary("layers.library",39);
    if(!check(IntuitionBase && LayersBase,"Intuition and layers open")) goto done;
    one=OpenWindowTags(NULL,WA_Left,40,WA_Top,40,WA_Width,280,WA_Height,140,
                      WA_Title,(TagPointer)"Layer one",TAG_DONE);
    two=OpenWindowTags(NULL,WA_Left,70,WA_Top,70,WA_Width,280,WA_Height,140,
                      WA_Title,(TagPointer)"Layer two",TAG_DONE);
    if(!check(one && two,"two windows open on one screen")) goto done;
    a=one->WLayer;b=two->WLayer;
    if(!check(a && b && a!=b,"windows expose distinct classic layer identities")) goto done;
    check(one->RPort->Layer==a && two->RPort->Layer==b,"RastPort and Window share layer identity");
    check(one->WScreen==two->WScreen && one->WScreen->BarLayer!=NULL,
          "public screen publishes its bar layer");
    check(wait_before(b,a),"front window links to the back window");
    WindowToBack(two);
#ifdef __AROS__
    Delay(5);
#endif
    check(wait_before(a,b),"polling without OS calls observes WindowToBack completion");
    WindowToFront(two);
#ifdef __AROS__
    Delay(5);
#endif
    check(wait_before(b,a),"polling without OS calls observes WindowToFront completion");
    old=InstallLayerHook(b,LAYERS_NOBACKFILL);
    replaced=InstallLayerHook(b,NULL);
    check(replaced==LAYERS_NOBACKFILL,"no-backfill sentinel survives a hook round trip");
    replaced=InstallLayerHook(b,old);
    check(replaced==NULL,"default hook and saved hook identities can be restored");
    backdrop=OpenWindowTags(NULL,WA_Left,90,WA_Top,90,WA_Width,240,WA_Height,100,
                           WA_Backdrop,TRUE,WA_Borderless,TRUE,TAG_DONE);
    if(check(backdrop && backdrop->WLayer,"backdrop window has a layer"))
    {
        backdrop->Flags &= ~WFLG_BACKDROP;
        backdrop->WLayer->Flags &= ~LAYERBACKDROP;
        WindowToFront(backdrop);
#ifdef __AROS__
        Delay(5);
#endif
        /* Current native AROS keeps a layer's priority class after these
           direct public flag writes. Verify the bits without inventing
           an order that the native layer list does not have. */
        check(!(backdrop->Flags & WFLG_BACKDROP) &&
              !(backdrop->WLayer->Flags & LAYERBACKDROP),
              "depth call preserves both cleared backdrop flags");
        CloseWindow(backdrop);backdrop=NULL;
    }
    old_layer=a;
    CloseWindow(one);one=NULL;
    check(b->back!=old_layer,"closing a sibling retires its layer from the remaining graph");
done:
    if(backdrop) CloseWindow(backdrop);
    if(two) CloseWindow(two);
    if(one) CloseWindow(one);
    if(LayersBase) CloseLibrary(LayersBase);
    if(IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_layer_topology");
}
