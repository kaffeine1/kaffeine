/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)

   Bounded read-only classic snapshots. Native identity and lock ownership
   stay private; no guest field is a native capability. A Window or Screen
   of the program is its own 68k object in a snapshot, never a mirror, and
   the roots that a program may read without LockIBase follow Intuition
   between snapshots. A screen that a program of this library opened is
   known by its native and its registry generation (kaffeine_owned_screens.c)
   in views and roots alike, so that another screen that opens at the same
   native address is another object; and the address of a Screen view that
   goes is retired, never reused for another object (kaffeine_screen_book.h). */
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
#include "kaffeine_screen_book.h"
/* One snapshot reaches at most CURRENT views (a Personal Paint screen with
   its menus takes about 1100). The table has room for the views of the
   last snapshot that fitted and as many new ones: a view keeps its guest
   address while the snapshots reach its native object, and its mirror goes
   back after the first snapshot that fits without reaching it. */
#define CURRENT 2048
#define SLOTS (2 * CURRENT)
#define MAX_DEPTH 128
/* gen: the registry generation of a Screen view, else 0 */
struct IBaseView { const struct M68KStructLayout *layout; void *native;
                   ULONG guest, size, epoch, gen; BOOL string; };
/* ActiveWindow, ActiveScreen and FirstScreen, at base+52, +56 and +60 */
#define ROOTS 3
struct KaffeineIBase { struct IBaseView views[SLOTS]; ULONG count, reached, epoch, depth, owner, key, base; BOOL failed;
                       /* the epoch that last reached each shadow-map entry as the program's own object */
                       ULONG visit[KAFFEINE_MAX_SHADOWS];
                       /* natives, compared and never dereferenced: those that the roots were
                          written from (rooted); those of the last refresh that left a root of
                          another program unwritten (pending); those of a refresh snapshot that
                          did not fit (refusal), tried again after backoff refresh calls */
                       void *root[ROOTS], *seen[ROOTS], *refused[ROOTS]; BOOL rooted[ROOTS], pending, refusal;
                       ULONG backoff, waited;
                       /* the generations that go with those natives, and those of the current
                          natives, kept while the natives and the registry's epoch stay */
                       ULONG root_gen[ROOTS], seen_gen[ROOTS], refused_gen[ROOTS];
                       void *gen_native[ROOTS]; ULONG gens[ROOTS], gen_epoch; BOOL gens_valid; };
static IPTR refuse(struct KaffeineContext *ctx, const char *why)
{ bug("[kaffeine] IBase: %s, stopping (20)\n",why); Kaffeine_StopGuest(ctx,ctx->cpu,20); return 20; }
static ULONG caller(struct KaffeineContext *ctx)
{ return ctx->sched && ctx->task_ops ? ctx->task_ops->caller_task(ctx,ctx->cpu) : ctx->guest_exec_task; }
BOOL Kaffeine_IBaseLocked(struct KaffeineContext *ctx)
{ return ctx->ibase && ctx->ibase->depth; }
/* The registry generation of a screen that a program of this library
   opened, OPEN or CLOSING, else 0. The native is compared, never
   dereferenced; the registry lock comes after IBase. */
static ULONG screen_gen(struct KaffeineContext *ctx, const void *native)
{
    const struct KaffeineOwnedScreenOps *o=ctx->owned_screen_ops;
    return o && native ? o->generation(ctx->emuBase,(const struct Screen *)native) : 0;
}
static BOOL screen_view(const struct IBaseView *v)
{ return v->layout && !strcmp(v->layout->name,"Screen"); }
/* A view goes: a Screen address may still be in the program's hands, so
   it is retired, every other mirror goes back */
static void view_release(struct KaffeineContext *ctx, const struct IBaseView *v)
{
    if(screen_view(v))Kaffeine_ScreenRetire(ctx,KAFFEINE_BOOK_VIEW,v->native,v->gen,v->guest,v->size);
    else Kaffeine_MirrorRelease(ctx,v->guest);
}
static struct IBaseView *view(struct KaffeineContext *ctx, void *native, const char *name, ULONG gen)
{
    struct KaffeineIBase *s=ctx->ibase; const struct M68KStructLayout *layout=name?shadow_find_layout(name):NULL;
    ULONG i, size=name?(layout?layout->m68k_size:0):256; struct IBaseView *v;
    if (!native || !size) return NULL;
    for (i=0;i<s->count;++i)
        if (s->views[i].native==native && s->views[i].layout==layout && s->views[i].string==!name &&
            s->views[i].gen==gen) return &s->views[i];
    if (s->count==SLOTS) { s->failed=TRUE; return NULL; }
    v=&s->views[s->count];
    /* the address that this very screen had in a view before */
    v->guest=layout && !strcmp(layout->name,"Screen") ? Kaffeine_ScreenRevive(ctx,KAFFEINE_BOOK_VIEW,native,gen,size) : 0;
    if (!v->guest) v->guest=Kaffeine_MirrorAlloc(ctx,size);
    if (!v->guest) { s->failed=TRUE; return NULL; }
    /* A slot that prune() or drop() freed still holds an old view: a new
       mirror takes epoch 0, which is never current, and is copied now. */
    v->size=size;v->native=native;v->layout=layout;v->string=!name;v->epoch=0;v->gen=gen;++s->count;return v;
}
/* Only classic offsets are fixed. Native members are always C field
   accesses: native Library/Layer_Info layouts differ across AROS builds. */
static ULONG snapshot(struct KaffeineContext *, const char *, void *, unsigned);
static ULONG token(struct KaffeineContext *, int, const char *, void *, unsigned);
/* The program's own 68k Window or Screen of a native one: the index of its
   shadow-map entry, or -1. Pointers are compared, never dereferenced. A
   BuildSysRequest() window has a WINDOW entry too. A screen is one that the
   program opened, else one that it holds through LockPubScreen() or a
   window (PUBSCREEN, as Kaffeine_ScreenGuest()). */
static int owned(struct KaffeineContext *ctx, const void *native, BOOL window)
{
    UWORD i;int pub=-1;
    for(i=0;native && i<ctx->num_shadows;++i) if(ctx->shadow_map[i].native_ptr==native)
    {
        UWORD type=ctx->shadow_map[i].type;
        if(window?type==KAFFEINE_SHADOW_WINDOW:type==KAFFEINE_SHADOW_SCREEN)return i;
        if(!window && type==KAFFEINE_SHADOW_PUBSCREEN)pub=i;
    }
    return pub;
}
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
    if (name && (!strcmp(name,"Window") || !strcmp(name,"Screen")))
    { int k=owned(ctx,native,*name=='W');if(k>=0)return token(ctx,k,name,native,depth); }
    v=view(ctx,native,name,name && !strcmp(name,"Screen") ? screen_gen(ctx,native) : 0);if(!v)return 0;g=v->guest;
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
/* The program's own Window or Screen is its 68k object, never a mirror.
   Only what Intuition keeps and a program only reads is written, once per
   snapshot: the list links, which name objects of this snapshot, and the
   scalars of Kaffeine_WindowSync() that need no collaborator. What the
   bridges keep (WScreen, UserPort, RPort, MenuStrip, gadgets, Flags, the
   layers, the BitMap handle, the ViewPort) stays as it is. */
static ULONG token(struct KaffeineContext *ctx, int k, const char *name, void *native, unsigned depth)
{
    struct KaffeineIBase *s=ctx->ibase;ULONG g=ctx->shadow_map[k].m68k_addr;
    if(s->visit[k]==s->epoch)return g;
    if(++s->reached>CURRENT) { s->failed=TRUE;return 0; }
    s->visit[k]=s->epoch;
    if(*name=='W')
    {
        struct Window *n=native;
        LINK(0,NextWindow,"Window");W(4,n->LeftEdge);W(6,n->TopEdge);W(8,n->Width);W(10,n->Height);
        W(12,n->MouseY);W(14,n->MouseX);
        W(108,n->GZZMouseX);W(110,n->GZZMouseY);W(112,n->GZZWidth);W(114,n->GZZHeight);
    }
    else
    { struct Screen *n=native;LINK(0,NextScreen,"Screen");LINK(4,FirstWindow,"Window");W(16,n->MouseY);W(18,n->MouseX); }
    return g;
}
/* The list links that token() writes. After a snapshot that fitted, those
   of the program's objects that it did not reach go to 0, as their views
   may go back now; after one that did not, or when an object of the
   program goes, all of them, so that no link of the program's own objects
   names a mirror that has gone back. */
static void unlink_owned(struct KaffeineContext *ctx, struct KaffeineIBase *s, BOOL all)
{
    UWORD i;
    for(i=0;i<ctx->num_shadows;++i)
    {
        UWORD type=ctx->shadow_map[i].type;ULONG g=ctx->shadow_map[i].m68k_addr;
        if(!all && s->visit[i]==s->epoch)continue;
        if(type==KAFFEINE_SHADOW_WINDOW)L(0,0);
        else if(type==KAFFEINE_SHADOW_SCREEN || type==KAFFEINE_SHADOW_PUBSCREEN) { L(0,0);L(4,0); }
    }
}
#undef LINK
#undef B
#undef W
#undef L
/* After a snapshot that fitted: the mirrors of what it did not reach, the
   windows, menus, gadgets and strings gone since the last one, go back,
   and the screens' addresses are retired. The views that it reached keep
   their guest addresses. */
static void prune(struct KaffeineContext *ctx, struct KaffeineIBase *s)
{
    ULONG i, kept=0;
    for(i=0;i<s->count;++i)
        if(s->views[i].epoch==s->epoch) s->views[kept++]=s->views[i];
        else view_release(ctx,&s->views[i]);
    s->count=kept;
}
/* After one that did not: every view goes, so that no guest token of it or
   of an older snapshot names a live object after the refusal; a screen's
   address names one again only for that very screen. */
static void drop(struct KaffeineContext *ctx, struct KaffeineIBase *s)
{
    ULONG i;
    for(i=0;i<s->count;++i)view_release(ctx,&s->views[i]);
    s->count=0;
}
static const char *const root_kind[ROOTS]={"Window","Screen","Screen"};
static void *root_native(int i)
{ return i==0?(void *)IntuitionBase->ActiveWindow:i==1?(void *)IntuitionBase->ActiveScreen:(void *)IntuitionBase->FirstScreen; }
/* The scalars that a program reads with the roots */
static void scalars(struct KaffeineContext *ctx, ULONG base)
{
    m68k_write32(ctx,base+64,IntuitionBase->Flags);
    m68k_write16(ctx,base+68,IntuitionBase->MouseY);m68k_write16(ctx,base+70,IntuitionBase->MouseX);
    m68k_write32(ctx,base+72,IntuitionBase->Seconds);m68k_write32(ctx,base+76,IntuitionBase->Micros);
}
/* The same natives, and for the screens the same generations */
static BOOL same(void *const *a, const ULONG *ag, void *const *b, const ULONG *bg)
{ return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && ag[1]==bg[1] && ag[2]==bg[2]; }
/* The generations of the current roots, looked up again only when a native
   or the registry's epoch has changed since: three comparisons otherwise */
static void root_gens(struct KaffeineContext *ctx, struct KaffeineIBase *s, void *const *n, ULONG *g)
{
    const struct KaffeineOwnedScreenOps *o=ctx->owned_screen_ops;
    ULONG e=o?o->epoch(ctx->emuBase):0;int i;
    if(!s->gens_valid || e!=s->gen_epoch || n[0]!=s->gen_native[0] || n[1]!=s->gen_native[1] || n[2]!=s->gen_native[2])
    {
        s->gen_epoch=e;s->gens_valid=TRUE;
        for(i=0;i<ROOTS;++i) { s->gen_native[i]=n[i];s->gens[i]=i?screen_gen(ctx,n[i]):0; }
    }
    for(i=0;i<ROOTS;++i)g[i]=s->gens[i];
}
/* Private storage, and the collaborators that it answers, for the program */
static struct KaffeineIBase *storage(struct KaffeineContext *ctx)
{
    struct KaffeineIBase *s=ctx->ibase;
    if(!s) { s=AllocVec(sizeof(*s),MEMF_PUBLIC|MEMF_CLEAR);if(!s)return NULL;ctx->ibase=s; }
    ctx->ibase_locked=Kaffeine_IBaseLocked;ctx->ibase_cleanup=Kaffeine_IBaseCleanup;ctx->ibase_task_removed=Kaffeine_IBaseTaskRemoved;
    ctx->ibase_identity=Kaffeine_IBaseIdentity;
    return s;
}
/* One snapshot of the roots under the native lock, which the caller holds:
   the natives are read here, after the lock. FALSE when it does not fit:
   the roots and the program's links are 0 before every view goes back. */
static BOOL take(struct KaffeineContext *ctx, struct KaffeineIBase *s, ULONG base)
{
    ULONG g[ROOTS],gen[ROOTS];void *n[ROOTS];int i;
    s->failed=FALSE;s->reached=0;
    /* Epoch 0 is never current: after the wrap no view and no object of the
       program counts as reached until this snapshot reaches it again. */
    if(++s->epoch==0)
    {
        ULONG k;s->epoch=1;
        for(k=0;k<s->count;++k)s->views[k].epoch=0;
        for(k=0;k<KAFFEINE_MAX_SHADOWS;++k)s->visit[k]=0;
    }
    for(i=0;i<ROOTS;++i) { n[i]=root_native(i);gen[i]=i?screen_gen(ctx,n[i]):0;g[i]=snapshot(ctx,root_kind[i],n[i],0); }
    scalars(ctx,base);
    if(s->failed)
    {
        /* A refresh tries the same natives again after 64 calls, then after
           twice as many each time, up to 2^20: capacity or mirrors may come
           back while the roots stay the same, but no math loop may take a
           failing snapshot on every call. */
        s->backoff=s->refusal && same(n,gen,s->refused,s->refused_gen) && s->backoff ? (s->backoff<(1UL<<20)?2*s->backoff:s->backoff) : 64;
        for(i=0;i<ROOTS;++i) { m68k_write32(ctx,base+52+4*i,0);s->rooted[i]=FALSE;s->refused[i]=n[i];s->refused_gen[i]=gen[i]; }
        s->refusal=TRUE;s->waited=0;unlink_owned(ctx,s,TRUE);drop(ctx,s);return FALSE;
    }
    for(i=0;i<ROOTS;++i) { m68k_write32(ctx,base+52+4*i,g[i]);s->root[i]=n[i];s->root_gen[i]=gen[i];s->rooted[i]=TRUE; }
    s->pending=s->refusal=FALSE;unlink_owned(ctx,s,FALSE);prune(ctx,s);
    return TRUE;
}
IPTR Kaffeine_LockIBase(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineIBase *s=ctx->ibase;ULONG who=caller(ctx),base=Kaffeine_FindLibBase(ctx,"intuition.library");
    if(THUNK_D(0) || !base || !who || ctx->broker_call || ctx->callback_depth || FindTask(NULL)!=ctx->exec_task) return refuse(ctx,"invalid lock owner or selector");
    if(s && s->depth) { if(s->owner!=who || s->depth==1024)return refuse(ctx,"lock nesting or owner");++s->depth;return s->key; }
    if(Kaffeine_LayerInfoLocked(ctx))return refuse(ctx,"layer lock order");
    if(!(s=storage(ctx)))return refuse(ctx,"no private snapshot storage");
    s->key=LockIBase(0);s->depth=1;s->owner=who;s->base=base;
    /* Preserve this fake library's own size/open bookkeeping. */
    m68k_write16(ctx,base+20,IntuitionBase->LibNode.lib_Version);
    m68k_write16(ctx,base+22,IntuitionBase->LibNode.lib_Revision);
    if(!take(ctx,s,base))
    {
        UnlockIBase(s->key);s->depth=0;s->owner=0;
        return refuse(ctx,"snapshot capacity exceeded");
    }
    return s->key;
}
/* Whether a refresh may take the native IBase lock here: never inside a
   native callback or broker call, under a lock of the program's that
   Intuition may wait for, or on another task. */
static BOOL admitted(struct KaffeineContext *ctx)
{
    return ctx->running && !ctx->broker_call && !ctx->callback_depth && !ctx->pubscreen_list_locks &&
           !Kaffeine_LayerInfoLocked(ctx) &&
           !(ctx->layer_ops && ctx->layer_ops->has_locks && ctx->layer_ops->has_locks(ctx)) &&
           FindTask(NULL)==ctx->exec_task;
}
/* The roots that need no lock: NULL, and the program's own objects, found
   by pointer comparison alone. A root of another program keeps its value,
   a view that stays until the next snapshot rewrites the roots. TRUE when
   such a root waits for a snapshot of exactly these natives, with these
   generations. */
static BOOL settle(struct KaffeineContext *ctx, struct KaffeineIBase *s, void *const *n, const ULONG *g)
{
    int i;BOOL foreign=FALSE;
    for(i=0;i<ROOTS;++i) { s->seen[i]=n[i];s->seen_gen[i]=g[i];if(!s->rooted[i] || n[i]!=s->root[i] || g[i]!=s->root_gen[i])
    {
        int k=n[i]?owned(ctx,n[i],!i):-1;
        if(n[i] && k<0) { foreign=TRUE;continue; }
        m68k_write32(ctx,s->base+52+4*i,k<0?0:ctx->shadow_map[k].m68k_addr);s->root[i]=n[i];s->root_gen[i]=g[i];s->rooted[i]=TRUE;
    } }
    return s->pending=foreign;
}
/* ctx->ibase_refresh, after each library call of the program: the roots
   that it may read without LockIBase, as Intuition has them now. Nothing
   while the program holds IBase, as it reads its own snapshot then. The
   fast path compares three native pointers. A root of another program
   needs a snapshot, taken under the native lock where that is admitted,
   else on a later call for the same natives; one that did not fit is
   taken again for the same natives only after its backoff. A screen of
   another generation at the same native counts as another root; finding
   that out costs one more comparison while the registry stays the same. */
void Kaffeine_IBaseRefresh(struct KaffeineContext *ctx)
{
    struct KaffeineIBase *s=ctx->ibase;void *n[ROOTS];ULONG g[ROOTS];int i;ULONG key;BOOL fitted;
    if(s && s->depth)return;
    if(!s || !s->base)
    {
        ULONG base=Kaffeine_FindLibBase(ctx,"intuition.library");
        if(!base || !(s=storage(ctx)))return;
        s->base=base;
    }
    scalars(ctx,s->base);
    for(i=0;i<ROOTS;++i)n[i]=root_native(i);
    root_gens(ctx,s,n,g);
    if(!s->pending || !same(n,g,s->seen,s->seen_gen))
    {
        if(s->rooted[0] && s->rooted[1] && s->rooted[2] && same(n,g,s->root,s->root_gen))return;
        if(!settle(ctx,s,n,g))return;
    }
    if(s->refusal && same(n,g,s->refused,s->refused_gen) && ++s->waited<s->backoff)return;
    if(!admitted(ctx))return;
    key=LockIBase(0);fitted=take(ctx,s,s->base);UnlockIBase(key);
    if(fitted)return;
    bug("[kaffeine] IBase: roots of other programs do not fit a snapshot, left 0\n");
    /* The roots that need no lock again, from the natives as they are now,
       read after the unlock: not those of before the lock, which may have
       gone in between. The others stay 0. */
    for(i=0;i<ROOTS;++i)n[i]=root_native(i);
    root_gens(ctx,s,n,g);
    settle(ctx,s,n,g);
}
/* ctx->ibase_identity: a Window or Screen of the program has come or goes.
   A root that names one that goes is 0 at once, as are the links of the
   program's objects; every root is taken again by the next refresh, so a
   native that the program now holds becomes its own object there, and one
   that it no longer holds a view again. */
void Kaffeine_IBaseIdentity(struct KaffeineContext *ctx, ULONG guest, BOOL gone)
{
    struct KaffeineIBase *s=ctx->ibase;int i;
    if(!s)return;
    for(i=0;i<ROOTS;++i)
    {
        if(gone && s->base && m68k_read32(ctx,s->base+52+4*i)==guest)m68k_write32(ctx,s->base+52+4*i,0);
        s->rooted[i]=FALSE;
    }
    s->pending=s->refusal=FALSE;
    if(gone)unlink_owned(ctx,s,TRUE);
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
    /* no root names a mirror that goes back */
    if(s->base)for(i=0;i<ROOTS;++i)m68k_write32(ctx,s->base+52+4*i,0);
    for(i=0;i<s->count;++i)view_release(ctx,&s->views[i]);
    FreeVec(s);ctx->ibase=NULL;ctx->ibase_locked=NULL;ctx->ibase_cleanup=NULL;ctx->ibase_task_removed=NULL;
    /* ibase_refresh stays: it was set with the intuition.library base, which
       outlives this storage, and allocates it again if the program goes on */
    ctx->ibase_identity=NULL;
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

/* The screen that an IEPointerPixel of the program names, held for one
   synchronous IND_WRITEEVENT, as kaffeine_exec_io.c holds it from before
   its DoIO() until right after. The guest address is a Screen view, with
   the native and generation that the snapshot captured, or the program's
   own 68k Screen. A screen that a program of this library opened is held
   by the gate of its registry slot, and only while it is OPEN with that
   very generation: never by a public visitor lock instead. Any other
   screen needs a real public visitor lock, taken first, which keeps it
   open; then, with the public list unlocked, the registry must still know
   it as no program's screen, and must not be binding a screen just opened,
   which could be this one. No lock is held across the DoIO(). */
BOOL Kaffeine_IBaseInputScreenHold(struct KaffeineContext *ctx, ULONG guest, struct KaffeineScreenHold *hold)
{
    struct KaffeineIBase *s=ctx->ibase;const struct KaffeineOwnedScreenOps *o=ctx->owned_screen_ops;
    struct Screen *native=NULL;ULONG gen=0,i;BOOL known=FALSE;LONG slot;
    hold->screen=NULL;hold->slot=-1;hold->visitor=FALSE;
    if(!guest || Kaffeine_IBaseLocked(ctx))return FALSE;
    if(s)for(i=0;i<s->count;++i)if(s->views[i].guest==guest && screen_view(&s->views[i]))
    { native=s->views[i].native;gen=s->views[i].gen;known=TRUE; }
    for(i=0;!known && i<KAFFEINE_MAX_PUBSCREENS;++i)if(guest==ctx->pubscreens[i].guest && ctx->pubscreens[i].native)
    { native=ctx->pubscreens[i].native;gen=ctx->pubscreens[i].generation;known=TRUE; }
    if(!known && (native=Kaffeine_ScreenNative(ctx,guest)))gen=screen_gen(ctx,native);
    if(!native)return FALSE;
    if(gen)
    {
        if(!o || !o->hold(ctx->emuBase,native,gen,&slot))return FALSE;
        hold->screen=native;hold->slot=slot;return TRUE;
    }
    if(!Kaffeine_ScreenPublicHold(native))return FALSE;
    if(o && !o->foreign(ctx->emuBase,native)) { UnlockPubScreen(NULL,native);return FALSE; }
    hold->screen=native;hold->visitor=TRUE;return TRUE;
}
void Kaffeine_IBaseInputScreenDrop(struct KaffeineContext *ctx, struct KaffeineScreenHold *hold)
{
    if(hold->visitor)UnlockPubScreen(NULL,hold->screen);
    else if(hold->screen && hold->slot>=0 && ctx->owned_screen_ops)ctx->owned_screen_ops->drop(ctx->emuBase,hold->slot);
    hold->screen=NULL;hold->slot=-1;hold->visitor=FALSE;
}

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
