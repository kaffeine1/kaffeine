/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Public and opened screens through literal Intuition LVOs, the real
    dispatcher, heap, tag walker, TextAttr bridge and RastPort bridge.
    Native Intuition below follows the list, name, private-screen and
    visitor-count rules of rom/intuition, the tag precedence and SA_Width
    check of rom/intuition/openscreen.c, the window and visitor refusals of
    closescreen.c, and records every call, including nested native list
    locks.
*/
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include <exec/lists.h>
#include <exec/memory.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"
#include "kaffeine_screens.h"
#include "kaffeine_layers.h"
#include "kaffeine_dispatch.h"

/* The native side. Names have backing storage for the bridge's bounded
   reads. No locale/POSIX string functions are needed for ASCII names. */
static bool equal_name(const char *a, const char *b)
{
    auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    while (*a && lower(*a) == lower(*b)) { ++a; ++b; }
    return lower(*a) == lower(*b);
}

struct NativeScreen
{
    Screen screen{};
    PubScreenNode node{};
    std::array<char, 256> name{};
    /* What an opened screen draws into, as a HIDD BitMap of AROS: no
       planes the guest could reach, and not the obsolete copy. */
    BitMap drawn{};
};
enum Kind { LOCK, UNLOCK, LIST_LOCK, LIST_UNLOCK, DEFAULT, NEXT, FONT_OPEN, FONT_CLOSE, OPEN, CLOSE };
struct Call
{
    Kind kind;
    const void *pointer;
    Screen *screen;
};
/* One OpenScreenTagList() as native Intuition got it: the NewScreen and
   tags, the values that count after the tags, and copies of what each
   native pointer showed during the call. */
struct Opened
{
    bool had_ns = false, had_tags = false, closed = false;
    NewScreen ns{};
    std::vector<TagItem> tags;
    WORD left = 0, top = 0, width = -1, height = -1, depth = 1;
    UBYTE detail = 0, block = 1;
    UWORD type = CUSTOMSCREEN | SHOWTITLE;
    TextAttr *font = nullptr;
    TextAttr font_copy{};
    std::string font_name;
    std::vector<TagItem> font_tags;
    UBYTE *title = nullptr;
    std::string title_text;
    std::vector<UWORD> pens;
    Rectangle clip{};
    bool had_clip = false;
    ULONG *error = nullptr;
    bool error_seen = false, pubname_seen = false;
    std::string pubname;
    Screen *result = nullptr;
};
struct NativeIntuition
{
    List list{};
    std::deque<NativeScreen> screens;
    std::vector<Call> calls;
    std::vector<TextAttr> opened_attrs;
    std::vector<const TagItem *> opened_tags;
    std::map<TextFont *, unsigned> font_opens;
    TextFont *font_result = nullptr;
    /* OpenScreenTagList() and CloseScreen() */
    std::deque<Opened> opened;
    TextFont *screen_font = nullptr;
    bool open_fail = false, close_refuse = false;
    ULONG open_error = 0;
    Screen *default_screen = nullptr, *workbench = nullptr;
    unsigned list_depth = 0;
    KaffeineContext *ctx = nullptr;

    NativeIntuition()
    {
        list.lh_Head = reinterpret_cast<Node *>(&list.lh_Tail);
        list.lh_TailPred = reinterpret_cast<Node *>(&list.lh_Head);
        list.lh_Type = 0xA1;
    }
    NativeScreen &add(const std::string &name, UWORD flags = 0)
    {
        // The guest itself may open another screen under its list lock.
        assert(name.size() < 256);
        screens.emplace_back();
        auto &s = screens.back();
        std::memcpy(s.name.data(), name.c_str(), name.size() + 1);
        s.node.psn_Node.ln_Name = s.name.data();
        s.node.psn_Node.ln_Type = 0xC3;
        s.node.psn_Node.ln_Pri = -17;
        s.node.psn_Screen = &s.screen;
        s.node.psn_Flags = flags;
        s.node.psn_Size = -300;
        s.node.psn_SigTask = reinterpret_cast<Task *>(&s.screen);
        s.node.psn_SigBit = 29;
        append(&s.node.psn_Node);
        return s;
    }
    void append(Node *n)
    {
        n->ln_Pred = list.lh_TailPred;
        n->ln_Succ = reinterpret_cast<Node *>(&list.lh_Tail);
        list.lh_TailPred->ln_Succ = n;
        list.lh_TailPred = n;
    }
    void remove(Node *n)
    {
        n->ln_Pred->ln_Succ = n->ln_Succ;
        n->ln_Succ->ln_Pred = n->ln_Pred;
        n->ln_Pred = n->ln_Succ = nullptr;
    }
    PubScreenNode *named(const char *name)
    {
        assert(list_depth);
        for (Node *n = list.lh_Head; n->ln_Succ; n = n->ln_Succ)
            if (n->ln_Name && equal_name(n->ln_Name, name))
                return reinterpret_cast<PubScreenNode *>(n);
        return nullptr;
    }
    PubScreenNode *node(Screen *screen)
    {
        for (auto &s : screens)
            if (&s.screen == screen) return &s.node;
        assert(false);
        return nullptr;
    }
    size_t count(Kind kind) const
    {
        return std::count_if(calls.begin(), calls.end(), [kind](const Call &c) { return c.kind == kind; });
    }
};
static NativeIntuition *native;

/* Configurable OpenFont result, with AROS's accessor accounting. The call
   records both the native TextAttr pointer and its values at that time,
   and the tags of a TTextAttr, read as FSF_TAGGED says (ASan sees a plain
   TextAttr read that far). */
extern "C" struct TextFont *OpenFont(const struct TextAttr *attr)
{
    assert(attr);
    native->calls.push_back({ FONT_OPEN, attr, nullptr });
    native->opened_attrs.push_back(*attr);
    native->opened_tags.push_back(attr->ta_Style & FSF_TAGGED
                                  ? reinterpret_cast<const TTextAttr *>(attr)->tta_Tags : nullptr);
    if (native->font_result)
    {
        ++native->font_result->tf_Accessors;
        ++native->font_opens[native->font_result];
    }
    return native->font_result;
}
extern "C" void CloseFont(struct TextFont *font)
{
    native->calls.push_back({ FONT_CLOSE, font, nullptr });
    auto at = native->font_opens.find(font);
    assert(at != native->font_opens.end() && at->second && font->tf_Accessors);
    --font->tf_Accessors;
    if (!--at->second) native->font_opens.erase(at);
}

extern "C" struct List *LockPubScreenList(void)
{
    native->calls.push_back({ LIST_LOCK, nullptr, nullptr });
    ++native->list_depth;
    return &native->list;
}
extern "C" void UnlockPubScreenList(void)
{
    native->calls.push_back({ LIST_UNLOCK, nullptr, nullptr });
    assert(native->list_depth);
    --native->list_depth;
}
extern "C" struct Screen *LockPubScreen(CONST_STRPTR name)
{
    native->calls.push_back({ LOCK, name, nullptr });
    LockPubScreenList();
    Screen *screen = nullptr;
    if (name)
    {
        PubScreenNode *n = native->named(name); // first matching node, private or not
        if (n && !(n->psn_Flags & PSNF_PRIVATE)) screen = n->psn_Screen;
    }
    else
        screen = native->default_screen ? native->default_screen : native->workbench;
    // Workbench's automatic open may fail; the fixture supplies it when available.
    if (!screen && (!name || equal_name(name, "Workbench"))) screen = native->workbench;
    if (screen)
    {
        auto *n = native->node(screen);
        n->psn_VisitorCount = (WORD)((UWORD)n->psn_VisitorCount + 1u);
    }
    UnlockPubScreenList();
    return screen;
}
extern "C" void UnlockPubScreen(UBYTE *name, struct Screen *screen)
{
    native->calls.push_back({ UNLOCK, name, screen });
    LockPubScreenList();
    if (name)
    {
        auto *n = native->named(reinterpret_cast<const char *>(name));
        screen = n ? n->psn_Screen : nullptr; // A1 is ignored for a name
    }
    if (screen)
    {
        auto *n = native->node(screen);
        n->psn_VisitorCount = (WORD)((UWORD)n->psn_VisitorCount - 1u);
    }
    UnlockPubScreenList();
}
extern "C" struct Screen *GetDefaultPubScreen(UBYTE *buffer)
{
    native->calls.push_back({ DEFAULT, buffer, nullptr });
    // The bridge must use native temporary storage, never guest memory.
    auto p = reinterpret_cast<std::uintptr_t>(buffer);
    auto g = reinterpret_cast<std::uintptr_t>(native->ctx->mem);
    assert(buffer && (p < g || p - g >= native->ctx->mem_size));
    LockPubScreenList();
    const char *name = native->default_screen ? native->node(native->default_screen)->psn_Node.ln_Name
                                               : "Workbench";
    assert(std::strlen(name) <= MAXPUBSCREENNAME);
    std::strcpy(reinterpret_cast<char *>(buffer), name);
    UnlockPubScreenList();
    return native->default_screen; // AROS returns this; the NDK thunk must return VOID.
}
/* This must never be delegated: AROS's NextPubScreen reads through NULL
   for a screen without pubScrNode (rom/intuition/nextpubscreen.c). */
extern "C" UBYTE *NextPubScreen(struct Screen *, UBYTE *)
{
    native->calls.push_back({ NEXT, nullptr, nullptr });
    assert(false);
    return nullptr;
}
static bool in_guest(const void *p)
{
    auto a = reinterpret_cast<std::uintptr_t>(p), g = reinterpret_cast<std::uintptr_t>(native->ctx->mem);
    return p && a >= g && a - g < native->ctx->mem_size;
}
/* Intuition keeps Font and DefaultTitle; it reads the pens, the TextAttr
   tags and writes the error during the call. None of these, nor the
   NewScreen or the tags, may be guest memory; SA_PubName is copied. */
extern "C" struct Screen *OpenScreenTagList(struct NewScreen *ns, struct TagItem *tags)
{
    native->calls.push_back({ OPEN, ns, nullptr });
    assert(!in_guest(ns) && !in_guest(tags));
    Opened o;
    bool bad = false;
    if (ns)
    {
        o.had_ns = true; o.ns = *ns;
        assert(!(ns->Type & NS_EXTENDED) && !(ns->Type & CUSTOMBITMAP) && !ns->Gadgets && !ns->CustomBitMap);
        o.left = ns->LeftEdge; o.top = ns->TopEdge; o.width = ns->Width; o.height = ns->Height;
        o.depth = ns->Depth; o.detail = ns->DetailPen; o.block = ns->BlockPen; o.type = ns->Type;
        o.font = ns->Font; o.title = ns->DefaultTitle;
    }
    const UWORD *pens = nullptr;
    for (TagItem *t = tags; t && t->ti_Tag != TAG_DONE; ++t)
    {
        o.had_tags = true;
        // The bridge flattens the control tags.
        assert(t->ti_Tag != TAG_MORE && t->ti_Tag != TAG_SKIP && t->ti_Tag != TAG_IGNORE);
        o.tags.push_back(*t);
        switch (t->ti_Tag)
        {
        case SA_Left: o.left = (WORD)t->ti_Data; break;
        case SA_Top: o.top = (WORD)t->ti_Data; break;
        case SA_Width:
        case SA_Height:
            if (t->ti_Data != ~static_cast<IPTR>(0) && t->ti_Data > 32767) bad = true;
            (t->ti_Tag == SA_Width ? o.width : o.height) = (WORD)t->ti_Data;
            break;
        case SA_Depth: o.depth = (WORD)t->ti_Data; break;
        case SA_DetailPen: o.detail = (UBYTE)t->ti_Data; break;
        case SA_BlockPen: o.block = (UBYTE)t->ti_Data; break;
        case SA_Type: o.type = (o.type & ~SCREENTYPE) | (UWORD)t->ti_Data; break;
        case SA_Title: o.title = reinterpret_cast<UBYTE *>(t->ti_Data); break;
        case SA_Font: o.font = reinterpret_cast<TextAttr *>(t->ti_Data); break;
        case SA_Pens: pens = reinterpret_cast<const UWORD *>(t->ti_Data); break;
        case SA_DClip:
            o.had_clip = t->ti_Data != 0;
            if (o.had_clip)
            {
                auto *clip = reinterpret_cast<const Rectangle *>(t->ti_Data);
                assert(!in_guest(clip));
                o.clip = *clip;
            }
            break;
        case SA_ErrorCode:
            if (!o.error_seen) o.error = reinterpret_cast<ULONG *>(t->ti_Data);
            o.error_seen = true;
            break;
        case SA_PubName:
            if (!o.pubname_seen && t->ti_Data) o.pubname = reinterpret_cast<const char *>(t->ti_Data);
            o.pubname_seen = true;
            break;
        }
    }
    assert(!in_guest(o.font) && !in_guest(o.title) && !in_guest(pens) && !in_guest(o.error));
    if (o.font)
    {
        assert(o.font->ta_Name && !in_guest(o.font->ta_Name));
        o.font_copy = *o.font; o.font_name = o.font->ta_Name;
        const TagItem *ft = (o.font->ta_Style & FSF_TAGGED)
                            ? reinterpret_cast<const TTextAttr *>(o.font)->tta_Tags : nullptr;
        assert(!in_guest(ft));
        for (; ft && ft->ti_Tag != TAG_DONE; ++ft) o.font_tags.push_back(*ft);
    }
    if (o.title) o.title_text = reinterpret_cast<const char *>(o.title);
    for (unsigned i = 0; pens && i < 65; ++i)
    {
        o.pens.push_back(pens[i]);
        if (pens[i] == (UWORD)~0) break;
    }
    if (native->open_fail || bad)
    {
        if (o.error && native->open_error) *o.error = native->open_error;
        native->opened.push_back(o);
        return nullptr;
    }
    auto &n = native->screens.emplace_back();
    Screen &s = n.screen;
    s.LeftEdge = o.left; s.TopEdge = o.top;
    s.Width = o.width == -1 ? 640 : o.width; s.Height = o.height == -1 ? 480 : o.height;
    s.Flags = o.type; s.Font = o.font; s.Title = s.DefaultTitle = o.title;
    s.MouseX = 17; s.MouseY = -9;
    s.BarHeight = 10; s.BarVBorder = 1; s.BarHBorder = 5; s.MenuVBorder = 2; s.MenuHBorder = 4;
    s.WBorTop = 6; s.WBorLeft = 4; s.WBorRight = 4; s.WBorBottom = 4;
    s.ViewPort.DWidth = s.Width; s.ViewPort.DHeight = s.Height;
    s.ViewPort.Modes = o.had_ns ? o.ns.ViewModes : 0x8004;
    // the obsolete copy disagrees with the BitMap that the screen draws into
    s.BitMap_OBSOLETE.BytesPerRow = 0x1357; s.BitMap_OBSOLETE.Rows = 0x2468;
    s.BitMap_OBSOLETE.Flags = 0x5A; s.BitMap_OBSOLETE.Depth = 0x3C;
    n.drawn.BytesPerRow = (UWORD)(((s.Width + 15) >> 4) * 2); n.drawn.Rows = (UWORD)s.Height;
    n.drawn.Depth = (UBYTE)o.depth; n.drawn.Flags = 0x84; n.drawn.pad = 0x4321;
    s.DetailPen = o.detail; s.BlockPen = o.block; s.SaveColor0 = 0x0AAA;
    s.RastPort.Font = native->screen_font;
    // native pointers that never reach the guest
    s.ViewPort.ColorMap = reinterpret_cast<ColorMap *>(&s);
    s.RastPort.BitMap = &n.drawn;
    std::memset(s.BitMap_OBSOLETE.Planes, 0x77, sizeof(s.BitMap_OBSOLETE.Planes));
    for (auto &p : n.drawn.Planes) p = reinterpret_cast<PLANEPTR>(&n.drawn);
    s.NextScreen = &s; s.ExtData = s.UserData = reinterpret_cast<UBYTE *>(&s);
    if (!o.pubname.empty())
    {
        assert(o.pubname.size() <= MAXPUBSCREENNAME);
        std::memcpy(n.name.data(), o.pubname.c_str(), o.pubname.size() + 1);
        n.node.psn_Node.ln_Name = n.name.data();
        n.node.psn_Screen = &s;
        // as if PubScreenStatus() had made it public
        native->append(&n.node.psn_Node);
    }
    o.result = &s;
    native->opened.push_back(o);
    return &s;
}
extern "C" BOOL CloseScreen(struct Screen *screen)
{
    native->calls.push_back({ CLOSE, nullptr, screen });
    if (!screen) return TRUE;
    auto at = std::find_if(native->opened.begin(), native->opened.end(),
                           [screen](const Opened &o) { return o.result == screen; });
    assert(at != native->opened.end() && !at->closed); // only screens that were opened, once
    if (screen->FirstWindow || native->close_refuse) return FALSE;
    auto *n = native->node(screen);
    if (n->psn_Node.ln_Succ)
    {
        if (n->psn_VisitorCount) return FALSE;
        native->remove(&n->psn_Node);
    }
    at->closed = true;
    // whatever still reads it reads garbage
    std::memset(static_cast<void *>(screen), 0xEE, sizeof(*screen));
    return TRUE;
}
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *)
{
    assert(false);
    return 0;
}

/* Guest CPU and registrations. Poisoned fallbacks share every ABI slot. */
struct TestCPU
{
    ULONG d[8]{}, a[8]{};
    UWORD sr = 0xA715;
    ULONG pc = 0x12345678;
};
static ULONG get_d(void *s, int n) { return static_cast<TestCPU *>(s)->d[n]; }
static ULONG get_a(void *s, int n) { return static_cast<TestCPU *>(s)->a[n]; }
static void set_d(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->d[n] = v; }
static void set_a(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->a[n] = v; }
static UWORD get_sr(void *s) { return static_cast<TestCPU *>(s)->sr; }
static void set_sr(void *s, UWORD v) { static_cast<TestCPU *>(s)->sr = v; }
static ULONG get_pc(void *s) { return static_cast<TestCPU *>(s)->pc; }
static void set_pc(void *s, ULONG v) { static_cast<TestCPU *>(s)->pc = v; }
static const KaffeineCPUOps ops =
    { get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, nullptr, nullptr };
static unsigned generated_calls;
static IPTR poison(KaffeineContext *, void *) { ++generated_calls; return 0xBAD0F00D; }
static const M68KThunkEntry manual[] = { KAFFEINE_INTUITION_PUBSCREEN_THUNKS, KAFFEINE_INTUITION_SCREEN_THUNKS };
static const M68KThunkEntry generated[] =
    { { 510, poison }, { 516, poison }, { 522, poison }, { 528, poison }, { 534, poison }, { 582, poison },
      { 66, poison }, { 198, poison }, { 612, poison } };

/* Literal NDK offsets, independent of the bridge's private enums. */
enum
{
    SCREEN_SIZE = 346, VP = 44, RP = 84, BM = 184, TA_SIZE = 8,
    RP_FONT = 52, LH_HEAD = 0, LH_TAIL = 4, LH_TAILPRED = 8, LH_TYPE = 12,
    LIST_ROOM = 16, NODE_SIZE = 30, LN_SUCC = 0, LN_PRED = 4, LN_TYPE = 8,
    LN_PRI = 9, LN_NAME = 10, PSN_SCREEN = 14, PSN_FLAGS = 18, PSN_SIZE = 20,
    PSN_VISITORS = 22, PSN_TASK = 24, PSN_SIGBIT = 28
};
static const UWORD FILLER = 99;

struct Fixture
{
    static constexpr ULONG intuition = 0x2000, stack = 0x3F00, text = 0x3000, buffer = 0x3200;
    KaffeineContext ctx{};
    std::array<UBYTE, 0x10000> memory;
    TestCPU state;
    KaffeineCPU cpu{ &ops, &state };
    NativeIntuition *previous = native;
    NativeIntuition intuition_native;
    TextAttr attr{};
    TextFont font{};
    std::array<char, 256> font_name{};
    ULONG initial_heap;

    Fixture()
    {
        memory.fill(0xA5);
        ctx.mem = memory.data(); ctx.mem_size = memory.size();
        ctx.heap_start = 0x4000; ctx.heap_end = 0xF000;
        assert(Kaffeine_HeapInit(&ctx));
        initial_heap = avail();
        native = &intuition_native; native->ctx = &ctx;
        auto &l = ctx.libs[0];
        l.m68k_addr = intuition; l.jt_start = intuition - 0xC00;
        std::strcpy(l.name, "intuition.library");
        l.thunks = manual; l.num_thunks = std::size(manual);
        l.gen_thunks = generated; l.num_gen_thunks = std::size(generated);
        ctx.num_libs = 1;
        m68k_write32(&ctx, stack, 0x76543210);
        std::strcpy(font_name.data(), "screen.font");
        attr.ta_Name = font_name.data(); attr.ta_YSize = 19;
        attr.ta_Style = FSF_TAGGED | 0x47; attr.ta_Flags = 0xA3;
        font.tf_Message.mn_Node.ln_Type = NT_FONT;
        font.tf_Message.mn_Node.ln_Name = font_name.data();
        font.tf_YSize = 19; font.tf_XSize = 11; font.tf_Baseline = 15;
        font.tf_Style = 0x43; font.tf_Flags = 0x21; font.tf_BoldSmear = 2;
        font.tf_Accessors = 3; font.tf_LoChar = 32; font.tf_HiChar = 255;
        font.tf_Modulo = 160;
        native->font_result = &font;
        native->screen_font = &font;
    }
    ~Fixture()
    {
        native = &intuition_native;
        Kaffeine_ScreensCleanup(&ctx);
        assert(native->list_depth == 0 && native->font_opens.empty());
        native = previous;
    }
    ULONG avail() { return Kaffeine_HeapAvail(&ctx, 0); }
    UWORD word(ULONG a) { return m68k_read16(&ctx, a); }
    ULONG lng(ULONG a) { return m68k_read32(&ctx, a); }
    std::string string(ULONG a)
    {
        assert(a && a < memory.size());
        auto *p = reinterpret_cast<const char *>(memory.data() + a);
        auto *end = static_cast<const char *>(std::memchr(p, 0, memory.size() - a));
        assert(end);
        return std::string(p, end);
    }
    ULONG name(const std::string &s)
    {
        assert(s.size() < buffer - text);
        std::memcpy(memory.data() + text, s.c_str(), s.size() + 1);
        return text;
    }
    ULONG call(ULONG lvo, std::initializer_list<ULONG> a = {})
    {
        for (unsigned i = 0; i < 8; ++i)
        { state.d[i] = 0xD0000000 + i; state.a[i] = 0xA0000000 + i; }
        unsigned i = 0;
        for (ULONG v : a) state.a[i++] = v;
        state.a[7] = stack;
        const auto before = state;
        ctx.running = TRUE; ctx.exit_code = 0; generated_calls = 0;
        KaffeineDispatchResult result{};
        assert(Kaffeine_DispatchLibraryCall(&ctx, &cpu, intuition - lvo, &result));
        assert(result.return_pc == 0x76543210 && !result.supervisor && state.a[7] == stack + 4);
        for (i = 1; i < 8; ++i) assert(state.d[i] == before.d[i]);
        for (i = 0; i < 7; ++i) assert(state.a[i] == before.a[i]);
        assert(state.sr == before.sr && state.pc == before.pc && generated_calls == 0);
        return state.d[0];
    }
    bool stopped() const { return !ctx.running && ctx.exit_code == 20 && state.d[0] == 20; }
    ULONG lock(ULONG name = 0) { return call(510, { name }); }
    void unlock(ULONG screen, ULONG name = 0) { assert(call(516, { name, screen }) == 0); }
    ULONG locklist() { return call(522); }
    void unlocklist() { assert(call(528) == 0); }
    unsigned entries() const
    {
        return std::count_if(std::begin(ctx.pubscreens), std::end(ctx.pubscreens),
                             [](const auto &s) { return s.guest != 0; });
    }
    unsigned held_fonts() const
    {
        return std::count_if(std::begin(ctx.pubscreen_fonts), std::end(ctx.pubscreen_fonts),
                             [](const TextFont *font) { return font != nullptr; });
    }
    void absent(ULONG g, Screen *s)
    {
        assert(!Kaffeine_ScreenNative(&ctx, g) && !Kaffeine_ScreenGuest(&ctx, s));
        assert(!Kaffeine_ShadowNative(&ctx, g + VP, KAFFEINE_SHADOW_VIEWPORT));
        assert(!Kaffeine_ShadowNative(&ctx, g + RP, KAFFEINE_SHADOW_RASTPORT));
    }
    NativeScreen &add(const std::string &name = "Workbench", bool with_font = false)
    {
        auto &n = native->add(name);
        if (with_font) { n.screen.Font = &attr; n.screen.RastPort.Font = &font; }
        if (name == "Workbench") native->workbench = &n.screen;
        return n;
    }
    void leave_shadows(UWORD free)
    {
        static int filler;
        while (ctx.num_shadows < KAFFEINE_MAX_SHADOWS - free)
            assert(Kaffeine_ShadowAdd(&ctx, 0xF000 + 4 * ctx.num_shadows, &filler, FILLER));
    }
    void free_shadows()
    {
        for (UWORD i = 0; i < ctx.num_shadows;)
            if (ctx.shadow_map[i].type == FILLER) ctx.shadow_map[i] = ctx.shadow_map[--ctx.num_shadows];
            else ++i;
    }
    void native_unlocks(Screen *s, size_t count)
    {
        size_t found = 0;
        for (auto &c : native->calls)
            if (c.kind == UNLOCK)
            { assert(!c.pointer); if (c.screen == s) ++found; }
        assert(found == count);
    }

    /* Guest input of OpenScreen() and OpenScreenTagList(), at literal NDK
       offsets, between the name buffers and the stack */
    static constexpr ULONG ns_at = 0x3300, attr_at = 0x3340, fname_at = 0x3360, title_at = 0x3380,
        title2_at = 0x33A0, error_at = 0x33C0, error2_at = 0x33C4, pens_at = 0x33D0,
        tags_at = 0x3400, more_at = 0x3C00, ftags_at = 0x3D00, long_at = 0x3D40;
    void put(ULONG at, const std::string &s)
    {
        assert(at + s.size() + 1 <= memory.size());
        std::memcpy(memory.data() + at, s.c_str(), s.size() + 1);
    }
    void put_tags(ULONG at, std::initializer_list<std::pair<ULONG, ULONG>> items, bool done = true)
    {
        for (const auto &t : items) { m68k_write32(&ctx, at, t.first); m68k_write32(&ctx, at + 4, t.second); at += 8; }
        if (done) { m68k_write32(&ctx, at, TAG_DONE); m68k_write32(&ctx, at + 4, 0); }
    }
    void put_attr(ULONG at, ULONG name, UWORD ysize, UBYTE style, UBYTE flags, ULONG tags = 0)
    {
        m68k_write32(&ctx, at, name); m68k_write16(&ctx, at + 4, ysize);
        memory[at + 6] = style; memory[at + 7] = flags;
        if (style & FSF_TAGGED) m68k_write32(&ctx, at + 8, tags);
    }
    void put_ns(ULONG at, WORD l, WORD t, WORD w, WORD h, WORD d, UBYTE dp, UBYTE bp, UWORD modes,
                UWORD type, ULONG font, ULONG title, ULONG gadgets = 0, ULONG bitmap = 0, ULONG ext = 0)
    {
        m68k_write16(&ctx, at, l); m68k_write16(&ctx, at + 2, t); m68k_write16(&ctx, at + 4, w);
        m68k_write16(&ctx, at + 6, h); m68k_write16(&ctx, at + 8, d);
        memory[at + 10] = dp; memory[at + 11] = bp;
        m68k_write16(&ctx, at + 12, modes); m68k_write16(&ctx, at + 14, type);
        m68k_write32(&ctx, at + 16, font); m68k_write32(&ctx, at + 20, title);
        m68k_write32(&ctx, at + 24, gadgets); m68k_write32(&ctx, at + 28, bitmap);
        if (type & NS_EXTENDED) m68k_write32(&ctx, at + 32, ext);
    }
    ULONG open(ULONG ns, ULONG tags) { return call(612, { ns, tags }); }
    ULONG open_ns(ULONG ns) { return call(198, { ns }); }
    ULONG close(ULONG screen) { return call(66, { screen }); }
    int entry(ULONG g) const
    {
        for (int i = 0; i < KAFFEINE_MAX_PUBSCREENS; ++i)
            if (g && ctx.pubscreens[i].guest == g) return i;
        return -1;
    }
    unsigned owned() const
    {
        return std::count_if(std::begin(ctx.pubscreens), std::end(ctx.pubscreens),
                             [](const auto &s) { return s.guest != 0 && s.owned; });
    }
    /* Nothing reached Intuition or guest memory, and the program stopped */
    void refused(const std::array<UBYTE, 0x10000> &before) const
    {
        assert(!ctx.running && ctx.exit_code == 20 && state.d[0] == 20);
        assert(native->calls.empty() && before == memory);
        assert(ctx.num_shadows == 0 && !entries() && !ctx.screen_release);
    }
};

static void fill_fields(Screen &s, int delta)
{
    s.LeftEdge = -321 + delta; s.TopEdge = -23 + delta;
    s.Width = 641 + delta; s.Height = 479 + delta;
    s.MouseY = -103 + delta; s.MouseX = -32000 + delta;
    s.Flags = 0xA502 + delta;
    s.BarHeight = -11 + delta; s.BarVBorder = -10 + delta; s.BarHBorder = -9 + delta;
    s.MenuVBorder = -8 + delta; s.MenuHBorder = -7 + delta;
    s.WBorTop = -6 + delta; s.WBorLeft = -5 + delta;
    s.WBorRight = -4 + delta; s.WBorBottom = -3 + delta;
    s.ViewPort.DWidth = 633 + delta; s.ViewPort.DHeight = 471 + delta;
    s.ViewPort.DxOffset = -201 + delta; s.ViewPort.DyOffset = -301 + delta;
    s.ViewPort.Modes = 0x8A01 + delta; s.ViewPort.SpritePriorities = 0xDA + delta;
    s.ViewPort.ExtendedModes = 0xB4 + delta;
    s.BitMap_OBSOLETE.BytesPerRow = 0x8120 + delta; s.BitMap_OBSOLETE.Rows = 0x9234 + delta;
    s.BitMap_OBSOLETE.Flags = 0x9B + delta; s.BitMap_OBSOLETE.Depth = 7 + delta;
    s.DetailPen = 0xD4 + delta; s.BlockPen = 0xB2 + delta; s.SaveColor0 = 0xCA01 + delta;
}

/* Compare the entire NDK Screen, including every unsupported pointer and
   padding byte, against a zero-based wire image built at literal offsets. */
static void screen_image(Fixture &f, ULONG g, Screen &s, ULONG ta, ULONG tf, ULONG title = 0)
{
    std::array<UBYTE, SCREEN_SIZE> want{};
    auto w = [&](unsigned at, UWORD v) { want[at] = v >> 8; want[at + 1] = v; };
    auto l = [&](unsigned at, ULONG v) { w(at, v >> 16); w(at + 2, v); };
    w(8, s.LeftEdge); w(10, s.TopEdge); w(12, s.Width); w(14, s.Height);
    w(16, s.MouseY); w(18, s.MouseX); w(20, s.Flags);
    l(22, title); l(26, title); // an opened screen's Title and DefaultTitle: the program's text
    want[30] = s.BarHeight; want[31] = s.BarVBorder; want[32] = s.BarHBorder;
    want[33] = s.MenuVBorder; want[34] = s.MenuHBorder; want[35] = s.WBorTop;
    want[36] = s.WBorLeft; want[37] = s.WBorRight; want[38] = s.WBorBottom;
    l(40, ta);
    w(VP + 24, s.ViewPort.DWidth); w(VP + 26, s.ViewPort.DHeight);
    w(VP + 28, s.ViewPort.DxOffset); w(VP + 30, s.ViewPort.DyOffset);
    w(VP + 32, s.ViewPort.Modes); want[VP + 34] = s.ViewPort.SpritePriorities;
    want[VP + 35] = s.ViewPort.ExtendedModes;
    const auto &rp = s.RastPort;
    want[RP + 24] = rp.Mask; want[RP + 25] = rp.FgPen; want[RP + 26] = rp.BgPen;
    want[RP + 27] = rp.AOlPen; want[RP + 28] = rp.DrawMode; want[RP + 29] = rp.AreaPtSz;
    want[RP + 30] = rp.linpatcnt; w(RP + 32, rp.Flags); w(RP + 34, rp.LinePtrn);
    w(RP + 36, rp.cp_x); w(RP + 38, rp.cp_y);
    std::copy(std::begin(rp.minterms), std::end(rp.minterms), want.begin() + RP + 40);
    w(RP + 48, rp.PenWidth); w(RP + 50, rp.PenHeight); l(RP + RP_FONT, tf);
    want[RP + 56] = rp.AlgoStyle; want[RP + 57] = rp.TxFlags;
    w(RP + 58, rp.TxHeight); w(RP + 60, rp.TxWidth); w(RP + 62, rp.TxBaseline);
    w(RP + 64, rp.TxSpacing);
    // the scalars of the BitMap that the screen draws into, never the
    // obsolete copy, with pad and planes 0; RastPort.BitMap is the handle
    if (const BitMap *bm = rp.BitMap)
    {
        l(RP + 4, g + BM);
        w(BM, bm->BytesPerRow); w(BM + 2, bm->Rows);
        want[BM + 4] = bm->Flags; want[BM + 5] = bm->Depth;
    }
    // Classic SignalSemaphore at Screen.LayerInfo(224).Lock(24), already
    // valid before any native layer snapshot succeeds.
    want[256] = 15;
    l(264, g + 268); l(272, g + 264); w(292, 0xFFFF);
    want[330] = s.DetailPen; want[331] = s.BlockPen; w(332, s.SaveColor0);
    assert(g && g + want.size() <= f.memory.size());
    assert(std::equal(want.begin(), want.end(), f.memory.begin() + g));
}

static void test_registrations()
{
    static_assert(KAFFEINE_SHADOW_SCREEN == 5 && KAFFEINE_SHADOW_PUBSCREEN == 13);
    const ULONG slots[] = { 510, 516, 522, 528, 534, 582, 66, 198, 612 };
    IPTR (*const functions[])(KaffeineContext *, void *) = { Kaffeine_LockPubScreen, Kaffeine_UnlockPubScreen,
        Kaffeine_LockPubScreenList, Kaffeine_UnlockPubScreenList, Kaffeine_NextPubScreen,
        Kaffeine_GetDefaultPubScreen, Kaffeine_CloseScreen, Kaffeine_OpenScreen, Kaffeine_OpenScreenTagList };
    assert(std::size(manual) == std::size(slots));
    for (size_t i = 0; i < std::size(slots); ++i)
    { assert(manual[i].lvo == slots[i] && manual[i].thunk == functions[i]); }
    Fixture f;
    auto &n = f.add();
    ULONG g = f.lock(); assert(g);
    f.unlock(g);
    assert(f.locklist()); f.unlocklist();
    assert(f.call(534, { 0, f.buffer }) == f.buffer && f.string(f.buffer) == "Workbench");
    assert(f.call(582, { f.buffer }) == 0 && f.string(f.buffer) == "Workbench");
    f.native_unlocks(&n.screen, 1);
    // OpenScreenTagList(NULL, NULL), CloseScreen() and OpenScreen() through 612, 66 and 198
    const ULONG opened = f.open(0, 0); assert(opened && native->count(OPEN) == 1);
    assert(f.close(opened) == 1 && native->count(CLOSE) == 1);
    f.put_ns(f.ns_at, 0, 0, 320, 200, 2, 0, 1, 0, CUSTOMSCREEN, 0, 0);
    const ULONG old = f.open_ns(f.ns_at); assert(old && native->count(OPEN) == 2);
    assert(f.close(old) == 1 && native->count(CLOSE) == 2);
    // Prove that every fallback is reachable when its manual registration is absent.
    f.ctx.libs[0].num_thunks = 0;
    for (ULONG lvo : slots)
    {
        auto calls = native->calls.size();
        f.state.a[7] = f.stack;
        KaffeineDispatchResult result{};
        generated_calls = 0;
        assert(Kaffeine_DispatchLibraryCall(&f.ctx, &f.cpu, f.intuition - lvo, &result));
        assert(generated_calls == 1 && f.state.d[0] == 0xBAD0F00D && native->calls.size() == calls);
    }
}

static void test_screen_fields_and_relock()
{
    for (bool by_name : { false, true })
    {
        Fixture f;
        auto &n = f.add("Workbench", true);
        auto &s = n.screen;
        fill_fields(s, 0);
        Window pointer_window{}; Gadget pointer_gadget{};
        s.NextScreen = &s; s.FirstWindow = &pointer_window;
        s.Title = f.memory.data(); s.DefaultTitle = f.memory.data() + 1;
        s.ViewPort.Next = &s.ViewPort;
        s.ViewPort.ColorMap = reinterpret_cast<ColorMap *>(&s);
        s.ViewPort.DspIns = s.ViewPort.SprIns = s.ViewPort.ClrIns = reinterpret_cast<CopList *>(&s);
        s.ViewPort.UCopIns = reinterpret_cast<UCopList *>(&s);
        s.ViewPort.RasInfo = reinterpret_cast<RasInfo *>(&s);
        s.LayerInfo.Lock.opaque = 0xDEADBEEF;
        s.FirstGadget = &pointer_gadget; s.BarLayer = reinterpret_cast<Layer *>(&s);
        s.ExtData = s.UserData = f.memory.data();
        s.BitMap_OBSOLETE.pad = 0xBEEF;
        for (auto &p : s.BitMap_OBSOLETE.Planes) p = f.memory.data();
        // the BitMap that the screen draws into, unlike the obsolete one
        BitMap drawn{};
        drawn.BytesPerRow = 0x0A2C; drawn.Rows = 0x01E7; drawn.Flags = 0x8C; drawn.Depth = 24;
        drawn.pad = 0xFACE;
        for (auto &p : drawn.Planes) p = f.memory.data() + 0x40;
        auto &rp = s.RastPort;
        rp.Layer = s.BarLayer; rp.BitMap = &drawn;
        rp.AreaPtrn = reinterpret_cast<UWORD *>(&s); rp.TmpRas = reinterpret_cast<TmpRas *>(&s);
        rp.AreaInfo = reinterpret_cast<AreaInfo *>(&s); rp.GelsInfo = reinterpret_cast<GelsInfo *>(&s);
        rp.RP_User = reinterpret_cast<APTR *>(&s);
        rp.Mask = 0xA5; rp.FgPen = -4; rp.BgPen = -5; rp.AOlPen = -6; rp.DrawMode = -7;
        rp.AreaPtSz = -8; rp.linpatcnt = -9; rp.Flags = 0x8112; rp.LinePtrn = 0x5AA5;
        rp.cp_x = -101; rp.cp_y = -202; rp.PenWidth = -3; rp.PenHeight = 4;
        for (unsigned i = 0; i < 8; ++i) rp.minterms[i] = 0x80 + i;
        rp.AlgoStyle = 0xC3; rp.TxFlags = 0xA9; rp.TxHeight = 19; rp.TxWidth = 11;
        rp.TxBaseline = 15; rp.TxSpacing = -13;
        const ULONG a0 = by_name ? f.name("wOrKbEnCh") : 0;
        const ULONG g = f.lock(a0);
        assert(g && !f.stopped() && n.node.psn_VisitorCount == 1);
        assert(native->calls[0].kind == LOCK && native->calls[0].pointer ==
               (by_name ? f.memory.data() + a0 : nullptr));
        const ULONG ta = g + SCREEN_SIZE, tf = f.lng(g + RP + RP_FONT);
        assert(f.lng(g + 40) == ta && f.lng(ta) == tf + 52 && f.lng(tf + 10) == tf + 52);
        assert(f.word(ta + 4) == 19 && f.memory[ta + 6] == 0x47 && f.memory[ta + 7] == 0xA3);
        assert(f.string(f.lng(ta)) == "screen.font");
        assert(tf && tf != ta && Kaffeine_FontNative(&f.ctx, tf) == &f.font);
        assert(f.word(tf + 20) == 19 && f.word(tf + 24) == 11 && f.word(tf + 26) == 15);
        assert(f.string(f.lng(tf + 10)) == "screen.font");
        assert(std::any_of(std::begin(f.ctx.fonts), std::end(f.ctx.fonts),
                          [tf](const auto &font) { return font.guest == tf && font.pinned; }));
        assert(f.ctx.num_shadows == 3 && f.entries() == 1 && f.ctx.screen_release);
        assert(Kaffeine_ShadowNative(&f.ctx, g, KAFFEINE_SHADOW_PUBSCREEN) == &s);
        assert(Kaffeine_ShadowNative(&f.ctx, g + VP, KAFFEINE_SHADOW_VIEWPORT) == &s.ViewPort);
        assert(Kaffeine_ShadowNative(&f.ctx, g + RP, KAFFEINE_SHADOW_RASTPORT) == &rp);
        assert(Kaffeine_ScreenNative(&f.ctx, g) == &s && Kaffeine_ScreenGuest(&f.ctx, &s) == g);
        assert(m68k_to_host_or_shadow(&f.ctx, g) == &s &&
               m68k_to_host_or_shadow(&f.ctx, g + VP) == &s.ViewPort);
        screen_image(f, g, s, ta, tf);
        const auto heap = f.avail();
        // Guest FreeMem cannot give back a mirror, at its base or inside it.
        std::vector<UBYTE> before(f.memory.begin() + g, f.memory.begin() + ta + TA_SIZE);
        Kaffeine_HeapFree(&f.ctx, g, SCREEN_SIZE + TA_SIZE);
        Kaffeine_HeapFree(&f.ctx, g + 8, 8);
        assert(f.avail() == heap && std::equal(before.begin(), before.end(), f.memory.begin() + g));
        fill_fields(s, 1);
        drawn.BytesPerRow = 0x0B00; drawn.Rows = 0x0300; drawn.Flags = 0x0C; drawn.Depth = 8;
        f.attr.ta_YSize = 23; f.attr.ta_Style = FSF_TAGGED | 0x09; f.attr.ta_Flags = 0x57;
        std::strcpy(f.font_name.data(), "changed.font");
        // a program's edits of the BitMap handle and its RastPort field
        // give way to the next snapshot
        m68k_write32(&f.ctx, g + RP + 4, 0x1234);
        m68k_write32(&f.ctx, g + BM + 8, 0x5678);
        assert(f.lock(a0) == g && f.avail() == heap && f.ctx.num_shadows == 3 && f.entries() == 1);
        screen_image(f, g, s, ta, tf);
        assert(f.word(g + BM) == 0x0B00 && f.word(g + BM + 2) == 0x0300 && f.memory[g + BM + 5] == 8);
        assert(f.word(ta + 4) == 23 && f.memory[ta + 6] == 9 && f.memory[ta + 7] == 0x57);
        assert(f.string(f.lng(ta)) == "screen.font"); // the TextFont's pinned name
        f.unlock(g); assert(Kaffeine_ScreenGuest(&f.ctx, &s) == g);
        f.unlock(g); f.absent(g, &s);
        assert(f.ctx.num_shadows == 0 && f.entries() == 0 && Kaffeine_FontNative(&f.ctx, tf) == &f.font);
        f.native_unlocks(&s, 2);
    }
    for (bool have_attr : { false, true })
    {
        Fixture f;
        auto &n = f.add();
        fill_fields(n.screen, 0);
        if (have_attr) { n.screen.Font = &f.attr; f.attr.ta_Name = nullptr; }
        const ULONG g = f.lock(); assert(g);
        screen_image(f, g, n.screen, have_attr ? g + SCREEN_SIZE : 0, 0);
        if (have_attr)
        {
            assert(f.lng(g + SCREEN_SIZE) == 0 && f.word(g + SCREEN_SIZE + 4) == 19);
            assert(f.memory[g + SCREEN_SIZE + 6] == 0x47);
        }
        f.unlock(g); assert(f.avail() == f.initial_heap);
    }
}

/* AskFont shares the TextFont's name pointer. Equal text in another native
   buffer must instead be copied into the Screen's own allocation. */
static void test_textattr_names()
{
    for (bool shared_name : { false, true })
    {
        std::array<char, 256> separate_name{};
        Fixture f;
        auto &n = f.add("Workbench", true);
        std::strcpy(separate_name.data(), f.font_name.data());
        if (!shared_name) f.attr.ta_Name = separate_name.data();
        const ULONG g = f.lock(); assert(g);
        const ULONG ta = f.lng(g + 40), tf = f.lng(g + RP + RP_FONT);
        const ULONG size = SCREEN_SIZE + TA_SIZE + (shared_name ? 0 : 12);
        assert(ta == g + SCREEN_SIZE && f.lng(tf + 10) == tf + 52);
        assert(f.lng(ta) == (shared_name ? tf + 52 : ta + TA_SIZE));
        assert(f.string(f.lng(ta)) == "screen.font");
        // Compare allocation cost with exactly a Screen/TextAttr and TextFont,
        // including an inline name only for the distinct-pointer case.
        {
            Fixture reference;
            assert(Kaffeine_MirrorAlloc(&reference.ctx, size));
            assert(Kaffeine_FontGuest(&reference.ctx, &f.font));
            assert(f.avail() == reference.avail());
        }
        if (!shared_name)
        {
            std::strcpy(separate_name.data(), "renamed.font");
            assert(f.lock() == g && f.string(f.lng(ta)) == "screen.font");
            f.unlock(g);
        }
        const ULONG saved = 0x3600;
        std::copy_n(f.memory.begin() + ta, TA_SIZE, f.memory.begin() + saved);
        const std::array<UBYTE, TA_SIZE> copy = [&] {
            std::array<UBYTE, TA_SIZE> bytes{};
            std::copy_n(f.memory.begin() + saved, TA_SIZE, bytes.begin());
            return bytes;
        }();
        f.unlock(g); f.absent(g, &n.screen);
        const ULONG heap = f.avail();
        if (shared_name) assert(f.string(f.lng(saved)) == "screen.font");
        // Reserve and overwrite the exact freed Screen address, even when the
        // allocator could otherwise choose a different free interval.
        assert(Kaffeine_HeapAllocAbs(&f.ctx, size, g) == g);
        std::fill_n(f.memory.begin() + g, size, 0x6D);
        assert(std::equal(copy.begin(), copy.end(), f.memory.begin() + saved));
        if (shared_name)
        {
            assert(f.lng(saved) == tf + 52 && f.string(f.lng(saved)) == "screen.font");
            assert(Kaffeine_FontNative(&f.ctx, tf) == &f.font && f.held_fonts() == 1);
        }
        Kaffeine_HeapFree(&f.ctx, g, size);
        assert(f.avail() == heap);
    }
}

static void test_native_font_lifetime()
{
    Fixture f;
    auto &a = f.add("Workbench", true); auto &b = f.add("Second", true);
    // ExtendFont() marks the fonts that AROS's OpenFont() has seen.
    f.font.tf_Style |= FSF_TAGGED; f.attr.ta_YSize = 23;
    const UWORD accessors = f.font.tf_Accessors;
    const ULONG first = f.lock(), tf = f.lng(first + RP + RP_FONT);
    assert(first && f.lock() == first);
    const ULONG second = f.lock(f.name("Second")); assert(second && second != first);
    assert(native->count(FONT_OPEN) == 1 && native->count(FONT_CLOSE) == 0 && f.held_fonts() == 1);
    assert(f.font.tf_Accessors == accessors + 1 && native->font_opens[&f.font] == 1);
    // AskFont() of the RastPort font, not the screen's TextAttr
    auto opened = std::find_if(native->calls.begin(), native->calls.end(),
                               [](const Call &c) { return c.kind == FONT_OPEN; });
    assert(opened != native->calls.end() && opened->pointer != &f.attr);
    assert(native->opened_attrs.size() == 1 && native->opened_attrs[0].ta_Name == f.font_name.data() &&
           native->opened_attrs[0].ta_YSize == 19 && native->opened_attrs[0].ta_Style == (FSF_TAGGED | 0x43) &&
           native->opened_attrs[0].ta_Flags == 0x21);
    assert(native->opened_tags.size() == 1 && native->opened_tags[0] == nullptr);
    assert(f.word(first + SCREEN_SIZE + 4) == 23);
    f.unlock(first); f.unlock(first); f.unlock(second);
    assert(f.ctx.num_shadows == 0 && f.entries() == 0 && f.held_fonts() == 1 && native->count(FONT_CLOSE) == 0);
    const ULONG again = f.lock(); assert(again && f.lng(again + RP + RP_FONT) == tf);
    assert(native->count(FONT_OPEN) == 1); // a new mirror of an already-held native font
    Kaffeine_ScreensCleanup(&f.ctx);
    assert(a.node.psn_VisitorCount == 0 && b.node.psn_VisitorCount == 0);
    assert(native->count(FONT_CLOSE) == 1 && !f.held_fonts() && native->font_opens.empty());
    assert(f.font.tf_Accessors == accessors && Kaffeine_FontNative(&f.ctx, tf) == &f.font);
    const auto calls = native->calls.size();
    Kaffeine_ScreensCleanup(&f.ctx); assert(native->calls.size() == calls);
}

static void test_native_font_without_attributes()
{
    for (const auto &have : { std::pair<bool, bool>{ false, false }, { false, true }, { true, false } })
    {
        Fixture f;
        auto &n = f.add("Workbench", true);
        if (!have.first) n.screen.RastPort.Font = nullptr;
        if (!have.second) n.screen.Font = nullptr;
        const ULONG g = f.lock(); assert(g);
        const ULONG tf = f.lng(g + RP + RP_FONT);
        assert((tf != 0) == have.first);
        if (have.second)
            assert(f.lng(g + 40) == g + SCREEN_SIZE && f.lng(g + SCREEN_SIZE) == g + SCREEN_SIZE + TA_SIZE);
        else
            assert(f.lng(g + 40) == 0);
        assert(native->count(FONT_OPEN) == (have.first ? 1u : 0u) && native->count(FONT_CLOSE) == 0);
        assert(f.held_fonts() == (have.first ? 1u : 0u));
        if (have.first)
            assert(native->opened_attrs[0].ta_Name == f.font_name.data() &&
                   native->opened_attrs[0].ta_YSize == 19 && native->opened_attrs[0].ta_Style == 0x43);
        f.unlock(g); Kaffeine_ScreensCleanup(&f.ctx);
        assert(native->count(FONT_CLOSE) == (have.first ? 1u : 0u) && f.font.tf_Accessors == 3);
    }
}

static void test_native_font_other_or_null()
{
    for (bool other_result : { false, true })
    {
        TextFont other{}; other.tf_Accessors = 7;
        Fixture f;
        auto &n = f.add("Workbench", true);
        native->font_result = other_result ? &other : nullptr;
        const ULONG heap = f.avail();
        const auto before = f.memory;
        assert(!f.lock() && !f.stopped() && n.node.psn_VisitorCount == 0);
        f.native_unlocks(&n.screen, 1);
        assert(native->count(FONT_OPEN) == 1 && native->count(FONT_CLOSE) == (other_result ? 1u : 0u));
        assert(other.tf_Accessors == 7 && f.font.tf_Accessors == 3 && native->font_opens.empty() && !f.held_fonts());
        if (other_result)
            assert(std::any_of(native->calls.begin(), native->calls.end(),
                   [&](const Call &c) { return c.kind == FONT_CLOSE && c.pointer == &other; }));
        // The font comes before the 68k TextFont: nothing was pinned or left.
        assert(!Kaffeine_FontFind(&f.ctx, &f.font) && !Kaffeine_FontFind(&f.ctx, &other));
        assert(f.avail() == heap && f.memory == before && f.ctx.num_shadows == 0 && !f.entries());
        assert(!f.ctx.screen_release);
        native->font_result = &f.font;
        const ULONG g = f.lock(); assert(g && f.lock() == g);
        assert(native->count(FONT_OPEN) == 2 && f.held_fonts() == 1 && f.font.tf_Accessors == 4);
        assert(Kaffeine_FontNative(&f.ctx, f.lng(g + RP + RP_FONT)) == &f.font);
        f.unlock(g); f.unlock(g);
        Kaffeine_ScreensCleanup(&f.ctx);
        assert(f.font.tf_Accessors == 3 && native->font_opens.empty());
    }
}

/* Every held font has a pinned 68k TextFont, so the table of held fonts,
   as large as the one of the 68k TextFonts, fills only with it. A full
   table refuses a new font before OpenFont() and before a 68k TextFont. */
static void test_native_font_table_limit()
{
    static_assert(KAFFEINE_MAX_FONTS == 32);
    constexpr unsigned count = KAFFEINE_MAX_FONTS + 1;
    std::array<TextFont, count> fonts{};
    std::array<TextAttr, count> attrs{};
    std::array<std::array<char, 256>, count> names{};
    {
        Fixture f;
        std::array<NativeScreen *, count> screens{};
        for (unsigned i = 0; i < count; ++i)
        {
            std::snprintf(names[i].data(), names[i].size(), "font%u.font", i);
            fonts[i].tf_Message.mn_Node.ln_Name = names[i].data();
            fonts[i].tf_YSize = 8 + i;
            fonts[i].tf_Accessors = 10 + i;
            attrs[i].ta_Name = names[i].data(); attrs[i].ta_YSize = 8 + i;
            screens[i] = &f.add("FontScreen" + std::to_string(i));
            screens[i]->screen.Font = &attrs[i]; screens[i]->screen.RastPort.Font = &fonts[i];
        }
        for (unsigned i = 0; i < KAFFEINE_MAX_FONTS; ++i)
        {
            native->font_result = &fonts[i];
            const ULONG g = f.lock(f.name(screens[i]->name.data())); assert(g);
            assert(Kaffeine_FontNative(&f.ctx, f.lng(g + RP + RP_FONT)) == &fonts[i]);
            assert(f.held_fonts() == i + 1 && native->count(FONT_OPEN) == i + 1);
            assert(fonts[i].tf_Accessors == 11 + i);
            f.unlock(g); assert(native->count(FONT_CLOSE) == 0);
        }
        for (const auto &font : f.ctx.fonts) assert(font.guest && font.pinned);
        native->font_result = &fonts[KAFFEINE_MAX_FONTS];
        const ULONG heap = f.avail();
        assert(!f.lock(f.name(screens[KAFFEINE_MAX_FONTS]->name.data())) && !f.stopped());
        f.native_unlocks(&screens[KAFFEINE_MAX_FONTS]->screen, 1);
        assert(native->count(FONT_OPEN) == KAFFEINE_MAX_FONTS && f.held_fonts() == KAFFEINE_MAX_FONTS);
        assert(f.avail() == heap && f.ctx.num_shadows == 0 && !f.entries());
        native->font_result = &fonts[0];
        const ULONG g = f.lock(f.name("FontScreen0")); assert(g); f.unlock(g);
        assert(native->count(FONT_OPEN) == KAFFEINE_MAX_FONTS);
        Kaffeine_ScreensCleanup(&f.ctx);
        assert(native->count(FONT_CLOSE) == KAFFEINE_MAX_FONTS && !f.held_fonts() && native->font_opens.empty());
        for (unsigned i = 0; i < count; ++i)
        {
            assert(fonts[i].tf_Accessors == 10 + i);
            const auto closes = std::count_if(native->calls.begin(), native->calls.end(),
                [&](const Call &c) { return c.kind == FONT_CLOSE && c.pointer == &fonts[i]; });
            assert(closes == (i < KAFFEINE_MAX_FONTS ? 1 : 0));
        }
        const auto calls = native->calls.size();
        Kaffeine_ScreensCleanup(&f.ctx); assert(native->calls.size() == calls);
    }
    // A full table with room for 68k TextFonts still makes no 68k Screen.
    Fixture f;
    auto &n = f.add("Workbench", true);
    for (unsigned i = 0; i < KAFFEINE_MAX_FONTS; ++i)
    {
        fonts[i].tf_Accessors = 1; native->font_opens[&fonts[i]] = 1;
        f.ctx.pubscreen_fonts[i] = &fonts[i];
    }
    const ULONG heap = f.avail();
    const auto before = f.memory;
    assert(!f.lock() && !f.stopped() && n.node.psn_VisitorCount == 0);
    f.native_unlocks(&n.screen, 1);
    assert(native->count(FONT_OPEN) == 0 && !Kaffeine_FontFind(&f.ctx, &f.font));
    assert(f.avail() == heap && f.memory == before && f.ctx.num_shadows == 0 && !f.entries());
    CloseFont(f.ctx.pubscreen_fonts[5]); f.ctx.pubscreen_fonts[5] = nullptr;
    const ULONG g = f.lock(); assert(g && f.ctx.pubscreen_fonts[5] == &f.font);
    assert(native->count(FONT_OPEN) == 1 && f.font.tf_Accessors == 4);
    f.unlock(g);
    Kaffeine_ScreensCleanup(&f.ctx);
    assert(native->count(FONT_CLOSE) == KAFFEINE_MAX_FONTS + 1 && native->font_opens.empty());
    assert(f.font.tf_Accessors == 3 && fonts[0].tf_Accessors == 0);
}

static void test_missing_guest_font()
{
    for (bool full_table : { false, true })
    {
        std::array<TextFont, 32> fillers{};
        std::array<ULONG, 32> guests{};
        Fixture f;
        auto &n = f.add("Workbench", true);
        if (full_table)
        {
            static_assert(KAFFEINE_MAX_FONTS == 32);
            for (unsigned i = 0; i < fillers.size(); ++i)
            { guests[i] = Kaffeine_FontGuest(&f.ctx, &fillers[i]); assert(guests[i]); }
        }
        else
        {
            // Size the heap so the Screen/TextAttr fits, with no room left
            // for its TextFont. Probe the real allocator rather than its headers.
            const ULONG probe = Kaffeine_MirrorAlloc(&f.ctx, SCREEN_SIZE + TA_SIZE); assert(probe);
            const ULONG cost = f.initial_heap - f.avail();
            Kaffeine_MirrorRelease(&f.ctx, probe);
            f.ctx.heap_end = f.ctx.heap_start + cost;
            assert(Kaffeine_HeapInit(&f.ctx));
            const ULONG fits = Kaffeine_MirrorAlloc(&f.ctx, SCREEN_SIZE + TA_SIZE);
            assert(fits && f.avail() == 0);
            Kaffeine_MirrorRelease(&f.ctx, fits);
        }
        const ULONG heap = f.avail();
        assert(!f.lock() && !f.stopped());
        f.native_unlocks(&n.screen, 1);
        assert(n.node.psn_VisitorCount == 0 && f.avail() == heap && f.ctx.num_shadows == 0 && !f.entries());
        assert(!f.ctx.screen_release && !Kaffeine_FontFind(&f.ctx, &f.font) && !f.held_fonts());
        // held before the 68k TextFont, and given back
        assert(native->count(FONT_OPEN) == 1 && native->count(FONT_CLOSE) == 1);
        assert(f.font.tf_Accessors == 3 && native->font_opens.empty());
        for (unsigned i = 0; i < fillers.size(); ++i)
        {
            assert(f.ctx.fonts[i].guest == guests[i]);
            if (full_table)
            {
                assert(f.ctx.fonts[i].native == &fillers[i] && !f.ctx.fonts[i].pinned && !f.ctx.fonts[i].opens);
                Kaffeine_FontClosed(&f.ctx, guests[i]);
            }
        }
        if (!full_table)
        { f.ctx.heap_end = 0xF000; assert(Kaffeine_HeapInit(&f.ctx)); }
        assert(f.avail() == f.initial_heap);
        const ULONG g = f.lock(); assert(g && f.ctx.num_shadows == 3 && native->count(FONT_OPEN) == 2);
        assert(f.held_fonts() == 1);
        f.unlock(g);
    }
}

static void test_release_and_recreate()
{
    Fixture f;
    auto &n = f.add("Workbench", true);
    ULONG g = f.lock(); assert(g);
    const ULONG tf = f.lng(g + RP + RP_FONT);
    f.unlock(g);
    const ULONG baseline = f.avail(); // font remains pinned after the first lock/unlock
    assert(baseline < f.initial_heap && f.ctx.num_shadows == 0);
    // Occupy the old block, proving the next lock creates a fresh allocation.
    const ULONG blocker = Kaffeine_HeapAlloc(&f.ctx, SCREEN_SIZE + TA_SIZE + 12, MEMF_ANY);
    assert(blocker);
    const ULONG occupied = f.avail();
    g = f.lock(); assert(g && g != blocker && f.avail() < occupied);
    assert(f.lng(g + RP + RP_FONT) == tf && f.ctx.num_shadows == 3);
    assert(f.lock() == g);
    f.unlock(g); assert(f.ctx.num_shadows == 3);
    f.unlock(g); f.absent(g, &n.screen);
    assert(f.avail() == occupied && f.ctx.num_shadows == 0 && f.entries() == 0);
    Kaffeine_HeapFree(&f.ctx, blocker, SCREEN_SIZE + TA_SIZE + 12);
    assert(f.avail() == baseline && n.node.psn_VisitorCount == 0);
    f.native_unlocks(&n.screen, 3);
}

static void test_unlock_ownership_and_names()
{
    Fixture f;
    auto &first = f.add("Duplicate"); auto &second = f.add("dUPLICATE");
    auto &other = f.add("Other");
    native->default_screen = &second.screen;
    ULONG second_g = f.lock(); assert(second_g);
    auto calls = native->calls.size();
    f.unlock(0); f.unlock(0xFFFF0000); f.unlock(f.text);
    assert(native->calls.size() == calls);
    auto unlocks = native->count(UNLOCK);
    // The first node matches, even though only the second is locked: A1 ignored.
    f.unlock(second_g, f.name("DuPlIcAtE"));
    assert(native->count(UNLOCK) == unlocks && second.node.psn_VisitorCount == 1);
    f.unlock(second_g, f.name("Other"));
    f.unlock(second_g, f.name("missing"));
    assert(native->count(UNLOCK) == unlocks && f.entries() == 1 && native->list_depth == 0);
    native->default_screen = &first.screen;
    ULONG first_g = f.lock(); assert(first_g && first_g != second_g);
    f.unlock(second_g, f.name("dUpLiCaTe")); // must unlock first, never the A1 screen
    assert(first.node.psn_VisitorCount == 0 && second.node.psn_VisitorCount == 1);
    f.absent(first_g, &first.screen);
    f.native_unlocks(&first.screen, 1); f.native_unlocks(&second.screen, 0);
    f.unlock(second_g); f.native_unlocks(&second.screen, 1);
    calls = native->calls.size();
    f.unlock(second_g); f.unlock(first_g);
    assert(native->calls.size() == calls && other.node.psn_VisitorCount == 0);
}

static void test_bad_names()
{
    for (ULONG lvo : { 510u, 516u })
        for (ULONG bad : { 0xFFFFu, 0x10000u, 0xFFFFFFF0u })
        {
            Fixture f; f.add();
            const auto before = f.memory;
            assert(f.call(lvo, { bad, 0x1234 }) == 20 && f.stopped());
            assert(native->calls.empty() && f.memory == before && f.entries() == 0 && f.ctx.num_shadows == 0);
        }
    // A NUL at the very last byte is inside guest memory, including an empty name there.
    for (ULONG lvo : { 510u, 516u })
    {
        Fixture f; f.add("X");
        f.memory[0xFFFE] = 'X'; f.memory[0xFFFF] = 0;
        assert(f.call(lvo, { 0xFFFE, 0 }) != 20 && !f.stopped());
        assert(!native->calls.empty());
        assert(f.call(lvo, { 0xFFFF, 0 }) == 0 && !f.stopped());
    }
}

static void test_window_retention()
{
    for (bool same_screen : { false, true })
    {
        Fixture f;
        auto &s = f.add(); auto &other = f.add("Other");
        const ULONG g = f.lock(); assert(g && f.ctx.screen_release);
        const ULONG used = f.avail();
        Window w{}; w.WScreen = same_screen ? &s.screen : &other.screen;
        const ULONG window = 0x3500;
        assert(Kaffeine_ShadowAdd(&f.ctx, window, &w, KAFFEINE_SHADOW_WINDOW));
        f.unlock(g);
        if (same_screen)
        {
            assert(f.avail() == used && Kaffeine_ScreenGuest(&f.ctx, &s.screen) == g);
            const auto unlocks = native->count(UNLOCK);
            f.unlock(g); f.unlock(g, f.name("workbench"));
            assert(native->count(UNLOCK) == unlocks && s.node.psn_VisitorCount == 0 &&
                   Kaffeine_ScreenGuest(&f.ctx, &s.screen) == g);
        }
        else
        { f.absent(g, &s.screen); assert(f.avail() == f.initial_heap); }
        // The real WindowDelete ordering is tested separately in test_window.
        f.ctx.screen_release(&f.ctx, w.WScreen); // a live Window still retains it
        if (same_screen) assert(Kaffeine_ScreenGuest(&f.ctx, &s.screen) == g);
        Kaffeine_ShadowForget(&f.ctx, window, KAFFEINE_SHADOW_WINDOW);
        f.ctx.screen_release(&f.ctx, w.WScreen);
        f.absent(g, &s.screen);
        assert(f.avail() == f.initial_heap && f.ctx.num_shadows == 0 && f.entries() == 0);
        f.ctx.screen_release(&f.ctx, w.WScreen); f.ctx.screen_release(&f.ctx, nullptr);
        assert(f.avail() == f.initial_heap);
    }
}

static void test_owned_screen()
{
    Fixture f;
    auto &n = f.add("Workbench", true);
    const ULONG g = Kaffeine_HeapAlloc(&f.ctx, SCREEN_SIZE, MEMF_ANY);
    assert(g);
    std::fill_n(f.memory.begin() + g, SCREEN_SIZE, 0x6B);
    const auto before = f.memory;
    const ULONG heap = f.avail();
    assert(Kaffeine_ShadowAdd(&f.ctx, g, &n.screen, KAFFEINE_SHADOW_SCREEN));
    assert(f.lock() == g && f.lock() == g && f.memory == before && f.ctx.num_shadows == 1 && f.avail() == heap);
    assert(Kaffeine_ScreenNative(&f.ctx, g) == &n.screen && Kaffeine_ScreenGuest(&f.ctx, &n.screen) == g);
    f.unlock(g); f.unlock(g);
    assert(f.memory == before && f.avail() == heap && f.ctx.num_shadows == 1 && f.entries() == 0);
    assert(Kaffeine_ScreenGuest(&f.ctx, &n.screen) == g);
    // Lookup order is type 5 before type 13, even if the latter was inserted first.
    const ULONG pub = 0x3600;
    Kaffeine_ShadowForget(&f.ctx, g, KAFFEINE_SHADOW_SCREEN);
    assert(Kaffeine_ShadowAdd(&f.ctx, pub, &n.screen, KAFFEINE_SHADOW_PUBSCREEN));
    assert(Kaffeine_ShadowAdd(&f.ctx, g, &n.screen, KAFFEINE_SHADOW_SCREEN));
    assert(Kaffeine_ScreenGuest(&f.ctx, &n.screen) == g && Kaffeine_ScreenNative(&f.ctx, pub) == &n.screen);
    Screen other{};
    assert(Kaffeine_ShadowAdd(&f.ctx, g, &other, KAFFEINE_SHADOW_PUBSCREEN));
    assert(Kaffeine_ScreenNative(&f.ctx, g) == &n.screen); // type 5 wins in this direction too
    Kaffeine_ShadowForget(&f.ctx, g, KAFFEINE_SHADOW_PUBSCREEN);
    Kaffeine_ShadowForget(&f.ctx, pub, KAFFEINE_SHADOW_PUBSCREEN);
    Kaffeine_ShadowForget(&f.ctx, g, KAFFEINE_SHADOW_SCREEN);
    Kaffeine_HeapFree(&f.ctx, g, SCREEN_SIZE);
    assert(f.avail() == f.initial_heap);
}

static void test_lock_failures()
{
    for (int free : { -1, 0, 1, 2 })
    for (bool with_font : { false, true })
    {
        Fixture f; auto &s = f.add("Workbench", with_font);
        ULONG taken = 0, size = 0;
        if (free < 0)
        { size = Kaffeine_HeapAvail(&f.ctx, MEMF_LARGEST); taken = Kaffeine_HeapAlloc(&f.ctx, size, MEMF_ANY); assert(taken); }
        else f.leave_shadows(free);
        const ULONG heap = f.avail(); const auto shadows = f.ctx.num_shadows;
        const auto before = f.memory;
        assert(f.lock() == 0 && !f.stopped() && s.node.psn_VisitorCount == 0);
        assert(f.entries() == 0 && f.ctx.num_shadows == shadows && f.avail() == heap && f.memory == before);
        f.native_unlocks(&s.screen, 1);
        assert(!f.ctx.screen_release && !f.held_fonts() && native->font_opens.empty());
        // the shadow entries before the font, the font before the memory
        assert(native->count(FONT_OPEN) == (with_font && free < 0 ? 1u : 0u));
        assert(native->count(FONT_CLOSE) == native->count(FONT_OPEN) && f.font.tf_Accessors == 3);
        if (taken) Kaffeine_HeapFree(&f.ctx, taken, size);
        f.free_shadows();
        // Exactly three entries suffice, and a failed entry did not poison the next lock.
        f.leave_shadows(3);
        const ULONG g = f.lock(); assert(g && f.ctx.num_shadows == KAFFEINE_MAX_SHADOWS);
        assert(f.held_fonts() == (with_font ? 1u : 0u));
        f.unlock(g); f.free_shadows();
        assert(f.entries() == 0 && f.ctx.num_shadows == 0);
        // the TextFont of a screen font stays pinned
        assert(with_font ? f.avail() < f.initial_heap : f.avail() == f.initial_heap);
    }
    Fixture f;
    f.add("Private").node.psn_Flags = PSNF_PRIVATE;
    assert(!f.lock(f.name("private")) && !f.lock(f.name("absent")) && !f.lock());
    assert(native->count(LOCK) == 3 && native->count(UNLOCK) == 0 && f.entries() == 0 &&
           f.ctx.num_shadows == 0 && f.avail() == f.initial_heap && !f.ctx.screen_release);
}

static void test_screen_table_limit()
{
    Fixture f;
    std::array<ULONG, 16> guests{};
    for (unsigned i = 0; i < guests.size(); ++i)
    {
        auto &s = f.add("Screen" + std::to_string(i));
        guests[i] = f.lock(f.name(s.name.data())); assert(guests[i]);
    }
    auto &extra = f.add("Extra");
    const ULONG heap = f.avail(); const auto shadows = f.ctx.num_shadows;
    const auto before = f.memory;
    assert(f.entries() == 16 && !f.lock(f.name("Extra")) && !f.stopped());
    // name() itself changed the guest buffer before the failed lock.
    assert(f.avail() == heap && f.ctx.num_shadows == shadows && extra.node.psn_VisitorCount == 0);
    assert(std::equal(before.begin() + 0x4000, before.end(), f.memory.begin() + 0x4000));
    f.native_unlocks(&extra.screen, 1);
    f.unlock(guests[0]);
    const ULONG g = f.lock(f.name("Extra")); assert(g && f.entries() == 16);
    f.unlock(g);
    for (unsigned i = 1; i < guests.size(); ++i) f.unlock(guests[i]);
    assert(f.avail() == f.initial_heap && f.ctx.num_shadows == 0 && f.entries() == 0);
}

static void test_screen_lock_limit()
{
    Fixture f; auto &n = f.add();
    const ULONG g = f.lock(); assert(g);
    for (unsigned i = 1; i < 32767; ++i) assert(f.lock() == g);
    const ULONG heap = f.avail(); const auto before = f.memory;
    assert(!f.lock() && !f.stopped() && f.ctx.pubscreens[0].locks == 32767);
    assert(n.node.psn_VisitorCount == 32767 && f.memory == before && f.avail() == heap && f.ctx.num_shadows == 3);
    f.native_unlocks(&n.screen, 1);
    for (unsigned i = 0; i < 32767; ++i) f.unlock(g);
    f.native_unlocks(&n.screen, 32768);
    f.absent(g, &n.screen);
    assert(f.avail() == f.initial_heap && f.entries() == 0 && n.node.psn_VisitorCount == 0);
}

/* List wire image: 14-byte List in 16 bytes, 30-byte nodes, then names. */
static void list_image(Fixture &f, ULONG list, size_t count,
                       const std::vector<std::string> &copied_names = {})
{
    assert(list && count <= 256 && f.ctx.pubscreen_list_nodes == count && f.lng(list + LH_TAIL) == 0 &&
           f.memory[list + LH_TYPE] == native->list.lh_Type && f.memory[list + 13] == 0 &&
           f.word(list + 14) == 0);
    assert(f.lng(list + LH_HEAD) == list + (count ? LIST_ROOM : LH_TAIL));
    assert(f.lng(list + LH_TAILPRED) == (count ? list + LIST_ROOM + (count - 1) * NODE_SIZE : list));
    ULONG names = list + LIST_ROOM + count * NODE_SIZE;
    for (size_t i = 0; i < count; ++i)
    {
        auto &n = native->screens[i].node;
        const ULONG node = list + LIST_ROOM + i * NODE_SIZE;
        assert(f.lng(node + LN_SUCC) == (i + 1 < count ? node + NODE_SIZE : list + LH_TAIL));
        assert(f.lng(node + LN_PRED) == (i ? node - NODE_SIZE : list + LH_HEAD));
        assert(f.memory[node + LN_TYPE] == n.psn_Node.ln_Type && f.memory[node + LN_PRI] == (UBYTE)n.psn_Node.ln_Pri);
        assert(f.lng(node + LN_NAME) == names);
        const std::string name = !copied_names.empty() ? copied_names[i] :
                                n.psn_Node.ln_Name ? std::string(n.psn_Node.ln_Name).substr(0, 139) : "";
        assert(f.string(names) == name);
        names += name.size() + 1;
        assert(f.lng(node + PSN_SCREEN) == Kaffeine_ScreenGuest(&f.ctx, n.psn_Screen));
        assert(f.word(node + PSN_FLAGS) == n.psn_Flags && f.word(node + PSN_SIZE) == (UWORD)n.psn_Size &&
               f.word(node + PSN_VISITORS) == (UWORD)n.psn_VisitorCount);
        assert(f.lng(node + PSN_TASK) == 0 && f.memory[node + PSN_SIGBIT] == n.psn_SigBit &&
               f.memory[node + 29] == 0);
    }
}

static void test_lists()
{
    Fixture f;
    auto &held = f.add("Workbench");
    auto &long_name = f.add(std::string(139, 'N'));
    auto &private_screen = f.add(std::string(200, 'P'));
    auto &nameless = f.add("ignored"); nameless.node.psn_Node.ln_Name = nullptr;
    private_screen.node.psn_Flags = PSNF_PRIVATE | 0x8100;
    private_screen.node.psn_VisitorCount = -23;
    const ULONG g = f.lock(); assert(g);
    const ULONG owned = Kaffeine_HeapAlloc(&f.ctx, SCREEN_SIZE, MEMF_CLEAR);
    assert(owned && Kaffeine_ShadowAdd(&f.ctx, owned, &long_name.screen, KAFFEINE_SHADOW_SCREEN));
    const ULONG heap = f.avail();
    const ULONG list = f.locklist(); assert(list && native->list_depth == 1 && f.ctx.pubscreen_list_locks == 1);
    list_image(f, list, 4);
    assert(f.lng(list + LIST_ROOM + PSN_SCREEN) == g &&
           f.lng(list + LIST_ROOM + NODE_SIZE + PSN_SCREEN) == owned &&
           f.lng(list + LIST_ROOM + 2 * NODE_SIZE + PSN_SCREEN) == 0);
    const ULONG used = f.avail();
    const auto snapshot = f.memory;
    private_screen.node.psn_Size = -42;
    private_screen.node.psn_Node.ln_Type = 0x51;
    private_screen.node.psn_Node.ln_Pri = 27;
    private_screen.node.psn_Flags = PSNF_PRIVATE | 0x4200;
    private_screen.node.psn_VisitorCount = -99;
    private_screen.node.psn_SigBit = 7;
    const ULONG mapped = 0x3500, third = list + LIST_ROOM + 2 * NODE_SIZE;
    assert(Kaffeine_ShadowAdd(&f.ctx, mapped, &private_screen.screen, KAFFEINE_SHADOW_PUBSCREEN));
    // It is one owned allocation, immune to guest FreeMem, and reused under nesting.
    Kaffeine_HeapFree(&f.ctx, list, 16 + 4 * 30 + 10 + 140 + 140 + 1);
    assert(f.avail() == used && f.locklist() == list && f.avail() == used && native->list_depth == 2);
    assert(f.memory != snapshot && f.word(third + PSN_SIZE) == (UWORD)-42 &&
           f.lng(third + PSN_SCREEN) == mapped && f.word(third + PSN_VISITORS) == (UWORD)-99);
    list_image(f, list, 4);
    f.unlock(g);
    assert(held.node.psn_VisitorCount == 0 && Kaffeine_ScreenGuest(&f.ctx, &held.screen) == g);
    private_screen.node.psn_Node.ln_Name = const_cast<char *>("renamed");
    assert(f.locklist() == list && f.avail() == used && native->list_depth == 3);
    assert(f.word(list + LIST_ROOM + PSN_VISITORS) == 0);
    list_image(f, list, 4, { "Workbench", std::string(139, 'N'), std::string(139, 'P'), "" });
    // Links, node addresses, name pointers and text stay fixed across refreshes.
    for (unsigned i = 0; i < 4; ++i)
    {
        const ULONG node = list + LIST_ROOM + i * NODE_SIZE;
        assert(std::equal(snapshot.begin() + node, snapshot.begin() + node + 8, f.memory.begin() + node));
        assert(std::equal(snapshot.begin() + node + LN_NAME, snapshot.begin() + node + LN_NAME + 4,
                          f.memory.begin() + node + LN_NAME));
    }
    f.unlocklist();
    f.unlocklist();
    assert(f.ctx.pubscreen_list == list && f.ctx.pubscreen_list_locks == 1 && f.avail() == used);
    f.unlocklist(); f.absent(g, &held.screen);
    assert(f.ctx.pubscreen_list == 0 && f.ctx.pubscreen_list_nodes == 0 &&
           f.ctx.pubscreen_list_locks == 0 && native->list_depth == 0);
    assert(Kaffeine_ScreenGuest(&f.ctx, &long_name.screen) == owned);
    assert(f.avail() > heap); // the screen block was also returned
    const ULONG after = f.avail(); auto calls = native->calls.size();
    f.unlocklist(); assert(native->calls.size() == calls && f.avail() == after);
    Kaffeine_ShadowForget(&f.ctx, owned, KAFFEINE_SHADOW_SCREEN);
    Kaffeine_ShadowForget(&f.ctx, mapped, KAFFEINE_SHADOW_PUBSCREEN);
    Kaffeine_HeapFree(&f.ctx, owned, SCREEN_SIZE);
    assert(f.avail() == f.initial_heap && f.ctx.num_shadows == 0);
    // A later list lock copies fresh native fields instead of reusing the old snapshot.
    private_screen.node.psn_Size = -42;
    private_screen.node.psn_Node.ln_Name = private_screen.name.data();
    const ULONG again = f.locklist(); list_image(f, again, 4); f.unlocklist();
    assert(f.avail() == f.initial_heap);
}

/* A guest can change its own native list under the recursive list lock.
   A copy with a different node count must not be partly refreshed. */
static void test_list_count_changes()
{
    for (bool grows : { false, true })
    {
        Fixture f;
        auto &first = f.add(); f.add("Second"); auto &third = f.add("Third");
        const ULONG list = f.locklist(); assert(list && f.ctx.pubscreen_list_nodes == 3);
        const ULONG heap = f.avail(); const auto before = f.memory;
        first.node.psn_Node.ln_Type = 0x91; first.node.psn_Node.ln_Pri = -31;
        first.node.psn_Flags = 0x8120; first.node.psn_Size = -73;
        first.node.psn_VisitorCount = 123; first.node.psn_SigBit = 5;
        assert(Kaffeine_ShadowAdd(&f.ctx, 0x3500, &first.screen, KAFFEINE_SHADOW_PUBSCREEN));
        Node *changed;
        if (grows) changed = &f.add("Fourth").node.psn_Node;
        else { changed = &third.node.psn_Node; native->remove(changed); }
        assert(f.locklist() == list && !f.stopped() && f.memory == before && f.avail() == heap);
        assert(f.ctx.pubscreen_list_nodes == 3 && f.ctx.pubscreen_list_locks == 2 && native->list_depth == 2);
        if (grows) native->remove(changed);
        else native->append(changed);
        assert(f.locklist() == list && f.avail() == heap && f.memory != before);
        list_image(f, list, 3);
        assert(f.lng(list + LIST_ROOM + PSN_SCREEN) == 0x3500);
        f.unlocklist(); f.unlocklist(); f.unlocklist();
        assert(f.avail() == f.initial_heap && f.ctx.pubscreen_list_nodes == 0);
        Kaffeine_ShadowForget(&f.ctx, 0x3500, KAFFEINE_SHADOW_PUBSCREEN);
    }
}

static void test_empty_and_capped_lists()
{
    {
        Fixture f;
        const ULONG list = f.locklist(); list_image(f, list, 0);
        assert(f.locklist() == list);
        f.unlocklist(); f.unlocklist();
        assert(f.avail() == f.initial_heap && native->list_depth == 0);
    }
    Fixture f;
    for (unsigned i = 0; i < 257; ++i) f.add("S" + std::to_string(i));
    const ULONG list = f.locklist(); list_image(f, list, 256);
    f.unlocklist(); assert(f.avail() == f.initial_heap);
}

static void test_list_allocation_failure()
{
    Fixture f; f.add();
    const ULONG size = Kaffeine_HeapAvail(&f.ctx, MEMF_LARGEST);
    const ULONG taken = Kaffeine_HeapAlloc(&f.ctx, size, MEMF_ANY); assert(taken);
    const ULONG heap = f.avail(); const auto before = f.memory;
    assert(f.locklist() == 0 && !f.stopped() && f.ctx.pubscreen_list_locks == 1 && native->list_depth == 1);
    assert(f.locklist() == 0 && f.ctx.pubscreen_list_locks == 2 && native->list_depth == 2);
    assert(f.ctx.pubscreen_list == 0 && f.memory == before && f.avail() == heap);
    f.unlocklist(); assert(native->list_depth == 1);
    f.unlocklist(); assert(native->list_depth == 0);
    auto calls = native->calls.size(); f.unlocklist(); assert(native->calls.size() == calls);
    Kaffeine_HeapFree(&f.ctx, taken, size);
    assert(f.avail() == f.initial_heap);
    const ULONG list = f.locklist(); assert(list); f.unlocklist();
    assert(f.avail() == f.initial_heap);
}

static void test_list_lock_limit()
{
    Fixture f;
    const ULONG list = f.locklist(); assert(list);
    for (unsigned i = 1; i < 32767; ++i) assert(f.locklist() == list);
    const auto calls = native->calls.size(); const ULONG heap = f.avail();
    assert(f.locklist() == 20 && f.stopped() && native->calls.size() == calls);
    assert(f.ctx.pubscreen_list_locks == 32767 && native->list_depth == 32767 && f.avail() == heap);
    for (unsigned i = 0; i < 32767; ++i) f.unlocklist();
    assert(native->list_depth == 0 && f.avail() == f.initial_heap && f.ctx.pubscreen_list == 0);
}

static void test_next_screen()
{
    Fixture f;
    auto &first = f.add("Workbench");
    auto &second = f.add(std::string(200, 'Z'));
    second.node.psn_Flags = PSNF_PRIVATE;
    const ULONG g = f.lock(); assert(g);
    const ULONG second_g = 0x3500, nonpublic_g = 0x3700;
    Screen nonpublic{};
    assert(Kaffeine_ShadowAdd(&f.ctx, second_g, &second.screen, KAFFEINE_SHADOW_SCREEN));
    assert(Kaffeine_ShadowAdd(&f.ctx, nonpublic_g, &nonpublic, KAFFEINE_SHADOW_SCREEN));
    const auto shadows = f.ctx.num_shadows; const ULONG heap = f.avail();
    for (auto c : { std::pair<ULONG, std::string>{ 0, "Workbench" }, { g, std::string(139, 'Z') } })
    {
        std::fill_n(f.memory.begin() + f.buffer - 1, 142, 0xA5);
        const auto locks = native->count(LIST_LOCK), unlocks = native->count(LIST_UNLOCK);
        assert(f.call(534, { c.first, f.buffer }) == f.buffer && f.string(f.buffer) == c.second);
        assert(f.memory[f.buffer - 1] == 0xA5 && f.memory[f.buffer + 140] == 0xA5);
        assert(native->count(LIST_LOCK) == locks + 1 && native->count(LIST_UNLOCK) == unlocks + 1);
    }
    for (ULONG screen : { second_g, nonpublic_g })
        for (ULONG buffer : { f.buffer, 0u, 0x10000u, 0xFFFFFFF0u })
    {
        const auto before = f.memory;
        assert(f.call(534, { screen, buffer }) == 0 && !f.stopped() && f.memory == before);
    }
    assert(f.ctx.pubscreen_list_locks == 0 && f.ctx.pubscreen_list == 0 && native->list_depth == 0 &&
           f.ctx.num_shadows == shadows && f.avail() == heap && native->count(NEXT) == 0);
    assert(Kaffeine_ScreenNative(&f.ctx, g) == &first.screen);
    f.unlock(g);
    Fixture empty;
    const auto before = empty.memory;
    for (ULONG buffer : { empty.buffer, 0u, 0x10000u, 0xFFFFFFF0u })
        assert(!empty.call(534, { 0, buffer }) && !empty.stopped() && empty.memory == before && native->list_depth == 0);
}

static void test_default_screen()
{
    Fixture f;
    auto &s = f.add(std::string(139, 'D'));
    for (bool have_default : { false, true })
    {
        native->default_screen = have_default ? &s.screen : nullptr;
        std::fill_n(f.memory.begin() + f.buffer - 1, 142, 0xA5);
        const auto calls = native->count(DEFAULT);
        assert(f.call(582, { f.buffer }) == 0 && !f.stopped());
        assert(f.string(f.buffer) == (have_default ? std::string(139, 'D') : "Workbench"));
        assert(f.memory[f.buffer - 1] == 0xA5 && f.memory[f.buffer + 140] == 0xA5);
        assert(native->count(DEFAULT) == calls + 1 && native->list_depth == 0);
    }
    const auto calls = native->calls.size(); const auto before = f.memory;
    assert(f.call(582, { 0 }) == 0 && !f.stopped() && native->calls.size() == calls && f.memory == before);
    assert(f.ctx.num_shadows == 0 && f.entries() == 0 && f.avail() == f.initial_heap);
}

static void test_name_buffers()
{
    for (ULONG lvo : { 534u, 582u })
    {
        for (const std::string &name : { std::string("Workbench"), std::string(139, 'E') })
            for (bool fits : { false, true })
            {
                Fixture f;
                auto &n = f.add(name); native->default_screen = &n.screen;
                const ULONG buffer = f.memory.size() - (name.size() + (fits ? 1 : 0));
                const auto before = f.memory;
                const ULONG result = f.call(lvo, lvo == 534 ? std::initializer_list<ULONG>{ 0, buffer } :
                                                            std::initializer_list<ULONG>{ buffer });
                assert(native->count(LIST_LOCK) == 1 && native->count(LIST_UNLOCK) == 1 && native->list_depth == 0);
                assert(native->count(DEFAULT) == (lvo == 582 ? 1u : 0u) && native->count(NEXT) == 0);
                if (fits)
                {
                    auto expected = before;
                    std::copy(name.begin(), name.end(), expected.begin() + buffer);
                    expected.back() = 0;
                    assert(result == (lvo == 534 ? buffer : 0) && !f.stopped() && f.memory == expected);
                    assert(f.string(buffer) == name); // last byte is precisely the name's NUL
                }
                else
                    assert(result == 20 && f.stopped() && f.memory == before);
                assert(f.ctx.num_shadows == 0 && !f.entries() && f.avail() == f.initial_heap);
            }
        // Plenty of room for Workbench, despite having fewer than 140 bytes.
        Fixture short_name; short_name.add();
        const ULONG buffer = short_name.memory.size() - 139;
        assert(short_name.call(lvo, lvo == 534 ? std::initializer_list<ULONG>{ 0, buffer } :
                                               std::initializer_list<ULONG>{ buffer }) == (lvo == 534 ? buffer : 0));
        assert(!short_name.stopped() && short_name.string(buffer) == "Workbench");
        for (ULONG bad : { 0x10000u, 0xFFFFFFF0u })
        {
            Fixture f; f.add(); const auto before = f.memory;
            assert(f.call(lvo, lvo == 534 ? std::initializer_list<ULONG>{ 0, bad } :
                                           std::initializer_list<ULONG>{ bad }) == 20 && f.stopped());
            assert(f.memory == before && native->list_depth == 0 &&
                   native->count(LIST_LOCK) == 1 && native->count(LIST_UNLOCK) == 1);
            // Default names are validated after the native call; Next after its walk.
            assert(native->count(DEFAULT) == (lvo == 582 ? 1u : 0u));
        }
    }
    // A NULL Next buffer writes nothing and returns zero after a valid walk.
    {
        Fixture f; auto &n = f.add();
        const ULONG owned = 0x3500;
        assert(Kaffeine_ShadowAdd(&f.ctx, owned, &n.screen, KAFFEINE_SHADOW_SCREEN));
        for (ULONG screen : { 0u, owned })
        {
            const auto before = f.memory; const auto locks = native->count(LIST_LOCK);
            assert(f.call(534, { screen, 0 }) == 0 && !f.stopped() && f.memory == before);
            assert(native->count(LIST_LOCK) == locks + 1 && native->list_depth == 0);
        }
    }
    for (ULONG bad : { Fixture::text, 0x10000u, 0xFFFFFFF0u })
        for (ULONG buffer : { 0u, Fixture::buffer })
        {
            Fixture f; const auto before = f.memory;
            assert(f.call(534, { bad, buffer }) == 20 && f.stopped() && native->calls.empty() && f.memory == before);
        }
    // A shadow of another type is still not a Screen, even with no name buffer.
    Fixture f; Window w{};
    assert(Kaffeine_ShadowAdd(&f.ctx, f.text, &w, KAFFEINE_SHADOW_WINDOW));
    for (ULONG buffer : { 0u, Fixture::buffer })
        assert(f.call(534, { f.text, buffer }) == 20 && f.stopped() && native->calls.empty());
}

static void test_cleanup()
{
    Fixture f;
    auto &a = f.add("Workbench", true); auto &b = f.add("Second", true);
    const ULONG first = f.lock(), second = f.lock(f.name("Second")); assert(first && second);
    assert(f.lock() == first);
    assert(f.locklist() && f.locklist());
    assert(native->count(FONT_OPEN) == 1 && f.held_fonts() == 1);
    const auto unlocks = native->count(UNLOCK), list_unlocks = native->count(LIST_UNLOCK);
    Kaffeine_ScreensCleanup(&f.ctx);
    assert(a.node.psn_VisitorCount == 0 && b.node.psn_VisitorCount == 0 && native->list_depth == 0);
    assert(native->count(UNLOCK) == unlocks + 3 && native->count(LIST_UNLOCK) == list_unlocks + 5);
    assert(f.ctx.pubscreen_list_locks == 0);
    assert(native->count(FONT_CLOSE) == 1 && !f.held_fonts() && f.font.tf_Accessors == 3);
    for (const auto &s : f.ctx.pubscreens) assert(s.locks == 0);
    f.native_unlocks(&a.screen, 2); f.native_unlocks(&b.screen, 1);
    const auto calls = native->calls.size();
    Kaffeine_ScreensCleanup(&f.ctx); assert(native->calls.size() == calls);
    // Cleanup runs at program end; mirrors belong to the context until destruction.
    Fixture empty;
    Kaffeine_ScreensCleanup(&empty.ctx); assert(native->calls.empty());
}

static void test_list_without_copy_retains_screen()
{
    Fixture f; auto &s = f.add();
    const ULONG g = f.lock(); assert(g);
    const ULONG size = Kaffeine_HeapAvail(&f.ctx, MEMF_LARGEST);
    const ULONG taken = Kaffeine_HeapAlloc(&f.ctx, size, MEMF_ANY); assert(taken);
    assert(f.locklist() == 0 && f.ctx.pubscreen_list_locks == 1 && native->list_depth == 1);
    f.unlock(g);
    // The held native list keeps every 68k Screen, also when list_copy()
    // could not allocate the copy.
    assert(Kaffeine_ScreenGuest(&f.ctx, &s.screen) == g);
    f.unlocklist(); f.absent(g, &s.screen);
    Kaffeine_HeapFree(&f.ctx, taken, size);
    assert(f.avail() == f.initial_heap && f.ctx.num_shadows == 0 && f.entries() == 0);
}

/* ── Screens that the program opens ──────────────────────────────── */

static void be32(std::array<UBYTE, 0x10000> &m, ULONG at, ULONG v)
{
    m[at] = v >> 24; m[at + 1] = v >> 16; m[at + 2] = v >> 8; m[at + 3] = v;
}

/* OpenScreen(): every NewScreen byte field reaches Intuition, which keeps
   native copies of the font and the title; the 68k Screen is the complete
   classic image, of type 5, and goes with a successful CloseScreen(). */
static void test_open_newscreen()
{
    Fixture f;
    f.put(f.fname_at, "topaz.font");
    f.put_attr(f.attr_at, f.fname_at, 8, 0x05, 0x42);
    f.put(f.title_at, "My Screen");
    f.put_ns(f.ns_at, -5, 7, 320, 200, 3, 2, 1, 0x8004, CUSTOMSCREEN | SHOWTITLE, f.attr_at, f.title_at);
    const ULONG g = f.open_ns(f.ns_at);
    assert(g && !f.stopped() && native->count(OPEN) == 1 && native->opened.size() == 1);
    const Opened &o = native->opened.back();
    assert(o.had_ns && !o.had_tags);
    assert(o.ns.LeftEdge == -5 && o.ns.TopEdge == 7 && o.ns.Width == 320 && o.ns.Height == 200);
    assert(o.ns.Depth == 3 && o.ns.DetailPen == 2 && o.ns.BlockPen == 1 && o.ns.ViewModes == 0x8004);
    assert(o.ns.Type == (CUSTOMSCREEN | SHOWTITLE) && !o.ns.Gadgets && !o.ns.CustomBitMap);
    assert(o.font_name == "topaz.font" && o.font_copy.ta_YSize == 8 && o.font_copy.ta_Style == 0x05 &&
           o.font_copy.ta_Flags == 0x42 && o.font_tags.empty());
    assert(o.title_text == "My Screen");
    Screen *s = o.result;
    assert(Kaffeine_ShadowNative(&f.ctx, g, KAFFEINE_SHADOW_SCREEN) == s);
    assert(!Kaffeine_ShadowNative(&f.ctx, g, KAFFEINE_SHADOW_PUBSCREEN));
    assert(Kaffeine_ShadowNative(&f.ctx, g + VP, KAFFEINE_SHADOW_VIEWPORT) == &s->ViewPort);
    assert(Kaffeine_ShadowNative(&f.ctx, g + RP, KAFFEINE_SHADOW_RASTPORT) == &s->RastPort);
    assert(Kaffeine_ScreenNative(&f.ctx, g) == s && Kaffeine_ScreenGuest(&f.ctx, s) == g);
    assert(f.ctx.num_shadows == 3 && f.entries() == 1 && f.owned() == 1 && f.ctx.screen_release);
    const ULONG ta = g + SCREEN_SIZE, tf = f.lng(g + RP + RP_FONT);
    assert(tf && Kaffeine_FontNative(&f.ctx, tf) == &f.font && f.held_fonts() == 1);
    // all 346 bytes: scalars, ViewPort, RastPort, BitMap and pens; no native pointer
    screen_image(f, g, *s, ta, tf, f.title_at);
    assert(f.lng(ta) == ta + TA_SIZE && f.string(ta + TA_SIZE) == "topaz.font");
    assert(f.word(ta + 4) == 8 && f.memory[ta + 6] == 0x05 && f.memory[ta + 7] == 0x42);
    // Intuition's pointers are native copies that outlive the program's text.
    f.put(f.fname_at, "other.font"); f.put(f.title_at, "Changed!!");
    std::fill_n(f.memory.begin() + f.ns_at, 32, 0x5A);
    assert(std::string(s->Font->ta_Name) == "topaz.font" &&
           std::string(reinterpret_cast<char *>(s->Title)) == "My Screen");
    // guest FreeMem cannot give back the mirror
    const ULONG used = f.avail();
    Kaffeine_HeapFree(&f.ctx, g, SCREEN_SIZE); assert(f.avail() == used);
    assert(f.close(g) == 1 && native->count(CLOSE) == 1 && native->calls.back().screen == s);
    assert(native->opened.back().closed);
    f.absent(g, s);
    assert(f.entries() == 0 && f.ctx.num_shadows == 0 && f.held_fonts() == 1);
    // the TextFont stays pinned; the second open and close leaves the heap where the first did
    const ULONG after = f.avail();
    auto calls = native->calls.size();
    assert(f.close(g) == 0 && f.close(0) == 1 && native->calls.size() == calls);
    assert(f.open_ns(0) == 0 && !f.stopped() && native->calls.size() == calls);
    f.put(f.fname_at, "topaz.font"); f.put(f.title_at, "My Screen");
    f.put_ns(f.ns_at, -5, 7, 320, 200, 3, 2, 1, 0x8004, CUSTOMSCREEN | SHOWTITLE, f.attr_at, f.title_at);
    const ULONG again = f.open_ns(f.ns_at); assert(again && f.close(again) == 1);
    assert(f.avail() == after && f.ctx.num_shadows == 0);
}

/* OpenScreenTagList(NULL, tags): the walker's flattening, scalars at
   native width, data that becomes native copies, and the display ID as it
   is, for a truecolor and an indexed mode */
static void test_open_tags()
{
    for (ULONG id : { 0x50031000u, 0x00029004u })
    {
        Fixture f;
        f.put(f.title_at, "Tags");
        f.put(f.fname_at, "tag.font");
        f.put_attr(f.attr_at, f.fname_at, 11, 0x02, 0x01);
        m68k_write16(&f.ctx, f.pens_at, 3); m68k_write16(&f.ctx, f.pens_at + 2, 0x0102);
        m68k_write16(&f.ctx, f.pens_at + 4, 0xFFFF);
        m68k_write32(&f.ctx, f.error_at, 0xA5A55A5A);
        f.put_tags(f.more_at, { { SA_Type, CUSTOMSCREEN }, { SA_ShowTitle, FALSE } });
        f.put_tags(f.tags_at, {
            { SA_Left, (ULONG)-3 }, { SA_Top, 4 }, { SA_Width, 0xFFFFFFFF }, { SA_Height, 0xFFFFFFFF },
            { SA_Depth, id == 0x50031000u ? 24u : 8u }, { SA_DisplayID, id },
            { TAG_IGNORE, 0xDEADBEEF }, { TAG_SKIP, 1 }, { SA_Colors32, 0x1234 },
            { SA_Title, f.title_at }, { SA_Font, f.attr_at }, { SA_Pens, f.pens_at },
            { SA_ErrorCode, f.error_at }, { SA_DetailPen, 0x1FF }, { SA_Behind, 0x10000 },
            { TAG_MORE, f.more_at } }, false);
        const ULONG g = f.open(0, f.tags_at);
        assert(g && !f.stopped() && native->count(OPEN) == 1);
        const Opened &o = native->opened.back();
        assert(!o.had_ns && o.had_tags);
        const ULONG order[] = { SA_Left, SA_Top, SA_Width, SA_Height, SA_Depth, SA_DisplayID, SA_Title,
                                SA_Font, SA_Pens, SA_ErrorCode, SA_DetailPen, SA_Behind, SA_Type, SA_ShowTitle };
        assert(o.tags.size() == std::size(order));
        for (size_t i = 0; i < std::size(order); ++i) assert(o.tags[i].ti_Tag == order[i]);
        // LONGs sign extended: Intuition compares SA_Width with a native ~0
        assert(o.tags[0].ti_Data == static_cast<IPTR>(static_cast<LONG>(-3)));
        assert(o.tags[2].ti_Data == ~static_cast<IPTR>(0) && o.tags[3].ti_Data == ~static_cast<IPTR>(0));
        // ULONGs, BOOLs and pens zero extended
        assert(o.tags[5].ti_Data == id && o.tags[10].ti_Data == 0x1FF && o.tags[11].ti_Data == 0x10000);
        assert(o.tags[12].ti_Data == CUSTOMSCREEN && o.tags[13].ti_Data == FALSE);
        // pointers are native, at native width, to what the program gave
        assert(reinterpret_cast<TextAttr *>(o.tags[7].ti_Data) == o.font && !in_guest(o.font));
        assert(reinterpret_cast<UBYTE *>(o.tags[6].ti_Data) == o.title && o.title_text == "Tags");
        assert(o.font_name == "tag.font" && o.font_copy.ta_YSize == 11 && o.font_copy.ta_Style == 2);
        assert((o.pens == std::vector<UWORD>{ 3, 0x0102, 0xFFFF }));
        assert(o.error && !in_guest(o.error));
        // success writes no error
        assert(f.lng(f.error_at) == 0xA5A55A5A);
        Screen *s = o.result;
        assert(s->LeftEdge == -3 && s->TopEdge == 4 && s->Width == 640 && s->Height == 480);
        assert(Kaffeine_ScreenNative(&f.ctx, g) == s && f.owned() == 1);
        screen_image(f, g, *s, g + SCREEN_SIZE, f.lng(g + RP + RP_FONT), f.title_at);
        assert(f.string(f.lng(g + SCREEN_SIZE)) == "tag.font");
        // left open: the end of the program closes it
    }
}

/* Intuition fails: no 68k Screen, and SA_ErrorCode gets Intuition's error
   as a big-endian ULONG, or keeps what the program had there */
static void test_screen_visualinfo_reference()
{
    Fixture f;
    ULONG g = f.open(0, 0);
    assert(g);
    auto *screen = Kaffeine_ScreenNative(&f.ctx, g);
    f.ctx.visualinfos[0].native = reinterpret_cast<APTR>(0x12345678);
    f.ctx.visualinfos[0].screen = screen;
    auto calls = native->count(CLOSE);
    assert(!f.close(g) && native->count(CLOSE) == calls && Kaffeine_ScreenNative(&f.ctx, g) == screen);
    f.ctx.visualinfos[0] = {};
    assert(f.close(g));
}

static void test_open_clip()
{
    for (bool null_clip : { false, true })
    {
        Fixture f;
        const ULONG clip = f.long_at;
        m68k_write16(&f.ctx, clip, (UWORD)-31);
        m68k_write16(&f.ctx, clip + 2, (UWORD)-17);
        m68k_write16(&f.ctx, clip + 4, 1023);
        m68k_write16(&f.ctx, clip + 6, 767);
        const auto before = f.memory;
        // Only the final effective pointer is read, as native Intuition does.
        f.put_tags(f.tags_at, { { SA_DClip, 0xFFFFFFFF },
                               { SA_DClip, null_clip ? 0u : clip } });
        ULONG g = f.open(0, f.tags_at);
        assert(g && !f.stopped());
        const auto &o = native->opened.back();
        assert(o.had_clip == !null_clip);
        if (!null_clip)
            assert(o.clip.MinX == -31 && o.clip.MinY == -17 &&
                   o.clip.MaxX == 1023 && o.clip.MaxY == 767);
        assert(std::equal(before.begin() + clip, before.begin() + clip + 8,
                          f.memory.begin() + clip));
        assert(f.close(g));
    }
}

static void test_open_native_failure()
{
    for (ULONG code : { (ULONG)OSERR_UNKNOWNMODE, (ULONG)OSERR_NOMONITOR, 0u })
    {
        Fixture f;
        m68k_write32(&f.ctx, f.error_at, 0x11223344);
        f.put_tags(f.tags_at, { { SA_DisplayID, 0xDEAD0000 }, { SA_ErrorCode, f.error_at } });
        native->open_fail = true; native->open_error = code;
        const ULONG heap = f.avail();
        auto expected = f.memory;
        be32(expected, f.error_at, code ? code : 0x11223344);
        assert(f.open(0, f.tags_at) == 0 && !f.stopped());
        assert(native->count(OPEN) == 1 && native->count(CLOSE) == 0 && f.memory == expected);
        assert(f.avail() == heap && f.ctx.num_shadows == 0 && !f.entries() && !f.ctx.screen_release);
        assert(native->count(FONT_OPEN) == 0);
    }
    {
        // No SA_ErrorCode: nothing is written.
        Fixture f;
        native->open_fail = true; native->open_error = OSERR_NOMEM;
        const auto before = f.memory;
        assert(f.open(0, 0) == 0 && f.memory == before && native->count(OPEN) == 1);
    }
    {
        // Intuition refuses a width that, zero extended, would exceed 32767.
        Fixture f;
        m68k_write32(&f.ctx, f.error_at, 0);
        f.put_tags(f.tags_at, { { SA_Width, 40000 }, { SA_ErrorCode, f.error_at } });
        assert(f.open(0, f.tags_at) == 0 && native->count(OPEN) == 1 && f.lng(f.error_at) == 0);
    }
}

/* Input that the bridge does not take stops the program before Intuition,
   without a write to guest memory, also SA_ErrorCode. */
static void test_open_refusals()
{
    struct Case { const char *what; ULONG lvo; std::function<std::pair<ULONG, ULONG>(Fixture &)> setup; };
    const Case cases[] =
    {
        { "NewScreen outside", 612, [](Fixture &) { return std::pair<ULONG, ULONG>{ 0xFFF0, 0 }; } },
        { "NewScreen outside, 198", 198, [](Fixture &) { return std::pair<ULONG, ULONG>{ 0xFFFFFFF0, 0 }; } },
        { "Gadgets", 198, [](Fixture &f) {
            f.put_ns(f.ns_at, 0, 0, 320, 200, 2, 0, 1, 0, CUSTOMSCREEN, 0, 0, 0x3000);
            return std::pair<ULONG, ULONG>{ f.ns_at, 0 }; } },
        { "CustomBitMap", 612, [](Fixture &f) {
            f.put_ns(f.ns_at, 0, 0, 320, 200, 2, 0, 1, 0, CUSTOMSCREEN, 0, 0, 0, 0x3000);
            return std::pair<ULONG, ULONG>{ f.ns_at, 0 }; } },
        { "CUSTOMBITMAP type", 612, [](Fixture &f) {
            f.put_ns(f.ns_at, 0, 0, 320, 200, 2, 0, 1, 0, CUSTOMSCREEN | CUSTOMBITMAP, 0, 0);
            return std::pair<ULONG, ULONG>{ f.ns_at, 0 }; } },
        { "ExtNewScreen cut by the end of memory", 198, [](Fixture &f) {
            f.put_ns(0x10000 - 32, 0, 0, 320, 200, 2, 0, 1, 0, CUSTOMSCREEN, 0, 0);
            m68k_write16(&f.ctx, 0x10000 - 32 + 14, CUSTOMSCREEN | NS_EXTENDED);
            return std::pair<ULONG, ULONG>{ 0x10000 - 32, 0 }; } },
        { "Extension with SA_Colors32", 198, [](Fixture &f) {
            f.put_tags(f.more_at, { { SA_Colors32, 0x3000 } });
            f.put_ns(f.ns_at, 0, 0, 320, 200, 2, 0, 1, 0, CUSTOMSCREEN | NS_EXTENDED, 0, 0, 0, 0, f.more_at);
            return std::pair<ULONG, ULONG>{ f.ns_at, 0 }; } },
        { "NewScreen font outside", 198, [](Fixture &f) {
            f.put_ns(f.ns_at, 0, 0, 320, 200, 2, 0, 1, 0, CUSTOMSCREEN, 0xFFFC, 0);
            return std::pair<ULONG, ULONG>{ f.ns_at, 0 }; } },
        { "NewScreen title without NUL", 198, [](Fixture &f) {
            std::fill_n(f.memory.begin() + 0xFF00, 0x100, 'T');
            f.put_ns(f.ns_at, 0, 0, 320, 200, 2, 0, 1, 0, CUSTOMSCREEN, 0, 0xFF00);
            return std::pair<ULONG, ULONG>{ f.ns_at, 0 }; } },
        { "tags outside", 612, [](Fixture &) { return std::pair<ULONG, ULONG>{ 0, 0xFFFC }; } },
        { "tag cycle", 612, [](Fixture &f) {
            f.put_tags(f.tags_at, { { SA_ErrorCode, f.error_at }, { TAG_MORE, f.tags_at } }, false);
            return std::pair<ULONG, ULONG>{ 0, f.tags_at }; } },
        { "unknown pointer tag after SA_ErrorCode", 612, [](Fixture &f) {
            f.put_tags(f.tags_at, { { SA_ErrorCode, f.error_at }, { SA_Colors32, 0x3000 } });
            return std::pair<ULONG, ULONG>{ 0, f.tags_at }; } },
        { "unknown scalar tag", 612, [](Fixture &f) {
            f.put_tags(f.tags_at, { { TAG_USER + 0x1234, 1 } });
            return std::pair<ULONG, ULONG>{ 0, f.tags_at }; } },
        { "SA_DClip truncated", 612, [](Fixture &f) {
            f.put_tags(f.tags_at, { { SA_DClip, 0xFFFC } });
            return std::pair<ULONG, ULONG>{ 0, f.tags_at }; } },
        { "SA_PubTask", 612, [](Fixture &f) {
            f.put_tags(f.tags_at, { { SA_PubTask, 0 } });
            return std::pair<ULONG, ULONG>{ 0, f.tags_at }; } },
        { "SA_BitMap", 612, [](Fixture &f) {
            f.put_tags(f.tags_at, { { SA_BitMap, 0x3000 } });
            return std::pair<ULONG, ULONG>{ 0, f.tags_at }; } },
        { "SA_Type CUSTOMBITMAP", 612, [](Fixture &f) {
            f.put_tags(f.tags_at, { { SA_Type, CUSTOMSCREEN | CUSTOMBITMAP } });
            return std::pair<ULONG, ULONG>{ 0, f.tags_at }; } },
        { "title of 256 characters", 612, [](Fixture &f) {
            f.put(f.long_at, std::string(256, 'L'));
            f.put_tags(f.tags_at, { { SA_Title, f.long_at } });
            return std::pair<ULONG, ULONG>{ 0, f.tags_at }; } },
        { "title outside", 612, [](Fixture &f) {
            f.put_tags(f.tags_at, { { SA_Title, 0x10000 } });
            return std::pair<ULONG, ULONG>{ 0, f.tags_at }; } },
        { "TextAttr outside", 612, [](Fixture &f) {
            f.put_tags(f.tags_at, { { SA_Font, 0xFFFC } });
            return std::pair<ULONG, ULONG>{ 0, f.tags_at }; } },
        { "font name of 256 characters", 612, [](Fixture &f) {
            f.put(f.long_at, std::string(256, 'F'));
            f.put_attr(f.attr_at, f.long_at, 8, 0, 0);
            f.put_tags(f.tags_at, { { SA_Font, f.attr_at } });
            return std::pair<ULONG, ULONG>{ 0, f.tags_at }; } },
        { "TTextAttr with a pointer tag", 612, [](Fixture &f) {
            f.put(f.fname_at, "x.font");
            f.put_tags(f.ftags_at, { { TAG_USER + 7, 0x3000 } });
            f.put_attr(f.attr_at, f.fname_at, 8, FSF_TAGGED, 0, f.ftags_at);
            f.put_tags(f.tags_at, { { SA_Font, f.attr_at } });
            return std::pair<ULONG, ULONG>{ 0, f.tags_at }; } },
        { "65 pens without ~0", 612, [](Fixture &f) {
            for (unsigned i = 0; i < 65; ++i) m68k_write16(&f.ctx, f.long_at + 2 * i, 1);
            m68k_write16(&f.ctx, f.long_at + 130, 0xFFFF);
            f.put_tags(f.tags_at, { { SA_Pens, f.long_at } });
            return std::pair<ULONG, ULONG>{ 0, f.tags_at }; } },
        { "pens outside", 612, [](Fixture &f) {
            m68k_write16(&f.ctx, 0xFFFE, 1);
            f.put_tags(f.tags_at, { { SA_Pens, 0xFFFE } });
            return std::pair<ULONG, ULONG>{ 0, f.tags_at }; } },
        { "SA_ErrorCode cut by the end of memory", 612, [](Fixture &f) {
            f.put_tags(f.tags_at, { { SA_ErrorCode, 0xFFFE } });
            return std::pair<ULONG, ULONG>{ 0, f.tags_at }; } },
        { "SA_PubName of 140 characters", 612, [](Fixture &f) {
            f.put(f.long_at, std::string(140, 'P'));
            f.put_tags(f.tags_at, { { SA_PubName, f.long_at } });
            return std::pair<ULONG, ULONG>{ 0, f.tags_at }; } },
        { "257 tags", 612, [](Fixture &f) {
            for (ULONG i = 0; i < 257; ++i)
            { m68k_write32(&f.ctx, f.tags_at + 8 * i, SA_Left); m68k_write32(&f.ctx, f.tags_at + 8 * i + 4, i); }
            m68k_write32(&f.ctx, f.tags_at + 8 * 257, TAG_DONE);
            return std::pair<ULONG, ULONG>{ 0, f.tags_at }; } },
    };
    for (const auto &c : cases)
    {
        Fixture f;
        m68k_write32(&f.ctx, f.error_at, 0x01020304);
        const auto args = c.setup(f);
        const auto before = f.memory;
        const ULONG result = c.lvo == 198 ? f.open_ns(args.first) : f.open(args.first, args.second);
        if (result != 20) std::printf("not refused: %s\n", c.what);
        assert(result == 20);
        f.refused(before);
        assert(f.avail() == f.initial_heap);
    }
    // Exactly 256 tags, a 255-character title and font name, 64 pens and
    // a 139-character public name are taken.
    Fixture f;
    f.put(f.long_at, std::string(255, 'L'));
    f.put(f.fname_at - 0x40, std::string(139, 'P')); // 0x3320, before the tags
    const ULONG pens = 0x3E40; // after the title, before the stack
    for (ULONG i = 0; i < 64; ++i) m68k_write16(&f.ctx, pens + 2 * i, (UWORD)i);
    m68k_write16(&f.ctx, pens + 128, 0xFFFF);
    f.put_tags(f.tags_at + 8 * 253, { { SA_Title, f.long_at }, { SA_Pens, pens },
                                      { SA_PubName, f.fname_at - 0x40 } });
    for (ULONG i = 0; i < 253; ++i)
    { m68k_write32(&f.ctx, f.tags_at + 8 * i, SA_Top); m68k_write32(&f.ctx, f.tags_at + 8 * i + 4, i); }
    const ULONG g = f.open(0, f.tags_at);
    assert(g && native->opened.back().tags.size() == 256);
    assert(native->opened.back().title_text == std::string(255, 'L') && native->opened.back().pens.size() == 65);
    assert(native->opened.back().pubname == std::string(139, 'P'));
}

/* rom/intuition/openscreentaglist.c: A1 replaces the Extension of an
   ExtNewScreen, which is used without A1; tags win over the NewScreen,
   the last SA_Font/SA_Title, the first SA_ErrorCode. Only the values that
   count are read. */
static void test_open_precedence()
{
    {
        Fixture f;
        f.put_tags(f.more_at, { { SA_Colors32, 0x3000 } }); // never read
        f.put_ns(f.ns_at, 0, 0, 640, 480, 2, 0, 1, 0, CUSTOMSCREEN | NS_EXTENDED, 0, 0, 0, 0, f.more_at);
        f.put_tags(f.tags_at, { { SA_Width, 800 } });
        const ULONG g = f.open(f.ns_at, f.tags_at);
        const Opened &o = native->opened.back();
        assert(g && o.had_ns && o.ns.Type == CUSTOMSCREEN && o.tags.size() == 1);
        assert(o.tags[0].ti_Tag == SA_Width && o.tags[0].ti_Data == 800 && o.result->Width == 800);
    }
    for (ULONG lvo : { 612u, 198u })
    {
        Fixture f;
        f.put_tags(f.more_at, { { SA_Height, 300 } });
        f.put_ns(f.ns_at, 0, 0, 640, 480, 2, 0, 1, 0, CUSTOMSCREEN | SHOWTITLE | NS_EXTENDED, 0, 0, 0, 0, f.more_at);
        const ULONG g = lvo == 612 ? f.open(f.ns_at, 0) : f.open_ns(f.ns_at);
        const Opened &o = native->opened.back();
        assert(g && o.ns.Type == (CUSTOMSCREEN | SHOWTITLE) && o.tags.size() == 1);
        assert(o.tags[0].ti_Data == 300 && o.result->Height == 300 && o.result->Width == 640);
        // an Extension of NULL is no tags
        m68k_write32(&f.ctx, f.ns_at + 32, 0);
        assert(f.open_ns(f.ns_at) && !native->opened.back().had_tags);
    }
    {
        Fixture f;
        f.put(f.title_at, "First"); f.put(f.title2_at, "Second");
        f.put(f.fname_at, "tag.font"); f.put_attr(f.attr_at, f.fname_at, 9, 0, 0);
        m68k_write32(&f.ctx, f.error_at, 0); m68k_write32(&f.ctx, f.error2_at, 0);
        // a NewScreen font that a tag replaces is never read
        f.put_ns(f.ns_at, 0, 0, 640, 480, 2, 0, 1, 0, CUSTOMSCREEN, 0xFFFFFFF0, f.title_at);
        f.put_tags(f.tags_at, { { SA_Title, f.title_at }, { SA_Font, f.attr_at }, { SA_Title, f.title2_at },
                                { SA_ErrorCode, f.error_at }, { SA_ErrorCode, f.error2_at } });
        const ULONG g = f.open(f.ns_at, f.tags_at);
        const Opened &o = native->opened.back();
        assert(g && o.title_text == "Second" && o.font_name == "tag.font" && o.font_copy.ta_YSize == 9);
        assert(o.ns.Font == o.font && o.ns.DefaultTitle == o.title); // one native copy of each
        assert(o.tags[0].ti_Data == o.tags[2].ti_Data && o.tags[3].ti_Data == o.tags[4].ti_Data);
        assert(f.lng(g + 22) == f.title2_at && f.lng(g + 26) == f.title2_at);
        // the first SA_ErrorCode gets the error
        native->open_fail = true; native->open_error = OSERR_PUBNOTUNIQUE;
        assert(f.open(f.ns_at, f.tags_at) == 0);
        assert(f.lng(f.error_at) == OSERR_PUBNOTUNIQUE && f.lng(f.error2_at) == 0);
        native->open_fail = false;
        // SA_Font 0 over the NewScreen's font, SA_Title 0 over its title
        f.put_attr(f.attr_at, f.fname_at, 9, 0, 0);
        f.put_ns(f.ns_at, 0, 0, 640, 480, 2, 0, 1, 0, CUSTOMSCREEN, f.attr_at, f.title_at);
        f.put_tags(f.tags_at, { { SA_Font, 0 }, { SA_Title, 0 } });
        const ULONG plain = f.open(f.ns_at, f.tags_at);
        const Opened &p = native->opened.back();
        assert(plain && !p.font && !p.title && !p.ns.Font && !p.ns.DefaultTitle);
        assert(f.lng(plain + 40) == 0 && f.lng(plain + 22) == 0);
    }
}

/* SA_Font: the style and flags as they are, FSF_TAGGED with its TA_DeviceDPI
   tags copied natively; the 68k TextAttr shows the style without FSF_TAGGED. */
static void test_open_font_flags()
{
    for (bool with_tags : { false, true })
    {
        Fixture f;
        f.put(f.fname_at, "dpi.font");
        f.put_tags(f.ftags_at, { { TA_DeviceDPI, 0x00480048 } });
        f.put_attr(f.attr_at, f.fname_at, 13, FSF_TAGGED | 0x03, 0x62, with_tags ? f.ftags_at : 0);
        f.put_tags(f.tags_at, { { SA_Font, f.attr_at } });
        const ULONG g = f.open(0, f.tags_at); assert(g);
        const Opened &o = native->opened.back();
        assert(o.font_copy.ta_Style == (FSF_TAGGED | 0x03) && o.font_copy.ta_Flags == 0x62);
        assert(o.font_tags.size() == (with_tags ? 1u : 0u));
        if (with_tags)
            assert(o.font_tags[0].ti_Tag == TA_DeviceDPI && o.font_tags[0].ti_Data == 0x00480048);
        const ULONG ta = f.lng(g + 40);
        assert(ta == g + SCREEN_SIZE && f.string(f.lng(ta)) == "dpi.font" && f.word(ta + 4) == 13);
        assert(f.memory[ta + 6] == 0x03 && f.memory[ta + 7] == 0x62);
        // the native tags stay with the screen, not with the guest list
        std::fill_n(f.memory.begin() + f.ftags_at, 16, 0);
        if (with_tags)
            assert(reinterpret_cast<const TTextAttr *>(o.result->Font)->tta_Tags[0].ti_Data == 0x00480048);
        assert(f.close(g) == 1);
    }
}

/* CloseScreen() closes only a screen that the program opened; a refusal
   of Intuition leaves everything; one 68k Screen across LockPubScreen(),
   the list and windows; no identity outlives the screen. */
static void test_close_ownership()
{
    Fixture f;
    f.add();
    const ULONG pub = f.lock(); assert(pub);
    f.put(f.title2_at, "MyPub");
    f.put_tags(f.tags_at, { { SA_PubName, f.title2_at } });
    const ULONG g = f.open(0, f.tags_at); assert(g && g != pub);
    Screen *s = native->opened.back().result;
    auto calls = native->calls.size();
    // A borrowed screen, another address, an address inside the 68k Screen
    assert(f.close(pub) == 0 && f.close(f.text) == 0 && f.close(g + 4) == 0 && f.close(g + VP) == 0);
    assert(f.close(0) == 1 && native->calls.size() == calls && Kaffeine_ScreenNative(&f.ctx, pub));
    // LockPubScreen() of the opened screen gives the same 68k Screen.
    assert(f.lock(f.name("mypub")) == g && f.ctx.pubscreens[f.entry(g)].locks == 1);
    assert(f.ctx.pubscreens[f.entry(g)].owned && f.entries() == 2 && f.ctx.num_shadows == 6);
    const ULONG list = f.locklist(); assert(list);
    assert(f.lng(list + LIST_ROOM + PSN_SCREEN) == pub && f.lng(list + LIST_ROOM + NODE_SIZE + PSN_SCREEN) == g);
    f.unlocklist();
    // Visitors: Intuition refuses and the 68k Screen stays as it was.
    std::vector<UBYTE> mirror(f.memory.begin() + g, f.memory.begin() + g + SCREEN_SIZE);
    ULONG heap = f.avail(); auto shadows = f.ctx.num_shadows;
    assert(f.close(g) == 0 && native->count(CLOSE) == 1 && native->calls.back().screen == s);
    assert(std::equal(mirror.begin(), mirror.end(), f.memory.begin() + g));
    assert(f.avail() == heap && f.ctx.num_shadows == shadows && Kaffeine_ScreenNative(&f.ctx, g) == s);
    // the last lock: an opened screen is not pruned
    f.unlock(g);
    assert(Kaffeine_ScreenNative(&f.ctx, g) == s && f.owned() == 1 && f.ctx.pubscreens[f.entry(g)].locks == 0);
    // A window there: refused again, and its going does not prune the screen.
    Window w{}; w.WScreen = s; s->FirstWindow = &w;
    assert(Kaffeine_ShadowAdd(&f.ctx, 0x3500, &w, KAFFEINE_SHADOW_WINDOW));
    assert(f.close(g) == 0 && Kaffeine_ScreenNative(&f.ctx, g) == s && f.avail() == heap);
    s->FirstWindow = nullptr; Kaffeine_ShadowForget(&f.ctx, 0x3500, KAFFEINE_SHADOW_WINDOW);
    f.ctx.screen_release(&f.ctx, s);
    assert(Kaffeine_ScreenNative(&f.ctx, g) == s && Kaffeine_ScreenGuest(&f.ctx, s) == g);
    native->close_refuse = true;
    assert(f.close(g) == 0 && Kaffeine_ScreenNative(&f.ctx, g) == s);
    native->close_refuse = false;
    assert(native->count(CLOSE) == 3);
    assert(f.close(g) == 1 && native->count(CLOSE) == 4);
    f.absent(g, s);
    assert(f.entries() == 1 && f.owned() == 0 && f.ctx.num_shadows == 3);
    // Nothing of the closed screen comes back.
    const auto unlocks = native->count(UNLOCK);
    assert(!f.lock(f.name("MyPub")));
    f.unlock(g);
    assert(f.close(g) == 0 && native->count(CLOSE) == 4 && native->count(UNLOCK) == unlocks);
    // A new screen is a new identity, even where the old 68k Screen was.
    const ULONG next = f.open(0, 0); assert(next);
    assert(Kaffeine_ScreenNative(&f.ctx, next) == native->opened.back().result);
    assert(next != g || Kaffeine_ScreenGuest(&f.ctx, s) == 0);
    f.unlock(pub);
}

/* Screen.RastPort.BitMap is the 68k BitMap inside the 68k Screen, a typed
   handle of the BitMap that the screen draws into: published at
   OpenScreen() and at each LockPubScreen() from the native RastPort's
   BitMap, not the obsolete copy, with no plane or other native pointer;
   graphics calls leave it; never a generic shadow. Only its exact address
   resolves, to the native screen only for a screen that the program opened
   and has not closed; the program's edits of the snapshot change nothing.
   CloseScreen() during a blit stops the program before Intuition. */
static void test_screen_bitmap_handle()
{
    static_assert(M68K_SCR_BITMAP == BM);
    Fixture f;
    auto &wb = f.add();
    BitMap wb_drawn{};
    wb_drawn.BytesPerRow = 80; wb_drawn.Rows = 256; wb_drawn.Depth = 4; wb_drawn.pad = 0x1111;
    for (auto &p : wb_drawn.Planes) p = reinterpret_cast<PLANEPTR>(&wb_drawn);
    wb.screen.RastPort.BitMap = &wb_drawn;
    const ULONG pub = f.lock(); assert(pub);
    f.put_tags(f.tags_at, { { SA_Width, 320 }, { SA_Height, 200 }, { SA_Depth, 5 } });
    const ULONG g = f.open(0, f.tags_at); assert(g && g != pub && f.ctx.running);
    Screen *s = native->opened.back().result;
    BitMap *drawn = s->RastPort.BitMap;
    assert(drawn && drawn != &s->BitMap_OBSOLETE);

    // published with the screen: the handle, and the drawn BitMap's scalars
    assert(f.lng(g + RP + 4) == g + BM && f.lng(pub + RP + 4) == pub + BM);
    assert(f.word(g + BM) == 40 && f.word(g + BM + 2) == 200);
    assert(f.memory[g + BM + 4] == 0x84 && f.memory[g + BM + 5] == 5 && f.word(g + BM + 6) == 0);
    assert(f.word(pub + BM) == 80 && f.word(pub + BM + 2) == 256 && f.memory[pub + BM + 5] == 4);
    for (ULONG at = 6; at < 40; ++at)
        assert(f.memory[g + BM + at] == 0 && f.memory[pub + BM + at] == 0);
    // no native pointer anywhere in either 68k Screen
    const void *pointers[] = { drawn, drawn->Planes[0], &wb_drawn, s, &wb.screen, &s->RastPort };
    for (ULONG base : { g, pub })
        for (ULONG off = 0; off + 4 <= SCREEN_SIZE; off += 2)
            for (const void *p : pointers)
                assert(f.lng(base + off) != (ULONG)reinterpret_cast<std::uintptr_t>(p));
    // never a generic shadow: the generic map gives guest memory for it
    for (UWORD i = 0; i < f.ctx.num_shadows; ++i)
        assert(f.ctx.shadow_map[i].m68k_addr != g + BM && f.ctx.shadow_map[i].m68k_addr != pub + BM);
    assert(f.ctx.num_shadows == 6);
    assert(m68k_to_host_or_shadow(&f.ctx, g + BM) == f.memory.data() + g + BM);

    // the exact handle of the opened screen alone resolves to it
    Screen *out = nullptr;
    assert(Kaffeine_ScreenBitMapOf(&f.ctx, g + BM, &out) == KAFFEINE_SCREEN_BITMAP_OWNED && out == s);
    assert(Kaffeine_ScreenBitMapOf(&f.ctx, pub + BM, &out) == KAFFEINE_SCREEN_BITMAP_BORROWED && !out);
    for (ULONG other : { 0u, g, g + RP, g + BM + 2, g + BM - 2, pub, pub + RP, (ULONG)f.text, 0xFFFFFFF0u })
    {
        assert(Kaffeine_ScreenBitMapOf(&f.ctx, other, &out) == KAFFEINE_SCREEN_BITMAP_NONE && !out);
    }
    // whatever the program writes into the snapshot
    m68k_write16(&f.ctx, g + BM, 0x7FFE);
    m68k_write32(&f.ctx, g + BM + 8, f.text);
    assert(Kaffeine_ScreenBitMapOf(&f.ctx, g + BM, &out) == KAFFEINE_SCREEN_BITMAP_OWNED && out == s);

    // around a graphics call through the 68k RastPort the handle stays
    assert(Kaffeine_RastPortIn(&f.ctx, g + RP) == &s->RastPort && f.ctx.after_thunk);
    s->RastPort.FgPen = 3;
    f.ctx.after_thunk(&f.ctx);
    f.ctx.after_thunk = nullptr;
    assert(f.memory[g + RP + 25] == 3 && f.lng(g + RP + 4) == g + BM);
    // a relock takes the snapshot of the borrowed screen again
    wb_drawn.Rows = 512;
    m68k_write32(&f.ctx, pub + RP + 4, 0);
    assert(f.lock() == pub && f.word(pub + BM + 2) == 512 && f.lng(pub + RP + 4) == pub + BM);
    f.unlock(pub);

    // CloseScreen() while a blit runs: stopped before Intuition, all stays
    auto calls = native->calls.size();
    const ULONG heap = f.avail();
    f.ctx.bitmap_blits = 1;
    assert(f.close(g) == 20 && f.stopped() && native->calls.size() == calls);
    f.ctx.bitmap_blits = 0;
    assert(f.avail() == heap && Kaffeine_ScreenNative(&f.ctx, g) == s);
    assert(Kaffeine_ScreenBitMapOf(&f.ctx, g + BM, &out) == KAFFEINE_SCREEN_BITMAP_OWNED && out == s);

    // closed: the handle that the program kept is cleared and names nothing
    assert(f.close(g) == 1 && f.ctx.running && native->count(CLOSE) == 1);
    assert(Kaffeine_ScreenBitMapOf(&f.ctx, g + BM, &out) == KAFFEINE_SCREEN_BITMAP_NONE && !out);
    assert(f.lng(g + RP + 4) == 0);
    for (ULONG at = 0; at < 40; ++at)
        assert(f.memory[g + BM + at] == 0);
    // so is the borrowed one, with the last unlock
    f.unlock(pub);
    assert(Kaffeine_ScreenBitMapOf(&f.ctx, pub + BM, &out) == KAFFEINE_SCREEN_BITMAP_NONE && !out);
    assert(f.lng(pub + RP + 4) == 0);
    for (ULONG at = 0; at < 40; ++at)
        assert(f.memory[pub + BM + at] == 0);
}

/* No room for the 68k Screen: before Intuition when the table of screens
   or the shadows are full, else Intuition's screen closes again. Either way
   SA_ErrorCode gets OSERR_NOMEM and nothing stays. */
static void test_open_bridge_failures()
{
    for (bool full_table : { true, false })
    {
        Fixture f;
        std::vector<ULONG> locked;
        if (full_table)
            for (unsigned i = 0; i < KAFFEINE_MAX_PUBSCREENS; ++i)
            {
                auto &s = f.add("Screen" + std::to_string(i));
                locked.push_back(f.lock(f.name(s.name.data()))); assert(locked.back());
            }
        else
            f.leave_shadows(2);
        m68k_write32(&f.ctx, f.error_at, 0);
        f.put_tags(f.tags_at, { { SA_ErrorCode, f.error_at } });
        const ULONG heap = f.avail(); const auto shadows = f.ctx.num_shadows;
        assert(f.open(0, f.tags_at) == 0 && !f.stopped() && native->count(OPEN) == 0);
        assert(f.lng(f.error_at) == OSERR_NOMEM && f.avail() == heap && f.ctx.num_shadows == shadows);
        if (full_table) f.unlock(locked[0]);
        else { f.free_shadows(); f.leave_shadows(3); }
        const ULONG g = f.open(0, f.tags_at); assert(g && f.owned() == 1);
        assert(f.close(g) == 1);
        for (size_t i = 1; i < locked.size(); ++i) f.unlock(locked[i]);
        f.free_shadows();
    }
    for (bool no_memory : { true, false })
    {
        Fixture f;
        ULONG taken = 0, size = 0;
        if (no_memory)
        { size = Kaffeine_HeapAvail(&f.ctx, MEMF_LARGEST); taken = Kaffeine_HeapAlloc(&f.ctx, size, MEMF_ANY); assert(taken); }
        else
            native->font_result = nullptr; // the screen font cannot be held
        m68k_write32(&f.ctx, f.error_at, 0);
        f.put(f.title_at, "Rolled back");
        f.put_tags(f.tags_at, { { SA_ErrorCode, f.error_at }, { SA_Title, f.title_at } });
        const ULONG heap = f.avail();
        assert(f.open(0, f.tags_at) == 0 && !f.stopped());
        assert(native->count(OPEN) == 1 && native->count(CLOSE) == 1);
        assert(native->calls.back().kind == CLOSE && native->calls.back().screen == native->opened.back().result);
        assert(native->opened.back().closed && f.lng(f.error_at) == OSERR_NOMEM);
        assert(f.avail() == heap && f.ctx.num_shadows == 0 && !f.entries() && !f.held_fonts());
        assert(native->font_opens.empty() && !Kaffeine_FontFind(&f.ctx, &f.font));
        if (taken) Kaffeine_HeapFree(&f.ctx, taken, size);
        native->font_result = &f.font;
        const ULONG g = f.open(0, f.tags_at); assert(g && f.close(g) == 1);
    }
}

/* The end of the program: the locks first, then the screens that the
   program left open, except one with its window, then the fonts. */
static void test_teardown()
{
    Window w{};
    Fixture f;
    auto &wb = f.add("Workbench", true);
    const ULONG pub = f.lock(); assert(pub);
    f.put(f.title2_at, "Shared");
    f.put_tags(f.tags_at, { { SA_PubName, f.title2_at } });
    const ULONG shared = f.open(0, f.tags_at); assert(shared);
    Screen *ss = native->opened.back().result;
    assert(f.lock(f.name("shared")) == shared);
    const ULONG plain = f.open(0, 0); assert(plain);
    Screen *sp = native->opened.back().result;
    const ULONG busy = f.open(0, 0); assert(busy);
    Screen *sb = native->opened.back().result;
    w.WScreen = sb; sb->FirstWindow = &w;
    assert(Kaffeine_ShadowAdd(&f.ctx, 0x3500, &w, KAFFEINE_SHADOW_WINDOW));
    assert(f.locklist() && f.locklist());
    assert(f.held_fonts() == 1 && f.owned() == 3);
    const size_t mark = native->calls.size();
    Kaffeine_ScreensCleanup(&f.ctx);
    size_t last_unlock = 0, first_close = SIZE_MAX, last_close = 0, first_font = SIZE_MAX;
    std::vector<Screen *> closed;
    for (size_t i = mark; i < native->calls.size(); ++i)
    {
        const auto &c = native->calls[i];
        // UnlockPubScreen() locks the list itself; all of it comes first.
        if (c.kind == UNLOCK || c.kind == LIST_LOCK || c.kind == LIST_UNLOCK) last_unlock = i;
        if (c.kind == CLOSE) { first_close = std::min(first_close, i); last_close = i; closed.push_back(c.screen); }
        if (c.kind == FONT_CLOSE) first_font = std::min(first_font, i);
    }
    assert(native->count(UNLOCK) >= 2 && last_unlock > mark);
    assert(last_unlock < first_close && last_close < first_font && first_font != SIZE_MAX);
    assert((closed == std::vector<Screen *>{ ss, sp }));
    assert(wb.node.psn_VisitorCount == 0 && native->node(ss)->psn_VisitorCount == 0 && native->list_depth == 0);
    f.absent(shared, ss); f.absent(plain, sp);
    assert(Kaffeine_ScreenNative(&f.ctx, busy) == sb && f.owned() == 1 && native->font_opens.empty());
    // again: nothing more for Intuition while the window stays
    const auto calls = native->calls.size();
    Kaffeine_ScreensCleanup(&f.ctx); assert(native->calls.size() == calls);
    // without the window, the cleanup of the fixture closes the last one
    sb->FirstWindow = nullptr; Kaffeine_ShadowForget(&f.ctx, 0x3500, KAFFEINE_SHADOW_WINDOW);
    Kaffeine_ScreensCleanup(&f.ctx);
    assert(native->count(CLOSE) == 3 && native->calls.back().screen == sb && !f.owned());
}

/* What the layer bridge hears (kaffeine_layers.c, not linked here): a
   snapshot for each screen that gets or refills a 68k Screen; its end
   before Intuition may close the screen and while the 68k Screen that
   holds its LayerInfo is still there; and the end of all of them before the
   cleanup gives back the locks that keep screens open. */
struct LayerOpCall
{
    std::string name;
    Screen *screen;
    size_t native_calls;
    ULONG guest;
};
static std::vector<LayerOpCall> layer_op_calls;
static void op_mirrored(KaffeineContext *ctx, Screen *s)
{
    const ULONG guest = Kaffeine_ScreenGuest(ctx, s);
    const ULONG ss = guest + M68K_SCR_LAYERINFO + M68K_LI_LOCK;
    const ULONG queue = ss + M68K_SS_WAITQUEUE;
    /* Screen construction initializes this before a snapshot callback can
       fail or skip because Intuition is busy. A held semaphore also keeps
       this empty queue intact across a refused close. */
    assert(guest && ctx->mem[ss + M68K_SS_LN_TYPE] == 15);
    assert(m68k_read32(ctx, queue) == queue + 4 && !m68k_read32(ctx, queue + 4));
    assert(m68k_read32(ctx, queue + 8) == queue);
    layer_op_calls.push_back({ "mirrored", s, native->calls.size(), guest });
}
static void op_closing(KaffeineContext *ctx, Screen *s)
{
    // only a pointer comparison: the screen may be gone already
    layer_op_calls.push_back({ "closing", s, native->calls.size(), s ? Kaffeine_ScreenGuest(ctx, s) : 0 });
}
static BOOL op_window_opened(KaffeineContext *, ULONG, Window *) { assert(false); return FALSE; }
static void op_window_closing(KaffeineContext *, ULONG, Window *) { assert(false); }
static ULONG op_window_flags(KaffeineContext *, ULONG, ULONG flags) { assert(false); return flags; }
static void op_window_synced(KaffeineContext *, ULONG, Window *) { assert(false); }
static const KaffeineLayerOps recording_layer_ops =
    { op_window_opened, op_window_closing, op_window_flags, op_window_synced, op_mirrored, op_closing };

static void test_layer_notifications()
{
    Fixture f;
    f.ctx.layer_ops = &recording_layer_ops;
    layer_op_calls.clear();
    f.add();
    const ULONG pub = f.lock(); assert(pub);
    Screen *ws = Kaffeine_ScreenNative(&f.ctx, pub);
    assert(layer_op_calls.size() == 1 && layer_op_calls[0].name == "mirrored");
    assert(layer_op_calls[0].screen == ws && layer_op_calls[0].guest == pub);
    // a relock takes the snapshot again; an unlock that is not the last does nothing
    assert(f.lock() == pub && layer_op_calls.size() == 2 && layer_op_calls[1].name == "mirrored");
    f.unlock(pub);
    assert(layer_op_calls.size() == 2);
    // the last unlock: the snapshot goes while the 68k Screen is still there
    f.unlock(pub);
    assert(layer_op_calls.size() == 3 && layer_op_calls[2].name == "closing");
    assert(layer_op_calls[2].screen == ws && layer_op_calls[2].guest == pub);
    assert(!Kaffeine_ScreenGuest(&f.ctx, ws));
    // an opened screen
    const ULONG g = f.open(0, 0); assert(g);
    Screen *s = native->opened.back().result;
    assert(layer_op_calls.size() == 4 && layer_op_calls[3].name == "mirrored");
    assert(layer_op_calls[3].screen == s && layer_op_calls[3].guest == g);
    // CloseScreen(): the snapshot goes before Intuition gets the screen;
    // after a refusal the snapshot is restored immediately
    native->close_refuse = true;
    size_t calls = native->calls.size();
    assert(f.close(g) == 0);
    assert(layer_op_calls.size() == 6 && layer_op_calls[4].name == "closing");
    assert(layer_op_calls[4].screen == s && layer_op_calls[4].native_calls == calls);
    assert(layer_op_calls[4].guest == g && native->calls.back().kind == CLOSE);
    assert(Kaffeine_ScreenGuest(&f.ctx, s) == g);
    assert(layer_op_calls[5].name == "mirrored" && layer_op_calls[5].guest == g);
    native->close_refuse = false;
    calls = native->calls.size();
    assert(f.close(g) == 1);
    assert(layer_op_calls.size() == 8);
    assert(layer_op_calls[6].name == "closing" && layer_op_calls[6].native_calls == calls);
    // and again as the 68k Screen goes after Intuition closed the screen
    assert(layer_op_calls[7].name == "closing" && layer_op_calls[7].screen == s);
    assert(layer_op_calls[7].native_calls == calls + 1 && layer_op_calls[7].guest == g);
    f.absent(g, s);
    // the cleanup ends every snapshot before it gives back any lock
    assert(f.lock());
    const size_t mark = layer_op_calls.size();
    calls = native->calls.size();
    Kaffeine_ScreensCleanup(&f.ctx);
    assert(layer_op_calls.size() > mark && layer_op_calls[mark].name == "closing");
    assert(!layer_op_calls[mark].screen && layer_op_calls[mark].native_calls == calls);
    assert(native->calls.size() > calls);
    f.ctx.layer_ops = nullptr;
}

int main()
{
    const struct Test { const char *name; void (*run)(); } tests[] =
    {
        { "Layer snapshots follow 68k Screens and end before CloseScreen, forget and cleanup unlocks", test_layer_notifications },
        { "Nine ABI registrations override poisoned generated fallbacks", test_registrations },
        { "Screen.RastPort.BitMap is a typed handle of the drawn BitMap, resolved only for opened screens", test_screen_bitmap_handle },
        { "NULL and named locks expose the NDK fields, font and three shadows; relocks refill in place", test_screen_fields_and_relock },
        { "The last unlock returns the mirror and shadows; the font stays pinned; a later lock allocates anew", test_release_and_recreate },
        { "TextAttr names share the pinned TextFont or use an inline copy; a saved AskFont TextAttr survives reuse", test_textattr_names },
        { "One native font reference spans relocks and new mirrors until cleanup", test_native_font_lifetime },
        { "A RastPort font is held, also without a TextAttr; none without a RastPort font", test_native_font_without_attributes },
        { "OpenFont giving another font or NULL makes no 68k Screen and pins nothing; the other font is closed", test_native_font_other_or_null },
        { "32 native fonts fill the table with the 68k fonts; a full table refuses before OpenFont; cleanup closes each once", test_native_font_table_limit },
        { "No 68k Screen without its RastPort TextFont: full font table and insufficient memory give the held font back", test_missing_guest_font },
        { "Unlock ownership, ASCII case, first matching name and ignored A1", test_unlock_ownership_and_names },
        { "Names must terminate in guest memory before Intuition", test_bad_names },
        { "Only windows on this screen retain it until their shadows are gone", test_window_retention },
        { "An opened type-5 Screen is reused unchanged and never freed by unlock", test_owned_screen },
        { "No memory, fewer than three shadows, private/missing native screens leave no lock, allocation or font", test_lock_failures },
        { "The 16-screen table gives an excess native lock back", test_screen_table_limit },
        { "The 32767-screen-lock limit gives an excess native lock back", test_screen_lock_limit },
        { "The list wire image, long names, private screens, nested locks and retained screens", test_lists },
        { "A changed native node count freezes every copied field until the count matches again", test_list_count_changes },
        { "Empty lists and the 256-node cap", test_empty_and_capped_lists },
        { "A failed list copy still owns each native list lock", test_list_allocation_failure },
        { "The 32767-list-lock limit refuses before Intuition", test_list_lock_limit },
        { "NextPubScreen walks the native list, including private and nonpublic screens", test_next_screen },
        { "GetDefaultPubScreen uses native storage, copies a bounded name and returns VOID", test_default_screen },
        { "Name buffers need the name and NUL only, with atomic refusals after the native lookup", test_name_buffers },
        { "Cleanup returns two screens' locks and nested list locks, once", test_cleanup },
        { "A native list lock without a copy still retains the Screen", test_list_without_copy_retains_screen },
        { "OpenScreen passes every NewScreen field; native font/title copies; a full type-5 image; CloseScreen releases it", test_open_newscreen },
        { "OpenScreenTagList flattens tags, sign-extends LONGs, copies pointer data natively, truecolor and indexed IDs", test_open_tags },
        { "SA_DClip converts a bounded classic Rectangle and keeps signed coordinates", test_open_clip },
        { "An owned screen remains open while a VisualInfo refers to it", test_screen_visualinfo_reference },
        { "Native open failure: no 68k Screen, Intuition's error as a BE ULONG, or the program's value kept", test_open_native_failure },
        { "Malformed or unsupported input stops the program before Intuition, writing nothing", test_open_refusals },
        { "A1 over the Extension, the Extension without A1, tags over the NewScreen, first SA_ErrorCode", test_open_precedence },
        { "SA_Font keeps style, flags and TA_DeviceDPI tags natively; the 68k TextAttr drops FSF_TAGGED", test_open_font_flags },
        { "CloseScreen only of opened screens; refusals keep everything; one identity across locks, list and windows", test_close_ownership },
        { "Full tables refuse before Intuition; missing memory or font closes Intuition's screen again", test_open_bridge_failures },
        { "Teardown releases locks, then closes window-free opened screens, then fonts", test_teardown }
    };
    for (const auto &test : tests)
    {
        std::printf("RUN: %s\n", test.name); std::fflush(stdout);
        test.run();
        std::printf("PASS: %s\n", test.name);
    }
    std::printf("Public screens: %zu groups passed\n", std::size(tests));
    return 0;
}
