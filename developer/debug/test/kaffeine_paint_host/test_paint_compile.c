/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Author: Michele Dipace (@kaffeine1)
 * Original synthetic 68000 programs: no application input or signatures. */
#include "kaffeine_paint_compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA 131072u
#define HOOK 0x1000u
#define CODE 0x2000u
#define DATA 0x8000u
#define EXEC 0xc000u
#define GFX 0xd000u
#define OWNER 0x9000u
#define CHECK(x) do { ++checks;if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);++failures;} } while(0)
static unsigned checks,failures;
struct Fixture {
    unsigned char arena[ARENA];unsigned pc,entry,code_end,reads;
    struct KaffeinePaintSource source;
    struct KaffeinePaintPlan plan;
    struct KaffeinePaintInputs input;
    struct KaffeinePaintOutput output;
    struct KaffeinePaintReport report;
    void *compile_work,*eval_work;
};
static void put16(struct Fixture*f,unsigned a,unsigned n)
{f->arena[a]=(unsigned char)(n>>8);f->arena[a+1]=(unsigned char)n;}
static void put32(struct Fixture*f,unsigned a,unsigned n)
{put16(f,a,n>>16);put16(f,a+2,n);}
static unsigned get(struct Fixture*f,unsigned a,unsigned n)
{unsigned i,v=0;for(i=0;i<n;++i)v=(v<<8)|f->arena[a+i];return v;}
static int reader(void*opaque,uint32_t a,void*out,uint32_t n)
{struct Fixture*f=(struct Fixture*)opaque;++f->reads;if(a>=ARENA || n>ARENA-a)return 0;memcpy(out,f->arena+a,n);return 1;}
static int ordinary(void*opaque,uint32_t a,uint32_t n)
{(void)opaque;return a>=DATA && a<DATA+4096 && n<=DATA+4096-a;}
static void w(struct Fixture*f,unsigned n){put16(f,f->pc,n);f->pc+=2;}
static void l(struct Fixture*f,unsigned n){w(f,n>>16);w(f,n);}
static void move(struct Fixture*f,unsigned size,unsigned sm,unsigned sr,unsigned dm,unsigned dr)
{w(f,(size==1?0x1000:size==2?0x3000:0x2000)|(dr<<9)|(dm<<6)|(sm<<3)|sr);}
static void movea(struct Fixture*f,unsigned src,unsigned dest){move(f,4,1,src,1,dest);}
static void constant(struct Fixture*f,unsigned v,unsigned dm,unsigned dr)
{move(f,4,7,4,dm,dr);l(f,v);}
static void coord(struct Fixture*f,unsigned i)
{move(f,2,5,5,0,i);w(f,4+2*i);}
static void coords(struct Fixture*f){unsigned i;for(i=0;i<4;++i)coord(f,i);}
static void call(struct Fixture*f,unsigned lvo){w(f,0x4eae);w(f,(unsigned)(-(int)lvo));}
static void init(struct Fixture*f)
{
    memset(f,0,sizeof(*f));f->pc=CODE;f->entry=CODE;
    f->source=(struct KaffeinePaintSource){f,reader,ordinary,ARENA,HOOK,EXEC,GFX};
    f->compile_work=malloc(Kaffeine_PaintCompileWorkspaceSize());
    f->eval_work=malloc(Kaffeine_PaintEvaluateWorkspaceSize());
    if(!f->compile_work || !f->eval_work)abort();
    put32(f,4,EXEC);put32(f,HOOK+8,CODE);put32(f,HOOK+16,DATA);
    put32(f,DATA,0x12345607);put32(f,DATA+4,OWNER);put32(f,DATA+8,1u<<13);
}
static void dispose(struct Fixture*f){free(f->compile_work);free(f->eval_work);}
static void prologue(struct Fixture*f,unsigned src,unsigned dst,unsigned counter,int longs)
{
    unsigned loop,db;
    w(f,0x48e7);w(f,0xfffe); /* save D0..D7/A0..A6 */
    movea(f,1,5);           /* BF pointer */
    w(f,0x4fef);w(f,(unsigned)-100); /* private classic RP frame */
    movea(f,2,src);movea(f,7,dst);
    w(f,0x7000|(counter<<9)|(longs?24:49));
    loop=f->pc;move(f,longs?4:2,3,src,3,dst);
    db=f->pc;w(f,0x51c8|counter);w(f,loop-(db+2));
    movea(f,7,4);w(f,0x4294); /* clear clone Layer */
    constant(f,GFX,1,6);movea(f,4,1);
}
static void epilogue(struct Fixture*f)
{w(f,0x4fef);w(f,100);w(f,0x4cdf);w(f,0x7fff);w(f,0x4e75);f->code_end=f->pc;}
static void paint(struct Fixture*f,int dynamic,int twice)
{
    move(f,4,7,1,0,0);l(f,DATA); /* dynamic word/long pen, low byte used */
    if(!dynamic){ /* overwrite with an arbitrary immediate pen */ w(f,0x7003); }
    call(f,342);w(f,0x7001);call(f,354);
    w(f,0x50ec);w(f,24); /* ST clone.Mask */
    coords(f);call(f,306);
    if(twice){w(f,0x7005);call(f,342);coords(f);call(f,306);}
}
static void signal_tail(struct Fixture*f)
{
    unsigned branch,done;
    move(f,4,7,1,0,0);l(f,DATA+4);
    branch=f->pc;w(f,0x6700);w(f,0); /* BEQ.W no signal */
    move(f,4,0,0,1,1);constant(f,1u<<13,0,0);constant(f,EXEC,1,6);call(f,324);
    done=f->pc;put16(f,branch+2,done-(branch+2));
}
static enum KaffeinePaintStatus compile(struct Fixture*f)
{return Kaffeine_PaintCompile(&f->source,f->compile_work,Kaffeine_PaintCompileWorkspaceSize(),&f->plan,&f->report);}
static void snapshot(struct Fixture*f)
{
    unsigned i;memset(&f->input,0,sizeof(f->input));
    for(i=0;i<f->plan.dependencies;++i)f->input.dependencies[i]=get(f,f->plan.dependency[i].address,f->plan.dependency[i].width);
    for(i=0;i<f->plan.cells;++i)f->input.cells[i]=get(f,f->plan.cell[i].address,f->plan.cell[i].width);
    for(i=0;i<100;++i)f->input.rp[i]=(unsigned char)(3*i+1);
    f->input.bounds[0]=-100;f->input.bounds[1]=-70;f->input.bounds[2]=250;f->input.bounds[3]=400;
    f->input.owner_task=OWNER;f->input.owner_cookie=0x1234567887654321ULL;f->input.allowed_signals=1u<<13;
}
static enum KaffeinePaintStatus evaluate(struct Fixture*f)
{return Kaffeine_PaintEvaluate(&f->plan,&f->input,f->eval_work,Kaffeine_PaintEvaluateWorkspaceSize(),&f->output);}
static unsigned cell_at(struct Fixture*f,unsigned a)
{unsigned i;for(i=0;i<f->plan.cells;++i)if(f->plan.cell[i].address==a)return i;return KPAINT_MAX_CELLS;}
static int unchanged_output(struct Fixture*f,enum KaffeinePaintStatus wanted)
{
    struct KaffeinePaintOutput saved;
    memset(&f->output,0xa5,sizeof(f->output));saved=f->output;
    return evaluate(f)==wanted && !memcmp(&saved,&f->output,sizeof(saved));
}
static void test_positive(void)
{
    struct Fixture f;unsigned src,dst,counter,i,pen,owner;
    for(src=3;src<=6;++src)for(dst=3;dst<=6;++dst)if(src!=dst && src!=5 && dst!=5)
        for(counter=0;counter<8;counter+=3)
    {
        init(&f);prologue(&f,src,dst,counter,(counter&1)==0);paint(&f,1,1);signal_tail(&f);epilogue(&f);
        if(compile(&f)!=KPAINT_OK){fprintf(stderr,"compile status%d pc%x step%u src%u dst%u d%u\n",f.report.status,f.report.pc,f.report.steps,src,dst,counter);CHECK(0);dispose(&f);continue;}
        CHECK(f.report.steps<400);CHECK(f.reads<1000);CHECK(f.plan.paths==2);CHECK(f.plan.signal_mask==(1u<<13));
        pen=cell_at(&f,DATA);owner=cell_at(&f,DATA+4);
        CHECK(pen<KPAINT_MAX_CELLS && f.plan.cell[pen].used_mask==255 && (f.plan.cell[pen].role&KPAINT_CELL_BYTE));
        CHECK(owner<KPAINT_MAX_CELLS && (f.plan.cell[owner].role&KPAINT_CELL_OWNER_CAP));
        snapshot(&f);CHECK(evaluate(&f)==KPAINT_OK);CHECK(f.output.count==9);
        CHECK(f.output.commands[0].kind==KPAINT_CLONE);
        CHECK(f.output.commands[1].kind==KPAINT_APEN && f.output.commands[1].value[0]==7);
        CHECK(f.output.commands[2].kind==KPAINT_DRAWMODE && f.output.commands[2].value[0]==1);
        CHECK(f.output.commands[4].kind==KPAINT_RECTFILL);
        CHECK(f.output.commands[5].kind==KPAINT_APEN && f.output.commands[5].value[0]==5);
        CHECK(f.output.commands[8].kind==KPAINT_DEFER_SIGNAL && f.output.commands[8].value[0]==OWNER && f.output.commands[8].owner_cookie==f.input.owner_cookie);
        for(i=0;i<4;++i)CHECK((int16_t)f.output.commands[4].value[i]==f.input.bounds[i]);
        f.input.cells[owner]=0;CHECK(evaluate(&f)==KPAINT_OK && f.output.count==8);
        f.input.cells[owner]=OWNER+4;CHECK(unchanged_output(&f,KPAINT_SIGNAL));
        f.input.cells[owner]=OWNER;f.input.owner_cookie=0;CHECK(unchanged_output(&f,KPAINT_SIGNAL));
        f.input.owner_cookie=1;f.input.allowed_signals=0;CHECK(unchanged_output(&f,KPAINT_SIGNAL));
        snapshot(&f);f.input.cells[pen]=0xffbc0033;CHECK(evaluate(&f)==KPAINT_OK && f.output.commands[1].value[0]==0x33);
        for(i=0;i<f.plan.dependencies;++i){snapshot(&f);f.input.dependencies[i]^=1;CHECK(unchanged_output(&f,KPAINT_CHANGED));}
        dispose(&f);
    }
}
static void test_trampoline_pointer(void)
{
    struct Fixture f;unsigned entry,ptr;
    init(&f);entry=f.pc;
    move(&f,4,5,0,1,6);w(&f,12);w(&f,0x4e96);w(&f,0x4e75);
    put32(&f,HOOK+12,f.pc);prologue(&f,3,4,7,1);
    /* Chase a scalar pointer, then a displacement: both addresses become
     * exact immutable dependencies, while the pointed-to pen remains live. */
    move(&f,4,5,0,1,3);w(&f,16);
    move(&f,4,5,3,1,3);w(&f,12);
    move(&f,4,5,3,0,0);w(&f,8);
    call(&f,342);coords(&f);call(&f,306);epilogue(&f);
    put32(&f,DATA+12,DATA+100);put32(&f,DATA+108,0x432109ab);put32(&f,HOOK+8,entry);
    CHECK(compile(&f)==KPAINT_OK);snapshot(&f);CHECK(evaluate(&f)==KPAINT_OK);
    CHECK(f.output.commands[1].kind==KPAINT_APEN && f.output.commands[1].value[0]==0xab);
    ptr=cell_at(&f,DATA+12);CHECK(ptr<KPAINT_MAX_CELLS && (f.plan.cell[ptr].role&KPAINT_CELL_IMMUTABLE));
    dispose(&f);
}
static void test_refusals(void)
{
    struct Fixture f;struct KaffeinePaintPlan saved;unsigned at,i;
    /* Every unsupported scalar/pointer byte in a stack RP is rejected. */
    for(i=4;i<100;++i)if(i!=24)
    {
        init(&f);prologue(&f,3,4,7,1);move(&f,1,7,4,5,4);w(&f,0x55);w(&f,i);
        coords(&f);call(&f,306);epilogue(&f);
        CHECK(compile(&f)!=KPAINT_OK);dispose(&f);
    }
    /* No original RP/global/code writes; no partial-copy fabrication. */
    init(&f);w(&f,0x4292);w(&f,0x4e75);CHECK(compile(&f)==KPAINT_STORE);dispose(&f);
    init(&f);move(&f,4,7,4,7,1);l(&f,0);l(&f,DATA);w(&f,0x4e75);CHECK(compile(&f)==KPAINT_STORE);dispose(&f);
    init(&f);prologue(&f,3,4,7,1);constant(&f,CODE,1,3);w(&f,0x4293);epilogue(&f);CHECK(compile(&f)==KPAINT_STORE);dispose(&f);
    init(&f);prologue(&f,3,4,7,1);w(&f,0x4eae);w(&f,(unsigned)-348);epilogue(&f);CHECK(compile(&f)==KPAINT_UNSUPPORTED);dispose(&f);
    init(&f);prologue(&f,3,4,7,1);w(&f,0x4eee);w(&f,(unsigned)-342);epilogue(&f);CHECK(compile(&f)==KPAINT_UNSUPPORTED);dispose(&f);
    init(&f);w(&f,0x66fe);CHECK(compile(&f)!=KPAINT_OK);dispose(&f);
    init(&f);w(&f,0x60fe);CHECK(compile(&f)==KPAINT_LIMIT);CHECK(f.report.steps==KPAINT_MAX_STEPS+1);dispose(&f);
    init(&f);w(&f,0x6c00);w(&f,2);w(&f,0x4e75);CHECK(compile(&f)==KPAINT_UNSUPPORTED);dispose(&f);
    init(&f);w(&f,0x51c8);w(&f,0xfffe);w(&f,0x4e75);CHECK(compile(&f)==KPAINT_DATAFLOW);dispose(&f);
    init(&f);w(&f,0x4fef);w(&f,4);w(&f,0x4e75);CHECK(compile(&f)==KPAINT_STACK);dispose(&f);
    init(&f);put32(&f,HOOK+8,ARENA-2);put16(&f,ARENA-2,0x4eb9);CHECK(compile(&f)==KPAINT_BOUNDS);dispose(&f);
    init(&f);put32(&f,HOOK+8,CODE+1);CHECK(compile(&f)==KPAINT_BOUNDS);dispose(&f);
    init(&f);prologue(&f,3,4,7,1);w(&f,0x7001);call(&f,342);coords(&f);call(&f,306);
    /* A direct unmodelled write after publication invalidates the clone. */
    move(&f,1,7,4,5,4);w(&f,3);w(&f,26);coords(&f);call(&f,306);epilogue(&f);CHECK(compile(&f)==KPAINT_BAD_CLONE);dispose(&f);
    init(&f);prologue(&f,3,4,7,1);for(i=0;i<34;++i){w(&f,0x7001);call(&f,342);}epilogue(&f);CHECK(compile(&f)==KPAINT_LIMIT);dispose(&f);
    init(&f);w(&f,0x4e75);memset(&f.plan,0x96,sizeof(f.plan));saved=f.plan;
    CHECK(Kaffeine_PaintCompile(&f.source,f.compile_work,0,&f.plan,&f.report)==KPAINT_WORKSPACE);
    CHECK(!memcmp(&saved,&f.plan,sizeof(saved)));
    at=f.pc;f.pc=CODE;w(&f,0xffff);f.pc=at;CHECK(compile(&f)==KPAINT_UNSUPPORTED);CHECK(!memcmp(&saved,&f.plan,sizeof(saved)));dispose(&f);
}
static void test_bounds_and_order(void)
{
    struct Fixture f;unsigned i;
    init(&f);prologue(&f,3,4,7,1);paint(&f,1,0);epilogue(&f);CHECK(compile(&f)==KPAINT_OK);snapshot(&f);
    for(i=0;i<1000;++i)
    {
        f.input.bounds[0]=(int16_t)(i*71u);f.input.bounds[1]=(int16_t)(i*113u);
        f.input.bounds[2]=(int16_t)((int)f.input.bounds[0]+(int)(i%17));
        f.input.bounds[3]=(int16_t)((int)f.input.bounds[1]+(int)(i%23));
        if(f.input.bounds[2]<f.input.bounds[0] || f.input.bounds[3]<f.input.bounds[1])continue;
        CHECK(evaluate(&f)==KPAINT_OK);
        CHECK((int16_t)f.output.commands[4].value[0]==f.input.bounds[0]);
        CHECK((int16_t)f.output.commands[4].value[2]==f.input.bounds[2]);
    }
    snapshot(&f);f.input.bounds[2]=f.input.bounds[0]-1;CHECK(unchanged_output(&f,KPAINT_RECT));
    CHECK(Kaffeine_PaintEvaluate(&f.plan,&f.input,f.eval_work,0,&f.output)==KPAINT_WORKSPACE);
    dispose(&f);
    /* A dynamically selected inverted endpoint pair is a native no-op. */
    init(&f);prologue(&f,3,4,7,1);coord(&f,0);coord(&f,1);coord(&f,2);coord(&f,3);
    move(&f,2,0,0,0,6);move(&f,2,0,2,0,0);move(&f,2,0,6,0,2);
    call(&f,306);epilogue(&f);CHECK(compile(&f)==KPAINT_OK);snapshot(&f);CHECK(evaluate(&f)==KPAINT_OK && f.output.count==2);dispose(&f);
}
static void test_additional_contracts(void)
{
    struct Fixture f;unsigned at,i;struct KaffeinePaintOutput saved;
    /* Pointer/private RP bytes may only prove a copy, never feed effects. */
    for(i=0;i<100;++i)if(i<24 || (i>=52 && i<56) || i>=66)
    {
        init(&f);prologue(&f,3,4,7,1);
        move(&f,1,5,2,0,0);w(&f,i);call(&f,342);coords(&f);call(&f,306);epilogue(&f);
        CHECK(compile(&f)==KPAINT_DATAFLOW);dispose(&f);
    }
    init(&f);prologue(&f,3,4,7,1);
    move(&f,1,5,2,0,0);w(&f,26);call(&f,342);coords(&f);call(&f,306);epilogue(&f);
    CHECK(compile(&f)==KPAINT_OK);CHECK(f.plan.rp_used[26]==255);snapshot(&f);
    CHECK(evaluate(&f)==KPAINT_OK && f.output.commands[1].value[0]==f.input.rp[26]);dispose(&f);
    /* Word pen load preserves arbitrary high register bits and takes low8. */
    init(&f);prologue(&f,3,4,7,1);constant(&f,0xa5a50000,0,0);
    move(&f,2,7,1,0,0);l(&f,DATA+2);call(&f,342);coords(&f);call(&f,306);epilogue(&f);
    CHECK(compile(&f)==KPAINT_OK);snapshot(&f);CHECK(evaluate(&f)==KPAINT_OK && f.output.commands[1].value[0]==7);
    at=cell_at(&f,DATA+2);CHECK(f.plan.cell[at].width==2 && f.plan.cell[at].used_mask==255);dispose(&f);
    /* EXT.W then EXT.L preserve the low byte needed by SetAPen. */
    init(&f);prologue(&f,3,4,7,1);move(&f,1,7,1,0,0);l(&f,DATA+3);w(&f,0x4880);w(&f,0x48c0);
    call(&f,342);coords(&f);call(&f,306);epilogue(&f);CHECK(compile(&f)==KPAINT_OK);
    snapshot(&f);at=cell_at(&f,DATA+3);f.input.cells[at]=0x80;CHECK(evaluate(&f)==KPAINT_OK && f.output.commands[1].value[0]==0x80);dispose(&f);
    /* A mutable mask expression is equality-promoted and contributes to the
     * lease union; changing even the separate input-cell copy is refused. */
    init(&f);prologue(&f,3,4,7,1);paint(&f,1,0);
    move(&f,4,7,1,1,1);l(&f,DATA+4);move(&f,4,7,1,0,0);l(&f,DATA+8);constant(&f,EXEC,1,6);call(&f,324);epilogue(&f);
    CHECK(compile(&f)==KPAINT_OK);CHECK(f.plan.signal_mask==(1u<<13));snapshot(&f);CHECK(evaluate(&f)==KPAINT_OK);
    at=cell_at(&f,DATA+8);CHECK(f.plan.cell[at].role&KPAINT_CELL_IMMUTABLE);f.input.cells[at]=0;CHECK(unchanged_output(&f,KPAINT_CHANGED));
    snapshot(&f);at=cell_at(&f,DATA+4);f.input.cells[at]=0;CHECK(unchanged_output(&f,KPAINT_SIGNAL));dispose(&f);
    /* Impossible instruction forms must not be accepted as valid semantics. */
    {static const unsigned invalid[]={0x4288,0x4e9a,0x4eda,0x41da,0x1008};
     for(i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i){init(&f);w(&f,invalid[i]);w(&f,0x4e75);CHECK(compile(&f)==KPAINT_UNSUPPORTED);dispose(&f);}}
    init(&f);w(&f,0x48e7);w(&f,1);w(&f,0x4e75);CHECK(compile(&f)==KPAINT_UNSUPPORTED);dispose(&f);
    /* TST only exposes Z; unsupported relational use remains refused. */
    init(&f);w(&f,0x7000);w(&f,0x4a80);w(&f,0x6702);w(&f,0xffff);w(&f,0x4e75);CHECK(compile(&f)==KPAINT_OK);dispose(&f);
    /* Compile and evaluation workspace capacity/alignment are bounded. */
    init(&f);w(&f,0x4e75);CHECK(Kaffeine_PaintWorkspaceAlignment()>=sizeof(uint32_t));
    CHECK(Kaffeine_PaintCompile(&f.source,(unsigned char*)f.compile_work+1,Kaffeine_PaintCompileWorkspaceSize(),&f.plan,&f.report)==KPAINT_WORKSPACE);
    CHECK(compile(&f)==KPAINT_OK);snapshot(&f);memset(&f.output,0x5a,sizeof(f.output));saved=f.output;
    CHECK(Kaffeine_PaintEvaluate(&f.plan,&f.input,(unsigned char*)f.eval_work+1,Kaffeine_PaintEvaluateWorkspaceSize(),&f.output)==KPAINT_WORKSPACE);
    CHECK(!memcmp(&saved,&f.output,sizeof(saved)));dispose(&f);
}
/* Original address-arithmetic variants, independent of any Hook input. */
static void adjust_op(struct Fixture*f,int sub,unsigned size,unsigned dest,
                      unsigned sm,unsigned sr)
{w(f,(sub?0x90c0:0xd0c0)|(size==4?0x0100:0)|(dest<<9)|(sm<<3)|sr);}
static void adjusted_prologue(struct Fixture*f,int sub,unsigned size,int reg_source)
{
    unsigned loop,db;uint32_t operand=sub?100u:(uint32_t)-100;
    w(f,0x48e7);w(f,0xfffe);movea(f,1,5);
    if(reg_source)constant(f,size==2?(0xa5a50000u|(operand&0xffff)):operand,0,4);
    w(f,0x7c00); /* Z=1, tested after address arithmetic */
    adjust_op(f,sub,size,7,reg_source?0:7,reg_source?4:4);
    if(!reg_source){if(size==4)l(f,operand);else w(f,operand);}
    w(f,0x6702);w(f,0xffff); /* ADDA/SUBA must leave Z unchanged */
    movea(f,2,3);movea(f,7,4);w(f,0x7e18);
    loop=f->pc;move(f,4,3,3,3,4);db=f->pc;w(f,0x51cf);w(f,loop-(db+2));
    movea(f,7,4);w(f,0x4294);constant(f,GFX,1,6);movea(f,4,1);
}
static void adjusted_epilogue(struct Fixture*f,int sub,unsigned size,int reg_source)
{
    uint32_t operand=sub?100u:(uint32_t)-100;
    if(reg_source)constant(f,size==2?(0xa5a50000u|(operand&0xffff)):operand,0,4);
    w(f,0x7c00);adjust_op(f,!sub,size,7,reg_source?0:7,reg_source?4:4);
    if(!reg_source){if(size==4)l(f,operand);else w(f,operand);}
    w(f,0x6702);w(f,0xffff);w(f,0x4cdf);w(f,0x7fff);w(f,0x4e75);f->code_end=f->pc;
}
static void test_address_adjustments(void)
{
    struct Fixture f;unsigned sub,size,reg;
    for(sub=0;sub<2;++sub)for(size=2;size<=4;size+=2)for(reg=0;reg<2;++reg)
    {
        init(&f);adjusted_prologue(&f,(int)sub,size,(int)reg);paint(&f,1,1);signal_tail(&f);
        adjusted_epilogue(&f,(int)sub,size,(int)reg);
        CHECK(compile(&f)==KPAINT_OK);snapshot(&f);CHECK(evaluate(&f)==KPAINT_OK);
        CHECK(f.output.count==9 && f.output.commands[1].value[0]==7);dispose(&f);
    }
    /* WORD operands sign-extend, including address-register sources. */
    init(&f);constant(&f,0x1234ffff,1,3);constant(&f,0x8001,1,4);
    adjust_op(&f,0,2,4,1,3);w(&f,0x4e75);CHECK(compile(&f)==KPAINT_OK);dispose(&f);
    /* Guest cells may change after compile: never freeze an arithmetic
     * operand merely because a sampled initial value happens to be100. */
    init(&f);adjust_op(&f,1,4,7,7,1);l(&f,DATA);w(&f,0x4e75);CHECK(compile(&f)==KPAINT_DATAFLOW);dispose(&f);
    init(&f);adjust_op(&f,1,2,7,7,4);w(&f,1021);w(&f,0x4e75);CHECK(compile(&f)==KPAINT_STACK);dispose(&f);
    init(&f);adjust_op(&f,0,4,7,7,4);l(&f,0x80000000);w(&f,0x4e75);CHECK(compile(&f)==KPAINT_STACK);dispose(&f);
    init(&f);adjust_op(&f,1,4,7,7,4);l(&f,0x80000000);w(&f,0x4e75);CHECK(compile(&f)==KPAINT_STACK);dispose(&f);
    init(&f);constant(&f,0xffffffff,1,3);adjust_op(&f,0,4,3,7,4);l(&f,1);w(&f,0x4e75);CHECK(compile(&f)==KPAINT_BOUNDS);dispose(&f);
    init(&f);constant(&f,0,1,3);adjust_op(&f,1,4,3,7,4);l(&f,1);w(&f,0x4e75);CHECK(compile(&f)==KPAINT_BOUNDS);dispose(&f);
}
int main(void)
{
    test_positive();test_trampoline_pointer();test_refusals();test_bounds_and_order();test_additional_contracts();test_address_adjustments();
    printf("paint compiler: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
