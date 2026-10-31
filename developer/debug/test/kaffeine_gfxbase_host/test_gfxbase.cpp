/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <proto/graphics.h>
#include "kaffeine_intern.h"
#include "kaffeine_cpu.h"
#include "kaffeine_gfxbase.h"
#include "kaffeine_rastport.h"
#include "kaffeine_messages.h"

static_assert(sizeof(Library) == 64);
static_assert(offsetof(GfxBase, DefaultFont) == 320);
static_assert(sizeof(ULONG) == 4 && sizeof(void *) == 8);
static unsigned checks, failures;
static void check(bool okay, const char *label)
{
    ++checks;
    if (!okay) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}
static std::map<APTR,ULONG> allocations;
static int allocation_countdown = -1;
static unsigned allocation_calls;
static SIPTR io_error;
static constexpr SIPTR saved_error = static_cast<SIPTR>(0x12345678000000abULL);
extern "C" APTR AllocMem(ULONG size, ULONG flags)
{
    ++allocation_calls;
    if (!allocation_countdown) return nullptr;
    if (allocation_countdown > 0) --allocation_countdown;
    void *p = std::malloc(size ? size : 1);
    if (p) { std::memset(p, flags & MEMF_CLEAR ? 0 : 0xd9, size); allocations[p] = size; }
    io_error = 0x101;
    return p;
}
extern "C" void FreeMem(APTR p, ULONG size)
{
    auto found = allocations.find(p);
    check(found != allocations.end() && found->second == size, "native allocation has one exact release");
    if (found != allocations.end()) allocations.erase(found);
    std::free(p); io_error = 0x102;
}
extern "C" APTR AllocVec(ULONG n, ULONG f) { return AllocMem(n,f); }
extern "C" void FreeVec(APTR p) { if (p) FreeMem(p,allocations.at(p)); }
extern "C" SIPTR IoErr(void) { return io_error; }
extern "C" SIPTR SetIoErr(SIPTR n) { SIPTR old = io_error; io_error = n; return old; }
extern "C" BOOL Kaffeine_LockProtected(KaffeineContext *,ULONG,ULONG) { return FALSE; }

struct Font
{
    TextFont native{};
    char name[64]{"public-default.font"};
    std::array<UBYTE,6> bits{0xa1,0x11,0xb2,0x22,0xc3,0x33};
    std::array<ULONG,3> loc{0x00000003,0x00030004,0x00070005};
    std::array<WORD,3> space{3,5,-2}, kern{-1,0,2};
    unsigned opens{}, closes{};
    Font()
    {
        native.tf_Message.mn_Node.ln_Type = NT_FONT;
        native.tf_Message.mn_Node.ln_Name = name;
        native.tf_Message.mn_Node.ln_Succ = reinterpret_cast<Node *>(static_cast<IPTR>(0x123456780000ULL));
        native.tf_Message.mn_ReplyPort = reinterpret_cast<MsgPort *>(static_cast<IPTR>(0x234567890000ULL));
        native.tf_YSize = 3; native.tf_Style = FSF_UNDERLINED | FSF_TAGGED;
        native.tf_Flags = FPF_DISKFONT | FPF_PROPORTIONAL; native.tf_XSize = 5;
        native.tf_Baseline = 2; native.tf_BoldSmear = 1; native.tf_Accessors = 1;
        native.tf_LoChar = 65; native.tf_HiChar = 66; native.tf_Modulo = 2;
        native.tf_CharData = bits.data(); native.tf_CharLoc = loc.data();
        native.tf_CharSpace = space.data(); native.tf_CharKern = kern.data();
    }
};
struct Native
{
    Font font, alternate;
    GfxBase gfx{};
    bool library_fail{}, open_fail{}, wrong_font{};
    unsigned library_opens{}, library_closes{}, library_refs{}, font_calls{};
    Native() { gfx.DefaultFont = &font.native; std::strcpy(alternate.name,"alternate.font"); }
};
static Native *native;
extern "C" Library *OpenLibrary(CONST_STRPTR name,ULONG version)
{
    check(native && !std::strcmp(name,"graphics.library") && version == 0,
          "only the real native graphics.library is opened");
    ++native->library_opens; io_error = 0x201;
    if (native->library_fail) return nullptr;
    ++native->library_refs;
    return &native->gfx.LibNode;
}
extern "C" void CloseLibrary(Library *base)
{
    check(native && base == &native->gfx.LibNode && native->library_refs,
          "native graphics library hold is balanced");
    ++native->library_closes; --native->library_refs; io_error = 0x202;
}
static Font *find_font(TextFont *p)
{
    if (p == &native->font.native) return &native->font;
    if (p == &native->alternate.native) return &native->alternate;
    return nullptr;
}
extern "C" TextFont *OpenFont(const TextAttr *attr)
{
    check(native && native->gfx.DefaultFont && attr,
          "a native default font supplies the hold attributes");
    TextFont *expected = native->gfx.DefaultFont;
    check(attr && attr->ta_Name == expected->tf_Message.mn_Node.ln_Name &&
          attr->ta_YSize == expected->tf_YSize &&
          attr->ta_Style == (expected->tf_Style & ~FSF_TAGGED) && attr->ta_Flags == expected->tf_Flags,
          "native open uses exact default attributes and strips only private extension style");
    ++native->font_calls; io_error = 0x301;
    if (native->open_fail) return nullptr;
    Font *f = native->wrong_font ? &native->alternate : find_font(expected);
    check(f && f->native.tf_Accessors != 0xffff,"font hold does not overflow native accessors");
    ++f->opens; ++f->native.tf_Accessors;
    return &f->native;
}
extern "C" void CloseFont(TextFont *font)
{
    Font *f = find_font(font);
    check(f && f->opens > f->closes && f->native.tf_Accessors > 1,
          "only an actually acquired native font reference is closed");
    if (f) { ++f->closes; --f->native.tf_Accessors; }
    io_error = 0x302;
}
extern "C" const M68KLibThunkSet kaffeine_all_gen_libs[] = {
    {"graphics.library", nullptr, 0}, {"public-neighbor.library",nullptr,0}
};
extern "C" const ULONG kaffeine_all_gen_libs_count = 2;

struct Guest
{
    KaffeineContext *ctx;
    Guest()
    {
        ctx = static_cast<KaffeineContext *>(AllocMem(sizeof(*ctx),MEMF_CLEAR));
        ctx->mem_size = 1024 * 1024;
        ctx->mem = static_cast<UBYTE *>(AllocMem(ctx->mem_size + KAFFEINE_MEM_GUARD,MEMF_CLEAR));
        ctx->heap_start = KAFFEINE_HEAP_BASE;
        ctx->heap_end = ctx->stack_bottom = ctx->mem_size - 65536;
        ctx->stack_top = ctx->mem_size;
        check(Kaffeine_HeapInit(ctx),"real production heap initializes");
        ctx->running = TRUE;
        std::memset(ctx->mem + KAFFEINE_LIBBASE_REGION,0xa5,KAFFEINE_LIBBASE_SIZE);
    }
    ~Guest()
    {
        allocation_countdown = -1;
        Kaffeine_FontsCleanup(ctx);
        Kaffeine_DestroyContext(ctx);
    }
    ULONG open(const char *name = "graphics.library")
    {
        io_error = saved_error;
        ULONG result = Kaffeine_OpenLibrary(ctx,name);
        check(io_error == saved_error,"library initialization preserves full-width IoErr");
        return result;
    }
    ULONG font(ULONG base) { return base ? m68k_read32(ctx,base + 154) : 0; }
    int font_slot(ULONG guest)
    {
        for (int i=0;i<KAFFEINE_MAX_FONTS;++i) if (ctx->fonts[i].guest == guest && guest) return i;
        return -1;
    }
    unsigned font_count()
    {
        unsigned result=0;
        for (const auto &f:ctx->fonts) if(f.guest) ++result;
        return result;
    }
};
static void ownership_clean(const Native &n)
{
    check(!n.library_refs && n.library_opens - (n.library_fail ? 1 : 0) == n.library_closes,
          "all native graphics-library references are released");
    check(n.font.opens == n.font.closes && n.alternate.opens == n.alternate.closes,
          "context cleanup releases exactly its own default-font references");
    check(allocations.empty(),"context and native heap metadata have no remaining allocations");
}
static void publication(bool reservation_only)
{
    Native n; native=&n;
    {
        Guest g;
        ULONG exec = Kaffeine_SetupLibBase(g.ctx,0,"exec.library",4,nullptr,0,nullptr,0);
        ULONG before = Kaffeine_SetupLibBase(g.ctx,1,"before.library",7,nullptr,0,nullptr,0);
        std::vector<UBYTE> preceding(g.ctx->mem + KAFFEINE_LIBBASE_REGION,
                                     g.ctx->mem + before + 4);
        ULONG base=g.open("LIBS:graphics.library");
        check(base && g.ctx->libs[2].pos_size == 552,"classic graphics positive area reserves all 552 bytes");
        check(g.ctx->libs[0].pos_size == 632 && g.ctx->libs[1].pos_size == 4 && exec,
              "other fake library positive-area sizes retain their previous semantics");
        check(!std::memcmp(preceding.data(),g.ctx->mem+KAFFEINE_LIBBASE_REGION,preceding.size()),
              "graphics initialization preserves preceding base and jump tables");
        ULONG f=g.font(base);
        check(f && Kaffeine_FontNative(g.ctx,f) == &n.font.native,
              "DefaultFont+154 is a typed view of the exact native default");
        if (!reservation_only && base && g.font_slot(f)>=0)
        {
            for (unsigned i=0;i<552;++i)
                if (i<154 || i>=158) check(g.ctx->mem[base+i]==0,"only DefaultFont is published in cleared classic GfxBase");
            check(g.ctx->mem[base+552]==0xa5,"clear stops exactly after the reserved positive area");
            for (unsigned i=0;i<512;++i)
            {
                ULONG p=base-6*(i+1);
                check(m68k_read16(g.ctx,p)==0xa000 && m68k_read16(g.ctx,p+2)==0x4e71 &&
                      m68k_read16(g.ctx,p+4)==0x4e71,"graphics negative vectors remain A-line/NOP/NOP");
            }
            check(m68k_read16(g.ctx,f+20)==3 && g.ctx->mem[f+22]==FSF_UNDERLINED &&
                  g.ctx->mem[f+23]==n.font.native.tf_Flags && m68k_read16(g.ctx,f+24)==5 &&
                  m68k_read16(g.ctx,f+26)==2,"typed default metrics preserve real native values");
            ULONG name=m68k_read32(g.ctx,f+10), bits=m68k_read32(g.ctx,f+34);
            check(name && name<g.ctx->mem_size && !std::strcmp(reinterpret_cast<char*>(g.ctx->mem+name),n.font.name),
                  "font name is a copied guest string, not a native pointer");
            check(bits && bits<g.ctx->mem_size && !std::memcmp(g.ctx->mem+bits,n.font.bits.data(),6),
                  "font bitmap is a copied readable guest block");
            check(m68k_read32(g.ctx,f)==0 && m68k_read32(g.ctx,f+4)==0 &&
                  m68k_read32(g.ctx,f+14)==0,"native node links and font extension pointers stay zero");
            ULONG loc=m68k_read32(g.ctx,f+40), space=m68k_read32(g.ctx,f+44), kern=m68k_read32(g.ctx,f+48);
            check(loc && space && kern && m68k_read32(g.ctx,loc+8)==n.font.loc[2] &&
                  m68k_read16(g.ctx,space+4)==static_cast<UWORD>(-2) &&
                  m68k_read16(g.ctx,kern)==static_cast<UWORD>(-1),"copied font tables use classic byte order");
            int i=g.font_slot(f);
            check(g.ctx->fonts[i].pinned && g.ctx->fonts[i].retained && !g.ctx->fonts[i].opens &&
                  n.font.native.tf_Accessors==2,"context owns one real native hold for its pinned default");
            check(Kaffeine_GuestBytes(g.ctx,bits,6),"default-font bitmap remains a legitimate guest data buffer");
            auto image=std::vector<UBYTE>(g.ctx->mem+base-512*6,g.ctx->mem+base+552);
            unsigned calls=n.font_calls, libs=n.library_opens;
            check(g.open()==base && g.open("graphics.library")==base && g.font(base)==f,
                  "repeated OpenLibrary returns stable base and font identities");
            check(n.font_calls==calls && n.library_opens==libs,"cached opens acquire no duplicate native hold");
            ULONG after=g.open("public-neighbor.library");
            check(after && g.ctx->libs[3].jt_start==(base+552+3U)/4U*4U,
                  "following fake library starts after the complete graphics positive area");
            check(!std::memcmp(image.data(),g.ctx->mem+base-512*6,image.size()),
                  "following jump table does not overwrite graphics base or font identity");
            check(m68k_read16(g.ctx,after-6)==0xa000,"following jump table keeps its own trap vector");
            Kaffeine_FontClosed(g.ctx,f);
            check(Kaffeine_FontNative(g.ctx,f)==&n.font.native && n.font.native.tf_Accessors==2,
                  "borrowed DefaultFont close cannot release the context hold");
        }
    }
    ownership_clean(n);
}
static void failed_initialization()
{
    for (unsigned kind=0;kind<8;++kind)
    {
        Native n; native=&n;
        {
            Guest g;
            if(kind==0) n.library_fail=true;
            if(kind==1) n.gfx.DefaultFont=nullptr;
            if(kind==2) n.open_fail=true;
            if(kind==3) n.wrong_font=true;
            if(kind==4) n.font.native.tf_Accessors=0xffff;
            if(kind==5) n.font.native.tf_Style|=FSF_COLORFONT;
            if(kind==6) n.font.native.tf_Message.mn_Node.ln_Name=nullptr;
            if(kind==7) n.font.native.tf_CharLoc=nullptr;
            ULONG available=Kaffeine_HeapAvail(g.ctx,MEMF_ANY);
            std::vector<UBYTE> bases(g.ctx->mem+KAFFEINE_LIBBASE_REGION,
                                     g.ctx->mem+KAFFEINE_LIBBASE_REGION+KAFFEINE_LIBBASE_SIZE);
            check(!g.open(),"native hold or typed snapshot failure publishes no graphics base");
            check(!g.ctx->num_libs && !g.ctx->num_libmap && !g.font_count(),
                  "failure leaves fake library and font registries empty");
            check(!std::memcmp(bases.data(),g.ctx->mem+KAFFEINE_LIBBASE_REGION,bases.size()) &&
                  Kaffeine_HeapAvail(g.ctx,MEMF_ANY)==available,"failure leaves base bytes and guest allocation capacity unchanged");
            check(g.ctx->running && !g.ctx->callback_failed,"ordinary unavailable native font fails without stopping guest execution");
            check(n.font.opens==n.font.closes && n.alternate.opens==n.alternate.closes,
                  "failed publication balances the exact acquired or mismatched native font");
        }
        ownership_clean(n);
    }
}
static void rollback_and_limits()
{
    for(unsigned kind=0;kind<4;++kind)
    {
        Native n; native=&n;
        {
            Guest g;
            ULONG available=Kaffeine_HeapAvail(g.ctx,MEMF_ANY);
            unsigned allocs=allocations.size();
            if(kind==0) allocation_countdown=0;
            if(kind==1) g.ctx->num_libs=KAFFEINE_MAX_LIBS;
            if(kind==2) g.ctx->num_libmap=KAFFEINE_MAX_LIBMAP;
            if(kind==3)
            {
                g.ctx->num_libs=1;
                g.ctx->libs[0].m68k_addr=KAFFEINE_LIBBASE_REGION+KAFFEINE_LIBBASE_SIZE-4;
                g.ctx->libs[0].pos_size=4;
            }
            UWORD oldlibs=g.ctx->num_libs, oldmap=g.ctx->num_libmap;
            check(!g.open(),"OOM or library reservation limits fail without publishing a base");
            allocation_countdown=-1;
            check(g.ctx->num_libs==oldlibs && g.ctx->num_libmap==oldmap && !g.font_count(),
                  "reservation rollback preserves all existing registry counts");
            check(Kaffeine_HeapAvail(g.ctx,MEMF_ANY)==available && allocations.size()==allocs,
                  "fresh font mirror is fully rolled back on publication failure");
            check(n.font.opens==n.font.closes && !n.library_refs,
                  "OOM and full registries retain no native hold");
            check(g.ctx->running && !g.ctx->callback_failed,"ordinary OOM and caps leave guest running");
            g.ctx->num_libs=g.ctx->num_libmap=0;
        }
        ownership_clean(n);
    }
    for(bool pinned:{false,true})
    {
        Native n; native=&n;
        {
            Guest g;
            ULONG f=Kaffeine_FontGuest(g.ctx,&n.font.native);
            if(pinned) Kaffeine_FontPin(g.ctx,f);
            int i=g.font_slot(f);
            ULONG available=Kaffeine_HeapAvail(g.ctx,MEMF_ANY);
            g.ctx->num_libs=KAFFEINE_MAX_LIBS;
            check(!g.open() && Kaffeine_FontFind(g.ctx,&n.font.native)==f,
                  "failed graphics publication preserves an existing borrowed font identity");
            check(g.ctx->fonts[i].pinned==pinned && !g.ctx->fonts[i].retained && !g.ctx->fonts[i].opens &&
                  n.font.native.tf_Accessors==1,"rollback does not adopt a new native reference for an existing pinned view");
            check(Kaffeine_HeapAvail(g.ctx,MEMF_ANY)==available,"existing mirror is never freed by failed default publication");
            g.ctx->num_libs=0;
        }
        ownership_clean(n);
    }
    Native n; native=&n;
    {
        Guest g;
        std::vector<std::unique_ptr<Font>> fonts;
        for(unsigned i=0;i<KAFFEINE_MAX_FONTS;++i)
        {
            fonts.push_back(std::make_unique<Font>());
            check(Kaffeine_FontGuest(g.ctx,&fonts.back()->native)!=0,"font table filled through real snapshot API");
        }
        check(!g.open() && g.font_count()==KAFFEINE_MAX_FONTS && n.font.opens==n.font.closes,
              "full font table refuses a default and balances its acquired native hold");
        // Borrowed native objects remain alive through production font cleanup.
        Kaffeine_FontsCleanup(g.ctx);
    }
    ownership_clean(n);
}
static void existing_opens_and_contexts()
{
    Native n; native=&n;
    {
        Guest g;
        TextAttr attr{n.font.name,3,FSF_UNDERLINED,n.font.native.tf_Flags};
        ULONG f=Kaffeine_FontAdopt(g.ctx,OpenFont(&attr));
        int i=g.font_slot(f);
        check(f && n.font.native.tf_Accessors==2 && g.ctx->fonts[i].opens==1,
              "ordinary guest font open has its own native reference");
        ULONG base=g.open();
        check(base && g.font(base)==f && n.font.native.tf_Accessors==2 && !g.ctx->fonts[i].retained &&
              g.ctx->fonts[i].pinned,"default initialization reuses existing owned font identity without another reference");
        Kaffeine_FontClosed(g.ctx,f);
        check(!g.ctx->fonts[i].opens && g.ctx->fonts[i].retained && n.font.native.tf_Accessors==2,
              "last guest close retains the pinned native font reference");
        check(Kaffeine_FontAdopt(g.ctx,OpenFont(&attr))==f && g.ctx->fonts[i].opens==1 &&
              !g.ctx->fonts[i].retained && n.font.native.tf_Accessors==2,
              "a later guest open replaces the retained reference without doubling it");
        Kaffeine_FontClosed(g.ctx,f);
    }
    ownership_clean(n);
    for (unsigned kind=0;kind<3;++kind)
    {
        Native borrowed; native=&borrowed;
        {
            Guest g;
            ULONG f;
            if(kind==2)
            {
                TextAttr attr{borrowed.font.name,3,FSF_UNDERLINED,borrowed.font.native.tf_Flags};
                f=Kaffeine_FontAdopt(g.ctx,OpenFont(&attr));
                Kaffeine_FontPin(g.ctx,f);
                Kaffeine_FontClosed(g.ctx,f);
            }
            else
            {
                f=Kaffeine_FontGuest(g.ctx,&borrowed.font.native);
                if(kind==1) Kaffeine_FontPin(g.ctx,f);
            }
            ULONG base=g.open();
            int i=g.font_slot(f);
            check(base && g.font(base)==f && g.ctx->fonts[i].pinned && g.ctx->fonts[i].retained &&
                  !g.ctx->fonts[i].opens && borrowed.font.native.tf_Accessors==2,
                  "existing borrowed, pinned or retained view reuses one owned default reference");
            unsigned calls=borrowed.font_calls;
            borrowed.gfx.DefaultFont=&borrowed.alternate.native;
            check(g.open()==base && g.font(base)==f && borrowed.font_calls==calls &&
                  Kaffeine_FontNative(g.ctx,f)==&borrowed.font.native,
                  "published default identity and native hold survive later preference selection changes");
        }
        ownership_clean(borrowed);
    }
    Native shared; native=&shared;
    {
        Guest a;
        ULONG ba=a.open(), fa=a.font(ba);
        {
            Guest b;
            ULONG bb=b.open(), fb=b.font(bb);
            check(ba && bb && fa && fb && Kaffeine_FontNative(a.ctx,fa)==&shared.font.native &&
                  Kaffeine_FontNative(b.ctx,fb)==&shared.font.native && shared.font.native.tf_Accessors==3,
                  "separate contexts each own one hold on the same real default font");
        }
        check(shared.font.native.tf_Accessors==2 && Kaffeine_FontNative(a.ctx,fa)==&shared.font.native,
              "cleanup of one context leaves the other default font alive");
    }
    ownership_clean(shared);
}
int main(int argc,char **argv)
{
    bool only=argc==2 && !std::strcmp(argv[1],"--reservation-only");
    publication(only);
    if(!only) { failed_initialization(); rollback_and_limits(); existing_opens_and_contexts(); }
    std::printf("gfx-default-font: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
