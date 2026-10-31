/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Independent classic ABI test of owned screen-bar drawing and LayerInfo. */
#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/semaphores.h>
#include <graphics/displayinfo.h>
#include <graphics/gfx.h>
#include <graphics/layers.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/layers.h>
#include "m68ktest.h"
#ifdef __AROS__
typedef IPTR TagPointer;
#else
typedef ULONG TagPointer;
#endif
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *LayersBase;
static ULONG indexed_mode(void)
{
    ULONG id=INVALID_ID; unsigned i;
    for(i=0;i<256;i++) {
        struct DimensionInfo dim; struct DisplayInfo disp;
        id=NextDisplayInfo(id); if(id==INVALID_ID) break;
        if(GetDisplayInfoData(NULL,(UBYTE *)&dim,sizeof(dim),DTAG_DIMS,id) &&
           GetDisplayInfoData(NULL,(UBYTE *)&disp,sizeof(disp),DTAG_DISP,id) &&
           !disp.NotAvailable && dim.MaxDepth>=3 && dim.MaxDepth<=8 &&
           dim.Nominal.MaxX-dim.Nominal.MinX+1>=320 &&
           dim.Nominal.MaxY-dim.Nominal.MinY+1>=200 &&
           !(disp.PropertyFlags & (DIPF_IS_HAM|DIPF_IS_DUALPF))) return id;
    }
    return INVALID_ID;
}
int main(void)
{
    struct Screen *screen=NULL;
    struct Layer_Info *one=NULL,*two=NULL;
    ULONG id;
    unsigned pass;
    IntuitionBase=(struct IntuitionBase *)OpenLibrary("intuition.library",39);
    GfxBase=(struct GfxBase *)OpenLibrary("graphics.library",39);
    LayersBase=OpenLibrary("layers.library",39);
    if(!check(IntuitionBase && GfxBase && LayersBase,"open graphics, Intuition and layers")) goto done;
    id=indexed_mode();
    if(!check(id!=INVALID_ID,"select an indexed native display")) goto done;
    for(pass=0;pass<2;pass++) {
        struct RastPort *rp;
        screen=OpenScreenTags(NULL,SA_DisplayID,id,SA_Width,320,SA_Height,200,
                 SA_Depth,3,SA_Type,CUSTOMSCREEN,SA_Quiet,FALSE,
                 SA_Title,(TagPointer)"Layer resource test",TAG_DONE);
        if(!check(screen!=NULL,"open a custom screen")) goto done;
        if(!check(screen->BarLayer && screen->BarLayer->rp,"screen bar has its own RastPort")) goto done;
        rp=screen->BarLayer->rp;
        check(rp!=&screen->RastPort,"bar context is distinct from the screen context");
        check(rp->Layer==screen->BarLayer,"bar RastPort and Layer share identity");
        SetAPen(rp,2);
        Move(rp,2,3); Draw(rp,12,3);
        check_long("bar drawing keeps the chosen pen",rp->FgPen,2);
        check_long("bar drawing publishes final pen position",rp->cp_x,12);
        check_long("bar line reaches the indexed display",ReadPixel(rp,7,3),2);
        {
            struct Window *w=OpenWindowTags(NULL,WA_CustomScreen,(TagPointer)screen,
                WA_Left,20,WA_Top,30,WA_Width,120,WA_Height,80,TAG_DONE);
            if(check(w!=NULL,"open a window to exercise a refused screen close")) {
                struct SignalSemaphore *lock=&screen->LayerInfo.Lock;
                ObtainSemaphore(lock);
                check(!CloseScreen(screen),"screen close is refused while a window stays open");
                check(lock->ss_NestCount==1 && lock->ss_Owner==FindTask(NULL),
                      "refused close preserves the held classic LayerInfo semaphore");
                ReleaseSemaphore(lock);
                check(lock->ss_NestCount==0,"LayerInfo semaphore releases after refused close");
                CloseWindow(w);
            }
        }
        if(!check(CloseScreen(screen),"close screen and retire its bar context")) goto done;
        screen=NULL;
    }
    for(pass=0;pass<3;pass++) {
        one=NewLayerInfo();two=NewLayerInfo();
        if(!check(one && two && one!=two,"allocate distinct live LayerInfo objects")) goto done;
        check(one->top_layer==NULL && two->top_layer==NULL,"new layer lists are empty");
        check((one->Flags & NEWLAYERINFO_CALLED)!=0,"LayerInfo retains the public creation flag");
        check(one->Lock.ss_Link.ln_Type==NT_SIGNALSEM,"LayerInfo has a classic signal semaphore");
        DisposeLayerInfo(one);one=NULL;
        check(two->top_layer==NULL,"disposing one LayerInfo preserves the other");
        DisposeLayerInfo(two);two=NULL;
    }
done:
    if(one) DisposeLayerInfo(one);
    if(two) DisposeLayerInfo(two);
    if(screen) CloseScreen(screen);
    if(LayersBase) CloseLibrary(LayersBase);
    if(GfxBase) CloseLibrary((struct Library *)GfxBase);
    if(IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_layer_resources");
}
