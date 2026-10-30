/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    GadTools menus of 68k code through literal LVOs, the real dispatcher
    and heap: VisualInfo handles, CreateMenusA() from a 68k NewMenu array
    and its tags, the 68k mirror of the strip with its UserData, texts,
    fonts and separators, the layout, SetMenuStrip(), ResetMenuStrip() and
    ClearMenuStrip() on a 68k window, ItemAddress(), OffMenu() and OnMenu(),
    the flags and NextSelect codes of a menu pick before the program reads
    them, FreeMenus(), DrawBevelBoxA() with native RastPorts and checked
    tags, refusals and lack of memory. Strips that the program built
    itself: the native copy that Intuition gets, the flags and codes that
    pass between the two by bit, windows that share a copy, CloseWindow()
    and the end of the program, and every strip that the bridge refuses.
    The native GadTools and Intuition below build and change menus as AROS
    does and record every call.
*/

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <exec/memory.h>
#include <graphics/rastport.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include "kaffeine_intern.h"
#include "kaffeine_copy.h"
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"

/* These fixtures borrow metadata-only fonts and acquire no native opens. */
extern "C" void CloseFont(struct TextFont *) { assert(false); }
#include "kaffeine_window.h"
#include "kaffeine_menus.h"
#include "kaffeine_dispatch.h"

/* ── The native side ─────────────────────────────────────────────── */

static unsigned native_calls;

/* AllocVec blocks of the menu bridge */
static std::set<APTR> native_blocks;
static int allocations_before_failure = -1;
static ULONG last_allocation;
extern "C" APTR AllocVec(ULONG size, ULONG)
{
    last_allocation = size;
    if (allocations_before_failure == 0)
    {
        allocations_before_failure = -1;
        return nullptr;
    }
    if (allocations_before_failure > 0)
        --allocations_before_failure;
    APTR block = std::calloc(1, size);
    assert(block && native_blocks.insert(block).second);
    return block;
}
extern "C" void FreeVec(APTR block)
{
    if (!block)
        return;
    assert(native_blocks.erase(block));
    std::free(block);
}

static std::map<MsgPort *, std::deque<IntuiMessage *>> queues;
static std::vector<IntuiMessage *> replied, made;
static MsgPort port0;

/* workbench.library is not linked here: no port has App objects. */
extern "C" BOOL Kaffeine_AppPortFill(KaffeineContext *, ULONG, BOOL) { return FALSE; }
extern "C" BOOL Kaffeine_AppPortMatches(KaffeineContext *, ULONG, Task *, UBYTE) { return TRUE; }
extern "C" BOOL Kaffeine_AppReplyMsg(KaffeineContext *, ULONG) { return FALSE; }

extern "C" struct Message *GetMsg(struct MsgPort *port)
{
    ++native_calls;
    auto &q = queues[port];
    if (q.empty())
        return nullptr;
    IntuiMessage *im = q.front();
    q.pop_front();
    return &im->ExecMessage;
}

extern "C" struct Message *WaitPort(struct MsgPort *port)
{
    ++native_calls;
    assert(!queues[port].empty());
    return &queues[port].front()->ExecMessage;
}

extern "C" void ReplyMsg(struct Message *message)
{
    ++native_calls;
    replied.push_back(reinterpret_cast<IntuiMessage *>(message));
}

extern "C" void PutMsg(struct MsgPort *, struct Message *) { assert(false); }
extern "C" BOOL ModifyIDCMP(struct Window *, ULONG) { assert(false); return FALSE; }
extern "C" void FreeSysRequest(Window *) { assert(false); }
extern "C" void CloseWindow(struct Window *) { ++native_calls; }
extern "C" struct Window *OpenWindowTagList(struct NewWindow *, struct TagItem *) { assert(false); return nullptr; }

/* GadTools copies messages as AROS's GT_IntuiMessage. */
struct GTMessage { IntuiMessage imsg; IntuiMessage *orig; };
static unsigned gt_live;

extern "C" struct IntuiMessage *GT_FilterIMsg(struct IntuiMessage *imsg)
{
    ++native_calls;
    auto *copy = new GTMessage{};
    copy->imsg = *imsg;
    copy->orig = imsg;
    ++gt_live;
    return &copy->imsg;
}

extern "C" struct IntuiMessage *GT_PostFilterIMsg(struct IntuiMessage *imsg)
{
    ++native_calls;
    auto *copy = reinterpret_cast<GTMessage *>(imsg);
    IntuiMessage *orig = copy->orig;
    delete copy;
    --gt_live;
    return orig;
}

extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *)
{
    assert(false);
    return 0;
}

/* VisualInfos */
struct VisualInfo { struct Screen *screen; };
static std::set<VisualInfo *> visualinfos;

/* Native public-screen visitor accounting for VisualInfo lifetime. */
static KaffeineContext *visual_context;
static std::map<Screen *, unsigned> visual_locks;
static bool visual_lock_denied;
static List visual_list{};
static PubScreenNode visual_nodes[16]{};
static unsigned visual_node_count, visual_list_depth;
extern "C" List *LockPubScreenList(void)
{
    ++visual_list_depth;
    visual_node_count = 0;
    for (UWORD i = 0; i < visual_context->num_shadows && visual_node_count < 16; ++i)
        if (visual_context->shadow_map[i].type == KAFFEINE_SHADOW_PUBSCREEN)
        {
            auto &p = visual_nodes[visual_node_count++];
            p = PubScreenNode{};
            p.psn_Screen = static_cast<Screen *>(visual_context->shadow_map[i].native_ptr);
            p.psn_Node.ln_Name = const_cast<STRPTR>("fixture-public");
        }
    visual_list = List{};
    visual_list.lh_Head = visual_node_count ? &visual_nodes[0].psn_Node : reinterpret_cast<Node *>(&visual_list.lh_Tail);
    for (unsigned i = 0; i < visual_node_count; ++i)
        visual_nodes[i].psn_Node.ln_Succ = i + 1 < visual_node_count ? &visual_nodes[i+1].psn_Node : reinterpret_cast<Node *>(&visual_list.lh_Tail);
    return &visual_list;
}
extern "C" void UnlockPubScreenList(void) { assert(visual_list_depth); --visual_list_depth; }
extern "C" Screen *LockPubScreen(CONST_STRPTR name)
{
    assert(visual_list_depth);
    if (visual_lock_denied) return nullptr;
    for (unsigned i = 0; i < visual_node_count; ++i)
        if (name == visual_nodes[i].psn_Node.ln_Name)
        {
            ++visual_locks[visual_nodes[i].psn_Screen];
            return visual_nodes[i].psn_Screen;
        }
    return nullptr;
}
extern "C" void UnlockPubScreen(UBYTE *name, Screen *screen)
{
    assert(!name && visual_locks[screen]);
    --visual_locks[screen];
}

extern "C" APTR GetVisualInfoA(struct Screen *screen, struct TagItem *tagList)
{
    ++native_calls;
    assert(!tagList);
    auto *vi = new VisualInfo{ screen };
    visualinfos.insert(vi);
    return vi;
}

extern "C" void FreeVisualInfo(APTR vi)
{
    ++native_calls;
    auto *v = static_cast<VisualInfo *>(vi);
    assert(visualinfos.erase(v));
    delete v;
}

/* DrawBevelBoxA() of AROS takes the geometry as WORDs and reads only
   GT_VisualInfo, GTBB_Recessed and GTBB_FrameType from the list; the stub
   keeps each call with the tags it got. */
struct BevelCall
{
    RastPort *rp;
    WORD left, top, width, height;
    std::vector<TagItem> tags;
};
static std::vector<BevelCall> bevels;

extern "C" void DrawBevelBoxA(struct RastPort *rport, WORD left, WORD top, WORD width, WORD height,
                              struct TagItem *taglist)
{
    ++native_calls;
    BevelCall call{ rport, left, top, width, height, {} };
    for (TagItem *t = taglist; t && t->ti_Tag != TAG_DONE; ++t)
        call.tags.push_back(*t);
    bevels.push_back(call);
}

/* Menus, one block per title and item as AROS's GadTools makes them: the
   Menu or MenuItem, its UserData, and for a text item two IntuiTexts, the
   second for a command string or the mark of a submenu. A separator is a
   native image. */
struct TitleBlock { Menu menu; APTR userdata; };
struct ItemBlock { MenuItem item; APTR userdata; IntuiText texts[2]; };
static std::map<void *, std::vector<void *>> strips;     /* root -> blocks */
static std::set<void *> bars;
static bool create_fails;
static std::vector<TagItem> create_tags;
static const char subitem_mark[] = "\xbb";

static IPTR tag_data(TagItem *tags, Tag tag, IPTR def)
{
    for (TagItem *t = tags; t && t->ti_Tag != TAG_DONE; ++t)
        if (t->ti_Tag == tag)
            return t->ti_Data;
    return def;
}

static TagItem *find_tag(TagItem *tags, Tag tag)
{
    for (TagItem *t = tags; t && t->ti_Tag != TAG_DONE; ++t)
        if (t->ti_Tag == tag)
            return t;
    return nullptr;
}

static MenuItem *make_item(NewMenu *nm, ULONG frontpen, std::vector<void *> &blocks)
{
    auto *b = new ItemBlock{};
    MenuItem *mi = &b->item;

    blocks.push_back(b);
    mi->Flags = ((nm->nm_Flags & (NM_ITEMDISABLED | CHECKIT | MENUTOGGLE | CHECKED)) ^ ITEMENABLED) |
                HIGHCOMP;
    if (nm->nm_CommKey && !(nm->nm_Flags & NM_COMMANDSTRING))
    {
        mi->Flags |= COMMSEQ;
        mi->Command = nm->nm_CommKey[0];
    }
    mi->MutualExclude = nm->nm_MutualExclude;
    b->userdata = nm->nm_UserData;
    if (nm->nm_Label == NM_BARLABEL)
    {
        auto *bar = new Image{};
        bar->LeftEdge = 4; bar->TopEdge = 3; bar->Width = 20; bar->Height = 2;
        bars.insert(bar);
        blocks.push_back(bar);
        mi->Flags &= ~(ITEMENABLED | ITEMTEXT);
        mi->ItemFill = bar;
        return mi;
    }
    IntuiText *it = &b->texts[0];
    mi->ItemFill = it;
    it->FrontPen = (UBYTE)frontpen;
    it->DrawMode = JAM1;
    it->IText = (UBYTE *)nm->nm_Label;
    mi->Flags |= ITEMTEXT;
    if (nm->nm_CommKey && (nm->nm_Flags & NM_COMMANDSTRING))
    {
        b->texts[1] = *it;
        b->texts[1].IText = (UBYTE *)nm->nm_CommKey;
        it->NextText = &b->texts[1];
    }
    return mi;
}

static void free_blocks(std::vector<void *> &blocks)
{
    for (void *p : blocks)
    {
        if (bars.erase(p))
            delete static_cast<Image *>(p);
        else
            ::operator delete(p);
    }
}

extern "C" struct Menu *CreateMenusA(struct NewMenu *nm, struct TagItem *tags)
{
    ++native_calls;
    create_tags.clear();
    for (TagItem *t = tags; t; ++t)
    {
        create_tags.push_back(*t);
        if (t->ti_Tag == TAG_DONE)
            break;
    }
    TagItem *err = find_tag(tags, GTMN_SecondaryError);
    if (create_fails)
    {
        /* AROS writes through the pointer when it runs out of memory at once */
        if (err)
            *(ULONG *)err->ti_Data = GTMENU_NOMEM;
        return nullptr;
    }
    const ULONG frontpen = tag_data(tags, GTMN_FrontPen, 0);
    const bool full = tag_data(tags, GTMN_FullMenu, FALSE);
    std::vector<void *> blocks;
    Menu *first = nullptr, *menu = nullptr;
    MenuItem *firstitem = nullptr, *item = nullptr, *sub = nullptr;
    ULONG code = 0;

    for (; nm->nm_Type != NM_END; ++nm)
    {
        if (nm->nm_Type == NM_TITLE)
        {
            auto *b = new TitleBlock{};
            blocks.push_back(b);
            b->menu.Flags = (nm->nm_Flags & NM_MENUDISABLED) ^ MENUENABLED;
            b->menu.MenuName = nm->nm_Label;
            b->userdata = nm->nm_UserData;
            (menu ? menu->NextMenu : first) = &b->menu;
            menu = &b->menu;
            item = sub = nullptr;
        }
        else if (nm->nm_Type == NM_ITEM)
        {
            if (full && !menu)
            {
                code = GTMENU_INVALID;
                break;
            }
            MenuItem *mi = make_item(nm, frontpen, blocks);
            if (item)
                item->NextItem = mi;
            else if (menu)
                menu->FirstItem = mi;
            else
                firstitem = mi;
            item = mi;
            sub = nullptr;
        }
        else if (nm->nm_Type == NM_SUB)
        {
            if (!item)
            {
                code = GTMENU_INVALID;
                break;
            }
            MenuItem *si = make_item(nm, frontpen, blocks);
            (sub ? sub->NextItem : item->SubItem) = si;
            sub = si;
            /* the mark of a submenu replaces the command key */
            item->Flags &= ~COMMSEQ;
            if (item->Flags & ITEMTEXT)
            {
                auto *it = static_cast<IntuiText *>(item->ItemFill);
                if (!it->NextText)
                {
                    it->NextText = it + 1;
                    it[1].IText = (UBYTE *)subitem_mark;
                }
            }
        }
        else
            assert(false);      /* the bridge passes only these */
    }
    /* AROS writes the error into the tag itself. */
    if (err)
        err->ti_Data = code;
    if (code)
    {
        free_blocks(blocks);
        return nullptr;
    }
    void *root = first ? (void *)first : (void *)firstitem;
    strips[root] = blocks;
    return static_cast<Menu *>(root);
}

extern "C" void FreeMenus(struct Menu *menu)
{
    ++native_calls;
    auto it = strips.find(menu);
    assert(it != strips.end());
    free_blocks(it->second);
    strips.erase(it);
}

/* A layout as GadTools makes one: places for titles and items, and the
   font of the texts, the one of GTMN_TextAttr or NULL for the screen's. */
static bool layout_fails;
static std::vector<TagItem> layout_tags;

static void record_layout_tags(TagItem *tags)
{
    layout_tags.clear();
    for (TagItem *t = tags; t; ++t)
    {
        layout_tags.push_back(*t);
        if (t->ti_Tag == TAG_DONE)
            break;
    }
}

static void layout_items(MenuItem *mi, TextAttr *font, WORD left)
{
    for (WORD top = 0; mi; mi = mi->NextItem, top += 12)
    {
        mi->LeftEdge = left;
        mi->TopEdge = top;
        mi->Width = 80;
        mi->Height = 12;
        if (mi->Flags & ITEMTEXT)
            for (auto *it = static_cast<IntuiText *>(mi->ItemFill); it; it = it->NextText)
            {
                it->LeftEdge = it->NextText ? 2 : 60;
                it->TopEdge = 1;
                it->ITextFont = font;
            }
        if (mi->SubItem)
            layout_items(mi->SubItem, font, 70);
    }
}

extern "C" BOOL LayoutMenusA(struct Menu *menu, APTR vi, struct TagItem *tags)
{
    ++native_calls;
    assert(visualinfos.count(static_cast<VisualInfo *>(vi)));
    record_layout_tags(tags);
    auto *font = (TextAttr *)tag_data(tags, GTMN_TextAttr, 0);
    WORD left = 0;
    for (Menu *m = menu; m; m = m->NextMenu, left += 60)
    {
        m->LeftEdge = left;
        m->Width = 50;
        m->Height = 10;
        layout_items(m->FirstItem, font, 0);
    }
    return layout_fails ? FALSE : TRUE;
}

extern "C" BOOL LayoutMenuItemsA(struct MenuItem *item, APTR vi, struct TagItem *tags)
{
    ++native_calls;
    assert(visualinfos.count(static_cast<VisualInfo *>(vi)));
    record_layout_tags(tags);
    layout_items(item, (TextAttr *)tag_data(tags, GTMN_TextAttr, 0), 5);
    return TRUE;
}

/* Intuition, as AROS's */
extern "C" BOOL SetMenuStrip(struct Window *window, struct Menu *menu)
{
    ++native_calls;
    window->MenuStrip = menu;
    for (Menu *m = menu; m; m = m->NextMenu)
    {
        m->JazzX = 7;
        m->BeatY = 9;
    }
    return TRUE;
}

/* What ClearMenuStrip() and ResetMenuStrip() did to which window, for a
   test that follows them */
static std::function<void(char, Window *)> menu_call;

extern "C" BOOL ResetMenuStrip(struct Window *window, struct Menu *menu)
{
    ++native_calls;
    window->MenuStrip = menu;
    if (menu_call)
        menu_call('R', window);
    return TRUE;
}

extern "C" void ClearMenuStrip(struct Window *window)
{
    ++native_calls;
    if (menu_call)
        menu_call('C', window);
    window->MenuStrip = nullptr;
}

extern "C" struct MenuItem *ItemAddress(struct Menu *strip, UWORD number)
{
    ++native_calls;
    if (number == MENUNULL)
        return nullptr;
    Menu *m = strip;
    for (int i = 0; m && i < MENUNUM(number); ++i)
        m = m->NextMenu;
    if (!m)
        return nullptr;
    MenuItem *mi = m->FirstItem;
    for (int i = 0; mi && i < ITEMNUM(number); ++i)
        mi = mi->NextItem;
    if (mi && SUBNUM(number) != NOSUB && mi->SubItem)
    {
        mi = mi->SubItem;
        for (int i = 0; mi && i < SUBNUM(number); ++i)
            mi = mi->NextItem;
    }
    return mi;
}

static void on_off(struct Window *window, UWORD number, bool on)
{
    if (ITEMNUM(number) == 0x3F)
    {
        Menu *m = window->MenuStrip;
        for (int i = 0; m && i < MENUNUM(number); ++i)
            m = m->NextMenu;
        if (m)
            m->Flags = on ? m->Flags | MENUENABLED : m->Flags & ~MENUENABLED;
        return;
    }
    if (MenuItem *mi = ItemAddress(window->MenuStrip, number))
        mi->Flags = on ? mi->Flags | ITEMENABLED : mi->Flags & ~ITEMENABLED;
}

extern "C" void OffMenu(struct Window *window, UWORD number)
{
    ++native_calls;
    on_off(window, number, false);
}

extern "C" void OnMenu(struct Window *window, UWORD number)
{
    ++native_calls;
    on_off(window, number, true);
}

/* ── The guest side ──────────────────────────────────────────────── */

struct TestCPU
{
    ULONG d[8] = {}, a[8] = {};
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

static const M68KThunkEntry exec_manual[] = { KAFFEINE_EXEC_IDCMP_THUNKS };
static const M68KThunkEntry exec_generated[] = { { 372, poison }, { 378, poison }, { 384, poison } };
static const M68KThunkEntry intuition_manual[] =
    { KAFFEINE_INTUITION_WINDOW_THUNKS, KAFFEINE_INTUITION_MENU_THUNKS };
static const M68KThunkEntry intuition_generated[] =
    { { 54, poison }, { 72, poison }, { 144, poison }, { 150, poison }, { 180, poison },
      { 192, poison }, { 264, poison }, { 702, poison } };
static const M68KThunkEntry gadtools_manual[] =
    { KAFFEINE_GADTOOLS_IDCMP_THUNKS, KAFFEINE_GADTOOLS_MENU_THUNKS };
static const M68KThunkEntry gadtools_generated[] =
    { { 48, poison }, { 54, poison }, { 60, poison }, { 66, poison }, { 72, poison },
      { 78, poison }, { 102, poison }, { 108, poison }, { 126, poison }, { 132, poison } };

/* Guest offsets of the NDK structures */
enum
{
    NM_SIZE = 20,
    MU_NEXTMENU = 0, MU_LEFTEDGE = 4, MU_TOPEDGE = 6, MU_WIDTH = 8, MU_HEIGHT = 10,
    MU_FLAGS = 12, MU_MENUNAME = 14, MU_FIRSTITEM = 18, MU_JAZZX = 22, MU_BEATY = 28,
    MU_USERDATA = 30, MENU_SIZE = 30,
    MI_NEXTITEM = 0, MI_LEFTEDGE = 4, MI_TOPEDGE = 6, MI_WIDTH = 8, MI_HEIGHT = 10,
    MI_FLAGS = 12, MI_MUTUALEXCLUDE = 14, MI_ITEMFILL = 18, MI_SELECTFILL = 22,
    MI_COMMAND = 26, MI_SUBITEM = 28, MI_NEXTSELECT = 32, MI_USERDATA = 34, MENUITEM_SIZE = 34,
    IT_FRONTPEN = 0, IT_BACKPEN = 1, IT_DRAWMODE = 2, IT_LEFTEDGE = 4, IT_TOPEDGE = 6,
    IT_ITEXTFONT = 8, IT_ITEXT = 12, IT_NEXTTEXT = 16, INTUITEXT_SIZE = 20,
    TA_NAME = 0, TA_YSIZE = 4, TA_STYLE = 6, TA_FLAGS = 7, TEXTATTR_SIZE = 8,
    IM_LEFTEDGE = 0, IM_TOPEDGE = 2, IM_WIDTH = 4, IM_HEIGHT = 6, IM_DEPTH = 8,
    IM_IMAGEDATA = 10, IM_PLANEPICK = 14, IM_PLANEONOFF = 15, IM_NEXTIMAGE = 16, IMAGE_SIZE = 20,
    IMSG_CLASS = 20, IMSG_CODE = 24
};

static const UWORD FILLER = 99;

struct Fixture
{
    static constexpr ULONG exec = 0x1000, intuition = 0x2000, gadtools = 0x3000;
    static constexpr ULONG stack = 0x3F00, task = 0x3800, raw = 0x3A00;
    static constexpr ULONG strings = 0x5000, newmenu = 0x5800, tags = 0x5C00, errout = 0x5D00;
    static constexpr ULONG textattr = 0x5D10, screen = 0x5D40, pubscreen = 0x5D80;
    static constexpr ULONG fillers = 0xF000;
    KaffeineContext ctx = {};
    std::array<UBYTE, 0x10000> memory;
    TestCPU state;
    KaffeineCPU cpu = { &ops, &state };
    Window native_window = {};
    int native_screen = 0, native_pubscreen = 0;
    ULONG window = 0, heap_avail = 0, next_string = strings;

    Fixture(RastPort *rport = nullptr)
    {
        native_calls = gt_live = 0;
        allocations_before_failure = -1;
        menu_call = nullptr;
        bevels.clear();
        queues.clear();
        replied.clear();
        made.clear();
        create_fails = layout_fails = false;
        memory.fill(0xA5);
        visual_context = &ctx;
        visual_locks.clear(); visual_lock_denied = false; visual_list_depth = 0;
        ctx.mem = memory.data();
        ctx.mem_size = memory.size();
        ctx.heap_start = 0x6000;
        ctx.heap_end = 0xF000;
        assert(Kaffeine_HeapInit(&ctx));
        ctx.m68k_sysbase = exec;
        m68k_write32(&ctx, exec + M68K_EXECBASE_THISTASK, task);
        lib(0, exec, "exec.library", exec_manual, std::size(exec_manual),
            exec_generated, std::size(exec_generated));
        lib(1, intuition, "intuition.library", intuition_manual, std::size(intuition_manual),
            intuition_generated, std::size(intuition_generated));
        lib(2, gadtools, "gadtools.library", gadtools_manual, std::size(gadtools_manual),
            gadtools_generated, std::size(gadtools_generated));
        ctx.num_libs = 3;
        m68k_write32(&ctx, stack, 0x76543210);

        /* a window with an IDCMP port, a screen of the program and a
           public screen as LockPubScreen() gives it */
        native_window.Width = 300;
        native_window.RPort = rport;
        native_window.UserPort = &port0;
        window = Kaffeine_WindowCreate(&ctx, &native_window);
        assert(window);
        assert(Kaffeine_ShadowAdd(&ctx, screen, &native_screen, KAFFEINE_SHADOW_SCREEN));
        assert(Kaffeine_ShadowAdd(&ctx, pubscreen, &native_pubscreen, KAFFEINE_SHADOW_PUBSCREEN));
        heap_avail = Kaffeine_HeapAvail(&ctx, 0);
    }

    ~Fixture()
    {
        for (IntuiMessage *im : made)
            delete im;
        made.clear();
        assert(!gt_live);
    }

    void lib(int i, ULONG base, const char *name, const M68KThunkEntry *manual, ULONG nmanual,
             const M68KThunkEntry *generated, ULONG ngenerated)
    {
        auto &l = ctx.libs[i];
        l.m68k_addr = base;
        l.jt_start = base - 0xC00;
        std::strcpy(l.name, name);
        l.thunks = manual;
        l.num_thunks = nmanual;
        l.gen_thunks = generated;
        l.num_gen_thunks = ngenerated;
    }

    /* A literal LVO through the dispatcher; only D0 may change. */
    ULONG call(ULONG base, ULONG lvo, std::initializer_list<ULONG> a = {},
               std::initializer_list<ULONG> d = {})
    {
        unsigned i;

        for (i = 0; i < 8; ++i)
        {
            state.d[i] = 0xD0000000 + i;
            state.a[i] = 0xA0000000 + i;
        }
        i = 0;
        for (ULONG v : a) state.a[i++] = v;
        i = 0;
        for (ULONG v : d) state.d[i++] = v;
        state.a[7] = stack;
        const auto before = state;
        ctx.running = TRUE; ctx.exit_code = 0;
        generated_calls = 0;
        KaffeineDispatchResult result = {};
        assert(Kaffeine_DispatchLibraryCall(&ctx, &cpu, base - lvo, &result));
        assert(result.return_pc == 0x76543210 && !result.supervisor);
        assert(state.a[7] == stack + 4);
        for (i = 1; i < 8; ++i) assert(state.d[i] == before.d[i]);
        for (i = 0; i < 7; ++i) assert(state.a[i] == before.a[i]);
        assert(state.sr == before.sr && state.pc == before.pc);
        assert(generated_calls == 0);
        return state.d[0];
    }
    bool stopped() const { return !ctx.running && ctx.exit_code == 20 && state.d[0] == 20; }

    void leave_shadows(UWORD n)
    {
        static int filler;
        for (ULONG a = fillers; ctx.num_shadows < KAFFEINE_MAX_SHADOWS - n; a += 4)
            assert(Kaffeine_ShadowAdd(&ctx, a, &filler, FILLER));
    }

    ULONG lng(ULONG addr) { return m68k_read32(&ctx, addr); }
    UWORD word(ULONG addr) { return m68k_read16(&ctx, addr); }
    std::string str(ULONG addr) { return reinterpret_cast<const char *>(memory.data() + addr); }

    /* A guest string for the NewMenu */
    ULONG s(const char *text)
    {
        const ULONG at = next_string;
        std::strcpy(reinterpret_cast<char *>(memory.data() + at), text);
        next_string += std::strlen(text) + 1;
        return at;
    }
    struct Entry { UBYTE type; ULONG label, key; UWORD flags; LONG exclude; ULONG userdata; };
    void newmenus(std::initializer_list<Entry> entries)
    {
        ULONG at = newmenu;
        for (const Entry &e : entries)
        {
            memory[at] = e.type;
            memory[at + 1] = 0;
            m68k_write32(&ctx, at + 2, e.label);
            m68k_write32(&ctx, at + 6, e.key);
            m68k_write16(&ctx, at + 10, e.flags);
            m68k_write32(&ctx, at + 12, (ULONG)e.exclude);
            m68k_write32(&ctx, at + 16, e.userdata);
            at += NM_SIZE;
        }
        std::memset(memory.data() + at, 0, NM_SIZE);
    }
    void tagitems(std::initializer_list<std::pair<ULONG, ULONG>> list, ULONG at = tags)
    {
        for (auto &t : list)
        {
            m68k_write32(&ctx, at, t.first);
            m68k_write32(&ctx, at + 4, t.second);
            at += 8;
        }
        m68k_write32(&ctx, at, TAG_DONE);
    }
    ULONG create(ULONG taglist = 0) { return call(gadtools, 48, { newmenu, taglist }); }
    ULONG visualinfo(ULONG scr) { return call(gadtools, 126, { scr, 0 }); }
    ULONG item(ULONG strip, UWORD menu, UWORD item, UWORD sub = NOSUB)
    {
        return call(intuition, 144, { strip }, { (ULONG)FULLMENUNUM(menu, item, sub) });
    }
    ULONG port() { return lng(window + M68K_WIN_USERPORT); }

    /* Take all of the guest heap, and give it back. */
    std::vector<std::pair<ULONG, ULONG>> taken;
    void exhaust_heap()
    {
        for (ULONG size = 0x1000; size >= 8; size /= 2)
            while (ULONG block = Kaffeine_HeapAlloc(&ctx, size, MEMF_ANY))
                taken.push_back({ block, size });
    }
    void release_heap()
    {
        for (auto &b : taken)
            Kaffeine_HeapFree(&ctx, b.first, b.second);
        taken.clear();
    }
};

/* The usual strip: Project with Open (key O), a separator, a submenu Recent
   with two files and Quit (key Q, command string); Edit disabled with
   Undo; Options with two mutually exclusive CHECKIT items and a toggle. */
static ULONG usual(Fixture &f)
{
    const ULONG project = f.s("Project"), open = f.s("Open..."), o = f.s("O");
    const ULONG recent = f.s("Recent"), one = f.s("one.txt"), two = f.s("two.txt");
    const ULONG quit = f.s("Quit"), q = f.s("Ctrl Q"), edit = f.s("Edit"), undo = f.s("Undo");
    const ULONG options = f.s("Options"), small = f.s("Small"), large = f.s("Large");
    const ULONG sound = f.s("Sound");
    f.newmenus({
        { NM_TITLE, project, 0, 0, 0, 0x100 },
        { NM_ITEM, open, o, 0, 0, 0x101 },
        { NM_ITEM, 0xFFFFFFFF, 0, 0, 0, 0 },
        { NM_ITEM, recent, 0, 0, 0, 0x102 },
        { NM_SUB, one, 0, 0, 0, 0x103 },
        { NM_SUB, two, 0, 0, 0, 0x104 },
        { NM_IGNORE | NM_ITEM, 0xDEADBEEF, 0xDEADBEEF, 0, 0, 0 },
        { NM_ITEM, quit, q, NM_COMMANDSTRING, 0, 0x105 },
        { NM_TITLE, edit, 0, NM_MENUDISABLED, 0, 0x200 },
        { NM_ITEM, undo, 0, NM_ITEMDISABLED, 0, 0x201 },
        { NM_TITLE, options, 0, 0, 0, 0x300 },
        { NM_ITEM, small, 0, CHECKIT | CHECKED, 2, 0x301 },
        { NM_ITEM, large, 0, CHECKIT, 1, 0x302 },
        { NM_ITEM, sound, 0, CHECKIT | MENUTOGGLE, 0, 0x303 },
    });
    return f.create();
}

/* A strip that the program built itself, in guest memory from 0x4000 on,
   written field by field in the NDK layout */
struct Built
{
    Fixture &f;
    ULONG at = 0x4000;

    ULONG take(ULONG size)
    {
        const ULONG a = at;
        at += (size + 1) & ~1u;
        assert(at <= Fixture::strings);
        std::memset(f.memory.data() + a, 0, size);
        return a;
    }
    ULONG str(const char *s)
    {
        const ULONG a = take(std::strlen(s) + 1);
        std::strcpy(reinterpret_cast<char *>(f.memory.data() + a), s);
        return a;
    }
    void w16(ULONG a, UWORD v) { m68k_write16(&f.ctx, a, v); }
    void w32(ULONG a, ULONG v) { m68k_write32(&f.ctx, a, v); }
    ULONG font(const char *name, UWORD ysize, UBYTE style)
    {
        const ULONG n = str(name), a = take(TEXTATTR_SIZE);
        w32(a + TA_NAME, n);
        w16(a + TA_YSIZE, ysize);
        f.memory[a + TA_STYLE] = style;
        f.memory[a + TA_FLAGS] = 0x40;
        return a;
    }
    ULONG text(const char *s, ULONG font = 0, ULONG next = 0, WORD left = 0, WORD top = 0)
    {
        const ULONG n = s ? str(s) : 0, a = take(INTUITEXT_SIZE);
        f.memory[a + IT_FRONTPEN] = 1;
        f.memory[a + IT_BACKPEN] = 2;
        f.memory[a + IT_DRAWMODE] = JAM1;
        w16(a + IT_LEFTEDGE, (UWORD)left);
        w16(a + IT_TOPEDGE, (UWORD)top);
        w32(a + IT_ITEXTFONT, font);
        w32(a + IT_ITEXT, n);
        w32(a + IT_NEXTTEXT, next);
        return a;
    }
    ULONG image(WORD width, WORD height, WORD depth, ULONG data, UBYTE pick, UBYTE onoff,
                ULONG next = 0)
    {
        const ULONG a = take(IMAGE_SIZE);
        w16(a + IM_LEFTEDGE, 1);
        w16(a + IM_TOPEDGE, (UWORD)-1);
        w16(a + IM_WIDTH, (UWORD)width);
        w16(a + IM_HEIGHT, (UWORD)height);
        w16(a + IM_DEPTH, (UWORD)depth);
        w32(a + IM_IMAGEDATA, data);
        f.memory[a + IM_PLANEPICK] = pick;
        f.memory[a + IM_PLANEONOFF] = onoff;
        w32(a + IM_NEXTIMAGE, next);
        return a;
    }
    ULONG item(UWORD flags, ULONG fill, ULONG next = 0, ULONG sub = 0, WORD top = 0)
    {
        const ULONG a = take(MENUITEM_SIZE);
        w32(a + MI_NEXTITEM, next);
        w16(a + MI_TOPEDGE, (UWORD)top);
        w16(a + MI_WIDTH, 80);
        w16(a + MI_HEIGHT, 10);
        w16(a + MI_FLAGS, flags);
        w32(a + MI_ITEMFILL, fill);
        w32(a + MI_SUBITEM, sub);
        w16(a + MI_NEXTSELECT, MENUNULL);
        return a;
    }
    ULONG menu(const char *name, UWORD flags, ULONG first, ULONG next = 0)
    {
        const ULONG n = str(name), a = take(MENU_SIZE);
        w32(a + MU_NEXTMENU, next);
        w16(a + MU_LEFTEDGE, 2);
        w16(a + MU_WIDTH, 60);
        w16(a + MU_HEIGHT, 9);
        w16(a + MU_FLAGS, flags);
        w32(a + MU_MENUNAME, n);
        w32(a + MU_FIRSTITEM, first);
        return a;
    }
};

/* Project: Open (key O, two texts in a font with tags), Check (a toggle),
   More (two subitems, the second with a SubItem that Intuition never
   follows) and a picture (two planes, a chained image without data, and
   a select image); Edit: A and B, which exclude each other. Some items
   and Edit carry flags that only Intuition sets. */
struct Classic
{
    ULONG strip, project, edit, open, check, more, sub1, sub2, picture, a, b;
    ULONG font, open_text, open_key, data, image, image2, select;
};

static Classic classic(Built &g)
{
    Classic c{};
    c.font = g.font("topaz.font", 8, FSF_TAGGED | FSF_UNDERLINED);
    c.open_key = g.text("Ctrl", c.font, 0, 50, 1);
    c.open_text = g.text("Open", c.font, c.open_key, -3, 1);
    c.data = g.take(16);
    for (ULONG i = 0; i < 16; ++i)
        g.f.memory[c.data + i] = (UBYTE)(0x80 + i);
    c.image2 = g.image(4, 4, 2, 0xA5A5A5A5, 0, 2);
    c.image = g.image(20, 2, 2, c.data, 3, 0, c.image2);
    c.select = g.image(20, 2, 1, c.data, 0xFF, 0);
    c.b = g.item(ITEMTEXT | CHECKIT | ITEMENABLED | HIGHCOMP, g.text("B"), 0, 0, 10);
    g.w32(c.b + MI_MUTUALEXCLUDE, 1);
    c.a = g.item(ITEMTEXT | CHECKIT | CHECKED | ITEMENABLED | HIGHCOMP, g.text("A"), c.b);
    g.w32(c.a + MI_MUTUALEXCLUDE, 2);
    c.edit = g.menu("Edit", MENUENABLED | MIDRAWN, c.a);
    c.picture = g.item(ITEMENABLED, c.image, 0, 0, 30);
    g.w32(c.picture + MI_SELECTFILL, c.select);
    c.sub2 = g.item(ITEMTEXT | ITEMENABLED | HIGHCOMP, g.text("Sub2"), 0, 0xA5A5A5A5, 10);
    c.sub1 = g.item(ITEMTEXT | ITEMENABLED | HIGHCOMP, g.text("Sub1"), c.sub2, 0, -2);
    g.w16(c.sub1 + MI_LEFTEDGE, 60);
    c.more = g.item(ITEMTEXT | ITEMENABLED | HIGHBOX | 0x8000, g.text("More", c.font), c.picture,
                    c.sub1, 20);
    c.check = g.item(ITEMTEXT | CHECKIT | MENUTOGGLE | ITEMENABLED | HIGHCOMP | HIGHITEM | ISDRAWN,
                     g.text("Check"), c.more, 0, 10);
    c.open = g.item(ITEMTEXT | COMMSEQ | ITEMENABLED | HIGHCOMP | MENUTOGGLED, c.open_text, c.check);
    g.f.memory[c.open + MI_COMMAND] = 'O';
    g.w16(c.open + MI_NEXTSELECT, 0);
    c.project = c.strip = g.menu("Project", MENUENABLED, c.open, c.edit);
    return c;
}

static void test_registration()
{
    const struct { ULONG lvo; IPTR (*thunk)(KaffeineContext *, void *); } gt[] =
        { { 48, Kaffeine_CreateMenusA }, { 54, Kaffeine_FreeMenus }, { 60, Kaffeine_LayoutMenuItemsA },
          { 66, Kaffeine_LayoutMenusA }, { 120, Kaffeine_DrawBevelBoxA }, { 126, Kaffeine_GetVisualInfoA },
          { 132, Kaffeine_FreeVisualInfo } },
      in[] =
        { { 54, Kaffeine_ClearMenuStrip }, { 144, Kaffeine_ItemAddress }, { 180, Kaffeine_OffMenu },
          { 192, Kaffeine_OnMenu }, { 264, Kaffeine_SetMenuStrip }, { 702, Kaffeine_ResetMenuStrip } };
    const M68KThunkEntry gadtools_menu[] = { KAFFEINE_GADTOOLS_MENU_THUNKS };
    const M68KThunkEntry intuition_menu[] = { KAFFEINE_INTUITION_MENU_THUNKS };

    assert(std::size(gadtools_menu) == 7 && std::size(intuition_menu) == 6);
    for (unsigned i = 0; i < 7; ++i)
        assert(gadtools_menu[i].lvo == gt[i].lvo && gadtools_menu[i].thunk == gt[i].thunk);
    for (unsigned i = 0; i < 6; ++i)
        assert(intuition_menu[i].lvo == in[i].lvo && intuition_menu[i].thunk == in[i].thunk);
}

static void test_visualinfo()
{
    Fixture f;

    /* a screen of the program, and one from LockPubScreen() */
    const ULONG vi = f.visualinfo(Fixture::screen), pub = f.visualinfo(Fixture::pubscreen);
    assert(vi && pub && vi != pub && visualinfos.size() == 2);
    auto *native = static_cast<VisualInfo *>(Kaffeine_VisualInfoNative(&f.ctx, vi));
    assert(native && native->screen == reinterpret_cast<Screen *>(&f.native_screen));
    assert(static_cast<VisualInfo *>(Kaffeine_VisualInfoNative(&f.ctx, pub))->screen ==
           reinterpret_cast<Screen *>(&f.native_pubscreen));
    f.call(Fixture::gadtools, 132, { vi });
    f.call(Fixture::gadtools, 132, { pub });
    assert(visualinfos.empty() && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);

    /* no screen gives no VisualInfo, as GadTools; anything else stops the
       program; freeing nothing or a stranger does nothing */
    const unsigned calls = native_calls;
    assert(f.visualinfo(0) == 0 && !f.stopped());
    f.call(Fixture::gadtools, 132, { 0 });
    f.call(Fixture::gadtools, 132, { Fixture::raw });
    assert(native_calls == calls && !f.stopped());
    f.visualinfo(Fixture::raw);
    assert(f.stopped() && native_calls == calls);

    /* without room for the handle, the native VisualInfo goes again */
    f.leave_shadows(0);
    assert(f.visualinfo(Fixture::screen) == 0 && visualinfos.empty());
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

static void test_visualinfo_lifetime()
{
    Fixture f;
    auto *screen = reinterpret_cast<Screen *>(&f.native_pubscreen);
    ULONG owned = f.visualinfo(Fixture::screen), borrowed = f.visualinfo(Fixture::pubscreen);
    assert(owned && borrowed && visual_locks[screen] == 1);
    Kaffeine_VisualInfosCleanup(&f.ctx);
    assert(visualinfos.empty() && visual_locks[screen] == 0 && !visual_list_depth);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    unsigned calls = native_calls;
    Kaffeine_VisualInfosCleanup(&f.ctx);
    assert(native_calls == calls);
    visual_lock_denied = true;
    assert(!f.visualinfo(Fixture::pubscreen) && visualinfos.empty() && visual_locks[screen] == 0);
    visual_lock_denied = false;
    for (int i = 0; i < KAFFEINE_MAX_VISUALINFOS; ++i) assert(f.visualinfo(Fixture::screen));
    calls = native_calls;
    assert(!f.visualinfo(Fixture::screen) && native_calls == calls);
    Kaffeine_VisualInfosCleanup(&f.ctx);
    assert(visualinfos.empty() && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

static void assert_bevel_tags(const BevelCall &call, std::initializer_list<TagItem> expected)
{
    assert(call.tags.size() == expected.size());
    for (const TagItem &tag : expected)
    {
        const auto actual = std::find_if(call.tags.begin(), call.tags.end(),
            [&tag](const TagItem &t) { return t.ti_Tag == tag.ti_Tag; });
        assert(actual != call.tags.end() && actual->ti_Data == tag.ti_Data);
    }
}

static void assert_bevel_refused(Fixture &f, ULONG rport, ULONG taglist)
{
    const unsigned calls = native_calls;
    const auto count = bevels.size();
    assert(f.call(Fixture::gadtools, 120, { rport, taglist }) == 20);
    assert(f.stopped());     /* !running, exit code 20 and D0 == 20 */
    assert(native_calls == calls && bevels.size() == count);
}

static void test_bevel_rastports_and_tags()
{
    RastPort window_rp = {}, screen_rp = {};
    Fixture f(&window_rp);
    const ULONG window_rport = f.lng(f.window + M68K_WIN_RPORT);
    const ULONG screen_rport = Fixture::screen + M68K_SCR_RASTPORT;
    assert(window_rport);
    assert(Kaffeine_RastPortAttach(&f.ctx, screen_rport, &screen_rp));
    const ULONG vi = f.visualinfo(Fixture::screen), other = f.visualinfo(Fixture::pubscreen);
    const APTR native_vi = Kaffeine_VisualInfoNative(&f.ctx, vi);
    assert(vi && other && native_vi && native_vi != Kaffeine_VisualInfoNative(&f.ctx, other));

    /* Only the first of each supported tag survives. Unknown tags carry
       both guest pointers and out-of-range data, neither to be resolved. */
    f.tagitems({ { GTBB_Recessed, 0x12345678 }, { GT_VisualInfo, vi },
                 { GTMN_TextAttr, Fixture::textattr }, { GTBB_FrameType, BBFT_RIDGE },
                 { 0x80001234, Fixture::raw }, { 0x80001235, 0xFFFFFFFC },
                 { GTBB_Recessed, FALSE }, { GT_VisualInfo, other },
                 { GTBB_FrameType, BBFT_BUTTON }, { GT_VisualInfo, Fixture::raw } });
    const struct { ULONG guest; RastPort *native; } ports[] =
        { { window_rport, &window_rp }, { screen_rport, &screen_rp } };
    for (const auto &port : ports)
    {
        const unsigned calls = native_calls;
        const auto count = bevels.size();
        assert(f.call(Fixture::gadtools, 120, { port.guest, Fixture::tags }, { 1, 2, 30, 40 }) == 0);
        assert(f.ctx.running && f.ctx.exit_code == 0);
        assert(native_calls == calls + 1 && bevels.size() == count + 1);
        assert(bevels.back().rp == port.native);
        assert_bevel_tags(bevels.back(), { { GT_VisualInfo, reinterpret_cast<IPTR>(native_vi) },
                                         { GTBB_Recessed, 0x12345678 }, { GTBB_FrameType, BBFT_RIDGE } });
    }
    f.call(Fixture::gadtools, 132, { vi });
    f.call(Fixture::gadtools, 132, { other });
    assert(visualinfos.empty());
}

static void test_bevel_tag_controls()
{
    RastPort rp = {};
    Fixture f(&rp);
    const ULONG rport = f.lng(f.window + M68K_WIN_RPORT), vi = f.visualinfo(Fixture::screen);
    const APTR native_vi = Kaffeine_VisualInfoNative(&f.ctx, vi);
    const ULONG more = Fixture::tags + 0x80;
    assert(rport && vi && native_vi);

    /* SKIP must bypass all three entries, including a bad MORE. MORE
       abandons the original tail; IGNORE's data is never a pointer. */
    f.tagitems({ { TAG_IGNORE, 0xFFFFFFFC }, { TAG_SKIP, 3 },
                 { GT_VisualInfo, Fixture::raw }, { GTBB_Recessed, FALSE },
                 { TAG_MORE, 0xFFFFFFFC }, { GTBB_Recessed, 0x12345678 },
                 { TAG_MORE, more }, { GT_VisualInfo, Fixture::raw } });
    f.tagitems({ { GTBB_FrameType, BBFT_ICONDROPBOX }, { TAG_IGNORE, Fixture::raw },
                 { GT_VisualInfo, vi }, { GTBB_Recessed, FALSE },
                 { GTBB_FrameType, BBFT_BUTTON }, { GT_VisualInfo, Fixture::raw } }, more);
    const unsigned calls = native_calls;
    assert(f.call(Fixture::gadtools, 120, { rport, Fixture::tags }) == 0);
    assert(f.ctx.running && f.ctx.exit_code == 0);
    assert(native_calls == calls + 1 && bevels.size() == 1 && bevels[0].rp == &rp);
    assert_bevel_tags(bevels[0], { { GT_VisualInfo, reinterpret_cast<IPTR>(native_vi) },
                                 { GTBB_Recessed, 0x12345678 }, { GTBB_FrameType, BBFT_ICONDROPBOX } });
    f.call(Fixture::gadtools, 132, { vi });
}

static void test_bevel_geometry()
{
    RastPort rp = {};
    Fixture f(&rp);
    const ULONG rport = f.lng(f.window + M68K_WIN_RPORT), vi = f.visualinfo(Fixture::screen);
    const APTR native_vi = Kaffeine_VisualInfoNative(&f.ctx, vi);
    assert(rport && vi && native_vi);
    f.tagitems({ { GT_VisualInfo, vi } });
    const struct { std::array<ULONG, 4> registers; std::array<WORD, 4> words; } cases[] =
    {
        { { 0x12345678, 0xABCD1234, 0x56787FFF, 0x9ABC0001 }, { 0x5678, 0x1234, 32767, 1 } },
        { { 0xFFFF8000, 0x1234FFFF, 0xABCD8001, 0xFFFFFEDC }, { -32768, -1, -32767, -292 } }
    };
    for (const auto &c : cases)
    {
        const unsigned calls = native_calls;
        const auto count = bevels.size();
        assert(f.call(Fixture::gadtools, 120, { rport, Fixture::tags },
                      { c.registers[0], c.registers[1], c.registers[2], c.registers[3] }) == 0);
        assert(f.ctx.running && f.ctx.exit_code == 0);
        assert(native_calls == calls + 1 && bevels.size() == count + 1);
        const auto &draw = bevels.back();
        assert(draw.rp == &rp);
        const std::array<WORD, 4> actual = { draw.left, draw.top, draw.width, draw.height };
        assert(actual == c.words);
        assert_bevel_tags(draw, { { GT_VisualInfo, reinterpret_cast<IPTR>(native_vi) } });
    }
    f.call(Fixture::gadtools, 132, { vi });
}

static void test_bevel_without_visualinfo()
{
    RastPort rp = {};
    Fixture f(&rp);
    const ULONG rport = f.lng(f.window + M68K_WIN_RPORT), vi = f.visualinfo(Fixture::screen);
    assert(rport && vi);
    f.tagitems({});
    f.tagitems({ { GTBB_Recessed, TRUE }, { GTBB_FrameType, BBFT_RIDGE },
                 { GTMN_TextAttr, Fixture::textattr } }, Fixture::tags + 0x40);
    f.tagitems({ { GT_VisualInfo, 0 } }, Fixture::tags + 0x80);
    /* A later valid or invalid VisualInfo cannot replace the first NULL. */
    f.tagitems({ { GT_VisualInfo, 0 }, { GT_VisualInfo, vi },
                 { GT_VisualInfo, Fixture::raw } }, Fixture::tags + 0xC0);
    const ULONG lists[] = { 0, Fixture::tags, Fixture::tags + 0x40,
                           Fixture::tags + 0x80, Fixture::tags + 0xC0 };
    const ULONG ports[] = { rport, 0, Fixture::raw, 0xFFFFFFFC };
    const unsigned calls = native_calls;
    for (ULONG list : lists)
        for (ULONG port : ports)
        {
            assert(f.call(Fixture::gadtools, 120, { port, list }) == 0);
            assert(f.ctx.running && f.ctx.exit_code == 0);
            assert(native_calls == calls && bevels.empty());
        }
    f.call(Fixture::gadtools, 132, { vi });
}

static void test_bevel_refused_handles()
{
    RastPort rp = {};
    Fixture f(&rp);
    const ULONG rport = f.lng(f.window + M68K_WIN_RPORT), vi = f.visualinfo(Fixture::screen);
    assert(rport && vi);
    /* An ordinary guest address is not a GetVisualInfoA() handle. */
    f.tagitems({ { GT_VisualInfo, Fixture::raw } });
    assert_bevel_refused(f, rport, Fixture::tags);

    /* A valid VisualInfo makes even a NULL RastPort an error. */
    f.tagitems({ { GT_VisualInfo, vi } });
    assert_bevel_refused(f, 0, Fixture::tags);
    assert_bevel_refused(f, Fixture::raw, Fixture::tags);

    f.call(Fixture::gadtools, 132, { vi });
    assert(visualinfos.empty() && !Kaffeine_VisualInfoNative(&f.ctx, vi));
    assert_bevel_refused(f, rport, Fixture::tags);
}

static void test_bevel_refused_taglists()
{
    RastPort rp = {};
    Fixture f(&rp);
    const ULONG rport = f.lng(f.window + M68K_WIN_RPORT), vi = f.visualinfo(Fixture::screen);
    const ULONG outside = f.ctx.mem_size, more = Fixture::tags + 0x40;
    assert(rport && vi);
    assert_bevel_refused(f, rport, outside);
    assert_bevel_refused(f, rport, 0xFFFFFFFC);
    /* Even TAG_DONE must fit a complete guest TagItem. */
    m68k_write32(&f.ctx, outside - 4, TAG_DONE);
    assert_bevel_refused(f, rport, outside - 4);

    f.tagitems({ { TAG_MORE, outside } });
    assert_bevel_refused(f, rport, Fixture::tags);
    /* A bad tail must still be checked after all useful tags were found. */
    f.tagitems({ { GT_VisualInfo, vi }, { GTBB_Recessed, TRUE },
                 { GTBB_FrameType, BBFT_RIDGE }, { TAG_MORE, outside } });
    assert_bevel_refused(f, rport, Fixture::tags);

    /* A control-only self-loop, and a two-list cycle with a VisualInfo. */
    f.tagitems({ { TAG_MORE, Fixture::tags } });
    assert_bevel_refused(f, rport, Fixture::tags);
    f.tagitems({ { GT_VisualInfo, vi }, { GTBB_Recessed, TRUE },
                 { GTBB_FrameType, BBFT_RIDGE }, { TAG_MORE, more } });
    f.tagitems({ { TAG_IGNORE, 0 }, { TAG_MORE, Fixture::tags } }, more);
    assert_bevel_refused(f, rport, Fixture::tags);
    f.call(Fixture::gadtools, 132, { vi });
}

static void test_create_mirror()
{
    Fixture f;
    const ULONG strip = usual(f);

    assert(strip && !f.stopped() && strips.size() == 1);
    /* the ignored entry never reaches GadTools */
    const ULONG project = strip, edit = f.lng(project + MU_NEXTMENU), options = f.lng(edit + MU_NEXTMENU);
    assert(edit && options && f.lng(options + MU_NEXTMENU) == 0);
    assert(f.str(f.lng(project + MU_MENUNAME)) == "Project" && f.lng(project + MU_USERDATA) == 0x100);
    assert(f.word(project + MU_FLAGS) == MENUENABLED && f.word(edit + MU_FLAGS) == 0 &&
           f.lng(edit + MU_USERDATA) == 0x200);
    /* the labels are the program's own strings */
    assert(f.lng(project + MU_MENUNAME) >= Fixture::strings && f.lng(project + MU_MENUNAME) < Fixture::newmenu);

    const ULONG open = f.lng(project + MU_FIRSTITEM), bar = f.lng(open + MI_NEXTITEM);
    const ULONG recent = f.lng(bar + MI_NEXTITEM), quit = f.lng(recent + MI_NEXTITEM);
    assert(quit && f.lng(quit + MI_NEXTITEM) == 0);
    /* Open: text with key O */
    assert(f.word(open + MI_FLAGS) == (ITEMTEXT | COMMSEQ | ITEMENABLED | HIGHCOMP) &&
           f.memory[open + MI_COMMAND] == 'O' && f.lng(open + MI_USERDATA) == 0x101);
    const ULONG open_text = f.lng(open + MI_ITEMFILL);
    assert(f.str(f.lng(open_text + IT_ITEXT)) == "Open..." && f.lng(open_text + IT_NEXTTEXT) == 0 &&
           f.memory[open_text + IT_DRAWMODE] == JAM1 && f.lng(open + MI_SELECTFILL) == 0);
    /* the separator: an image of its size, without data, and disabled */
    const ULONG bar_image = f.lng(bar + MI_ITEMFILL);
    assert(bar_image && f.word(bar + MI_FLAGS) == HIGHCOMP && f.word(bar_image + IM_WIDTH) == 20 &&
           f.word(bar_image + IM_HEIGHT) == 2 && f.lng(bar_image + IM_IMAGEDATA) == 0);
    /* Recent: the mark of its submenu, a guest copy of GadTools' string */
    const ULONG recent_text = f.lng(recent + MI_ITEMFILL), mark = f.lng(recent_text + IT_NEXTTEXT);
    assert(mark && f.str(f.lng(mark + IT_ITEXT)) == "\xbb" &&
           f.lng(mark + IT_ITEXT) >= f.ctx.heap_start && !(f.word(recent + MI_FLAGS) & COMMSEQ));
    const ULONG one = f.lng(recent + MI_SUBITEM), two = f.lng(one + MI_NEXTITEM);
    assert(f.str(f.lng(f.lng(one + MI_ITEMFILL) + IT_ITEXT)) == "one.txt" &&
           f.lng(two + MI_USERDATA) == 0x104 && f.lng(two + MI_NEXTITEM) == 0 && f.lng(two + MI_SUBITEM) == 0);
    /* Quit: a command string as a second text */
    const ULONG quit_text = f.lng(quit + MI_ITEMFILL), quit_key = f.lng(quit_text + IT_NEXTTEXT);
    assert(f.str(f.lng(quit_key + IT_ITEXT)) == "Ctrl Q" && !(f.word(quit + MI_FLAGS) & COMMSEQ));
    /* Edit's Undo disabled, the Options with their flags and exclusions */
    assert(f.word(f.lng(edit + MU_FIRSTITEM) + MI_FLAGS) == (ITEMTEXT | HIGHCOMP));
    const ULONG small = f.lng(options + MU_FIRSTITEM), large = f.lng(small + MI_NEXTITEM);
    const ULONG sound = f.lng(large + MI_NEXTITEM);
    assert(f.word(small + MI_FLAGS) == (ITEMTEXT | CHECKIT | CHECKED | ITEMENABLED | HIGHCOMP) &&
           f.lng(small + MI_MUTUALEXCLUDE) == 2 && f.lng(large + MI_MUTUALEXCLUDE) == 1 &&
           (f.word(sound + MI_FLAGS) & MENUTOGGLE) && f.lng(sound + MI_USERDATA) == 0x303);

    /* FreeMenus() gives everything back */
    f.call(Fixture::gadtools, 54, { strip });
    assert(strips.empty() && !f.ctx.menus && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    const unsigned calls = native_calls;
    f.call(Fixture::gadtools, 54, { 0 });
    f.call(Fixture::gadtools, 54, { strip });
    assert(native_calls == calls && !f.stopped());
}

static void test_create_tags()
{
    Fixture f;
    const ULONG file = f.s("File"), quit = f.s("Quit");

    /* GTMN_FrontPen reaches the texts; the error ULONG gets 0 */
    f.newmenus({ { NM_TITLE, file, 0, 0, 0, 0 }, { NM_ITEM, quit, 0, 0, 0, 7 } });
    m68k_write32(&f.ctx, Fixture::errout, 0x55555555);
    f.tagitems({ { GTMN_FrontPen, 3 }, { GTMN_SecondaryError, Fixture::errout }, { 0x80001234, 0 },
                 { TAG_MORE, Fixture::tags + 0x60 } });
    m68k_write32(&f.ctx, Fixture::tags + 0x60, GTMN_FrontPen);
    m68k_write32(&f.ctx, Fixture::tags + 0x64, 1);
    m68k_write32(&f.ctx, Fixture::tags + 0x68, TAG_DONE);
    ULONG strip = f.create(Fixture::tags);
    assert(strip && f.lng(Fixture::errout) == 0);
    assert(f.memory[f.lng(f.lng(strip + MU_FIRSTITEM) + MI_ITEMFILL) + IT_FRONTPEN] == 3);
    assert(create_tags[0].ti_Tag == GTMN_FrontPen && create_tags[0].ti_Data == 3 &&
           create_tags[1].ti_Tag == GTMN_FullMenu && create_tags[1].ti_Data == FALSE);
    f.call(Fixture::gadtools, 54, { strip });

    /* items only, without GTMN_FullMenu: the strip is 68k MenuItems, which
       Intuition does not take as a strip */
    f.newmenus({ { NM_ITEM, quit, 0, 0, 0, 9 } });
    strip = f.create();
    assert(strip && f.lng(strip + MI_USERDATA) == 9 && f.str(f.lng(f.lng(strip + MI_ITEMFILL) + IT_ITEXT)) == "Quit");
    f.call(Fixture::intuition, 264, { f.window, strip });
    assert(f.stopped());
    f.call(Fixture::gadtools, 54, { strip });

    /* With GTMN_FullMenu, an item before a title is invalid: AROS writes the
       error into its tag, the 68k ULONG gets it. TAG_MORE and TAG_SKIP. */
    f.tagitems({ { TAG_SKIP, 1 }, { GTMN_FullMenu, FALSE }, { TAG_MORE, Fixture::tags + 0x40 } });
    m68k_write32(&f.ctx, Fixture::tags + 0x40, GTMN_FullMenu);
    m68k_write32(&f.ctx, Fixture::tags + 0x44, TRUE);
    m68k_write32(&f.ctx, Fixture::tags + 0x48, GTMN_SecondaryError);
    m68k_write32(&f.ctx, Fixture::tags + 0x4C, Fixture::errout);
    m68k_write32(&f.ctx, Fixture::tags + 0x50, TAG_DONE);
    assert(f.create(Fixture::tags) == 0 && !f.stopped() && f.lng(Fixture::errout) == GTMENU_INVALID);
    /* out of memory at once: AROS writes through the pointer */
    create_fails = true;
    m68k_write32(&f.ctx, Fixture::errout, 0);
    assert(f.create(Fixture::tags) == 0 && f.lng(Fixture::errout) == GTMENU_NOMEM);
    create_fails = false;
    assert(strips.empty() && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

static void test_create_refused()
{
    Fixture f;
    const ULONG file = f.s("File"), pic = 0x5F00;

    /* a label outside guest memory, a title that is a separator, an image
       item, a key that is no string, an array that leaves guest memory and
       an error ULONG outside it: nothing reaches GadTools */
    const unsigned calls = native_calls;
    f.newmenus({ { NM_TITLE, 0xFFFFFF00, 0, 0, 0, 0 } });
    f.create();
    assert(f.stopped());
    f.newmenus({ { NM_TITLE, 0xFFFFFFFF, 0, 0, 0, 0 } });
    f.create();
    assert(f.stopped());
    f.newmenus({ { NM_TITLE, file, 0, 0, 0, 0 }, { IM_ITEM, pic, 0, 0, 0, 0 } });
    f.create();
    assert(f.stopped());
    f.newmenus({ { NM_TITLE, file, 0xFFFFFFF0, 0, 0, 0 } });
    f.create();
    assert(f.stopped());
    f.call(Fixture::gadtools, 48, { 0xFFF8, 0 });
    assert(f.stopped());
    f.newmenus({ { NM_TITLE, file, 0, 0, 0, 0 } });
    f.tagitems({ { GTMN_SecondaryError, 0xFFFFFFFE } });
    f.create(Fixture::tags);
    assert(f.stopped() && native_calls == calls);

    /* no room for the mirror: no strip, the native one freed, NOMEM */
    f.newmenus({ { NM_TITLE, file, 0, 0, 0, 0 } });
    f.tagitems({ { GTMN_SecondaryError, Fixture::errout } });
    f.exhaust_heap();
    assert(f.create(Fixture::tags) == 0 && !f.stopped() && f.lng(Fixture::errout) == GTMENU_NOMEM);
    assert(strips.empty() && !f.ctx.menus);
    f.release_heap();
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

static void test_layout_and_strip()
{
    Fixture f;
    const ULONG strip = usual(f), vi = f.visualinfo(Fixture::screen);
    const ULONG name = f.s("helvetica.font");

    /* LayoutMenusA() with the program's TextAttr: the places come to the
       mirror, and ITextFont is the program's own TextAttr */
    m68k_write32(&f.ctx, Fixture::textattr + TA_NAME, name);
    m68k_write16(&f.ctx, Fixture::textattr + TA_YSIZE, 13);
    f.memory[Fixture::textattr + 6] = 0;
    f.memory[Fixture::textattr + 7] = 0;
    f.tagitems({ { GTMN_TextAttr, Fixture::textattr }, { GTMN_NewLookMenus, TRUE },
                 { GTMN_Checkmark, 0x5F00 } });
    assert(f.call(Fixture::gadtools, 66, { strip, vi, Fixture::tags }) == TRUE);
    assert(layout_tags.size() == 3 && layout_tags[1].ti_Tag == GTMN_NewLookMenus &&
           layout_tags[2].ti_Tag == TAG_DONE);
    auto *native_attr = reinterpret_cast<TextAttr *>(layout_tags[0].ti_Data);
    assert(native_attr->ta_YSize == 13 && std::string(native_attr->ta_Name) == "helvetica.font");
    const ULONG options = f.lng(f.lng(strip + MU_NEXTMENU) + MU_NEXTMENU);
    const ULONG small = f.lng(options + MU_FIRSTITEM), large = f.lng(small + MI_NEXTITEM);
    assert(f.word(options + MU_LEFTEDGE) == 120 && f.word(options + MU_WIDTH) == 50 &&
           f.word(large + MI_TOPEDGE) == 12 && f.word(large + MI_WIDTH) == 80);
    assert(f.lng(f.lng(small + MI_ITEMFILL) + IT_ITEXTFONT) == Fixture::textattr);

    /* without a TextAttr, ITextFont is NULL for the font of the screen;
       the UserData that the program changed stays its own */
    m68k_write32(&f.ctx, large + MI_USERDATA, 0x777);
    f.tagitems({ { GTMN_NewLookMenus, TRUE }, { GTMN_NewLookMenus, FALSE } });
    assert(f.call(Fixture::gadtools, 66, { strip, vi, Fixture::tags }) == TRUE);
    assert(layout_tags.size() == 2 && layout_tags[0].ti_Data == TRUE);
    assert(f.lng(f.lng(small + MI_ITEMFILL) + IT_ITEXTFONT) == 0);
    assert(f.lng(large + MI_USERDATA) == 0x777);

    /* LayoutMenuItemsA() on the submenu, placed against its menu; more
       tags than the bridge passes on, repeated ones counting once */
    const ULONG project = strip, recent = f.lng(f.lng(f.lng(project + MU_FIRSTITEM) + MI_NEXTITEM) + MI_NEXTITEM);
    const ULONG one = f.lng(recent + MI_SUBITEM);
    f.tagitems({ { GTMN_FrontPen, 1 }, { GTMN_FrontPen, 2 }, { GTMN_FrontPen, 3 },
                 { GTMN_NewLookMenus, TRUE }, { GTMN_NewLookMenus, TRUE }, { GTMN_Menu, project } });
    assert(f.call(Fixture::gadtools, 60, { one, vi, Fixture::tags }) == TRUE);
    assert(layout_tags.size() == 4 && layout_tags[0].ti_Data == 1 && layout_tags[2].ti_Tag == GTMN_Menu &&
           f.word(one + MI_LEFTEDGE) == 5);

    /* a failed layout still shows what GadTools did */
    layout_fails = true;
    assert(f.call(Fixture::gadtools, 66, { strip, vi, 0 }) == FALSE);
    assert(!f.stopped() && f.word(options + MU_WIDTH) == 50);
    layout_fails = false;

    /* SetMenuStrip(): the 68k window shows the strip, the mirror gets what
       Intuition set; the flags the program changed reach Intuition first */
    const UWORD small_flags = f.word(small + MI_FLAGS);
    m68k_write16(&f.ctx, small + MI_FLAGS, small_flags & ~CHECKED);
    m68k_write16(&f.ctx, large + MI_FLAGS, f.word(large + MI_FLAGS) | CHECKED);
    assert(f.call(Fixture::intuition, 264, { f.window, strip }) == TRUE);
    Menu *root = f.native_window.MenuStrip;
    assert(root && f.lng(f.window + M68K_WIN_MENUSTRIP) == strip && f.word(strip + MU_JAZZX) == 7 &&
           f.word(options + MU_BEATY) == 9);
    MenuItem *nsmall = root->NextMenu->NextMenu->FirstItem;
    assert(!(nsmall->Flags & CHECKED) && (nsmall->NextItem->Flags & CHECKED));

    /* ItemAddress(): the 68k items and subitems */
    assert(f.item(strip, 2, 1) == large && f.item(strip, 0, 2, 1) == f.lng(one + MI_NEXTITEM));
    assert(f.call(Fixture::intuition, 144, { strip }, { MENUNULL }) == 0);
    assert(f.item(strip, 5, 0) == 0 && f.call(Fixture::intuition, 144, { 0 }, { 0 }) == 0);
    f.call(Fixture::intuition, 144, { Fixture::raw }, { 0 });
    assert(f.stopped());

    /* OffMenu() and OnMenu() */
    f.call(Fixture::intuition, 180, { f.window }, { (ULONG)FULLMENUNUM(2, 1, NOSUB) });
    assert(!(f.word(large + MI_FLAGS) & ITEMENABLED));
    f.call(Fixture::intuition, 192, { f.window }, { (ULONG)FULLMENUNUM(2, 1, NOSUB) });
    assert(f.word(large + MI_FLAGS) & ITEMENABLED);
    f.call(Fixture::intuition, 180, { f.window }, { (ULONG)FULLMENUNUM(2, 0x3F, NOSUB) });
    assert(!(f.word(options + MU_FLAGS) & MENUENABLED));
    f.call(Fixture::intuition, 192, { f.window }, { (ULONG)FULLMENUNUM(2, 0x3F, NOSUB) });
    assert(f.word(options + MU_FLAGS) & MENUENABLED);

    /* ResetMenuStrip() takes the CHECKED that the program set */
    m68k_write16(&f.ctx, small + MI_FLAGS, f.word(small + MI_FLAGS) | CHECKED);
    assert(f.call(Fixture::intuition, 702, { f.window, strip }) == TRUE && (nsmall->Flags & CHECKED));

    /* ClearMenuStrip(), then the strip and the VisualInfo go */
    f.call(Fixture::intuition, 54, { f.window });
    assert(!f.native_window.MenuStrip && f.lng(f.window + M68K_WIN_MENUSTRIP) == 0);
    f.call(Fixture::gadtools, 54, { strip });
    f.call(Fixture::gadtools, 132, { vi });
    assert(strips.empty() && visualinfos.empty() && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);

    /* refusals: a strip or window of nobody, a layout without a VisualInfo,
       a GTMN_Menu that is no menu of a strip */
    const ULONG again = usual(f);
    f.call(Fixture::intuition, 264, { Fixture::raw, again });
    assert(f.stopped());
    f.call(Fixture::intuition, 264, { f.window, Fixture::raw });
    assert(f.stopped());
    f.call(Fixture::gadtools, 66, { again, Fixture::raw, 0 });
    assert(f.stopped());
    const ULONG vi2 = f.visualinfo(Fixture::screen);
    f.tagitems({ { GTMN_Menu, Fixture::raw } });
    f.call(Fixture::gadtools, 60, { f.lng(again + MU_FIRSTITEM), vi2, Fixture::tags });
    assert(f.stopped());
    f.call(Fixture::intuition, 54, { Fixture::raw });
    assert(f.stopped());
    f.call(Fixture::gadtools, 54, { again });
    f.call(Fixture::gadtools, 132, { vi2 });
}

/* A menu pick: Intuition checks Large, clears Small, which excludes it, and
   chains Large and Sound in NextSelect before the message goes out. */
static void test_menupick()
{
    Fixture f;
    const ULONG strip = usual(f);

    assert(f.call(Fixture::intuition, 264, { f.window, strip }) == TRUE);
    Menu *options = f.native_window.MenuStrip->NextMenu->NextMenu;
    MenuItem *small = options->FirstItem, *large = small->NextItem, *sound = large->NextItem;
    const UWORD code_large = FULLMENUNUM(2, 1, NOSUB), code_sound = FULLMENUNUM(2, 2, NOSUB);

    for (int path = 0; path < 3; ++path)
    {
        small->Flags &= ~CHECKED;
        large->Flags |= CHECKED;
        sound->Flags ^= CHECKED;
        large->NextSelect = code_sound;
        sound->NextSelect = MENUNULL;
        auto *im = new IntuiMessage{};
        made.push_back(im);
        im->Class = IDCMP_MENUPICK;
        im->Code = code_large;
        im->IDCMPWindow = &f.native_window;
        queues[&port0].push_back(im);

        /* GetMsg(), WaitPort() and GT_GetIMsg() all show the menu state */
        ULONG msg;
        if (path == 0)
            msg = f.call(Fixture::exec, 372, { f.port() });
        else if (path == 1)
            msg = f.call(Fixture::exec, 384, { f.port() });
        else
            msg = f.call(Fixture::gadtools, 72, { f.port() });
        assert(msg && f.lng(msg + IMSG_CLASS) == IDCMP_MENUPICK && f.word(msg + IMSG_CODE) == code_large);
        const ULONG g_large = f.item(strip, 2, 1), g_small = f.item(strip, 2, 0);
        const ULONG g_sound = f.item(strip, 2, 2);
        assert((f.word(g_large + MI_FLAGS) & CHECKED) && !(f.word(g_small + MI_FLAGS) & CHECKED));
        assert(f.word(g_large + MI_NEXTSELECT) == code_sound && f.word(g_sound + MI_NEXTSELECT) == MENUNULL);
        assert(f.lng(g_large + MI_USERDATA) == 0x302);
        if (path == 0)
            f.call(Fixture::exec, 378, { 0, msg });
        else if (path == 1)
            f.call(Fixture::exec, 378, { 0, f.call(Fixture::exec, 372, { f.port() }) });
        else
            f.call(Fixture::gadtools, 78, { 0, msg });
    }

    /* Intuition checks Small for a message still on its way, and OffMenu()
       of another item does not undo it. Then it checks Large for another,
       and after ClearMenuStrip() that message still finds the state that
       Intuition set last. */
    const ULONG g_small = f.item(strip, 2, 0), g_large = f.item(strip, 2, 1);
    for (int clear = 0; clear < 2; ++clear)
    {
        MenuItem *picked = clear ? large : small, *other = clear ? small : large;
        picked->Flags |= CHECKED;
        other->Flags &= ~CHECKED;
        picked->NextSelect = MENUNULL;
        auto *im = new IntuiMessage{};
        made.push_back(im);
        im->Class = IDCMP_MENUPICK;
        im->Code = FULLMENUNUM(2, clear, NOSUB);
        im->IDCMPWindow = &f.native_window;
        queues[&port0].push_back(im);
        if (clear)
            f.call(Fixture::intuition, 54, { f.window });
        else
            f.call(Fixture::intuition, 180, { f.window }, { (ULONG)FULLMENUNUM(0, 0, NOSUB) });
        assert(picked->Flags & CHECKED);
        const ULONG msg = f.call(Fixture::exec, 372, { f.port() });
        const ULONG g_picked = clear ? g_large : g_small, g_other = clear ? g_small : g_large;
        assert(msg && (f.word(g_picked + MI_FLAGS) & CHECKED) && !(f.word(g_other + MI_FLAGS) & CHECKED));
        f.call(Fixture::exec, 378, { 0, msg });
    }
    f.call(Fixture::gadtools, 54, { strip });
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

/* The planes that DrawImage() reads have at most 32767 words each, which
   only a guest memory larger than the fixture's can hold with their data;
   an image without planes may be of any size. */
static void test_copy_plane_words()
{
    std::vector<UBYTE> memory(0x30000, 0);
    KaffeineContext ctx = {};
    ctx.mem = memory.data();
    ctx.mem_size = memory.size();
    const ULONG image = 0x100, data = 0x1000;
    auto copy = [&](UWORD height, UBYTE pick) {
        m68k_write16(&ctx, image + IM_WIDTH, 0x7FFF);
        m68k_write16(&ctx, image + IM_HEIGHT, height);
        m68k_write16(&ctx, image + IM_DEPTH, 1);
        m68k_write32(&ctx, image + IM_IMAGEDATA, data);
        memory[image + IM_PLANEPICK] = pick;
        KaffeineCopy c = {};
        Kaffeine_CopyImages(&ctx, &c, image);
        return c.why == nullptr;
    };
    assert(copy(15, 1) && !copy(16, 1) && copy(0x7FFF, 0));
}

/* SetMenuStrip() of a strip that the program built: Intuition gets a copy
   of all of it, the program's structures stay as it wrote them, and
   ItemAddress() finds the program's own items. */
static void test_classic_copy()
{
    Fixture f;
    Built g{ f };
    const Classic c = classic(g);
    const size_t blocks = native_blocks.size();
    const std::vector<UBYTE> before(f.memory.begin() + 0x4000, f.memory.begin() + g.at);
    auto at = [&](ULONG a) { return f.memory.data() + a; };
    auto inside = [&](const void *ptr) {
        auto *b = static_cast<const UBYTE *>(ptr);
        return b >= f.memory.data() && b < f.memory.data() + f.memory.size();
    };

    assert(f.call(Fixture::intuition, 264, { f.window, c.strip }) == TRUE && !f.stopped());
    assert(f.lng(f.window + M68K_WIN_MENUSTRIP) == c.strip && native_blocks.size() == blocks + 1);
    assert(std::equal(before.begin(), before.end(), f.memory.begin() + 0x4000));
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);

    /* the menus, with the page sizes of SetMenuStrip() in the copy only and
       without the flags that Intuition keeps for itself */
    Menu *project = f.native_window.MenuStrip, *edit = project->NextMenu;
    assert(project->JazzX == 7 && f.word(c.project + MU_JAZZX) == 0 && !edit->NextMenu);
    assert(std::string(project->MenuName) == "Project" && !inside(project->MenuName) &&
           project->LeftEdge == 2 && project->Width == 60 && project->Height == 9);
    assert(project->Flags == MENUENABLED && edit->Flags == MENUENABLED);
    MenuItem *open = project->FirstItem, *check = open->NextItem, *more = check->NextItem;
    MenuItem *picture = more->NextItem;
    assert(!picture->NextItem);
    assert(open->Flags == (ITEMTEXT | COMMSEQ | ITEMENABLED | HIGHCOMP) && open->Command == 'O' &&
           open->NextSelect == 0 && open->Width == 80 && open->Height == 10);
    assert(check->Flags == (ITEMTEXT | CHECKIT | MENUTOGGLE | ITEMENABLED | HIGHCOMP) &&
           check->TopEdge == 10 && check->NextSelect == MENUNULL);
    assert(more->Flags == (ITEMTEXT | ITEMENABLED | HIGHBOX) && more->TopEdge == 20);

    /* the texts: their places, copies of the program's strings, and one
       copy of the font that they use one after another, without the tags
       of a TTextAttr */
    auto *text = static_cast<IntuiText *>(open->ItemFill), *key = text->NextText;
    assert(text->LeftEdge == -3 && text->TopEdge == 1 && text->FrontPen == 1 && text->BackPen == 2 &&
           text->DrawMode == JAM1 && std::string((char *)text->IText) == "Open" && !inside(text->IText));
    assert(key && !key->NextText && key->LeftEdge == 50 && std::string((char *)key->IText) == "Ctrl");
    TextAttr *font = text->ITextFont;
    assert(font && key->ITextFont == font && static_cast<IntuiText *>(more->ItemFill)->ITextFont == font);
    assert(std::string(font->ta_Name) == "topaz.font" && !inside(font->ta_Name) && font->ta_YSize == 8 &&
           font->ta_Style == FSF_UNDERLINED && font->ta_Flags == 0x40);
    assert(!static_cast<IntuiText *>(check->ItemFill)->ITextFont && !open->SelectFill);

    /* one level of subitems: the SubItem of a subitem stays behind */
    MenuItem *sub1 = more->SubItem, *sub2 = sub1->NextItem;
    assert(sub1->LeftEdge == 60 && sub1->TopEdge == -2 && sub2->TopEdge == 10 && !sub2->NextItem);
    assert(!sub2->SubItem && !open->SubItem);

    /* the images: the data where it lies, none behind a PlanePick of 0 */
    auto *image = static_cast<Image *>(picture->ItemFill), *image2 = image->NextImage;
    auto *select = static_cast<Image *>(picture->SelectFill);
    assert(picture->Flags == ITEMENABLED && image->LeftEdge == 1 && image->TopEdge == -1);
    assert(image->Width == 20 && image->Height == 2 && image->Depth == 2 && image->PlanePick == 3 &&
           (UBYTE *)image->ImageData == at(c.data));
    assert(image2 && !image2->NextImage && !image2->ImageData && image2->PlaneOnOff == 2);
    assert(select && !select->NextImage && select->Depth == 1 && (UBYTE *)select->ImageData == at(c.data));

    /* every copy on the boundary of its kind */
    auto aligned = [](const void *ptr, size_t a) { return reinterpret_cast<uintptr_t>(ptr) % a == 0; };
    assert(aligned(project, alignof(Menu)) && aligned(edit, alignof(Menu)) && aligned(sub2, alignof(MenuItem)));
    assert(aligned(open, alignof(MenuItem)) && aligned(picture, alignof(MenuItem)) && aligned(key, alignof(IntuiText)));
    assert(aligned(font, alignof(TextAttr)) && aligned(image2, alignof(Image)) && aligned(select, alignof(Image)));

    /* A and B exclude each other */
    MenuItem *a = edit->FirstItem, *b = a->NextItem;
    assert(a->MutualExclude == 2 && b->MutualExclude == 1 && (a->Flags & CHECKED) && !(b->Flags & CHECKED));

    /* ItemAddress(): the program's items and subitems, as AROS finds them;
       an item without subitems for any subitem number */
    assert(f.item(c.strip, 0, 0) == c.open && f.item(c.strip, 0, 2) == c.more &&
           f.item(c.strip, 1, 1) == c.b && f.item(c.strip, 0, 2, 1) == c.sub2);
    assert(f.item(c.strip, 0, 2, 2) == 0 && f.item(c.strip, 0, 1, 3) == c.check);
    assert(f.item(c.strip, 2, 0) == 0 && f.item(c.strip, 0, 4) == 0);
    assert(f.call(Fixture::intuition, 144, { c.strip }, { MENUNULL }) == 0 &&
           f.call(Fixture::intuition, 144, { 0 }, { 0 }) == 0 && !f.stopped());

    /* ClearMenuStrip(): the copy waits for a ResetMenuStrip() of the
       window, and ItemAddress() also reads a strip that no window shows;
       the end of the program frees it */
    f.call(Fixture::intuition, 54, { f.window });
    assert(!f.native_window.MenuStrip && f.lng(f.window + M68K_WIN_MENUSTRIP) == 0 && f.ctx.menus);
    assert(native_blocks.size() == blocks + 1 && f.item(c.strip, 1, 0) == c.a);
    Kaffeine_MenusCleanup(&f.ctx);
    assert(!f.ctx.menus && native_blocks.size() == blocks);
}

/* Strings are copies of at most 255 characters, so that what the program
   writes in its own while a window shows the strip, even over their end,
   does not reach Intuition; a TextAttr that texts use one after another
   is counted once, as it is copied once. */
static void test_classic_strings()
{
    Fixture f;
    Built g{ f };
    const Classic c = classic(g);
    const size_t blocks = native_blocks.size();

    assert(f.call(Fixture::intuition, 264, { f.window, c.strip }) == TRUE);
    Menu *project = f.native_window.MenuStrip;
    auto *text = static_cast<IntuiText *>(project->FirstItem->ItemFill);
    std::memset(f.memory.data() + f.lng(c.project + MU_MENUNAME), 'x', 8);
    std::memset(f.memory.data() + f.lng(c.open_text + IT_ITEXT), 'y', 5);
    std::memset(f.memory.data() + f.lng(c.font + TA_NAME), 'z', 11);
    assert(std::string(project->MenuName) == "Project" && std::string((char *)text->IText) == "Open" &&
           std::string(text->ITextFont->ta_Name) == "topaz.font");
    Kaffeine_MenusCleanup(&f.ctx);

    /* a menu name and a text of 255 characters go, of 256 not */
    const ULONG name = Kaffeine_HeapAlloc(&f.ctx, 257, MEMF_CLEAR);
    assert(name);
    std::memset(f.memory.data() + name, 'n', 256);
    const ULONG item = g.item(ITEMTEXT | ITEMENABLED, g.text("T")), menu = g.menu("M", 0, item);
    const ULONG plain = f.lng(menu + MU_MENUNAME), label = f.lng(f.lng(item + MI_ITEMFILL) + IT_ITEXT);
    for (int field = 0; field < 2; ++field)
    {
        const ULONG at = field ? f.lng(item + MI_ITEMFILL) + IT_ITEXT : menu + MU_MENUNAME;
        m68k_write32(&f.ctx, at, name);
        f.memory[name + 255] = 0;
        assert(f.call(Fixture::intuition, 264, { f.window, menu }) == TRUE && !f.stopped());
        Menu *m = f.native_window.MenuStrip;
        const char *copy = field ? (char *)static_cast<IntuiText *>(m->FirstItem->ItemFill)->IText : m->MenuName;
        assert(std::strlen(copy) == 255);
        Kaffeine_MenusCleanup(&f.ctx);
        f.memory[name + 255] = 'n';
        f.call(Fixture::intuition, 264, { f.window, menu });
        assert(f.stopped() && !f.native_window.MenuStrip && native_blocks.size() == blocks);
        m68k_write32(&f.ctx, at, field ? label : plain);
    }
    Kaffeine_HeapFree(&f.ctx, name, 257);

    /* 1000 texts in one font of a name of 200 characters: one copy of it,
       and the block counts no more */
    const ULONG font = g.font(std::string(200, 'f').c_str(), 8, 0);
    const ULONG chain = Kaffeine_HeapAlloc(&f.ctx, 1000 * INTUITEXT_SIZE, MEMF_CLEAR);
    assert(chain);
    for (ULONG i = 0; i < 1000; ++i)
    {
        m68k_write32(&f.ctx, chain + i * INTUITEXT_SIZE + IT_ITEXTFONT, font);
        m68k_write32(&f.ctx, chain + i * INTUITEXT_SIZE + IT_NEXTTEXT,
                     i < 999 ? chain + (i + 1) * INTUITEXT_SIZE : 0);
    }
    const ULONG fonts = g.menu("F", 0, g.item(ITEMTEXT | ITEMENABLED, chain));
    assert(f.call(Fixture::intuition, 264, { f.window, fonts }) == TRUE && !f.stopped());
    assert(last_allocation < 1000 * sizeof(IntuiText) + 4096);
    Kaffeine_MenusCleanup(&f.ctx);
    Kaffeine_HeapFree(&f.ctx, chain, 1000 * INTUITEXT_SIZE);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail && native_blocks.size() == blocks);
}

/* A menu pick, OffMenu() and OnMenu() in Intuition's copy reach the
   program's strip by bit: a change of the program that has not reached
   Intuition yet stays, and the items of the pick get their NextSelect in
   any case. ResetMenuStrip() after ClearMenuStrip() takes the copy that
   waited, without memory; SetMenuStrip() makes a new one. */
static void test_classic_pick()
{
    Fixture f;
    Built g{ f };
    const Classic c = classic(g);

    assert(f.call(Fixture::intuition, 264, { f.window, c.strip }) == TRUE);
    Menu *project = f.native_window.MenuStrip, *edit = project->NextMenu;
    MenuItem *open = project->FirstItem, *check = open->NextItem, *a = edit->FirstItem, *b = a->NextItem;
    const UWORD code_b = FULLMENUNUM(1, 1, NOSUB), code_check = FULLMENUNUM(0, 1, NOSUB);
    const size_t blocks = native_blocks.size();

    /* the program disables Open and Check itself and writes into the
       NextSelect of Check; Intuition checks B, clears A, toggles Check and
       chains B and Check, whose NextSelect it leaves as it was */
    m68k_write16(&f.ctx, c.open + MI_FLAGS, f.word(c.open + MI_FLAGS) & ~ITEMENABLED);
    m68k_write16(&f.ctx, c.check + MI_FLAGS, f.word(c.check + MI_FLAGS) & ~ITEMENABLED);
    m68k_write16(&f.ctx, c.check + MI_NEXTSELECT, 0x1234);
    b->Flags |= CHECKED;
    a->Flags &= ~CHECKED;
    check->Flags |= CHECKED | HIGHITEM;
    b->NextSelect = code_check;
    auto *im = new IntuiMessage{};
    made.push_back(im);
    im->Class = IDCMP_MENUPICK;
    im->Code = code_b;
    im->IDCMPWindow = &f.native_window;
    queues[&port0].push_back(im);
    const ULONG msg = f.call(Fixture::exec, 372, { f.port() });
    assert(msg && f.word(msg + IMSG_CODE) == code_b);
    assert((f.word(c.b + MI_FLAGS) & CHECKED) && !(f.word(c.a + MI_FLAGS) & CHECKED) &&
           (f.word(c.check + MI_FLAGS) & CHECKED));
    assert(f.word(c.b + MI_NEXTSELECT) == code_check && f.word(c.check + MI_NEXTSELECT) == MENUNULL);
    /* the program's own bits stay: Open and Check disabled, the flags of
       Intuition that it wrote itself, and no HIGHITEM of Intuition's copy */
    assert(!(f.word(c.open + MI_FLAGS) & ITEMENABLED) && (open->Flags & ITEMENABLED));
    assert(!(f.word(c.check + MI_FLAGS) & ITEMENABLED) && (check->Flags & ITEMENABLED));
    assert((f.word(c.open + MI_FLAGS) & MENUTOGGLED) && (f.word(c.check + MI_FLAGS) & HIGHITEM));
    m68k_write16(&f.ctx, c.check + MI_FLAGS, f.word(c.check + MI_FLAGS) & ~HIGHITEM);
    f.call(Fixture::exec, 378, { 0, msg });
    assert(!(f.word(c.check + MI_FLAGS) & HIGHITEM));

    /* OffMenu() and OnMenu() of an item and of a menu, after which the
       program also sees a NextSelect that Intuition changed outside a pick */
    open->NextSelect = code_check;
    f.call(Fixture::intuition, 180, { f.window }, { (ULONG)FULLMENUNUM(1, 0, NOSUB) });
    assert(!(a->Flags & ITEMENABLED) && !(f.word(c.a + MI_FLAGS) & ITEMENABLED));
    assert(f.word(c.open + MI_NEXTSELECT) == code_check);
    f.call(Fixture::intuition, 180, { f.window }, { (ULONG)FULLMENUNUM(1, NOITEM, NOSUB) });
    assert(!(f.word(c.edit + MU_FLAGS) & MENUENABLED) && (f.word(c.edit + MU_FLAGS) & MIDRAWN));
    f.call(Fixture::intuition, 192, { f.window }, { (ULONG)FULLMENUNUM(1, NOITEM, NOSUB) });
    f.call(Fixture::intuition, 192, { f.window }, { (ULONG)FULLMENUNUM(1, 0, NOSUB) });
    assert((f.word(c.edit + MU_FLAGS) & MENUENABLED) && (f.word(c.a + MI_FLAGS) & ITEMENABLED));

    /* ItemAddress() shows what Intuition set in the item that it returns,
       and only there */
    check->Flags &= ~CHECKED;
    b->Flags &= ~CHECKED;
    assert(f.item(c.strip, 0, 1) == c.check && !(f.word(c.check + MI_FLAGS) & CHECKED));
    assert(f.word(c.b + MI_FLAGS) & CHECKED);
    b->Flags |= CHECKED;

    /* ClearMenuStrip(), the program changes CHECKED and NextSelect, then
       ResetMenuStrip() takes the copy that waited, with the program's
       flags, without memory; NextSelect stays Intuition's */
    f.call(Fixture::intuition, 54, { f.window });
    assert(f.ctx.menus && native_blocks.size() == blocks);
    m68k_write16(&f.ctx, c.a + MI_FLAGS, f.word(c.a + MI_FLAGS) | CHECKED);
    m68k_write16(&f.ctx, c.b + MI_FLAGS, f.word(c.b + MI_FLAGS) & ~CHECKED);
    m68k_write16(&f.ctx, c.b + MI_NEXTSELECT, FULLMENUNUM(0, 0, NOSUB));
    allocations_before_failure = 0;
    assert(f.call(Fixture::intuition, 702, { f.window, c.strip }) == TRUE);
    assert(allocations_before_failure == 0 && f.native_window.MenuStrip == project);
    allocations_before_failure = -1;
    assert(project->JazzX == 7 && (a->Flags & CHECKED) && !(b->Flags & CHECKED) && b->NextSelect == code_check);
    assert(!(open->Flags & ITEMENABLED) && f.lng(f.window + M68K_WIN_MENUSTRIP) == c.strip);
    assert(native_blocks.size() == blocks);

    /* SetMenuStrip() of a strip that waits makes a new copy, which may
       not be had without memory; the copy that waits stays for Reset */
    f.call(Fixture::intuition, 54, { f.window });
    assert(f.call(Fixture::intuition, 264, { f.window, c.strip }) == TRUE);
    Menu *anew = f.native_window.MenuStrip;
    assert(anew && native_blocks.size() == blocks && !native_blocks.count(project));
    f.call(Fixture::intuition, 54, { f.window });
    allocations_before_failure = 0;
    assert(f.call(Fixture::intuition, 264, { f.window, c.strip }) == FALSE && !f.stopped());
    assert(allocations_before_failure == -1 && !f.native_window.MenuStrip && native_blocks.size() == blocks);
    assert(f.call(Fixture::intuition, 702, { f.window, c.strip }) == TRUE && f.native_window.MenuStrip == anew);
    Kaffeine_MenusCleanup(&f.ctx);
    assert(!f.ctx.menus && native_blocks.size() == blocks - 1);
}

/* Windows that show the same strip share its copy: the program's changes
   go into it while they let it go. A copy that no window shows waits for
   ResetMenuStrip(), the eight most recent ones, also after CloseWindow(),
   until the end of the program. */
static void test_classic_windows()
{
    Fixture f;
    Built g{ f };
    const Classic c = classic(g), d = classic(g);
    Window native2 = {};
    const ULONG window2 = Kaffeine_WindowCreate(&f.ctx, &native2);
    const size_t blocks = native_blocks.size();

    assert(window2);
    assert(f.call(Fixture::intuition, 264, { f.window, c.strip }) == TRUE);
    assert(f.call(Fixture::intuition, 264, { window2, c.strip }) == TRUE);
    Menu *copy = f.native_window.MenuStrip;
    assert(copy && native2.MenuStrip == copy && native_blocks.size() == blocks + 1);
    MenuItem *a = copy->NextMenu->FirstItem, *b = a->NextItem;

    /* One window lets the strip go, the program clears A, and Intuition
       checks B in the other window, which the program has not seen yet:
       ResetMenuStrip() brings A to the shared copy and keeps B, while the
       other window lets the copy go. */
    f.call(Fixture::intuition, 54, { f.window });
    assert(native2.MenuStrip == copy && f.ctx.menus && native_blocks.size() == blocks + 1);
    m68k_write16(&f.ctx, c.a + MI_FLAGS, f.word(c.a + MI_FLAGS) & ~CHECKED);
    b->Flags |= CHECKED;
    std::vector<std::tuple<char, Window *, bool>> calls;
    menu_call = [&](char op, Window *w) { calls.push_back({ op, w, (a->Flags & CHECKED) != 0 }); };
    assert(f.call(Fixture::intuition, 702, { f.window, c.strip }) == TRUE && f.native_window.MenuStrip == copy);
    menu_call = nullptr;
    assert(calls.size() == 3 && calls[0] == std::make_tuple('C', &native2, true) &&
           calls[1] == std::make_tuple('R', &native2, false) &&
           calls[2] == std::make_tuple('R', &f.native_window, false));
    assert(!(a->Flags & CHECKED) && (b->Flags & CHECKED) && copy->JazzX == 7 && native2.MenuStrip == copy);
    /* the program checks A again, which a later export must not undo */
    m68k_write16(&f.ctx, c.a + MI_FLAGS, f.word(c.a + MI_FLAGS) | CHECKED);
    f.call(Fixture::intuition, 54, { window2 });
    assert((f.word(c.b + MI_FLAGS) & CHECKED) && f.native_window.MenuStrip == copy);
    assert(f.word(c.a + MI_FLAGS) & CHECKED);

    /* a copy that another window shows does not wait, and CloseWindow() of
       a window that let it go leaves it */
    Window native3 = {};
    const ULONG window3 = Kaffeine_WindowCreate(&f.ctx, &native3);
    assert(window3 && f.call(Fixture::intuition, 264, { window3, c.strip }) == TRUE && native3.MenuStrip == copy);
    f.call(Fixture::intuition, 54, { window3 });
    f.call(Fixture::intuition, 72, { window3 });
    assert(f.native_window.MenuStrip == copy && f.ctx.menus && native_blocks.size() == blocks + 1);

    /* CloseWindow() of the last window that shows the strip: the copy
       waits, and ResetMenuStrip() on another window takes it */
    f.call(Fixture::intuition, 72, { f.window });
    assert(!f.native_window.MenuStrip && f.ctx.menus && native_blocks.size() == blocks + 1);
    allocations_before_failure = 0;
    assert(f.call(Fixture::intuition, 702, { window2, c.strip }) == TRUE && native2.MenuStrip == copy);
    assert(allocations_before_failure == 0);
    allocations_before_failure = -1;

    /* another strip on a window without ClearMenuStrip(), then none: the
       old copies wait, with what Intuition set in them for the program,
       and the Set A, Clear, Set B, Clear, Reset A of the NDK needs no
       memory */
    b->Flags &= ~CHECKED;
    assert(f.word(c.b + MI_FLAGS) & CHECKED);
    assert(f.call(Fixture::intuition, 264, { window2, d.strip }) == TRUE);
    assert(!(f.word(c.b + MI_FLAGS) & CHECKED));
    Menu *dcopy = native2.MenuStrip;
    assert(dcopy && f.lng(window2 + M68K_WIN_MENUSTRIP) == d.strip && native_blocks.size() == blocks + 2);
    assert(f.call(Fixture::intuition, 264, { window2, 0 }) == TRUE);
    assert(!native2.MenuStrip && f.lng(window2 + M68K_WIN_MENUSTRIP) == 0 && native_blocks.size() == blocks + 2);
    allocations_before_failure = 0;
    assert(f.call(Fixture::intuition, 702, { window2, c.strip }) == TRUE && native2.MenuStrip == copy);
    f.call(Fixture::intuition, 54, { window2 });
    assert(f.call(Fixture::intuition, 702, { window2, d.strip }) == TRUE && native2.MenuStrip == dcopy);
    assert(allocations_before_failure == 0);
    allocations_before_failure = -1;
    f.call(Fixture::intuition, 54, { window2 });

    /* A copy that a window still shows does not wait. The eight most
       recent copies wait: a ninth lets the oldest go, whose ResetMenuStrip()
       needs a new copy, and one that came back waits as the most recent. */
    Window native4 = {};
    const ULONG window4 = Kaffeine_WindowCreate(&f.ctx, &native4);
    assert(window4 && f.call(Fixture::intuition, 264, { window4, c.strip }) == TRUE);
    Menu *shared = native4.MenuStrip;
    assert(f.call(Fixture::intuition, 264, { window2, c.strip }) == TRUE && native2.MenuStrip == shared);
    f.call(Fixture::intuition, 54, { window2 });
    assert(native4.MenuStrip == shared && native_blocks.size() == blocks + 2);
    ULONG small[9];
    for (int i = 0; i < 9; ++i)
        small[i] = g.menu("S", MENUENABLED, g.item(ITEMENABLED, 0));
    for (int i = 0; i < 8; ++i)
    {
        assert(f.call(Fixture::intuition, 264, { window2, small[i] }) == TRUE);
        f.call(Fixture::intuition, 54, { window2 });
    }
    assert(native_blocks.size() == blocks + 9 && native4.MenuStrip == shared);
    allocations_before_failure = 0;
    assert(f.call(Fixture::intuition, 702, { window2, d.strip }) == FALSE && !native2.MenuStrip);
    assert(allocations_before_failure == -1);
    assert(f.call(Fixture::intuition, 702, { window2, small[0] }) == TRUE && native2.MenuStrip);
    f.call(Fixture::intuition, 54, { window2 });
    assert(f.call(Fixture::intuition, 264, { window2, small[8] }) == TRUE);
    f.call(Fixture::intuition, 54, { window2 });
    assert(native_blocks.size() == blocks + 9);
    allocations_before_failure = 0;
    assert(f.call(Fixture::intuition, 702, { window2, small[0] }) == TRUE && native2.MenuStrip);
    f.call(Fixture::intuition, 54, { window2 });
    assert(f.call(Fixture::intuition, 702, { window2, small[1] }) == FALSE && allocations_before_failure == -1);
    /* a copy that came back to a window waits no more, however many others
       come to wait */
    f.call(Fixture::intuition, 54, { window4 });
    assert(f.call(Fixture::intuition, 702, { window4, c.strip }) == TRUE && native4.MenuStrip == shared);
    for (int i = 0; i < 9; ++i)
    {
        assert(f.call(Fixture::intuition, 264, { window2, small[i] }) == TRUE);
        f.call(Fixture::intuition, 54, { window2 });
    }
    assert(native4.MenuStrip == shared && native_blocks.size() == blocks + 9);
    f.call(Fixture::intuition, 72, { window4 });
    assert(native_blocks.size() == blocks + 8);

    /* ResetMenuStrip() of a strip without a copy makes one through
       SetMenuStrip(), which works out its page sizes */
    Kaffeine_MenusCleanup(&f.ctx);
    assert(!f.ctx.menus && native_blocks.size() == blocks);
    assert(f.call(Fixture::intuition, 702, { window2, c.strip }) == TRUE && native2.MenuStrip);
    assert(native2.MenuStrip->JazzX == 7 && native_blocks.size() == blocks + 1);
    f.call(Fixture::intuition, 72, { window2 });
    assert(f.ctx.menus && native_blocks.size() == blocks + 1);
    Kaffeine_MenusCleanup(&f.ctx);
    assert(!f.ctx.menus && native_blocks.size() == blocks);
}

/* Every strip that the bridge refuses stops the program and leaves the
   strip that the window showed; no memory for a copy fails the call. */
static void test_classic_refused()
{
    Fixture f;
    Built g{ f };
    const Classic c = classic(g), d = classic(g);
    const ULONG beyond = 0xFFF0;    /* 0xA5 up to the end of guest memory */

    assert(f.call(Fixture::intuition, 264, { f.window, c.strip }) == TRUE);
    Menu *shown = f.native_window.MenuStrip;
    const size_t blocks = native_blocks.size();
    auto refused = [&](ULONG addr, ULONG value, bool word = false) {
        const ULONG old = word ? f.word(addr) : f.lng(addr);
        if (word)
            m68k_write16(&f.ctx, addr, (UWORD)value);
        else
            m68k_write32(&f.ctx, addr, value);
        f.call(Fixture::intuition, 264, { f.window, d.strip });
        assert(f.stopped() && f.native_window.MenuStrip == shown && f.lng(f.window + M68K_WIN_MENUSTRIP) == c.strip);
        assert(native_blocks.size() == blocks && f.ctx.menus);
        if (word)
            m68k_write16(&f.ctx, addr, (UWORD)old);
        else
            m68k_write32(&f.ctx, addr, old);
    };

    /* menus and items outside guest memory or in a cycle, more than a menu
       number names, or in two places of the strip */
    refused(d.project + MU_NEXTMENU, 0xFFFFFF00);
    refused(d.edit + MU_NEXTMENU, d.project);
    refused(d.b + MI_NEXTITEM, 0x10000 - 20);
    refused(d.b + MI_NEXTITEM, d.a);
    refused(d.sub2 + MI_NEXTITEM, d.sub1);
    refused(d.more + MI_SUBITEM, 0xFFFFFFF8);
    refused(d.edit + MU_FIRSTITEM, d.open);
    refused(d.check + MI_SUBITEM, d.sub1);
    /* a menu without a name, or one that does not end in guest memory */
    refused(d.edit + MU_MENUNAME, 0);
    refused(d.edit + MU_MENUNAME, beyond);
    /* texts: a text item without one, outside guest memory, in a cycle, a
       TextAttr outside guest memory, strings that do not end */
    refused(d.check + MI_ITEMFILL, 0);
    refused(d.open + MI_ITEMFILL, 0xFFFFFFF0);
    refused(d.open_key + IT_NEXTTEXT, d.open_text);
    refused(d.open_text + IT_ITEXTFONT, 0xFFFC);
    refused(d.open_key + IT_ITEXT, beyond);
    refused(d.font + TA_NAME, beyond);
    refused(d.open + MI_SELECTFILL, 0xFFFFFF00);
    /* images: BOOPSI, negative, too deep, too large, data outside guest
       memory or at an odd address, outside guest memory, in a cycle */
    refused(d.image + IM_DEPTH, 0xFFFF, true);
    refused(d.image + IM_WIDTH, 0xFFFF, true);
    refused(d.image + IM_HEIGHT, 0xFFFE, true);
    refused(d.image + IM_DEPTH, 9, true);
    m68k_write16(&f.ctx, d.image + IM_HEIGHT, 16);
    refused(d.image + IM_WIDTH, 0x7FFF, true);
    m68k_write16(&f.ctx, d.image + IM_HEIGHT, 2);
    refused(d.image + IM_IMAGEDATA, 0x10000 - 8);
    refused(d.select + IM_IMAGEDATA, d.data + 1);
    refused(d.picture + MI_ITEMFILL, 0xFFFFFF00);
    refused(d.image2 + IM_NEXTIMAGE, d.image);

    /* a data pointer behind no plane is never looked at, nor the size of
       an image without planes */
    m68k_write32(&f.ctx, d.image2 + IM_IMAGEDATA, 0xFFFFFFF1);
    m68k_write16(&f.ctx, d.image2 + IM_WIDTH, 0x7FFF);
    m68k_write16(&f.ctx, d.image2 + IM_HEIGHT, 0x7FFF);
    assert(f.call(Fixture::intuition, 264, { f.window, d.strip }) == TRUE && !f.stopped());
    Kaffeine_MenusCleanup(&f.ctx);
    assert(f.call(Fixture::intuition, 264, { f.window, c.strip }) == TRUE);
    shown = f.native_window.MenuStrip;

    /* chains of texts: 1024 long at most, and more copies than any strip
       a program means, 40 items that share a chain of 1000 texts */
    const ULONG chain = Kaffeine_HeapAlloc(&f.ctx, 1025 * INTUITEXT_SIZE, MEMF_CLEAR);
    assert(chain);
    for (ULONG i = 0; i < 1025; ++i)
        m68k_write32(&f.ctx, chain + i * INTUITEXT_SIZE + IT_NEXTTEXT,
                     i < 1024 ? chain + (i + 1) * INTUITEXT_SIZE : 0);
    const ULONG long_item = g.item(ITEMTEXT | ITEMENABLED, chain), long_menu = g.menu("Long", 0, long_item);
    f.call(Fixture::intuition, 264, { f.window, long_menu });
    assert(f.stopped() && f.native_window.MenuStrip == shown && native_blocks.size() == blocks);
    m68k_write32(&f.ctx, chain + 1023 * INTUITEXT_SIZE + IT_NEXTTEXT, 0);
    assert(f.call(Fixture::intuition, 264, { f.window, long_menu }) == TRUE);
    Kaffeine_MenusCleanup(&f.ctx);
    m68k_write32(&f.ctx, chain + 999 * INTUITEXT_SIZE + IT_NEXTTEXT, 0);
    ULONG items = 0;
    for (int i = 0; i < 40; ++i)
        items = g.item(ITEMTEXT | ITEMENABLED, chain, items);
    const ULONG big = g.menu("Big", MENUENABLED, items);
    assert(f.call(Fixture::intuition, 264, { f.window, c.strip }) == TRUE);
    shown = f.native_window.MenuStrip;
    f.call(Fixture::intuition, 264, { f.window, big });
    assert(f.stopped() && f.native_window.MenuStrip == shown && native_blocks.size() == blocks);
    Kaffeine_HeapFree(&f.ctx, chain, 1025 * INTUITEXT_SIZE);

    /* no memory for a copy of a strip that no copy waits for: FALSE, and
       the window keeps its strip */
    allocations_before_failure = 0;
    assert(f.call(Fixture::intuition, 264, { f.window, d.strip }) == FALSE && !f.stopped());
    assert(f.native_window.MenuStrip == shown && native_blocks.size() == blocks);

    /* ItemAddress() of a strip that leaves guest memory on the way */
    m68k_write32(&f.ctx, d.project + MU_NEXTMENU, 0xFFFFFF00);
    f.call(Fixture::intuition, 144, { d.strip }, { (ULONG)FULLMENUNUM(1, 0, NOSUB) });
    assert(f.stopped());
    m68k_write32(&f.ctx, d.project + MU_NEXTMENU, d.edit);
    m68k_write32(&f.ctx, d.a + MI_NEXTITEM, 0xFFFFFF00);
    f.call(Fixture::intuition, 144, { d.strip }, { (ULONG)FULLMENUNUM(1, 1, NOSUB) });
    assert(f.stopped());
    f.call(Fixture::intuition, 144, { d.strip }, { (ULONG)FULLMENUNUM(1, 0, 0) });
    assert(!f.stopped());
    m68k_write32(&f.ctx, d.more + MI_SUBITEM, 0xFFFFFF00);
    f.call(Fixture::intuition, 144, { d.strip }, { (ULONG)FULLMENUNUM(0, 2, 0) });
    assert(f.stopped());
    Kaffeine_MenusCleanup(&f.ctx);
    assert(!f.ctx.menus && native_blocks.size() == blocks - 1);
}

/* As many menus, items and subitems as a menu number names, and one more,
   which the bridge refuses */
static void test_classic_limits()
{
    Fixture f;
    Built g{ f };
    const ULONG name = g.str("M");
    const size_t blocks = native_blocks.size();
    std::vector<std::pair<ULONG, ULONG>> taken;
    auto chain = [&](ULONG count, ULONG size, ULONG link, UWORD flags_at, UWORD flags) {
        const ULONG block = Kaffeine_HeapAlloc(&f.ctx, count * size, MEMF_CLEAR);
        assert(block);
        taken.push_back({ block, count * size });
        for (ULONG i = 0; i < count; ++i)
        {
            m68k_write16(&f.ctx, block + i * size + flags_at, flags);
            m68k_write32(&f.ctx, block + i * size + link, i + 1 < count ? block + (i + 1) * size : 0);
        }
        return block;
    };
    const ULONG menus = chain(32, MENU_SIZE, MU_NEXTMENU, MU_FLAGS, MENUENABLED);
    const ULONG items = chain(64, MENUITEM_SIZE, MI_NEXTITEM, MI_FLAGS, ITEMENABLED);
    const ULONG subs = chain(32, MENUITEM_SIZE, MI_NEXTITEM, MI_FLAGS, ITEMENABLED);
    for (ULONG i = 0; i < 32; ++i)
        m68k_write32(&f.ctx, menus + i * MENU_SIZE + MU_MENUNAME, name);
    m68k_write32(&f.ctx, menus + MU_FIRSTITEM, items);
    m68k_write32(&f.ctx, items + MI_SUBITEM, subs);
    auto cut = [&](ULONG last, ULONG link) {
        const ULONG next = f.lng(last + link);
        m68k_write32(&f.ctx, last + link, 0);
        return next;
    };
    const ULONG menu31 = cut(menus + 30 * MENU_SIZE, MU_NEXTMENU);
    const ULONG item63 = cut(items + 62 * MENUITEM_SIZE, MI_NEXTITEM);
    const ULONG sub31 = cut(subs + 30 * MENUITEM_SIZE, MI_NEXTITEM);

    assert(f.call(Fixture::intuition, 264, { f.window, menus }) == TRUE && !f.stopped());
    assert(f.item(menus, 0, 62) == items + 62 * MENUITEM_SIZE && f.item(menus, 0, 0, 30) == subs + 30 * MENUITEM_SIZE);
    assert(f.item(menus, 30, 0) == 0 && native_blocks.size() == blocks + 1);
    f.call(Fixture::intuition, 54, { f.window });
    for (int which = 0; which < 3; ++which)
    {
        const ULONG last = which == 0 ? menus + 30 * MENU_SIZE : which == 1 ? items + 62 * MENUITEM_SIZE
                                                                         : subs + 30 * MENUITEM_SIZE;
        const ULONG link = which == 0 ? MU_NEXTMENU : MI_NEXTITEM;
        m68k_write32(&f.ctx, last + link, which == 0 ? menu31 : which == 1 ? item63 : sub31);
        f.call(Fixture::intuition, 264, { f.window, menus });
        assert(f.stopped() && !f.native_window.MenuStrip && native_blocks.size() == blocks + 1);
        m68k_write32(&f.ctx, last + link, 0);
    }
    Kaffeine_MenusCleanup(&f.ctx);
    assert(native_blocks.size() == blocks);
    for (auto &b : taken)
        Kaffeine_HeapFree(&f.ctx, b.first, b.second);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

/* The end of the program takes every strip off the windows that it left
   open and frees them; FreeMenus() and LayoutMenusA() take only strips of
   CreateMenusA(). */
static void test_classic_cleanup()
{
    Fixture f;
    Built g{ f };
    const Classic c = classic(g);
    Window native2 = {};
    const ULONG window2 = Kaffeine_WindowCreate(&f.ctx, &native2);
    const size_t blocks = native_blocks.size();

    assert(f.call(Fixture::intuition, 264, { f.window, c.strip }) == TRUE);
    const ULONG gt = usual(f);
    assert(f.call(Fixture::intuition, 264, { window2, gt }) == TRUE && native2.MenuStrip);
    Kaffeine_MenusCleanup(&f.ctx);
    assert(!f.native_window.MenuStrip && !native2.MenuStrip && !f.ctx.menus && strips.empty());
    assert(native_blocks.size() == blocks && f.lng(window2 + M68K_WIN_MENUSTRIP) == 0);

    /* FreeMenus() of a strip still on a window takes it off first */
    const ULONG gt2 = usual(f);
    assert(f.call(Fixture::intuition, 264, { f.window, gt2 }) == TRUE);
    f.call(Fixture::gadtools, 54, { gt2 });
    assert(!f.native_window.MenuStrip && f.lng(f.window + M68K_WIN_MENUSTRIP) == 0 && strips.empty());

    /* a strip that the program built is no strip of CreateMenusA() */
    assert(f.call(Fixture::intuition, 264, { f.window, c.strip }) == TRUE);
    const unsigned calls = native_calls;
    f.call(Fixture::gadtools, 54, { c.strip });
    assert(!f.stopped() && native_calls == calls && f.native_window.MenuStrip);
    const ULONG vi = f.visualinfo(Fixture::screen);
    f.call(Fixture::gadtools, 66, { c.strip, vi, 0 });
    assert(f.stopped());
    f.call(Fixture::gadtools, 132, { vi });
    /* a copy that waits goes at the end too */
    f.call(Fixture::intuition, 54, { f.window });
    f.call(Fixture::intuition, 72, { window2 });
    assert(f.ctx.menus && native_blocks.size() == blocks + 1);
    Kaffeine_MenusCleanup(&f.ctx);
    assert(!f.ctx.menus && native_blocks.size() == blocks);
}

int main()
{
    struct Test { const char *name; void (*run)(); };
    const Test tests[] =
    {
        { "GadTools and Intuition menu tables use their ABI slots", test_registration },
        { "A VisualInfo is a 68k handle", test_visualinfo },
        { "VisualInfo retains borrowed screens, bounds ownership and cleans up before screens", test_visualinfo_lifetime },
        { "DrawBevelBoxA() resolves RastPorts and keeps only the first supported tags", test_bevel_rastports_and_tags },
        { "DrawBevelBoxA() follows TAG_MORE, TAG_SKIP and TAG_IGNORE", test_bevel_tag_controls },
        { "DrawBevelBoxA() takes signed WORD geometry from D0 through D3", test_bevel_geometry },
        { "DrawBevelBoxA() does nothing without a VisualInfo, for any RastPort", test_bevel_without_visualinfo },
        { "DrawBevelBoxA() refuses raw or freed handles and invalid RastPorts", test_bevel_refused_handles },
        { "DrawBevelBoxA() refuses out-of-bounds tag lists and cycles", test_bevel_refused_taglists },
        { "CreateMenusA() gives a 68k mirror of the strip", test_create_mirror },
        { "The tags of CreateMenusA() and its error ULONG", test_create_tags },
        { "68k input that CreateMenusA() refuses, and no memory", test_create_refused },
        { "Layout, SetMenuStrip(), ItemAddress() and the flags", test_layout_and_strip },
        { "A menu pick shows CHECKED and NextSelect", test_menupick },
        { "An image plane has at most the words that DrawImage() counts", test_copy_plane_words },
        { "SetMenuStrip() of a strip that the program built gives Intuition a copy", test_classic_copy },
        { "The strings of a copy are its own, of 255 characters at most", test_classic_strings },
        { "A pick, OffMenu() and OnMenu() reach the program's strip by bit; Reset needs no memory", test_classic_pick },
        { "Windows share the copy of a strip; the eight most recent copies wait", test_classic_windows },
        { "Strips that the bridge refuses, and no memory for a copy", test_classic_refused },
        { "As many menus, items and subitems as a menu number names", test_classic_limits },
        { "The end of the program frees every strip, and GadTools takes only its own", test_classic_cleanup }
    };
    for (const Test &test : tests)
    {
        test.run();
        std::printf("PASS: %s\n", test.name);
    }
    std::printf("GadTools menus: %zu groups passed\n", std::size(tests));
    return 0;
}
