/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#define PAINT_FIXTURE_EMIT_ONLY
#define PAINT_FIXTURE_NO_MAIN
#include "paint_fixture_native.c"
#include "kaffeine_paint_compile.h"
#include <stdlib.h>

#define TEST_ARENA 131072u
#define TEST_BASE 0x4000u
#define TEST_OWNER 0x18000u
#define REQUIRE(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);++failed;}}while(0)
static unsigned checks,failed;
static uint8_t arena[TEST_ARENA],image[16384],second[16384];
static uint32_t number(const uint8_t*p,unsigned n)
{uint32_t v=0;unsigned i;for(i=0;i<n;++i)v=(v<<8)|p[i];return v;}
static int copy_read(void*opaque,uint32_t address,void*out,uint32_t n)
{(void)opaque;if(address>=TEST_ARENA || n>TEST_ARENA-address)return 0;memcpy(out,arena+address,n);return 1;}
static int data_read(void*opaque,uint32_t address,uint32_t n)
{
    struct PaintFixtureLayout*l=(struct PaintFixtureLayout*)opaque;
    return (address>=TEST_BASE+l->hook_data && address<TEST_BASE+l->hook_data+12 &&
            n<=TEST_BASE+l->hook_data+12-address) ||
           (address>=TEST_BASE+l->pen && address<TEST_BASE+l->pen+2 && n<=TEST_BASE+l->pen+2-address);
}
static enum KaffeinePaintStatus eval(const struct KaffeinePaintPlan*p,const struct KaffeinePaintInputs*i,
                                   void*work,struct KaffeinePaintOutput*out)
{return Kaffeine_PaintEvaluate(p,i,work,Kaffeine_PaintEvaluateWorkspaceSize(),out);}
int main(void)
{
    struct PaintFixtureLayout layout,other,sentinel;
    struct KaffeinePaintPlan*plan=malloc(sizeof(*plan));struct KaffeinePaintInputs*input=calloc(1,sizeof(*input));
    struct KaffeinePaintOutput output,saved;
    struct KaffeinePaintReport report;
    void*compile_work=malloc(Kaffeine_PaintCompileWorkspaceSize());void*eval_work=malloc(Kaffeine_PaintEvaluateWorkspaceSize());
    struct KaffeinePaintSource source;
    size_t bytes;uint32_t at,count,i,j,seen[256],owner=KPAINT_MAX_CELLS,pen=KPAINT_MAX_CELLS;
    if(!plan || !input || !compile_work || !eval_work)abort();
    bytes=pf_build_hunk(image,sizeof(image),&layout);REQUIRE(bytes>0 && bytes<=sizeof(image));
    REQUIRE(pf_build_hunk(second,sizeof(second),&other)==bytes && !memcmp(image,second,bytes));
    memset(second,0xa5,sizeof(second));memset(&other,0x5a,sizeof(other));sentinel=other;
    REQUIRE(pf_build_hunk(second,bytes-1,&other)==0 && !memcmp(&other,&sentinel,sizeof(other)));
    for(i=0;i<sizeof(second);++i)if(second[i]!=0xa5){REQUIRE(0);break;}
    REQUIRE(pf_build_hunk(NULL,0,&other)==0);
    REQUIRE(number(image,4)==0x3f3 && number(image+4,4)==0 && number(image+8,4)==1);
    REQUIRE(number(image+12,4)==0 && number(image+16,4)==0);
    REQUIRE(number(image+24,4)==0x3e9 && number(image+20,4)*4==layout.code_bytes && number(image+28,4)*4==layout.code_bytes);
    REQUIRE(layout.code_bytes<TEST_ARENA-TEST_BASE && 32+layout.code_bytes<bytes);
    memcpy(arena+TEST_BASE,image+32,layout.code_bytes);
    at=32+layout.code_bytes;REQUIRE(number(image+at,4)==0x3ec);at+=4;
    count=number(image+at,4);at+=4;REQUIRE(count>0 && count<=256);
    REQUIRE(number(image+at,4)==0);at+=4;
    for(i=0;i<count;++i)
    {
        uint32_t offset=number(image+at,4),value;at+=4;
        REQUIRE(!(offset&1) && offset<=layout.code_bytes-4);
        for(j=0;j<i;++j)REQUIRE(seen[j]!=offset);
        seen[i]=offset;value=number(arena+TEST_BASE+offset,4);REQUIRE(value<layout.code_bytes);
        pf_put32(arena+TEST_BASE+offset,value+TEST_BASE);
    }
    REQUIRE(number(image+at,4)==0 && number(image+at+4,4)==0x3f2 && at+8==bytes);
    REQUIRE(number(arena+TEST_BASE+layout.hook_header+8,4)==TEST_BASE+layout.hook_entry);
    REQUIRE(number(arena+TEST_BASE+layout.hook_header+12,4)==TEST_BASE+layout.pen);
    REQUIRE(number(arena+TEST_BASE+layout.hook_header+16,4)==TEST_BASE+layout.hook_data);
    REQUIRE(number(arena+TEST_BASE+layout.pen,2)==2);
    REQUIRE(number(arena+TEST_BASE+layout.new_window+4,2)==128 && number(arena+TEST_BASE+layout.new_window+6,2)==96);
    REQUIRE(number(arena+TEST_BASE+layout.new_window+10,4)==0 && number(arena+TEST_BASE+layout.new_window+46,2)==1);
    pf_put32(arena+TEST_BASE+layout.hook_data,0x1000);pf_put32(arena+TEST_BASE+layout.hook_data+4,0x3000);
    pf_put32(arena+TEST_BASE+layout.hook_data+8,TEST_OWNER);
    source=(struct KaffeinePaintSource){&layout,copy_read,data_read,TEST_ARENA,TEST_BASE+layout.hook_header,0x1000,0x3000};
    REQUIRE(Kaffeine_PaintCompile(&source,compile_work,Kaffeine_PaintCompileWorkspaceSize(),plan,&report)==KPAINT_OK);
    REQUIRE(plan->paths==2 && plan->signal_mask==PF_CTRL_D && report.steps<KPAINT_MAX_STEPS);
    for(i=0;i<plan->dependencies;++i)input->dependencies[i]=number(arena+plan->dependency[i].address,plan->dependency[i].width);
    for(i=0;i<plan->cells;++i)
    {
        input->cells[i]=number(arena+plan->cell[i].address,plan->cell[i].width);
        if(plan->cell[i].address==TEST_BASE+layout.hook_data+8)owner=i;
        if(plan->cell[i].address==TEST_BASE+layout.pen)pen=i;
    }
    REQUIRE(owner<KPAINT_MAX_CELLS && (plan->cell[owner].role&KPAINT_CELL_OWNER_CAP));
    REQUIRE(pen<KPAINT_MAX_CELLS && plan->cell[pen].used_mask==0xff && (plan->cell[pen].role&KPAINT_CELL_BYTE));
    input->bounds[0]=-30;input->bounds[1]=-20;input->bounds[2]=100;input->bounds[3]=90;
    input->owner_task=TEST_OWNER;input->owner_cookie=0x123456789abcdefULL;input->allowed_signals=PF_CTRL_D;
    REQUIRE(eval(plan,input,eval_work,&output)==KPAINT_OK && output.count==6);
    REQUIRE(output.commands[0].kind==KPAINT_CLONE && output.commands[1].kind==KPAINT_APEN && output.commands[1].value[0]==2);
    REQUIRE(output.commands[2].kind==KPAINT_DRAWMODE && output.commands[2].value[0]==0);
    REQUIRE(output.commands[3].kind==KPAINT_MASK && output.commands[3].value[0]==255);
    REQUIRE(output.commands[4].kind==KPAINT_RECTFILL);
    for(i=0;i<4;++i)REQUIRE((int16_t)output.commands[4].value[i]==input->bounds[i]);
    REQUIRE(output.commands[5].kind==KPAINT_DEFER_SIGNAL && output.commands[5].value[0]==TEST_OWNER &&
            output.commands[5].value[1]==PF_CTRL_D && output.commands[5].owner_cookie==input->owner_cookie);
    input->cells[owner]=0;REQUIRE(eval(plan,input,eval_work,&output)==KPAINT_OK && output.count==5);
    memset(&output,0xa5,sizeof(output));saved=output;input->cells[owner]=TEST_OWNER+4;
    REQUIRE(eval(plan,input,eval_work,&output)==KPAINT_SIGNAL && !memcmp(&saved,&output,sizeof(output)));
    input->cells[owner]=TEST_OWNER;input->cells[pen]=7;
    REQUIRE(eval(plan,input,eval_work,&output)==KPAINT_OK && output.commands[1].value[0]==7);
    for(i=0;i<plan->dependencies;++i)
    {
        input->dependencies[i]^=1;memset(&output,0xa5,sizeof(output));saved=output;
        REQUIRE(eval(plan,input,eval_work,&output)==KPAINT_CHANGED && !memcmp(&saved,&output,sizeof(output)));
        input->dependencies[i]^=1;
    }
    free(plan);free(input);free(compile_work);free(eval_work);
    printf("paint fixture emitter/compiler: %u checks, %u failed\n",checks,failed);return failed?1:0;
}
