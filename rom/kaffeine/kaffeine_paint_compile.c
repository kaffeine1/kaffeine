/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "kaffeine_paint_compile.h"
#include <limits.h>
#include <string.h>

/* This is a driver-side bounded symbolic compiler, not a runtime 68k CPU.
 * Foreign callbacks evaluate only its immutable integer expression plan. */
enum { X_CONST=1, X_UNKNOWN, X_RP, X_BOUND, X_OFFSET, X_LAYER, X_CELL,
       X_JOIN, X_EXTRACT, X_SIGN, X_ZERO, X_NOT, X_AND, X_BITAND };
enum { V_SCALAR=1, V_STACK, V_RP, V_BF, V_HOOK, V_RETURN };
#define MAX_VALUES 2048u
#define MAX_PENDING KPAINT_MAX_PATHS
#define BAD_EXPR ((uint16_t)0)
struct Value { uint32_t number; int32_t offset; uint16_t expr; uint8_t kind, pad; };
struct Byte { uint16_t value; uint8_t shift, pad; };
struct Clone { int32_t offset; uint8_t active, valid, pad[2]; };
struct State {
    uint32_t pc;
    uint16_t reg[16], zero, guard;
    uint16_t commands, calls;
    uint8_t clones, signalled, pad[2];
    struct Byte stack[KPAINT_STACK_BYTES];
    struct Clone clone[KPAINT_MAX_CLONES];
    struct KaffeinePaintAction action[KPAINT_MAX_COMMANDS];
};
struct Compiler {
    const struct KaffeinePaintSource *source;
    struct KaffeinePaintPlan plan;
    struct Value values[MAX_VALUES];
    uint16_t value_count, pending_count;
    uint32_t steps, pc;
    enum KaffeinePaintStatus error;
    uint32_t initial[KPAINT_MAX_NODES], cell_mask[KPAINT_MAX_NODES];
    uint8_t initial_known[KPAINT_MAX_NODES];
    uint32_t used[KPAINT_MAX_NODES];
    struct State state, pending[MAX_PENDING];
};
struct Eval { uint32_t value[KPAINT_MAX_NODES]; struct KaffeinePaintOutput out; };
struct Address { uint32_t number; int32_t offset; uint8_t kind; };
struct EA { uint8_t reg, memory; struct Address address; };

static uint32_t mask(unsigned width) { return width==32 ? UINT32_MAX : ((uint32_t)1<<width)-1; }
static uint32_t sign_value(uint32_t n,unsigned width)
{
    uint32_t sign=(uint32_t)1<<(width-1), m=mask(width);
    n&=m;
    return (n^sign)-sign;
}
static int fail(struct Compiler *c,enum KaffeinePaintStatus e)
{ if(c->error==KPAINT_OK)c->error=e; return 0; }
static int span(const struct KaffeinePaintSource *s,uint32_t address,uint32_t n)
{ return address<s->arena_size && n<=s->arena_size-address; }
static int read_bytes(struct Compiler *c,uint32_t address,void *out,uint32_t n)
{
    return span(c->source,address,n) && c->source->read(c->source->opaque,address,out,n) ?
           1 : fail(c,KPAINT_BOUNDS);
}
static int read_number(struct Compiler *c,uint32_t address,unsigned n,uint32_t *out)
{
    uint8_t b[4];unsigned i;uint32_t v=0;
    if(n!=1 && n!=2 && n!=4)return fail(c,KPAINT_INPUT);
    if(!read_bytes(c,address,b,n))return 0;
    for(i=0;i<n;++i)v=(v<<8)|b[i];
    *out=v;return 1;
}
static int dependency(struct Compiler *c,uint32_t address,unsigned width,unsigned kind,uint32_t value)
{
    unsigned i;
    for(i=0;i<c->plan.dependencies;++i)
        if(c->plan.dependency[i].address==address && c->plan.dependency[i].width==width &&
           c->plan.dependency[i].kind==kind)
            return c->plan.dependency[i].expected==value ? 1 : fail(c,KPAINT_CHANGED);
    if(c->plan.dependencies==KPAINT_MAX_DEPS)return fail(c,KPAINT_LIMIT);
    c->plan.dependency[c->plan.dependencies++]=(struct KaffeinePaintDependency){address,value,(uint8_t)width,(uint8_t)kind,0};
    return 1;
}
static int word(struct Compiler *c,struct State *s,uint16_t *out)
{
    uint32_t n;
    c->pc=s->pc;
    if(s->pc&1)return fail(c,KPAINT_BOUNDS);
    if(!read_number(c,s->pc,2,&n) || !dependency(c,s->pc,2,KPAINT_CODE,n))return 0;
    if(s->pc>UINT32_MAX-2)return fail(c,KPAINT_BOUNDS);
    s->pc+=2;*out=(uint16_t)n;return 1;
}
static const struct KaffeinePaintExpr *xp(const struct Compiler *c,uint16_t id)
{ return id && id<=c->plan.nodes ? &c->plan.expressions[id-1] : NULL; }
static unsigned xwidth(const struct Compiler *c,uint16_t id)
{ const struct KaffeinePaintExpr*x=xp(c,id);return x ? x->width : 0; }
static int constant(const struct Compiler*c,uint16_t id,uint32_t*out)
{const struct KaffeinePaintExpr*x=xp(c,id);if(!x || x->op!=X_CONST)return 0;*out=x->value;return 1;}
static uint16_t node(struct Compiler*c,unsigned op,uint16_t a,uint16_t b,unsigned width,unsigned shift,uint32_t value)
{
    struct KaffeinePaintExpr n={value,a,b,(uint8_t)op,(uint8_t)width,(uint8_t)shift,0};
    unsigned i;uint32_t av=0,bv=0;int known=0;
    if(c->error)return 0;
    for(i=0;i<c->plan.nodes;++i)if(!memcmp(&c->plan.expressions[i],&n,sizeof(n)))return (uint16_t)(i+1);
    if(c->plan.nodes==KPAINT_MAX_NODES){fail(c,KPAINT_LIMIT);return 0;}
    i=c->plan.nodes++;c->plan.expressions[i]=n;
    if(a)c->cell_mask[i]|=c->cell_mask[a-1];
    if(b)c->cell_mask[i]|=c->cell_mask[b-1];
    if(op==X_CONST){known=1;av=value;}
    else if(op==X_CELL){known=1;av=c->plan.cell[value].initial;c->cell_mask[i]=(uint32_t)1<<value;}
    else if(a && c->initial_known[a-1] && (!b || c->initial_known[b-1]))
    {
        av=c->initial[a-1];bv=b?c->initial[b-1]:0;known=1;
        switch(op){
        case X_JOIN:av=((av&mask(32-shift))<<shift)|(bv&mask(shift));break;
        case X_EXTRACT:av=(av>>shift)&mask(width);break;
        case X_SIGN:av=sign_value(av,shift);break;
        case X_ZERO:av=(av&mask(shift))==0;break;
        case X_NOT:av=!av;break;
        case X_AND:av=!!av && !!bv;break;
        case X_BITAND:av&=bv;break;
        default:known=0;break;
        }
    }
    c->initial[i]=av;c->initial_known[i]=(uint8_t)known;
    return (uint16_t)(i+1);
}
static uint16_t xc(struct Compiler*c,uint32_t n,unsigned width)
{ return node(c,X_CONST,0,0,width,0,n&mask(width)); }
static uint16_t extract(struct Compiler*c,uint16_t id,unsigned shift,unsigned width);
static uint16_t join(struct Compiler*c,uint16_t hi,uint16_t lo,unsigned low_width)
{
    uint32_t h,l;const struct KaffeinePaintExpr *a=xp(c,hi),*b=xp(c,lo);
    unsigned width=xwidth(c,hi)+low_width;
    if(!a || !b || width>32 || !low_width){fail(c,KPAINT_DATAFLOW);return 0;}
    if(constant(c,hi,&h) && constant(c,lo,&l))return xc(c,(h<<low_width)|(l&mask(low_width)),width);
    if(a->op==X_EXTRACT && b->op==X_EXTRACT && a->a==b->a && a->shift==b->shift+low_width)
        return extract(c,a->a,b->shift,width);
    return node(c,X_JOIN,hi,lo,width,low_width,0);
}
static uint16_t extract(struct Compiler*c,uint16_t id,unsigned shift,unsigned width)
{
    const struct KaffeinePaintExpr*x=xp(c,id);uint32_t n;
    if(!x || !width || width>32 || shift>32-width){fail(c,KPAINT_DATAFLOW);return 0;}
    if(!shift && width==x->width)return id;
    if(constant(c,id,&n))return xc(c,(n>>shift)&mask(width),width);
    if(x->op==X_EXTRACT && shift+width<=x->width)return extract(c,x->a,x->shift+shift,width);
    if(x->op==X_JOIN)
    {
        if(shift>=x->shift)return extract(c,x->a,shift-x->shift,width);
        if(shift+width<=x->shift)return extract(c,x->b,shift,width);
        return join(c,extract(c,x->a,0,shift+width-x->shift),extract(c,x->b,shift,x->shift-shift),x->shift-shift);
    }
    if(x->op==X_SIGN && shift+width<=x->shift)return extract(c,x->a,shift,width);
    return node(c,X_EXTRACT,id,0,width,shift,0);
}
static uint16_t sx(struct Compiler*c,uint16_t id,unsigned width)
{
    uint32_t n;id=extract(c,id,0,width);
    if(constant(c,id,&n))return xc(c,sign_value(n,width),32);
    return node(c,X_SIGN,id,0,32,width,0);
}
static uint16_t zexpr(struct Compiler*c,uint16_t id,unsigned width)
{
    uint32_t n;id=extract(c,id,0,width);
    if(constant(c,id,&n))return xc(c,!n,1);
    return node(c,X_ZERO,id,0,1,width,0);
}
static uint16_t xnot(struct Compiler*c,uint16_t id)
{
    uint32_t n;const struct KaffeinePaintExpr*x=xp(c,id);
    if(constant(c,id,&n))return xc(c,!n,1);
    if(x && x->op==X_NOT)return x->a;
    return node(c,X_NOT,id,0,1,0,0);
}
static uint16_t xand(struct Compiler*c,uint16_t a,uint16_t b)
{
    uint32_t n;const struct KaffeinePaintExpr*x=xp(c,a),*y=xp(c,b);
    if(constant(c,a,&n))return n?b:a;
    if(constant(c,b,&n))return n?a:b;
    if(a==b)return a;
    if((x && x->op==X_NOT && x->a==b)||(y && y->op==X_NOT && y->a==a))return xc(c,0,1);
    return node(c,X_AND,a,b,1,0,0);
}
static uint16_t value(struct Compiler*c,unsigned kind,uint16_t expr,int32_t off,uint32_t n)
{
    struct Value v={n,off,expr,(uint8_t)kind,0};unsigned i;
    if(c->error)return 0;
    for(i=0;i<c->value_count;++i)if(!memcmp(&c->values[i],&v,sizeof(v)))return (uint16_t)(i+1);
    if(c->value_count==MAX_VALUES){fail(c,KPAINT_LIMIT);return 0;}
    c->values[c->value_count++]=v;return c->value_count;
}
static const struct Value *vp(const struct Compiler*c,uint16_t id)
{return id && id<=c->value_count?&c->values[id-1]:NULL;}
static uint16_t scalar(struct Compiler*c,uint16_t expr){return expr?value(c,V_SCALAR,expr,0,0):0;}
static int promote(struct Compiler*c,uint16_t id,unsigned kind);
static uint16_t add_pointer(struct Compiler*c,uint16_t id,int32_t delta)
{
    const struct Value*v=vp(c,id);int64_t off;
    if(!v){fail(c,KPAINT_DATAFLOW);return 0;}
    if(v->kind==V_SCALAR)
    {
        uint32_t n;
        if(!constant(c,v->expr,&n))
        {if(!promote(c,v->expr,KPAINT_POINTER))return 0;n=c->initial[v->expr-1];}
        return scalar(c,xc(c,n+(uint32_t)delta,32));
    }
    if(v->kind==V_RETURN){fail(c,KPAINT_DATAFLOW);return 0;}
    off=(int64_t)v->offset+delta;
    if(off<INT32_MIN || off>INT32_MAX){fail(c,KPAINT_BOUNDS);return 0;}
    return value(c,v->kind,0,(int32_t)off,v->number);
}
/* ADDA/SUBA keep CCR intact. Their operand must be a compile-time
 * scalar constant. Abstract stack coordinates have no numeric guest base:
 * validate their mathematical displacement before narrowing or publishing. */
static uint16_t adjust_address(struct Compiler*c,uint16_t id,int64_t delta)
{
    const struct Value*v=vp(c,id);int64_t result;
    if(!v){fail(c,KPAINT_DATAFLOW);return 0;}
    if(v->kind==V_SCALAR)
    {
        if(!promote(c,v->expr,KPAINT_POINTER))return 0;
        result=(int64_t)c->initial[v->expr-1]+delta;
        /* Wrapping numeric addresses are outside this restricted profile. */
        if(result<0 || result>UINT32_MAX){fail(c,KPAINT_BOUNDS);return 0;}
        return scalar(c,xc(c,(uint32_t)result,32));
    }
    if(v->kind==V_RETURN){fail(c,KPAINT_DATAFLOW);return 0;}
    result=(int64_t)v->offset+delta;
    if(v->kind==V_STACK && (result<0 || result>KPAINT_STACK_BYTES))
    {fail(c,KPAINT_STACK);return 0;}
    if(result<INT32_MIN || result>INT32_MAX){fail(c,KPAINT_BOUNDS);return 0;}
    return value(c,v->kind,0,(int32_t)result,v->number);
}
static uint16_t scalar_expr(struct Compiler*c,uint16_t id)
{
    const struct Value*v=vp(c,id);
    if(!v){fail(c,KPAINT_DATAFLOW);return 0;}
    if(v->kind==V_SCALAR)return v->expr;
    if(v->kind==V_HOOK)return xc(c,c->source->hook+(uint32_t)v->offset,32);
    fail(c,KPAINT_DATAFLOW);return 0;
}
static int promote(struct Compiler*c,uint16_t id,unsigned kind)
{
    uint32_t cells;unsigned i;
    if(!id || !c->initial_known[id-1])return fail(c,KPAINT_DATAFLOW);
    cells=c->cell_mask[id-1];
    for(i=0;i<c->plan.cells;++i)if(cells&((uint32_t)1<<i))
    {
        struct KaffeinePaintCell*cell=&c->plan.cell[i];
        if(!dependency(c,cell->address,cell->width,kind,cell->initial))return 0;
        cell->role|=KPAINT_CELL_IMMUTABLE;
    }
    return 1;
}
static int address(struct Compiler*c,uint16_t id,struct Address*out)
{
    const struct Value*v=vp(c,id);
    if(!v)return fail(c,KPAINT_DATAFLOW);
    if(v->kind==V_SCALAR)
    {
        if(!promote(c,v->expr,KPAINT_POINTER))return 0;
        out->kind=V_SCALAR;out->number=c->initial[v->expr-1];out->offset=0;return 1;
    }
    if(v->kind==V_RETURN)return fail(c,KPAINT_DATAFLOW);
    out->kind=v->kind;out->offset=v->offset;out->number=v->number;return 1;
}
static int advance_address(struct Compiler*c,struct Address*a,int32_t n)
{
    int64_t off=(int64_t)a->offset+n;
    if(a->kind==V_SCALAR){a->number+=(uint32_t)n;return 1;}
    if(off<INT32_MIN || off>INT32_MAX)return fail(c,KPAINT_BOUNDS);
    a->offset=(int32_t)off;return 1;
}
static uint16_t byte_expr(struct Compiler*c,struct Byte b)
{
    const struct Value*v=vp(c,b.value);
    if(!v || v->kind!=V_SCALAR){fail(c,KPAINT_DATAFLOW);return 0;}
    return extract(c,v->expr,b.shift,8);
}
static uint16_t memory_read(struct Compiler*c,struct State*s,struct Address a,unsigned n)
{
    uint16_t e=0;unsigned i;uint32_t v;
    if(n!=1 && ((a.kind==V_SCALAR?a.number:(uint32_t)a.offset)&1)){fail(c,KPAINT_BOUNDS);return 0;}
    if(a.kind==V_STACK)
    {
        struct Byte b;
        if(a.offset<0 || (uint32_t)a.offset>KPAINT_STACK_BYTES-n){fail(c,KPAINT_STACK);return 0;}
        b=s->stack[a.offset];
        if(n==4 && b.value && b.shift==24)
        {
            for(i=1;i<4;++i)if(s->stack[a.offset+i].value!=b.value || s->stack[a.offset+i].shift!=24-8*i)break;
            if(i==4)return b.value;
        }
        for(i=0;i<n;++i)
        {uint16_t part=byte_expr(c,s->stack[a.offset+i]);if(!part)return 0;e=i?join(c,e,part,8):part;}
        return scalar(c,e);
    }
    if(a.kind==V_RP)
    {
        if(a.offset<0 || (uint32_t)a.offset>KPAINT_RASTPORT_BYTES-n){fail(c,KPAINT_BOUNDS);return 0;}
        for(i=0;i<n;++i){uint16_t p=node(c,X_RP,0,0,8,0,(uint32_t)a.offset+i);e=i?join(c,e,p,8):p;}
        return scalar(c,e);
    }
    if(a.kind==V_BF)
    {
        if(a.offset<0 || (uint32_t)a.offset>20-n){fail(c,KPAINT_BOUNDS);return 0;}
        for(i=0;i<n;++i)
        {
            unsigned at=(unsigned)a.offset+i,shift;uint16_t p;
            if(at<4){p=node(c,X_LAYER,0,0,32,0,0);shift=24-8*at;}
            else if(at<12){p=node(c,X_BOUND,0,0,16,0,(at-4)/2);shift=(at&1)?0:8;}
            else {p=node(c,X_OFFSET,0,0,32,0,(at-12)/4);shift=24-8*((at-12)%4);}
            p=extract(c,p,shift,8);e=i?join(c,e,p,8):p;
        }
        return scalar(c,e);
    }
    if(a.kind==V_HOOK)
    {
        if(a.offset<0 || (uint32_t)a.offset>20-n){fail(c,KPAINT_BOUNDS);return 0;}
        a.number=c->source->hook+(uint32_t)a.offset;
        if(!read_number(c,a.number,n,&v) || !dependency(c,a.number,n,KPAINT_HEADER,v))return 0;
        return scalar(c,xc(c,v,n*8));
    }
    if(a.kind!=V_SCALAR){fail(c,KPAINT_DATAFLOW);return 0;}
    if(!read_number(c,a.number,n,&v))return 0;
    if(a.number==4 && n==4 && v==c->source->exec_base)
    {
        if(!dependency(c,4,4,KPAINT_POINTER,v))return 0;
        return scalar(c,xc(c,v,32));
    }
    if(!c->source->ordinary || !c->source->ordinary(c->source->opaque,a.number,n))
    {fail(c,KPAINT_BOUNDS);return 0;}
    for(i=0;i<c->plan.cells;++i)if(c->plan.cell[i].address==a.number && c->plan.cell[i].width==n)
    {
        if(c->plan.cell[i].initial!=v){fail(c,KPAINT_CHANGED);return 0;}
        return scalar(c,node(c,X_CELL,0,0,n*8,0,i));
    }
    if(i==KPAINT_MAX_CELLS){fail(c,KPAINT_LIMIT);return 0;}
    c->plan.cell[c->plan.cells++]=(struct KaffeinePaintCell){a.number,v,0,(uint8_t)n,0,{0,0}};
    return scalar(c,node(c,X_CELL,0,0,n*8,0,i));
}
static int memory_write(struct Compiler*c,struct State*s,struct Address a,unsigned n,uint16_t id,int api)
{
    unsigned i,k;const struct Value*v=vp(c,id);
    if(a.kind!=V_STACK)return fail(c,KPAINT_STORE);
    if(!v || a.offset<0 || (uint32_t)a.offset>KPAINT_STACK_BYTES-n || (n!=1 && (a.offset&1)))return fail(c,KPAINT_STACK);
    if(n!=4 && v->kind!=V_SCALAR)return fail(c,KPAINT_DATAFLOW);
    if(!api)for(k=0;k<s->clones;++k)if(s->clone[k].valid)
        for(i=0;i<n;++i)
        {
            int32_t at=a.offset+(int32_t)i-s->clone[k].offset;
            if(at>=0 && at<100 && at!=24)
            {
                uint32_t zero;
                if(at>=4 || v->kind!=V_SCALAR || !constant(c,extract(c,v->expr,(n-1-i)*8,8),&zero) || zero)
                    s->clone[k].valid=0;
            }
        }
    for(i=0;i<n;++i)s->stack[a.offset+i]=(struct Byte){id,(uint8_t)((n-1-i)*8),0};
    return 1;
}
static int ea(struct Compiler*c,struct State*s,unsigned mode,unsigned reg,unsigned n,int address_only,struct EA*out)
{
    uint16_t w;uint32_t v;int32_t displacement=0;uint16_t base=0;
    memset(out,0,sizeof(*out));
    if(mode<2)
    {
        if(address_only)return fail(c,KPAINT_UNSUPPORTED);
        out->reg=(uint8_t)(reg+(mode?8:0));return 1;
    }
    out->memory=1;
    if(mode<=6)
    {
        base=s->reg[8+reg];
        if(mode==4)
        {base=add_pointer(c,base,-(int32_t)((n==1 && reg==7)?2:n));s->reg[8+reg]=base;}
        if(mode==5)
        {if(!word(c,s,&w))return 0;displacement=(int16_t)w;}
        if(mode==6)
        {
            const struct Value*index;uint32_t number;
            if(!word(c,s,&w) || (w&0x0700))return fail(c,KPAINT_UNSUPPORTED);
            index=vp(c,s->reg[(w>>12)&15]);
            if(!index || index->kind!=V_SCALAR || !constant(c,index->expr,&number))return fail(c,KPAINT_DATAFLOW);
            if(!(w&0x0800))number=sign_value(number,16);
            displacement=(int32_t)(number+(uint32_t)(int32_t)(int8_t)w);
        }
        if(displacement)base=add_pointer(c,base,displacement);
        if(!address(c,base,&out->address))return 0;
        if(mode==3)s->reg[8+reg]=add_pointer(c,s->reg[8+reg],(int32_t)((n==1 && reg==7)?2:n));
        return c->error==KPAINT_OK;
    }
    if(mode!=7)return fail(c,KPAINT_UNSUPPORTED);
    if(reg==0){if(!word(c,s,&w))return 0;v=(uint32_t)(int32_t)(int16_t)w;}
    else if(reg==1){uint16_t lo;if(!word(c,s,&w)||!word(c,s,&lo))return 0;v=((uint32_t)w<<16)|lo;}
    else if(reg==2)
    {uint32_t pc=s->pc;if(!word(c,s,&w))return 0;v=pc+(uint32_t)(int32_t)(int16_t)w;}
    else if(reg==4 && !address_only)
    {
        if(n==4){uint16_t lo;if(!word(c,s,&w)||!word(c,s,&lo))return 0;v=((uint32_t)w<<16)|lo;}
        else {if(!word(c,s,&w))return 0;v=w&mask(n*8);}
        out->memory=2;out->address.number=v;return 1;
    }
    else return fail(c,KPAINT_UNSUPPORTED);
    out->address.kind=V_SCALAR;out->address.number=v;return 1;
}
static uint16_t get_ea(struct Compiler*c,struct State*s,const struct EA*e,unsigned n)
{
    const struct Value*v;
    if(e->memory==1)return memory_read(c,s,e->address,n);
    if(e->memory==2)return scalar(c,xc(c,e->address.number,n*8));
    v=vp(c,s->reg[e->reg]);
    if(!v){fail(c,KPAINT_DATAFLOW);return 0;}
    if(n==4)return s->reg[e->reg];
    if(v->kind!=V_SCALAR){fail(c,KPAINT_DATAFLOW);return 0;}
    return scalar(c,extract(c,v->expr,0,n*8));
}
static int put_ea(struct Compiler*c,struct State*s,const struct EA*e,unsigned n,uint16_t id)
{
    const struct Value*v=vp(c,id),*old;
    if(e->memory==1)return memory_write(c,s,e->address,n,id,0);
    if(e->memory==2)return fail(c,KPAINT_STORE);
    if(e->reg>=8)
    {
        if(n!=4)return fail(c,KPAINT_UNSUPPORTED);
        s->reg[e->reg]=id;return 1;
    }
    if(n==4){s->reg[e->reg]=id;return 1;}
    old=vp(c,s->reg[e->reg]);
    if(!v || !old || v->kind!=V_SCALAR || old->kind!=V_SCALAR)return fail(c,KPAINT_DATAFLOW);
    s->reg[e->reg]=scalar(c,join(c,extract(c,old->expr,n*8,32-n*8),extract(c,v->expr,0,n*8),n*8));
    return c->error==KPAINT_OK;
}
static int flag_zero(struct Compiler*c,struct State*s,uint16_t id,unsigned n)
{
    const struct Value*v=vp(c,id);
    if(!v)return fail(c,KPAINT_DATAFLOW);
    s->zero=v->kind==V_SCALAR?zexpr(c,v->expr,n*8):xc(c,0,1);
    return c->error==KPAINT_OK;
}
static int emit(struct Compiler*c,struct State*s,unsigned kind,unsigned clone,uint16_t a,uint16_t b,uint16_t d,uint16_t e)
{
    if(s->commands==KPAINT_MAX_COMMANDS)return fail(c,KPAINT_LIMIT);
    s->action[s->commands++]=(struct KaffeinePaintAction){{a,b,d,e},(uint8_t)kind,(uint8_t)clone,0};return 1;
}
static int clone_rp(struct Compiler*c,struct State*s,int32_t at)
{
    unsigned i,k;uint32_t zero;
    if(at<0 || (uint32_t)at>KPAINT_STACK_BYTES-100 || (at&1))return fail(c,KPAINT_BAD_CLONE),-1;
    for(k=0;k<s->clones;++k)if(s->clone[k].active && s->clone[k].offset==at && s->clone[k].valid)return (int)k;
    for(i=0;i<100;++i)
    {
        uint16_t e=byte_expr(c,s->stack[at+i]);
        if(!e)return -1;
        if(i<4){if(!constant(c,e,&zero) || zero)return fail(c,KPAINT_BAD_CLONE),-1;}
        else if(i!=24 && e!=node(c,X_RP,0,0,8,0,i))return fail(c,KPAINT_BAD_CLONE),-1;
    }
    for(k=0;k<s->clones;++k)if(s->clone[k].offset==at)break;
    if(k==s->clones)
    {if(s->clones==KPAINT_MAX_CLONES)return fail(c,KPAINT_LIMIT),-1;++s->clones;}
    s->clone[k]=(struct Clone){at,1,1,{0,0}};
    if(!emit(c,s,KPAINT_CLONE,k,0,0,0,0))return -1;
    return (int)k;
}
static int bound_rank(struct Compiler*c,uint16_t expr,unsigned axis)
{
    const struct KaffeinePaintExpr*x;
    expr=extract(c,expr,0,16);x=xp(c,expr);
    if(x && x->op==X_BOUND && x->value%2==axis)return (int)(x->value/2);
    return -1;
}
static int known_call(struct Compiler*c,struct State*s,uint32_t target)
{
    unsigned lvo=0;int clone;uint16_t e[4]={0,0,0,0};unsigned i;
    const struct Value*rp;
    if(c->source->graphics_base && target<c->source->graphics_base)lvo=c->source->graphics_base-target;
    if(lvo==342 || lvo==354 || lvo==306)
    {
        if(s->signalled)return fail(c,KPAINT_DATAFLOW);
        rp=vp(c,s->reg[9]);
        if(!rp || rp->kind!=V_STACK)return fail(c,KPAINT_BAD_CLONE);
        clone=clone_rp(c,s,rp->offset);if(clone<0)return 0;
        if(lvo==306)
        {
            int rank[4];
            for(i=0;i<4;++i){e[i]=scalar_expr(c,s->reg[i]);rank[i]=bound_rank(c,e[i],i%2);}
            if(c->error)return 0;
            if(rank[0]<0 || rank[1]<0 || rank[2]<0 || rank[3]<0)return fail(c,KPAINT_RECT);
            e[0]=byte_expr(c,s->stack[rp->offset+24]);
            if(!emit(c,s,KPAINT_MASK,clone,e[0],0,0,0))return 0;
            for(i=0;i<4;++i)e[i]=extract(c,scalar_expr(c,s->reg[i]),0,16);
            if(!emit(c,s,KPAINT_RECTFILL,clone,e[0],e[1],e[2],e[3]))return 0;
        }
        else
        {
            struct Address a={0,rp->offset+(lvo==342?25:28),V_STACK};
            e[0]=extract(c,scalar_expr(c,s->reg[0]),0,8);
            if(!e[0] || !emit(c,s,lvo==342?KPAINT_APEN:KPAINT_DRAWMODE,clone,e[0],0,0,0))return 0;
            if(!memory_write(c,s,a,1,scalar(c,e[0]),1))return 0;
            a.offset=rp->offset+30;if(!memory_write(c,s,a,1,scalar(c,xc(c,15,8)),1))return 0;
            if(lvo==342)
            {
                /* Native SetAPen clears RPF_NO_PENS (bit14), without
                 * recomputing minterms in current AROS. */
                a.offset=rp->offset+32;
                e[1]=scalar_expr(c,memory_read(c,s,a,2));
                e[1]=node(c,X_BITAND,e[1],xc(c,0xbfff,16),16,0,0);
                if(!memory_write(c,s,a,2,scalar(c,e[1]),1))return 0;
            }
            if(lvo==354)
            {
                for(i=40;i<48;++i){a.offset=rp->offset+(int32_t)i;if(!memory_write(c,s,a,1,scalar(c,node(c,X_UNKNOWN,0,0,8,0,0x100+i)),1))return 0;}
            }
        }
        s->reg[0]=scalar(c,xc(c,0,32));return c->error==KPAINT_OK;
    }
    if(c->source->exec_base>=324 && target==c->source->exec_base-324)
    {
        const struct KaffeinePaintExpr*x;
        e[0]=scalar_expr(c,s->reg[9]);e[1]=scalar_expr(c,s->reg[0]);
        if(!e[0] || !e[1])return 0;
        x=xp(c,e[0]);
        if(x->op==X_CELL && x->width==32)c->plan.cell[x->value].role|=KPAINT_CELL_OWNER_CAP;
        else if(x->op!=X_CONST || !x->value || x->value>=c->source->arena_size)return fail(c,KPAINT_SIGNAL);
        if(!promote(c,e[1],KPAINT_IMMUTABLE_DATA))return 0;
        c->plan.signal_mask|=c->initial[e[1]-1];
        if(!emit(c,s,KPAINT_DEFER_SIGNAL,0,e[0],e[1],0,0))return 0;
        s->signalled=1;s->reg[0]=scalar(c,xc(c,0,32));return 1;
    }
    return -1;
}
static int branch(struct Compiler*c,struct State*s,uint32_t target,uint16_t condition)
{
    uint32_t n;
    if(constant(c,condition,&n)){if(n)s->pc=target;return 1;}
    if(c->pending_count==MAX_PENDING)return fail(c,KPAINT_LIMIT);
    c->pending[c->pending_count]=*s;
    c->pending[c->pending_count].pc=target;
    c->pending[c->pending_count].guard=xand(c,s->guard,condition);
    ++c->pending_count;
    s->guard=xand(c,s->guard,xnot(c,condition));
    return c->error==KPAINT_OK;
}
static int finish_path(struct Compiler*c,struct State*s)
{
    struct KaffeinePaintPath*p;uint32_t n;
    if(constant(c,s->guard,&n) && !n)return 1;
    if(c->plan.paths==KPAINT_MAX_PATHS)return fail(c,KPAINT_LIMIT);
    p=&c->plan.path[c->plan.paths++];p->guard=s->guard;p->count=s->commands;
    memcpy(p->commands,s->action,s->commands*sizeof(s->action[0]));return 1;
}
static int execute_symbolic(struct Compiler*c,struct State*s)
{
    uint16_t op,w,from;unsigned top,size,mode,reg;struct EA a,b;
    uint32_t number;const struct Value*v;
    for(;;)
    {
        if(c->error)return 0;
        if(++c->steps>KPAINT_MAX_STEPS)return fail(c,KPAINT_LIMIT);
        if(constant(c,s->guard,&number) && !number)return 1;
        if(!word(c,s,&op))return 0;
        top=op>>12;
        if(top==1 || top==2 || top==3)
        {
            size=top==1?1:top==2?4:2;
            if(size==1 && ((op>>3)&7)==1)return fail(c,KPAINT_UNSUPPORTED);
            if(!ea(c,s,(op>>3)&7,op&7,size,0,&a))return 0;
            from=get_ea(c,s,&a,size);if(!from)return 0;
            mode=(op>>6)&7;reg=(op>>9)&7;
            if(mode==1)
            {
                if(size==1)return fail(c,KPAINT_UNSUPPORTED);
                if(size==2)from=scalar(c,sx(c,scalar_expr(c,from),16));
                s->reg[8+reg]=from;
            }
            else
            {
                if(!ea(c,s,mode,reg,size,0,&b) || !put_ea(c,s,&b,size,from) || !flag_zero(c,s,from,size))return 0;
            }
            continue;
        }
        if((op&0xf100)==0x7000)
        {s->reg[(op>>9)&7]=scalar(c,xc(c,(uint32_t)(int32_t)(int8_t)op,32));if(!flag_zero(c,s,s->reg[(op>>9)&7],4))return 0;continue;}
        if((op&0xf1c0)==0x41c0)
        {
            mode=(op>>3)&7;reg=op&7;
            if(mode<2 || mode==3 || mode==4 || (mode==7 && reg>3))return fail(c,KPAINT_UNSUPPORTED);
            if(!ea(c,s,(op>>3)&7,op&7,4,1,&a))return 0;
            s->reg[8+((op>>9)&7)]=a.address.kind==V_SCALAR?scalar(c,xc(c,a.address.number,32)):value(c,a.address.kind,0,a.address.offset,a.address.number);
            continue;
        }
        if((op&0xfff8)==0x4880 || (op&0xfff8)==0x48c0)
        {
            reg=op&7;size=(op&0x0040)?16:8;
            from=scalar_expr(c,s->reg[reg]);if(!from)return 0;
            if(size==8)
            {
                a=(struct EA){(uint8_t)reg,0,{0,0,0}};
                if(!put_ea(c,s,&a,2,scalar(c,extract(c,sx(c,from,8),0,16))))return 0;
            }
            else s->reg[reg]=scalar(c,sx(c,from,16));
            if(!flag_zero(c,s,s->reg[reg],size==8?2:4))return 0;
            continue;
        }
        if((op&0xf0c0)==0x90c0 || (op&0xf0c0)==0xd0c0)
        {
            int64_t delta;size=(op&0x0100)?4:2;
            if(!ea(c,s,(op>>3)&7,op&7,size,0,&a))return 0;
            from=get_ea(c,s,&a,size);if(!from)return 0;
            from=scalar_expr(c,from);
            if(!constant(c,from,&number))return fail(c,KPAINT_DATAFLOW);
            delta=size==2?(int64_t)(number&0xffff)-((number&0x8000)?65536:0):
                         (int64_t)number-((number&0x80000000u)?4294967296LL:0);
            if((op&0xf000)==0x9000)delta=-delta;
            reg=8+((op>>9)&7);s->reg[reg]=adjust_address(c,s->reg[reg],delta);
            if(!s->reg[reg])return 0;
            continue;
        }
        if((op&0xfb80)==0x4880)
        {
            uint16_t saved[16];unsigned count=0,i,load=(op&0x0400)!=0;
            struct Address at;uint16_t base_id;int32_t delta;
            mode=(op>>3)&7;reg=op&7;size=(op&0x40)?4:2;
            if((load && mode==4)||(!load && mode==3)||mode<2 ||
               (mode==7 && reg>(load?3u:1u)))return fail(c,KPAINT_UNSUPPORTED);
            if(!word(c,s,&w))return 0;
            /* Predecrement base-in-mask differs across CPU models; outside
             * this deliberately 68000-only proof profile. */
            if(mode==4 && (w&((uint16_t)1<<(7-reg))))return fail(c,KPAINT_UNSUPPORTED);
            for(i=0;i<16;++i)if(w&((uint16_t)1<<i))++count;
            if(!count)return fail(c,KPAINT_UNSUPPORTED);
            memcpy(saved,s->reg,sizeof(saved));base_id=s->reg[8+reg];
            if(mode==4)
            {
                if(!address(c,base_id,&at))return 0;
                for(i=0;i<16;++i)if(w&((uint16_t)1<<i))
                {if(!advance_address(c,&at,-(int32_t)size)||!memory_write(c,s,at,size,saved[15-i],0))return 0;}
                s->reg[8+reg]=add_pointer(c,base_id,-(int32_t)(count*size));
            }
            else
            {
                if(!ea(c,s,mode==3?2:mode,reg,size,1,&a))return 0;
                at=a.address;
                for(i=0;i<16;++i)if(w&((uint16_t)1<<i))
                {
                    if(load){from=memory_read(c,s,at,size);if(size==2)from=scalar(c,sx(c,scalar_expr(c,from),16));if(!from)return 0;s->reg[i]=from;}
                    else if(!memory_write(c,s,at,size,saved[i],0))return 0;
                    if(!advance_address(c,&at,(int32_t)size))return 0;
                }
                delta=(int32_t)(count*size);
                if(mode==3)s->reg[8+reg]=add_pointer(c,base_id,delta);
            }
            continue;
        }
        if((op&0xff00)==0x4200 || (op&0xffc0)==0x50c0)
        {
            int st=(op&0xffc0)==0x50c0;
            mode=(op>>3)&7;reg=op&7;
            if(mode==1 || (mode==7 && reg>1))return fail(c,KPAINT_UNSUPPORTED);
            size=st?1:((op>>6)&3)==0?1:((op>>6)&3)==1?2:((op>>6)&3)==2?4:0;
            if(!size || !ea(c,s,(op>>3)&7,op&7,size,0,&a))return 0;
            from=scalar(c,xc(c,st?0xff:0,size*8));
            if(!put_ea(c,s,&a,size,from))return 0;
            if(!st)s->zero=xc(c,1,1);
            continue;
        }
        if((op&0xff00)==0x4a00)
        {
            mode=(op>>3)&7;reg=op&7;
            size=((op>>6)&3)==0?1:((op>>6)&3)==1?2:((op>>6)&3)==2?4:0;
            if(!size || mode==1 || (mode==7 && reg>1))return fail(c,KPAINT_UNSUPPORTED);
            if(!ea(c,s,mode,reg,size,0,&a))return 0;
            from=get_ea(c,s,&a,size);if(!from || !flag_zero(c,s,from,size))return 0;
            continue;
        }
        if((op&0xfff8)==0x51c8)
        {
            uint32_t base=s->pc;reg=op&7;
            if(!word(c,s,&w))return 0;
            from=extract(c,scalar_expr(c,s->reg[reg]),0,16);
            if(!constant(c,from,&number))return fail(c,KPAINT_DATAFLOW);
            number=(number-1)&0xffff;
            a=(struct EA){(uint8_t)reg,0,{0,0,0}};
            if(!put_ea(c,s,&a,2,scalar(c,xc(c,number,16))))return 0;
            if(number!=0xffff)s->pc=base+(uint32_t)(int32_t)(int16_t)w;
            continue;
        }
        if((op&0xf000)==0x6000)
        {
            unsigned cc=(op>>8)&15;uint32_t base=s->pc;int32_t off=(int8_t)op;
            if(!(op&255)){if(!word(c,s,&w))return 0;off=(int16_t)w;}
            if(cc==0){s->pc=base+(uint32_t)off;continue;}
            if(cc!=6 && cc!=7)return fail(c,KPAINT_UNSUPPORTED);
            if(!s->zero)return fail(c,KPAINT_DATAFLOW);
            if(!branch(c,s,base+(uint32_t)off,cc==7?s->zero:xnot(c,s->zero)))return 0;
            continue;
        }
        if((op&0xffc0)==0x4e80 || (op&0xffc0)==0x4ec0)
        {
            int result;uint32_t target;
            mode=(op>>3)&7;reg=op&7;
            if(mode<2 || mode==3 || mode==4 || (mode==7 && reg>3))return fail(c,KPAINT_UNSUPPORTED);
            if(!ea(c,s,(op>>3)&7,op&7,4,1,&a) || a.address.kind!=V_SCALAR)return fail(c,KPAINT_DATAFLOW);
            target=a.address.number;
            result=known_call(c,s,target);
            if(result>=0)
            {if(!result)return 0;if((op&0xffc0)==0x4ec0)return fail(c,KPAINT_UNSUPPORTED);continue;}
            if((target<c->source->exec_base && c->source->exec_base-target<=3072) ||
               (target<c->source->graphics_base && c->source->graphics_base-target<=3072))
                return fail(c,KPAINT_UNSUPPORTED);
            /* Internal static guest calls remain compile-time control flow.
               No native or emulated runtime callback is emitted. */
            if((op&0xffc0)==0x4e80)
            {
                if(s->calls==KPAINT_MAX_CALLS)return fail(c,KPAINT_LIMIT);
                s->reg[15]=add_pointer(c,s->reg[15],-4);
                if(!address(c,s->reg[15],&a.address) ||
                   !memory_write(c,s,a.address,4,value(c,V_RETURN,0,0,s->pc),0))return 0;
                ++s->calls;
            }
            if(target&1 || !span(c->source,target,2))return fail(c,KPAINT_BOUNDS);
            s->pc=target;continue;
        }
        if(op==0x4e75)
        {
            if(!address(c,s->reg[15],&a.address))return 0;
            from=memory_read(c,s,a.address,4);v=vp(c,from);
            if(!v || v->kind!=V_RETURN)return fail(c,KPAINT_STACK);
            s->reg[15]=add_pointer(c,s->reg[15],4);
            if(!v->number)
            {
                v=vp(c,s->reg[15]);
                if(!v || v->kind!=V_STACK || v->offset!=KPAINT_STACK_BYTES || s->calls)return fail(c,KPAINT_STACK);
                return finish_path(c,s);
            }
            if(!s->calls)return fail(c,KPAINT_STACK);
            --s->calls;s->pc=v->number;continue;
        }
        if(op==0x4e71)continue;
        return fail(c,KPAINT_UNSUPPORTED);
    }
}

/* Propagate the bits which may affect externally visible recipe effects.
 * This also rejects unmodelled initial registers/stack bytes rather than
 * supplying a made-up value. DAG edges always point to earlier nodes. */
static int analyse_uses(struct Compiler *c)
{
    unsigned i,j,k;uint32_t bits;
    for(i=0;i<c->plan.paths;++i)
    {
        const struct KaffeinePaintPath *p=&c->plan.path[i];
        if(!p->guard)return fail(c,KPAINT_DATAFLOW);
        c->used[p->guard-1]|=1;
        for(j=0;j<p->count;++j)
        {
            const struct KaffeinePaintAction *a=&p->commands[j];
            unsigned n=a->kind==KPAINT_RECTFILL?4:a->kind==KPAINT_DEFER_SIGNAL?2:a->kind==KPAINT_CLONE?0:1;
            bits=a->kind==KPAINT_RECTFILL?0xffff:a->kind==KPAINT_DEFER_SIGNAL?UINT32_MAX:0xff;
            for(k=0;k<n;++k)
            {if(!a->value[k])return fail(c,KPAINT_DATAFLOW);
                if(a->kind!=KPAINT_DEFER_SIGNAL)
                {
                    unsigned cell;
                    for(cell=0;cell<c->plan.cells;++cell)
                        if((c->plan.cell[cell].role&KPAINT_CELL_OWNER_CAP) &&
                           (c->cell_mask[a->value[k]-1]&((uint32_t)1<<cell)))return fail(c,KPAINT_SIGNAL);
                }
                c->used[a->value[k]-1]|=bits;}
        }
    }
    for(i=c->plan.nodes;i>0;--i)
    {
        const struct KaffeinePaintExpr *x=&c->plan.expressions[i-1];
        uint32_t need=c->used[i-1]&mask(x->width);
        if(!need)continue;
        switch(x->op)
        {
        case X_UNKNOWN:return fail(c,KPAINT_DATAFLOW);
        case X_CONST:case X_BOUND:case X_OFFSET:break;
        case X_LAYER:return fail(c,KPAINT_DATAFLOW);
        case X_RP:
            /* Opaque pointer/private bytes prove clone identity only. Their
             * numeric values are never inputs to a paint/branch expression. */
            if(x->value<24 || (x->value>=52 && x->value<56) || x->value>=66)
                return fail(c,KPAINT_DATAFLOW);
            c->plan.rp_used[x->value]|=(uint8_t)need;break;
        case X_CELL:c->plan.cell[x->value].used_mask|=need;break;
        case X_JOIN:c->used[x->a-1]|=need>>x->shift;c->used[x->b-1]|=need&mask(x->shift);break;
        case X_EXTRACT:c->used[x->a-1]|=need<<x->shift;break;
        case X_SIGN:
            c->used[x->a-1]|=need&mask(x->shift);
            if(need&~mask(x->shift))c->used[x->a-1]|=(uint32_t)1<<(x->shift-1);
            break;
        case X_ZERO:c->used[x->a-1]|=mask(x->shift);break;
        case X_NOT:c->used[x->a-1]|=1;break;
        case X_AND:c->used[x->a-1]|=1;c->used[x->b-1]|=1;break;
        case X_BITAND:
            if(constant(c,x->b,&bits))c->used[x->a-1]|=need&bits;
            else {c->used[x->a-1]|=need;c->used[x->b-1]|=need;}
            break;
        default:return fail(c,KPAINT_DATAFLOW);
        }
    }
    for(i=0;i<c->plan.cells;++i)
        if(c->plan.cell[i].used_mask && !(c->plan.cell[i].used_mask&~(uint32_t)0xff))
            c->plan.cell[i].role|=KPAINT_CELL_BYTE;
    return 1;
}
size_t Kaffeine_PaintCompileWorkspaceSize(void) { return sizeof(struct Compiler); }
size_t Kaffeine_PaintEvaluateWorkspaceSize(void) { return sizeof(struct Eval); }

size_t Kaffeine_PaintWorkspaceAlignment(void)
{
    struct CAlignment { char c; struct Compiler value; };
    struct EAlignment { char c; struct Eval value; };
    size_t a=offsetof(struct CAlignment,value),b=offsetof(struct EAlignment,value);
    return a>b?a:b;
}
static int aligned_workspace(const void *p,size_t n,size_t need)
{ return p && !((uintptr_t)p%Kaffeine_PaintWorkspaceAlignment()) && n>=need; }

enum KaffeinePaintStatus Kaffeine_PaintCompile(
    const struct KaffeinePaintSource *source,void *workspace,size_t workspace_size,
    struct KaffeinePaintPlan *out,struct KaffeinePaintReport *report)
{
    struct Compiler *c;unsigned i;uint32_t header[5];struct Address sp;
    enum KaffeinePaintStatus error=KPAINT_INPUT;
    if(!source || !source->read || !source->ordinary || !out ||
       !source->hook || (source->hook&1) || !source->exec_base ||
       !source->graphics_base || source->exec_base==source->graphics_base ||
       !span(source,source->hook,20))goto early;
    error=KPAINT_WORKSPACE;
    if(!aligned_workspace(workspace,workspace_size,sizeof(*c)))goto early;
    c=workspace;memset(c,0,sizeof(*c));c->source=source;c->plan.version=KPAINT_VERSION;
    for(i=0;i<5;++i)
        if(!read_number(c,source->hook+4*i,4,&header[i]) ||
           !dependency(c,source->hook+4*i,4,KPAINT_HEADER,header[i]))goto done;
    if(!header[2] || (header[2]&1) || !span(source,header[2],2))
    {fail(c,KPAINT_BOUNDS);goto done;}
    for(i=0;i<16;++i)c->state.reg[i]=scalar(c,node(c,X_UNKNOWN,0,0,32,0,i+1));
    c->state.reg[8]=value(c,V_HOOK,0,0,0);
    c->state.reg[9]=value(c,V_BF,0,0,0);
    c->state.reg[10]=value(c,V_RP,0,0,0);
    c->state.reg[15]=value(c,V_STACK,0,KPAINT_STACK_BYTES-4,0);
    c->state.zero=node(c,X_UNKNOWN,0,0,1,0,100);
    c->state.guard=xc(c,1,1);c->state.pc=header[2];
    sp=(struct Address){0,KPAINT_STACK_BYTES-4,V_STACK};
    if(!memory_write(c,&c->state,sp,4,value(c,V_RETURN,0,0,0),0))goto done;
    for(;;)
    {
        if(!execute_symbolic(c,&c->state))
        {if(!c->error)fail(c,KPAINT_DATAFLOW);goto done;}
        if(!c->pending_count)break;
        c->state=c->pending[--c->pending_count];
    }
    if(!c->plan.paths){fail(c,KPAINT_DATAFLOW);goto done;}
    if(!analyse_uses(c))goto done;
    memmove(out,&c->plan,sizeof(*out));
done:
    if(report)*report=(struct KaffeinePaintReport){c->error,c->pc,c->steps};
    return c->error;
early:
    if(report)*report=(struct KaffeinePaintReport){error,0,0};
    return error;
}

static int valid_plan(const struct KaffeinePaintPlan *p)
{
    unsigned i,j;
    if(!p || p->version!=KPAINT_VERSION || !p->nodes || p->nodes>KPAINT_MAX_NODES ||
       p->dependencies>KPAINT_MAX_DEPS || p->cells>KPAINT_MAX_CELLS ||
       !p->paths || p->paths>KPAINT_MAX_PATHS)return 0;
    for(i=0;i<p->nodes;++i)
    {
        const struct KaffeinePaintExpr *x=&p->expressions[i];
        if(!x->width || x->width>32 || x->a>i || x->b>i)return 0;
        switch(x->op)
        {
        case X_CONST:case X_UNKNOWN:case X_LAYER:if(x->a || x->b)return 0;break;
        case X_RP:if(x->a || x->b || x->width!=8 || x->value>=100)return 0;break;
        case X_BOUND:if(x->a || x->b || x->width!=16 || x->value>=4)return 0;break;
        case X_OFFSET:if(x->a || x->b || x->width!=32 || x->value>=2)return 0;break;
        case X_CELL:if(x->a || x->b || x->value>=p->cells || x->width!=8*p->cell[x->value].width)return 0;break;
        case X_JOIN:if(!x->a || !x->b || !x->shift || x->shift>=32 || x->width<=x->shift)return 0;break;
        case X_EXTRACT:if(!x->a || x->b || x->shift>32-x->width)return 0;break;
        case X_SIGN:if(!x->a || x->b || !x->shift || x->shift>32 || x->width!=32)return 0;break;
        case X_ZERO:if(!x->a || x->b || !x->shift || x->shift>32 || x->width!=1)return 0;break;
        case X_NOT:if(!x->a || x->b || x->width!=1)return 0;break;
        case X_AND:if(!x->a || !x->b || x->width!=1)return 0;break;
        case X_BITAND:if(!x->a || !x->b)return 0;break;
        default:return 0;
        }
    }
    for(i=0;i<p->dependencies;++i)
        if((p->dependency[i].width!=1 && p->dependency[i].width!=2 && p->dependency[i].width!=4) ||
           p->dependency[i].kind<KPAINT_CODE || p->dependency[i].kind>KPAINT_IMMUTABLE_DATA)return 0;
    for(i=0;i<p->cells;++i)
        if(p->cell[i].width!=1 && p->cell[i].width!=2 && p->cell[i].width!=4)return 0;
    for(i=0;i<p->paths;++i)
    {
        const struct KaffeinePaintPath *path=&p->path[i];
        if(!path->guard || path->guard>p->nodes || path->count>KPAINT_MAX_COMMANDS)return 0;
        for(j=0;j<path->count;++j)
        {
            const struct KaffeinePaintAction *a=&path->commands[j];unsigned k,n;
            if(a->kind<KPAINT_CLONE || a->kind>KPAINT_DEFER_SIGNAL || a->clone>=KPAINT_MAX_CLONES)return 0;
            n=a->kind==KPAINT_CLONE?0:a->kind==KPAINT_RECTFILL?4:a->kind==KPAINT_DEFER_SIGNAL?2:1;
            for(k=0;k<n;++k)if(!a->value[k] || a->value[k]>p->nodes)return 0;
        }
    }
    return 1;
}

enum KaffeinePaintStatus Kaffeine_PaintEvaluate(
    const struct KaffeinePaintPlan *p,const struct KaffeinePaintInputs *in,
    void *workspace,size_t workspace_size,struct KaffeinePaintOutput *out)
{
    struct Eval *e;unsigned i,j,selected=KPAINT_MAX_PATHS;uint8_t clones[KPAINT_MAX_CLONES]={0};
    if(!in || !out || !valid_plan(p))return KPAINT_INPUT;
    if(!aligned_workspace(workspace,workspace_size,sizeof(*e)))return KPAINT_WORKSPACE;
    for(i=0;i<p->dependencies;++i)
        if((in->dependencies[i]&mask(p->dependency[i].width*8))!=p->dependency[i].expected)return KPAINT_CHANGED;
    for(i=0;i<p->cells;++i)
        if((p->cell[i].role&KPAINT_CELL_IMMUTABLE) &&
           (in->cells[i]&mask(p->cell[i].width*8))!=p->cell[i].initial)return KPAINT_CHANGED;
    for(i=0;i<p->cells;++i)
        if((p->cell[i].role&KPAINT_CELL_OWNER_CAP) && in->cells[i] && in->cells[i]!=in->owner_task)return KPAINT_SIGNAL;
    if(in->bounds[0]>in->bounds[2] || in->bounds[1]>in->bounds[3])return KPAINT_RECT;
    e=workspace;memset(e,0,sizeof(*e));
    for(i=0;i<p->nodes;++i)
    {
        const struct KaffeinePaintExpr *x=&p->expressions[i];
        uint32_t a=x->a?e->value[x->a-1]:0,b=x->b?e->value[x->b-1]:0,v=0;
        switch(x->op)
        {
        case X_CONST:v=x->value;break;
        case X_UNKNOWN:break; /* Proven unused by the driver-side compiler. */
        case X_RP:v=in->rp[x->value];break;
        case X_BOUND:v=(uint16_t)in->bounds[x->value];break;
        case X_OFFSET:v=x->value?(uint32_t)in->offset_y:(uint32_t)in->offset_x;break;
        case X_LAYER:v=in->layer_token;break;
        case X_CELL:v=in->cells[x->value];break;
        case X_JOIN:v=(a<<x->shift)|(b&mask(x->shift));break;
        case X_EXTRACT:v=a>>x->shift;break;
        case X_SIGN:v=sign_value(a,x->shift);break;
        case X_ZERO:v=!(a&mask(x->shift));break;
        case X_NOT:v=!a;break;
        case X_AND:v=!!a && !!b;break;
        case X_BITAND:v=a&b;break;
        default:return KPAINT_INPUT;
        }
        e->value[i]=v&mask(x->width);
    }
    for(i=0;i<p->paths;++i)if(e->value[p->path[i].guard-1])
    {if(selected!=KPAINT_MAX_PATHS)return KPAINT_DATAFLOW;selected=i;}
    if(selected==KPAINT_MAX_PATHS)return KPAINT_DATAFLOW;
    for(i=0;i<p->path[selected].count;++i)
    {
        const struct KaffeinePaintAction *a=&p->path[selected].commands[i];
        struct KaffeinePaintCommand command={{0},0,0,0,{0}};
        unsigned n=a->kind==KPAINT_CLONE?0:a->kind==KPAINT_RECTFILL?4:a->kind==KPAINT_DEFER_SIGNAL?2:1;
        command.kind=a->kind;command.clone=a->clone;
        for(j=0;j<n;++j)command.value[j]=e->value[a->value[j]-1];
        if(a->kind==KPAINT_CLONE)clones[a->clone]=1;
        else if(a->kind==KPAINT_DEFER_SIGNAL)
        {
            if(!in->owner_task || !in->owner_cookie || command.value[0]!=in->owner_task ||
               (command.value[1]&~in->allowed_signals) || (command.value[1]&~p->signal_mask))return KPAINT_SIGNAL;
            command.owner_cookie=in->owner_cookie;
        }
        else
        {
            if(!clones[a->clone])return KPAINT_BAD_CLONE;
            if(a->kind==KPAINT_RECTFILL)
            {
                int32_t xy[4];for(j=0;j<4;++j)xy[j]=(int16_t)command.value[j];
                for(j=0;j<4;++j)if(xy[j]<in->bounds[j%2] || xy[j]>in->bounds[2+j%2])return KPAINT_RECT;
                /* Current AROS RectFill is a no-op for inverted endpoints. */
                if(xy[0]>xy[2] || xy[1]>xy[3])continue;
            }
            else if(command.value[0]>255)return KPAINT_DATAFLOW;
        }
        e->out.commands[e->out.count++]=command;
    }
    memmove(out,&e->out,sizeof(*out));return KPAINT_OK;
}
