/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original classic Requester storage, stacking and cleanup probe. */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <graphics/clip.h>
#include <proto/graphics.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <string.h>
#include "m68ktest.h"
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct CheckedRequest { struct Requester request; UBYTE guard[64]; };
static void prepare(struct CheckedRequest *b, WORD x, WORD y)
{
    memset(b,0xA5,sizeof(*b));
    InitRequester(&b->request);
    b->request.LeftEdge=x;b->request.TopEdge=y;
    b->request.Width=100;b->request.Height=40;
    b->request.Flags=SIMPLEREQ|NOREQBACKFILL;
}
static BOOL guarded(const struct CheckedRequest *b)
{
    unsigned i;for(i=0;i<sizeof(b->guard);i++)if(b->guard[i]!=0xA5)return FALSE;
    return TRUE;
}
int main(void)
{
    struct CheckedRequest a,b;
    struct Window *w;
    struct IntuiText text={1,0,JAM1,4,4,NULL,(UBYTE*)"Classic requester",NULL};
    IntuitionBase=(struct IntuitionBase*)OpenLibrary("intuition.library",37);
    if(!check(IntuitionBase!=NULL,"intuition.library"))return test_done("gfx_classic_request");
    GfxBase=(struct GfxBase*)OpenLibrary("graphics.library",37);
    if(!check(GfxBase!=NULL,"graphics.library"))goto done;
    w=OpenWindowTags(NULL,WA_Left,20,WA_Top,25,WA_Width,300,WA_Height,180,
                    WA_Title,(ULONG)"Classic Request test",WA_Flags,WFLG_ACTIVATE|WFLG_SIMPLE_REFRESH,TAG_DONE);
    if(!check(w!=NULL,"owned regular window"))goto done;
    prepare(&a,5,6);prepare(&b,7,8);
    a.request.ReqText=&text;
    check(guarded(&a)&&guarded(&b),"InitRequester preserves both guards");
    if(check(Request(&a.request,w),"Request opens native copy with text"))
    {
        check((a.request.Flags&REQACTIVE)!=0,"first requester active flag");
        check(a.request.RWindow==w,"requester names its guest window");
        if(check(a.request.ReqLayer&&a.request.ReqLayer->rp,"requester exposes a typed Layer and RastPort"))
        {
            struct RastPort *rp=a.request.ReqLayer->rp;
            check(rp->Layer==a.request.ReqLayer,"requester RastPort names the same layer");
            SetAPen(rp,1);
            check(WritePixel(rp,0,0)==0,"WritePixel draws through requester RastPort");
            /* On a truecolor public screen the returned pixel need not
               equal the indexed pen. Here check that it is readable. */
            check(ReadPixel(rp,0,0)>=0,"ReadPixel reads requester pixel");
        }
        check(w->FirstRequest==&a.request&&w->ReqCount==1,"window names first requester and count");
        if(check(Request(&b.request,w),"second requester stacks"))
        {
            check(b.request.OlderRequest==&a.request,"second requester names first");
            check(w->FirstRequest==&b.request&&w->ReqCount==2,"window names stack head and count");
            EndRequest(&a.request,w);
            check(!(a.request.Flags&REQACTIVE)&&a.request.OlderRequest==NULL,"ending first clears active links");
            check(b.request.OlderRequest==NULL&&w->FirstRequest==&b.request&&w->ReqCount==1,
                  "ending an older requester preserves stack head");
            EndRequest(&b.request,w);
        }
        else EndRequest(&a.request,w);
        check(w->FirstRequest==NULL&&w->ReqCount==0,"ending all requesters clears window stack");
    }
    check(guarded(&a)&&guarded(&b),"Request and EndRequest preserve caller guards");
    prepare(&a,0,0);a.request.Width=0;
    check(!Request(&a.request,w),"zero width returns failure without an active requester");
    check(w->ReqCount==0&&!(a.request.Flags&REQACTIVE),"native failure leaves no active state");
    prepare(&a,0,0);a.request.Width=a.request.Height=1;
    if(check(Request(&a.request,w),"empty one-pixel requester opens"))
    {
        memset(&a.request,0xC3,sizeof(a.request));
        CloseWindow(w);w=NULL;
        {unsigned i;BOOL untouched=TRUE;for(i=0;i<sizeof(a.request);i++)if(((UBYTE*)&a.request)[i]!=0xC3)untouched=FALSE;
         check(untouched&&guarded(&a),"window cleanup leaves released caller storage untouched");}
    }
    if(w)CloseWindow(w);
done:
    if(GfxBase)CloseLibrary((struct Library*)GfxBase);
    CloseLibrary((struct Library*)IntuitionBase);
    return test_done("gfx_classic_request");
}
