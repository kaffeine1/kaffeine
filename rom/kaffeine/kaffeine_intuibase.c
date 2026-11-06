/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)

   Bounded read-only classic snapshots. Native identity and lock ownership
   stay private; no guest field is a native capability. */
#include <exec/memory.h>
#include <aros/debug.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <graphics/layers.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <string.h>
#include <stddef.h>
#include "kaffeine_intuibase.h"
#include "kaffeine_shadow.h"
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"
#include "kaffeine_screens.h"
#include "kaffeine_layers.h"
#include "kaffeine_sched.h"
#include "kaffeine_cpu.h"
#include "kaffeine_callback.h"
/* One snapshot reaches at most CURRENT views (a Personal Paint screen with
   its menus takes about 1100). The table has room for the views of the
   last snapshot that fitted and as many new ones: a view keeps its guest
   address while the snapshots reach its native object, and its mirror goes
   back after the first snapshot that fits without reaching it. */
#define CURRENT 2048
#define SLOTS (2 * CURRENT)
#define MAX_DEPTH 128
struct IBaseView { const struct M68KStructLayout *layout; void *native;
                   ULONG guest, size, epoch; BOOL string; };
struct KaffeineIBase { struct IBaseView views[SLOTS]; ULONG count, reached, epoch, depth, owner, key; BOOL failed; };
static IPTR refuse(struct KaffeineContext *ctx, const char *why)
{ bug("[kaffeine] IBase: %s, stopping (20)\n",why); Kaffeine_StopGuest(ctx,ctx->cpu,20); return 20; }
static ULONG caller(struct KaffeineContext *ctx)
{ return ctx->sched && ctx->task_ops ? ctx->task_ops->caller_task(ctx,ctx->cpu) : ctx->guest_exec_task; }
BOOL Kaffeine_IBaseLocked(struct KaffeineContext *ctx)
{ return ctx->ibase && ctx->ibase->depth; }
static struct IBaseView *view(struct KaffeineContext *ctx, void *native, const char *name)
{
    struct KaffeineIBase *s=ctx->ibase; const struct M68KStructLayout *layout=name?shadow_find_layout(name):NULL;
    ULONG i, size=name?(layout?layout->m68k_size:0):256; struct IBaseView *v;
    if (!native || !size) return NULL;
    for (i=0;i<s->count;++i)
        if (s->views[i].native==native && s->views[i].layout==layout && s->views[i].string==!name) return &s->views[i];
    if (s->count==SLOTS) { s->failed=TRUE; return NULL; }
    v=&s->views[s->count]; v->guest=Kaffeine_MirrorAlloc(ctx,size);
    if (!v->guest) { s->failed=TRUE; return NULL; }
    v->size=size;v->native=native;v->layout=layout;v->string=!name;++s->count;return v;
}
/* Only classic offsets are fixed. Native members are always C field
   accesses: native Library/Layer_Info layouts differ across AROS builds. */
static ULONG snapshot(struct KaffeineContext *, const char *, void *, unsigned);
#define W(off,value) m68k_write16(ctx,g+(off),(UWORD)(value))
#define L(off,value) m68k_write32(ctx,g+(off),(ULONG)(value))
#define B(off,value) (ctx->mem[g+(off)]=(UBYTE)(value))
#define LINK(off,member,kind) L(off,snapshot(ctx,kind,n->member,depth+1))
static void bitmap_fields(struct KaffeineContext *ctx, ULONG g, struct BitMap *n)
{
    W(0,n->BytesPerRow);W(2,n->Rows);B(4,n->Flags);B(5,n->Depth);
    /* No native plane pointer is exposed; use the typed capture API. */
}
static ULONG snapshot(struct KaffeineContext *ctx, const char *name, void *native, unsigned depth)
{
    struct KaffeineIBase *s=ctx->ibase;struct IBaseView *v;ULONG g;
    /* A snapshot that has failed is refused whole: stop walking at once. */
    if (!native || s->failed) return 0;
    if (depth>MAX_DEPTH) { s->failed=TRUE;return 0; }
    v=view(ctx,native,name);if(!v)return 0;g=v->guest;
    if(v->epoch==s->epoch)return g;
    if(++s->reached>CURRENT) { s->failed=TRUE;return 0; }
    v->epoch=s->epoch;memset(ctx->mem+g,0,v->size);
    if(!name) { size_t n=0;while(n<255 && ((char*)native)[n])++n;memcpy(ctx->mem+g,native,n);return g; }
    if(!strcmp(name,"Screen"))
    {
        struct Screen *n=native;
        LINK(0,NextScreen,"Screen");LINK(4,FirstWindow,"Window");
        W(8,n->LeftEdge);W(10,n->TopEdge);W(12,n->Width);W(14,n->Height);
        W(16,n->MouseY);W(18,n->MouseX);W(20,n->Flags);
        LINK(22,Title,NULL);LINK(26,DefaultTitle,NULL);
        B(30,n->BarHeight);B(31,n->BarVBorder);B(32,n->BarHBorder);
        B(33,n->MenuVBorder);B(34,n->MenuHBorder);B(35,n->WBorTop);
        B(36,n->WBorLeft);B(37,n->WBorRight);B(38,n->WBorBottom);
        W(44+24,n->ViewPort.DWidth);W(44+26,n->ViewPort.DHeight);
        W(44+28,n->ViewPort.DxOffset);W(44+30,n->ViewPort.DyOffset);W(44+32,n->ViewPort.Modes);
        L(M68K_SCR_RASTPORT+M68K_RP_BITMAP,snapshot(ctx,"BitMap",n->RastPort.BitMap,depth+1));
        if(n->RastPort.BitMap)bitmap_fields(ctx,g+M68K_SCR_BITMAP,n->RastPort.BitMap);
        L(224,snapshot(ctx,"Layer",n->LayerInfo.top_layer,depth+1));
    }
    else if(!strcmp(name,"Window"))
    {
        struct Window *n=native;
        LINK(0,NextWindow,"Window");W(4,n->LeftEdge);W(6,n->TopEdge);W(8,n->Width);W(10,n->Height);
        W(12,n->MouseY);W(14,n->MouseX);W(16,n->MinWidth);W(18,n->MinHeight);
        W(20,n->MaxWidth);W(22,n->MaxHeight);L(24,n->Flags);
        LINK(28,MenuStrip,"Menu");LINK(32,Title,NULL);
        LINK(46,WScreen,"Screen");LINK(50,RPort,"RastPort");
        B(54,n->BorderLeft);B(55,n->BorderTop);B(56,n->BorderRight);B(57,n->BorderBottom);
        LINK(62,FirstGadget,"Gadget");L(82,n->IDCMPFlags);B(98,n->DetailPen);B(99,n->BlockPen);
        W(108,n->GZZMouseX);W(110,n->GZZMouseY);W(112,n->GZZWidth);W(114,n->GZZHeight);
    }
    else if(!strcmp(name,"Layer"))
    {
        struct Layer *n=native;
        LINK(0,front,"Layer");LINK(4,back,"Layer");LINK(12,rp,"RastPort");
        W(16,n->bounds.MinX);W(18,n->bounds.MinY);W(20,n->bounds.MaxX);W(22,n->bounds.MaxY);
        /* NDK Layer.Window offset, independent of native semaphores. */
        LINK(40,Window,"Window");
    }
    else if(!strcmp(name,"RastPort"))
    { struct RastPort *n=native;LINK(4,BitMap,"BitMap"); }
    else if(!strcmp(name,"BitMap"))bitmap_fields(ctx,g,native);
    else if(!strcmp(name,"Gadget"))
    {
        struct Gadget *n=native;LINK(0,NextGadget,"Gadget");
        W(4,n->LeftEdge);W(6,n->TopEdge);W(8,n->Width);W(10,n->Height);W(12,n->Flags);
        W(14,n->Activation);W(16,n->GadgetType);L(30,n->MutualExclude);W(38,n->GadgetID);
        if((n->Flags&GFLG_LABELMASK)==GFLG_LABELSTRING || (n->Flags&GFLG_LABELMASK)==GFLG_LABELITEXT)
            L(26,snapshot(ctx,(n->Flags&GFLG_LABELMASK)==GFLG_LABELSTRING?NULL:"IntuiText",n->GadgetText,depth+1));
        if((n->GadgetType&GTYP_GTYPEMASK)==GTYP_STRGADGET) LINK(34,SpecialInfo,"StringInfo");
        else if((n->GadgetType&GTYP_GTYPEMASK)==GTYP_PROPGADGET) LINK(34,SpecialInfo,"PropInfo");
    }
    else if(!strcmp(name,"IntuiText"))
    { struct IntuiText *n=native;B(0,n->FrontPen);B(1,n->BackPen);B(2,n->DrawMode);W(4,n->LeftEdge);W(6,n->TopEdge);LINK(12,IText,NULL);LINK(16,NextText,"IntuiText"); }
    else if(!strcmp(name,"StringInfo"))
    { struct StringInfo *n=native;LINK(0,Buffer,NULL);W(8,n->BufferPos);W(10,n->MaxChars);W(12,n->DispPos);W(16,n->NumChars);W(18,n->DispCount);W(20,n->CLeft);W(22,n->CTop);L(28,n->LongInt); }
    else if(!strcmp(name,"PropInfo"))
    { struct PropInfo *n=native;W(0,n->Flags);W(2,n->HorizPot);W(4,n->VertPot);W(6,n->HorizBody);W(8,n->VertBody);W(10,n->CWidth);W(12,n->CHeight);W(14,n->HPotRes);W(16,n->VPotRes);W(18,n->LeftBorder);W(20,n->TopBorder); }
    else if(!strcmp(name,"Menu"))
    /* MenuName is BYTE * in AROS and const in other SDKs */
    { struct Menu *n=native;LINK(0,NextMenu,"Menu");W(4,n->LeftEdge);W(6,n->TopEdge);W(8,n->Width);W(10,n->Height);W(12,n->Flags);L(14,snapshot(ctx,NULL,(void *)n->MenuName,depth+1));LINK(18,FirstItem,"MenuItem"); }
    else if(!strcmp(name,"MenuItem"))
    { struct MenuItem *n=native;LINK(0,NextItem,"MenuItem");W(4,n->LeftEdge);W(6,n->TopEdge);W(8,n->Width);W(10,n->Height);W(12,n->Flags);L(14,n->MutualExclude);B(26,n->Command);LINK(28,SubItem,"MenuItem");W(32,n->NextSelect);if(n->Flags&ITEMTEXT) LINK(18,ItemFill,"IntuiText"); }
    return g;
}
#undef LINK
#undef B
#undef W
#undef L
/* After a snapshot that fitted: the mirrors of what it did not reach, the
   windows, menus, gadgets and strings gone since the last one, go back.
   The views that it reached keep their guest addresses. */
static void prune(struct KaffeineContext *ctx, struct KaffeineIBase *s)
{
    ULONG i, kept=0;
    for(i=0;i<s->count;++i)
        if(s->views[i].epoch==s->epoch) s->views[kept++]=s->views[i];
        else Kaffeine_MirrorRelease(ctx,s->views[i].guest);
    s->count=kept;
}
/* After one that did not: every mirror goes back, so that no guest token
   of it or of an older snapshot outlives the refusal. */
static void drop(struct KaffeineContext *ctx, struct KaffeineIBase *s)
{
    ULONG i;
    for(i=0;i<s->count;++i)Kaffeine_MirrorRelease(ctx,s->views[i].guest);
    s->count=0;
}
IPTR Kaffeine_LockIBase(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineIBase *s=ctx->ibase;ULONG who=caller(ctx),base=Kaffeine_FindLibBase(ctx,"intuition.library");
    if(THUNK_D(0) || !base || !who || ctx->broker_call || ctx->callback_depth || FindTask(NULL)!=ctx->exec_task) return refuse(ctx,"invalid lock owner or selector");
    if(s && s->depth) { if(s->owner!=who || s->depth==1024)return refuse(ctx,"lock nesting or owner");++s->depth;return s->key; }
    if(Kaffeine_LayerInfoLocked(ctx))return refuse(ctx,"layer lock order");
    if(!s) { s=AllocVec(sizeof(*s),MEMF_PUBLIC|MEMF_CLEAR);if(!s)return refuse(ctx,"no private snapshot storage");ctx->ibase=s; }
    ctx->ibase_locked=Kaffeine_IBaseLocked;ctx->ibase_cleanup=Kaffeine_IBaseCleanup;ctx->ibase_task_removed=Kaffeine_IBaseTaskRemoved;
    s->key=LockIBase(0);s->depth=1;s->owner=who;s->failed=FALSE;s->reached=0;
    /* Epoch 0 is never current: after the wrap no view counts as reached
       until this snapshot reaches it again. */
    if(++s->epoch==0) { ULONG i;s->epoch=1;for(i=0;i<s->count;++i)s->views[i].epoch=0; }
    /* Preserve this fake library's own size/open bookkeeping. */
    m68k_write16(ctx,base+20,IntuitionBase->LibNode.lib_Version);
    m68k_write16(ctx,base+22,IntuitionBase->LibNode.lib_Revision);
    m68k_write32(ctx,base+52,snapshot(ctx,"Window",IntuitionBase->ActiveWindow,0));
    m68k_write32(ctx,base+56,snapshot(ctx,"Screen",IntuitionBase->ActiveScreen,0));
    m68k_write32(ctx,base+60,snapshot(ctx,"Screen",IntuitionBase->FirstScreen,0));
    m68k_write32(ctx,base+64,IntuitionBase->Flags);
    m68k_write16(ctx,base+68,IntuitionBase->MouseY);m68k_write16(ctx,base+70,IntuitionBase->MouseX);
    m68k_write32(ctx,base+72,IntuitionBase->Seconds);m68k_write32(ctx,base+76,IntuitionBase->Micros);
    if(s->failed)
    {
        UnlockIBase(s->key);s->depth=0;s->owner=0;
        m68k_write32(ctx,base+52,0);m68k_write32(ctx,base+56,0);m68k_write32(ctx,base+60,0);
        drop(ctx,s);return refuse(ctx,"snapshot capacity exceeded");
    }
    prune(ctx,s);
    return s->key;
}
IPTR Kaffeine_UnlockIBase(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineIBase *s=ctx->ibase;
    if(!s || !s->depth || s->owner!=caller(ctx) || THUNK_A(0)!=s->key)return refuse(ctx,"unmatched unlock");
    if(!--s->depth) { UnlockIBase(s->key);s->owner=0; }
    return 0;
}
void Kaffeine_IBaseTaskRemoved(struct KaffeineContext *ctx, ULONG task)
{
    struct KaffeineIBase *s=ctx->ibase;
    if(s && s->depth && s->owner==task) { UnlockIBase(s->key);s->depth=0;s->owner=0; }
}
void Kaffeine_IBaseCleanup(struct KaffeineContext *ctx)
{
    struct KaffeineIBase *s=ctx->ibase;ULONG i;if(!s)return;
    if(s->depth)UnlockIBase(s->key);
    for(i=0;i<s->count;++i)Kaffeine_MirrorRelease(ctx,s->views[i].guest);
    FreeVec(s);ctx->ibase=NULL;ctx->ibase_locked=NULL;ctx->ibase_cleanup=NULL;ctx->ibase_task_removed=NULL;
}
static struct IBaseView *active(struct KaffeineContext *ctx, ULONG guest, const char *kind)
{
    struct KaffeineIBase *s=ctx->ibase;ULONG i;
    if(!s || !s->depth || s->owner!=caller(ctx))return NULL;
    for(i=0;i<s->count;++i) { struct IBaseView *v=&s->views[i];
        if(v->guest==guest && v->epoch==s->epoch && v->layout && !strcmp(v->layout->name,kind))return v; }
    return NULL;
}
/* A BitMap view, or the BitMap embedded in a Screen snapshot, which carries
   the scalars of that screen's RastPort BitMap */
struct BitMap *Kaffeine_IBaseBitmap(struct KaffeineContext *ctx, ULONG guest)
{
    struct KaffeineIBase *s=ctx->ibase;struct IBaseView *v=active(ctx,guest,"BitMap");ULONG i;
    if(v)return v->native;
    if(!s || !s->depth || s->owner!=caller(ctx))return NULL;
    for(i=0;i<s->count;++i) { v=&s->views[i];if(v->epoch==s->epoch && v->layout && !strcmp(v->layout->name,"Screen") && guest==v->guest+M68K_SCR_BITMAP)return ((struct Screen*)v->native)->RastPort.BitMap; }
    return NULL;
}
struct RastPort *Kaffeine_IBaseRastPort(struct KaffeineContext *ctx, ULONG guest)
{
    struct KaffeineIBase *s=ctx->ibase;struct IBaseView *v=active(ctx,guest,"RastPort");ULONG i;
    if(v)return v->native;
    if(!s || !s->depth || s->owner!=caller(ctx))return NULL;
    for(i=0;i<s->count;++i) { v=&s->views[i];if(v->epoch==s->epoch && v->layout && !strcmp(v->layout->name,"Screen") && guest==v->guest+M68K_SCR_RASTPORT)return &((struct Screen*)v->native)->RastPort; }
    return NULL;
}
IPTR Kaffeine_IBaseVPMode(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineIBase *s=ctx->ibase;ULONG guest=THUNK_A(0),i;struct ViewPort *vp=NULL;
    if(s && s->depth && s->owner==caller(ctx))
        for(i=0;i<s->count;++i) { struct IBaseView *v=&s->views[i];if(v->epoch==s->epoch && v->layout && !strcmp(v->layout->name,"Screen") && guest==v->guest+M68K_SCR_VIEWPORT)vp=&((struct Screen*)v->native)->ViewPort; }
    if(!vp)vp=Kaffeine_ShadowNative(ctx,guest,KAFFEINE_SHADOW_VIEWPORT);
    return vp?GetVPModeID(vp):~0UL;
}
BOOL Kaffeine_IBaseScreenHold(struct KaffeineContext *ctx, ULONG guest, struct Screen **out)
{
    struct KaffeineIBase *s=ctx->ibase;ULONG i;struct Screen *candidate=NULL;
    *out=NULL;if(Kaffeine_IBaseLocked(ctx))return FALSE;
    if(s)for(i=0;i<s->count;++i)if(s->views[i].guest==guest && s->views[i].layout && !strcmp(s->views[i].layout->name,"Screen"))candidate=s->views[i].native;
    if(!candidate)candidate=Kaffeine_ScreenNative(ctx,guest);
    if(!candidate)return FALSE;
    /* PublicHold first compares only pointer identities under the native
       public-list lock, then acquires the exact matching visitor. No stale
       candidate is dereferenced, and IBase is never held across native IO. */
    if(Kaffeine_ScreenPublicHold(candidate))*out=candidate;
    return *out!=NULL;
}
void Kaffeine_IBaseScreenDrop(struct Screen *screen)
{ if(screen)UnlockPubScreen(NULL,screen); }

/* A validated read-only screen token is not a generic mutable shadow.
   Revalidate by a real public visitor lease before queuing screen depth.
   Screens already owned/borrowed by the screen bridge retain its lease. */
IPTR Kaffeine_IBaseScreenToFront(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest=THUNK_A(0);struct Screen *native;
    if(Kaffeine_IBaseLocked(ctx))return refuse(ctx,"ScreenToFront inside IBase");
    native=Kaffeine_ScreenNative(ctx,guest);
    if(native) { ScreenToFront(native);return 0; }
    if(!Kaffeine_IBaseScreenHold(ctx,guest,&native))return refuse(ctx,"unleased screen depth target");
    ScreenToFront(native);Kaffeine_IBaseScreenDrop(native);return 0;
}
