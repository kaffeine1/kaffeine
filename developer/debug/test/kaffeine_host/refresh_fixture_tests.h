/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original refresh cases through the production dispatcher/layer bridge and
   unchanged public native Intuition bodies. Native layer primitives record
   reference counts, update state, contention and OOM; no GUI proof is implied. */
#pragma once

static void test_refresh_scopes()
{
    for(bool gzz:{false,true})for(bool oom:{false,true})for(bool complete:{false,true})
    {
        Fixture f;NativeWindow a;Layer border={};RastPort border_rp={};
        make_window(a,scr,10,20,200,100);a.layer.Flags|=LAYERREFRESH;
        if(gzz)
        {
            make_layer(border,10,20,200,100,LAYERSIMPLE);border.Window=&a.window;
            border_rp.Layer=&border;a.window.BorderRPort=&border_rp;a.window.Flags|=WFLG_GIMMEZEROZERO;
            order(scr,{&bar,&a.layer,&border,&foreign});
        }
        else order(scr,{&bar,&a.layer,&foreign});
        ULONG mw=f.open(a),layer=f.L(mw+M68K_WIN_WLAYER),info=f.L(layer+M68K_LAYER_LAYERINFO);
        ULONG rp=f.L(mw+M68K_WIN_RPORT),flags=f.L(mw+M68K_WIN_FLAGS)^0x40000100;
        m68k_write32(&f.ctx,mw+M68K_WIN_FLAGS,flags);
        m68k_write16(&f.ctx,mw+M68K_WIN_LEFTEDGE,0x7777);f.memory[rp+25]=13;
        auto native_left=a.window.LeftEdge;auto native_pen=a.rp.FgPen;
        assert(f.call(Fixture::layers_base,120,{info})==0);
        assert(f.call(Fixture::layers_base,120,{info})==0);
        update_fails=oom;
        assert(f.call(Fixture::intuition_base,354,{mw})==0 && f.ctx.running);
        assert(refresh_begins==1 && update_begins==1 && !refresh_ends && !update_ends);
        assert(info_depth==3 && held_locks==(oom?1:2)+(gzz?1:0) && Kaffeine_LayerInfoLocked(&f.ctx));
        assert(((a.window.Flags&WFLG_WINDOWREFRESH)!=0)==!oom);
        assert((f.L(mw+M68K_WIN_FLAGS)&~WFLG_WINDOWREFRESH)==(flags&~WFLG_WINDOWREFRESH));
        assert(f.W(mw+M68K_WIN_LEFTEDGE)==0x7777 && f.memory[rp+25]==13 &&
               a.window.LeftEdge==native_left && a.rp.FgPen==native_pen);
        auto before=f.heap();auto calls=info_calls;Kaffeine_LayersRefresh(&f.ctx);
        assert(info_calls==calls && f.heap_is(before));
        // A pure hook swap on the already-held layer does not restart update.
        ULONG hook=f.call(Fixture::layers_base,198,{layer,1});
        assert(f.ctx.running && update_begins==1 && update_ends==0);
        assert(f.call(Fixture::layers_base,198,{layer,hook})==1 && f.ctx.running);
        assert(f.call(Fixture::layers_base,96,{0,layer})==0);
        assert(f.call(Fixture::layers_base,102,{layer})==0);
        // Forging the guest flags cannot suppress or invent native EndUpdate.
        m68k_write32(&f.ctx,mw+M68K_WIN_FLAGS,f.L(mw+M68K_WIN_FLAGS)^WFLG_WINDOWREFRESH);
        assert(f.call(Fixture::intuition_base,366,{mw},{complete?1u:0u})==0 && f.ctx.running);
        assert(refresh_ends==1 && refresh_complete.back()==complete && update_ends==!oom &&
               !held_locks && info_depth==2 && !(a.window.Flags&WFLG_WINDOWREFRESH));
        assert(((a.layer.Flags&LAYERREFRESH)==0)==complete);
        assert((f.L(mw+M68K_WIN_FLAGS)&~WFLG_WINDOWREFRESH)==(flags&~WFLG_WINDOWREFRESH) &&
               f.W(mw+M68K_WIN_LEFTEDGE)==0x7777 && f.memory[rp+25]==13);
        assert(f.call(Fixture::layers_base,138,{info})==0);
        assert(f.call(Fixture::layers_base,138,{info})==0 && !info_depth && !Kaffeine_LayerInfoLocked(&f.ctx));
        f.close(mw);
    }
}

static void test_refresh_aliases_and_gates()
{
    const M68KThunkEntry aliases[]={KAFFEINE_GADTOOLS_LAYER_THUNKS};
    assert(aliases[0].lvo==90 && aliases[0].thunk==Kaffeine_BeginRefresh &&
           aliases[1].lvo==96 && aliases[1].thunk==Kaffeine_EndRefresh);
    for(ULONG lvo:{78u,84u})
    {
        bool found=false;
        for(ULONG i=0;i<kaffeine_thunks_layers_manual_count;++i)
            if(kaffeine_thunks_layers_manual[i].lvo==lvo)
                found=kaffeine_thunks_layers_manual[i].thunk==kaffeine_layers_Unsupported;
        assert(found); // real manual selection prevents native Begin/EndUpdate
    }
    for(bool oom:{false,true})
    {
        Fixture f;NativeWindow a;make_window(a,scr,10,20,200,100);order(scr,{&bar,&a.layer,&foreign});
        ULONG mw=f.open(a),layer=f.L(mw+M68K_WIN_WLAYER);
        auto saved=f.ctx.libs[1];std::strcpy(f.ctx.libs[1].name,"gadtools.library");
        f.ctx.libs[1].thunks=aliases;f.ctx.libs[1].num_thunks=2;update_fails=oom;
        assert(f.call(Fixture::intuition_base,90,{mw})==0 && info_depth==1 && refresh_begins==1);
        assert(Kaffeine_LayerInfoLocked(&f.ctx)); // also when both native update flags are clear
        if(oom)assert(!(a.window.Flags&WFLG_WINDOWREFRESH) && !(a.layer.Flags&LAYERUPDATING));
        auto calls=info_calls;
        for(ULONG lvo:{78u,84u,174u})
            assert(f.call(Fixture::layers_base,lvo,{layer})==20 && f.stopped() &&
                   update_begins==1 && !update_ends && info_depth==1);
        assert(f.call(Fixture::exec_base,318)==20 && f.stopped());
        assert(f.call(Fixture::graphics_base,270)==20 && f.stopped());
        // Only the exact manual refresh vectors, never other Intuition APIs.
        f.ctx.libs[1]=saved;
        assert(f.call(Fixture::intuition_base,312,{mw})==20 && f.stopped() && depth_calls.empty());
        assert(f.call(Fixture::intuition_base,816,{mw})==20 && f.stopped());
        auto graphics=f.ctx.libs[3];f.ctx.libs[3].dos=TRUE;
        assert(f.call(Fixture::graphics_base,42)==20 && f.stopped());f.ctx.libs[3]=graphics;
        KaffeineGuestCall call={};f.ctx.cpu=&f.cpu;f.ctx.running=TRUE;
        assert(!Kaffeine_CallGuest(&f.ctx,&f.cpu,0x5000,&call) && f.ctx.callback_failed);
        f.ctx.cpu=nullptr;assert(info_depth==1 && info_calls==calls && !refresh_ends);
        std::strcpy(f.ctx.libs[1].name,"gadtools.library");
        f.ctx.libs[1].thunks=aliases;f.ctx.libs[1].num_thunks=2;
        assert(f.call(Fixture::intuition_base,96,{mw},{1})==0 && !info_depth && !held_locks &&
               refresh_ends==1 && update_ends==!oom);
        f.ctx.libs[1]=saved;f.close(mw);
    }
}

static void test_refresh_refusals()
{
    for(unsigned mode=0;mode<12;++mode)
    {
        Fixture f;NativeWindow a;Layer border={};RastPort brp={};
        make_window(a,scr,10,20,200,100);order(scr,{&bar,&a.layer,&foreign});
        ULONG mw=f.open(a),layer=f.L(mw+M68K_WIN_WLAYER),arg=mw;
        if(mode==0)arg=0x5555;
        if(mode==1)f.ctx.callback_depth=1;
        if(mode==2)f.ctx.broker_call=TRUE;
        if(mode==3)native_task=reinterpret_cast<Task*>(static_cast<IPTR>(0x22220000));
        if(mode==4)m68k_write32(&f.ctx,Fixture::exec_base+M68K_EXECBASE_THISTASK,Fixture::task+4);
        if(mode==5)info_busy=true;
        if(mode==6)attempt_busy_layer=&a.layer;
        if(mode==7)a.window.Flags|=WFLG_WINDOWREFRESH;
        if(mode==8)a.layer.Flags|=LAYERUPDATING;
        if(mode==9)assert(f.call(Fixture::layers_base,96,{0,layer})==0);
        if(mode==10)f.ctx.bitmap_blits=1;
        if(mode==11)
        {
            make_layer(border,0,0,200,100,LAYERSIMPLE);border.LayerInfo=&scr.LayerInfo;
            brp.Layer=&border;a.window.BorderRPort=&brp;a.window.Flags|=WFLG_GIMMEZEROZERO;
            attempt_busy_layer=&a.layer; // border attempt succeeds, main attempt rolls it back
        }
        assert(f.call(Fixture::intuition_base,354,{arg})==20 && f.stopped() && !refresh_begins &&
               !refresh_ends && !update_begins && !info_depth && held_locks==(mode==9?1:0));
        f.ctx.callback_depth=0;f.ctx.broker_call=FALSE;native_task=f.ctx.exec_task;info_busy=false;
        f.ctx.bitmap_blits=0;attempt_busy_layer=nullptr;
        m68k_write32(&f.ctx,Fixture::exec_base+M68K_EXECBASE_THISTASK,Fixture::task);
        a.window.Flags&=~(WFLG_WINDOWREFRESH|WFLG_GIMMEZEROZERO);a.layer.Flags&=~LAYERUPDATING;
        if(mode==9)assert(f.call(Fixture::layers_base,102,{layer})==0);
        f.close(mw);
    }
    for(bool oom:{false,true})
    {
        Fixture f;NativeWindow a,b;make_window(a,scr,10,20,200,100);make_window(b,scr,30,40,200,100);
        order(scr,{&bar,&a.layer,&b.layer,&foreign});ULONG ma=f.open(a),mb=f.open(b);
        ULONG la=f.L(ma+M68K_WIN_WLAYER),lb=f.L(mb+M68K_WIN_WLAYER),info=f.L(la+M68K_LAYER_LAYERINFO);
        update_fails=oom;assert(f.call(Fixture::intuition_base,354,{ma})==0);auto calls=info_calls;
        for(ULONG mw:{ma,mb})assert(f.call(Fixture::intuition_base,354,{mw})==20 && f.stopped());
        assert(f.call(Fixture::intuition_base,366,{mb},{1})==20 && f.stopped());
        assert(f.call(Fixture::layers_base,120,{info})==20 && f.stopped());
        assert(f.call(Fixture::layers_base,138,{info})==20 && f.stopped());
        assert(f.call(Fixture::layers_base,96,{0,lb})==20 && f.stopped());
        assert(f.call(Fixture::layers_base,198,{lb,1})==20 && f.stopped());
        f.ctx.callback_depth=1;assert(f.call(Fixture::intuition_base,366,{ma})==20 && f.stopped());f.ctx.callback_depth=0;
        native_task=reinterpret_cast<Task*>(static_cast<IPTR>(0x22220000));
        assert(f.call(Fixture::intuition_base,366,{ma})==20 && f.stopped());
        f.ctx.layer_ops->release_locks(&f.ctx);assert(info_depth==1 && !refresh_ends);
        native_task=f.ctx.exec_task;
        assert(info_calls==calls && refresh_begins==1 && !refresh_ends);
        assert(f.call(Fixture::layers_base,96,{0,la})==0);
        assert(f.call(Fixture::intuition_base,366,{ma})==20 && f.stopped() && !refresh_ends);
        assert(f.call(Fixture::layers_base,102,{la})==0);
        assert(f.call(Fixture::intuition_base,366,{ma},{1})==0 && !held_locks && !info_depth);
        f.close(ma);f.close(mb);
    }
}

static void test_refresh_lifetime()
{
    for(bool oom:{false,true})for(unsigned mode=0;mode<4;++mode)
    {
        Fixture f;NativeWindow a;make_window(a,scr,10,20,200,100);order(scr,{&bar,&a.layer,&foreign});
        ULONG mw=f.open(a),la=f.L(mw+M68K_WIN_WLAYER),info=f.L(la+M68K_LAYER_LAYERINFO);
        assert(f.call(Fixture::layers_base,120,{info})==0);
        assert(f.call(Fixture::layers_base,120,{info})==0);update_fails=oom;
        assert(f.call(Fixture::intuition_base,354,{mw})==0);
        assert(f.call(Fixture::layers_base,96,{0,la})==0);events.clear();
        f.ctx.layer_ops->task_removed(&f.ctx,Fixture::task+4);
        assert(info_depth==3 && !refresh_ends);
        if(mode==0)f.ctx.layer_ops->task_removed(&f.ctx,Fixture::task);
        if(mode==1)Kaffeine_WindowPrepareClose(&f.ctx,mw);
        if(mode==2)f.ctx.layer_ops->screen_closing(&f.ctx,&scr);
        if(mode==3)Kaffeine_LayersCleanup(&f.ctx);
        assert(!held_locks && !info_depth && !Kaffeine_LayerInfoLocked(&f.ctx) && refresh_ends==1 &&
               refresh_complete.back()==FALSE && update_ends==!oom);
        assert(event_at("UnlockLayer")<event_at("EndRefresh") &&
               event_at("EndRefresh")<event_at("UnlockLayerInfo"));
        if(f.ctx.layer_ops)f.ctx.layer_ops->release_locks(&f.ctx);
        assert(refresh_ends==1);f.close(mw);
    }
    for(bool oom:{false,true})
    {
        Fixture f;NativeWindow a;make_window(a,scr,10,20,200,100);order(scr,{&bar,&a.layer,&foreign});
        ULONG mw=f.open(a);update_fails=oom;KaffeineCPUOps run=ops;unsigned drained=0;
        before_completion=[&] {
            assert(f.call(Fixture::intuition_base,354,{mw})==0 && Kaffeine_LayerInfoLocked(&f.ctx));
            Kaffeine_StopGuest(&f.ctx,&f.cpu,20);Kaffeine_SetD(&f.cpu,0,0);
        };
        run.execute=[](void*){before_completion();};f.cpu.ops=&run;
        f.ctx.root_completion_data=&drained;
        f.ctx.domain_drain=[](KaffeineContext *ctx)->BOOL {
            ++*static_cast<unsigned*>(ctx->root_completion_data);
            assert(!Kaffeine_LayerInfoLocked(ctx) && !held_locks && !info_depth);return TRUE;
        };
        assert(Kaffeine_RunCPU(&f.ctx,&f.cpu)==20 && drained && refresh_ends==1 &&
               refresh_complete.back()==FALSE && update_ends==!oom);
        before_completion={};f.cpu.ops=&ops;f.ctx.domain_drain=nullptr;f.ctx.root_completion_data=nullptr;
        f.close(mw);
    }
}

/* Stand-ins for the 28 manual vectors of kaffeine_p96.c, so that only the
   dispatcher decides; each records the native Info depth it ran under. */
static std::vector<int> p96_writes;
static IPTR record_p96(KaffeineContext *ctx, void *)
{
    p96_writes.push_back(Kaffeine_LayerInfoLocked(ctx) ? info_depth : 0);
    return 0;
}

static void test_refresh_p96_write()
{
    static constexpr ULONG p96_base=0x30000,ri=0x5000;
    M68KThunkEntry p96[29]={},no_write[28]={};
    for(unsigned i=0,j=0;i<28;++i)
    {
        p96[i]={30+6*i,record_p96};
        if(p96[i].lvo!=102)no_write[j++]=p96[i];
    }
    const std::vector<M68KThunkEntry> none,generated={{102,poison}};
    for(bool oom:{false,true})
    {
        Fixture f;NativeWindow a;make_window(a,scr,10,20,200,100);a.layer.Flags|=LAYERREFRESH;
        order(scr,{&bar,&a.layer,&foreign});
        ULONG mw=f.open(a),rp=f.L(mw+M68K_WIN_RPORT),la=f.L(mw+M68K_WIN_WLAYER),info=f.L(la+M68K_LAYER_LAYERINFO);
        f.lib(4,p96_base,"Picasso96API.library",p96,28,none);f.ctx.num_libs=5;p96_writes.clear();
        // Outside any scope the dispatcher passes every vector.
        assert(f.call(p96_base,102,{ri,rp})==0 && f.call(p96_base,108,{ri,rp})==0 && f.ctx.running);
        assert(p96_writes==std::vector<int>({0,0}));
        update_fails=oom;
        assert(f.call(Fixture::intuition_base,354,{mw})==0 && f.ctx.running && refresh_begins==1);
        assert(Kaffeine_LayerInfoLocked(&f.ctx) && ((a.window.Flags&WFLG_WINDOWREFRESH)!=0)==!oom);
        auto locks=held_locks;auto depth=info_depth;
        // The manual write runs between the native Begin and EndRefresh,
        // also when BeginUpdate failed, and leaves the scope as it was.
        assert(f.call(p96_base,102,{ri,rp},{0,0,0,0,8,4})==0 && f.ctx.running);
        assert(p96_writes.size()==3 && p96_writes.back()==depth && depth>0);
        assert(held_locks==locks && info_depth==depth && !refresh_ends && !update_ends);
        for(ULONG lvo=30;lvo<=192;lvo+=6)
            if(lvo!=102)assert(f.call(p96_base,lvo,{ri,rp})==20 && f.stopped() && p96_writes.size()==3);
        // A generated 102, or a manual 102 of a library of another name, stops.
        f.lib(4,p96_base,"Picasso96API.library",no_write,27,generated);
        assert(f.call(p96_base,102,{ri,rp})==20 && f.stopped());
        for(const char *name:{"picasso96api.library","Picasso96API.libraryX","Picasso96API"})
        {
            f.lib(4,p96_base,name,p96,28,none);
            assert(f.call(p96_base,102,{ri,rp})==20 && f.stopped() && p96_writes.size()==3);
        }
        f.lib(4,p96_base,"Picasso96API.library",p96,28,none);
        assert(f.call(p96_base,102,{ri,rp})==0 && f.ctx.running && p96_writes.size()==4);
        assert(held_locks==locks && info_depth==depth && !refresh_ends);
        assert(f.call(Fixture::intuition_base,366,{mw},{1})==0 && f.ctx.running);
        assert(refresh_ends==1 && update_ends==!oom && !held_locks && !info_depth &&
               !Kaffeine_LayerInfoLocked(&f.ctx));
        // An explicit Info scope takes the same single vector.
        assert(f.call(Fixture::layers_base,120,{info})==0 && Kaffeine_LayerInfoLocked(&f.ctx));
        assert(f.call(p96_base,102,{ri,rp})==0 && f.ctx.running && p96_writes.back()==1);
        assert(f.call(p96_base,60,{ri})==20 && f.stopped() && p96_writes.size()==5);
        assert(f.call(Fixture::layers_base,138,{info})==0 && !info_depth);
        assert(f.call(p96_base,60,{ri})==0 && p96_writes.size()==6 && p96_writes.back()==0);
        f.close(mw);
    }
}
