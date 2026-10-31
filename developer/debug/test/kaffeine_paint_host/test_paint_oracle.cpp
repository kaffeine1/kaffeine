/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Author: Michele Dipace (@kaffeine1)
 * Independent instruction execution: pinned project Moira, original programs.
 * Known bridge void calls are intercepted at their typed fake vector boundary. */
#include "Moira.h"
#define main compiler_tests_main
#include "test_paint_compile.c"
#undef main
#include <array>
#include <vector>
#include <cstring>

using Bytes=std::array<unsigned char,100>;
struct Fill { Bytes rp;std::array<int16_t,4> bounds; };
struct Oracle:moira::Moira {
    Fixture &f;mutable bool bad=false;
    explicit Oracle(Fixture &fixture):f(fixture){}
    moira::u8 read8(moira::u32 a)const override
    {if(a>=ARENA){bad=true;return 0;}return f.arena[a];}
    moira::u16 read16(moira::u32 a)const override
    {if(a>=ARENA-1 || (a&1)){bad=true;return 0;}return (moira::u16)get(&f,a,2);}
    void write8(moira::u32 a,moira::u8 n)const override
    {if(a>=ARENA){bad=true;return;}f.arena[a]=n;}
    void write16(moira::u32 a,moira::u16 n)const override
    {if(a>=ARENA-1 || (a&1)){bad=true;return;}put16(&f,a,n);}
    void jump(unsigned pc)
    {setPC(pc);setPC0(pc);setIRD(read16(pc));setIRC(read16(pc+2));}
};
/* A small classic-byte collaborator of the PUBLIC native setter definitions,
 * deliberately separate from the compiler expression/stack implementation. */
static void set_pen(Bytes &rp,unsigned pen)
{rp[25]=(unsigned char)pen;rp[30]=15;rp[32]&=0xbf;}
static void set_mode(Bytes &rp,unsigned mode)
{
    rp[28]=(unsigned char)mode;rp[30]=15;
    for(unsigned i=0;i<8;++i)
    {
        unsigned mt;
        if(mode&2)mt=(mode&4)?0x6a:0x9a;
        else {unsigned a=((rp[25]>>i)&1)*3,b=(mode&1)?((rp[26]>>i)&1)*3:2;
            mt=(((mode&4)?(b<<2)|a:(a<<2)|b)<<4)|0x0a;}
        rp[40+i]=(unsigned char)mt;
    }
}
static std::vector<Fill> run_moira(Fixture &f,std::vector<unsigned> &signals)
{
    constexpr unsigned rp_addr=0x11000,bf_addr=0x11100,stack=0x1f000,stop=0xe000;
    Oracle cpu(f);std::vector<Fill> fills;
    std::memcpy(f.arena+rp_addr,f.input.rp,100);put32(&f,bf_addr,f.input.layer_token);
    for(unsigned i=0;i<4;++i)put16(&f,bf_addr+4+2*i,(uint16_t)f.input.bounds[i]);
    put32(&f,bf_addr+12,(uint32_t)f.input.offset_x);put32(&f,bf_addr+16,(uint32_t)f.input.offset_y);
    for(unsigned i=0;i<8;++i){cpu.setD((int)i,0xabcd3000u+17*i);cpu.setA((int)i,0x77770000u+19*i);}
    cpu.setSR(0x2000);cpu.setA(0,HOOK);cpu.setA(1,bf_addr);cpu.setA(2,rp_addr);
    cpu.setA(7,stack-4);put32(&f,stack-4,stop);cpu.jump(get(&f,HOOK+8,4));
    unsigned steps=0;
    while(cpu.getPC()!=stop && !cpu.bad && ++steps<10000)
    {
        unsigned pc=cpu.getPC(),lvo=pc<GFX?GFX-pc:0;
        bool gfx=lvo==342 || lvo==354 || lvo==306;
        if(gfx || pc==EXEC-324)
        {
            if(gfx)
            {
                unsigned at=cpu.getA(1);CHECK(at>=stack-512 && at+100<=stack);
                if(at>=ARENA-100){cpu.bad=true;break;}
                Bytes bytes;std::memcpy(bytes.data(),f.arena+at,100);
                if(lvo==342)set_pen(bytes,cpu.getD(0));
                else if(lvo==354)set_mode(bytes,cpu.getD(0));
                else {
                    Fill fill;fill.rp=bytes;
                    for(unsigned i=0;i<4;++i)fill.bounds[i]=(int16_t)cpu.getD((int)i);
                    if(fill.bounds[0]<=fill.bounds[2] && fill.bounds[1]<=fill.bounds[3])fills.push_back(fill);
                }
                std::memcpy(f.arena+at,bytes.data(),100);
            }
            else {CHECK(cpu.getA(1)==OWNER);signals.push_back(cpu.getD(0));}
            cpu.setD(0,0);unsigned sp=cpu.getA(7),ret=get(&f,sp,4);cpu.setA(7,sp+4);cpu.jump(ret);
        }
        else {
            unsigned op=get(&f,pc,2),sr=cpu.getSR();cpu.execute();
            if((op&0xf0c0)==0x90c0 || (op&0xf0c0)==0xd0c0)
                CHECK(cpu.getSR()==sr); /* Every CCR bit, independently. */
        }
    }
    CHECK(!cpu.bad && cpu.getPC()==stop && steps<10000);
    CHECK(cpu.getA(7)==stack);
    for(unsigned i=0;i<8;++i)CHECK(cpu.getD((int)i)==0xabcd3000u+17*i);
    CHECK(cpu.getA(0)==HOOK && cpu.getA(1)==bf_addr && cpu.getA(2)==rp_addr);
    return fills;
}
static void compare(Fixture &f,const std::vector<Fill> &reference,const std::vector<unsigned> &signals)
{
    Bytes clone[KPAINT_MAX_CLONES];std::vector<Fill> actual;std::vector<unsigned> deferred;
    for(unsigned i=0;i<f.output.count;++i)
    {
        const auto &command=f.output.commands[i];auto &rp=clone[command.clone];
        switch(command.kind)
        {
        case KPAINT_CLONE:std::memcpy(rp.data(),f.input.rp,100);std::memset(rp.data(),0,4);break;
        case KPAINT_APEN:set_pen(rp,command.value[0]);break;
        case KPAINT_DRAWMODE:set_mode(rp,command.value[0]);break;
        case KPAINT_MASK:rp[24]=(unsigned char)command.value[0];break;
        case KPAINT_RECTFILL:{Fill fill;fill.rp=rp;for(unsigned k=0;k<4;++k)fill.bounds[k]=(int16_t)command.value[k];actual.push_back(fill);break;}
        case KPAINT_DEFER_SIGNAL:CHECK(command.value[0]==OWNER);deferred.push_back(command.value[1]);break;
        default:CHECK(false);break;
        }
    }
    CHECK(actual.size()==reference.size());CHECK(deferred==signals);
    for(unsigned i=0;i<actual.size() && i<reference.size();++i)
    {CHECK(!std::memcmp(actual[i].rp.data(),reference[i].rp.data(),100));CHECK(actual[i].bounds==reference[i].bounds);}
}
int main()
{
    Fixture f;
    for(unsigned source=3;source<=6;++source)for(unsigned dest=3;dest<=6;++dest)
        if(source!=dest && source!=5 && dest!=5)for(unsigned words=0;words<2;++words)
    {
        init(&f);prologue(&f,source,dest,words?3:7,!words);paint(&f,1,1);signal_tail(&f);epilogue(&f);
        CHECK(compile(&f)==KPAINT_OK);snapshot(&f);CHECK(evaluate(&f)==KPAINT_OK);
        std::vector<unsigned> signals;auto fills=run_moira(f,signals);compare(f,fills,signals);dispose(&f);
    }
    for(unsigned sub=0;sub<2;++sub)for(unsigned size=2;size<=4;size+=2)for(unsigned reg=0;reg<2;++reg)
    {
        init(&f);adjusted_prologue(&f,(int)sub,size,(int)reg);paint(&f,1,1);signal_tail(&f);
        adjusted_epilogue(&f,(int)sub,size,(int)reg);
        CHECK(compile(&f)==KPAINT_OK);snapshot(&f);CHECK(evaluate(&f)==KPAINT_OK);
        std::vector<unsigned> signals;auto fills=run_moira(f,signals);compare(f,fills,signals);dispose(&f);
    }
    /* Zero owner selects the statically proven no-Signal path. */
    init(&f);prologue(&f,3,4,7,1);paint(&f,1,0);signal_tail(&f);epilogue(&f);put32(&f,DATA+4,0);
    CHECK(compile(&f)==KPAINT_OK);snapshot(&f);CHECK(evaluate(&f)==KPAINT_OK);
    std::vector<unsigned> signals;auto fills=run_moira(f,signals);compare(f,fills,signals);CHECK(signals.empty());dispose(&f);
    std::printf("paint Moira oracle: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
