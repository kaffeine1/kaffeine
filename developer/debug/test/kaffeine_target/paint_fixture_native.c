/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Author: Michele Dipace (@kaffeine1)
 * Original foreign-native paint-Hook regression. No application input.
 * PAINT_FIXTURE_EMIT_ONLY builds the bounded HUNK emitter without AROS.
 * Target execution needs UP AROS, a managed public screen and an external
 * deadline: RunHunk and a faulty native graphics implementation may block.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define PF_TITLE "Kaffeine paint fixture"
#define PF_READY "RAM:kaffeine-paint-ready"
#define PF_SIGNALLED "RAM:kaffeine-paint-signalled"
#define PF_ACK "RAM:kaffeine-paint-ack"
#define PF_HUNK "RAM:kaffeine-paint.hunk"
#define PF_OBSERVER "kaffeine-paint-observer"
#define PF_CTRL_D 0x00002000u

struct PaintFixtureLayout {
    uint32_t code_bytes, hook_entry, hook_header, hook_data, pen, new_window;
};
enum PFLabel {
    PF_START, PF_FAIL, PF_CLEANUP, PF_NO_HOOK, PF_NO_WINDOW,
    PF_NO_LAYERS, PF_NO_GRAPHICS, PF_NO_INTUITION, PF_NO_DOS,
    PF_ACK_LOOP, PF_ACK_DELAY, PF_ACK_SEEN, PF_MAKE_MARKER, PF_MARKER_BAD,
    PF_HOOK_ENTRY, PF_COPY_LOOP, PF_NO_SIGNAL, PF_HOOK_HEADER, PF_HOOK_DATA,
    PF_PEN, PF_NEW_WINDOW, PF_WINDOW, PF_LAYER, PF_OLD_HOOK, PF_INSTALLED,
    PF_DOS_BASE, PF_INT_BASE, PF_GFX_BASE, PF_LAY_BASE,
    PF_DOS_NAME, PF_INT_NAME, PF_GFX_NAME, PF_LAY_NAME, PF_WINDOW_TITLE,
    PF_READY_NAME, PF_SIGNALLED_NAME, PF_ACK_NAME, PF_LABELS
};
struct PFFix { uint32_t at; int label, add, relative; };
struct PFBuilder {
    uint8_t code[8192]; uint32_t used, reloc[256], relocs;
    int32_t label[PF_LABELS]; struct PFFix fix[256]; unsigned fixes; int bad;
};
static struct PFBuilder pf;
static void pf_w(uint16_t v)
{
    if (pf.used > sizeof(pf.code)-2) { pf.bad=1; return; }
    pf.code[pf.used++]=(uint8_t)(v>>8);pf.code[pf.used++]=(uint8_t)v;
}
static void pf_l(uint32_t v) { pf_w((uint16_t)(v>>16));pf_w((uint16_t)v); }
static void pf_put32(uint8_t*p,uint32_t v)
{p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static void pf_mark(int id)
{if(id<0 || id>=PF_LABELS || pf.label[id]>=0)pf.bad=1;else pf.label[id]=(int32_t)pf.used;}
static void pf_fix(int id,int add,int relative)
{
    if(pf.fixes==256 || (!relative && pf.relocs==256)){pf.bad=1;return;}
    pf.fix[pf.fixes++]=(struct PFFix){pf.used,id,add,relative};
    if(relative)pf_w(0);else {pf.reloc[pf.relocs++]=pf.used;pf_l(0);}
}
static void pf_align(void) { while(pf.used&3)pf_w(0); }
static void pf_zero(int id,unsigned bytes)
{pf_align();pf_mark(id);while(bytes>=2){pf_w(0);bytes-=2;}if(bytes)pf.bad=1;}
static void pf_text(int id,const char*s)
{
    size_t n=strlen(s)+1,i;pf_align();pf_mark(id);
    if(n>sizeof(pf.code)-pf.used){pf.bad=1;return;}
    for(i=0;i<n;++i)pf.code[pf.used++]=(uint8_t)s[i];
    if(pf.used&1){if(pf.used==sizeof(pf.code))pf.bad=1;else pf.code[pf.used++]=0;}
}
static void pf_branch(unsigned op,int id) { pf_w((uint16_t)op);pf_fix(id,0,1); }
static void pf_call(unsigned lvo) { pf_w(0x4eae);pf_w((uint16_t)-(int)lvo); }
static void pf_exec(void) { pf_w(0x2c78);pf_w(4); } /* MOVEA.L AbsExecBase.w,A6 */
static void pf_ma(unsigned src,unsigned dst) {pf_w((uint16_t)(0x2048|(dst<<9)|src));}
static void pf_md(unsigned src,unsigned dst) {pf_w((uint16_t)(0x2000|(dst<<9)|src));}
static void pf_da(unsigned src,unsigned dst) {pf_w((uint16_t)(0x2040|(dst<<9)|src));}
static void pf_ad(unsigned src,unsigned dst) {pf_w((uint16_t)(0x2008|(dst<<9)|src));}
static void pf_imm(unsigned reg,uint32_t value) {pf_w((uint16_t)(0x203c|(reg<<9)));pf_l(value);}
static void pf_addr(unsigned reg,int id,int add) {pf_w((uint16_t)(0x41f9|(reg<<9)));pf_fix(id,add,0);}
static void pf_geta(int id,unsigned reg) {pf_w((uint16_t)(0x2079|(reg<<9)));pf_fix(id,0,0);}
static void pf_save(unsigned reg,int id,int add) {pf_w((uint16_t)(0x23c0|reg));pf_fix(id,add,0);}
static void pf_savea(unsigned reg,int id,int add) {pf_w((uint16_t)(0x23c8|reg));pf_fix(id,add,0);}
static void pf_loada(unsigned base,unsigned off,unsigned dst)
{pf_w((uint16_t)(0x2068|(dst<<9)|base));pf_w((uint16_t)off);}
static void pf_load(unsigned base,unsigned off,unsigned dst)
{pf_w((uint16_t)(0x2028|(dst<<9)|base));pf_w((uint16_t)off);}
static void pf_test(unsigned reg) {pf_w((uint16_t)(0x4a80|reg));}
static void pf_nonzero(void) {pf_test(0);pf_branch(0x6700,PF_FAIL);}
static void pf_path(unsigned reg,int label) {pf_addr(0,label,0);pf_ad(0,reg);}
static void pf_library(int name,int save)
{pf_exec();pf_addr(1,name,0);pf_imm(0,0);pf_call(552);pf_save(0,save,0);pf_nonzero();}
static void pf_close_library(int base,int skip)
{pf_geta(base,1);pf_ad(1,0);pf_test(0);pf_branch(0x6700,skip);pf_exec();pf_call(414);pf_mark(skip);}

size_t pf_build_hunk(uint8_t*out,size_t capacity,struct PaintFixtureLayout*layout)
{
    struct PaintFixtureLayout result;unsigned i;uint32_t at=0,words;
    if(!out || !layout)return 0;
    memset(&pf,0,sizeof(pf));for(i=0;i<PF_LABELS;++i)pf.label[i]=-1;
    pf_mark(PF_START);pf_w(0x48e7);pf_w(0x3f3e); /* D2-D7/A2-A6 */
    pf_w(0x7c14); /* D6=20; retained through every cleanup call */
    pf_w(0x9bcd); /* SUBA.L A5,A5: no DOS base until OpenLibrary succeeds */
    pf_exec();pf_addr(1,PF_DOS_NAME,0);pf_imm(0,0);pf_call(552);pf_da(0,5);pf_nonzero();
    pf_library(PF_INT_NAME,PF_INT_BASE);pf_library(PF_GFX_NAME,PF_GFX_BASE);
    pf_library(PF_LAY_NAME,PF_LAY_BASE);
    pf_exec();pf_w(0x93c9);pf_call(294);pf_nonzero();pf_save(0,PF_HOOK_DATA,8);
    pf_savea(6,PF_HOOK_DATA,0);pf_geta(PF_GFX_BASE,0);pf_savea(0,PF_HOOK_DATA,4);
    pf_addr(0,PF_NEW_WINDOW,0);pf_geta(PF_INT_BASE,6);pf_call(204);pf_save(0,PF_WINDOW,0);pf_nonzero();
    pf_da(0,4);pf_loada(4,124,0); /* classic Window.WLayer, NOT native offsetof */
    pf_ad(0,0);pf_nonzero();pf_savea(0,PF_LAYER,0);
    pf_addr(1,PF_HOOK_HEADER,0);pf_geta(PF_LAY_BASE,6);pf_call(198);pf_save(0,PF_OLD_HOOK,0);
    pf_imm(0,1);pf_save(0,PF_INSTALLED,0);
    /* Remove any signal generated during installation BEFORE ready is visible. */
    pf_exec();pf_imm(0,0);pf_imm(1,PF_CTRL_D);pf_call(306);
    pf_path(1,PF_READY_NAME);pf_branch(0x6100,PF_MAKE_MARKER);pf_nonzero();
    pf_exec();pf_imm(0,PF_CTRL_D);pf_call(318);
    pf_w(0x0c80);pf_l(PF_CTRL_D);pf_branch(0x6600,PF_FAIL); /* exact mask: no leaked wake bit */
    pf_path(1,PF_SIGNALLED_NAME);pf_branch(0x6100,PF_MAKE_MARKER);pf_nonzero();
    pf_imm(7,999);pf_mark(PF_ACK_LOOP);
    pf_path(1,PF_ACK_NAME);pf_imm(2,1005);pf_ma(5,6);pf_call(30);pf_test(0);pf_branch(0x6600,PF_ACK_SEEN);
    pf_mark(PF_ACK_DELAY);pf_imm(1,1);pf_ma(5,6);pf_call(198);pf_w(0x51cf);pf_fix(PF_ACK_LOOP,0,1);
    pf_branch(0x6000,PF_FAIL);
    pf_mark(PF_ACK_SEEN);pf_md(0,1);pf_call(36);pf_nonzero();pf_imm(6,0);pf_branch(0x6000,PF_CLEANUP);
    pf_mark(PF_FAIL);pf_imm(6,20);
    pf_mark(PF_CLEANUP);
    pf_geta(PF_INSTALLED,0);pf_ad(0,0);pf_test(0);pf_branch(0x6700,PF_NO_HOOK);
    pf_geta(PF_LAYER,0);pf_geta(PF_OLD_HOOK,1);pf_geta(PF_LAY_BASE,6);pf_call(198);
    pf_mark(PF_NO_HOOK);pf_geta(PF_WINDOW,0);pf_ad(0,0);pf_test(0);pf_branch(0x6700,PF_NO_WINDOW);
    pf_geta(PF_INT_BASE,6);pf_call(72);pf_mark(PF_NO_WINDOW);
    pf_close_library(PF_LAY_BASE,PF_NO_LAYERS);pf_close_library(PF_GFX_BASE,PF_NO_GRAPHICS);
    pf_close_library(PF_INT_BASE,PF_NO_INTUITION);
    pf_ad(5,0);pf_test(0);pf_branch(0x6700,PF_NO_DOS);pf_ma(5,1);pf_exec();pf_call(414);
    pf_mark(PF_NO_DOS);pf_md(6,0);pf_w(0x4cdf);pf_w(0x7cfc);pf_w(0x4e75);

    /* D1=path. Open/Close each marker and check Close, preserving D4-D7. */
    pf_mark(PF_MAKE_MARKER);pf_imm(2,1006);pf_ma(5,6);pf_call(30);pf_test(0);pf_branch(0x6700,PF_MARKER_BAD);
    pf_md(0,1);pf_call(36);pf_w(0x4e75);pf_mark(PF_MARKER_BAD);pf_imm(0,0);pf_w(0x4e75);

    /* Original generic solid-fill Hook; only private-stack stores. */
    pf_align();pf_mark(PF_HOOK_ENTRY);pf_w(0x48e7);pf_w(0xfffe); /* D0-D7/A0-A6 */
    pf_ma(0,6);pf_ma(1,5);pf_w(0x4fef);pf_w((uint16_t)-128);
    pf_ma(2,3);pf_ma(7,4);pf_w(0x7e18);pf_mark(PF_COPY_LOOP);
    pf_w(0x28db);pf_w(0x51cf);pf_fix(PF_COPY_LOOP,0,1); /* 25 LONGs */
    pf_ma(7,4);pf_w(0x4294); /* local RP.Layer=NULL */
    pf_loada(6,12,3);pf_w(0x3013); /* h_SubEntry points to public fixture pen WORD */
    pf_loada(6,16,2);pf_loada(2,4,6);pf_ma(4,1);pf_call(342);
    pf_w(0x7000);pf_call(354); /* JAM1, with native setter side effects */
    pf_w(0x50ec);pf_w(24); /* local RP.Mask=0xff */
    for(i=0;i<4;++i){pf_w((uint16_t)(0x302d|(i<<9)));pf_w((uint16_t)(4+2*i));pf_w((uint16_t)(0x48c0|i));}
    pf_call(306);
    pf_load(2,8,0);pf_branch(0x6700,PF_NO_SIGNAL);pf_da(0,1);pf_loada(2,0,6);pf_imm(0,PF_CTRL_D);pf_call(324);
    pf_mark(PF_NO_SIGNAL);pf_w(0x4fef);pf_w(128);pf_w(0x4cdf);pf_w(0x7fff);pf_w(0x4e75);

    pf_zero(PF_HOOK_HEADER,8);pf_fix(PF_HOOK_ENTRY,0,0);pf_fix(PF_PEN,0,0);pf_fix(PF_HOOK_DATA,0,0);
    pf_zero(PF_HOOK_DATA,12);pf_zero(PF_PEN,4);pf.code[pf.label[PF_PEN]+1]=2;
    pf_align();pf_mark(PF_NEW_WINDOW);
    pf_w(40);pf_w(40);pf_w(128);pf_w(96);pf_w(0x0001); /* Detail0,Block1 */
    pf_l(0);pf_l(0); /* no IDCMP, SMART_REFRESH */
    pf_l(0);pf_l(0);pf_fix(PF_WINDOW_TITLE,0,0);pf_l(0);pf_l(0);
    pf_w(128);pf_w(96);pf_w(128);pf_w(96);pf_w(1); /* WBENCHSCREEN */
    pf_zero(PF_WINDOW,4);pf_zero(PF_LAYER,4);pf_zero(PF_OLD_HOOK,4);pf_zero(PF_INSTALLED,4);
    pf_zero(PF_DOS_BASE,4);pf_zero(PF_INT_BASE,4);pf_zero(PF_GFX_BASE,4);pf_zero(PF_LAY_BASE,4);
    pf_text(PF_DOS_NAME,"dos.library");pf_text(PF_INT_NAME,"intuition.library");
    pf_text(PF_GFX_NAME,"graphics.library");pf_text(PF_LAY_NAME,"layers.library");
    pf_text(PF_WINDOW_TITLE,PF_TITLE);pf_text(PF_READY_NAME,PF_READY);
    pf_text(PF_SIGNALLED_NAME,PF_SIGNALLED);pf_text(PF_ACK_NAME,PF_ACK);
    for(i=0;i<pf.fixes;++i)
    {
        const struct PFFix*f=&pf.fix[i];int64_t v;
        if(f->label<0 || f->label>=PF_LABELS || pf.label[f->label]<0)return 0;
        v=(int64_t)pf.label[f->label]+f->add;
        if(f->relative){v-=f->at;if(v<INT16_MIN || v>INT16_MAX)return 0;pf.code[f->at]=(uint8_t)((uint64_t)v>>8);pf.code[f->at+1]=(uint8_t)v;}
        else {if(v<0 || v>UINT32_MAX)return 0;pf_put32(pf.code+f->at,(uint32_t)v);}
    }
    pf_align();words=pf.used/4;
    if(pf.bad || !pf.relocs || capacity<pf.used+(13+pf.relocs)*4)return 0;
#define PF_OUT(v) do{pf_put32(out+at,(uint32_t)(v));at+=4;}while(0)
    PF_OUT(0x3f3);PF_OUT(0);PF_OUT(1);PF_OUT(0);PF_OUT(0);PF_OUT(words);
    PF_OUT(0x3e9);PF_OUT(words);memcpy(out+at,pf.code,pf.used);at+=pf.used;
    PF_OUT(0x3ec);PF_OUT(pf.relocs);PF_OUT(0);
    for(i=0;i<pf.relocs;++i)PF_OUT(pf.reloc[i]);
    PF_OUT(0);PF_OUT(0x3f2);
#undef PF_OUT
    result=(struct PaintFixtureLayout){pf.used,(uint32_t)pf.label[PF_HOOK_ENTRY],
        (uint32_t)pf.label[PF_HOOK_HEADER],(uint32_t)pf.label[PF_HOOK_DATA],
        (uint32_t)pf.label[PF_PEN],(uint32_t)pf.label[PF_NEW_WINDOW]};
    *layout=result;return at;
}

#ifdef PAINT_FIXTURE_EMIT_ONLY
#ifndef PAINT_FIXTURE_NO_MAIN
int main(int argc,char**argv)
{
    uint8_t bytes[16384];struct PaintFixtureLayout layout;FILE*file;size_t n;
    if(argc!=3){fprintf(stderr,"usage: paint-emitter image.hunk metadata.json\n");return 2;}
    n=pf_build_hunk(bytes,sizeof(bytes),&layout);if(!n)return 2;
    file=fopen(argv[1],"wb");if(!file)return 2;
    {int good=fwrite(bytes,1,n,file)==n;int closed=fclose(file)==0;if(!good || !closed)return 2;}
    file=fopen(argv[2],"w");if(!file)return 2;
    {int good=fprintf(file,"{\"bytes\":%lu,\"code_offset\":32,\"code_bytes\":%lu,\"hook_entry\":%lu,\"hook_header\":%lu,\"hook_data\":%lu,\"pen\":%lu,\"new_window\":%lu}\n",
        (unsigned long)n,(unsigned long)layout.code_bytes,(unsigned long)layout.hook_entry,
        (unsigned long)layout.hook_header,(unsigned long)layout.hook_data,(unsigned long)layout.pen,
        (unsigned long)layout.new_window)>0;int closed=fclose(file)==0;return good && closed?0:2;}
}
#endif
#else

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/var.h>
#include <graphics/gfxbase.h>
#include <graphics/layers.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <utility/hooks.h>
#include <hidd/gfx.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/kernel.h>
#include <proto/kaffeine.h>
#include <proto/utility.h>

#if defined(__AROSEXEC_SMP__)
#error This foreign paint fixture requires uniprocessor Exec
#endif

APTR KernelBase;
struct Library *KaffeineBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
static unsigned pf_checks,pf_failures;
static UBYTE pf_image[16384],pf_old_backend[4096];
static ULONG pf_old_length,pf_old_flags;
static BOOL pf_old_present;
struct PFObserver {
    struct Task *controller,*driver;
    IPTR observer_identity;
    ULONG wake;
    volatile ULONG abort,done;
    BOOL ready,foreign,blocked,managed,called,pixels,ack;
    LONG before,reference,painted;
};
static struct PFObserver pf_job;
static BOOL pf_check(BOOL yes,const char*what)
{++pf_checks;if(!yes)++pf_failures;printf("%s %s\n",yes?"ok  ":"FAIL",what);fflush(stdout);return yes;}
static BOOL pf_exists(const char*path)
{BPTR lock=Lock(path,ACCESS_READ);if(!lock)return FALSE;UnLock(lock);return TRUE;}
static BOOL pf_clean(const char*path)
{
    BPTR lock;
    if(!DeleteFile(path) && IoErr()!=ERROR_OBJECT_NOT_FOUND)return FALSE;
    lock=Lock(path,ACCESS_READ);if(lock){UnLock(lock);return FALSE;}
    return IoErr()==ERROR_OBJECT_NOT_FOUND;
}
static BOOL pf_write(const char*path,const void*bytes,LONG n)
{
    BPTR file=Open(path,MODE_NEWFILE);LONG written;BOOL closed;
    if(!file)return FALSE;
    written=Write(file,bytes,n);closed=Close(file);return written==n && closed;
}
static BOOL pf_save_backend(void)
{
    const struct LocalVar*v=FindVar("KAFFEINE_CPU",LV_VAR);
    pf_old_present=v!=NULL;pf_old_length=pf_old_flags=0;
    if(!v)return TRUE;
    if(v->lv_Len>sizeof(pf_old_backend))return FALSE;
    pf_old_length=v->lv_Len;pf_old_flags=v->lv_Flags;
    if(v->lv_Len)memcpy(pf_old_backend,v->lv_Value,v->lv_Len);
    return TRUE;
}
static BOOL pf_restore_backend(void)
{
    const struct LocalVar*v;
    if(pf_old_present)
    {if(!SetVar("KAFFEINE_CPU",(CONST_STRPTR)pf_old_backend,pf_old_length,GVF_LOCAL_ONLY|pf_old_flags))return FALSE;}
    else if(!DeleteVar("KAFFEINE_CPU",GVF_LOCAL_ONLY) && FindVar("KAFFEINE_CPU",LV_VAR))return FALSE;
    v=FindVar("KAFFEINE_CPU",LV_VAR);
    return (!!v==!!pf_old_present) && (!v || (v->lv_Len==pf_old_length && v->lv_Flags==pf_old_flags &&
           !memcmp(v->lv_Value,pf_old_backend,pf_old_length)));
}
static BOOL pf_aborted(struct PFObserver*j)
{return __atomic_load_n(&j->abort,__ATOMIC_ACQUIRE)!=0;}

/* The reserved unique native driver is created by this controller's
 * RunHunk(nonzero stack). It must be blocked specifically on CTRL_D before
 * the observer fires; this is inspection, never a native broadcast. */
static BOOL pf_driver_waiting(struct PFObserver*j)
{
    struct Task*driver;BOOL waiting;
    Forbid();driver=FindTask("kaffeine");
    waiting=driver && driver!=FindTask(NULL) && driver->tc_State==TS_WAIT &&
            (driver->tc_SigWait&PF_CTRL_D)!=0;
    if(waiting)j->driver=driver;
    Permit();return waiting;
}

static LONG pf_observer_entry(void)
{
    struct Process*me=(struct Process*)FindTask(NULL);
    struct PFObserver*j=me->pr_Task.tc_UserData;
    struct Screen*screen=NULL;
    unsigned tick;
    j->observer_identity=(IPTR)FindTask(NULL);
    for(tick=0;tick<1000 && !pf_aborted(j);++tick)
    {if(pf_exists(PF_READY)){j->ready=TRUE;break;}Delay(1);}
    if(!j->ready || pf_aborted(j))goto finished;
    for(tick=0;tick<250 && !pf_aborted(j);++tick)
    {if(pf_driver_waiting(j)){j->blocked=TRUE;break;}Delay(1);}
    if(!j->blocked || pf_aborted(j))goto finished;
    screen=LockPubScreen(NULL);if(!screen)goto finished;
    for(tick=0;tick<250 && !pf_aborted(j);++tick)
    {
        struct Window*w;struct Layer*layer=NULL;struct RastPort rp;
        struct Hook*hook=NULL;struct BackFillMessage message;
        ULONG key=LockIBase(0);LONG x=0,y=0;unsigned windows=0;
        for(w=screen->FirstWindow;w && ++windows<=128;w=w->NextWindow)
            if(w->Title && !strncmp((const char*)w->Title,PF_TITLE,sizeof(PF_TITLE)) && w->Width==128 && w->Height==96)break;
        if(w && w->WScreen==screen && w->WLayer && w->WLayer->rp &&
           AttemptSemaphore(&w->WLayer->Lock))
        {
            struct ClipRect*clip;unsigned clips=0;BOOL visible=FALSE;
            layer=w->WLayer;rp=*layer->rp;hook=layer->BackFill;
            /* Stay inside an actually visible piece of this Window. A
             * LayerNULL RP must never overwrite an occluding Window. */
            for(clip=layer->ClipRect;clip && ++clips<=128;clip=clip->Next)
            {
                if(clip->lobs)continue;
                x=(LONG)layer->bounds.MinX+16;y=(LONG)layer->bounds.MinY+24;
                if(x<clip->bounds.MinX)x=clip->bounds.MinX;
                if(y<clip->bounds.MinY)y=clip->bounds.MinY;
                if(x+8<=clip->bounds.MaxX && y+1<=clip->bounds.MaxY)
                {visible=TRUE;break;}
            }
            if(!visible || rp.BitMap!=screen->RastPort.BitMap)hook=NULL;
            if(x<layer->bounds.MinX || y<layer->bounds.MinY || x+8>layer->bounds.MaxX || y+2>layer->bounds.MaxY ||
               x<0 || y<0 || x+8>=screen->Width || y+2>=screen->Height)hook=NULL;
        }
        UnlockIBase(key); /* Never block on Layer while IBase is held. */
        if(!layer){Delay(1);continue;}
        /* No Window access after releasing IBase: Layer hold protects the
         * copied objects; the guest retains its Window through our ack. */
        j->foreign=FindTask(NULL)==&me->pr_Task && FindTask(NULL)!=j->controller &&
                   j->driver && FindTask(NULL)!=j->driver;
        j->managed=rp.BitMap && IS_HIDD_BM(rp.BitMap) && !rp.AreaPtrn;
        if(hook && hook!=(struct Hook*)1 && j->managed && j->foreign)
        {
            struct RastPort reference=rp;
            rp.Layer=NULL;rp.Mask=0xff;reference.Layer=NULL;reference.Mask=0xff;
            SetDrMd(&reference,JAM1);SetAPen(&reference,0);
            RectFill(&reference,x,y,x+1,y+1);RectFill(&reference,x+8,y,x+8,y);
            WaitBlit();j->before=ReadPixel(&reference,x,y);
            SetAPen(&reference,2);RectFill(&reference,x+8,y,x+8,y);
            WaitBlit();j->reference=ReadPixel(&reference,x+8,y);
            memset(&message,0,sizeof(message));message.Layer=layer;
            message.Bounds.MinX=(WORD)x;message.Bounds.MinY=(WORD)y;
            message.Bounds.MaxX=(WORD)(x+1);message.Bounds.MaxY=(WORD)(y+1);
            j->called=TRUE;CallHookPkt(hook,&rp,&message);WaitBlit();
            j->painted=ReadPixel(&reference,x,y);
            j->pixels=j->before!=-1 && j->reference!=-1 && j->painted==j->reference && j->painted!=j->before;
        }
        ReleaseSemaphore(&layer->Lock);break;
    }
    UnlockPubScreen(NULL,screen);screen=NULL;
    /* Ack follows EVERY pixel check and native-lock release. No Window or
     * Hook pointer is used after this publication. */
    j->ack=pf_write(PF_ACK,j->pixels?"OK\n":"FAIL\n",j->pixels?3:5);
finished:
    if(screen)UnlockPubScreen(NULL,screen);
    __atomic_store_n(&j->done,1,__ATOMIC_RELEASE);
    Signal(j->controller,j->wake);return 0;
}

/* UP-only join: done is not sufficient to unload an NP_Entry. DOS removes
 * the unique Process only after our function has returned. All finite polls
 * can fail, but an active observer is quarantined here until it retires;
 * the external target deadline handles a kernel-level graphics hang. */
static void pf_join_observer(struct PFObserver*j)
{
    for(;;)
    {
        BOOL alive=TRUE;
        if(__atomic_load_n(&j->done,__ATOMIC_ACQUIRE))
        {Forbid();alive=(IPTR)FindTask(PF_OBSERVER)==j->observer_identity;Permit();}
        if(!alive)break;
        Delay(1);
    }
}
int main(int argc,char**argv)
{
    const char*backend=argc>1?argv[1]:"moira";
    struct PaintFixtureLayout layout;BPTR segment=BNULL;struct Process*observer=NULL;
    BOOL backend_changed=FALSE,paths_prepared=FALSE;BYTE signal=-1;LONG result=-1;size_t bytes;
    if(!pf_check(argc<=2 && (!strcmp(backend,"moira") || !strcmp(backend,"jit")),"select one backend"))goto done;
    KernelBase=OpenResource("kernel.resource");
    if(!pf_check(KernelBase && KrnGetCPUCount()==1,"require UP SDK and exactly one active CPU"))goto done;
    if(!pf_check(!FindTask("kaffeine") && !FindTask(PF_OBSERVER),"fixture native driver and observer names are unoccupied"))goto done;
    KaffeineBase=OpenLibrary("kaffeine.library",0);
    IntuitionBase=(struct IntuitionBase*)OpenLibrary("intuition.library",0);
    GfxBase=(struct GfxBase*)OpenLibrary("graphics.library",0);
    if(!pf_check(KaffeineBase && IntuitionBase && GfxBase,"pin only the native fixture libraries"))goto done;
    if(!pf_check(pf_save_backend(),"snapshot exact local backend bytes and flags"))goto done;
    backend_changed=TRUE;
    if(!pf_check(SetVar("KAFFEINE_CPU",backend,strlen(backend),GVF_LOCAL_ONLY),"select the local guest backend"))goto done;
    if(!pf_check(pf_clean(PF_READY) && pf_clean(PF_SIGNALLED) && pf_clean(PF_ACK) && pf_clean(PF_HUNK),"clear only reserved fixture paths and verify absence"))goto done;
    paths_prepared=TRUE;
    bytes=pf_build_hunk(pf_image,sizeof(pf_image),&layout);
    if(!pf_check(bytes && pf_write(PF_HUNK,pf_image,(LONG)bytes),"write the original single-hunk relocated68000 program"))goto done;
    segment=LoadSeg(PF_HUNK);
    if(!pf_check(segment!=BNULL,"native DOS loads the original68k HUNK"))goto done;
    {
        IPTR hunk=0,relocs=0;struct TagItem tags[]={{GSLI_68KHUNK,(IPTR)&hunk},{GSLI_68KRelocs,(IPTR)&relocs},{TAG_DONE,0}};
        if(!pf_check(GetSegListInfo(segment,tags)==2 && hunk && relocs,"DOS publishes68k identity and relocation metadata"))goto done;
    }
    signal=AllocSignal(-1);if(!pf_check(signal>=0,"reserve one native observer-completion bit"))goto done;
    memset(&pf_job,0,sizeof(pf_job));pf_job.controller=FindTask(NULL);pf_job.wake=1UL<<signal;
    {
        struct TagItem tags[]={{NP_Entry,(IPTR)pf_observer_entry},{NP_UserData,(IPTR)&pf_job},
            {NP_Name,(IPTR)PF_OBSERVER},{NP_StackSize,65536},{NP_CopyVars,FALSE},{NP_Cli,FALSE},
            {NP_Input,(IPTR)Input()},{NP_CloseInput,FALSE},{NP_Output,(IPTR)Output()},{NP_CloseOutput,FALSE},
            {NP_Synchronous,FALSE},{TAG_DONE,0}};
        observer=CreateNewProc(tags);
    }
    if(!pf_check(observer!=NULL,"start the native observer before RunHunk"))goto done;
    result=RunHunk(segment,65536,"",0);
    __atomic_store_n(&pf_job.abort,1,__ATOMIC_RELEASE);pf_join_observer(&pf_job);
    pf_check(result==0,"classic root received exactlyCTRL_D and completed bounded ack cleanup");
    pf_check(pf_job.ready && pf_job.blocked,"observer saw ready and the root natively blocked onCTRL_D");
    pf_check(pf_job.foreign,"Hook invocation came from a different native Task");
    pf_check(pf_job.managed,"observer used managed HIDD bitmap and no area pattern");
    pf_check(pf_job.called && pf_job.pixels,"installed Hook painted the native reference value and changed the seeded pixel");
    pf_check(pf_job.ack && pf_exists(PF_SIGNALLED) && pf_exists(PF_ACK),"guest wake and observer acknowledgment markers were published");
    printf("paint pixels: seed=%ld reference=%ld painted=%ld\n",(long)pf_job.before,(long)pf_job.reference,(long)pf_job.painted);
done:
    if(observer)
    {__atomic_store_n(&pf_job.abort,1,__ATOMIC_RELEASE);pf_join_observer(&pf_job);}
    if(signal>=0){SetSignal(0,1UL<<signal);FreeSignal(signal);}
    if(segment)UnLoadSeg(segment);
    if(paths_prepared)
    {
        BOOL clean=pf_clean(PF_READY);
        clean=pf_clean(PF_SIGNALLED) && clean;clean=pf_clean(PF_ACK) && clean;
        clean=pf_clean(PF_HUNK) && clean;
        pf_check(clean,"remove only the generated fixture paths after observer retirement");
    }
    if(backend_changed)pf_check(pf_restore_backend(),"restore exact local backend state");
    if(GfxBase)CloseLibrary((struct Library*)GfxBase);
    if(IntuitionBase)CloseLibrary((struct Library*)IntuitionBase);
    if(KaffeineBase)CloseLibrary(KaffeineBase);
    printf("paint_fixture_native: %u checks, %u failed\n",pf_checks,pf_failures);fflush(stdout);
    return pf_failures?20:0;
}
#endif
